#ifndef PAGER_IQ_H
#define PAGER_IQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PGR_IQ_INPUT_RATE 125000
#define PGR_IQ_OUTPUT_RATE 25000

typedef struct {
    float prev_i, prev_q;
    float lowpass[4];
    uint8_t partial[4];
    uint8_t partial_n, phase;
    bool have_prev;
} pgr_iq_t;

/* Reset on a stream gap/retune; also call pocsag_seam on each decoder. */
void pgr_iq_init(pgr_iq_t *s);
void pgr_iq_reset(pgr_iq_t *s);

/* Signed little-endian IQ16 at 125 ksps to discriminator radians/sample at
   25 ksps. Byte fragments, filter history and decimation phase carry across
   calls. Set each decoder's sample rate to PGR_IQ_OUTPUT_RATE. The POCSAG
   decoder handles offset and polarity; do not hard-slice these samples.
   Output capacity bytes/20 + 1 is always sufficient. Returns false without
   consuming input if capacity is insufficient or arguments are invalid.
   Input and output must not overlap. No allocation or decoder ownership. */
bool pgr_iq_process(pgr_iq_t *s, const uint8_t *iq, size_t bytes,
                    float *out, size_t capacity, size_t *written);

#endif
