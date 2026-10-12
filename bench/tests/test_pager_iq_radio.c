/* Exercise the ESP adapter itself with a controlled LR2021 stream task.
 * No radio hardware, experiment-layer replacement, or private adapter access.
 * Unlike the P25 queue, this stream ABI has no generation argument: session
 * isolation is checked by ending with queued data and starting at pair zero. */
#include "pager_iq_radio.h"
#include "pager_iq.h"
#include "ls_lora.h"
#include "ls_lora_lr20xx.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
int ls_shim_log_enabled;
static ls_lora_cfg_t saved, current;
static ls_lora_chip_t chip;
static unsigned allocations, allocation_calls, fail_allocation, config_calls;
static UBaseType_t priority, original_priority;
static bool fail_create, no_saved, startup_overflow;
static esp_err_t config_error, receive_error, stream_error, restore_error;
static atomic_bool stream_active, completed, silent, stopping;
static atomic_int command, acknowledged;
static atomic_uint delays;
static uint64_t next_pair;
static size_t next_bytes;
static int64_t now;
/* Advance startup's virtual one-second deadline in 100 owner yields. */
#define TEST_TICK_US 10000
static TaskFunction_t task_fn;
static void *task_arg;
static bool task_created;
#ifdef _WIN32
static HANDLE thread;
static void pause_thread(void) { Sleep(1); }
static DWORD WINAPI run_task(LPVOID unused)
#else
static pthread_t thread;
static void pause_thread(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static void *run_task(void *unused)
#endif
{
    (void)unused;
    task_fn(task_arg);
    atomic_store(&completed, true);
    return 0;
}

void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{
    CHECK(caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_SPIRAM)); /* queue blocks stay in PSRAM */
    if (++allocation_calls == fail_allocation) return NULL;
    void *p = calloc(n, size);
    CHECK(p); allocations++;
    return p;
}
void heap_caps_free(void *p)
{
    /* Also catches restoration/freeing while the producer is unwinding. */
    CHECK(!atomic_load(&stream_active));
    if (p) { CHECK(allocations); allocations--; free(p); }
}
UBaseType_t uxTaskPriorityGet(TaskHandle_t task) { (void)task; return priority; }
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t p) { (void)task; priority = p; }
void vTaskDelete(TaskHandle_t task) { CHECK(!task); }
void vTaskDelay(TickType_t ticks)
{
    now += (int64_t)ticks * TEST_TICK_US;
    atomic_fetch_add(&delays, 1);
    pause_thread();
}
int64_t esp_timer_get_time(void) { return now; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
        uint32_t stack, void *arg, UBaseType_t p, TaskHandle_t *handle, BaseType_t core)
{
    (void)name; (void)stack; (void)handle; (void)core;
    CHECK(p == configMAX_PRIORITIES - 1);
    if (fail_create) return pdFAIL;
    task_fn = fn; task_arg = arg; task_created = true;
#ifdef _WIN32
    thread = CreateThread(NULL, 0, run_task, NULL, 0, NULL);
    CHECK(thread);
#else
    CHECK(pthread_create(&thread, NULL, run_task, NULL) == 0);
#endif
    if (startup_overflow)
        while (!atomic_load(&completed)) pause_thread();
    return pdPASS;
}
ls_lora_chip_t ls_lora_chip(void) { return chip; }
uint32_t ls_lora_caps(void) { return 0; }
const ls_lora_cfg_t *ls_lora_cfg(void) { return no_saved ? NULL : &current; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { *out = saved; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *cfg)
{
    CHECK(!atomic_load(&stream_active));
    current = *cfg;
    return ++config_calls == 1 ? config_error : restore_error;
}
esp_err_t ls_lora_receive(void) { return receive_error; }
bool lr20xx_stream_source2_active(void) { return atomic_load(&stream_active); }

esp_err_t lr20xx_stream_source2_live(lr20xx_iq_sink_t sink, void *arg,
                                     lr20xx_stream_result_t *result)
{
    uint8_t data[1280];
    /* Signed little-endian IQ16, as in the radio and DSP fixtures. */
    for (unsigned k = 0; k < sizeof(data); k += 4) {
        data[k] = 0; data[k+1] = 32; data[k+2] = 0; data[k+3] = 16;
    }
    atomic_store(&stream_active, true);
    if (stream_error != ESP_OK) goto finish;
    if (!atomic_load(&silent)) CHECK(sink(arg, data, sizeof(data), 0));
    if (startup_overflow) {
        for (unsigned i = 1; i <= 32; i++)
            CHECK(sink(arg, data, sizeof(data), (uint64_t)i * 320) == (i < 32));
        goto finish;
    }
    for (;;) {
        int count = atomic_exchange(&command, 0);
        if (count) {
            for (int i = 0; i < count; i++) {
                if (!sink(arg, data, next_bytes, next_pair + (uint64_t)i * 320)) {
                    atomic_store(&acknowledged, 1);
                    goto unwind;
                }
            }
            atomic_store(&acknowledged, 1);
        }
        /* During timeout/end, keep offering a block until stop is observed. */
        if ((atomic_load(&stopping) ||
             (atomic_load(&silent) && atomic_load(&delays) > 1000000 / TEST_TICK_US)) &&
            !sink(arg, data, sizeof(data), 320)) goto unwind;
        pause_thread();
    }
unwind:;
    /* Keep the radio and callback context live across three owner yields.
     * Removing end's wait deterministically trips the free/configure guards. */
    unsigned at = atomic_load(&delays);
    while (atomic_load(&delays) - at < 3) pause_thread();
    CHECK(allocations == 2);
    result->max_fifo_level = 768;
finish:
    atomic_store(&stream_active, false);
    return stream_error;
}

static void reset(void)
{
    CHECK(!allocations && !task_created);
    memset(&saved, 0, sizeof(saved));
    saved.freq_hz = 915000000; saved.sf = 11; saved.bw_hz = 125000;
    saved.invert_iq = true; saved.cal_min_mhz = 902; saved.cal_max_mhz = 928;
    current = saved; chip = LS_LORA_CHIP_LR20XX;
    priority = original_priority = 4;
    allocation_calls = fail_allocation = config_calls = 0;
    fail_create = no_saved = startup_overflow = false;
    config_error = receive_error = stream_error = restore_error = ESP_OK;
    atomic_store(&stream_active, false); atomic_store(&completed, false);
    atomic_store(&silent, false); atomic_store(&stopping, false);
    atomic_store(&command, 0); atomic_store(&acknowledged, 0);
    atomic_store(&delays, 0); now = 0;
}
static pgr_iq_radio_stats_t stats(void)
{
    pgr_iq_radio_stats_t s; pgr_iq_radio_stats(&s); return s;
}
static void released(void)
{
    CHECK(!allocations && !lr20xx_stream_source2_active());
    CHECK(priority == original_priority);
    CHECK(memcmp(&current, &saved, sizeof(saved)) == 0);
    CHECK(!stats().active);
    float out[64]; CHECK(pgr_iq_radio_read(out, 64) == -1);
}
static void finish(void)
{
    atomic_store(&stopping, true);
    pgr_iq_radio_end();
    if (task_created) {
#ifdef _WIN32
        CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0); CloseHandle(thread);
#else
        CHECK(pthread_join(thread, NULL) == 0);
#endif
        CHECK(atomic_load(&completed)); task_created = false;
    }
    released();
    unsigned calls = config_calls;
    pgr_iq_radio_end(); CHECK(config_calls == calls);
}
static void start(void)
{
    CHECK(pgr_iq_radio_begin(433920000) == ESP_OK);
    CHECK(stats().active && allocations == 2);
    CHECK(priority == (original_priority < 10 ? 10 : original_priority));
    CHECK(current.freq_hz == 433920000 && current.sf == 7 &&
          current.bw_hz == 31250 && !current.invert_iq);
}
static void send_blocks(int count, uint64_t first, size_t bytes)
{
    next_pair = first; next_bytes = bytes;
    atomic_store(&acknowledged, 0); atomic_store(&command, count);
    unsigned tries = 0;
    while (!atomic_load(&acknowledged)) { CHECK(++tries < 5000); pause_thread(); }
}
static void rejected(esp_err_t error, unsigned blocks)
{
    float out[64], before[64];
    for (unsigned i = 0; i < 64; i++) out[i] = 12345.0f;
    memcpy(before, out, sizeof(out));
    CHECK(pgr_iq_radio_read(out, 64) == -1);
    CHECK(memcmp(out, before, sizeof(out)) == 0);
    CHECK(pgr_iq_radio_read(out, 64) == -1);
    CHECK(stats().blocks == blocks && stats().errors == 1 && stats().gaps == 1);
    CHECK(stats().error == error);
    finish(); CHECK(stats().error == error);
}

int main(void)
{
    float out[64];
    reset(); start();
    CHECK(pgr_iq_radio_begin(433920000) == ESP_ERR_INVALID_STATE);
    CHECK(pgr_iq_radio_read(out, 64) > 0);
    CHECK(pgr_iq_radio_read(out, 64) == 0);
    CHECK(stats().blocks == 1 && stats().bytes == 1280);
    send_blocks(1, 320, 1280); CHECK(pgr_iq_radio_read(out, 64) > 0);
    send_blocks(1, 640, 1280); /* Leave a block queued at end. */
    finish(); CHECK(stats().max_fifo_level == 768 && stats().errors == 0);
    reset(); original_priority = priority = 12; no_saved = true; start();
    CHECK(pgr_iq_radio_read(out, 64) > 0); /* Fresh session starts at zero. */
    CHECK(stats().blocks == 1); finish();
    puts("pager: stop-before-free, configuration/priority restoration, restart isolation");

    const uint64_t wrong[] = {0, 319, 321, UINT64_MAX};
    for (unsigned i = 0; i < sizeof(wrong)/sizeof(wrong[0]); i++) {
        reset(); start(); CHECK(pgr_iq_radio_read(out, 64) > 0);
        send_blocks(1, wrong[i], 1280); rejected(ESP_ERR_INVALID_RESPONSE, 1);
    }
    reset(); start(); send_blocks(32, 320, 1280);
    rejected(ESP_ERR_NO_MEM, 0); CHECK(stats().queue_peak == 32);
    reset(); start(); send_blocks(1, 320, 3); rejected(ESP_ERR_INVALID_SIZE, 0);
    reset(); start(); CHECK(pgr_iq_radio_read(NULL, 64) == -1);
    rejected(ESP_ERR_INVALID_ARG, 0);
    reset(); start(); CHECK(pgr_iq_radio_read(out, 63) == -1);
    rejected(ESP_ERR_INVALID_ARG, 0);
    puts("pager: discontinuities, overflow, malformed blocks and invalid reads rejected");

    for (unsigned mode = 0; mode < 6; mode++) {
        reset();
        esp_err_t expected = ESP_FAIL;
        if (mode == 0) config_error = ESP_FAIL;
        if (mode == 1) receive_error = ESP_FAIL;
        if (mode == 2) { fail_create = true; expected = ESP_ERR_NO_MEM; }
        if (mode == 3) stream_error = ESP_FAIL;
        if (mode == 4) { atomic_store(&silent, true); expected = ESP_ERR_TIMEOUT; }
        if (mode == 5) { startup_overflow = true; expected = ESP_ERR_NO_MEM; }
        esp_err_t actual = pgr_iq_radio_begin(433920000);
        if (actual != expected)
            fprintf(stderr, "startup case %u: expected %x, got %x\n", mode,
                    (unsigned)expected, (unsigned)actual);
        CHECK(actual == expected);
        released(); /* begin's failure must clean up without a caller end. */
        finish(); CHECK(config_calls == 2 && stats().error == expected);
        CHECK(stats().errors == 1);
        /* A new capture can acquire the released adapter after each failure. */
        reset(); start(); finish();
    }
    for (unsigned allocation = 1; allocation <= 2; allocation++) {
        reset(); fail_allocation = allocation;
        CHECK(pgr_iq_radio_begin(433920000) == ESP_ERR_NO_MEM); released(); finish();
        CHECK(config_calls == 0);
    }
    reset(); chip = LS_LORA_CHIP_SX126X;
    CHECK(pgr_iq_radio_begin(433920000) == ESP_ERR_NOT_SUPPORTED); finish();
    reset(); CHECK(pgr_iq_radio_begin(1) == ESP_ERR_INVALID_ARG); finish();
    reset(); atomic_store(&stream_active, true);
    CHECK(pgr_iq_radio_begin(433920000) == ESP_ERR_INVALID_STATE);
    CHECK(!allocation_calls && !config_calls && priority == original_priority);
    atomic_store(&stream_active, false); finish();
    reset(); start(); restore_error = ESP_FAIL; finish();
    CHECK(stats().recovery_error == ESP_FAIL && stats().error == ESP_FAIL);
    puts("pager: startup/allocation/preflight failures and restore error release resources");
    return 0;
}
