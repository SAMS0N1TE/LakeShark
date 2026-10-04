/* p25_os4_decode's mechanics: it must lock to the symbol boundary from any
   start bit and read the level from the middle three of the symbol's bits
   (three ones +3, two +1, one -1, none -3). The streams here mark each
   boundary with a bit change, as the LR2021's 4x output mostly does. */
#include "ls_test.h"
#include "p25_os4_decode.h"
#include <string.h>

enum { N = 6000 };
static const int kOnes[4] = { 2, 3, 1, 0 };          /* dibit 0 +1, 1 +3, 2 -1, 3 -3 */

static unsigned rng(unsigned *s)
{
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7fff;
}

static void put(uint8_t *bytes, unsigned at, int bit)
{
    if (bit) bytes[at / 8] |= (uint8_t)(0x80 >> (at % 8));
}

/* lead bits of noise first, so the decoder has to find the phase */
static unsigned detector(const uint8_t *dib, int n, unsigned lead, unsigned *s, uint8_t *bytes)
{
    unsigned at = 0;
    for (unsigned i = 0; i < lead; i++) put(bytes, at++, (int)(rng(s) & 1));
    int last = 0;
    for (int k = 0; k < n; k++) {
        const int ones = kOnes[dib[k]];
        static const uint8_t kMid[4][3] = { {0,0,0}, {0,1,0}, {1,1,0}, {1,1,1} };
        const int rot = (int)(rng(s) % 3);                /* where the ones of +-1 fall */
        put(bytes, at++, !last);                          /* the boundary bit */
        for (int t = 0; t < 3; t++) {
            last = kMid[ones][(t + rot) % 3];
            put(bytes, at++, last);
        }
    }
    return at;
}

static int recovered(unsigned lead)
{
    static uint8_t dib[N], bytes[N + 8], out[N + 8];
    memset(bytes, 0, sizeof(bytes));
    unsigned s = 11;
    for (int k = 0; k < N; k++) dib[k] = (uint8_t)(rng(&s) & 3);
    const unsigned bits = detector(dib, N, lead, &s, bytes);
    p25_os4_decoder_t d;
    p25_os4_reset(&d);
    const size_t got = p25_os4_decode(&d, bytes, (bits + 7) / 8, out, sizeof(out));
    /* align the decoded run to the sent one by its best offset in the first symbols */
    int best = 0;
    for (int off = 0; off < 8; off++) {
        int same = 0;
        for (size_t k = 400; k + off < got && k < (size_t)N; k++) same += out[k + off] == dib[k];
        if (same > best) best = same;
    }
    return best;
}

LS_CASE(levels_come_back_from_the_middle_bits)
{
    const int same = recovered(0);
    LS_CHECK_MSG(same > (N - 400) * 97 / 100, "%d of %d symbols", same, N - 400);
}

LS_CASE(the_symbol_phase_is_found_from_any_start)
{
    for (unsigned lead = 1; lead < 4; lead++) {
        const int same = recovered(lead);
        LS_CHECK_MSG(same > (N - 400) * 97 / 100, "lead %u: %d of %d symbols", lead, same, N - 400);
    }
}

LS_CASE(bytes_split_across_calls_give_the_same_symbols)
{
    static uint8_t bytes[64], a[300], b[300];
    unsigned s = 5;
    for (int i = 0; i < 64; i++) bytes[i] = (uint8_t)rng(&s);
    p25_os4_decoder_t d;
    p25_os4_reset(&d);
    const size_t na = p25_os4_decode(&d, bytes, 64, a, sizeof(a));
    p25_os4_reset(&d);
    size_t nb = 0;
    for (int i = 0; i < 64; i++) nb += p25_os4_decode(&d, &bytes[i], 1, &b[nb], sizeof(b) - nb);
    LS_EQ_INT((int)nb, (int)na);
    LS_CHECK(memcmp(a, b, na) == 0);
}

/* With no bit change at the boundaries, the bits cannot say where a symbol
   starts; the frame syncs must. Each frame is the 24-symbol sync and a body
   of random symbols, and from the first sync on the frames come back. */
LS_CASE(the_frame_sync_sets_the_phase_when_the_bits_cannot)
{
    enum { FRAMES = 12, BODY = 840, LEN = 24 + BODY, TOTAL = FRAMES * LEN };
    static const uint32_t kSync = 0xFB30A0;                 /* first symbol in bit 23, 1 = +3 */
    static const uint8_t kMid[4][3] = { {0,0,0}, {0,1,0}, {1,1,0}, {1,1,1} };
    static uint8_t dib[TOTAL], bytes[TOTAL / 2 + 8], out[TOTAL + 64];
    memset(bytes, 0, sizeof(bytes));
    unsigned s = 23;
    for (int f = 0; f < FRAMES; f++) {
        for (int k = 0; k < 24; k++) dib[f * LEN + k] = (kSync >> (23 - k)) & 1 ? 1 : 3;
        for (int k = 24; k < LEN; k++) dib[f * LEN + k] = (uint8_t)(rng(&s) & 3);
    }
    unsigned at = 3;                                          /* three bits of lead */
    for (int k = 0; k < TOTAL; k++) {
        const int rot = (int)(rng(&s) % 3);
        put(bytes, at++, (int)(rng(&s) & 1));                 /* a boundary bit of either value */
        for (int t = 0; t < 3; t++) put(bytes, at++, kMid[kOnes[dib[k]]][(t + rot) % 3]);
    }
    p25_os4_decoder_t d;
    p25_os4_reset(&d);
    const size_t got = p25_os4_decode(&d, bytes, (at + 7) / 8, out, sizeof(out));
    size_t first = got;
    for (size_t i = 0; i + 24 <= got && first == got; i++)
        if (!memcmp(&out[i], dib, 24)) first = i;
    LS_CHECK_MSG(first < got, "no frame sync in %u symbols", (unsigned)got);
    if (first >= got) return;
    int same = 0, n = 0;
    for (size_t k = 0; first + k < got && k < (size_t)TOTAL; k++, n++) same += out[first + k] == dib[k];
    LS_CHECK_MSG(n > TOTAL * 9 / 10 && same > n * 97 / 100, "%d of %d symbols after the first sync", same, n);
}
