#include "p25_os4_decode.h"
#include <string.h>

/* The frame sync's 24 symbols, oldest first: + + + + + - + + - - + + - - - - + - + - - - - - (all outer). */
static const uint32_t kSyncSigns = 0xFB30A0;   /* 1 = positive, first symbol in bit 23 */

void p25_os4_reset(p25_os4_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
    d->phase = -1;
}

static unsigned transition_phase(const p25_os4_decoder_t *d)
{
    unsigned best = 0;
    for (unsigned p = 1; p < 4; p++)
        if (d->trans[p] > d->trans[best]) best = p;
    return best;
}

/* how many of a phase's last 24 symbols are the sync's outer level */
static unsigned sync_score(const p25_os4_decoder_t *d, unsigned p)
{
    const uint32_t signs = ((d->hist2[p] & 0xFF) << 16) | (d->hist[p] & 0xFFFF);
    const uint32_t outer = ((d->outer2[p] & 0xFF) << 16) | (d->outer[p] & 0xFFFF);
    const uint32_t right = ~(signs ^ kSyncSigns) & outer & 0xFFFFFF;
    return (unsigned)__builtin_popcount(right);
}

size_t p25_os4_decode(p25_os4_decoder_t *d, const uint8_t *bytes, size_t n,
                      uint8_t *dibits, size_t max)
{
    static const uint8_t kDibit[4] = { 3, 2, 0, 1 };   /* ones in the middle three: -3 -1 +1 +3 */
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        for (int k = 7; k >= 0; k--) {
            const uint8_t b = (bytes[i] >> k) & 1;
            const uint32_t at = d->pos++;
            if (at && b != d->last) d->trans[at & 3]++;
            d->last = b;
            d->win = (uint8_t)(((d->win << 1) | b) & 0x1F);
            if ((d->pos & 4095) == 0)
                for (int p = 0; p < 4; p++) d->trans[p] >>= 1;
            if (at < 4) continue;
            /* the window holds bits at-4..at: the symbol of phase at&3 */
            const unsigned p = at & 3;
            const unsigned ones = ((d->win >> 1) & 1) + ((d->win >> 2) & 1) + ((d->win >> 3) & 1);
            const uint8_t dib = kDibit[ones];
            d->hist2[p] = (d->hist2[p] << 1) | (d->hist[p] >> 15 & 1);
            d->outer2[p] = (d->outer2[p] << 1) | (d->outer[p] >> 15 & 1);
            d->hist[p] = (d->hist[p] << 1) | (ones >= 2);
            d->outer[p] = (d->outer[p] << 1) | (ones == 0 || ones == 3);
            d->last24[p][d->ring_at[p]] = dib;
            d->ring_at[p] = (uint8_t)((d->ring_at[p] + 1) % 24);

            const unsigned score = sync_score(d, p);
            d->score[p] = (uint8_t)score;
            if (d->phase < 0 && score < 21 && p == transition_phase(d)) {
                if (out < max) dibits[out++] = dib;
                continue;
            }
            /* the phase in use covered nearly the same air a bit or two earlier */
            if (score >= 21 && (d->phase < 0 ||
                                (p != (unsigned)d->phase && score >= d->score[(unsigned)d->phase] + 2u))) {
                /* a better phase: send its sync again so the frame after it is found */
                d->phase = (int8_t)p;
                for (int j = 0; j < 24 && out < max; j++)
                    dibits[out++] = d->last24[p][(d->ring_at[p] + j) % 24];
                continue;
            }
            if (p == (unsigned)d->phase && out < max) dibits[out++] = dib;
        }
    }
    return out;
}
