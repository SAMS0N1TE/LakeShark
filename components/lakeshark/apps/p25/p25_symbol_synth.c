#include "p25_symbol_synth.h"

void p25_symbol_synth(uint8_t dibit, int16_t out[P25_SYNTH_SAMPLES_PER_SYMBOL])
{
    static const int16_t level[4] = { 707, 2121, -707, -2121 };
    int16_t v = level[dibit & 3];
    for (int i = 0; i < P25_SYNTH_SAMPLES_PER_SYMBOL; i++) out[i] = v;
}
