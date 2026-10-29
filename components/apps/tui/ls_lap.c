#include "ls_lap.h"
#include "ls_lap_api.h"
#include "ls_lap_signature.h"
#include "ls_lap_sys.h"
#include "ls_lap_net.h"
#include "ls_lap_radio.h"
#include "ls_lap_grants.h"
#include "ls_lap_service.h"
#include "ls_lap_kit.h"
#include <stdio.h>
#include <string.h>
#include "ls_textpack.h"
#ifdef ESP_PLATFORM
#include "core/ls_trail.h"
#include "esp_attr.h"
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ls_app.h"
#include "ls_icons.h"
#include "ls_lap_ui.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <sys/stat.h>
#ifndef LS_LAP_DATA_ROOT
#define LS_LAP_DATA_ROOT "/sdcard/apps_data"
#endif

#if !CONFIG_ELF_LOADER_LOAD_PSRAM || CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR
#error "Native apps require PSRAM segment loading (P4)"
#endif
enum { IO_IDLE, IO_LOAD, IO_READY, IO_CLEAN, IO_CLEANED };
EXT_RAM_BSS_ATTR static struct {
    ls_lap_info_t apps[LS_LAP_MAX_APPS];
    int count;
    char error[128], work_error[128], crash[64];
    ls_lap_status_t status;
    esp_elf_t elf;
    const lap_app_t *app;
    uint32_t generation, opened_ms;
    int active, desired;
    bool visible, entered;
    atomic_uint stack_unused;
    unsigned callbacks;
    struct {
        uintptr_t start, end;
    } exec[16];
    unsigned nexec;
    uint8_t work_hash[32];
    bool work_signed;
    size_t image_size;
    tui_rect area;
} s;
static atomic_int operation;
static atomic_int gate; /* 0 free, 1 active, 2 maintenance, 3 both (different ids) */
static atomic_bool pending;
static atomic_int open_slot;
EXT_RAM_BSS_ATTR static ls_lap_info_t update;
static void ls_lap_select(const char *id);
extern const ls_tui_screen_t ls_lap_screen;
static TaskHandle_t worker;
static StaticTask_t worker_tcb;
EXT_RAM_BSS_ATTR static StackType_t worker_stack[6144 / sizeof(StackType_t)];
static const ls_app_doc_t doc = {
    .purpose = "A native app installed on the SD card. Its author supplies its behavior.",
    .records = LS_APP_RECORDS_MANUAL,
    .record_note = "May keep files in its private app data folder.",
    .gps = LS_APP_GPS_STAMPS,
    .gps_note = "Can read published values, including position."};
static bool file_read(void *ctx, size_t offset, void *out, size_t n) {
    return fseek(ctx, (long)offset, SEEK_SET) == 0 && fread(out, 1, n, ctx) == n;
}
static bool memory_read(void *ctx, size_t offset, void *out, size_t n) {
    memcpy(out, (const uint8_t *)ctx + offset, n);
    return true;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static unsigned le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static bool digest(ls_lap_reader_t *r, uint8_t hash[32]) {
    uint8_t *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
        return false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
    for (size_t off = 0; ok && off < r->size;) {
        size_t n = r->size - off;
        if (n > 1024)
            n = 1024;
        ok = r->read(r->ctx, off, buf, n) && mbedtls_sha256_update(&sha, buf, n) == 0;
        off += n;
    }
    ok = ok && mbedtls_sha256_finish(&sha, hash) == 0;
    mbedtls_sha256_free(&sha);
    heap_caps_free(buf);
    return ok;
}
static void crash_boot(void) {
    const char *id = ls_trail_last_native();
    if (id && !s.crash[0])
        LS_SNPRINTF(t_ls_lap_31s_stopped_the_board, s.crash, sizeof(s.crash), "%.31s stopped the board", id);
}
static int register_info(const ls_lap_info_t *a) {
    int slot = -1;
    bool exists = false;
    for (int i = 0; i < s.count; i++)
        if (!strcmp(s.apps[i].manifest.id, a->manifest.id)) {
            slot = i;
            exists = s.apps[i].installed;
            break;
        }
    if (slot < 0)
        for (int i = 0; i < s.count; i++)
            if (!s.apps[i].installed) {
                slot = i;
                break;
            }
    if (slot < 0)
        slot = s.count;
    if (slot >= LS_LAP_MAX_APPS) {
        LS_SNPUTS(t_ls_lap_native_directory_full_at, s.error, sizeof(s.error), "native directory full at 24 apps");
        return -1;
    }
    if (!exists && ls_app_by_id(a->manifest.id)) {
        LS_SNPRINTF(t_ls_lap_duplicate_app_id_31s, s.error, sizeof(s.error), "duplicate app id %.31s", a->manifest.id);
        return -1;
    }
    if (!a->installed) {
        ls_app_unregister(a->manifest.id);
        s.apps[slot].installed = false;
        return 1;
    }
    s.apps[slot] = *a;
    ls_lap_info_t *info = &s.apps[slot];
    const int builtin = info->manifest.icon > 0 && info->manifest.icon < LS_ICON__COUNT
                            ? info->manifest.icon : LS_ICON_STAR;
    ls_app_t app = {.id = info->manifest.id,
                    .name = info->manifest.name,
                    .sub = info->manifest.sub,
                    .icon = info->has_icon_px
                                ? ls_icon_px(slot, info->icon_px, builtin)
                                : ls_icon_art(slot, (const char (*)[LS_ICON_COLS + 1])info->manifest.icon_art, builtin),
                    .hue = info->manifest.color,
                    .cat = LS_APP_USER,
                    .screen = &ls_lap_screen,
                    .doc = &doc,
                    .select = ls_lap_select};
    int rc = exists ? (ls_app_update(&app) ? 1 : -1) : ls_app_register(&app);
    if (rc < 0) {
        snprintf(s.error, sizeof(s.error), "%s", ls_app_register_why(rc));
        info->installed = false;
        return -1;
    }
    if (slot == s.count)
        s.count++;
    return 1;
}
bool ls_lap_maintenance_begin(const char *id) {
    int held = atomic_load_explicit(&gate, memory_order_acquire);
    for (;;) {
        if (held >= 2)
            return false;
        int next = held ? 3 : 2;
        if (atomic_compare_exchange_weak(&gate, &held, next))
            break;
    }
    if ((!id && atomic_load(&gate) == 3) || (id && ls_lap_is_open(id))) {
        ls_lap_maintenance_end();
        return false;
    }
    return true;
}
void ls_lap_maintenance_end(void) {
    int held = atomic_load_explicit(&gate, memory_order_acquire);
    while (held >= 2 && !atomic_compare_exchange_weak(&gate, &held, held == 3 ? 1 : 0)) {
    }
}
static void release_active(void) {
    int held = atomic_load_explicit(&gate, memory_order_acquire);
    while ((held == 1 || held == 3) &&
           !atomic_compare_exchange_weak(&gate, &held, held == 3 ? 2 : 0)) {
    }
}

bool ls_lap_publish(const ls_lap_info_t *info) {
    if (atomic_load(&gate) < 2 || atomic_load(&pending))
        return false;
    update = *info;
    atomic_store_explicit(&pending, true, memory_order_release);
    return true;
}
static bool read_info(const char *path, ls_lap_info_t *a, bool verify, char *why, size_t cap) {
    struct stat st;
    FILE *f = NULL;
    bool ok = false;
    if (!path || strlen(path) >= sizeof(a->path) || stat(path, &st) || st.st_size < 52 ||
        (uint64_t)st.st_size > LS_LAP_MAX_BYTES || !(f = fopen(path, "rb"))) {
        LS_SNPUTS(t_ls_lap_invalid_size_path_or, why, cap, "invalid size/path or unreadable app");
        return false;
    }
    memset(a, 0, sizeof(*a));
    ls_lap_reader_t r = {file_read, f, (size_t)st.st_size};
    size_t elf_size;
    bool present;
    if (verify) {
        if (!ls_lap_signature(&r, ls_lap_verify_release, NULL, &elf_size, &present, why, cap) ||
            !digest(&r, a->sha256))
            goto done;
    } else if (!ls_lap_signature_layout(&r, &elf_size, &present, why, cap))
        goto done;
    ls_lap_reader_t er = r;
    er.size = elf_size;
    if (!ls_lap_manifest_read(&er, &a->manifest, why, cap))
        goto done;
    a->has_icon_px = ls_lap_icon_read(&er, a->icon_px);
    a->size = r.size;
    a->signature_present = present;
    a->signature_checked = verify;
    a->is_signed = verify && present;
    a->installed = true;
    snprintf(a->path, sizeof(a->path), "%s", path);
    ok = true;
done:
    fclose(f);
    return ok;
}
bool ls_lap_inspect_file(const char *path, ls_lap_info_t *out, char *why, size_t cap) {
    return out && read_info(path, out, true, why, cap);
}
int ls_lap_scan_file(const char *path) {
    crash_boot();
    ls_lap_grants_load(LS_LAP_GRANTS_PATH);   /* the boot scan already reads the card */
    int rc = -1;
    ls_lap_info_t *a = heap_caps_calloc(1, sizeof(*a), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a) {
        LS_SNPUTS(t_ls_lap_no_psram_for_app, s.error, sizeof(s.error), "no PSRAM for app metadata");
        return -1;
    }
    if (!read_info(path, a, false, s.error, sizeof(s.error)))
        goto done;
    const char *crashed = ls_trail_last_native();
    a->needs_confirmation = (crashed && !strcmp(crashed, a->manifest.id)) || !a->signature_present;
    if (a->needs_confirmation)
        LS_SNPRINTF(t_ls_lap_15s_stopped_the_board, s.crash, sizeof(s.crash), "%.15s stopped the board", a->manifest.name);
    const ls_lap_info_t *other = ls_lap_by_id(a->manifest.id);
    if (other && other->installed) {
        LS_SNPRINTF(t_ls_lap_duplicate_app_id_31s, s.error, sizeof(s.error), "duplicate app id %.31s", a->manifest.id);
        goto done;
    }
    rc = register_info(a);
done:
    heap_caps_free(a);
    return rc;
}

const char *ls_lap_last_error(void) { return s.error[0] ? s.error : NULL; }
const ls_lap_info_t *ls_lap_at(int i) { return i >= 0 && i < s.count ? &s.apps[i] : NULL; }
const ls_lap_info_t *ls_lap_by_id(const char *id) {
    if (id)
        for (int i = 0; i < s.count; i++)
            if (!strcmp(s.apps[i].manifest.id, id))
                return &s.apps[i];
    return NULL;
}
const char (*ls_lap_icon_art(const char *id))[LAP_ICON_W + 1] {
    const ls_lap_info_t *a = ls_lap_by_id(id);
    return a && a->installed ? a->manifest.icon_art : NULL;
}
const ls_lap_status_t *ls_lap_status(void) { return &s.status; }
const char *ls_lap_last_crash(void) {
    crash_boot();
    return s.crash[0] ? s.crash : NULL;
}
bool ls_lap_is_open(const char *id) {
    int op = atomic_load_explicit(&operation, memory_order_acquire);
    if (!id || op == IO_IDLE || op == IO_CLEANED)
        return false;
    int i = atomic_load(&open_slot);
    return i >= 0 && i < s.count && !strcmp(id, s.apps[i].manifest.id);
}
bool ls_lap_catalog_busy(void) { return atomic_load_explicit(&gate, memory_order_acquire) >= 2; }

unsigned ls_lap_stack_unused(void) { return s.stack_unused; }
static void before(void) {
    ls_trail_native_callback(s.status.id);
    ls_lap_api_callback(true);
}
static void after(void) {
    ls_lap_api_callback(false);
    ls_trail_native_callback(NULL);
    if (++s.callbacks == 1 || !(s.callbacks & 63u)) {
        unsigned n = uxTaskGetStackHighWaterMark(NULL);
        if (!s.stack_unused || n < s.stack_unused)
            s.stack_unused = n;
    }
}
static bool executable(uintptr_t ptr) {
    if (!ptr)
        return true;
    if (ptr & 1u)
        return false;
    for (unsigned i = 0; i < s.nexec; i++)
        if (ptr >= s.exec[i].start && ptr < s.exec[i].end)
            return true;
    return false;
}
/* One big job at a time. A receiver left running in the background - P25
   on a busy system takes a core almost whole - leaves an app a third of its
   frame rate, so the receiver mode HOME was keeping is parked while an app
   is open and asked for again when the app's screen is left. A screen
   switched to after that asks for its own receiver, which wins. An app
   that names a radio in its manifest asks for that one itself. */
static char s_radio_held[16];
static uint32_t s_entered_ms;     /* when the app last opened */
static void radio_hold(const lap_app_t *app) {
    const char *claimed = ls_tui_radio_claimed();
    if (s_radio_held[0] || !claimed || app->radio[0])
        return;
    snprintf(s_radio_held, sizeof(s_radio_held), "%s", claimed);
    ls_tui_radio_want(NULL);
}
static void radio_release(void) {
    if (!s_radio_held[0])
        return;
    ls_tui_radio_want(s_radio_held);
    s_radio_held[0] = 0;
}
/* API 2.1: how the shared controls and the service tick call into the app. */
static bool code_ok(uintptr_t fn) { return fn && executable(fn); }
static const ls_lap_guard_t guard = {code_ok, before, after};

/* A service keeps the app loaded while its screen is closed. */
static bool hidden(void) { return s.entered && !ls_lap_service_shown(); }
static bool hint_given;               /* the "keeps running" notice, once a launch */
static void close_active(void);
static void request(int op);
/* Ends the service. With the screen closed the app goes too: its `stopped`
   callback, then everything an app close gives back. */
static void service_kill(const char *why) {
    const bool was = ls_lap_service_on();
    void (*stopped)(void) = ls_lap_service_off(why);
    if (was && why) {
        char line[64];
        snprintf(line, sizeof(line), "stopped: %s", why);
        ls_lap_service_notice(line);
    }
    if (!hidden())
        return;
    if (stopped && code_ok((uintptr_t)stopped)) {
        before();
        stopped();
        after();
    }
    ls_lap_sys_end();
    s.entered = false;
    ls_lap_service_set_shown(true);
    if (atomic_load_explicit(&operation, memory_order_acquire) == IO_READY)
        request(IO_CLEAN);
}

static void unload(void) {
    ls_lap_api_log_service();
    ls_lap_api_cleanup();
    ls_lap_api_set_image(NULL, 0);
    esp_elf_deinit(&s.elf);
    s.app = NULL;
    s.image_size = 0;
    s.nexec = 0;
}
/* esp_elf_relocate ends with a cache flush that disables the caches
   (spi_flash_disable_interrupts_caches_and_other_cpu), and that asserts when
   the calling task's stack is in PSRAM - which lap_io's is. So the relocation
   alone runs on a short task whose stack and TCB are internal, borrowed for
   the few milliseconds it takes and returned at once.
   The flush asserts that the stack is in main DRAM or, with RTC fast memory
   on the heap, in the LP RAM; MALLOC_CAP_INTERNAL alone can return the P4's
   TCM, which it rejects - as it did whenever P25 had taken most of DRAM. So
   the stack comes from the LP RAM first, which the radios leave alone (P25
   decoding leaves no 6 KB piece of DRAM), then from DMA-capable DRAM.
   Linking DICE used 468 bytes of it; 2 KB leaves four times that. */
#define RELOC_STACK 2048
typedef struct {
    esp_elf_t *elf;
    const uint8_t *data;
    int rc;
    TaskHandle_t caller;
    unsigned spare;                 /* bytes of the stack never touched */
} reloc_job;
static bool reloc_stack_ok(const void *p) {
    const uint8_t *a = p, *b = a + RELOC_STACK - 1;
    if (esp_ptr_in_dram(a) && esp_ptr_in_dram(b))
        return true;
#if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
    if (esp_ptr_in_rtc_dram_fast(a) && esp_ptr_in_rtc_dram_fast(b))
        return true;
#endif
    return false;
}
static StackType_t *reloc_stack(void) {
    static const uint32_t caps[] = {
#if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
        MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT,
#endif
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
    };
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        StackType_t *p = heap_caps_malloc(RELOC_STACK, caps[i]);
        if (p && reloc_stack_ok(p))
            return p;
        heap_caps_free(p);
    }
    return NULL;
}
static void reloc_task(void *arg) {
    reloc_job *j = arg;
    j->rc = esp_elf_relocate(j->elf, j->data);
    j->spare = (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    xTaskNotifyGive(j->caller);
    vTaskSuspend(NULL); /* the caller deletes this task before freeing its stack */
}
static int relocate_internal(esp_elf_t *elf, const uint8_t *data) {
    StackType_t *stack = reloc_stack();
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int rc = -1;
    if (stack && tcb) {
        reloc_job j = {elf, data, -1, xTaskGetCurrentTaskHandle(), 0};
        TaskHandle_t t = xTaskCreateStaticPinnedToCore(reloc_task, "lap_reloc", RELOC_STACK / sizeof(StackType_t), &j,
                                                       uxTaskPriorityGet(NULL), stack, tcb, xPortGetCoreID());
        if (t) {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000))) {
                rc = j.rc;
                if (j.spare < 512)          /* an app with far more relocations than any so far */
                    ESP_LOGW("lap", "linking left %u of %u stack bytes", j.spare, (unsigned)RELOC_STACK);
            }
            else
                LS_SNPUTS(t_ls_lap_elf_relocation_timed_out, s.work_error, sizeof(s.work_error), "ELF relocation timed out");
            vTaskDelete(t);
        }
    } else
        LS_SNPUTS(t_ls_lap_memory_is_short_stop, s.work_error, sizeof(s.work_error), "memory is short: stop a radio app (RADIOS) and open again");
    heap_caps_free(stack);
    heap_caps_free(tcb);
    return rc;
}
static bool load(void) {
    ls_lap_info_t *a = &s.apps[s.active];
    struct stat st;
    uint8_t *data = NULL;
    FILE *f = NULL;
    bool ok = false;
    s.work_error[0] = 0;
    if (stat(a->path, &st) || st.st_size != (long)a->size ||
        !(data = heap_caps_malloc(a->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) ||
        !(f = fopen(a->path, "rb")))
        goto done;
    if (fread(data, 1, a->size, f) != a->size)
        goto done;
    ls_lap_reader_t r = {memory_read, data, a->size};
    uint8_t hash[32];
    size_t elf_size;
    bool signed_app;
    ls_lap_api_begin(a->manifest.id, ++s.generation, s.opened_ms);
    if (!digest(&r, hash) || (a->signature_checked && memcmp(hash, a->sha256, 32))) {
        LS_SNPUTS(t_ls_lap_app_changed_since_scan, s.work_error, sizeof(s.work_error), "app changed since scan");
        goto done;
    }
    if (!ls_lap_signature(&r, ls_lap_verify_release, NULL, &elf_size, &signed_app, s.work_error,
                          sizeof(s.work_error)) ||
        !ls_lap_preflight_resolved(data, elf_size, ls_lap_resolve, s.work_error,
                                   sizeof(s.work_error)))
        goto done;
    lap_app_t current_manifest;
    if (!ls_lap_manifest(data, elf_size, &current_manifest, s.work_error, sizeof(s.work_error)) ||
        memcmp(&current_manifest, &a->manifest, LS_LAP_MANIFEST_BYTES)) {
        LS_SNPUTS(t_ls_lap_app_manifest_changed_since, s.work_error, sizeof(s.work_error), "app manifest changed since scan");
        goto done;
    }
    elf_set_symbol_resolver(ls_lap_resolve);
    if (esp_elf_init(&s.elf) || relocate_internal(&s.elf, data)) {
        if (!s.work_error[0])
            snprintf(s.work_error, sizeof(s.work_error), "%s",
                     ls_lap_missing_symbol()[0] ? ls_lap_missing_symbol() : "ELF relocation failed");
        goto done;
    }
    /* RISC-V loader 1.3.5 adds svaddr when applying relocations: require zero base. */
    if (s.elf.svaddr || !esp_ptr_external_ram(s.elf.psegment)) {
        LS_SNPUTS(t_ls_lap_elf_must_execute_in, s.work_error, sizeof(s.work_error), "ELF must execute in PSRAM with zero base");
        goto done;
    }
    s.nexec = 0;
    s.image_size = 0;
    uint32_t ph = le32(data + 28);
    unsigned np = le16(data + 44);
    for (unsigned i = 0; i < np; i++) {
        const uint8_t *p = data + ph + i * 32;
        uint32_t addr = le32(p + 8), mem = le32(p + 20);
        if (le32(p) != 1)
            continue;
        if (addr + mem > s.image_size)
            s.image_size = addr + mem;
        if (le32(p + 24) & 1) {
            s.exec[s.nexec].start = (uintptr_t)s.elf.psegment + addr;
            s.exec[s.nexec++].end = (uintptr_t)s.elf.psegment + addr + mem;
        }
    }
    ls_lap_api_set_image(s.elf.psegment, s.image_size);
    heap_caps_free(data);
    data = NULL;
    fclose(f);
    f = NULL;
    if (!ls_lap_api_files_load(LS_LAP_DATA_ROOT)) {
        snprintf(s.work_error, sizeof(s.work_error), "%s", ls_lap_api_error());
        goto done;
    }
    memcpy(s.work_hash, hash, 32);
    s.work_signed = signed_app;
    ok = true;
done:
    if (f)
        fclose(f);
    /* relocate copies every PT_LOAD, and dynamic shared object tables are disabled. */
    heap_caps_free(data);
    if (!ok) {
        if (!s.work_error[0])
            LS_SNPUTS(t_ls_lap_cannot_read_app_into, s.work_error, sizeof(s.work_error), "cannot read app into PSRAM");
        unload();
    }
    return ok;
}
static void io_service(void) {
    int op = atomic_load_explicit(&operation, memory_order_acquire);
    if (op == IO_LOAD) {
        load();
        atomic_store_explicit(&operation, IO_READY, memory_order_release);
    } else if (op == IO_CLEAN) {
        if (!ls_lap_api_files_flush())
            snprintf(s.work_error, sizeof(s.work_error), "%s", ls_lap_api_error());
        unload();
        atomic_store_explicit(&operation, IO_CLEANED, memory_order_release);
        release_active();
    }
}
static void io_task(void *arg) {
    (void)arg;
    for (;;) {
        static bool busy;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(busy ? 20 : 1000));
        ls_lap_console_expire();
        ls_lap_api_log_service();
        io_service();
        busy = ls_lap_sys_service();
        ls_lap_grants_save(LS_LAP_GRANTS_PATH);   /* only when a grant changed */
    }
}
static void wake_io(void) {
    if (worker)
        xTaskNotifyGive(worker);
}
bool ls_lap_start_worker(void) {
    if (!worker)
        worker = xTaskCreateStaticPinnedToCore(io_task, "lap_io", sizeof(worker_stack), NULL, 2,
                                               worker_stack, &worker_tcb, 0);
    if (worker) {
        ls_lap_api_set_wake(wake_io);
        ls_lap_sys_set_wake(wake_io);
        ls_lap_radio_set_wake(wake_io);
    }
    return worker != NULL;
}
static void request(int op) {
    atomic_store_explicit(&operation, op, memory_order_release);
    xTaskNotifyGive(worker);
}
static void close_active(void) {
    if (s.entered && s.app && s.app->leave && !hidden()) {
        before();
        s.app->leave();
        after();
    }
    if (s.entered)
        ls_lap_sys_end();
    s.entered = false;
    ls_lap_service_set_shown(true);
    ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
    if (atomic_load_explicit(&operation, memory_order_acquire) == IO_READY)
        request(IO_CLEAN);
}
static void ls_lap_select(const char *id) {
    int desired = -1;
    for (int i = 0; i < s.count; i++)
        if (!strcmp(id, s.apps[i].manifest.id))
            desired = i;
    if (hidden() && desired == s.active) {
        /* Its service kept it loaded: the screen comes back as it was. */
        s.desired = desired;
        radio_hold(&s.apps[desired].manifest);
        return;
    }
    if (hidden()) {
        char why[48];
        snprintf(why, sizeof(why), "%.15s opened", desired >= 0 ? s.apps[desired].manifest.name : "an app");
        service_kill(why);
    }
    if (s.visible)
        close_active();
    s.desired = desired;
    if (desired >= 0 && (s.apps[desired].manifest.flags & LAP_CAP_NETWORK))
        s.apps[desired].needs_confirmation = true;
    /* Radio access is asked once per install, then remembered. */
    if (desired >= 0 && !ls_lap_grant_has(&s.apps[desired], s.apps[desired].manifest.flags))
        s.apps[desired].needs_confirmation = true;
    s.opened_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s.status.state = desired >= 0 ? LS_LAP_LOADING : LS_LAP_IDLE;
    if (desired >= 0)
        radio_hold(&s.apps[desired].manifest);
}
static void enter(void) {
    ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
    s.visible = true;
    ls_lap_poll();
}
/* The screen closes with a service running: the app's leave() runs, the
   screen is given back, and the app stays loaded for its tick. */
static bool hide(void) {
    if (!s.entered || s.status.state != LS_LAP_RUNNING || !s.app || !ls_lap_service_on() ||
        ls_lap_api_exit_requested())
        return false;
    if (s.app->leave) {
        before();
        s.app->leave();
        after();
    }
    ls_lap_service_set_shown(false); /* leave() has run: close_active must not run it again */
    if (!ls_lap_service_on())        /* it stopped its service on the way out */
        return false;
    ls_lap_sys_hide();
    ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
    if (!hint_given) {
        hint_given = true;
        ls_lap_service_notice(LS_TEXT(t_ls_lap_keeps_running_stop_in, "keeps running; STOP in RADIOS"));
    }
    return true;
}
static void leave(void) {
    s.visible = false;
    s.desired = -1;
    if (!hide())
        close_active();
    radio_release();
}
bool ls_lap_confirm(const char *id) {
    for (int i = 0; i < s.count; i++)
        if (id && !strcmp(id, s.apps[i].manifest.id)) {
            s.apps[i].needs_confirmation = false;
            ls_lap_grant_set(&s.apps[i], s.apps[i].manifest.flags);
            if (s.visible && s.desired == i && s.status.needs_confirmation) {
                s.status.state = LS_LAP_IDLE;
                s.status.needs_confirmation = false;
            }
            return true;
        }
    return false;
}
void ls_lap_poll(void) {
    if (atomic_load_explicit(&pending, memory_order_acquire)) {
        register_info(&update);
        atomic_store(&pending, false);
        ls_lap_maintenance_end();
    }
    if (s.entered && s.status.state == LS_LAP_RUNNING && s.app) {
        if (ls_lap_service_take_stop())
            service_kill("STOP pressed");
        else if (ls_lap_service_poll() < 0)
            service_kill(ls_lap_service_why()[0] ? ls_lap_service_why() : NULL);
        if (hidden() && ls_lap_api_exit_requested())
            service_kill(NULL);
    }
    if (s.visible && hidden() && s.desired == s.active && s.app) {
        /* Shown again: the app is entered again with everything it kept. */
        ls_lap_service_set_shown(true);
        ls_lap_kit_begin();
        s_entered_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s.app->enter) {
            before();
            s.app->enter();
            after();
        }
    }
    int op = atomic_load_explicit(&operation, memory_order_acquire);
    if (op == IO_CLEANED) {
        if (s.work_error[0])
            snprintf(s.error, sizeof(s.error), "%s", s.work_error);
        atomic_store(&operation, IO_IDLE);
        release_active();
        op = IO_IDLE;
        if (s.status.state != LS_LAP_FAILED || s.desired != s.active)
            s.status.state = LS_LAP_IDLE;
    }
    if (op == IO_READY && (!s.visible || s.desired != s.active) && !hidden()) {
        request(IO_CLEAN);
        return;
    }
    if (op == IO_IDLE && s.visible && s.desired >= 0 && s.status.state != LS_LAP_FAILED) {
        s.active = s.desired;
        atomic_store(&open_slot, s.active);
        ls_lap_info_t *a = &s.apps[s.active];
        s.status = (ls_lap_status_t){LS_LAP_LOADING,        a->manifest.id, s.error,
                                     a->needs_confirmation, a->is_signed,   a->signature_checked};
        if (!a->installed || a->needs_confirmation || !ls_lap_start_worker()) {
            snprintf(s.error, sizeof(s.error), "%s",
                     a->needs_confirmation ? "confirmation required"
                     : !a->installed       ? "app removed; restart to refresh"
                                           : "cannot start app worker");
            s.status.state = LS_LAP_FAILED;
            return;
        }
        int free_gate = 0;
        if (!atomic_compare_exchange_strong(&gate, &free_gate, 1)) {
            LS_SNPUTS(t_ls_lap_app_install_in_progress, s.error, sizeof(s.error), "app install in progress");
            s.status.state = LS_LAP_FAILED;
            return;
        }
        s.error[0] = 0;
        request(IO_LOAD);
        return;
    }
    if (op == IO_READY && s.status.state == LS_LAP_LOADING) {
        if (s.work_error[0]) {
            snprintf(s.error, sizeof(s.error), "%s", s.work_error);
            s.status.state = LS_LAP_FAILED;
            return;
        }
        memcpy(s.apps[s.active].sha256, s.work_hash, 32);
        s.apps[s.active].is_signed = s.work_signed;
        s.apps[s.active].signature_present = s.work_signed;
        s.apps[s.active].signature_checked = true;
        s.status.is_signed = s.work_signed;
        s.status.signature_checked = true;
        before();
        s.app = ((const lap_app_t *(*)(void))(uintptr_t)s.elf.entry)();
        after();
        uintptr_t p = (uintptr_t)s.app, base = (uintptr_t)s.elf.psegment;
        if (!s.app || p % _Alignof(lap_app_t) || p < base || p - base > s.image_size ||
            sizeof(lap_app_t) > s.image_size - (p - base) ||
            memcmp(s.app, &s.apps[s.active].manifest, LS_LAP_MANIFEST_BYTES) ||
            !executable((uintptr_t)s.app->enter) || !executable((uintptr_t)s.app->leave) ||
            !executable((uintptr_t)s.app->draw) || !executable((uintptr_t)s.app->key) ||
            !executable((uintptr_t)s.app->touch)) {
            s.app = NULL;
            LS_SNPUTS(t_ls_lap_app_descriptor_differs_from, s.error, sizeof(s.error), "app descriptor differs from scanned manifest");
            s.status.state = LS_LAP_FAILED;
            request(IO_CLEAN);
            return;
        }
        if (ls_lap_api_exit_requested()) {
            ls_tui_screen_show(0);
            return;
        }
        s.status.state = LS_LAP_RUNNING;
        s.entered = true;
        s_entered_ms = (uint32_t)(esp_timer_get_time() / 1000);
        static const char *const RADIOS[] = {"FM", "P25", "ADS-B", "REC"};
        if (memchr(s.app->radio, 0, sizeof(s.app->radio)))
            for (unsigned i = 0; i < sizeof(RADIOS) / sizeof(RADIOS[0]); i++)
                if (!strcmp(s.app->radio, RADIOS[i]))
                    ls_tui_radio_want(RADIOS[i]);
        ls_lap_set_guard(&guard);
        hint_given = false;
        ls_lap_sys_begin();
        ls_lap_net_begin((s.app->flags & LAP_CAP_NETWORK) != 0);
        ls_lap_radio_begin(s.app);   /* granted: the board asked before loading it */
        if (s.app->enter) {
            before();
            s.app->enter();
            after();
        }
    }
    if (s.entered && s.visible && ls_lap_api_exit_requested())
        ls_tui_screen_show(0);
}
/* A full-screen app has hidden the tab strip, so the board keeps a way
   out on the glass itself: a close button in the top right corner, drawn
   over the app and answered before the app sees the tap. Apps leave the
   CLOSE_W x CLOSE_H cells there clear (lakeshark_app.h says so); a tap
   anywhere in the wider CLOSE_HIT corner counts, so a thumb finds it. */
#define CLOSE_W 7
#define CLOSE_H 3
#define CLOSE_HIT_W 10
#define CLOSE_HIT_H 5
static tui_rect close_rect(void) {
    return tui_rect_make(s.area.x + s.area.w - CLOSE_W, s.area.y, CLOSE_W, CLOSE_H);
}
static tui_rect close_hit(void) {
    return tui_rect_make(s.area.x + s.area.w - CLOSE_HIT_W, s.area.y, CLOSE_HIT_W, CLOSE_HIT_H);
}
static void draw_close(tui_surface *sf) {
    /* A bold X three cells square, in quadrant blocks, on a red tab. */
    const tui_rect r = close_rect();
    const uint8_t at = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_RED);
    tui_fill(sf, r, ' ', at);
    const int x = r.x + (r.w - 3) / 2;
    tui_put_char(sf, r, x, r.y, LS_TUI_QUAD(1, 0, 0, 1), at);
    tui_put_char(sf, r, x + 2, r.y, LS_TUI_QUAD(0, 1, 1, 0), at);
    tui_put_char(sf, r, x + 1, r.y + 1, LS_TUI_QUAD(1, 1, 1, 1), at);
    tui_put_char(sf, r, x, r.y + 2, LS_TUI_QUAD(0, 1, 1, 0), at);
    tui_put_char(sf, r, x + 2, r.y + 2, LS_TUI_QUAD(1, 0, 0, 1), at);
}
/* For a few seconds after an app opens, the top line says which receiver
   it paused, so nobody comes back to P25 wondering why it missed calls. */
#define PAUSED_NOTE_MS 4000
static void draw_paused(tui_surface *sf) {
    if (!s_radio_held[0] || (uint32_t)(esp_timer_get_time() / 1000) - s_entered_ms > PAUSED_NOTE_MS)
        return;
    const char *who = s_radio_held;
    char line[96];
    LS_SNPRINTF(t_ls_lap_s_paused_until_this, line, sizeof(line), " %s PAUSED UNTIL THIS APP CLOSES ", who);
    const tui_rect r = tui_rect_make(s.area.x, s.area.y, s.area.w - CLOSE_W - 1, 1);
    if (r.w < 8)
        return;
    const int n = (int)strlen(line) < r.w ? (int)strlen(line) : r.w;
    tui_fill(sf, tui_rect_make(r.x, r.y, n, 1), ' ', TUI_ATTR(TUI_BLACK, TUI_YELLOW));
    tui_put_str(sf, r, r.x, r.y, line, TUI_ATTR(TUI_BLACK, TUI_YELLOW));
}
static void draw(tui_surface *sf, tui_rect area) {
    s.area = area;
    if (s.status.state == LS_LAP_RUNNING && s.app && s.app->draw) {
        ls_lap_sys_frame(area);
        ls_lap_api_draw_begin(sf, area);
        before();
        s.app->draw();
        after();
        ls_lap_api_draw_end();
        if (ls_tui_screen_is_bare() && !ls_lap_sys_immersive())
            draw_close(sf);
        draw_paused(sf);
        if (ls_lap_api_exit_requested())
            ls_tui_screen_show(0);
    } else {
        const ls_lap_info_t *a = s.desired >= 0 && s.desired < s.count ? &s.apps[s.desired] : NULL;
        const char *crashed = ls_trail_last_native();
        ls_lap_ui_draw(sf, area, a, &s.status, s.error,
                       a && a->signature_present && crashed && !strcmp(crashed, a->manifest.id));
    }
}
/* OPEN on the question releases the app; BACK goes HOME. */
static bool frame_act(ls_lap_ui_act_t act) {
    if (act == LS_LAP_UI_OPEN && s.desired >= 0 && s.desired < s.count) {
        ls_lap_confirm(s.apps[s.desired].manifest.id);
        return true;
    }
    if (act == LS_LAP_UI_BACK) {
        ls_tui_screen_show(0);
        return true;
    }
    return false;
}
static bool key(ls_tk_t k, char ch) {
    bool used = false;
    if (s.status.state != LS_LAP_RUNNING) {
        if (k == LS_TK_WHEEL_UP || k == LS_TK_WHEEL_DOWN) k = k == LS_TK_WHEEL_UP ? LS_TK_UP : LS_TK_DOWN;
        return frame_act(ls_lap_ui_key(k, ch, &s.status));
    }
    if (s.entered && s.status.state == LS_LAP_RUNNING && s.app && s.app->key && k >= LS_TK_CHAR &&
        k <= LS_TK_WHEEL_DOWN) {
        /* The enum vocabularies are identical through WHEEL_DOWN (API 1.2); ESC is BACK.
           An app that leaves the wheel alone gets it again as UP or DOWN. */
        before();
        used = s.app->key((lap_key_t)k, ch);
        if (!used && (k == LS_TK_WHEEL_UP || k == LS_TK_WHEEL_DOWN))
            used = s.app->key(k == LS_TK_WHEEL_UP ? LAP_KEY_UP : LAP_KEY_DOWN, 0);
        after();
    }
    if (ls_lap_api_exit_requested()) {
        ls_tui_screen_show(0);
        return true;
    }
    return used;
}
static bool touch(int col, int row) {
    bool used = false;
    if (s.status.state != LS_LAP_RUNNING)
        return frame_act(ls_lap_ui_touch(col, row));
    if (s.entered && s.status.state == LS_LAP_RUNNING && s.app && s.app->touch) {
        /* Router supplies absolute cells. area from last draw owns the origin. */
        if (!tui_rect_contains(s.area, col, row))
            return false;
        if (ls_tui_screen_is_bare() && !ls_lap_sys_immersive() && tui_rect_contains(close_hit(), col, row)) {
            ls_tui_screen_show(0);
            return true;
        }
        before();
        used = s.app->touch(col - s.area.x, row - s.area.y);
        after();
    }
    if (ls_lap_api_exit_requested()) {
        ls_tui_screen_show(0);
        return true;
    }
    return used;
}
const ls_tui_screen_t ls_lap_screen = {.name = "APP",
                                       .hint = "ESC  HOME",
                                       .enter = enter,
                                       .leave = leave,
                                       .draw = draw,
                                       .key = key,
                                       .touch = touch,
                                       .wheel = true};
#else
int ls_lap_scan_file(const char *path) {
    (void)path;
    return -1;
}
bool ls_lap_inspect_file(const char *path, ls_lap_info_t *out, char *why, size_t cap) {
    (void)path;
    (void)out;
    if (why && cap)
        snprintf(why, cap, "native apps need the board");
    return false;
}
const char *ls_lap_last_error(void) { return "native apps need the board"; }
const ls_lap_info_t *ls_lap_at(int i) {
    (void)i;
    return NULL;
}
const ls_lap_info_t *ls_lap_by_id(const char *id) {
    (void)id;
    return NULL;
}
const char (*ls_lap_icon_art(const char *id))[LAP_ICON_W + 1] {
    (void)id;
    return NULL;
}
const ls_lap_status_t *ls_lap_status(void) {
    static const ls_lap_status_t status = {.state = LS_LAP_FAILED,
                                           .reason = "native apps need the board"};
    return &status;
}
const char *ls_lap_last_crash(void) { return NULL; }
bool ls_lap_confirm(const char *id) {
    (void)id;
    return false;
}
bool ls_lap_is_open(const char *id) {
    (void)id;
    return false;
}
unsigned ls_lap_stack_unused(void) { return 0; }
void ls_lap_poll(void) {}
void ls_lap_console_register(void) {}
bool ls_lap_catalog_busy(void) { return false; }
bool ls_lap_maintenance_begin(const char *id) {
    (void)id;
    return false;
}
void ls_lap_maintenance_end(void) {}
bool ls_lap_publish(const ls_lap_info_t *i) {
    (void)i;
    return false;
}
void ls_lap_console_expire(void) {}
#endif
