/* Shared OTA/native-app SHA-256 / ECDSA P-256 public-key verification. */
#ifndef LS_SIGNATURE_H
#define LS_SIGNATURE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool ls_signature_verify(const char *pem, const uint8_t hash[32], const uint8_t *der, size_t n);
#endif
