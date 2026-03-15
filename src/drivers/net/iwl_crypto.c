// TaterTOS64v3 — Cryptographic primitives for WPA2
// AES-128, SHA-1, HMAC-SHA1, PRF, PBKDF2, AES-CCMP, AES Key Unwrap

#include <stdint.h>
#include "iwl_crypto.h"

void kprint(const char *fmt, ...);

/* ---- helpers ---- */

static void cry_zero(void *p, uint32_t len) {
    uint8_t *d = (uint8_t *)p;
    for (uint32_t i = 0; i < len; i++) d[i] = 0;
}

static void cry_copy(void *dst, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}

static void cry_xor(uint8_t *dst, const uint8_t *a, const uint8_t *b, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) dst[i] = a[i] ^ b[i];
}

int crypto_memcmp(const uint8_t *a, const uint8_t *b, uint32_t len) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
    return diff;
}

/* ================================================================
 * AES-128 Block Cipher
 * ================================================================ */

static const uint8_t aes_sbox[256] = {
    0x63,0x7C,0x77,0x7B,0xF2,0x6B,0x6F,0xC5,0x30,0x01,0x67,0x2B,0xFE,0xD7,0xAB,0x76,
    0xCA,0x82,0xC9,0x7D,0xFA,0x59,0x47,0xF0,0xAD,0xD4,0xA2,0xAF,0x9C,0xA4,0x72,0xC0,
    0xB7,0xFD,0x93,0x26,0x36,0x3F,0xF7,0xCC,0x34,0xA5,0xE5,0xF1,0x71,0xD8,0x31,0x15,
    0x04,0xC7,0x23,0xC3,0x18,0x96,0x05,0x9A,0x07,0x12,0x80,0xE2,0xEB,0x27,0xB2,0x75,
    0x09,0x83,0x2C,0x1A,0x1B,0x6E,0x5A,0xA0,0x52,0x3B,0xD6,0xB3,0x29,0xE3,0x2F,0x84,
    0x53,0xD1,0x00,0xED,0x20,0xFC,0xB1,0x5B,0x6A,0xCB,0xBE,0x39,0x4A,0x4C,0x58,0xCF,
    0xD0,0xEF,0xAA,0xFB,0x43,0x4D,0x33,0x85,0x45,0xF9,0x02,0x7F,0x50,0x3C,0x9F,0xA8,
    0x51,0xA3,0x40,0x8F,0x92,0x9D,0x38,0xF5,0xBC,0xB6,0xDA,0x21,0x10,0xFF,0xF3,0xD2,
    0xCD,0x0C,0x13,0xEC,0x5F,0x97,0x44,0x17,0xC4,0xA7,0x7E,0x3D,0x64,0x5D,0x19,0x73,
    0x60,0x81,0x4F,0xDC,0x22,0x2A,0x90,0x88,0x46,0xEE,0xB8,0x14,0xDE,0x5E,0x0B,0xDB,
    0xE0,0x32,0x3A,0x0A,0x49,0x06,0x24,0x5C,0xC2,0xD3,0xAC,0x62,0x91,0x95,0xE4,0x79,
    0xE7,0xC8,0x37,0x6D,0x8D,0xD5,0x4E,0xA9,0x6C,0x56,0xF4,0xEA,0x65,0x7A,0xAE,0x08,
    0xBA,0x78,0x25,0x2E,0x1C,0xA6,0xB4,0xC6,0xE8,0xDD,0x74,0x1F,0x4B,0xBD,0x8B,0x8A,
    0x70,0x3E,0xB5,0x66,0x48,0x03,0xF6,0x0E,0x61,0x35,0x57,0xB9,0x86,0xC1,0x1D,0x9E,
    0xE1,0xF8,0x98,0x11,0x69,0xD9,0x8E,0x94,0x9B,0x1E,0x87,0xE9,0xCE,0x55,0x28,0xDF,
    0x8C,0xA1,0x89,0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16
};

static const uint8_t aes_inv_sbox[256] = {
    0x52,0x09,0x6A,0xD5,0x30,0x36,0xA5,0x38,0xBF,0x40,0xA3,0x9E,0x81,0xF3,0xD7,0xFB,
    0x7C,0xE3,0x39,0x82,0x9B,0x2F,0xFF,0x87,0x34,0x8E,0x43,0x44,0xC4,0xDE,0xE9,0xCB,
    0x54,0x7B,0x94,0x32,0xA6,0xC2,0x23,0x3D,0xEE,0x4C,0x95,0x0B,0x42,0xFA,0xC3,0x4E,
    0x08,0x2E,0xA1,0x66,0x28,0xD9,0x24,0xB2,0x76,0x5B,0xA2,0x49,0x6D,0x8B,0xD1,0x25,
    0x72,0xF8,0xF6,0x64,0x86,0x68,0x98,0x16,0xD4,0xA4,0x5C,0xCC,0x5D,0x65,0xB6,0x92,
    0x6C,0x70,0x48,0x50,0xFD,0xED,0xB9,0xDA,0x5E,0x15,0x46,0x57,0xA7,0x8D,0x9D,0x84,
    0x90,0xD8,0xAB,0x00,0x8C,0xBC,0xD3,0x0A,0xF7,0xE4,0x58,0x05,0xB8,0xB3,0x45,0x06,
    0xD0,0x2C,0x1E,0x8F,0xCA,0x3F,0x0F,0x02,0xC1,0xAF,0xBD,0x03,0x01,0x13,0x8A,0x6B,
    0x3A,0x91,0x11,0x41,0x4F,0x67,0xDC,0xEA,0x97,0xF2,0xCF,0xCE,0xF0,0xB4,0xE6,0x73,
    0x96,0xAC,0x74,0x22,0xE7,0xAD,0x35,0x85,0xE2,0xF9,0x37,0xE8,0x1C,0x75,0xDF,0x6E,
    0x47,0xF1,0x1A,0x71,0x1D,0x29,0xC5,0x89,0x6F,0xB7,0x62,0x0E,0xAA,0x18,0xBE,0x1B,
    0xFC,0x56,0x3E,0x4B,0xC6,0xD2,0x79,0x20,0x9A,0xDB,0xC0,0xFE,0x78,0xCD,0x5A,0xF4,
    0x1F,0xDD,0xA8,0x33,0x88,0x07,0xC7,0x31,0xB1,0x12,0x10,0x59,0x27,0x80,0xEC,0x5F,
    0x60,0x51,0x7F,0xA9,0x19,0xB5,0x4A,0x0D,0x2D,0xE5,0x7A,0x9F,0x93,0xC9,0x9C,0xEF,
    0xA0,0xE0,0x3B,0x4D,0xAE,0x2A,0xF5,0xB0,0xC8,0xEB,0xBB,0x3C,0x83,0x53,0x99,0x61,
    0x17,0x2B,0x04,0x7E,0xBA,0x77,0xD6,0x26,0xE1,0x69,0x14,0x63,0x55,0x21,0x0C,0x7D
};

static const uint8_t aes_rcon[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
};

/* GF(2^8) multiplication helpers for MixColumns / InvMixColumns */
static uint8_t gf_mul2(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1B));
}

static uint8_t gf_mul3(uint8_t x)  { return gf_mul2(x) ^ x; }
static uint8_t gf_mul9(uint8_t x)  { return gf_mul2(gf_mul2(gf_mul2(x))) ^ x; }
static uint8_t gf_mul11(uint8_t x) { return gf_mul2(gf_mul2(gf_mul2(x)) ^ x) ^ x; }
static uint8_t gf_mul13(uint8_t x) { return gf_mul2(gf_mul2(gf_mul2(x) ^ x)) ^ x; }
static uint8_t gf_mul14(uint8_t x) { return gf_mul2(gf_mul2(gf_mul2(x) ^ x) ^ x); }

/* AES key schedule expansion */
void aes128_init(struct aes128_ctx *ctx, const uint8_t key[16]) {
    uint8_t *rk = ctx->round_keys;
    cry_copy(rk, key, 16);

    for (int i = 0; i < 10; i++) {
        uint8_t *prev = rk + i * 16;
        uint8_t *next = rk + (i + 1) * 16;
        /* RotWord(prev[12..15]) + SubBytes + Rcon */
        next[0] = prev[0] ^ aes_sbox[prev[13]] ^ aes_rcon[i];
        next[1] = prev[1] ^ aes_sbox[prev[14]];
        next[2] = prev[2] ^ aes_sbox[prev[15]];
        next[3] = prev[3] ^ aes_sbox[prev[12]];
        for (int j = 4; j < 16; j++)
            next[j] = prev[j] ^ next[j - 4];
    }
}

/* AES-128 encrypt: state is column-major (s[row + 4*col]) */
void aes128_encrypt(const struct aes128_ctx *ctx, uint8_t s[16]) {
    const uint8_t *rk = ctx->round_keys;
    uint8_t t;

    /* Initial AddRoundKey */
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];

    for (int r = 1; r <= 10; r++) {
        rk += 16;

        /* SubBytes */
        for (int i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];

        /* ShiftRows */
        /* Row 1: left shift 1 */
        t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
        /* Row 2: left shift 2 */
        t = s[2]; s[2] = s[10]; s[10] = t;
        t = s[6]; s[6] = s[14]; s[14] = t;
        /* Row 3: left shift 3 = right shift 1 */
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;

        /* MixColumns (skip on last round) */
        if (r < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = gf_mul2(a0) ^ gf_mul3(a1) ^ a2 ^ a3;
                col[1] = a0 ^ gf_mul2(a1) ^ gf_mul3(a2) ^ a3;
                col[2] = a0 ^ a1 ^ gf_mul2(a2) ^ gf_mul3(a3);
                col[3] = gf_mul3(a0) ^ a1 ^ a2 ^ gf_mul2(a3);
            }
        }

        /* AddRoundKey */
        for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    }
}

/* AES-128 decrypt */
void aes128_decrypt(const struct aes128_ctx *ctx, uint8_t s[16]) {
    const uint8_t *rk = ctx->round_keys + 160; /* start from last round key */
    uint8_t t;

    /* Initial AddRoundKey (round 10) */
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];

    for (int r = 9; r >= 0; r--) {
        rk -= 16;

        /* InvShiftRows */
        /* Row 1: right shift 1 */
        t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
        /* Row 2: right shift 2 */
        t = s[2]; s[2] = s[10]; s[10] = t;
        t = s[6]; s[6] = s[14]; s[14] = t;
        /* Row 3: right shift 3 = left shift 1 */
        t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;

        /* InvSubBytes */
        for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]];

        /* AddRoundKey */
        for (int i = 0; i < 16; i++) s[i] ^= rk[i];

        /* InvMixColumns (skip on round 0) */
        if (r > 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = gf_mul14(a0) ^ gf_mul11(a1) ^ gf_mul13(a2) ^ gf_mul9(a3);
                col[1] = gf_mul9(a0) ^ gf_mul14(a1) ^ gf_mul11(a2) ^ gf_mul13(a3);
                col[2] = gf_mul13(a0) ^ gf_mul9(a1) ^ gf_mul14(a2) ^ gf_mul11(a3);
                col[3] = gf_mul11(a0) ^ gf_mul13(a1) ^ gf_mul9(a2) ^ gf_mul14(a3);
            }
        }
    }
}

/* ================================================================
 * SHA-1
 * ================================================================ */

static uint32_t sha1_rotl(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static uint32_t sha1_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void sha1_be32_put(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void sha1_compress(struct sha1_ctx *ctx, const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = sha1_be32(block + i * 4);
    for (int i = 16; i < 80; i++)
        w[i] = sha1_rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2],
             d = ctx->h[3], e = ctx->h[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = sha1_rotl(b, 30); b = a; a = temp;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c;
    ctx->h[3] += d; ctx->h[4] += e;
}

void sha1_init(struct sha1_ctx *ctx) {
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

void sha1_update(struct sha1_ctx *ctx, const uint8_t *data, uint32_t len) {
    ctx->total_len += len;
    uint32_t off = 0;

    /* Fill partial buffer first */
    if (ctx->buf_len > 0) {
        uint32_t need = 64 - ctx->buf_len;
        if (len < need) {
            cry_copy(ctx->buf + ctx->buf_len, data, len);
            ctx->buf_len += (uint8_t)len;
            return;
        }
        cry_copy(ctx->buf + ctx->buf_len, data, need);
        sha1_compress(ctx, ctx->buf);
        ctx->buf_len = 0;
        off = need;
    }

    /* Process full 64-byte blocks */
    while (off + 64 <= len) {
        sha1_compress(ctx, data + off);
        off += 64;
    }

    /* Buffer remaining */
    uint32_t rem = len - off;
    if (rem > 0) {
        cry_copy(ctx->buf, data + off, rem);
        ctx->buf_len = (uint8_t)rem;
    }
}

void sha1_final(struct sha1_ctx *ctx, uint8_t digest[20]) {
    uint64_t total_bits = ctx->total_len * 8;

    /* Pad: 0x80, then zeros, then 64-bit big-endian bit count */
    uint8_t pad = 0x80;
    sha1_update(ctx, &pad, 1);

    uint8_t zero = 0;
    while (ctx->buf_len != 56)
        sha1_update(ctx, &zero, 1);

    uint8_t len_be[8];
    len_be[0] = (uint8_t)(total_bits >> 56);
    len_be[1] = (uint8_t)(total_bits >> 48);
    len_be[2] = (uint8_t)(total_bits >> 40);
    len_be[3] = (uint8_t)(total_bits >> 32);
    len_be[4] = (uint8_t)(total_bits >> 24);
    len_be[5] = (uint8_t)(total_bits >> 16);
    len_be[6] = (uint8_t)(total_bits >> 8);
    len_be[7] = (uint8_t)(total_bits);
    sha1_update(ctx, len_be, 8);

    /* Output hash */
    for (int i = 0; i < 5; i++)
        sha1_be32_put(digest + i * 4, ctx->h[i]);
}

void sha1(const uint8_t *data, uint32_t len, uint8_t digest[20]) {
    struct sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
}

/* ================================================================
 * HMAC-SHA1
 * ================================================================ */

void hmac_sha1(const uint8_t *key, uint32_t key_len,
               const uint8_t *data, uint32_t data_len,
               uint8_t output[20]) {
    uint8_t k_pad[64];
    uint8_t k_hash[20];

    /* If key > 64 bytes, hash it first */
    if (key_len > 64) {
        sha1(key, key_len, k_hash);
        key = k_hash;
        key_len = 20;
    }

    /* Zero-pad key to 64 bytes */
    cry_zero(k_pad, 64);
    cry_copy(k_pad, key, key_len);

    /* Inner: SHA1(K XOR ipad || data) */
    uint8_t ipad[64];
    for (int i = 0; i < 64; i++) ipad[i] = k_pad[i] ^ 0x36;

    struct sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, ipad, 64);
    sha1_update(&ctx, data, data_len);
    uint8_t inner[20];
    sha1_final(&ctx, inner);

    /* Outer: SHA1(K XOR opad || inner) */
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) opad[i] = k_pad[i] ^ 0x5C;

    sha1_init(&ctx);
    sha1_update(&ctx, opad, 64);
    sha1_update(&ctx, inner, 20);
    sha1_final(&ctx, output);
}

/* ================================================================
 * PRF (IEEE 802.11i Pseudo-Random Function)
 *
 * PRF-N(K, A, B) = HMAC-SHA1(K, A || 0x00 || B || counter)
 * where counter = 0, 1, 2, ... until we have N bits
 * ================================================================ */

void prf(const uint8_t *key, uint32_t key_len,
         const char *label,
         const uint8_t *data, uint32_t data_len,
         uint8_t *output, uint32_t out_len) {
    /* Calculate label length (not including NUL) */
    uint32_t label_len = 0;
    while (label[label_len]) label_len++;

    /* Build the input: label || 0x00 || data || counter
       Max size: 22 (label) + 1 + 76 (data) + 1 = 100 bytes */
    uint8_t input[128];
    uint32_t input_base_len = label_len + 1 + data_len;
    if (input_base_len + 1 > sizeof(input)) return;

    cry_copy(input, label, label_len);
    input[label_len] = 0x00;
    cry_copy(input + label_len + 1, data, data_len);

    uint32_t pos = 0;
    uint8_t counter = 0;

    while (pos < out_len) {
        input[input_base_len] = counter;
        uint8_t hmac_out[20];
        hmac_sha1(key, key_len, input, input_base_len + 1, hmac_out);

        uint32_t copy = out_len - pos;
        if (copy > 20) copy = 20;
        cry_copy(output + pos, hmac_out, copy);

        pos += copy;
        counter++;
    }
}

/* ================================================================
 * PBKDF2-SHA1
 * ================================================================ */

void pbkdf2_sha1(const uint8_t *password, uint32_t password_len,
                 const uint8_t *salt, uint32_t salt_len,
                 uint32_t iterations,
                 uint8_t *dk, uint32_t dk_len) {
    /* PBKDF2 with HMAC-SHA1:
       DK = T1 || T2 || ...
       Ti = F(Password, Salt, c, i)
       F(P, S, c, i) = U1 XOR U2 XOR ... XOR Uc
       U1 = HMAC(P, S || INT(i))   [INT = big-endian 4 bytes]
       Uj = HMAC(P, U_{j-1})
    */
    uint32_t pos = 0;
    uint32_t block = 1;

    /* Temp buffer for salt || INT(i) */
    uint8_t salt_block[68]; /* max SSID 32 + 4 = 36 */
    if (salt_len > 64) salt_len = 64; /* safety clamp */
    cry_copy(salt_block, salt, salt_len);

    while (pos < dk_len) {
        /* Append block counter (big-endian) */
        salt_block[salt_len + 0] = (uint8_t)(block >> 24);
        salt_block[salt_len + 1] = (uint8_t)(block >> 16);
        salt_block[salt_len + 2] = (uint8_t)(block >> 8);
        salt_block[salt_len + 3] = (uint8_t)(block);

        /* U1 = HMAC(password, salt || INT(block)) */
        uint8_t u[20], t[20];
        hmac_sha1(password, password_len,
                  salt_block, salt_len + 4, u);
        cry_copy(t, u, 20);

        /* U2..Uc */
        for (uint32_t j = 1; j < iterations; j++) {
            uint8_t u_next[20];
            hmac_sha1(password, password_len, u, 20, u_next);
            cry_copy(u, u_next, 20);
            cry_xor(t, t, u, 20);
        }

        /* Copy T_block to output */
        uint32_t copy = dk_len - pos;
        if (copy > 20) copy = 20;
        cry_copy(dk + pos, t, copy);

        pos += copy;
        block++;
    }
}

/* ================================================================
 * AES-CCMP (802.11i)
 *
 * CCMP uses AES-128 in CCM mode:
 * - CBC-MAC for authentication (8-byte MIC)
 * - CTR for encryption
 *
 * Nonce (13 bytes): flags(1) + A2(6) + PN(6)
 * where flags = priority & 0x0F
 *
 * The 802.11 header is authenticated (AAD) but not encrypted.
 * ================================================================ */

/* Build the CCM nonce for 802.11i CCMP.
 * nonce[0] = priority
 * nonce[1..6] = A2 (transmitter address)
 * nonce[7..12] = PN (packet number, big-endian from the LE PN bytes)
 */
static void ccmp_build_nonce(uint8_t nonce[13],
                              uint8_t priority,
                              const uint8_t a2[6],
                              const uint8_t pn[6]) {
    nonce[0] = priority;
    cry_copy(nonce + 1, a2, 6);
    /* PN is stored little-endian in the CCMP header (pn[0]=PN0, pn[5]=PN5).
       In the nonce it goes big-endian: nonce[7]=PN5, nonce[12]=PN0. */
    nonce[7]  = pn[5];
    nonce[8]  = pn[4];
    nonce[9]  = pn[3];
    nonce[10] = pn[2];
    nonce[11] = pn[1];
    nonce[12] = pn[0];
}

/* Build B0 block for CBC-MAC (first block in CCM authentication).
 *
 * B0 = flags(1) || nonce(13) || Adata_length(2)
 * flags: bit 6 = Adata present (1), bits 5-3 = (t-2)/2 where t=8 (MIC len),
 *        bits 2-0 = q-1 where q=2 (length field size)
 * So flags = 0x40 | ((8-2)/2 << 3) | (2-1) = 0x40 | 0x18 | 0x01 = 0x59
 */
static void ccmp_build_b0(uint8_t b0[16], const uint8_t nonce[13],
                           uint16_t payload_len) {
    b0[0] = 0x59; /* flags: Adata=1, t=8, q=2 */
    cry_copy(b0 + 1, nonce, 13);
    b0[14] = (uint8_t)(payload_len >> 8);
    b0[15] = (uint8_t)(payload_len);
}

/* Build AAD (Additional Authenticated Data) from 802.11 header.
 *
 * For a standard 24-byte 802.11 header:
 * AAD = AAD_len(2) || FC_masked(2) || A1(6) || A2(6) || A3(6) || SC_masked(2)
 *
 * FC mask: clear subtype bits 4-7, clear retry/PM/MD/order bits
 * SC mask: clear sequence number (keep fragment number)
 */
static uint16_t ccmp_build_aad(uint8_t *aad, uint32_t aad_max,
                                const uint8_t *hdr, uint16_t hdr_len) {
    if (aad_max < 22 + 2 || hdr_len < 24) return 0;

    /* AAD length (excluding the 2-byte length field itself) */
    uint16_t aad_body_len = 22; /* for 3-address header (24 bytes → 22 AAD bytes) */

    /* Check if 4-address header (has A4): FC ToDS+FromDS both set */
    uint16_t fc = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
    int has_a4 = ((fc & 0x0300) == 0x0300) && hdr_len >= 30;
    if (has_a4) aad_body_len = 28;

    /* Check for QoS header */
    int is_qos = (fc & 0x0080) != 0;
    if (is_qos) aad_body_len += 2;

    /* Length prefix */
    aad[0] = (uint8_t)(aad_body_len >> 8);
    aad[1] = (uint8_t)(aad_body_len);

    /* FC masked: clear bits we must not authenticate
       Mask: clear retry(11), PM(12), moredata(13), protected(14), order(15)
       and subtype bits only for certain types. For CCMP, mask Protected bit. */
    uint16_t fc_masked = fc & 0x008F; /* keep type/subtype lower, toDS, fromDS */
    fc_masked |= fc & 0x0070; /* keep subtype upper bits */
    /* Actually the correct mask per 802.11i:
       Clear: retry, PM, moredata, protected, order
       Keep: protocol, type, subtype, toDS, fromDS */
    fc_masked = fc & ~(uint16_t)(0xF800 | 0x0400 | 0x0100);
    /* Simpler: keep bits [0..7] except retry(bit 3 of byte 1), and
       clear protected(bit 6 of byte 1), PM(bit 4 of byte 1),
       moredata(bit 5 of byte 1), order(bit 7 of byte 1) */
    /* Per IEEE 802.11-2012 table 11-17:
       FC: Subtype b4..b7 masked for non-QoS, otherwise kept.
       Retry, PwrMgmt, MoreData, Protected, Order bits are zeroed. */
    fc_masked = fc & 0x00FF; /* keep first byte entirely */
    fc_masked |= (uint16_t)(hdr[1] & 0x07) << 8; /* keep just DS bits from byte 1 */
    /* That's still not quite right. Let me do it properly:
       Byte 0: FC[7:0] — keep as-is
       Byte 1: FC[15:8] — zero out retry(3), PM(4), MD(5), Protected(6), Order(7)
                           keep: moreFrag(2), toDS(0), fromDS(1) wait these are in byte 0.

       Actually in 802.11, FC is: */
    /* FC byte 0: Protocol(2) | Type(2) | Subtype(4)
       FC byte 1: ToDS(1) | FromDS(1) | MoreFrag(1) | Retry(1) | PwrMgmt(1) |
                  MoreData(1) | Protected(1) | Order(1) */
    /* For AAD, zero: Retry, PwrMgmt, MoreData, Protected, Order
       Keep: Protocol, Type, Subtype, ToDS, FromDS, MoreFrag */
    fc_masked = (uint16_t)(hdr[0]); /* byte 0 fully kept */
    fc_masked |= (uint16_t)(hdr[1] & 0x07) << 8; /* byte 1: keep ToDS+FromDS+MoreFrag */

    aad[2] = (uint8_t)(fc_masked);
    aad[3] = (uint8_t)(fc_masked >> 8);

    /* A1, A2, A3 (18 bytes total) */
    cry_copy(aad + 4, hdr + 4, 18);

    /* Sequence Control masked: zero sequence number, keep fragment */
    aad[22] = hdr[22] & 0x0F; /* fragment number only */
    aad[23] = 0;

    uint16_t total = 2 + aad_body_len;

    /* A4 if present */
    if (has_a4) {
        cry_copy(aad + 24, hdr + 24, 6);
        total = 2 + aad_body_len;
    }

    return total;
}

/* Build CTR counter block.
 * A_i = flags(1) || nonce(13) || counter(2)
 * flags: bits 2-0 = q-1 = 1 (for q=2)
 * counter: 0 for S_0 (used to encrypt MIC), 1+ for payload blocks
 */
static void ccmp_build_ctr(uint8_t ctr[16], const uint8_t nonce[13], uint16_t counter) {
    ctr[0] = 0x01; /* flags: q-1 = 1 */
    cry_copy(ctr + 1, nonce, 13);
    ctr[14] = (uint8_t)(counter >> 8);
    ctr[15] = (uint8_t)(counter);
}

int ccmp_encrypt(const uint8_t tk[16],
                 const uint8_t pn[6], const uint8_t a2[6], uint8_t priority,
                 const uint8_t *hdr, uint16_t hdr_len,
                 const uint8_t *plaintext, uint16_t pt_len,
                 uint8_t *out) {
    struct aes128_ctx aes;
    aes128_init(&aes, tk);

    uint8_t nonce[13];
    ccmp_build_nonce(nonce, priority, a2, pn);

    /* === CBC-MAC (compute MIC over AAD + plaintext) === */

    /* B0 */
    uint8_t cbc[16];
    ccmp_build_b0(cbc, nonce, pt_len);
    aes128_encrypt(&aes, cbc);

    /* AAD blocks */
    uint8_t aad[48];
    uint16_t aad_len = ccmp_build_aad(aad, sizeof(aad), hdr, hdr_len);
    if (aad_len == 0) return -1;

    /* Pad AAD to 16-byte blocks, XOR into CBC state, encrypt */
    uint16_t aad_pos = 0;
    while (aad_pos < aad_len) {
        uint8_t block[16];
        cry_zero(block, 16);
        uint16_t chunk = aad_len - aad_pos;
        if (chunk > 16) chunk = 16;
        cry_copy(block, aad + aad_pos, chunk);
        cry_xor(cbc, cbc, block, 16);
        aes128_encrypt(&aes, cbc);
        aad_pos += 16;
    }

    /* Plaintext blocks */
    uint16_t pt_pos = 0;
    while (pt_pos < pt_len) {
        uint8_t block[16];
        cry_zero(block, 16);
        uint16_t chunk = pt_len - pt_pos;
        if (chunk > 16) chunk = 16;
        cry_copy(block, plaintext + pt_pos, chunk);
        cry_xor(cbc, cbc, block, 16);
        aes128_encrypt(&aes, cbc);
        pt_pos += 16;
    }

    /* MIC = first 8 bytes of CBC-MAC result */
    uint8_t mic[8];
    cry_copy(mic, cbc, 8);

    /* === CTR mode encryption === */

    /* S_0: encrypt counter 0 (used to encrypt MIC) */
    uint8_t s0[16];
    ccmp_build_ctr(s0, nonce, 0);
    aes128_encrypt(&aes, s0);

    /* Encrypt plaintext with counter 1, 2, ... */
    pt_pos = 0;
    uint16_t ctr_val = 1;
    while (pt_pos < pt_len) {
        uint8_t ctr_block[16];
        ccmp_build_ctr(ctr_block, nonce, ctr_val);
        aes128_encrypt(&aes, ctr_block);

        uint16_t chunk = pt_len - pt_pos;
        if (chunk > 16) chunk = 16;
        cry_xor(out + pt_pos, plaintext + pt_pos, ctr_block, chunk);

        pt_pos += chunk;
        ctr_val++;
    }

    /* Encrypt MIC with S_0 */
    cry_xor(out + pt_len, mic, s0, 8);

    return 0;
}

int ccmp_decrypt(const uint8_t tk[16],
                 const uint8_t pn[6], const uint8_t a2[6], uint8_t priority,
                 const uint8_t *hdr, uint16_t hdr_len,
                 const uint8_t *ciphertext, uint16_t ct_len,
                 uint8_t *out) {
    if (ct_len < 8) return -1; /* need at least 8 bytes for MIC */
    uint16_t pt_len = ct_len - 8;

    struct aes128_ctx aes;
    aes128_init(&aes, tk);

    uint8_t nonce[13];
    ccmp_build_nonce(nonce, priority, a2, pn);

    /* === CTR mode decryption === */

    /* S_0 for MIC decryption */
    uint8_t s0[16];
    ccmp_build_ctr(s0, nonce, 0);
    aes128_encrypt(&aes, s0);

    /* Decrypt plaintext */
    uint16_t pos = 0;
    uint16_t ctr_val = 1;
    while (pos < pt_len) {
        uint8_t ctr_block[16];
        ccmp_build_ctr(ctr_block, nonce, ctr_val);
        aes128_encrypt(&aes, ctr_block);

        uint16_t chunk = pt_len - pos;
        if (chunk > 16) chunk = 16;
        cry_xor(out + pos, ciphertext + pos, ctr_block, chunk);

        pos += chunk;
        ctr_val++;
    }

    /* Decrypt received MIC */
    uint8_t received_mic[8];
    cry_xor(received_mic, ciphertext + pt_len, s0, 8);

    /* === CBC-MAC verification === */

    uint8_t cbc[16];
    ccmp_build_b0(cbc, nonce, pt_len);
    aes128_encrypt(&aes, cbc);

    /* AAD */
    uint8_t aad[48];
    uint16_t aad_len = ccmp_build_aad(aad, sizeof(aad), hdr, hdr_len);
    if (aad_len == 0) return -1;

    uint16_t aad_pos = 0;
    while (aad_pos < aad_len) {
        uint8_t block[16];
        cry_zero(block, 16);
        uint16_t chunk = aad_len - aad_pos;
        if (chunk > 16) chunk = 16;
        cry_copy(block, aad + aad_pos, chunk);
        cry_xor(cbc, cbc, block, 16);
        aes128_encrypt(&aes, cbc);
        aad_pos += 16;
    }

    /* Plaintext blocks (now decrypted) */
    pos = 0;
    while (pos < pt_len) {
        uint8_t block[16];
        cry_zero(block, 16);
        uint16_t chunk = pt_len - pos;
        if (chunk > 16) chunk = 16;
        cry_copy(block, out + pos, chunk);
        cry_xor(cbc, cbc, block, 16);
        aes128_encrypt(&aes, cbc);
        pos += 16;
    }

    /* Verify MIC */
    if (crypto_memcmp(cbc, received_mic, 8) != 0) {
        kprint("IWL-CCMP: MIC verification FAILED\n");
        return -1;
    }

    return 0;
}

/* ================================================================
 * AES Key Unwrap (RFC 3394)
 *
 * Used to decrypt the GTK in EAPOL message 3.
 * KEK (16 bytes) unwraps wrapped_len bytes into wrapped_len-8 output bytes.
 * ================================================================ */

int aes_key_unwrap(const uint8_t kek[16],
                   const uint8_t *wrapped, uint32_t wrapped_len,
                   uint8_t *output) {
    if (wrapped_len < 16 || (wrapped_len % 8) != 0) return -1;

    uint32_t n = (wrapped_len / 8) - 1; /* number of 64-bit blocks */
    if (n == 0 || n > 32) return -1;

    struct aes128_ctx aes;
    aes128_init(&aes, kek);

    /* A = wrapped[0..7], R[1..n] = wrapped[8..] */
    uint8_t a[8];
    cry_copy(a, wrapped, 8);
    cry_copy(output, wrapped + 8, n * 8);

    /* Unwrap: 6 rounds, each round processes n blocks in reverse */
    for (int j = 5; j >= 0; j--) {
        for (int i = (int)n; i >= 1; i--) {
            /* Counter value = n*j + i */
            uint32_t ctr = (uint32_t)(n * (uint32_t)j + (uint32_t)i);

            /* A XOR counter */
            uint8_t b[16];
            cry_copy(b, a, 8);
            b[7] ^= (uint8_t)(ctr);
            b[6] ^= (uint8_t)(ctr >> 8);
            b[5] ^= (uint8_t)(ctr >> 16);
            b[4] ^= (uint8_t)(ctr >> 24);

            /* B = A XOR t || R[i] */
            cry_copy(b + 8, output + (i - 1) * 8, 8);

            /* AES decrypt */
            aes128_decrypt(&aes, b);

            /* A = B[0..7], R[i] = B[8..15] */
            cry_copy(a, b, 8);
            cry_copy(output + (i - 1) * 8, b + 8, 8);
        }
    }

    /* Verify IV (A must equal default IV: 0xA6A6A6A6A6A6A6A6) */
    static const uint8_t default_iv[8] = {0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6};
    if (crypto_memcmp(a, default_iv, 8) != 0) {
        kprint("IWL-AKW: key unwrap integrity check FAILED\n");
        return -1;
    }

    return 0;
}

/* ================================================================
 * PRNG (Pseudo-Random Number Generator)
 *
 * Seeded from RDTSC. Used for SNonce generation.
 * Not cryptographically strong, but sufficient for SNonce which
 * only needs to be unique per association attempt.
 * ================================================================ */

static uint64_t prng_state[2] = {0, 0};
static int prng_seeded = 0;

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* xorshift128+ */
static uint64_t prng_next(void) {
    uint64_t s0 = prng_state[0];
    uint64_t s1 = prng_state[1];
    uint64_t result = s0 + s1;

    s1 ^= s0;
    prng_state[0] = ((s0 << 55) | (s0 >> 9)) ^ s1 ^ (s1 << 14);
    prng_state[1] = (s1 << 36) | (s1 >> 28);

    return result;
}

void crypto_random(uint8_t *buf, uint32_t len) {
    if (!prng_seeded) {
        /* Seed from RDTSC with multiple reads for entropy mixing */
        prng_state[0] = rdtsc();
        for (volatile int i = 0; i < 100; i++) {} /* small delay */
        prng_state[1] = rdtsc();
        /* Mix with a few PRNG rounds to spread the seed */
        for (int i = 0; i < 20; i++) prng_next();
        prng_seeded = 1;
    }

    uint32_t pos = 0;
    while (pos < len) {
        uint64_t r = prng_next();
        uint32_t chunk = len - pos;
        if (chunk > 8) chunk = 8;
        for (uint32_t i = 0; i < chunk; i++) {
            buf[pos + i] = (uint8_t)(r >> (i * 8));
        }
        pos += chunk;
    }
}
