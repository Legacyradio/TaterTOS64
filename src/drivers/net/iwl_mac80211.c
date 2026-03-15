// Intel 9260 WiFi: 802.11 MAC layer — scanning, auth, association, EAPOL

#include <stdint.h>
#include "iwl_mac80211.h"
#include "iwl_wpa2.h"
#include "iwl_cmd.h"

void kprint(const char *fmt, ...);

/* ---- Helpers ---- */

static void mac_zero(void *ptr, uint32_t len) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < len; i++) p[i] = 0;
}

static void mac_copy(void *dst, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}

static int mac_equal(const uint8_t *a, const uint8_t *b, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static uint16_t mac_read16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void mac_write16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

__attribute__((unused))
static void mac_write32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* Broadcast address */
static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Our MAC address (set during init) */
static uint8_t g_mac_addr[6];

/* Sequence number counter for TX frames */
static uint16_t g_seq_num;

/* Context IDs assigned during init */
static uint32_t g_mac_ctx_id;
static uint32_t g_phy_ctx_id;
static uint32_t g_binding_id;

/* ---- 2.4 GHz Channel List ---- */
/* Channels 1-11 (US), frequencies for reference only — firmware uses channel numbers */
static const uint8_t CHANNELS_2G[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
#define NUM_CHANNELS_2G  11

/* 5 GHz channels (common UNII bands) — will be used when 5 GHz scan is added */
__attribute__((unused))
static const uint8_t CHANNELS_5G[] = {
    36, 40, 44, 48,      /* UNII-1 */
    52, 56, 60, 64,      /* UNII-2 */
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, /* UNII-2e */
    149, 153, 157, 161, 165 /* UNII-3 */
};
#define NUM_CHANNELS_5G  25

/* ---- MAC Context Setup ---- */

static int iwl_mac_ctx_add(const uint8_t addr[6]) {
    struct iwl_mac_ctx_cmd cmd;
    mac_zero(&cmd, sizeof(cmd));

    g_mac_ctx_id = 0; /* MAC context ID 0 */
    cmd.id_and_color = g_mac_ctx_id;
    cmd.action = IWL_MAC_CTXT_ACTION_ADD;
    cmd.mac_type = IWL_MAC_CTXT_TYPE_STA;
    mac_copy(cmd.addr, addr, 6);
    cmd.tsf_id = 0;
    cmd.filter_flags = 0;

    int rc = iwl_send_cmd_sync(IWL_MAC_CONTEXT_CMD, 0,
                                &cmd, sizeof(cmd), 0, 0, 0);
    if (rc != 0) {
        kprint("IWL-MAC: MAC_CONTEXT_CMD ADD failed rc=%d\n", rc);
        return rc;
    }
    kprint("IWL-MAC: MAC context added (STA mode)\n");
    return 0;
}

/* ---- PHY Context Setup ---- */

static int iwl_phy_ctx_add(uint8_t channel, uint8_t band) {
    struct iwl_phy_ctx_cmd cmd;
    mac_zero(&cmd, sizeof(cmd));

    g_phy_ctx_id = 0; /* PHY context ID 0 */
    cmd.id_and_color = g_phy_ctx_id;
    cmd.action = IWL_PHY_CTXT_ACTION_ADD;
    cmd.channel = channel;
    cmd.band = band; /* 0=2.4GHz, 1=5GHz */
    cmd.width = 0;   /* 20 MHz */
    cmd.position = 0;
    cmd.txchain = 0x3; /* chains A+B */
    cmd.rxchain = 0x3;

    int rc = iwl_send_cmd_sync(IWL_PHY_CONTEXT_CMD, 0,
                                &cmd, sizeof(cmd), 0, 0, 0);
    if (rc != 0) {
        kprint("IWL-MAC: PHY_CONTEXT_CMD ADD failed rc=%d\n", rc);
        return rc;
    }
    kprint("IWL-MAC: PHY context added ch=%u band=%u\n", channel, band);
    return 0;
}

/* ---- Binding ---- */

static int iwl_binding_add(void) {
    struct iwl_binding_cmd cmd;
    mac_zero(&cmd, sizeof(cmd));

    g_binding_id = 0;
    cmd.id_and_color = g_binding_id;
    cmd.action = IWL_BINDING_ACTION_ADD;
    cmd.macs[0] = g_mac_ctx_id;
    cmd.macs[1] = 0xFFFFFFFFu; /* unused slot */
    cmd.phy_id = g_phy_ctx_id;

    int rc = iwl_send_cmd_sync(IWL_BINDING_CONTEXT_CMD, 0,
                                &cmd, sizeof(cmd), 0, 0, 0);
    if (rc != 0) {
        kprint("IWL-MAC: BINDING_CMD ADD failed rc=%d\n", rc);
        return rc;
    }
    kprint("IWL-MAC: binding created (MAC %u -> PHY %u)\n",
           g_mac_ctx_id, g_phy_ctx_id);
    return 0;
}

/* ---- Build Probe Request Frame ---- */

/*
 * Build a probe request frame for active scanning.
 * Returns total frame length.
 */
static uint16_t build_probe_request(uint8_t *buf, uint16_t max_len,
                                     const uint8_t *sa,
                                     const uint8_t *ssid, uint8_t ssid_len) {
    if (max_len < 128) return 0;
    uint16_t pos = 0;

    /* Management header */
    struct ieee80211_mgmt_hdr *hdr = (struct ieee80211_mgmt_hdr *)buf;
    mac_zero(hdr, sizeof(*hdr));
    hdr->frame_control = IEEE80211_FC_TYPE_MGMT | IEEE80211_FC_SUBTYPE_PROBE_REQ;
    mac_copy(hdr->da, BROADCAST_ADDR, 6);
    mac_copy(hdr->sa, sa, 6);
    mac_copy(hdr->bssid, BROADCAST_ADDR, 6);
    pos = sizeof(struct ieee80211_mgmt_hdr);

    /* SSID IE (empty for wildcard scan, or specific SSID) */
    buf[pos++] = IEEE80211_IE_SSID;
    buf[pos++] = ssid_len;
    if (ssid_len > 0 && ssid) {
        mac_copy(buf + pos, ssid, ssid_len);
        pos += ssid_len;
    }

    /* Supported Rates IE: 1, 2, 5.5, 11, 6, 9, 12, 18 Mbps */
    buf[pos++] = IEEE80211_IE_SUPPORTED_RATES;
    buf[pos++] = 8;
    buf[pos++] = 0x82; /* 1 Mbps (basic) */
    buf[pos++] = 0x84; /* 2 Mbps (basic) */
    buf[pos++] = 0x8B; /* 5.5 Mbps (basic) */
    buf[pos++] = 0x96; /* 11 Mbps (basic) */
    buf[pos++] = 0x0C; /* 6 Mbps */
    buf[pos++] = 0x12; /* 9 Mbps */
    buf[pos++] = 0x18; /* 12 Mbps */
    buf[pos++] = 0x24; /* 18 Mbps */

    /* Extended Supported Rates IE: 24, 36, 48, 54 Mbps */
    buf[pos++] = IEEE80211_IE_EXT_RATES;
    buf[pos++] = 4;
    buf[pos++] = 0x30; /* 24 Mbps */
    buf[pos++] = 0x48; /* 36 Mbps */
    buf[pos++] = 0x60; /* 48 Mbps */
    buf[pos++] = 0x6C; /* 54 Mbps */

    return pos;
}

/* ---- UMAC Scan ---- */

/*
 * Send UMAC scan request to firmware.
 * The firmware handles channel hopping and probe TX autonomously.
 * Scan results come back as RX notifications containing beacon/probe response frames.
 */
static int iwl_start_scan(void) {
    /* Build scan command with channel list and probe request template.
       We pack everything into a single buffer:
       [scan_req_umac header][channel entries][probe request template] */
    uint8_t cmd_buf[512];
    mac_zero(cmd_buf, sizeof(cmd_buf));

    struct iwl_scan_req_umac *scan = (struct iwl_scan_req_umac *)cmd_buf;
    scan->flags = 0;
    scan->uid = 1;
    scan->ooc_priority = 0;
    scan->general_flags = 0x04; /* passive-to-active promotion */
    scan->active_dwell = 30;    /* 30 ms active dwell per channel */
    scan->passive_dwell = 110;  /* 110 ms passive dwell per channel */
    scan->fragmented_dwell = 0;
    scan->max_out_of_time = 0;
    scan->suspend_time = 0;

    /* Add 2.4 GHz channels */
    scan->n_channels = NUM_CHANNELS_2G;
    uint16_t pos = sizeof(struct iwl_scan_req_umac);

    for (uint32_t i = 0; i < NUM_CHANNELS_2G; i++) {
        /* Channel entry: channel(2 bytes) | band(2 bytes) = 4 bytes */
        mac_write16(cmd_buf + pos, CHANNELS_2G[i]);
        mac_write16(cmd_buf + pos + 2, 0); /* band 0 = 2.4 GHz */
        pos += 4;
    }

    /* Build probe request template after channel list */
    uint16_t probe_len = build_probe_request(cmd_buf + pos,
                                              (uint16_t)(sizeof(cmd_buf) - pos),
                                              g_mac_addr, 0, 0);
    pos += probe_len;

    kprint("IWL-MAC: starting UMAC scan (%u channels, cmd=%u bytes)\n",
           scan->n_channels, pos);

    int rc = iwl_send_cmd_sync(IWL_SCAN_REQ_UMAC, IWL_SCAN_GROUP_ID,
                                cmd_buf, pos, 0, 0, 0);
    if (rc != 0) {
        kprint("IWL-MAC: SCAN_REQ_UMAC failed rc=%d\n", rc);
        return rc;
    }

    return 0;
}

/*
 * Parse a beacon or probe response frame from an RX buffer.
 * Extracts SSID, BSSID, channel, capabilities, and RSN presence.
 */
static int parse_beacon(const uint8_t *frame, uint16_t frame_len,
                         struct iwl_bss_entry *entry) {
    if (frame_len < sizeof(struct ieee80211_mgmt_hdr) + 12) return -1;

    const struct ieee80211_mgmt_hdr *hdr = (const struct ieee80211_mgmt_hdr *)frame;
    uint16_t fc = hdr->frame_control;

    /* Check it's a beacon or probe response */
    if ((fc & 0x00FC) != IEEE80211_FC_SUBTYPE_BEACON &&
        (fc & 0x00FC) != IEEE80211_FC_SUBTYPE_PROBE_RESP)
        return -1;

    mac_zero(entry, sizeof(*entry));
    mac_copy(entry->bssid, hdr->bssid, 6);

    /* Fixed fields after management header: timestamp(8) + beacon_interval(2) + capability(2) */
    uint16_t pos = sizeof(struct ieee80211_mgmt_hdr);
    /* Skip timestamp (8 bytes) */
    pos += 8;
    entry->beacon_interval = mac_read16(frame + pos);
    pos += 2;
    entry->capability = mac_read16(frame + pos);
    pos += 2;

    /* Walk IEs */
    while (pos + 2 <= frame_len) {
        uint8_t ie_id = frame[pos];
        uint8_t ie_len = frame[pos + 1];
        if (pos + 2 + ie_len > frame_len) break;

        const uint8_t *ie_data = frame + pos + 2;

        switch (ie_id) {
        case IEEE80211_IE_SSID:
            if (ie_len <= IWL_MAX_SSID_LEN) {
                mac_copy(entry->ssid, ie_data, ie_len);
                entry->ssid[ie_len] = '\0';
                entry->ssid_len = ie_len;
            }
            break;

        case IEEE80211_IE_DS_PARAMS:
            if (ie_len >= 1) {
                entry->channel = ie_data[0];
            }
            break;

        case IEEE80211_IE_RSN:
            entry->has_rsn = 1;
            break;
        }

        pos += 2 + ie_len;
    }

    entry->valid = 1;
    return 0;
}

/* ---- Scan Results Collection ---- */

/*
 * Wait for scan results. Polls the RX queue for beacon/probe response
 * notifications for up to timeout_ms, parses them into BSS entries.
 */
static int iwl_collect_scan_results(struct iwl_bss_entry *results,
                                     uint32_t max_results, uint32_t *count,
                                     uint32_t timeout_ms) {
    *count = 0;
    uint32_t timeout_us = timeout_ms * 1000u;
    uint32_t elapsed = 0;
    uint32_t poll_interval = 2000u; /* 2 ms */
    uint32_t poll_slice_ms = poll_interval / 1000u;

    uint8_t resp_buf[4096];

    while (elapsed < timeout_us && *count < max_results) {
        /* Try to receive the next RX notification */
        int rc = iwl_wait_resp(0xFFFF, poll_slice_ms, resp_buf, sizeof(resp_buf));
        /* Note: sequence 0xFFFF means "accept any" — we scan all RX buffers
           in iwl_wait_resp by checking for new closed_rb entries */

        if (rc == 0) {
            const struct iwl_rx_packet *pkt = (const struct iwl_rx_packet *)resp_buf;
            uint32_t payload_len = iwl_rx_packet_payload_len(pkt);

            /* Got an RX buffer — check if it's a beacon/probe response.
               The RX packet wrapper is 8 bytes total (len_n_flags + fw hdr).
               For scan results, the firmware payload wraps the 802.11 frame. */
            uint8_t cmd_id = pkt->cmd_id;

            /* Check for SCAN_COMPLETE notification */
            if (cmd_id == IWL_SCAN_COMPLETE_UMAC) {
                kprint("IWL-MAC: scan complete notification received\n");
                break;
            }

            struct iwl_bss_entry entry;
            if (payload_len > 28u &&
                parse_beacon(pkt->data + 28, (uint16_t)(payload_len - 28u), &entry) == 0) {
                /* Check for duplicate BSSID */
                int dup = 0;
                for (uint32_t i = 0; i < *count; i++) {
                    if (mac_equal(results[i].bssid, entry.bssid, 6)) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    results[*count] = entry;
                    (*count)++;
                    kprint("IWL-MAC: found BSS \"%s\" ch=%u %02x:%02x:%02x:%02x:%02x:%02x%s\n",
                           entry.ssid, entry.channel,
                           entry.bssid[0], entry.bssid[1], entry.bssid[2],
                           entry.bssid[3], entry.bssid[4], entry.bssid[5],
                           entry.has_rsn ? " [WPA2]" : " [OPEN]");
                }
            }
        }

        elapsed += poll_interval;
    }

    return 0;
}

/* ---- TX Management Frame ---- */

int iwl_tx_mgmt(const uint8_t *frame, uint16_t frame_len) {
    if (!frame || frame_len == 0 || frame_len > 2048) return -1;

    /* Build TX_CMD + frame payload */
    uint8_t cmd_buf[2200];
    mac_zero(cmd_buf, sizeof(cmd_buf));

    struct iwl_tx_cmd *tx = (struct iwl_tx_cmd *)cmd_buf;
    tx->len = frame_len;
    tx->tx_flags = IWL_TX_FLAGS_CMD_RATE | IWL_TX_FLAGS_ACK;
    tx->rate_n_flags = IWL_RATE_1M_CCK;
    tx->sta_id = 0;
    tx->data_retry_limit = 3;
    tx->rts_retry_limit = 3;
    tx->life_time = 0xFFFFFFFFu; /* infinite */

    /* Copy 802.11 frame after TX_CMD header */
    mac_copy(cmd_buf + sizeof(struct iwl_tx_cmd), frame, frame_len);

    uint16_t total = (uint16_t)(sizeof(struct iwl_tx_cmd) + frame_len);

    int rc = iwl_send_cmd_sync(IWL_TX_CMD, 0, cmd_buf, total, 0, 0, 0);
    if (rc != 0) {
        kprint("IWL-MAC: TX_CMD failed rc=%d\n", rc);
        return rc;
    }

    return 0;
}

/* ---- Authentication ---- */

static int iwl_send_auth(const uint8_t *bssid, const uint8_t *sa) {
    uint8_t frame[64];
    mac_zero(frame, sizeof(frame));

    struct ieee80211_mgmt_hdr *hdr = (struct ieee80211_mgmt_hdr *)frame;
    hdr->frame_control = IEEE80211_FC_TYPE_MGMT | IEEE80211_FC_SUBTYPE_AUTH;
    mac_copy(hdr->da, bssid, 6);
    mac_copy(hdr->sa, sa, 6);
    mac_copy(hdr->bssid, bssid, 6);
    hdr->seq_ctrl = (uint16_t)((g_seq_num++ & 0xFFF) << 4);

    uint16_t pos = sizeof(struct ieee80211_mgmt_hdr);

    /* Auth body: algorithm(2) + seq_number(2) + status(2) */
    mac_write16(frame + pos, IEEE80211_AUTH_ALG_OPEN); pos += 2;
    mac_write16(frame + pos, 1);  /* auth sequence 1 */    pos += 2;
    mac_write16(frame + pos, 0);  /* status = success */   pos += 2;

    kprint("IWL-MAC: sending auth to %02x:%02x:%02x:%02x:%02x:%02x\n",
           bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);

    return iwl_tx_mgmt(frame, pos);
}

/* ---- Association ---- */

static int iwl_send_assoc_req(const struct iwl_bss_entry *bss, const uint8_t *sa) {
    uint8_t frame[256];
    mac_zero(frame, sizeof(frame));

    struct ieee80211_mgmt_hdr *hdr = (struct ieee80211_mgmt_hdr *)frame;
    hdr->frame_control = IEEE80211_FC_TYPE_MGMT | IEEE80211_FC_SUBTYPE_ASSOC_REQ;
    mac_copy(hdr->da, bss->bssid, 6);
    mac_copy(hdr->sa, sa, 6);
    mac_copy(hdr->bssid, bss->bssid, 6);
    hdr->seq_ctrl = (uint16_t)((g_seq_num++ & 0xFFF) << 4);

    uint16_t pos = sizeof(struct ieee80211_mgmt_hdr);

    /* Capability info */
    uint16_t cap = IEEE80211_CAP_ESS | IEEE80211_CAP_SHORT_PREAMBLE |
                   IEEE80211_CAP_SHORT_SLOT;
    mac_write16(frame + pos, cap); pos += 2;

    /* Listen interval */
    mac_write16(frame + pos, 10); pos += 2;

    /* SSID IE */
    frame[pos++] = IEEE80211_IE_SSID;
    frame[pos++] = bss->ssid_len;
    mac_copy(frame + pos, bss->ssid, bss->ssid_len);
    pos += bss->ssid_len;

    /* Supported Rates IE */
    frame[pos++] = IEEE80211_IE_SUPPORTED_RATES;
    frame[pos++] = 8;
    frame[pos++] = 0x82; /* 1 Mbps (basic) */
    frame[pos++] = 0x84; /* 2 Mbps (basic) */
    frame[pos++] = 0x8B; /* 5.5 Mbps (basic) */
    frame[pos++] = 0x96; /* 11 Mbps (basic) */
    frame[pos++] = 0x0C; /* 6 Mbps */
    frame[pos++] = 0x12; /* 9 Mbps */
    frame[pos++] = 0x18; /* 12 Mbps */
    frame[pos++] = 0x24; /* 18 Mbps */

    /* Extended Supported Rates */
    frame[pos++] = IEEE80211_IE_EXT_RATES;
    frame[pos++] = 4;
    frame[pos++] = 0x30; /* 24 Mbps */
    frame[pos++] = 0x48; /* 36 Mbps */
    frame[pos++] = 0x60; /* 48 Mbps */
    frame[pos++] = 0x6C; /* 54 Mbps */

    kprint("IWL-MAC: sending assoc req to \"%s\" %02x:%02x:%02x:%02x:%02x:%02x\n",
           bss->ssid,
           bss->bssid[0], bss->bssid[1], bss->bssid[2],
           bss->bssid[3], bss->bssid[4], bss->bssid[5]);

    return iwl_tx_mgmt(frame, pos);
}

/* ---- Wait for Auth/Assoc Response ---- */

static int iwl_wait_mgmt_resp(uint16_t expected_subtype, uint32_t timeout_ms) {
    uint32_t timeout_us = timeout_ms * 1000u;
    uint32_t elapsed = 0;

    uint8_t resp_buf[4096];

    while (elapsed < timeout_us) {
        int rc = iwl_wait_resp(0xFFFF, 50, resp_buf, sizeof(resp_buf));
        if (rc == 0) {
            const struct iwl_rx_packet *pkt = (const struct iwl_rx_packet *)resp_buf;
            uint32_t payload_len = iwl_rx_packet_payload_len(pkt);
            if (payload_len < 28u + sizeof(struct ieee80211_mgmt_hdr)) {
                elapsed += 50000u;
                continue;
            }

            const uint8_t *frame = pkt->data + 28;
            uint16_t fc = mac_read16(frame);

            if ((fc & 0x00FC) == expected_subtype) {
                /* Found the response frame */
                uint16_t status_pos = sizeof(struct ieee80211_mgmt_hdr);

                if (expected_subtype == IEEE80211_FC_SUBTYPE_AUTH) {
                    /* Auth response: algo(2) + seq(2) + status(2) */
                    uint16_t status = mac_read16(frame + status_pos + 4);
                    kprint("IWL-MAC: auth response status=%u\n", status);
                    return (status == 0) ? 0 : -2;
                }

                if (expected_subtype == IEEE80211_FC_SUBTYPE_ASSOC_RESP) {
                    /* Assoc response: capability(2) + status(2) + AID(2) */
                    uint16_t status = mac_read16(frame + status_pos + 2);
                    uint16_t aid = mac_read16(frame + status_pos + 4) & 0x3FFF;
                    kprint("IWL-MAC: assoc response status=%u AID=%u\n", status, aid);
                    return (status == 0) ? 0 : -3;
                }
            }
        }

        elapsed += 2000;
    }

    return -1; /* timeout */
}

/* ---- Public API ---- */

int iwl_mac_init(const uint8_t mac_addr[6]) {
    mac_copy(g_mac_addr, mac_addr, 6);
    g_seq_num = 0;

    kprint("IWL-MAC: initializing 802.11 MAC layer\n");

    /* Set up MAC context (STA mode) */
    int rc = iwl_mac_ctx_add(mac_addr);
    if (rc != 0) return rc;

    /* Set up PHY context on channel 1 (2.4 GHz) as default */
    rc = iwl_phy_ctx_add(1, 0);
    if (rc != 0) return rc;

    /* Create binding (MAC <-> PHY) */
    rc = iwl_binding_add();
    if (rc != 0) return rc;

    kprint("IWL-MAC: 802.11 MAC layer initialized\n");
    return 0;
}

int iwl_scan(struct iwl_bss_entry *results, uint32_t max_results, uint32_t *count) {
    if (!results || !count) return -1;
    *count = 0;

    /* Start UMAC scan */
    int rc = iwl_start_scan();
    if (rc != 0) return rc;

    /* Collect results for up to 5 seconds (11 channels * ~140ms dwell + margin) */
    rc = iwl_collect_scan_results(results, max_results, count, 5000);
    if (rc != 0) return rc;

    kprint("IWL-MAC: scan found %u networks\n", *count);
    return 0;
}

int iwl_associate(const struct iwl_bss_entry *bss, const uint8_t mac_addr[6]) {
    if (!bss || !bss->valid) return -1;

    kprint("IWL-MAC: associating with \"%s\" ch=%u\n", bss->ssid, bss->channel);

    /* Update PHY context to the target channel */
    uint8_t band = (bss->channel > 14) ? 1 : 0;
    int rc = iwl_phy_ctx_add(bss->channel, band);
    if (rc != 0) return rc;

    /* Send authentication (open system) */
    rc = iwl_send_auth(bss->bssid, mac_addr);
    if (rc != 0) return rc;

    /* Wait for auth response (1 second timeout) */
    rc = iwl_wait_mgmt_resp(IEEE80211_FC_SUBTYPE_AUTH, 1000);
    if (rc != 0) {
        kprint("IWL-MAC: auth failed rc=%d\n", rc);
        return rc;
    }

    /* Send association request */
    rc = iwl_send_assoc_req(bss, mac_addr);
    if (rc != 0) return rc;

    /* Wait for association response (1 second timeout) */
    rc = iwl_wait_mgmt_resp(IEEE80211_FC_SUBTYPE_ASSOC_RESP, 1000);
    if (rc != 0) {
        kprint("IWL-MAC: assoc failed rc=%d\n", rc);
        return rc;
    }

    kprint("IWL-MAC: *** ASSOCIATED with \"%s\" ***\n", bss->ssid);
    return 0;
}

/* ---- WPA2 Association (includes RSN IE) ---- */

static int iwl_send_assoc_req_wpa2(const struct iwl_bss_entry *bss, const uint8_t *sa) {
    uint8_t frame[320];
    mac_zero(frame, sizeof(frame));

    struct ieee80211_mgmt_hdr *hdr = (struct ieee80211_mgmt_hdr *)frame;
    hdr->frame_control = IEEE80211_FC_TYPE_MGMT | IEEE80211_FC_SUBTYPE_ASSOC_REQ;
    mac_copy(hdr->da, bss->bssid, 6);
    mac_copy(hdr->sa, sa, 6);
    mac_copy(hdr->bssid, bss->bssid, 6);
    hdr->seq_ctrl = (uint16_t)((g_seq_num++ & 0xFFF) << 4);

    uint16_t pos = sizeof(struct ieee80211_mgmt_hdr);

    /* Capability info */
    uint16_t cap = IEEE80211_CAP_ESS | IEEE80211_CAP_SHORT_PREAMBLE |
                   IEEE80211_CAP_SHORT_SLOT;
    mac_write16(frame + pos, cap); pos += 2;

    /* Listen interval */
    mac_write16(frame + pos, 10); pos += 2;

    /* SSID IE */
    frame[pos++] = IEEE80211_IE_SSID;
    frame[pos++] = bss->ssid_len;
    mac_copy(frame + pos, bss->ssid, bss->ssid_len);
    pos += bss->ssid_len;

    /* Supported Rates IE */
    frame[pos++] = IEEE80211_IE_SUPPORTED_RATES;
    frame[pos++] = 8;
    frame[pos++] = 0x82; frame[pos++] = 0x84;
    frame[pos++] = 0x8B; frame[pos++] = 0x96;
    frame[pos++] = 0x0C; frame[pos++] = 0x12;
    frame[pos++] = 0x18; frame[pos++] = 0x24;

    /* Extended Supported Rates */
    frame[pos++] = IEEE80211_IE_EXT_RATES;
    frame[pos++] = 4;
    frame[pos++] = 0x30; frame[pos++] = 0x48;
    frame[pos++] = 0x60; frame[pos++] = 0x6C;

    /* RSN IE (WPA2-PSK, CCMP) */
    uint16_t rsn_len = wpa2_build_rsn_ie(frame + pos, (uint16_t)(sizeof(frame) - pos));
    pos += rsn_len;

    kprint("IWL-MAC: sending WPA2 assoc req to \"%s\" %02x:%02x:%02x:%02x:%02x:%02x\n",
           bss->ssid,
           bss->bssid[0], bss->bssid[1], bss->bssid[2],
           bss->bssid[3], bss->bssid[4], bss->bssid[5]);

    return iwl_tx_mgmt(frame, pos);
}

int iwl_associate_wpa2(const struct iwl_bss_entry *bss, const uint8_t mac_addr[6]) {
    if (!bss || !bss->valid) return -1;

    kprint("IWL-MAC: WPA2 associating with \"%s\" ch=%u\n", bss->ssid, bss->channel);

    /* Update PHY context to the target channel */
    uint8_t band = (bss->channel > 14) ? 1 : 0;
    int rc = iwl_phy_ctx_add(bss->channel, band);
    if (rc != 0) return rc;

    /* Send authentication (open system — WPA2 still uses open auth) */
    rc = iwl_send_auth(bss->bssid, mac_addr);
    if (rc != 0) return rc;

    /* Wait for auth response */
    rc = iwl_wait_mgmt_resp(IEEE80211_FC_SUBTYPE_AUTH, 1000);
    if (rc != 0) {
        kprint("IWL-MAC: WPA2 auth failed rc=%d\n", rc);
        return rc;
    }

    /* Send association request with RSN IE */
    rc = iwl_send_assoc_req_wpa2(bss, mac_addr);
    if (rc != 0) return rc;

    /* Wait for association response */
    rc = iwl_wait_mgmt_resp(IEEE80211_FC_SUBTYPE_ASSOC_RESP, 1000);
    if (rc != 0) {
        kprint("IWL-MAC: WPA2 assoc failed rc=%d\n", rc);
        return rc;
    }

    kprint("IWL-MAC: *** WPA2 ASSOCIATED with \"%s\" — awaiting EAPOL ***\n", bss->ssid);
    return 0;
}

/* ---- EAPOL Frame Transmission ---- */

int iwl_tx_eapol(const uint8_t *da, const uint8_t *sa, const uint8_t *bssid,
                  const uint8_t *eapol_body, uint16_t eapol_len) {
    /* Build an 802.11 data frame carrying an EAPOL payload.
     *
     * Frame structure:
     *   802.11 data header (24 bytes)
     *   LLC/SNAP header (8 bytes)
     *   EAPOL body (eapol_len bytes)
     */
    uint8_t frame[512];
    if (eapol_len > sizeof(frame) - 24 - 8) return -1;

    mac_zero(frame, sizeof(frame));

    /* 802.11 data header */
    /* FC: Data frame, ToDS=1, FromDS=0 (STA → AP) */
    uint16_t fc = IEEE80211_FC_TYPE_DATA | 0x0100; /* type=data + ToDS */
    mac_write16(frame, fc);
    /* Duration: 0 */
    /* Address 1 = BSSID (receiver = AP), Address 2 = SA (transmitter = us),
       Address 3 = DA (destination) */
    mac_copy(frame + 4, bssid, 6);    /* A1 = BSSID */
    mac_copy(frame + 10, sa, 6);      /* A2 = SA */
    mac_copy(frame + 16, da, 6);      /* A3 = DA */
    /* Seq ctrl */
    mac_write16(frame + 22, (uint16_t)((g_seq_num++ & 0xFFF) << 4));

    uint16_t pos = 24;

    /* LLC/SNAP header for 802.1X (EtherType 0x888E) */
    frame[pos++] = EAPOL_LLC_DSAP;     /* 0xAA */
    frame[pos++] = EAPOL_LLC_SSAP;     /* 0xAA */
    frame[pos++] = EAPOL_LLC_CTRL;     /* 0x03 */
    frame[pos++] = EAPOL_SNAP_OUI0;    /* 0x00 */
    frame[pos++] = EAPOL_SNAP_OUI1;    /* 0x00 */
    frame[pos++] = EAPOL_SNAP_OUI2;    /* 0x00 */
    frame[pos++] = EAPOL_ETHERTYPE_HI; /* 0x88 */
    frame[pos++] = EAPOL_ETHERTYPE_LO; /* 0x8E */

    /* EAPOL body */
    mac_copy(frame + pos, eapol_body, eapol_len);
    pos += eapol_len;

    kprint("IWL-MAC: TX EAPOL frame (%u bytes)\n", pos);
    return iwl_tx_mgmt(frame, pos);
}
