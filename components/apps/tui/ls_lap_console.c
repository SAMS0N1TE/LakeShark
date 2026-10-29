/* Console task owns SD uploads; lap_io expires idle transactions, UI publishes tiles. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_console.h"
#include "ls_textpack.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ls_app.h"
#include "ls_lap.h"
#include "ls_lap_api.h"
#include "ls_lap_radio.h"
#include "ls_lap_service.h"
#include "ls_lap_signature.h"
#include "ls_lap_storage.h"
#include "ls_lap_transfer.h"
#include "mbedtls/sha256.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define INCOMING "/sdcard/apps/.incoming"
EXT_RAM_BSS_ATTR static struct {
    ls_lap_transfer_t transfer;
    FILE *incoming;
    mbedtls_sha256_context sha;
    bool hash_live, initialized;
    ls_lap_info_t candidate;
} s;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static uint32_t now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void cancel(void *ctx) {
    (void)ctx;
    if (s.incoming)
        fclose(s.incoming);
    s.incoming = NULL;
    if (s.hash_live)
        mbedtls_sha256_free(&s.sha);
    s.hash_live = false;
    remove(INCOMING);
}
static bool begin(void *ctx, const char *name) {
    (void)ctx;
    (void)name;
    if (mkdir("/sdcard/apps", 0777) && errno != EEXIST)
        return false;
    s.incoming = fopen(INCOMING, "wb");
    if (!s.incoming)
        return false;
    mbedtls_sha256_init(&s.sha);
    s.hash_live = true;
    return mbedtls_sha256_starts(&s.sha, 0) == 0;
}
static bool write_chunk(void *ctx, const uint8_t *bytes, size_t n) {
    (void)ctx;
    return s.incoming && fwrite(bytes, 1, n, s.incoming) == n &&
           mbedtls_sha256_update(&s.sha, bytes, n) == 0;
}
static bool memory_read(void *ctx, size_t off, void *out, size_t n) {
    memcpy(out, (const uint8_t *)ctx + off, n);
    return true;
}
static bool finish(void *ctx, const char *name, size_t size, const uint8_t expected[32], char *why,
                   size_t cap) {
    (void)ctx;
    bool ok = false, lease = false;
    uint8_t actual[32];
    uint8_t *bytes = NULL;
    FILE *f = NULL;
    if (!s.incoming || fclose(s.incoming)) {
        s.incoming = NULL;
        LS_SNPUTS(t_ls_lap_console_incoming_close_failed, why, cap, "incoming close failed");
        goto done;
    }
    s.incoming = NULL;
    if (mbedtls_sha256_finish(&s.sha, actual) || memcmp(actual, expected, 32)) {
        snprintf(why, cap, "SHA256 mismatch");
        goto done;
    }
    mbedtls_sha256_free(&s.sha);
    s.hash_live = false;
    struct stat st;
    if (stat(INCOMING, &st) || st.st_size != (long)size ||
        !(bytes = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) ||
        !(f = fopen(INCOMING, "rb")) || fread(bytes, 1, size, f) != size) {
        LS_SNPUTS(t_ls_lap_console_cannot_read_incoming, why, cap, "cannot read incoming");
        goto done;
    }
    fclose(f);
    f = NULL;
    if (mbedtls_sha256(bytes, size, actual, 0) || memcmp(actual, expected, 32)) {
        LS_SNPUTS(t_ls_lap_console_incoming_readback_sha256_mismatch, why, cap, "incoming readback SHA256 mismatch");
        goto done;
    }
    size_t elf_size;
    bool is_signed;
    ls_lap_reader_t r = {memory_read, bytes, size};
    memset(&s.candidate, 0, sizeof(s.candidate));
    if (!ls_lap_signature(&r, ls_lap_verify_release, NULL, &elf_size, &is_signed, why, cap) ||
        !ls_lap_manifest(bytes, elf_size, &s.candidate.manifest, why, cap) ||
        !ls_lap_preflight(bytes, elf_size, why, cap))
        goto done;
    {
        const ls_lap_reader_t er = {memory_read, bytes, elf_size};
        s.candidate.has_icon_px = ls_lap_icon_read(&er, s.candidate.icon_px);
    }
    snprintf(s.candidate.path, sizeof(s.candidate.path), "/sdcard/apps/%s", name);
    lease = ls_lap_maintenance_begin(s.candidate.manifest.id);
    if (!lease) {
        LS_SNPUTS(t_ls_lap_console_native_app_open_or, why, cap, "native app open or refresh pending");
        goto done;
    }
    const ls_lap_info_t *existing = ls_lap_by_id(s.candidate.manifest.id);
    int installed = 0;
    if (!existing && ls_app_by_id(s.candidate.manifest.id)) {
        LS_SNPUTS(t_ls_lap_console_id_belongs_to_a, why, cap, "id belongs to a built-in app");
        goto done;
    }
    if ((!existing || !existing->installed) && ls_app_count() >= LS_TUI_MAX_SCREENS) {
        snprintf(why, cap, "app registry full");
        goto done;
    }
    for (int i = 0; ls_lap_at(i); i++) {
        const ls_lap_info_t *a = ls_lap_at(i);
        if (!a->installed)
            continue;
        installed++;
        if (ls_lap_name_equal(a->path, s.candidate.path) &&
            strcmp(a->manifest.id, s.candidate.manifest.id)) {
            LS_SNPUTS(t_ls_lap_console_filename_belongs_to_another, why, cap, "filename belongs to another id");
            goto done;
        }
    }
    if (existing && existing->installed && !ls_lap_name_equal(existing->path, s.candidate.path)) {
        LS_SNPUTS(t_ls_lap_console_id_already_installed_under, why, cap, "id already installed under another filename");
        goto done;
    }
    if ((!existing || !existing->installed) && installed >= LS_LAP_MAX_APPS) {
        LS_SNPUTS(t_ls_lap_console_native_directory_full, why, cap, "native directory full");
        goto done;
    }
    s.candidate.size = size;
    s.candidate.is_signed = is_signed;
    s.candidate.signature_checked = true;
    s.candidate.signature_present = is_signed;
    s.candidate.installed = true;
    s.candidate.needs_confirmation = existing && existing->needs_confirmation;
    memcpy(s.candidate.sha256, actual, 32);
    if (!ls_lap_replace(INCOMING, s.candidate.path)) {
        LS_SNPUTS(t_ls_lap_console_atomic_rename_failed, why, cap, "atomic rename failed");
        goto done;
    }
    ok = ls_lap_publish(&s.candidate);
    if (ok)
        lease = false;
    else
        LS_SNPUTS(t_ls_lap_console_installed_refresh_applies_at, why, cap, "installed; refresh applies at next boot");
done:
    if (f)
        fclose(f);
    heap_caps_free(bytes);
    if (lease)
        ls_lap_maintenance_end();
    return ok;
}
static void print_info(const ls_lap_info_t *a) {
    /* Manifest text is percent encoded so a store name cannot forge reply lines. */
    char name[46], version[46];
    char *dest[] = {name, version};
    const char *src[] = {a->manifest.name, a->manifest.version};
    for (int field = 0; field < 2; field++) {
        char *p = dest[field];
        for (const unsigned char *q = (const unsigned char *)src[field]; *q; q++) {
            if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') ||
                *q == '_' || *q == '.' || *q == '-')
                *p++ = (char)*q;
            else {
                snprintf(p, 4, "%%%02X", *q);
                p += 3;
            }
        }
        *p = 0;
    }
    char hash[17];
    for (int i = 0; i < 8; i++)
        snprintf(hash + i * 2, 3, "%02x", a->sha256[i]);
    LS_PRINTF(t_ls_lap_console_app_ok_app_id, "APP OK app id=%s name=%s version=%s size=%lu signed=%s sha256=%s\n", a->manifest.id,
           name, version, (unsigned long)a->size, a->is_signed ? "yes" : "no", hash);
}
static void inspect_info(const ls_lap_info_t *a) {
    char why[96] = {0};
    if (!ls_lap_inspect_file(a->path, &s.candidate, why, sizeof(why)) ||
        memcmp(&a->manifest, &s.candidate.manifest, LS_LAP_MANIFEST_BYTES)) {
        LS_PRINTF(t_ls_lap_console_app_err_verify_31s, "APP ERR verify %.31s %s\n", a->manifest.id,
               why[0] ? why : "manifest changed since scan");
        return;
    }
    print_info(&s.candidate);
}
static bool data_name(const char *name) { return ls_lap_plain_name(name, 48); }
static void rx_line(const char *text) { puts(text); }

static int command(int argc, char **argv) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    char reply[160];
    if (ls_lap_transfer_command(&s.transfer, argc, (const char *const *)argv, now(), reply,
                                sizeof(reply))) {
        puts(reply);
        goto done;
    }
    if (ls_lap_transfer_expire(&s.transfer, now())) {
        LS_PUTS(t_ls_lap_console_app_err_timeout_upload, "APP ERR timeout upload expired");
        goto done;
    }
    if (argc == 2 && !strcmp(argv[1], "rx")) {   /* API 2.0: radios and the open app's session */
        ls_lap_radio_describe(rx_line);
        goto done;
    }
    if (argc == 2 && !strcmp(argv[1], "svc")) {   /* API 2.1: the background service, if any */
        ls_lap_service_info_t v;
        if (ls_lap_services(&v, 1) > 0)
            LS_PRINTF(t_ls_lap_console_app_ok_svc_id, "APP OK svc id=%s screen=%s period_ms=%lu ticks=%lu last_us=%lu worst_us=%lu over=%lu\n", v.id,
                   v.shown ? "open" : "closed", (unsigned long)v.period_ms, (unsigned long)v.stats.ticks,
                   (unsigned long)v.stats.last_us, (unsigned long)v.stats.worst_us, (unsigned long)v.stats.over);
        puts("APP OK svc end");
        goto done;
    }
    if (argc == 3 && !strcmp(argv[1], "stop")) {
        puts(ls_lap_service_request_stop(argv[2]) ? "APP OK stop queued" : "APP ERR absent no service by that id");
        goto done;
    }
    if (ls_lap_catalog_busy()) {
        LS_PUTS(t_ls_lap_console_app_err_busy_tile, "APP ERR busy tile refresh pending");
        goto done;
    }
    if (argc == 2 && !strcmp(argv[1], "list")) {
        for (int i = 0; ls_lap_at(i); i++)
            if (ls_lap_at(i)->installed)
                inspect_info(ls_lap_at(i));
        puts("APP OK list end");
        goto done;
    }
    if (argc == 3 && !strcmp(argv[1], "info")) {
        const ls_lap_info_t *a = ls_lap_by_id(argv[2]);
        if (a && a->installed) {
            inspect_info(a);
            if (ls_lap_is_open(a->manifest.id))
                LS_PRINTF(t_ls_lap_console_app_ok_runtime_heap, "APP OK runtime heap=%lu limit=4194304 bad_free=%u stack_unused=%u\n",
                       (unsigned long)ls_lap_api_bytes(), ls_lap_api_bad_frees(),
                       ls_lap_stack_unused());
        } else
            LS_PUTS(t_ls_lap_console_app_err_absent_unknown, "APP ERR absent unknown app id");
        goto done;
    }
    if ((argc == 3 || (argc == 4 && !strcmp(argv[3], "all"))) && !strcmp(argv[1], "rm")) {
        const ls_lap_info_t *a = ls_lap_by_id(argv[2]);
        if (!a || !a->installed) {
            LS_PUTS(t_ls_lap_console_app_err_absent_unknown, "APP ERR absent unknown app id");
            goto done;
        }
        if (!ls_lap_maintenance_begin(a->manifest.id)) {
            LS_PUTS(t_ls_lap_console_app_err_busy_native, "APP ERR busy native app open or refresh pending");
            goto done;
        }
        s.candidate = *a;
        s.candidate.installed = false;
        if (remove(a->path)) {
            ls_lap_maintenance_end();
            LS_PUTS(t_ls_lap_console_app_err_io_app, "APP ERR io app removal failed");
            goto done;
        }
        bool data_ok = true;
        if (argc == 4) {
            char dir[128];
            LS_SNPRINTF(t_ls_lap_console_sdcard_apps_data_s, dir, sizeof(dir), "/sdcard/apps_data/%s", a->manifest.id);
            if (!ls_lap_recover_dir(dir))
                data_ok = false;
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                        continue;
                    if (!data_name(e->d_name) && strcmp(e->d_name, ".lap-write")) {
                        data_ok = false;
                        continue;
                    }
                    char full[192];
                    snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
                    if (remove(full))
                        data_ok = false;
                }
                closedir(d);
                if (rmdir(dir))
                    data_ok = false;
            }
        }
        if (!ls_lap_publish(&s.candidate)) {
            ls_lap_maintenance_end();
            LS_PUTS(t_ls_lap_console_app_err_refresh_removed, "APP ERR refresh removed; restart to refresh");
        } else if (!data_ok)
            LS_PUTS(t_ls_lap_console_app_err_data_app, "APP ERR data app removed; some data files remain");
        else
            LS_PUTS(t_ls_lap_console_app_ok_rm_refresh, "APP OK rm refresh=queued");
        goto done;
    }
    LS_PUTS(t_ls_lap_console_app_err_args_use, "APP ERR args use list|put|data|end|rm|info|rx|svc|stop");
done:
    xSemaphoreGive(mutex);
    return 0;
}
void ls_lap_console_expire(void) {
    if (mutex && xSemaphoreTake(mutex, 0) == pdTRUE) {
        if (ls_lap_transfer_expire(&s.transfer, now()))
            LS_PUTS(t_ls_lap_console_app_err_timeout_upload, "APP ERR timeout upload expired");
        xSemaphoreGive(mutex);
    }
}
void ls_lap_console_register(void) {
    if (s.initialized)
        return;
    mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    ls_lap_transfer_init(&s.transfer,
                         (ls_lap_transfer_io_t){begin, write_chunk, finish, cancel, NULL});
    if (!ls_lap_start_worker())
        return;
    cancel(NULL); /* discard any interrupted transfer on boot */
    const esp_console_cmd_t cmd = {
        .command = "app", .help = LS_TXT(con_ls_lap_console_app_help, "Native SD apps: list|put|data|end|rm|info|rx"), .func = command};
    if (esp_console_cmd_register(&cmd) == ESP_OK)
        s.initialized = true;
}
#endif
