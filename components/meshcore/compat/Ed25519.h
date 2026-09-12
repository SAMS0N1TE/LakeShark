

#ifndef LS_COMPAT_ED25519_H
#define LS_COMPAT_ED25519_H

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "ed_25519.h"
}

class Ed25519 {
public:
    /* orlp's ed25519_verify returns 1 on a good signature, 0 otherwise. */
    static bool verify(const uint8_t signature[64], const uint8_t publicKey[32],
                       const void *message, size_t len)
    {
        return ed25519_verify(signature, (const unsigned char *)message, len, publicKey) == 1;
    }
};

#endif /* LS_COMPAT_ED25519_H */
