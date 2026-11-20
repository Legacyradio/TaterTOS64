/* OpenSSL shim for BearSSL — x509.h */
#include <stddef.h>
#include "bio.h"
typedef struct x509_st X509;
typedef struct x509_store_st X509_STORE;
typedef struct x509_store_ctx_st X509_STORE_CTX;
typedef struct x509_verify_param_st X509_VERIFY_PARAM;
typedef struct x509_lookup_st X509_LOOKUP;
typedef struct x509_object_st X509_OBJECT;
#define X509_FILETYPE_PEM 1
#define X509_FILETYPE_ASN1 2
#define X509_V_OK 0
#define X509_V_FLAG_X509_STRICT 0
#define X509_CHECK_FLAG_NO_WILDCARDS 0
X509 *X509_new(void);
void X509_free(X509 *x);
X509_STORE *X509_STORE_new(void);
void X509_STORE_free(X509_STORE *s);
#define X509_V_ERR_HOSTNAME_MISMATCH 62
#define X509_V_ERR_IP_ADDRESS_MISMATCH 63
#define X509_V_FLAG_CRL_CHECK 2
#define X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT 1
typedef struct asn1_object_st ASN1_OBJECT;
typedef struct asn1_string_st ASN1_STRING;
typedef struct X509_name_st X509_NAME;
#define NID_subject_alt_name 85
#define NID_commonName 13
#define NID_organizationName 10
#define NID_organizationalUnitName 11
#define NID_countryName 14
#define NID_stateOrProvinceName 8
#define NID_localityName 7
