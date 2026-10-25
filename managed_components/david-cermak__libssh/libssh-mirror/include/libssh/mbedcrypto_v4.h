#ifndef MBEDCRYPTO_V4_H
#define MBEDCRYPTO_V4_H

#include <stddef.h>

#include <mbedtls/version.h>
#ifndef MBEDTLS_VERSION_MAJOR
#include <mbedtls/build_info.h>
#endif

#if MBEDTLS_VERSION_MAJOR >= 4

#define SSH_MBEDTLS_RSA_KEY_PAIR_BUF_SIZE \
    PSA_KEY_EXPORT_RSA_KEY_PAIR_MAX_SIZE(PSA_VENDOR_RSA_MAX_KEY_BITS)
#define SSH_MBEDTLS_RSA_PUBKEY_BUF_SIZE \
    PSA_KEY_EXPORT_RSA_PUBLIC_KEY_MAX_SIZE(PSA_VENDOR_RSA_MAX_KEY_BITS)

#ifndef MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#endif

#include "mbedtls/asn1.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/private/bignum.h"
#include "mbedtls/private/chacha20.h"
#include "mbedtls/private/cipher.h"
#include "mbedtls/private/ecdsa.h"
#include "mbedtls/private/ecp.h"
#include "mbedtls/private/gcm.h"
#include "mbedtls/private/pk_private.h"
#include "mbedtls/private/poly1305.h"
#include "mbedtls/private/rsa.h"
#include "psa/crypto.h"
#include "psa/crypto_extra.h"
#include "psa/crypto_sizes.h"

/* 3DES removed in mbedTLS v4; keep compile-time symbols for legacy tables. */
#ifndef MBEDTLS_CIPHER_DES_EDE3_CBC
#define MBEDTLS_CIPHER_DES_EDE3_CBC MBEDTLS_CIPHER_NONE
#endif
#ifndef MBEDTLS_CIPHER_DES_EDE3_ECB
#define MBEDTLS_CIPHER_DES_EDE3_ECB MBEDTLS_CIPHER_NONE
#endif
#ifndef MBEDTLS_CIPHER_DES_CBC
#define MBEDTLS_CIPHER_DES_CBC MBEDTLS_CIPHER_NONE
#endif
#ifndef MBEDTLS_CIPHER_DES_ECB
#define MBEDTLS_CIPHER_DES_ECB MBEDTLS_CIPHER_NONE
#endif

#ifndef MBEDTLS_PK_RSA_ALT
#define MBEDTLS_PK_RSA_ALT MBEDTLS_PK_NONE
#endif
#ifndef MBEDTLS_PK_ECKEY_DH
#define MBEDTLS_PK_ECKEY_DH MBEDTLS_PK_ECKEY
#endif

#ifdef __cplusplus
extern "C" {
#endif

int ssh_mbedtls_initialized(void);
int ssh_mbedtls_rng(void *ctx, unsigned char *output, size_t len);

#define SSH_MBEDTLS_RNG     ssh_mbedtls_rng
#define SSH_MBEDTLS_RNG_CTX NULL

void ssh_mbedtls_strerror(int errnum, char *buffer, size_t buflen);

int ssh_mbedtls_pk_is_rsa(const mbedtls_pk_context *pk);

int ssh_mbedtls_pk_dup(const mbedtls_pk_context *src,
                       mbedtls_pk_context *dst,
                       int public_only);

int ssh_mbedtls_rsa_export_from_pk(const mbedtls_pk_context *pk,
                                   mbedtls_mpi *N,
                                   mbedtls_mpi *P,
                                   mbedtls_mpi *Q,
                                   mbedtls_mpi *D,
                                   mbedtls_mpi *E);

int ssh_mbedtls_rsa_export_iqmp_from_pk(const mbedtls_pk_context *pk,
                                        mbedtls_mpi *IQMP);

int ssh_mbedtls_pk_build_rsa_pubkey(mbedtls_pk_context *pk,
                                    const unsigned char *n,
                                    size_t nlen,
                                    const unsigned char *e,
                                    size_t elen);

int ssh_mbedtls_pk_build_rsa_privkey(mbedtls_pk_context *pk,
                                     const unsigned char *n,
                                     size_t nlen,
                                     const unsigned char *e,
                                     size_t elen,
                                     const unsigned char *d,
                                     size_t dlen,
                                     const unsigned char *p,
                                     size_t plen,
                                     const unsigned char *q,
                                     size_t qlen);

int ssh_mbedtls_pk_generate_rsa(mbedtls_pk_context *pk, int bits);

int ssh_mbedtls_pk_to_ecdsa(const mbedtls_pk_context *pk,
                            mbedtls_ecdsa_context *ecdsa);

int ssh_mbedtls_ecdh_compute_shared(mbedtls_ecp_group *grp,
                                    mbedtls_mpi *z,
                                    const mbedtls_ecp_point *Q,
                                    const mbedtls_mpi *d,
                                    int (*f_rng)(void *,
                                                 unsigned char *,
                                                 size_t),
                                    void *p_rng);

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_VERSION_MAJOR >= 4 */
#endif /* MBEDCRYPTO_V4_H */
