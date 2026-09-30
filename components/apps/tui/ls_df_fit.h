/* A bearing from the whole turn, not from its loudest moment.

   ls_df_estimate aims at the strongest held bin. One lucky read (a fade
   that happened to add up, a reflection off a car) can win that, and a
   bin never heard can hide a stronger one. Here every direction heard
   counts: each bin's median level (the last few reads there, so a single
   deep fade or spike does not move it) is fitted with a smooth curve round
   the circle,

       level(h) = a + b1 cos h + c1 sin h + b2 cos 2h + c2 sin 2h

   by weighted least squares. The first harmonic is the body's shadow or a
   lobe; the second is what a flat-held whip adds (a figure eight, which
   cannot tell front from back). The bearing is where the fitted curve peaks,
   or opposite its lowest point for NULL, and the fit says how well it knows:
   the residual spread over the first harmonic's amplitude gives a 1-sigma
   in degrees (sigma ~ resid / (amp * sqrt(n / 2)) radians).

   With a pattern learnt from a beacon at a known bearing (ls_df_pattern_t),
   the heard levels are matched against that pattern at every rotation
   instead, which takes out this board's own lopsidedness and needs no
   offset afterwards.

   Pure: no radio, sensor or storage is touched here. */

#ifndef LS_DF_FIT_H
#define LS_DF_FIT_H

#include <stdbool.h>
#include <stdint.h>
#include "ls_df.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_DF_PAT_BINS 36          /* 10 degrees each */

/* Level against where the board faces relative to the transmitter: bin 0 is
   facing it, bin 9 is the transmitter on the right. dB about the mean. */
typedef struct {
    float g[LS_DF_PAT_BINS];
    uint16_t circles;
} ls_df_pattern_t;

typedef struct {
    bool valid;
    float bearing;       /* true degrees, where the signal is */
    float sigma;         /* 1-sigma, degrees */
    float depth;         /* the fitted curve's highest minus lowest, dB */
    float r2;            /* share of the level's variation the curve explains */
    float match;         /* correlation with the learnt pattern, NAN without one */
    bool ambiguous;      /* front and back look alike: `alt` is as likely */
    float alt;
    int bins;            /* directions heard */
    int gap;             /* widest arc not heard, degrees */
} ls_df_fit_t;

bool ls_df_fit(const ls_df_sweep_t *s, ls_df_method_t method, const ls_df_pattern_t *pattern,
               ls_df_fit_t *out);
/* The fit as an ls_df_estimate_t, for everything drawn from one: the spread
   is two sigma. */
void ls_df_fit_estimate(const ls_df_sweep_t *s, const ls_df_fit_t *f, ls_df_estimate_t *out);

void ls_df_pattern_clear(ls_df_pattern_t *p);
/* One circle heard with the transmitter at `truth` (true degrees) goes into
   the pattern, averaged with the circles before it. False when the circle
   covers too little to say anything. */
bool ls_df_pattern_learn(ls_df_pattern_t *p, const ls_df_sweep_t *s, float truth);
/* Packed as whole half-dB steps, for storage. */
void ls_df_pattern_pack(const ls_df_pattern_t *p, int8_t out[LS_DF_PAT_BINS]);
void ls_df_pattern_unpack(ls_df_pattern_t *p, const int8_t in[LS_DF_PAT_BINS], uint16_t circles);

#ifdef __cplusplus
}
#endif
#endif
