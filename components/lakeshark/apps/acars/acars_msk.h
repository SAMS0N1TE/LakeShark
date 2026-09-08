
#ifndef ACARS_MSK_H
#define ACARS_MSK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MSK bit slicer split out from acars.c so it can be tested on its own.

   Feed the slicer audio-band samples at ACARS_SAMP_RATE via
   acars_msk_process().  It produces one soft-sliced bit per bit period
   (8 samples nominal) into an internal ring, which the framer reads with
   acars_msk_read_bits().

   The slicer runs a matched filter for each tone (mark = 1200 Hz, space =
   2400 Hz) over the last 8 samples.  Bit value = which tone has more energy.
   Bit timing is a bang-bang loop on the sliced output - transitions pull the
   sample point toward the bit centre, so a real transmitter's clock error is
   absorbed by the same loop the FLEX/POCSAG paths use.

   The MSK slicer knows nothing about ACARS framing - that lives in acars.c.
   This separation is what lets a bench case drive audio through the slicer,
   read bits back, and compare to what was transmitted, without any framing
   state in the way. */

typedef struct acars_msk acars_msk_t;

acars_msk_t *acars_msk_create (void);
void         acars_msk_destroy(acars_msk_t *m);
void         acars_msk_reset  (acars_msk_t *m);

/* Process `n` audio samples.  Returns the number of new bits appended to
   the internal ring - up to `n / 8`, with rounding. */
int          acars_msk_process(acars_msk_t *m, const float *audio, int n);

/* Read up to `cap` bits from the internal ring.  Returns the number
   written to `out`.  Bit values are 0 or 1. */
int          acars_msk_read_bits(acars_msk_t *m, uint8_t *out, int cap);

/* Diagnostics. */
int          acars_msk_available(const acars_msk_t *m);

#ifdef __cplusplus
}
#endif

#endif
