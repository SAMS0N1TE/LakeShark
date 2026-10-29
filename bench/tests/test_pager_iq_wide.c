/* The pager adapter with WIDE I/Q on: the shared LR session is the producer
 * (faked here, its owner's calls made inline), the adapter regroups its
 * small 125k calls into whole blocks, never touches the modem, and treats
 * a stream end it did not ask for as a fault. Narrow is test_pager_iq_radio. */
#include "pager_iq_radio.h"
#include "pager_iq.h"
#include "ls_lora.h"
#include "ls_lora_lr20xx.h"
#include "lr_wf_session.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
int ls_shim_log_enabled;
static ls_lora_cfg_t current = {.freq_hz = 915000000, .sf = 11, .bw_hz = 125000, .cal_min_mhz = 902, .cal_max_mhz = 928};
static unsigned allocations, config_calls, tasks, delays;
static UBaseType_t priority = 4;
static int64_t now;

void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{
    (void)caps; void *p = calloc(n, size); CHECK(p); allocations++; return p;
}
void heap_caps_free(void *p) { if (p) { CHECK(allocations); allocations--; free(p); } }
UBaseType_t uxTaskPriorityGet(TaskHandle_t t) { (void)t; return priority; }
void vTaskPrioritySet(TaskHandle_t t, UBaseType_t p) { (void)t; priority = p; }
void vTaskDelete(TaskHandle_t t) { (void)t; }
int64_t esp_timer_get_time(void) { return now; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t p, TaskHandle_t *h, BaseType_t core)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)p; (void)h; (void)core;
    tasks++; return pdFAIL;
}
ls_lora_chip_t ls_lora_chip(void) { return LS_LORA_CHIP_LR20XX; }
uint32_t ls_lora_caps(void) { return 0; }
const ls_lora_cfg_t *ls_lora_cfg(void) { return &current; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { *out = current; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *cfg) { (void)cfg; config_calls++; return ESP_OK; }
esp_err_t ls_lora_receive(void) { return ESP_OK; }
bool lr20xx_stream_source2_active(void) { return false; }
esp_err_t lr20xx_stream_source2_live(lr20xx_iq_sink_t sink, void *arg, lr20xx_stream_result_t *r)
{
    (void)sink; (void)arg; (void)r; CHECK(!"narrow capture started with WIDE I/Q on"); return ESP_FAIL;
}

/* The shared session. */
static bool wide_on = true, session_live, session_done;
static esp_err_t start_result, session_error;
static lr_wf_consumer_t consumer;
static void *consumer_ctx;
static uint32_t session_hz;
static unsigned stops;
static uint32_t pairs_sent;
static unsigned per_delay = 111;    /* one wide slot's output */
bool lr_wf_wide_on(void) { return wide_on; }
esp_err_t lr_wf_start_consumer(uint32_t hz, lr_wf_consumer_t consume, void *ctx)
{
    if (start_result != ESP_OK) return start_result;
    CHECK(!session_live);
    session_live = true; session_done = false; session_error = ESP_OK;
    consumer = consume; consumer_ctx = ctx; session_hz = hz; pairs_sent = 0;
    return ESP_OK;
}
void lr_wf_get_stats(lr_wf_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->done = session_done; out->error = session_done ? session_error : ESP_OK;
}
void lr_wf_stop_stats(lr_wf_stats *out)
{
    if (out) lr_wf_get_stats(out);
    if (session_live && !session_done) consumer(consumer_ctx, NULL, 0, false);
    session_live = false; stops++;
}
/* Owner side: a run of IQ16 pairs, each pair's I carrying its index. */
static void send(unsigned pairs)
{
    uint8_t d[1024];
    CHECK(pairs * 4 <= sizeof d);
    for (unsigned k = 0; k < pairs; ++k) {
        const int16_t i = (int16_t)(pairs_sent + k), q = 1000;
        d[4*k] = (uint8_t)i; d[4*k+1] = (uint8_t)((uint16_t)i >> 8);
        d[4*k+2] = (uint8_t)q; d[4*k+3] = (uint8_t)((uint16_t)q >> 8);
    }
    pairs_sent += pairs;
    consumer(consumer_ctx, d, pairs * 4, true);
}
void vTaskDelay(TickType_t t)
{
    now += (int64_t)t * 1000; delays++;
    if (session_live && !session_done && per_delay) send(per_delay);
}

static void reset(void)
{
    CHECK(!allocations && !session_live);
    wide_on = true; start_result = ESP_OK; config_calls = tasks = delays = stops = 0;
    priority = 4; now = 0; per_delay = 111;
}
static pgr_iq_radio_stats_t stats(void) { pgr_iq_radio_stats_t s; pgr_iq_radio_stats(&s); return s; }

int main(void)
{
    float out[64];
    reset();
    CHECK(pgr_iq_radio_begin(152600000) == ESP_OK);
    /* Wide never programs the modem and starts no capture task of its own. */
    CHECK(session_hz == 152600000 && !config_calls && !tasks);
    CHECK(priority == 10 && stats().active);
    /* 320-pair blocks out of 111-pair calls: three calls make the first. */
    CHECK(delays == 3);
    int got = 0, n;
    for (int i = 0; i < 40; ++i) {
        while ((n = pgr_iq_radio_read(out, 64)) > 0) got += n;
        CHECK(n == 0);
        vTaskDelay(1);
    }
    CHECK(got > 0 && stats().blocks >= 12 && stats().bytes == (uint64_t)stats().blocks * 1280);
    CHECK(stats().errors == 0);
    pgr_iq_radio_end();
    CHECK(stops == 1 && !config_calls && priority == 4 && !allocations);
    CHECK(!stats().active && stats().errors == 0);
    puts("pager wide: whole blocks from the shared session, modem untouched, clean end");

    /* A stream end the pager did not ask for (a switch, a failed run). */
    reset();
    CHECK(pgr_iq_radio_begin(152600000) == ESP_OK);
    while (pgr_iq_radio_read(out, 64) > 0) {}
    consumer(consumer_ctx, NULL, 0, false);
    CHECK(pgr_iq_radio_read(out, 64) == -1);
    CHECK(stats().error == ESP_ERR_INVALID_RESPONSE && stats().gaps == 1);
    pgr_iq_radio_end(); CHECK(!allocations);
    /* The session itself ends with an error. */
    reset();
    CHECK(pgr_iq_radio_begin(152600000) == ESP_OK);
    session_done = true; session_error = ESP_ERR_INVALID_SIZE;
    while ((n = pgr_iq_radio_read(out, 64)) > 0) {}
    CHECK(n == -1 && stats().error == ESP_ERR_INVALID_SIZE);
    pgr_iq_radio_end(); CHECK(!allocations && priority == 4);
    puts("pager wide: unasked stream ends and session errors are faults");

    /* Refused or silent: begin cleans up without a caller end. */
    reset(); start_result = ESP_ERR_INVALID_STATE;
    CHECK(pgr_iq_radio_begin(152600000) == ESP_ERR_INVALID_STATE);
    CHECK(!allocations && priority == 4 && !config_calls);
    reset(); per_delay = 0;
    CHECK(pgr_iq_radio_begin(152600000) == ESP_ERR_TIMEOUT);
    CHECK(!allocations && stops == 1 && !session_live);
    /* WIDE I/Q off: back to the narrow adapter (its own capture task). */
    reset(); wide_on = false;
    CHECK(pgr_iq_radio_begin(152600000) == ESP_ERR_NO_MEM);  /* the fake refuses the task */
    CHECK(config_calls == 2 && tasks == 1 && !allocations);
    puts("pager wide: refusal, silence and the narrow fallback release resources");
    return 0;
}
