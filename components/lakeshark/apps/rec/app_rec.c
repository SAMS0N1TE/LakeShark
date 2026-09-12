/**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "app_registry.h"
#include "settings.h"
#include "iq_app_control.h"
#include "radio_endpoint.h"
#include "rec_state.h"
#include "rec_unique_name.h"
#include "rec_file_open.h"
#include "rec_list_format.h"
#include "rec_sidecar.h"
/**/
#include "rec_space.h"
/**/
#include "spectrum.h"
/**/
#include "ls_time.h"
/**/
#include "ls_version.h"

#include "bsp/esp-bsp.h"
/* statvfs() is not available in ESP-IDF v5.5.4's picolibc, so
   the "free bytes" probe reads through the FATFS and SPIFFS VFS info calls
   directly.  esp_vfs_fat_info takes a mount base_path, not a path inside
   the partition; esp_spiffs_info reports total/used and NULL asks the
   runtime for the only registered partition, which is how bsp_spiffs_mount
   registers it. */
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"

static const char *TAG = "rec";

/**/
/* Captures land on the SD card when one is mounted and fall back to SPIFFS when it is not. */

#define REC_SD_DIR      BSP_SD_MOUNT_POINT "/lakeshark"
#define REC_SPIFFS_DIR  BSP_SPIFFS_MOUNT_POINT
#define REC_IQ_READ_BYTES 8192
#define REC_READ_TIMEOUT_MS 20

#define REC_US_PER_SAMPLE_Q8  ((256u * 1000000u) / REC_RTL_RATE)

static ls_radio_session_t *s_session;
static ls_iq_control_t s_radio_control;
static volatile bool s_active  = false;
static volatile bool s_running = false;

static int32_t *s_edge;
static int      s_edges;

static volatile rec_phase_t s_phase = REC_IDLE;
static uint32_t s_freq_hz = REC_DEFAULT_FREQ;
static int      s_gain    = REC_DEFAULT_GAIN;
static uint32_t s_captures = 0;
static char     s_last_file[64] = "";

static int s_mag_now = 0, s_mag_floor = 0, s_mag_thresh = 0;
/* Peak magnitude across the whole capture, snapshotted into the
   sidecar at SAVE time.  s_mag_now is only the last block's peak - a strong
   burst that ended a few blocks before SAVE would have already decayed
   there, so a capture-scoped max is the honest thing to record. */
static int s_capture_peak = 0;
/* Floor level at the moment the capture actually landed - so the
   sidecar records the noise the detector was looking at, not whatever the
   floor drifted to after the burst.  s_mag_floor keeps drifting after the
   capture ends. */
static int s_capture_floor_at_start = 0;

/**/
#define REC_FLOOR_SHIFT 12
#define REC_MIN_SNR     10
static int32_t s_floor_acc = 0;
static int      s_thresh_fixed = 0;
/**/
static uint32_t s_gap_end_us = REC_GAP_END_US;

/**/
static uint32_t s_bw_hz        = 0;
static uint32_t s_min_pulse_us = REC_MIN_PULSE_US;
static uint32_t s_max_span_us  = REC_MAX_SPAN_US;
static int      s_min_edges    = REC_MIN_EDGES;

/**/
static int      s_end_reason = REC_END_NONE;
static uint32_t s_min_mark_us = 0, s_max_mark_us = 0, s_baud_est = 0;

static bool     s_level  = false;
static uint32_t s_run_samples = 0;
static uint32_t s_span_us = 0;

/**/
static volatile bool s_arm_pending = false;

/**/
static volatile uint32_t s_bytes_sec = 0;

/**/

#define REC_SCOUT_ACCUM      4
#define REC_SCOUT_FLOOR_DB   (-85.0f)
#define REC_SCOUT_TOP_DB     (-20.0f)

static volatile bool s_scout_active = false;
static volatile bool s_scout_reset  = false;
static EXT_RAM_BSS_ATTR float s_scout_db[SPEC_FFT_N];
static EXT_RAM_BSS_ATTR float s_scout_pub[SPEC_FFT_N];
static volatile bool     s_scout_ready = false;
static volatile uint32_t s_scout_sweeps = 0;
static volatile uint32_t s_scout_peak_hz = 0;
static volatile float    s_scout_peak_lev = 0.0f;

static inline uint32_t samples_to_us(uint32_t n)
{
    return (uint32_t)(((uint64_t)n * REC_US_PER_SAMPLE_Q8) >> 8);
}

static void rec_reset_capture(void)
{
    s_edges = 0;
    s_level = false;
    s_run_samples = 0;
    s_span_us = 0;
    /**/
    s_end_reason  = REC_END_NONE;
    s_min_mark_us = 0;
    s_max_mark_us = 0;
    s_baud_est    = 0;
    /**/
    s_capture_peak = 0;
    s_capture_floor_at_start = 0;
}

/**/
static void rec_finish(int reason)
{
    s_end_reason = reason;

    uint32_t mn = 0, mx = 0;
    if (s_edge) {
        for (int i = 0; i < s_edges; i++) {
            if (s_edge[i] <= 0) continue;
            uint32_t v = (uint32_t)s_edge[i];
            if (!mn || v < mn) mn = v;
            if (v > mx) mx = v;
        }
    }
    s_min_mark_us = mn;
    s_max_mark_us = mx;
    s_baud_est    = mn ? (uint32_t)(1000000UL / mn) : 0;

    s_phase = REC_DONE;
    s_captures++;
}

const char *rec_end_reason_name(int reason)
{
    switch (reason) {
    case REC_END_GAP:   return "gap";
    case REC_END_SPAN:  return "span cap";
    case REC_END_EDGES: return "edge cap";
    default:            return "-";
    }
}

static void edge_push(bool level, uint32_t samples)
{
    if (!s_edge || s_edges >= REC_MAX_EDGES) return;

    uint32_t us = samples_to_us(samples);
    if (us == 0) return;

    if (s_edges == 0 && !level) return;

    if (s_edges > 0) {
        bool prev_pos = s_edge[s_edges - 1] > 0;
        if (prev_pos == level) {
            int32_t merged = s_edge[s_edges - 1] + (level ? (int32_t)us : -(int32_t)us);
            s_edge[s_edges - 1] = merged;
            s_span_us += us;
            return;
        }
    }

    s_edge[s_edges++] = level ? (int32_t)us : -(int32_t)us;
    s_span_us += us;
}

static void slice_block(const uint8_t *iq, int len)
{
    /**/
    int blk_peak = 0;

    for (int i = 0; i + 1 < len; i += 2) {
        int di = (int)iq[i]     - 127;
        int dq = (int)iq[i + 1] - 127;
        if (di < 0) di = -di;
        if (dq < 0) dq = -dq;
        int mag = di + dq;

        /**/
        if (mag > blk_peak) blk_peak = mag;

        /**/
        /**/
        /* THE FLOOR IS TRACKED ONLY WHILE THE CARRIER IS ABSENT. */

        if (!s_level) {
            s_floor_acc += mag - (s_floor_acc >> REC_FLOOR_SHIFT);
            s_mag_floor = s_floor_acc >> REC_FLOOR_SHIFT;
            if (s_mag_floor < 2) s_mag_floor = 2;
        }

        int on_thresh, off_thresh;
        /**/
        if (s_thresh_fixed > 0) {
            on_thresh  = s_thresh_fixed;
            off_thresh = s_thresh_fixed - (s_thresh_fixed >> 2);
        } else {
            on_thresh  = s_mag_floor * 4;
            off_thresh = s_mag_floor * 2;
            if (on_thresh  < s_mag_floor + REC_MIN_SNR) on_thresh  = s_mag_floor + REC_MIN_SNR;
            if (off_thresh < s_mag_floor + REC_MIN_SNR / 2) off_thresh = s_mag_floor + REC_MIN_SNR / 2;
        }
        s_mag_thresh = on_thresh;

        bool hi = s_level ? (mag > off_thresh) : (mag > on_thresh);

        if (hi == s_level) {
            s_run_samples++;
            continue;
        }

        uint32_t run_us = samples_to_us(s_run_samples);

        if (s_phase == REC_ARMED) {
            /**/
            if (!hi && s_level && run_us >= s_min_pulse_us) {
                s_phase = REC_CAPTURING;
                rec_reset_capture();
                s_edge[s_edges++] = (int32_t)run_us;
                s_span_us = run_us;
                s_level = false;
                s_run_samples = 1;
                /* Freeze the floor as it looked when the capture
                   started - the sidecar wants the noise the detector was
                   looking AT, not whatever the floor drifted to later. */
                s_capture_floor_at_start = s_mag_floor;
                ESP_LOGI(TAG, "carrier (%lu us) - capture started",
                         (unsigned long)run_us);
                continue;
            }
            s_level = hi;
            s_run_samples = 1;
            continue;
        }

        if (s_phase == REC_CAPTURING) {
            if (run_us >= s_min_pulse_us) {
                edge_push(s_level, s_run_samples);
            } else if (s_edges > 0) {
                s_edge[s_edges - 1] += s_edge[s_edges - 1] > 0
                                           ? (int32_t)run_us : -(int32_t)run_us;
                s_span_us += run_us;
            }
        }

        s_level = hi;
        s_run_samples = 1;
    }

    /**/
    s_mag_now = blk_peak;

    if (s_phase == REC_CAPTURING && blk_peak > s_capture_peak) {
        s_capture_peak = blk_peak;
    }

    if (s_phase == REC_CAPTURING) {
        /**/
        uint32_t idle_us = 0;
        if (!s_level) {
            idle_us = samples_to_us(s_run_samples);
            if (s_edges > 0 && s_edge[s_edges - 1] < 0) {
                idle_us += (uint32_t)(-s_edge[s_edges - 1]);
            }
        }
        /**/
        bool quiet_end = (!s_level && idle_us >= s_gap_end_us);
        if (quiet_end && s_edges < s_min_edges) {
            /**/

            ESP_LOGI(TAG, "discarding %d-edge blip (min %d), still armed",
                     s_edges, s_min_edges);
            rec_reset_capture();
            s_phase = REC_ARMED;
        } else if (quiet_end || s_span_us >= s_max_span_us ||
                   s_edges >= REC_MAX_EDGES) {
            /**/
            int reason = quiet_end            ? REC_END_GAP
                       : s_edges >= REC_MAX_EDGES ? REC_END_EDGES
                                                  : REC_END_SPAN;
            rec_finish(reason);
            ESP_LOGI(TAG, "capture done: %d edges, %lu us span, ended on %s"
                          " (mark %lu-%lu us, ~%lu baud)",
                     s_edges, (unsigned long)s_span_us,
                     rec_end_reason_name(reason),
                     (unsigned long)s_min_mark_us, (unsigned long)s_max_mark_us,
                     (unsigned long)s_baud_est);
        }
    }
}

static void rec_receiver_lost(ls_radio_err_t error)
{
    s_bytes_sec = 0;
    s_mag_now = 0;
    s_scout_ready = false;
    s_scout_reset = true;
    ls_iq_control_receiver_lost(&s_radio_control, error);
}

static ls_radio_err_t rec_configure_radio(void)
{
    if (!s_session) return LS_RADIO_ERR_UNAVAILABLE;
    const ls_radio_iq_config_t requested = {
        .center_hz = s_freq_hz,
        .sample_rate_hz = REC_RTL_RATE,
        .bandwidth_hz = s_bw_hz,
        .gain_mode = s_gain == 0 ? LS_RADIO_GAIN_AUTO
                                 : LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = s_gain,
    };
    ls_radio_iq_config_t actual;
    ls_radio_err_t error = ls_iq_control_configure(
        &s_radio_control, s_session, &requested, &actual);
    if (error != LS_RADIO_OK)
        ESP_LOGE(TAG, "configure failed: %s", ls_radio_err_name(error));
    return error;
}

static bool rec_radio_open(void)
{
    const ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .min_hz = 24000000UL,
        .max_hz = 1766000000UL,
        .sample_rate_hz = REC_RTL_RATE,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
    };
    ls_radio_err_t error = ls_radio_acquire("rec", &requirements, &s_session);
    if (error != LS_RADIO_OK) {
        rec_receiver_lost(error);
        return false;
    }
    error = rec_configure_radio();
    if (error == LS_RADIO_OK) error = ls_radio_iq_start(s_session);
    if (error != LS_RADIO_OK) {
        rec_receiver_lost(error);
        ESP_LOGE(TAG, "radio open failed: %s", ls_radio_err_name(error));
        ls_radio_release(s_session);
        s_session = NULL;
        return false;
    }
    ls_iq_control_set_streaming(&s_radio_control, true, LS_RADIO_OK);
    return true;
}

static void rec_rx_task(void *arg)
{
    (void)arg;
    uint8_t *iq = heap_caps_malloc(REC_IQ_READ_BYTES, MALLOC_CAP_SPIRAM);
    if (!iq) iq = malloc(REC_IQ_READ_BYTES);
    if (!iq) {
        ESP_LOGE(TAG, "OOM iq buf");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    s_running = true;
    ESP_LOGI(TAG, "rx task up: %.4f MHz %u kSPS gain=%d",
             s_freq_hz / 1e6, (unsigned)(REC_RTL_RATE / 1000), s_gain);

    /**/
    int64_t  win_us = esp_timer_get_time();
    uint32_t win_bytes = 0;

    /**/
    int scout_folds = 0;
    s_scout_reset = true;

    while (s_active) {
        if (!s_session) {
            if (!rec_radio_open()) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        ls_iq_control_request_t radio_request;
        if (ls_iq_control_take(&s_radio_control, &radio_request)) {
            if (radio_request.flags & LS_IQ_CONTROL_BANDWIDTH) {
                (void)ls_radio_iq_stop(s_session);
                ls_radio_err_t error = rec_configure_radio();
                if (error == LS_RADIO_OK)
                    error = ls_radio_iq_start(s_session);
                if (error != LS_RADIO_OK) {
                    rec_receiver_lost(error);
                    ls_radio_release(s_session);
                    s_session = NULL;
                    continue;
                }
                ls_iq_control_set_streaming(&s_radio_control, true,
                                            LS_RADIO_OK);
            } else {
                if (radio_request.flags & LS_IQ_CONTROL_TUNE) {
                    ls_radio_err_t error = ls_iq_control_apply_tune(
                        &s_radio_control, s_session, &radio_request);
                    if (error != LS_RADIO_OK)
                        ESP_LOGW(TAG, "retune failed: %s",
                                 ls_radio_err_name(error));

                    s_scout_reset = true;
                }
                if (radio_request.flags & LS_IQ_CONTROL_GAIN) {
                    ls_radio_err_t error = ls_iq_control_apply_gain(
                        &s_radio_control, s_session, &radio_request);
                    if (error != LS_RADIO_OK)
                        ESP_LOGW(TAG, "gain failed: %s",
                                 ls_radio_err_name(error));
                }
            }
        }

        size_t got = 0;
        ls_radio_err_t read_error = ls_radio_iq_read(
            s_session, iq, REC_IQ_READ_BYTES, REC_READ_TIMEOUT_MS, &got);

        /**/
        if (read_error == LS_RADIO_OK && got > 0) win_bytes += (uint32_t)got;
        int64_t now_us = esp_timer_get_time();
        if (now_us - win_us >= 1000000) {
            s_bytes_sec = (uint32_t)(((uint64_t)win_bytes * 1000000u) /
                                     (uint64_t)(now_us - win_us));
            win_bytes = 0;
            win_us = now_us;
        }

        if (read_error != LS_RADIO_OK || got == 0) {
            if (read_error == LS_RADIO_ERR_DISCONNECTED) {

                rec_receiver_lost(LS_RADIO_ERR_DISCONNECTED);
                win_bytes = 0;
                win_us = now_us;
                ls_radio_release(s_session);
                s_session = NULL;
            }
            continue;
        }

        /**/
        slice_block(iq, (int)got);

        /**/
        /* Fold the same IQ block into the scout FFT and publish a
           frame every REC_SCOUT_ACCUM accumulations.  Only runs when
           scout is enabled, so a headless capture is not paying for
           an FFT nobody is watching. */
        if (s_scout_active) {
            if (s_scout_reset) {
                spectrum_reset();
                scout_folds   = 0;
                s_scout_ready = false;
                s_scout_reset = false;
            }
            spectrum_accum(iq, (int)got);
            if (++scout_folds >= REC_SCOUT_ACCUM) {
                scout_folds = 0;
                if (spectrum_read_db(s_scout_db, SPEC_FFT_N)) {
                    const float rng = REC_SCOUT_TOP_DB - REC_SCOUT_FLOOR_DB;
                    uint32_t center = s_freq_hz;
                    uint32_t peak_hz = 0;
                    float peak_lev   = 0.0f;
                    for (int i = 0; i < SPEC_FFT_N; i++) {
                        float lin = (s_scout_db[i] - REC_SCOUT_FLOOR_DB) / rng;
                        if (lin < 0.0f) lin = 0.0f;
                        if (lin > 1.0f) lin = 1.0f;
                        s_scout_pub[i] = lin;
                        if (lin > peak_lev) {
                            peak_lev = lin;
                            /* bin i covers this shifted slice; centre of
                               the bin is (i - N/2) * rate / N off centre. */
                            int32_t off = (int32_t)(((int64_t)(i - SPEC_FFT_N / 2)
                                                     * REC_RTL_RATE) / SPEC_FFT_N);
                            int64_t hz = (int64_t)center + off;
                            if (hz < 0) hz = 0;
                            peak_hz = (uint32_t)hz;
                        }
                    }
                    s_scout_peak_hz  = peak_hz;
                    s_scout_peak_lev = peak_lev;
                    s_scout_sweeps++;
                    s_scout_ready = true;
                }
                spectrum_reset();
            }
        }
    }

    s_bytes_sec = 0;
    if (s_session) {
        (void)ls_radio_iq_stop(s_session);
    }
    heap_caps_free(iq);
    s_running = false;
    vTaskDelete(NULL);
}

static void rec_on_enter(void)
{
    if (s_active) return;

    for (int i = 0; i < 200 && s_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_running) {
        ESP_LOGE(TAG, "previous rec_rx_task still alive - refusing to start another");
        return;
    }
    if (s_session) {
        ls_radio_release(s_session);
        s_session = NULL;
    }

    if (!s_edge) {
        s_edge = heap_caps_malloc(REC_MAX_EDGES * sizeof(int32_t), MALLOC_CAP_SPIRAM);
        if (!s_edge) {
            ESP_LOGE(TAG, "edge buffer alloc failed");
            return;
        }
    }

    rec_reset_capture();
    s_phase = REC_IDLE;
    s_floor_acc = 8 << REC_FLOOR_SHIFT;
    s_mag_floor = 8;

    /**/
    spectrum_init();
    spectrum_reset();
    s_scout_ready  = false;
    s_scout_sweeps = 0;
    s_scout_peak_hz  = 0;
    s_scout_peak_lev = 0.0f;

    ls_iq_control_reset(&s_radio_control);
    ls_iq_control_set_initial(&s_radio_control, s_freq_hz, s_gain, s_bw_hz);
    rec_receiver_lost(LS_RADIO_ERR_UNAVAILABLE);

    s_active = true;
    xTaskCreatePinnedToCore(rec_rx_task, "rec_rx", 4096, NULL, 6, NULL, 1);

    /**/
    if (s_arm_pending) {
        s_arm_pending = false;
        rec_arm();
    }
}

static void rec_on_exit(void)
{
    s_arm_pending = false;
    s_active = false;
    for (int i = 0; i < 300 && s_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_running) ESP_LOGW(TAG, "drain timeout");
    else if (s_session) {
        ls_radio_release(s_session);
        s_session = NULL;
    }
    s_phase = REC_IDLE;
}

static void rec_on_sample(uint8_t *iq, int len) { (void)iq; (void)len; }

static const app_t REC_APP = {
    .name         = "REC",
    .default_freq = REC_DEFAULT_FREQ,
    .default_rate = REC_RTL_RATE,
    .default_gain = REC_DEFAULT_GAIN,
    .banner       = "RECORDER",
    .signal_label = "SIGNAL",
    .diag_label   = "CAPTURE",
    .on_enter     = rec_on_enter,
    .on_exit      = rec_on_exit,
    .on_sample    = rec_on_sample,
};

int rec_app_register(void) { return app_register(&REC_APP); }

void rec_get_hub_status(rec_hub_status_t *out)
{
    if (!out) return;
    out->phase      = s_phase;
    out->freq_hz    = s_freq_hz;
    out->edges      = s_edges;
    out->mag_now    = s_mag_now;
    out->mag_thresh = s_mag_thresh;
    out->bytes_sec  = s_bytes_sec;
    out->captures   = s_captures;
    ls_iq_control_status_t radio;
    ls_iq_control_status(&s_radio_control, &radio);
    out->receiver_streaming = radio.receiver_streaming;
}

void rec_get_receiver_status(ls_iq_control_status_t *out)
{
    ls_iq_control_status(&s_radio_control, out);
}

void rec_get_status(rec_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->phase       = s_phase;
    out->freq_hz     = s_freq_hz;
    out->gain_tenths = s_gain;
    ls_iq_control_status_t radio;
    ls_iq_control_status(&s_radio_control, &radio);
    out->effective_freq_hz = radio.effective_center_hz;
    out->effective_gain_tenths = radio.effective_gain_tenths_db;
    out->effective_freq_known = radio.effective_center_known;
    out->effective_gain_known = radio.effective_gain_known;
    out->tune_state = radio.tune_state;
    out->gain_state = radio.gain_state;
    out->tune_error = radio.tune_error;
    out->gain_error = radio.gain_error;
    out->receiver_streaming = radio.receiver_streaming;
    out->receiver_error = radio.receiver_error;
    out->edges       = s_edges;
    out->span_us     = s_span_us;
    out->mag_now     = s_mag_now;
    out->mag_floor   = s_mag_floor;
    out->mag_thresh  = s_mag_thresh;
    out->thresh_fixed = s_thresh_fixed;
    out->gap_ms       = rec_get_gap_ms();
    out->captures    = s_captures;
    out->bytes_sec   = s_bytes_sec;
    /**/
    out->bw_hz        = s_bw_hz;
    out->min_pulse_us = s_min_pulse_us;
    out->max_span_us  = s_max_span_us;
    out->min_edges    = s_min_edges;
    /**/
    out->end_reason  = s_end_reason;
    out->min_mark_us = s_min_mark_us;
    out->max_mark_us = s_max_mark_us;
    out->baud_est    = s_baud_est;
    /**/
    out->mag_peak           = s_capture_peak;
    out->mag_floor_at_start = s_capture_floor_at_start;
    /**/
    out->bytes_free         = rec_dir_free_bytes();
    strlcpy(out->last_file, s_last_file, sizeof(out->last_file));
}

/**/
uint32_t rec_bytes_sec(void) { return s_bytes_sec; }

/**/
int rec_edge_count(void) { return s_edges; }

int rec_edges_copy(int from, int32_t *out, int max)
{
    if (!out || max <= 0 || from < 0) return -1;
    if (s_phase == REC_CAPTURING) return -1;
    if (!s_edge || from >= s_edges) return 0;

    int n = s_edges - from;
    if (n > max) n = max;
    memcpy(out, s_edge + from, (size_t)n * sizeof(int32_t));
    return n;
}

void rec_set_freq(uint32_t hz)
{
    if (hz < 1000000UL || hz > 2000000000UL) return;
    s_freq_hz = hz;
    /* The accumulator reset lives in the rx task (see
       LS_IQ_CONTROL_TUNE) so only the task that owns the FFT ever
       writes to it.  Snap the peak here so the GUI does not display
       yesterday's peak on the new centre for the second until the
       first fresh sweep lands. */
    s_scout_peak_hz  = 0;
    s_scout_peak_lev = 0.0f;
    ls_iq_control_request_tune(&s_radio_control, s_freq_hz, false);
}

uint32_t rec_get_freq(void) { return s_freq_hz; }

void rec_set_gain(int tenths)
{
    if (tenths < 0)   tenths = 0;
    if (tenths > 496) tenths = 496;
    s_gain = tenths;
    ls_iq_control_request_gain(&s_radio_control, s_gain);
}

bool rec_active(void) { return s_active; }

void rec_arm(void)
{
    rec_reset_capture();
    s_phase = REC_ARMED;
    ESP_LOGI(TAG, "armed at %.4f MHz - waiting for carrier", s_freq_hz / 1e6);
}

/**/
void rec_arm_request(void)
{
    if (s_active) {
        rec_arm();
        return;
    }
    s_arm_pending = true;
}

void rec_disarm(void)
{
    s_arm_pending = false;
    s_phase = REC_IDLE;
}

/**/
void rec_set_thresh(int absolute)
{
    if (absolute < 0)   absolute = 0;
    if (absolute > 255) absolute = 255;
    s_thresh_fixed = absolute;
}

int rec_get_thresh(void) { return s_thresh_fixed; }

/**/
void rec_set_gap_ms(int ms)
{
    if (ms < 2)    ms = 2;
    if (ms > 2000) ms = 2000;
    s_gap_end_us = (uint32_t)ms * 1000u;
}

int rec_get_gap_ms(void) { return (int)(s_gap_end_us / 1000u); }

/**/
void rec_set_bw(uint32_t hz)
{
    if (hz && hz < 50000u)   hz = 50000u;
    if (hz > 8000000u)       hz = 8000000u;
    s_bw_hz = hz;
    /* setters may run on GUI, link, or console tasks. Reconfiguration
       is posted to rec_rx so only the session-owning task touches the radio. */
    ls_iq_control_request_bandwidth(&s_radio_control, s_bw_hz);
}

uint32_t rec_get_bw(void) { return s_bw_hz; }

void rec_set_min_pulse(uint32_t us)
{
    if (us < 4)     us = 4;
    if (us > 10000) us = 10000;
    s_min_pulse_us = us;
}

uint32_t rec_get_min_pulse(void) { return s_min_pulse_us; }

void rec_set_max_span(uint32_t us)
{
    if (us < 10000)     us = 10000;
    if (us > 30000000u) us = 30000000u;
    s_max_span_us = us;
}

uint32_t rec_get_max_span(void) { return s_max_span_us; }

void rec_set_min_edges(int n)
{
    if (n < 2)             n = 2;
    if (n > REC_MAX_EDGES) n = REC_MAX_EDGES;
    s_min_edges = n;
}

int rec_get_min_edges(void) { return s_min_edges; }

/**/
void rec_scout_enable(bool on)
{
    if (s_scout_active == on) return;
    /* Off->on: request an accumulator reset so a stale burst from a
       previous enable cannot land as the first frame.  The reset
       itself runs on the rx task so the accumulator has one writer. */
    if (on) s_scout_reset = true;
    s_scout_active = on;
}

bool rec_scout_enabled(void) { return s_scout_active; }

bool rec_scout_read(float *out, int n)
{
    if (!out || n < 1 || !s_scout_ready) return false;
    for (int o = 0; o < n; o++) {
        int lo = (int)(((int64_t)o       * SPEC_FFT_N) / n);
        int hi = (int)(((int64_t)(o + 1) * SPEC_FFT_N) / n);
        if (hi <= lo) hi = lo + 1;
        if (hi > SPEC_FFT_N) hi = SPEC_FFT_N;
        float pk = 0.0f;
        for (int i = lo; i < hi; i++) {
            float v = s_scout_pub[i];
            if (v > pk) pk = v;
        }
        out[o] = pk;
    }
    return true;
}

void rec_scout_peak(uint32_t *hz_out, float *level_out)
{
    if (hz_out)    *hz_out    = s_scout_peak_hz;
    if (level_out) *level_out = s_scout_peak_lev;
}

uint32_t rec_scout_sweeps(void) { return s_scout_sweeps; }

static void sanitize_name(const char *in, char *out, size_t len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < len; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    if (!j) strlcpy(out, "capture", len);
}

int rec_save(const char *name, char *path_out, size_t path_len)
{
    /**/
    if (s_phase == REC_CAPTURING) return -3;
    if (s_edges <= 0) return -1;

    char clean[24];
    sanitize_name(name && *name ? name : "capture", clean, sizeof(clean));

    /* A process-local recNNN counter restarted at every boot, so a
       directory sorted by name did not reflect when captures happened. Keep
       the caller's label as a prefix, but make the timestamp the identity:
       wall clock after sync and the visibly different up-<seconds>s before
       sync. Existing files are only consulted by the unique-name picker and
       are never renamed. */
    char timed_name[64];
    ls_time_render_filename(timed_name, sizeof(timed_name), clean);

    /**/
    /* Refuse before opening anything if the volume cannot hold the
       capture at its worst case.  The pessimistic estimate covers the
       .sub header, per-edge textual width and the sidecar.  UINT64_MAX
       from the probe means statvfs failed - refuse in that case too,
       because writing into an unknown filesystem is how a truncated
       file lands on the FILES tab looking valid. */
    uint64_t need = rec_space_estimate_bytes(s_edges);
    uint64_t have = rec_dir_free_bytes();
    if (have == UINT64_MAX) {
        ESP_LOGE(TAG, "cannot query free space on %s - refusing save",
                 rec_dir());
        return -4;
    }
    if (!rec_space_ok(need, have)) {
        char msg[80];
        rec_space_format_shortage(msg, sizeof(msg), need, have);
        ESP_LOGE(TAG, "insufficient space on %s: %s", rec_dir(), msg);
        return -4;
    }

    /**/
    /* The GUI feeds this "rec%03lu" off a process-local counter, so after a
       reboot, a wrap or a NEWER file being deleted the same base could name
       an OLDER capture that is still on disk.  fopen("w") would silently
       truncate it.  Walk to the next free suffix instead, so an automatic
       SAVE cannot destroy an old capture without the caller renaming it. */
    char unique[64];
    if (rec_pick_unique_name(rec_dir(), timed_name, ".sub",
                             unique, sizeof(unique)) != 0) {
        ESP_LOGE(TAG, "no free name for %s in %s", timed_name, rec_dir());
        return -2;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s.sub", rec_dir(), unique);
    /**/
    /* Two-step write: land the .sub as `<name>.sub.part`, then rename to
       `<name>.sub` after the writer closes cleanly.  A crash, an unplug,
       or an ENOSPC that slipped past the estimate leaves the `.part`
       around as evidence, and the listing filter (rec_capture_name_is_partial)
       keeps it off the FILES tab so it is never mistaken for a good
       capture. */
    char part_path[128];
    snprintf(part_path, sizeof(part_path), "%s%s", path, REC_CAPTURE_PART_EXT);

    FILE *f = rec_file_open_new(part_path);
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for write", part_path);
        return -2;
    }

    fprintf(f, "Filetype: Flipper SubGhz RAW File\n");
    fprintf(f, "Version: 1\n");
    fprintf(f, "Frequency: %lu\n", (unsigned long)s_freq_hz);
    fprintf(f, "Preset: FuriHalSubGhzPresetOok650Async\n");
    fprintf(f, "Protocol: RAW\n");
    /**/
    /* When we know the wall clock (SNTP or an RTC has landed), stamp the capture with it so the file has more provenance than "the counter said 003". */

    {
        char stamp[LS_TIME_STAMP_MAX];
        ls_time_render_stamp(stamp, sizeof(stamp));
        fprintf(f, "# Recorded: %s\n", stamp);
    }

    int per_line = 0;
    for (int i = 0; i < s_edges; i++) {
        if (per_line == 0) fprintf(f, "RAW_Data:");
        fprintf(f, " %ld", (long)s_edge[i]);
        if (++per_line >= 512) {
            fprintf(f, "\n");
            per_line = 0;
        }
    }
    if (per_line) fprintf(f, "\n");

    /**/

    int write_ok = (fflush(f) == 0) && !ferror(f);
    if (fclose(f) != 0) write_ok = 0;

    if (!write_ok || rename(part_path, path) != 0) {
        ESP_LOGE(TAG, "write to %s did not complete - discarding", path);
        (void)unlink(part_path);
        return -2;
    }

    /**/

    {
        rec_sidecar_t sc;
        memset(&sc, 0, sizeof(sc));
        ls_time_render_stamp(sc.time, sizeof(sc.time));
        sc.freq_hz     = s_freq_hz;
        sc.gain_tenths = s_gain;
        sc.bw_hz       = s_bw_hz;
        sc.sample_rate = REC_RTL_RATE;
        sc.edges       = s_edges;
        sc.span_us     = s_span_us;
        sc.mag_peak    = s_capture_peak;
        sc.mag_floor   = s_capture_floor_at_start;
#ifdef ESP_PLATFORM
        {
            ls_version_info_t vi;
            ls_version_get(&vi);
            snprintf(sc.board,    sizeof(sc.board),    "%s",
                     vi.board    ? vi.board    : "?");
            snprintf(sc.firmware, sizeof(sc.firmware), "%s",
                     vi.version  ? vi.version  : "?");
        }
#else
        snprintf(sc.board,    sizeof(sc.board),    "%s", "host");
        snprintf(sc.firmware, sizeof(sc.firmware), "%s", "host");
#endif
        if (rec_sidecar_write(rec_dir(), unique, &sc) != 0) {
            ESP_LOGW(TAG, "sidecar for %s not written - .sub still landed",
                     unique);
        }
    }

    strlcpy(s_last_file, unique, sizeof(s_last_file));
    if (path_out) strlcpy(path_out, path, path_len);
    ESP_LOGI(TAG, "wrote %s (%d edges)", path, s_edges);
    return s_edges;
}

/**/
const char *rec_dir(void)
{
    static const char *s_dir = NULL;
    if (s_dir) return s_dir;

    struct stat st;
    if (stat(BSP_SD_MOUNT_POINT, &st) == 0 && S_ISDIR(st.st_mode)) {
        mkdir(REC_SD_DIR, 0777);   /* already-exists is the normal case */
        if (stat(REC_SD_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
            s_dir = REC_SD_DIR;
            ESP_LOGI(TAG, "captures -> %s (SD card)", s_dir);
            return s_dir;
        }
        ESP_LOGW(TAG, "SD mounted but %s is not usable - falling back", REC_SD_DIR);
    }

    s_dir = REC_SPIFFS_DIR;
    ESP_LOGI(TAG, "captures -> %s (no SD card)", s_dir);
    return s_dir;
}

/**/
/* Free-byte count of the volume rec_dir() points at, or UINT64_MAX when the probe fails. */

uint64_t rec_dir_free_bytes(void)
{
    const char *dir = rec_dir();
    if (!dir || !*dir) return UINT64_MAX;

    /* SD mounts land under BSP_SD_MOUNT_POINT and are FATFS; the probe
       needs the mount point (not the subdirectory rec_dir returns). */
    if (strncmp(dir, BSP_SD_MOUNT_POINT,
                strlen(BSP_SD_MOUNT_POINT)) == 0) {
        uint64_t total = 0, free_bytes = 0;
        if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_bytes) != ESP_OK)
            return UINT64_MAX;
        return free_bytes;
    }

    /* SPIFFS returns total/used; free is the difference.  A partition
       label of NULL asks the runtime to pick the only registered one,
       which matches how bsp_spiffs_mount registers it. */
    if (strncmp(dir, BSP_SPIFFS_MOUNT_POINT,
                strlen(BSP_SPIFFS_MOUNT_POINT)) == 0) {
        size_t total = 0, used = 0;
        if (esp_spiffs_info(NULL, &total, &used) != ESP_OK)
            return UINT64_MAX;
        if (used > total) return UINT64_MAX;
        return (uint64_t)(total - used);
    }

    return UINT64_MAX;
}

/**/
/* Reports the total number of .sub captures on disk, not the count that
   happened to fit in `out`.  The previous version returned only what
   fit, and the FILES tab's row cap was compared against that - so a
   directory of maximum-length names could fill the byte buffer with
   fewer than FILES_MAX rows and the tab would call the truncated view
   complete.  Byte truncation is now signalled through *out_truncated so
   the caller can OR it with its own row-capacity check. */
int rec_list(char *out, size_t len, bool *out_truncated)
{
    if (out_truncated) *out_truncated = false;
    if (out && len > 0) out[0] = '\0';

    DIR *d = opendir(rec_dir());
    if (!d) return 0;

    int total = 0;
    size_t used = 0;
    bool byte_full = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".sub") != 0) continue;

        total++;

        /* Keep counting after the buffer fills so the returned total is
           the ground truth for the FILES tab to compare against. */
        if (byte_full || !out || len < 2) continue;

        /**/
        /* Trim the ".sub" so the row shows just the base name; the walker
           in AppREC::refreshFiles takes the first space-delimited token as
           the row key when it wants to DELETE. */
        char base[64];
        size_t bl = (size_t)(dot - e->d_name);
        if (bl >= sizeof(base)) bl = sizeof(base) - 1;
        memcpy(base, e->d_name, bl);
        base[bl] = '\0';

        /**/

        uint32_t freq = 0;
        {
            char full[96];
            snprintf(full, sizeof(full), "%s/%s", rec_dir(), e->d_name);
            FILE *f = fopen(full, "r");
            if (f) {
                char hdr[64];
                while (fgets(hdr, sizeof(hdr), f)) {
                    if (!strncmp(hdr, "Frequency:", 10)) {
                        freq = (uint32_t)strtoul(hdr + 10, NULL, 10);
                        break;
                    }
                    if (!strncmp(hdr, "RAW_Data:", 9)) break;
                }
                fclose(f);
            }
        }

        /**/
        /* Time comes from the sidecar.  Older captures written before
           have no sidecar and rec_sidecar_read returns false with
           an empty struct; the row helper then emits "-" so the column
           lines up either way. */
        const char *tm = NULL;
        rec_sidecar_t sc;
        if (rec_sidecar_read(rec_dir(), base, &sc) && sc.time[0]) {
            tm = sc.time;
        }

        if (!rec_files_append_row(out, len, &used, base, freq, tm)) {
            byte_full = true;
        }
    }
    closedir(d);

    if (out_truncated) *out_truncated = byte_full;
    return total;
}

/**/
/* Indexed access to the saved set, so the head can browse without the board
   ever building a list that has to fit in one reply. rec_list() formats every
   name into one string for the GUI's FILES tab; that is fine at 768 B on the
   LVGL side and useless over a 384 B link reply (). One entry per round
   trip is the same shape %D already uses, and for the same reason. */
int rec_file_info(int index, char *name, size_t nlen, uint32_t *freq_hz, long *size)
{
    DIR *d = opendir(rec_dir());
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".sub") != 0) continue;

        if (n == index) {
            if (name && nlen) {
                size_t base = (size_t)(dot - e->d_name);
                if (base >= nlen) base = nlen - 1;
                memcpy(name, e->d_name, base);
                name[base] = '\0';
            }

            char full[96];
            snprintf(full, sizeof(full), "%s/%s", rec_dir(), e->d_name);

            if (size) {
                struct stat st;
                *size = (stat(full, &st) == 0) ? (long)st.st_size : -1;
            }
            /* Frequency lives in the header, so this is a few hundred bytes of
               read, not a full parse. The head needs it to label the row. */
            if (freq_hz) {
                *freq_hz = 0;
                FILE *f = fopen(full, "r");
                if (f) {
                    char hdr[64];
                    while (fgets(hdr, sizeof(hdr), f)) {
                        if (!strncmp(hdr, "Frequency:", 10)) {
                            *freq_hz = (uint32_t)strtoul(hdr + 10, NULL, 10);
                            break;
                        }
                        if (!strncmp(hdr, "RAW_Data:", 9)) break;
                    }
                    fclose(f);
                }
            }
        }
        n++;
    }
    closedir(d);
    return n;
}

/**/

int rec_load(int index)
{
    if (s_phase == REC_CAPTURING) return -3;
    if (!s_edge) return -2;

    char name[40];
    uint32_t freq = 0;
    int total = rec_file_info(index, name, sizeof(name), &freq, NULL);
    if (index < 0 || index >= total) return -1;

    char path[96];
    snprintf(path, sizeof(path), "%s/%s.sub", rec_dir(), name);
    FILE *f = fopen(path, "r");
    if (!f) return -2;

    /* Heap, not stack. A RAW_Data line is up to 512 values and this runs on the
       link task - is the standing warning about what a few hundred bytes
       of new stack in that path can do. */
    const size_t LINE = 6144;
    char *line = heap_caps_malloc(LINE, MALLOC_CAP_SPIRAM);
    if (!line) { fclose(f); return -2; }

    /* Take the app out of ARMED first so the rx task cannot start a capture
       into the buffer while it is being filled. */
    s_phase = REC_IDLE;
    rec_reset_capture();

    int n = 0;
    uint32_t span = 0;
    while (fgets(line, LINE, f)) {
        if (!strncmp(line, "Frequency:", 10)) {
            uint32_t hz = (uint32_t)strtoul(line + 10, NULL, 10);
            if (hz) freq = hz;
            continue;
        }
        if (strncmp(line, "RAW_Data:", 9) != 0) continue;

        char *p = line + 9, *end;
        for (;;) {
            long v = strtol(p, &end, 10);
            if (end == p) break;
            p = end;
            if (v == 0) continue;              /* zeros are illegal in a .sub */
            if (n >= REC_MAX_EDGES) break;
            s_edge[n++] = (int32_t)v;
            span += (uint32_t)(v < 0 ? -v : v);
        }
    }

    heap_caps_free(line);
    fclose(f);

    if (n <= 0) { s_phase = REC_IDLE; return -1; }

    s_edges   = n;
    s_span_us = span;
    if (freq) s_freq_hz = freq;
    strlcpy(s_last_file, name, sizeof(s_last_file));

    uint32_t saved = s_captures;
    rec_finish(REC_END_GAP);
    s_captures = saved;

    ESP_LOGI(TAG, "loaded %s (%d edges, %lu us, %.4f MHz) - ready for GET",
             name, n, (unsigned long)span, s_freq_hz / 1e6);
    return n;
}

int rec_dump(const char *name, void (*emit)(const char *line, void *ctx), void *ctx)
{
    if (!name || !emit) return -1;

    char clean[24];
    sanitize_name(name, clean, sizeof(clean));

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.sub", rec_dir(), clean);

    FILE *f = fopen(path, "r");
    if (!f) return -2;

    char line[600];
    int lines = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        emit(line, ctx);
        lines++;
    }
    fclose(f);
    return lines;
}

int rec_remove(const char *name)
{
    if (!name) return -1;
    char clean[24];
    sanitize_name(name, clean, sizeof(clean));
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.sub", rec_dir(), clean);
    int rc = unlink(path) == 0 ? 0 : -2;
    /* Best-effort remove of the sidecar too.  A missing sidecar is
       normal for older captures and unlink returning -1 is not a failure of
       the delete; the .sub is what the caller wanted gone. */
    char sidecar[96];
    snprintf(sidecar, sizeof(sidecar), "%s/%s%s",
             rec_dir(), clean, REC_SIDECAR_EXT);
    (void)unlink(sidecar);
    return rc;
}
