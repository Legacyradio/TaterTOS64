/*
 * tatertos_stubs.c — Stub symbols for mbedTLS features not included
 *
 * PSA crypto wrappers reference these algorithms unconditionally.
 * All stubs return error codes. None are reachable at runtime because
 * TLS 1.2/1.3 with AES-GCM/ChaCha20 + SHA-256/384/512 never calls
 * into DES, ARIA, Camellia, CCM, MD5, SHA-1, SHA-3, RIPEMD-160,
 * CMAC, HMAC-DRBG, ECJPAKE, or FFDH.
 */

#include <stddef.h>
#include <stdint.h>

/* Stub helper: all return -1 (generic feature-not-available error) */
#define STUB_VOID(name) void name(void *ctx) { (void)ctx; }
#define STUB_INT(name) int name() { return -1; }

/* ---- __udivti3: 128-bit division needed by bignum on some GCC configs ---- */
typedef unsigned __int128 uint128_t;
uint128_t __udivti3(uint128_t a, uint128_t b) {
    if (b == 0) return 0;
    uint128_t result = 0;
    uint128_t remainder = 0;
    for (int i = 127; i >= 0; i--) {
        remainder = (remainder << 1) | ((a >> i) & 1);
        if (remainder >= b) {
            remainder -= b;
            result |= ((uint128_t)1 << i);
        }
    }
    return result;
}

uint128_t __udivmodti4(uint128_t a, uint128_t b, uint128_t *rem) {
    if (b == 0) { if (rem) *rem = 0; return 0; }
    uint128_t result = 0;
    uint128_t remainder = 0;
    for (int i = 127; i >= 0; i--) {
        remainder = (remainder << 1) | ((a >> i) & 1);
        if (remainder >= b) {
            remainder -= b;
            result |= ((uint128_t)1 << i);
        }
    }
    if (rem) *rem = remainder;
    return result;
}

uint128_t __umodti3(uint128_t a, uint128_t b) {
    if (b == 0) return 0;
    uint128_t remainder = 0;
    for (int i = 127; i >= 0; i--) {
        remainder = (remainder << 1) | ((a >> i) & 1);
        if (remainder >= b) remainder -= b;
    }
    return remainder;
}

/* ---- DES / 3DES stubs ---- */
void mbedtls_des_init(void *ctx) { (void)ctx; }
void mbedtls_des_free(void *ctx) { (void)ctx; }
int mbedtls_des_setkey_enc(void *ctx, const unsigned char *key) { (void)ctx; (void)key; return -1; }
int mbedtls_des_setkey_dec(void *ctx, const unsigned char *key) { (void)ctx; (void)key; return -1; }
int mbedtls_des_crypt_ecb(void *ctx, const unsigned char *in, unsigned char *out) { (void)ctx; (void)in; (void)out; return -1; }
int mbedtls_des_crypt_cbc(void *ctx, int mode, size_t len, unsigned char iv[8], const unsigned char *in, unsigned char *out) { (void)ctx; (void)mode; (void)len; (void)iv; (void)in; (void)out; return -1; }
void mbedtls_des_key_set_parity(unsigned char key[8]) { (void)key; }
void mbedtls_des3_init(void *ctx) { (void)ctx; }
void mbedtls_des3_free(void *ctx) { (void)ctx; }
int mbedtls_des3_set2key_enc(void *ctx, const unsigned char *key) { (void)ctx; (void)key; return -1; }
int mbedtls_des3_set2key_dec(void *ctx, const unsigned char *key) { (void)ctx; (void)key; return -1; }
int mbedtls_des3_set3key_enc(void *ctx, const unsigned char *key) { (void)ctx; (void)key; return -1; }
int mbedtls_des3_set3key_dec(void *ctx, const unsigned char *key) { (void)ctx; (void)key; return -1; }
int mbedtls_des3_crypt_ecb(void *ctx, const unsigned char *in, unsigned char *out) { (void)ctx; (void)in; (void)out; return -1; }
int mbedtls_des3_crypt_cbc(void *ctx, int mode, size_t len, unsigned char iv[8], const unsigned char *in, unsigned char *out) { (void)ctx; (void)mode; (void)len; (void)iv; (void)in; (void)out; return -1; }

/* ---- ARIA stubs ---- */
void mbedtls_aria_init(void *ctx) { (void)ctx; }
void mbedtls_aria_free(void *ctx) { (void)ctx; }
int mbedtls_aria_setkey_enc(void *ctx, const unsigned char *key, unsigned int keybits) { (void)ctx; (void)key; (void)keybits; return -1; }
int mbedtls_aria_setkey_dec(void *ctx, const unsigned char *key, unsigned int keybits) { (void)ctx; (void)key; (void)keybits; return -1; }
int mbedtls_aria_crypt_ecb(void *ctx, const unsigned char *in, unsigned char *out) { (void)ctx; (void)in; (void)out; return -1; }
int mbedtls_aria_crypt_cbc(void *ctx, int mode, size_t len, unsigned char *iv, const unsigned char *in, unsigned char *out) { (void)ctx; (void)mode; (void)len; (void)iv; (void)in; (void)out; return -1; }
int mbedtls_aria_crypt_cfb128(void *ctx, int mode, size_t len, size_t *iv_off, unsigned char *iv, const unsigned char *in, unsigned char *out) { (void)ctx; (void)mode; (void)len; (void)iv_off; (void)iv; (void)in; (void)out; return -1; }
int mbedtls_aria_crypt_ctr(void *ctx, size_t len, size_t *nc_off, unsigned char *nc, unsigned char *sb, const unsigned char *in, unsigned char *out) { (void)ctx; (void)len; (void)nc_off; (void)nc; (void)sb; (void)in; (void)out; return -1; }

/* ---- Camellia stubs ---- */
void mbedtls_camellia_init(void *ctx) { (void)ctx; }
void mbedtls_camellia_free(void *ctx) { (void)ctx; }
int mbedtls_camellia_setkey_enc(void *ctx, const unsigned char *key, unsigned int keybits) { (void)ctx; (void)key; (void)keybits; return -1; }
int mbedtls_camellia_setkey_dec(void *ctx, const unsigned char *key, unsigned int keybits) { (void)ctx; (void)key; (void)keybits; return -1; }
int mbedtls_camellia_crypt_ecb(void *ctx, int mode, const unsigned char *in, unsigned char *out) { (void)ctx; (void)mode; (void)in; (void)out; return -1; }
int mbedtls_camellia_crypt_cbc(void *ctx, int mode, size_t len, unsigned char *iv, const unsigned char *in, unsigned char *out) { (void)ctx; (void)mode; (void)len; (void)iv; (void)in; (void)out; return -1; }
int mbedtls_camellia_crypt_cfb128(void *ctx, int mode, size_t len, size_t *iv_off, unsigned char *iv, const unsigned char *in, unsigned char *out) { (void)ctx; (void)mode; (void)len; (void)iv_off; (void)iv; (void)in; (void)out; return -1; }
int mbedtls_camellia_crypt_ctr(void *ctx, size_t len, size_t *nc_off, unsigned char *nc, unsigned char *sb, const unsigned char *in, unsigned char *out) { (void)ctx; (void)len; (void)nc_off; (void)nc; (void)sb; (void)in; (void)out; return -1; }

/* ---- CCM stubs ---- */
void mbedtls_ccm_init(void *ctx) { (void)ctx; }
void mbedtls_ccm_free(void *ctx) { (void)ctx; }
int mbedtls_ccm_setkey(void *ctx, int cipher, const unsigned char *key, unsigned int keybits) { (void)ctx; (void)cipher; (void)key; (void)keybits; return -1; }
int mbedtls_ccm_starts(void *ctx, int mode, const unsigned char *iv, size_t iv_len) { (void)ctx; (void)mode; (void)iv; (void)iv_len; return -1; }
int mbedtls_ccm_set_lengths(void *ctx, size_t total_ad_len, size_t plaintext_len, size_t tag_len) { (void)ctx; (void)total_ad_len; (void)plaintext_len; (void)tag_len; return -1; }
int mbedtls_ccm_update_ad(void *ctx, const unsigned char *ad, size_t ad_len) { (void)ctx; (void)ad; (void)ad_len; return -1; }
int mbedtls_ccm_update(void *ctx, const unsigned char *in, size_t in_len, unsigned char *out, size_t out_size, size_t *out_len) { (void)ctx; (void)in; (void)in_len; (void)out; (void)out_size; (void)out_len; return -1; }
int mbedtls_ccm_finish(void *ctx, unsigned char *tag, size_t tag_len) { (void)ctx; (void)tag; (void)tag_len; return -1; }
int mbedtls_ccm_encrypt_and_tag(void *ctx, size_t len, const unsigned char *iv, size_t iv_len, const unsigned char *ad, size_t ad_len, const unsigned char *in, unsigned char *out, unsigned char *tag, size_t tag_len) { (void)ctx; (void)len; (void)iv; (void)iv_len; (void)ad; (void)ad_len; (void)in; (void)out; (void)tag; (void)tag_len; return -1; }
int mbedtls_ccm_auth_decrypt(void *ctx, size_t len, const unsigned char *iv, size_t iv_len, const unsigned char *ad, size_t ad_len, const unsigned char *in, unsigned char *out, const unsigned char *tag, size_t tag_len) { (void)ctx; (void)len; (void)iv; (void)iv_len; (void)ad; (void)ad_len; (void)in; (void)out; (void)tag; (void)tag_len; return -1; }

/* ---- MD5 stubs ---- */
void mbedtls_md5_init(void *ctx) { (void)ctx; }
void mbedtls_md5_free(void *ctx) { (void)ctx; }
int mbedtls_md5_starts(void *ctx) { (void)ctx; return -1; }
int mbedtls_md5_update(void *ctx, const unsigned char *in, size_t ilen) { (void)ctx; (void)in; (void)ilen; return -1; }
int mbedtls_md5_finish(void *ctx, unsigned char out[16]) { (void)ctx; (void)out; return -1; }
int mbedtls_md5_clone(void *dst, const void *src) { (void)dst; (void)src; return -1; }
int mbedtls_md5(const unsigned char *in, size_t ilen, unsigned char out[16]) { (void)in; (void)ilen; (void)out; return -1; }

/* ---- SHA-1 stubs ---- */
void mbedtls_sha1_init(void *ctx) { (void)ctx; }
void mbedtls_sha1_free(void *ctx) { (void)ctx; }
int mbedtls_sha1_starts(void *ctx) { (void)ctx; return -1; }
int mbedtls_sha1_update(void *ctx, const unsigned char *in, size_t ilen) { (void)ctx; (void)in; (void)ilen; return -1; }
int mbedtls_sha1_finish(void *ctx, unsigned char out[20]) { (void)ctx; (void)out; return -1; }
int mbedtls_sha1_clone(void *dst, const void *src) { (void)dst; (void)src; return -1; }
int mbedtls_sha1(const unsigned char *in, size_t ilen, unsigned char out[20]) { (void)in; (void)ilen; (void)out; return -1; }

/* ---- RIPEMD-160 stubs ---- */
void mbedtls_ripemd160_init(void *ctx) { (void)ctx; }
void mbedtls_ripemd160_free(void *ctx) { (void)ctx; }
int mbedtls_ripemd160_starts(void *ctx) { (void)ctx; return -1; }
int mbedtls_ripemd160_update(void *ctx, const unsigned char *in, size_t ilen) { (void)ctx; (void)in; (void)ilen; return -1; }
int mbedtls_ripemd160_finish(void *ctx, unsigned char out[20]) { (void)ctx; (void)out; return -1; }
int mbedtls_ripemd160_clone(void *dst, const void *src) { (void)dst; (void)src; return -1; }
int mbedtls_ripemd160(const unsigned char *in, size_t ilen, unsigned char out[20]) { (void)in; (void)ilen; (void)out; return -1; }

/* ---- SHA-3 stubs ---- */
void mbedtls_sha3_init(void *ctx) { (void)ctx; }
void mbedtls_sha3_free(void *ctx) { (void)ctx; }
int mbedtls_sha3_starts(void *ctx, int id) { (void)ctx; (void)id; return -1; }
int mbedtls_sha3_update(void *ctx, const unsigned char *in, size_t ilen) { (void)ctx; (void)in; (void)ilen; return -1; }
int mbedtls_sha3_finish(void *ctx, unsigned char *out, size_t olen) { (void)ctx; (void)out; (void)olen; return -1; }
int mbedtls_sha3_clone(void *dst, const void *src) { (void)dst; (void)src; return -1; }
int mbedtls_sha3(int id, const unsigned char *in, size_t ilen, unsigned char *out, size_t olen) { (void)id; (void)in; (void)ilen; (void)out; (void)olen; return -1; }

/* ---- CMAC stubs ---- */
int mbedtls_cipher_cmac_starts(void *ctx, const unsigned char *key, size_t keybits) { (void)ctx; (void)key; (void)keybits; return -1; }
int mbedtls_cipher_cmac_update(void *ctx, const unsigned char *in, size_t ilen) { (void)ctx; (void)in; (void)ilen; return -1; }
int mbedtls_cipher_cmac_finish(void *ctx, unsigned char *out) { (void)ctx; (void)out; return -1; }

/* ---- HMAC-DRBG stubs ---- */
void mbedtls_hmac_drbg_init(void *ctx) { (void)ctx; }
void mbedtls_hmac_drbg_free(void *ctx) { (void)ctx; }
int mbedtls_hmac_drbg_seed_buf(void *ctx, const void *md, const unsigned char *data, size_t len) { (void)ctx; (void)md; (void)data; (void)len; return -1; }
int mbedtls_hmac_drbg_random(void *ctx, unsigned char *out, size_t len) { (void)ctx; (void)out; (void)len; return -1; }

/* ---- ECJPAKE stubs ---- */
void mbedtls_ecjpake_init(void *ctx) { (void)ctx; }
void mbedtls_ecjpake_free(void *ctx) { (void)ctx; }
int mbedtls_ecjpake_setup(void *ctx, int role, int hash, int curve, const unsigned char *secret, size_t len) { (void)ctx; (void)role; (void)hash; (void)curve; (void)secret; (void)len; return -1; }
int mbedtls_ecjpake_write_round_one(void *ctx, unsigned char *buf, size_t len, size_t *olen, int (*f_rng)(void *, unsigned char *, size_t), void *p_rng) { (void)ctx; (void)buf; (void)len; (void)olen; (void)f_rng; (void)p_rng; return -1; }
int mbedtls_ecjpake_write_round_two(void *ctx, unsigned char *buf, size_t len, size_t *olen, int (*f_rng)(void *, unsigned char *, size_t), void *p_rng) { (void)ctx; (void)buf; (void)len; (void)olen; (void)f_rng; (void)p_rng; return -1; }
int mbedtls_ecjpake_read_round_one(void *ctx, const unsigned char *buf, size_t len) { (void)ctx; (void)buf; (void)len; return -1; }
int mbedtls_ecjpake_read_round_two(void *ctx, const unsigned char *buf, size_t len) { (void)ctx; (void)buf; (void)len; return -1; }
int mbedtls_ecjpake_write_shared_key(void *ctx, unsigned char *buf, size_t len, size_t *olen, int (*f_rng)(void *, unsigned char *, size_t), void *p_rng) { (void)ctx; (void)buf; (void)len; (void)olen; (void)f_rng; (void)p_rng; return -1; }

/* ---- FFDH stubs ---- */
int mbedtls_psa_ffdh_export_public_key(const void *attr, const uint8_t *key, size_t key_len, uint8_t *data, size_t data_size, size_t *data_len) { (void)attr; (void)key; (void)key_len; (void)data; (void)data_size; (void)data_len; return -1; }
int mbedtls_psa_ffdh_generate_key(const void *attr, uint8_t *key, size_t key_size, size_t *key_len) { (void)attr; (void)key; (void)key_size; (void)key_len; return -1; }
int mbedtls_psa_ffdh_import_key(const void *attr, const uint8_t *data, size_t data_len, uint8_t *key, size_t key_size, size_t *key_len, size_t *bits) { (void)attr; (void)data; (void)data_len; (void)key; (void)key_size; (void)key_len; (void)bits; return -1; }
int mbedtls_psa_ffdh_key_agreement(const void *attr, const uint8_t *priv, size_t priv_len, const uint8_t *peer, size_t peer_len, uint8_t *shared, size_t shared_size, size_t *shared_len) { (void)attr; (void)priv; (void)priv_len; (void)peer; (void)peer_len; (void)shared; (void)shared_size; (void)shared_len; return -1; }

/* ---- PAKE stubs ---- */
int mbedtls_psa_pake_setup(void *ctx, const void *inputs) { (void)ctx; (void)inputs; return -1; }
int mbedtls_psa_pake_output(void *ctx, int step, uint8_t *out, size_t out_size, size_t *out_len) { (void)ctx; (void)step; (void)out; (void)out_size; (void)out_len; return -1; }
int mbedtls_psa_pake_input(void *ctx, int step, const uint8_t *in, size_t in_len) { (void)ctx; (void)step; (void)in; (void)in_len; return -1; }
int mbedtls_psa_pake_get_implicit_key(void *ctx, void *output) { (void)ctx; (void)output; return -1; }
int mbedtls_psa_pake_abort(void *ctx) { (void)ctx; return -1; }
