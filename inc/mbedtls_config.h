#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* Workaround for some mbedtls source files using INT_MAX without including limits.h */
#include <limits.h>

/*============================================================================
 * Platform / memory (RP2350 has 520KB RAM)
 *============================================================================*/
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* Smaller implementations */
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_ECP_NIST_OPTIM

/* TLS record buffers (16KB is the max TLS record size) */
#define MBEDTLS_SSL_MAX_CONTENT_LEN 16384
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

/* Reduce MPI (bignum) window size to save RAM */
#define MBEDTLS_MPI_WINDOW_SIZE 2
#define MBEDTLS_MPI_MAX_SIZE 384

/*============================================================================
 * TLS 1.2 client (HTTPS via altcp_tls)
 *============================================================================*/
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ALPN  // for altcp_tls_configure_alpn_protocols

/*============================================================================
 * ECC curves used by modern HTTPS servers
 *============================================================================*/
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED   /* X25519 ECDHE */

/*============================================================================
 * Key exchange - ECDHE only (no RSA key exchange)
 *============================================================================*/
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/*============================================================================
 * Ciphers
 *============================================================================*/
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_GCM_C
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C

/*============================================================================
 * Core crypto
 *============================================================================*/
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ENTROPY_SHA256_ACCUMULATOR
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C

/* RSA — most server certs are RSA-signed */
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

/* ECDSA / ECDH */
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C

/*============================================================================
 * X.509 certificate parsing (verify server cert chain)
 *============================================================================*/
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

/*============================================================================
 * Misc
 *============================================================================*/
#define MBEDTLS_ERROR_C
#define MBEDTLS_DEBUG_C  /* TLS debug — see why CloudFront never sends response */
#define __unix__

#endif  // MBEDTLS_CONFIG_H
