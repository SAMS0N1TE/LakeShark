#include "ls_signature.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include <string.h>
bool ls_signature_verify(const char *pem, const uint8_t hash[32], const uint8_t *der, size_t n) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)pem, strlen(pem) + 1) == 0 &&
              mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA) &&
              mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(pk)) == MBEDTLS_ECP_DP_SECP256R1 &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, der, n) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}
