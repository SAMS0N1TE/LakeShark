

#ifndef LS_COMPAT_SHA256_H
#define LS_COMPAT_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

class SHA256 {
public:
    SHA256()  { plain_begin(); }
    ~SHA256() { finish(); }

    void reset() { finish(); plain_begin(); }

    void update(const void *data, size_t len)
    {
        if (_hmac) mbedtls_md_hmac_update(&_md, (const unsigned char *)data, len);
        else       mbedtls_sha256_update(&_sha, (const unsigned char *)data, len);
    }

    /* Truncating on purpose: see the header comment. */
    void finalize(void *out, size_t len)
    {
        uint8_t full[32];
        mbedtls_sha256_finish(&_sha, full);
        if (len > sizeof(full)) len = sizeof(full);
        memcpy(out, full, len);
        reset();
    }

    void resetHMAC(const void *key, size_t key_len)
    {
        finish();
        mbedtls_md_init(&_md);
        mbedtls_md_setup(&_md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
        mbedtls_md_hmac_starts(&_md, (const unsigned char *)key, key_len);
        _hmac = true;
        _live = true;
    }

    /* The key arguments are part of rweather's signature and are unused
       here - mbedtls keeps the key in the context from resetHMAC. Taking
       them anyway keeps MeshCore's call sites untouched. */
    void finalizeHMAC(const void *key, size_t key_len, void *out, size_t len)
    {
        (void)key; (void)key_len;
        uint8_t full[32];
        mbedtls_md_hmac_finish(&_md, full);
        if (len > sizeof(full)) len = sizeof(full);
        memcpy(out, full, len);
        reset();
    }

private:
    void plain_begin()
    {
        mbedtls_sha256_init(&_sha);
        mbedtls_sha256_starts(&_sha, 0);   /* 0 = SHA-256, not SHA-224 */
        _hmac = false;
        _live = true;
    }

    void finish()
    {
        if (!_live) return;
        if (_hmac) mbedtls_md_free(&_md);
        else       mbedtls_sha256_free(&_sha);
        _live = false;
    }

    mbedtls_sha256_context _sha;
    mbedtls_md_context_t   _md;
    bool _hmac = false;
    bool _live = false;
};

#endif /* LS_COMPAT_SHA256_H */
