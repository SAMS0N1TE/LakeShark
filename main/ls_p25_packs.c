/* See ls_p25_packs.h. No I/O in registry readers or action handlers. */
#include "ls_p25_packs.h"
#include "ls_ota.h"
#include "p25_sites.h"
#include "p25_controls.h"
#include "tui/ls_action.h"
#include "tui/ls_value.h"
#include "ls_nvs_safe.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static EXT_RAM_BSS_ATTR p25_pack_catalog_t catalog;
static EXT_RAM_BSS_ATTR ls_p25_pack_info_t info[P25_PACK_STATE_MAX];
static EXT_RAM_BSS_ATTR char catalog_text[LS_OTA_DATA_MAX_TEXT + 1], token[192];
static EXT_RAM_BSS_ATTR char entries_text[8192], entries_staging[8192];
static EXT_RAM_BSS_ATTR ls_p25_pack_info_t info_staging[P25_PACK_STATE_MAX];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char stage[96] = "idle";
static unsigned count, got, total;
static bool busy;
static QueueHandle_t queue;
typedef struct { char op; char arg[192]; } request_t;
static void state(const char *s)
{ portENTER_CRITICAL(&mux); snprintf(stage, sizeof(stage), "%s", s); portEXIT_CRITICAL(&mux); }
static void path_for(const char *st, char *path, size_t cap)
{ snprintf(path, cap, "/sdcard/p25/sites/%.2s.txt", st); }
static esp_err_t token_nvs(void *ctx)
{
    request_t *r = ctx; nvs_handle_t n;
    esp_err_t e = nvs_open("p25packs", NVS_READWRITE, &n);
    if (e != ESP_OK) return e;
    if (r->op == 't') {
        e = nvs_set_str(n, "token", r->arg);
        if (e == ESP_OK) e = nvs_commit(n);
    } else {
        size_t size = sizeof(token); e = nvs_get_str(n, "token", token, &size);
        if (e == ESP_ERR_NVS_NOT_FOUND) { token[0] = 0; e = ESP_OK; }
    }
    nvs_close(n); return e;
}
static bool hash_file(const char *path, uint8_t digest[32], size_t *bytes)
{
    FILE *f = fopen(path, "rb"); if (!f) return false;
    char buf[1024]; size_t n; bool ok = true;
    mbedtls_sha256_context sha; mbedtls_sha256_init(&sha);
    ok = mbedtls_sha256_starts(&sha, 0) == 0; *bytes = 0;
    while (ok && (n = fread(buf, 1, sizeof(buf), f))) {
        *bytes += n;
        ok = *bytes <= P25_PACK_MAX_BYTES && mbedtls_sha256_update(&sha, (uint8_t *)buf, n) == 0;
    }
    ok = ok && !ferror(f) && mbedtls_sha256_finish(&sha, digest) == 0;
    fclose(f); mbedtls_sha256_free(&sha); return ok;
}
static void installed(void)
{
    /* Worker assembles summaries; readers only copy published metadata. */
    size_t used = 0; entries_staging[0] = 0;
    for (unsigned i = 0; i < catalog.count; i++) {
        ls_p25_pack_info_t entry = {.pack = catalog.entries[i]};
        char path[80]; path_for(entry.pack.state, path, sizeof(path));
        uint8_t digest[32]; size_t bytes;
        bool exists = hash_file(path, digest, &bytes);
        entry.installed = exists;
        if (exists) {
            if (p25_pack_digest_ok(&entry.pack, bytes, digest))
                snprintf(entry.installed_version, sizeof(entry.installed_version), "%s", entry.pack.version);
            else {
                char vp[96]; snprintf(vp, sizeof(vp), "%s.version", path);
                FILE *f = fopen(vp, "rb");
                if (f) { size_t n = fread(entry.installed_version, 1, sizeof(entry.installed_version) - 1, f); entry.installed_version[n] = 0; fclose(f); }
                else strcpy(entry.installed_version, "local");
            }
        }
        info_staging[i] = entry;
        int n = snprintf(entries_staging + used, sizeof(entries_staging) - used, "%s|%s|%s|%s|%s\n",
                         entry.pack.state, entry.pack.name, entry.pack.plus ? "plus" : "free",
                         entry.installed ? entry.installed_version : "absent", entry.pack.version);
        if (n > 0 && (size_t)n < sizeof(entries_staging) - used) used += n;
    }
    portENTER_CRITICAL(&mux);
    memcpy(info, info_staging, catalog.count * sizeof(info[0]));
    memcpy(entries_text, entries_staging, used + 1);
    count = catalog.count;
    portEXIT_CRITICAL(&mux);
}
static void refresh(void)
{
    state("refreshing");
    int status = 0; int64_t length;
    const int64_t deadline = esp_timer_get_time() + 30000000;
    esp_http_client_handle_t c = ls_ota_open_data(P25_PACK_CATALOG_URL, NULL, deadline, &length, &status);
    if (!c || status != 200) { if (c) esp_http_client_cleanup(c); state("catalog unavailable"); return; }
    size_t len = 0; int n = 0;
    while (len < sizeof(catalog_text) && esp_timer_get_time() < deadline) {
        n = esp_http_client_read(c, catalog_text + len, 1);
        if (n <= 0) break;
        len += (size_t)n;
    }
    esp_http_client_close(c); esp_http_client_cleanup(c);
    p25_pack_catalog_t *candidate = heap_caps_malloc(sizeof(*candidate), MALLOC_CAP_SPIRAM);
    bool ok = candidate && n == 0 && len <= LS_OTA_DATA_MAX_TEXT && esp_timer_get_time() < deadline &&
              p25_pack_catalog_parse(candidate, catalog_text, len, ls_ota_signed_by_us);
    if (ok) { catalog = *candidate; installed(); state("ready"); }
    else state("catalog refused: format or signature");
    heap_caps_free(candidate);
}
/* FAT cannot replace an existing destination. Keep the last good file
 * until the verified candidate is in place, and roll back on rename failure. */
static bool install_pack(const char *tmp, const char *path)
{
    char backup[96]; snprintf(backup, sizeof(backup), "%s.bak", path);
    struct stat st;
    bool have = stat(path, &st) == 0;
    if (have) {
        remove(backup);
        if (rename(path, backup)) return false;
    }
    if (rename(tmp, path)) {
        if (have) (void)rename(backup, path);
        return false;
    }
    if (have) remove(backup);
    return true;
}
static void download(const char *st)
{
    const p25_pack_entry_t *e = NULL;
    for (unsigned i = 0; i < catalog.count; i++) if (!strcmp(st, catalog.entries[i].state)) e = &catalog.entries[i];
    if (!e) { state("refresh catalog first; state not listed"); return; }
    if (!p25_pack_allowed(e, token)) { state("plus pack requires an entitlement token"); return; }
    char path[80], tmp[96], url[256];
    path_for(st, path, sizeof(path)); snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(url, sizeof(url), "https://" LS_OTA_HOST_PRIMARY "/p25/%s", e->file);
    mkdir("/sdcard/p25", 0775); mkdir("/sdcard/p25/sites", 0775);
    state("downloading");
    portENTER_CRITICAL(&mux); got = 0; total = e->bytes; portEXIT_CRITICAL(&mux);
    const int64_t deadline = esp_timer_get_time() + 120000000;
    int status; int64_t length;
    esp_http_client_handle_t c = ls_ota_open_data(url, token, deadline, &length, &status);
    FILE *f = c && status == 200 ? fopen(tmp, "wb") : NULL;
    bool ok = f && (length <= 0 || length == e->bytes);
    uint32_t bytes = 0; char buf[1024];
    while (ok && esp_timer_get_time() < deadline) {
        int n = esp_http_client_read(c, buf, sizeof(buf));
        if (n < 0) { ok = false; break; }
        if (!n) break;
        if ((size_t)n > e->bytes - bytes || fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
        bytes += (unsigned)n;
        portENTER_CRITICAL(&mux); got = bytes; portEXIT_CRITICAL(&mux);
    }
    if (f && fclose(f)) ok = false;
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    uint8_t digest[32]; size_t hashed;
    ok = ok && esp_timer_get_time() < deadline && bytes == e->bytes &&
         hash_file(tmp, digest, &hashed) && p25_pack_digest_ok(e, hashed, digest);
    /* Hash is authoritative; profile syntax is checked before installation too. */
    p25_pack_db_t *db = NULL; p25_profile_parse_scratch_t *scratch = NULL; char *text = NULL;
    if (ok) {
        db = heap_caps_calloc(1, sizeof(*db), MALLOC_CAP_SPIRAM);
        scratch = heap_caps_malloc(sizeof(*scratch), MALLOC_CAP_SPIRAM);
        text = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        f = text ? fopen(tmp, "rb") : NULL;
        ok = db && scratch && f && fread(text, 1, bytes, f) == bytes;
        if (f) fclose(f);
        const ls_radio_range_t ranges[] = {{P25_CONTROL_TUNER_MIN_HZ, P25_CONTROL_TUNER_MAX_HZ}};
        const p25_profile_parse_config_t cfg = {ranges, 1};
        if (ok) ok = p25_pack_parse(db, scratch, text, bytes, &cfg, NULL);
    }
    heap_caps_free(db); heap_caps_free(scratch); heap_caps_free(text);
    if (ok) ok = install_pack(tmp, path);
    if (!ok) { remove(tmp); state("pack refused: transfer, hash, format or rename"); return; }
    char vp[96]; snprintf(vp, sizeof(vp), "%s.version", path);
    f = fopen(vp, "wb"); if (f) { fwrite(e->version, 1, strlen(e->version), f); fclose(f); }
    installed();
    state(p25_sites_reload() ? "installed; site reload queued" : "installed; site reload busy");
}
static void worker(void *arg)
{
    (void)arg; request_t r = {0};
    if (ls_nvs_run(token_nvs, &r, 0) != ESP_OK) state("token storage unavailable");
    for (;;) {
        if (xQueueReceive(queue, &r, portMAX_DELAY) != pdTRUE) continue;
        if (r.op == 'f') refresh();
        else if (r.op == 'g') download(r.arg);
        else if (r.op == 'r') {
            char path[80], vp[96]; path_for(r.arg, path, sizeof(path));
            if (remove(path)) state("pack removal failed or absent");
            else {
                snprintf(vp, sizeof(vp), "%s.version", path); remove(vp);
                installed(); state(p25_sites_reload() ? "removed; site reload queued" : "removed; site reload busy");
            }
        } else if (r.op == 't') {
            if (ls_nvs_run(token_nvs, &r, 0) == ESP_OK) { strcpy(token, r.arg); state("token stored"); }
            else state("token storage failed");
        }
        memset(&r, 0, sizeof(r));
        portENTER_CRITICAL(&mux); busy = false; portEXIT_CRITICAL(&mux);
    }
}
static bool request(char op, const char *arg)
{
    if (!queue) return false;
    request_t r = {.op = op};
    if (arg) {
        if (strlen(arg) >= sizeof(r.arg)) return false;
        if ((op == 'g' || op == 'r') && !p25_pack_state_valid(arg)) return false;
        for (const char *p = arg; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) return false;
        strcpy(r.arg, arg);
    }
    portENTER_CRITICAL(&mux); bool available = !busy; if (available) busy = true; portEXIT_CRITICAL(&mux);
    if (!available) return false;
    if (xQueueSend(queue, &r, 0) == pdTRUE) return true;
    portENTER_CRITICAL(&mux); busy = false; portEXIT_CRITICAL(&mux); return false;
}
unsigned ls_p25_pack_entry(unsigned index, ls_p25_pack_info_t *out)
{
    portENTER_CRITICAL(&mux); unsigned n = count; if (out && index < n) *out = info[index]; portEXIT_CRITICAL(&mux); return n;
}
static bool v_geo_enabled(ls_val_t *v) { p25_sites_status_t s; p25_sites_status(&s); v->kind = LS_VAL_BOOL; v->i = s.enabled; return true; }
#define GEO_TEXT(fn, field) static bool fn(ls_val_t *v) { static char text[sizeof(((p25_sites_status_t *)0)->field)]; p25_sites_status_t s; p25_sites_status(&s); strcpy(text, s.field); v->kind = LS_VAL_TEXT; v->s = text; return true; }
GEO_TEXT(v_geo_state, state)
GEO_TEXT(v_geo_site, site)
GEO_TEXT(v_geo_system, system)
GEO_TEXT(v_geo_error, error)
#define GEO_INT(fn, field) static bool fn(ls_val_t *v) { p25_sites_status_t s; p25_sites_status(&s); v->kind = LS_VAL_INT; v->i = s.field; return true; }
GEO_INT(v_geo_sites, sites)
GEO_INT(v_geo_packs, packs)
static bool v_geo_distance(ls_val_t *v) { p25_sites_status_t s; p25_sites_status(&s); if (s.distance_km < 0) return false; v->kind = LS_VAL_FLOAT; v->f = s.distance_km; return true; }
static bool v_pack_state(ls_val_t *v) { static char text[96]; portENTER_CRITICAL(&mux); strcpy(text, stage); portEXIT_CRITICAL(&mux); v->kind = LS_VAL_TEXT; v->s = text; return true; }
static bool v_pack_count(ls_val_t *v) { v->kind = LS_VAL_INT; v->i = ls_p25_pack_entry(0, NULL); return true; }
static bool v_pack_progress(ls_val_t *v) { portENTER_CRITICAL(&mux); v->f = total ? (float)got / total : 0; portEXIT_CRITICAL(&mux); v->kind = LS_VAL_FLOAT; return true; }
static bool v_pack_entries(ls_val_t *v) {
    static EXT_RAM_BSS_ATTR char text[8192];
    portENTER_CRITICAL(&mux); strcpy(text, entries_text); portEXIT_CRITICAL(&mux);
    v->kind = LS_VAL_TEXT; v->s = text; return true;
}
static ls_act_status_t a_toggle(const ls_args_t *a, ls_val_t *v) { (void)a; (void)v; p25_sites_status_t s; p25_sites_status(&s); return p25_sites_set(!s.enabled) ? LS_ACT_OK : LS_ACT_BUSY; }
static ls_act_status_t a_resume(const ls_args_t *a, ls_val_t *v) { (void)a; (void)v; return p25_sites_resume() ? LS_ACT_OK : LS_ACT_BUSY; }
static ls_act_status_t a_refresh(const ls_args_t *a, ls_val_t *v) { (void)a; (void)v; return request('f', NULL) ? LS_ACT_OK : LS_ACT_BUSY; }
static ls_act_status_t a_get(const ls_args_t *a, ls_val_t *v) { (void)v; if (!p25_pack_state_valid(a->v[0].s)) return LS_ACT_BADARG; return request('g', a->v[0].s) ? LS_ACT_OK : LS_ACT_BUSY; }
static ls_act_status_t a_remove(const ls_args_t *a, ls_val_t *v) { (void)v; if (!p25_pack_state_valid(a->v[0].s)) return LS_ACT_BADARG; return request('r', a->v[0].s) ? LS_ACT_OK : LS_ACT_BUSY; }
void ls_p25_packs_publish(void)
{
    static bool done; if (done) return; done = true;
    p25_sites_init();
    queue = xQueueCreate(1, sizeof(request_t));
    if (queue && xTaskCreatePinnedToCoreWithCaps(worker, "p25_packs", 6144, NULL, 3, NULL,
                                              tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) { vQueueDelete(queue); queue = NULL; }
    ls_value_publish("p25.geo.enabled", NULL, v_geo_enabled);
    ls_value_publish("p25.geo.state", NULL, v_geo_state);
    ls_value_publish("p25.geo.site", NULL, v_geo_site);
    ls_value_publish("p25.geo.system", NULL, v_geo_system);
    ls_value_publish("p25.geo.distance", "km", v_geo_distance);
    ls_value_publish("p25.geo.sites", NULL, v_geo_sites);
    ls_value_publish("p25.geo.packs", NULL, v_geo_packs);
    ls_value_publish("p25.geo.error", NULL, v_geo_error);
    ls_value_publish("p25.pack.state", NULL, v_pack_state);
    ls_value_publish("p25.pack.count", NULL, v_pack_count);
    ls_value_publish("p25.pack.progress", NULL, v_pack_progress);
    ls_value_publish("p25.pack.entries", NULL, v_pack_entries);
    ls_action_register("p25.geo.toggle", "", LS_CAP_STORE, a_toggle, "toggle persisted GPS sites");
    ls_action_register("p25.geo.resume", "", LS_CAP_TUNE, a_resume, "resume GPS sites after manual selection");
    ls_action_register("p25.pack.refresh", "", LS_CAP_STORE, a_refresh, "refresh signed state catalog");
    ls_action_register("p25.pack.get", "s", LS_CAP_STORE, a_get, "download a signed state pack by USPS code");
    ls_action_register("p25.pack.remove", "s", LS_CAP_STORE, a_remove, "remove a state pack by USPS code");
}
int ls_p25_packs_command(int argc, char **argv)
{
    if (argc < 1) return 1;
    if (!strcmp(argv[0], "geo")) {
        if (argc == 2 && !strcmp(argv[1], "status")) {
            p25_sites_status_t s; p25_sites_status(&s);
            printf("GPS sites %s: %s / %s, %.1f km, %u sites / %u packs; %s\n", s.enabled ? s.state : "off", s.system, s.site, (double)s.distance_km, s.sites, s.packs, s.error); return 0;
        }
        if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
            bool ok = p25_sites_set(!strcmp(argv[1], "on")); puts(ok ? "GPS setting queued" : "GPS worker busy"); return !ok;
        }
    } else if (!strcmp(argv[0], "pack")) {
        if (argc == 2 && !strcmp(argv[1], "list")) {
            for (unsigned i = 0; i < ls_p25_pack_entry(0, NULL); i++) {
                ls_p25_pack_info_t e; ls_p25_pack_entry(i, &e);
                printf("%s %s %s available=%s installed=%s\n", e.pack.state, e.pack.name, e.pack.plus ? "plus" : "free", e.pack.version, e.installed ? e.installed_version : "no");
            }
            ls_val_t v; v_pack_state(&v); puts(v.s); return 0;
        }
        char op = argc == 2 && !strcmp(argv[1], "refresh") ? 'f' :
                  argc == 3 && !strcmp(argv[1], "get") ? 'g' :
                  argc == 3 && !strcmp(argv[1], "rm") ? 'r' :
                  argc == 3 && !strcmp(argv[1], "token") ? 't' : 0;
        if (op) { bool ok = request(op, argc == 3 ? argv[2] : NULL); puts(ok ? "Pack job queued" : "Pack worker busy or invalid argument"); return !ok; }
    }
    puts("usage: p25 pack list|refresh|get <ST>|rm <ST>|token <t>; p25 geo on|off|status"); return 1;
}
