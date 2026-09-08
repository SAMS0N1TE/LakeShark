
#ifndef ACARS_RESAMPLE_H
#define ACARS_RESAMPLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rational 5:3 resampler for the ACARS audio path.

   The FM NBFM chain hands out demod audio at FM_RTL_RATE / 8 = 32000 Hz;
   the ACARS MSK slicer only understands ACARS_SAMP_RATE = 19200 Hz, and
   its bit-clock tables are baked at that rate.  32000 x 3 / 5 = 19200
   exactly, so a rational-fraction phase tracked in integers converts
   without float drift accumulating across the 32 ms blocks the FM task
   feeds through.  Linear interpolation is enough: the fm_demod cascade
   band-limits well below 16 kHz and the ACARS tones sit at 1.2 / 2.4 kHz,
   nowhere near where a coarser downsampler would fold energy on top of
   the signal. */

typedef struct {
    float last;      /* in[-1] carried across blocks                     */
    int   pos_num;   /* current position in units of 1/3 input sample;
                        can be negative when the next output falls between
                        the previous block's tail and this block's head   */
    bool  primed;    /* set once 'last' holds a real sample              */
} acars_rs_t;

void acars_rs_init (acars_rs_t *r);

/* Resample n_in samples at 32000 Hz into up to `cap` samples at 19200 Hz.
   Returns the number of output samples written. */
int  acars_rs_5to3 (acars_rs_t *r, const float *in, int n_in,
                    float *out, int cap);

#ifdef __cplusplus
}
#endif

#endif
