/*
 * mbedTLS v4 / PSA helpers for libssh.
 */

#include "config.h"

#include "libssh/libmbedcrypto.h"
#include "libssh/mbedcrypto_v4.h"
#include "libssh/wrapper.h"

#if defined(HAVE_LIBMBEDCRYPTO) && MBEDTLS_VERSION_MAJOR >= 4

#include "psa/crypto.h"
#include <stdio.h>
#include <string.h>

/*
 * Internal mbedTLS symbols not exposed in public headers.
 * Verified against TF-PSA-Crypto v4.0.0 / v4.1.0. If linking fails after an
 * mbedTLS update, check drivers/builtin/src/rsa.c (parse helpers) and
 * extras/pk_rsa.c / extras/pk.c (PK import helpers).
 */
int mbedtls_rsa_parse_pubkey(mbedtls_rsa_context *rsa,
                             const unsigned char *key,
                             size_t keylen);
int mbedtls_rsa_parse_key(mbedtls_rsa_context *rsa,
                          const unsigned char *key,
                          size_t keylen);

int mbedtls_pk_rsa_set_key(mbedtls_pk_context *pk,
                           const unsigned char *key,
                           size_t key_len);
int mbedtls_pk_rsa_set_pubkey(mbedtls_pk_context *pk,
                              const unsigned char *key,
                              size_t key_len);
int mbedtls_pk_set_pubkey_from_prv(mbedtls_pk_context *pk);

static int ssh_mbedtls_rsa_compute_crt(const mbedtls_mpi *P,
                                       const mbedtls_mpi *Q,
                                       const mbedtls_mpi *D,
                                       mbedtls_mpi *DP,
                                       mbedtls_mpi *DQ,
                                       mbedtls_mpi *QP)
{
    mbedtls_mpi P1, Q1;
    int rc;

    mbedtls_mpi_init(&P1);
    mbedtls_mpi_init(&Q1);

    rc = mbedtls_mpi_sub_int(&P1, P, 1);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_mpi_sub_int(&Q1, Q, 1);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_mpi_mod_mpi(DP, D, &P1);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_mpi_mod_mpi(DQ, D, &Q1);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_mpi_inv_mod(QP, Q, P);

out:
    mbedtls_mpi_free(&P1);
    mbedtls_mpi_free(&Q1);
    return rc;
}

static int ssh_mbedtls_rsa_write_pubkey_der(const mbedtls_mpi *N,
                                            const mbedtls_mpi *E,
                                            unsigned char *buf,
                                            size_t buf_len,
                                            unsigned char **out,
                                            size_t *out_len)
{
    unsigned char *p = buf + buf_len;
    size_t len = 0;
    int ret;

    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_mpi(&p, buf, E));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_mpi(&p, buf, N));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_len(&p, buf, len));
    MBEDTLS_ASN1_CHK_ADD(len,
                         mbedtls_asn1_write_tag(&p,
                                                buf,
                                                MBEDTLS_ASN1_CONSTRUCTED |
                                                    MBEDTLS_ASN1_SEQUENCE));

    *out = p;
    *out_len = len;
    return 0;
}

static int ssh_mbedtls_rsa_write_privkey_der(const mbedtls_mpi *N,
                                             const mbedtls_mpi *E,
                                             const mbedtls_mpi *D,
                                             const mbedtls_mpi *P,
                                             const mbedtls_mpi *Q,
                                             const mbedtls_mpi *DP,
                                             const mbedtls_mpi *DQ,
                                             const mbedtls_mpi *QP,
                                             unsigned char *buf,
                                             size_t buf_len,
                                             unsigned char **out,
                                             size_t *out_len)
{
    unsigned char *p = buf + buf_len;
    size_t len = 0;
    int ret;

    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_mpi(&p, buf, QP));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_mpi(&p, buf, DQ));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_mpi(&p, buf, DP));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_mpi(&p, buf, Q));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_mpi(&p, buf, P));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_mpi(&p, buf, D));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_mpi(&p, buf, E));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_mpi(&p, buf, N));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_int(&p, buf, 0));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_len(&p, buf, len));
    MBEDTLS_ASN1_CHK_ADD(len,
                         mbedtls_asn1_write_tag(&p,
                                                buf,
                                                MBEDTLS_ASN1_CONSTRUCTED |
                                                    MBEDTLS_ASN1_SEQUENCE));

    *out = p;
    *out_len = len;
    return 0;
}

static int ssh_mbedtls_rsa_copy_mpis(const mbedtls_rsa_context *rsa,
                                     mbedtls_mpi *N,
                                     mbedtls_mpi *P,
                                     mbedtls_mpi *Q,
                                     mbedtls_mpi *D,
                                     mbedtls_mpi *E)
{
    int rc;

    if (N != NULL) {
        rc = mbedtls_mpi_copy(N, &rsa->MBEDTLS_PRIVATE(N));
        if (rc != 0) {
            return rc;
        }
    }
    if (P != NULL) {
        rc = mbedtls_mpi_copy(P, &rsa->MBEDTLS_PRIVATE(P));
        if (rc != 0) {
            return rc;
        }
    }
    if (Q != NULL) {
        rc = mbedtls_mpi_copy(Q, &rsa->MBEDTLS_PRIVATE(Q));
        if (rc != 0) {
            return rc;
        }
    }
    if (D != NULL) {
        rc = mbedtls_mpi_copy(D, &rsa->MBEDTLS_PRIVATE(D));
        if (rc != 0) {
            return rc;
        }
    }
    if (E != NULL) {
        rc = mbedtls_mpi_copy(E, &rsa->MBEDTLS_PRIVATE(E));
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int ssh_mbedtls_pk_load_rsa(const mbedtls_pk_context *pk,
                                   mbedtls_rsa_context *rsa,
                                   int private_key)
{
    const unsigned char *key = NULL;
    size_t key_len = 0;
    psa_status_t status;

    if (pk == NULL) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }

    if (private_key) {
        unsigned char buf[SSH_MBEDTLS_RSA_KEY_PAIR_BUF_SIZE];
        int rc;

        if (mbedtls_svc_key_id_is_null(pk->MBEDTLS_PRIVATE(priv_id))) {
            return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
        }

        status = psa_export_key(pk->MBEDTLS_PRIVATE(priv_id),
                                buf,
                                sizeof(buf),
                                &key_len);
        if (status != PSA_SUCCESS) {
            return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
        }

        rc = mbedtls_rsa_parse_key(rsa, buf, key_len);
        mbedtls_platform_zeroize(buf, sizeof(buf));
        return rc;
    }

    if (pk->MBEDTLS_PRIVATE(pub_raw_len) > 0) {
        key = pk->MBEDTLS_PRIVATE(pub_raw);
        key_len = pk->MBEDTLS_PRIVATE(pub_raw_len);
        return mbedtls_rsa_parse_pubkey(rsa, key, key_len);
    }

    if (!mbedtls_svc_key_id_is_null(pk->MBEDTLS_PRIVATE(priv_id))) {
        unsigned char buf[SSH_MBEDTLS_RSA_PUBKEY_BUF_SIZE];
        int rc;

        status = psa_export_public_key(pk->MBEDTLS_PRIVATE(priv_id),
                                       buf,
                                       sizeof(buf),
                                       &key_len);
        if (status != PSA_SUCCESS) {
            return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
        }

        rc = mbedtls_rsa_parse_pubkey(rsa, buf, key_len);
        mbedtls_platform_zeroize(buf, sizeof(buf));
        return rc;
    }

    return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
}

int ssh_mbedtls_pk_is_rsa(const mbedtls_pk_context *pk)
{
    psa_key_type_t psa_type;
    int can_do_rsa;

    if (pk == NULL) {
        return 0;
    }

    can_do_rsa = mbedtls_pk_can_do(pk, MBEDTLS_PK_RSA);
    if (can_do_rsa) {
        return 1;
    }

    psa_type = pk->MBEDTLS_PRIVATE(psa_type);
    return PSA_KEY_TYPE_IS_RSA(psa_type) != 0;
}

int ssh_mbedtls_rng(void *ctx, unsigned char *output, size_t len)
{
    int initialized;

    (void)ctx;
    initialized = ssh_mbedtls_initialized();
    if (!initialized) {
        return -1;
    }
    return psa_generate_random(output, len) == PSA_SUCCESS ? 0 : -1;
}

int ssh_mbedtls_rsa_export_from_pk(const mbedtls_pk_context *pk,
                                   mbedtls_mpi *N,
                                   mbedtls_mpi *P,
                                   mbedtls_mpi *Q,
                                   mbedtls_mpi *D,
                                   mbedtls_mpi *E)
{
    mbedtls_rsa_context rsa;
    int need_private;
    int rc;

    mbedtls_rsa_init(&rsa);
    need_private = (P != NULL) || (Q != NULL) || (D != NULL);
    rc = ssh_mbedtls_pk_load_rsa(pk, &rsa, need_private);
    if (rc == 0) {
        rc = ssh_mbedtls_rsa_copy_mpis(&rsa, N, P, Q, D, E);
    }
    mbedtls_rsa_free(&rsa);
    return rc;
}

int ssh_mbedtls_rsa_export_iqmp_from_pk(const mbedtls_pk_context *pk,
                                        mbedtls_mpi *IQMP)
{
    mbedtls_rsa_context rsa;
    int rc;

    if (IQMP == NULL) {
        return MBEDTLS_ERR_RSA_BAD_INPUT_DATA;
    }

    mbedtls_rsa_init(&rsa);
    rc = ssh_mbedtls_pk_load_rsa(pk, &rsa, 1);
    if (rc == 0) {
        rc = mbedtls_mpi_copy(IQMP, &rsa.MBEDTLS_PRIVATE(QP));
    }
    mbedtls_rsa_free(&rsa);
    return rc;
}

static int ssh_mbedtls_pk_dup_pubkey_raw(const mbedtls_pk_context *src,
                                         mbedtls_pk_context *dst)
{
    const mbedtls_pk_info_t *info = NULL;
    int rc;

    if (src->MBEDTLS_PRIVATE(pub_raw_len) == 0) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }

    info = mbedtls_pk_info_from_type(mbedtls_pk_get_type(src));
    if (info == NULL) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }

    rc = mbedtls_pk_setup(dst, info);
    if (rc != 0) {
        return rc;
    }

    if (src->MBEDTLS_PRIVATE(pub_raw_len) > MBEDTLS_PK_MAX_PUBKEY_RAW_LEN) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }

    memcpy(dst->MBEDTLS_PRIVATE(pub_raw),
           src->MBEDTLS_PRIVATE(pub_raw),
           src->MBEDTLS_PRIVATE(pub_raw_len));
    dst->MBEDTLS_PRIVATE(pub_raw_len) = src->MBEDTLS_PRIVATE(pub_raw_len);
    dst->MBEDTLS_PRIVATE(psa_type) = src->MBEDTLS_PRIVATE(psa_type);
    dst->MBEDTLS_PRIVATE(bits) = src->MBEDTLS_PRIVATE(bits);
#if defined(PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY)
    dst->MBEDTLS_PRIVATE(ec_family) = src->MBEDTLS_PRIVATE(ec_family);
#endif
    return 0;
}

int ssh_mbedtls_pk_dup(const mbedtls_pk_context *src,
                       mbedtls_pk_context *dst,
                       int public_only)
{
    if (src == NULL || mbedtls_pk_get_type(src) == MBEDTLS_PK_NONE) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }

    if (!mbedtls_svc_key_id_is_null(src->MBEDTLS_PRIVATE(priv_id))) {
        if (public_only) {
            return mbedtls_pk_copy_public_from_psa(
                src->MBEDTLS_PRIVATE(priv_id),
                dst);
        }
        return mbedtls_pk_copy_from_psa(src->MBEDTLS_PRIVATE(priv_id), dst);
    }

    if (public_only) {
        return ssh_mbedtls_pk_dup_pubkey_raw(src, dst);
    }

    return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
}

/*
 * TF-PSA-Crypto v4 removes mbedtls_rsa_import_raw/complete from the public
 * API. Import RSA keys by encoding PKCS#1 DER and loading it through the
 * internal mbedtls_pk_rsa_set_key() / mbedtls_pk_rsa_set_pubkey() helpers.
 */
static int ssh_mbedtls_pk_import_rsa_pkcs1(mbedtls_pk_context *pk,
                                           const unsigned char *der,
                                           size_t der_len,
                                           int private_key)
{
    const mbedtls_pk_info_t *info = NULL;
    int rc;

    mbedtls_pk_free(pk);
    mbedtls_pk_init(pk);

    info = mbedtls_pk_info_from_type(MBEDTLS_PK_RSA);
    if (info == NULL) {
        return MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE;
    }

    rc = mbedtls_pk_setup(pk, info);
    if (rc != 0) {
        return rc;
    }

    if (private_key) {
        rc = mbedtls_pk_rsa_set_key(pk, der, der_len);
        if (rc != 0) {
            mbedtls_pk_free(pk);
            return rc;
        }
        return mbedtls_pk_set_pubkey_from_prv(pk);
    }

    rc = mbedtls_pk_rsa_set_pubkey(pk, der, der_len);
    if (rc != 0) {
        mbedtls_pk_free(pk);
    }
    return rc;
}

int ssh_mbedtls_pk_build_rsa_pubkey(mbedtls_pk_context *pk,
                                    const unsigned char *n,
                                    size_t nlen,
                                    const unsigned char *e,
                                    size_t elen)
{
    unsigned char buf[SSH_MBEDTLS_RSA_PUBKEY_BUF_SIZE];
    unsigned char *der = NULL;
    size_t der_len = 0;
    mbedtls_mpi N, E;
    int rc;

    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&E);

    rc = mbedtls_mpi_read_binary(&N, n, nlen);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_mpi_read_binary(&E, e, elen);
    if (rc != 0) {
        goto out;
    }

    rc = ssh_mbedtls_rsa_write_pubkey_der(&N,
                                          &E,
                                          buf,
                                          sizeof(buf),
                                          &der,
                                          &der_len);
    if (rc != 0) {
        goto out;
    }

    rc = ssh_mbedtls_pk_import_rsa_pkcs1(pk, der, der_len, 0);

out:
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&E);
    return rc;
}

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
                                     size_t qlen)
{
    unsigned char buf[SSH_MBEDTLS_RSA_KEY_PAIR_BUF_SIZE];
    unsigned char *der = NULL;
    size_t der_len = 0;
    mbedtls_mpi N, E, D, P, Q, DP, DQ, QP;
    int rc;

    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&D);
    mbedtls_mpi_init(&P);
    mbedtls_mpi_init(&Q);
    mbedtls_mpi_init(&DP);
    mbedtls_mpi_init(&DQ);
    mbedtls_mpi_init(&QP);

    rc = mbedtls_mpi_read_binary(&N, n, nlen);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_mpi_read_binary(&E, e, elen);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_mpi_read_binary(&D, d, dlen);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_mpi_read_binary(&P, p, plen);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_mpi_read_binary(&Q, q, qlen);
    if (rc != 0) {
        goto out;
    }

    rc = ssh_mbedtls_rsa_compute_crt(&P, &Q, &D, &DP, &DQ, &QP);
    if (rc != 0) {
        goto out;
    }

    rc = ssh_mbedtls_rsa_write_privkey_der(&N,
                                           &E,
                                           &D,
                                           &P,
                                           &Q,
                                           &DP,
                                           &DQ,
                                           &QP,
                                           buf,
                                           sizeof(buf),
                                           &der,
                                           &der_len);
    if (rc != 0) {
        goto out;
    }

    rc = ssh_mbedtls_pk_import_rsa_pkcs1(pk, der, der_len, 1);

out:
    mbedtls_platform_zeroize(buf, sizeof(buf));
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&D);
    mbedtls_mpi_free(&P);
    mbedtls_mpi_free(&Q);
    mbedtls_mpi_free(&DP);
    mbedtls_mpi_free(&DQ);
    mbedtls_mpi_free(&QP);
    return rc;
}

int ssh_mbedtls_pk_generate_rsa(mbedtls_pk_context *pk, int bits)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status;
    int rc;

    psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attr, (size_t)bits);
    psa_set_key_usage_flags(
        &attr,
        PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE |
            PSA_KEY_USAGE_VERIFY_HASH | PSA_KEY_USAGE_VERIFY_MESSAGE |
            PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_COPY);
    psa_set_key_algorithm(&attr, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_ANY_HASH));
#if defined(MBEDTLS_PSA_CRYPTO_C)
    psa_set_key_enrollment_algorithm(&attr, PSA_ALG_RSA_PSS(PSA_ALG_ANY_HASH));
#endif

    status = psa_generate_key(&attr, &key_id);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }

    rc = mbedtls_pk_copy_from_psa(key_id, pk);
    psa_destroy_key(key_id);
    return rc;
}

#if defined(PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY)
static mbedtls_ecp_group_id
ssh_mbedtls_psa_ec_to_group_id(psa_ecc_family_t family, size_t bits)
{
    if (family == PSA_ECC_FAMILY_SECP_R1) {
        switch (bits) {
        case 256:
            return MBEDTLS_ECP_DP_SECP256R1;
        case 384:
            return MBEDTLS_ECP_DP_SECP384R1;
        case 521:
            return MBEDTLS_ECP_DP_SECP521R1;
        default:
            break;
        }
    }
    return MBEDTLS_ECP_DP_NONE;
}
#endif /* PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY */

int ssh_mbedtls_pk_to_ecdsa(const mbedtls_pk_context *pk,
                            mbedtls_ecdsa_context *ecdsa)
{
#if defined(PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY)
    unsigned char priv[PSA_KEY_EXPORT_ECC_KEY_PAIR_MAX_SIZE(
        PSA_VENDOR_ECC_MAX_CURVE_BITS)];
    size_t priv_len = 0;
    mbedtls_ecp_group_id gid;
    psa_status_t status;
    int rc;

    if (pk == NULL ||
        mbedtls_svc_key_id_is_null(pk->MBEDTLS_PRIVATE(priv_id))) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }

    gid = ssh_mbedtls_psa_ec_to_group_id(pk->MBEDTLS_PRIVATE(ec_family),
                                         pk->MBEDTLS_PRIVATE(bits));
    if (gid == MBEDTLS_ECP_DP_NONE) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }

    status = psa_export_key(pk->MBEDTLS_PRIVATE(priv_id),
                            priv,
                            sizeof(priv),
                            &priv_len);
    if (status != PSA_SUCCESS) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }

    mbedtls_ecdsa_init(ecdsa);
    rc = mbedtls_ecp_read_key(gid, ecdsa, priv, priv_len);
    if (rc != 0) {
        goto fail;
    }

    if (pk->MBEDTLS_PRIVATE(pub_raw_len) > 0) {
        rc = mbedtls_ecp_point_read_binary(&ecdsa->MBEDTLS_PRIVATE(grp),
                                           &ecdsa->MBEDTLS_PRIVATE(Q),
                                           pk->MBEDTLS_PRIVATE(pub_raw),
                                           pk->MBEDTLS_PRIVATE(pub_raw_len));
        if (rc != 0) {
            goto fail;
        }
    }

    mbedtls_platform_zeroize(priv, sizeof(priv));
    return 0;

fail:
    mbedtls_ecdsa_free(ecdsa);
    mbedtls_platform_zeroize(priv, sizeof(priv));
    return rc;
#else
    (void)pk;
    (void)ecdsa;
    return MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE;
#endif /* PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY */
}

#if defined(MBEDTLS_ECP_C)
int ssh_mbedtls_ecdh_compute_shared(mbedtls_ecp_group *grp,
                                    mbedtls_mpi *z,
                                    const mbedtls_ecp_point *Q,
                                    const mbedtls_mpi *d,
                                    int (*f_rng)(void *,
                                                 unsigned char *,
                                                 size_t),
                                    void *p_rng)
{
    int ret = -1;
    mbedtls_ecp_point P;

    mbedtls_ecp_point_init(&P);

    ret = mbedtls_ecp_mul_restartable(grp, &P, d, Q, f_rng, p_rng, NULL);
    if (ret != 0) {
        goto cleanup;
    }

    if (mbedtls_ecp_is_zero(&P)) {
        ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
        goto cleanup;
    }

    ret = mbedtls_mpi_copy(z, &P.MBEDTLS_PRIVATE(X));

cleanup:
    mbedtls_ecp_point_free(&P);
    return ret;
}
#endif /* MBEDTLS_ECP_C */

void ssh_mbedtls_strerror(int errnum, char *buffer, size_t buflen)
{
    if (buffer == NULL || buflen == 0) {
        return;
    }

    if (errnum == 0) {
        (void)snprintf(buffer, buflen, "SUCCESS");
        return;
    }

    (void)snprintf(buffer, buflen, "error -0x%04x", -errnum);
}

int ssh_mbedtls_random(void *where, int len, int strong)
{
    (void)strong;
    if (!ssh_mbedtls_initialized()) {
        return 0;
    }
    return psa_generate_random(where, (size_t)len) == PSA_SUCCESS;
}

void ssh_reseed(void)
{
    /* No-op for PSA */
}

struct ssh_mbedtls_hmac_context {
    psa_mac_operation_t op;
    psa_key_id_t key_id;
    size_t mac_size;
};

static psa_algorithm_t ssh_hmac_type_to_psa(enum ssh_hmac_e type)
{
    switch (type) {
    case SSH_HMAC_SHA1:
        return PSA_ALG_HMAC(PSA_ALG_SHA_1);
    case SSH_HMAC_SHA256:
        return PSA_ALG_HMAC(PSA_ALG_SHA_256);
    case SSH_HMAC_SHA512:
        return PSA_ALG_HMAC(PSA_ALG_SHA_512);
    default:
        return 0;
    }
}

static void ssh_mbedtls_hmac_cleanup(struct ssh_mbedtls_hmac_context *ctx)
{
    if (ctx == NULL) {
        return;
    }

    psa_mac_abort(&ctx->op);
    if (ctx->key_id != PSA_KEY_ID_NULL) {
        psa_destroy_key(ctx->key_id);
    }
    free(ctx);
}

HMACCTX hmac_init(const void *key, size_t len, enum ssh_hmac_e type)
{
    struct ssh_mbedtls_hmac_context *ctx = NULL;
    psa_algorithm_t alg;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status;

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }

    alg = ssh_hmac_type_to_psa(type);
    if (alg == 0) {
        goto error;
    }

    ctx->mac_size = PSA_MAC_LENGTH(PSA_KEY_TYPE_HMAC, len * 8, alg);
    if (ctx->mac_size == 0) {
        switch (type) {
        case SSH_HMAC_SHA1:
            ctx->mac_size = SHA_DIGEST_LENGTH;
            break;
        case SSH_HMAC_SHA256:
            ctx->mac_size = SHA256_DIGEST_LENGTH;
            break;
        case SSH_HMAC_SHA512:
            ctx->mac_size = SHA512_DIGEST_LENGTH;
            break;
        default:
            goto error;
        }
    }

    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, alg);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, len * 8);

    status = psa_import_key(&attributes, key, len, &ctx->key_id);
    if (status != PSA_SUCCESS) {
        goto error;
    }

    status = psa_mac_sign_setup(&ctx->op, ctx->key_id, alg);
    if (status != PSA_SUCCESS) {
        goto error;
    }

    return ctx;

error:
    ssh_mbedtls_hmac_cleanup(ctx);
    return NULL;
}

/* libssh HMAC wrappers return 1 on success, 0 on failure. */
int hmac_update(HMACCTX c, const void *data, size_t len)
{
    psa_status_t status;

    status = psa_mac_update(&c->op, data, len);
    if (status != PSA_SUCCESS) {
        ssh_mbedtls_hmac_cleanup(c);
        return 0;
    }
    return 1;
}

int hmac_final(HMACCTX c, unsigned char *hashmacbuf, size_t *len)
{
    psa_status_t status;
    size_t mac_len = 0;
    int ret = 0;

    status = psa_mac_sign_finish(&c->op, hashmacbuf, c->mac_size, &mac_len);
    if (status == PSA_SUCCESS && mac_len <= c->mac_size) {
        *len = mac_len;
        ret = 1;
    } else {
        psa_mac_abort(&c->op);
    }
    if (c->key_id != PSA_KEY_ID_NULL) {
        psa_destroy_key(c->key_id);
    }
    free(c);
    return ret;
}

#endif /* HAVE_LIBMBEDCRYPTO && MBEDTLS_VERSION_MAJOR >= 4 */
