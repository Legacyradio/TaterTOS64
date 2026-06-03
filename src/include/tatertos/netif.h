/*
 * TaterTOS64v3 network interface query ABI.
 *
 * This is the native userspace view of the kernel netcore state. It is not
 * Linux ifaddrs/ioctl ABI; ports should use this when they need interface
 * addresses on TaterTOS.
 */

#ifndef TATERTOS_NETIF_H
#define TATERTOS_NETIF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TATERTOS_NETIF_MAX 8u
#define TATERTOS_NETIF_NAME_MAX 16u

#define TATERTOS_NETIF_FLAG_UP       0x00000001u
#define TATERTOS_NETIF_FLAG_RUNNING  0x00000002u
#define TATERTOS_NETIF_FLAG_LOOPBACK 0x00000004u

#define TATERTOS_NETIF_TYPE_UNKNOWN  0u
#define TATERTOS_NETIF_TYPE_LOOPBACK 1u
#define TATERTOS_NETIF_TYPE_ETHERNET 2u
#define TATERTOS_NETIF_TYPE_WIFI     3u

struct tatertos_netif {
    char     name[TATERTOS_NETIF_NAME_MAX];
    uint32_t flags;
    uint32_t type;
    uint32_t ipv4;       /* host byte order, 0 when no IPv4 address */
    uint32_t netmask;    /* host byte order, 0 when unknown */
    uint32_t gateway;    /* host byte order, 0 when none */
    uint32_t dns_server; /* host byte order, 0 when none */
    uint8_t  mac[6];
    uint8_t  pad[2];
};

int tatertos_netif_list(struct tatertos_netif *entries,
                        uint32_t max_entries,
                        uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* TATERTOS_NETIF_H */
