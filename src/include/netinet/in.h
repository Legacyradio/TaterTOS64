/*
 * TaterTOS64v3 — <netinet/in.h>
 *
 * POSIX IP-specific network definitions.
 */

#ifndef _TATERTOS_NETINET_IN_H
#define _TATERTOS_NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    uint8_t        sin_zero[8];
};

#ifndef IPPROTO_TCP
#  define IPPROTO_TCP 6
#  define IPPROTO_UDP 17
#endif
#define IPPROTO_IP    0
#define IPPROTO_ICMP  1
#define IPPROTO_RAW   255

#define INADDR_ANY       0x00000000
#define INADDR_LOOPBACK  0x7F000001
#define INADDR_NONE      0xFFFFFFFF

#ifdef __cplusplus
}
#endif

#endif
