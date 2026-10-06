/* p25_os4_decode's mechanics: it must lock to the symbol boundary from any
   start bit and read the level from the middle three of the symbol's bits
   (three ones +3, two +1, one -1, none -3). The streams here mark each
   boundary with a bit change, as the LR2021's 4x output mostly does. */
#include "ls_test.h"
#include "p25_os4_decode.h"
#include "p25_os4_nn.h"
#include "p25_os4_nn_vectors.h"
#include <math.h>
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

/* These streams are the rule's ideal patterns: the learned lookup and
   network are off. */
static void rule_only(void) { p25_os4_lut_on = 0; p25_os4_nn_on = 0; }

static int recovered(unsigned lead)
{
    rule_only();
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
    rule_only();
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
    rule_only();
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

/* A packet seam inside an LDU: the source loses 150 symbols and a few bits
   and starts again at another bit phase. The symbols after the seam wait for
   the next frame sync; then the gap goes out as filler, so every frame after
   the seam lands exactly an LDU after the one before, and the rest of the
   interrupted LDU comes back. */
LS_CASE(a_seam_inside_an_ldu_keeps_the_frames_in_place)
{
    rule_only();
    enum { FRAMES = 6, BODY = 840, LEN = 24 + BODY, TOTAL = FRAMES * LEN,
           CUT = 2 * LEN + 400, LOST = 150 };
    static const uint32_t kSync = 0xFB30A0;
    static const uint8_t kMid[4][3] = { {0,0,0}, {0,1,0}, {1,1,0}, {1,1,1} };
    static uint8_t dib[TOTAL], a[TOTAL / 2 + 8], b[TOTAL / 2 + 8], out[TOTAL + 2048];
    memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
    unsigned s = 29;
    for (int f = 0; f < FRAMES; f++) {
        for (int k = 0; k < 24; k++) dib[f * LEN + k] = (kSync >> (23 - k)) & 1 ? 1 : 3;
        for (int k = 24; k < LEN; k++) dib[f * LEN + k] = (uint8_t)(rng(&s) & 3);
    }
    /* part one ends on a byte; part two starts 150 symbols and 3 bits later */
    unsigned at = 0, bt = 0, cut_bits = 0;
    for (int k = 0; k < TOTAL; k++) {
        const int rot = (int)(rng(&s) % 3);
        uint8_t sym[4] = { (uint8_t)(rng(&s) & 1), 0, 0, 0 };
        for (int t2 = 0; t2 < 3; t2++) sym[1 + t2] = kMid[kOnes[dib[k]]][(t2 + rot) % 3];
        for (int q = 0; q < 4; q++) {
            if (k < CUT) { put(a, at++, sym[q]); continue; }
            if (k < CUT + LOST || (k == CUT + LOST && q < 3)) continue;
            put(b, bt++, sym[q]);
        }
        if (k == CUT - 1) cut_bits = at;
    }
    p25_os4_decoder_t d;
    p25_os4_reset(&d);
    size_t got = p25_os4_decode(&d, a, cut_bits / 8, out, sizeof(out));
    p25_os4_seam(&d);
    got += p25_os4_decode(&d, b, (bt + 7) / 8, &out[got], sizeof(out) - got);
    /* placed by frame 1, its sync and the start of its body: the first sync
       went out at the phase the bits suggested before any sync, and with a
       boundary bit of either value that can be the wrong one */
    size_t first = got;
    for (size_t i = LEN; i + 64 <= got && first == got; i++)
        if (!memcmp(&out[i], &dib[LEN], 64)) first = i - LEN;
    LS_CHECK_MSG(first < got, "frame 1 not found in %u symbols", (unsigned)got);
    if (first >= got) return;
    for (int f = 3; f < FRAMES; f++) {
        const size_t at_f = first + (size_t)f * LEN;
        LS_CHECK_MSG(at_f + 24 <= got && !memcmp(&out[at_f], &dib[f * LEN], 24),
                     "frame %d's sync is not an LDU after the last", f);
    }
    /* the interrupted LDU after the gap */
    int same = 0, n = 0;
    for (int k = CUT + LOST + 2; k < 3 * LEN; k++, n++) same += out[first + (size_t)k] == dib[k];
    LS_CHECK_MSG(same > n * 95 / 100, "%d of %d symbols after the gap", same, n);
    /* and the gap itself goes out as erasures, not as symbols */
    int erased = 0;
    for (int k = CUT; k < CUT + LOST; k++) erased += out[first + (size_t)k] == P25_OS4_FILL;
    LS_CHECK_MSG(erased >= LOST - 4, "%d of %d gap symbols sent as erasures", erased, LOST);
}

/* The transmitter's symbol clock and the LR2021's bit clock differ, so the
   symbol boundary walks across the bit phases and the phase in use changes
   at a sync now and then. No symbol may go out twice when it does: a sync
   sent again cost the LDU after it on the air (the frame reader read the
   copy as the NID). Here the boundary moves a bit later every 700 symbols;
   every sync that comes out must sit a whole number of LDUs after the last. */
LS_CASE(a_phase_change_sends_no_symbol_twice)
{
    rule_only();
    enum { FRAMES = 12, BODY = 840, LEN = 24 + BODY, TOTAL = FRAMES * LEN, SLIP = 700 };
    static const uint32_t kSync = 0xFB30A0;
    static const uint8_t kMid[4][3] = { {0,0,0}, {0,1,0}, {1,1,0}, {1,1,1} };
    static uint8_t dib[TOTAL], bytes[TOTAL / 2 + TOTAL / SLIP / 8 + 16], out[TOTAL + 256];
    memset(bytes, 0, sizeof(bytes));
    unsigned s = 41;
    for (int f = 0; f < FRAMES; f++) {
        for (int k = 0; k < 24; k++) dib[f * LEN + k] = (kSync >> (23 - k)) & 1 ? 1 : 3;
        for (int k = 24; k < LEN; k++) dib[f * LEN + k] = (uint8_t)(rng(&s) & 3);
    }
    unsigned at = 0;
    int last = 0;
    for (int k = 0; k < TOTAL; k++) {
        const int rot = (int)(rng(&s) % 3);
        put(bytes, at++, !last);
        for (int t = 0; t < 3; t++) {
            last = kMid[kOnes[dib[k]]][(t + rot) % 3];
            put(bytes, at++, last);
        }
        if (k % SLIP == SLIP - 1) put(bytes, at++, last);    /* the last bit held one longer */
    }
    p25_os4_decoder_t d;
    p25_os4_reset(&d);
    const size_t got = p25_os4_decode(&d, bytes, (at + 7) / 8, out, sizeof(out));
    long prev = -1;
    int syncs = 0, off = 0;
    for (size_t i = 0; i + 24 <= got; i++) {
        int same = 0;
        for (int k = 0; k < 24; k++) same += out[i + (size_t)k] == dib[k];
        if (same < 20 || (prev >= 0 && (long)i - prev < 3)) continue;
        if (prev >= 0 && ((long)i - prev) % LEN != 0) off++;
        prev = (long)i;
        syncs++;
    }
    LS_CHECK_MSG(syncs >= FRAMES - 2 && off == 0, "%d syncs out, %d not a whole number of LDUs after the last",
                 syncs, off);
}

/* The lookup is chosen at the reset and kept across a seam. app_p25.c turns
   the soft FEC on from that choice at every packet, so a reset that left it
   0 ran the LR2021 on the hard FEC with the lookup still deciding levels. */
LS_CASE(the_lookup_choice_is_taken_at_the_reset_and_kept_across_a_seam)
{
    static p25_os4_decoder_t d;
    p25_os4_lut_on = 1;
    p25_os4_reset(&d);
    LS_EQ_INT(d.use_lut, 1);
    p25_os4_seam(&d);
    LS_EQ_INT(d.use_lut, 1);
    p25_os4_lut_on = 0;
    p25_os4_reset(&d);
    LS_EQ_INT(d.use_lut, 0);
    p25_os4_lut_on = 1;
}

/* The board runs the network export_nn.py trained: its probabilities for
   the windows it was exported with. */
LS_CASE(the_network_gives_the_trained_probabilities)
{
    const int n = (int)(sizeof(kNnVectors) / sizeof(kNnVectors[0]));
    float worst = 0.0f;
    for (int i = 0; i < n; i++) {
        float p[4];
        p25_os4_nn_probs(kNnVectors[i].win, p);
        for (int c = 0; c < 4; c++) {
            const float e = fabsf(p[c] - kNnVectors[i].p[c]);
            if (e > worst) worst = e;
        }
    }
    LS_CHECK_MSG(worst < 1e-3f, "largest difference %g over %d windows", (double)worst, n);
}

/* Its byte: the likeliest dibit, and a doubt on a bit it is unsure of. */
LS_CASE(the_network_byte_is_its_likeliest_dibit_with_doubts)
{
    const int n = (int)(sizeof(kNnVectors) / sizeof(kNnVectors[0]));
    int doubted = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t b = p25_os4_nn_byte(kNnVectors[i].win);
        int best = 0;
        for (int c = 1; c < 4; c++)
            if (kNnVectors[i].p[c] > kNnVectors[i].p[best]) best = c;
        LS_EQ_INT(P25_OS4_DIBIT(b), best);
        doubted += P25_OS4_DOUBTS(b) != 0;
    }
    LS_CHECK_MSG(doubted > 0 && doubted < n, "%d of %d bytes doubted", doubted, n);
}

/* The network needs bits further ahead, so each symbol is decided
   P25_OS4_NN_DELAY bits after its start instead of nine: on the same bits,
   that many fewer symbols are out yet, and none is lost. */
LS_CASE(the_network_decides_later_and_drops_nothing)
{
    enum { SYMS = 3000 };
    static uint8_t dib[SYMS], bytes[SYMS / 2 + 8], a[SYMS + 64], b[SYMS + 64];
    memset(bytes, 0, sizeof(bytes));
    unsigned s = 17;
    for (int k = 0; k < SYMS; k++) dib[k] = (uint8_t)(rng(&s) & 3);
    const unsigned bits = detector(dib, SYMS, 0, &s, bytes);
    static p25_os4_decoder_t d;
    p25_os4_lut_on = 1;
    p25_os4_nn_on = 0;
    p25_os4_reset(&d);
    const size_t na = p25_os4_decode(&d, bytes, (bits + 7) / 8, a, sizeof(a));
    p25_os4_nn_on = 1;
    p25_os4_reset(&d);
    LS_EQ_INT(d.use_nn, 1);
    const size_t nb = p25_os4_decode(&d, bytes, (bits + 7) / 8, b, sizeof(b));
    const int behind = (int)na - (int)nb;
    LS_CHECK_MSG(behind >= (P25_OS4_NN_DELAY - 9) / 4 && behind <= (P25_OS4_NN_DELAY - 9) / 4 + 1,
                 "%d symbols behind the lookup", behind);
    p25_os4_seam(&d);
    LS_EQ_INT(d.use_nn, 1);
}
