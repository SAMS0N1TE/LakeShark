/* AES-128 counter mode, AES-CMAC, the key derivation and CRC-32 for the
   link (ls_lsx_crypto.h). */
#include "ls_lsx_crypto.h"

#include <string.h>

#if defined(ESP_PLATFORM) && !defined(LS_LSX_SOFT_AES)

void lsx_aes_init(lsx_aes_t *a, const uint8_t key[16])
{
    if (a->set) mbedtls_aes_free(&a->c);
    mbedtls_aes_init(&a->c);
    mbedtls_aes_setkey_enc(&a->c, key, 128);
    a->set = true;
}

void lsx_aes_free(lsx_aes_t *a)
{
    if (a->set) mbedtls_aes_free(&a->c);
    a->set = false;
}

void lsx_aes_block(lsx_aes_t *a, const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_crypt_ecb(&a->c, MBEDTLS_AES_ENCRYPT, in, out);
}

#else

/* The forward S-box (FIPS-197 figure 7). Encryption only: counter mode and
   CMAC never run the cipher backwards. */
static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t xt(uint8_t b) { return (uint8_t)((b << 1) ^ ((b & 0x80) ? 0x1b : 0)); }

void lsx_aes_init(lsx_aes_t *a, const uint8_t key[16])
{
    uint8_t *w = a->rk, rcon = 1;
    memcpy(w, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { w[i - 4], w[i - 3], w[i - 2], w[i - 1] };
        if (i % 16 == 0) {
            const uint8_t r = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[r];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++) w[i + k] = (uint8_t)(w[i - 16 + k] ^ t[k]);
    }
    a->set = true;
}

void lsx_aes_free(lsx_aes_t *a)
{
    memset(a->rk, 0, sizeof(a->rk));
    a->set = false;
}

void lsx_aes_block(lsx_aes_t *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ a->rk[i]);
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        /* SubBytes and ShiftRows together: byte (row, col) comes from column
           col + row of the state, columns being four bytes each. */
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[4 * c + row] = SBOX[s[4 * ((c + row) & 3) + row]];
        if (r < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *p = t + 4 * c;
                const uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                p[0] = (uint8_t)(a0 ^ x ^ xt((uint8_t)(a0 ^ a1)));
                p[1] = (uint8_t)(a1 ^ x ^ xt((uint8_t)(a1 ^ a2)));
                p[2] = (uint8_t)(a2 ^ x ^ xt((uint8_t)(a2 ^ a3)));
                p[3] = (uint8_t)(a3 ^ x ^ xt((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(t[i] ^ a->rk[16 * r + i]);
    }
    memcpy(out, s, 16);
}

#endif

void lsx_ctr(lsx_aes_t *a, const uint8_t iv[16], uint8_t *buf, size_t n)
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    while (n) {
        lsx_aes_block(a, ctr, ks);
        const size_t k = n < 16 ? n : 16;
        for (size_t i = 0; i < k; i++) buf[i] ^= ks[i];
        buf += k;
        n -= k;
        for (int i = 15; i >= 12 && ++ctr[i] == 0; i--) {}
    }
}

static void dbl(uint8_t b[16])
{
    const uint8_t carry = b[0] & 0x80;
    for (int i = 0; i < 15; i++) b[i] = (uint8_t)((b[i] << 1) | (b[i + 1] >> 7));
    b[15] = (uint8_t)((b[15] << 1) ^ (carry ? 0x87 : 0));
}

void lsx_cmac(lsx_aes_t *a, const uint8_t *m, size_t n, uint8_t mac[16])
{
    uint8_t k1[16] = { 0 }, x[16] = { 0 }, last[16];
    lsx_aes_block(a, k1, k1);   /* L = AES(K, 0) */
    dbl(k1);                    /* K1 */
    const size_t blocks = n ? (n + 15) / 16 : 1;
    const bool whole = n && n % 16 == 0;
    for (size_t b = 0; b + 1 < blocks; b++) {
        for (int i = 0; i < 16; i++) x[i] ^= m[16 * b + i];
        lsx_aes_block(a, x, x);
    }
    const size_t off = (blocks - 1) * 16, rem = n - off;
    memset(last, 0, sizeof(last));
    memcpy(last, m + off, rem);
    if (whole) {
        for (int i = 0; i < 16; i++) last[i] ^= k1[i];
    } else {
        last[rem] = 0x80;
        dbl(k1);                /* K2 */
        for (int i = 0; i < 16; i++) last[i] ^= k1[i];
    }
    for (int i = 0; i < 16; i++) x[i] ^= last[i];
    lsx_aes_block(a, x, mac);
}

void lsx_kdf(const uint8_t key[16], const char *label, const uint8_t *ctx, size_t n, uint8_t out[16])
{
    uint8_t m[96];
    size_t l = strlen(label);
    if (l > 32) l = 32;
    if (n > sizeof(m) - l - 1) n = sizeof(m) - l - 1;
    memcpy(m, label, l);
    m[l] = 0;
    if (n) memcpy(m + l + 1, ctx, n);
    lsx_aes_t a = { 0 };
    lsx_aes_init(&a, key);
    lsx_cmac(&a, m, l + 1 + n, out);
    lsx_aes_free(&a);
}

void lsx_phrase_key(const char *text, uint8_t out[16])
{
    static const uint8_t zero[16];
    const size_t n = strlen(text);
    lsx_kdf(zero, "LSX phrase", (const uint8_t *)text, n, out);
    lsx_aes_t a = { 0 };
    for (int i = 0; i < 4096; i++) {
        lsx_aes_init(&a, out);
        lsx_cmac(&a, (const uint8_t *)text, n, out);
    }
    lsx_aes_free(&a);
}

uint32_t lsx_crc32(uint32_t crc, const uint8_t *d, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *d++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool lsx_crypto_selftest(void)
{
    static const uint8_t k1[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t p1[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const uint8_t c1[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                    0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    static const uint8_t k2[16] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
    static const uint8_t m2[16] = { 0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
                                    0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a };
    static const uint8_t t2[16] = { 0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
                                    0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c };
    static const uint8_t t0[16] = { 0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28,
                                    0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46 };
    uint8_t out[16];
    lsx_aes_t a = { 0 };
    lsx_aes_init(&a, k1);
    lsx_aes_block(&a, p1, out);
    bool ok = !memcmp(out, c1, 16);
    lsx_aes_init(&a, k2);
    lsx_cmac(&a, m2, 16, out);
    ok = ok && !memcmp(out, t2, 16);
    lsx_cmac(&a, m2, 0, out);
    ok = ok && !memcmp(out, t0, 16);
    lsx_aes_free(&a);
    return ok && lsx_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u;
}
