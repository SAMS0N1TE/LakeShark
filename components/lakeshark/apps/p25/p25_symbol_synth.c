#include "p25_symbol_synth.h"

void p25_symbol_synth(uint8_t dibit, int16_t out[P25_SYNTH_SAMPLES_PER_SYMBOL])
{
    static const int16_t level[5] = { 707, 2121, -707, -2121, P25_SYNTH_ERASED_LEVEL };
    const uint8_t d = dibit & 7u;          /* the bits above carry doubts */
    int16_t v = level[d <= P25_SYNTH_ERASED ? d : P25_SYNTH_ERASED];
    for (int i = 0; i < P25_SYNTH_SAMPLES_PER_SYMBOL; i++) out[i] = v;
}
