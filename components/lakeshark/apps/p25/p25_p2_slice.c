#include "p25_p2_slice.h"
#include <math.h>

int p25_p2_slice(const int16_t *samples, int count, float demod_gain,
                 uint8_t *dibits, int max)
{
    const float threshold = fabsf(demod_gain) * 2.0f;
    int out = 0;
    for (int i = 0; i + P25_P2_SAMPLES_PER_SYMBOL - 1 < count && out < max;
         i += P25_P2_SAMPLES_PER_SYMBOL) {
        const int v = samples[i];
        dibits[out++] = (uint8_t)(v >= 0 ? (v > threshold ? 1 : 0)
                                         : (v < -threshold ? 3 : 2));
    }
    return out;
}
