#include "call_archive.h"
#include "fm_app.h"
#include "fm_state.h"
#include "fm_mode_handoff.h"
#include "fm_dsp.h"
#include "fm_lr_dsp.h"
#include "lr_wf_session.h"
#include "lr_iq_spectrum.h"
#include "ls_lora.h"
/* Declared, not included (meshcore is not a dependency here); app_p25.c
   carries the weak fallbacks for builds without a mesh. */
bool ls_mesh_radio_hold_bg(bool hold);
bool ls_mesh_radio_wanted(void);
#include "fm_lifecycle.h"
#include "fm_rx_worker.h"
#include "fm_receive.h"
#include "fm_hf.h"
#include "hf_demod.h"
#include "digi_engine.h"
#include "ls_radio_hf.h"
#include "pocsag.h"
#include "ls_rx_head.h"
#include "rx_pocsag.h"
#include "rx_voice.h"
#include "rx_fm_data.h"
#include "ls_rx_front.h"
#include "acars.h"
#include "fm_spectrum.h"
#include "fm_wfm.h"
#include "fm_bcast.h"
#include "fm_air.h"
#include "fm_wx.h"
#include "../sstv/fm_sstv.h"
#include "acars_app.h"
#include "acars_resample.h"
#include "flex.h"
#include "same_store.h"
#include "aprs_store.h"
#include "ais_store.h"
#include "app_registry.h"
#include "settings.h"
#include "ls_nvs_safe.h"
/**/
#include "scan_engine.h"
#include "scan_channels.h"
/**/
#include "spectrum.h"
#include "iq_app_control.h"
#include "radio_endpoint.h"
#include "radio_choice.h"
/**/
#include "radio_open_retry.h"
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
#include "ls_cold.h"

static const char *TAG = "fm";

#define FM_IQ_BLOCK_BYTES   16384

#define FM_SCAN_SETTLE      2
/**/ /* FM_SCAN_DWELL retired - see FM_SPEC_AVG. */

fm_state_t FM;

static uint32_t s_mode_freq[FM_MODE_COUNT];
static bool     s_frequency_locked;
static uint32_t s_frequency_lock_hz;

/* An accepted manual tune during SCAN wins the return to a live mode. */
static uint32_t s_scan_tune_hz;
static fm_mode_handoff_t s_mode_handoff = FM_MODE_HANDOFF_INITIALIZER;
/* Has anyone actually asked for a gain, or are we still on the
   built-in default? POCSAG entry only overrides the latter. */
static volatile bool     s_gain_chosen  = false;
static volatile int      s_baud_req      = -1;
static volatile int      s_squelch_req   = -1;
static volatile bool     s_tune_peak_req = false;
static volatile bool     s_scan_restart  = false;

static ls_radio_session_t *s_session;
static const char *s_session_endpoint;
static bool s_lr_session;
static ls_iq_control_t s_radio_control;
/* One receiver-task writer; console callers copy a bounded snapshot. Never
 * allocate, format, wait or touch storage while holding this mux. In
 * particular this cannot spin on a preempted lower-priority owner. */
static portMUX_TYPE s_observation_mux = portMUX_INITIALIZER_UNLOCKED;
static ls_iq_observation_t s_observation;
/* Lazy, bounded PSRAM allocation retained for the latest operator mission.
 * No allocation or formatting in the DSP/transition path. */
static ls_iq_mission_t *s_mission;
static ls_iq_coverage_t *s_coverage;
static ls_rx_plan_t *s_rx_plan;
static uint32_t s_plan_manual_epoch, s_plan_owner_epoch, s_plan_request_generation;
static uint32_t s_plan_manual_publishers;
static uint64_t s_plan_context_epoch;
static ls_radio_session_t *s_plan_session;
static bool s_plan_request_pending, s_plan_coverage_owned, s_plan_control_inflight;
static bool s_plan_calls_suppressed;
static EXT_RAM_BSS_ATTR ls_radio_endpoint_info_t s_plan_endpoint;
static void fm_rx_plan_intervene(void);
static void fm_rx_plan_publish_begin(void);
static void fm_rx_plan_publish_end(void);
static void fm_rx_plan_poll(void);
static void fm_rx_plan_lost(void);
static bool fm_rx_plan_control_allowed(const ls_iq_control_request_t *request);
static void fm_rx_plan_tune_result(const ls_iq_control_request_t *request,ls_radio_err_t error);
static void fm_rx_plan_end_locked(uint64_t now)
{
    if (s_rx_plan && !s_rx_plan->status.active) {
        __atomic_store_n(&s_plan_calls_suppressed,false,__ATOMIC_RELEASE);
        if (s_plan_coverage_owned) {
            ls_iq_coverage_boundary(s_coverage,LS_IQ_COVERAGE_UNKNOWN,now);
            ls_iq_coverage_calls_suppressed(s_coverage,false);
            (void)ls_iq_coverage_stop(s_coverage,now);
            if(s_coverage->status.overwritten_intervals)s_rx_plan->status.detail_truncated=true;
            s_plan_coverage_owned=false;
        }
    }
}
static uint32_t s_observation_rate;
static uint64_t s_observation_center;
static bool s_observation_config_valid;
static void fm_observation_boundary(ls_iq_observation_state_t reason)
{
    portENTER_CRITICAL(&s_observation_mux);
    ls_iq_observation_close(&s_observation, reason);
    ls_iq_mission_boundary(s_mission, &s_observation, (uint64_t)esp_timer_get_time());
    ls_iq_coverage_state_t state = reason==LS_IQ_OBS_TUNE ? LS_IQ_COVERAGE_SETTLING
        : reason==LS_IQ_OBS_RECEIVER_ERROR || reason==LS_IQ_OBS_STOPPED || reason==LS_IQ_OBS_READ_ERROR
          ? LS_IQ_COVERAGE_UNAVAILABLE : LS_IQ_COVERAGE_UNKNOWN;
    ls_iq_coverage_boundary(s_coverage,state,(uint64_t)esp_timer_get_time());
    portEXIT_CRITICAL(&s_observation_mux);
}
static void fm_observation_delivered(uint64_t now)
{
    if (!s_observation_config_valid || FM.mode == FM_MODE_SCAN) return;
    portENTER_CRITICAL(&s_observation_mux);
    if (!s_observation.accepting)
        ls_iq_observation_begin(&s_observation, s_observation_rate,
                                 s_observation_center);
    if (ls_iq_observation_deliver(&s_observation, FM_IQ_BLOCK_BYTES, now))
        ls_iq_mission_observe(s_mission, &s_observation, now);
    portEXIT_CRITICAL(&s_observation_mux);
}
static void fm_coverage_block_done(uint64_t now,bool current)
{
    portENTER_CRITICAL(&s_observation_mux);
    if(!current)ls_iq_coverage_boundary(s_coverage,LS_IQ_COVERAGE_UNKNOWN,now);
    else if(ls_iq_coverage_delivered(s_coverage,FM_IQ_BLOCK_BYTES,now)) {
        ls_rx_plan_qualified_block(s_rx_plan,now,FM_IQ_BLOCK_BYTES,
            s_coverage->pending.read_started_us,s_coverage->pending.nominal_rate_hz);
        if(s_rx_plan && s_coverage->status.overwritten_intervals)s_rx_plan->status.detail_truncated=true;
    }
    portEXIT_CRITICAL(&s_observation_mux);
}
/**/
static EXT_RAM_BSS_ATTR fm_dsp_t s_dsp;
static EXT_RAM_BSS_ATTR fm_lr_dsp_t s_lr_dsp;
_Static_assert(FM_DEMOD_RATE == 32000, "Receive tone decimation requires 32 kHz");

_Static_assert(FM_RTL_RATE == AIS_SAMPLE_RATE, "AIS channel filters require 256 kHz IQ");
_Static_assert(FM_AUDIO_RATE == APRS_SAMPLE_RATE, "APRS needs FM audio at 16 kHz");
_Static_assert(FM_AUDIO_RATE == SAME_SAMPLE_RATE, "SAME needs FM audio at 16 kHz");

/* HF: AM mode below 30 MHz, and any SSB, CW or SAM, runs hf_demod.c
   (fm_hf.h). The radio is tuned FM_HF_LO_OFFSET_HZ beside the dial so the
   dongle's DC spike and the noise close to it fall outside the channel; the
   demodulator mixes the dial back to 0 Hz. */
#define FM_HF_LO_OFFSET_HZ 20000
_Static_assert((int)FM_HF_AM == (int)HF_DEMOD_AM && (int)FM_HF_SAM == (int)HF_DEMOD_SAM &&
               (int)FM_HF_USB == (int)HF_DEMOD_USB && (int)FM_HF_LSB == (int)HF_DEMOD_LSB &&
               (int)FM_HF_CW == (int)HF_DEMOD_CW, "fm_hf and hf_demod number modes alike");
_Static_assert(FM_RTL_RATE == HF_IN_RATE && FM_AUDIO_RATE == HF_OUT_RATE,
               "the HF demod runs on the FM rates");
_Static_assert(HF_OUT_RATE == 16000u, "the HF audio feeds fm_sstv_audio, which takes 16 kHz");
static EXT_RAM_BSS_ATTR hf_demod_t s_hf;
static bool     s_hf_ready;     /* hf_demod_init has run                      */
static bool     s_hf_on;        /* the radio is offset for HF, the chain runs */
static int32_t  s_hf_tune_hz;   /* where the dial sits in the IQ              */
static uint32_t s_hf_gen;

static bool fm_hf_wanted(uint32_t hz)
{
    return fm_hf_chain(FM.mode == FM_MODE_AM, hz, fm_hf_get()->demod);
}

static uint32_t fm_hf_hw_center(uint32_t dial, int32_t *tune_hz)
{
    if (dial + FM_HF_LO_OFFSET_HZ <= 1766000000u) {
        *tune_hz = -FM_HF_LO_OFFSET_HZ;
        return dial + FM_HF_LO_OFFSET_HZ;
    }
    *tune_hz = FM_HF_LO_OFFSET_HZ;
    return dial - FM_HF_LO_OFFSET_HZ;
}

static void fm_hf_configure(void)
{
    const fm_hf_settings_t *h = fm_hf_get();
    s_hf_gen = fm_hf_generation();
    const hf_demod_cfg_t cfg = {
        .mode = (hf_demod_mode_t)h->demod, .filter_hz = fm_hf_filter_hz(h),
        .tune_hz = s_hf_tune_hz, .rit_hz = h->rit_hz, .cw_pitch_hz = h->pitch_hz,
        .agc = (hf_agc_t)h->agc, .nb = (hf_nb_t)h->nb,
    };
    if (!s_hf_ready) { hf_demod_init(&s_hf); s_hf_ready = true; }
    hf_demod_set(&s_hf, &cfg);
}


/* The receiver as an RX HEAD stream (rx/ls_rx_head.h): the decoders a mode
   names are plugins attached to it, fed the discriminator blocks below. A
   preset changes only with the LR capture stopped or on this task. */
static EXT_RAM_BSS_ATTR ls_rx_head_t s_head;
static EXT_RAM_BSS_ATTR ls_rx_front_t s_front;
static int s_head_mode = -1;
static volatile bool s_head_lr;      /* the head is fed from the LR2021 */
static volatile bool s_head_redo;    /* the preset's plugin set changed */
static bool s_head_gate;             /* the voice's squelch, for the recorder */

/* Calls are recorded from NFM listening only, while no plan holds them. */
static bool fm_head_records(void)
{
    return FM.mode == FM_MODE_LISTEN && FM.squelch_tenths > 0 &&
           !__atomic_load_n(&s_plan_calls_suppressed, __ATOMIC_ACQUIRE);
}

static void fm_head_gate(void *user, bool open)
{
    (void)user;
    s_head_gate = open;
    call_archive_gate(CALL_FM, FM.freq_hz, open && fm_head_records());
}

static bool fm_head_audio(void *user, const int16_t *pcm, int n, uint32_t rate)
{
    (void)user; (void)rate;
    if (s_head_gate && fm_head_records())
        call_archive_audio(CALL_FM, FM.freq_hz, 0, 0, true, pcm, (unsigned)n);
    if (!scan_engine_audio_open()) return false;
    const uint32_t before = audio_drops_get();
    audio_write_mono(pcm, n);
    const uint32_t lost = audio_drops_get() - before;
    if (s_head_lr) FM.lr_audio_drops += lost;
    return lost != 0;
}

/* The plugins a mode names. POCSAG may keep the channel audible beside it
   (FM.pager_voice, off by default). */
static const ls_rx_plugin_t *fm_head_data(int mode)
{
    switch (mode) {
    case FM_MODE_POCSAG: return &rx_pocsag;
    case FM_MODE_FLEX:   return &rx_flex;
    case FM_MODE_APRS:   return &rx_aprs;
    case FM_MODE_SAME:   return &rx_same;
    case FM_MODE_AIS:    return &rx_ais;
    case FM_MODE_ACARS:  return &rx_acars;
    default:             return NULL;
    }
}

/* False when the mode's decoder could not be made (no PSRAM). */
LS_COLD static bool fm_head_preset(int mode)
{
    ls_rx_head_detach_all(&s_head);
    /* ACARS rides the AM envelope; AIS reads the SDR's own I/Q. */
    const ls_rx_stream_t s = { .center_hz = FM.freq_hz,
        .front = mode == FM_MODE_ACARS ? LS_RX_FRONT_AM : mode == FM_MODE_AIS ? LS_RX_FRONT_NONE
                                       : LS_RX_FRONT_NFM,
        .inputs = LS_RX_IQ_U8 | LS_RX_IQ_S16 | LS_RX_DISC | LS_RX_ENV | LS_RX_PCM16 };
    ls_rx_head_init(&s_head, &s);
    ls_rx_head_set_audio(&s_head, fm_head_audio, fm_head_gate, NULL);
    ls_rx_front_init(&s_front, &s_dsp, &s_lr_dsp);
    ls_rx_front_bind(&s_front, &s_head);
    s_head_mode = mode;
    s_head_redo = false;
    char why[48];
    if (mode == FM_MODE_LISTEN || (mode == FM_MODE_POCSAG && FM.pager_voice)) {
        if (ls_rx_head_attach(&s_head, &rx_voice, why, sizeof(why)) < 0) ESP_LOGE(TAG, "%s", why);
        /* The LR path's conservative peak cap and its fed speaker. */
        const rx_voice_cfg_t cfg = { .peak_cap = s_head_lr ? 6000 : 0, .pad_closed = s_head_lr };
        rx_voice_bind(ls_rx_head_state(&s_head, &rx_voice), &FM, &cfg);
    }
    /* SAME alerts open the weather speaker (fm_wx.h) as well as storing. */
    if (mode == FM_MODE_SAME) rx_same_sink(fm_wx_on_same, NULL);
    const ls_rx_plugin_t *data = fm_head_data(mode);
    if (data && ls_rx_head_attach(&s_head, data, why, sizeof(why)) < 0) {
        ESP_LOGE(TAG, "%s", why);
        return false;
    }
    if (mode == FM_MODE_POCSAG) {
        rx_pocsag_bind(ls_rx_head_state(&s_head, &rx_pocsag), &FM);
        FM.pocsag_lock_baud = 0;
    }
    return true;
}

/* A decoder's lock, for the receive log (owner task only). */
static bool fm_head_synced(const ls_rx_plugin_t *p)
{
    ls_rx_status_t st;
    for (int i = 0; i < s_head.n; i++)
        if (s_head.slot[i].p == p) return ls_rx_head_status(&s_head, i, &st) && st.synced;
    return false;
}

/* BROADCAST's taps on the front's audio, after the plugins had the block:
   SSTV listens on NFM (a shut squelch is silence of the same length, so a
   picture's timing survives a fade) and SAME mode's weather speaker. */
static void fm_head_taps(int mode)
{
    if (mode == FM_MODE_LISTEN && fm_sstv_wanted())
        fm_sstv_audio(FM.squelch_open ? s_front.pcm : NULL, s_front.na);
    else if (mode == FM_MODE_SAME)
        fm_wx_audio(&s_dsp, s_front.nd, s_front.pcm, s_front.na);
}

/* Modes whose receive chain is the head's front and plugins. */
static inline bool fm_head_mode(int mode)
{
    return mode == FM_MODE_LISTEN || fm_head_data(mode) != NULL;
}

/* The head follows the mode and the path feeding it (the voice's cap). */
static void fm_head_sync(bool lr)
{
    if (s_head_mode != (int)FM.mode || s_head_lr != lr || s_head_redo) {
        s_head_lr = lr;
        fm_head_preset(FM.mode);
    }
}

const struct ls_rx_head *lakeshark_fm_rx_head(void) { return &s_head; }

void lakeshark_fm_pager_voice(bool on)
{
    FM.pager_voice = on;
    s_head_redo = true;
}

static int      s_scan_phase = 0;
static int      s_scan_count = 0;
/**/ /* s_scan_accum/s_scan_naccum retired - the sweep averages in
   the frequency domain now, inside spectrum.c. */

static int fm_requested_gain(int tenths)
{

    if ((FM.mode == FM_MODE_POCSAG || FM.mode == FM_MODE_ACARS) &&
        !s_gain_chosen && tenths != 0) {

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

/**/

/**/
/* THE THIRD FAST-HOPPING CALLER, and the one nobody had covered. */

static inline bool fm_fast_hopping(void)
{
    return scan_engine_sweeping() || FM.mode == FM_MODE_SCAN || fm_air_running();
}

LS_COLD static void fm_tune_hw(uint32_t hz)
{
    if (!s_session || hz < 1000000UL) return;
    fm_observation_boundary(LS_IQ_OBS_TUNE);
    s_observation_config_valid = false;
    fm_spectrum_invalidate();
    uint64_t actual_hz = 0;
    ls_radio_err_t error = ls_radio_iq_retune(s_session, hz,
                                               fm_fast_hopping(), &actual_hz);
    ls_iq_control_note_tune_result(&s_radio_control, hz, error, actual_hz);
    s_observation_center = actual_hz;
    s_observation_config_valid = error == LS_RADIO_OK && s_observation_rate != 0;
    if (error != LS_RADIO_OK)
        ESP_LOGW(TAG, "retune failed: %s", ls_radio_err_name(error));
}

static uint32_t fm_mode_default_freq(fm_mode_t m)
{
    switch (m) {
        case FM_MODE_SAME:   return 162550000UL;
        case FM_MODE_APRS:   return aprs_frequency();
        case FM_MODE_AIS:    return AIS_CENTER_HZ;
        case FM_MODE_WFM:    return FM_FREQ_WFM;
        case FM_MODE_AM:     return 127500000UL;
        case FM_MODE_POCSAG: return FM_FREQ_POCSAG;
        case FM_MODE_FLEX:   return FM_FREQ_FLEX;
        case FM_MODE_LISTEN: return FM_FREQ_LISTEN;
        case FM_MODE_ACARS:  return FM_FREQ_ACARS;
        default:             return FM_FREQ_POCSAG;
    }
}

static void fm_load_tone(uint32_t hz)
{
    FM.tone_required = 0;
    int zone = scan_engine_get_zone();
    for (int i = 0; i < scan_channels_count(); ++i) {
        const scan_channel_t *c = scan_channel_get(i);
        if (c && c->freq_hz == hz && c->mode == SCAN_MODE_NFM &&
            (zone < 0 || c->zone == zone)) {
            FM.tone_required = scan_channel_tone(c);
            break;
        }
    }
}

static void fm_apply_freq(uint32_t hz)
{
    if (hz < LS_HF_MIN_HZ) return;
    fm_load_tone(hz);
    FM.freq_hz = hz;
    ls_iq_control_request_tune(&s_radio_control, hz, fm_fast_hopping());
}

/**/
/* THE SWEEP IS AN FFT NOW, NOT A POWER METER ON A LADDER. */

#define FM_SPEC_USABLE_HZ   200000u
/* Buffers folded into each tune position before the spectrum is read. */
#define FM_SPEC_AVG         4
/* Bins either side of centre discarded as the DC/LO spike. */
#define FM_SPEC_DC_GUARD    3
/* Display normalises against FM_SCAN_FLOOR_DB; below it everything reads as
   floor. moved it to fm_state.h, where the readers can see it. */

EXT_RAM_BSS_ATTR static float s_spec_db[SPEC_FFT_N];

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

/* How long a sweep takes, published for the waterfall. */

static int64_t s_sweep_t0;
static bool    s_sweep_whole;

static uint32_t scan_sweep_estimate_ms(void)
{
    /* A block is FM_IQ_BLOCK_BYTES of interleaved 8-bit I and Q. */
    const uint64_t block_us =
        (uint64_t)(FM_IQ_BLOCK_BYTES / 2) * 1000000u / FM_RTL_RATE;
    return (uint32_t)((uint64_t)FM.scan_tunes *
                      (FM_SCAN_SETTLE + FM_SPEC_AVG) * block_us / 1000u);
}

LS_COLD static void scan_begin(bool reset_pos)
{
    uint32_t span = (FM.scan_stop_hz > FM.scan_start_hz)
                  ? (FM.scan_stop_hz - FM.scan_start_hz) : 0;

    FM.scan_tunes = scan_tune_count();

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
    /* The band may be new, so the estimate is too. */
    FM.scan_sweep_ms = scan_sweep_estimate_ms();
    s_sweep_t0 = esp_timer_get_time();
    s_sweep_whole = (FM.scan_idx == 0);
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

        float lin = (s_spec_db[j] - FM_SCAN_FLOOR_DB)
                  / (FM_SCAN_TOP_DB - FM_SCAN_FLOOR_DB);
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
        /* A whole sweep timed replaces the estimate. Written before
           the count moves, so a reader that sees the new sweep sees its
           period with it. */
        const int64_t now = esp_timer_get_time();
        if (s_sweep_whole)
            FM.scan_sweep_ms = (uint32_t)((now - s_sweep_t0) / 1000);
        s_sweep_t0 = now;
        s_sweep_whole = true;
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
    fm_rx_plan_lost();
    fm_observation_boundary(error == LS_RADIO_ERR_STOPPED
        ? LS_IQ_OBS_STOPPED : LS_IQ_OBS_RECEIVER_ERROR);
    ls_rx_head_reset(&s_head, LS_RX_RESET_GAP);
    fm_spectrum_invalidate();
    FM.iq_bytes_sec = 0;
    FM.iq_level = 0.0f;
    FM.audio_level = 0.0f;
    FM.noise = 1.0f;
    FM.squelch_open = false;
    FM.tone_detected = (fm_tone_result_t){0};
    FM.pocsag_sync = false;
    FM.flex_sync = false;
    FM.hf_active = false;
    ls_iq_control_receiver_lost(&s_radio_control, error);
}

/* What the receiver is doing, as the RADIO picker files it: a pager and
   ACARS can each be given their own dongle. */
static ls_rsel_job_t fm_radio_job(void)
{
    if (FM.mode == FM_MODE_POCSAG || FM.mode == FM_MODE_FLEX) return LS_RSEL_PAGER;
    if (FM.mode == FM_MODE_ACARS) return LS_RSEL_ACARS;
    return LS_RSEL_FM;
}

static ls_radio_err_t fm_radio_open(void)
{
    const uint32_t dial_hz = FM.mode == FM_MODE_SCAN
                               ? scan_tune_center(FM.scan_idx) : FM.freq_hz;
    const bool hf = FM.mode != FM_MODE_SCAN && fm_hf_wanted(dial_hz);
    const uint32_t center_hz = hf ? fm_hf_hw_center(dial_hz, &s_hf_tune_hz) : dial_hz;
    /* Below the old 24 MHz floor only a receiver that reaches HF will do. */
    const ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .min_hz = center_hz < LS_RTL_TUNER_MIN_HZ ? center_hz : LS_RTL_TUNER_MIN_HZ,
        .max_hz = 1766000000UL,
        .sample_rate_hz = FM_RTL_RATE,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .preferred_endpoint_id = ls_rsel_sdr_endpoint(fm_radio_job()),
    };
    ls_radio_err_t error = ls_radio_acquire("fm", &requirements, &s_session);
    if (error != LS_RADIO_OK) {
        fm_receiver_lost(error);
        return error;
    }

    s_session_endpoint = requirements.preferred_endpoint_id;
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
        return error;
    }
    ls_iq_control_set_streaming(&s_radio_control, false, LS_RADIO_OK);
    s_observation_rate = actual.sample_rate_hz;
    s_observation_center = actual.center_hz;
    s_observation_config_valid = actual.sample_rate_hz != 0;
    fm_observation_boundary(FM.mode == FM_MODE_SCAN
        ? LS_IQ_OBS_UNSUPPORTED : LS_IQ_OBS_TUNE);
    s_hf_on = hf;
    if (hf) {
        /* The dial was asked for and is in force, offset or not. */
        ls_iq_control_note_tune_result(&s_radio_control, dial_hz, LS_RADIO_OK,
                                       actual.center_hz);
        fm_hf_configure();
        hf_demod_reset(&s_hf);
    }
    ESP_LOGI(TAG, "radio: %.4f MHz %lukSPS gain=%d",
             actual.center_hz / 1e6,
             (unsigned long)(actual.sample_rate_hz / 1000),
             actual.gain_tenths_db);
    return LS_RADIO_OK;
}

static uint32_t s_lr_hz, s_lr_channel, s_lr_lost, s_lr_gaps, s_lr_bytes;
static int64_t s_lr_stats_us;

static void fm_lr_reset_chain(void)
{
    fm_lr_reset(&s_lr_dsp);
    fm_dsp_init(&s_dsp);
    ls_rx_head_reset(&s_head, LS_RX_RESET_GAP);
    FM.squelch_open = false;
    FM.iq_level = 0; FM.noise = 1;
}
/* Each run of the shared session is narrow or wide; the first block after a
   stream end says which, and narrow gets the DC blocker for the LO leakage
   it carries at 0 Hz (fm_lr_dsp.h). */
static bool s_lr_dc_pending;
static void fm_lr_consume(void *ctx, const uint8_t *data, size_t bytes, bool spare)
{
    (void)ctx;
    if (!data) { fm_lr_reset_chain(); s_lr_dc_pending = true; return; }
    if (s_lr_dc_pending) {
        lr_wf_wide_status w; lr_wf_wide_get(&w);
        fm_lr_set_dc_block(&s_lr_dsp, !w.wide);
        s_lr_dc_pending = false;
    }
    s_lr_bytes += bytes;
    int64_t now = esp_timer_get_time();
    if (now - s_lr_stats_us >= 1000000) {
        FM.iq_bytes_sec = (uint32_t)((uint64_t)s_lr_bytes * 1000000 / (now-s_lr_stats_us));
        s_lr_bytes = 0; s_lr_stats_us = now;
    }
    /* At most 320 pairs per checked block, 83 demod samples. PSRAM state;
       bounded stack scratch. Catch up by dropping audio, never waiting. */
    int16_t pcm[44];
    const int mode = FM.mode;
    if (fm_head_mode(mode)) {
        /* The receive task sets the preset before a capture starts; this
           only catches a caller that feeds without one. */
        if (s_head_mode != mode) { s_head_lr = true; fm_head_preset(mode); }
        if (!ls_rx_head_admit(&s_head, spare)) {
            FM.lr_audio_drops++;
            fm_lr_reset_chain();
            return;
        }
        ls_rx_front_lr(&s_front, &s_head, data, bytes, now);
        FM.iq_level = s_dsp.iq_block_peak;
        FM.audio_level = s_front.rms;
        fm_head_taps(mode);
        ls_iq_control_set_streaming(&s_radio_control, true, LS_RADIO_OK);
        ls_iq_control_note_tune_result(&s_radio_control, s_lr_hz, LS_RADIO_OK, s_lr_hz);
        return;
    }
    if (!spare) {
        FM.lr_audio_drops++;
        fm_lr_reset_chain();
        return;
    }
    if (mode == FM_MODE_AM) {
        /* AM voice: envelope, levelled, gated on carrier level like the SDR path. */
        int na = fm_lr_demod_am(&s_lr_dsp, &s_dsp, data, bytes, pcm, 44);
        FM.iq_level = s_dsp.iq_peak;
        FM.squelch_open = (FM.iq_level * 1000.0f) >= (float)FM.squelch_tenths;
        float ss = 0.0f;
        for (int i = 0; i < na; ++i) ss += (float)pcm[i] * pcm[i];
        FM.audio_level = na > 0 ? sqrtf(ss / na) / 8000.0f : 0.0f;
        if (FM.squelch_tenths > 0 && FM.squelch_open && na > 0)
            call_archive_audio(CALL_FM, FM.freq_hz, 0, 0, true, pcm, (unsigned)na);
        call_archive_gate(CALL_FM, FM.freq_hz, FM.squelch_open && FM.squelch_tenths > 0);
        if (fm_sstv_wanted()) fm_sstv_audio(pcm, na);
        if (na > 0 && FM.squelch_open && scan_engine_audio_open()) {
            uint32_t before = audio_drops_get();
            audio_write_mono(pcm, na);
            FM.lr_audio_drops += audio_drops_get() - before;
        }
        ls_iq_control_set_streaming(&s_radio_control, true, LS_RADIO_OK);
        ls_iq_control_note_tune_result(&s_radio_control, s_lr_hz, LS_RADIO_OK, s_lr_hz);
        return;
    }
}
/* A retune keeps the mesh lease and the LR ownership flag: handing the radio
   back to MeshCore between captures made the next hold_bg(true) fail until the
   mesh task acknowledged, so every scanner step reported BUSY. */
static bool s_lr_lease;   /* this task is a background holder of the mesh radio */
static void fm_lr_release(void)
{
    FM.lr_iq_active = false;
    if (s_lr_lease) { ls_mesh_radio_hold_bg(false); s_lr_lease = false; }
}
static void fm_lr_stop_capture(bool retune)
{
    if (s_lr_session) {
        lr_wf_stats st;
        lr_wf_stop_stats(&st);
        FM.lr_lost_pairs = s_lr_lost + st.lost_pairs;
        FM.lr_gaps = s_lr_gaps + st.gaps;
        s_lr_session = false;
        fm_receiver_lost(LS_RADIO_ERR_STOPPED);
    }
    /* Teardown releases a lease kept across a retune even with no capture. */
    if (!retune) fm_lr_release();
}
static void fm_lr_stop(void) { fm_lr_stop_capture(false); }
static bool fm_lr_wanted(void)
{
    const ls_rsel_job_t job = fm_radio_job();
    return fm_lr_supported(FM.freq_hz, FM.mode) && !fm_hf_wanted(FM.freq_hz) &&
        ls_lora_chip() == LS_LORA_CHIP_LR20XX &&
        (ls_rsel_saved(job) == LS_RSEL_LORA || !ls_rsel_sdr_endpoint(job));
}
static void fm_lr_step(void)
{
    fm_observation_boundary(LS_IQ_OBS_UNSUPPORTED);
    s_observation_config_valid = false;
    if (s_session) { ls_radio_iq_stop(s_session); ls_radio_release(s_session); s_session = NULL; }
    /* Data modes take the wide filter: a 1200 baud pager at 4.5 kHz deviation
       is about 11 kHz wide, right at the 12.5 kHz filter's edge. */
    uint32_t channel = (FM.mode == FM_MODE_LISTEN || FM.mode == FM_MODE_AM) &&
                       FM.lr_channel_hz == 12500 ? 12500 : 25000;
    if (s_lr_session) {
        lr_wf_stats st; lr_wf_get_stats(&st);
        FM.lr_lost_pairs = s_lr_lost + st.lost_pairs;
        FM.lr_gaps = s_lr_gaps + st.gaps;
        if (st.done || s_lr_hz != FM.freq_hz || s_lr_channel != channel || ls_mesh_radio_wanted()) {
            fm_lr_stop_capture(!st.done && !ls_mesh_radio_wanted());
            if (st.done) { FM.read_errors++; vTaskDelay(pdMS_TO_TICKS(100)); }
        }
    }
    if (!s_lr_session) {
        /* A foreground owner wants the radio: give back a lease kept across a
           retune, or the owner waits on it forever. A refused hold_bg stays
           registered so the mesh task can hand over; retry next step. */
        if (ls_mesh_radio_wanted()) {
            fm_lr_release();
            fm_receiver_lost(LS_RADIO_ERR_BUSY); vTaskDelay(pdMS_TO_TICKS(100)); return;
        }
        s_lr_lease = true;
        if (!ls_mesh_radio_hold_bg(true)) {
            fm_receiver_lost(LS_RADIO_ERR_BUSY); vTaskDelay(pdMS_TO_TICKS(100)); return;
        }
        s_lr_hz = FM.freq_hz; s_lr_channel = channel;
        s_lr_bytes = 0; s_lr_stats_us = esp_timer_get_time();
        s_lr_lost = FM.lr_lost_pairs; s_lr_gaps = FM.lr_gaps;
        fm_lr_init(&s_lr_dsp, s_lr_channel);
        s_lr_dc_pending = true;
        fm_head_sync(true);
        fm_lr_reset_chain();
        /* Mark ownership before launch so the waterfall never creates a second capture. */
        FM.lr_iq_active = true;
        if (lr_wf_start_consumer(s_lr_hz, fm_lr_consume, NULL) != ESP_OK) {
            fm_lr_release();
            fm_receiver_lost(LS_RADIO_ERR_BUSY); vTaskDelay(pdMS_TO_TICKS(100)); return;
        }
        s_lr_session = true;
    }
    if (s_squelch_req >= 0) { FM.squelch_tenths = s_squelch_req; s_squelch_req = -1; }
    ls_iq_control_request_t request;
    (void)ls_iq_control_take(&s_radio_control, &request);
    vTaskDelay(1);
}

static void fm_fail_receiver_start(void);

static void fm_rx_run_once(void)
{
    /* PSRAM, not internal. */

    uint8_t *iq = heap_caps_malloc(FM_IQ_BLOCK_BYTES, MALLOC_CAP_SPIRAM);
    if (!iq) iq = heap_caps_malloc(FM_IQ_BLOCK_BYTES,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    /**/
    static EXT_RAM_BSS_ATTR int16_t pcm[600];

    if (!iq) {
        ESP_LOGE(TAG, "OOM iq buf");
        fm_fail_receiver_start();
        return;
    }
    ESP_LOGI(TAG, "rx task up: mode=%d freq=%lu gain=%d",
             FM.mode, (unsigned long)FM.freq_hz, FM.gain_tenths);
    fm_air_install(fm_tune_hw);
    fm_wx_install(fm_tune_hw);

    uint32_t iq_bucket = 0;
    int64_t stats_ts = esp_timer_get_time();
    int64_t last_yield = stats_ts;
    bool allocation_failed = false;
    radio_open_retry_t open_retry;
    radio_open_retry_reset(&open_retry);
    while (fm_lifecycle_active()) {
        fm_rx_plan_poll();

        fm_mode_t requested_mode;
        if (fm_mode_handoff_take(&s_mode_handoff, &requested_mode)) {
            fm_observation_boundary(LS_IQ_OBS_MODE);
            fm_lr_stop();
            if (requested_mode != FM_MODE_AM) fm_air_stop(true);
            if (requested_mode != FM_MODE_SAME) fm_wx_stop();
            call_archive_end(CALL_FM, false);
            int m = (int)requested_mode;

            if (FM.mode != FM_MODE_SCAN) s_mode_freq[FM.mode] = FM.freq_hz;
            const uint32_t scan_tune = FM.mode == FM_MODE_SCAN ? s_scan_tune_hz : 0;
            s_scan_tune_hz = 0;
            const ls_rsel_job_t previous_job = fm_radio_job();
            FM.mode = (fm_mode_t)m;
            if (FM.mode != FM_MODE_SCAN) settings_set_fm_mode((uint32_t)FM.mode);
            if (previous_job != fm_radio_job()) radio_open_retry_reset(&open_retry);
            if (fm_receive_switch(&s_session, s_session_endpoint,
                                  ls_rsel_sdr_endpoint(fm_radio_job()))) {
                fm_receiver_lost(LS_RADIO_ERR_UNAVAILABLE);
                radio_open_retry_reset(&open_retry);
                iq_bucket = 0;
                stats_ts = esp_timer_get_time();
            }
            fm_dsp_init(&s_dsp);
            if (FM.mode == FM_MODE_WFM) fm_wfm_reset();
            else fm_wfm_idle();
            fm_sstv_reset();
            if (FM.mode == FM_MODE_SCAN) {
                scan_begin(true);
            } else {
                uint32_t f = s_frequency_locked ? s_frequency_lock_hz
                                                : (FM.mode == FM_MODE_AIS ? AIS_CENTER_HZ : FM.mode == FM_MODE_APRS ? aprs_frequency() : scan_tune ? scan_tune : s_mode_freq[FM.mode]);
                FM.freq_hz = FM.mode == FM_MODE_AIS ? AIS_CENTER_HZ : (f >= LS_HF_MIN_HZ) ? f : fm_mode_default_freq(FM.mode);
                fm_apply_freq(FM.freq_hz);
            }
            s_head_lr = false;
            if (!fm_head_preset(FM.mode) && FM.mode != FM_MODE_SAME) {
                fm_receiver_lost(LS_RADIO_ERR_NO_MEMORY);
                allocation_failed = true;
                break;
            }
            if (FM.mode == FM_MODE_POCSAG || FM.mode == FM_MODE_ACARS) {
                /* POCSAG defaults to AGC. */

                if (!s_gain_chosen && FM.gain_tenths != 0) {
                    ESP_LOGI(TAG, "pager/ACARS: no gain chosen - using AGC");
                    fm_apply_gain(0);   /* fm_apply_gain() also enforces it */
                }
            }
            if (FM.mode == FM_MODE_LISTEN || FM.mode == FM_MODE_WFM || FM.mode == FM_MODE_AM) audio_out_ensure_unmuted();
        }
        /* A changed plugin set (the pager's voice) restarts the capture. */
        if (s_head_redo) { fm_lr_stop(); s_head_mode = -1; }
        if (fm_lr_wanted()) { fm_lr_step(); continue; }
        fm_lr_stop();
        if (!s_session) {
            /* Bounded for io; see radio_open_retry.c. Holding costs
               one atomic read every quarter second and never touches the
               dongle; the next attach or detach lifts it. */
            const uint32_t generation = ls_radio_endpoint_generation();
            if (!radio_open_retry_may_try(&open_retry, generation)) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            const ls_radio_err_t open_error = fm_radio_open();
            if (open_error != LS_RADIO_OK) {
                if (radio_open_retry_failed(&open_retry, open_error,
                                            generation) ==
                    RADIO_OPEN_RETRY_HOLD)
                    ESP_LOGE(TAG, "radio open failed with io %u times in a row - "
                                  "not retrying until a receiver attaches or "
                                  "detaches (replug, 'rtl reset', or re-enter FM)",
                             (unsigned)RADIO_OPEN_IO_ATTEMPTS);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            radio_open_retry_succeeded(&open_retry);
        }

        /* The airband / CB channel scanner takes the screen's requests here,
           before the HF check: while it runs it owns the LO (its own fs/4
           offset, fm_air.c) and the HF chain stands down, CB on 27 MHz
           included. When it stops, the check below puts the HF offset back
           if the dial it left wants one. */
        if (FM.mode == FM_MODE_AM) fm_air_poll(FM.freq_hz);
        /* A demod change can move AM mode onto or off the HF chain (SSB on
           2 m, plain AM back on airband): retune so the offset follows. */
        if (FM.mode == FM_MODE_SCAN || fm_air_running()) s_hf_on = false;
        else if (fm_hf_wanted(FM.freq_hz) != s_hf_on)
            ls_iq_control_request_tune(&s_radio_control, FM.freq_hz, false);
        if (s_hf_on && fm_hf_generation() != s_hf_gen) fm_hf_configure();

        ls_iq_control_request_t radio_request;
        if (ls_iq_control_take(&s_radio_control, &radio_request)) {
            if ((radio_request.flags & LS_IQ_CONTROL_TUNE) && fm_rx_plan_control_allowed(&radio_request)) {
                /* A hand on the dial ends the channel scan where it is. */
                fm_air_stop(false);
                fm_observation_boundary(LS_IQ_OBS_TUNE);
                s_observation_config_valid = false;
                call_archive_end(CALL_FM, false);
                fm_spectrum_invalidate();
                FM.squelch_open = false;
                FM.iq_level = 0;
                fm_dsp_init(&s_dsp);
                ls_rx_head_retune(&s_head, radio_request.center_hz);
                if (FM.mode == FM_MODE_WFM) fm_wfm_reset();
                fm_sstv_reset();
                ls_iq_control_request_t applied = radio_request;
                const bool hf = FM.mode != FM_MODE_SCAN &&
                                fm_hf_wanted(radio_request.center_hz);
                if (hf) applied.center_hz = fm_hf_hw_center(radio_request.center_hz,
                                                            &s_hf_tune_hz);
                ls_radio_err_t error = ls_iq_control_apply_tune(
                    &s_radio_control, s_session, &applied);
                s_observation_center = s_radio_control.effective_center_known
                    ? s_radio_control.effective_center_hz : 0;
                s_observation_config_valid = error == LS_RADIO_OK && s_observation_rate != 0;
                fm_rx_plan_tune_result(&radio_request,error);
                s_hf_on = hf;
                if (hf) { fm_hf_configure(); hf_demod_reset(&s_hf); }
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
            ls_rx_head_reset(&s_head, LS_RX_RESET_USER);
        }
        if (s_squelch_req >= 0) { FM.squelch_tenths = s_squelch_req; s_squelch_req = -1; }
        if (s_scan_restart) { s_scan_restart = false; if (FM.mode == FM_MODE_SCAN) scan_begin(true); }
        if (s_tune_peak_req) {
            s_tune_peak_req = false;
            if (FM.scan_peak_hz) { FM.freq_hz = FM.scan_peak_hz; }
        }

        if (!fm_lifecycle_active()) break;
        ls_radio_iq_read_receipt_t read_stamp={0};
        portENTER_CRITICAL(&s_observation_mux);
        bool coverage_active=s_coverage && s_coverage->status.active;
        portEXIT_CRITICAL(&s_observation_mux);
        const ls_radio_err_t read_error = coverage_active
            ? fm_receive_block_observed(s_session,iq,FM_IQ_BLOCK_BYTES,fm_lifecycle_active,esp_timer_get_time,&read_stamp)
            : fm_receive_block(s_session,iq,FM_IQ_BLOCK_BYTES,fm_lifecycle_active,esp_timer_get_time);
        if (read_error != LS_RADIO_OK) {
            if (read_error == LS_RADIO_ERR_STOPPED) break;
            call_archive_gate(CALL_FM, FM.freq_hz, false);
            FM.read_errors++;
            fm_receiver_lost(read_error);
            fm_observation_boundary(LS_IQ_OBS_READ_ERROR);
            iq_bucket = 0;
            stats_ts = esp_timer_get_time();
            if (read_error != LS_RADIO_ERR_TIMEOUT) {
                ls_radio_release(s_session);
                s_session = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        ls_iq_control_set_streaming(&s_radio_control, true, LS_RADIO_OK);
        portENTER_CRITICAL(&s_observation_mux);
        bool admitted=(!s_coverage || !s_coverage->status.active) ||
            ls_iq_coverage_accept(s_coverage,&read_stamp,FM_IQ_BLOCK_BYTES);
        portEXIT_CRITICAL(&s_observation_mux);
        if(!admitted){vTaskDelay(1);continue;}
        iq_bucket += FM_IQ_BLOCK_BYTES;

        if (FM.mode != FM_MODE_SCAN && s_radio_control.effective_center_known)
            fm_spectrum_feed(iq, FM_IQ_BLOCK_BYTES,
                (uint32_t)s_radio_control.effective_center_hz, FM_RTL_RATE,
                (uint32_t)(esp_timer_get_time() / 1000));

        if (FM.mode == FM_MODE_SCAN) {
            scan_step(iq, FM_IQ_BLOCK_BYTES);
            FM.iq_level = fm_iq_rms(iq, FM_IQ_BLOCK_BYTES);
        } else if (FM.mode == FM_MODE_SAME && (fm_wx_poll(), fm_wx_finding())) {
            /* One look at all seven weather channels for the strongest. */
            fm_wx_block(iq, FM_IQ_BLOCK_BYTES);
            FM.iq_level = fm_iq_rms(iq, FM_IQ_BLOCK_BYTES);
            FM.audio_level = 0;
        } else if (FM.mode == FM_MODE_AM && fm_air_running()) {
            /* The airband / CB channel scanner: it tunes, measures and parks
               on its own, and hands back audio only for a live carrier. */
            int na = fm_air_block(&s_dsp, iq, FM_IQ_BLOCK_BYTES, pcm, 600);
            FM.hf_active = false;
            FM.iq_level = s_dsp.iq_peak;
            FM.squelch_open = na > 0;
            float ss = 0.0f;
            for (int i = 0; i < na; ++i) ss += (float)pcm[i] * pcm[i];
            FM.audio_level = na > 0 ? sqrtf(ss / na) / 8000.0f : 0.0f;
            const uint32_t heard = fm_air_listen_hz();
            if (na > 0 && heard) call_archive_audio(CALL_FM, heard, 0, 0, true, pcm, (unsigned)na);
            call_archive_gate(CALL_FM, heard, na > 0 && heard);
            if (na > 0) audio_write_mono(pcm, na);
        } else if (s_hf_on) {
            int na = hf_demod_u8(&s_hf, iq, FM_IQ_BLOCK_BYTES, pcm, 600);
            FM.hf_active = true;
            FM.hf_level_db = hf_demod_level_dbfs(&s_hf);
            FM.hf_gain_db = hf_demod_gain_db(&s_hf);
            FM.hf_sam_hz = hf_demod_sam_offset_hz(&s_hf);
            FM.hf_blanked = hf_demod_blanked(&s_hf);
            /* On AM's scale (iq_level x10 is the meter, the squelch is
               tenths of it) but read through the HF meter's dB travel, so the
               shared meter and its squelch gate need nothing of their own. */
            FM.iq_level = fm_hf_meter(FM.hf_level_db) * 0.1f;
            FM.squelch_open = FM.squelch_tenths == 0 ||
                              (FM.iq_level * 1000.0f) >= (float)FM.squelch_tenths;
            float ss = 0.0f;
            for (int i = 0; i < na; ++i) ss += (float)pcm[i] * pcm[i];
            FM.audio_level = na > 0 ? sqrtf(ss / na) / 8000.0f : 0.0f;
            if (FM.squelch_tenths > 0 && FM.squelch_open && na > 0)
                call_archive_audio(CALL_FM, FM.freq_hz, 0, 0, true, pcm, (unsigned)na);
            call_archive_gate(CALL_FM, FM.freq_hz, FM.squelch_open && FM.squelch_tenths > 0);
            /* DIGI decodes from the SSB audio before squelch and volume:
               FT8 and WSPR sit below the noise a squelch would gate. */
            if (na > 0 && digi_sink_wanted())
                digi_sink_push(pcm, na, (int)HF_OUT_RATE, FM.freq_hz);
            /* SSTV on 20 m and friends is USB: the decoder takes the HF
               audio as it comes out, before squelch, at HF_OUT_RATE. */
            if (na > 0 && fm_sstv_wanted()) fm_sstv_audio(pcm, na);
            if (na > 0 && FM.squelch_open) audio_write_mono(pcm, na);
        } else if (fm_head_mode(FM.mode)) {
            fm_head_sync(false);
            ls_rx_front_u8(&s_front, &s_head, iq, FM_IQ_BLOCK_BYTES, esp_timer_get_time());
            FM.iq_level = FM.mode == FM_MODE_AIS ? fm_iq_rms(iq, FM_IQ_BLOCK_BYTES)
                        : ls_rx_head_has_audio(&s_head) ? s_dsp.iq_block_peak : s_dsp.iq_peak;
            FM.audio_level = s_front.rms;
            if (FM.mode == FM_MODE_AIS) FM.squelch_open = false;
            fm_head_taps(FM.mode);
        } else if (FM.mode == FM_MODE_AM) {
            FM.hf_active = false;
            int na = fm_demod_am(&s_dsp, iq, FM_IQ_BLOCK_BYTES, pcm, 600);
            FM.iq_level = s_dsp.iq_peak;
            /* AM is a carrier mode, so level is the right measure here, but
               against a reachable scale: the same receiver reads 3% on dead
               air and 5% on a strong signal, so the control is read in tenths
               of a percent rather than whole percent. */
            FM.squelch_open = (FM.iq_level * 1000.0f) >= (float)FM.squelch_tenths;
            float ss = 0.0f;
            for (int i = 0; i < na; ++i) ss += (float)pcm[i] * pcm[i];
            FM.audio_level = na > 0 ? sqrtf(ss / na) / 8000.0f : 0.0f;
            if (FM.squelch_tenths > 0 && FM.squelch_open && na > 0)
                call_archive_audio(CALL_FM, FM.freq_hz, 0, 0, true, pcm, (unsigned)na);
            call_archive_gate(CALL_FM, FM.freq_hz, FM.squelch_open && FM.squelch_tenths > 0);
            if (fm_sstv_wanted()) fm_sstv_audio(pcm, na);
            if (na > 0 && FM.squelch_open) audio_write_mono(pcm, na);
        } else if (FM.mode == FM_MODE_WFM) {

            int na = fm_wfm_block(&s_dsp, iq, FM_IQ_BLOCK_BYTES, pcm, 600);
            FM.iq_level = s_dsp.iq_peak;
            FM.squelch_open = true;
            if (na > 0) {
                audio_write_mono(pcm, na);
                float ss = 0.0f;
                for (int i = 0; i < na; i++) ss += (float)pcm[i] * (float)pcm[i];
                FM.audio_level = sqrtf(ss / na) / 8000.0f;
            }
        }

        fm_sstv_service();
        int64_t now = esp_timer_get_time();
        /* This full block reached the selected DSP path above. The sample
         * count uses U8 I/Q pairs and actual configured nominal rate, never
         * time between calls. Successful tuning still does not prove which
         * RF interval buffered samples came from, or that none were lost. */
        bool post_context_current=read_stamp.known && ls_radio_iq_receipt_current(s_session,&read_stamp);
        if(!coverage_active || post_context_current)fm_observation_delivered((uint64_t)now);
        fm_coverage_block_done((uint64_t)now,post_context_current);
        if (now - stats_ts > 2000000LL) {
            FM.iq_bytes_sec = iq_bucket; iq_bucket = 0; stats_ts = now;

            if (FM.mode == FM_MODE_SCAN) {
                ESP_LOGI(TAG, "SCAN now=%.4f peak=%.4f (%d%%) sweeps=%lu iq=%d%%",
                    (FM.scan_start_hz + (uint32_t)FM.scan_idx * FM.scan_step_hz) / 1e6,
                    FM.scan_peak_hz / 1e6, (int)(FM.scan_peak_db * 100.0f),
                    (unsigned long)FM.scan_sweeps, (int)(FM.iq_level * 100.0f));
            } else if (FM.mode == FM_MODE_POCSAG) {
                int nm; uint32_t nn;
                rx_pocsag_near(ls_rx_head_state(&s_head, &rx_pocsag), &nm, &nn);
                ESP_LOGI(TAG, "POCSAG %.4f %s baud=%s%d frames=%lu pages=%lu err=%lu iq=%d%% act=%d%% near_min=%d near=%lu",
                    FM.freq_hz / 1e6, FM.pocsag_sync ? "SYNC" : "hunt",
                    FM.pocsag_auto ? "auto:" : "", FM.pocsag_baud,
                    (unsigned long)FM.pocsag_frames, (unsigned long)FM.pocsag_pages,
                    (unsigned long)FM.pocsag_cw_errs,
                    (int)(FM.iq_level * 100.0f), (int)(FM.audio_level / 0.65f * 100.0f),
                    nm, (unsigned long)nn);
            } else if (FM.mode == FM_MODE_FLEX) {
                ESP_LOGI(TAG, "FLEX %.4f %s mode=%u frames=%lu pages=%lu err=%lu iq=%d%% act=%d%% near_min=%d",
                    FM.freq_hz / 1e6, FM.flex_sync ? "SYNC" : "hunt",
                    (unsigned)FM.flex_mode, (unsigned long)FM.flex_frames,
                    (unsigned long)FM.flex_pages, (unsigned long)FM.flex_cw_errs,
                    (int)(FM.iq_level * 100.0f), (int)(FM.audio_level / 0.65f * 100.0f),
                    FM.flex_near_min);
            } else if (s_hf_on) {
                const fm_hf_settings_t *h = fm_hf_get();
                ESP_LOGI(TAG, "HF %.4f %s %luHz rit=%+d agc=%s nb=%s lvl=%.1fdBFS gain=%.0fdB sq=%s",
                    FM.freq_hz / 1e6, fm_hf_demod_label(h->demod),
                    (unsigned long)fm_hf_filter_hz(h), h->rit_hz,
                    fm_hf_agc_label(h->agc), fm_hf_nb_label(h->nb),
                    FM.hf_level_db, FM.hf_gain_db, FM.squelch_open ? "open" : "mute");
            } else if (FM.mode == FM_MODE_WFM) {
                char rds[128];
                ESP_LOGI(TAG, "WFM %.4f iq=%d%% af=%d%% audio(drop=%lu under=%lu) %s",
                    FM.freq_hz / 1e6, (int)(FM.iq_level * 100.0f),
                    (int)(FM.audio_level * 100.0f),
                    (unsigned long)audio_drops_get(), (unsigned long)audio_underruns_get(),
                    fm_wfm_summary(rds, sizeof(rds)));
                int core0_pct, core1_pct;
                if (ls_cpu_busy(&core0_pct, &core1_pct)) {
                    ESP_LOGW(TAG, "CPU busy: core0=%d%% core1=%d%% (tasks=%u)",
                             core0_pct, core1_pct,
                             (unsigned)uxTaskGetNumberOfTasks());
                }
            } else if (FM.mode == FM_MODE_AM && fm_air_running()) {
                char air[128];
                ESP_LOGI(TAG, "AIR %s audio(drop=%lu under=%lu)", fm_air_summary(air, sizeof(air)),
                         (unsigned long)audio_drops_get(), (unsigned long)audio_underruns_get());
            } else if (FM.mode == FM_MODE_ACARS) {
                const acars_state_t *as = acars_app_state();
                ESP_LOGI(TAG, "ACARS %.4f %s msgs=%lu bad=%lu sync=%lu iq=%d%%",
                    FM.freq_hz / 1e6,
                    fm_head_synced(&rx_acars) ? "SYNC" : "hunt",
                    (unsigned long)as->n_delivered,
                    (unsigned long)as->n_bad_crc,
                    (unsigned long)as->n_synced,
                    (int)(FM.iq_level * 100.0f));
            } else {
                ESP_LOGI(TAG, "LISTEN %.4f sq=%s iq=%d%% act=%d%% nse=%d audio(drop=%lu under=%lu)",
                    FM.freq_hz / 1e6, FM.squelch_open ? "open" : "mute",
                    (int)(FM.iq_level * 100.0f), (int)(FM.audio_level / 0.65f * 100.0f),
                    (int)(s_dsp.demod_noise * 1000.0f),
                    (unsigned long)audio_drops_get(), (unsigned long)audio_underruns_get());
            }
        }

        if (now - last_yield > 25000) { last_yield = now; vTaskDelay(1); }
    }

    fm_lr_stop();
    if (s_session) {
        (void)ls_radio_iq_stop(s_session);
    }
    call_archive_end(CALL_FM, false);
    fm_wfm_idle();
    fm_air_stop(false);
    heap_caps_free(iq);
    if (allocation_failed) fm_fail_receiver_start();
    else {
        /* A receiver that has stopped says so. */

        fm_receiver_lost(LS_RADIO_ERR_STOPPED);
        fm_lifecycle_task_finished();
    }
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
    /* Admission is closed and drained (fm_lifecycle_stop): the plugin's
       ACARS context goes with it. */
    ls_rx_head_detach(&s_head, &rx_acars);
}

static void fm_reset_audio(void *user)
{
    (void)user;
    if (scan_engine_decoder_handoff()) audio_out_reprime();
    else audio_out_reset();
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

static const app_t FM_APP;

void lakeshark_fm_hf_tune(uint32_t hz);

static void fm_hf_changed(void)
{
    /* The RIT is not in the word, so nudging it writes nothing. */
    settings_set_fm_hf(fm_hf_pack(fm_hf_get()));
}

static esp_err_t fm_read_saved_gain(void *ctx)
{
    *(int *)ctx = settings_get_gain(&FM_APP);
    return ESP_OK;
}

static void fm_defaults_once(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    same_options_load();
    aprs_options_load();
    ais_options_load();
    fm_hf_settings_t hf = *fm_hf_get();
    if (fm_hf_unpack(settings_get_fm_hf(0), &hf)) fm_hf_load(&hf);
    static const fm_hf_hooks_t hf_hooks = { fm_hf_changed, lakeshark_fm_hf_tune };
    fm_hf_install(&hf_hooks);
    fm_bcast_load();
    /* Voice, not pager data. A fresh install has no saved mode, so this is
       what the FM app comes up in, and coming up in POCSAG makes NFM look
       broken: the speaker carries decoder bursts instead of speech. */
    FM.mode           = FM_MODE_LISTEN;
    FM.freq_hz        = FM_DEFAULT_FREQ;
    FM.gain_tenths    = FM_DEFAULT_GAIN;
    /* 30 puts the NFM noise gate at 0.70, between a measured broadcast at
       0.28..0.59 and measured dead air at 0.81..0.97. */
    FM.squelch_tenths = 30;
    FM.tone_show = true;
    FM.pocsag_baud    = 1200;
    FM.pocsag_lock_baud = 0;
    FM.pocsag_auto    = true;
    FM.scan_start_hz  = 150000000UL;
    FM.scan_stop_hz   = 162000000UL;
    FM.scan_step_hz   = 100000UL;
    /* Come back in the mode the operator left (POCSAG on its channel), not
       NFM on NFM's frequency. The scanner and the reserved slot never restore. */
    const uint32_t saved = settings_get_fm_mode();
    if (saved < FM_MODE_COUNT && saved != 6 && saved != FM_MODE_SCAN) {
        FM.mode = (fm_mode_t)saved;
        s_mode_freq[FM.mode] = settings_get_freq_mode(&FM_APP, FM.mode, fm_mode_default_freq(FM.mode));
        FM.freq_hz = FM.mode == FM_MODE_AIS ? AIS_CENTER_HZ : FM.mode == FM_MODE_APRS ? aprs_frequency() : s_mode_freq[FM.mode];
    }
}

LS_COLD static void fm_on_enter(void)
{
    call_archive_init();
    if (fm_lifecycle_active()) return;

    for (int i = 0; i < 200 && fm_lifecycle_task_live(); i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (fm_lifecycle_task_live()) {
        ESP_LOGE(TAG, "previous fm_rx_task still alive - refusing to start a second one");
        return;
    }

    (void)fm_lifecycle_stop(&FM_LIFECYCLE_HOOKS, 0, 0);

    fm_defaults_once();

    static bool gain_loaded = false;
    if (!gain_loaded) {
        int saved_gain = FM_DEFAULT_GAIN;
        if (ls_nvs_call(fm_read_saved_gain, &saved_gain, 0) == ESP_OK) {
            FM.gain_tenths = saved_gain;
            s_gain_chosen = saved_gain != 0;
            gain_loaded = true;
        }
    }

    /* fl FM ACARS posts its submode before the GUI timer launches
       AppFM.  Starting from retained/default FM.mode here made the receiver
       configure and announce POCSAG before the worker eventually noticed the
       request.  Atomically consume the single handoff at backend entry so an
       explicit request chooses the initial mode and its saved/default
       frequency. With no request, retaining FM.mode preserves manual entry. */
    FM.mode = fm_mode_handoff_resolve_entry(&s_mode_handoff, FM.mode,
                                             FM_MODE_LISTEN);

    fm_dsp_init(&s_dsp);
    FM.tone_detected = (fm_tone_result_t){0};
    if (FM.mode == FM_MODE_WFM) fm_wfm_reset();
    /**/
    spectrum_init();
    const bool decoder_ready = fm_head_preset(FM.mode) || FM.mode == FM_MODE_SAME;

    {
        const app_t *cur = &FM_APP;   /* FM's own settings, whichever app holds the radio now */
        for (int m = 0; m < FM_MODE_COUNT; m++)
            s_mode_freq[m] = settings_get_freq_mode(cur, m, fm_mode_default_freq(m));
    }

    const app_t *cur = &FM_APP;   /* FM's own settings, whichever app holds the radio now */
    if (FM.mode != FM_MODE_SCAN)
        FM.freq_hz = settings_get_freq_mode(cur, FM.mode,
                                             fm_mode_default_freq(FM.mode));
    if (FM.mode == FM_MODE_APRS) FM.freq_hz = aprs_frequency();
    if (FM.mode == FM_MODE_AIS) FM.freq_hz = AIS_CENTER_HZ;
    fm_load_tone(FM.freq_hz);
    ls_iq_control_reset(&s_radio_control);
    /* Preserve before the session's initial configuration: POCSAG
       defaults to automatic gain until the user explicitly chooses one. */
    FM.gain_tenths = fm_requested_gain(FM.gain_tenths);
    ls_iq_control_set_initial(&s_radio_control, FM.freq_hz,
                              FM.gain_tenths, 0);
    fm_receiver_lost(LS_RADIO_ERR_UNAVAILABLE);
    if (FM.mode == FM_MODE_SCAN) scan_begin(false);
    if (FM.mode == FM_MODE_LISTEN || FM.mode == FM_MODE_WFM || FM.mode == FM_MODE_AM)
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

LS_COLD static bool fm_on_stop(void)
{
    fm_rx_plan_publish_begin();
    bool stopped = fm_lifecycle_stop(&FM_LIFECYCLE_HOOKS, 300, 10);
    if (!stopped) {
        ESP_LOGW(TAG, "drain timeout - retaining FM ownership");
        fm_rx_plan_publish_end();return false;
    }
    if (!fm_rx_worker_release(300, 10)) {
        /* task_live becomes false just before the worker callback
         * returns.  Do not hand its shared static stack to P25 until the
         * kernel confirms the wrapper is suspended and safe to delete. */
        ESP_LOGW(TAG, "worker quiesce timeout - retaining FM ownership");
        fm_rx_plan_publish_end();return false;
    }
    fm_rx_plan_publish_end();return true;
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

LS_COLD void lakeshark_fm_set_mode(int mode)
{
    if (mode < 0 || mode >= FM_MODE_COUNT || mode == 6) return;
    fm_rx_plan_publish_begin();
    /* The stored-channel scanner owns the tuner and deliberately
       forces LISTEN on every pass. Any requested non-listen mode must release
       that owner here so GUI, console and control-head selection all reach the
       same decoder. LISTEN remains composable with the scanner. */
    if (mode != FM_MODE_LISTEN && scan_engine_active()) scan_engine_stop();
    fm_mode_handoff_request(&s_mode_handoff, (fm_mode_t)mode);
    /* With no receiver open nothing takes the request until one is, and the
       screen - and a pager on the LoRa chip, which needs no receiver at all -
       would keep the old mode. It is applied here; a receiver that opens
       later takes the same request and lands in the same place. */
    if (!s_session && !s_lr_session && mode != FM_MODE_SCAN) {
        if (FM.mode != FM_MODE_SCAN) s_mode_freq[FM.mode] = FM.freq_hz;
        const uint32_t scan_tune = FM.mode == FM_MODE_SCAN ? s_scan_tune_hz : 0;
        s_scan_tune_hz = 0;
        FM.mode = (fm_mode_t)mode;
        settings_set_fm_mode((uint32_t)FM.mode);
        const uint32_t f = s_frequency_locked ? s_frequency_lock_hz : (FM.mode == FM_MODE_AIS ? AIS_CENTER_HZ : FM.mode == FM_MODE_APRS ? aprs_frequency() : scan_tune ? scan_tune : s_mode_freq[FM.mode]);
        FM.freq_hz = FM.mode == FM_MODE_AIS ? AIS_CENTER_HZ : f >= LS_HF_MIN_HZ ? f : fm_mode_default_freq(FM.mode);
    }
    fm_rx_plan_publish_end();
}
int  lakeshark_fm_get_mode(void) { return (int)FM.mode; }

LS_COLD bool lakeshark_fm_tune(int delta_hz)
{
    fm_rx_plan_publish_begin();
    if (FM.mode == FM_MODE_AIS) {fm_rx_plan_publish_end();return false;}
    /* THE LOCK IS HONOURED HERE TOO.

       This path ignored it entirely: stepping while locked moved the tuner
       and left s_frequency_lock_hz pointing at the old carrier, so the next
       mode change snapped back to a frequency the operator had already left.
       A lock that only the mode-change path respects is not a lock. */
    if (s_frequency_locked) {
        ESP_LOGW(TAG, "frequency locked at %.4f MHz - step refused",
                 s_frequency_lock_hz / 1e6);
        fm_rx_plan_publish_end();return false;
    }
    long f = (long)FM.freq_hz + delta_hz;
    if (f < (long)LS_HF_MIN_HZ) f = (long)LS_HF_MIN_HZ;
    FM.freq_hz = (uint32_t)f;
    ls_iq_control_request_tune(&s_radio_control, FM.freq_hz, false);
    if (FM.mode != FM_MODE_SCAN) s_mode_freq[FM.mode] = FM.freq_hz;
    const app_t *a = &FM_APP;   /* FM's own settings, whichever app holds the radio now */
    if (a) settings_set_freq_mode(a, FM.mode, FM.freq_hz);
    fm_rx_plan_publish_end();return true;
}
LS_COLD bool lakeshark_fm_set_freq(uint32_t hz)
{
    fm_rx_plan_publish_begin();
    if (FM.mode == FM_MODE_AIS && hz != AIS_CENTER_HZ) {fm_rx_plan_publish_end();return false;}
    if (hz < LS_HF_MIN_HZ) {fm_rx_plan_publish_end();return false;}
    /* This one used to drag the lock along to wherever it was told to go,
       which is the same as not having one. Tuning somewhere else is a
       decision to leave the locked carrier, so it asks for the lock off. */
    if (s_frequency_locked && hz != s_frequency_lock_hz) {
        ESP_LOGW(TAG, "frequency locked at %.4f MHz - tune to %.4f refused",
                 s_frequency_lock_hz / 1e6, hz / 1e6);
        fm_rx_plan_publish_end();return false;
    }
    fm_load_tone(hz);
    FM.freq_hz = hz;
    ls_iq_control_request_tune(&s_radio_control, hz, false);
    if (FM.mode != FM_MODE_SCAN) s_mode_freq[FM.mode] = hz;
    else s_scan_tune_hz = hz;
    const app_t *a = &FM_APP;   /* FM's own settings, whichever app holds the radio now */
    if (a) settings_set_freq_mode(a, FM.mode, hz);
    fm_rx_plan_publish_end();return true;
}
uint32_t lakeshark_fm_get_freq(void) { return FM.freq_hz; }

LS_COLD void lakeshark_fm_frequency_lock(bool on)
{
    fm_rx_plan_publish_begin();
    s_frequency_locked = on;
    if (on) {
        /* During a sweep FM.freq_hz is the transient tuner position.  The
           scan range centre is the stable carrier unless a manual tune has
           already queued the return to listening. Capture that explicit tune
           even while the RX worker is still applying the mode handoff. */
        s_frequency_lock_hz = FM.mode == FM_MODE_SCAN && s_scan_tune_hz
            ? s_scan_tune_hz
            : FM.mode == FM_MODE_SCAN && FM.scan_stop_hz > FM.scan_start_hz
              ? FM.scan_start_hz + (FM.scan_stop_hz - FM.scan_start_hz) / 2
              : FM.freq_hz;
    }
    fm_rx_plan_publish_end();
}

bool lakeshark_fm_frequency_locked(void) { return s_frequency_locked; }
uint32_t lakeshark_fm_frequency_lock_hz(void)
{
    return s_frequency_locked ? s_frequency_lock_hz : 0;
}

/**/
/* Retune without persisting. The scanner hops every ~120 ms, and going
   through lakeshark_fm_set_freq() for that queued an NVS write per hop:
   flash wear, a settings queue that starts dropping the user's real
   changes, and a saved FM frequency quietly replaced by whichever channel
   the sweep sampled last. The sweep is transient - it must not be sticky. */
LS_COLD void lakeshark_fm_tune_transient(uint32_t hz)
{
    fm_rx_plan_publish_begin();
    if (hz < LS_HF_MIN_HZ) {fm_rx_plan_publish_end();return;}
    fm_load_tone(hz);
    FM.freq_hz = hz;
    ls_iq_control_request_tune(&s_radio_control, hz, true);
    fm_rx_plan_publish_end();
}

LS_COLD void lakeshark_fm_gain_step(void)
{
    fm_rx_plan_publish_begin();
    static const int ladder[] = { 0, 90, 200, 300, 408, 496 };
    int n = sizeof(ladder) / sizeof(ladder[0]);
    int cur = FM.gain_tenths, idx = 0;
    for (int i = 0; i < n; i++) if (ladder[i] == cur) { idx = i; break; }
    idx = (idx + 1) % n;
    s_gain_chosen = true;   /**/
    fm_apply_gain(ladder[idx]);
    const app_t *a = &FM_APP;   /* FM's own settings, whichever app holds the radio now */
    if (a) settings_set_gain(a, ladder[idx]);
    fm_rx_plan_publish_end();
}
LS_COLD void lakeshark_fm_agc(void)
{
    fm_rx_plan_publish_begin();
    s_gain_chosen = true;   /* asking for AGC is still a choice */
    fm_apply_gain(0);
    const app_t *a = &FM_APP;   /* FM's own settings, whichever app holds the radio now */
    if (a) settings_set_gain(a, 0);
    fm_rx_plan_publish_end();
}

LS_COLD void lakeshark_fm_set_gain(int tenths)
{
    fm_rx_plan_publish_begin();
    if (tenths < 0)   tenths = 0;
    if (tenths > ls_rsel_gain_max(fm_radio_job())) tenths = ls_rsel_gain_max(fm_radio_job());
    s_gain_chosen = true;   /**/
    fm_apply_gain(tenths);
    const app_t *a = &FM_APP;   /* FM's own settings, whichever app holds the radio now */
    if (a) settings_set_gain(a, tenths);
    fm_rx_plan_publish_end();
}

LS_COLD void lakeshark_fm_set_gain_live(int tenths)
{
    fm_rx_plan_publish_begin();
    if (tenths < 0)   tenths = 0;
    if (tenths > ls_rsel_gain_max(fm_radio_job())) tenths = ls_rsel_gain_max(fm_radio_job());
    s_gain_chosen = true;   /**/
    fm_apply_gain(tenths);
    fm_rx_plan_publish_end();
}
void lakeshark_fm_gain_delta(int dt) { lakeshark_fm_set_gain(FM.gain_tenths + dt); }
int  lakeshark_fm_gain_tenths(void) { return FM.gain_tenths; }

void fm_get_receiver_status(ls_iq_control_status_t *out)
{
    ls_iq_control_status(&s_radio_control, out);
}

void fm_get_iq_observation(ls_iq_observation_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_observation_mux);
    *out = s_observation;
    portEXIT_CRITICAL(&s_observation_mux);
}

LS_COLD bool fm_mission_start(const char *name)
{
    if (!ls_iq_mission_name_valid(name)) return false;
    portENTER_CRITICAL(&s_observation_mux);
    bool needs_memory = !s_mission;
    portEXIT_CRITICAL(&s_observation_mux);
    ls_iq_mission_t *candidate = needs_memory ? heap_caps_calloc(1, sizeof(*candidate),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (needs_memory && !candidate) return false;
    portENTER_CRITICAL(&s_observation_mux);
    if (!s_mission) { s_mission = candidate; candidate = NULL; }
    bool ok = (!s_rx_plan || !s_rx_plan->status.active) && ls_iq_mission_start(s_mission, name, &s_observation,
                                   (uint64_t)esp_timer_get_time());
    portEXIT_CRITICAL(&s_observation_mux);
    if (candidate) heap_caps_free(candidate);
    return ok;
}
bool fm_mission_stop(void)
{
    portENTER_CRITICAL(&s_observation_mux);
    bool ok = ls_iq_mission_stop(s_mission, (uint64_t)esp_timer_get_time());
    portEXIT_CRITICAL(&s_observation_mux);
    return ok;
}
bool fm_mission_snapshot(ls_iq_mission_status_t *out,
                          ls_iq_mission_entry_t *entries, size_t cap, size_t *count)
{
    portENTER_CRITICAL(&s_observation_mux);
    bool ok = ls_iq_mission_snapshot(s_mission, out, entries, cap, count);
    portEXIT_CRITICAL(&s_observation_mux);
    return ok;
}
bool fm_mission_status(ls_iq_mission_status_t *out)
{ return fm_mission_snapshot(out, NULL, 0, NULL); }

static bool fm_coverage_memory(void)
{
    portENTER_CRITICAL(&s_observation_mux); bool needed=!s_coverage; portEXIT_CRITICAL(&s_observation_mux);
    ls_iq_coverage_t *candidate=needed?heap_caps_calloc(1,sizeof(*candidate),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    if(needed&&!candidate)return false;
    portENTER_CRITICAL(&s_observation_mux);
    if(!s_coverage){s_coverage=candidate;candidate=NULL;}
    portEXIT_CRITICAL(&s_observation_mux);
    if(candidate)heap_caps_free(candidate);
    return true;
}
static bool fm_plan_endpoint_ok(const ls_radio_endpoint_info_t *info)
{
    return info->present && info->leased && info->streaming && !info->paused &&
        info->configured && info->diagnostic_context_known && info->tuner_model_known &&
        !strcmp(info->endpoint_id,LS_RADIO_ENDPOINT_RTL_USB) && !strcmp(info->owner,"fm") &&
        info->actual_iq.sample_rate_hz==FM_RTL_RATE && info->last_error==LS_RADIO_OK;
}
static bool fm_plan_frequency_ok(const ls_radio_endpoint_info_t *info,uint32_t hz)
{
    for(size_t i=0;i<info->frequency_range_count && i<LS_RADIO_MAX_FREQUENCY_RANGES;++i)
        if(hz>=info->frequency_ranges[i].min_hz && hz<=info->frequency_ranges[i].max_hz)return true;
    return false;
}
bool fm_coverage_start(void)
{
    if(!fm_lifecycle_active() || FM.mode!=FM_MODE_LISTEN || !s_session || !s_session_endpoint ||
       strcmp(s_session_endpoint,LS_RADIO_ENDPOINT_RTL_USB) || !fm_coverage_memory())return false;
    portENTER_CRITICAL(&s_observation_mux);
    bool ok=(!s_rx_plan || !s_rx_plan->status.active) && ls_iq_coverage_start(s_coverage,(uint64_t)esp_timer_get_time());
    if(ok)ls_iq_coverage_bind_plan(s_coverage,0,0,0);
    portEXIT_CRITICAL(&s_observation_mux);return ok;
}
bool fm_coverage_stop(void)
{
    portENTER_CRITICAL(&s_observation_mux);
    uint64_t now=(uint64_t)esp_timer_get_time();
    ls_rx_plan_cancel(s_rx_plan,LS_RX_PLAN_MANUAL_OVERRIDE,now);
    __atomic_store_n(&s_plan_calls_suppressed,false,__ATOMIC_RELEASE);
    s_plan_coverage_owned=false;
    ls_iq_coverage_boundary(s_coverage,LS_IQ_COVERAGE_UNKNOWN,now);
    ls_iq_coverage_calls_suppressed(s_coverage,false);
    bool ok=ls_iq_coverage_stop(s_coverage,now);
    portEXIT_CRITICAL(&s_observation_mux);return ok;
}
bool fm_coverage_snapshot(ls_iq_coverage_status_t *out,ls_iq_coverage_interval_t *entries,size_t cap,size_t *count)
{
    portENTER_CRITICAL(&s_observation_mux);
    bool ok=ls_iq_coverage_snapshot(s_coverage,out,entries,cap,count);
    portEXIT_CRITICAL(&s_observation_mux);return ok;
}
bool fm_rx_plan_load(const char *name,const ls_rx_plan_step_t *steps,size_t count,uint32_t repeats)
{
    portENTER_CRITICAL(&s_observation_mux);bool needed=!s_rx_plan;portEXIT_CRITICAL(&s_observation_mux);
    ls_rx_plan_t *candidate=needed?heap_caps_calloc(1,sizeof(*candidate),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    if(needed&&!candidate)return false;
    portENTER_CRITICAL(&s_observation_mux);
    if(!s_rx_plan){s_rx_plan=candidate;candidate=NULL;}
    bool ok=!s_plan_request_pending && ls_rx_plan_load(s_rx_plan,name,steps,count,repeats);
    portEXIT_CRITICAL(&s_observation_mux);
    if(candidate)heap_caps_free(candidate);
    return ok;
}
LS_COLD bool fm_rx_plan_start(void)
{
    /* The console allocates its metadata workspace in PSRAM, not its shallow
     * internal stack. Admission never opens, steals or starts a receiver. */
    ls_radio_endpoint_info_t *info=heap_caps_calloc(1,sizeof(*info),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!info)return false;
    portENTER_CRITICAL(&s_observation_mux);uint32_t epoch=s_plan_manual_epoch;portEXIT_CRITICAL(&s_observation_mux);
    ls_iq_control_status_t control;
    ls_iq_control_status(&s_radio_control,&control);
    bool ready=ls_radio_endpoint_try_get(LS_RADIO_ENDPOINT_RTL_USB,info)==LS_RADIO_OK &&
        fm_plan_endpoint_ok(info) && control.receiver_streaming && control.effective_center_known &&
        control.tune_state==LS_IQ_RESULT_EFFECTIVE && control.gain_state!=LS_IQ_RESULT_PENDING &&
        control.requested_center_hz==control.effective_center_hz &&
        info->actual_iq.center_hz==control.effective_center_hz &&
        control.effective_center_hz<=UINT32_MAX && !call_archive_busy() &&
        fm_coverage_memory();
    portENTER_CRITICAL(&s_observation_mux);
    ready=ready && epoch==s_plan_manual_epoch && !s_plan_manual_publishers && epoch!=UINT32_MAX && s_rx_plan &&
        !s_plan_request_pending && fm_lifecycle_active() && s_session && s_session_endpoint &&
        !strcmp(s_session_endpoint,LS_RADIO_ENDPOINT_RTL_USB) && !s_lr_session &&
        FM.mode==FM_MODE_LISTEN && s_mode_handoff.pending_mode<0 && !s_frequency_locked &&
        !scan_engine_active() && (!s_coverage || !s_coverage->status.active) &&
        (!s_mission || !s_mission->status.active) &&
        fm_plan_frequency_ok(info,(uint32_t)control.effective_center_hz);
    if(ready)for(size_t i=0;i<s_rx_plan->status.step_count;++i)
        ready &= fm_plan_frequency_ok(info,s_rx_plan->status.steps[i].planned.center_hz);
    bool ok=ready && ls_rx_plan_start(s_rx_plan,(uint32_t)control.effective_center_hz,(uint64_t)esp_timer_get_time());
    if(ok) {
        s_plan_owner_epoch=epoch;s_plan_context_epoch=info->diagnostic_context_epoch;s_plan_session=s_session;
        s_plan_coverage_owned=ls_iq_coverage_start(s_coverage,(uint64_t)esp_timer_get_time());
        ls_iq_coverage_bind_plan(s_coverage,s_rx_plan->status.plan_id,UINT32_MAX,UINT32_MAX);
        ls_iq_coverage_calls_suppressed(s_coverage,true);
        __atomic_store_n(&s_plan_calls_suppressed,true,__ATOMIC_RELEASE);
    }
    portEXIT_CRITICAL(&s_observation_mux);
    heap_caps_free(info);return ok;
}
bool fm_rx_plan_stop(void)
{
    portENTER_CRITICAL(&s_observation_mux);bool ok=ls_rx_plan_stop(s_rx_plan);portEXIT_CRITICAL(&s_observation_mux);return ok;
}
bool fm_rx_plan_snapshot(ls_rx_plan_status_t *out)
{
    if(!out)return false;
    portENTER_CRITICAL(&s_observation_mux);
    bool ok=s_rx_plan && s_rx_plan->status.plan_id;
    *out=ok?s_rx_plan->status:(ls_rx_plan_status_t){0};
    portEXIT_CRITICAL(&s_observation_mux);return ok;
}
static esp_err_t fm_plan_saved_context(void *context)
{
    /* Settings keys depend on app name/defaults, not an active app or its
     * on-enter callbacks. Read them without selecting/starting FM. */
    static const app_t key={.name="FM",.default_gain=FM_DEFAULT_GAIN};
    fm_rx_plan_context_t *out=context;
    out->entry_listen_hz=settings_get_freq_mode(&key,FM_MODE_LISTEN,fm_mode_default_freq(FM_MODE_LISTEN));
    out->entry_gain_tenths_db=settings_get_gain(&key);return ESP_OK;
}
bool fm_rx_plan_context(fm_rx_plan_context_t *out)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));
    portENTER_CRITICAL(&s_observation_mux);
    out->frequency_hz=FM.freq_hz;out->mode=(int)FM.mode;out->gain_tenths_db=FM.gain_tenths;
    out->squelch_tenths=s_squelch_req>=0?s_squelch_req:FM.squelch_tenths;
    out->fm_active=fm_lifecycle_active();out->plan_active=s_rx_plan&&s_rx_plan->status.active;
    portEXIT_CRITICAL(&s_observation_mux);
    out->calls_fm_enabled=call_archive_option(CALL_OPT_FM)!=0;
    out->calls_ready=call_archive_ready();out->archive_errors=call_archive_errors();
    out->entry_values_known=ls_nvs_call(fm_plan_saved_context,out,0)==ESP_OK;
    out->calls_saved_known=call_archive_fm_saved_option(&out->calls_saved_enabled);
    out->endpoint_known=ls_radio_endpoint_try_get(LS_RADIO_ENDPOINT_RTL_USB,&out->endpoint)==LS_RADIO_OK;
    return true;
}
bool fm_rx_plan_record_fm(bool enabled)
{
    if(fm_lifecycle_active() || fm_lifecycle_task_live() || call_archive_busy())return false;
    portENTER_CRITICAL(&s_observation_mux);
    bool busy=s_rx_plan && s_rx_plan->status.active;
    portEXIT_CRITICAL(&s_observation_mux);
    if(busy)return false;
    call_archive_init();
    if(!call_archive_ready() || call_archive_busy() || fm_lifecycle_active() || fm_lifecycle_task_live())return false;
    call_archive_set_option(CALL_OPT_FM,enabled?1:0);
    return (call_archive_option(CALL_OPT_FM)!=0)==enabled;
}
static void fm_rx_plan_intervene(void)
{
    portENTER_CRITICAL(&s_observation_mux);
    if(s_plan_manual_epoch!=UINT32_MAX)++s_plan_manual_epoch;
    uint64_t now=(uint64_t)esp_timer_get_time();
    if(s_plan_request_pending && !s_plan_control_inflight) {
        uint64_t effective=0;
        if(ls_iq_control_cancel_tune(&s_radio_control,s_plan_request_generation,&effective) && effective)
            FM.freq_hz=(uint32_t)effective;
    }
    ls_rx_plan_cancel(s_rx_plan,LS_RX_PLAN_MANUAL_OVERRIDE,now);
    fm_rx_plan_end_locked(now);
    if(s_coverage && s_coverage->status.active) {
        ls_iq_coverage_boundary(s_coverage,LS_IQ_COVERAGE_UNKNOWN,now);
        ls_iq_coverage_calls_suppressed(s_coverage,false);
        (void)ls_iq_coverage_stop(s_coverage,now);
    }
    portEXIT_CRITICAL(&s_observation_mux);
}
static void fm_rx_plan_lost(void)
{
    portENTER_CRITICAL(&s_observation_mux);
    uint64_t now=(uint64_t)esp_timer_get_time();
    ls_rx_plan_cancel(s_rx_plan,LS_RX_PLAN_CONTEXT_LOST,now);fm_rx_plan_end_locked(now);
    portEXIT_CRITICAL(&s_observation_mux);
}
static void fm_rx_plan_publish_begin(void)
{
    /* Reserve the whole manual publication, including validation and queued
     * settings work. An epoch bump alone left an admission window before
     * the new control request became visible. No lock spans that work. */
    portENTER_CRITICAL(&s_observation_mux);
    if(s_plan_manual_publishers!=UINT32_MAX)++s_plan_manual_publishers;
    portEXIT_CRITICAL(&s_observation_mux);
    fm_rx_plan_intervene();
}
static void fm_rx_plan_publish_end(void)
{
    portENTER_CRITICAL(&s_observation_mux);
    if(s_plan_manual_publishers)--s_plan_manual_publishers;
    if(s_plan_manual_epoch!=UINT32_MAX)++s_plan_manual_epoch;
    portEXIT_CRITICAL(&s_observation_mux);
}
static void fm_rx_plan_poll(void)
{
    portENTER_CRITICAL(&s_observation_mux);
    bool active=s_rx_plan&&s_rx_plan->status.active;
    if(!active && s_plan_request_pending && !s_plan_control_inflight &&
       s_radio_control.tune_generation!=s_plan_request_generation)s_plan_request_pending=false;
    portEXIT_CRITICAL(&s_observation_mux);
    if(!active)return;
    bool known=ls_radio_endpoint_try_get(LS_RADIO_ENDPOINT_RTL_USB,&s_plan_endpoint)==LS_RADIO_OK;
    portENTER_CRITICAL(&s_observation_mux);
    bool owned=known && fm_plan_endpoint_ok(&s_plan_endpoint) && s_session==s_plan_session &&
        !s_plan_manual_publishers && s_plan_manual_epoch==s_plan_owner_epoch && FM.mode==FM_MODE_LISTEN &&
        s_mode_handoff.pending_mode<0 && !scan_engine_active() && !s_frequency_locked &&
        s_plan_endpoint.diagnostic_context_epoch==s_plan_context_epoch;
    uint32_t hz=0;uint64_t now=(uint64_t)esp_timer_get_time();
    ls_rx_plan_action_t action=ls_rx_plan_poll(s_rx_plan,now,owned,&hz);
    if(action!=LS_RX_PLAN_ACTION_NONE) {
        /* Only transient request fields change. NVS mode/frequency, gain,
         * bandwidth, squelch and recording options retain operator values. */
        FM.freq_hz=hz;
        ls_iq_coverage_boundary(s_coverage,LS_IQ_COVERAGE_SETTLING,now);
        ls_iq_coverage_bind_plan(s_coverage,s_rx_plan->status.plan_id,
            action==LS_RX_PLAN_ACTION_RESTORE?UINT32_MAX:s_rx_plan->status.current_step,
            s_rx_plan->status.current_repeat);
        ls_iq_control_request_tune(&s_radio_control,hz,false);
        s_plan_request_generation=s_radio_control.tune_generation;
        s_plan_request_pending=true;
    }
    fm_rx_plan_end_locked(now);portEXIT_CRITICAL(&s_observation_mux);
}
static bool fm_rx_plan_control_allowed(const ls_iq_control_request_t *request)
{
    portENTER_CRITICAL(&s_observation_mux);
    bool ours=s_plan_request_pending && request->tune_generation==s_plan_request_generation;
    if(s_plan_request_pending && !ours) {
        s_plan_request_pending=false;
        ls_rx_plan_cancel(s_rx_plan,LS_RX_PLAN_MANUAL_OVERRIDE,(uint64_t)esp_timer_get_time());
        fm_rx_plan_end_locked((uint64_t)esp_timer_get_time());
    }
    portEXIT_CRITICAL(&s_observation_mux);
    bool current=!ours || (ls_radio_endpoint_try_get(LS_RADIO_ENDPOINT_RTL_USB,&s_plan_endpoint)==LS_RADIO_OK &&
        fm_plan_endpoint_ok(&s_plan_endpoint) && s_plan_endpoint.diagnostic_context_epoch==s_plan_context_epoch);
    portENTER_CRITICAL(&s_observation_mux);
    bool allowed=!ours || (current && s_rx_plan && s_rx_plan->status.active &&
        !s_plan_manual_publishers && s_plan_owner_epoch==s_plan_manual_epoch && s_session==s_plan_session);
    if(ours&&!allowed)s_plan_request_pending=false;
    if(ours && allowed)s_plan_control_inflight=true;
    if(ours&&!current) {
        ls_rx_plan_cancel(s_rx_plan,LS_RX_PLAN_CONTEXT_LOST,(uint64_t)esp_timer_get_time());
        fm_rx_plan_end_locked((uint64_t)esp_timer_get_time());
    }
    portEXIT_CRITICAL(&s_observation_mux);return allowed;
}
static void fm_rx_plan_tune_result(const ls_iq_control_request_t *request,ls_radio_err_t error)
{
    portENTER_CRITICAL(&s_observation_mux);
    bool ours=s_plan_request_pending && request->tune_generation==s_plan_request_generation;
    portEXIT_CRITICAL(&s_observation_mux);
    if(!ours)return;
    bool known=ls_radio_endpoint_try_get(LS_RADIO_ENDPOINT_RTL_USB,&s_plan_endpoint)==LS_RADIO_OK;
    portENTER_CRITICAL(&s_observation_mux);
    s_plan_request_pending=false;
    s_plan_control_inflight=false;
    bool ok=error==LS_RADIO_OK && known && fm_plan_endpoint_ok(&s_plan_endpoint) &&
        s_plan_endpoint.actual_iq.center_hz==request->center_hz &&
        s_plan_owner_epoch==s_plan_manual_epoch && s_session==s_plan_session;
    if(ok)s_plan_context_epoch=s_plan_endpoint.diagnostic_context_epoch;
    uint64_t now=(uint64_t)esp_timer_get_time();
    if(s_rx_plan && s_rx_plan->status.state==LS_RX_PLAN_RESTORING)ls_rx_plan_restore_result(s_rx_plan,ok,now);
    else ls_rx_plan_tuned(s_rx_plan,ok,now);
    fm_rx_plan_end_locked(now);portEXIT_CRITICAL(&s_observation_mux);
}

LS_COLD void lakeshark_fm_set_baud(int baud)
{
    fm_rx_plan_publish_begin();
    /* Recorded here as well as queued for the receive loop, which resets the
       decoders when it takes it: the screen shows the new rate at once, and
       POCSAG on the LoRa chip, which runs with no receive loop, reads it from
       here. */
    if (baud == 0) FM.pocsag_auto = true;
    else { FM.pocsag_auto = false; FM.pocsag_baud = baud; }
    s_baud_req = baud;
    fm_rx_plan_publish_end();
}
int  lakeshark_fm_get_baud(void)     { return FM.pocsag_baud; }

LS_COLD void lakeshark_fm_squelch_delta(int d)
{
    fm_rx_plan_publish_begin();
    int base = (s_squelch_req >= 0) ? s_squelch_req : FM.squelch_tenths;
    int v = base + d;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_squelch_req = v;
    fm_rx_plan_publish_end();
}
LS_COLD void lakeshark_fm_set_squelch(int v)
{
    fm_rx_plan_publish_begin();
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_squelch_req = v;
    fm_rx_plan_publish_end();
}
int  lakeshark_fm_squelch_get(void)
{
    int pending = s_squelch_req;
    return (pending >= 0) ? pending : FM.squelch_tenths;
}

LS_COLD void lakeshark_fm_scan_restart(void) { fm_rx_plan_publish_begin(); s_scan_restart = true; fm_rx_plan_publish_end(); }
LS_COLD void lakeshark_fm_tune_to_peak(void)
{
    fm_rx_plan_publish_begin();
    if (FM.scan_peak_hz) {
        lakeshark_fm_set_freq(FM.scan_peak_hz);
        fm_mode_handoff_request(&s_mode_handoff, FM_MODE_LISTEN);
    }
    fm_rx_plan_publish_end();
}
uint32_t lakeshark_fm_scan_peak_hz(void) { return FM.scan_peak_hz; }

/* Go to HF on `hz`: AM mode (whose demod fm_hf.h picked), the frequency lock
   released, and the squelch open, which is how HF is listened to - the noise
   between signals is part of finding them. */
void lakeshark_fm_hf_tune(uint32_t hz)
{
    if (hz < LS_HF_MIN_HZ || hz > 1766000000u) return;
    scan_engine_stop();
    s_frequency_locked = false;
    if (FM.mode == FM_MODE_AM) {
        (void)lakeshark_fm_set_freq(hz);
        return;
    }
    s_mode_freq[FM_MODE_AM] = hz;
    settings_set_freq_mode(&FM_APP, FM_MODE_AM, hz);
    s_squelch_req = 0;
    lakeshark_fm_set_mode(FM_MODE_AM);
}
