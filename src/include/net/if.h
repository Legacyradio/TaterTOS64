#ifndef _TATERTOS_NET_IF_H
#define _TATERTOS_NET_IF_H

/*
 * TaterTOS64v3 POSIX network interface name/index API.
 *
 * Backed by the native tatertos_netif_list() ABI rather than Linux netlink or
 * ioctl state.
 */

#include <tatertos/netif.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IF_NAMESIZE TATERTOS_NETIF_NAME_MAX

#define IFF_UP       TATERTOS_NETIF_FLAG_UP
#define IFF_RUNNING  TATERTOS_NETIF_FLAG_RUNNING
#define IFF_LOOPBACK TATERTOS_NETIF_FLAG_LOOPBACK

unsigned int if_nametoindex(const char *ifname);
char *if_indextoname(unsigned int ifindex, char *ifname);

#ifdef __cplusplus
}
#endif

#endif /* _TATERTOS_NET_IF_H */
