
#ifndef ACARS_MSK_H
#define ACARS_MSK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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
