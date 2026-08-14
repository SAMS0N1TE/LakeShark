#include "scan_engine.h"
#include "scan_channels.h"
#include "app_registry.h"
#include "settings.h"
#include "p25_state.h"
/*LS-713*/
#include "fm_state.h"
#include "lakeshark_backend.h"
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

/*LS-701*/
#define SETTLE_MS        45
#define MEASURE_MS       75
#define POWER_POLL_MS    5
/*LS-700*/
#define IDLE_TICK_MS     4
#define SYNC_DWELL_MS    900
#define DEFAULT_HANG_MS  3000
/*LS-702*/
/*LS-709*/
#define DEFAULT_THRESH   4
/*LS-703*/
#define ZONE_ALL         (-1)
/*LS-704*/
#define PRI_SETTLE_MS    35
#define PRI_MEASURE_MS   45

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
/*LS-705*/
static int           s_pk_max = 0;
static int           s_pk_acc = 0;
/*LS-703*/
static int           s_zone = 0;
/*LS-704*/
static int           s_pri_ms = 0;
static int           s_force_idx = -1;

static bool sess_skipped(int idx) { return idx >= 0 && idx < 64 && ((s_session_skip >> idx) & 1ULL); }
static void sess_skip(int idx)    { if (idx >= 0 && idx < 64) s_session_skip |= (1ULL << idx); }

/*LS-713*/
/* The scanner scans the mode the foreground app can actually demodulate. It
   does NOT switch apps to change mode - that would fight the shell for the
   screen. In P25 this behaves exactly as it always has; in FM it scans the
   NFM channels instead. WFM is deliberately not scanned. */
static int foreground_mode(void)
{
    const app_t *a = app_current();
    if (!a || !a->name) return -1;
    if (strcmp(a->name, "P25") == 0) return SCAN_MODE_P25;
    if (strcmp(a->name, "FM")  == 0) return SCAN_MODE_NFM;
    return -1;
}

/* Mode the current sweep is built for; -1 when no scannable app is up. */
static int s_fg_mode = -1;

static bool scan_foreground(void) { return foreground_mode() == s_fg_mode && s_fg_mode >= 0; }

/*LS-713*/
static void tune_to(const scan_channel_t *c)
{
    if (c->mode == SCAN_MODE_P25) s_p25_freq_req = c->freq_hz;
    else                          lakeshark_fm_set_freq(c->freq_hz);
}

/*LS-713*/
static int rx_power_pct(int mode)
{
    float v = (mode == SCAN_MODE_P25) ? p25_rx_power : FM.iq_level;
    return (int)(v * 100.0f + 0.5f);
}

/* P25 has a sync word to converge on; NFM has only carrier/squelch. */
static bool carrier_held(int mode)
{
    return (mode == SCAN_MODE_P25) ? P25.dsd_has_sync : FM.squelch_open;
}

/*LS-703*/
static bool zone_admits(const scan_channel_t *c)
{
    return s_zone == ZONE_ALL || c->zone == (uint8_t)s_zone;
}

static bool channel_eligible(const scan_channel_t *c)
{
    if (!(c->flags & SCAN_FLAG_ENABLED)) return false;
    if (c->flags & SCAN_FLAG_LOCKOUT)    return false;
    /*LS-713*/
    if (c->mode != s_fg_mode)            return false;
    return zone_admits(c);
}

/*LS-711*/
static void empty_reason(char *buf, size_t n)
{
    int total = scan_channels_count();
    if (total <= 0) { snprintf(buf, n, "no channels - add one"); return; }

    int lock = 0, off = 0, zone = 0, mode = 0, skip = 0;
    for (int i = 0; i < total; i++) {
        const scan_channel_t *c = scan_channel_get(i);
        if (!c) continue;
        if (!(c->flags & SCAN_FLAG_ENABLED)) { off++;  continue; }
        if (c->flags & SCAN_FLAG_LOCKOUT)    { lock++; continue; }
        /*LS-713*/
        if (c->mode != s_fg_mode)            { mode++; continue; }
        if (!zone_admits(c))                 { zone++; continue; }
        if (sess_skipped(i))                 { skip++; continue; }
    }
    snprintf(buf, n, "%d ch none eligible: %dlock %doff %dzone %dmode %dskip",
             total, lock, off, zone, mode, skip);
}

static void rebuild_order(void)
{
    s_order_n = 0;
    int n = scan_channels_count();
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            const scan_channel_t *c = scan_channel_get(i);
            if (!c) continue;
            if (!channel_eligible(c)) continue;
            if (sess_skipped(i)) continue;
            bool pri = (c->flags & SCAN_FLAG_PRIORITY) != 0;
            if (pass == 0 && !pri) continue;
            if (pass == 1 && pri)  continue;
            if (s_order_n < SCAN_MAX_CHANNELS) s_order[s_order_n++] = i;
        }
    }
    if (s_order_pos >= s_order_n) s_order_pos = 0;
}

/*LS-701*/
static int measure_peak(int settle_ms, int win_ms, int mode)
{
    vTaskDelay(pdMS_TO_TICKS(settle_ms));

    int pk = 0;
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < (int64_t)win_ms * 1000) {
        if (!s_enabled || !scan_foreground()) break;
        int p = rx_power_pct(mode);
        if (p > pk) pk = p;
        vTaskDelay(pdMS_TO_TICKS(POWER_POLL_MS));
    }
    return pk;
}

/*LS-704*/
static int priority_sample(void)
{
    int n = scan_channels_count();
    for (int i = 0; i < n; i++) {
        const scan_channel_t *c = scan_channel_get(i);
        if (!c) continue;
        if (!(c->flags & SCAN_FLAG_PRIORITY)) continue;
        if (!channel_eligible(c)) continue;
        if (sess_skipped(i)) continue;

        tune_to(c);
        int pk = measure_peak(PRI_SETTLE_MS, PRI_MEASURE_MS, c->mode);
        if (!s_enabled || !scan_foreground()) return -1;
        if (pk >= s_thresh) return i;
    }
    return -1;
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
        /*LS-713*/
        const int fg = foreground_mode();
        if (fg < 0) {
            s_cur     = -1;
            s_fg_mode = -1;
            strncpy(s_status, "open P25 or FM to scan", sizeof(s_status) - 1);
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        if (fg != s_fg_mode) {
            /* The built order belongs to the old mode - throw it away. */
            s_fg_mode   = fg;
            s_order_n   = 0;
            s_order_pos = 0;
            s_cur       = -1;
        }

        /*LS-705*/
        if (s_force_idx < 0 && s_order_pos == 0) {
            rebuild_order();
            s_pk_max = s_pk_acc;
            s_pk_acc = 0;
        }
        if (s_order_n == 0) {
            s_cur = -1;
            /*LS-711*/
            empty_reason(s_status, sizeof(s_status));
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        int idx;
        /*LS-704*/
        if (s_force_idx >= 0) {
            idx = s_force_idx;
            s_force_idx = -1;
        } else {
            idx = s_order[s_order_pos];
            s_order_pos = (s_order_pos + 1) % s_order_n;
        }

        const scan_channel_t *c = scan_channel_get(idx);
        /*LS-700*/
        /*LS-713*/
        if (!c || c->mode != s_fg_mode) {
            vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
            continue;
        }

        tune_to(c);

        /*LS-701*/
        int pwi = measure_peak(SETTLE_MS, MEASURE_MS, c->mode);
        if (pwi > s_pk_acc) s_pk_acc = pwi;
        if (!s_enabled || !scan_foreground()) continue;

        if (pwi < s_thresh) {
            snprintf(s_status, sizeof(s_status), "SCAN %-9s p=%02d", c->name, pwi);
            continue;
        }
        snprintf(s_status, sizeof(s_status), "CHECK %-9s p=%02d", c->name, pwi);

        /*LS-713*/
        /* P25 has to re-converge on the sync word after every retune, which is
           what SYNC_DWELL_MS buys. NFM has no sync - a carrier over threshold
           already IS the hit, so waiting 900 ms would just miss the call. */
        bool sync = (c->mode != SCAN_MODE_P25);
        if (!sync) {
            int64_t t0 = esp_timer_get_time();
            while (esp_timer_get_time() - t0 < (int64_t)SYNC_DWELL_MS * 1000) {
                if (!s_enabled || !scan_foreground()) break;
                if (P25.dsd_has_sync) { sync = true; break; }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        if (!sync) continue;

        s_cur = idx;
        snprintf(s_status, sizeof(s_status), "HOLD %-10s %.4f", c->name, c->freq_hz / 1e6);

        /*LS-704*/
        const bool cur_is_pri = (c->flags & SCAN_FLAG_PRIORITY) != 0;
        const uint32_t hold_hz = c->freq_hz;

        int64_t last = esp_timer_get_time();
        int64_t pri_next = esp_timer_get_time() + (int64_t)s_pri_ms * 1000;
        for (;;) {
            if (!s_enabled || !scan_foreground()) break;
            if (s_skip_req) { s_skip_req = false; sess_skip(idx); break; }
            /*LS-713*/
            if (carrier_held(c->mode)) last = esp_timer_get_time();
            else if (esp_timer_get_time() - last > (int64_t)s_hang_ms * 1000) break;

            /*LS-704*/
            if (s_pri_ms > 0 && !cur_is_pri && esp_timer_get_time() >= pri_next) {
                int hit = priority_sample();
                if (hit >= 0 && hit != idx) {
                    s_force_idx = hit;
                    break;
                }
                /*LS-713*/
                if (c->mode == SCAN_MODE_P25) s_p25_freq_req = hold_hz;
                else                          lakeshark_fm_set_freq(hold_hz);
                last = esp_timer_get_time();
                pri_next = esp_timer_get_time() + (int64_t)s_pri_ms * 1000;
            }

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
    /*LS-703*/
    s_zone = settings_get_scan_zone();
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
    s_pk_acc       = 0;
    s_force_idx    = -1;
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

/*LS-702*/
void scan_engine_set_threshold_pct(int pct)
{
    if (pct < 1)   pct = 1;
    if (pct > 100) pct = 100;
    s_thresh = pct;
}

/*LS-703*/
void scan_engine_set_zone(int zone)
{
    if (zone < 0) zone = ZONE_ALL;
    else if (zone >= SCAN_MAX_ZONES) zone = SCAN_MAX_ZONES - 1;
    s_zone = zone;
    settings_set_scan_zone(zone);
    s_order_pos = 0;
    s_session_skip = 0;
}

/*LS-704*/
void scan_engine_set_priority_ms(int ms)
{
    if (ms > 0 && ms < 500) ms = 500;
    if (ms > 60000) ms = 60000;
    s_pri_ms = ms > 0 ? ms : 0;
}

int   scan_engine_current(void)           { return s_cur; }
int   scan_engine_get_hang_ms(void)       { return s_hang_ms; }
int   scan_engine_get_threshold_pct(void) { return s_thresh; }
int   scan_engine_get_zone(void)          { return s_zone; }
int   scan_engine_get_priority_ms(void)   { return s_pri_ms; }

void scan_engine_status(char *buf, size_t n)
{
    if (!buf || n == 0) return;
    char zbuf[8];
    /*LS-703*/
    if (s_zone == ZONE_ALL) snprintf(zbuf, sizeof(zbuf), "all");
    else                    snprintf(zbuf, sizeof(zbuf), "%d", s_zone);

    /*LS-705*/
    snprintf(buf, n, "%s %s  sql=%d pk=%d hang=%.0fs z=%s pri=%d",
             s_enabled ? "on" : "off", s_status, s_thresh, s_pk_max,
             s_hang_ms / 1000.0, zbuf, s_pri_ms / 1000);
}
