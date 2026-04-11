/*
 * TaterTOS64v3 mbedTLS configuration — stripped for TLS 1.2 + 1.3 client
 *
 * No filesystem, no OS entropy, no networking module, no server mode,
 * no obsolete ciphers, no threading. Entropy via fry_getrandom(),
 * socket I/O via fry_send()/fry_recv() callbacks.
 */

#ifndef TATERTOS_MBEDTLS_CONFIG_H
#define TATERTOS_MBEDTLS_CONFIG_H

/* ================================================================== */
/* System support                                                      */
/* ================================================================== */

#define MBEDTLS_HAVE_ASM

/* We do NOT have time() — disable time-dependent features */
/* #undef MBEDTLS_HAVE_TIME */
/* #undef MBEDTLS_HAVE_TIME_DATE */

/* Platform abstraction — we provide calloc/free via our libc */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
/* Use our libc calloc/free directly */
/* #define MBEDTLS_PLATFORM_STD_CALLOC   calloc */
/* #define MBEDTLS_PLATFORM_STD_FREE     free */

/* No filesystem */
/* #undef MBEDTLS_FS_IO */

/* No networking module — we provide send/recv callbacks */
/* #undef MBEDTLS_NET_C */

/* No threading */
/* #undef MBEDTLS_THREADING_C */

/* No timing */
/* #undef MBEDTLS_TIMING_C */

/* No platform entropy — we use PSA external RNG */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

/* ================================================================== */
/* Crypto features                                                     */
/* ================================================================== */

/* AES */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
/* AES-NI hardware acceleration on x86_64 */
#define MBEDTLS_AESNI_C

/* ChaCha20-Poly1305 */
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_POLY1305_C

/* Block cipher abstraction */
#define MBEDTLS_CIPHER_C

/* Hashes */
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C

/* HKDF (required for TLS 1.3 key schedule) */
#define MBEDTLS_HKDF_C

/* Bignum (required for RSA and ECC) */
#define MBEDTLS_BIGNUM_C

/* Elliptic curves (ECDHE) */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C

/* Curve selection — P-256 and P-384 cover most servers */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
/* Also enable x25519 for modern servers */
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

/* RSA (for server certificate verification) */
#define MBEDTLS_RSA_C

/* Public key abstraction */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* ASN.1 (for X.509 and key parsing) */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

/* OID tables */
#define MBEDTLS_OID_C

/* PEM parsing (certificates often in PEM format) */
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

/* X.509 certificate parsing */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRL_PARSE_C

/* ================================================================== */
/* PSA Crypto (required for TLS 1.3)                                   */
/* ================================================================== */

#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_CONFIG
#define MBEDTLS_USE_PSA_CRYPTO

/* ================================================================== */
/* TLS protocol                                                        */
/* ================================================================== */

/* Core TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3

/* Client only — no server */
#define MBEDTLS_SSL_CLI_C
/* #undef MBEDTLS_SSL_SRV_C */

/* Key exchange methods */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* TLS 1.3 key exchange */
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED

/* Server Name Indication (required by many HTTPS servers) */
#define MBEDTLS_SSL_SERVER_NAME_INDICATION

/* Keep peer certificate (required by TLS 1.3) */
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

/* Max content length (16KB is TLS standard) */
#define MBEDTLS_SSL_MAX_CONTENT_LEN 16384

/* ================================================================== */
/* Entropy / RNG                                                       */
/* ================================================================== */

/* CTR-DRBG seeded from our external RNG */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

/* ================================================================== */
/* Misc                                                                */
/* ================================================================== */

#define MBEDTLS_ERROR_C
#define MBEDTLS_VERSION_C
/* MBEDTLS_DEBUG_C disabled — f_dbg is never set, and every
 * MBEDTLS_SSL_DEBUG_MSG call allocates a 512-byte stack buffer via
 * mbedtls_debug_print_msg even when returning early on NULL f_dbg.
 * The TLS 1.3 handshake processes 10+ states in a single call to
 * mbedtls_ssl_handshake, each with multiple debug calls. This
 * accumulated stack pressure causes stack overflow on state 14
 * (FLUSH_BUFFERS) for the resource TLS connection. */
/* #define MBEDTLS_DEBUG_C */

/* Constant-time operations */
#define MBEDTLS_CONSTANT_TIME

/* ================================================================== */
/* Features explicitly DISABLED                                        */
/* ================================================================== */

/* Explicitly undefine features we excluded source files for.
   PSA crypto core references these unconditionally in some code paths,
   so the defines must be absent to compile out the dead code. */
#undef MBEDTLS_CMAC_C
#undef MBEDTLS_ECJPAKE_C
#undef MBEDTLS_DHM_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_CCM_C
#undef MBEDTLS_MD5_C
#undef MBEDTLS_SHA1_C
#undef MBEDTLS_SHA3_C
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_PKCS12_C
#undef MBEDTLS_LMS_C
#undef MBEDTLS_LMOTS_C
#undef MBEDTLS_HMAC_DRBG_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_SE_C
#undef MBEDTLS_MEMORY_BUFFER_ALLOC_C
#undef MBEDTLS_PK_WRITE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_COOKIE_C

#endif /* TATERTOS_MBEDTLS_CONFIG_H */
