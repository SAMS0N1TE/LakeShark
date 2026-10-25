#ifndef MBEDCRYPTO_COMPAT_H
#define MBEDCRYPTO_COMPAT_H

#include <mbedtls/version.h>

#ifndef MBEDTLS_VERSION_MAJOR
#include <mbedtls/build_info.h>
#endif /* MBEDTLS_VERSION_MAJOR */

/*
 * Heavy v4-only headers (private RSA/ECP/…) live in mbedcrypto_v4.h.
 * Include that file explicitly only from translation units that need it.
 */
#if MBEDTLS_VERSION_MAJOR < 4
#include <mbedtls/cipher.h>
#endif

#if MBEDTLS_VERSION_MAJOR < 3

static inline size_t
mbedtls_cipher_info_get_key_bitlen(const mbedtls_cipher_info_t *info)
{
    if (info == NULL) {
        return 0;
    }
    return info->key_bitlen;
}

static inline size_t
mbedtls_cipher_info_get_iv_size(const mbedtls_cipher_info_t *info)
{
    if (info == NULL) {
        return 0;
    }
    return (size_t)info->iv_size;
}

#define MBEDTLS_PRIVATE(X) X

#ifdef HAVE_MBEDTLS_CURVE25519
#include <mbedtls/ecdh.h>

#define MBEDTLS_ECDH_PRIVATE(X) X
#define MBEDTLS_ECDH_PARAMS(X)  X
typedef mbedtls_ecdh_context mbedtls_ecdh_params;
#endif /* HAVE_MBEDTLS_CURVE25519 */

#else /* MBEDTLS_VERSION_MAJOR < 3 */

#ifdef HAVE_MBEDTLS_CURVE25519
#include <mbedtls/ecdh.h>

#define MBEDTLS_ECDH_PRIVATE(X) MBEDTLS_PRIVATE(X)
#define MBEDTLS_ECDH_PARAMS(X)  X.MBEDTLS_PRIVATE(ctx).MBEDTLS_PRIVATE(mbed_ecdh)
typedef mbedtls_ecdh_context_mbed mbedtls_ecdh_params;
#endif /* HAVE_MBEDTLS_CURVE25519 */

#endif /* MBEDTLS_VERSION_MAJOR < 3 */

#if MBEDTLS_VERSION_MAJOR >= 4
void ssh_mbedtls_strerror(int errnum, char *buffer, size_t buflen);
#else
#include <mbedtls/error.h>
static inline void ssh_mbedtls_strerror(int errnum, char *buffer, size_t buflen)
{
    mbedtls_strerror(errnum, buffer, buflen);
}
/* On v2/v3, delegate to the library-provided mbedtls_ecdh_compute_shared. */
#define ssh_mbedtls_ecdh_compute_shared mbedtls_ecdh_compute_shared
#endif

#endif /* MBEDCRYPTO_COMPAT_H */
