/* imbe_chase: soft-decision Golay(23,12) and Hamming(15,11) correction.
   With no doubts it must be mbelib's hard decode; with the receiver's doubts
   it must reach the sent codeword past the hard decode's reach when the
   errors sit on doubted bits. */
#include "ls_test.h"
#include "imbe_chase.h"
#include <string.h>

extern const int hammingGenerator[4];

static unsigned rng(unsigned *s)
{
    *s = *s * 1103515245u + 12345u;
    return (*s >> 8) & 0xFFFFFFu;
}

static uint32_t hamming_encode(uint32_t data11)
{
    for (uint32_t p = 0; p < 16; p++) {
        const uint32_t w = (data11 << 4) | p;
        uint32_t s = 0;
        for (int i = 0; i < 4; i++)
            s = (s << 1) | ((uint32_t)__builtin_popcount(w & (uint32_t)hammingGenerator[i]) & 1u);
        if (!s) return w;
    }
    return 0;
}

LS_CASE(with_no_doubts_golay_is_the_hard_decode)
{
    static const uint8_t none[23];
    unsigned s = 7;
    int bad = 0;
    for (int n = 0; n < 2000; n++) {
        const uint32_t cw = imbe_golay_encode(rng(&s) & 0xFFFu);
        uint32_t e = 0;
        for (int k = (int)(rng(&s) % 4); k > 0; k--) e |= 1u << (rng(&s) % 23);
        bad += imbe_golay_correct(cw ^ e, none) != cw;     /* up to 3 errors: fixed */
    }
    LS_EQ_INT(bad, 0);
}

LS_CASE(four_golay_errors_on_doubted_bits_are_corrected)
{
    unsigned s = 11;
    int hard_ok = 0, soft_ok = 0;
    for (int n = 0; n < 500; n++) {
        const uint32_t cw = imbe_golay_encode(rng(&s) & 0xFFFu);
        uint8_t doubt[23] = {0};
        static const uint8_t none[23];
        int at[4];
        for (int k = 0; k < 4; k++) {
            int p;
            do { p = (int)(rng(&s) % 23); } while (k && (p == at[0] || (k > 1 && p == at[1]) || (k > 2 && p == at[2])));
            at[k] = p;
        }
        /* two of the four on the most doubted bits, the other two plain */
        doubt[at[0]] = 3;
        doubt[at[1]] = 3;
        doubt[(at[0] + 11) % 23] = doubt[(at[0] + 11) % 23] ? doubt[(at[0] + 11) % 23] : 1;
        uint32_t rx = cw;
        for (int k = 0; k < 4; k++) rx ^= 1u << at[k];
        hard_ok += imbe_golay_correct(rx, none) == cw;
        soft_ok += imbe_golay_correct(rx, doubt) == cw;
    }
    ls_note("four errors: hard %d of 500, Chase %d of 500", hard_ok, soft_ok);
    LS_EQ_INT(hard_ok, 0);
    LS_CHECK(soft_ok >= 490);
}

/* Two errors on doubted bits: past Hamming's one. (With one of the two on a
   sure bit, the codeword one sure bit away is the likelier, and Chase rightly
   keeps it.) */
LS_CASE(two_hamming_errors_on_doubted_bits_are_corrected)
{
    unsigned s = 13;
    int hard_ok = 0, soft_ok = 0;
    static const uint8_t none[15];
    int one_bad = 0;
    for (int n = 0; n < 500; n++) {
        const uint32_t cw = hamming_encode(rng(&s) & 0x7FFu);
        const int a = (int)(rng(&s) % 15);
        int b;
        do { b = (int)(rng(&s) % 15); } while (b == a);
        uint8_t doubt[15] = {0};
        doubt[a] = 3;
        doubt[b] = 2;
        const uint32_t rx = cw ^ (1u << a) ^ (1u << b);
        hard_ok += imbe_hamming_correct(rx, none) == cw;
        soft_ok += imbe_hamming_correct(rx, doubt) == cw;
        one_bad += imbe_hamming_correct(cw ^ (1u << b), none) != cw;
    }
    ls_note("two errors on doubted bits: hard %d of 500, Chase %d of 500", hard_ok, soft_ok);
    LS_EQ_INT(one_bad, 0);
    LS_EQ_INT(hard_ok, 0);
    LS_EQ_INT(soft_ok, 500);
}

LS_CASE(a_frame_is_left_as_codewords_and_counts_what_changed)
{
    char fr[8][23];
    uint8_t doubt[8][23];
    memset(fr, 0, sizeof(fr));
    memset(doubt, 0, sizeof(doubt));
    unsigned s = 17;
    const uint32_t c0 = imbe_golay_encode(rng(&s) & 0xFFFu);
    for (int i = 0; i < 23; i++) fr[0][i] = (char)((c0 >> i) & 1u);
    fr[0][15] ^= 1;   /* a data bit */
    fr[0][3] ^= 1;    /* a parity bit */
    int sure = -1;
    LS_EQ_INT(imbe_chase_c0(fr, doubt, &sure), 1);        /* data bits, as mbelib counts */
    LS_EQ_INT(sure, 2);                                   /* both were sure bits */
    uint32_t w = 0;
    for (int i = 0; i < 23; i++) w |= (uint32_t)fr[0][i] << i;
    LS_EQ_INT((int)w, (int)c0);

    /* the same two errors on doubted bits: changed, but none of them sure */
    fr[0][15] ^= 1;
    fr[0][3] ^= 1;
    doubt[0][15] = doubt[0][3] = 3;
    LS_EQ_INT(imbe_chase_c0(fr, doubt, &sure), 1);
    LS_EQ_INT(sure, 0);
}
