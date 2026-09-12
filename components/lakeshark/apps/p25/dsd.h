/*
 * DSD-derived source, modified for LakeShark.
 * Comparison source: https://github.com/szechyjs/dsd/blob/59423fa46be8b41ef0bd2f3d2b45590600be29f0/include/dsd.h
 * Original import revision is unrecorded.
 *
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef DSD_H
#define DSD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "mbelib.h"
#include "p25p1_heuristics.h"
#include "p25_acquisition.h"

/* the live 240 kSPS IQ path decimates by five and DSD consumes ten
 * samples per 4800-baud symbol.  The old 24 kHz declaration was unused, but
 * contradicted both sides of that measured 48 kHz boundary. */
#define SAMPLE_RATE_IN  48000
#define SAMPLE_RATE_OUT 8000

#define DSD_SAMPLE_RING_SIZE 8192
#define P25_TSDU_DIBIT_COUNT 303
#define P25_IDEN_TABLE_SIZE 16
#define P25_NEIGHBOR_TABLE_SIZE 16
#define P25_TSBK_OPCODE_COUNT 64

extern int exitflag;

typedef struct {
    int16_t buf[DSD_SAMPLE_RING_SIZE];
    volatile int write_idx;
    volatile int read_idx;
} dsd_sample_ring_t;

typedef struct {
    uint64_t base_hz;
    uint32_t spacing_hz;
    /* transmit offset carried by IDEN_UP (0x3d) and IDEN_UP_VU (0x34).
     * VU form has a signed 14-bit field (bit 24 sign + 13-bit magnitude), UHF
     * form is 9 bits (bit 29 sign + 8-bit magnitude); both are units of
     * 250 kHz. Stored here in Hz already scaled and signed. */
    int32_t  tx_offset_hz;
    /* channel bandwidth in Hz. 0x3d carries a 9-bit field in 125 Hz
     * units; 0x34 carries a 4-bit code where 4 => 6.25 kHz and 5 => 12.5 kHz.
     * Normalised here so a filter chooser does not need the source opcode. */
    uint32_t channel_bw_hz;
    /* TDMA logical channels share one RF carrier. Zero identifies FDMA (or
     * an entry not populated yet); a nonzero value is the number of logical
     * slots carried at each frequency. */
    uint8_t slots_per_carrier;
    uint8_t channel_type;
    uint8_t valid;
    uint8_t voice_unsupported; /* FDMA half-rate is not Phase I IMBE. */
} p25_iden_entry_t;

typedef enum {
    P25_CALL_NONE = 0,
    P25_CALL_PHASE1,
    P25_CALL_UNSUPPORTED,
    P25_CALL_MISSING_IDEN,
    P25_CALL_INVALID,
} p25_call_support_t;

/* resolved grant evidence, independent of the legacy zero-frequency
 * safety gate. slots_per_carrier defaults to ONE for FDMA; slot is zero-based.
 * No allocation: six entries cover both pairs in each of three TSDU blocks. */
typedef struct {
    uint64_t carrier_hz;
    uint32_t source;
    uint32_t wacn;
    uint16_t sysid;
    uint16_t nac;
    uint16_t channel;
    uint16_t talkgroup;
    uint8_t slot;
    uint8_t slots_per_carrier;
    uint8_t channel_type;
    uint8_t system_valid;
    uint8_t service_options;
    p25_call_support_t support;
} p25_call_info_t;

#define P25_GRANTS_PER_TSDU 6

typedef struct {
    uint8_t  valid;
    uint8_t  rfss_id;
    uint8_t  site_id;
    uint8_t  lra;
    uint16_t sysid;
    uint16_t channel;
    uint8_t  service_class;
    uint64_t freq_hz;
} p25_neighbor_entry_t;

typedef struct dsd_opts {
    int errorbars;
    int verbose;
    int p25enc;
    int p25lc;
    int p25status;
    int p25tg;
    int frame_p25p1;
    int mod_c4fm;
    int mod_qpsk;
    int mod_gfsk;
    int uvquality;
    int mod_threshold;
    int ssize;
    int msize;
    int use_cosine_filter;
    int unmute_encrypted_p25;
    float audio_gain;
    int audio_out;
    int symboltiming;
    int datascope;
    int scoperate;
    dsd_sample_ring_t *ring;
} dsd_opts;

typedef struct dsd_state {
    p25_hunt_status_t acquisition_hunt;
    int *dibit_buf;
    int *dibit_buf_p;
    int repeat;
    short *audio_out_buf;
    short *audio_out_buf_p;
    float *audio_out_float_buf;
    float *audio_out_float_buf_p;
    float audio_out_temp_buf[160];
    float *audio_out_temp_buf_p;
    int audio_out_idx;
    int audio_out_idx2;
    int center;
    int jitter;
    int synctype;
    int min;
    int max;
    int lmid;
    int umid;
    int minref;
    int maxref;
    int lastsample;
    int sbuf[128];
    int sidx;
    int maxbuf[1024];
    int minbuf[1024];
    int midx;
    char err_str[64];
    char fsubtype[16];
    char ftype[16];
    int symbolcnt;
    int rf_mod;
    int numflips;
    int lastsynctype;
    int lastp25type;
    int offset;
    int carrier;
    char tg[25][16];
    int tgcount;
    int lasttg;
    int lastsrc;
    int nac;
    int errs;
    int errs2;
    int optind;
    int numtdulc;
    int firstframe;
    char slot0light[8];
    char slot1light[8];
    float aout_gain;
    float aout_max_buf[200];
    float *aout_max_buf_p;
    int aout_max_buf_idx;
    int samplesPerSymbol;
    int symbolCenter;
    /* ──────────────────────────────────────────────────────────────
     * Session 10: C4FM clock-assist TED state (ported from DSD-Neo
     * src/dsp/dsd_symbol.c::maybe_c4fm_clock). Mueller-&-Müller mode.
     * Observes early/mid/late samples around symbolCenter each symbol,
     * builds a run of consistent directional error, and nudges
     * symbolCenter ±1 when the run reaches 4 consecutive hits.
     * Cooldown prevents chatter after a nudge.
     * ────────────────────────────────────────────────────────────── */
    int c4fm_clk_mode;        /* 0=off, 1=Early-Late, 2=Mueller-Müller */
    int c4fm_clk_prev_dec;    /* sliced decision from previous symbol, {-3,-1,1,3} or 0 = none yet */
    int c4fm_clk_run_dir;     /* current run direction: -1 / 0 / +1 */
    int c4fm_clk_run_len;     /* consecutive symbols in same direction */
    int c4fm_clk_cooldown;    /* symbols remaining before next nudge allowed */
    int c4fm_clk_nudges;      /* diagnostic: total nudges performed since init */
    char algid[9];
    char keyid[17];
    /* Encryption Sync Stream from LDU2 (ALGID, KID, 72-bit MI). */

    uint8_t  p25_algid;
    uint16_t p25_kid;
    uint8_t  p25_mi[9];
    uint8_t  p25_ess_valid;
    /* cumulative IMBE frames dropped by the encryption gate. Read by
     * the UI/console; incremented inside process_IMBE, so a fault report that
     * says "it went quiet" can be answered with "42 frames muted on TG X". */
    uint32_t p25_enc_muted_frames;
    int currentslot;
    mbe_parms *cur_mp;
    mbe_parms *prev_mp;
    mbe_parms *prev_mp_enhanced;
    int p25kid;
    unsigned int debug_audio_errors;
    unsigned int debug_header_errors;
    unsigned int debug_header_critical_errors;
    int last_dibit;
    /* TSDUs were counted and then discarded, leaving no payload for
     * trunking. Keep the 303 wire-order dibits in the PSRAM-resident decoder
     * state; later decoding stages can consume them without demod-path allocs. */
    uint8_t p25_tsdu_dibits[P25_TSDU_DIBIT_COUNT];
    unsigned int p25_tsdu_dibit_count;

    p25_iden_entry_t p25_iden_table[P25_IDEN_TABLE_SIZE];
    uint8_t p25_tsbk_last_opcode;
    uint16_t p25_tsbk_channel;
    uint16_t p25_tsbk_talkgroup;
    uint32_t p25_tsbk_source;
    uint64_t p25_tsbk_frequency_hz;
    p25_call_info_t p25_grants[P25_GRANTS_PER_TSDU];
    uint8_t p25_grant_count;
    uint8_t p25_grant_batch_valid;
    uint32_t p25_grant_generation;
    /* Validated current frame, not lastp25type (which also tracks fallbacks). */
    uint8_t p25_frame_valid;
    uint8_t p25_frame_duid;
    uint8_t p25_frame_tsbks;
    uint16_t p25_control_nac;
    uint8_t p25_control_nac_valid;
    unsigned int p25_phase2_grant_count;
    uint16_t p25_phase2_last_talkgroup;
    uint64_t p25_phase2_last_frequency_hz;
    uint8_t p25_phase2_last_slot;
    uint8_t p25_phase2_last_slots_per_carrier;
    uint32_t p25_tsbk_wacn;
    uint16_t p25_tsbk_sysid;
    /* zero is representable on the wire, so value!=0 cannot mean
     * "decoded". Generations advance only on CRC-valid broadcasts and let the
     * bounded health snapshot age each retained identity class independently. */
    uint8_t p25_net_valid;
    uint32_t p25_net_generation;
    uint32_t p25_rfss_generation;
    uint32_t p25_sccb_generation;
    uint32_t p25_neighbor_generation;
    unsigned int p25_tsbk_valid_count;
    unsigned int p25_tsbk_crc_errors;
    unsigned int p25_tsbk_trellis_errors;
    /* vendor TSBKs (MFID other than 0x00 / 0x01) are counted rather
     * than parsed with the standard bit layout. See p25_tsbk_parse. */
    unsigned int p25_tsbk_vendor_count;
    uint8_t p25_tsbk_last_vendor_mfid;

    uint16_t p25_tsbk_unhandled[P25_TSBK_OPCODE_COUNT];
    /* RFSS_STS_BCST (0x3a). Which RFSS/site this control channel
     * belongs to. Otherwise a guess - the site ID does not appear anywhere
     * else on the control channel. */
    uint8_t  p25_rfss_valid;
    uint8_t  p25_rfss_lra;
    uint8_t  p25_rfss_id;
    uint8_t  p25_rfss_site_id;
    uint16_t p25_rfss_sysid;
    uint16_t p25_rfss_ch_t;
    uint16_t p25_rfss_ch_r;
    uint8_t  p25_rfss_svc_class;
    /* SCCB (0x39 SCCB_EXP / 0x3e SCCB). Secondary control channel
     * broadcast - a site running more than one control channel. */
    uint8_t  p25_sccb_valid;
    uint8_t  p25_sccb_rfss_id;
    uint8_t  p25_sccb_site_id;
    uint16_t p25_sccb_ch1;
    uint16_t p25_sccb_ch2;
    uint8_t  p25_sccb_svc_class1;
    uint8_t  p25_sccb_svc_class2;
    /* SYS_SRV_BCST (0x38). Which services the system offers. */
    uint8_t  p25_sys_srv_valid;
    uint32_t p25_sys_srv_available;
    uint32_t p25_sys_srv_supported;
    uint8_t  p25_sys_srv_twv;
    /* SYNC_BCST (0x30). System time and microslot count, needed
     * for Phase 2 slot alignment - stored here even though no consumer
     * uses it yet so 656's work can pick it up. */
    uint8_t  p25_sync_valid;
    uint16_t p25_sync_microslot;
    uint32_t p25_sync_us;
    uint32_t p25_sync_month_day;
    uint32_t p25_sync_year;
    /* adjacent site table populated by ADJ_STS_BCST (0x3c). Each
     * broadcast overwrites the slot keyed by (rfss_id, site_id) so the
     * list does not grow when a site re-announces itself. p25_neighbor_count
     * is the number of valid slots, useful as a "have we heard any" flag. */
    p25_neighbor_entry_t p25_neighbors[P25_NEIGHBOR_TABLE_SIZE];
    uint8_t              p25_neighbor_count;
    /* data-channel grants (0x10 GRP_D_CH_GRANT / 0x14 SNDCP_CH_GRANT).
     * We cannot decode the payload, but counting them keeps a busy data
     * exchange from looking like a broken follower. Never written to the
     * voice-grant fields so the follower cannot chase them. */
    unsigned int p25_data_grant_count;
    uint16_t     p25_last_data_channel;
    uint16_t     p25_last_data_group;

    unsigned int p25_interconnect_grant_count;
    uint16_t     p25_last_interconnect_channel;
    uint32_t     p25_last_interconnect_address;

    unsigned int p25_uu_grant_count;
    uint16_t     p25_last_uu_channel;
    uint32_t     p25_last_uu_source;
    uint32_t     p25_last_uu_target;
    /* registration/affiliation responses (0x20 ACK_RSP_FNE,
     * 0x27 DENY_RSP, 0x28 GRP_AFF_RSP, 0x2c UNIT_REG_RSP, 0x2f U_DE_REG_ACK).
     * The raw material for a "who is on this system" view. A DENY explains
     * a grant that never produced audio, which currently looks like the
     * decoder is broken. */
    unsigned int p25_reg_event_count;
    uint8_t      p25_last_reg_opcode;
    uint8_t      p25_last_reg_reason;
    uint32_t     p25_last_reg_source;
    uint32_t     p25_last_reg_target;
    /* LDU1/TDULC Link Control Word. */

    uint8_t  p25_lcw_valid;
    uint8_t  p25_lcw_lco;
    uint8_t  p25_lcw_mfid;
    uint8_t  p25_lcw_svc_options;
    uint8_t  p25_lcw_emergency;
    uint8_t  p25_lcw_encrypted;
    uint8_t  p25_lcw_priority;
    uint8_t  p25_lcw_is_unit_to_unit;
    uint8_t  p25_lcw_is_regroup;
    uint16_t p25_lcw_talkgroup;
    uint32_t p25_lcw_source;
    uint32_t p25_lcw_target;
    uint16_t p25_lcw_patch_sg;
    uint32_t p25_lcw_ok_count;
    uint32_t p25_lcw_fec_reject_count;

    uint8_t  p25_lcw_alias_expected;
    uint8_t  p25_lcw_alias_blocks;
    uint8_t  p25_lcw_alias_len;
    uint8_t  p25_lcw_alias_ready;
    char     p25_lcw_alias[32];
    int64_t  p25_lcw_alias_start_us;
    P25Heuristics p25_heuristics;
    P25Heuristics inv_p25_heuristics;
    short *pcm_out_buf;
    int pcm_out_write;
    int pcm_out_size;
} dsd_state;

#define INV_P25P1_SYNC "333331331133111131311111"
#define P25P1_SYNC     "111113113311333313133333"

/* Set to 1 to make the decode task bail out of blocking sample reads
 * immediately (on P25 app teardown). Prevents the dsd task from wedging in
 * starved ring reads past the drain timeout and being re-created over its
 * still-running static stack. */
extern volatile int dsd_abort;

void initOpts(dsd_opts *opts);
void initState(dsd_state *state);
int  getSymbol(dsd_opts *opts, dsd_state *state, int have_sync);
int  getDibit(dsd_opts *opts, dsd_state *state);
int  get_dibit_and_analog_signal(dsd_opts *opts, dsd_state *state, int *out_analog_signal);
void skipDibit(dsd_opts *opts, dsd_state *state, int count);
int  comp(const void *a, const void *b);
void noCarrier(dsd_opts *opts, dsd_state *state);
void processFrame(dsd_opts *opts, dsd_state *state);
int  getFrameSync(dsd_opts *opts, dsd_state *state);
void printFrameSync(dsd_opts *opts, dsd_state *state, char *frametype, int offset, char *modulation);
void printFrameInfo(dsd_opts *opts, dsd_state *state);
void processHDU(dsd_opts *opts, dsd_state *state);
void processLDU1(dsd_opts *opts, dsd_state *state);
void processLDU2(dsd_opts *opts, dsd_state *state);
void processTDU(dsd_opts *opts, dsd_state *state);
void processTDULC(dsd_opts *opts, dsd_state *state);
void processP25lcw(dsd_opts *opts, dsd_state *state, char *lcformat, char *mfid, char *lcinfo, int fec_ok);
/* byte-oriented dispatcher. processP25lcw wraps this after converting
 * the bit-strings coming out of LDU1/TDULC. Bench tests call the byte form
 * directly. Returns 1 when the LCW was recognised and state was updated, 0
 * when it was ignored (unknown LCO, vendor MFID, or fec_ok == 0). */
#define P25_LCW_LCINFO_BYTES 7
#define P25_LCW_ALIAS_TIMEOUT_US 2000000LL
int p25_lcw_dispatch(dsd_state *state, uint8_t lco, uint8_t mfid,
                     const uint8_t lcinfo[P25_LCW_LCINFO_BYTES],
                     int fec_ok, int64_t now_us);
void p25_lcw_call_clear(dsd_state *state);
void processMbeFrame(dsd_opts *opts, dsd_state *state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]);
void processAudio(dsd_opts *opts, dsd_state *state);
void upsample(dsd_state *state, float invalue);
short dmr_filter(short sample);
short nxdn_filter(short sample);

int dsd_ring_available(dsd_sample_ring_t *r);
int16_t dsd_ring_read_one(dsd_sample_ring_t *r);

/* */
void p25_ess_clear(dsd_state *state);
const char *p25_algid_name(uint8_t algid);
int  p25_ldu_should_mute_encrypted(const dsd_state *state, const dsd_opts *opts);
/* byte-in predicate for the grant follower - it does not carry a
 * dsd_state and only needs the CLEAR vs anything-else decision. */
int  p25_algid_is_encrypted(uint8_t algid);

/* format a control-channel status summary (IDEN table, neighbour
 * list, TSBK counts, unhandled opcodes) into a caller-supplied buffer.
 * The console `p25tsbk` command prints this; bench cases assert against
 * substrings of it so what is on-screen and what is in the tests cannot
 * disagree. Returns the number of characters that would be written if the
 * buffer were unbounded (as snprintf does). */
size_t p25_tsbk_status_format(const dsd_state *state, char *buf, size_t buf_sz);

#endif
