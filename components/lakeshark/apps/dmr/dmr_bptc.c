#include "dmr.h"

#include <string.h>

/* BPTC(196,96) - ETSI TS 102 361-1 Annex B.1.
 *
 * Layout after deinterleave:
 *   bit 0 = R(3) spare (always 0)
 *   bits 1..195 = 13 rows x 15 columns matrix M[r][c] at index (1 + r*15 + c)
 *
 * Rows 0..8 are Hamming(15,11,3) codewords (11 data + 4 parity)
 * Columns 0..14 are Hamming(13,9,3) codewords (9 data + 4 parity)
 *
 * The 96 information bits I(0)..I(95) are laid into cells row-major starting
 * at row 0, column 3 (the first three cells of row 0 are the R(0..2) spares).
 *
 * Interleave permutation: index_out = (index_in * 181) mod 196.  Its inverse
 * is *13 mod 196 (Bezout: 13*181 - 12*196 = 1). */

/* ---- bit helpers ------------------------------------------------------- */

static int rd_bit(const uint8_t *buf, unsigned int idx)
{
    return (buf[idx >> 3] >> (7u - (idx & 7u))) & 1u;
}

static void wr_bit(uint8_t *buf, unsigned int idx, int v)
{
    uint8_t mask = (uint8_t)(1u << (7u - (idx & 7u)));
    if (v) buf[idx >> 3] |= mask;
    else   buf[idx >> 3] &= (uint8_t)~mask;
}

/* ---- Hamming(15,11,3) - matches MMDVMHost Hamming::decode15113_2 -------- */

static void hamming_15_11_3_parity(const uint8_t d[11], uint8_t p_out[4])
{
    /* p0..p3 land at row positions 11..14. Data d[0..10] at 0..10. */
    p_out[0] = (uint8_t)(d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8]);
    p_out[1] = (uint8_t)(d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9]);
    p_out[2] = (uint8_t)(d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10]);
    p_out[3] = (uint8_t)(d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10]);
}

/* Returns -1 on uncorrectable double error, else the number of bit flips
 * applied to `row` (0 or 1). The syndrome-to-position table is derived
 * directly from hamming_15_11_3_parity() above: for each error position i,
 * the syndrome is the bit pattern of parity bits that toggle when position
 * i is flipped. */
static int hamming_15_11_3_correct(uint8_t row[15])
{
    uint8_t p[4];
    hamming_15_11_3_parity(row, p);
    unsigned s = 0;
    if (p[0] != row[11]) s |= 0x1;
    if (p[1] != row[12]) s |= 0x2;
    if (p[2] != row[13]) s |= 0x4;
    if (p[3] != row[14]) s |= 0x8;
    switch (s) {
    case 0x0: return 0;
    /* data-bit flips: syndromes as toggled parity-bit sets */
    case 0x9: row[ 0] ^= 1u; return 1;   /* d0 -> p0,p3 */
    case 0xB: row[ 1] ^= 1u; return 1;   /* d1 -> p0,p1,p3 */
    case 0xF: row[ 2] ^= 1u; return 1;   /* d2 -> p0,p1,p2,p3 */
    case 0x7: row[ 3] ^= 1u; return 1;   /* d3 -> p0,p1,p2 */
    case 0xE: row[ 4] ^= 1u; return 1;   /* d4 -> p1,p2,p3 */
    case 0x5: row[ 5] ^= 1u; return 1;   /* d5 -> p0,p2 */
    case 0xA: row[ 6] ^= 1u; return 1;   /* d6 -> p1,p3 */
    case 0xD: row[ 7] ^= 1u; return 1;   /* d7 -> p0,p2,p3 */
    case 0x3: row[ 8] ^= 1u; return 1;   /* d8 -> p0,p1 */
    case 0x6: row[ 9] ^= 1u; return 1;   /* d9 -> p1,p2 */
    case 0xC: row[10] ^= 1u; return 1;   /* d10 -> p2,p3 */
    /* parity-bit flips */
    case 0x1: row[11] ^= 1u; return 1;   /* p0 */
    case 0x2: row[12] ^= 1u; return 1;   /* p1 */
    case 0x4: row[13] ^= 1u; return 1;   /* p2 */
    case 0x8: row[14] ^= 1u; return 1;   /* p3 */
    default:  return -1;
    }
}

/* ---- Hamming(13,9,3) - matches MMDVMHost Hamming::decode1393 ----------- */

static void hamming_13_9_3_parity(const uint8_t d[9], uint8_t p_out[4])
{
    p_out[0] = (uint8_t)(d[0] ^ d[1] ^ d[3] ^ d[5] ^ d[6]);
    p_out[1] = (uint8_t)(d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7]);
    p_out[2] = (uint8_t)(d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8]);
    p_out[3] = (uint8_t)(d[0] ^ d[2] ^ d[4] ^ d[5] ^ d[6] ^ d[8]);
}

static int hamming_13_9_3_correct(uint8_t col[13])
{
    uint8_t p[4];
    hamming_13_9_3_parity(col, p);
    unsigned s = 0;
    if (p[0] != col[ 9]) s |= 0x1;
    if (p[1] != col[10]) s |= 0x2;
    if (p[2] != col[11]) s |= 0x4;
    if (p[3] != col[12]) s |= 0x8;
    switch (s) {
    case 0x0: return 0;
    case 0xF: col[0] ^= 1u; return 1;   /* d0 -> p0,p1,p2,p3 */
    case 0x7: col[1] ^= 1u; return 1;   /* d1 -> p0,p1,p2 */
    case 0xE: col[2] ^= 1u; return 1;   /* d2 -> p1,p2,p3 */
    case 0x5: col[3] ^= 1u; return 1;   /* d3 -> p0,p2 */
    case 0xA: col[4] ^= 1u; return 1;   /* d4 -> p1,p3 */
    case 0xD: col[5] ^= 1u; return 1;   /* d5 -> p0,p2,p3 */
    case 0xB: col[6] ^= 1u; return 1;   /* d6 -> p0,p1,p3 */
    case 0x6: col[7] ^= 1u; return 1;   /* d7 -> p1,p2 */
    case 0xC: col[8] ^= 1u; return 1;   /* d8 -> p2,p3 */
    case 0x1: col[ 9] ^= 1u; return 1;   /* p0 */
    case 0x2: col[10] ^= 1u; return 1;   /* p1 */
    case 0x4: col[11] ^= 1u; return 1;   /* p2 */
    case 0x8: col[12] ^= 1u; return 1;   /* p3 */
    default:  return -1;
    }
}

/* ---- matrix <-> raw stream --------------------------------------------- */

static void raw_to_matrix(const uint8_t raw[196], uint8_t M[13][15])
{
    /* raw[0] is R(3) - ignored. */
    for (unsigned r = 0; r < 13; r++)
        for (unsigned c = 0; c < 15; c++)
            M[r][c] = raw[1u + r * 15u + c];
}

static void matrix_to_raw(const uint8_t M[13][15], uint8_t raw[196])
{
    raw[0] = 0;                                /* R(3) */
    for (unsigned r = 0; r < 13; r++)
        for (unsigned c = 0; c < 15; c++)
            raw[1u + r * 15u + c] = M[r][c];
}

static void unpack_bits(const uint8_t *packed, uint8_t *bits, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        bits[i] = (uint8_t)rd_bit(packed, i);
}

static void pack_bits(const uint8_t *bits, uint8_t *packed, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        wr_bit(packed, i, bits[i]);
}

/* ---- BPTC encode ------------------------------------------------------- */

void dmr_bptc_encode(const uint8_t data[12], uint8_t coded[DMR_BPTC_BITS / 8 + 1])
{
    uint8_t bits96[96];
    uint8_t M[13][15];
    uint8_t raw[196];
    uint8_t out_raw[196];

    unpack_bits(data, bits96, DMR_LC_BITS);

    memset(M, 0, sizeof(M));

    /* Fill data cells (row-major from row 0 col 3, skipping R(0..2)). */
    unsigned p = 0;
    for (unsigned r = 0; r < 9; r++) {
        for (unsigned c = 0; c < 11; c++) {
            if (r == 0 && c < 3) continue;         /* R(0), R(1), R(2) */
            M[r][c] = bits96[p++];
        }
    }
    /* p should be 96 now. */

    /* Row parity for rows 0..8. */
    for (unsigned r = 0; r < 9; r++) {
        uint8_t par[4];
        hamming_15_11_3_parity(M[r], par);
        M[r][11] = par[0];
        M[r][12] = par[1];
        M[r][13] = par[2];
        M[r][14] = par[3];
    }
    /* Column parity for every column across rows 0..8 -> rows 9..12. */
    for (unsigned c = 0; c < 15; c++) {
        uint8_t col_data[9];
        uint8_t par[4];
        for (unsigned r = 0; r < 9; r++) col_data[r] = M[r][c];
        hamming_13_9_3_parity(col_data, par);
        M[ 9][c] = par[0];
        M[10][c] = par[1];
        M[11][c] = par[2];
        M[12][c] = par[3];
    }

    matrix_to_raw(M, raw);

    /* Interleave: out_raw[(a*181)%196] = raw[a]. */
    memset(out_raw, 0, sizeof(out_raw));
    for (unsigned a = 0; a < 196; a++) {
        unsigned j = (a * 181u) % 196u;
        out_raw[j] = raw[a];
    }

    memset(coded, 0, DMR_BPTC_BITS / 8 + 1);
    pack_bits(out_raw, coded, 196);
}

/* ---- BPTC decode ------------------------------------------------------- */

int dmr_bptc_decode(const uint8_t coded[DMR_BPTC_BITS / 8 + 1],
                    uint8_t data_out[12])
{
    uint8_t in_bits[196];
    uint8_t raw[196];
    uint8_t M[13][15];
    int corrections = 0;

    unpack_bits(coded, in_bits, 196);

    /* Deinterleave: raw[a] = in_bits[(a*181)%196]. */
    for (unsigned a = 0; a < 196; a++) {
        unsigned j = (a * 181u) % 196u;
        raw[a] = in_bits[j];
    }
    raw_to_matrix(raw, M);

    /* First pass: correct data rows. */
    for (unsigned r = 0; r < 9; r++) {
        int c = hamming_15_11_3_correct(M[r]);
        if (c < 0) return -1;
        corrections += c;
    }
    /* Correct columns using row-corrected data. */
    for (unsigned col = 0; col < 15; col++) {
        uint8_t colbits[13];
        for (unsigned r = 0; r < 13; r++) colbits[r] = M[r][col];
        int c = hamming_13_9_3_correct(colbits);
        if (c < 0) return -1;
        corrections += c;
        for (unsigned r = 0; r < 13; r++) M[r][col] = colbits[r];
    }
    /* Second row pass sweeps residual data-row flips introduced when a column
     * fix touched a row's data bit; a re-check is cheap and matches the
     * MMDVMHost convention. */
    for (unsigned r = 0; r < 9; r++) {
        int c = hamming_15_11_3_correct(M[r]);
        if (c < 0) return -1;
        corrections += c;
    }

    uint8_t bits96[96];
    unsigned p = 0;
    for (unsigned r = 0; r < 9; r++) {
        for (unsigned c = 0; c < 11; c++) {
            if (r == 0 && c < 3) continue;
            bits96[p++] = M[r][c];
        }
    }

    memset(data_out, 0, 12);
    pack_bits(bits96, data_out, DMR_LC_BITS);
    return corrections;
}
