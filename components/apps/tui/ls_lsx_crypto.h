/* The link's crypto (ls_lsx.c): AES-128 in counter mode for secrecy, AES-CMAC
   (RFC 4493) for authentication and key derivation, CRC-32 for whole
   objects. On the board the block cipher is mbedTLS's, which the ESP-IDF
   port runs on the P4's AES peripheral; on the host bench it is the small
   table implementation in ls_lsx_crypto.c, checked against FIPS-197. */
#ifndef LS_LSX_CRYPTO_H
#define LS_LSX_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(ESP_PLATFORM) && !defined(LS_LSX_SOFT_AES)
#include "mbedtls/aes.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
#if defined(ESP_PLATFORM) && !defined(LS_LSX_SOFT_AES)
    mbedtls_aes_context c;
#else
    uint8_t rk[176];        /* the expanded key, eleven round keys */
#endif
    bool set;
} lsx_aes_t;

void lsx_aes_init(lsx_aes_t *a, const uint8_t key[16]);
void lsx_aes_free(lsx_aes_t *a);
void lsx_aes_block(lsx_aes_t *a, const uint8_t in[16], uint8_t out[16]);

/* XOR `n` bytes at `buf` with the AES-CTR keystream from `iv` (the last four
   bytes are the big-endian block counter and count up from what iv holds). */
void lsx_ctr(lsx_aes_t *a, const uint8_t iv[16], uint8_t *buf, size_t n);

/* AES-CMAC over m[0..n). */
void lsx_cmac(lsx_aes_t *a, const uint8_t *m, size_t n, uint8_t mac[16]);

/* A 16-byte key from `key`: CMAC(key, label || ctx), the SP 800-108
   counter-mode PRF with one block. */
void lsx_kdf(const uint8_t key[16], const char *label, const uint8_t *ctx, size_t n, uint8_t out[16]);

/* The key for a party code or a home phrase: CMAC chained 4096 times over the
   text, so guessing a short code costs that many AES operations a try. */
void lsx_phrase_key(const char *text, uint8_t out[16]);

/* CRC-32 (IEEE, reflected), continuing from `crc` (0 to start). */
uint32_t lsx_crc32(uint32_t crc, const uint8_t *d, size_t n);

/* FIPS-197 C.1 and RFC 4493 example 2: true when this build's AES and CMAC
   give the published answers. */
bool lsx_crypto_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
