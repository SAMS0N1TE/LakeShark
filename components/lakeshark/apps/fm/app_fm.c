#include "fm_app.h"
#include "fm_state.h"
#include "fm_mode_handoff.h"
#include "fm_dsp.h"
#include "fm_lifecycle.h"
#include "fm_rx_worker.h"
#include "pocsag.h"
#include "acars.h"
#include "acars_app.h"
#include "acars_resample.h"
#include "flex.h"
#include "app_registry.h"
#include "settings.h"
/*LS-730*/
#include "scan_engine.h"
/*LS-748*/
#include "spectrum.h"
#include "iq_app_control.h"
#include "radio_endpoint.h"
#include "audio_out.h"
#include "ls_cpu_busy.h"
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

#define FM_IQ_BLOCK_BYTES   16384
#define FM_READ_TIMEOUT_MS  20

#define FM_SCAN_SETTLE      2
/*LS-748*/ /* FM_SCAN_DWELL retired - see FM_SPEC_AVG. */

fm_state_t FM;

static uint32_t s_mode_freq[FM_MODE_COUNT];

static fm_mode_handoff_t s_mode_handoff = FM_MODE_HANDOFF_INITIALIZER;
/*LS-828  Has anyone actually asked for a gain, or are we still on the
   built-in default? POCSAG entry only overrides the latter. */
static volatile bool     s_gain_chosen  = false;
static volatile int      s_baud_req      = -1;
static volatile int      s_squelch_req   = -1;
static volatile bool     s_tune_peak_req = false;
static volatile bool     s_scan_restart  = false;

static ls_radio_session_t *s_session;
static ls_iq_control_t s_radio_control;
/*LS-405*/
static EXT_RAM_BSS_ATTR fm_dsp_t s_dsp;

static const int     POC_BAUDS[3] = { 512, 1200, 2400 };
static pocsag_ctx_t *s_poc[3] = { NULL, NULL, NULL };
static flex_ctx_t   *s_flex[4] = { NULL, NULL, NULL, NULL };

/* ACARS wiring.  The FM app owns the RTL session and demodulates NFM at
   32 kHz already, so an ACARS mode here is a mode switch and a resampler
   rather than a second radio worker fighting for the tuner - which is
   exactly the pattern the task file argued for.  s_acars is created on
   first entry into FM_MODE_ACARS and torn down by checked FM stop. */
static acars_ctx_t *s_acars = NULL;
static acars_rs_t   s_acars_rs;

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
/*LS-748*/ /* s_scan_accum/s_scan_naccum retired - the sweep averages in
   the frequency domain now, inside spectrum.c. */

static int fm_requested_gain(int tenths)
{
    /*LS-828  POCSAG defaults to AGC, enforced HERE rather than at mode entry.

       Doing it at mode entry did not hold: the saved per-app gain is restored
       through this same function immediately afterwards and simply overwrote
       it - the log said "using AGC" while the tuner went back to 20 dB and
       iq stayed at 6-11%.

       Coercing at the one place every gain change funnels through means the
       restore path cannot undo it, and an explicit choice still wins because
       that sets s_gain_chosen. */
    if ((FM.mode == FM_MODE_POCSAG || FM.mode == FM_MODE_ACARS) &&
        !s_gain_chosen && tenths != 0) {
        /* ACARS shares the "burst data with nothing to hear" property that
           made a wrong fixed gain lethal for POCSAG - see LS-828. */
        tenths = 0;
    }
    return tenths;
}

static void fm_apply_gain(int tenths)
{
    tenths = fm_requested_gain(tenths);
    FM.gain_tenths = tenths;
    ls_iq_control_request_gain(&s_radio_control, tenths);
}

/*LS-984  FLEX had a complete, host-tested decoder but the firmware
   never constructed or fed it. Keep ownership beside POCSAG: mode
   entry resets only that protocol, and the RX loop dispatches the same 32 kHz
   discriminator blocks that the host fixtures exercise. */
static void flex_ensure(void)
{
    for (int i = 0; i < 4; i++)
        if (!s_flex[i]) s_flex[i] = flex_create(&FM, (ls_flex_mode_t)i);
}

static void flex_reset_all(void)
{
    for (int i = 0; i < 4; i++) if (s_flex[i]) flex_reset(s_flex[i]);
    FM.flex_sync = false;
    FM.flex_mode = LS_FLEX_MODE_1600_2;
    FM.flex_frames = FM.flex_pages = FM.flex_cw_errs = 0;
    FM.flex_near_min = 32;
}

static void flex_dispatch(const float *demod, int nd)
{
    uint32_t frames = 0, pages = 0, cw_errs = 0, best_frames = 0;
    int near_min = 32;
    bool sync = false;
    uint8_t best_mode = FM.flex_mode;

    /* Run one timing loop per FLEX rate/level. The mode word rejects the
       wrong loops, while the panel can still say exactly what is being hunted. */
    for (int i = 0; i < 4; i++) {
        if (!s_flex[i]) continue;
        flex_process(s_flex[i], demod, nd);
        uint32_t nf = flex_n_frames(s_flex[i]);
        frames += nf;
        pages += flex_n_pages(s_flex[i]);
        cw_errs += flex_n_cwerr(s_flex[i]);
        int nm = flex_near_min(s_flex[i]);
        if (nm < near_min) near_min = nm;
        if (flex_synced(s_flex[i])) sync = true;
        if (nf >= best_frames) { best_frames = nf; best_mode = (uint8_t)i; }
    }
    FM.flex_sync = sync;
    FM.flex_mode = best_mode;
    FM.flex_frames = frames;
    FM.flex_pages = pages;
    FM.flex_cw_errs = cw_errs;
    FM.flex_near_min = near_min;
}

static bool acars_prepare(void)
{
    /* Lazy-create the decoder the first time ACARS mode is entered;
       s_acars is torn down only after FM stops, so flipping in and out keeps
       the sync counters the panel is watching.  Reset both stages so no old
       framing or interpolation state crosses the retune. */
    if (!s_acars) s_acars = acars_create(acars_app_state_mut());
    if (s_acars)  acars_reset(s_acars);
    acars_rs_init(&s_acars_rs);
    return s_acars != NULL;
}

/*LS-730*/
/* LS-708 ON THE FM SIDE. Fast hops used to reset a hardware FIFO while RX
   transfers were in flight. At ~120 ms per hop the stream collapsed from
   524288 to 32768 B/s and FM.iq_level pinned at zero. The app now supplies
   only the fast-hop hint; the endpoint adapter owns stale-data discard and
   any transport recovery. */
/*LS-745*/
/* THE THIRD FAST-HOPPING CALLER, and the one nobody had covered. LS-730 put
   the guard here against the stored-channel scanner; LS-736(a) widened it to
   scan_engine_sweeping() so the auto-squelch calibration sweep was covered
   too. Both of those live in scan_engine. The FM app's OWN band sweep
   (FM_MODE_SCAN) does not: scan_step() hops a bin with the scan engine
   STOPPED, so scan_engine_sweeping() is false and every single bin took the
   full USB FIFO reset. That is LS-708 for the third time, and it is the
   "intermittent stuck RTL on FM" reported from the field - enter SWEEP and
   the stream dies with a flood of "Enqueue URB error: ESP_ERR_INVALID_STATE",
   throughput falls 524288 -> 32768 B/s and FM.iq_level pins at 0, which
   presents as a receiver that has simply stopped hearing anything.
   The predicate has to mean "is ANYTHING driving the tuner fast right now",
   which is why the FM app's own sweep is part of it rather than a second
   guard bolted on beside it. A manual tune still gets the full reset. */
static inline bool fm_fast_hopping(void)
{
    return scan_engine_sweeping() || FM.mode == FM_MODE_SCAN;
}

static void fm_tune_hw(uint32_t hz)
{
    if (!s_session || hz < 1000000UL) return;
    uint64_t actual_hz = 0;
    ls_radio_err_t error = ls_radio_iq_retune(s_session, hz,
                                               fm_fast_hopping(), &actual_hz);
    ls_iq_control_note_tune_result(&s_radio_control, hz, error, actual_hz);
    if (error != LS_RADIO_OK)
        ESP_LOGW(TAG, "retune failed: %s", ls_radio_err_name(error));
}

static uint32_t fm_mode_default_freq(fm_mode_t m)
{
    switch (m) {
        case FM_MODE_WFM:    return FM_FREQ_WFM;
        case FM_MODE_POCSAG: return FM_FREQ_POCSAG;
        case FM_MODE_FLEX:   return FM_FREQ_FLEX;
        case FM_MODE_LISTEN: return FM_FREQ_LISTEN;
        case FM_MODE_ACARS:  return FM_FREQ_ACARS;
        default:             return FM_FREQ_POCSAG;
    }
}

static void fm_apply_freq(uint32_t hz)
{
    if (hz < 1000000UL) return;
    FM.freq_hz = hz;
    ls_iq_control_request_tune(&s_radio_control, hz, fm_fast_hopping());
}

/*LS-748*/
/* THE SWEEP IS AN FFT NOW, NOT A POWER METER ON A LADDER.
   It used to tune, dwell, reduce the whole passband to one fm_iq_rms() number,
   store that in one display bin and step by scan_step_hz. At 256 kSPS the
   receiver hands us +/-128 kHz on every buffer, so that discarded almost
   everything it was given: 81 retunes to cover 151-152 MHz, each producing a
   single 12.5 kHz-resolution sample.
   Now each tune is transformed and paints a few hundred display bins at once,
   so the same 1 MHz needs ~5 tunes at ~500 Hz resolution. Fewer retunes AND
   finer - and fewer retunes matters for more than speed, because retuning is
   the operation LS-708/730/745 keep having to defuse. */

/* Middle slice of the passband we are willing to believe. The RTL's IF filter
   rolls off toward the edges of 256 kSPS, and the tuner's own LO leakage puts
   a DC spike dead centre. Using the full width would draw the filter's shape
   and a permanent fake carrier down the middle of every tune - which on a
   signal finder is exactly the quiet wrong answer this file keeps collecting.
   200 kHz of 256 kHz keeps the flat part; the DC bins are skipped below. */
#define FM_SPEC_USABLE_HZ   200000u
/* Buffers folded into each tune position before the spectrum is read. */
#define FM_SPEC_AVG         4
/* Bins either side of centre discarded as the DC/LO spike. */
#define FM_SPEC_DC_GUARD    3
/* Display normalises against this; below it everything reads as floor. */
#define FM_SPEC_FLOOR_DB    (-90.0f)

static float s_spec_db[SPEC_FFT_N];

static int scan_tune_count(void)
{
    uint32_t span = (FM.scan_stop_hz > FM.scan_start_hz)
                  ? (FM.scan_stop_hz - FM.scan_start_hz) : 0;
    if (span == 0) return 1;
    int n = (int)((span + FM_SPEC_USABLE_HZ - 1) / FM_SPEC_USABLE_HZ);
    return n < 1 ? 1 : n;
}

static uint32_t scan_tune_center(int i)
{
    return FM.scan_start_hz + FM_SPEC_USABLE_HZ / 2
         + (uint32_t)i * FM_SPEC_USABLE_HZ;
}

static void scan_begin(bool reset_pos)
{
    uint32_t span = (FM.scan_stop_hz > FM.scan_start_hz)
                  ? (FM.scan_stop_hz - FM.scan_start_hz) : 0;

    FM.scan_tunes = scan_tune_count();

    /* One display bin per column, but never claim resolution the FFT cannot
       deliver: the transform resolves FM_RTL_RATE/SPEC_FFT_N (500 Hz), so a
       narrow span gets proportionally fewer bins rather than interpolated
       ones. Drawing bins finer than the data is how a display invents peaks. */
    int bins = FM_SCAN_BINS_MAX;
    if (span > 0) {
        uint32_t res = (uint32_t)FM_RTL_RATE / SPEC_FFT_N;
        if (res < 1) res = 1;
        int max_useful = (int)(span / res);
        if (max_useful < 1) max_useful = 1;
        if (bins > max_useful) bins = max_useful;
    }
    if (bins < 1) bins = 1;
    FM.scan_bins = bins;

    if (reset_pos) {
        FM.scan_idx = 0;
        FM.scan_peak_db = 0.0f; FM.scan_peak_hz = 0;
        for (int i = 0; i < FM_SCAN_BINS_MAX; i++) FM.scan_db[i] = 0.0f;
    }
    if (FM.scan_idx >= FM.scan_tunes) FM.scan_idx = 0;
    s_scan_phase = 0; s_scan_count = 0;
    spectrum_reset();
    fm_tune_hw(scan_tune_center(FM.scan_idx));
}

/* Paint one tune's spectrum into the display bins it actually covers. */
static void scan_paint(uint32_t center)
{
    uint32_t span = (FM.scan_stop_hz > FM.scan_start_hz)
                  ? (FM.scan_stop_hz - FM.scan_start_hz) : 0;
    if (span == 0 || FM.scan_bins < 1) return;

    const int N    = SPEC_FFT_N;
    const int32_t half = (int32_t)(FM_SPEC_USABLE_HZ / 2);

    /* Clear only the bins this tune is about to own. Without this the array
       is a running maximum that never decays, so a burst leaves a permanent
       spike and the display slowly fills in with history. */
    int64_t lo_f = (int64_t)center - half;
    int64_t hi_f = (int64_t)center + half;
    for (int b = 0; b < FM.scan_bins; b++) {
        int64_t f = (int64_t)FM.scan_start_hz + ((int64_t)b * span) / FM.scan_bins;
        if (f >= lo_f && f <= hi_f) FM.scan_db[b] = 0.0f;
    }

    for (int j = 0; j < N; j++) {
        if (j >= N / 2 - FM_SPEC_DC_GUARD && j <= N / 2 + FM_SPEC_DC_GUARD)
            continue;
        int32_t off = (int32_t)(((int64_t)(j - N / 2) * FM_RTL_RATE) / N);
        if (off < -half || off > half) continue;

        int64_t f = (int64_t)center + off;
        if (f < (int64_t)FM.scan_start_hz || f > (int64_t)FM.scan_stop_hz) continue;

        int b = (int)(((f - (int64_t)FM.scan_start_hz) * FM.scan_bins) / span);
        if (b < 0 || b >= FM.scan_bins) continue;

        float lin = (s_spec_db[j] - FM_SPEC_FLOOR_DB) / (0.0f - FM_SPEC_FLOOR_DB);
        if (lin < 0.0f) lin = 0.0f;
        if (lin > 1.0f) lin = 1.0f;

        if (lin > FM.scan_db[b]) FM.scan_db[b] = lin;
        if (lin > FM.scan_peak_db) {
            FM.scan_peak_db = lin;
            FM.scan_peak_hz = (uint32_t)f;
        }
    }
}

static void scan_step(const uint8_t *iq, int len)
{
    if (s_scan_phase == 0) {
        if (++s_scan_count >= FM_SCAN_SETTLE) {
            s_scan_phase = 1; s_scan_count = 0;
            /* Discard whatever arrived while the tuner was still moving. */
            spectrum_reset();
        }
        return;
    }

    spectrum_accum(iq, len);
    if (++s_scan_count < FM_SPEC_AVG) return;

    if (spectrum_read_db(s_spec_db, SPEC_FFT_N))
        scan_paint(scan_tune_center(FM.scan_idx));

    FM.scan_idx++;
    s_scan_phase = 0; s_scan_count = 0;
    spectrum_reset();

    if (FM.scan_idx >= FM.scan_tunes) {
        FM.scan_sweeps++;
        FM.scan_idx = 0;
        /* Peak is per-sweep. A peak latched forever is a record of something
           that may have stopped transmitting minutes ago, and ->PEAK would
           tune to silence. */
        FM.scan_peak_db = 0.0f; FM.scan_peak_hz = 0;
    }
    fm_tune_hw(scan_tune_center(FM.scan_idx));
}

static void fm_receiver_lost(ls_radio_err_t error)
{
    FM.iq_bytes_sec = 0;
    FM.iq_level = 0.0f;
    FM.audio_level = 0.0f;
    FM.squelch_open = false;
    FM.pocsag_sync = false;
    FM.flex_sync = false;
    ls_iq_control_receiver_lost(&s_radio_control, error);
}

static bool fm_radio_open(void)
{
    const ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .min_hz = 24000000UL,
        .max_hz = 1766000000UL,
        .sample_rate_hz = FM_RTL_RATE,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
    };
    ls_radio_err_t error = ls_radio_acquire("fm", &requirements, &s_session);
    if (error != LS_RADIO_OK) {
        fm_receiver_lost(error);
        return false;
    }

    uint32_t center_hz = FM.mode == FM_MODE_SCAN
                           ? scan_tune_center(FM.scan_idx) : FM.freq_hz;
    const ls_radio_iq_config_t requested = {
        .center_hz = center_hz,
        .sample_rate_hz = FM_RTL_RATE,
        .bandwidth_hz = 0,
        .gain_mode = FM.gain_tenths == 0 ? LS_RADIO_GAIN_AUTO
                                         : LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = FM.gain_tenths,
    };
    ls_radio_iq_config_t actual;
    error = ls_iq_control_configure(&s_radio_control, s_session, &requested,
                                    &actual);
    if (error == LS_RADIO_OK) error = ls_radio_iq_start(s_session);
    if (error != LS_RADIO_OK) {
        fm_receiver_lost(error);
        ESP_LOGE(TAG, "radio open failed: %s", ls_radio_err_name(error));
        ls_radio_release(s_session);
        s_session = NULL;
        return false;
    }
    ls_iq_control_set_streaming(&s_radio_control, true, LS_RADIO_OK);
    ESP_LOGI(TAG, "radio: %.4f MHz %lukSPS gain=%d",
             actual.center_hz / 1e6,
             (unsigned long)(actual.sample_rate_hz / 1000),
             actual.gain_tenths_db);
    return true;
}

static void fm_fail_receiver_start(void);

static void fm_rx_run_once(void)
{
    /*LS-825  PSRAM, not internal. This is a 16 KB IQ assembly buffer and
       plain malloc() takes it from internal RAM, where it competes with the
       USB stream's DMA transfers on a GUI build that has almost nothing left.

       Measured on the Touch-LCD-4.3, entering FM from P25:
           P25   int_30319 dma_7595
           FM    int_3323  dma_1975     <- 27 KB gone, 2 KB of DMA left
       and the radio then reports rhs=absent bps=0, because the 16 x 16 KB
       stream transfers cannot be allocated. POCSAG shows nothing - not
       because the decoder is wrong, but because no samples ever arrive.

       The buffer never needs to be internal or DMA-capable: it is only the
       destination of a memcpy out of the PSRAM IQ ring in
       esp_libusb_stream_read(). The other big FM buffers (demod, pcm, s_dsp)
       are already EXT_RAM_BSS_ATTR for the same reason.

       The 16 KB fm_rx stack stays in statically reserved DRAM deliberately -
       a task stack in PSRAM is what crashed the BLE telemetry task (LS-821). */
    uint8_t *iq = heap_caps_malloc(FM_IQ_BLOCK_BYTES, MALLOC_CAP_SPIRAM);
    if (!iq) iq = heap_caps_malloc(FM_IQ_BLOCK_BYTES,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    /*LS-405*/
    static EXT_RAM_BSS_ATTR float   demod[1100];
    static EXT_RAM_BSS_ATTR int16_t pcm[600];
    /* ACARS_SAMP_RATE / FM_DEMOD_RATE = 3/5, so 1100 * 3/5 = 660; a little
       slack in case the resampler carries a fractional bit across blocks. */
    static EXT_RAM_BSS_ATTR float   acars_pcm[700];

    if (!iq) {
        ESP_LOGE(TAG, "OOM iq buf");
        fm_fail_receiver_start();
        return;
    }
    ESP_LOGI(TAG, "rx task up: mode=%d freq=%lu gain=%d",
             FM.mode, (unsigned long)FM.freq_hz, FM.gain_tenths);

    int read_errors = 0;
    uint32_t iq_bucket = 0;
    int64_t stats_ts = esp_timer_get_time();
    int64_t last_yield = stats_ts;
    bool allocation_failed = false;
    while (fm_lifecycle_active()) {
        if (!s_session) {
            if (!fm_radio_open()) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        fm_mode_t requested_mode;
        if (fm_mode_handoff_take(&s_mode_handoff, &requested_mode)) {
            int m = (int)requested_mode;

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
            if (FM.mode == FM_MODE_POCSAG) {
                poc_ensure(); poc_reset_all();
                /*LS-828  POCSAG defaults to AGC.

                   A fixed FM_DEFAULT_GAIN of 20 dB is the difference between
                   working and dead here, and it fails silently: measured on a
                   live 152.6000 pager site, 20 dB gave iq=3-12%, near_min=4
                   (statistically indistinguishable from noise) and zero
                   frames. `gain auto` on the same signal, same antenna, same
                   second: iq=40-69%, near_min=0, 3 pages, err=0.

                   Voice modes tolerate a wrong gain - you hear something and
                   reach for the knob. A POCSAG burst is a second long and
                   either slices or does not, so there is nothing to hear and
                   nothing to tell you the gain is why. Default to AGC and let
                   an explicit choice win. */
                if (!s_gain_chosen && FM.gain_tenths != 0) {
                    ESP_LOGI(TAG, "POCSAG: no gain chosen - using AGC");
                    fm_apply_gain(0);   /* fm_apply_gain() also enforces it */
                }
            } else if (FM.mode == FM_MODE_FLEX) {
                flex_ensure(); flex_reset_all();
            }
            if (FM.mode == FM_MODE_ACARS) {
                if (!acars_prepare()) {
                    ESP_LOGE(TAG, "ACARS decoder could not be allocated");
                    fm_receiver_lost(LS_RADIO_ERR_NO_MEMORY);
                    allocation_failed = true;
                    break;
                }
                if (!s_gain_chosen && FM.gain_tenths != 0) {
                    ESP_LOGI(TAG, "ACARS: no gain chosen - using AGC");
                    fm_apply_gain(0);
                }
            }
            if (FM.mode == FM_MODE_LISTEN || FM.mode == FM_MODE_WFM) audio_out_ensure_unmuted();
        }
        ls_iq_control_request_t radio_request;
        if (ls_iq_control_take(&s_radio_control, &radio_request)) {
            if (radio_request.flags & LS_IQ_CONTROL_TUNE) {
                ls_radio_err_t error = ls_iq_control_apply_tune(
                    &s_radio_control, s_session, &radio_request);
                if (error != LS_RADIO_OK)
                    ESP_LOGW(TAG, "retune failed: %s",
                             ls_radio_err_name(error));
            }
            if (radio_request.flags & LS_IQ_CONTROL_GAIN) {
                ls_radio_err_t error = ls_iq_control_apply_gain(
                    &s_radio_control, s_session, &radio_request);
                if (error != LS_RADIO_OK)
                    ESP_LOGW(TAG, "gain request failed: %s",
                             ls_radio_err_name(error));
            }
        }
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

        if (!fm_lifecycle_active()) break;
        int got = 0; bool full = true;
        while (got < FM_IQ_BLOCK_BYTES) {
            if (!fm_lifecycle_active() || !s_session) { full = false; break; }
            size_t part = 0;
            ls_radio_err_t error = ls_radio_iq_read(
                s_session, &iq[got], FM_IQ_BLOCK_BYTES - got,
                FM_READ_TIMEOUT_MS, &part);
            if (error == LS_RADIO_OK) { got += (int)part; continue; }
            if (error == LS_RADIO_ERR_TIMEOUT) continue;
            if (++read_errors > 50 || error == LS_RADIO_ERR_DISCONNECTED) {
                full = false;
                read_errors = 0;
                if (error == LS_RADIO_ERR_DISCONNECTED) {
                    /*LS-1001  The two-second rate window only advanced after
                       a full block, so detach left the previous DEV/rate and
                       decoder activity visible forever. Clear every live
                       derivative before releasing the dead session. */
                    fm_receiver_lost(LS_RADIO_ERR_DISCONNECTED);
                    iq_bucket = 0;
                    stats_ts = esp_timer_get_time();
                    ls_radio_release(s_session);
                    s_session = NULL;
                }
                break;
            }
        }
        if (!full || got < FM_IQ_BLOCK_BYTES) { continue; }
        read_errors = 0;
        iq_bucket += FM_IQ_BLOCK_BYTES;

        if (FM.mode == FM_MODE_SCAN) {
            scan_step(iq, FM_IQ_BLOCK_BYTES);
            FM.iq_level = fm_iq_rms(iq, FM_IQ_BLOCK_BYTES);
        } else if (FM.mode == FM_MODE_WFM) {

            int na = fm_demod_wide(&s_dsp, iq, FM_IQ_BLOCK_BYTES, pcm, 600);
            FM.iq_level = s_dsp.iq_peak;
            FM.squelch_open = true;
            if (na > 0) {
                audio_write_mono(pcm, na);
                float ss = 0.0f;
                for (int i = 0; i < na; i++) ss += (float)pcm[i] * (float)pcm[i];
                FM.audio_level = sqrtf(ss / na) / 8000.0f;
            }
        } else {
            int nd = fm_demod_iq(&s_dsp, iq, FM_IQ_BLOCK_BYTES, demod, 1100);
            FM.iq_level = s_dsp.iq_peak;

            float ss = 0.0f;
            for (int i = 0; i < nd; i++) ss += demod[i] * demod[i];
            FM.audio_level = (nd > 0) ? sqrtf(ss / nd) : 0.0f;

            if (FM.mode == FM_MODE_POCSAG) {
                poc_dispatch(demod, nd);
            } else if (FM.mode == FM_MODE_ACARS) {
                /* ACARS on VHF is 2400-baud MSK.  The FM_DEMOD_RATE 32 kHz
                   stream is resampled to ACARS_SAMP_RATE 19.2 kHz (exact
                   3:5 ratio) and handed straight into the MSK slicer;
                   framed messages arrive in acars_app_state()'s ring, so
                   the same log the panel's TEST/CLEAR buttons touch. */
                if (fm_lifecycle_acars_enter()) {
                    acars_ctx_t *acars = s_acars;
                    int na = acars_rs_5to3(&s_acars_rs, demod, nd,
                                           acars_pcm, 700);
                    if (acars && na > 0) acars_process(acars, acars_pcm, na);
                    fm_lifecycle_acars_leave();
                }
            } else if (FM.mode == FM_MODE_FLEX) {
                flex_dispatch(demod, nd);
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
            } else if (FM.mode == FM_MODE_FLEX) {
                ESP_LOGI(TAG, "FLEX %.4f %s mode=%u frames=%lu pages=%lu err=%lu iq=%d%% act=%d%% near_min=%d",
                    FM.freq_hz / 1e6, FM.flex_sync ? "SYNC" : "hunt",
                    (unsigned)FM.flex_mode, (unsigned long)FM.flex_frames,
                    (unsigned long)FM.flex_pages, (unsigned long)FM.flex_cw_errs,
                    (int)(FM.iq_level * 100.0f), (int)(FM.audio_level / 0.65f * 100.0f),
                    FM.flex_near_min);
            } else if (FM.mode == FM_MODE_WFM) {
                ESP_LOGI(TAG, "WFM %.4f iq=%d%% af=%d%% audio(drop=%lu under=%lu)",
                    FM.freq_hz / 1e6, (int)(FM.iq_level * 100.0f),
                    (int)(FM.audio_level * 100.0f),
                    (unsigned long)audio_drops_get(), (unsigned long)audio_underruns_get());
                int core0_pct, core1_pct;
                if (ls_cpu_busy(&core0_pct, &core1_pct)) {
                    ESP_LOGW(TAG, "CPU busy: core0=%d%% core1=%d%% (tasks=%u)",
                             core0_pct, core1_pct,
                             (unsigned)uxTaskGetNumberOfTasks());
                }
            } else if (FM.mode == FM_MODE_ACARS) {
                const acars_state_t *as = acars_app_state();
                ESP_LOGI(TAG, "ACARS %.4f %s msgs=%lu bad=%lu sync=%lu iq=%d%%",
                    FM.freq_hz / 1e6,
                    (s_acars && acars_synced(s_acars)) ? "SYNC" : "hunt",
                    (unsigned long)as->n_delivered,
                    (unsigned long)as->n_bad_crc,
                    (unsigned long)as->n_synced,
                    (int)(FM.iq_level * 100.0f));
            } else {
                ESP_LOGI(TAG, "LISTEN %.4f sq=%s iq=%d%% act=%d%% audio(drop=%lu under=%lu)",
                    FM.freq_hz / 1e6, FM.squelch_open ? "open" : "mute",
                    (int)(FM.iq_level * 100.0f), (int)(FM.audio_level / 0.65f * 100.0f),
                    (unsigned long)audio_drops_get(), (unsigned long)audio_underruns_get());
            }
        }

        if (now - last_yield > 25000) { last_yield = now; vTaskDelay(1); }
    }

    if (s_session) {
        (void)ls_radio_iq_stop(s_session);
    }
    heap_caps_free(iq);
    if (allocation_failed) fm_fail_receiver_start();
    else fm_lifecycle_task_finished();
}

static void fm_rx_task(void *arg)
{
    (void)arg;
    if (!fm_lifecycle_active()) fm_lifecycle_task_finished();
    else fm_rx_run_once();
}

static void fm_cancel_iq(void *user)
{
    (void)user;
    if (s_session) (void)ls_radio_iq_stop(s_session);
}

static void fm_release_session(void *user)
{
    (void)user;
    if (s_session) {
        ls_radio_release(s_session);
        s_session = NULL;
    }
}

static void fm_destroy_acars(void *user)
{
    (void)user;
    if (s_acars) {
        acars_destroy(s_acars);
        s_acars = NULL;
    }
}

static void fm_reset_audio(void *user)
{
    (void)user;
    audio_out_reset();
}

static void fm_report_not_receiving(void *user)
{
    (void)user;
    fm_receiver_lost(LS_RADIO_ERR_NO_MEMORY);
}

static void fm_stop_delay(void *user, uint32_t delay_ms)
{
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static const fm_lifecycle_hooks_t FM_LIFECYCLE_HOOKS = {
    .report_not_receiving = fm_report_not_receiving,
    .cancel_iq = fm_cancel_iq,
    .release_session = fm_release_session,
    .destroy_acars = fm_destroy_acars,
    .reset_audio = fm_reset_audio,
    .delay = fm_stop_delay,
    .user = NULL,
};

static void fm_fail_receiver_start(void)
{
    /* Publish failure before cleanup so every observer sees "not receiving"
       even when the selected submode remains ACARS.  task_failed releases the
       lifecycle reservation; stop then drains/destroys any decoder prepared
       before the worker could start. */
    (void)fm_lifecycle_start_failed(&FM_LIFECYCLE_HOOKS);
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
    if (fm_lifecycle_active()) return;

    for (int i = 0; i < 200 && fm_lifecycle_task_live(); i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (fm_lifecycle_task_live()) {
        ESP_LOGE(TAG, "previous fm_rx_task still alive - refusing to start a second one");
        return;
    }
    /* Complete a previously timed-out stop before re-entry. */
    (void)fm_lifecycle_stop(&FM_LIFECYCLE_HOOKS, 0, 0);

    fm_defaults_once();

    /*LS-724  fl FM ACARS posts its submode before the GUI timer launches
       AppFM.  Starting from retained/default FM.mode here made the receiver
       configure and announce POCSAG before the worker eventually noticed the
       request.  Atomically consume the single handoff at backend entry so an
       explicit request chooses the initial mode and its saved/default
       frequency. With no request, retaining FM.mode preserves manual entry. */
    FM.mode = fm_mode_handoff_resolve_entry(&s_mode_handoff, FM.mode,
                                             FM_MODE_POCSAG);

    fm_dsp_init(&s_dsp);
    /*LS-748*/
    spectrum_init();
    poc_ensure();
    poc_reset_all();
    bool decoder_ready = true;
    if (FM.mode == FM_MODE_FLEX) {
        flex_ensure();
        flex_reset_all();
    } else if (FM.mode == FM_MODE_ACARS) {
        decoder_ready = acars_prepare();
    }

    {
        const app_t *cur = app_current();
        for (int m = 0; m < FM_MODE_COUNT; m++)
            s_mode_freq[m] = settings_get_freq_mode(cur, m, fm_mode_default_freq(m));
    }

    const app_t *cur = app_current();
    if (FM.mode != FM_MODE_SCAN)
        FM.freq_hz = settings_get_freq_mode(cur, FM.mode,
                                             fm_mode_default_freq(FM.mode));
    ls_iq_control_reset(&s_radio_control);
    /* Preserve LS-828 before the session's initial configuration: POCSAG
       defaults to automatic gain until the user explicitly chooses one. */
    FM.gain_tenths = fm_requested_gain(FM.gain_tenths);
    ls_iq_control_set_initial(&s_radio_control, FM.freq_hz,
                              FM.gain_tenths, 0);
    fm_receiver_lost(LS_RADIO_ERR_UNAVAILABLE);
    if (FM.mode == FM_MODE_SCAN) scan_begin(false);
    if (FM.mode == FM_MODE_LISTEN || FM.mode == FM_MODE_WFM)
        audio_out_ensure_unmuted();

    if (!decoder_ready) {
        fm_receiver_lost(LS_RADIO_ERR_NO_MEMORY);
        ESP_LOGE(TAG, "ACARS decoder could not be allocated - not receiving");
        return;
    }

    if (!fm_lifecycle_start()) {
        ESP_LOGE(TAG, "FM lifecycle not clean - refusing task start");
        return;
    }

    if (!fm_rx_worker_start(fm_rx_task)) {
        fm_fail_receiver_start();
        ESP_LOGE(TAG, "FM RX task could not be created");
    }
}

static bool fm_on_stop(void)
{
    bool stopped = fm_lifecycle_stop(&FM_LIFECYCLE_HOOKS, 300, 10);
    if (!stopped) {
        ESP_LOGW(TAG, "drain timeout - retaining FM ownership");
        return false;
    }
    if (!fm_rx_worker_release(300, 10)) {
        /* LS-726: task_live becomes false just before the worker callback
         * returns.  Do not hand its shared static stack to P25 until the
         * kernel confirms the wrapper is suspended and safe to delete. */
        ESP_LOGW(TAG, "worker quiesce timeout - retaining FM ownership");
        return false;
    }
    return true;
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
    .on_stop      = fm_on_stop,
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
    /*LS-984  The stored-channel scanner owns the tuner and deliberately
       forces LISTEN on every pass. Any requested non-listen mode must release
       that owner here so GUI, console and control-head selection all reach the
       same decoder. LISTEN remains composable with the scanner. */
    if (mode != FM_MODE_LISTEN && scan_engine_active()) scan_engine_stop();
    fm_mode_handoff_request(&s_mode_handoff, (fm_mode_t)mode);
}
int  lakeshark_fm_get_mode(void) { return (int)FM.mode; }

void lakeshark_fm_tune(int delta_hz)
{
    long f = (long)FM.freq_hz + delta_hz;
    if (f < 1000000L) f = 1000000L;
    FM.freq_hz = (uint32_t)f;
    ls_iq_control_request_tune(&s_radio_control, FM.freq_hz, false);
    if (FM.mode != FM_MODE_SCAN) s_mode_freq[FM.mode] = FM.freq_hz;
    const app_t *a = app_current();
    if (a) settings_set_freq_mode(a, FM.mode, FM.freq_hz);
}
void lakeshark_fm_set_freq(uint32_t hz)
{
    if (hz < 1000000UL) return;
    FM.freq_hz = hz;
    ls_iq_control_request_tune(&s_radio_control, hz, false);
    if (FM.mode != FM_MODE_SCAN) s_mode_freq[FM.mode] = hz;
    const app_t *a = app_current();
    if (a) settings_set_freq_mode(a, FM.mode, hz);
}
uint32_t lakeshark_fm_get_freq(void) { return FM.freq_hz; }

/*LS-717*/
/* Retune without persisting. The scanner hops every ~120 ms, and going
   through lakeshark_fm_set_freq() for that queued an NVS write per hop:
   flash wear, a settings queue that starts dropping the user's real
   changes, and a saved FM frequency quietly replaced by whichever channel
   the sweep sampled last. The sweep is transient - it must not be sticky. */
void lakeshark_fm_tune_transient(uint32_t hz)
{
    if (hz < 1000000UL) return;
    FM.freq_hz = hz;
    ls_iq_control_request_tune(&s_radio_control, hz, true);
}

void lakeshark_fm_gain_step(void)
{
    static const int ladder[] = { 0, 90, 200, 300, 408, 496 };
    int n = sizeof(ladder) / sizeof(ladder[0]);
    int cur = FM.gain_tenths, idx = 0;
    for (int i = 0; i < n; i++) if (ladder[i] == cur) { idx = i; break; }
    idx = (idx + 1) % n;
    s_gain_chosen = true;   /*LS-828*/
    fm_apply_gain(ladder[idx]);
    const app_t *a = app_current();
    if (a) settings_set_gain(a, ladder[idx]);
}
void lakeshark_fm_agc(void)
{
    s_gain_chosen = true;   /*LS-828  asking for AGC is still a choice */
    fm_apply_gain(0);
    const app_t *a = app_current();
    if (a) settings_set_gain(a, 0);
}

void lakeshark_fm_set_gain(int tenths)
{
    if (tenths < 0)   tenths = 0;
    if (tenths > 496) tenths = 496;
    s_gain_chosen = true;   /*LS-828*/
    fm_apply_gain(tenths);
    const app_t *a = app_current();
    if (a) settings_set_gain(a, tenths);
}

void lakeshark_fm_set_gain_live(int tenths)
{
    if (tenths < 0)   tenths = 0;
    if (tenths > 496) tenths = 496;
    s_gain_chosen = true;   /*LS-828*/
    fm_apply_gain(tenths);
}
void lakeshark_fm_gain_delta(int dt) { lakeshark_fm_set_gain(FM.gain_tenths + dt); }
int  lakeshark_fm_gain_tenths(void) { return FM.gain_tenths; }

void fm_get_receiver_status(ls_iq_control_status_t *out)
{
    ls_iq_control_status(&s_radio_control, out);
}

void lakeshark_fm_set_baud(int baud) { s_baud_req = baud; }
int  lakeshark_fm_get_baud(void)     { return FM.pocsag_baud; }

void lakeshark_fm_squelch_delta(int d)
{
    int base = (s_squelch_req >= 0) ? s_squelch_req : FM.squelch_tenths;
    int v = base + d;
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
int  lakeshark_fm_squelch_get(void)
{
    int pending = s_squelch_req;
    return (pending >= 0) ? pending : FM.squelch_tenths;
}

void lakeshark_fm_scan_restart(void) { s_scan_restart = true; }
void lakeshark_fm_tune_to_peak(void)
{
    if (FM.scan_peak_hz) {
        lakeshark_fm_set_freq(FM.scan_peak_hz);
        fm_mode_handoff_request(&s_mode_handoff, FM_MODE_LISTEN);
    }
}
uint32_t lakeshark_fm_scan_peak_hz(void) { return FM.scan_peak_hz; }
