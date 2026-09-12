

#ifndef LS_COMPAT_AES_H
#define LS_COMPAT_AES_H

#include <stddef.h>
#include <stdint.h>

#include "mbedtls/aes.h"

class AES128 {
public:
    AES128()  { mbedtls_aes_init(&_enc); mbedtls_aes_init(&_dec); }
    ~AES128() { mbedtls_aes_free(&_enc); mbedtls_aes_free(&_dec); }

    /* mbedtls keeps separate schedules for the two directions, so both are
       set here. Returns false on a wrong key length rather than half
       configuring, which would decrypt to plausible rubbish. */
    bool setKey(const uint8_t *key, size_t len)
    {
        if (len != 16) return false;
        return mbedtls_aes_setkey_enc(&_enc, key, 128) == 0 &&
               mbedtls_aes_setkey_dec(&_dec, key, 128) == 0;
    }

    void encryptBlock(uint8_t *out, const uint8_t *in)
    {
        mbedtls_aes_crypt_ecb(&_enc, MBEDTLS_AES_ENCRYPT, in, out);
    }

    void decryptBlock(uint8_t *out, const uint8_t *in)
    {
        mbedtls_aes_crypt_ecb(&_dec, MBEDTLS_AES_DECRYPT, in, out);
    }

    static constexpr size_t blockSize() { return 16; }
    static constexpr size_t keySize()   { return 16; }

private:
    mbedtls_aes_context _enc, _dec;
};

#endif /* LS_COMPAT_AES_H */
