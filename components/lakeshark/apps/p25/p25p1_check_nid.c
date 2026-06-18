
#include "p25p1_check_nid.h"
#include "diag.h"

#include <string.h>
#include <stdio.h>

#define BCH_MM   6
#define BCH_NN   63
#define BCH_KK   16
#define BCH_TT   11
#define BCH_RR   (BCH_NN - BCH_KK)

#define BCH_PRIM_POLY 0x61

static int g_log[BCH_NN + 1];
static int g_exp[BCH_NN + 1];
static int g_tables_built = 0;

static const int g_bch_gen[48] = {
    1, 1, 0,  0, 1, 1,  0, 1, 1,  0, 0, 1,  0, 0, 1,  1, 0, 0,
    0, 0, 1,  0, 1, 1,  1, 1, 0,  1, 1, 1,  0, 1, 0,  0, 1, 1,
    1, 0, 1,  1, 0, 0,  1, 0, 1,  0, 1, 1
};

static int g_self_test_ran = 0;
static int g_self_test_ok  = 0;

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

static int bch_decode(int *recd, int *errors_out)
{
    if (!g_tables_built) { build_gf_tables(); g_tables_built = 1; }
    if (errors_out) *errors_out = 0;

    int s[2 * BCH_TT + 1];
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

    for (int i = 0; i < err_count; i++) {
        int loc = err_locs[i];
        if (loc >= 0 && loc < BCH_NN) recd[loc] ^= 1;
    }

    if (errors_out) *errors_out = err_count;
    return 1;
}

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

static void bch_self_test(void)
{
    if (!g_tables_built) { build_gf_tables(); g_tables_built = 1; }

    int data[BCH_KK];
    int v = 0x5C21;
    for (int i = 0; i < BCH_KK; i++) data[BCH_KK - 1 - i] = (v >> i) & 1;
    int par[BCH_RR];
    bch_encode(data, par);

    int cw[BCH_NN];
    for (int i = 0; i < BCH_KK; i++) cw[i] = data[i];
    for (int i = 0; i < BCH_RR; i++) cw[BCH_KK + i] = par[i];

    int all_ok = 1;

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

    const int err_pos[11][11] = {
        { 3 },
        { 3, 17 },
        { 3, 17, 30 },
        { 3, 17, 30, 44 },
        { 3, 17, 30, 44, 58 },
        { 3, 17, 30, 44, 58, 7 },
        { 3, 17, 30, 44, 58, 7, 22 },
        { 3, 17, 30, 44, 58, 7, 22, 39 },
        { 3, 17, 30, 44, 58, 7, 22, 39, 51 },
        { 3, 17, 30, 44, 58, 7, 22, 39, 51, 12 },
        { 1, 4, 9, 14, 21, 25, 33, 40, 47, 55, 62 },
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
