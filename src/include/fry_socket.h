/*
 * fry_socket.h — TaterTOS64v3 BSD-socket-style constants and types
 *
 * Used by both kernel (syscall.c) and userspace (libc).
 */

#ifndef _TATERTOS_FRY_SOCKET_H
#define _TATERTOS_FRY_SOCKET_H

#include <stdint.h>

/* Address families */
#define AF_INET   2

/* Socket types */
#define SOCK_STREAM  1   /* TCP */
#define SOCK_DGRAM   2   /* UDP */

/* Protocols (for socket()) */
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

/* shutdown() how */
#define SHUT_RD    0
#define SHUT_WR    1
#define SHUT_RDWR  2

/* Socket options (getsockopt / setsockopt) */
#define SOL_SOCKET       1
#define SO_REUSEADDR     2
#define SO_RCVTIMEO      20
#define SO_SNDTIMEO      21
#define SO_ERROR         4
#define SO_KEEPALIVE     9

/* send/recv flags */
#define MSG_DONTWAIT  0x40

/* sockaddr_in — IPv4 socket address (matches POSIX layout) */
struct fry_sockaddr_in {
    uint16_t sin_family;      /* AF_INET */
    uint16_t sin_port;        /* port in network byte order (big-endian) */
    uint32_t sin_addr;        /* IPv4 address in network byte order */
    uint8_t  sin_zero[8];     /* padding to 16 bytes */
};

/* Generic sockaddr for API compatibility */
struct fry_sockaddr {
    uint16_t sa_family;
    uint8_t  sa_data[14];
};

/* Byte order helpers */
static inline uint16_t fry_htons(uint16_t h) {
    return (uint16_t)((h >> 8) | (h << 8));
}

static inline uint16_t fry_ntohs(uint16_t n) {
    return (uint16_t)((n >> 8) | (n << 8));
}

static inline uint32_t fry_htonl(uint32_t h) {
    return (h >> 24) | ((h >> 8) & 0xFF00) |
           ((h << 8) & 0xFF0000) | (h << 24);
}

static inline uint32_t fry_ntohl(uint32_t n) {
    return (n >> 24) | ((n >> 8) & 0xFF00) |
           ((n << 8) & 0xFF0000) | (n << 24);
}

#endif /* _TATERTOS_FRY_SOCKET_H */
