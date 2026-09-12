#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdatomic.h>
#include "p25_tg_observed_frame.h"

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
#include "ls_cpu_busy.h"

#include "app_registry.h"
#include "event_bus.h"
#include "settings.h"
#include "audio_out.h"
#include "tone.h"
#include "iq_app_control.h"
#include "radio_endpoint.h"
#include "radio_decode_worker.h"

#include "p25_state.h"
#include "scanner.h"
#include "scan_engine.h"
#include "p25_iq_capture.h"
#include "scan_ctrl.h"
#include "diag.h"
#include "p25_qual.h"
#include "p25_demod_control.h"
#include "grant_follower.h"
#include "p25_receive.h"
#include "p25_tsbk.h"
#include "p25_controls.h"
#include "p25_program.h"
#include "p25_spectrum.h"
#include "p25_health.h"
#include "p25_tune_policy.h"
#include "p25_entry_settings.h"
#include "rtl-sdr.h"

p25_state_t       P25 = {0};
scan_state_t      SCAN = {0};
uint32_t          s_tune_freq_hz = 154785000UL;
dsp_state_t       s_dsp;
dsd_opts          s_dsd_opts;
/**/
EXT_RAM_BSS_ATTR dsd_state s_dsd_state;
dsd_sample_ring_t s_ring;

int autoscan_bch_ok_flag = 0;
int dsd_bch_fail_counter = 0;
volatile float    p25_rx_power   = 0.0f;
volatile bool     p25_agc_on     = false;

#define P25_STRONG_SIGNAL_GAIN 280

static const char *TAG = "p25";

static volatile bool s_app_active  = false;
static volatile bool s_rx_running  = false;
static volatile bool s_dsd_running = false;
typedef enum {
    P25_DSD_START_IDLE = 0,
    P25_DSD_START_PENDING,
    P25_DSD_START_READY,
    P25_DSD_START_FAILED,
} p25_dsd_start_t;
static atomic_int s_dsd_start = ATOMIC_VAR_INIT(P25_DSD_START_IDLE);
static uint8_t      *s_iq_buf      = NULL;
static ls_radio_session_t *s_session;
static ls_iq_control_t s_radio_control;
static p25_cqpsk_control_t s_cqpsk_control;
static uint32_t s_radio_freq_hz;
static uint32_t s_radio_sample_rate_hz;
static uint32_t s_radio_bandwidth_hz;
static p25_demod_control_t s_demod_control;
static p25_grant_follower_t s_grant_follower;
static atomic_uint s_decode_tune_generation;
static portMUX_TYPE s_acquisition_lock = portMUX_INITIALIZER_UNLOCKED;
static p25_acquisition_status_t s_acquisition_status;

void p25_get_acquisition_status(p25_acquisition_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_acquisition_lock);
    *out = s_acquisition_status;
    portEXIT_CRITICAL(&s_acquisition_lock);
}

static void p25_publish_acquisition(const p25_acquisition_gate_t *gate,
                                    uint32_t tune_state)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    portENTER_CRITICAL(&s_acquisition_lock);
    s_acquisition_status.gate = *gate;
    s_acquisition_status.hunt = s_dsd_state.acquisition_hunt;
    s_acquisition_status.tune_state = tune_state;
    s_acquisition_status.decoder_updated_ms = now_ms;
    portEXIT_CRITICAL(&s_acquisition_lock);
}
extern int dsp_has_signal_lock;

static volatile bool s_health_cpu_valid;
static volatile uint8_t s_health_cpu0;
static volatile uint8_t s_health_cpu1;
static volatile uint32_t s_health_internal_free;
static volatile uint32_t s_health_internal_largest;
static volatile uint32_t s_health_psram_free;
static EXT_RAM_BSS_ATTR p25_health_raw_t s_health_raw;

static void p25_health_publish_ui(uint32_t now_ms)
{
    /* One decoder-task writer gathers the independently measured scalars and
     * decoder-owned identity into the seqlocked LCD snapshot. The raw staging
     * object is static PSRAM BSS, not a task-stack allocation. */
    p25_health_raw_t *raw = &s_health_raw;
    memset(raw, 0, sizeof(*raw));
    raw->net_generation = s_dsd_state.p25_net_generation;
    raw->rfss_generation = s_dsd_state.p25_rfss_generation;
    raw->sccb_generation = s_dsd_state.p25_sccb_generation;
    raw->neighbor_generation = s_dsd_state.p25_neighbor_generation;
    raw->net_valid = s_dsd_state.p25_net_valid != 0;
    raw->rfss_valid = s_dsd_state.p25_rfss_valid != 0;
    raw->sccb_valid = s_dsd_state.p25_sccb_valid != 0;
    raw->wacn = s_dsd_state.p25_tsbk_wacn;
    raw->sysid = s_dsd_state.p25_tsbk_sysid;
    raw->rfss_sysid = s_dsd_state.p25_rfss_sysid;
    raw->rfss = s_dsd_state.p25_rfss_id;
    raw->site = s_dsd_state.p25_rfss_site_id;
    raw->control_channel = s_dsd_state.p25_rfss_ch_t;
    raw->control_hz = s_grant_follower.control_hz;
    raw->sccb_rfss = s_dsd_state.p25_sccb_rfss_id;
    raw->sccb_site = s_dsd_state.p25_sccb_site_id;
    raw->sccb_channel_1 = s_dsd_state.p25_sccb_ch1;
    raw->sccb_channel_2 = s_dsd_state.p25_sccb_ch2;
    raw->neighbor_count = s_dsd_state.p25_neighbor_count;
    size_t ni = 0;
    for (size_t i = 0; i < P25_NEIGHBOR_TABLE_SIZE &&
                       ni < P25_HEALTH_NEIGHBOR_MAX; ++i) {
        const p25_neighbor_entry_t *src = &s_dsd_state.p25_neighbors[i];
        if (!src->valid) continue;
        raw->neighbors[ni].rfss = src->rfss_id;
        raw->neighbors[ni].site = src->site_id;
        raw->neighbors[ni].sysid = src->sysid;
        raw->neighbors[ni].channel = src->channel;
        raw->neighbors[ni].frequency_hz = src->freq_hz;
        ni++;
    }

    raw->nid_valid = (uint32_t)P25.dsd_bch_ok_count;
    raw->nid_invalid = (uint32_t)P25.dsd_bch_fail_count;
    raw->tsbk_valid = s_dsd_state.p25_tsbk_valid_count;
    raw->tsbk_invalid = s_dsd_state.p25_tsbk_crc_errors +
                        s_dsd_state.p25_tsbk_trellis_errors;
    raw->tsbk_vendor = s_dsd_state.p25_tsbk_vendor_count;
    memcpy(raw->unhandled, s_dsd_state.p25_tsbk_unhandled,
           sizeof(raw->unhandled));

    snprintf(raw->effective_demod, sizeof(raw->effective_demod), "%s",
             p25_demod_mode_name(s_demod_control.active));
    if (s_demod_control.preference != P25_DEMOD_AUTO)
        raw->acquisition = P25_HEALTH_ACQ_MANUAL;
    else if (s_demod_control.locked)
        raw->acquisition = P25_HEALTH_ACQ_LOCKED;
    else if (s_demod_control.phase == P25_DEMOD_NO_LOCK)
        raw->acquisition = P25_HEALTH_ACQ_NO_LOCK;
    else
        raw->acquisition = P25_HEALTH_ACQ_HUNT;
    raw->c4fm_nids = s_demod_control.c4fm_nids;
    raw->c4fm_tsbks = s_demod_control.c4fm_tsbks;
    raw->cqpsk_nids = s_demod_control.cqpsk_nids;
    raw->cqpsk_tsbks = s_demod_control.cqpsk_tsbks;
    raw->reacquires = s_demod_control.reacquire_count;
    raw->timing_error_valid = s_demod_control.active == DEMOD_CQPSK;
    raw->timing_error_samples = raw->timing_error_valid
                                   ? s_dsp.g_period - (float)DSP_SPS : 0.0f;
    raw->carrier_error_valid = s_demod_control.active == DEMOD_CQPSK ||
                              s_demod_control.active == DEMOD_FSK4_TRACKING;
    raw->carrier_error_hz = P25.demod_carrier_hz;

    float level = P25.iq_level;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    raw->rf_level_permille = (uint16_t)(level * 1000.0f + 0.5f);
    raw->usb_read_errors = P25.read_errors_total;
    raw->usb_dropped_bytes = rtlsdr_stream_dropped();
    raw->ring_fill = P25.ring_fill > 0 ? (uint16_t)P25.ring_fill : 0;
    raw->ring_size = P25.ring_size > 0 ? (uint16_t)P25.ring_size : 0;
    raw->audio_drops = audio_drops_get();
    raw->audio_underruns = audio_underruns_get();
    raw->buffers_ok = P25.dsd_buffers_ok;
    raw->cpu_valid = s_health_cpu_valid;
    raw->cpu_core0_pct = s_health_cpu0;
    raw->cpu_core1_pct = s_health_cpu1;
    raw->heap_internal_free = s_health_internal_free;
    raw->heap_internal_largest = s_health_internal_largest;
    raw->heap_psram_free = s_health_psram_free;

    raw->phase2_grants = s_dsd_state.p25_phase2_grant_count;
    raw->phase2_talkgroup = s_dsd_state.p25_phase2_last_talkgroup;
    raw->phase2_frequency_hz = s_dsd_state.p25_phase2_last_frequency_hz;
    raw->phase2_slot = s_dsd_state.p25_phase2_last_slot;
    raw->phase2_slots_per_carrier =
        s_dsd_state.p25_phase2_last_slots_per_carrier;
    p25_health_publish(raw, now_ms);
}

static void p25_publish_demod(void)
{
    p25_cqpsk_config_t loops;
    bool loops_pending = false;
    p25_cqpsk_control_status(&s_cqpsk_control, NULL, &loops,
                             &loops_pending);
    P25.cqpsk_timing_gain = loops.timing_gain;
    P25.cqpsk_carrier_gain = loops.carrier_gain;
    P25.cqpsk_config_pending = loops_pending;
    P25.demod_auto = s_demod_control.preference == P25_DEMOD_AUTO;
    P25.demod_locked = s_demod_control.locked;
    P25.demod_active = (int)s_demod_control.active;
    P25.demod_c4fm_nids = s_demod_control.c4fm_nids;
    P25.demod_c4fm_tsbks = s_demod_control.c4fm_tsbks;
    P25.demod_cqpsk_nids = s_demod_control.cqpsk_nids;
    P25.demod_cqpsk_tsbks = s_demod_control.cqpsk_tsbks;
    P25.demod_reacquires = s_demod_control.reacquire_count;
    P25.demod_timing = s_demod_control.active == DEMOD_CQPSK
                           ? s_dsp.g_period : (float)DSP_SPS;
    P25.demod_carrier_hz = s_demod_control.active == DEMOD_CQPSK
        ? s_dsp.cqpsk_afc_phase_err * (float)DSP_BAUD / (2.0f * (float)M_PI)
        : s_demod_control.active == DEMOD_FSK4_TRACKING
              ? (float)dsp_fsk4_get_fine_freq_hz(&s_dsp) : 0.0f;
}

bool p25_set_cqpsk_config(const p25_cqpsk_config_t *config)
{
    if (!p25_cqpsk_config_valid(config)) return false;
    bool saved = settings_set_p25_cqpsk(config);
    bool queued = p25_cqpsk_control_request(&s_cqpsk_control, config);
    if (!saved) sys_log(4, "CQPSK loops changed but preference was not saved");
    return queued && saved;
}

void p25_get_cqpsk_config(p25_cqpsk_config_t *config, bool *pending)
{
    p25_cqpsk_control_status(&s_cqpsk_control, NULL, config, pending);
}

bool p25_reset_cqpsk_config(void)
{
    p25_cqpsk_config_t tested;
    p25_cqpsk_config_defaults(&tested);
    return p25_set_cqpsk_config(&tested);
}

static float step_loop_gain(float value, float minimum, float maximum,
                            int direction)
{
    float next = direction < 0 ? value * 0.5f : value * 2.0f;
    if (next < minimum) next = minimum;
    if (next > maximum) next = maximum;
    return next;
}

bool p25_step_cqpsk_timing(int direction)
{
    p25_cqpsk_config_t config;
    p25_cqpsk_control_status(&s_cqpsk_control, &config, NULL, NULL);
    config.timing_gain = step_loop_gain(config.timing_gain,
                                        P25_CQPSK_TIMING_GAIN_MIN,
                                        P25_CQPSK_TIMING_GAIN_MAX,
                                        direction);
    return p25_set_cqpsk_config(&config);
}

bool p25_step_cqpsk_carrier(int direction)
{
    p25_cqpsk_config_t config;
    p25_cqpsk_control_status(&s_cqpsk_control, &config, NULL, NULL);
    config.carrier_gain = step_loop_gain(config.carrier_gain,
                                         P25_CQPSK_CARRIER_GAIN_MIN,
                                         P25_CQPSK_CARRIER_GAIN_MAX,
                                         direction);
    return p25_set_cqpsk_config(&config);
}

static void p25_apply_cqpsk_request(void)
{
    p25_cqpsk_config_t config;
    uint32_t generation = 0;
    if (!p25_cqpsk_control_take(&s_cqpsk_control, &config, &generation))
        return;
    if (!dsp_set_cqpsk_loops(&s_dsp, &config)) return;
    p25_cqpsk_control_applied(&s_cqpsk_control, &config, generation);
    P25.cqpsk_timing_gain = config.timing_gain;
    P25.cqpsk_carrier_gain = config.carrier_gain;
    if (s_demod_control.active == DEMOD_CQPSK) {
        p25_iq_capture_interrupt(P25_IQ_CAPTURE_CONFIG_CHANGED);
        s_ring.read_idx = s_ring.write_idx;
        atomic_fetch_add_explicit(&s_decode_tune_generation, 1,
                                  memory_order_release);
        P25.dsd_has_sync = false;
        P25.sync_active_until_us = 0;
        dsp_has_signal_lock = 0;
    }
    sys_log(1, "CQPSK loops: timing %.8f carrier %.4f",
            (double)config.timing_gain, (double)config.carrier_gain);
}

static void p25_apply_demod_mode(demod_mode_t mode)
{
    P25.demod_gain = p25_demod_output_gain(mode, P25.demod_invert);
    if (!dsp_select_mode(&s_dsp, mode, P25.demod_gain)) {
        p25_publish_demod();
        return;
    }
    p25_iq_capture_interrupt(P25_IQ_CAPTURE_CONFIG_CHANGED);
    s_ring.read_idx = s_ring.write_idx;
    /* A decoder already inside sync/NID must discard the crossed result and
     * reset itself before consuming a differently sliced sample stream. */
    atomic_fetch_add_explicit(&s_decode_tune_generation, 1,
                              memory_order_release);
    P25.dsd_has_sync = false;
    P25.sync_active_until_us = 0;
    dsp_has_signal_lock = 0;
    p25_publish_demod();
}

void p25_demod_set_preference(int preference)
{
    int normalized = p25_demod_preference_normalize(preference);
    p25_demod_control_init(&s_demod_control, normalized,
                           (uint32_t)(esp_timer_get_time() / 1000LL),
                           (uint32_t)P25.dsd_bch_ok_count,
                           P25.p25_tsbk_ok_count);
    settings_set_p25_demod(normalized);
    p25_apply_demod_mode(s_demod_control.active);
    sys_log(1, "DSP mode: %s", p25_demod_control_name(&s_demod_control));
}

int p25_demod_get_preference(void) { return s_demod_control.preference; }
demod_mode_t p25_demod_get_active(void) { return s_demod_control.active; }
const char *p25_demod_get_name(void)
{
    return p25_demod_control_name(&s_demod_control);
}

/* grant follower. Retune requests go through the same iq_app_control
 * queue as a manual tune - the p25_rx_task drains it and calls
 * ls_radio_iq_retune. From the follower's side it is fire and forget. */
static void p25_grant_retune_cb(void *user, uint64_t center_hz, bool to_traffic)
{
    (void)user;
    if (center_hz == 0 || center_hz > UINT32_MAX) return;

    if (to_traffic)
        (void)p25_program_survey_cancel_now(
            P25_SURVEY_CANCEL_FOLLOWING_CALL);
    p25_request_tune((uint32_t)center_hz, /* fast=*/to_traffic);
    sys_log(1, "Grant %s %.4f MHz tg=%u",
            to_traffic ? "->traffic" : "->control",
            (double)center_hz / 1e6,
            (unsigned)s_grant_follower.talkgroup);
}

static void p25_grant_publish_ui(void)
{
    P25.grant_observed = s_grant_follower.observed_grant;
    P25.grant_active = s_grant_follower.active_call;
    P25.receive_state = s_grant_follower.receive_state;
    P25.grant_unsupported_count = s_grant_follower.unsupported_grants;
    P25.grant_unresolved_count = s_grant_follower.unresolved_grants;
    P25.control_relock_count = s_grant_follower.control_relocks;
    P25.grant_on_traffic     = (s_grant_follower.state == P25_GRANT_ON_TRAFFIC);
    P25.grant_talkgroup      = s_grant_follower.talkgroup;
    P25.grant_source         = s_grant_follower.source;
    P25.grant_freq_hz        = P25.grant_on_traffic
                                   ? s_grant_follower.traffic_hz
                                   : s_grant_follower.control_hz;
    P25.grant_followed_count = s_grant_follower.followed_count;
    P25.grant_auto_follow    = p25_scan_get_auto_follow(&g_p25_scan);

    /* sample TSBK/IDEN telemetry off dsd_state. The IDEN table is
     * 16 slots so this loop is a handful of loads. */
    uint8_t iden = 0;
    for (int i = 0; i < P25_IDEN_TABLE_SIZE; i++)
        if (s_dsd_state.p25_iden_table[i].valid) iden++;
    P25.p25_iden_valid_count = iden;
    P25.p25_tsbk_ok_count    = s_dsd_state.p25_tsbk_valid_count;
    P25.p25_tsbk_err_count   = s_dsd_state.p25_tsbk_crc_errors +
                                s_dsd_state.p25_tsbk_trellis_errors;
    P25.p25_phase2_grant_count = s_dsd_state.p25_phase2_grant_count;
    P25.p25_phase2_last_talkgroup =
        s_dsd_state.p25_phase2_last_talkgroup;
    P25.p25_phase2_last_frequency_hz =
        s_dsd_state.p25_phase2_last_frequency_hz;
    P25.p25_phase2_last_slot = s_dsd_state.p25_phase2_last_slot;
    P25.p25_phase2_last_slots_per_carrier =
        s_dsd_state.p25_phase2_last_slots_per_carrier;

    /* mirror the ESS. p25_enc_muted uses the same predicate as
     * process_IMBE so the UI label matches what the audio path actually
     * does with the frame. */
    P25.p25_ess_valid = s_dsd_state.p25_ess_valid;
    P25.p25_algid    = s_dsd_state.p25_algid;
    P25.p25_kid      = s_dsd_state.p25_kid;
    P25.p25_enc_muted = p25_ldu_should_mute_encrypted(&s_dsd_state, &s_dsd_opts) != 0;

    /* publish encryption counters and the per-TG history. The
     * follower is authoritative for encrypted_returns / encrypted_skips and
     * the TG table; process_IMBE increments p25_enc_muted_frames on the
     * decoder state. */
    P25.p25_enc_muted_frames_total = s_dsd_state.p25_enc_muted_frames;
    P25.p25_enc_returns            = s_grant_follower.encrypted_returns;
    P25.p25_enc_skips              = s_grant_follower.encrypted_skips;
    P25.p25_enc_tg_evictions       = s_grant_follower.tg_state_evictions;
    P25.p25_leave_on_encrypted     = s_grant_follower.leave_on_encrypted;
    P25.p25_encrypted_skip_ms      =
        (uint32_t)(s_grant_follower.encrypted_skip_us / 1000LL);

    int64_t now_pub = esp_timer_get_time();
    size_t src_n = p25_grant_tg_state_count(&s_grant_follower);
    size_t dst_n = src_n > P25_STATE_ENC_TG_MAX
                       ? P25_STATE_ENC_TG_MAX : src_n;
    P25.p25_enc_tg_count = (uint8_t)dst_n;
    for (size_t i = 0; i < dst_n; i++) {
        const p25_grant_tg_state_t *e =
            p25_grant_tg_state(&s_grant_follower, i);
        P25.p25_enc_tg[i].talkgroup = e ? e->talkgroup : 0;
        P25.p25_enc_tg[i].algid     = e ? e->algid     : 0;
        P25.p25_enc_tg[i].kid       = e ? e->kid       : 0;
        int32_t rem = 0;
        if (e && e->skip_expires_us > now_pub)
            rem = (int32_t)((e->skip_expires_us - now_pub) / 1000LL);
        P25.p25_enc_tg[i].skip_remaining_ms = rem;
    }

    /* mirror the LCW so the panel can label a late-entry call.
     * Emergency in particular has to be impossible to miss - the decode
     * tab flips the readout lamp on this. */
    P25.p25_lcw_valid           = s_dsd_state.p25_lcw_valid;
    P25.p25_lcw_emergency       = s_dsd_state.p25_lcw_emergency;
    P25.p25_lcw_encrypted       = s_dsd_state.p25_lcw_encrypted;
    P25.p25_lcw_priority        = s_dsd_state.p25_lcw_priority;
    P25.p25_lcw_is_unit_to_unit = s_dsd_state.p25_lcw_is_unit_to_unit;
    P25.p25_lcw_is_regroup      = s_dsd_state.p25_lcw_is_regroup;
    P25.p25_lcw_talkgroup       = s_dsd_state.p25_lcw_talkgroup;
    P25.p25_lcw_source          = s_dsd_state.p25_lcw_source;
    P25.p25_lcw_target          = s_dsd_state.p25_lcw_target;
    P25.p25_lcw_patch_sg        = s_dsd_state.p25_lcw_patch_sg;
    P25.p25_lcw_alias_ready     = s_dsd_state.p25_lcw_alias_ready;
    if (s_dsd_state.p25_lcw_alias_ready) {
        memcpy(P25.p25_lcw_alias, s_dsd_state.p25_lcw_alias,
               sizeof(P25.p25_lcw_alias));
        P25.p25_lcw_alias[sizeof(P25.p25_lcw_alias) - 1] = 0;
    } else {
        P25.p25_lcw_alias[0] = 0;
    }
    P25.p25_lcw_ok_count         = s_dsd_state.p25_lcw_ok_count;
    P25.p25_lcw_fec_reject_count = s_dsd_state.p25_lcw_fec_reject_count;
}

#define P25_READ_TIMEOUT_MS 20

void p25_request_tune(uint32_t center_hz, bool fast)
{
    ls_iq_control_request_tune(&s_radio_control, center_hz, fast);
    atomic_fetch_add_explicit(&s_decode_tune_generation, 1, memory_order_release);
}

void p25_request_gain(int gain_tenths_db)
{
    /* The setting moves when it is asked to, not when the receiver gets round to applying it. */

    if (gain_tenths_db < 0)   gain_tenths_db = 0;
    if (gain_tenths_db > 496) gain_tenths_db = 496;
    P25.rtl_gain_tenths = gain_tenths_db;
    ls_iq_control_request_gain(&s_radio_control, gain_tenths_db);
}

void p25_get_receiver_status(ls_iq_control_status_t *out)
{
    ls_iq_control_status(&s_radio_control, out);
}

bool p25_set_auto_follow(bool enabled)
{
    bool returned = p25_scan_set_auto_follow(&g_p25_scan, &s_grant_follower,
                                             enabled);
    bool saved = settings_set_p25_auto_follow(enabled);
    if (returned) sys_log(1, "Auto follow off: returning to control");
    if (!saved) sys_log(4, "Auto follow changed but preference was not saved");
    return saved;
}

bool p25_get_auto_follow(void)
{
    return p25_scan_get_auto_follow(&g_p25_scan);
}

bool p25_set_leave_on_encrypted(bool enabled)
{
    p25_grant_set_leave_on_encrypted(&s_grant_follower, enabled);
    bool saved = settings_set_p25_skip_encrypted(enabled);
    if (!saved) sys_log(4, "Encrypted-skip changed but preference was not saved");
    return saved;
}

bool p25_set_encrypted_skip_ms(unsigned int ms)
{
    uint32_t bounded = p25_controls_clamp_encrypted_skip_ms(ms);
    p25_grant_set_encrypted_skip_ms(&s_grant_follower, bounded);
    bool saved = settings_set_p25_encrypted_skip_ms(bounded);
    if (!saved) sys_log(4, "Encrypted-skip duration changed but preference was not saved");
    return saved;
}

bool p25_get_leave_on_encrypted(void)
{
    return s_grant_follower.leave_on_encrypted;
}

unsigned int p25_get_encrypted_skip_ms(void)
{
    return (unsigned int)(s_grant_follower.encrypted_skip_us / 1000LL);
}

/* the two operations a PROGRAM profile apply needs from the decoder
 * owner. Both go through the follower and the same tune latch every other
 * retune uses, so the profile does not become a second thing that tunes. */
void p25_return_to_control(void)
{
    (void)p25_grant_force_return_to_control(&s_grant_follower);
}

void p25_set_control_channel(uint64_t control_hz)
{
    if (control_hz == 0 || control_hz > UINT32_MAX) return;
    /* selecting/applying a profile control is distinct from carrier
     * scan and takes the tuner from it explicitly. This also covers PROGRAM
     * next/previous and reload, not only the SURVEY button. */
    scan_engine_stop();
    /* Adopt first: if a grant lands between these two lines the follower has
     * to already know which frequency it is expected to come back to. */
    p25_grant_set_control(&s_grant_follower, control_hz);
    p25_request_tune((uint32_t)control_hz, /* fast=*/false);
    /* The latch holds one request, so this supersedes anything the outgoing
     * profile's follower had queued. */
    sys_log(1, "Control channel: %.4f MHz", (double)control_hz / 1e6);
}

/* one-press UI actions. Each names its target explicitly - the
 * currently-followed grant TG, else the last-seen TG on decode. Both
 * fall back to zero (nothing to act on) so a stray button press with no
 * signal does not corrupt state. */
static uint16_t p25_ui_current_tg(void)
{
    if (s_grant_follower.state == P25_GRANT_ON_TRAFFIC &&
        s_grant_follower.talkgroup) {
        return s_grant_follower.talkgroup;
    }
    return (uint16_t)P25.dsd_tg;
}

bool p25_ui_hold_toggle(void)
{
    uint16_t tg = p25_ui_current_tg();
    if (p25_scan_hold_active(&g_p25_scan)) {
        p25_scan_hold_set(&g_p25_scan, 0);
        sys_log(1, "Hold cleared");
        p25_scan_persist_save_now();
        return false;
    }
    if (tg == 0) return false;
    p25_scan_hold_set(&g_p25_scan, tg);
    sys_log(1, "Hold TG %u", (unsigned)tg);
    p25_scan_persist_save_now();
    return true;
}

bool p25_ui_lockout_current(void)
{
    uint16_t tg = p25_ui_current_tg();
    if (tg == 0) return false;
    bool ok = p25_scan_lockout_add(&g_p25_scan, tg);
    if (ok) {

        (void)p25_grant_force_return_to_control(&s_grant_follower);
        sys_log(1, "Lockout TG %u", (unsigned)tg);
        p25_scan_persist_save_now();
    }
    return ok;
}

bool p25_ui_lockout_remove(uint16_t tg)
{
    bool ok = p25_scan_lockout_remove(&g_p25_scan, tg);
    if (ok) p25_scan_persist_save_now();
    return ok;
}

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
    /* Task 655: the DSD mod_* booleans are NOT a mirror of dsp_pipeline's
     * demod_mode_t. They only pick between three internal slicer variants
     * inside dsd_frame_sync.c (rf_mod == 0 adaptive min/max, rf_mod == 1
     * same slicer with a QPSK sync-pattern label, rf_mod == 2 fixed
     * maxref/minref for GFSK). Every mode in dsp_pipeline hands DSD a
     * 4-level signed audio stream shaped like C4FM to a slicer - the
     * differential-atan2 paths all rescale to the same ±(3,1,-1,-3)
     * cluster geometry. So the correct slicer for all four DSP modes is
     * rf_mod == 0 (C4FM), and pinning the mod_* booleans to enable only
     * that is deliberate, not an oversight. If a future DSP mode emits
     * something that a fixed-slice GFSK path would decode better, that
     * mapping goes here. */
    p25_demod_dsd_flags(s_demod_control.active,
                        &s_dsd_opts.mod_c4fm,
                        &s_dsd_opts.mod_qpsk,
                        &s_dsd_opts.mod_gfsk);
    s_dsd_opts.mod_threshold = 26;
    s_dsd_opts.ssize = 36;

    s_dsd_opts.msize = 256;
    s_dsd_opts.use_cosine_filter = 1;
    /* do not force this - it kept the encryption gate wide open, so
     * ALGID from LDU2 could never mute a call. process_IMBE now consults
     * state->p25_algid; opts->unmute_encrypted_p25 remains as an escape
     * hatch, defaulted OFF in initOpts(). */
    s_dsd_opts.unmute_encrypted_p25 = 0;

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
        atomic_store_explicit(&s_dsd_start, P25_DSD_START_FAILED,
                              memory_order_release);
        esp_task_wdt_delete(NULL);
        return;
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

    /**/
    static EXT_RAM_BSS_ATTR int16_t pcm_buf[2000];
    s_dsd_state.pcm_out_buf = pcm_buf;
    s_dsd_state.pcm_out_size = 2000;
    s_dsd_state.pcm_out_write = 0;
    sys_log(1, "DSD decoder running heap=%lu", (unsigned long)esp_get_free_heap_size());

    s_dsd_running = true;
    atomic_store_explicit(&s_dsd_start, P25_DSD_START_READY,
                          memory_order_release);
    int64_t s_tel_us = 0;

    /* Per-call decode quality, ported from headless-p25-wifi6. A call
      opens on first sync and closes 1.5 s after the last one; the deltas over
      that window are what p25_qual stores. Locals, not statics: one decoder
      task, and they must reset when it restarts.*/
    bool     q_active = false;
    int64_t  q_t0 = 0, q_last_sync_us = 0;
    int      q_bch_ok0 = 0, q_bch_fail0 = 0, q_voice0 = 0;
    uint32_t q_under0 = 0, q_drop0 = 0;
    int      q_nac = 0;
    p25_acquisition_gate_t acquisition = {0};
    acquisition.generation = atomic_load_explicit(
        &s_decode_tune_generation, memory_order_acquire);
    uint64_t decode_control_hz = s_grant_follower.control_hz;
    while (s_app_active) {
        esp_task_wdt_reset();
        diag_emit_periodic();

        P25.dsd_bch_ok_count   = autoscan_bch_ok_flag;
        P25.dsd_bch_fail_count = dsd_bch_fail_counter;

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

        /* a retune can interrupt a frame on the other core. Only
         * this task resets decoder state; generation checks discard crossed
         * frames before grants/ESS/PCM can affect the new call. */
        unsigned int tune_generation = atomic_load_explicit(
            &s_decode_tune_generation, memory_order_acquire);
        if (tune_generation != acquisition.generation) {
            P25.voice_active_until_us = 0;
            P25.dsd_tg = P25.dsd_src = 0;
        }
        if (decode_control_hz != s_grant_follower.control_hz) {
            p25_tsbk_reset_system(&s_dsd_state);
            decode_control_hz = s_grant_follower.control_hz;
        }
        ls_iq_control_status_t tune_status;
        ls_iq_control_status(&s_radio_control, &tune_status);
        bool acquisition_ready = p25_acquisition_prepare(&acquisition,
            &s_dsd_opts, &s_dsd_state, tune_generation,
            tune_status.tune_state == LS_IQ_RESULT_PENDING,
            tune_status.tune_state == LS_IQ_RESULT_FAILED);
        p25_publish_acquisition(&acquisition, tune_status.tune_state);
        if (!acquisition_ready) {
            if (tune_status.tune_state == LS_IQ_RESULT_FAILED) {
                bool returned = p25_grant_force_return_to_control(&s_grant_follower);
                if (returned) p25_receive_call_reset(&s_dsd_state);
                s_grant_follower.receive_state = P25_RX_TUNE_FAILED;
            }
            p25_grant_publish_ui();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int sync = getFrameSync(&s_dsd_opts, &s_dsd_state);
        bool same_tune = p25_acquisition_accept_sync(&acquisition,
            atomic_load_explicit(&s_decode_tune_generation, memory_order_acquire), sync);
        p25_publish_acquisition(&acquisition, tune_status.tune_state);
        if (!same_tune) continue;
        if (sync >= 0) {
            P25.dsd_sync_count++;
            P25.dsd_has_sync = true;

            P25.sync_active_until_us = esp_timer_get_time() + 500000LL;
            extern int dsp_has_signal_lock;
            dsp_has_signal_lock = 1;

            if (s_dsd_state.nac != 0) {
                P25.dsd_nac = s_dsd_state.nac;
                P25.nac_seen_us = esp_timer_get_time();
            }
            if (s_dsd_state.lasttg != 0) {
                P25.dsd_tg = s_dsd_state.lasttg;
                P25.tg_seen_us = esp_timer_get_time();
            }
            if (s_dsd_state.lastsrc != 0) {
                P25.dsd_src = s_dsd_state.lastsrc;
                P25.src_seen_us = esp_timer_get_time();
            }
            snprintf(P25.dsd_ftype, sizeof(P25.dsd_ftype), "%s", s_dsd_state.ftype);
            if (s_dsd_state.rf_mod == 0) strcpy(P25.dsd_modulation, "C4FM");
            else if (s_dsd_state.rf_mod == 1) strcpy(P25.dsd_modulation, "QPSK");
            else strcpy(P25.dsd_modulation, "GFSK");

            esp_task_wdt_reset();
            s_dsd_state.pcm_out_write = 0;

            int64_t pf_t0 = esp_timer_get_time();
            /* sample the validated-LCW counter before decoding so the
               observer can tell a talkgroup this frame validated from one left
               over in state by an earlier frame. */
            uint32_t observed_lcw_before = s_dsd_state.p25_lcw_ok_count;
            processFrame(&s_dsd_opts, &s_dsd_state);
            same_tune = p25_acquisition_accept_frame(&acquisition,
                atomic_load_explicit(&s_decode_tune_generation, memory_order_acquire));
            p25_publish_acquisition(&acquisition, tune_status.tune_state);
            if (!same_tune) continue;
            /* only after the acquisition epoch accepts the frame, and
               with tune_status captured BEFORE decoding - a retune that landed
               mid-frame must not relabel the previous channel's observations. */
            p25_tg_observed_frame(&s_dsd_state, observed_lcw_before,
                tune_status.effective_center_known ? tune_status.effective_center_hz : 0,
                (uint32_t)(esp_timer_get_time() / 1000LL));
            float pf_ms = (float)(esp_timer_get_time() - pf_t0) / 1000.0f;
            P25.dsd_decode_ms = P25.dsd_decode_ms * 0.9f + pf_ms * 0.1f;

            esp_task_wdt_reset();

            if (s_dsd_state.lasttg != 0) {
                P25.dsd_tg = s_dsd_state.lasttg;
                P25.tg_seen_us = esp_timer_get_time();
            }
            if (s_dsd_state.lastsrc != 0) {
                P25.dsd_src = s_dsd_state.lastsrc;
                P25.src_seen_us = esp_timer_get_time();
            }
            snprintf(P25.dsd_fsubtype, sizeof(P25.dsd_fsubtype), "%s", s_dsd_state.fsubtype);
            snprintf(P25.dsd_err_str, sizeof(P25.dsd_err_str), "%s", s_dsd_state.err_str);

            if (s_dsd_state.nac != 0) P25.dsd_last_ok_nac = s_dsd_state.nac;

            {
                int64_t q_now = esp_timer_get_time();
                if (!q_active) {
                    q_active    = true;
                    q_t0        = q_now;
                    q_bch_ok0   = P25.dsd_bch_ok_count;
                    q_bch_fail0 = P25.dsd_bch_fail_count;
                    q_voice0    = P25.dsd_voice_count;
                    q_under0    = audio_underruns_get();
                    q_drop0     = audio_drops_get();
                }
                q_last_sync_us = q_now;
                q_nac = P25.dsd_nac;
            }

            int64_t now_grant = esp_timer_get_time();
            (void)p25_receive_frame(&g_p25_scan, &s_grant_follower,
                &s_dsd_state, now_grant, p25_tune_policy_allows_grant(
                    scan_engine_active(), p25_program_survey_active_now()));

            if (s_dsd_state.pcm_out_write > 0) {
                p25_grant_on_voice(&s_grant_follower, now_grant);
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
            s_dsd_state.p25_frame_valid = 0;
            p25_grant_on_frame(&s_grant_follower, &s_dsd_state,
                               esp_timer_get_time());
            if (esp_timer_get_time() >= P25.sync_active_until_us) {
                P25.dsd_has_sync = false;
            }
            extern int dsp_has_signal_lock;
            dsp_has_signal_lock = 0;
        }

        if (q_active) {
            int64_t q_now = esp_timer_get_time();
            if (q_now - q_last_sync_us > 1500000LL) {
                int dok   = P25.dsd_bch_ok_count   - q_bch_ok0;   if (dok   < 0) dok   = 0;
                int dfail = P25.dsd_bch_fail_count - q_bch_fail0; if (dfail < 0) dfail = 0;
                int dvox  = P25.dsd_voice_count    - q_voice0;    if (dvox  < 0) dvox  = 0;
                uint32_t dund = audio_underruns_get() - q_under0;
                uint32_t ddrp = audio_drops_get()     - q_drop0;
                int tot   = dok + dfail;
                int okpct = tot > 0 ? (dok * 100) / tot : 0;
                double dur = (double)(q_now - q_t0) / 1e6 - 1.5;
                if (dur < 0) dur = 0;
                p25_qual_rec_t qr = {
                    .nac = (uint32_t)q_nac, .dur_s = (float)dur,
                    .bch_ok = dok, .bch_fail = dfail, .ok_pct = okpct,
                    .vox = dvox, .under = dund, .drop = ddrp,
                    .dec_ms = (int)(P25.dsd_decode_ms + 0.5f),
                };
                p25_qual_push(&qr);
                /* ESP_LOGW, not sys_log: sys_log publishes EVT_LOG to
                   the event bus and NOTHING in the headless build prints that
                   to the console, so the line would be invisible on UART.
                   ESP_LOGW is what headless-p25-wifi6 used, and "P25QUAL" is
                   not in LOG_QUIET_TAGS, so it shows without a `log` command. */
                ESP_LOGW("P25QUAL",
                    "nac=%03X dur=%.1fs bchOK=%d bchFAIL=%d ok%%=%d vox=%d under=%u drop=%u dec=%dms",
                    q_nac, dur, dok, dfail, okpct, dvox,
                    (unsigned)dund, (unsigned)ddrp, qr.dec_ms);
                q_active = false;
            }
        }

        /* silence timeout runs even when sync is lost - the call
         * ended by squelch or fade, not by a decoded TDU. */
        if (p25_grant_tick(&s_grant_follower, esp_timer_get_time()))
            p25_receive_call_reset(&s_dsd_state);
        p25_grant_publish_ui();
        p25_health_publish_ui((uint32_t)(esp_timer_get_time() / 1000LL));
        (void)p25_program_survey_poll_now(
            (uint32_t)(esp_timer_get_time() / 1000LL),
            (uint32_t)P25.dsd_bch_ok_count, P25.p25_tsbk_ok_count);
    }
    esp_task_wdt_delete(NULL);
    s_dsd_running = false;
    atomic_store_explicit(&s_dsd_start, P25_DSD_START_IDLE,
                          memory_order_release);
}

#define P25_RADIO_OPEN_NO_MEMORY_ATTEMPTS 3

static ls_radio_err_t p25_radio_open(void)
{
    const ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .min_hz = P25_CONTROL_TUNER_MIN_HZ,
        .max_hz = P25_CONTROL_TUNER_MAX_HZ,
        .sample_rate_hz = RTL_SAMPLE_RATE,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
    };
    ls_radio_err_t error = ls_radio_acquire("p25", &requirements, &s_session);
    if (error != LS_RADIO_OK) {
        ls_iq_control_receiver_lost(&s_radio_control, error);
        return error;
    }

    const ls_radio_iq_config_t requested = {
        .center_hz = s_tune_freq_hz,
        .sample_rate_hz = RTL_SAMPLE_RATE,
        .bandwidth_hz = 0,
        .gain_mode = P25.rtl_gain_tenths == 0 ? LS_RADIO_GAIN_AUTO
                                              : LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = P25.rtl_gain_tenths,
    };
    ls_radio_iq_config_t actual;
    error = ls_iq_control_configure(&s_radio_control, s_session, &requested,
                                    &actual);
    if (error == LS_RADIO_OK) error = ls_radio_iq_start(s_session);
    if (error != LS_RADIO_OK) {
        sys_log(4, "radio open failed: %s", ls_radio_err_name(error));
        ls_radio_release(s_session);
        s_session = NULL;
        ls_iq_control_receiver_lost(&s_radio_control, error);
        return error;
    }
    ls_iq_control_set_streaming(&s_radio_control, true, LS_RADIO_OK);
    s_radio_freq_hz = actual.center_hz;
    s_radio_sample_rate_hz = actual.sample_rate_hz;
    s_radio_bandwidth_hz = actual.bandwidth_hz;
    sys_log(1, "Radio: %.4f MHz %lukSPS", actual.center_hz / 1e6,
            (unsigned long)(actual.sample_rate_hz / 1000));
    return LS_RADIO_OK;
}

static void p25_rx_task(void *arg)
{
    (void)arg;

    dsp_init(&s_dsp);
    {
        p25_cqpsk_config_t config;
        p25_cqpsk_control_status(&s_cqpsk_control, &config, NULL, NULL);
        (void)dsp_set_cqpsk_loops(&s_dsp, &config);
        p25_cqpsk_control_applied(&s_cqpsk_control, &config, 0);
        P25.cqpsk_timing_gain = config.timing_gain;
        P25.cqpsk_carrier_gain = config.carrier_gain;
    }

    int stored = settings_get_p25_demod();
    p25_demod_control_init(&s_demod_control, stored,
                           (uint32_t)(esp_timer_get_time() / 1000LL),
                           (uint32_t)P25.dsd_bch_ok_count,
                           P25.p25_tsbk_ok_count);
    demod_mode_t start_mode = s_demod_control.active;
    dsp_set_mode(&s_dsp, start_mode);
    P25.demod_gain = p25_demod_output_gain(start_mode, P25.demod_invert);
    dsp_set_gain(&s_dsp, P25.demod_gain);
    sys_log(1, "DSP mode: %s", p25_demod_control_name(&s_demod_control));
    memset(&s_ring, 0, sizeof(s_ring));
    P25.ring_size = DSD_SAMPLE_RING_SIZE;

    s_iq_buf = heap_caps_malloc(P25_IQ_BLOCK_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_iq_buf) s_iq_buf = malloc(P25_IQ_BLOCK_BYTES);
    if (!s_iq_buf) {
        sys_log(4, "OOM rx buffer");
        s_rx_running = false;

        s_app_active = false;
        vTaskDelete(NULL);
        return;
    }
    sys_log(1, "RX buf OK %d bytes heap=%lu",
            P25_IQ_BLOCK_BYTES, (unsigned long)esp_get_free_heap_size());

    /**/
    static EXT_RAM_BSS_ATTR int16_t audio_buf[8192];

    ESP_LOGW("P25DBG", "pre-decode-task: free_int=%u largest_int=%u "
                        "free_dma=%u largest_dma=%u free_psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    for (int i = 0; i < 200 && s_dsd_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_dsd_running) { ESP_LOGE("P25DBG", "prev dsd_decode still alive; skipping"); }
    dsd_abort = 0;
    P25.dsd_buffers_ok = false;
    atomic_store_explicit(&s_dsd_start, P25_DSD_START_PENDING,
                          memory_order_release);
    bool decoder_created = !s_dsd_running && ls_radio_decode_worker_start(
        dsd_decoder_task, "dsd_decode", 10, 0);
    if (!decoder_created) {
        atomic_store_explicit(&s_dsd_start, P25_DSD_START_FAILED,
                              memory_order_release);
    }
    ESP_LOGW("P25DBG", "dsd_decode create=%s stack=%u shared; "
                        "free_int=%u largest_int=%u free_dma=%u largest_dma=%u",
             decoder_created ? "OK" : "FAILED",
             (unsigned)LS_RADIO_DECODE_STACK_BYTES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    for (int i = 0;
         i < 200 && atomic_load_explicit(&s_dsd_start,
                                         memory_order_acquire) ==
                        P25_DSD_START_PENDING;
         ++i)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (atomic_load_explicit(&s_dsd_start, memory_order_acquire) !=
        P25_DSD_START_READY) {
        /* the old path logged a NULL decoder handle and then marked
         * RX running anyway.  Make task/decoder allocation failure visible
         * through receiver status and leave no IQ-only zombie behind. */
        s_app_active = false;
        dsd_abort = 1;
        ls_iq_control_receiver_lost(&s_radio_control,
                                    LS_RADIO_ERR_NO_MEMORY);
        sys_log(4, "P25 decoder task unavailable - not receiving");
        (void)ls_radio_decode_worker_release(200, 10);
        heap_caps_free(s_iq_buf);
        s_iq_buf = NULL;
        s_rx_running = false;
        vTaskDelete(NULL);
        return;
    }
    sys_log(1, "IQ read loop starting");

    int read_errors = 0;
    unsigned no_memory_open_attempts = 0;
    ls_radio_err_t terminal_radio_error = LS_RADIO_OK;
    uint32_t iq_bucket = 0, audio_bucket = 0;
    int64_t stats_ts = esp_timer_get_time();
    int64_t last_yield = stats_ts;

    while (s_app_active) {
        /* UI and profile workers only publish commands.  The RX owner drains
         * this latch here, between complete input blocks. */
        p25_apply_cqpsk_request();
        if (!s_session) {
            ls_radio_err_t open_error = p25_radio_open();
            if (open_error != LS_RADIO_OK) {
                if (open_error == LS_RADIO_ERR_NO_MEMORY &&
                    ++no_memory_open_attempts >=
                        P25_RADIO_OPEN_NO_MEMORY_ATTEMPTS) {
                    /* a permanent cold-entry allocation failure used
                     * to retune/reconfigure/log every ~405 ms forever.  Bound
                     * this app entry; leaving and reentering creates a fresh
                     * attempt after other modes have released their owners. */
                    terminal_radio_error = open_error;
                    sys_log(4, "radio start stopped after %u no-memory attempts; reenter P25 to retry",
                            no_memory_open_attempts);
                    s_app_active = false;
                    break;
                }
                if (open_error != LS_RADIO_ERR_NO_MEMORY)
                    no_memory_open_attempts = 0;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            no_memory_open_attempts = 0;
            p25_iq_capture_receiver(true);
        }
        if (SCAN.request_scan) {
            p25_iq_capture_interrupt(P25_IQ_CAPTURE_TUNE_CHANGED);
            SCAN.request_scan = false;
            uint32_t restored_hz = scanner_run_sweep(s_session,
                                                       &s_app_active);
            if (restored_hz) {
                s_radio_freq_hz = restored_hz;
                p25_spectrum_invalidate();
            }
        }
        if (SCAN.request_tune && SCAN.peak_freq > 0) {
            p25_iq_capture_interrupt(P25_IQ_CAPTURE_TUNE_CHANGED);
            SCAN.request_tune = false;
            s_tune_freq_hz = SCAN.peak_freq;
            uint64_t actual_hz = 0;
            ls_radio_err_t error = ls_radio_iq_retune(
                s_session, s_tune_freq_hz, false, &actual_hz);
            if (error != LS_RADIO_OK)
                sys_log(4, "Tune failed: %s", ls_radio_err_name(error));
            else
                s_radio_freq_hz = (uint32_t)actual_hz;
            p25_spectrum_invalidate();
            vTaskDelay(pdMS_TO_TICKS(10));
            sys_log(1, "Tuned: %.4f MHz", s_tune_freq_hz / 1e6);
        }

        ls_iq_control_request_t radio_request = {0};
        if (ls_iq_control_take(&s_radio_control, &radio_request) &&
            (radio_request.flags & LS_IQ_CONTROL_TUNE)) {
            uint32_t f = radio_request.center_hz;
            /**/
            if (f != s_radio_freq_hz) {
                p25_iq_capture_interrupt(P25_IQ_CAPTURE_TUNE_CHANGED);
                s_tune_freq_hz = f;
                ls_radio_err_t error = ls_iq_control_apply_tune(
                    &s_radio_control, s_session, &radio_request);
                ls_iq_control_status_t radio_status;
                ls_iq_control_status(&s_radio_control, &radio_status);
                if (error != LS_RADIO_OK)
                    sys_log(4, "Tune failed: %s", ls_radio_err_name(error));
                else if (radio_status.effective_center_known)
                    s_radio_freq_hz =
                        (uint32_t)radio_status.effective_center_hz;
                p25_spectrum_invalidate();
                s_ring.write_idx = s_ring.read_idx;
                atomic_fetch_add_explicit(&s_decode_tune_generation, 1,
                                          memory_order_release);
                P25.dsd_has_sync = false;
                P25.sync_active_until_us = 0;
                /* if the follower is on control, a retune request
                 * either came from the user (adopt as new control) or came
                 * from the follower's own return-to-control (target is
                 * already control_hz, so this is a no-op). If the follower
                 * is on traffic, this retune is the follower moving to a
                 * grant, so we must not touch control_hz. */
                if (s_grant_follower.state == P25_GRANT_ON_CONTROL)
                    p25_grant_set_control(&s_grant_follower, (uint64_t)f);
                sys_log(1, "Tuned: %.4f MHz", f / 1e6);
            } else {
                /* suppressing a redundant hardware retune must still
                 * complete the request. Leaving it PENDING made the carrier
                 * scanner remain in STARTING even though the endpoint was
                 * already at the requested frequency. */
                ls_iq_control_note_tune_result(
                    &s_radio_control, f, LS_RADIO_OK, s_radio_freq_hz);
            }
        }

        if ((radio_request.flags & LS_IQ_CONTROL_GAIN) != 0) {
            p25_iq_capture_interrupt(P25_IQ_CAPTURE_CONFIG_CHANGED);
            int gain = radio_request.gain_tenths_db;
            p25_agc_on = false;
            P25.rtl_gain_tenths = gain;
            ls_radio_err_t error = ls_iq_control_apply_gain(
                &s_radio_control, s_session, &radio_request);
            if (error != LS_RADIO_OK)
                sys_log(4, "Gain failed: %s", ls_radio_err_name(error));
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
        int got = 0;
        while (full && got < P25_IQ_BLOCK_BYTES) {
            if (!s_app_active || !s_session) { full = false; break; }
            size_t part = 0;
            ls_radio_err_t error = ls_radio_iq_read(
                s_session, &s_iq_buf[got], P25_IQ_BLOCK_BYTES - got,
                P25_READ_TIMEOUT_MS, &part);
            if (error == LS_RADIO_OK) { got += (int)part; continue; }
            if (error == LS_RADIO_ERR_TIMEOUT) continue;
            p25_iq_capture_interrupt(P25_IQ_CAPTURE_GAP);
            P25.read_errors_total++;
            if (++read_errors > 50 || error == LS_RADIO_ERR_DISCONNECTED) {
                full = false;
                read_errors = 0;
                if (error == LS_RADIO_ERR_DISCONNECTED) {
                    p25_iq_capture_receiver(false);
                    ls_iq_control_receiver_lost(&s_radio_control, error);
                    ls_radio_release(s_session);
                    s_session = NULL;
                    s_radio_freq_hz = 0;
                }
            }
        }

        if (full && got >= P25_IQ_BLOCK_BYTES) {
            read_errors = 0;
            iq_bucket += P25_IQ_BLOCK_BYTES;

            if (p25_iq_capture_collecting()) {
                ls_iq_control_status_t actual;
                ls_iq_control_status(&s_radio_control, &actual);
                p25_iq_capture_meta_t meta = {
                    .frequency_hz = s_radio_freq_hz,
                    .sample_rate_hz = s_radio_sample_rate_hz,
                    .bandwidth_hz = s_radio_bandwidth_hz,
                    .tune_generation = atomic_load_explicit(
                        &s_decode_tune_generation, memory_order_acquire),
                    .gain_tenths = actual.effective_gain_known ?
                        actual.effective_gain_tenths_db : INT32_MIN,
                    .demod_mode = s_dsp.mode,
                    .demod_gain = s_dsp.demod_gain,
                    .now_ms = (uint32_t)(esp_timer_get_time() / 1000LL),
                };
                p25_iq_capture_feed(s_iq_buf, P25_IQ_BLOCK_BYTES, &meta);
            }

            /**/
            if (scan_engine_active()) {
                uint32_t sum = 0;
                uint32_t cnt = 0;
                for (int i = 0; i + 1 < P25_IQ_BLOCK_BYTES; i += 8) {
                    int di = (int)s_iq_buf[i]     - 128;
                    int dq = (int)s_iq_buf[i + 1] - 128;
                    int a = di < 0 ? -di : di;
                    int b = dq < 0 ? -dq : dq;
                    sum += (uint32_t)(a + b);
                    cnt++;
                }
                p25_rx_power = cnt ? ((float)sum / (float)cnt) / 254.0f : 0.0f;
            }

            {
                int peak_fast = 0;
                for (int i = 0; i + 1 < P25_IQ_BLOCK_BYTES; i += 8) {
                    int di = (int)s_iq_buf[i]     - 128;
                    int dq = (int)s_iq_buf[i + 1] - 128;
                    int a = di < 0 ? -di : di;
                    int b = dq < 0 ? -dq : dq;
                    if (a > peak_fast) peak_fast = a;
                    if (b > peak_fast) peak_fast = b;
                }
                float lvl = (float)peak_fast / 127.5f;

                P25.iq_level = P25.iq_level * 0.6f + lvl * 0.4f;
            }

            {
                static int64_t last_iqr_us = 0;
                int64_t tnow_iqr = esp_timer_get_time();
                if (tnow_iqr - last_iqr_us > 1000000LL) {
                    last_iqr_us = tnow_iqr;
                    p25_iq_probe_t probe;
                    p25_acquisition_probe_iq(&probe, s_iq_buf, P25_IQ_BLOCK_BYTES);
                    portENTER_CRITICAL(&s_acquisition_lock);
                    s_acquisition_status.iq = probe;
                    s_acquisition_status.iq_updated_ms = (uint32_t)(tnow_iqr / 1000LL);
                    portEXIT_CRITICAL(&s_acquisition_lock);
#if DIAG_UART_ENABLE
                    uint32_t sum_i = 0, sum_q = 0;
                    uint64_t sumsq = 0;
                    uint8_t  umin = 255, umax = 0;
                    int      peak_dev = 0;
                    const int half_n = P25_IQ_BLOCK_BYTES / 2;
                    for (int i = 0; i < P25_IQ_BLOCK_BYTES; i += 2) {
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

                    int rms_mmm  = (int)(rms_norm  * 1000.0f + 0.5f);
                    int peak_mmm = (int)(peak_norm * 1000.0f + 0.5f);
                    diag_line("IQR",
                        "umin=%u umax=%u peak=%d.%03d rms=%d.%03d "
                        "mean_i=%+.2f mean_q=%+.2f n=%d",
                        (unsigned)umin, (unsigned)umax,
                        peak_mmm / 1000, peak_mmm % 1000,
                        rms_mmm  / 1000, rms_mmm  % 1000,
                        (double)mean_i, (double)mean_q, half_n);
#endif
                }
            }

            /* fold a bounded snapshot from this already-owned IQ
             * block.  The producer performs one FFT per eight blocks only
             * while SIGNAL is visible; it accepts traffic-channel samples
             * without starting the tuner sweep that would steal a call. */
            (void)p25_spectrum_feed_iq(
                P25_SPECTRUM_OWNER_RX, s_iq_buf, P25_IQ_BLOCK_BYTES,
                s_radio_freq_hz,
                s_radio_sample_rate_hz ? s_radio_sample_rate_hz
                                       : RTL_SAMPLE_RATE,
                s_radio_bandwidth_hz,
                s_grant_follower.state == P25_GRANT_ON_TRAFFIC,
                (uint32_t)(esp_timer_get_time() / 1000LL));

            int na = dsp_process_iq(&s_dsp, s_iq_buf, P25_IQ_BLOCK_BYTES, audio_buf, 8192);
            audio_bucket += na;

            uint32_t ring_drops = 0;
            for (int i = 0; i < na; i++) {
                int next = (s_ring.write_idx + 1) % DSD_SAMPLE_RING_SIZE;
                if (next != s_ring.read_idx) {
                    s_ring.buf[s_ring.write_idx] = audio_buf[i];
                    s_ring.write_idx = next;
                } else ring_drops++;
            }
            if (ring_drops) {
                portENTER_CRITICAL(&s_acquisition_lock);
                s_acquisition_status.ring_drops += ring_drops;
                portEXIT_CRITICAL(&s_acquisition_lock);
            }
        }

        P25.read_errors = read_errors;
        P25.ring_fill = dsd_ring_available(&s_ring);

        int64_t now = esp_timer_get_time();
        /* TSDU (3) is the control channel we are comparing, not a call.
         * Treating every nonzero lastp25type as a call holds C4FM forever as
         * soon as its first valid control frame lands. */
        bool in_call = p25_demod_call_active(
                           s_dsd_state.lastp25type,
                           now < P25.voice_active_until_us);
        if (p25_demod_control_feed(&s_demod_control,
                                   full && got >= P25_IQ_BLOCK_BYTES
                                       ? P25_IQ_BLOCK_BYTES / 2 : 0,
                                   s_radio_sample_rate_hz,
                                   (uint32_t)P25.dsd_bch_ok_count,
                                   P25.p25_tsbk_ok_count,
                                   in_call)) {

            p25_apply_demod_mode(s_demod_control.active);
            sys_log(1,
                    "AUTO %s C4FM NID=%lu TSBK=%lu score=%d; "
                    "CQPSK NID=%lu TSBK=%lu score=%d",
                    p25_demod_control_name(&s_demod_control),
                    (unsigned long)s_demod_control.c4fm_nids,
                    (unsigned long)s_demod_control.c4fm_tsbks,
                    s_demod_control.c4fm_score,
                    (unsigned long)s_demod_control.cqpsk_nids,
                    (unsigned long)s_demod_control.cqpsk_tsbks,
                    s_demod_control.cqpsk_score);
        }
        p25_publish_demod();
        if (now - stats_ts > 1000000LL) {
            P25.iq_bytes_sec = iq_bucket;
            P25.audio_samples_sec = audio_bucket;
            P25.iq_bytes_total += iq_bucket;
            iq_bucket = 0; audio_bucket = 0; stats_ts = now;
            int cpu0 = 0, cpu1 = 0;
            if (ls_cpu_busy(&cpu0, &cpu1)) {
                s_health_cpu_valid = true;
                s_health_cpu0 = (uint8_t)cpu0;
                s_health_cpu1 = (uint8_t)cpu1;
            }
            s_health_internal_free =
                (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            s_health_internal_largest =
                (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            s_health_psram_free =
                (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
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

    p25_iq_capture_receiver(false);
    if (s_session) {
        (void)ls_radio_iq_stop(s_session);
    }
    ls_iq_control_receiver_lost(
        &s_radio_control, terminal_radio_error == LS_RADIO_OK
                              ? LS_RADIO_ERR_STOPPED
                              : terminal_radio_error);

    for (int i = 0; i < 300 && s_dsd_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    heap_caps_free(s_iq_buf);
    s_iq_buf = NULL;
    s_rx_running = false;
    vTaskDelete(NULL);
}

static void p25_on_enter(void)
{
    portENTER_CRITICAL(&s_acquisition_lock);
    memset(&s_acquisition_status, 0, sizeof(s_acquisition_status));
    portEXIT_CRITICAL(&s_acquisition_lock);
    if (s_app_active) return;

    if (s_rx_running || s_dsd_running) {
        for (int i = 0; i < 200 && (s_rx_running || s_dsd_running); i++)
            vTaskDelay(pdMS_TO_TICKS(10));
        if (s_rx_running || s_dsd_running) {
            sys_log(4, "P25 enter refused: previous tasks still running "
                       "(rx=%d dsd=%d)", s_rx_running, s_dsd_running);
            return;
        }
    }
    if (s_session) {
        ls_radio_release(s_session);
        s_session = NULL;
        s_radio_freq_hz = 0;
    }

    const app_t *cur = app_current();
    p25_entry_settings_t entry_settings;
    esp_err_t entry_settings_err = p25_entry_settings_load(
        cur, s_tune_freq_hz, &entry_settings);
    if (entry_settings_err != ESP_OK) {
        sys_log(4, "P25 settings unavailable (%d); using defaults",
                (int)entry_settings_err);
    }

    diag_init();
    p25_health_init();
    s_health_cpu_valid = false;
    s_health_cpu0 = 0;
    s_health_cpu1 = 0;
    s_health_internal_free =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_health_internal_largest =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s_health_psram_free =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    P25.dsd_has_sync       = false;
    P25.dsd_nac            = 0;
    P25.dsd_tg             = 0;
    P25.dsd_src            = 0;
    P25.nac_seen_us        = 0;
    P25.tg_seen_us         = 0;
    P25.src_seen_us        = 0;
    P25.dsd_ftype[0]       = 0;
    P25.dsd_fsubtype[0]    = 0;
    P25.dsd_err_str[0]     = 0;
    P25.dsd_modulation[0]  = 0;
    P25.iq_level           = 0;
    P25.read_errors        = 0;
    P25.read_errors_total  = 0;
    P25.ring_fill          = 0;
    P25.iq_bytes_sec       = 0;
    P25.audio_samples_sec  = 0;
    P25.voice_active_until_us = 0;
    P25.demod_auto         = true;
    P25.demod_locked       = false;
    P25.demod_active       = DEMOD_C4FM;
    P25.demod_c4fm_nids    = 0;
    P25.demod_c4fm_tsbks   = 0;
    P25.demod_cqpsk_nids   = 0;
    P25.demod_cqpsk_tsbks  = 0;
    P25.demod_reacquires   = 0;
    P25.demod_timing       = 0.0f;
    P25.demod_carrier_hz   = 0.0f;
    {
        p25_cqpsk_config_t config = entry_settings.cqpsk;
        p25_cqpsk_control_init(&s_cqpsk_control, &config);
        P25.cqpsk_timing_gain = config.timing_gain;
        P25.cqpsk_carrier_gain = config.carrier_gain;
        P25.cqpsk_config_pending = false;
    }

    P25.demod_gain = p25_demod_output_gain(DEMOD_C4FM, P25.demod_invert);

    {
        int saved = entry_settings.gain_tenths;

        P25.rtl_gain_tenths = (saved >= 0) ? saved : P25_STRONG_SIGNAL_GAIN;
    }
    P25.sync_beep_enabled  = false;

    scanner_init();
    p25_spectrum_init();
    ls_iq_control_reset(&s_radio_control);
    audio_out_reset();

    uint32_t freq = entry_settings.freq_hz;
    if (freq) s_tune_freq_hz = freq;

    {
        const p25_program_t *program = p25_program_session();
        uint64_t control = p25_program_selected_control_hz(program);
        if (control > 0 && control <= UINT32_MAX)
            s_tune_freq_hz = (uint32_t)control;
    }
    ls_iq_control_set_initial(&s_radio_control, s_tune_freq_hz,
                              P25.rtl_gain_tenths, 0);
    ls_iq_control_receiver_lost(&s_radio_control,
                                LS_RADIO_ERR_UNAVAILABLE);

    /* init the grant follower against whatever the current tune is
     * treated as the control channel. If the user retunes, the follower's
     * control_hz is refreshed in p25_rx_task when it processes the request. */
    p25_grant_init(&s_grant_follower, (uint64_t)s_tune_freq_hz,
                   p25_grant_retune_cb, NULL);
    /* scan controller singleton is init once on app enter. The
     * persisted lockouts/allow list/hold are then reloaded on top of the
     * empty state by p25_scan_persist_reload(); a failing load leaves the
     * defaults in place. */
    p25_scan_init(&g_p25_scan);
    p25_scan_persist_reload();
    /* restore receiver controls after the scan blob, because loading
     * that blob reinitialises the controller. This only mutates policy; an OFF
     * auto-follow preference cannot tune from app entry, and a later grant is
     * still routed through scan_ctrl's normal precedence. */
    p25_scan_restore_controls(&g_p25_scan, &s_grant_follower,
                              entry_settings.auto_follow,
                              entry_settings.skip_encrypted,
                              entry_settings.encrypted_skip_ms);
    /* and the profile outranks those persisted preferences in turn,
     * for the same reason. Reapply reads nothing from SD - what is on the card
     * may have changed, and re-entering an app is not a reload. */
    (void)p25_program_reapply_now();
    memset(&P25.grant_observed, 0, sizeof(P25.grant_observed));
    memset(&P25.grant_active, 0, sizeof(P25.grant_active));
    P25.receive_state = P25_RX_CONTROL_SEARCH;
    P25.grant_unsupported_count = P25.grant_unresolved_count = 0;
    P25.control_relock_count = 0;
    P25.grant_on_traffic     = false;
    P25.grant_talkgroup      = 0;
    P25.grant_source         = 0;
    P25.grant_freq_hz        = s_tune_freq_hz;
    P25.grant_followed_count = 0;
    P25.grant_auto_follow    = p25_scan_get_auto_follow(&g_p25_scan);
    P25.p25_iden_valid_count = 0;
    P25.p25_tsbk_ok_count    = 0;
    P25.p25_tsbk_err_count   = 0;
    /* */
    P25.p25_ess_valid = 0;
    P25.p25_algid    = 0;
    P25.p25_kid      = 0;
    P25.p25_enc_muted = false;
    /* */
    P25.p25_enc_muted_frames_total = 0;
    P25.p25_enc_returns            = 0;
    P25.p25_enc_skips              = 0;
    P25.p25_enc_tg_evictions       = 0;
    P25.p25_leave_on_encrypted     = s_grant_follower.leave_on_encrypted;
    P25.p25_encrypted_skip_ms      =
        (uint32_t)(s_grant_follower.encrypted_skip_us / 1000LL);
    P25.p25_enc_tg_count           = 0;
    memset(P25.p25_enc_tg, 0, sizeof(P25.p25_enc_tg));
    /* */
    P25.p25_lcw_valid           = 0;
    P25.p25_lcw_emergency       = 0;
    P25.p25_lcw_encrypted       = 0;
    P25.p25_lcw_priority        = 0;
    P25.p25_lcw_is_unit_to_unit = 0;
    P25.p25_lcw_is_regroup      = 0;
    P25.p25_lcw_talkgroup       = 0;
    P25.p25_lcw_source          = 0;
    P25.p25_lcw_target          = 0;
    P25.p25_lcw_patch_sg        = 0;
    P25.p25_lcw_alias_ready     = 0;
    P25.p25_lcw_alias[0]        = 0;
    P25.p25_lcw_ok_count        = 0;
    P25.p25_lcw_fec_reject_count = 0;

    s_app_active = true;
    /* Reserve the PSRAM RX owner before creation, closing the same
     * create-to-task-entry window that FM's checked lifecycle closes. */
    s_rx_running = true;
    TaskHandle_t rx_task = xTaskCreateStaticPinnedToCore(
        p25_rx_task, "p25_rx", P25RX_STACK_WORDS, NULL, 10,
        s_p25rx_stack, &s_p25rx_tcb, 1);
    if (!rx_task) {

        s_app_active = false;
        s_rx_running = false;
        ls_iq_control_receiver_lost(&s_radio_control,
                                    LS_RADIO_ERR_NO_MEMORY);
        sys_log(4, "P25 RX task could not be created");
    }
}

static bool p25_on_stop(void)
{
    (void)p25_program_survey_cancel_now(P25_SURVEY_CANCEL_APP_EXIT);
    s_app_active = false;
    p25_iq_capture_receiver(false);

    dsd_abort = 1;

    for (int i = 0; i < 300 && (s_rx_running || s_dsd_running); i++)
        vTaskDelay(pdMS_TO_TICKS(10));

    if (s_rx_running || s_dsd_running) {
        ESP_LOGW(TAG, "drain timeout: rx=%d dsd=%d",
                 s_rx_running, s_dsd_running);
        return false;
    }
    if (!ls_radio_decode_worker_release(300, 10)) {
        ESP_LOGW(TAG, "decoder worker quiesce timeout - retaining P25 ownership");
        return false;
    }
    if (s_session) {
        ls_radio_release(s_session);
        s_session = NULL;
        s_radio_freq_hz = 0;
    }
    return true;
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
    .banner       = "CONV VOICE",
    .signal_label = "DEMOD",
    .on_enter     = p25_on_enter,
    .on_stop      = p25_on_stop,
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
