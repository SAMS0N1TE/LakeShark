#include "scan_engine.h"
#include "scan_channels.h"
#include "app_registry.h"
#include "p25_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern volatile uint32_t s_p25_freq_req;
extern volatile float    p25_rx_power;

static const char *TAG = "scaneng";

#define SETTLE_MS        120
#define SYNC_DWELL_MS    900
#define DEFAULT_HANG_MS  3000
#define DEFAULT_THRESH   10

static volatile bool s_enabled = false;
static int           s_cur     = -1;
static int           s_hang_ms = DEFAULT_HANG_MS;
static int           s_thresh  = DEFAULT_THRESH;
static volatile bool s_skip_req = false;
static uint64_t      s_session_skip = 0;
static int           s_order[SCAN_MAX_CHANNELS];
static int           s_order_n = 0;
static int           s_order_pos = 0;
static char          s_status[96] = "off";
static int           s_pk_max = 0;

static bool sess_skipped(int idx) { return idx >= 0 && idx < 64 && ((s_session_skip >> idx) & 1ULL); }
static void sess_skip(int idx)    { if (idx >= 0 && idx < 64) s_session_skip |= (1ULL << idx); }

static bool p25_foreground(void)
{
    const app_t *a = app_current();
    return a && a->name && strcmp(a->name, "P25") == 0;
}

static void rebuild_order(void)
{
    s_order_n = 0;
    int n = scan_channels_count();
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            const scan_channel_t *c = scan_channel_get(i);
            if (!c) continue;
            if (!(c->flags & SCAN_FLAG_ENABLED)) continue;
            if (c->flags & SCAN_FLAG_LOCKOUT) continue;
            if (sess_skipped(i)) continue;
            bool pri = (c->flags & SCAN_FLAG_PRIORITY) != 0;
            if (pass == 0 && !pri) continue;
            if (pass == 1 && pri)  continue;
            if (s_order_n < SCAN_MAX_CHANNELS) s_order[s_order_n++] = i;
        }
    }
    if (s_order_pos >= s_order_n) s_order_pos = 0;
}

static void scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_enabled) {
            s_cur = -1;
            strncpy(s_status, "off", sizeof(s_status) - 1);
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        if (!p25_foreground()) {
            s_cur = -1;
            strncpy(s_status, "open P25 app to scan", sizeof(s_status) - 1);
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        rebuild_order();
        if (s_order_n == 0) {
            s_cur = -1;
            strncpy(s_status, "no channels", sizeof(s_status) - 1);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        int idx = s_order[s_order_pos];
        s_order_pos = (s_order_pos + 1) % s_order_n;
        const scan_channel_t *c = scan_channel_get(idx);
        if (!c) continue;

        if (c->mode != SCAN_MODE_P25) continue;

        s_p25_freq_req = c->freq_hz;
        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
        if (!s_enabled || !p25_foreground()) continue;

        int pwi = (int)(p25_rx_power * 100.0f + 0.5f);
        if (pwi > s_pk_max) s_pk_max = pwi;

        if (pwi < s_thresh) {
            snprintf(s_status, sizeof(s_status), "SCAN %-9s p=%02d", c->name, pwi);
            continue;
        }
        snprintf(s_status, sizeof(s_status), "CHECK %-9s p=%02d", c->name, pwi);

        int64_t t0 = esp_timer_get_time();
        bool sync = false;
        while (esp_timer_get_time() - t0 < (int64_t)SYNC_DWELL_MS * 1000) {
            if (!s_enabled || !p25_foreground()) break;
            if (P25.dsd_has_sync) { sync = true; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!sync) continue;

        s_cur = idx;
        snprintf(s_status, sizeof(s_status), "HOLD %-10s %.4f", c->name, c->freq_hz / 1e6);
        int64_t last = esp_timer_get_time();
        for (;;) {
            if (!s_enabled || !p25_foreground()) break;
            if (s_skip_req) { s_skip_req = false; sess_skip(idx); break; }
            if (P25.dsd_has_sync) last = esp_timer_get_time();
            else if (esp_timer_get_time() - last > (int64_t)s_hang_ms * 1000) break;
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        s_cur = -1;
    }
}

#define SCAN_STACK_WORDS (4096u / sizeof(StackType_t))
static EXT_RAM_BSS_ATTR StackType_t s_scan_stack[SCAN_STACK_WORDS];
static StaticTask_t s_scan_tcb;

void scan_engine_init(void)
{
    xTaskCreateStaticPinnedToCore(scan_task, "scan_eng", SCAN_STACK_WORDS, NULL, 4,
                                  s_scan_stack, &s_scan_tcb, 0);
    ESP_LOGI(TAG, "scan engine ready");
}

void scan_engine_start(void)
{
    s_session_skip = 0;
    s_order_pos    = 0;
    s_skip_req     = false;
    s_pk_max       = 0;
    s_enabled      = true;
}

void scan_engine_stop(void) { s_enabled = false; }
bool scan_engine_active(void) { return s_enabled; }
void scan_engine_skip(void) { s_skip_req = true; }

void scan_engine_set_hang_ms(int ms)
{
    if (ms < 0) ms = 0;
    if (ms > 30000) ms = 30000;
    s_hang_ms = ms;
}

void scan_engine_set_threshold_db(float v)
{
    int t = (int)v;
    if (t < 1)  t = 1;
    if (t > 40) t = 40;
    s_thresh = t;
}

int   scan_engine_current(void)          { return s_cur; }
int   scan_engine_get_hang_ms(void)      { return s_hang_ms; }
float scan_engine_get_threshold_db(void) { return (float)s_thresh; }

void scan_engine_status(char *buf, size_t n)
{
    if (!buf || n == 0) return;
    snprintf(buf, n, "%s %s  sql=%d pk=%d hang=%.0fs",
             s_enabled ? "on" : "off", s_status, s_thresh, s_pk_max, s_hang_ms / 1000.0);
}
