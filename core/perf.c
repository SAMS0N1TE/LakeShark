/* perf.c - global counters, 1Hz history rings, and heartbeat publisher. */

#include "perf.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "perf";

/* ── Cumulative / instantaneous counters ────────────────────────── */
static volatile uint32_t s_bytes_window  = 0;
static volatile uint32_t s_bytes_per_sec = 0;
static volatile int      s_msgs_total    = 0;
static volatile int      s_msgs_window   = 0;
static volatile int      s_msgs_per_sec  = 0;
static volatile int      s_crc_good      = 0;
static volatile int      s_crc_err       = 0;
static volatile int      s_mag_avg       = 0;
static volatile int      s_mag_peak      = 0;
static volatile int      s_active        = 0;
static int64_t           s_window_start  = 0;

/* Burst counter (preamble candidates seen by the demod, before CRC). */
static volatile uint32_t s_burst_total      = 0;
static volatile uint16_t s_burst_window     = 0;   /* count this second */
static volatile uint16_t s_bursts_per_sec   = 0;

/* Last-event timestamps (microseconds, 0 = never). */
static volatile int64_t  s_last_good_us     = 0;
static volatile int64_t  s_last_burst_us    = 0;
static volatile int64_t  s_last_position_us = 0;

/* ── 1Hz rolling history rings ──────────────────────────────────── */
/* Indexed [0..PERF_HISTORY_LEN-1] in oldest-to-newest order. The
 * sampler task copies the in-progress window counters into the rings
 * once per second, then resets the windows. Reading is lock-free; a
 * torn read at the boundary is harmless because we render at ~6 Hz
 * and the next frame will be consistent. */
static uint16_t s_hist_bursts[PERF_HISTORY_LEN];
static uint16_t s_hist_good  [PERF_HISTORY_LEN];
static uint8_t  s_hist_mag   [PERF_HISTORY_LEN];

/* For the sampler: previous cumulative crc_good so we can compute the
 * delta (per-second rate) without needing a second window counter. */
static int  s_prev_crc_good = 0;

static void roll_window_if_needed(void)
{
    int64_t now = esp_timer_get_time();
    int64_t dt  = now - s_window_start;
    if (dt < 1000000) return;
    s_bytes_per_sec = (uint32_t)((s_bytes_window * 1000000ULL) / (uint64_t)dt);
    s_msgs_per_sec  = (int)((int64_t)s_msgs_window * 1000000LL / dt);
    s_bytes_window  = 0;
    s_msgs_window   = 0;
    s_window_start  = now;
}

void perf_init(void)
{
    s_window_start  = esp_timer_get_time();
    s_bytes_window  = 0;
    s_bytes_per_sec = 0;
    s_msgs_window   = 0;
    s_msgs_per_sec  = 0;
    s_msgs_total    = 0;
    s_crc_good      = 0;
    s_crc_err       = 0;
    s_mag_avg       = 0;
    s_mag_peak      = 0;
    s_active        = 0;
    s_burst_total      = 0;
    s_burst_window     = 0;
    s_bursts_per_sec   = 0;
    s_last_good_us     = 0;
    s_last_burst_us    = 0;
    s_last_position_us = 0;
    s_prev_crc_good    = 0;
    memset(s_hist_bursts, 0, sizeof(s_hist_bursts));
    memset(s_hist_good,   0, sizeof(s_hist_good));
    memset(s_hist_mag,    0, sizeof(s_hist_mag));
}

/* ── Counter setters ────────────────────────────────────────────── */

void perf_count_bytes(uint32_t n)    { s_bytes_window += n; roll_window_if_needed(); }
void perf_count_msg_good(void)       { s_crc_good++;  s_msgs_total++; s_msgs_window++; }
void perf_count_msg_bad(void)        { s_crc_err++; }
void perf_count_burst(void)          { s_burst_total++; s_burst_window++; s_last_burst_us = esp_timer_get_time(); }
void perf_mark_good_msg(int64_t t)   { s_last_good_us = t; }
void perf_mark_position(int64_t t)   { s_last_position_us = t; }
void perf_set_mag(int avg, int peak) { s_mag_avg = avg; s_mag_peak = peak; }
void perf_set_active_count(int n)    { s_active = n; }

/* ── Counter getters ────────────────────────────────────────────── */

uint32_t perf_get_bytes_per_sec(void) { roll_window_if_needed(); return s_bytes_per_sec; }
int  perf_get_msgs_per_sec(void)      { roll_window_if_needed(); return s_msgs_per_sec; }
int  perf_get_msgs_total(void)        { return s_msgs_total; }
int  perf_get_crc_good(void)          { return s_crc_good; }
int  perf_get_crc_err(void)           { return s_crc_err; }
int  perf_get_burst_total(void)       { return (int)s_burst_total; }
int  perf_get_bursts_per_sec(void)    { return (int)s_bursts_per_sec; }
int  perf_get_mag_avg(void)           { return s_mag_avg; }
int  perf_get_mag_peak(void)          { return s_mag_peak; }
int  perf_get_active_count(void)      { return s_active; }
int64_t perf_get_last_good_us(void)     { return s_last_good_us; }
int64_t perf_get_last_burst_us(void)    { return s_last_burst_us; }
int64_t perf_get_last_position_us(void) { return s_last_position_us; }

const uint16_t *perf_history_bursts(void) { return s_hist_bursts; }
const uint16_t *perf_history_good(void)   { return s_hist_good; }
const uint8_t  *perf_history_mag_avg(void){ return s_hist_mag; }

/* ── 1Hz sampler ────────────────────────────────────────────────── */

/* Shift each ring left by one and append the just-completed second's
 * value at the end. Called from the heartbeat task at exactly 1 Hz. */
static void roll_history_one_second(void)
{
    /* Bursts: snapshot and zero the window counter. */
    uint16_t b = s_burst_window;
    s_burst_window = 0;

    /* CRC-good per second: derive from cumulative delta. Atomic enough
     * for the read since s_crc_good is volatile and we're the only
     * reader. */
    int cur_good = s_crc_good;
    int g_delta  = cur_good - s_prev_crc_good;
    if (g_delta < 0) g_delta = 0;
    if (g_delta > 65535) g_delta = 65535;
    s_prev_crc_good = cur_good;

    /* Magnitude: snapshot the most recent set value. The decoder writes
     * raw lib magnitudes which run 0..~65211 (sqrt(I²+Q²)*360, where I
     * and Q are 8-bit signed centered at 0). The history ring is uint8,
     * so scale to 0..255 here — that's the display range the diag page
     * thresholds are tuned against (60/180/220 ~ low/hot/sat). */
    int mag_raw = s_mag_avg;
    int mag = mag_raw >> 8;          /* divide by 256, ceiling stays ~254 */
    if (mag < 0)   mag = 0;
    if (mag > 255) mag = 255;

    /* Shift left and append. PERF_HISTORY_LEN is small (60) so the
     * memmove is trivial. */
    memmove(&s_hist_bursts[0], &s_hist_bursts[1],
            (PERF_HISTORY_LEN - 1) * sizeof(s_hist_bursts[0]));
    memmove(&s_hist_good[0],   &s_hist_good[1],
            (PERF_HISTORY_LEN - 1) * sizeof(s_hist_good[0]));
    memmove(&s_hist_mag[0],    &s_hist_mag[1],
            (PERF_HISTORY_LEN - 1) * sizeof(s_hist_mag[0]));
    s_hist_bursts[PERF_HISTORY_LEN - 1] = b;
    s_hist_good  [PERF_HISTORY_LEN - 1] = (uint16_t)g_delta;
    s_hist_mag   [PERF_HISTORY_LEN - 1] = (uint8_t)mag;

    /* Make the per-second rate visible to readers. */
    s_bursts_per_sec = b;
}

typedef struct {
    char     app_name[16];
    uint32_t period_ms;
} hb_ctx_t;

/* 1 Hz sampler — drives the history rings. Independent of the
 * heartbeat task so the sampler stays on a clean 1s cadence even if
 * the heartbeat period is reconfigured. */
static void sampler_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        roll_history_one_second();
    }
}

static void hb_task(void *arg)
{
    hb_ctx_t *ctx = (hb_ctx_t *)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ctx->period_ms));
        evt_heartbeat_t hb = {
            .bytes_per_sec = perf_get_bytes_per_sec(),
            .msgs_total    = perf_get_msgs_total(),
            .msgs_per_sec  = perf_get_msgs_per_sec(),
            .crc_good      = perf_get_crc_good(),
            .crc_err       = perf_get_crc_err(),
            .mag_avg       = perf_get_mag_avg(),
            .mag_peak      = perf_get_mag_peak(),
            .active_count  = perf_get_active_count(),
        };
        event_bus_publish_heartbeat(ctx->app_name, &hb);
    }
}

void perf_start_heartbeat_task(const char *app_name, uint32_t period_ms)
{
    hb_ctx_t *ctx = pvPortMalloc(sizeof(hb_ctx_t));
    if (!ctx) {
        ESP_LOGW(TAG, "hb_ctx alloc failed");
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    if (app_name) strncpy(ctx->app_name, app_name, sizeof(ctx->app_name) - 1);
    ctx->period_ms = period_ms ? period_ms : 30000;
    xTaskCreatePinnedToCore(hb_task,      "perf_hb",      3072, ctx,  1, NULL, 0);
    xTaskCreatePinnedToCore(sampler_task, "perf_sampler", 2048, NULL, 1, NULL, 0);
}
