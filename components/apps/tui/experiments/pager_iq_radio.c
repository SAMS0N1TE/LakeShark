#include "esp_attr.h"
#include "pager_iq_radio.h"
#include "pager_iq.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "ls_lora.h"
#include "lr_iq_spectrum.h"
#include "ls_lora_lr20xx.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

#define QUEUE_SLOTS 32u
#define BLOCK_BYTES 1280u
typedef struct {
    uint64_t first_pair;
    size_t bytes;
    uint8_t data[BLOCK_BYTES];
} iq_block_t;
typedef struct {
    atomic_uint write_at, read_at, peak;
    atomic_bool stop, done;
    atomic_int sink_error;
    iq_block_t *blocks;
    lr20xx_stream_result_t result;
    esp_err_t error;
    ls_lora_cfg_t saved;
    pgr_iq_t frontend;
    uint64_t expected;
    UBaseType_t owner_priority;
    bool configured, failed;
} capture_t;
static capture_t *s_capture;
static pgr_iq_radio_stats_t s_stats;

static bool IRAM_ATTR collect(void *arg, const uint8_t *data, size_t bytes, uint64_t first)
{
    capture_t *c = arg;
    if (atomic_load_explicit(&c->stop, memory_order_relaxed)) return false;
    if (!data || !bytes || bytes > BLOCK_BYTES || bytes % 4) {
        atomic_store(&c->sink_error, ESP_ERR_INVALID_SIZE);
        return false;
    }
    const unsigned w = atomic_load_explicit(&c->write_at, memory_order_relaxed);
    const unsigned r = atomic_load_explicit(&c->read_at, memory_order_acquire);
    if (w - r >= QUEUE_SLOTS) {
        atomic_store(&c->sink_error, ESP_ERR_NO_MEM);
        return false;
    }
    iq_block_t *b = &c->blocks[w & (QUEUE_SLOTS - 1u)];
    b->first_pair = first;
    b->bytes = bytes;
    memcpy(b->data, data, bytes);
    if (w - r + 1 > atomic_load_explicit(&c->peak, memory_order_relaxed))
        atomic_store_explicit(&c->peak, w - r + 1, memory_order_relaxed);
    atomic_store_explicit(&c->write_at, w + 1, memory_order_release);
    return true;
}

static void producer(void *arg)
{
    capture_t *c = arg;
    c->error = lr20xx_stream_source2_live(collect, c, &c->result);
    /* The owner may free c after this publication. */
    atomic_store_explicit(&c->done, true, memory_order_release);
    vTaskDelete(NULL);
}

static void fail(capture_t *c, esp_err_t error)
{
    if (!c->failed) {
        c->failed = true;
        s_stats.gaps++;
        s_stats.errors++;
        s_stats.error = error;
    }
    atomic_store(&c->stop, true);
}

bool pgr_iq_radio_supported(void)
{
    /* ROM/version validation remains in the raw-IQ driver. */
    return ls_lora_chip() == LS_LORA_CHIP_LR20XX;
}

void pgr_iq_radio_stats(pgr_iq_radio_stats_t *out)
{
    if (!out) return;
    capture_t *c = s_capture;
    if (c) {
        s_stats.queue_peak = atomic_load(&c->peak);
        if (atomic_load_explicit(&c->done, memory_order_acquire)) {
            s_stats.max_fifo_level = c->result.max_fifo_level;
            s_stats.fifo_flags = c->result.fifo_flags;
            s_stats.recovery_error = c->result.recovery_error;
        }
    }
    *out = s_stats;
}

void pgr_iq_radio_end(void)
{
    capture_t *c = s_capture;
    if (!c) return;
    atomic_store(&c->stop, true);
    while (!atomic_load_explicit(&c->done, memory_order_acquire)) vTaskDelay(1);
    if (c->error != ESP_OK) fail(c, c->error);
    const int sink_error = atomic_load(&c->sink_error);
    if (sink_error != ESP_OK) fail(c, sink_error);
    pgr_iq_radio_stats(&s_stats);
    if (c->configured) {
        const esp_err_t e = ls_lora_configure(&c->saved);
        if (e != ESP_OK) {
            s_stats.recovery_error = e;
            fail(c, e);
        }
    }
    ESP_LOGI("PGRIQ", "end err=%x capture=%x sink=%x flags=%02x fifo=%lu queue=%lu recovery=%x service_us=%ld",
             (unsigned)s_stats.error, (unsigned)c->error, (unsigned)sink_error,
             c->result.fifo_flags, (unsigned long)c->result.max_fifo_level,
             (unsigned long)s_stats.queue_peak, (unsigned)s_stats.recovery_error,
             (long)c->result.max_service_gap_us);
    lr_spec_end();
    s_stats.active = false;
    s_capture = NULL;
    const UBaseType_t priority = c->owner_priority;
    heap_caps_free(c->blocks);
    heap_caps_free(c);
    vTaskPrioritySet(NULL, priority);
}

esp_err_t pgr_iq_radio_begin(uint32_t hz)
{
    if (s_capture || lr20xx_stream_source2_active()) return ESP_ERR_INVALID_STATE;
    memset(&s_stats, 0, sizeof(s_stats));
    if (!pgr_iq_radio_supported()) return ESP_ERR_NOT_SUPPORTED;
    if (!ls_lora_packet_rx_hz_ok(ls_lora_caps(), hz)) return ESP_ERR_INVALID_ARG;
    capture_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!c) return ESP_ERR_NO_MEM;
    /* The producer must never wait on PSRAM cache/writeback while draining IQ. */
    c->blocks = heap_caps_calloc(QUEUE_SLOTS, sizeof(*c->blocks), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->blocks) { heap_caps_free(c); return ESP_ERR_NO_MEM; }
    atomic_init(&c->write_at, 0); atomic_init(&c->read_at, 0); atomic_init(&c->peak, 0);
    atomic_init(&c->stop, false); atomic_init(&c->done, true);
    atomic_init(&c->sink_error, ESP_OK);
    if (!atomic_is_lock_free(&c->write_at) || !atomic_is_lock_free(&c->stop)) {
        heap_caps_free(c->blocks); heap_caps_free(c); return ESP_ERR_NOT_SUPPORTED;
    }
    const ls_lora_cfg_t *saved = ls_lora_cfg();
    if (saved) c->saved = *saved; else ls_lora_cfg_default(&c->saved);
    ls_lora_cfg_t cfg = c->saved;
    cfg.freq_hz = hz; cfg.sf = 7; cfg.bw_hz = 31250; cfg.invert_iq = false;
    s_capture = c;
    c->owner_priority = uxTaskPriorityGet(NULL);
    /* Match the P25 consumer while this owner drains raw I/Q. */
    if (c->owner_priority < 10) vTaskPrioritySet(NULL, 10);
    c->configured = true;
    esp_err_t e = ls_lora_configure(&cfg);
    if (e == ESP_OK) e = ls_lora_receive();
    if (e != ESP_OK) { fail(c, e); pgr_iq_radio_end(); return e; }
    pgr_iq_init(&c->frontend);
    atomic_store(&c->done, false);
    /* Hosted transport workers run at 23. Resume FIFO draining ahead of
       them after the driver's one-tick yield; another tick can overflow. */
    if (xTaskCreatePinnedToCore(producer, "pager_iq_rx", 4096, c,
                              configMAX_PRIORITIES - 1, NULL, 0) != pdPASS) {
        atomic_store(&c->done, true);
        fail(c, ESP_ERR_NO_MEM); pgr_iq_radio_end(); return ESP_ERR_NO_MEM;
    }
    const int64_t deadline = esp_timer_get_time() + 1000000;
    while (!atomic_load_explicit(&c->write_at, memory_order_acquire) &&
           !atomic_load_explicit(&c->done, memory_order_acquire) &&
           esp_timer_get_time() < deadline) vTaskDelay(1);
    if (atomic_load_explicit(&c->done, memory_order_acquire) || !atomic_load(&c->write_at)) {
        e = atomic_load(&c->sink_error);
        if (e == ESP_OK)
            e = atomic_load(&c->done) && c->error != ESP_OK ? c->error : ESP_ERR_TIMEOUT;
        fail(c, e); pgr_iq_radio_end(); return e;
    }
    lr_spec_begin(hz);
    s_stats.active = true;
    return ESP_OK;
}

int pgr_iq_radio_read(float *out, size_t capacity)
{
    capture_t *c = s_capture;
    if (!c) return -1;
    if (!out || capacity < PGR_IQ_RADIO_BLOCK_SAMPLES) {
        fail(c, ESP_ERR_INVALID_ARG); return -1;
    }
    if (c->failed) return -1;
    const int sink_error = atomic_load(&c->sink_error);
    if (sink_error != ESP_OK) { fail(c, sink_error); return -1; }
    if (atomic_load_explicit(&c->done, memory_order_acquire)) {
        fail(c, c->error != ESP_OK ? c->error : ESP_ERR_INVALID_STATE);
        return -1;
    }
    const unsigned r = atomic_load_explicit(&c->read_at, memory_order_relaxed);
    const unsigned w = atomic_load_explicit(&c->write_at, memory_order_acquire);
    if (r == w) return 0;
    const iq_block_t *b = &c->blocks[r & (QUEUE_SLOTS - 1u)];
    if (b->first_pair != c->expected) { fail(c, ESP_ERR_INVALID_RESPONSE); return -1; }
    lr_spec_feed(b->data, b->bytes, b->first_pair, 1,
                 (uint32_t)(esp_timer_get_time() / 1000), w - r <= 1);
    size_t n = 0;
    if (!pgr_iq_process(&c->frontend, b->data, b->bytes, out, capacity, &n)) {
        fail(c, ESP_ERR_INVALID_SIZE); return -1;
    }
    c->expected += b->bytes / 4;
    s_stats.bytes += b->bytes;
    s_stats.blocks++;
    atomic_store_explicit(&c->read_at, r + 1, memory_order_release);
    return (int)n;
}
#else
bool pgr_iq_radio_supported(void) { return false; }
esp_err_t pgr_iq_radio_begin(uint32_t hz) { (void)hz; return ESP_ERR_NOT_SUPPORTED; }
int pgr_iq_radio_read(float *out, size_t capacity) { (void)out; (void)capacity; return -1; }
void pgr_iq_radio_end(void) {}
void pgr_iq_radio_stats(pgr_iq_radio_stats_t *out) { if (out) memset(out, 0, sizeof(*out)); }
#endif
