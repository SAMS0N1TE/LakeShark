
#ifndef ACARS_RESAMPLE_H
#define ACARS_RESAMPLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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
