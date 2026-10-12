#include "pager_iq.h"
#include <math.h>
#include <string.h>

void pgr_iq_reset(pgr_iq_t *s)
{
    if (s) memset(s, 0, sizeof(*s));
}

void pgr_iq_init(pgr_iq_t *s) { pgr_iq_reset(s); }

static float signed_le16(const uint8_t *p)
{
    const unsigned u = (unsigned)p[0] | ((unsigned)p[1] << 8);
    return (float)(u < 32768u ? (int)u : (int)u - 65536);
}

bool pgr_iq_process(pgr_iq_t *s, const uint8_t *iq, size_t bytes,
                    float *out, size_t capacity, size_t *written)
{
    if (!written) return false;
    *written = 0;
    if (!s || (!iq && bytes) || s->partial_n > 3 || s->phase > 4) return false;
    const size_t pairs = bytes / 4 + (bytes % 4 + s->partial_n) / 4;
    const size_t need = pairs / 5 + (pairs % 5 + s->phase) / 5;
    if (need > capacity || (need && !out)) return false;

    for (size_t k = 0; k < bytes; k++) {
        s->partial[s->partial_n++] = iq[k];
        if (s->partial_n < 4) continue;
        s->partial_n = 0;
        const float i = signed_le16(s->partial);
        const float q = signed_le16(s->partial + 2);
        float x = 0.0f;
        if (s->have_prev && (i != 0.0f || q != 0.0f))
            x = atan2f(q * s->prev_i - i * s->prev_q,
                       i * s->prev_i + q * s->prev_q);
        s->prev_i = i;
        s->prev_q = q;
        s->have_prev = i != 0.0f || q != 0.0f;
        /* Four 0.25-pole sections: about 2.5 kHz combined -3 dB bandwidth,
           with filtering before decimation to suppress phase noise. */
        for (unsigned j = 0; j < 4; j++) {
            s->lowpass[j] += 0.25f * (x - s->lowpass[j]);
            x = s->lowpass[j];
        }
        if (++s->phase == 5) {
            s->phase = 0;
            out[(*written)++] = x;
        }
    }
    return true;
}
