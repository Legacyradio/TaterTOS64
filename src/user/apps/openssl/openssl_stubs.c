/*
 * TaterTOS64v3 — OpenSSL compatibility shim (stubs)
 *
 * CPython's _ssl.c requires ~400 OpenSSL API symbols.
 * This file provides stub implementations — all return 0/error/NULL.
 *
 * As BearSSL wrappers are written, individual functions can be
 * replaced with real implementations.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Forward-declare opaque types we define in headers */
typedef struct SSL_CTX SSL_CTX;
typedef struct SSL SSL;
typedef struct SSL_SESSION SSL_SESSION;
typedef struct SSL_CIPHER SSL_CIPHER;
typedef struct SSL_METHOD SSL_METHOD;
typedef struct BIO BIO;
typedef struct EVP_PKEY EVP_PKEY;
typedef struct EVP_MD EVP_MD;
typedef struct EVP_CIPHER EVP_CIPHER;
typedef struct X509 X509;
typedef struct X509_STORE X509_STORE;
typedef struct X509_VERIFY_PARAM X509_VERIFY_PARAM;
typedef struct X509_LOOKUP X509_LOOKUP;
typedef struct X509_OBJECT X509_OBJECT;
typedef struct X509_STORE_CTX X509_STORE_CTX;
typedef struct EC_KEY EC_KEY;
typedef struct DH DH;
typedef struct RSA RSA;

/* ===================================================================
 * SSL / TLS
 * =================================================================== */

#define TLS1_3_VERSION 0x0304
#define TLS1_2_VERSION 0x0303
#define SSL_ERROR_NONE 0
#define SSL_ERROR_SSL 1
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_SYSCALL 5
#define SSL_ERROR_ZERO_RETURN 6
#define SSL_VERIFY_NONE 0
#define SSL_VERIFY_PEER 1
#define SSL_FILETYPE_PEM 1
#define SSL_FILETYPE_ASN1 2
#define SSL_CTRL_OPTIONS 32
#define SSL_OP_NO_SSLv2 0
#define SSL_OP_NO_SSLv3 0
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define SSL_MODE_AUTO_RETRY 4
#define SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER 8
#define SSL_MODE_ENABLE_PARTIAL_WRITE 1
#define SSL_SENT_SHUTDOWN 1
#define SSL_RECEIVED_SHUTDOWN 2

SSL_CTX *SSL_CTX_new(const SSL_METHOD *m) { return NULL; }
void SSL_CTX_free(SSL_CTX *c) {}
int SSL_CTX_set_cipher_list(SSL_CTX *c, const char *s) { return 0; }
int SSL_CTX_set_protocol_version(SSL_CTX *c, int v) { return 0; }
int SSL_CTX_set_max_protocol_version(SSL_CTX *c, int v) { return 0; }
int SSL_CTX_set_min_protocol_version(SSL_CTX *c, int v) { return 0; }
int SSL_CTX_set_options(SSL_CTX *c, long o) { return 0; }
long SSL_CTX_get_options(const SSL_CTX *c) { return 0; }
int SSL_CTX_set_mode(SSL_CTX *c, long m) { return 0; }
int SSL_CTX_set_verify(SSL_CTX *c, int m, int (*cb)(int,X509_STORE_CTX*)) { return 0; }
int SSL_CTX_set_verify_depth(SSL_CTX *c, int d) { return 0; }
int SSL_CTX_load_verify_locations(SSL_CTX *c, const char *f, const char *d) { return 0; }
int SSL_CTX_use_certificate_file(SSL_CTX *c, const char *f, int t) { return 0; }
int SSL_CTX_use_PrivateKey_file(SSL_CTX *c, const char *f, int t) { return 0; }
int SSL_CTX_check_private_key(const SSL_CTX *c) { return 0; }
int SSL_CTX_set_session_id_context(SSL_CTX *c, const unsigned char *s, unsigned int l) { return 0; }
int SSL_CTX_set_alpn_protos(SSL_CTX *c, const unsigned char *p, unsigned int l) { return 0; }
int SSL_CTX_set_alpn_select_cb(SSL_CTX *c, int (*cb)(SSL*,const unsigned char**,unsigned char*,const unsigned char*,unsigned int,void*), void *a) { return 0; }
void SSL_CTX_set_cert_verify_callback(SSL_CTX *c, int (*cb)(X509_STORE_CTX*,void*), void *a) {}
int SSL_CTX_set_num_tickets(SSL_CTX *c, size_t n) { return 0; }
int SSL_CTX_set_session_cache_mode(SSL_CTX *c, int m) { return 0; }
void SSL_CTX_set_security_level(SSL_CTX *c, int l) {}
void SSL_CTX_set_keylog_callback(SSL_CTX *c, void (*cb)(const SSL*,const char*)) {}
void SSL_CTX_set_post_handshake_auth(SSL_CTX *c, int v) {}
int SSL_CTX_use_certificate(SSL_CTX *c, X509 *x) { return 0; }
int SSL_CTX_use_PrivateKey(SSL_CTX *c, EVP_PKEY *k) { return 0; }
X509_STORE *SSL_CTX_get_cert_store(const SSL_CTX *c) { return NULL; }

SSL *SSL_new(SSL_CTX *c) { return NULL; }
void SSL_free(SSL *s) {}
int SSL_set_fd(SSL *s, int fd) { return 0; }
int SSL_set_rfd(SSL *s, int fd) { return 0; }
int SSL_set_wfd(SSL *s, int fd) { return 0; }
int SSL_get_fd(const SSL *s) { return -1; }
int SSL_accept(SSL *s) { return -1; }
int SSL_connect(SSL *s) { return -1; }
int SSL_do_handshake(SSL *s) { return -1; }
int SSL_read(SSL *s, void *b, int n) { return -1; }
int SSL_write(SSL *s, const void *b, int n) { return -1; }
int SSL_shutdown(SSL *s) { return 0; }
int SSL_get_error(const SSL *s, int r) { return SSL_ERROR_SSL; }
int SSL_want_read(const SSL *s) { return 0; }
int SSL_want_write(const SSL *s) { return 0; }
long SSL_get_verify_result(const SSL *s) { return 0; }
const SSL_CIPHER *SSL_get_current_cipher(const SSL *s) { return NULL; }
X509 *SSL_get_peer_certificate(const SSL *s) { return NULL; }
void SSL_set_verify(SSL *s, int m, int (*cb)(int,X509_STORE_CTX*)) {}
int SSL_set_tlsext_host_name(SSL *s, const char *n) { return 0; }
int SSL_get_shutdown(const SSL *s) { return 0; }
const SSL_METHOD *SSLv23_method(void) { return NULL; }
const SSL_METHOD *SSLv23_server_method(void) { return NULL; }
const SSL_METHOD *SSLv23_client_method(void) { return NULL; }
const SSL_METHOD *TLS_server_method(void) { return NULL; }
const SSL_METHOD *TLS_client_method(void) { return NULL; }
int SSL_session_reused(SSL *s) { return 0; }
void SSL_SESSION_free(SSL_SESSION *s) {}

/* ===================================================================
 * BIO (I/O abstraction)
 * =================================================================== */
BIO *BIO_new(const void *m) { return NULL; }
BIO *BIO_new_mem_buf(const void *b, int l) { return NULL; }
int BIO_free(BIO *b) { return 0; }
void BIO_free_all(BIO *b) {}
int BIO_read(BIO *b, void *d, int l) { return -1; }
int BIO_write(BIO *b, const void *d, int l) { return -1; }
int BIO_gets(BIO *b, char *s, int l) { return -1; }
int BIO_reset(BIO *b) { return 0; }
int BIO_eof(BIO *b) { return 1; }
int BIO_up_ref(BIO *b) { return 0; }
int BIO_set_nbio(BIO *b, long n) { return 0; }
void BIO_set_retry_read(BIO *b) {}
int BIO_ctrl_pending(BIO *b) { return 0; }
const void *BIO_s_mem(void) { return NULL; }
const void *BIO_s_file(void) { return NULL; }
void BIO_clear_retry_flags(BIO *b) {}
int BIO_read_filename(BIO *b, const char *n) { return 0; }
int BIO_set_mem_eof_return(BIO *b, int v) { return 0; }

/* ===================================================================
 * EVP (digest / cipher / key)
 * =================================================================== */
EVP_PKEY *EVP_PKEY_new(void) { return NULL; }
void EVP_PKEY_free(EVP_PKEY *k) {}
const EVP_MD *EVP_sha256(void) { return NULL; }

/* ===================================================================
 * X509 (certificates)
 * =================================================================== */
X509 *X509_new(void) { return NULL; }
void X509_free(X509 *x) {}
X509_STORE *X509_STORE_new(void) { return NULL; }
void X509_STORE_free(X509_STORE *s) {}

/* ===================================================================
 * ERR (error handling)
 * =================================================================== */
#define ERR_LIB_NONE 0
#define ERR_LIB_SSL 20
#define ERR_LIB_SYS 2
#define ERR_LIB_X509 11
#define ERR_LIB_PEM 9
#define ERR_LIB_X 12
#define ERR_GET_LIB(e) (((e)>>23)&0xff)
#define ERR_GET_REASON(e) ((e)&0x7fffff)
#define ERR_PACK(l,r) ((int)(((unsigned int)(l)<<23)|(unsigned int)(r)))

unsigned long ERR_get_error(void) { return 0; }
unsigned long ERR_peek_last_error(void) { return 0; }
void ERR_clear_error(void) {}
const char *ERR_reason_error_string(unsigned long e) { return "SSL not available (TaterTOS stub)"; }
unsigned long ERR_peek_error(void) { return 0; }

/* ===================================================================
 * RAND (random)
 * =================================================================== */
int RAND_bytes(unsigned char *b, int n) { memset(b,0,n); return 1; }
int RAND_status(void) { return 1; }
void RAND_add(const void *b, int n, double e) {}

/* ===================================================================
 * PEM (privacy-enhanced mail — cert format)
 * =================================================================== */
#define PEM_R_NO_START_LINE 108
X509 *PEM_read_bio_X509(BIO *b, X509 **x, void *cb, void *u) { return NULL; }
DH *PEM_read_bio_DHparams(BIO *b, DH **x, void *cb, void *u) { return NULL; }

/* ===================================================================
 * RSA / DH / EC
 * =================================================================== */
void RSA_free(RSA *r) {}
void DH_free(DH *d) {}
void EC_KEY_free(EC_KEY *k) {}

/* ===================================================================
 * OPENSSL version / misc
 * =================================================================== */
#define OPENSSL_VERSION_NUMBER 0x30000000L
#define OPENSSL_VERSION_MAJOR 3
#define OPENSSL_VERSION "OpenSSL 3.0.0 (TaterTOS BearSSL shim)"
void OPENSSL_free(void *p) {}
void *OPENSSL_malloc(size_t n) { return NULL; }

/* ===================================================================
 * OSSL (OpenSSL 3.x provider API — _ssl may reference these)
 * =================================================================== */
typedef void OSSL_LIB_CTX;
typedef void OSSL_PROVIDER;
void OSSL_LIB_CTX_free(OSSL_LIB_CTX *c) {}
OSSL_PROVIDER *OSSL_PROVIDER_load(OSSL_LIB_CTX *c, const char *n) { return NULL; }
int OSSL_PROVIDER_unload(OSSL_PROVIDER *p) { return 0; }

/* Additional SSL stubs */
void *SSL_get_app_data(SSL *s) { return NULL; }
void SSL_CTX_set_msg_callback(SSL_CTX *c, void (*cb)(int w, int v, int ct, const void *b, size_t l, SSL *s, void *a)) {}
int BIO_printf(BIO *b, const char *fmt, ...) { return 0; }

/* SSL3 record type constants */
#define SSL3_RT_CHANGE_CIPHER_SPEC 20
#define SSL3_RT_ALERT 21
#define SSL3_RT_HANDSHAKE 22
#define SSL3_MT_CHANGE_CIPHER_SPEC 1

/* Additional types */
typedef struct ocsp_response_st OCSP_RESPONSE;

int BIO_flush(BIO *b) { return 0; }
BIO *BIO_new_fp(void *stream, int flags) { return NULL; }
long BIO_tell(BIO *b) { return 0; }
int BIO_puts(BIO *b, const char *s) { return 0; }

/* More SSL error constants */
#define SSL_ERROR_WANT_X509_LOOKUP 4
#define SSL_ERROR_WANT_CONNECT 7
#define SSL_R_CERTIFICATE_VERIFY_FAILED 134
#define SSL_R_CERTIFICATE_ALREADY_EXISTS 88
#define SSL_R_SSLV3_ALERT_CERTIFICATE_UNKNOWN 46
#define SSL_R_TLSV1_ALERT_UNKNOWN_CA 48

/* X509 verify error string */
const char *X509_verify_cert_error_string(long e) { return "certificate verify failed (TaterTOS stub)"; }

/* a2i_IPADDRESS */
int a2i_IPADDRESS(const char *s) { return -1; }

/* ASN1 */
void ASN1_OCTET_STRING_free(void *a) {}
const unsigned char *ASN1_STRING_get0_data(const void *s) { return NULL; }

/* GENERAL_NAME */
typedef void GENERAL_NAME;
typedef void GENERAL_NAMES;
GENERAL_NAME *GENERAL_NAME_new(void) { return NULL; }
void GENERAL_NAME_free(GENERAL_NAME *gn) {}
void GENERAL_NAMES_free(GENERAL_NAMES *gns) {}

/* SSL_get_peer_cert_chain */
void *SSL_get_peer_cert_chain(const SSL *s) { return NULL; }

/* ASN1 stubs */
typedef void ASN1_OBJECT;
typedef void ASN1_STRING;
typedef void X509_NAME;
