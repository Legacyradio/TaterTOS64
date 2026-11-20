#include "x509.h"
typedef struct dh_st DH;
typedef struct bio_st BIO;
#define PEM_R_NO_START_LINE 108
X509 *PEM_read_bio_X509(BIO *b, X509 **x, void *cb, void *u);
DH *PEM_read_bio_DHparams(BIO *b, DH **x, void *cb, void *u);
