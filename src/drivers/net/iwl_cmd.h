#ifndef TATER_IWL_CMD_H
#define TATER_IWL_CMD_H

#include <stdint.h>

/*
 * Device RX packets start with a 32-bit len/flags field, then a 4-byte
 * firmware header (cmd_id, group_id, sequence), then the payload.
 */
#define IWL_RX_PACKET_FRAME_SIZE_MASK 0x00003FFFu

struct iwl_rx_packet {
    uint32_t len_n_flags;
    uint8_t  cmd_id;
    uint8_t  group_id;
    uint16_t sequence;
    uint8_t  data[];
} __attribute__((packed));

static inline uint32_t iwl_rx_packet_len(const struct iwl_rx_packet *pkt) {
    return pkt ? (pkt->len_n_flags & IWL_RX_PACKET_FRAME_SIZE_MASK) : 0u;
}

static inline uint32_t iwl_rx_packet_payload_len(const struct iwl_rx_packet *pkt) {
    uint32_t len = iwl_rx_packet_len(pkt);
    return len > 4u ? (len - 4u) : 0u;
}

static inline uint32_t iwl_rx_packet_total_len(const struct iwl_rx_packet *pkt) {
    uint32_t len = iwl_rx_packet_len(pkt);
    return len ? (len + 4u) : 0u;
}

/* Exposed from wifi_9260.c for use by iwl_mac80211.c */
int iwl_send_cmd_sync(uint8_t cmd_id, uint8_t group_id,
                       const void *data, uint16_t data_len,
                       void *resp_buf, uint32_t resp_max,
                       uint32_t timeout_ms);

int iwl_send_cmd(uint8_t cmd_id, uint8_t group_id,
                  const void *data, uint16_t data_len, int want_resp);

int iwl_wait_resp(uint16_t seq, uint32_t timeout_ms,
                   void *resp_buf, uint32_t resp_max);

#endif
