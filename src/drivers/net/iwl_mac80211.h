#ifndef TATER_IWL_MAC80211_H
#define TATER_IWL_MAC80211_H

#include <stdint.h>

/* Forward declaration */
struct wpa2_ctx;

/* ---- 802.11 Frame Definitions ---- */

/* Frame Control field bits */
#define IEEE80211_FC_TYPE_MGMT      0x0000
#define IEEE80211_FC_TYPE_DATA      0x0008
#define IEEE80211_FC_SUBTYPE_ASSOC_REQ   0x0000
#define IEEE80211_FC_SUBTYPE_ASSOC_RESP  0x0010
#define IEEE80211_FC_SUBTYPE_PROBE_REQ   0x0040
#define IEEE80211_FC_SUBTYPE_PROBE_RESP  0x0050
#define IEEE80211_FC_SUBTYPE_BEACON      0x0080
#define IEEE80211_FC_SUBTYPE_AUTH        0x00B0
#define IEEE80211_FC_SUBTYPE_DEAUTH      0x00C0

/* 802.11 management frame header — 24 bytes */
struct ieee80211_mgmt_hdr {
    uint16_t frame_control;
    uint16_t duration;
    uint8_t  da[6];       /* destination address */
    uint8_t  sa[6];       /* source address */
    uint8_t  bssid[6];    /* BSS ID */
    uint16_t seq_ctrl;
} __attribute__((packed));

/* Information Element header */
struct ieee80211_ie {
    uint8_t id;
    uint8_t len;
    /* data follows */
} __attribute__((packed));

/* IE IDs */
#define IEEE80211_IE_SSID           0
#define IEEE80211_IE_SUPPORTED_RATES 1
#define IEEE80211_IE_DS_PARAMS      3
#define IEEE80211_IE_RSN            48
#define IEEE80211_IE_HT_CAP         45
#define IEEE80211_IE_HT_OPERATION   61
#define IEEE80211_IE_EXT_RATES      50

/* Authentication algorithm numbers */
#define IEEE80211_AUTH_ALG_OPEN      0
#define IEEE80211_AUTH_ALG_SHARED    1

/* Capability info bits */
#define IEEE80211_CAP_ESS           (1u << 0)
#define IEEE80211_CAP_SHORT_PREAMBLE (1u << 5)
#define IEEE80211_CAP_SHORT_SLOT    (1u << 10)

/* ---- MVM Firmware Command IDs ---- */
#define IWL_MAC_CONTEXT_CMD         0x28
#define IWL_PHY_CONTEXT_CMD         0x08
#define IWL_BINDING_CONTEXT_CMD     0x2B
#define IWL_TIME_EVENT_CMD          0x29
#define IWL_ADD_STA_CMD             0x18
#define IWL_REMOVE_STA_CMD          0x19
#define IWL_TX_CMD                  0x1C
#define IWL_SCAN_REQ_UMAC           0x0C  /* UMAC scan for gen2 */
#define IWL_SCAN_ABORT_UMAC         0x0E
#define IWL_SCAN_COMPLETE_UMAC      0x0D  /* notification */

/* Scan request command group for gen2 */
#define IWL_SCAN_GROUP_ID           0x0D

/* MAC context actions */
#define IWL_MAC_CTXT_ACTION_ADD     0
#define IWL_MAC_CTXT_ACTION_MODIFY  1
#define IWL_MAC_CTXT_ACTION_REMOVE  2

/* MAC context types */
#define IWL_MAC_CTXT_TYPE_STA       1

/* PHY context actions */
#define IWL_PHY_CTXT_ACTION_ADD     0
#define IWL_PHY_CTXT_ACTION_MODIFY  1

/* Binding actions */
#define IWL_BINDING_ACTION_ADD      0
#define IWL_BINDING_ACTION_REMOVE   2

/* STA flags */
#define IWL_STA_FLG_ADD             0

/* ---- Scan Result / BSS Entry ---- */
#define IWL_MAX_SCAN_RESULTS    32
#define IWL_MAX_SSID_LEN        32

struct iwl_bss_entry {
    uint8_t  bssid[6];
    uint8_t  ssid[IWL_MAX_SSID_LEN + 1];
    uint8_t  ssid_len;
    uint8_t  channel;
    int8_t   rssi;          /* signal strength in dBm */
    uint16_t capability;
    uint16_t beacon_interval;
    uint8_t  has_rsn;       /* 1 if WPA2/RSN IE present */
    uint8_t  valid;
};

/* ---- MVM Command Structures ---- */

/* MAC context command (simplified for STA mode) */
struct iwl_mac_ctx_cmd {
    uint32_t id_and_color;    /* MAC ID in bits [3:0], color in [7:4] */
    uint32_t action;          /* ADD/MODIFY/REMOVE */
    uint32_t mac_type;        /* STA=1 */
    uint8_t  addr[6];        /* our MAC address */
    uint16_t reserved1;
    uint32_t tsf_id;
    uint32_t assoc_id;
    uint32_t bi;              /* beacon interval (0 for STA before assoc) */
    uint32_t dtim_interval;
    uint32_t filter_flags;
    uint32_t qos_flags;
    uint8_t  ac[4 * 12];     /* 4 AC parameters, 12 bytes each */
} __attribute__((packed));

/* PHY context command (simplified) */
struct iwl_phy_ctx_cmd {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t apply_time;
    uint32_t tx_param_color;
    uint32_t channel;         /* channel number */
    uint32_t band;            /* 0=2.4GHz, 1=5GHz */
    uint32_t width;           /* 0=20MHz */
    uint32_t position;        /* 0=primary */
    uint32_t txchain;         /* TX chain mask */
    uint32_t rxchain;         /* RX chain mask */
    uint32_t acquisition;
} __attribute__((packed));

/* Binding context command (simplified) */
struct iwl_binding_cmd {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t macs[2];         /* MAC IDs bound to this binding */
    uint32_t phy_id;          /* PHY context ID */
} __attribute__((packed));

/* UMAC scan request (simplified — gen2 format) */
struct iwl_scan_req_umac {
    uint32_t flags;           /* scan flags */
    uint32_t uid;             /* unique scan ID */
    uint32_t ooc_priority;    /* out-of-channel priority */
    uint32_t general_flags;
    uint8_t  active_dwell;    /* active scan dwell time (ms) */
    uint8_t  passive_dwell;   /* passive scan dwell time (ms) */
    uint8_t  fragmented_dwell;
    uint8_t  n_channels;     /* number of channels to scan */
    uint16_t max_out_of_time; /* max time away from serving channel */
    uint16_t suspend_time;
    uint32_t scan_start_tsf;
    /* Channel list follows (4 bytes per channel: channel | band<<16) */
    /* Then probe request template */
} __attribute__((packed));

/* TX command header for sending management frames */
struct iwl_tx_cmd {
    uint16_t len;             /* MPDU length */
    uint16_t next_frame_len;
    uint32_t tx_flags;        /* TX flags */
    uint32_t rate_n_flags;    /* rate and modulation flags */
    uint8_t  sta_id;          /* station ID */
    uint8_t  sec_ctl;         /* security control */
    uint8_t  initial_rate_idx;
    uint8_t  reserved;
    uint8_t  key[16];         /* encryption key */
    uint16_t next_frame_flags;
    uint16_t reserved2;
    uint32_t life_time;       /* frame lifetime */
    uint32_t dram_lsb_ptr;
    uint8_t  dram_msb_ptr;
    uint8_t  rts_retry_limit;
    uint8_t  data_retry_limit;
    uint8_t  tid_tspec;
    uint16_t pm_frame_timeout;
    uint16_t reserved3;
    /* 802.11 frame follows */
} __attribute__((packed));

/* TX flags */
#define IWL_TX_FLAGS_CMD_RATE   (1u << 0)  /* use rate_n_flags field */
#define IWL_TX_FLAGS_ACK        (1u << 3)  /* expect ACK */

/* Rate flags for 1 Mbps CCK (basic rate for management frames) */
#define IWL_RATE_1M_CCK         0x0000000Au

/* ---- External API ---- */

/* Initialize 802.11 MAC layer (called after host command interface is ready) */
int iwl_mac_init(const uint8_t mac_addr[6]);

/* Perform a scan and populate results */
int iwl_scan(struct iwl_bss_entry *results, uint32_t max_results, uint32_t *count);

/* Associate with a BSS (open network) */
int iwl_associate(const struct iwl_bss_entry *bss, const uint8_t mac_addr[6]);

/* Associate with a WPA2 BSS (includes RSN IE in assoc request) */
int iwl_associate_wpa2(const struct iwl_bss_entry *bss, const uint8_t mac_addr[6]);

/* Send a raw management frame via TX_CMD */
int iwl_tx_mgmt(const uint8_t *frame, uint16_t frame_len);

/* Send an EAPOL frame as an 802.11 data frame via TX_CMD.
 * Wraps the EAPOL body in LLC/SNAP + 802.11 data header.
 * Used during the WPA2 four-way handshake.
 */
int iwl_tx_eapol(const uint8_t *da, const uint8_t *sa, const uint8_t *bssid,
                  const uint8_t *eapol_body, uint16_t eapol_len);

#endif
