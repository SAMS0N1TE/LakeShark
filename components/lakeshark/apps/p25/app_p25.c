#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_attr.h"

#include "app_registry.h"
#include "event_bus.h"
#include "settings.h"
#include "audio_out.h"
#include "tone.h"
#include "rtl-sdr.h"

#include "p25_state.h"
#include "scanner.h"
#include "scan_engine.h"
#include "diag.h"

p25_state_t       P25 = {0};
scan_state_t      SCAN = {0};
uint32_t          s_tune_freq_hz = 154785000UL;
rtlsdr_dev_t     *rtldev = NULL;
dsp_state_t       s_dsp;
dsd_opts          s_dsd_opts;
dsd_state         s_dsd_state;
dsd_sample_ring_t s_ring;

int autoscan_bch_ok_flag = 0;
int dsd_bch_fail_counter = 0;
volatile int rtl_gain_request = -1;

volatile uint32_t s_p25_freq_req = 0;
volatile float    p25_rx_power   = 0.0f;
volatile bool     p25_agc_on     = false;

#define P25_STRONG_SIGNAL_GAIN 280

static const char *TAG = "p25";

static volatile bool s_app_active  = false;
static volatile bool s_rx_running  = false;
static volatile bool s_dsd_running = false;
static uint8_t      *s_iq_buf      = NULL;

void sys_log(uint8_t color, const char *fmt, ...)
{
    event_t e = { .kind = EVT_LOG, .ts_us = esp_timer_get_time() };
    strncpy(e.app, "P25", EVT_APP_NAME_MAX);

    int level;
    switch (color) {
        case 4:  level = 1; break;
        case 3:  level = 2; break;
        default: level = 3; break;
    }
    e.u.log.level = level;
    strncpy(e.u.log.tag, "p25", sizeof(e.u.log.tag) - 1);
    va_list ap; va_start(ap, fmt);
    vsnprintf(e.u.log.text, sizeof(e.u.log.text), fmt, ap);
    va_end(ap);
    event_bus_publish(&e);
}

void dsd_yield(void) { vTaskDelay(1); }

void audio_beep_request(int kind)
{
    if (!P25.sync_beep_enabled) return;
    if (kind != 1) return;

    static int64_t last_sync_us = 0;
    int64_t now = esp_timer_get_time();
    bool new_keyup = (now - last_sync_us) > 1500000LL;
    last_sync_us = now;
    if (new_keyup) snd_p25_chirp();
}

extern void audio_write_p25_voice(const int16_t *src8k, int n);

#define DSD_STACK_WORDS (16384u / sizeof(StackType_t))

#define P25RX_STACK_WORDS (16384u / sizeof(StackType_t))
static EXT_RAM_BSS_ATTR StackType_t s_p25rx_stack[P25RX_STACK_WORDS];
static StaticTask_t s_p25rx_tcb;

static void dsd_decoder_task(void *arg)
{
    esp_task_wdt_add(NULL);
    initOpts(&s_dsd_opts);
    s_dsd_opts.ring = &s_ring;
    s_dsd_opts.verbose = 0;
    s_dsd_opts.errorbars = 1;
    s_dsd_opts.frame_p25p1 = 1;
    s_dsd_opts.mod_c4fm = 1;
    s_dsd_opts.mod_qpsk = 0;
    s_dsd_opts.mod_gfsk = 0;
    s_dsd_opts.mod_threshold = 26;
    s_dsd_opts.ssize = 36;

    s_dsd_opts.msize = 256;
    s_dsd_opts.use_cosine_filter = 1;
    s_dsd_opts.unmute_encrypted_p25 = 1;

    initState(&s_dsd_state);
    s_dsd_state.p25kid = 0;

    sys_log(0, "DSD alloc: dibit=%p audio=%p audio_f=%p cur_mp=%p prev_mp=%p enh=%p",
            (void*)s_dsd_state.dibit_buf,
            (void*)s_dsd_state.audio_out_buf,
            (void*)s_dsd_state.audio_out_float_buf,
            (void*)s_dsd_state.cur_mp,
            (void*)s_dsd_state.prev_mp,
            (void*)s_dsd_state.prev_mp_enhanced);
    if (!s_dsd_state.dibit_buf || !s_dsd_state.audio_out_buf || !s_dsd_state.audio_out_float_buf ||
        !s_dsd_state.cur_mp || !s_dsd_state.prev_mp || !s_dsd_state.prev_mp_enhanced) {
        sys_log(4, "DSD alloc FAILED - one or more pointers NULL above");
        P25.dsd_buffers_ok = false;
        esp_task_wdt_delete(NULL); vTaskDelete(NULL); return;
    }
    P25.dsd_buffers_ok = true;
    sys_log(0, "DSD buffers_ok=true heap=%lu",
            (unsigned long)esp_get_free_heap_size());

    ESP_LOGW("P25DBG", "buffers_ok=1 cur_mp=%c prev_mp=%c enh=%c audio=%c free_int=%u",
             esp_ptr_internal(s_dsd_state.cur_mp) ? 'I' : 'P',
             esp_ptr_internal(s_dsd_state.prev_mp) ? 'I' : 'P',
             esp_ptr_internal(s_dsd_state.prev_mp_enhanced) ? 'I' : 'P',
             esp_ptr_internal(s_dsd_state.audio_out_float_buf) ? 'I' : 'P',
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    static int16_t pcm_buf[2000];
    s_dsd_state.pcm_out_buf = pcm_buf;
    s_dsd_state.pcm_out_size = 2000;
    s_dsd_state.pcm_out_write = 0;
    sys_log(1, "DSD decoder running heap=%lu", (unsigned long)esp_get_free_heap_size());

    s_dsd_running = true;
    int64_t s_tel_us = 0;
    while (s_app_active) {
        esp_task_wdt_reset();
        diag_emit_periodic();

        int64_t tel_now = esp_timer_get_time();
        if (tel_now - s_tel_us > 1000000LL) {
            s_tel_us = tel_now;
            ESP_LOGW("P25TEL",
                "iq=%.3f gain=%d ring=%d/%d sync=%d nac=%03X mod=%s min=%d max=%d ctr=%d "
                "lmid=%d umid=%d bchOK=%d bchFAIL=%d dec=%.0fms vox=%d drops=%u under=%u ringB=%u",
                (double)P25.iq_level, P25.rtl_gain_tenths, P25.ring_fill, P25.ring_size,
                P25.dsd_sync_count, P25.dsd_nac,
                P25.dsd_modulation[0] ? P25.dsd_modulation : "----",
                s_dsd_state.min, s_dsd_state.max, s_dsd_state.center,
                s_dsd_state.lmid, s_dsd_state.umid,
                P25.dsd_bch_ok_count, P25.dsd_bch_fail_count,
                (double)P25.dsd_decode_ms, P25.dsd_voice_count,
                (unsigned)P25.audio_drops, (unsigned)audio_underruns_get(),
                (unsigned)audio_out_ring_avail());
        }

        int sync = getFrameSync(&s_dsd_opts, &s_dsd_state);
        if (sync >= 0) {
            P25.dsd_sync_count++;
            P25.dsd_has_sync = true;

            P25.sync_active_until_us = esp_timer_get_time() + 500000LL;
            extern int dsp_has_signal_lock;
            dsp_has_signal_lock = 1;
            P25.dsd_nac = s_dsd_state.nac;
            P25.dsd_tg = s_dsd_state.lasttg;
            P25.dsd_src = s_dsd_state.lastsrc;
            snprintf(P25.dsd_ftype, sizeof(P25.dsd_ftype), "%s", s_dsd_state.ftype);
            if (s_dsd_state.rf_mod == 0) strcpy(P25.dsd_modulation, "C4FM");
            else if (s_dsd_state.rf_mod == 1) strcpy(P25.dsd_modulation, "QPSK");
            else strcpy(P25.dsd_modulation, "GFSK");

            esp_task_wdt_reset();
            s_dsd_state.pcm_out_write = 0;

            int64_t pf_t0 = esp_timer_get_time();
            processFrame(&s_dsd_opts, &s_dsd_state);
            float pf_ms = (float)(esp_timer_get_time() - pf_t0) / 1000.0f;
            P25.dsd_decode_ms = P25.dsd_decode_ms * 0.9f + pf_ms * 0.1f;

            esp_task_wdt_reset();

            P25.dsd_tg = s_dsd_state.lasttg;
            P25.dsd_src = s_dsd_state.lastsrc;
            snprintf(P25.dsd_fsubtype, sizeof(P25.dsd_fsubtype), "%s", s_dsd_state.fsubtype);
            snprintf(P25.dsd_err_str, sizeof(P25.dsd_err_str), "%s", s_dsd_state.err_str);

            P25.dsd_bch_ok_count = autoscan_bch_ok_flag;
            P25.dsd_bch_fail_count = dsd_bch_fail_counter;
            if (s_dsd_state.nac != 0) P25.dsd_last_ok_nac = s_dsd_state.nac;

            if (s_dsd_state.pcm_out_write > 0) {
                P25.dsd_voice_count++;
                P25.voice_active_until_us = esp_timer_get_time() + 500000LL;
                int n = s_dsd_state.pcm_out_write;
                if (n > s_dsd_state.pcm_out_size) n = s_dsd_state.pcm_out_size;
                if (!audio_is_muted()) {
                    audio_write_p25_voice(pcm_buf, n);
                    P25.audio_drops = audio_drops_get();

                    esp_task_wdt_reset();

                    static unsigned vox_count = 0;
                    if ((++vox_count % 50) == 0) {
                        sys_log(0, "VOX active +50 frames errs=%u",
                                s_dsd_state.debug_audio_errors);
                    }
                }
            }

            {
                static int prev_nac = -1, prev_tg = -1;
                static char prev_subtype[16] = {0};
                if (P25.dsd_nac != prev_nac ||
                    P25.dsd_tg  != prev_tg  ||
                    strncmp(prev_subtype, P25.dsd_fsubtype, sizeof(prev_subtype)) != 0) {
                    sys_log(0, "SYNC %s nac=%04X tg=%d %s",
                            P25.dsd_modulation, P25.dsd_nac,
                            P25.dsd_tg, P25.dsd_fsubtype);
                    prev_nac = P25.dsd_nac;
                    prev_tg  = P25.dsd_tg;
                    strncpy(prev_subtype, P25.dsd_fsubtype, sizeof(prev_subtype) - 1);
                    prev_subtype[sizeof(prev_subtype) - 1] = 0;
                }
            }
        } else {

            if (esp_timer_get_time() >= P25.sync_active_until_us) {
                P25.dsd_has_sync = false;
            }
            extern int dsp_has_signal_lock;
            dsp_has_signal_lock = 0;
        }
    }
    esp_task_wdt_delete(NULL);
    s_dsd_running = false;
    vTaskDelete(NULL);
}

static void p25_rx_task(void *arg)
{

    rtlsdr_dev_t *dev = rtldev;

    dsp_init(&s_dsp);
    dsp_set_mode(&s_dsp, DEMOD_C4FM);
    dsp_set_gain(&s_dsp, P25.demod_gain);
    sys_log(1, "DSP mode: C4FM");
    memset(&s_ring, 0, sizeof(s_ring));
    P25.ring_size = DSD_SAMPLE_RING_SIZE;

    s_iq_buf = malloc(P25_USB_BUF_LENGTH);
    if (!s_iq_buf) {
        sys_log(4, "OOM rx buffer");
        s_rx_running = false;
        vTaskDelete(NULL);
        return;
    }
    sys_log(1, "RX buf OK %d bytes heap=%lu",
            P25_USB_BUF_LENGTH, (unsigned long)esp_get_free_heap_size());

    static int16_t audio_buf[8192];

    ESP_LOGW("P25DBG", "pre-decode-task: free_int=%u largest_int=%u free_psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    for (int i = 0; i < 200 && s_dsd_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_dsd_running) { ESP_LOGE("P25DBG", "prev dsd_decode still alive; skipping"); }
    dsd_abort = 0;
    TaskHandle_t dec_h = NULL;
    if (!s_dsd_running) {
        xTaskCreatePinnedToCore(dsd_decoder_task, "dsd_decode",
                                DSD_STACK_WORDS, NULL, 10, &dec_h, 1);
    }
    ESP_LOGW("P25DBG", "dsd_decode create = %s (internal stack)",
             dec_h ? "OK" : "FAILED");
    sys_log(1, "IQ read loop starting");

    int read_errors = 0;
    uint32_t iq_bucket = 0, audio_bucket = 0;
    int64_t stats_ts = esp_timer_get_time();
    int64_t last_yield = stats_ts;

    s_rx_running = true;

    bool stream_started = false;
    if (dev && rtlsdr_stream_start(dev) == 0) stream_started = true;
    else sys_log(4, "stream start failed");

    while (s_app_active) {
        if (SCAN.request_scan) {
            SCAN.request_scan = false;

            if (stream_started) { rtlsdr_stream_stop(); stream_started = false; }
            scanner_run_sweep();
            if (dev) { rtlsdr_reset_buffer(dev);
                       if (rtlsdr_stream_start(dev) == 0) stream_started = true; }
        }
        if (SCAN.request_tune && SCAN.peak_freq > 0) {
            SCAN.request_tune = false;
            s_tune_freq_hz = SCAN.peak_freq;
            if (dev) {
                rtlsdr_set_center_freq(dev, s_tune_freq_hz);
                rtlsdr_reset_buffer(dev);
                rtlsdr_stream_reset();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            sys_log(1, "Tuned: %.4f MHz", s_tune_freq_hz / 1e6);
        }

        if (s_p25_freq_req) {
            uint32_t f = s_p25_freq_req; s_p25_freq_req = 0;
            s_tune_freq_hz = f;
            if (dev) {
                rtlsdr_set_center_freq(dev, f);
                rtlsdr_reset_buffer(dev);
                rtlsdr_stream_reset();
            }
            s_ring.write_idx = s_ring.read_idx;
            P25.dsd_has_sync = false;
            P25.sync_active_until_us = 0;
            sys_log(1, "Tuned: %.4f MHz", f / 1e6);
        }

        if (rtl_gain_request >= 0) {
            int gain = rtl_gain_request;
            rtl_gain_request = -1;
            p25_agc_on = false;
            P25.rtl_gain_tenths = gain;
            if (dev) {
                rtlsdr_set_tuner_gain_mode(dev, gain == 0 ? 0 : 1);
                if (gain > 0) rtlsdr_set_tuner_gain(dev, gain);
            }
            sys_log(1, "RTL gain: %.1f dB%s", gain / 10.0f, gain == 0 ? " (AGC)" : "");
        }

        if (s_dsp.mode == DEMOD_FSK4_TRACKING) {
            static int64_t last_sync_us = 0;
            static int reset_check_count = 0;
            if (P25.dsd_has_sync) last_sync_us = esp_timer_get_time();
            if (++reset_check_count >= 100) {
                reset_check_count = 0;
                int64_t now_us = esp_timer_get_time();
                if ((now_us - last_sync_us) >= 3000000LL)
                    dsp_fsk4_reset_tracker(&s_dsp);
            }
        }

        if (!s_app_active) break;

        bool full = true;
        if (!stream_started) { vTaskDelay(pdMS_TO_TICKS(10)); full = false; }
        int got = 0;
        while (full && got < P25_USB_BUF_LENGTH) {
            if (!s_app_active || !dev) { full = false; break; }
            int r = rtlsdr_stream_read(&s_iq_buf[got], P25_USB_BUF_LENGTH - got);
            if (r > 0) { got += r; continue; }

            if (++read_errors > 50) { full = false; read_errors = 0; break; }
            vTaskDelay(1);
        }

        if (full && got >= P25_USB_BUF_LENGTH) {
            read_errors = 0;
            iq_bucket += P25_USB_BUF_LENGTH;

            if (scan_engine_active()) {
                int peak = 0;
                for (int i = 0; i + 1 < P25_USB_BUF_LENGTH; i += 8) {
                    int di = (int)s_iq_buf[i]     - 128;
                    int dq = (int)s_iq_buf[i + 1] - 128;
                    int a = di < 0 ? -di : di;
                    int b = dq < 0 ? -dq : dq;
                    if (a > peak) peak = a;
                    if (b > peak) peak = b;
                }
                p25_rx_power = (float)peak / 127.5f;
            }

            {
                static int64_t last_iqr_us = 0;
                int64_t tnow_iqr = esp_timer_get_time();
                if (tnow_iqr - last_iqr_us > 1000000LL) {
                    last_iqr_us = tnow_iqr;
                    uint32_t sum_i = 0, sum_q = 0;
                    uint64_t sumsq = 0;
                    uint8_t  umin = 255, umax = 0;
                    int      peak_dev = 0;
                    const int half_n = P25_USB_BUF_LENGTH / 2;
                    for (int i = 0; i < P25_USB_BUF_LENGTH; i += 2) {
                        uint8_t ui = s_iq_buf[i];
                        uint8_t uq = s_iq_buf[i + 1];
                        if (ui < umin) umin = ui;
                        if (ui > umax) umax = ui;
                        if (uq < umin) umin = uq;
                        if (uq > umax) umax = uq;
                        sum_i += ui;
                        sum_q += uq;
                        int di = (int)ui - 128;
                        int dq = (int)uq - 128;
                        int adi = di < 0 ? -di : di;
                        int adq = dq < 0 ? -dq : dq;
                        if (adi > peak_dev) peak_dev = adi;
                        if (adq > peak_dev) peak_dev = adq;
                        sumsq += (uint64_t)(di * di) + (uint64_t)(dq * dq);
                    }
                    float mean_i = (float)sum_i / (float)half_n - 128.0f;
                    float mean_q = (float)sum_q / (float)half_n - 128.0f;
                    float ms = (float)sumsq / (float)(2 * half_n);
                    float rms_norm = sqrtf(ms) / 127.5f;
                    float peak_norm = (float)peak_dev / 127.5f;
                    P25.iq_level = peak_norm;

                    int rms_mmm  = (int)(rms_norm  * 1000.0f + 0.5f);
                    int peak_mmm = (int)(peak_norm * 1000.0f + 0.5f);
                    diag_line("IQR",
                        "umin=%u umax=%u peak=%d.%03d rms=%d.%03d "
                        "mean_i=%+.2f mean_q=%+.2f n=%d",
                        (unsigned)umin, (unsigned)umax,
                        peak_mmm / 1000, peak_mmm % 1000,
                        rms_mmm  / 1000, rms_mmm  % 1000,
                        (double)mean_i, (double)mean_q, half_n);
                }
            }

            int na = dsp_process_iq(&s_dsp, s_iq_buf, P25_USB_BUF_LENGTH, audio_buf, 8192);
            audio_bucket += na;

            for (int i = 0; i < na; i++) {
                int next = (s_ring.write_idx + 1) % DSD_SAMPLE_RING_SIZE;
                if (next != s_ring.read_idx) {
                    s_ring.buf[s_ring.write_idx] = audio_buf[i];
                    s_ring.write_idx = next;
                }
            }
        }

        P25.read_errors = read_errors;
        P25.ring_fill = dsd_ring_available(&s_ring);

        int64_t now = esp_timer_get_time();
        if (now - stats_ts > 1000000LL) {
            P25.iq_bytes_sec = iq_bucket;
            P25.audio_samples_sec = audio_bucket;
            P25.iq_bytes_total += iq_bucket;
            iq_bucket = 0; audio_bucket = 0; stats_ts = now;
        }

        {
            static int64_t last_hb_us = 0;
            if (!P25.dsd_has_sync && (now - last_hb_us) > 10000000LL) {
                last_hb_us = now;
                sys_log(0, "alive: iq=%d%% ring=%d/%d gain=%d.%d sweeps=%d",
                        (int)(P25.iq_level * 100.0f + 0.5f),
                        P25.ring_fill, P25.ring_size,
                        P25.rtl_gain_tenths / 10, P25.rtl_gain_tenths % 10,
                        SCAN.sweep_count);
            }
        }

        if (read_errors > 20) {
            if (s_app_active) sys_log(4, "Read errors, pause 2s");
            vTaskDelay(pdMS_TO_TICKS(2000));
            read_errors = 0;
        }

        if (now - last_yield > 25000) { last_yield = now; vTaskDelay(1); }
    }

    if (stream_started) rtlsdr_stream_stop();

    for (int i = 0; i < 300 && s_dsd_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    free(s_iq_buf);
    s_iq_buf = NULL;
    s_rx_running = false;
    vTaskDelete(NULL);
}

static void p25_on_enter(void)
{
    if (s_app_active) return;

    diag_init();

    P25.dsd_has_sync       = false;
    P25.dsd_nac            = 0;
    P25.dsd_tg             = 0;
    P25.dsd_src            = 0;
    P25.dsd_ftype[0]       = 0;
    P25.dsd_fsubtype[0]    = 0;
    P25.dsd_err_str[0]     = 0;
    P25.dsd_modulation[0]  = 0;
    P25.iq_level           = 0;
    P25.read_errors        = 0;
    P25.ring_fill          = 0;
    P25.iq_bytes_sec       = 0;
    P25.audio_samples_sec  = 0;
    P25.voice_active_until_us = 0;

    P25.demod_gain         = P25.demod_invert ? 9000.0f : -9000.0f;

    {
        const app_t *cur = app_current();
        int saved = cur ? settings_get_gain(cur) : -1;

        P25.rtl_gain_tenths = (saved >= 0) ? saved : P25_STRONG_SIGNAL_GAIN;
    }
    P25.sync_beep_enabled  = false;

    scanner_init();
    s_p25_freq_req = 0;
    audio_out_reset();    /* re-assert 16k codec rate + re-prime ring (fixes first-launch chop) */

    extern rtlsdr_dev_t *rtlsdr_dev_get(void);
    rtldev = rtlsdr_dev_get();

    if (rtldev) {
        const app_t *cur = app_current();
        uint32_t freq = cur ? settings_get_freq(cur) : s_tune_freq_hz;
        if (freq) s_tune_freq_hz = freq;
        rtlsdr_set_center_freq(rtldev, s_tune_freq_hz);
        rtlsdr_set_sample_rate(rtldev, RTL_SAMPLE_RATE);
        rtlsdr_set_tuner_gain_mode(rtldev, P25.rtl_gain_tenths == 0 ? 0 : 1);
        if (P25.rtl_gain_tenths > 0) rtlsdr_set_tuner_gain(rtldev, P25.rtl_gain_tenths);
        rtlsdr_set_tuner_bandwidth(rtldev, 0);
        rtlsdr_reset_buffer(rtldev);
        sys_log(1, "Radio: %.4f MHz %dkSPS", s_tune_freq_hz / 1e6, RTL_SAMPLE_RATE / 1000);
    } else {
        sys_log(4, "no RTL device available");
    }

    s_app_active = true;
    xTaskCreateStaticPinnedToCore(p25_rx_task, "p25_rx", P25RX_STACK_WORDS, NULL, 10,
                                  s_p25rx_stack, &s_p25rx_tcb, 1);
}

static void p25_on_exit(void)
{

    s_app_active = false;

    dsd_abort = 1;

    for (int i = 0; i < 300 && (s_rx_running || s_dsd_running); i++)
        vTaskDelay(pdMS_TO_TICKS(10));

    if (s_rx_running || s_dsd_running) {
        ESP_LOGW(TAG, "drain timeout: rx=%d dsd=%d",
                 s_rx_running, s_dsd_running);
    }

}

static void p25_on_sample(uint8_t *iq, int len)
{

    (void)iq; (void)len;
}

extern void p25_draw_main(int top, int rows, int cols);
extern void p25_draw_signal(int top, int rows, int cols);
extern void p25_on_key(tui_key_t k);

static const app_t P25_APP = {
    .name         = "P25",
    .default_freq = 154785000UL,
    .default_rate = RTL_SAMPLE_RATE,
    .default_gain = RTL_DEFAULT_GAIN,
    .banner       = "TRUNK MONITOR",
    .signal_label = "DEMOD",
    .on_enter     = p25_on_enter,
    .on_exit      = p25_on_exit,
    .on_sample    = p25_on_sample,
#ifdef CONFIG_ENABLE_TUI
    .draw_main    = p25_draw_main,
    .draw_signal  = p25_draw_signal,
    .on_key       = p25_on_key,
#else
    .draw_main    = NULL,
    .draw_signal  = NULL,
    .on_key       = NULL,
#endif
};

int p25_app_register(void)
{
    return app_register(&P25_APP);
}
