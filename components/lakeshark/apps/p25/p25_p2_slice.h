#pragma once
/* Phase II symbol decisions from the CQPSK demodulator's output.
 *
 * With dsp->phase2 set the demodulator repeats each 6000-baud symbol eight
 * times at 48 kHz; one sample in eight is a symbol, sliced at +-2 x the demod
 * gain into dibits 1 (+3), 0 (+1), 2 (-1), 3 (-3). Pulled out of
 * p25_p2_runtime.c, which also owns an esp_timer and a spinlock, so the host
 * bench can drive the whole Phase II front end - IQ, demodulator, this, the
 * decoder - with no radio (test_p25_phase2_iq).
 */
#include <stdint.h>

#define P25_P2_SAMPLES_PER_SYMBOL 8

/* Returns the number of dibits written (at most max). */
int p25_p2_slice(const int16_t *samples, int count, float demod_gain,
                 uint8_t *dibits, int max);
