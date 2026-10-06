#pragma once
#include <stdint.h>
#include "p25_os4_nn_model.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The LR2021 symbol network: the 64 bits from P25_OS4_NN_PRE before a
 * symbol's first bit to the P25_OS4_NN_POST - 1st after it in, the four
 * levels' probabilities out. Learned against an RTL's decode of the same
 * calls (p25_os4_nn_model.c is generated). On 75 calls it never trained on
 * it read 93.1% of voice symbols right against the 16-bit lookup's 89.3%,
 * and the RTL frames played bit-exact rose from 57% to 69%: the detector's
 * leaky tracker spreads a symbol's level far beyond its own four bits.
 * Retrained 2026-10-05 with 19 more calls (5.58M symbols in all): on those
 * calls, each held out of its own training, 97.17% -> 97.33%, every one
 * better. Layer 2's sums are scaled to stay inside int16 with room to
 * spare, negative ones too: the vector unit's shift out of its accumulator
 * wraps rather than saturating.
 *
 * The window is the decoder's 64-bit register, newest bit in bit 0, so a
 * symbol is decided when the P25_OS4_NN_POST - 1st bit after its start
 * arrives. It runs in integers: the first layer's inputs are +-1, so it is
 * stored as the sums for every value of each 4-bit chunk of the window, and
 * on the P4 the first two layers run on the vector unit (p25_os4_nn_pie.S),
 * about 26 us a symbol, an eighth of a core at 4800 symbols a second. */

#define P25_OS4_NN_DELAY (P25_OS4_NN_POST - 1)

/* Copies the tables to PSRAM on the chip, once (a no-op elsewhere):
   p25_os4_reset calls it. */
void    p25_os4_nn_init(void);
/* +1, +3, -1, -3: the decoder's dibit order */
void    p25_os4_nn_probs(uint64_t win, float p[4]);
/* the byte p25_os4_decode sends: the likeliest dibit and its two bits' doubts */
uint8_t p25_os4_nn_byte(uint64_t win);
/* `p25 lr nnbench`: what a symbol costs on the chip, by layer, and whether
   the vector kernels agree with the plain C */
void    p25_os4_nn_bench(void);

#ifdef __cplusplus
}
#endif
