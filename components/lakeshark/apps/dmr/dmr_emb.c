/* Embedded signalling in voice bursts B-F: EMB and the embedded LC. */
#include "dmr.h"

#include <string.h>

static int rd_bit(const uint8_t *buf, unsigned idx)
{
    return (buf[idx >> 3] >> (7u - (idx & 7u))) & 1u;
}

static int popcount16(uint32_t v)
{
    int n = 0;
    for (v &= 0xFFFFu; v; v &= v - 1u) n++;
    return n;
}

/* ---------------------------------------------------------------- EMB ---
 * Systematic: the seven data bits on top, x^8 * d(x) mod g(x) below them in
 * 15 bits, then a sixteenth bit making the weight even. */
#define QR_GEN 0x139u                    /* x^8 + x^5 + x^4 + x^3 + 1 */

uint16_t dmr_emb_encode(uint8_t colour_code, bool pi, uint8_t lcss)
{
    const uint32_t d = ((uint32_t)(colour_code & 0xFu) << 3) |
                       ((pi ? 1u : 0u) << 2) | (lcss & 3u);
    uint32_t r = d << 8;
    for (int bit = 14; bit >= 8; bit--)
        if (r & (1u << bit)) r ^= QR_GEN << (bit - 8);
    const uint32_t cw15 = (d << 8) | (r & 0xFFu);
    return (uint16_t)((cw15 << 1) | (popcount16(cw15) & 1u));
}

int dmr_emb_decode(uint16_t emb, uint8_t *cc, bool *pi, uint8_t *lcss)
{
    /* 128 codewords at distance 6: two errors are always nearer one word
       than any other, three may not be. */
    int best = 99;
    uint8_t best_d = 0;
    for (unsigned d = 0; d < 128; d++) {
        const int e = popcount16((uint32_t)emb ^
                                 dmr_emb_encode((uint8_t)(d >> 3), (d >> 2) & 1u, (uint8_t)(d & 3u)));
        if (e < best) { best = e; best_d = (uint8_t)d; if (!e) break; }
    }
    if (best > 2) return -1;
    if (cc)   *cc   = best_d >> 3;
    if (pi)   *pi   = (best_d >> 2) & 1u;
    if (lcss) *lcss = best_d & 3u;
    return best;
}

/* EMB straddles the embedded fragment: burst bits 108-115 and 148-155, the
   fragment 116-147, together where a sync would otherwise be. */
uint16_t dmr_burst_emb(const uint8_t burst[DMR_BURST_BITS / 8])
{
    uint16_t v = 0;
    if (!burst) return 0;
    for (unsigned i = 0; i < 8; i++)  v = (uint16_t)((v << 1) | rd_bit(burst, 108u + i));
    for (unsigned i = 0; i < 8; i++)  v = (uint16_t)((v << 1) | rd_bit(burst, 148u + i));
    return v;
}

uint32_t dmr_burst_embedded(const uint8_t burst[DMR_BURST_BITS / 8])
{
    uint32_t v = 0;
    if (!burst) return 0;
    for (unsigned i = 0; i < 32; i++) v = (v << 1) | (uint32_t)rd_bit(burst, 116u + i);
    return v;
}

/* --------------------------------------------------------- embedded LC ---
 * Matrix M[8][16], row major. Rows 0-6: eleven data columns then five
 * Hamming(16,11,4) parity - the BPTC row code's four plus overall parity.
 * Row 7: even parity down each column. Data: rows 0 and 1 carry eleven LC
 * bits each, rows 2-6 ten LC bits and one checksum bit (MSB in row 2) in
 * column 10. Sent column by column: wire bit k is M[k % 8][k / 8]. */

static void hamming16_parity(const uint8_t d[11], uint8_t p[5])
{
    p[0] = (uint8_t)(d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8]);
    p[1] = (uint8_t)(d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9]);
    p[2] = (uint8_t)(d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10]);
    p[3] = (uint8_t)(d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10]);
    p[4] = (uint8_t)(d[0] ^ d[2] ^ d[5] ^ d[6] ^ d[8] ^ d[9] ^ d[10]);
}

/* Single errors corrected, double detected: the four (15,11) syndromes
   locate, the overall parity says whether there was one error or two. */
static int hamming16_correct(uint8_t row[16])
{
    uint8_t p[5];
    hamming16_parity(row, p);
    unsigned s = 0;
    for (unsigned i = 0; i < 4; i++) if (p[i] != row[11 + i]) s |= 1u << i;
    unsigned overall = 0;
    for (unsigned i = 0; i < 16; i++) overall ^= row[i];
    if (!s && !overall) return 0;
    if (!overall) return -1;                     /* even weight: two errors */
    if (!s) { row[15] ^= 1u; return 1; }         /* the overall bit itself  */
    /* The data columns' syndromes, as hamming16_parity() toggles them. */
    static const uint8_t syn[11] = { 0x9, 0xB, 0xF, 0x7, 0xE, 0x5, 0xA, 0xD, 0x3, 0x6, 0xC };
    for (unsigned i = 0; i < 11; i++) if (syn[i] == s) { row[i] ^= 1u; return 1; }
    for (unsigned i = 0; i < 4; i++) if (s == (1u << i)) { row[11 + i] ^= 1u; return 1; }
    return -1;
}

static unsigned lc_checksum(const uint8_t lc9[DMR_LC_BYTES])
{
    unsigned sum = 0;
    for (unsigned i = 0; i < DMR_LC_BYTES; i++) sum += lc9[i];
    return sum % 31u;
}

/* Where LC bit i sits in the matrix, and where checksum bit j does. */
static void lc_position(unsigned i, unsigned *row, unsigned *col)
{
    if (i < 22) { *row = i / 11; *col = i % 11; return; }
    *row = 2 + (i - 22) / 10;
    *col = (i - 22) % 10;
}

void dmr_emb_lc_encode(const uint8_t lc9[DMR_LC_BYTES], uint32_t frag[4])
{
    uint8_t M[8][16];
    memset(M, 0, sizeof(M));
    for (unsigned i = 0; i < 72; i++) {
        unsigned r, c;
        lc_position(i, &r, &c);
        M[r][c] = (uint8_t)rd_bit(lc9, i);
    }
    const unsigned cs = lc_checksum(lc9);
    for (unsigned j = 0; j < 5; j++) M[2 + j][10] = (uint8_t)((cs >> (4 - j)) & 1u);
    for (unsigned r = 0; r < 7; r++) hamming16_parity(M[r], &M[r][11]);
    for (unsigned c = 0; c < 16; c++) {
        uint8_t v = 0;
        for (unsigned r = 0; r < 7; r++) v ^= M[r][c];
        M[7][c] = v;
    }
    for (unsigned f = 0; f < 4; f++) {
        uint32_t v = 0;
        for (unsigned k = f * 32; k < f * 32 + 32; k++) v = (v << 1) | M[k % 8][k / 8];
        frag[f] = v;
    }
}

int dmr_emb_lc_decode(const uint32_t frag[4], uint8_t lc9[DMR_LC_BYTES])
{
    if (!frag || !lc9) return -1;
    uint8_t M[8][16];
    for (unsigned k = 0; k < 128; k++)
        M[k % 8][k / 8] = (uint8_t)((frag[k / 32] >> (31 - k % 32)) & 1u);

    int fixed = 0;
    for (unsigned r = 0; r < 7; r++) {
        const int e = hamming16_correct(M[r]);
        if (e < 0) return -1;
        fixed += e;
    }
    for (unsigned c = 0; c < 16; c++) {
        uint8_t v = 0;
        for (unsigned r = 0; r < 8; r++) v ^= M[r][c];
        if (v) return -1;
    }
    uint8_t out[DMR_LC_BYTES];
    memset(out, 0, sizeof(out));
    for (unsigned i = 0; i < 72; i++) {
        unsigned r, c;
        lc_position(i, &r, &c);
        if (M[r][c]) out[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
    }
    unsigned cs = 0;
    for (unsigned j = 0; j < 5; j++) cs = (cs << 1) | M[2 + j][10];
    if (cs != lc_checksum(out)) return -1;
    memcpy(lc9, out, sizeof(out));
    return fixed;
}

void dmr_emb_asm_reset(dmr_emb_asm_t *a)
{
    if (a) memset(a, 0, sizeof(*a));
}

int dmr_emb_asm_feed(dmr_emb_asm_t *a, uint8_t lcss, uint32_t frag,
                     uint8_t lc9[DMR_LC_BYTES])
{
    if (!a) return 0;
    switch (lcss & 3u) {
    case DMR_LCSS_FIRST:
        a->frag[0] = frag;
        a->have = 1;
        return 0;
    case DMR_LCSS_CONT:
        if (a->have < 1 || a->have > 2) { a->have = 0; return 0; }
        a->frag[a->have++] = frag;
        return 0;
    case DMR_LCSS_LAST:
        if (a->have != 3) { a->have = 0; return 0; }
        a->frag[3] = frag;
        a->have = 0;
        return dmr_emb_lc_decode(a->frag, lc9) >= 0 ? 1 : -1;
    default:
        /* A single-fragment burst (F's reverse channel or null) sits
           outside an LC and leaves the assembly alone. */
        return 0;
    }
}
