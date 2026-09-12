/* rweather/Crypto's Ed25519, reduced to the one method MeshCore calls.

   LS-969  MeshCore already carries a complete pure-C ed25519 in lib/ed25519
   (orlp's, public domain / MIT) and uses it for keygen, signing and key
   exchange. It reaches for rweather's Ed25519 in exactly one place -
   Identity.cpp's verify - so this forwards that one call to the
   implementation already being linked.

   Using both would be worse than untidy: two ed25519 implementations in one
   binary is roughly 30 KB of duplicated tables, and a signature that
   verifies under one and not the other is the kind of bug that only appears
   in the field. */
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
