#pragma once
#include <stdint.h>

/* One P25 Phase 1 symbol as the C4FM demodulator hands it to the decoder:
 * ten samples at 48 kHz, held flat at the symbol's level. Levels match the
 * demodulator at its default gain, where +-600 Hz reads about +-707 and
 * +-1800 Hz about +-2121, so the slicer starts from thresholds it would see
 * on the air. Dibits are the decoder's own: 1 = +3, 0 = +1, 2 = -1, 3 = -3. */
enum { P25_SYNTH_SAMPLES_PER_SYMBOL = 10 };

void p25_symbol_synth(uint8_t dibit, int16_t out[P25_SYNTH_SAMPLES_PER_SYMBOL]);
