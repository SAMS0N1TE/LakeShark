#include "p25_os4_decode.h"
#include "p25_os4_nn.h"
#include <string.h>

volatile int p25_os4_lut_on = 1;
volatile int p25_os4_nn_on = 1;

/* The frame sync's 24 symbols, oldest first: + + + + + - + + - - + + - - - - + - + - - - - - (all outer). */
static const uint32_t kSyncSigns = 0xFB30A0;   /* 1 = positive, first symbol in bit 23 */

void p25_os4_reset(p25_os4_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
    d->use_lut = p25_os4_lut_on != 0;
    d->use_nn = p25_os4_nn_on != 0;
    d->phase = -1;
    d->since_sync = -1;
    d->seam_left = -1;
    d->sent_at = -8;
    if (d->use_nn) p25_os4_nn_init();
}

void p25_os4_seam(p25_os4_decoder_t *d)
{
    const int32_t since = d->since_sync;
    const uint8_t use_lut = d->use_lut, use_nn = d->use_nn;
    /* the symbols kept back still go out first; bits count from 0 again,
       so no later sync reads the same air as any of them */
    uint8_t q[P25_OS4_Q];
    const uint8_t q_at = d->q_at, q_n = d->q_n;
    memcpy(q, d->q, sizeof(q));
    p25_os4_reset(d);
    d->use_lut = use_lut;
    d->use_nn = use_nn;
    memcpy(d->q, q, sizeof(q));
    d->q_at = q_at;
    d->q_n = q_n;
    for (int i = 0; i < P25_OS4_Q; i++) d->q_bit[i] = P25_OS4_NO_BIT;
    if (since >= 24 && since < 3 * P25_OS4_LDU)
        d->seam_left = (int16_t)(P25_OS4_LDU - since % P25_OS4_LDU);
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

/* Keep a symbol back (with its first bit, or P25_OS4_NO_BIT); a full queue
   sends its oldest. */
static size_t put_q(p25_os4_decoder_t *d, uint8_t dib, int32_t bit, uint8_t *dibits, size_t out, size_t max)
{
    if (d->q_n == P25_OS4_Q) {
        if (out < max) dibits[out++] = d->q[d->q_at];
        d->q_at = (uint8_t)((d->q_at + 1) % P25_OS4_Q);
        d->q_n--;
    }
    const unsigned w = (d->q_at + d->q_n) % P25_OS4_Q;
    d->q[w] = dib;
    d->q_bit[w] = bit;
    d->q_n++;
    return out;
}

/* Send the symbol that starts at bit s, unless the one sent last started
   under 3 bits before it: that was the same symbol, read at the phase next
   to this one. Says whether it went. */
static int send(p25_os4_decoder_t *d, int32_t s, uint8_t dib, uint8_t *dibits, size_t *out, size_t max)
{
    if (s - d->sent_at < 3) return 0;
    *out = put_q(d, dib, s, dibits, *out, max);
    d->sent_at = s;
    return 1;
}

/* Phase p's sync ended with the symbol at bit s: the symbols kept back that
   read the same air at the phase before (from 2 bits before the sync's
   first symbol on) give way to p's 24. */
static size_t take_sync(p25_os4_decoder_t *d, unsigned p, int32_t s, uint8_t *dibits, size_t out, size_t max)
{
    const int32_t from = s - 23 * 4 - 2;
    while (d->q_n) {
        const unsigned last = (d->q_at + d->q_n - 1u) % P25_OS4_Q;
        if (d->q_bit[last] == P25_OS4_NO_BIT || d->q_bit[last] < from) break;
        d->q_n--;
    }
    for (int j = 0; j < 24; j++)
        out = put_q(d, d->last24[p][(d->ring_at[p] + j) % 24], s - (23 - j) * 4, dibits, out, max);
    d->sent_at = s;
    return out;
}

/* While bridging: hold phase p's symbol (starting at bit s); on a sync,
   send the gap and the held symbols of that phase and lock to it. */
static size_t bridge(p25_os4_decoder_t *d, unsigned p, int32_t s, uint8_t dib, unsigned score,
                     uint8_t *dibits, size_t out, size_t max)
{
    /* a full hold no longer ends with the symbols that just scored */
    const int kept = d->held_n[p] < P25_OS4_HOLD;
    if (kept) d->held[p][d->held_n[p]++] = dib;
    if (kept && score >= 21 && d->held_n[p] >= 24) {
        /* the sync's first symbol is the 24th from the end of the hold */
        int gap = d->seam_left - ((int)d->held_n[p] - 24);
        while (gap < 0) gap += P25_OS4_LDU;
        if (gap > P25_OS4_GAP_MAX) gap = 0;
        for (int i = 0; i < gap; i++) out = put_q(d, P25_OS4_FILL, P25_OS4_NO_BIT, dibits, out, max);
        for (unsigned i = 0; i < d->held_n[p]; i++)
            out = put_q(d, d->held[p][i], s - 4 * (int32_t)(d->held_n[p] - 1u - i), dibits, out, max);
        d->phase = (int8_t)p;
        d->since_sync = 24;
        d->seam_left = -1;
        d->sent_at = s;
        memset(d->held_n, 0, sizeof(d->held_n));
    } else if (d->held_n[p] >= P25_OS4_HOLD && p == transition_phase(d)) {
        /* no sync after a whole hold: the call ended or this was no LDU */
        for (unsigned i = 0; i < d->held_n[p]; i++)
            out = put_q(d, d->held[p][i], s - 4 * (int32_t)(d->held_n[p] - 1u - i), dibits, out, max);
        d->seam_left = -1;
        d->sent_at = s;
        memset(d->held_n, 0, sizeof(d->held_n));
    }
    return out;
}

size_t p25_os4_decode(p25_os4_decoder_t *d, const uint8_t *bytes, size_t n,
                      uint8_t *dibits, size_t max)
{
    static const uint8_t kDibit[4] = { 3, 2, 0, 1 };   /* ones in the middle three: -3 -1 +1 +3 */
    /* how many bits after its start a symbol is decided */
    const unsigned delay = d->use_nn ? P25_OS4_NN_DELAY : 9u;
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        for (int k = 7; k >= 0; k--) {
            const uint8_t b = (bytes[i] >> k) & 1;
            const uint32_t at = d->pos++;
            if (at && b != d->last) d->trans[at & 3]++;
            d->last = b;
            d->win = (d->win << 1) | b;
            if ((d->pos & 4095) == 0)
                for (int p = 0; p < 4; p++) d->trans[p] >>= 1;
            if (at < delay) continue;
            /* the symbol starting at bit s, of phase s&3; its lookup key is
               the 6 bits before it (0 before the stream) and the 10 from its
               start */
            const int32_t s = (int32_t)(at - delay);
            const unsigned p = (unsigned)s & 3;
            const uint16_t key = (uint16_t)(d->win >> (delay - 9u));
            const unsigned ones = ((key >> 8) & 1) + ((key >> 7) & 1) + ((key >> 6) & 1);
            uint8_t dib = kDibit[ones];
            if (d->use_lut)
                dib = p25_os4_lut[key & (P25_OS4_LUT_SIZE - 1u)];
            /* the network for a symbol that goes out (or may, while a seam is
               bridged); the other phases only keep a sync's worth for a change */
            if (d->use_nn && !d->nn_off && (d->seam_left >= 0 || p == (unsigned)d->phase ||
                                            (d->phase < 0 && p == transition_phase(d))))
                dib = p25_os4_nn_byte(d->win);
            /* the sync is looked for in the symbols as decided: the
               middle-three rule's levels made 21 of 24 so seldom on the air
               that 70% of seams came with no sync to bridge from */
            const uint8_t lv = P25_OS4_DIBIT(dib);
            d->hist2[p] = (d->hist2[p] << 1) | (d->hist[p] >> 15 & 1);
            d->outer2[p] = (d->outer2[p] << 1) | (d->outer[p] >> 15 & 1);
            d->hist[p] = (d->hist[p] << 1) | (lv < 2);
            d->outer[p] = (d->outer[p] << 1) | (lv & 1);
            d->last24[p][d->ring_at[p]] = dib;
            d->ring_at[p] = (uint8_t)((d->ring_at[p] + 1) % 24);

            const unsigned score = sync_score(d, p);
            d->score[p] = (uint8_t)score;
            if (d->seam_left >= 0) {
                out = bridge(d, p, s, dib, score, dibits, out, max);
                continue;
            }
            if (d->phase < 0 && score < 21) {
                if (p == transition_phase(d)) send(d, s, dib, dibits, &out, max);
                continue;
            }
            /* a sync better by two than the phase in use made there: that
               phase from here on, and its sync in place of the one kept back */
            if (score >= 21 && (d->phase < 0 ||
                                (p != (unsigned)d->phase && score >= d->score[(unsigned)d->phase] + 2u))) {
                d->phase = (int8_t)p;
                out = take_sync(d, p, s, dibits, out, max);
                d->since_sync = 24;
                continue;
            }
            if (p == (unsigned)d->phase && send(d, s, dib, dibits, &out, max)) {
                if (score >= 21) d->since_sync = 24;
                else if (d->since_sync >= 0) d->since_sync++;
            }
        }
    }
    return out;
}
