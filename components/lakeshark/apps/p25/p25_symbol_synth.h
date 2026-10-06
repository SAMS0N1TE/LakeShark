#pragma once
#include <stdint.h>

/* One P25 Phase 1 symbol as the C4FM demodulator hands it to the decoder:
 * ten samples at 48 kHz, held flat at the symbol's level. Levels match the
 * demodulator at its default gain, where +-600 Hz reads about +-707 and
 * +-1800 Hz about +-2121, so the slicer starts from thresholds it would see
 * on the air. Dibits are the decoder's own: 1 = +3, 0 = +1, 2 = -1, 3 = -3. */
enum { P25_SYNTH_SAMPLES_PER_SYMBOL = 10 };
/* A dibit of 4 is a symbol that was never received. Its samples are 1: the
   real levels are multiples of 707, so no window of them averages to 1, and
   the decoder counts a symbol of exactly 1 as an erasure when
   dsd_opts.erasure_marks is set. */
enum { P25_SYNTH_ERASED = 4, P25_SYNTH_ERASED_LEVEL = 1 };

void p25_symbol_synth(uint8_t dibit, int16_t out[P25_SYNTH_SAMPLES_PER_SYMBOL]);
