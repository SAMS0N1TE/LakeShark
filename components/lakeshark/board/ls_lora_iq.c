#include "ls_lora_iq.h"
#include <string.h>

/* Ported from the MIT lr2021-sdr project's lr2021_iq_dev.c:
   input-rate dividers (including fractional families) and raw 0x0290 route. */
uint32_t lr20xx_iq_source1_rate(uint8_t code)
{
    static const unsigned num[4] = {1, 13, 13, 13}, den[4] = {1, 14, 15, 18};
    const uint64_t d = (uint64_t)den[code >> 6] * (((code >> 3) & 7u) + 1u) << (code & 7u);
    return (uint32_t)((8000000ull * num[code >> 6] + d / 2) / d);
}

bool lr20xx_iq_raw_route(uint8_t code, uint32_t rate, unsigned source, uint8_t out[11])
{
    if (!out || source > 7 || !rate) return false;
    const uint8_t a[11] = {code, rate >> 24, rate >> 16, rate >> 8, rate,
                           0x08u | source, 0, 0, 0, 0, 1};
    memcpy(out, a, sizeof(a));
    return true;
}

bool lr20xx_iq_route_build(lr20xx_iq_route_t kind, uint8_t code, uint32_t source2_rate,
                          uint8_t out[11], uint32_t *rate, lr20xx_iq_format_t *format)
{
    if (!out || !rate || !format) return false;
    if (kind == LR20XX_IQ_SOURCE2) {
        if (source2_rate != 125000 && source2_rate != 250000 &&
            source2_rate != 500000 && source2_rate != 1000000) return false;
        const uint8_t a[11] = {0xFF, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 1};
        memcpy(out, a, sizeof(a));
        *rate = source2_rate; *format = LR20XX_IQ16;
        return true;
    }
    if (kind != LR20XX_IQ_SOURCE0_576 && kind != LR20XX_IQ_SOURCE1) return false;
    *rate = kind == LR20XX_IQ_SOURCE0_576 ? 576000 : lr20xx_iq_source1_rate(code);
    *format = LR20XX_IQ8;
    return lr20xx_iq_raw_route(kind == LR20XX_IQ_SOURCE0_576 ? 0x11 : code,
                             *rate, kind == LR20XX_IQ_SOURCE0_576 ? 0 : 1, out);
}

void lr20xx_iq_expand8(const uint8_t *in, int16_t *out, size_t pairs)
{
    for (size_t i = 0; i < pairs * 2; ++i) {
        const int v = in[i] < 128 ? in[i] : (int)in[i] - 256;
        out[i] = (int16_t)(v * 256); /* multiplication avoids shifting negatives */
    }
}
