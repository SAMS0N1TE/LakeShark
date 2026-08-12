/*LS-500*/

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
#include "rtl-sdr.h"
#include "rec_state.h"

#include "bsp/esp-bsp.h"

static const char *TAG = "rec";

extern rtlsdr_dev_t *rtlsdr_dev_get(void);

#define REC_DIR       BSP_SPIFFS_MOUNT_POINT
#define REC_USB_BUF   8192

#define REC_US_PER_SAMPLE_Q8  ((256u * 1000000u) / REC_RTL_RATE)

static rtlsdr_dev_t *s_dev = NULL;
static volatile bool s_active  = false;
static volatile bool s_running = false;

static int32_t *s_edge;
static int      s_edges;

static volatile rec_phase_t s_phase = REC_IDLE;
static uint32_t s_freq_hz = REC_DEFAULT_FREQ;
static int      s_gain    = REC_DEFAULT_GAIN;
static uint32_t s_captures = 0;
static char     s_last_file[40] = "";

static int s_mag_now = 0, s_mag_floor = 0, s_mag_thresh = 0;

/*LS-502*/
#define REC_FLOOR_SHIFT 12
#define REC_MIN_SNR     10
static int32_t s_floor_acc = 0;
static int      s_thresh_fixed = 0;
/*LS-504*/
static uint32_t s_gap_end_us = REC_GAP_END_US;

/*LS-516*/
static uint32_t s_bw_hz        = 0;
static uint32_t s_min_pulse_us = REC_MIN_PULSE_US;
static uint32_t s_max_span_us  = REC_MAX_SPAN_US;
static int      s_min_edges    = REC_MIN_EDGES;

/*LS-517*/
static int      s_end_reason = REC_END_NONE;
static uint32_t s_min_mark_us = 0, s_max_mark_us = 0, s_baud_est = 0;

static bool     s_level  = false;
static uint32_t s_run_samples = 0;
static uint32_t s_span_us = 0;

/*LS-506*/
static volatile bool s_arm_pending = false;

/*LS-507*/
static volatile uint32_t s_bytes_sec = 0;

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
    /*LS-517*/
    s_end_reason  = REC_END_NONE;
    s_min_mark_us = 0;
    s_max_mark_us = 0;
    s_baud_est    = 0;
}

/*LS-517*/
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
    /*LS-514*/
    int blk_peak = 0;

    for (int i = 0; i + 1 < len; i += 2) {
        int di = (int)iq[i]     - 127;
        int dq = (int)iq[i + 1] - 127;
        if (di < 0) di = -di;
        if (dq < 0) dq = -dq;
        int mag = di + dq;

        /*LS-514*/
        if (mag > blk_peak) blk_peak = mag;

        /*LS-502*/
        s_floor_acc += mag - (s_floor_acc >> REC_FLOOR_SHIFT);
        s_mag_floor = s_floor_acc >> REC_FLOOR_SHIFT;
        if (s_mag_floor < 2) s_mag_floor = 2;

        int on_thresh, off_thresh;
        /*LS-503*/
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
            /*LS-502*/
            if (!hi && s_level && run_us >= s_min_pulse_us) {
                s_phase = REC_CAPTURING;
                rec_reset_capture();
                s_edge[s_edges++] = (int32_t)run_us;
                s_span_us = run_us;
                s_level = false;
                s_run_samples = 1;
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

    /*LS-514*/
    s_mag_now = blk_peak;

    if (s_phase == REC_CAPTURING) {
        /*LS-505*/
        uint32_t idle_us = 0;
        if (!s_level) {
            idle_us = samples_to_us(s_run_samples);
            if (s_edges > 0 && s_edge[s_edges - 1] < 0) {
                idle_us += (uint32_t)(-s_edge[s_edges - 1]);
            }
        }
        /*LS-504*/
        bool quiet_end = (!s_level && idle_us >= s_gap_end_us);
        if (quiet_end && s_edges < s_min_edges) {
            ESP_LOGD(TAG, "discarding %d-edge blip, still armed", s_edges);
            rec_reset_capture();
            s_phase = REC_ARMED;
        } else if (quiet_end || s_span_us >= s_max_span_us ||
                   s_edges >= REC_MAX_EDGES) {
            /*LS-517*/
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

static void rec_apply_radio(void)
{
    if (!s_dev) return;
    rtlsdr_set_sample_rate(s_dev, REC_RTL_RATE);
    /*LS-516*/
    rtlsdr_set_tuner_bandwidth(s_dev, s_bw_hz);
    rtlsdr_set_tuner_gain_mode(s_dev, s_gain > 0 ? 1 : 0);
    if (s_gain > 0) rtlsdr_set_tuner_gain(s_dev, s_gain);
    rtlsdr_set_agc_mode(s_dev, s_gain > 0 ? 0 : 1);
    rtlsdr_set_center_freq(s_dev, s_freq_hz);
}

static void rec_rx_task(void *arg)
{
    (void)arg;
    uint8_t *iq = malloc(REC_USB_BUF);
    if (!iq) {
        ESP_LOGE(TAG, "OOM iq buf");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    s_running = true;
    bool stream_started = false;
    if (s_dev && rtlsdr_stream_start(s_dev) == 0) stream_started = true;

    ESP_LOGI(TAG, "rx task up: %.4f MHz %u kSPS gain=%d",
             s_freq_hz / 1e6, (unsigned)(REC_RTL_RATE / 1000), s_gain);

    /*LS-507*/
    int64_t  win_us = esp_timer_get_time();
    uint32_t win_bytes = 0;

    while (s_active) {
        if (!s_dev) {
            s_dev = rtlsdr_dev_get();
            if (s_dev) {
                rec_apply_radio();
                if (!stream_started && rtlsdr_stream_start(s_dev) == 0) stream_started = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        int got = rtlsdr_stream_read(iq, REC_USB_BUF);

        /*LS-507*/
        if (got > 0) win_bytes += (uint32_t)got;
        int64_t now_us = esp_timer_get_time();
        if (now_us - win_us >= 1000000) {
            s_bytes_sec = (uint32_t)(((uint64_t)win_bytes * 1000000u) /
                                     (uint64_t)(now_us - win_us));
            win_bytes = 0;
            win_us = now_us;
        }

        if (got <= 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        /*LS-514*/
        slice_block(iq, got);
    }

    s_bytes_sec = 0;
    free(iq);
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

    s_dev = rtlsdr_dev_get();
    rec_apply_radio();
    if (!s_dev) ESP_LOGW(TAG, "no RTL device available");

    s_active = true;
    xTaskCreatePinnedToCore(rec_rx_task, "rec_rx", 4096, NULL, 6, NULL, 1);

    /*LS-506*/
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

void rec_get_status(rec_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->phase       = s_phase;
    out->freq_hz     = s_freq_hz;
    out->gain_tenths = s_gain;
    out->edges       = s_edges;
    out->span_us     = s_span_us;
    out->mag_now     = s_mag_now;
    out->mag_floor   = s_mag_floor;
    out->mag_thresh  = s_mag_thresh;
    out->thresh_fixed = s_thresh_fixed;
    out->gap_ms       = rec_get_gap_ms();
    out->captures    = s_captures;
    out->bytes_sec   = s_bytes_sec;
    /*LS-516*/
    out->bw_hz        = s_bw_hz;
    out->min_pulse_us = s_min_pulse_us;
    out->max_span_us  = s_max_span_us;
    out->min_edges    = s_min_edges;
    /*LS-517*/
    out->end_reason  = s_end_reason;
    out->min_mark_us = s_min_mark_us;
    out->max_mark_us = s_max_mark_us;
    out->baud_est    = s_baud_est;
    strlcpy(out->last_file, s_last_file, sizeof(out->last_file));
}

/*LS-507*/
uint32_t rec_bytes_sec(void) { return s_bytes_sec; }

/*LS-508*/
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
    if (s_dev) rtlsdr_set_center_freq(s_dev, s_freq_hz);
}

uint32_t rec_get_freq(void) { return s_freq_hz; }

void rec_set_gain(int tenths)
{
    if (tenths < 0)   tenths = 0;
    if (tenths > 496) tenths = 496;
    s_gain = tenths;
    if (s_dev) {
        rtlsdr_set_tuner_gain_mode(s_dev, s_gain > 0 ? 1 : 0);
        if (s_gain > 0) rtlsdr_set_tuner_gain(s_dev, s_gain);
        rtlsdr_set_agc_mode(s_dev, s_gain > 0 ? 0 : 1);
    }
}

bool rec_active(void) { return s_active; }

void rec_arm(void)
{
    rec_reset_capture();
    s_phase = REC_ARMED;
    ESP_LOGI(TAG, "armed at %.4f MHz - waiting for carrier", s_freq_hz / 1e6);
}

/*LS-506*/
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

/*LS-503*/
void rec_set_thresh(int absolute)
{
    if (absolute < 0)   absolute = 0;
    if (absolute > 255) absolute = 255;
    s_thresh_fixed = absolute;
}

int rec_get_thresh(void) { return s_thresh_fixed; }

/*LS-504*/
void rec_set_gap_ms(int ms)
{
    if (ms < 2)    ms = 2;
    if (ms > 2000) ms = 2000;
    s_gap_end_us = (uint32_t)ms * 1000u;
}

int rec_get_gap_ms(void) { return (int)(s_gap_end_us / 1000u); }

/*LS-516*/
void rec_set_bw(uint32_t hz)
{
    if (hz && hz < 50000u)   hz = 50000u;
    if (hz > 8000000u)       hz = 8000000u;
    s_bw_hz = hz;
    if (s_dev) rtlsdr_set_tuner_bandwidth(s_dev, s_bw_hz);
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
    /*LS-513*/
    if (s_phase == REC_CAPTURING) return -3;
    if (s_edges <= 0) return -1;

    char clean[24];
    sanitize_name(name && *name ? name : "capture", clean, sizeof(clean));

    char path[64];
    snprintf(path, sizeof(path), REC_DIR "/%s.sub", clean);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for write", path);
        return -2;
    }

    fprintf(f, "Filetype: Flipper SubGhz RAW File\n");
    fprintf(f, "Version: 1\n");
    fprintf(f, "Frequency: %lu\n", (unsigned long)s_freq_hz);
    fprintf(f, "Preset: FuriHalSubGhzPresetOok650Async\n");
    fprintf(f, "Protocol: RAW\n");

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

    fclose(f);

    strlcpy(s_last_file, clean, sizeof(s_last_file));
    if (path_out) strlcpy(path_out, path, path_len);
    ESP_LOGI(TAG, "wrote %s (%d edges)", path, s_edges);
    return s_edges;
}

int rec_list(char *out, size_t len)
{
    if (!out || len < 2) return 0;
    out[0] = '\0';

    DIR *d = opendir(REC_DIR);
    if (!d) return 0;

    int n = 0;
    size_t used = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".sub") != 0) continue;

        char full[80];
        snprintf(full, sizeof(full), REC_DIR "/%s", e->d_name);
        struct stat st;
        long sz = (stat(full, &st) == 0) ? (long)st.st_size : -1;

        int w = snprintf(out + used, len - used, "%s%s (%ld B)",
                         used ? ", " : "", e->d_name, sz);
        if (w < 0 || (size_t)w >= len - used) break;
        used += (size_t)w;
        n++;
    }
    closedir(d);
    return n;
}

int rec_dump(const char *name, void (*emit)(const char *line, void *ctx), void *ctx)
{
    if (!name || !emit) return -1;

    char clean[24];
    sanitize_name(name, clean, sizeof(clean));

    char path[64];
    snprintf(path, sizeof(path), REC_DIR "/%s.sub", clean);

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
    char path[64];
    snprintf(path, sizeof(path), REC_DIR "/%s.sub", clean);
    return unlink(path) == 0 ? 0 : -2;
}
