#include "uat_stream_rx.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/task.h"
#include "radio_decode_worker.h"
#endif

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR uat_hard_ring_t ring;
static EXT_RAM_BSS_ATTR uat_hard_t decoder;
static EXT_RAM_BSS_ATTR uat_hard_chunk_t chunk;
static EXT_RAM_BSS_ATTR uint8_t rx[256];
static EXT_RAM_BSS_ATTR uat_stream_rx_stats_t stats;
static atomic_bool quit;
static int64_t rate_at;
static uint64_t rate_bytes;
bool uat_stream_rate_valid(uint32_t r)
{
    return uat_hard_rate_valid(r);
}
void uat_stream_rx_config(ls_fsk_cfg_t *c, uint32_t rate)
{
    memset(c, 0, sizeof(*c));
    c->freq_hz = 978000000; c->bitrate = rate; c->deviation_hz = 312500;
    uint32_t wanted = rate + 625000;
    c->bandwidth_hz = ls_lora_fsk_bw_snap(wanted > 3076923 ? 3076923 : wanted);
    if (c->bandwidth_hz > 3076923) c->bandwidth_hz = 3076923;
    /* The driver takes the TOP sync_bits of sync_word. 0xCC (11001100) is the
       pager stream trigger that works on the board, and doubled alternating
       bits are what UAT looks like sampled near twice its rate.
       Zero input selection retains the LF band default used by PACKET. */
    c->sync_word = 0xCC000000u; c->sync_bits = 8; c->stream = true;
}
int uat_stream_rx_read(void)
{
    bool restarted = false;
    int n = ls_lora_fsk_stream_read(rx, sizeof(rx), &restarted);
    bool drop = false;
    if (n > 0) drop = !uat_hard_ring_push(&ring, rx, n, restarted);
    else if (n < 0 || restarted) ring.pending_seam = true;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    ++stats.reads;
    if (restarted) ++stats.restarts;
    /* The board API cannot distinguish a packet restart from FIFO loss.
       Every discontinuity is a conservative loss seam, including ring loss. */
    if (restarted || n < 0 || drop) ++stats.seams;
    if (n < 0) ++stats.faults;
    if (drop) ++stats.drops;
    if (n > 0) stats.bytes += n;
    if (now - rate_at >= 1000000) {
        stats.bytes_sec = (uint32_t)((stats.bytes - rate_bytes) * 1000000 / (now - rate_at));
        rate_bytes = stats.bytes; rate_at = now;
    }
    portEXIT_CRITICAL(&lock);
    return n;
}
bool uat_stream_rx_decode(void)
{
    if (!uat_hard_ring_pop(&ring, &chunk)) return false;
    uat_hard_feed(&decoder, chunk.bytes, chunk.n, chunk.seam);
    portENTER_CRITICAL(&lock); stats.decoded = decoder.stats; portEXIT_CRITICAL(&lock);
    return true;
}
void uat_stream_rx_snapshot(uat_stream_rx_stats_t *out)
{
    portENTER_CRITICAL(&lock); *out = stats; portEXIT_CRITICAL(&lock);
}

#ifdef ESP_PLATFORM
/* Follow LR2021 capture placement: core 0 above decode, internal stack.
   The shared cache-safe radio decoder stack runs on core 1. */
static DRAM_ATTR StackType_t reader_stack[4096] __attribute__((aligned(16)));
static DRAM_ATTR StaticTask_t reader_tcb;
static TaskHandle_t reader;
static esp_timer_handle_t wake_timer;
static bool decode_started;
static void IRAM_ATTR wake(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    if (reader) vTaskNotifyGiveFromISR(reader, &woken);
    if (woken) esp_timer_isr_dispatch_need_yield();
}
static void read_task(void *arg)
{
    (void)arg;
    while (!atomic_load_explicit(&quit, memory_order_acquire)) {
        int n = uat_stream_rx_read();
        if (n < 192) {
            /* 200 us is comfortably inside the 1024 us FIFO budget at
               2 Mbps. Sleeping on the high-resolution ISR timer also lets
               core 0's idle task service its watchdog during reception too.
               Drain a high backlog immediately; a low one gets this bounded
               break even if every SPI poll finds a few newly arrived bytes. */
            if (esp_timer_start_once(wake_timer, 200) == ESP_OK)
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            else vTaskDelay(1);
        }
    }
    vTaskSuspend(NULL);
}
static void decode_task(void *arg)
{
    (void)arg;
    while (!atomic_load_explicit(&quit, memory_order_acquire))
        if (!uat_stream_rx_decode()) vTaskDelay(1);
}
#endif
bool uat_stream_rx_start(uint32_t rate)
{
    if (!uat_stream_rate_valid(rate)) return false;
#ifdef ESP_PLATFORM
    if (reader || decode_started) return false;
#endif
    uat_hard_ring_init(&ring); uat_hard_init(&decoder, rate);
    portENTER_CRITICAL(&lock); memset(&stats, 0, sizeof(stats)); stats.rate = rate; portEXIT_CRITICAL(&lock);
    rate_at = esp_timer_get_time(); rate_bytes = 0;
    atomic_store_explicit(&quit, false, memory_order_release);
#ifdef ESP_PLATFORM
    const esp_timer_create_args_t args = {.callback = wake, .name = "uat_fifo", .dispatch_method = ESP_TIMER_ISR};
    if (esp_timer_create(&args, &wake_timer) != ESP_OK) return false;
    decode_started = ls_radio_decode_worker_start(decode_task, "uat_decode", 10, 1);
    if (decode_started)
        reader = xTaskCreateStaticPinnedToCore(read_task, "uat_read", sizeof(reader_stack), NULL,
                                              configMAX_PRIORITIES - 2, reader_stack, &reader_tcb, 0);
    if (!reader) { uat_stream_rx_stop(); return false; }
#endif
    return true;
}
void uat_stream_rx_stop(void)
{
    atomic_store_explicit(&quit, true, memory_order_release);
#ifdef ESP_PLATFORM
    /* Cooperative quiescence precedes deleting TCBs, ending the radio session
       or reinitializing PSRAM. Never delete a task still using its stack. */
    if (reader) {
        while (eTaskGetState(reader) != eSuspended) vTaskDelay(1);
        vTaskDelete(reader); reader = NULL;
    }
    if (decode_started) {
        while (!ls_radio_decode_worker_release(10, 1)) vTaskDelay(1);
        decode_started = false;
    }
    if (wake_timer) {
        esp_timer_stop(wake_timer); esp_timer_delete(wake_timer); wake_timer = NULL;
    }
#endif
}
