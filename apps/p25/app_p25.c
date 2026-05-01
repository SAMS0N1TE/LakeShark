#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

#include "app_registry.h"
#include "event_bus.h"
#include "settings.h"
#include "audio_out.h"
#include "tone.h"
#include "rtl-sdr.h"

#include "p25_state.h"
#include "scanner.h"
#include "diag.h"
#include "diaglog.h"
#include "sdkconfig.h"

p25_state_t       P25 = {0};
scan_state_t      SCAN = {0};
uint32_t          s_tune_freq_hz = (uint32_t)CONFIG_P25_TUNE_FREQ_HZ;
rtlsdr_dev_t     *rtldev = NULL;
dsp_state_t       s_dsp;
dsd_opts          s_dsd_opts;
dsd_state         s_dsd_state;
dsd_sample_ring_t s_ring;

int autoscan_bch_ok_flag = 0;
int dsd_bch_fail_counter = 0;
volatile int rtl_gain_request = -1;

static const char *TAG = "p25";

/* Lifecycle flags. The handshake is:
 *   on_enter:
 *     - clear all three flags, allocate buffers, spawn rx task
 *     - rx task spawns dsd task; both set their *_running flags
 *   on_exit:
 *     - set s_app_active = false (signal both tasks to drain)
 *     - spin until both *_running flags clear
 *     - the rx task itself frees the IQ buffer and joins the dsd task
 *
 * We deliberately do NOT NULL rtldev here. The device handle is owned
 * by the radio framework (rtlsdr_dev.c); apps fetch via rtlsdr_dev_get()
 * at on_enter and otherwise leave it alone. NULLing it from on_exit
 * was racing the dsd task's tail-end logging on the previous version. */
static volatile bool s_app_active  = false;
static volatile bool s_rx_running  = false;
static volatile bool s_dsd_running = false;
static uint8_t      *s_iq_buf      = NULL;

void sys_log(uint8_t color, const char *fmt, ...)
{
    event_t e = { .kind = EVT_LOG, .ts_us = esp_timer_get_time() };
    strncpy(e.app, "P25", EVT_APP_NAME_MAX);
    /* Map P25's color codes to ESP-IDF log levels. The previous mapping
     * sent color=1 (the most common, used by sys_log(1, ...) for routine
     * INFO lines) to level=0 = ESP_LOG_NONE, which made ESP_LOG_LEVEL()
     * silently discard them. That's why P25 log activity wasn't reaching
     * the TUI log page or the diag UART even though sys_log was being
     * called constantly.
     *
     *   color 0 / 1 -> INFO   (routine status, sync, voice, tune)
     *   color 2     -> INFO   (kept INFO; was historically green)
     *   color 3     -> WARN   (something unusual)
     *   color 4     -> ERROR  (alloc failure, USB error) */
    int level;
    switch (color) {
        case 4:  level = 1; break;   /* ESP_LOG_ERROR */
        case 3:  level = 2; break;   /* ESP_LOG_WARN  */
        default: level = 3; break;   /* ESP_LOG_INFO  */
    }
    e.u.log.level = level;
    strncpy(e.u.log.tag, "p25", sizeof(e.u.log.tag) - 1);
    va_list ap; va_start(ap, fmt);
    vsnprintf(e.u.log.text, sizeof(e.u.log.text), fmt, ap);
    va_end(ap);
    event_bus_publish(&e);
}

void dsd_yield(void)
{
    static int64_t last_wdt_us = 0;
    static int64_t last_log_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_wdt_us > 500000LL) {
        esp_task_wdt_reset();
        last_wdt_us = now;
    }
    if (now - last_log_us > 1000000LL) {
        diaglog_write('W', "p25", "dsd_yield: ring empty, write=%d read=%d",
                      s_ring.write_idx, s_ring.read_idx);
        last_log_us = now;
    }
    vTaskDelay(1);
}

void audio_beep_request(int kind)
{
    if (!P25.sync_beep_enabled) return;
    if (kind == 1) {
        audio_tone(800.0f,  0.08f, 0.06f);
        audio_tone(1200.0f, 0.08f, 0.06f);
        audio_tone(1600.0f, 0.08f, 0.06f);
    }
}

extern void audio_write_p25_voice(const int16_t *src8k, int n);

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
    s_dsd_opts.msize = 15;
    s_dsd_opts.use_cosine_filter = 1;
    s_dsd_opts.unmute_encrypted_p25 = 1;

    initState(&s_dsd_state);
    s_dsd_state.p25kid = 0;
    /* Verbose alloc diagnostics so we can see exactly which buffer
     * failed if any. Previously we just printed "DSD alloc FAILED"
     * with no breakdown - but it's also possible none failed and the
     * page is showing a stale dsd_buffers_ok=false because the task
     * never ran. Print all five pointers before the check to confirm. */
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

    static int16_t pcm_buf[2000];
    s_dsd_state.pcm_out_buf = pcm_buf;
    s_dsd_state.pcm_out_size = 2000;
    s_dsd_state.pcm_out_write = 0;
    sys_log(1, "DSD decoder running heap=%lu", (unsigned long)esp_get_free_heap_size());

    s_dsd_running = true;
    while (s_app_active) {
        esp_task_wdt_reset();
        diag_emit_periodic();
        int sync = getFrameSync(&s_dsd_opts, &s_dsd_state);
        if (sync >= 0) {
            P25.dsd_sync_count++;
            P25.dsd_has_sync = true;
            /* Latch the visible sync indicator for 500ms past the last
             * sync hit. Without this, the MAIN page sees has_sync=true
             * for only one render frame between successful syncs (TUI
             * refreshes at 6Hz; getFrameSync may run 1-3 times/sec when
             * working). The user sees "NO SYNC" almost continuously
             * even when we're decoding ~15 frames over 30 seconds. The
             * DEMOD page shows the sync count which is monotonic, so
             * that page is fine. */
            P25.sync_active_until_us = esp_timer_get_time() + 500000LL;
            extern int dsp_has_signal_lock;
            dsp_has_signal_lock = 1;
            /* Freeze the C4FM DC tracker for 2 seconds past now. As long
             * as syncs keep hitting at >0.5 Hz, the tracker stays frozen
             * for the duration of the call. Between calls (or during
             * sustained sync misses) the deadline expires and the tracker
             * resumes converging on the noise floor. */
            extern int64_t dsp_dc_freeze_until_us;
            dsp_dc_freeze_until_us = esp_timer_get_time() + 2000000LL;
            P25.dsd_nac = s_dsd_state.nac;
            P25.dsd_tg = s_dsd_state.lasttg;
            P25.dsd_src = s_dsd_state.lastsrc;
            snprintf(P25.dsd_ftype, sizeof(P25.dsd_ftype), "%s", s_dsd_state.ftype);
            if (s_dsd_state.rf_mod == 0) strcpy(P25.dsd_modulation, "C4FM");
            else if (s_dsd_state.rf_mod == 1) strcpy(P25.dsd_modulation, "QPSK");
            else strcpy(P25.dsd_modulation, "GFSK");

            esp_task_wdt_reset();
            s_dsd_state.pcm_out_write = 0;
            processFrame(&s_dsd_opts, &s_dsd_state);
            /* processFrame is the hot path: it calls into mbelib IMBE+
             * synthesis which can take 50-200 ms per LDU frame on the
             * P4 depending on PSRAM contention. Reset WDT immediately
             * after to give the rest of this loop body fresh budget. */
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
                    /* I2S writes can block for tens of milliseconds when
                     * the DMA queue is full. Reset WDT after to keep our
                     * budget fresh for the next sync attempt. */
                    esp_task_wdt_reset();
                    /* Rate-limit: voice frames fire ~50/sec when active.
                     * Log every 50th one (= roughly 1/sec) so the log
                     * page stays readable. The TUI MAIN page already
                     * shows voice count + active indicator at refresh
                     * rate, so per-frame logging is overkill. */
                    static unsigned vox_count = 0;
                    if ((++vox_count % 50) == 0) {
                        sys_log(0, "VOX active +50 frames errs=%u",
                                s_dsd_state.debug_audio_errors);
                    }
                }
            }
            /* Rate-limit: SYNC fires per-frame (~30/sec when locked).
             * Only log when something CHANGES (NAC, TG, or fsubtype) so
             * the log shows interesting events instead of a wall of
             * identical lines. */
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
            /* Honor the latch: dsd_has_sync stays true for up to 500ms
             * past the last hit so the MAIN page indicator doesn't
             * flicker between every getFrameSync iteration. */
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

    esp_task_wdt_add(NULL);
    diaglog_write('I', "p25", "rx_task starting dev=%p", dev);

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

    xTaskCreatePinnedToCore(dsd_decoder_task, "dsd_decode", 16384, NULL, 5, NULL, 1);
    sys_log(1, "IQ read loop starting");

    int n_read = 0, read_errors = 0;
    uint32_t iq_bucket = 0, audio_bucket = 0;
    int64_t stats_ts = esp_timer_get_time();

    s_rx_running = true;

    while (s_app_active) {
        esp_task_wdt_reset();
        if (SCAN.request_scan) { SCAN.request_scan = false; scanner_run_sweep(); }
        if (SCAN.request_tune && SCAN.peak_freq > 0) {
            SCAN.request_tune = false;
            s_tune_freq_hz = SCAN.peak_freq;
            if (dev) {
                rtlsdr_set_center_freq(dev, s_tune_freq_hz);
                rtlsdr_reset_buffer(dev);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            sys_log(1, "Tuned: %.4f MHz", s_tune_freq_hz / 1e6);
        }

        if (rtl_gain_request >= 0) {
            int gain = rtl_gain_request;
            rtl_gain_request = -1;
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

        /* Recheck the active flag right before issuing a USB transfer.
         * This is the cheap-but-effective fix for the "scary errors
         * flashing on switch" - if the framework has already flipped
         * s_app_active to false, we exit cleanly here without spending
         * 16 ms in a doomed bulk read. */
        if (!s_app_active) break;

        bool full = true;
        for (int i = 0; i < P25_USB_BUF_LENGTH; i += P25_USB_PACKET_SIZE) {
            if (!s_app_active || !dev) { full = false; break; }
            int r = rtlsdr_read_sync(dev, &s_iq_buf[i], P25_USB_PACKET_SIZE, &n_read);
            if (r < 0) {
                full = false; read_errors++;
                diaglog_write('E', "p25", "rtlsdr_read_sync r=%d n=%d errs=%d",
                              r, n_read, read_errors);
                if (s_app_active && read_errors <= 3) sys_log(4, "USB err r=%d", r);
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }
            if ((uint32_t)n_read < P25_USB_PACKET_SIZE) {
                full = false;
                diaglog_write('W', "p25", "rtlsdr_read_sync short n=%d/%d",
                              n_read, P25_USB_PACKET_SIZE);
                break;
            }
        }

        if (full) {
            read_errors = 0;
            iq_bucket += P25_USB_BUF_LENGTH;

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
            diaglog_write('I', "p25",
                          "rx 1s: iq=%lu B audio=%lu samp ring=%d/%d errs=%d",
                          (unsigned long)iq_bucket,
                          (unsigned long)audio_bucket,
                          P25.ring_fill, P25.ring_size,
                          read_errors);
            iq_bucket = 0; audio_bucket = 0; stats_ts = now;
        }

        /* Liveness heartbeat: when not synced, log a one-line summary
         * every 10 seconds so the LOG page actually reflects what P25
         * is doing. Without this, an idle P25 produces zero log entries
         * and the page looks dead even though the task is healthy. */
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
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* Wait for dsd task to drain. */
    for (int i = 0; i < 300 && s_dsd_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    free(s_iq_buf);
    s_iq_buf = NULL;
    s_rx_running = false;
    esp_task_wdt_delete(NULL);
    diaglog_write('I', "p25", "rx_task exiting");
    vTaskDelete(NULL);
}

static void p25_on_enter(void)
{
    if (s_app_active) return;

    diag_init();

    /* Clear visible state so the TUI doesn't briefly show stale numbers
     * from the previous session. */
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

    P25.demod_gain         = -9000.0f;
    /* Gain: prefer the user's saved value from settings/NVS over the
     * compiled-in default. Without this, every switch back to P25
     * stomps user adjustments back to RTL_DEFAULT_GAIN, making the
     * Settings-page gain control and the [g] keybind effectively
     * write-only.
     *
     * EXCEPTION: when CONFIG_P25_FORCE_KCONFIG_GAIN is set, the Kconfig
     * value wins on every boot regardless of NVS. Useful while iterating
     * on Kconfig defaults — saves you from having to erase-flash to make
     * a config change take effect. */
    {
#ifdef CONFIG_P25_FORCE_KCONFIG_GAIN
        P25.rtl_gain_tenths = CONFIG_P25_TUNER_GAIN_TENTHS;
#else
        const app_t *cur = app_current();
        int saved = cur ? settings_get_gain(cur) : RTL_DEFAULT_GAIN;
        P25.rtl_gain_tenths = (saved >= 0) ? saved : RTL_DEFAULT_GAIN;
#endif
    }
    P25.sync_beep_enabled  = false;

    scanner_init();

    extern rtlsdr_dev_t *rtlsdr_dev_get(void);
    rtldev = rtlsdr_dev_get();

    if (rtldev) {
        const app_t *cur = app_current();
        uint32_t saved_freq = cur ? settings_get_freq(cur) : 0;
        if (saved_freq) s_tune_freq_hz = saved_freq;

        /* Apply tune offset if configured. The dongle is tuned to
         * (target + offset); whether the DSP mixes back is currently
         * left to the existing pipeline. With offset=0 (default), no
         * change from previous behavior. */
        uint32_t actual_tune = s_tune_freq_hz +
            (uint32_t)CONFIG_P25_TUNE_OFFSET_HZ;

        /* CRITICAL ORDERING:
         *
         * Some librtlsdr functions reset the tuner state when called,
         * even if you're "setting them to the default". Specifically:
         *
         *   - rtlsdr_set_direct_sampling(0) re-runs the tuner init(),
         *     resets the IF freq, reinits the R820T from scratch.
         *     That nukes any gain/freq we set BEFORE calling it.
         *
         *   - rtlsdr_set_bias_tee() and rtlsdr_set_agc_mode() are
         *     less destructive but we order them up front anyway.
         *
         * Solution: call the "mode" settings FIRST, then center_freq /
         * sample_rate / gain LAST so they can't be stomped.
         *
         * Belt-and-suspenders: skip set_direct_sampling entirely when
         * the configured value is 0 (the radio's natural default).
         * The original (working) firmware never called it; calling it
         * with 0 was breaking gain settings. */

        /* Direct sampling — only call if non-default. */
        if (CONFIG_P25_DIRECT_SAMPLING != 0) {
            rtlsdr_set_direct_sampling(rtldev, CONFIG_P25_DIRECT_SAMPLING);
        }

        /* RTL2832U digital AGC (chip-side, separate from R820T LNA). */
#ifdef CONFIG_P25_RTL_AGC_ENABLE
        rtlsdr_set_agc_mode(rtldev, 1);
        const int rtl_agc_on = 1;
#else
        const int rtl_agc_on = 0;
        /* don't call when off — let the chip default stand */
#endif

        /* Bias tee — only call if turning it ON. */
#ifdef CONFIG_P25_BIAS_TEE_ENABLE
        rtlsdr_set_bias_tee(rtldev, 1);
        const int bias_tee_on = 1;
#else
        const int bias_tee_on = 0;
#endif

        /* PPM correction — only call if non-zero. */
        if (CONFIG_P25_FREQ_CORRECTION_PPM != 0) {
            rtlsdr_set_freq_correction(rtldev, CONFIG_P25_FREQ_CORRECTION_PPM);
        }

        /* Now the settings that actually matter for tuning. ORDER MATTERS:
         * the original (working) firmware did center_freq → sample_rate →
         * gain → bandwidth(0). Calling bandwidth before center_freq causes
         * the R820T to attempt a PLL lock at a stale init frequency
         * (~2.1 MHz) which fails ("PLL not locked!") and wedges the tuner.
         * Always do center_freq first. */
        rtlsdr_set_center_freq(rtldev, actual_tune);
        rtlsdr_set_sample_rate(rtldev, (uint32_t)CONFIG_P25_SAMPLE_RATE);

        /* Tuner gain. P25.rtl_gain_tenths can be overridden by user via
         * the 'g' menu key — we initialize it from Kconfig at boot but
         * runtime adjustments still take effect. */
        if (P25.rtl_gain_tenths == 0) {
            P25.rtl_gain_tenths = CONFIG_P25_TUNER_GAIN_TENTHS;
        }
        rtlsdr_set_tuner_gain_mode(rtldev, P25.rtl_gain_tenths == 0 ? 0 : 1);
        if (P25.rtl_gain_tenths > 0) {
            rtlsdr_set_tuner_gain(rtldev, P25.rtl_gain_tenths);
        }

        /* IF bandwidth — call AFTER tuning. 0 = driver auto. */
        if (CONFIG_P25_TUNER_BANDWIDTH_HZ != 0) {
            rtlsdr_set_tuner_bandwidth(rtldev,
                (uint32_t)CONFIG_P25_TUNER_BANDWIDTH_HZ);
        } else {
            /* Original firmware called this with 0 explicitly, so we do too. */
            rtlsdr_set_tuner_bandwidth(rtldev, 0);
        }

        rtlsdr_reset_buffer(rtldev);

        sys_log(1, "Radio: %.4f MHz%s%+d Hz, %d kSPS, gain=%d.%d dB%s, "
                   "ppm=%d, bw=%d Hz, agc=%d, bias=%d, ds=%d",
                s_tune_freq_hz / 1e6,
                CONFIG_P25_TUNE_OFFSET_HZ ? " offset=" : " ",
                CONFIG_P25_TUNE_OFFSET_HZ,
                CONFIG_P25_SAMPLE_RATE / 1000,
                P25.rtl_gain_tenths / 10, P25.rtl_gain_tenths % 10,
                P25.rtl_gain_tenths == 0 ? " (auto)" : "",
                CONFIG_P25_FREQ_CORRECTION_PPM,
                CONFIG_P25_TUNER_BANDWIDTH_HZ,
                rtl_agc_on,
                bias_tee_on,
                CONFIG_P25_DIRECT_SAMPLING);
    } else {
        sys_log(4, "no RTL device available");
    }

    s_app_active = true;
    xTaskCreatePinnedToCore(p25_rx_task, "p25_rx", 16384, NULL, 5, NULL, 1);
}

static void p25_on_exit(void)
{
    /* Clear the active flag first - both tasks check it before issuing
     * new USB transfers, so they exit cleanly between transfers rather
     * than mid-flight. */
    s_app_active = false;

    /* Wait for both tasks to drain, but cap the wait. The rx_task is
     * responsible for joining the dsd_task before clearing s_rx_running,
     * so once s_rx_running is false everything is genuinely cleaned up. */
    for (int i = 0; i < 300 && (s_rx_running || s_dsd_running); i++)
        vTaskDelay(pdMS_TO_TICKS(10));

    if (s_rx_running || s_dsd_running) {
        ESP_LOGW(TAG, "drain timeout: rx=%d dsd=%d",
                 s_rx_running, s_dsd_running);
    }

    /* Note: NOT setting rtldev = NULL. The device handle is owned by
     * rtlsdr_dev.c; apps fetch it on entry via rtlsdr_dev_get() and
     * leave it alone otherwise. NULLing it raced the dsd task's tail
     * logging on the previous version. */
}

static void p25_on_sample(uint8_t *iq, int len)
{
    /* P25 owns its own bulk-read loop in p25_rx_task because it needs
     * different packet sizing (16 KB vs ADS-B's 32 KB) and its DSP
     * cadence is tied to the read schedule. The framework's stream
     * dispatcher (radio/stream.c) is ADS-B-only; this callback is a
     * required-interface stub that never gets called for P25. */
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
