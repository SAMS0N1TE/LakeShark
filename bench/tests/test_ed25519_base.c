/* LS_TEST_SOURCES: components/meshcore/ls_ed25519_base.c and upstream/ed25519
   with ge.c's ge_scalarmult_base renamed, as the firmware builds them.

   The run-time base table must be the 30 KB constant upstream ships, and
   keys and signatures must come out bit for bit as before. */

#include "ls_test.h"

#include <stdint.h>
#include <stdlib.h>

#include "ed_25519.h"
#include "fe.h"
#include "ge.h"
#include "ls_ed25519_base.h"
#include "precomp_data.h"   /* upstream's base[32][8] (and Bi[8]) */

void ls_upstream_ge_scalarmult_base(ge_p3 *h, const unsigned char *a);

static void fe_bytes(unsigned char out[32], const fe f) { fe_tobytes(out, f); }

static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
}

static size_t unhex(unsigned char *out, const char *hex)
{
    size_t n = 0;
    for (; hex[0] && hex[1]; hex += 2) out[n++] = (unsigned char)(hexval(hex[0]) << 4 | hexval(hex[1]));
    return n;
}

LS_CASE(ed25519_base_table_matches_upstream_constant)
{
    const ge_precomp (*t)[8] = ls_ed25519_base_table();
    int field_diff = 0, limb_diff = 0;
    for (int i = 0; i < 32; ++i)
        for (int j = 0; j < 8; ++j) {
            const ge_precomp *a = &t[i][j], *b = &base[i][j];
            const int32_t *fa[3] = {a->yplusx, a->yminusx, a->xy2d};
            const int32_t *fb[3] = {b->yplusx, b->yminusx, b->xy2d};
            for (int k = 0; k < 3; ++k) {
                unsigned char sa[32], sb[32];
                fe_bytes(sa, fa[k]);
                fe_bytes(sb, fb[k]);
                if (memcmp(sa, sb, 32)) ++field_diff;
                if (memcmp(fa[k], fb[k], sizeof(fe))) ++limb_diff;
            }
        }
    LS_EQ_INT(field_diff, 0);
    LS_EQ_INT(limb_diff, 0);
    LS_CHECK(ls_ed25519_base_table() == t);
}

LS_CASE(ed25519_scalarmult_base_matches_upstream)
{
    srand(12345);
    for (int n = 0; n < 200; ++n) {
        unsigned char a[32], ours[32], theirs[32];
        for (int i = 0; i < 32; ++i) a[i] = (unsigned char)rand();
        a[31] &= 127;
        if (n == 0) memset(a, 0, sizeof(a));
        if (n == 1) { memset(a, 0xff, sizeof(a)); a[31] = 127; }
        ge_p3 h1, h2;
        ge_scalarmult_base(&h1, a);
        ls_upstream_ge_scalarmult_base(&h2, a);
        ge_p3_tobytes(ours, &h1);
        ge_p3_tobytes(theirs, &h2);
        LS_CHECK_MSG(!memcmp(ours, theirs, 32), "scalar %d differs", n);
    }
}

/* RFC 8032 section 7.1, tests 1-3. */
static const struct { const char *sk, *pk, *msg, *sig; } RFC8032[] = {
    {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
};

LS_CASE(ed25519_rfc8032_keys_and_signatures)
{
    for (size_t v = 0; v < sizeof(RFC8032) / sizeof(RFC8032[0]); ++v) {
        unsigned char seed[32], pk_want[32], msg[8], sig_want[64];
        unsigned char pk[32], sk[64], sig[64];
        unhex(seed, RFC8032[v].sk);
        unhex(pk_want, RFC8032[v].pk);
        size_t len = unhex(msg, RFC8032[v].msg);
        unhex(sig_want, RFC8032[v].sig);
        ed25519_create_keypair(pk, sk, seed);
        LS_CHECK_MSG(!memcmp(pk, pk_want, 32), "vector %u public key", (unsigned)v + 1);
        ed25519_sign(sig, msg, len, pk, sk);
        LS_CHECK_MSG(!memcmp(sig, sig_want, 64), "vector %u signature", (unsigned)v + 1);
        LS_CHECK(ed25519_verify(sig, msg, len, pk));
    }
}
