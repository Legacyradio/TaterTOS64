#ifndef TATER_NETCORE_H
#define TATER_NETCORE_H

#include <stdint.h>

/* ---- Network Configuration ---- */

struct net_config {
    uint32_t ip;           /* our IP (host byte order) */
    uint32_t netmask;      /* subnet mask */
    uint32_t gateway;      /* default gateway */
    uint32_t dns_server;   /* DNS server */
    uint8_t  mac[6];       /* our MAC address */
    uint8_t  configured;   /* 1 if DHCP succeeded */
};

/* ---- Core API ---- */

void netcore_init(void);

/* Get the current network configuration (after DHCP) */
const struct net_config *net_get_config(void);

/* Set the active interface MAC address. */
void net_set_mac(const uint8_t mac[6]);

/* Clear dynamic IP configuration while preserving the interface MAC. */
void net_reset_config(void);

/* Process incoming frames (call periodically or from timer) */
void net_poll(void);

/* ---- IP Helpers ---- */

uint32_t net_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void net_ip_str(uint32_t ip, char *buf, uint32_t buf_len);

/* ---- ARP ---- */

/* Resolve IP → MAC. Returns 0 on success (mac_out filled), -1 if pending.
 * Sends ARP request if not cached. Caller should retry after net_poll(). */
int arp_resolve(uint32_t ip, uint8_t mac_out[6]);

/* ---- UDP ---- */

/* UDP receive callback: called for each received datagram */
typedef void (*udp_rx_cb_t)(uint32_t src_ip, uint16_t src_port,
                             uint16_t dst_port,
                             const uint8_t *data, uint16_t len);

/* Bind a UDP port to a receive callback. Returns 0 on success. */
int udp_bind(uint16_t port, udp_rx_cb_t cb);

/* Send a UDP datagram. Returns 0 on success. */
int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
             const uint8_t *data, uint16_t len);

/* ---- DHCP ---- */

/* Run DHCP discovery. Blocks until IP is obtained or timeout.
 * Returns 0 on success. */
int dhcp_discover(void);

/* ---- TCP ---- */

#define TCP_MAX_CONNECTIONS 4
#define TCP_RX_BUF_SIZE     4096
#define TCP_TX_BUF_SIZE     4096

/* TCP connection handle */
typedef int tcp_conn_t;

/* Connect to a remote host. Returns connection handle or -1. */
tcp_conn_t tcp_connect(uint32_t dst_ip, uint16_t dst_port);

/* Send data on a TCP connection. Returns bytes sent or -1. */
int tcp_send(tcp_conn_t conn, const uint8_t *data, uint16_t len);

/* Receive data from a TCP connection. Returns bytes received or -1.
 * Non-blocking: returns 0 if no data available. */
int tcp_recv(tcp_conn_t conn, uint8_t *buf, uint16_t max_len);

/* Close a TCP connection. */
void tcp_close(tcp_conn_t conn);

/* Check if a TCP connection is established. */
int tcp_is_connected(tcp_conn_t conn);

/* ---- DNS ---- */

/* Resolve a hostname to an IP address.
 * Uses the DNS server from DHCP config.
 * Blocks until resolved or timeout.
 * Returns IP in host byte order, or 0 on failure. */
uint32_t dns_resolve(const char *hostname);

/* ---- WiFi Data Path (from step 9) ---- */

int wifi_tx_packet(const uint8_t *eth_frame, uint16_t eth_len);
int iwl_rx_poll(uint32_t timeout_ms);

typedef void (*wifi_rx_callback_t)(const uint8_t *data, uint16_t len);
void wifi_set_rx_callback(wifi_rx_callback_t cb);

int iwl_connect_wpa2(const char *ssid, const char *passphrase,
                      const uint8_t mac[6]);

#endif
