#include "esp_attr.h"
#include "lr_wf_session.h"
#include "lr_iq_spectrum.h"
#include "lr2021_iq_queue.h"
#include "ls_lora.h"
#include "ls_lora_lr20xx.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    lr2021_iq_queue queue;
    atomic_bool stop, done, capture_stop, capture_done;
    lr20xx_stream_result_t result;
    esp_err_t error, capture_error;
    uint32_t hz;
    lr_wf_consumer_t consume;
    void *ctx;
    atomic_uint lost_pairs, gaps;
} session_t;
static session_t *session;

static bool IRAM_ATTR collect(void *arg, const uint8_t *data, size_t bytes, uint64_t first)
{
    session_t *s = arg;
    if (atomic_load(&s->capture_stop)) return false;
    if (lr2021_iq_queue_push(&s->queue, data, bytes, first, 1)) return true;
    atomic_fetch_add(&s->lost_pairs, bytes / 4);
    atomic_fetch_add(&s->gaps, 1);
    return false; /* producer stops; next session starts a fresh generation */
}
static void capture(void *arg)
{
    session_t *s = arg;
    s->capture_error = lr20xx_stream_source2_live(collect, s, &s->result);
    atomic_store_explicit(&s->capture_done, true, memory_order_release);
    vTaskDelete(NULL);
}
static void owner(void *arg)
{
    session_t *s = arg;
    ls_lora_cfg_t saved;
    const ls_lora_cfg_t *previous = ls_lora_cfg();
    if (previous) saved = *previous; else ls_lora_cfg_default(&saved);
    ls_lora_cfg_t cfg = saved;
    cfg.freq_hz = s->hz; cfg.sf = 7; cfg.bw_hz = 31250; cfg.invert_iq = false;
    s->error = ls_lora_configure(&cfg);
    if (s->error == ESP_OK) s->error = ls_lora_receive();
    bool confirmed = false;
    if (s->error != ESP_OK) goto restore;
    atomic_store(&s->capture_done, false);
    if (xTaskCreatePinnedToCore(capture, "wf_iq_rx", 4096, s,
                               configMAX_PRIORITIES - 1, NULL, 0) != pdPASS) {
        atomic_store(&s->capture_done, true);
        s->error = ESP_ERR_NO_MEM; goto restore;
    }
    uint64_t expected = 0;
    int64_t deadline = esp_timer_get_time() + 1000000;
    while (!atomic_load(&s->stop) && !atomic_load_explicit(&s->capture_done, memory_order_acquire)) {
        const lr2021_iq_block *b = lr2021_iq_queue_peek(&s->queue);
        if (!b) {
            if (esp_timer_get_time() > deadline) { s->error = ESP_ERR_TIMEOUT; break; }
            vTaskDelay(1); continue;
        }
        if (b->generation != 1 || b->first_pair != expected) {
            atomic_fetch_add(&s->gaps, 1);
            atomic_fetch_add(&s->lost_pairs, (b->first_pair > expected ? (uint32_t)(b->first_pair-expected) : 0));
            s->error = ESP_ERR_INVALID_RESPONSE; break;
        }
        if (!confirmed) { lr_spec_begin(s->hz); confirmed = true; }
        bool spare = atomic_load(&s->queue.write_index) - atomic_load(&s->queue.read_index) <= 1;
        if (s->consume) s->consume(s->ctx, b->data, b->bytes, spare);
        lr_spec_feed(b->data, b->bytes, b->first_pair, b->generation,
                     (uint32_t)(esp_timer_get_time() / 1000),
                     atomic_load(&s->queue.write_index) - atomic_load(&s->queue.read_index) <= 1);
        expected += b->bytes / 4;
        lr2021_iq_queue_release(&s->queue);
        deadline = esp_timer_get_time() + 1000000;
    }
    atomic_store(&s->capture_stop, true);
    while (!atomic_load_explicit(&s->capture_done, memory_order_acquire)) vTaskDelay(1);
    /* Account for checked blocks abandoned when capture ended. */
    const lr2021_iq_block *left;
    while ((left = lr2021_iq_queue_peek(&s->queue))) {
        atomic_fetch_add(&s->lost_pairs, left->bytes/4);
        lr2021_iq_queue_release(&s->queue);
    }
    if (!atomic_load(&s->stop) && !atomic_load(&s->gaps)) atomic_fetch_add(&s->gaps, 1);
    if (s->consume) s->consume(s->ctx, NULL, 0, false);
    if (s->capture_error != ESP_OK) s->error = s->capture_error;
    if (s->result.recovery_error != ESP_OK) s->error = s->result.recovery_error;
    if (!atomic_load(&s->stop) && s->error == ESP_OK) s->error = ESP_ERR_INVALID_STATE;
restore:
    if (confirmed) lr_spec_end();
    esp_err_t restored = ls_lora_configure(&saved);
    if (restored != ESP_OK) s->error = restored;
    atomic_store_explicit(&s->done, true, memory_order_release);
    vTaskDelete(NULL);
}
void lr_wf_stop(void) { lr_wf_stop_stats(NULL); }
void lr_wf_stop_stats(lr_wf_stats *out)
{
    if (out) *out = (lr_wf_stats){0};
    if (!session) return;
    atomic_store(&session->stop, true);
    while (!atomic_load_explicit(&session->done, memory_order_acquire)) vTaskDelay(1);
    if (out) lr_wf_get_stats(out);
    heap_caps_free(session); session = NULL;
}
esp_err_t lr_wf_start(uint32_t hz) { return lr_wf_start_consumer(hz, NULL, NULL); }
void lr_wf_get_stats(lr_wf_stats *out)
{
    *out = (lr_wf_stats){0};
    if (!session) return;
    out->lost_pairs = atomic_load(&session->lost_pairs);
    out->gaps = atomic_load(&session->gaps);
    out->done = atomic_load_explicit(&session->done, memory_order_acquire);
    out->error = out->done ? session->error : ESP_OK;
}
esp_err_t lr_wf_start_consumer(uint32_t hz, lr_wf_consumer_t consume, void *ctx)
{
    if (session && (session->consume != consume || session->ctx != ctx))
        return ESP_ERR_INVALID_STATE;
    if (session && session->hz != hz) lr_wf_stop();
    if (session) return atomic_load_explicit(&session->done, memory_order_acquire)
                         ? session->error : ESP_OK;
    if (hz < 150000000u || hz > 960000000u ||
        !ls_lora_packet_rx_hz_ok(ls_lora_caps(), hz)) return ESP_ERR_INVALID_ARG;
    if (ls_lora_chip() != LS_LORA_CHIP_LR20XX) return ESP_ERR_NOT_SUPPORTED;
    if (lr20xx_stream_source2_active() || ls_lora_fsk_active() || ls_lora_scanning())
        return ESP_ERR_INVALID_STATE;
    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) return ESP_ERR_NO_MEM;
    atomic_init(&s->stop, false); atomic_init(&s->done, false);
    atomic_init(&s->capture_stop, false); atomic_init(&s->capture_done, true);
    s->hz = hz; s->consume = consume; s->ctx = ctx;
    atomic_init(&s->lost_pairs, 0); atomic_init(&s->gaps, 0);
    if (!lr2021_iq_queue_init(&s->queue) ||
        xTaskCreatePinnedToCore(owner, "wf_iq", 4096, s, 10, NULL, 1) != pdPASS) {
        heap_caps_free(s); return ESP_ERR_NO_MEM;
    }
    session = s;
    return ESP_OK;
}
