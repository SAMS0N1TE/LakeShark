/* Public-key direct messages (Meshtastic 2.5+): X25519 between the two nodes,
   SHA-256 of the shared secret as an AES-256 key, AES-CCM with an 8-byte tag.
   The 13-byte nonce is the packet id (4 bytes LE), a 4-byte extra nonce the
   sender picks, the sender's node number (4 bytes LE) and a zero byte; the
   frame carries ciphertext, tag, extra nonce. Plus SHA-256 and base64 for
   channel keys. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MT_PKI_OVERHEAD 12      /* 8-byte tag + 4-byte extra nonce */

void mt_sha256(const uint8_t *data, size_t n, uint8_t out[32]);
/* RFC 7748 X25519: out = scalar * point (point NULL = the base point 9). */
void mt_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t *point);
/* The AES key two nodes share: SHA-256(X25519(my_private, their_public)). */
void mt_pki_shared(const uint8_t my_private[32], const uint8_t their_public[32], uint8_t key[32]);

/* AES-CCM, L = 2 (13-byte nonce), tag of m bytes; aad may be NULL. */
bool mt_ccm_encrypt(const uint8_t *key, int key_len, const uint8_t nonce[13], int m,
                    const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t n,
                    uint8_t *out, uint8_t *tag);
bool mt_ccm_decrypt(const uint8_t *key, int key_len, const uint8_t nonce[13], int m,
                    const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t n,
                    uint8_t *out, const uint8_t *tag);

/* Frame helpers: plain (n bytes) -> out (n + 12 bytes), and back (n includes
   the 12). Decrypt returns the plain length, -1 when the tag does not match. */
int mt_pki_seal(const uint8_t key[32], uint32_t from, uint32_t id, uint32_t extra,
                const uint8_t *plain, size_t n, uint8_t *out);
int mt_pki_open(const uint8_t key[32], uint32_t from, uint32_t id,
                const uint8_t *frame, size_t n, uint8_t *out);

/* Base64, standard or URL alphabet, padding optional. Returns bytes, -1 on bad input. */
int mt_b64_decode(const char *in, uint8_t *out, int cap);
void mt_b64_encode(const uint8_t *in, int n, char *out, int cap);
