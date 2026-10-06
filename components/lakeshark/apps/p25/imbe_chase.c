#include "imbe_chase.h"

/* mbelib's tables (ecc_const.h, defined in ecc.c) */
extern const int golayGenerator[12];
extern const int golayMatrix[2048];
extern const int hammingGenerator[4];
extern const int hammingMatrix[16];

/* What a changed bit weighs, by its doubt 0 (sure) .. 3: in proportion to
   the log-odds the symbol network's doubts carry on calls it never saw
   (about 6, 2.8, 1.5 and 0.42). Against 8 4 2 1, over 75 calls, with five
   positions: RTL frames exact 70.80 -> 70.94%, c0-c6 right 78.78 ->
   79.06%, garbage before the gate 4.96 -> 4.59%. */
uint8_t imbe_chase_weight[4] = { 13, 7, 4, 1 };

/* the most doubted positions a word's test patterns flip, up to 5 */
int imbe_chase_npos = 5;

static uint32_t golay_parity(uint32_t data)
{
    uint32_t p = 0;
    for (int k = 0; k < 12; k++)
        if (data & (1u << (11 - k))) p ^= (uint32_t)golayGenerator[k];
    return p;
}

uint32_t imbe_golay_encode(uint32_t data12)
{
    data12 &= 0xFFFu;
    return (data12 << 11) | golay_parity(data12);
}

/* the codeword mbe_golay2312 corrects a word to, parity included */
static uint32_t golay_decode(uint32_t block)
{
    uint32_t data = block >> 11;
    data ^= (uint32_t)golayMatrix[golay_parity(data) ^ (block & 0x7FFu)];
    return imbe_golay_encode(data);
}

static uint32_t hamming_decode(uint32_t block)
{
    uint32_t s = 0;
    for (int i = 0; i < 4; i++)
        s = (s << 1) | ((uint32_t)__builtin_popcount(block & (uint32_t)hammingGenerator[i]) & 1u);
    return s ? block ^ (uint32_t)hammingMatrix[s] : block;
}

static uint32_t chase(uint32_t word, const uint8_t *doubt, int nbits, uint32_t (*decode)(uint32_t))
{
    const int most = imbe_chase_npos < 1 ? 1 : imbe_chase_npos > 5 ? 5 : imbe_chase_npos;
    int pos[5], npos = 0;
    for (int d = 3; d >= 1 && npos < most; d--)
        for (int i = 0; i < nbits && npos < most; i++)
            if (doubt[i] == d) pos[npos++] = i;
    uint32_t best = 0, best_cost = UINT32_MAX;
    for (unsigned m = 0; m < (1u << npos); m++) {
        uint32_t t = word;
        for (int b = 0; b < npos; b++)
            if (m >> b & 1u) t ^= 1u << pos[b];
        const uint32_t cw = decode(t);
        uint32_t cost = 0;
        for (uint32_t diff = cw ^ word; diff; diff &= diff - 1)
            cost += imbe_chase_weight[doubt[__builtin_ctz(diff)] & 3u];
        if (cost < best_cost) {
            best_cost = cost;
            best = cw;
        }
    }
    return best;
}

uint32_t imbe_golay_correct(uint32_t word, const uint8_t doubt[23])
{
    return chase(word & 0x7FFFFFu, doubt, 23, golay_decode);
}

uint32_t imbe_hamming_correct(uint32_t word, const uint8_t doubt[15])
{
    return chase(word & 0x7FFFu, doubt, 15, hamming_decode);
}

static uint32_t row_word(const char *row, int nbits)
{
    uint32_t w = 0;
    for (int i = 0; i < nbits; i++) w |= (uint32_t)(row[i] & 1) << i;
    return w;
}

static void put_row(char *row, uint32_t w, int nbits)
{
    for (int i = 0; i < nbits; i++) row[i] = (char)((w >> i) & 1u);
}

int imbe_chase_c0(char fr[8][23], const uint8_t doubt[8][23], int *sure_changed)
{
    const uint32_t w = row_word(fr[0], 23);
    const uint32_t cw = imbe_golay_correct(w, doubt[0]);
    put_row(fr[0], cw, 23);
    if (sure_changed) {
        int n = 0;
        for (uint32_t diff = cw ^ w; diff; diff &= diff - 1)
            n += doubt[0][__builtin_ctz(diff)] == 0;
        *sure_changed = n;
    }
    return __builtin_popcount((cw ^ w) >> 11);
}

int imbe_chase_data(char fr[8][23], const uint8_t doubt[8][23])
{
    int errs = 0;
    for (int r = 1; r < 4; r++) {
        const uint32_t w = row_word(fr[r], 23);
        const uint32_t cw = imbe_golay_correct(w, doubt[r]);
        put_row(fr[r], cw, 23);
        errs += __builtin_popcount((cw ^ w) >> 11);
    }
    for (int r = 4; r < 7; r++) {
        const uint32_t w = row_word(fr[r], 15);
        const uint32_t cw = imbe_hamming_correct(w, doubt[r]);
        put_row(fr[r], cw, 15);
        errs += __builtin_popcount(cw ^ w);
    }
    return errs;
}
