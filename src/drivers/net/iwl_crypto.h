#ifndef TATER_IWL_CRYPTO_H
#define TATER_IWL_CRYPTO_H

#include <stdint.h>

/* ---- AES-128 Block Cipher ---- */

/* AES-128 expanded key schedule (11 round keys * 16 bytes = 176 bytes) */
struct aes128_ctx {
    uint8_t round_keys[176];
};

/* Initialize AES-128 context with a 16-byte key */
void aes128_init(struct aes128_ctx *ctx, const uint8_t key[16]);

/* Encrypt a single 16-byte block in-place */
void aes128_encrypt(const struct aes128_ctx *ctx, uint8_t block[16]);

/* Decrypt a single 16-byte block in-place */
void aes128_decrypt(const struct aes128_ctx *ctx, uint8_t block[16]);

/* ---- SHA-1 Hash ---- */

struct sha1_ctx {
    uint32_t h[5];          /* state */
    uint64_t total_len;     /* total bytes processed */
    uint8_t  buf[64];       /* partial block buffer */
    uint8_t  buf_len;       /* bytes in buf */
};

void sha1_init(struct sha1_ctx *ctx);
void sha1_update(struct sha1_ctx *ctx, const uint8_t *data, uint32_t len);
void sha1_final(struct sha1_ctx *ctx, uint8_t digest[20]);

/* One-shot SHA-1 */
void sha1(const uint8_t *data, uint32_t len, uint8_t digest[20]);

/* ---- HMAC-SHA1 ---- */

/* HMAC-SHA1: output is 20 bytes */
void hmac_sha1(const uint8_t *key, uint32_t key_len,
               const uint8_t *data, uint32_t data_len,
               uint8_t output[20]);

/* ---- PRF (IEEE 802.11i Pseudo-Random Function) ---- */

/*
 * PRF-N using HMAC-SHA1.
 * Used for PTK derivation (N=384 for CCMP → 48 bytes output).
 *
 * key:     PMK (32 bytes for WPA2-PSK)
 * label:   "Pairwise key expansion" (NUL-terminated)
 * data:    min(AA,SA) || max(AA,SA) || min(ANonce,SNonce) || max(ANonce,SNonce)
 * output:  PTK bytes
 * out_len: desired output length in bytes (48 for CCMP)
 */
void prf(const uint8_t *key, uint32_t key_len,
         const char *label,
         const uint8_t *data, uint32_t data_len,
         uint8_t *output, uint32_t out_len);

/* ---- PBKDF2-SHA1 (WPA2-PSK: passphrase → PMK) ---- */

/*
 * PBKDF2 with HMAC-SHA1.
 * For WPA2-PSK: iterations=4096, dk_len=32.
 *
 * password: passphrase (8-63 ASCII chars)
 * salt:     SSID (up to 32 bytes)
 * iterations: 4096
 * dk:       output derived key (PMK)
 * dk_len:   output length (32 bytes for WPA2)
 */
void pbkdf2_sha1(const uint8_t *password, uint32_t password_len,
                 const uint8_t *salt, uint32_t salt_len,
                 uint32_t iterations,
                 uint8_t *dk, uint32_t dk_len);

/* ---- AES-CCMP (802.11i) ---- */

/*
 * CCMP encrypt: produces encrypted payload + 8-byte MIC appended.
 *
 * tk:           temporal key (16 bytes, from PTK bytes [32..47])
 * pn:           packet number, 6 bytes (little-endian, increments per frame)
 * a2:           address 2 from 802.11 header (transmitter, 6 bytes)
 * priority:     QoS TID (0 for non-QoS)
 * hdr:          802.11 header (AAD: authenticated but not encrypted)
 * hdr_len:      802.11 header length
 * plaintext:    payload to encrypt
 * pt_len:       plaintext length
 * out:          output buffer (must be at least pt_len + 8 bytes)
 *
 * Returns 0 on success.
 */
int ccmp_encrypt(const uint8_t tk[16],
                 const uint8_t pn[6], const uint8_t a2[6], uint8_t priority,
                 const uint8_t *hdr, uint16_t hdr_len,
                 const uint8_t *plaintext, uint16_t pt_len,
                 uint8_t *out);

/*
 * CCMP decrypt + verify MIC.
 *
 * tk:           temporal key (16 bytes)
 * pn:           packet number from CCMP header (6 bytes)
 * a2:           address 2 (transmitter, 6 bytes)
 * priority:     QoS TID (0 for non-QoS)
 * hdr:          802.11 header (AAD)
 * hdr_len:      802.11 header length
 * ciphertext:   encrypted payload + 8-byte MIC
 * ct_len:       ciphertext length (including 8-byte MIC)
 * out:          output plaintext buffer (ct_len - 8 bytes)
 *
 * Returns 0 on success, -1 on MIC failure.
 */
int ccmp_decrypt(const uint8_t tk[16],
                 const uint8_t pn[6], const uint8_t a2[6], uint8_t priority,
                 const uint8_t *hdr, uint16_t hdr_len,
                 const uint8_t *ciphertext, uint16_t ct_len,
                 uint8_t *out);

/* ---- AES Key Unwrap (RFC 3394) ---- */

/*
 * Unwrap a key encrypted with AES Key Wrap.
 * Used to decrypt GTK from EAPOL message 3.
 *
 * kek:          key encryption key (16 bytes, from PTK bytes [16..31])
 * wrapped:      wrapped key data (8*(n+1) bytes, includes 8-byte IV)
 * wrapped_len:  length of wrapped data
 * output:       unwrapped key (wrapped_len - 8 bytes)
 *
 * Returns 0 on success, -1 on integrity check failure.
 */
int aes_key_unwrap(const uint8_t kek[16],
                   const uint8_t *wrapped, uint32_t wrapped_len,
                   uint8_t *output);

/* ---- Utility ---- */

/* Constant-time comparison (returns 0 if equal) */
int crypto_memcmp(const uint8_t *a, const uint8_t *b, uint32_t len);

/* Get pseudo-random bytes (seeded from RDTSC + mixing) */
void crypto_random(uint8_t *buf, uint32_t len);

#endif
