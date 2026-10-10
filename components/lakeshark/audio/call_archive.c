#include "call_archive.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "ls_gps.h"
#include "ls_time.h"
#include "ls_nvs_safe.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CALL_ROOT "/sdcard/lakeshark/calls"
#define CALL_SLOTS 64
#define CALL_TGS 32
#ifdef ESP_PLATFORM
static EXT_RAM_BSS_ATTR call_recorder_t recorders[CALL_INPUTS];
static EXT_RAM_BSS_ATTR char group_text[192], deletion[CALL_PATH_MAX];
#else
static call_recorder_t recorders[CALL_INPUTS];
static char group_text[192], deletion[CALL_PATH_MAX];
#endif
static call_writer_t *writers;
static call_entry_t *recent, *scanning;
static atomic_int options[CALL_OPTIONS] = {1, 1, 500, 30, 0};
static atomic_uint groups[CALL_TGS], group_count, group_generation, peak, errors;
#ifdef ESP_PLATFORM
static EXT_RAM_BSS_ATTR char protection[CALL_PATH_MAX];
#else
static char protection[CALL_PATH_MAX];
#endif
static bool protect_value;
static atomic_bool ready, refresh, refresh_space, save_options;
#ifdef ESP_PLATFORM
static _Atomic(TaskHandle_t) writer_task;
static void wake_writer(void)
{
    TaskHandle_t task = atomic_load(&writer_task);
    if (task) xTaskNotifyGive(task);
}
#elif defined(CALL_ARCHIVE_HOST_TEST)
static unsigned test_wakes;
static void wake_writer(void) { ++test_wakes; }
#else
static void wake_writer(void) {}
#endif
static void wake_input(call_recorder_t *r, unsigned before)
{
    /* A quiet gate may run many times while older blocks await SD service.
       Only newly published events should notify the consumer. */
    if (atomic_load_explicit(&r->head, memory_order_relaxed) != before) wake_writer();
}
static int recent_count;
static atomic_bool paused;
static call_archive_storage_t storage;
#ifdef ESP_PLATFORM
static portMUX_TYPE list_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&list_lock)
#define UNLOCK() portEXIT_CRITICAL(&list_lock)
static void *allocate(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
static void *allocate(size_t n) { return calloc(1, n); }
#endif
int call_archive_option(call_option_t option)
{
    return (unsigned)option < CALL_OPTIONS ? atomic_load(&options[option]) : 0;
}
void call_archive_set_option(call_option_t option, int value)
{
    if ((unsigned)option >= CALL_OPTIONS) return;
    int max = option == CALL_OPT_MIN_MS ? 10000 : option == CALL_OPT_DAYS ? 365 : option == CALL_OPT_UTC_QUARTERS ? 56 : 1;
    int min = option == CALL_OPT_UTC_QUARTERS ? -48 : 0;
    if (value < min || value > max) return;
    atomic_store(&options[option], value); atomic_store(&save_options, true);
    wake_writer();
}
const char *call_archive_talkgroups(void) { return group_text; }
bool call_archive_set_talkgroups(const char *text)
{
    unsigned parsed[CALL_TGS], count = 0;
    if (strlen(text) >= sizeof(group_text)) return false;
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p) break;
        char *end; unsigned long v = strtoul(p, &end, 10);
        if (p == end || v == 0 || v > 65535 || count == CALL_TGS ||
            (*end && *end != ',' && *end != ' ')) return false;
        parsed[count++] = (unsigned)v; p = end;
    }
    /* A zero count fails closed while a replacement list is published. */
    atomic_fetch_add(&group_generation, 1);
    atomic_store(&group_count, 0);
    for (unsigned i = 0; i < count; ++i) atomic_store(&groups[i], parsed[i]);
    atomic_store(&group_count, count);
    atomic_fetch_add(&group_generation, 1);
    LOCK(); strcpy(group_text, text); UNLOCK();
    atomic_store(&save_options, true); wake_writer(); return true;
}
static bool permitted(call_input_t input, uint32_t tg)
{
    if (!call_archive_option(input == CALL_FM ? CALL_OPT_FM : CALL_OPT_P25)) return false;
    if (input == CALL_FM || !call_archive_option(CALL_OPT_FILTER)) return true;
    unsigned generation = atomic_load(&group_generation);
    if (generation & 1) return false;
    unsigned count = atomic_load(&group_count);
    bool match = false;
    for (unsigned i = 0; i < count; ++i) if (atomic_load(&groups[i]) == tg) match = true;
    return match && generation == atomic_load(&group_generation);
}
static uint32_t now_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000);
#else
    return (uint32_t)((uint64_t)clock() * 1000 / CLOCKS_PER_SEC);
#endif
}
static call_meta_t metadata(call_input_t input, uint32_t hz, uint32_t tg, uint32_t source)
{
    call_meta_t m = {.hz = hz, .talkgroup = tg, .source = source,
        .rate = input == CALL_FM ? 16000 : 8000,
        .minimum_ms = (uint32_t)call_archive_option(CALL_OPT_MIN_MS)};
#ifdef ESP_PLATFORM
    if (ls_time_is_synced()) m.time = time(NULL);
    ls_gps_state_t gps; ls_gps_get(&gps);
    int64_t age = esp_timer_get_time() - gps.last_fix_us;
    if (gps.fix && gps.last_fix_us > 0 && age >= 0 && age <= 5000000) {
        m.gps = true; m.lat = gps.lat_deg; m.lon = gps.lon_deg;
    }
#else
    m.time = time(NULL);
#endif
    return m;
}
void call_archive_audio(call_input_t input, uint32_t hz, uint32_t tg,
                        uint32_t source, bool clear, const int16_t *pcm, unsigned n)
{
    if (!atomic_load(&ready) || (unsigned)input >= CALL_INPUTS) return;
    call_recorder_t *r = &recorders[input];
    unsigned before = atomic_load_explicit(&r->head, memory_order_relaxed);
    if (!clear) { call_recorder_end(r, true); wake_input(r, before); return; }
    if (atomic_load(&paused)) { call_recorder_end(r, false); wake_input(r, before); return; }
    if (!permitted(input, tg)) { call_recorder_end(r, false); wake_input(r, before); return; }
    call_meta_t m = r->current;
    unsigned hang = input == CALL_FM ? 300 : 1500;
    uint32_t now = now_ms();
    if (!atomic_load(&r->active) || m.hz != hz || m.talkgroup != tg ||
        (uint32_t)(now - r->last_open_ms) >= hang)
        m = metadata(input, hz, tg, source);
    call_recorder_gate(r, &m, true, true, now, hang);
    call_recorder_pcm(r, pcm, n);
    wake_input(r, before);
    unsigned high = 0;
    for (unsigned i = 0; i < n; ++i) {
        unsigned v = pcm[i] < 0 ? -(int)pcm[i] : pcm[i]; if (v > high) high = v;
    }
    atomic_store(&peak, high);
}
void call_archive_gate(call_input_t input, uint32_t hz, bool open)
{
    if (!atomic_load(&ready) || (unsigned)input >= CALL_INPUTS) return;
    call_recorder_t *r = &recorders[input];
    unsigned before = atomic_load_explicit(&r->head, memory_order_relaxed);
    call_meta_t m = r->current;
    m.hz = hz;
    /* Opening is paired with PCM, so a gate alone never creates empty files. */
    if (!open || !permitted(input, m.talkgroup))
        call_recorder_gate(r, &m, false, true, now_ms(), input == CALL_FM ? 300 : 1500);
    wake_input(r, before);
}
void call_archive_end(call_input_t input, bool discard)
{
    if (atomic_load(&ready) && (unsigned)input < CALL_INPUTS) {
        call_recorder_t *r = &recorders[input];
        unsigned before = atomic_load_explicit(&r->head, memory_order_relaxed);
        call_recorder_end(r, discard);
        wake_input(r, before);
    }
}
void call_archive_refresh(void)
{
    atomic_store(&refresh_space, true);
    atomic_store(&refresh, true);
    wake_writer();
}
int call_archive_count(void) { LOCK(); int n = recent_count; UNLOCK(); return n; }
bool call_archive_entry(int index, call_entry_t *entry)
{
    LOCK(); bool ok = recent && index >= 0 && index < recent_count;
    if (ok) *entry = recent[index];
    UNLOCK(); return ok;
}
bool call_archive_protect(const char *path,bool protect) {
    if(!atomic_load(&ready) || !path || strlen(path)>=sizeof(protection))return false;
    LOCK();bool ok=!protection[0];if(ok){strcpy(protection,path);protect_value=protect;}UNLOCK();if(ok)wake_writer();return ok;
}
bool call_archive_delete(const char *path)
{
    if (!atomic_load(&ready) || strlen(path) >= sizeof(deletion)) return false;
    LOCK(); bool ok = !deletion[0]; if (ok) strcpy(deletion, path); UNLOCK(); if (ok) wake_writer(); return ok;
}
bool call_archive_busy(void)
{
    if (!atomic_load(&ready)) return false;
    for (unsigned i = 0; i < CALL_INPUTS; ++i)
        if (atomic_load(&recorders[i].active) || atomic_load(&recorders[i].head) != atomic_load(&recorders[i].tail)) return true;
    return false;
}
unsigned call_archive_peak(void) { return atomic_exchange(&peak, 0); }
unsigned call_archive_errors(void) { return atomic_load(&errors); }
unsigned call_archive_drops(void)
{
    unsigned n = 0;
    if (atomic_load(&ready)) for (unsigned i = 0; i < CALL_INPUTS; ++i) n += atomic_load(&recorders[i].drops);
    return n;
}
void call_archive_storage(call_archive_storage_t *out)
{
    LOCK(); *out = storage; UNLOCK();
    out->paused = atomic_load(&paused);
}
#if defined(ESP_PLATFORM) || defined(CALL_ARCHIVE_HOST_TEST)
static void load_options(bool (*read)(void *, int, int32_t *), void *context)
{
    for (int i = 0; i < CALL_OPTIONS; ++i) {
        int32_t value;
        if (read(context, i, &value)) call_archive_set_option(i, value);
    }
}
#endif
#ifdef ESP_PLATFORM
static bool read_option(void *context, int option, int32_t *value)
{
    char key[8]; snprintf(key, sizeof(key), "opt%d", option);
    return nvs_get_i32(*(nvs_handle_t *)context, key, value) == ESP_OK;
}
static esp_err_t preferences(void *context)
{
    bool writing = *(bool *)context;
    nvs_handle_t h;
    esp_err_t result = nvs_open("call_archive", writing ? NVS_READWRITE : NVS_READONLY, &h);
    if (result != ESP_OK) return !writing && result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
    if (!writing) load_options(read_option, &h);
    for (int i = 0; writing && i < CALL_OPTIONS; ++i) {
        char key[8]; snprintf(key, sizeof(key), "opt%d", i);
        int32_t value = call_archive_option(i);
        esp_err_t err = nvs_set_i32(h, key, value); if (err != ESP_OK) result = err;
    }
    if (writing) {
        char text[192]; LOCK(); strcpy(text, group_text); UNLOCK();
        esp_err_t err = nvs_set_str(h, "groups", text); if (err != ESP_OK) result = err;
    }
    else {
        char text[192]; size_t len = sizeof(text);
        if (nvs_get_str(h, "groups", text, &len) == ESP_OK) call_archive_set_talkgroups(text);
    }
    if (writing && result == ESP_OK) result = nvs_commit(h);
    nvs_close(h); return result;
}
#endif
#ifndef CALL_ARCHIVE_HOST_TEST
bool call_archive_card_space(uint64_t *total, uint64_t *free)
{
#ifdef ESP_PLATFORM
    return esp_vfs_fat_info("/sdcard", total, free) == ESP_OK;
#else
    (void)total; (void)free; return false;
#endif
}
#endif
#if defined(ESP_PLATFORM) || defined(CALL_ARCHIVE_HOST_TEST)
static bool service_once(const char *root)
{
    static int64_t retained_day = -1;
    static int retained_keep = -1;
    static uint32_t checked_ms, written;
    static bool checked;
    bool worked = false, changed = false, beginning = false, recording = false;
    char path[CALL_PATH_MAX];
    LOCK(); strcpy(path, deletion); deletion[0] = 0; UNLOCK();
    if (path[0]) {
        if (!call_store_delete(root, path)) atomic_fetch_add(&errors, 1);
        changed = true; atomic_store(&refresh, true);
    }
    LOCK();strcpy(path,protection);protection[0]=0;bool protect=protect_value;UNLOCK();
    if(path[0]) {
        if(!call_store_protect(root,path,protect))atomic_fetch_add(&errors,1);
        changed=true;atomic_store(&refresh, true);
    }
    int keep = call_archive_option(CALL_OPT_DAYS);
#ifdef ESP_PLATFORM
    int64_t day = ls_time_is_synced() ? time(NULL) / 86400 : -1;
#else
    int64_t day = time(NULL) / 86400;
#endif
    if (day >= 0 && (day != retained_day || keep != retained_keep)) {
        call_store_retain(root, day, keep);
        retained_day = day; retained_keep = keep;
        changed = true; atomic_store(&refresh, true);
    }
    for (unsigned i = 0; i < CALL_INPUTS; ++i) {
        const call_block_t *b = call_recorder_peek(&recorders[i]);
        beginning |= b && b->event == CALL_BEGIN;
        recording |= writers[i].file != NULL;
    }
    uint32_t now = now_ms();
    /* f_getfree can scan every FAT cluster on its first invocation. Do not
       start that scan merely because a quiet receiver initialized us. The
       first queued BEGIN still checks space before opening any recording;
       the browser explicitly requests space along with its inventory. */
    bool space_requested = atomic_exchange(&refresh_space, false);
    if (beginning || space_requested || (changed && checked) ||
        written >= 65536 || ((recording || atomic_load(&paused)) &&
        (uint32_t)(now - checked_ms) >= 1000)) {
        uint64_t total = 0, free = 0;
        bool valid = call_archive_card_space(&total, &free) && total && free <= total;
        uint64_t floor = total / 20;
        if (floor < 1000000000ULL) floor = 1000000000ULL;
        bool low = !valid || free < floor;
        bool was_low = atomic_exchange(&paused, low);
        if (low != was_low) atomic_store(&refresh, true);
        LOCK(); storage.valid = valid; storage.total = total; storage.free = free; UNLOCK();
        checked = true; checked_ms = now; written = 0;
        if (low) for (unsigned i = 0; i < CALL_INPUTS; ++i) call_store_close(&writers[i]);
    }
    for (unsigned i = 0; i < CALL_INPUTS; ++i) {
        const call_block_t *b = call_recorder_peek(&recorders[i]);
        bool ending = b && (b->event == CALL_END || b->event == CALL_ABORT);
        if (atomic_load(&paused)) {
            if (b) { call_recorder_consume(&recorders[i]); worked = true; }
        } else {
            uint32_t before = writers[i].bytes;
            worked |= call_store_step(&writers[i], &recorders[i], root);
            if (writers[i].bytes > before) written += writers[i].bytes - before;
        }
        if (ending) atomic_store(&refresh, true);
    }
    if (!worked) {
#ifdef ESP_PLATFORM
        if (atomic_exchange(&save_options, false)) {
            bool writing = true;
            if (ls_nvs_run(preferences, &writing, 3072) != ESP_OK) atomic_fetch_add(&errors, 1);
        }
#endif
        if (atomic_exchange(&refresh, false)) {
            uint64_t bytes = call_store_size(root);
            int n = call_store_scan(root, scanning, CALL_RECENT_MAX);
            LOCK(); call_entry_t *swap = recent; recent = scanning; scanning = swap;
            recent_count = n; storage.archive = bytes; UNLOCK();
        }
    }
    unsigned total = 0;
    for (unsigned i = 0; i < CALL_INPUTS; ++i) total += writers[i].errors;
    static unsigned previous;
    if (total > previous) atomic_fetch_add(&errors, total - previous);
    previous = total;
    return worked;
}
#endif
#ifdef ESP_PLATFORM
static void worker(void *arg)
{
    (void)arg;
    bool directory_ready = false;
    uint32_t directory_checked = 0;
    for (;;) {
        /* No directory lookup or free-space query on a healthy idle card.
           A missing card retries once a second; PCM and commands wake us. */
        uint32_t now = now_ms();
        if (!directory_ready && (!directory_checked || (uint32_t)(now - directory_checked) >= 1000)) {
            directory_ready = !mkdir("/sdcard/lakeshark", 0775) || !access("/sdcard/lakeshark", F_OK);
            directory_checked = now;
        }
        bool worked = service_once(CALL_ROOT);
        if (worked) vTaskDelay(1);
        else ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}
#endif
#ifdef CALL_ARCHIVE_HOST_TEST
void call_archive_test_load_options(bool (*read)(void *, int, int32_t *), void *context)
{
    load_options(read, context);
}
bool call_archive_test_pump(const char *root)
{
    return service_once(root);
}
unsigned call_archive_test_wakes(void) { return test_wakes; }
#endif

void call_archive_init(void)
{
    if (atomic_load(&ready)) return;
    writers = allocate(sizeof(*writers) * CALL_INPUTS);
    recent = allocate(sizeof(*recent) * CALL_RECENT_MAX);
    scanning = allocate(sizeof(*scanning) * CALL_RECENT_MAX);
    bool ok = writers && recent && scanning;
    for (unsigned i = 0; i < CALL_INPUTS; ++i) {
        call_block_t *blocks = allocate(sizeof(*blocks) * CALL_SLOTS);
        call_recorder_init(&recorders[i], blocks, CALL_SLOTS); ok &= blocks != NULL;
    }
#ifdef ESP_PLATFORM
    bool writing = false;
    if (ls_nvs_run(preferences, &writing, 3072) != ESP_OK) atomic_fetch_add(&errors, 1);
    atomic_store(&save_options, false);
    /* The writer stack lives in PSRAM; only its small TCB uses internal RAM. */
    static StaticTask_t tcb;
    StackType_t *stack = allocate(6144);
    if (ok && stack) {
        writer_task = xTaskCreateStaticPinnedToCore(worker, "call_sd", 6144,
            NULL, 2, stack, &tcb, 0);
        ok = writer_task != NULL;
    }
    else ok = false;
    if (!ok) heap_caps_free(stack);
#endif
    if (!ok) {
#ifdef ESP_PLATFORM
#define RELEASE(p) heap_caps_free(p)
#else
#define RELEASE(p) free(p)
#endif
        RELEASE(writers); RELEASE(recent); RELEASE(scanning);
        writers = NULL; recent = NULL; scanning = NULL;
        for (unsigned i = 0; i < CALL_INPUTS; ++i) { RELEASE(recorders[i].blocks); recorders[i].blocks = NULL; }
        atomic_fetch_add(&errors, 1); return;
    }
    /* Inventory/retention is independent of a potentially cold FAT space
       scan. Neither an empty archive nor quiet decoder gates need it. */
    atomic_store(&ready, true);
    atomic_store(&refresh, true);
    wake_writer();
}
