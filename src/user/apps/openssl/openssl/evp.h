/* OpenSSL shim — evp.h */
typedef struct evp_pkey_st EVP_PKEY;
typedef struct evp_md_st EVP_MD;
typedef struct evp_cipher_st EVP_CIPHER;
EVP_PKEY *EVP_PKEY_new(void);
void EVP_PKEY_free(EVP_PKEY *k);
const EVP_MD *EVP_sha256(void);
