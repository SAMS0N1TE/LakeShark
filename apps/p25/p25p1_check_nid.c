/*
 * p25p1_check_nid.c — BCH(63,16,11) decoder for the P25 Phase 1 NID.
 *
 * Replaces the previous home-grown implementation, which had TWO independent
 * math bugs:
 *   1. Used primitive polynomial p(x) = x^6 + x + 1 (0x43), but the P25
 *      generator polynomial has consecutive roots alpha^1..alpha^22 only
 *      under p(x) = x^6 + x^5 + 1 (0x61).
 *   2. generate_gf() / gen_poly() produced a wrong generator; the ELP
 *      loop had unreachable code and dead allocations, making every
 *      received word look uncorrectable.
 *
 * Both bugs were verified by off-target reproduction (see bch_test.c).
 *
 * Algorithm
 * ─────────
 * 1. Build GF(64) log/antilog tables from primitive polynomial 0x61.
 * 2. Compute syndromes s_i = v(alpha^i) for i = 1..2t.
 * 3. If all syndromes are zero → clean codeword.
 * 4. Berlekamp-Massey (binary-BCH simplification) to find the error
 *    locator polynomial sigma(x).
 * 5. Chien search: find roots of sigma(x) to locate errors.
 * 6. Flip bits at error positions. Return OK if and only if the number
 *    of roots matches deg(sigma) and deg(sigma) ≤ t=11.
 *
 * Codeword bit layout (as filled by dsd_frame.c):
 *   recd[0..11]   = NAC (12 bits, MSB first)
 *   recd[12..15]  = DUID
 *   recd[16..62]  = BCH redundancy (47 bits)
 *   parity arg    = trailing parity bit (bit 63 of the NID, not used by BCH)
 *
 * Self-test runs on first call and logs result via diag_line("SELF", ...).
 *
 * License: GPL-3.0 (matches DSD-neo). Generator polynomial coefficients are
 * from MMDVMHost (g4klx, GPL-3.0). Decoder is an original implementation.
 */

#include "p25p1_check_nid.h"
#include "diag.h"

#include <string.h>
#include <stdio.h>

/* ── Code parameters ── */
#define BCH_MM   6
#define BCH_NN   63
#define BCH_KK   16
#define BCH_TT   11
#define BCH_RR   (BCH_NN - BCH_KK)      /* 47 parity bits */

/* Primitive polynomial p(x) = x^6 + x^5 + 1. Verified: under this poly,
 * g_bch_gen has exactly the roots alpha^1..alpha^22. */
#define BCH_PRIM_POLY 0x61

static int g_log[BCH_NN + 1];
static int g_exp[BCH_NN + 1];
static int g_tables_built = 0;

/* Generator polynomial coefficients from MMDVMHost BCH.cpp (g4klx, GPL-3.0).
 * Degree 47, g[0] is coefficient of x^0. Used for self-test encoding only. */
static const int g_bch_gen[48] = {
    1, 1, 0,  0, 1, 1,  0, 1, 1,  0, 0, 1,  0, 0, 1,  1, 0, 0,
    0, 0, 1,  0, 1, 1,  1, 1, 0,  1, 1, 1,  0, 1, 0,  0, 1, 1,
    1, 0, 1,  1, 0, 0,  1, 0, 1,  0, 1, 1
};

static int g_self_test_ran = 0;
static int g_self_test_ok  = 0;

/* ──────────────────────────────────────────────────────────────
 * GF(2^6) table construction.
 * alpha^6 = alpha^5 + 1 in GF(2). We iterate alpha^0, alpha^1, ...
 * reducing mod p(x) whenever bit 6 is set.
 * ────────────────────────────────────────────────────────────── */
static void build_gf_tables(void)
{
    int reg = 1;
    for (int i = 0; i < BCH_NN; i++) {
        g_exp[i] = reg;
        g_log[reg] = i;
        reg <<= 1;
        if (reg & (1 << BCH_MM)) reg ^= BCH_PRIM_POLY;
    }
    g_exp[BCH_NN] = g_exp[0];
    g_log[0] = -1;
}

static inline int gf_mul(int a, int b)
{
    if (a == 0 || b == 0) return 0;
    int e = g_log[a] + g_log[b];
    if (e >= BCH_NN) e -= BCH_NN;
    return g_exp[e];
}

/* ──────────────────────────────────────────────────────────────
 * Core decoder. Input:
 *   recd[0..62]  — received 63-bit codeword (each element 0 or 1).
 * Output:
 *   recd[] corrected in place on success.
 *   *errors_out = # corrected (0..11) on success, -1 on failure.
 * Returns 1 on success, 0 on uncorrectable.
 * ────────────────────────────────────────────────────────────── */
static int bch_decode(int *recd, int *errors_out)
{
    if (!g_tables_built) { build_gf_tables(); g_tables_built = 1; }
    if (errors_out) *errors_out = 0;

    /* 1. Syndromes s[1..2t] */
    int s[2 * BCH_TT + 1];          /* s[0] unused */
    int syn_nz = 0;
    for (int i = 1; i <= 2 * BCH_TT; i++) {
        int syn = 0;
        for (int j = 0; j < BCH_NN; j++) {
            if (recd[j]) {
                int e = (i * j) % BCH_NN;
                syn ^= g_exp[e];
            }
        }
        s[i] = syn;
        if (syn) syn_nz = 1;
    }
    if (!syn_nz) return 1;

    /* 2. Berlekamp-Massey. Canonical pseudocode from Massey 1969.
     *    C(x) = current locator polynomial.
     *    B(x) = connection from last length change.
     *    L    = current locator degree.
     *    m    = distance since last length change.
     *    b    = GF element discrepancy from last length change. */
    const int SZ = 2 * BCH_TT + 2;
    int C[SZ], B[SZ], T[SZ];
    memset(C, 0, sizeof(C));
    memset(B, 0, sizeof(B));
    C[0] = 1;
    B[0] = 1;
    int L = 0;
    int m = 1;
    int b = 1;

    for (int n = 0; n < 2 * BCH_TT; n++) {
        int d = s[n + 1];
        for (int i = 1; i <= L; i++) {
            if (C[i] != 0 && s[n + 1 - i] != 0) {
                d ^= gf_mul(C[i], s[n + 1 - i]);
            }
        }

        if (d == 0) {
            m++;
        } else if (2 * L <= n) {
            memcpy(T, C, sizeof(T));
            int log_coef = (g_log[d] - g_log[b] + BCH_NN) % BCH_NN;
            for (int i = 0; i + m < SZ; i++) {
                if (B[i] != 0) {
                    C[i + m] ^= gf_mul(g_exp[log_coef], B[i]);
                }
            }
            L = n + 1 - L;
            memcpy(B, T, sizeof(B));
            b = d;
            m = 1;
        } else {
            int log_coef = (g_log[d] - g_log[b] + BCH_NN) % BCH_NN;
            for (int i = 0; i + m < SZ; i++) {
                if (B[i] != 0) {
                    C[i + m] ^= gf_mul(g_exp[log_coef], B[i]);
                }
            }
            m++;
        }
    }

    if (L > BCH_TT) { if (errors_out) *errors_out = -1; return 0; }

    /* 3. Chien search: roots of C(x) at x = alpha^{-loc}. */
    int err_locs[BCH_TT + 1];
    int err_count = 0;
    for (int j = 0; j < BCH_NN; j++) {
        int val = 0;
        for (int i = 0; i <= L; i++) {
            if (C[i] != 0) {
                int e = (g_log[C[i]] + i * j) % BCH_NN;
                val ^= g_exp[e];
            }
        }
        if (val == 0) {
            if (err_count >= BCH_TT + 1) {
                if (errors_out) *errors_out = -1;
                return 0;
            }
            err_locs[err_count++] = (BCH_NN - j) % BCH_NN;
        }
    }

    if (err_count != L) { if (errors_out) *errors_out = -1; return 0; }

    /* 4. Apply corrections. */
    for (int i = 0; i < err_count; i++) {
        int loc = err_locs[i];
        if (loc >= 0 && loc < BCH_NN) recd[loc] ^= 1;
    }

    if (errors_out) *errors_out = err_count;
    return 1;
}

/* ──────────────────────────────────────────────────────────────
 * Self-test encoder. Systematic LFSR division by g(x).
 * data[0..15] = message, bb[0..46] = parity.
 * ────────────────────────────────────────────────────────────── */
static void bch_encode(const int *data, int *bb)
{
    for (int i = 0; i < BCH_RR; i++) bb[i] = 0;
    for (int i = BCH_KK - 1; i >= 0; i--) {
        int fb = data[i] ^ bb[BCH_RR - 1];
        if (fb) {
            for (int j = BCH_RR - 1; j > 0; j--) {
                bb[j] = g_bch_gen[j] ? (bb[j - 1] ^ fb) : bb[j - 1];
            }
            bb[0] = g_bch_gen[0] && fb;
        } else {
            for (int j = BCH_RR - 1; j > 0; j--) bb[j] = bb[j - 1];
            bb[0] = 0;
        }
    }
}

/* ──────────────────────────────────────────────────────────────
 * Self-test: encode a known NID, inject 0..11 errors, verify decode.
 * Emits one SELF line per test case and a final PASS/FAIL summary.
 * ────────────────────────────────────────────────────────────── */
static void bch_self_test(void)
{
    if (!g_tables_built) { build_gf_tables(); g_tables_built = 1; }

    /* Build the P25 codeword in the same layout dsd_frame.c uses:
     *   cw[0..15]  = data (NAC<<4 | DUID)
     *   cw[16..62] = parity from g(x) */
    int data[BCH_KK];
    int v = 0x5C21;        /* NAC=0x5C2, DUID=1 */
    for (int i = 0; i < BCH_KK; i++) data[BCH_KK - 1 - i] = (v >> i) & 1;
    int par[BCH_RR];
    bch_encode(data, par);

    int cw[BCH_NN];
    for (int i = 0; i < BCH_KK; i++) cw[i] = data[i];
    for (int i = 0; i < BCH_RR; i++) cw[BCH_KK + i] = par[i];

    int all_ok = 1;

    /* Test 0: clean decode. */
    {
        int recd[BCH_NN];
        memcpy(recd, cw, sizeof(recd));
        int ec = -1;
        int ok = bch_decode(recd, &ec);
        int match = (memcmp(recd, cw, sizeof(recd)) == 0);
        if (!(ok && ec == 0 && match)) {
            diag_line("SELF", "T0 FAIL 0-err ok=%d ec=%d match=%d", ok, ec, match);
            all_ok = 0;
        } else {
            diag_line("SELF", "T0 OK 0-err");
        }
    }

    /* Tests 1..11: inject n errors at fixed positions and verify correction. */
    const int err_pos[11][11] = {
        { 3 },                                   /* 1 */
        { 3, 17 },                               /* 2 */
        { 3, 17, 30 },                           /* 3 */
        { 3, 17, 30, 44 },                       /* 4 */
        { 3, 17, 30, 44, 58 },                   /* 5 */
        { 3, 17, 30, 44, 58, 7 },                /* 6 */
        { 3, 17, 30, 44, 58, 7, 22 },            /* 7 */
        { 3, 17, 30, 44, 58, 7, 22, 39 },        /* 8 */
        { 3, 17, 30, 44, 58, 7, 22, 39, 51 },    /* 9 */
        { 3, 17, 30, 44, 58, 7, 22, 39, 51, 12 },/* 10 */
        { 1, 4, 9, 14, 21, 25, 33, 40, 47, 55, 62 }, /* 11 */
    };
    for (int n = 1; n <= 11; n++) {
        int recd[BCH_NN];
        memcpy(recd, cw, sizeof(recd));
        for (int i = 0; i < n; i++) recd[err_pos[n - 1][i]] ^= 1;
        int ec = -1;
        int ok = bch_decode(recd, &ec);
        int match = (memcmp(recd, cw, sizeof(recd)) == 0);
        if (!(ok && ec == n && match)) {
            diag_line("SELF", "T%d FAIL %d-err ok=%d ec=%d match=%d",
                      n, n, ok, ec, match);
            all_ok = 0;
        } else {
            diag_line("SELF", "T%d OK %d-err", n, n);
        }
    }

    g_self_test_ok  = all_ok;
    g_self_test_ran = 1;
    diag_line("SELF", "bch_self_test %s", all_ok ? "PASS" : "FAIL");
}

/* ──────────────────────────────────────────────────────────────
 * Public API — signatures unchanged from the old file.
 *
 * bch_code[0..62] must be the 63-bit NID with NAC (MSB first) at
 * [0..11], DUID at [12..15], and BCH parity at [16..62]. The 64th
 * bit of the NID (the "parity" dibit) is passed separately but
 * is ignored — BCH uses all 63 bits already.
 * ────────────────────────────────────────────────────────────── */
int check_NID_ec(char *bch_code, int *new_nac, char *new_duid,
                 unsigned char parity, int *errors_out)
{
    (void)parity;

    if (!g_self_test_ran) bch_self_test();

    int recd[BCH_NN];
    for (int i = 0; i < BCH_NN; i++) recd[i] = bch_code[i] ? 1 : 0;

    int ec = 0;
    int ok = bch_decode(recd, &ec);
    if (errors_out) *errors_out = ok ? ec : -1;
    if (!ok) return 0;

    int nac = 0;
    for (int i = 0; i < 12; i++) nac = (nac << 1) | (recd[i] & 1);
    *new_nac = nac;

    int d0 = (recd[12] ? 2 : 0) | (recd[13] ? 1 : 0);
    int d1 = (recd[14] ? 2 : 0) | (recd[15] ? 1 : 0);
    new_duid[0] = (char)('0' + d0);
    new_duid[1] = (char)('0' + d1);
    new_duid[2] = 0;

    return 1;
}

int check_NID(char *bch_code, int *new_nac, char *new_duid, unsigned char parity)
{
    int ec_unused;
    return check_NID_ec(bch_code, new_nac, new_duid, parity, &ec_unused);
}
