#ifndef TATER_IWL_WPA2_H
#define TATER_IWL_WPA2_H

#include <stdint.h>
#include "iwl_mac80211.h"

/* ---- WPA2 Key Hierarchy ---- */

/* PTK (Pairwise Transient Key) components for CCMP:
 * Bytes  [0..15]  = KCK (Key Confirmation Key, for EAPOL MIC)
 * Bytes [16..31]  = KEK (Key Encryption Key, for wrapping GTK)
 * Bytes [32..47]  = TK  (Temporal Key, for CCMP data encryption)
 * Total: 48 bytes
 */
#define WPA2_KCK_LEN    16
#define WPA2_KEK_LEN    16
#define WPA2_TK_LEN     16
#define WPA2_PTK_LEN    48
#define WPA2_PMK_LEN    32
#define WPA2_NONCE_LEN  32
#define WPA2_GTK_LEN    16  /* CCMP group key */

/* WPA2 security context — holds all keys and state for a connection */
struct wpa2_ctx {
    /* PMK (Pairwise Master Key) — derived from passphrase via PBKDF2 */
    uint8_t pmk[WPA2_PMK_LEN];
    int pmk_valid;

    /* Nonces */
    uint8_t anonce[WPA2_NONCE_LEN]; /* AP's nonce (from message 1) */
    uint8_t snonce[WPA2_NONCE_LEN]; /* our nonce (generated locally) */

    /* PTK components */
    uint8_t kck[WPA2_KCK_LEN]; /* Key Confirmation Key */
    uint8_t kek[WPA2_KEK_LEN]; /* Key Encryption Key */
    uint8_t tk[WPA2_TK_LEN];   /* Temporal Key (CCMP) */
    int ptk_valid;

    /* GTK (Group Temporal Key) — from message 3 */
    uint8_t gtk[WPA2_GTK_LEN];
    uint8_t gtk_idx;            /* key index (0-3) */
    int gtk_valid;

    /* Addresses (for PTK derivation) */
    uint8_t our_mac[6];        /* STA address (AA for some, SA for others) */
    uint8_t ap_mac[6];         /* AP BSSID */

    /* Replay counter (from last EAPOL-Key received) */
    uint64_t replay_counter;

    /* TX packet number for CCMP */
    uint64_t tx_pn;
    /* RX packet number for replay protection */
    uint64_t rx_pn;

    /* 4-way handshake state */
    uint8_t hs_state; /* 0=idle, 1=waiting msg1, 2=sent msg2, 3=waiting msg3, 4=complete */
};

/* ---- EAPOL Frame Definitions ---- */

#define EAPOL_VERSION       2
#define EAPOL_TYPE_KEY      3
#define EAPOL_KEY_TYPE_RSN  2  /* WPA2 (802.11i) */

/* EAPOL-Key frame info bits */
#define WPA2_KEY_INFO_TYPE_MASK     0x0007
#define WPA2_KEY_INFO_TYPE_HMAC_SHA1 0x0002  /* HMAC-SHA1 MIC + AES Key Wrap */
#define WPA2_KEY_INFO_INSTALL       0x0040
#define WPA2_KEY_INFO_ACK           0x0080
#define WPA2_KEY_INFO_MIC           0x0100
#define WPA2_KEY_INFO_SECURE        0x0200
#define WPA2_KEY_INFO_ENC_KEY_DATA  0x1000
#define WPA2_KEY_INFO_PAIRWISE      0x0008

/* EAPOL header (4 bytes) */
struct eapol_hdr {
    uint8_t  version;       /* EAPOL version (2) */
    uint8_t  type;          /* EAPOL_TYPE_KEY = 3 */
    uint16_t body_len;      /* big-endian, length of body after this header */
} __attribute__((packed));

/* EAPOL-Key body (follows EAPOL header) */
struct eapol_key {
    uint8_t  key_type;      /* EAPOL_KEY_TYPE_RSN = 2 */
    uint16_t key_info;      /* big-endian: key info flags */
    uint16_t key_len;       /* big-endian: key length (16 for CCMP) */
    uint8_t  replay[8];     /* replay counter (big-endian) */
    uint8_t  nonce[32];     /* key nonce */
    uint8_t  iv[16];        /* key IV (zero for WPA2) */
    uint8_t  rsc[8];        /* receive sequence counter */
    uint8_t  reserved[8];   /* reserved */
    uint8_t  mic[16];       /* MIC (computed over entire EAPOL frame) */
    uint16_t key_data_len;  /* big-endian: length of key data */
    /* Key data follows (variable length) */
} __attribute__((packed));

/* 802.1X LLC/SNAP header for EAPOL frames over 802.11 */
#define EAPOL_LLC_DSAP      0xAA
#define EAPOL_LLC_SSAP      0xAA
#define EAPOL_LLC_CTRL      0x03
#define EAPOL_SNAP_OUI0     0x00
#define EAPOL_SNAP_OUI1     0x00
#define EAPOL_SNAP_OUI2     0x00
#define EAPOL_ETHERTYPE_HI  0x88  /* EtherType 0x888E = 802.1X Authentication */
#define EAPOL_ETHERTYPE_LO  0x8E

/* ---- RSN IE (for association request) ---- */

/* Build RSN IE for WPA2-PSK with CCMP.
 * Returns IE length (including ID + len bytes).
 * Writes to buf which must be at least 22 bytes.
 */
uint16_t wpa2_build_rsn_ie(uint8_t *buf, uint16_t max_len);

/* ---- WPA2 API ---- */

/* Initialize WPA2 context */
void wpa2_init(struct wpa2_ctx *ctx,
               const uint8_t our_mac[6],
               const uint8_t ap_mac[6]);

/* Derive PMK from passphrase + SSID using PBKDF2-SHA1 (4096 iterations).
 * This is slow (~4096 HMAC-SHA1 iterations per block).
 */
void wpa2_set_passphrase(struct wpa2_ctx *ctx,
                          const char *passphrase,
                          const uint8_t *ssid, uint8_t ssid_len);

/* Set PMK directly (if already derived) */
void wpa2_set_pmk(struct wpa2_ctx *ctx, const uint8_t pmk[32]);

/* Process an incoming EAPOL-Key frame.
 * Called when the driver receives an EAPOL frame from the AP during
 * the 4-way handshake.
 *
 * eapol_frame: the EAPOL frame (starting with eapol_hdr)
 * frame_len:   total length
 *
 * Returns 0 on success (may have advanced handshake state),
 *         -1 on error (bad MIC, bad frame, etc.)
 *
 * When handshake completes (after processing message 3 and sending message 4),
 * ctx->hs_state will be 4 and PTK/GTK will be installed.
 */
int wpa2_process_eapol(struct wpa2_ctx *ctx,
                        const uint8_t *eapol_frame, uint16_t frame_len);

/* Build EAPOL-Key message 2 (response to message 1).
 * Called internally by wpa2_process_eapol, but exposed for flexibility.
 *
 * out: output buffer for the EAPOL frame
 * out_max: output buffer size
 * Returns frame length, or -1 on error.
 */
int wpa2_build_msg2(struct wpa2_ctx *ctx,
                     uint8_t *out, uint16_t out_max);

/* Build EAPOL-Key message 4 (response to message 3).
 * out: output buffer
 * out_max: output buffer size
 * Returns frame length, or -1 on error.
 */
int wpa2_build_msg4(struct wpa2_ctx *ctx,
                     uint8_t *out, uint16_t out_max);

/* Check if handshake is complete (PTK installed) */
int wpa2_is_connected(const struct wpa2_ctx *ctx);

/* Encrypt a data frame using CCMP (for TX).
 * Increments tx_pn.
 *
 * hdr: 802.11 data frame header
 * hdr_len: header length
 * payload: plaintext payload
 * pt_len: payload length
 * out: output buffer (hdr_len + 8 [CCMP header] + pt_len + 8 [MIC])
 * Returns total output length, or -1 on error.
 */
int wpa2_encrypt_data(struct wpa2_ctx *ctx,
                       const uint8_t *hdr, uint16_t hdr_len,
                       const uint8_t *payload, uint16_t pt_len,
                       uint8_t *out);

/* Decrypt a received CCMP data frame.
 * Checks replay protection (PN must be > rx_pn).
 *
 * frame: full 802.11 frame (header + CCMP header + encrypted + MIC)
 * frame_len: total length
 * out: output plaintext (frame_len - hdr_len - 8 [CCMP hdr] - 8 [MIC])
 * out_max: capacity of the output buffer in bytes
 * Returns plaintext length, or -1 on error.
 */
int wpa2_decrypt_data(struct wpa2_ctx *ctx,
                       const uint8_t *frame, uint16_t frame_len,
                       uint8_t *out, uint16_t out_max);

#endif
