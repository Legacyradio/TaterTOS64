/* TaterTOS OpenSSL shim — ssl.h */
#ifndef OSSL_SHIM_SSL_H
#define OSSL_SHIM_SSL_H
#include <stddef.h>
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct ssl_session_st SSL_SESSION;
typedef struct ssl_cipher_st SSL_CIPHER;
typedef struct ssl_method_st SSL_METHOD;
#define TLS1_3_VERSION 0x0304
#define TLS1_2_VERSION 0x0303
#define TLS1_1_VERSION 0x0302
#define TLS1_VERSION 0x0301
#define SSL3_VERSION 0x0300
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
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define SSL_OP_NO_SSLv2 0
#define SSL_OP_NO_SSLv3 0
#define SSL_MODE_AUTO_RETRY 4
#define SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER 8
#define SSL_MODE_ENABLE_PARTIAL_WRITE 1
#define SSL_SENT_SHUTDOWN 1
#define SSL_RECEIVED_SHUTDOWN 2
SSL_CTX *SSL_CTX_new(const SSL_METHOD *m);
void SSL_CTX_free(SSL_CTX *c);
int SSL_CTX_set_cipher_list(SSL_CTX *c, const char *s);
int SSL_CTX_set_options(SSL_CTX *c, long o);
long SSL_CTX_get_options(const SSL_CTX *c);
int SSL_CTX_set_mode(SSL_CTX *c, long m);
int SSL_CTX_set_verify(SSL_CTX *c, int m, int (*cb)(int,void*));
int SSL_CTX_set_verify_depth(SSL_CTX *c, int d);
int SSL_CTX_load_verify_locations(SSL_CTX *c, const char *f, const char *d);
int SSL_CTX_use_certificate_file(SSL_CTX *c, const char *f, int t);
int SSL_CTX_use_PrivateKey_file(SSL_CTX *c, const char *f, int t);
int SSL_CTX_check_private_key(const SSL_CTX *c);
int SSL_CTX_set_session_id_context(SSL_CTX *c, const unsigned char *s, unsigned int l);
int SSL_CTX_set_alpn_protos(SSL_CTX *c, const unsigned char *p, unsigned int l);
void SSL_CTX_set_security_level(SSL_CTX *c, int l);
SSL *SSL_new(SSL_CTX *c);
void SSL_free(SSL *s);
int SSL_set_fd(SSL *s, int fd);
int SSL_get_fd(const SSL *s);
int SSL_accept(SSL *s);
int SSL_connect(SSL *s);
int SSL_do_handshake(SSL *s);
int SSL_read(SSL *s, void *b, int n);
int SSL_write(SSL *s, const void *b, int n);
int SSL_shutdown(SSL *s);
int SSL_get_error(const SSL *s, int r);
int SSL_want_read(const SSL *s);
int SSL_want_write(const SSL *s);
long SSL_get_verify_result(const SSL *s);
const SSL_CIPHER *SSL_get_current_cipher(const SSL *s);
void *SSL_get_peer_certificate(const SSL *s);
int SSL_set_tlsext_host_name(SSL *s, const char *n);
const SSL_METHOD *TLS_client_method(void);
const SSL_METHOD *TLS_server_method(void);
#endif
void *SSL_get_app_data(SSL *s);
void SSL_CTX_set_msg_callback(SSL_CTX *c, void (*cb)(int,int,int,const void*,size_t,SSL*,void*));
#define SSL3_RT_CHANGE_CIPHER_SPEC 20
#define SSL3_RT_ALERT 21
#define SSL3_RT_HANDSHAKE 22
#define SSL3_MT_CHANGE_CIPHER_SPEC 1
void SSL_CTX_set_keylog_callback(SSL_CTX *c, void (*cb)(const SSL*, const char*));
void *SSL_get_peer_cert_chain(const SSL *s);
#define SSL_ERROR_WANT_X509_LOOKUP 4
#define SSL_ERROR_WANT_CONNECT 7
#define SSL_R_CERTIFICATE_VERIFY_FAILED 134
