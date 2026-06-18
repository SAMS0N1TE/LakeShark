
#include "fm_app.h"
#include "fm_state.h"
#include "fm_dsp.h"
#include "pocsag.h"
#include "app_registry.h"
#include "settings.h"
#include "rtl-sdr.h"
#include "audio_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "fm";

static void fm_log_cpu(void)
{
    static uint32_t last_idle[2];
    static int64_t  last_us;
    TaskStatus_t *st = NULL;
    UBaseType_t n = uxTaskGetNumberOfTasks();
    st = malloc(sizeof(TaskStatus_t) * n);
    if (!st) return;
    uint32_t total_rt = 0;
    n = uxTaskGetSystemState(st, n, &total_rt);
    uint32_t idle[2] = {0, 0};
    for (UBaseType_t i = 0; i < n; i++) {
        if (st[i].pcTaskName && strncmp(st[i].pcTaskName, "IDLE", 4) == 0) {
            int core = st[i].xCoreID;
            if (core == 0 || core == 1) idle[core] += st[i].ulRunTimeCounter;
        }
    }
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - last_us;
    if (last_us && elapsed > 0) {
        int b0 = 100 - (int)((int64_t)(idle[0] - last_idle[0]) * 100 / elapsed);
        int b1 = 100 - (int)((int64_t)(idle[1] - last_idle[1]) * 100 / elapsed);
        if (b0 < 0) b0 = 0;
        if (b1 < 0) b1 = 0;
        ESP_LOGW(TAG, "CPU busy: core0=%d%% core1=%d%% (tasks=%u)", b0, b1, (unsigned)n);
    }
    last_idle[0] = idle[0]; last_idle[1] = idle[1]; last_us = now;
    free(st);
}

#define FM_USB_PACKET_SIZE  8192
#define FM_USB_BUF_LENGTH   (FM_USB_PACKET_SIZE * 2)

#define FM_SCAN_SETTLE      2
#define FM_SCAN_DWELL       3

fm_state_t FM;

static uint32_t s_mode_freq[FM_MODE_COUNT];

#define FMRX_STACK_BYTES 16384u

extern rtlsdr_dev_t *rtlsdr_dev_get(void);

static volatile bool s_active   = false;
static volatile bool s_running  = false;

static volatile int      s_mode_req    = -1;
static volatile int      s_gain_req     = -1;
static volatile uint32_t s_freq_req     = 0;
static volatile int      s_baud_req      = -1;
static volatile int      s_squelch_req   = -1;
static volatile bool     s_tune_peak_req = false;
static volatile bool     s_scan_restart  = false;

static rtlsdr_dev_t *s_dev   = NULL;
static fm_dsp_t      s_dsp;

static const int     POC_BAUDS[3] = { 512, 1200, 2400 };
static pocsag_ctx_t *s_poc[3] = { NULL, NULL, NULL };

static void poc_ensure(void)
{
    for (int i = 0; i < 3; i++)
        if (!s_poc[i]) s_poc[i] = pocsag_create(&FM, POC_BAUDS[i]);
}
static void poc_reset_all(void)
{
    for (int i = 0; i < 3; i++) if (s_poc[i]) pocsag_reset(s_poc[i]);
    FM.pocsag_lock_baud = 0;
}
static int poc_sel_index(void)
{
    for (int i = 0; i < 3; i++) if (POC_BAUDS[i] == FM.pocsag_baud) return i;
    return 1;
}

static void poc_dispatch(const float *demod, int nd)
{
    if (FM.pocsag_auto) {
        for (int i = 0; i < 3; i++) if (s_poc[i]) pocsag_process(s_poc[i], demod, nd);
    } else {
        int sel = poc_sel_index();
        if (s_poc[sel]) pocsag_process(s_poc[sel], demod, nd);
    }

    uint32_t fr = 0, pg = 0, ce = 0, ad = 0, ms = 0;
    bool sync = false; int lock_baud = 0; uint32_t best_fr = 0;
    for (int i = 0; i < 3; i++) {
        if (!s_poc[i]) continue;
        fr += pocsag_n_frames(s_poc[i]);
        pg += pocsag_n_pages(s_poc[i]);
        ce += pocsag_n_cwerr(s_poc[i]);
        ad += pocsag_n_addr(s_poc[i]);
        ms += pocsag_n_msg(s_poc[i]);
        if (pocsag_synced(s_poc[i])) {
            sync = true;
            if (pocsag_n_frames(s_poc[i]) >= best_fr) {
                best_fr = pocsag_n_frames(s_poc[i]); lock_baud = POC_BAUDS[i];
            }
        }
    }
    FM.pocsag_sync    = sync;
    FM.pocsag_frames  = fr;
    FM.pocsag_pages   = pg;
    FM.pocsag_cw_errs = ce;
    FM.pocsag_addr    = ad;
    FM.pocsag_msg     = ms;
    if (lock_baud) {
        FM.pocsag_lock_baud = lock_baud;
        if (FM.pocsag_auto) FM.pocsag_baud = lock_baud;
    }
}

static int      s_scan_phase = 0;
static int      s_scan_count = 0;
static float    s_scan_accum = 0.0f;
static int      s_scan_naccum = 0;

static void fm_apply_gain(int tenths)
{
    if (!s_dev) return;
    FM.gain_tenths = tenths;
    rtlsdr_set_tuner_gain_mode(s_dev, tenths == 0 ? 0 : 1);
    if (tenths > 0) rtlsdr_set_tuner_gain(s_dev, tenths);
    rtlsdr_set_agc_mode(s_dev, tenths == 0 ? 1 : 0);
}

static void fm_tune_hw(uint32_t hz)
{
    if (!s_dev || hz < 1000000UL) return;
    rtlsdr_set_center_freq(s_dev, hz);
    rtlsdr_reset_buffer(s_dev);
    rtlsdr_stream_reset();
}

static uint32_t fm_mode_default_freq(fm_mode_t m)
{
    switch (m) {
        case FM_MODE_WFM:    return FM_FREQ_WFM;
        case FM_MODE_POCSAG: return FM_FREQ_POCSAG;
        case FM_MODE_LISTEN: return FM_FREQ_LISTEN;
        default:             return FM_FREQ_POCSAG;
    }
}

static void fm_apply_freq(uint32_t hz)
{
    if (!s_dev || hz < 1000000UL) return;
    FM.freq_hz = hz;
    fm_tune_hw(hz);
}

static void scan_begin(bool reset_pos)
{
    uint32_t span = (FM.scan_stop_hz > FM.scan_start_hz)
                  ? (FM.scan_stop_hz - FM.scan_start_hz) : 0;
    int bins = (FM.scan_step_hz > 0) ? (int)(span / FM.scan_step_hz) + 1 : 0;
    if (bins < 1)  bins = 1;
    if (bins > FM_SCAN_BINS_MAX) bins = FM_SCAN_BINS_MAX;
    FM.scan_bins = bins;
    if (reset_pos) {
        FM.scan_idx = 0;
        FM.scan_peak_db = 0.0f; FM.scan_peak_hz = 0;
    }
    if (FM.scan_idx >= bins) FM.scan_idx = 0;
    s_scan_phase = 0; s_scan_count = 0;
    s_scan_accum = 0.0f; s_scan_naccum = 0;
    fm_tune_hw(FM.scan_start_hz + (uint32_t)FM.scan_idx * FM.scan_step_hz);
}

static void scan_step(const uint8_t *iq, int len)
{
    if (s_scan_phase == 0) {
        if (++s_scan_count >= FM_SCAN_SETTLE) { s_scan_phase = 1; s_scan_count = 0; }
        return;
    }
    s_scan_accum += fm_iq_rms(iq, len);
    s_scan_naccum++;
    if (++s_scan_count < FM_SCAN_DWELL) return;

    float e = (s_scan_naccum > 0) ? s_scan_accum / (float)s_scan_naccum : 0.0f;
    int bi = FM.scan_idx;
    if (bi >= 0 && bi < FM.scan_bins) {
        FM.scan_db[bi] = e;
        uint32_t f = FM.scan_start_hz + (uint32_t)bi * FM.scan_step_hz;
        if (e > FM.scan_peak_db) { FM.scan_peak_db = e; FM.scan_peak_hz = f; }
    }

    FM.scan_idx++;
    s_scan_phase = 0; s_scan_count = 0;
    s_scan_accum = 0.0f; s_scan_naccum = 0;
    if (FM.scan_idx >= FM.scan_bins) {
        FM.scan_sweeps++;
        FM.scan_idx = 0;
    }
    uint32_t nf = FM.scan_start_hz + (uint32_t)FM.scan_idx * FM.scan_step_hz;
    fm_tune_hw(nf);
}

static void fm_rx_task(void *arg)
{
    (void)arg;
    s_dev = rtlsdr_dev_get();
    uint8_t *iq = malloc(FM_USB_BUF_LENGTH);

    static float   demod[1100];
    static int16_t pcm[600];

    if (!iq) { ESP_LOGE(TAG, "OOM iq buf"); s_running = false; vTaskDelete(NULL); return; }
    if (!s_dev) ESP_LOGW(TAG, "no RTL device at entry");

    ESP_LOGI(TAG, "rx task up: mode=%d freq=%lu gain=%d",
             FM.mode, (unsigned long)FM.freq_hz, FM.gain_tenths);

    int read_errors = 0;
    uint32_t iq_bucket = 0;
    int64_t stats_ts = esp_timer_get_time();
    int64_t last_yield = stats_ts;
    bool stream_started = false;
    s_running = true;

    if (s_dev && rtlsdr_stream_start(s_dev) == 0) stream_started = true;

    while (s_active) {

        if (!s_dev) {
            s_dev = rtlsdr_dev_get();
            if (s_dev) {
                rtlsdr_set_sample_rate(s_dev, FM_RTL_RATE);
                rtlsdr_set_tuner_bandwidth(s_dev, 0);
                fm_apply_gain(FM.gain_tenths);
                if (FM.mode == FM_MODE_SCAN) scan_begin(false);
                else fm_apply_freq(FM.freq_hz);
                if (!stream_started && rtlsdr_stream_start(s_dev) == 0) stream_started = true;
                ESP_LOGI(TAG, "device acquired late: %.4f MHz", FM.freq_hz / 1e6);
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        if (s_mode_req >= 0) {
            int m = s_mode_req; s_mode_req = -1;

            if (FM.mode != FM_MODE_SCAN) s_mode_freq[FM.mode] = FM.freq_hz;
            FM.mode = (fm_mode_t)m;
            fm_dsp_init(&s_dsp);
            if (FM.mode == FM_MODE_SCAN) {
                scan_begin(false);
            } else {
                uint32_t f = s_mode_freq[FM.mode];
                FM.freq_hz = (f >= 1000000UL) ? f : fm_mode_default_freq(FM.mode);
                fm_apply_freq(FM.freq_hz);
            }
            if (FM.mode == FM_MODE_POCSAG) { poc_ensure(); poc_reset_all(); }
            if (FM.mode == FM_MODE_LISTEN || FM.mode == FM_MODE_WFM) audio_out_ensure_unmuted();
        }
        if (s_freq_req) { uint32_t f = s_freq_req; s_freq_req = 0; fm_apply_freq(f); }
        if (s_gain_req >= 0) { int g = s_gain_req; s_gain_req = -1; fm_apply_gain(g); }
        if (s_baud_req >= 0) {
            int b = s_baud_req; s_baud_req = -1;
            if (b == 0) FM.pocsag_auto = true;
            else { FM.pocsag_auto = false; FM.pocsag_baud = b; }
            poc_reset_all();
        }
        if (s_squelch_req >= 0) { FM.squelch_tenths = s_squelch_req; s_squelch_req = -1; }
        if (s_scan_restart) { s_scan_restart = false; if (FM.mode == FM_MODE_SCAN) scan_begin(true); }
        if (s_tune_peak_req) {
            s_tune_peak_req = false;
            if (FM.scan_peak_hz) { FM.freq_hz = FM.scan_peak_hz; }
        }

        if (!s_active || !s_dev) break;

        if (!stream_started) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        int got = 0; bool full = true;
        while (got < FM_USB_BUF_LENGTH) {
            if (!s_active || !s_dev) { full = false; break; }
            int r = rtlsdr_stream_read(&iq[got], FM_USB_BUF_LENGTH - got);
            if (r > 0) { got += r; continue; }

            if (++read_errors > 50) { full = false; read_errors = 0; break; }
            vTaskDelay(1);
        }
        if (!full || got < FM_USB_BUF_LENGTH) { continue; }
        read_errors = 0;
        iq_bucket += FM_USB_BUF_LENGTH;

        if (FM.mode == FM_MODE_SCAN) {
            scan_step(iq, FM_USB_BUF_LENGTH);
            FM.iq_level = fm_iq_rms(iq, FM_USB_BUF_LENGTH);
        } else if (FM.mode == FM_MODE_WFM) {

            int na = fm_demod_wide(&s_dsp, iq, FM_USB_BUF_LENGTH, pcm, 600);
            FM.iq_level = s_dsp.iq_peak;
            FM.squelch_open = true;
            if (na > 0) {
                audio_write_mono(pcm, na);
                float ss = 0.0f;
                for (int i = 0; i < na; i++) ss += (float)pcm[i] * (float)pcm[i];
                FM.audio_level = sqrtf(ss / na) / 8000.0f;
            }
        } else {
            int nd = fm_demod_iq(&s_dsp, iq, FM_USB_BUF_LENGTH, demod, 1100);
            FM.iq_level = s_dsp.iq_peak;

            float ss = 0.0f;
            for (int i = 0; i < nd; i++) ss += demod[i] * demod[i];
            FM.audio_level = (nd > 0) ? sqrtf(ss / nd) : 0.0f;

            if (FM.mode == FM_MODE_POCSAG) {
                poc_dispatch(demod, nd);
            } else {
                int sq_open = (int)(FM.iq_level * 100.0f) >= FM.squelch_tenths;
                FM.squelch_open = sq_open;
                if (sq_open) {
                    int na = fm_demod_to_audio(&s_dsp, demod, nd, pcm, 600);
                    if (na > 0) audio_write_mono(pcm, na);
                }
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - stats_ts > 2000000LL) {
            FM.iq_bytes_sec = iq_bucket; iq_bucket = 0; stats_ts = now;
            FM.read_errors = read_errors;

            if (FM.mode == FM_MODE_SCAN) {
                ESP_LOGI(TAG, "SCAN now=%.4f peak=%.4f (%d%%) sweeps=%lu iq=%d%%",
                    (FM.scan_start_hz + (uint32_t)FM.scan_idx * FM.scan_step_hz) / 1e6,
                    FM.scan_peak_hz / 1e6, (int)(FM.scan_peak_db * 100.0f),
                    (unsigned long)FM.scan_sweeps, (int)(FM.iq_level * 100.0f));
            } else if (FM.mode == FM_MODE_POCSAG) {
                int nm = 32; unsigned long nn = 0;
                for (int i = 0; i < 3; i++) if (s_poc[i]) {
                    int m = pocsag_near_min(s_poc[i]); if (m < nm) nm = m;
                    nn += pocsag_n_near(s_poc[i]);
                }
                ESP_LOGI(TAG, "POCSAG %.4f %s baud=%s%d frames=%lu pages=%lu err=%lu iq=%d%% act=%d%% near_min=%d near=%lu",
                    FM.freq_hz / 1e6, FM.pocsag_sync ? "SYNC" : "hunt",
                    FM.pocsag_auto ? "auto:" : "", FM.pocsag_baud,
                    (unsigned long)FM.pocsag_frames, (unsigned long)FM.pocsag_pages,
                    (unsigned long)FM.pocsag_cw_errs,
                    (int)(FM.iq_level * 100.0f), (int)(FM.audio_level / 0.65f * 100.0f),
                    nm, nn);
            } else if (FM.mode == FM_MODE_WFM) {
                ESP_LOGI(TAG, "WFM %.4f iq=%d%% af=%d%% audio(drop=%lu under=%lu)",
                    FM.freq_hz / 1e6, (int)(FM.iq_level * 100.0f),
                    (int)(FM.audio_level * 100.0f),
                    (unsigned long)audio_drops_get(), (unsigned long)audio_underruns_get());
                fm_log_cpu();
            } else {
                ESP_LOGI(TAG, "LISTEN %.4f sq=%s iq=%d%% act=%d%% audio(drop=%lu under=%lu)",
                    FM.freq_hz / 1e6, FM.squelch_open ? "open" : "mute",
                    (int)(FM.iq_level * 100.0f), (int)(FM.audio_level / 0.65f * 100.0f),
                    (unsigned long)audio_drops_get(), (unsigned long)audio_underruns_get());
            }
        }

        if (now - last_yield > 25000) { last_yield = now; vTaskDelay(1); }
    }

    if (stream_started) rtlsdr_stream_stop();
    free(iq);
    s_running = false;
    vTaskDelete(NULL);
}

static void fm_defaults_once(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    FM.mode           = FM_MODE_POCSAG;
    FM.freq_hz        = FM_DEFAULT_FREQ;
    FM.gain_tenths    = FM_DEFAULT_GAIN;
    FM.squelch_tenths = 15;
    FM.pocsag_baud    = 1200;
    FM.pocsag_lock_baud = 0;
    FM.pocsag_auto    = true;
    FM.scan_start_hz  = 150000000UL;
    FM.scan_stop_hz   = 162000000UL;
    FM.scan_step_hz   = 100000UL;
}

static void fm_on_enter(void)
{
    if (s_active) return;
    fm_defaults_once();

    fm_dsp_init(&s_dsp);
    poc_ensure();
    poc_reset_all();

    {
        const app_t *cur = app_current();
        for (int m = 0; m < FM_MODE_COUNT; m++)
            s_mode_freq[m] = settings_get_freq_mode(cur, m, fm_mode_default_freq(m));
    }

    s_dev = rtlsdr_dev_get();
    if (s_dev) {
        const app_t *cur = app_current();

        if (FM.mode != FM_MODE_SCAN)
            FM.freq_hz = settings_get_freq_mode(cur, FM.mode, fm_mode_default_freq(FM.mode));
        rtlsdr_set_sample_rate(s_dev, FM_RTL_RATE);
        rtlsdr_set_tuner_bandwidth(s_dev, 0);
        fm_apply_gain(FM.gain_tenths);
        if (FM.mode == FM_MODE_SCAN) scan_begin(false);
        else fm_apply_freq(FM.freq_hz);
        ESP_LOGI(TAG, "radio: %.4f MHz %dkSPS gain=%d",
                 FM.freq_hz / 1e6, FM_RTL_RATE / 1000, FM.gain_tenths);
    } else {
        ESP_LOGW(TAG, "no RTL device available");
    }

    s_active = true;

    xTaskCreatePinnedToCore(fm_rx_task, "fm_rx", FMRX_STACK_BYTES, NULL, 6, NULL, 1);
}

static void fm_on_exit(void)
{
    s_active = false;
    for (int i = 0; i < 300 && s_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_running) ESP_LOGW(TAG, "drain timeout");

}

static void fm_on_sample(uint8_t *iq, int len) { (void)iq; (void)len; }

static const app_t FM_APP = {
    .name         = "FM",
    .default_freq = FM_DEFAULT_FREQ,
    .default_rate = FM_RTL_RATE,
    .default_gain = FM_DEFAULT_GAIN,
    .banner       = "FM MONITOR",
    .signal_label = "SIGNAL",
    .diag_label   = "PAGES",
    .on_enter     = fm_on_enter,
    .on_exit      = fm_on_exit,
    .on_sample    = fm_on_sample,
};

int fm_app_register(void)
{
    fm_defaults_once();
    return app_register(&FM_APP);
}

void lakeshark_fm_set_mode(int mode)
{
    if (mode < 0 || mode >= FM_MODE_COUNT) return;
    s_mode_req = mode;
}
int  lakeshark_fm_get_mode(void) { return (int)FM.mode; }

void lakeshark_fm_tune(int delta_hz)
{
    long f = (long)FM.freq_hz + delta_hz;
    if (f < 1000000L) f = 1000000L;
    FM.freq_hz = (uint32_t)f;
    s_freq_req = FM.freq_hz;
    if (FM.mode != FM_MODE_SCAN) s_mode_freq[FM.mode] = FM.freq_hz;
    const app_t *a = app_current();
    if (a) settings_set_freq_mode(a, FM.mode, FM.freq_hz);
}
void lakeshark_fm_set_freq(uint32_t hz)
{
    if (hz < 1000000UL) return;
    FM.freq_hz = hz;
    s_freq_req = hz;
    if (FM.mode != FM_MODE_SCAN) s_mode_freq[FM.mode] = hz;
    const app_t *a = app_current();
    if (a) settings_set_freq_mode(a, FM.mode, hz);
}
uint32_t lakeshark_fm_get_freq(void) { return FM.freq_hz; }

void lakeshark_fm_gain_step(void)
{
    static const int ladder[] = { 0, 90, 200, 300, 408, 496 };
    int n = sizeof(ladder) / sizeof(ladder[0]);
    int cur = FM.gain_tenths, idx = 0;
    for (int i = 0; i < n; i++) if (ladder[i] == cur) { idx = i; break; }
    idx = (idx + 1) % n;
    s_gain_req = ladder[idx];
    const app_t *a = app_current();
    if (a) settings_set_gain(a, ladder[idx]);
}
void lakeshark_fm_agc(void)
{
    s_gain_req = 0;
    const app_t *a = app_current();
    if (a) settings_set_gain(a, 0);
}

void lakeshark_fm_set_gain(int tenths)
{
    if (tenths < 0)   tenths = 0;
    if (tenths > 496) tenths = 496;
    s_gain_req = tenths;
    const app_t *a = app_current();
    if (a) settings_set_gain(a, tenths);
}

void lakeshark_fm_set_gain_live(int tenths)
{
    if (tenths < 0)   tenths = 0;
    if (tenths > 496) tenths = 496;
    s_gain_req = tenths;
}
void lakeshark_fm_gain_delta(int dt) { lakeshark_fm_set_gain(FM.gain_tenths + dt); }
int  lakeshark_fm_gain_tenths(void) { return FM.gain_tenths; }

void lakeshark_fm_set_baud(int baud) { s_baud_req = baud; }
int  lakeshark_fm_get_baud(void)     { return FM.pocsag_baud; }

void lakeshark_fm_squelch_delta(int d)
{
    int v = FM.squelch_tenths + d;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_squelch_req = v;
}
void lakeshark_fm_set_squelch(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_squelch_req = v;
}
int  lakeshark_fm_squelch_get(void) { return FM.squelch_tenths; }

void lakeshark_fm_scan_restart(void) { s_scan_restart = true; }
void lakeshark_fm_tune_to_peak(void)
{
    if (FM.scan_peak_hz) {
        lakeshark_fm_set_freq(FM.scan_peak_hz);
        s_mode_req = FM_MODE_LISTEN;
    }
}
uint32_t lakeshark_fm_scan_peak_hz(void) { return FM.scan_peak_hz; }
