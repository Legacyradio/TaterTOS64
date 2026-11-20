#define OPENSSL_VERSION_NUMBER 0x30000000L
#define OPENSSL_VERSION "OpenSSL 3.0.0 (TaterTOS)"
#define SSLEAY_VERSION 0
#define OPENSSL_THREADS 1
#define OPENSSL_NO_COMP 1
long OpenSSL_version_num(void);
const char *OpenSSL_version(int t);
void OPENSSL_free(void *p);
