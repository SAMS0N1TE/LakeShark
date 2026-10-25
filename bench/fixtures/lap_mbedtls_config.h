/* Minimal host verifier configuration; no TLS/network/platform integration. */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDSA_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_NO_UDBL_DIVISION

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_ASN1_WRITE_C
