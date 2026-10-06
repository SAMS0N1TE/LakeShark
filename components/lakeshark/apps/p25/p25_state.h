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
    /* The LoRa socket's LR2021 is the receiver: Phase 1 only, and no IQ, so
       no spectrum. Its RSSI is what iq_level shows then. */
    bool     lora_rx;
    float    lora_rssi_dbm;
    /* The LR2021 is the receiver but something else is using it now. */
    bool     lora_wait;

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

    /* automatic demod acquisition diagnostics. The panel exposes
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

    bool     grant_on_traffic;
    uint16_t grant_talkgroup;
    uint32_t grant_source;
    uint64_t grant_freq_hz;
    uint32_t grant_followed_count;
    bool     grant_auto_follow;
    /* observed grant != active call != decoded PCM. GUI may show
     * unsupported observations on control, but only RX_AUDIO claims audio. */
    p25_call_info_t grant_observed;
    p25_call_info_t grant_active;
    p25_receive_state_t receive_state;
    uint32_t grant_unsupported_count;
    uint32_t grant_unresolved_count;
    uint32_t control_relock_count;
    /* TSBK/IDEN telemetry. iden_valid_count separates "we haven't
     * seen a control channel yet" from "we have the tables and are waiting
     * for a grant"; tsbk_ok/err lives next to the BCH counts. */
    uint8_t  p25_iden_valid_count;
    uint32_t p25_tsbk_ok_count;
    uint32_t p25_tsbk_err_count;

    uint32_t p25_phase2_grant_count;
    uint16_t p25_phase2_last_talkgroup;
    uint64_t p25_phase2_last_frequency_hz;
    uint8_t  p25_phase2_last_slot;
    uint8_t  p25_phase2_last_slots_per_carrier;

    uint8_t  p25_ess_valid;
    uint8_t  p25_algid;
    uint16_t p25_kid;
    bool     p25_enc_muted;

    uint32_t p25_enc_muted_frames_total;
    uint32_t p25_enc_muted_unknown_total;
    uint32_t p25_ess_rs_failed_total;
    uint32_t p25_ess_rs_kept_total;
    /* IMBE frames decoded before the call proved clear (p25_voice_hold.h):
       held, then released once proven or discarded unplayed. */
    uint32_t p25_voice_held_total;
    uint32_t p25_voice_released_total;
    uint32_t p25_voice_discarded_total;
    /* Frames whose NID passed BCH but were not corroborated, so did not
       count as signal (p25_sync_confirm.h). Mostly noise. */
    uint32_t sync_unconfirmed_total;
    uint16_t sync_unconfirmed_nac;
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
    /* LDU1/TDULC LCW mirror. On a call joined mid-stream the LCW
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

bool p25_set_auto_follow(bool enabled);
bool p25_get_auto_follow(void);
bool p25_set_leave_on_encrypted(bool enabled);
bool p25_set_encrypted_skip_ms(unsigned int ms);
bool p25_get_leave_on_encrypted(void);
unsigned int p25_get_encrypted_skip_ms(void);
void p25_set_phase2_follow(bool enabled);
bool p25_get_phase2_follow(void);
void p25_p2_follow_describe(char *text, unsigned capacity);
/* one-press hold/lockout actions the P25 main screen calls. Both
 * key on the currently-followed grant TG, falling back to the last-decoded
 * dsd_tg when the follower is on control. Return true on success. */
bool p25_ui_hold_toggle(void);
bool p25_ui_lockout_current(void);
bool p25_ui_lockout_remove(uint16_t tg);

/* The LR2021 receive: its settings, applied at once (the session restarts),
   and a raw capture of the bits it hands over, for experiments. The capture
   is a ring counted from its start, so a stretch of it can be read out while
   it runs on. At a bitrate other than the decoder's own the bits are only
   captured, not decoded. */
typedef struct {
    uint32_t bitrate, deviation_hz, bandwidth_hz;
    uint8_t  rx_boost_step, pulse_shape;          /* as in ls_fsk_cfg_t */
    uint8_t  rx_gain_step;                        /* 0 AGC, 1..13 fixed */
    int16_t  freq_trim_hz;                        /* added to the tuned frequency, -5000..5000; 0 none */
} p25_lr_cfg_t;
/* Where a session for a channel at hz listens with these settings. */
static inline uint32_t p25_lr_tuned_hz(uint32_t hz, const p25_lr_cfg_t *cfg)
{
    return (uint32_t)((int32_t)hz + cfg->freq_trim_hz);
}
typedef struct {
    bool on;
    uint32_t bytes, packets;      /* since the capture began */
    uint32_t oldest;              /* the oldest byte still held */
    uint32_t freq_hz;
    p25_lr_cfg_t cfg;             /* the settings now */
} p25_lr_capture_info_t;
void p25_lr_get_cfg(p25_lr_cfg_t *out);
void p25_lr_set_cfg(const p25_lr_cfg_t *cfg);
/* The receive gain the P25 screen offers: 0 the chip's AGC, 1..13 a held step
   (13 the most). Choosing one saves it and restarts the receive; call it from
   a task whose stack is in internal RAM. */
uint8_t p25_lr_gain_step(void);
void p25_lr_choose_gain(uint8_t step);
/* the symbol decoder's learned lookup (p25_os4_decode.h) on or off */
void p25_lr_set_lut(bool on);
void p25_lr_set_nn(bool on);
bool p25_lr_nn(void);
bool p25_lr_lut(void);
/* For comparing two receive settings on the same calls: with an alternate set,
   the receive takes each in turn for a whole packet (~3.4 s). The alternate's
   packets are decoded with the middle-three rule and hard FEC, as the lookup
   is learned for the main setting; the capture says which packet had which. */
void p25_lr_set_alt(const p25_lr_cfg_t *alt);     /* NULL: off */
bool p25_lr_get_alt(p25_lr_cfg_t *out);
void p25_lr_capture_start(void);
void p25_lr_capture_stop(void);
void p25_lr_capture_info(p25_lr_capture_info_t *out);
/* How the symbols kept up on their way to the frame decoder, since boot. */
typedef struct {
    uint32_t q_max;          /* most symbols ever waiting for the decoder's ring */
    uint32_t dropped;        /* symbols turned away by a full queue             */
    uint32_t chunk_us_max;   /* the longest one read of bits took to decode     */
    uint32_t us_per_kb;      /* decode time per 1024 bytes of bits, on average  */
    uint32_t iter_us_max;    /* the longest pass of the receive loop            */
    uint32_t lookup_reads;   /* reads decoded by the lookup, the reader behind  */
    uint32_t reads;          /* reads decoded                                   */
} p25_lr_flow_t;
void p25_lr_get_flow(p25_lr_flow_t *out);
bool p25_lr_capture_read(uint32_t at, uint8_t *out, uint32_t n);
bool p25_lr_capture_packet(uint32_t i, uint32_t *offset, uint32_t *ms, bool *alt);

/* The decoder's own symbols as it reads them, whatever the radio: each one's
   dibit by the decoder's thresholds of the moment (1 = +3, 0 = +1, 2 = -1,
   3 = -3, before any inversion), counted from the capture's start, for
   comparing one board's receive with another's. */
typedef struct {
    bool on;
    uint32_t symbols, oldest;
} p25_sym_capture_info_t;
void p25_sym_capture_start(void);
void p25_sym_capture_stop(void);
void p25_sym_capture_info(p25_sym_capture_info_t *out);
/* n symbols from `at` (a multiple of 4), four to a byte, first in the top bits */
bool p25_sym_capture_read(uint32_t at, uint8_t *out, uint32_t n);

/* The IMBE frames the decoder settled, whatever the radio: 16 bytes each, the
   88 bits, how (dsd.h DSD_IMBE_*, | 4 when the frame held an erasure), the
   errors FEC fixed in c0 and in c1..c6, and the time in 10 ms from the
   capture's start (16 bits). A ring counted in frames. */
typedef struct {
    bool on;
    uint32_t frames, oldest;
} p25_imbe_capture_info_t;
#define P25_IMBE_REC 16
void p25_imbe_capture_start(void);
void p25_imbe_capture_stop(void);
void p25_imbe_capture_info(p25_imbe_capture_info_t *out);
bool p25_imbe_capture_read(uint32_t at, uint8_t *out, uint32_t n);

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
