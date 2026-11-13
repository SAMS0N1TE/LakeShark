/* A NOISE FLOOR PER BIN, and detections measured against it.

   Every spectrum this board draws has a floor that is not flat. The LR2021's
   I/Q channel is shaped by its own receive filter: with nothing on the air the
   125 kHz view is a hump about 60% of the span wide, ~36 dB over the bins at
   its edges. The RSSI sweep is the same thing across a band: the front end's
   response, its calibration steps and the chip's own spurs move the floor
   from one frequency to the next by tens of dB. Anything that compares a bin
   with ONE floor for the whole row - a percentile of it, its quietest bin -
   paints that shape as a signal and detects it on every pass. That is the
   solid red BAND waterfall and the scanner that "goes nuts" with nothing
   transmitting; it is not the receiver's sensitivity.

   So each bin learns its own floor. The first `learn` frames seed it (their
   lower median, so a burst during them is not learned), and afterwards it
   follows the bin's median, a fixed step a frame: fast enough to follow a
   drifting front end, far too slow to absorb a burst. While a bin is over its
   gate the floor almost stops, so a carrier that stays on stays visible for
   minutes rather than being learned in seconds.

   A detection is a bin over ITS OWN floor by the gate for `dwell` frames, and
   it ends only when the bin falls `hyst_db` below the gate, so a reading that
   sits on the line is one event and not one a frame. The gate is never under
   `dev_k` times the bin's own scatter, so a threshold set into the noise of a
   receiver whose readings jump still does not fire on noise.

   Pure: no ESP-IDF, no allocation, so the bench drives it with synthetic
   rows. Levels are dB (dBm or dBFS - only differences are used). */
#ifndef LS_FLOOR_H
#define LS_FLOOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_FLOOR_BINS_MAX  256
#define LS_FLOOR_LEARN_MAX 16

typedef struct {
    float step_db;   /* how far a floor moves a frame toward the bin's median */
    int   learn;     /* frames that seed the floor, 1..LS_FLOOR_LEARN_MAX      */
    int   dwell;     /* frames over the gate before a bin is a hit, >= 1       */
    float hyst_db;   /* a hit ends below gate - hyst_db                        */
    float dev_k;     /* the gate is at least dev_k times the bin's scatter     */
} ls_floor_cfg_t;

typedef struct {
    ls_floor_cfg_t cfg;
    int      n;
    uint32_t frames;
    float    pool;                      /* the row's median scatter, dB         */
    float    base[LS_FLOOR_BINS_MAX];   /* the floor, dB                        */
    float    dev[LS_FLOOR_BINS_MAX];    /* mean absolute scatter about it, dB   */
    uint8_t  run[LS_FLOOR_BINS_MAX];    /* frames in a row over the gate        */
    bool     hit[LS_FLOOR_BINS_MAX];
    float    seed[LS_FLOOR_LEARN_MAX][LS_FLOOR_BINS_MAX];
    float    scratch[LS_FLOOR_BINS_MAX];    /* sorting, so no stack is spent  */
    float    row[LS_FLOOR_BINS_MAX];        /* ls_floor_iq_row's levels in dB */
} ls_floor_t;

/* The two users' settings, named so a test and the firmware agree.
   I/Q rows arrive ten a second, one FFT each (+/-6 dB of scatter): a floor
   that follows at 0.1 dB a row and a hit that needs two rows. A sweep pass is
   0.2-2 s of RSSI looks: it follows faster per pass, and a remote's 60 ms
   press is in one pass at most, so one pass is enough to be a hit. */
extern const ls_floor_cfg_t LS_FLOOR_IQ;
extern const ls_floor_cfg_t LS_FLOOR_SWEEP;

void  ls_floor_init(ls_floor_t *f, const ls_floor_cfg_t *cfg);
/* Forget what was learned (a retune, another band); keeps the settings. */
void  ls_floor_reset(ls_floor_t *f);
/* Whether the floor is learned, i.e. detections are possible. */
bool  ls_floor_ready(const ls_floor_t *f);

/* One frame of `n` levels in dB. A row of another length starts again.
   Returns how many bins BECAME hits this frame - events, not bins held. */
int   ls_floor_update(ls_floor_t *f, const float *db, int n, float gate_db);

static inline float ls_floor_base(const ls_floor_t *f, int i) { return f->base[i]; }
static inline bool  ls_floor_hit(const ls_floor_t *f, int i)  { return f->hit[i]; }
/* The gate this bin is actually held to, for `gate_db` asked. */
float ls_floor_gate(const ls_floor_t *f, int i, float gate_db);
/* The median floor and the median scatter (one standard deviation) across
   the row, for a readout that wants a single number. */
float ls_floor_median_base(ls_floor_t *f);
float ls_floor_median_spread(ls_floor_t *f);

/* The row to draw: each bin as dB over its own floor, mapped 0..1 so that
   `pad_db` under the floor is 0 and `window_db - pad_db` over it is 1. Noise
   lands near pad_db / window_db - dark - and only a real signal climbs.
   With `edge_db` > 0, bins whose floor sits more than edge_db under the
   passband's (the receive filter's skirts) are drawn at half. */
void  ls_floor_view(ls_floor_t *f, const float *db, float *out, int n,
                    float pad_db, float window_db, float edge_db);

/* THE LR I/Q VIEW, as BAND draws it. `bins` arrive 0..1 over -100..0 dBFS
   (lr_iq_spectrum.h) and are replaced in place by the view above: 6 dB of
   room under each floor, a 40 dB window over it (a 20 dB carrier lands in
   the warm half, noise never reaches yellow), and the receive filter's
   skirts - floors 12 dB under the passband - at half.
   Returns the detections that began this row against `gate_db`. */
#define LS_FLOOR_IQ_PAD_DB    6.0f
#define LS_FLOOR_IQ_WINDOW_DB 40.0f
#define LS_FLOOR_IQ_EDGE_DB   12.0f
int   ls_floor_iq_row(ls_floor_t *f, float *bins, int n, float gate_db);

#ifdef __cplusplus
}
#endif

#endif
