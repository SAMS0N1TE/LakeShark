#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t rk[240];    /* round keys, up to AES-256 */
    int rounds;
} mt_aes_t;

bool mt_aes_setkey(mt_aes_t *a, const uint8_t *key, int len);   /* 16 or 32 bytes */
void mt_aes_block(const mt_aes_t *a, const uint8_t in[16], uint8_t out[16]);
/* CTR: the IV is the initial 16-byte counter block, incremented big-endian.
   Encrypt and decrypt are the same call. */
void mt_aes_ctr(const mt_aes_t *a, const uint8_t iv[16], uint8_t *buf, size_t n);
