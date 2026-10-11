#include "p25_lr_iq.h"
#include <limits.h>
static int sample(const uint8_t *p)
{
    unsigned v = p[0] | (unsigned)p[1] << 8;
    return v < 32768 ? (int)v : (int)v - 65536;
}
size_t p25_lr_iq_convert(const uint8_t *iq, size_t bytes, uint8_t *out, size_t capacity)
{
    if (!iq || !out || bytes < 8 || bytes % 4 || bytes > 125256) return 0;
    size_t pairs = bytes / 4;
    size_t count = (pairs - 1) * 48 / 25 + 1;
    if (count > capacity / 2) return 0;
    int peak = 1;
    for (size_t i = 0; i < bytes; i += 2) {
        int v = sample(iq + i); if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    for (size_t j = 0; j < count; j++) {
        size_t pos = j * 25, k = pos / 48;
        unsigned frac = pos % 48;
        for (unsigned c = 0; c < 2; c++) {
            int a = sample(iq + 4*k + 2*c);
            int b = k + 1 < pairs ? sample(iq + 4*(k+1) + 2*c) : a;
            float v = a + (b - a) * (frac / 48.0f);
            out[2*j+c] = (uint8_t)(128.0f + 110.0f * v / peak);
        }
    }
    return 2 * count;
}
