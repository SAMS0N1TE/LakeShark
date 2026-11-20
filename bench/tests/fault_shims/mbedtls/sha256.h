/* Host stand-in for mbedtls/sha256.h: a small self-contained SHA-256. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } mbedtls_sha256_context;

static const uint32_t ls_sha_k[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define LS_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static inline void ls_sha_block(mbedtls_sha256_context *c, const uint8_t *p)
{
    uint32_t w[64], a, b, d, e, f, g, h, cc, t1, t2;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = LS_ROR(w[i-15], 7) ^ LS_ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = LS_ROR(w[i-2], 17) ^ LS_ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (LS_ROR(e, 6) ^ LS_ROR(e, 11) ^ LS_ROR(e, 25)) + ((e & f) ^ (~e & g)) + ls_sha_k[i] + w[i];
        t2 = (LS_ROR(a, 2) ^ LS_ROR(a, 13) ^ LS_ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static inline void mbedtls_sha256_init(mbedtls_sha256_context *c) { memset(c, 0, sizeof(*c)); }
static inline void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; }
static inline int mbedtls_sha256_starts(mbedtls_sha256_context *c, int is224)
{
    static const uint32_t i0[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    (void)is224;
    memcpy(c->h, i0, sizeof(i0)); c->len = 0; c->n = 0;
    return 0;
}
static inline int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *in, size_t n)
{
    c->len += n;
    while (n) {
        size_t k = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, in, k); c->n += k; in += k; n -= k;
        if (c->n == 64) { ls_sha_block(c, c->buf); c->n = 0; }
    }
    return 0;
}
static inline int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, l[8];
    mbedtls_sha256_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) mbedtls_sha256_update(c, &pad, 1);
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    mbedtls_sha256_update(c, l, 8);
    for (int i = 0; i < 8; i++) { out[4*i] = (uint8_t)(c->h[i] >> 24); out[4*i+1] = (uint8_t)(c->h[i] >> 16); out[4*i+2] = (uint8_t)(c->h[i] >> 8); out[4*i+3] = (uint8_t)c->h[i]; }
    return 0;
}
static inline int mbedtls_sha256(const unsigned char *in, size_t n, unsigned char out[32], int is224)
{
    mbedtls_sha256_context c;
    mbedtls_sha256_starts(&c, is224); mbedtls_sha256_update(&c, in, n);
    return mbedtls_sha256_finish(&c, out);
}
