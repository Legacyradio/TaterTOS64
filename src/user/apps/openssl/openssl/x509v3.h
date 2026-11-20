/* OpenSSL shim — x509v3.h (general names, extensions) */
typedef struct GENERAL_NAME_st GENERAL_NAME;
typedef struct GENERAL_NAMES_st GENERAL_NAMES;
typedef struct DIST_POINT_st DIST_POINT;
typedef struct ASN1_OCTET_STRING_st ASN1_OCTET_STRING;

/* general name types */
#define GEN_OTHERNAME 0
#define GEN_EMAIL 1
#define GEN_DNS 2
#define GEN_X400 3
#define GEN_DIRNAME 4
#define GEN_EDIPARTY 5
#define GEN_URI 6
#define GEN_IPADD 7
#define GEN_RID 8

GENERAL_NAME *GENERAL_NAME_new(void);
void GENERAL_NAME_free(GENERAL_NAME *gn);
void GENERAL_NAMES_free(GENERAL_NAMES *gns);
void ASN1_OCTET_STRING_free(ASN1_OCTET_STRING *a);
const unsigned char *ASN1_STRING_get0_data(const void *s);

/* verify error strings */
const char *X509_verify_cert_error_string(long e);

/* IP address */
int a2i_IPADDRESS(const char *s);
