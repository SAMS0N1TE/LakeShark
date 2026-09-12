#include "scan_engine.h"
#include "scan_channels.h"
#include "app_registry.h"
#include "settings.h"
#include "p25_state.h"
#include "p25_program.h"
/**/
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
/**/
#include <stdlib.h>

extern volatile float    p25_rx_power;

static const char *TAG = "scaneng";

/**/
#define SETTLE_MS        45
#define MEASURE_MS       75
#define POWER_POLL_MS    5
/**/
#define IDLE_TICK_MS     4
#define SYNC_DWELL_MS    900
#define DEFAULT_HANG_MS  3000
/**/
/**/
#define DEFAULT_THRESH   4
/**/
#define ZONE_ALL         (-1)
/**/
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
/**/
static int           s_pk_max = 0;
static int           s_pk_acc = 0;
/**/
static int           s_zone = 0;
/**/
static int           s_pri_ms = 0;
static int           s_force_idx = -1;

static bool          s_task_ready = false;
static bool          s_started = false;
static volatile ls_radio_err_t s_scan_error = LS_RADIO_OK;

static bool sess_skipped(int idx) { return idx >= 0 && idx < 64 && ((s_session_skip >> idx) & 1ULL); }
static void sess_skip(int idx)    { if (idx >= 0 && idx < 64) s_session_skip |= (1ULL << idx); }

/**/
/* The scanner scans the mode the foreground app can actually demodulate. It
   does NOT switch apps to change mode - that would fight the shell for the
   screen. In P25 this behaves exactly as it always has; in FM it scans the
   NFM channels instead. WFM is deliberately not scanned. */
static int selected_mode(void)
{
    const app_t *a = app_current();
    if (!a || !a->name) return -1;
    if (strcmp(a->name, "P25") == 0) return SCAN_MODE_P25;
    if (strcmp(a->name, "FM")  == 0) return SCAN_MODE_NFM;
    return -1;
}

static int foreground_mode(void)
{
    /* app_current() names the selected backend even while it is
       parked. Treating that as foreground let the scanner enqueue tunes with
       no RX owner alive to consume them. */
    return app_parked() ? -1 : selected_mode();
}

/* Mode the current sweep is built for; -1 when no scannable app is up. */
static int s_fg_mode = -1;

static bool scan_foreground(void) { return foreground_mode() == s_fg_mode && s_fg_mode >= 0; }

static void receiver_status_for(int mode, ls_iq_control_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->receiver_error = LS_RADIO_ERR_UNAVAILABLE;
    if (mode == SCAN_MODE_P25) p25_get_receiver_status(out);
    else if (mode == SCAN_MODE_NFM) fm_get_receiver_status(out);
}

/* a scanner tune is asynchronous: tune_to() only writes the app
   owner's single-slot request latch. The old path slept 45 ms and measured
   whatever frequency happened to be effective then. Wait for the matching
   request to be acknowledged by the endpoint, and never label the requested
   value as actual. */
static bool wait_for_tune(uint32_t target_hz, int mode,
                          ls_radio_err_t *error)
{
    for (int waited = 0; waited < 500; waited += 10) {
        if (!s_enabled || !scan_foreground()) {
            if (error) *error = LS_RADIO_ERR_STOPPED;
            return false;
        }
        ls_iq_control_status_t rx;
        receiver_status_for(mode, &rx);
        if (!rx.receiver_streaming) {
            if (rx.receiver_error != LS_RADIO_OK &&
                rx.receiver_error != LS_RADIO_ERR_UNAVAILABLE &&
                rx.receiver_error != LS_RADIO_ERR_STOPPED) {
                if (error) *error = rx.receiver_error;
                return false;
            }
        } else if (rx.requested_center_hz == target_hz) {
            if (rx.tune_state == LS_IQ_RESULT_FAILED) {
                if (error) *error = rx.tune_error;
                return false;
            }
            if (rx.tune_state == LS_IQ_RESULT_EFFECTIVE) {
                if (error) *error = LS_RADIO_OK;
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (error) *error = LS_RADIO_ERR_TIMEOUT;
    return false;
}

/**/
static void tune_to(const scan_channel_t *c)
{
    /**/
    if (c->mode == SCAN_MODE_P25) p25_request_tune(c->freq_hz, true);
    else                          lakeshark_fm_tune_transient(c->freq_hz);
}

/**/
static int rx_power_pct(int mode)
{
    float v = (mode == SCAN_MODE_P25) ? p25_rx_power : FM.iq_level;
    return (int)(v * 100.0f + 0.5f);
}

/* P25 has a sync word to converge on; NFM has only carrier/squelch. */
/**/
/* FM.squelch_open IS ONLY MAINTAINED IN FM_MODE_LISTEN. */

static bool carrier_held(int mode)
{
    if (mode == SCAN_MODE_P25)     return P25.dsd_has_sync;
    if (FM.mode != FM_MODE_LISTEN) return false;
    return FM.squelch_open;
}

/**/
/* THE STOP GATE AND THE SQUELCH MUST BE THE SAME NUMBER FOR NFM. */

static int stop_threshold(int mode)
{
    if (mode != SCAN_MODE_NFM) return s_thresh;
    int sq = FM.squelch_tenths;
    return (sq > 0) ? sq : s_thresh;
}

/**/

static void nfm_ensure_listen(void)
{
    if (FM.mode == FM_MODE_LISTEN) return;
    ESP_LOGW(TAG, "scanner: FM submode was %d - forcing LISTEN, squelch is "
                  "only maintained there", (int)FM.mode);
    lakeshark_fm_set_mode(FM_MODE_LISTEN);
}

/**/

static bool nfm_wait_listen(int timeout_ms)
{
    nfm_ensure_listen();
    int waited = 0;
    while (FM.mode != FM_MODE_LISTEN && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    return FM.mode == FM_MODE_LISTEN;
}

/**/
/* BAND SCAN - a bare frequency grid, no stored channels. */

static scan_src_t s_src        = SCAN_SRC_CHANNELS;
static uint32_t   s_band_start = 150000000UL;
static uint32_t   s_band_stop  = 162000000UL;
static uint32_t   s_band_step  = 12500UL;
static int        s_band_pos   = 0;
static scan_channel_t s_band_ch;   /* scratch, refilled every step */

static int band_steps(void)
{
    if (s_band_step == 0 || s_band_stop <= s_band_start) return 0;
    uint32_t span = s_band_stop - s_band_start;
    uint32_t n    = span / s_band_step + 1;
    if (n > 100000UL) n = 100000UL;      /* sanity, not a real limit */
    return (int)n;
}

/* Build the synthetic channel for grid position i. */
static const scan_channel_t *band_channel(int i)
{
    uint32_t hz = s_band_start + (uint32_t)i * s_band_step;
    memset(&s_band_ch, 0, sizeof(s_band_ch));
    s_band_ch.freq_hz = hz;
    /* Follows the foreground demodulator, same rule as . A band scan
       has no stored mode to obey, so it takes whatever is up. */
    s_band_ch.mode  = (uint8_t)(s_fg_mode < 0 ? SCAN_MODE_NFM : s_fg_mode);
    s_band_ch.flags = SCAN_FLAG_ENABLED;
    s_band_ch.zone  = 0;
    snprintf(s_band_ch.name, SCAN_NAME_LEN, "%lu.%04lu",
             (unsigned long)(hz / 1000000UL),
             (unsigned long)((hz % 1000000UL) / 100UL));
    return &s_band_ch;
}

void scan_engine_set_source(scan_src_t src)
{
    if (src != SCAN_SRC_CHANNELS && src != SCAN_SRC_BAND) return;
    s_src       = src;
    s_band_pos  = 0;
    s_order_n   = 0;      /* force a rebuild when going back to channels */
    s_order_pos = 0;
    s_cur       = -1;
    s_session_skip = 0;
}
scan_src_t scan_engine_get_source(void) { return s_src; }

bool scan_engine_set_band(uint32_t start_hz, uint32_t stop_hz, uint32_t step_hz)
{
    if (step_hz == 0) step_hz = s_band_step;
    if (step_hz < 100UL) return false;
    if (stop_hz <= start_hz) return false;
    if (start_hz < 1000000UL || stop_hz > 2000000000UL) return false;
    s_band_start = start_hz;
    s_band_stop  = stop_hz;
    s_band_step  = step_hz;
    s_band_pos   = 0;
    return true;
}

void scan_engine_get_band(uint32_t *a, uint32_t *b, uint32_t *s)
{
    if (a) *a = s_band_start;
    if (b) *b = s_band_stop;
    if (s) *s = s_band_step;
}
int scan_engine_band_steps(void) { return band_steps(); }

/**/
/* Defined below; the calibration sweep needs both and sits above them so it
   can read next to the squelch logic it exists to set. */
static int  measure_peak(int settle_ms, int win_ms, int mode);
static bool channel_eligible(const scan_channel_t *c);

/**/
/* AUTO SQUELCH - measure the floor, then sit a margin above it. */

#define AUTOSQ_SAMPLES   24
#define AUTOSQ_MARGIN    6
#define AUTOSQ_SETTLE_MS 25
#define AUTOSQ_MEAS_MS   35

static volatile bool s_autosq_req    = false;
static volatile bool s_autosq_busy   = false;
static volatile int  s_autosq_margin = AUTOSQ_MARGIN;
static int           s_autosq_floor  = -1;

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* Runs ON THE SCAN TASK - it tunes and blocks, so it must not be called from
   the console or LVGL thread. Both request it via s_autosq_req instead. */
static void do_autosquelch(void)
{
    if (s_fg_mode != SCAN_MODE_NFM) {
        snprintf(s_status, sizeof(s_status),
                 "autosql needs the FM app (squelch is an NFM idea)");
        return;
    }
    /**/
    if (!nfm_wait_listen(800)) {
        snprintf(s_status, sizeof(s_status),
                 "autosql: FM never reached LISTEN - not calibrating");
        ESP_LOGW(TAG, "autosql: FM submode stuck at %d, refusing to calibrate",
                 (int)FM.mode);
        return;
    }
    /**/
    /* MUST be set across the whole calibration sweep. It hops as fast as a
       scan, so it needs the retune path - without this the guard in
       fm_tune_hw sees scan_engine_active()==false, resets the USB FIFO on
       every one of the 24 hops, and the stream never delivers a sample. That
       is not hypothetical: the first run of this measured floor=0 across all
       24 samples for exactly this reason. */
    s_autosq_busy = true;

    int  samples[AUTOSQ_SAMPLES];
    int  n = 0;

    if (s_src == SCAN_SRC_BAND) {
        int steps = band_steps();
        if (steps <= 0) { s_autosq_busy = false;
                          snprintf(s_status, sizeof(s_status), "autosql: empty band"); return; }

        int stride = steps / AUTOSQ_SAMPLES;
        if (stride < 1) stride = 1;
        for (int i = 0; i < steps && n < AUTOSQ_SAMPLES; i += stride) {
            if (!scan_foreground()) { s_autosq_busy = false; return; }
            tune_to(band_channel(i));
            samples[n++] = measure_peak(AUTOSQ_SETTLE_MS, AUTOSQ_MEAS_MS, SCAN_MODE_NFM);
        }
    } else {
        int total = scan_channels_count();
        for (int i = 0; i < total && n < AUTOSQ_SAMPLES; i++) {
            const scan_channel_t *c = scan_channel_get(i);
            if (!c || !channel_eligible(c)) continue;
            if (!scan_foreground()) { s_autosq_busy = false; return; }
            tune_to(c);
            samples[n++] = measure_peak(AUTOSQ_SETTLE_MS, AUTOSQ_MEAS_MS, SCAN_MODE_NFM);
        }
    }

    s_autosq_busy = false;
    if (n < 3) {
        snprintf(s_status, sizeof(s_status),
                 "autosql: only %d samples - need at least 3", n);
        return;
    }

    qsort(samples, n, sizeof(samples[0]), cmp_int);

    /**/
    /* A FLAT ZERO IS NOT A QUIET BAND, IT IS NO SAMPLES. */

    if (samples[n - 1] == 0) {
        snprintf(s_status, sizeof(s_status),
                 "autosql: all %d samples read 0 - no signal path, squelch unchanged", n);
        ESP_LOGW(TAG, "autosql: %d samples all zero - refusing to set squelch "
                      "from a measurement that did not happen", n);
        return;
    }

    int floor_pct = samples[n / 2];
    int want = floor_pct + s_autosq_margin;
    if (want < 1)   want = 1;
    if (want > 100) want = 100;

    s_autosq_floor = floor_pct;
    lakeshark_fm_set_squelch(want);
    ESP_LOGW(TAG, "autosql: floor=%d (median of %d, %d..%d) margin=%d -> squelch=%d",
             floor_pct, n, samples[0], samples[n - 1], s_autosq_margin, want);
    snprintf(s_status, sizeof(s_status),
             "autosql floor=%d n=%d -> sq=%d", floor_pct, n, want);
}

void scan_engine_autosquelch(int margin)
{
    if (margin >= 0) s_autosq_margin = margin;
    s_autosq_req = true;
}
int scan_engine_autosquelch_floor(void) { return s_autosq_floor; }

/**/
static bool zone_admits(const scan_channel_t *c)
{
    return s_zone == ZONE_ALL || c->zone == (uint8_t)s_zone;
}

static bool channel_eligible(const scan_channel_t *c)
{
    if (!(c->flags & SCAN_FLAG_ENABLED)) return false;
    if (c->flags & SCAN_FLAG_LOCKOUT)    return false;
    /**/
    if (c->mode != s_fg_mode)            return false;
    return zone_admits(c);
}

static int candidate_count(void)
{
    if (s_src == SCAN_SRC_BAND) return band_steps();
    int eligible = 0;
    int total = scan_channels_count();
    for (int i = 0; i < total; ++i) {
        const scan_channel_t *c = scan_channel_get(i);
        if (c && channel_eligible(c) && !sess_skipped(i)) eligible++;
    }
    return eligible;
}

/**/
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
        /**/
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

/**/
static int measure_peak(int settle_ms, int win_ms, int mode)
{
    vTaskDelay(pdMS_TO_TICKS(settle_ms));

    int pk = 0;
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < (int64_t)win_ms * 1000) {
        /**/

        if (!(s_enabled || s_autosq_busy) || !scan_foreground()) break;
        int p = rx_power_pct(mode);
        if (p > pk) pk = p;
        vTaskDelay(pdMS_TO_TICKS(POWER_POLL_MS));
    }
    return pk;
}

/**/
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
        ls_radio_err_t tune_error = LS_RADIO_OK;
        if (!wait_for_tune(c->freq_hz, c->mode, &tune_error)) {
            s_scan_error = tune_error;
            return -1;
        }
        int pk = measure_peak(PRI_SETTLE_MS, PRI_MEASURE_MS, c->mode);
        if (!s_enabled || !scan_foreground()) return -1;
        /**/
        if (pk >= stop_threshold(c->mode)) return i;
    }
    return -1;
}

static void scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        /**/
        /* Handled BEFORE the enabled check: calibrating then scanning is the
           natural order, so autosql has to work with the scanner stopped.
           s_fg_mode is only maintained by the scanning path below, so refresh
           it here or a cold calibration measures against a stale -1. */
        if (s_autosq_req) {
            s_autosq_req = false;
            s_fg_mode = foreground_mode();
            do_autosquelch();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!s_enabled) {
            s_cur = -1;
            s_started = false;
            strncpy(s_status, "off", sizeof(s_status) - 1);
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        /**/
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

        /**/
        if (fg == SCAN_MODE_NFM) nfm_ensure_listen();

        /* Report absence/contention before issuing another optimistic tune.
           Empty/disabled lists are checked first so their actionable reason
           is not hidden by an unrelated receiver condition. */
        if (candidate_count() == 0) {
            s_cur = -1;
            if (s_src == SCAN_SRC_BAND)
                snprintf(s_status, sizeof(s_status), "empty band range");
            else
                empty_reason(s_status, sizeof(s_status));
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        ls_iq_control_status_t receiver;
        receiver_status_for(fg, &receiver);
        if (!receiver.receiver_streaming) {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        int idx;
        const scan_channel_t *c;

        /**/
        if (s_src == SCAN_SRC_BAND) {
            int n = band_steps();
            if (n <= 0) {
                s_cur = -1;
                snprintf(s_status, sizeof(s_status),
                         "band %.4f-%.4f step %lu - empty range",
                         s_band_start / 1e6, s_band_stop / 1e6,
                         (unsigned long)s_band_step);
                vTaskDelay(pdMS_TO_TICKS(300));
                continue;
            }
            if (s_band_pos >= n) s_band_pos = 0;
            if (s_band_pos == 0) { s_pk_max = s_pk_acc; s_pk_acc = 0; }
            idx = s_band_pos;
            s_band_pos = (s_band_pos + 1) % n;
            c = band_channel(idx);
        } else {
            /**/
            if (s_force_idx < 0 && s_order_pos == 0) {
                rebuild_order();
                s_pk_max = s_pk_acc;
                s_pk_acc = 0;
            }
            if (s_order_n == 0) {
                s_cur = -1;
                /**/
                empty_reason(s_status, sizeof(s_status));
                vTaskDelay(pdMS_TO_TICKS(300));
                continue;
            }

            /**/
            if (s_force_idx >= 0) {
                idx = s_force_idx;
                s_force_idx = -1;
            } else {
                idx = s_order[s_order_pos];
                s_order_pos = (s_order_pos + 1) % s_order_n;
            }
            c = scan_channel_get(idx);
        }
        /**/
        /**/
        if (!c || c->mode != s_fg_mode) {
            vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
            continue;
        }

        s_scan_error = LS_RADIO_OK;
        tune_to(c);
        ls_radio_err_t tune_error = LS_RADIO_OK;
        if (!wait_for_tune(c->freq_hz, c->mode, &tune_error)) {
            if (s_enabled && scan_foreground()) {
                s_scan_error = tune_error;
                snprintf(s_status, sizeof(s_status),
                         "tune %.4f failed", c->freq_hz / 1e6);
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            continue;
        }
        s_started = true;

        /**/
        int pwi = measure_peak(SETTLE_MS, MEASURE_MS, c->mode);
        if (pwi > s_pk_acc) s_pk_acc = pwi;
        if (!s_enabled || !scan_foreground()) continue;

        /**/
        if (pwi < stop_threshold(c->mode)) {
            snprintf(s_status, sizeof(s_status), "SCAN %-9s p=%02d", c->name, pwi);
            continue;
        }
        snprintf(s_status, sizeof(s_status), "CHECK %-9s p=%02d", c->name, pwi);

        /**/
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

        /**/
        const bool cur_is_pri = (c->flags & SCAN_FLAG_PRIORITY) != 0;
        const uint32_t hold_hz = c->freq_hz;

        int64_t last = esp_timer_get_time();
        int64_t pri_next = esp_timer_get_time() + (int64_t)s_pri_ms * 1000;
        for (;;) {
            if (!s_enabled || !scan_foreground()) break;
            if (s_skip_req) { s_skip_req = false; sess_skip(idx); break; }
            /**/
            if (carrier_held(c->mode)) last = esp_timer_get_time();
            else if (esp_timer_get_time() - last > (int64_t)s_hang_ms * 1000) break;

            /**/
            /**/
            /* Priority is a STORED-CHANNEL idea and is off in band mode.
               priority_sample() walks the channel store and returns a channel
               index; band mode's idx is a grid position, so letting it through
               would both retune into a stored channel mid-band-scan and hand
               back an index meaning something else entirely. */
            if (s_src == SCAN_SRC_CHANNELS &&
                s_pri_ms > 0 && !cur_is_pri && esp_timer_get_time() >= pri_next) {
                int hit = priority_sample();
                if (hit >= 0 && hit != idx) {
                    s_force_idx = hit;
                    break;
                }
                /**/
                /**/
                if (c->mode == SCAN_MODE_P25) p25_request_tune(hold_hz, true);
                else                          lakeshark_fm_tune_transient(hold_hz);
                ls_radio_err_t restore_error = LS_RADIO_OK;
                if (!wait_for_tune(hold_hz, c->mode, &restore_error)) {
                    s_scan_error = restore_error;
                    break;
                }
                s_scan_error = LS_RADIO_OK;
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
    /**/
    s_zone = settings_get_scan_zone();
    TaskHandle_t task = xTaskCreateStaticPinnedToCore(
        scan_task, "scan_eng", SCAN_STACK_WORDS, NULL, 4,
        s_scan_stack, &s_scan_tcb, 0);
    s_task_ready = task != NULL;
    if (s_task_ready) ESP_LOGI(TAG, "scan engine ready");
    else ESP_LOGE(TAG, "scan engine task could not be created");
}

void scan_engine_start(void)
{
    /* scan may be started from the panel or the console.  Cancel at
     * this common boundary so its direct P25 tune_to() calls cannot race a
     * profile control survey, regardless of who pressed start. */
    if (selected_mode() == SCAN_MODE_P25)
    {
        (void)p25_program_survey_cancel_now(P25_SURVEY_CANCEL_MANUAL_TUNE);
        /* If start lands during a followed call, transition the follower to
           control before the carrier scanner begins. Otherwise a later call
           timeout/terminator can overwrite the scan tune with its stale
           return-to-control request. The first carrier candidate supersedes
           this request in the same owner latch. */
        p25_return_to_control();
    }
    s_session_skip = 0;
    s_order_pos    = 0;
    s_skip_req     = false;
    s_pk_max       = 0;
    s_pk_acc       = 0;
    s_force_idx    = -1;
    s_started      = false;
    s_scan_error   = LS_RADIO_OK;
    snprintf(s_status, sizeof(s_status), "waiting for first actual tune");
    s_enabled      = s_task_ready;
}

void scan_engine_stop(void)
{
    s_enabled = false;
    s_started = false;
    s_scan_error = LS_RADIO_OK;
}
bool scan_engine_active(void) { return s_enabled; }

/**/
/* "Is the engine driving the tuner fast right now", which is the question the
   retune guard actually needs to ask. scan_engine_active() answers a
   different one - "is the scanner switched on" - and the autosquelch sweep
   hops just as fast while that is false. Keep the FM and P25 retune guards on
   THIS, and keep them identical to each other. */
bool scan_engine_sweeping(void) { return s_enabled || s_autosq_busy; }
void scan_engine_skip(void) { s_skip_req = true; }

void scan_engine_set_hang_ms(int ms)
{
    if (ms < 0) ms = 0;
    if (ms > 30000) ms = 30000;
    s_hang_ms = ms;
}

/**/
void scan_engine_set_threshold_pct(int pct)
{
    if (pct < 1)   pct = 1;
    if (pct > 100) pct = 100;
    s_thresh = pct;
}

/**/
void scan_engine_set_zone(int zone)
{
    if (zone < 0) zone = ZONE_ALL;
    else if (zone >= SCAN_MAX_ZONES) zone = SCAN_MAX_ZONES - 1;
    s_zone = zone;
    settings_set_scan_zone(zone);
    s_order_pos = 0;
    s_session_skip = 0;
}

/**/
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

static void feedback_input(scan_feedback_input_t *input)
{
    memset(input, 0, sizeof(*input));
    const int mode = selected_mode();
    input->task_ready = s_task_ready;
    input->enabled = s_enabled;
    input->switching = app_switch_in_progress();
    input->parked = app_parked();
    input->foreground_supported = mode >= 0;
    input->receiver_present = lakeshark_iq_receiver_ready();
    input->started = s_started;
    input->holding = s_cur >= 0;
    input->candidates = mode >= 0 && mode == s_fg_mode
                            ? candidate_count() : -1;
    input->scan_error = s_scan_error;
    receiver_status_for(mode, &input->receiver);
}

scan_phase_t scan_engine_phase(void)
{
    scan_feedback_input_t input;
    feedback_input(&input);
    return scan_feedback_classify(&input);
}

void scan_engine_status(char *buf, size_t n)
{
    if (!buf || n == 0) return;
    scan_feedback_input_t input;
    feedback_input(&input);
    scan_feedback_format(&input,
                         s_src == SCAN_SRC_BAND ? "BAND" : "PRESET",
                         s_status, buf, n);
}
