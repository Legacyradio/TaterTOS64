/* OpenSSL shim — bio.h, evp.h, err.h, rand.h, rsa.h, crypto.h, pem.h, dh.h, x509v3.h */

/* bio.h */
typedef struct bio_st BIO;
BIO *BIO_new(const void *m);
BIO *BIO_new_mem_buf(const void *b, int l);
int BIO_free(BIO *b);
void BIO_free_all(BIO *b);
int BIO_read(BIO *b, void *d, int l);
int BIO_write(BIO *b, const void *d, int l);
int BIO_gets(BIO *b, char *s, int l);
int BIO_reset(BIO *b);
int BIO_eof(BIO *b);
int BIO_set_nbio(BIO *b, long n);
const void *BIO_s_mem(void);
const void *BIO_s_file(void);
int BIO_ctrl_pending(BIO *b);
int BIO_read_filename(BIO *b, const char *n);
int BIO_set_mem_eof_return(BIO *b, int v);
int BIO_printf(BIO *b, const char *fmt, ...);
int BIO_flush(BIO *b);
BIO *BIO_new_fp(void *stream, int flags);
long BIO_tell(BIO *b);
int BIO_puts(BIO *b, const char *s);
#define BIO_CLOSE 1
#define BIO_FP_TEXT 0x80
