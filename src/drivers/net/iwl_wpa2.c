// TaterTOS64v3 — WPA2-PSK (802.11i) EAPOL four-way handshake + key management

#include <stdint.h>
#include "iwl_wpa2.h"
#include "iwl_crypto.h"
#include "iwl_cmd.h"
#include "iwl_mac80211.h"

void kprint(const char *fmt, ...);

/* ---- helpers ---- */

static void w_zero(void *p, uint32_t len) {
    uint8_t *d = (uint8_t *)p;
    for (uint32_t i = 0; i < len; i++) d[i] = 0;
}

static void w_copy(void *dst, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}

static uint16_t w_be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static void w_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static uint64_t w_be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void w_put_be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

/* Compare two byte sequences (for min/max ordering in PRF data) */
static int w_memcmp(const uint8_t *a, const uint8_t *b, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

/* ================================================================
 * RSN IE Builder
 * ================================================================ */

uint16_t wpa2_build_rsn_ie(uint8_t *buf, uint16_t max_len) {
    if (max_len < 22) return 0;

    /* RSN IE for WPA2-PSK with CCMP:
     * Tag: 48 (RSN)
     * Length: 20
     * Version: 1
     * Group cipher suite: 00-0F-AC:4 (CCMP)
     * Pairwise cipher count: 1
     * Pairwise cipher suite: 00-0F-AC:4 (CCMP)
     * AKM count: 1
     * AKM suite: 00-0F-AC:2 (PSK)
     * RSN capabilities: 0x000C (MFPC + MFPR off, 4 PTKSA replay counters)
     */
    uint8_t ie[] = {
        0x30,       /* Element ID: RSN */
        20,         /* Length */
        0x01, 0x00, /* Version 1 */
        0x00, 0x0F, 0xAC, 0x04, /* Group cipher: CCMP */
        0x01, 0x00, /* Pairwise cipher count: 1 */
        0x00, 0x0F, 0xAC, 0x04, /* Pairwise cipher: CCMP */
        0x01, 0x00, /* AKM count: 1 */
        0x00, 0x0F, 0xAC, 0x02, /* AKM: PSK */
        0x0C, 0x00  /* RSN capabilities: 4 PTKSA replay counters */
    };

    w_copy(buf, ie, 22);
    return 22;
}

/* ================================================================
 * WPA2 Context Management
 * ================================================================ */

void wpa2_init(struct wpa2_ctx *ctx,
               const uint8_t our_mac[6],
               const uint8_t ap_mac[6]) {
    w_zero(ctx, sizeof(*ctx));
    w_copy(ctx->our_mac, our_mac, 6);
    w_copy(ctx->ap_mac, ap_mac, 6);
    ctx->hs_state = 1; /* waiting for message 1 */
}

void wpa2_set_passphrase(struct wpa2_ctx *ctx,
                          const char *passphrase,
                          const uint8_t *ssid, uint8_t ssid_len) {
    /* Calculate passphrase length */
    uint32_t pp_len = 0;
    while (passphrase[pp_len]) pp_len++;

    kprint("IWL-WPA2: deriving PMK from passphrase (%u chars, SSID len=%u)\n",
           pp_len, ssid_len);

    /* PBKDF2-SHA1: 4096 iterations, 32 bytes output */
    pbkdf2_sha1((const uint8_t *)passphrase, pp_len,
                ssid, ssid_len, 4096,
                ctx->pmk, WPA2_PMK_LEN);

    ctx->pmk_valid = 1;
    kprint("IWL-WPA2: PMK derived OK\n");
}

void wpa2_set_pmk(struct wpa2_ctx *ctx, const uint8_t pmk[32]) {
    w_copy(ctx->pmk, pmk, WPA2_PMK_LEN);
    ctx->pmk_valid = 1;
}

/* ================================================================
 * PTK Derivation
 *
 * PTK = PRF-384(PMK, "Pairwise key expansion",
 *               min(AA,SA) || max(AA,SA) || min(ANonce,SNonce) || max(ANonce,SNonce))
 *
 * AA = Authenticator Address (AP BSSID)
 * SA = Supplicant Address (our MAC)
 * ================================================================ */

static void wpa2_derive_ptk(struct wpa2_ctx *ctx) {
    /* Build the data: min(AA,SA) || max(AA,SA) || min(ANonce,SNonce) || max(ANonce,SNonce) */
    uint8_t data[76]; /* 6 + 6 + 32 + 32 = 76 */

    const uint8_t *aa = ctx->ap_mac;
    const uint8_t *sa = ctx->our_mac;

    /* min/max of MAC addresses */
    if (w_memcmp(aa, sa, 6) < 0) {
        w_copy(data, aa, 6);
        w_copy(data + 6, sa, 6);
    } else {
        w_copy(data, sa, 6);
        w_copy(data + 6, aa, 6);
    }

    /* min/max of nonces */
    if (w_memcmp(ctx->anonce, ctx->snonce, WPA2_NONCE_LEN) < 0) {
        w_copy(data + 12, ctx->anonce, WPA2_NONCE_LEN);
        w_copy(data + 12 + WPA2_NONCE_LEN, ctx->snonce, WPA2_NONCE_LEN);
    } else {
        w_copy(data + 12, ctx->snonce, WPA2_NONCE_LEN);
        w_copy(data + 12 + WPA2_NONCE_LEN, ctx->anonce, WPA2_NONCE_LEN);
    }

    /* PRF-384: 48 bytes output */
    uint8_t ptk[WPA2_PTK_LEN];
    prf(ctx->pmk, WPA2_PMK_LEN,
        "Pairwise key expansion",
        data, 76,
        ptk, WPA2_PTK_LEN);

    /* Split PTK into KCK, KEK, TK */
    w_copy(ctx->kck, ptk, WPA2_KCK_LEN);
    w_copy(ctx->kek, ptk + WPA2_KCK_LEN, WPA2_KEK_LEN);
    w_copy(ctx->tk, ptk + WPA2_KCK_LEN + WPA2_KEK_LEN, WPA2_TK_LEN);

    ctx->ptk_valid = 1;

    kprint("IWL-WPA2: PTK derived (KCK+KEK+TK = %u bytes)\n", WPA2_PTK_LEN);
}

/* ================================================================
 * EAPOL-Key MIC computation
 *
 * For WPA2 with HMAC-SHA1:
 * MIC = HMAC-SHA1(KCK, EAPOL frame with MIC field zeroed)[0..15]
 * ================================================================ */

static void wpa2_compute_mic(const uint8_t kck[16],
                              const uint8_t *eapol_frame, uint16_t frame_len,
                              uint8_t mic_out[16]) {
    /* The MIC is computed over the entire EAPOL frame with MIC set to zero.
       The MIC field is at offset 4 (eapol_hdr) + 1 + 2 + 2 + 8 + 32 + 16 + 8 + 8 = 81
       i.e., at offset: sizeof(eapol_hdr) + offsetof(eapol_key, mic) */
    uint16_t mic_offset = 4 + 77; /* eapol_hdr(4) + key_type(1) + key_info(2) + key_len(2)
                                       + replay(8) + nonce(32) + iv(16) + rsc(8) + reserved(8) = 77 */

    /* Make a copy with MIC zeroed */
    uint8_t frame_copy[512];
    if (frame_len > sizeof(frame_copy)) return;
    w_copy(frame_copy, eapol_frame, frame_len);
    w_zero(frame_copy + mic_offset, 16);

    /* HMAC-SHA1(KCK, frame) → 20 bytes, take first 16 */
    uint8_t hmac_out[20];
    hmac_sha1(kck, WPA2_KCK_LEN, frame_copy, frame_len, hmac_out);
    w_copy(mic_out, hmac_out, 16);
}

/* ================================================================
 * Build EAPOL-Key Message 2 (STA → AP)
 *
 * Response to Message 1: sends our SNonce + MIC.
 * Key Info: Pairwise + MIC + HMAC-SHA1-128
 * ================================================================ */

int wpa2_build_msg2(struct wpa2_ctx *ctx,
                     uint8_t *out, uint16_t out_max) {
    /* EAPOL-Key message 2 structure:
       eapol_hdr (4) + eapol_key (95 base) + RSN IE as key data */

    uint8_t rsn_ie[22];
    uint16_t rsn_len = wpa2_build_rsn_ie(rsn_ie, sizeof(rsn_ie));

    uint16_t body_len = (uint16_t)(sizeof(struct eapol_key) + rsn_len);
    uint16_t total = (uint16_t)(sizeof(struct eapol_hdr) + body_len);
    if (total > out_max) return -1;

    w_zero(out, total);

    struct eapol_hdr *ehdr = (struct eapol_hdr *)out;
    ehdr->version = EAPOL_VERSION;
    ehdr->type = EAPOL_TYPE_KEY;
    w_put_be16((uint8_t *)&ehdr->body_len, body_len);

    struct eapol_key *key = (struct eapol_key *)(out + sizeof(struct eapol_hdr));
    key->key_type = EAPOL_KEY_TYPE_RSN;

    /* Key Info: Pairwise + MIC + HMAC-SHA1 (type 2) */
    uint16_t key_info = WPA2_KEY_INFO_TYPE_HMAC_SHA1 | WPA2_KEY_INFO_PAIRWISE |
                        WPA2_KEY_INFO_MIC;
    w_put_be16((uint8_t *)&key->key_info, key_info);

    /* Key length: 16 (CCMP TK length) */
    w_put_be16((uint8_t *)&key->key_len, 16);

    /* Replay counter (copy from message 1) */
    w_put_be64(key->replay, ctx->replay_counter);

    /* SNonce */
    w_copy(key->nonce, ctx->snonce, WPA2_NONCE_LEN);

    /* Key data: RSN IE */
    w_put_be16((uint8_t *)&key->key_data_len, rsn_len);
    w_copy(out + sizeof(struct eapol_hdr) + sizeof(struct eapol_key),
           rsn_ie, rsn_len);

    /* Compute MIC over the frame */
    uint8_t mic[16];
    wpa2_compute_mic(ctx->kck, out, total, mic);
    w_copy(key->mic, mic, 16);

    kprint("IWL-WPA2: built message 2 (%u bytes)\n", total);
    return (int)total;
}

/* ================================================================
 * Build EAPOL-Key Message 4 (STA → AP)
 *
 * Response to Message 3: ACK with MIC.
 * Key Info: Pairwise + MIC + Secure + HMAC-SHA1-128
 * ================================================================ */

int wpa2_build_msg4(struct wpa2_ctx *ctx,
                     uint8_t *out, uint16_t out_max) {
    uint16_t body_len = (uint16_t)sizeof(struct eapol_key);
    uint16_t total = (uint16_t)(sizeof(struct eapol_hdr) + body_len);
    if (total > out_max) return -1;

    w_zero(out, total);

    struct eapol_hdr *ehdr = (struct eapol_hdr *)out;
    ehdr->version = EAPOL_VERSION;
    ehdr->type = EAPOL_TYPE_KEY;
    w_put_be16((uint8_t *)&ehdr->body_len, body_len);

    struct eapol_key *key = (struct eapol_key *)(out + sizeof(struct eapol_hdr));
    key->key_type = EAPOL_KEY_TYPE_RSN;

    /* Key Info: Pairwise + MIC + Secure + HMAC-SHA1 */
    uint16_t key_info = WPA2_KEY_INFO_TYPE_HMAC_SHA1 | WPA2_KEY_INFO_PAIRWISE |
                        WPA2_KEY_INFO_MIC | WPA2_KEY_INFO_SECURE;
    w_put_be16((uint8_t *)&key->key_info, key_info);

    /* Key length: 16 */
    w_put_be16((uint8_t *)&key->key_len, 16);

    /* Replay counter (from message 3) */
    w_put_be64(key->replay, ctx->replay_counter);

    /* No key data in message 4 */
    w_put_be16((uint8_t *)&key->key_data_len, 0);

    /* Compute MIC */
    uint8_t mic[16];
    wpa2_compute_mic(ctx->kck, out, total, mic);
    w_copy(key->mic, mic, 16);

    kprint("IWL-WPA2: built message 4 (%u bytes)\n", total);
    return (int)total;
}

/* ================================================================
 * Process Incoming EAPOL-Key Frame
 *
 * Handles messages 1 and 3 of the four-way handshake.
 * ================================================================ */

int wpa2_process_eapol(struct wpa2_ctx *ctx,
                        const uint8_t *eapol_frame, uint16_t frame_len) {
    if (frame_len < sizeof(struct eapol_hdr) + sizeof(struct eapol_key)) {
        kprint("IWL-WPA2: EAPOL frame too short (%u bytes)\n", frame_len);
        return -1;
    }

    if (!ctx->pmk_valid) {
        kprint("IWL-WPA2: PMK not set, cannot process EAPOL\n");
        return -1;
    }

    const struct eapol_hdr *ehdr = (const struct eapol_hdr *)eapol_frame;
    const struct eapol_key *key = (const struct eapol_key *)(eapol_frame +
                                   sizeof(struct eapol_hdr));

    if (ehdr->type != EAPOL_TYPE_KEY || key->key_type != EAPOL_KEY_TYPE_RSN) {
        kprint("IWL-WPA2: not an RSN EAPOL-Key frame\n");
        return -1;
    }

    uint16_t key_info = w_be16((const uint8_t *)&key->key_info);
    uint64_t replay = w_be64(key->replay);
    uint16_t key_data_len = w_be16((const uint8_t *)&key->key_data_len);

    int has_ack = (key_info & WPA2_KEY_INFO_ACK) != 0;
    int has_mic = (key_info & WPA2_KEY_INFO_MIC) != 0;
    int has_secure = (key_info & WPA2_KEY_INFO_SECURE) != 0;
    int has_enc = (key_info & WPA2_KEY_INFO_ENC_KEY_DATA) != 0;
    int is_pairwise = (key_info & WPA2_KEY_INFO_PAIRWISE) != 0;
    int has_install = (key_info & WPA2_KEY_INFO_INSTALL) != 0;

    kprint("IWL-WPA2: EAPOL-Key info=0x%04x replay=%llu kd_len=%u\n",
           key_info, (unsigned long long)replay, key_data_len);

    /* ---- Message 1: ACK + Pairwise, no MIC, no Secure ---- */
    if (has_ack && is_pairwise && !has_mic && !has_secure) {
        if (ctx->hs_state != 1) {
            kprint("IWL-WPA2: unexpected message 1 in state %u\n", ctx->hs_state);
        }

        kprint("IWL-WPA2: received message 1 (ANonce)\n");

        /* Save ANonce from AP */
        w_copy(ctx->anonce, key->nonce, WPA2_NONCE_LEN);
        ctx->replay_counter = replay;

        /* Generate our SNonce */
        crypto_random(ctx->snonce, WPA2_NONCE_LEN);

        /* Derive PTK */
        wpa2_derive_ptk(ctx);

        ctx->hs_state = 2;
        return 0;
    }

    /* ---- Message 3: ACK + Pairwise + MIC + Secure + Install + Encrypted ---- */
    if (has_ack && is_pairwise && has_mic && has_secure && has_install) {
        if (ctx->hs_state != 2 && ctx->hs_state != 3) {
            kprint("IWL-WPA2: unexpected message 3 in state %u\n", ctx->hs_state);
        }

        kprint("IWL-WPA2: received message 3 (GTK)\n");

        /* Verify MIC */
        uint8_t computed_mic[16];
        wpa2_compute_mic(ctx->kck, eapol_frame, frame_len, computed_mic);
        if (crypto_memcmp(computed_mic, key->mic, 16) != 0) {
            kprint("IWL-WPA2: message 3 MIC verification FAILED!\n");
            return -1;
        }
        kprint("IWL-WPA2: message 3 MIC verified OK\n");

        /* Replay counter must be >= last seen */
        if (replay < ctx->replay_counter) {
            kprint("IWL-WPA2: message 3 replay counter too low\n");
            return -1;
        }
        ctx->replay_counter = replay;

        /* Verify ANonce matches message 1 */
        if (crypto_memcmp(ctx->anonce, key->nonce, WPA2_NONCE_LEN) != 0) {
            kprint("IWL-WPA2: message 3 ANonce mismatch!\n");
            return -1;
        }

        /* Decrypt key data to extract GTK */
        if (has_enc && key_data_len >= 24) {
            const uint8_t *wrapped_data = eapol_frame +
                                           sizeof(struct eapol_hdr) +
                                           sizeof(struct eapol_key);
            uint8_t unwrapped[64];

            if (key_data_len > 64 + 8) {
                kprint("IWL-WPA2: key data too large (%u)\n", key_data_len);
                return -1;
            }

            int rc = aes_key_unwrap(ctx->kek, wrapped_data, key_data_len, unwrapped);
            if (rc != 0) {
                kprint("IWL-WPA2: GTK key unwrap FAILED\n");
                return -1;
            }

            /* The unwrapped data contains KDE (Key Data Encapsulation):
               - RSN IE (optional)
               - GTK KDE: type=0xDD, len, OUI=00-0F-AC, data_type=1,
                          key_id(1), tx(1), reserved(1), GTK(16)
             * We search for the GTK KDE. */
            uint16_t ud_len = key_data_len - 8; /* unwrapped length */
            uint16_t pos = 0;
            int gtk_found = 0;

            while (pos + 2 <= ud_len) {
                uint8_t kde_type = unwrapped[pos];
                uint8_t kde_len = unwrapped[pos + 1];
                if (pos + 2 + kde_len > ud_len) break;

                if (kde_type == 0xDD && kde_len >= 6 + WPA2_GTK_LEN) {
                    /* Check OUI: 00-0F-AC, type 1 (GTK) */
                    if (unwrapped[pos + 2] == 0x00 &&
                        unwrapped[pos + 3] == 0x0F &&
                        unwrapped[pos + 4] == 0xAC &&
                        unwrapped[pos + 5] == 0x01) {
                        /* GTK KDE found */
                        ctx->gtk_idx = unwrapped[pos + 6] & 0x03;
                        w_copy(ctx->gtk, unwrapped + pos + 8, WPA2_GTK_LEN);
                        ctx->gtk_valid = 1;
                        gtk_found = 1;
                        kprint("IWL-WPA2: GTK installed (idx=%u)\n", ctx->gtk_idx);
                        break;
                    }
                }

                pos += 2 + kde_len;
            }

            if (!gtk_found) {
                kprint("IWL-WPA2: GTK KDE not found in key data\n");
                /* Non-fatal: PTK alone is sufficient for unicast */
            }
        }

        ctx->hs_state = 3; /* ready to send message 4 */
        ctx->tx_pn = 0;
        ctx->rx_pn = 0;
        return 0;
    }

    kprint("IWL-WPA2: unhandled EAPOL-Key info=0x%04x\n", key_info);
    return -1;
}

int wpa2_is_connected(const struct wpa2_ctx *ctx) {
    return ctx->hs_state >= 4 && ctx->ptk_valid;
}

/* ================================================================
 * CCMP Data Encryption (TX)
 *
 * Builds: 802.11 header + CCMP header (8 bytes) + encrypted + MIC (8 bytes)
 * ================================================================ */

int wpa2_encrypt_data(struct wpa2_ctx *ctx,
                       const uint8_t *hdr, uint16_t hdr_len,
                       const uint8_t *payload, uint16_t pt_len,
                       uint8_t *out) {
    if (!ctx->ptk_valid) return -1;

    /* Copy 802.11 header (with Protected bit set) */
    w_copy(out, hdr, hdr_len);
    out[1] |= 0x40; /* set Protected Frame bit in FC byte 1 */

    /* Build CCMP header (8 bytes):
     * Byte 0: PN0
     * Byte 1: PN1
     * Byte 2: 0 (reserved)
     * Byte 3: (ExtIV=1) | (KeyID << 6) = 0x20 | (key_id << 6)
     * Bytes 4-7: PN2, PN3, PN4, PN5
     */
    uint64_t pn = ctx->tx_pn++;
    uint8_t pn_bytes[6];
    pn_bytes[0] = (uint8_t)(pn);
    pn_bytes[1] = (uint8_t)(pn >> 8);
    pn_bytes[2] = (uint8_t)(pn >> 16);
    pn_bytes[3] = (uint8_t)(pn >> 24);
    pn_bytes[4] = (uint8_t)(pn >> 32);
    pn_bytes[5] = (uint8_t)(pn >> 40);

    uint8_t *ccmp_hdr = out + hdr_len;
    ccmp_hdr[0] = pn_bytes[0];
    ccmp_hdr[1] = pn_bytes[1];
    ccmp_hdr[2] = 0;
    ccmp_hdr[3] = 0x20; /* ExtIV=1, KeyID=0 */
    ccmp_hdr[4] = pn_bytes[2];
    ccmp_hdr[5] = pn_bytes[3];
    ccmp_hdr[6] = pn_bytes[4];
    ccmp_hdr[7] = pn_bytes[5];

    /* A2 = transmitter address = out[10..15] (address 2 in 802.11 header) */
    const uint8_t *a2 = out + 10;

    /* Encrypt with CCMP */
    int rc = ccmp_encrypt(ctx->tk, pn_bytes, a2, 0,
                           out, hdr_len,
                           payload, pt_len,
                           out + hdr_len + 8);
    if (rc != 0) return -1;

    /* Total: hdr + 8 (CCMP hdr) + pt_len + 8 (MIC) */
    return (int)(hdr_len + 8 + pt_len + 8);
}

/* ================================================================
 * CCMP Data Decryption (RX)
 * ================================================================ */

int wpa2_decrypt_data(struct wpa2_ctx *ctx,
                       const uint8_t *frame, uint16_t frame_len,
                       uint8_t *out, uint16_t out_max) {
    if (!ctx->ptk_valid) return -1;

    /* Determine header length (24 for 3-addr, 30 for 4-addr, +2 for QoS) */
    uint16_t hdr_len = 24;
    uint16_t fc = (uint16_t)(frame[0] | ((uint16_t)frame[1] << 8));
    if ((fc & 0x0300) == 0x0300) hdr_len = 30; /* 4-address */
    if (fc & 0x0080) hdr_len += 2; /* QoS */

    if (frame_len < hdr_len + 8 + 8) return -1; /* need at least CCMP hdr + MIC */

    /* Extract PN from CCMP header */
    const uint8_t *ccmp_hdr = frame + hdr_len;
    uint8_t pn_bytes[6];
    pn_bytes[0] = ccmp_hdr[0];
    pn_bytes[1] = ccmp_hdr[1];
    pn_bytes[2] = ccmp_hdr[4];
    pn_bytes[3] = ccmp_hdr[5];
    pn_bytes[4] = ccmp_hdr[6];
    pn_bytes[5] = ccmp_hdr[7];

    /* Replay protection: PN must be strictly greater than last received */
    uint64_t pn = (uint64_t)pn_bytes[0] |
                  ((uint64_t)pn_bytes[1] << 8) |
                  ((uint64_t)pn_bytes[2] << 16) |
                  ((uint64_t)pn_bytes[3] << 24) |
                  ((uint64_t)pn_bytes[4] << 32) |
                  ((uint64_t)pn_bytes[5] << 40);

    if (pn <= ctx->rx_pn) {
        kprint("IWL-WPA2: replay detected! pn=%llu <= rx_pn=%llu\n",
               (unsigned long long)pn, (unsigned long long)ctx->rx_pn);
        return -1;
    }

    /* A2 = transmitter address (offset 10 in 802.11 header) */
    const uint8_t *a2 = frame + 10;

    /* Ciphertext starts after CCMP header, includes MIC */
    uint16_t ct_len = frame_len - hdr_len - 8;
    uint16_t pt_len = ct_len - 8;

    if (ct_len < 8 || pt_len > out_max)
        return -1;

    int rc = ccmp_decrypt(ctx->tk, pn_bytes, a2, 0,
                           frame, hdr_len,
                           frame + hdr_len + 8, ct_len,
                           out);
    if (rc != 0) return -1;

    /* Update replay counter */
    ctx->rx_pn = pn;

    return (int)pt_len; /* plaintext length */
}
