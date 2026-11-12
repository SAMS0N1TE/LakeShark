/* SHA-256 (FIPS 180-4), X25519 (RFC 7748, a 16-limb field in the style of
   the public-domain TweetNaCl), AES-CCM (RFC 3610) and base64. */
#include "mt_pki.h"
#include "mt_aes.h"
#include <string.h>

/* ---- SHA-256 ------------------------------------------------------------ */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t s[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        const uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

void mt_sha256(const uint8_t *data, size_t n, uint8_t out[32])
{
    uint32_t s[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint64_t bits = (uint64_t)n * 8;
    while (n >= 64) { sha_block(s, data); data += 64; n -= 64; }
    uint8_t last[128];
    memset(last, 0, sizeof(last));
    memcpy(last, data, n);
    last[n] = 0x80;
    const size_t total = n + 9 <= 64 ? 64 : 128;
    for (int i = 0; i < 8; i++) last[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha_block(s, last);
    if (total == 128) sha_block(s, last + 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s[i] >> 24); out[4 * i + 1] = (uint8_t)(s[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s[i] >> 8); out[4 * i + 3] = (uint8_t)s[i];
    }
}

/* ---- X25519 ------------------------------------------------------------- */
typedef int64_t gf[16];

static void car(gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        const int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * ((int64_t)1 << 16);
    }
}

static void sel(gf p, gf q, int b)
{
    const int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) { const int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}

static void pack(uint8_t *o, const gf n)
{
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car(t); car(t); car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}

static void unpack(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fmul(gf o, const gf a, const gf b)
{
    int64_t t[31];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car(o); car(o);
}

static void finv(gf o, const gf in)
{
    gf c;
    memcpy(c, in, sizeof(gf));
    for (int a = 253; a >= 0; a--) {
        fmul(c, c, c);
        if (a != 2 && a != 4) fmul(c, c, in);
    }
    memcpy(o, c, sizeof(gf));
}

void mt_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t *point)
{
    static const uint8_t BASE[32] = { 9 };
    static const gf A24 = { 0xDB41, 1 };
    /* Static: an app callback's stack is small. */
    static gf x, a, b, c, d, e, f;
    uint8_t z[32];
    memcpy(z, scalar, 32);
    z[31] = (uint8_t)((z[31] & 127) | 64);
    z[0] &= 248;
    unpack(x, point ? point : BASE);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        const int r = (z[i >> 3] >> (i & 7)) & 1;
        sel(a, b, r); sel(c, d, r);
        fadd(e, a, c); fsub(a, a, c); fadd(c, b, d); fsub(b, b, d);
        fmul(d, e, e); fmul(f, a, a); fmul(a, c, a); fmul(c, b, e);
        fadd(e, a, c); fsub(a, a, c); fmul(b, a, a); fsub(c, d, f);
        fmul(a, c, A24); fadd(a, a, d); fmul(c, c, a); fmul(a, d, f);
        fmul(d, b, x); fmul(b, e, e);
        sel(a, b, r); sel(c, d, r);
    }
    finv(c, c);
    fmul(a, a, c);
    pack(out, a);
    memset(z, 0, sizeof(z));
}

void mt_pki_shared(const uint8_t my_private[32], const uint8_t their_public[32], uint8_t key[32])
{
    uint8_t s[32];
    mt_x25519(s, my_private, their_public);
    mt_sha256(s, 32, key);
    memset(s, 0, sizeof(s));
}

/* ---- AES-CCM ------------------------------------------------------------ */
static void ccm_ctr_block(const uint8_t nonce[13], uint16_t i, uint8_t out[16])
{
    out[0] = 1;                       /* L - 1 */
    memcpy(out + 1, nonce, 13);
    out[14] = (uint8_t)(i >> 8);
    out[15] = (uint8_t)i;
}

static void ccm_mac(const mt_aes_t *k, const uint8_t nonce[13], int m, const uint8_t *aad, size_t aad_len,
                    const uint8_t *msg, size_t n, uint8_t x[16])
{
    uint8_t b[16];
    b[0] = (uint8_t)((aad_len ? 64 : 0) | (((m - 2) / 2) << 3) | 1);
    memcpy(b + 1, nonce, 13);
    b[14] = (uint8_t)(n >> 8);
    b[15] = (uint8_t)n;
    mt_aes_block(k, b, x);
    if (aad_len) {
        uint8_t blk[16];
        memset(blk, 0, 16);
        blk[0] = (uint8_t)(aad_len >> 8);
        blk[1] = (uint8_t)aad_len;
        size_t used = aad_len < 14 ? aad_len : 14;
        memcpy(blk + 2, aad, used);
        for (int i = 0; i < 16; i++) x[i] ^= blk[i];
        mt_aes_block(k, x, x);
        while (used < aad_len) {
            const size_t take = aad_len - used < 16 ? aad_len - used : 16;
            for (size_t i = 0; i < take; i++) x[i] ^= aad[used + i];
            mt_aes_block(k, x, x);
            used += take;
        }
    }
    for (size_t at = 0; at < n; at += 16) {
        const size_t take = n - at < 16 ? n - at : 16;
        for (size_t i = 0; i < take; i++) x[i] ^= msg[at + i];
        mt_aes_block(k, x, x);
    }
}

static void ccm_ctr(const mt_aes_t *k, const uint8_t nonce[13], const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t a[16], s[16];
    for (size_t at = 0, blk = 1; at < n; at += 16, blk++) {
        ccm_ctr_block(nonce, (uint16_t)blk, a);
        mt_aes_block(k, a, s);
        const size_t take = n - at < 16 ? n - at : 16;
        for (size_t i = 0; i < take; i++) out[at + i] = in[at + i] ^ s[i];
    }
}

bool mt_ccm_encrypt(const uint8_t *key, int key_len, const uint8_t nonce[13], int m,
                    const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t n,
                    uint8_t *out, uint8_t *tag)
{
    mt_aes_t k;
    if (n > 0xFFFF || m < 4 || m > 16 || (m & 1) || !mt_aes_setkey(&k, key, key_len)) return false;
    uint8_t x[16], a[16], s0[16];
    ccm_mac(&k, nonce, m, aad, aad_len, in, n, x);
    ccm_ctr(&k, nonce, in, n, out);
    ccm_ctr_block(nonce, 0, a);
    mt_aes_block(&k, a, s0);
    for (int i = 0; i < m; i++) tag[i] = x[i] ^ s0[i];
    return true;
}

bool mt_ccm_decrypt(const uint8_t *key, int key_len, const uint8_t nonce[13], int m,
                    const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t n,
                    uint8_t *out, const uint8_t *tag)
{
    mt_aes_t k;
    if (n > 0xFFFF || m < 4 || m > 16 || (m & 1) || !mt_aes_setkey(&k, key, key_len)) return false;
    uint8_t x[16], a[16], s0[16];
    ccm_ctr(&k, nonce, in, n, out);
    ccm_mac(&k, nonce, m, aad, aad_len, out, n, x);
    ccm_ctr_block(nonce, 0, a);
    mt_aes_block(&k, a, s0);
    uint8_t diff = 0;
    for (int i = 0; i < m; i++) diff |= (uint8_t)(tag[i] ^ x[i] ^ s0[i]);
    if (diff) memset(out, 0, n);
    return diff == 0;
}

static void pki_nonce(uint32_t from, uint32_t id, uint32_t extra, uint8_t n[13])
{
    memset(n, 0, 13);
    for (int i = 0; i < 4; i++) {
        n[i] = (uint8_t)(id >> (8 * i));
        n[4 + i] = (uint8_t)(extra >> (8 * i));
        n[8 + i] = (uint8_t)(from >> (8 * i));
    }
}

int mt_pki_seal(const uint8_t key[32], uint32_t from, uint32_t id, uint32_t extra,
                const uint8_t *plain, size_t n, uint8_t *out)
{
    uint8_t nonce[13];
    if (!extra) extra = 1;            /* zero would leave the id's high half in its place */
    pki_nonce(from, id, extra, nonce);
    if (!mt_ccm_encrypt(key, 32, nonce, 8, NULL, 0, plain, n, out, out + n)) return -1;
    for (int i = 0; i < 4; i++) out[n + 8 + i] = (uint8_t)(extra >> (8 * i));
    return (int)n + MT_PKI_OVERHEAD;
}

int mt_pki_open(const uint8_t key[32], uint32_t from, uint32_t id,
                const uint8_t *frame, size_t n, uint8_t *out)
{
    if (n <= MT_PKI_OVERHEAD) return -1;
    const size_t pn = n - MT_PKI_OVERHEAD;
    const uint8_t *e = frame + pn + 8;
    const uint32_t extra = e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24;
    uint8_t nonce[13];
    pki_nonce(from, id, extra, nonce);
    return mt_ccm_decrypt(key, 32, nonce, 8, NULL, 0, frame, pn, out, frame + pn) ? (int)pn : -1;
}

/* ---- base64 ------------------------------------------------------------- */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int mt_b64_decode(const char *in, uint8_t *out, int cap)
{
    uint32_t acc = 0;
    int bits = 0, n = 0;
    for (; *in; in++) {
        if (*in == '=' || *in == ' ') continue;
        const int v = b64_val(*in);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

void mt_b64_encode(const uint8_t *in, int n, char *out, int cap)
{
    int j = 0;
    for (int i = 0; i < n && j + 4 < cap; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[j++] = B64[v >> 18 & 63];
        out[j++] = B64[v >> 12 & 63];
        out[j++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[j++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[j] = 0;
}
