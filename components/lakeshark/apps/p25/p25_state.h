#ifndef P25_STATE_H
#define P25_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "dsd.h"
#include "dsp_pipeline.h"
#include "iq_app_control.h"
#include "grant_follower.h"

#define RTL_SAMPLE_RATE       240000
#define RTL_DEFAULT_GAIN      200

#define P25_IQ_BLOCK_BYTES    16384

#define SCAN_BINS_MAX         60
#define SCAN_IQ_SAMPLES       4096
#define SCAN_WATERFALL_ROWS   8

typedef struct {
    uint32_t start_freq;
    uint32_t stop_freq;
    uint32_t step_hz;
    int      num_bins;
    float    power[SCAN_BINS_MAX];
    float    waterfall[SCAN_WATERFALL_ROWS][SCAN_BINS_MAX];
    int      wf_row;
    int      peak_bin;
    float    peak_power;
    uint32_t peak_freq;
    float    noise_floor;
    int      sweep_count;
    bool     scanning;
    bool     request_scan;
    bool     request_tune;
} scan_state_t;

typedef struct {
    uint32_t iq_bytes_total;
    uint32_t iq_bytes_sec;
    uint32_t audio_samples_sec;
    int      ring_fill;
    int      ring_size;
    int      read_errors;
    uint32_t read_errors_total;
    float    iq_level;

    int      dsd_sync_count;
    int      dsd_voice_count;
    int      dsd_nac;
    int      dsd_tg;
    int      dsd_src;
    char     dsd_ftype[16];
    char     dsd_fsubtype[16];
    char     dsd_modulation[8];
    char     dsd_err_str[64];
    bool     dsd_has_sync;
    bool     dsd_buffers_ok;
    int      dsd_bch_ok_count;
    int      dsd_bch_fail_count;
    int      dsd_last_ok_nac;
    char     dsd_last_ok_duid[4];

    int64_t  voice_active_until_us;

    int64_t  sync_active_until_us;

    int64_t  nac_seen_us;
    int64_t  tg_seen_us;
    int64_t  src_seen_us;

    float    demod_gain;
    bool     demod_invert;
    int      rtl_gain_tenths;

    /* LS-655: automatic demod acquisition diagnostics. The panel exposes
     * the protocol evidence so a wrong choice is distinguishable from weak
     * RF. Timing/carrier are snapshots of the active pipeline loops. */
    bool     demod_auto;
    bool     demod_locked;
    int      demod_active;
    uint32_t demod_c4fm_nids;
    uint32_t demod_c4fm_tsbks;
    uint32_t demod_cqpsk_nids;
    uint32_t demod_cqpsk_tsbks;
    uint32_t demod_reacquires;
    float    demod_timing;
    float    demod_carrier_hz;
    float    cqpsk_timing_gain;
    float    cqpsk_carrier_gain;
    bool     cqpsk_config_pending;

    float    dsd_decode_ms;
    uint32_t audio_drops;

    bool     autoscan_active;
    bool     autoscan_locked;
    int      autoscan_bch_ok;
    int      autoscan_step;

    int      input_mode;
    char     input_buf[16];
    int      input_pos;

    bool     sync_beep_enabled;

    /* LS-303: what the grant follower is currently doing. Sampled by the
     * UI so a scanner that jumps between control and traffic is visible
     * rather than silent. Populated by the DSD decoder task after each
     * processFrame cycle. */
    bool     grant_on_traffic;
    uint16_t grant_talkgroup;
    uint32_t grant_source;
    uint64_t grant_freq_hz;
    uint32_t grant_followed_count;
    bool     grant_auto_follow;
    /* LS-739: observed grant != active call != decoded PCM. GUI may show
     * unsupported observations on control, but only RX_AUDIO claims audio. */
    p25_call_info_t grant_observed;
    p25_call_info_t grant_active;
    p25_receive_state_t receive_state;
    uint32_t grant_unsupported_count;
    uint32_t grant_unresolved_count;
    uint32_t control_relock_count;
    /* LS-400: TSBK/IDEN telemetry. iden_valid_count separates "we haven't
     * seen a control channel yet" from "we have the tables and are waiting
     * for a grant"; tsbk_ok/err lives next to the BCH counts. */
    uint8_t  p25_iden_valid_count;
    uint32_t p25_tsbk_ok_count;
    uint32_t p25_tsbk_err_count;
    /* LS-672: a TDMA grant is intentionally kept on the control channel,
     * but the operator still needs proof that the site is active Phase 2.
     * These mirror the parser's last carrier/slot instead of overloading the
     * follower fields, whose frequency remains the control channel. */
    uint32_t p25_phase2_grant_count;
    uint16_t p25_phase2_last_talkgroup;
    uint64_t p25_phase2_last_frequency_hz;
    uint8_t  p25_phase2_last_slot;
    uint8_t  p25_phase2_last_slots_per_carrier;
    /* LS-610: encryption status from the last LDU2 seen on this call. The
     * decoder mutes when ess_valid && algid != 0x80; the UI must be able
     * to say "ENC ADP" so the operator can tell muted-because-encrypted
     * from muted-because-broken. Cleared alongside the decoder ESS on
     * TDU/TDULC/talkgroup change. */
    uint8_t  p25_ess_valid;
    uint8_t  p25_algid;
    uint16_t p25_kid;
    bool     p25_enc_muted;
    /* LS-611: cumulative encryption counters and the operator-visible skip
     * table. p25_enc_muted_frames_total mirrors dsd_state.p25_enc_muted_frames;
     * p25_enc_returns / p25_enc_skips mirror the grant-follower fields with the
     * same names. p25_leave_on_encrypted / p25_encrypted_skip_ms are the
     * operator settings (default: leave=on, skip=30 s). The tg_state array is
     * a copy of the follower's per-TG history at UI publish time - a small
     * table (P25_STATE_ENC_TG_MAX) so the P25 CONFIG tab can render it
     * without a laptop. */
    uint32_t p25_enc_muted_frames_total;
    uint32_t p25_enc_returns;
    uint32_t p25_enc_skips;
    uint32_t p25_enc_tg_evictions;
    bool     p25_leave_on_encrypted;
    uint32_t p25_encrypted_skip_ms;
#define P25_STATE_ENC_TG_MAX 8
    uint8_t  p25_enc_tg_count;
    struct {
        uint16_t talkgroup;
        uint8_t  algid;
        uint16_t kid;
        int32_t  skip_remaining_ms; /* negative or zero = no active skip */
    }        p25_enc_tg[P25_STATE_ENC_TG_MAX];
    /* LS-650: LDU1/TDULC LCW mirror. On a call joined mid-stream the LCW
     * is the only path to a talkgroup / source / emergency flag - the
     * TSBK grant was missed. p25_lcw_emergency in particular must be
     * loud on the panel, so the AppP25 decode tab flips the readout lamp
     * red when this is set. */
    uint8_t  p25_lcw_valid;
    uint8_t  p25_lcw_emergency;
    uint8_t  p25_lcw_encrypted;
    uint8_t  p25_lcw_priority;
    uint8_t  p25_lcw_is_unit_to_unit;
    uint8_t  p25_lcw_is_regroup;
    uint16_t p25_lcw_talkgroup;
    uint32_t p25_lcw_source;
    uint32_t p25_lcw_target;
    uint16_t p25_lcw_patch_sg;
    uint8_t  p25_lcw_alias_ready;
    char     p25_lcw_alias[32];
    uint32_t p25_lcw_ok_count;
    uint32_t p25_lcw_fec_reject_count;
} p25_state_t;

extern p25_state_t      P25;
extern scan_state_t     SCAN;
extern uint32_t         s_tune_freq_hz;
extern dsp_state_t      s_dsp;
extern dsd_opts         s_dsd_opts;
extern dsd_state        s_dsd_state;
extern dsd_sample_ring_t s_ring;

void sys_log(uint8_t color, const char *fmt, ...);
void p25_request_tune(uint32_t center_hz, bool fast);
void p25_request_gain(int gain_tenths_db);
void p25_get_receiver_status(ls_iq_control_status_t *out);
/* LS-611: operator settings for the encrypted-channel policy. Applied to
 * the running grant follower. Safe to call from another task; both are
 * scalar writes on a struct the DSD task also reads, and both settings tolerate
 * a torn update (worst case: one grant treated with the old policy). */
bool p25_set_auto_follow(bool enabled);
bool p25_get_auto_follow(void);
bool p25_set_leave_on_encrypted(bool enabled);
bool p25_set_encrypted_skip_ms(unsigned int ms);
bool p25_get_leave_on_encrypted(void);
unsigned int p25_get_encrypted_skip_ms(void);
/* LS-670: one-press hold/lockout actions the P25 main screen calls. Both
 * key on the currently-followed grant TG, falling back to the last-decoded
 * dsd_tg when the follower is on control. Return true on success. */
bool p25_ui_hold_toggle(void);
bool p25_ui_lockout_current(void);
bool p25_ui_lockout_remove(uint16_t tg);
/* LS-689: what a PROGRAM profile apply is allowed to do to the radio, and the
 * only two things it needs that were not already exposed. Both keep the tune
 * in this file's single owner: p25_return_to_control drops any call in
 * progress through the follower's own retune hook, and p25_set_control_channel
 * adopts the new control frequency and issues exactly one tune request. The
 * request latch holds one entry, so calling them in that order leaves no stale
 * retune for the outgoing system's traffic channel. */
void p25_return_to_control(void);
void p25_set_control_channel(uint64_t control_hz);
void p25_demod_set_preference(int preference);
int p25_demod_get_preference(void);
demod_mode_t p25_demod_get_active(void);
const char *p25_demod_get_name(void);
bool p25_set_cqpsk_config(const p25_cqpsk_config_t *config);
void p25_get_cqpsk_config(p25_cqpsk_config_t *config, bool *pending);
bool p25_reset_cqpsk_config(void);
bool p25_step_cqpsk_timing(int direction);
bool p25_step_cqpsk_carrier(int direction);

#endif
