#ifndef DSD_H
#define DSD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "mbelib.h"
#include "p25p1_heuristics.h"

#define SAMPLE_RATE_IN  24000   /* session 9: was 48000 */
#define SAMPLE_RATE_OUT 8000

#define DSD_SAMPLE_RING_SIZE 16384

extern int exitflag;

typedef struct {
    int16_t buf[DSD_SAMPLE_RING_SIZE];
    volatile int write_idx;
    volatile int read_idx;
} dsd_sample_ring_t;

typedef struct {
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

typedef struct {
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
    int currentslot;
    mbe_parms *cur_mp;
    mbe_parms *prev_mp;
    mbe_parms *prev_mp_enhanced;
    int p25kid;
    unsigned int debug_audio_errors;
    unsigned int debug_header_errors;
    unsigned int debug_header_critical_errors;
    int last_dibit;
    P25Heuristics p25_heuristics;
    P25Heuristics inv_p25_heuristics;
    short *pcm_out_buf;
    int pcm_out_write;
    int pcm_out_size;
} dsd_state;

#define INV_P25P1_SYNC "333331331133111131311111"
#define P25P1_SYNC     "111113113311333313133333"

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
void processP25lcw(dsd_opts *opts, dsd_state *state, char *lcformat, char *mfid, char *lcinfo);
void processMbeFrame(dsd_opts *opts, dsd_state *state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]);
void processAudio(dsd_opts *opts, dsd_state *state);
void upsample(dsd_state *state, float invalue);
short dmr_filter(short sample);
short nxdn_filter(short sample);

int dsd_ring_available(dsd_sample_ring_t *r);
int16_t dsd_ring_read_one(dsd_sample_ring_t *r);

#endif
