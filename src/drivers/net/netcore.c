// TaterTOS64v3 — Network Core: ARP, IPv4, ICMP, UDP, DHCP, TCP, DNS

#include <stdint.h>
#include "netcore.h"

void kprint(const char *fmt, ...);

/* NIC driver hooks — I219 wired (primary), WiFi (shelved fallback) */
int i219_is_ready(void);
int i219_tx_packet(const uint8_t *frame, uint16_t len);
int i219_rx_poll(uint32_t timeout_ms);
void i219_set_rx_callback(void (*cb)(const uint8_t *data, uint16_t len));

/* ---- Helpers ---- */

static void n_zero(void *p, uint32_t len) {
    uint8_t *d = (uint8_t *)p;
    for (uint32_t i = 0; i < len; i++) d[i] = 0;
}

static void n_copy(void *dst, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}


static uint32_t n_strlen(const char *s) {
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

/* Big-endian read/write */
static uint16_t n_be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t n_be32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 |
                                                   (uint32_t)p[2]<<8 | p[3]; }

static void n_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v>>8); p[1] = (uint8_t)v; }
static void n_put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
                                                p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }

/* Host ↔ Network byte order */
static uint32_t htonl(uint32_t h) { return (h>>24)|((h>>8)&0xFF00)|((h<<8)&0xFF0000)|(h<<24); }

uint32_t net_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

void net_ip_str(uint32_t ip, char *buf, uint32_t buf_len) {
    if (buf_len < 16) return;
    uint8_t a = (uint8_t)(ip >> 24), b = (uint8_t)(ip >> 16),
            c = (uint8_t)(ip >> 8), d = (uint8_t)(ip);
    /* Simple decimal conversion */
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = (i==0) ? a : (i==1) ? b : (i==2) ? c : d;
        if (v >= 100) buf[pos++] = '0' + v/100;
        if (v >= 10) buf[pos++] = '0' + (v/10)%10;
        buf[pos++] = '0' + v%10;
        if (i < 3) buf[pos++] = '.';
    }
    buf[pos] = '\0';
}

/* ---- Busy-wait timer (~1 us per iteration) ---- */
static void net_udelay(uint32_t us) {
    for (uint32_t i = 0; i < us; i++)
        __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
}

/* ---- Global State ---- */

static struct net_config g_net;
static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static uint16_t g_ip_id = 1; /* IP identification counter */

const struct net_config *net_get_config(void) { return &g_net; }

void net_set_mac(const uint8_t mac[6]) {
    if (!mac) return;
    n_copy(g_net.mac, mac, 6);
    kprint("NET: MAC address set to %02x:%02x:%02x:%02x:%02x:%02x\n",
           g_net.mac[0], g_net.mac[1], g_net.mac[2],
           g_net.mac[3], g_net.mac[4], g_net.mac[5]);
}

void net_reset_config(void) {
    g_net.ip = 0;
    g_net.netmask = 0;
    g_net.gateway = 0;
    g_net.dns_server = 0;
    g_net.configured = 0;
}

/* ================================================================
 * Internet Checksum (RFC 1071)
 * ================================================================ */

static uint16_t ip_checksum(const void *data, uint16_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)p[0] << 8 | p[1];
        p += 2; len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ================================================================
 * Ethernet Frame TX
 * ================================================================ */

/* Build and send an Ethernet frame.
 * dst_mac: destination MAC (6 bytes)
 * ethertype: in host byte order (0x0800=IPv4, 0x0806=ARP)
 * payload: frame payload
 * payload_len: payload length
 */
static int eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
                     const uint8_t *payload, uint16_t payload_len) {
    uint8_t frame[1518];
    if ((uint32_t)(14 + payload_len) > sizeof(frame)) return -1;

    n_copy(frame, dst_mac, 6);
    n_copy(frame + 6, g_net.mac, 6);
    n_put16(frame + 12, ethertype);
    n_copy(frame + 14, payload, payload_len);

    /* Try wired NIC first, fall back to WiFi */
    if (i219_is_ready())
        return i219_tx_packet(frame, 14 + payload_len);
    return wifi_tx_packet(frame, 14 + payload_len);
}

/* ================================================================
 * ARP (Address Resolution Protocol)
 * ================================================================ */

#define ARP_CACHE_SIZE  16
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;
    uint8_t  pending; /* ARP request sent, waiting for reply */
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];

/* Build and send an ARP packet */
static int arp_send(uint16_t op, const uint8_t *target_mac,
                     uint32_t sender_ip, uint32_t target_ip) {
    uint8_t pkt[28]; /* ARP packet = 28 bytes */
    n_put16(pkt + 0, 0x0001);  /* hardware type: Ethernet */
    n_put16(pkt + 2, 0x0800);  /* protocol type: IPv4 */
    pkt[4] = 6;                 /* hardware addr len */
    pkt[5] = 4;                 /* protocol addr len */
    n_put16(pkt + 6, op);

    n_copy(pkt + 8, g_net.mac, 6);        /* sender MAC */
    n_put32(pkt + 14, sender_ip);          /* sender IP (network order) */
    n_copy(pkt + 18, target_mac, 6);       /* target MAC */
    n_put32(pkt + 24, target_ip);          /* target IP (network order) */

    const uint8_t *dst = (op == ARP_OP_REQUEST) ? BROADCAST_MAC : target_mac;
    return eth_send(dst, 0x0806, pkt, 28);
}

static void arp_process(const uint8_t *pkt, uint16_t len) {
    if (len < 28) return;

    uint16_t op = n_be16(pkt + 6);
    uint32_t sender_ip = n_be32(pkt + 14);
    const uint8_t *sender_mac = pkt + 8;
    uint32_t target_ip = n_be32(pkt + 24);

    /* Update cache with sender info */
    int free_slot = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == sender_ip) {
            n_copy(arp_cache[i].mac, sender_mac, 6);
            arp_cache[i].pending = 0;
            return;
        }
        if (!arp_cache[i].valid && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        arp_cache[free_slot].ip = sender_ip;
        n_copy(arp_cache[free_slot].mac, sender_mac, 6);
        arp_cache[free_slot].valid = 1;
        arp_cache[free_slot].pending = 0;
    }

    /* Reply to ARP requests for our IP */
    if (op == ARP_OP_REQUEST && target_ip == g_net.ip && g_net.configured) {
        arp_send(ARP_OP_REPLY, sender_mac, htonl(g_net.ip), htonl(sender_ip));
    }
}

int arp_resolve(uint32_t ip, uint8_t mac_out[6]) {
    /* Broadcast? */
    if (ip == 0xFFFFFFFF || (g_net.configured && ip == (g_net.ip | ~g_net.netmask))) {
        n_copy(mac_out, BROADCAST_MAC, 6);
        return 0;
    }

    /* Check cache */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            n_copy(mac_out, arp_cache[i].mac, 6);
            return 0;
        }
    }

    /* Send ARP request */
    uint8_t zero_mac[6] = {0};
    arp_send(ARP_OP_REQUEST, zero_mac, htonl(g_net.ip), htonl(ip));
    return -1; /* pending */
}

/* ================================================================
 * IPv4
 * ================================================================ */

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

/* Forward declarations for protocol handlers */
static void icmp_process(uint32_t src_ip, const uint8_t *data, uint16_t len);
static void udp_process(uint32_t src_ip, const uint8_t *data, uint16_t len);
static void tcp_process(uint32_t src_ip, const uint8_t *data, uint16_t len);

/* Send an IP packet */
static int ip_send(uint32_t dst_ip, uint8_t protocol,
                    const uint8_t *payload, uint16_t payload_len) {
    uint8_t pkt[1500];
    uint16_t total_len = 20 + payload_len;
    if (total_len > sizeof(pkt)) return -1;

    /* IP header (20 bytes, no options) */
    pkt[0] = 0x45;                      /* version 4, IHL 5 */
    pkt[1] = 0x00;                      /* DSCP + ECN */
    n_put16(pkt + 2, total_len);        /* total length */
    n_put16(pkt + 4, g_ip_id++);        /* identification */
    n_put16(pkt + 6, 0x4000);           /* flags: DF, fragment offset 0 */
    pkt[8] = 64;                         /* TTL */
    pkt[9] = protocol;
    n_put16(pkt + 10, 0);               /* checksum placeholder */
    n_put32(pkt + 12, g_net.ip);         /* source IP (already host order) */
    n_put32(pkt + 16, dst_ip);           /* destination IP */

    /* Calculate IP header checksum */
    uint16_t cksum = ip_checksum(pkt, 20);
    n_put16(pkt + 10, cksum);

    /* Copy payload */
    n_copy(pkt + 20, payload, payload_len);

    /* Determine next-hop: if on same subnet → direct, else → gateway */
    uint32_t next_hop = dst_ip;
    if (g_net.configured && (dst_ip & g_net.netmask) != (g_net.ip & g_net.netmask)) {
        next_hop = g_net.gateway;
    }

    /* Special case: broadcast */
    if (dst_ip == 0xFFFFFFFF) {
        return eth_send(BROADCAST_MAC, 0x0800, pkt, total_len);
    }

    /* Resolve next-hop MAC via ARP */
    uint8_t dst_mac[6];
    if (arp_resolve(next_hop, dst_mac) != 0) {
        /* ARP pending — wait briefly and retry once */
        net_udelay(50000); /* 50 ms */
        net_poll();
        if (arp_resolve(next_hop, dst_mac) != 0) {
            /* Still pending — send anyway to broadcast (works for DHCP) */
            return eth_send(BROADCAST_MAC, 0x0800, pkt, total_len);
        }
    }

    return eth_send(dst_mac, 0x0800, pkt, total_len);
}

static void ip_process(const uint8_t *pkt, uint16_t len) {
    if (len < 20) return;

    uint8_t version = pkt[0] >> 4;
    uint8_t ihl = (pkt[0] & 0x0F) * 4;
    if (version != 4 || ihl < 20 || ihl > len) return;

    uint16_t total_len = n_be16(pkt + 2);
    if (total_len > len) total_len = len;
    if (total_len < ihl) return;

    uint8_t protocol = pkt[9];
    uint32_t src_ip = n_be32(pkt + 12);
    uint32_t dst_ip = n_be32(pkt + 16);

    /* Accept packets for our IP or broadcast */
    if (g_net.configured && dst_ip != g_net.ip &&
        dst_ip != 0xFFFFFFFF &&
        dst_ip != (g_net.ip | ~g_net.netmask)) {
        return; /* not for us */
    }

    const uint8_t *payload = pkt + ihl;
    uint16_t payload_len = total_len - ihl;

    switch (protocol) {
    case IP_PROTO_ICMP: icmp_process(src_ip, payload, payload_len); break;
    case IP_PROTO_UDP:  udp_process(src_ip, payload, payload_len); break;
    case IP_PROTO_TCP:  tcp_process(src_ip, payload, payload_len); break;
    }
}

/* ================================================================
 * ICMP (Echo Reply for ping)
 * ================================================================ */

static void icmp_process(uint32_t src_ip, const uint8_t *data, uint16_t len) {
    if (len < 8) return;
    uint8_t type = data[0];

    /* Echo Request (ping) → reply */
    if (type == 8 && g_net.configured) {
        uint8_t reply[1480];
        if (len > sizeof(reply)) return;
        n_copy(reply, data, len);
        reply[0] = 0; /* type = Echo Reply */
        reply[2] = 0; reply[3] = 0; /* clear checksum */
        uint16_t cksum = ip_checksum(reply, len);
        n_put16(reply + 2, cksum);
        ip_send(src_ip, IP_PROTO_ICMP, reply, len);
    }
}

/* ================================================================
 * UDP
 * ================================================================ */

#define UDP_MAX_BINDS   8

struct udp_binding {
    uint16_t port;
    udp_rx_cb_t cb;
    uint8_t active;
};

static struct udp_binding udp_bindings[UDP_MAX_BINDS];
static uint16_t udp_ephemeral = 49152; /* ephemeral port counter */

int udp_bind(uint16_t port, udp_rx_cb_t cb) {
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (!udp_bindings[i].active) {
            udp_bindings[i].port = port;
            udp_bindings[i].cb = cb;
            udp_bindings[i].active = 1;
            return 0;
        }
    }
    return -1;
}

int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
             const uint8_t *data, uint16_t len) {
    uint8_t pkt[1480];
    uint16_t udp_len = 8 + len;
    if (udp_len > sizeof(pkt)) return -1;

    /* UDP header */
    n_put16(pkt + 0, src_port);
    n_put16(pkt + 2, dst_port);
    n_put16(pkt + 4, udp_len);
    n_put16(pkt + 6, 0); /* checksum (optional for IPv4) */
    n_copy(pkt + 8, data, len);

    return ip_send(dst_ip, IP_PROTO_UDP, pkt, udp_len);
}

static void udp_process(uint32_t src_ip, const uint8_t *data, uint16_t len) {
    if (len < 8) return;

    uint16_t src_port = n_be16(data + 0);
    uint16_t dst_port = n_be16(data + 2);
    uint16_t udp_len = n_be16(data + 4);
    if (udp_len > len) udp_len = len;
    if (udp_len < 8) return;

    const uint8_t *payload = data + 8;
    uint16_t payload_len = udp_len - 8;

    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (udp_bindings[i].active && udp_bindings[i].port == dst_port) {
            udp_bindings[i].cb(src_ip, src_port, dst_port, payload, payload_len);
            return;
        }
    }
}

/* ================================================================
 * DHCP
 * ================================================================ */

#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68
#define DHCP_MAGIC_COOKIE   0x63825363

#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_ACK        5

/* DHCP state */
static uint32_t dhcp_xid;
static uint32_t dhcp_offered_ip;
static uint32_t dhcp_server_ip;
static uint8_t  dhcp_state; /* 0=idle, 1=discover sent, 2=offer rcvd, 3=request sent, 4=done */

static void dhcp_build_packet(uint8_t *pkt, uint16_t *out_len, uint8_t msg_type) {
    n_zero(pkt, 576);

    pkt[0] = 1;        /* op: BOOTREQUEST */
    pkt[1] = 1;        /* htype: Ethernet */
    pkt[2] = 6;        /* hlen: 6 */
    pkt[3] = 0;        /* hops */
    n_put32(pkt + 4, dhcp_xid);    /* transaction ID */
    /* secs, flags: 0 */
    n_put16(pkt + 10, 0x8000);     /* flags: broadcast */
    /* ciaddr = 0 (we don't have an IP yet) */
    /* yiaddr, siaddr, giaddr = 0 */
    n_copy(pkt + 28, g_net.mac, 6); /* chaddr */
    /* sname, file: zero */

    /* Magic cookie at offset 236 */
    n_put32(pkt + 236, DHCP_MAGIC_COOKIE);

    /* Options start at 240 */
    uint16_t pos = 240;

    /* Option 53: DHCP Message Type */
    pkt[pos++] = 53; pkt[pos++] = 1; pkt[pos++] = msg_type;

    if (msg_type == DHCP_REQUEST) {
        /* Option 50: Requested IP Address */
        pkt[pos++] = 50; pkt[pos++] = 4;
        n_put32(pkt + pos, dhcp_offered_ip); pos += 4;

        /* Option 54: DHCP Server Identifier */
        pkt[pos++] = 54; pkt[pos++] = 4;
        n_put32(pkt + pos, dhcp_server_ip); pos += 4;
    }

    /* Option 55: Parameter Request List */
    pkt[pos++] = 55; pkt[pos++] = 3;
    pkt[pos++] = 1;   /* Subnet Mask */
    pkt[pos++] = 3;   /* Router */
    pkt[pos++] = 6;   /* DNS Server */

    /* End option */
    pkt[pos++] = 255;

    *out_len = pos;
    if (*out_len < 300) *out_len = 300; /* minimum DHCP packet size */
}

static void dhcp_rx(uint32_t src_ip, uint16_t src_port,
                     uint16_t dst_port, const uint8_t *data, uint16_t len) {
    (void)src_ip; (void)src_port; (void)dst_port;
    if (len < 240) return;

    /* Check magic cookie */
    if (n_be32(data + 236) != DHCP_MAGIC_COOKIE) return;

    /* Check transaction ID */
    if (n_be32(data + 4) != dhcp_xid) return;

    /* Parse options starting at 240 */
    uint8_t msg_type = 0;
    uint32_t subnet = 0, router = 0, dns = 0, server_id = 0;

    uint16_t pos = 240;
    while (pos < len && data[pos] != 255) {
        if (data[pos] == 0) { pos++; continue; } /* pad */
        uint8_t opt = data[pos++];
        if (pos >= len) break;
        uint8_t opt_len = data[pos++];
        if (pos + opt_len > len) break;

        switch (opt) {
        case 53: /* Message Type */
            if (opt_len >= 1) msg_type = data[pos];
            break;
        case 1: /* Subnet Mask */
            if (opt_len >= 4) subnet = n_be32(data + pos);
            break;
        case 3: /* Router */
            if (opt_len >= 4) router = n_be32(data + pos);
            break;
        case 6: /* DNS */
            if (opt_len >= 4) dns = n_be32(data + pos);
            break;
        case 54: /* Server Identifier */
            if (opt_len >= 4) server_id = n_be32(data + pos);
            break;
        }
        pos += opt_len;
    }

    uint32_t your_ip = n_be32(data + 16); /* yiaddr */

    if (msg_type == DHCP_OFFER && dhcp_state == 1) {
        dhcp_offered_ip = your_ip;
        dhcp_server_ip = server_id ? server_id : src_ip;
        dhcp_state = 2;

        char ip_buf[16];
        net_ip_str(your_ip, ip_buf, sizeof(ip_buf));
        kprint("NET: DHCP offer: %s\n", ip_buf);

        /* Send DHCP Request */
        uint8_t req[576];
        uint16_t req_len;
        dhcp_build_packet(req, &req_len, DHCP_REQUEST);
        udp_send(0xFFFFFFFF, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, req, req_len);
        dhcp_state = 3;
    }

    if (msg_type == DHCP_ACK && dhcp_state == 3) {
        g_net.ip = your_ip;
        g_net.netmask = subnet ? subnet : 0xFFFFFF00;
        g_net.gateway = router;
        g_net.dns_server = dns;
        g_net.configured = 1;
        dhcp_state = 4;

        char ip_buf[16], gw_buf[16], dns_buf[16];
        net_ip_str(g_net.ip, ip_buf, sizeof(ip_buf));
        net_ip_str(g_net.gateway, gw_buf, sizeof(gw_buf));
        net_ip_str(g_net.dns_server, dns_buf, sizeof(dns_buf));
        kprint("NET: DHCP ACK: ip=%s gw=%s dns=%s\n", ip_buf, gw_buf, dns_buf);
    }
}

int dhcp_discover(void) {
    /* Bind DHCP client port */
    udp_bind(DHCP_CLIENT_PORT, dhcp_rx);

    /* Generate transaction ID from simple counter */
    static uint32_t xid_counter = 0x54415445; /* "TATE" */
    dhcp_xid = xid_counter++;

    /* Send DHCP Discover */
    uint8_t disc[576];
    uint16_t disc_len;
    dhcp_build_packet(disc, &disc_len, DHCP_DISCOVER);
    dhcp_state = 1;

    kprint("NET: sending DHCP discover...\n");
    udp_send(0xFFFFFFFF, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, disc, disc_len);

    /* Wait for DHCP to complete (up to 10 seconds, 3 retries) */
    for (int retry = 0; retry < 3; retry++) {
        uint32_t elapsed = 0;
        while (elapsed < 3000000) { /* 3 seconds per attempt */
            net_poll();
            if (dhcp_state == 4) return 0; /* success */
            net_udelay(10000); /* 10 ms */
            elapsed += 10000;
        }

        if (dhcp_state < 4) {
            kprint("NET: DHCP retry %d...\n", retry + 1);
            dhcp_build_packet(disc, &disc_len, DHCP_DISCOVER);
            dhcp_state = 1;
            udp_send(0xFFFFFFFF, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, disc, disc_len);
        }
    }

    kprint("NET: DHCP failed\n");
    return -1;
}

/* ================================================================
 * TCP (Transmission Control Protocol)
 * ================================================================ */

/* TCP states */
#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_FIN_WAIT_1  3
#define TCP_FIN_WAIT_2  4
#define TCP_CLOSE_WAIT  5
#define TCP_LAST_ACK    6
#define TCP_TIME_WAIT   7

/* TCP flags */
#define TCP_FIN     0x01
#define TCP_SYN     0x02
#define TCP_RST     0x04
#define TCP_PSH     0x08
#define TCP_ACK     0x10

struct tcp_conn {
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t seq_num;       /* our sequence number */
    uint32_t ack_num;       /* expected next byte from remote */
    uint8_t  state;
    uint8_t  active;

    /* Receive buffer */
    uint8_t  rx_buf[TCP_RX_BUF_SIZE];
    uint16_t rx_head;
    uint16_t rx_tail;

    /* Retransmit state */
    uint32_t last_ack_sent;
    uint32_t retransmit_timer;
};

static struct tcp_conn tcp_conns[TCP_MAX_CONNECTIONS];
static uint16_t tcp_next_port = 50000;

/* Simple ISN (Initial Sequence Number) from a counter */
static uint32_t tcp_isn(void) {
    static uint32_t isn_counter = 0x12345678;
    isn_counter += 64000;
    return isn_counter;
}

/* Build and send a TCP segment */
static int tcp_send_segment(struct tcp_conn *c, uint8_t flags,
                              const uint8_t *data, uint16_t data_len) {
    uint8_t pkt[1480];
    uint16_t tcp_hdr_len = 20;
    uint16_t total = tcp_hdr_len + data_len;
    if (total > sizeof(pkt)) return -1;

    n_zero(pkt, tcp_hdr_len);
    n_put16(pkt + 0, c->local_port);     /* src port */
    n_put16(pkt + 2, c->remote_port);    /* dst port */
    n_put32(pkt + 4, c->seq_num);         /* sequence number */
    n_put32(pkt + 8, c->ack_num);         /* ack number */
    pkt[12] = (uint8_t)((tcp_hdr_len / 4) << 4); /* data offset */
    pkt[13] = flags;
    n_put16(pkt + 14, 8192);              /* window size */
    n_put16(pkt + 16, 0);                 /* checksum placeholder */
    n_put16(pkt + 18, 0);                 /* urgent pointer */

    if (data_len > 0) n_copy(pkt + tcp_hdr_len, data, data_len);

    /* TCP pseudo-header checksum */
    uint8_t pseudo[12];
    n_put32(pseudo + 0, g_net.ip);
    n_put32(pseudo + 4, c->remote_ip);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    n_put16(pseudo + 10, total);

    uint32_t sum = 0;
    /* Sum pseudo-header */
    for (int i = 0; i < 12; i += 2)
        sum += (uint32_t)pseudo[i] << 8 | pseudo[i+1];
    /* Sum TCP segment */
    for (uint16_t i = 0; i < total; i += 2) {
        uint16_t word = (uint16_t)pkt[i] << 8;
        if (i + 1 < total) word |= pkt[i + 1];
        sum += word;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    n_put16(pkt + 16, (uint16_t)(~sum));

    return ip_send(c->remote_ip, IP_PROTO_TCP, pkt, total);
}

tcp_conn_t tcp_connect(uint32_t dst_ip, uint16_t dst_port) {
    /* Find a free connection slot */
    int slot = -1;
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!tcp_conns[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    struct tcp_conn *c = &tcp_conns[slot];
    n_zero(c, sizeof(*c));
    c->remote_ip = dst_ip;
    c->remote_port = dst_port;
    c->local_port = tcp_next_port++;
    c->seq_num = tcp_isn();
    c->state = TCP_SYN_SENT;
    c->active = 1;

    /* Send SYN */
    tcp_send_segment(c, TCP_SYN, 0, 0);
    c->seq_num++; /* SYN consumes one sequence number */

    kprint("NET: TCP connect %u → %u (SYN sent)\n", c->local_port, dst_port);

    /* Wait for SYN-ACK (up to 5 seconds) */
    uint32_t elapsed = 0;
    while (elapsed < 5000000 && c->state == TCP_SYN_SENT) {
        net_poll();
        net_udelay(5000);
        elapsed += 5000;
    }

    if (c->state == TCP_ESTABLISHED) {
        kprint("NET: TCP connected\n");
        return slot;
    }

    kprint("NET: TCP connect timeout\n");
    c->active = 0;
    c->state = TCP_CLOSED;
    return -1;
}

int tcp_send(tcp_conn_t conn, const uint8_t *data, uint16_t len) {
    if (conn < 0 || conn >= TCP_MAX_CONNECTIONS) return -1;
    struct tcp_conn *c = &tcp_conns[conn];
    if (c->state != TCP_ESTABLISHED) return -1;

    /* Send in segments of max 1400 bytes */
    uint16_t sent = 0;
    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > 1400) chunk = 1400;

        tcp_send_segment(c, TCP_ACK | TCP_PSH, data + sent, chunk);
        c->seq_num += chunk;
        sent += chunk;
    }

    return sent;
}

int tcp_recv(tcp_conn_t conn, uint8_t *buf, uint16_t max_len) {
    if (conn < 0 || conn >= TCP_MAX_CONNECTIONS) return -1;
    struct tcp_conn *c = &tcp_conns[conn];

    uint16_t available = 0;
    if (c->rx_head >= c->rx_tail)
        available = c->rx_head - c->rx_tail;
    else
        available = TCP_RX_BUF_SIZE - c->rx_tail + c->rx_head;

    if (available == 0) return 0;

    uint16_t to_read = available;
    if (to_read > max_len) to_read = max_len;

    for (uint16_t i = 0; i < to_read; i++) {
        buf[i] = c->rx_buf[c->rx_tail];
        c->rx_tail = (c->rx_tail + 1) % TCP_RX_BUF_SIZE;
    }

    return to_read;
}

void tcp_close(tcp_conn_t conn) {
    if (conn < 0 || conn >= TCP_MAX_CONNECTIONS) return;
    struct tcp_conn *c = &tcp_conns[conn];
    if (c->state == TCP_ESTABLISHED) {
        tcp_send_segment(c, TCP_FIN | TCP_ACK, 0, 0);
        c->seq_num++;
        c->state = TCP_FIN_WAIT_1;
    } else {
        c->state = TCP_CLOSED;
        c->active = 0;
    }
}

int tcp_is_connected(tcp_conn_t conn) {
    if (conn < 0 || conn >= TCP_MAX_CONNECTIONS) return 0;
    return tcp_conns[conn].state == TCP_ESTABLISHED;
}

static void tcp_process(uint32_t src_ip, const uint8_t *data, uint16_t len) {
    if (len < 20) return;

    uint16_t src_port = n_be16(data + 0);
    uint16_t dst_port = n_be16(data + 2);
    uint32_t seq = n_be32(data + 4);
    uint32_t ack = n_be32(data + 8);
    uint8_t data_off = (data[12] >> 4) * 4;
    uint8_t flags = data[13];

    if (data_off > len) return;

    const uint8_t *payload = data + data_off;
    uint16_t payload_len = len - data_off;

    /* Find matching connection */
    struct tcp_conn *c = 0;
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_conns[i].active &&
            tcp_conns[i].remote_ip == src_ip &&
            tcp_conns[i].remote_port == src_port &&
            tcp_conns[i].local_port == dst_port) {
            c = &tcp_conns[i];
            break;
        }
    }

    if (!c) {
        /* Send RST for unexpected segments */
        if (!(flags & TCP_RST)) {
            struct tcp_conn dummy;
            n_zero(&dummy, sizeof(dummy));
            dummy.remote_ip = src_ip;
            dummy.remote_port = src_port;
            dummy.local_port = dst_port;
            dummy.seq_num = ack;
            dummy.ack_num = seq + 1;
            tcp_send_segment(&dummy, TCP_RST | TCP_ACK, 0, 0);
        }
        return;
    }

    if (flags & TCP_RST) {
        c->state = TCP_CLOSED;
        c->active = 0;
        return;
    }

    switch (c->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            c->ack_num = seq + 1;
            c->state = TCP_ESTABLISHED;
            tcp_send_segment(c, TCP_ACK, 0, 0);
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_FIN) {
            c->ack_num = seq + payload_len + 1;
            tcp_send_segment(c, TCP_ACK, 0, 0);
            c->state = TCP_CLOSE_WAIT;
            /* Auto-close our end too */
            tcp_send_segment(c, TCP_FIN | TCP_ACK, 0, 0);
            c->seq_num++;
            c->state = TCP_LAST_ACK;
        } else {
            /* Data received */
            if (payload_len > 0) {
                for (uint16_t i = 0; i < payload_len; i++) {
                    uint16_t next = (c->rx_head + 1) % TCP_RX_BUF_SIZE;
                    if (next == c->rx_tail) break; /* buffer full */
                    c->rx_buf[c->rx_head] = payload[i];
                    c->rx_head = next;
                }
                c->ack_num = seq + payload_len;
                tcp_send_segment(c, TCP_ACK, 0, 0);
            }
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_ACK) {
            if (flags & TCP_FIN) {
                c->ack_num = seq + 1;
                tcp_send_segment(c, TCP_ACK, 0, 0);
                c->state = TCP_CLOSED;
                c->active = 0;
            } else {
                c->state = TCP_FIN_WAIT_2;
            }
        }
        break;

    case TCP_FIN_WAIT_2:
        if (flags & TCP_FIN) {
            c->ack_num = seq + 1;
            tcp_send_segment(c, TCP_ACK, 0, 0);
            c->state = TCP_CLOSED;
            c->active = 0;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) {
            c->state = TCP_CLOSED;
            c->active = 0;
        }
        break;
    }
}

/* ================================================================
 * DNS (Domain Name System)
 * ================================================================ */

static uint32_t dns_result_ip;
static uint8_t  dns_resolved;
static uint16_t dns_txid;

static void dns_rx(uint32_t src_ip, uint16_t src_port,
                    uint16_t dst_port, const uint8_t *data, uint16_t len) {
    (void)src_ip; (void)src_port; (void)dst_port;
    if (len < 12) return;

    uint16_t txid = n_be16(data + 0);
    if (txid != dns_txid) return;

    uint16_t flags = n_be16(data + 2);
    if (!(flags & 0x8000)) return; /* not a response */
    if ((flags & 0x000F) != 0) return; /* error */

    uint16_t qdcount = n_be16(data + 4);
    uint16_t ancount = n_be16(data + 6);
    if (ancount == 0) return;

    /* Skip question section */
    uint16_t pos = 12;
    for (uint16_t q = 0; q < qdcount; q++) {
        /* Skip name */
        while (pos < len && data[pos] != 0) {
            if ((data[pos] & 0xC0) == 0xC0) { pos += 2; goto q_done; }
            pos += 1 + data[pos];
        }
        pos++; /* skip zero terminator */
    q_done:
        pos += 4; /* skip type + class */
    }

    /* Parse first A record answer */
    for (uint16_t a = 0; a < ancount && pos + 12 <= len; a++) {
        /* Skip name (may be pointer) */
        if ((data[pos] & 0xC0) == 0xC0) {
            pos += 2;
        } else {
            while (pos < len && data[pos] != 0) pos += 1 + data[pos];
            pos++;
        }

        if (pos + 10 > len) break;
        uint16_t rtype = n_be16(data + pos);
        uint16_t rdlen = n_be16(data + pos + 8);
        pos += 10;

        if (rtype == 1 && rdlen == 4 && pos + 4 <= len) {
            /* A record: 4-byte IPv4 address */
            dns_result_ip = n_be32(data + pos);
            dns_resolved = 1;
            return;
        }
        pos += rdlen;
    }
}

uint32_t dns_resolve(const char *hostname) {
    if (!g_net.configured || !g_net.dns_server) return 0;

    /* Bind DNS response port */
    uint16_t dns_port = udp_ephemeral++;
    udp_bind(dns_port, dns_rx);
    dns_resolved = 0;
    dns_result_ip = 0;
    dns_txid = (uint16_t)(dns_port ^ 0x1234);

    /* Build DNS query */
    uint8_t pkt[256];
    n_zero(pkt, sizeof(pkt));

    /* Header */
    n_put16(pkt + 0, dns_txid); /* transaction ID */
    n_put16(pkt + 2, 0x0100);   /* flags: standard query, recursion desired */
    n_put16(pkt + 4, 1);        /* questions: 1 */
    n_put16(pkt + 6, 0);        /* answers: 0 */

    /* Question: encode hostname as DNS name */
    uint16_t pos = 12;
    uint32_t hlen = n_strlen(hostname);
    uint32_t label_start = pos;
    pos++; /* skip length byte (fill in later) */
    uint32_t label_len = 0;

    for (uint32_t i = 0; i <= hlen; i++) {
        if (i == hlen || hostname[i] == '.') {
            pkt[label_start] = (uint8_t)label_len;
            if (i < hlen) {
                label_start = pos;
                pos++;
                label_len = 0;
            }
        } else {
            pkt[pos++] = (uint8_t)hostname[i];
            label_len++;
        }
    }
    pkt[pos++] = 0; /* root label */

    /* QTYPE: A (1), QCLASS: IN (1) */
    n_put16(pkt + pos, 1); pos += 2;
    n_put16(pkt + pos, 1); pos += 2;

    kprint("NET: DNS query for \"%s\"\n", hostname);
    udp_send(g_net.dns_server, 53, dns_port, pkt, pos);

    /* Wait for response (up to 5 seconds, 2 retries) */
    for (int retry = 0; retry < 3; retry++) {
        uint32_t elapsed = 0;
        while (elapsed < 2000000) {
            net_poll();
            if (dns_resolved) {
                /* Unbind port */
                for (int i = 0; i < UDP_MAX_BINDS; i++) {
                    if (udp_bindings[i].active && udp_bindings[i].port == dns_port) {
                        udp_bindings[i].active = 0;
                        break;
                    }
                }
                char ip_buf[16];
                net_ip_str(dns_result_ip, ip_buf, sizeof(ip_buf));
                kprint("NET: DNS resolved \"%s\" → %s\n", hostname, ip_buf);
                return dns_result_ip;
            }
            net_udelay(5000);
            elapsed += 5000;
        }
        if (!dns_resolved && retry < 2) {
            udp_send(g_net.dns_server, 53, dns_port, pkt, pos);
        }
    }

    /* Unbind port on failure */
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (udp_bindings[i].active && udp_bindings[i].port == dns_port) {
            udp_bindings[i].active = 0;
            break;
        }
    }

    kprint("NET: DNS failed for \"%s\"\n", hostname);
    return 0;
}

/* ================================================================
 * Ethernet RX Dispatcher
 * ================================================================ */

static void net_rx(const uint8_t *data, uint16_t len) {
    if (len < 14) return;

    uint16_t ethertype = (uint16_t)((uint16_t)data[12] << 8 | data[13]);
    const uint8_t *payload = data + 14;
    uint16_t payload_len = len - 14;

    switch (ethertype) {
    case 0x0806: arp_process(payload, payload_len); break;
    case 0x0800: ip_process(payload, payload_len); break;
    }
}

/* ================================================================
 * Initialization + Polling
 * ================================================================ */

void netcore_init(void) {
    n_zero(&g_net, sizeof(g_net));
    n_zero(arp_cache, sizeof(arp_cache));
    n_zero(udp_bindings, sizeof(udp_bindings));
    n_zero(tcp_conns, sizeof(tcp_conns));

    /* RX callback registered after NIC driver init.
     * netcore_attach_i219() is called from i219_init() once HW is up. */

    kprint("NET: network stack initialized\n");
}

/* Called by i219_init() after HW is ready */
void netcore_attach_i219(void) {
    i219_set_rx_callback(net_rx);
    kprint("NET: attached I219 wired Ethernet\n");
}

void net_poll(void) {
    if (i219_is_ready()) {
        i219_rx_poll(0);
        return;
    }
    iwl_rx_poll(0);
}
