/*
 * TaterTOS64v3 — <sys/socket.h>
 *
 * POSIX BSD socket API.
 */

#ifndef _TATERTOS_SYS_SOCKET_H
#define _TATERTOS_SYS_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Socket Types and Constants
 * ----------------------------------------------------------------------- */

typedef uint32_t  socklen_t;
typedef uint16_t  sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    uint8_t     sa_data[14];
};

#ifndef AF_INET
#  define AF_INET     2
#endif
#define AF_UNSPEC     0
#define AF_UNIX       1
#define AF_INET6     10

#ifndef SOCK_STREAM
#  define SOCK_STREAM 1
#  define SOCK_DGRAM  2
#  define SOCK_RAW    3
#endif

#ifndef SOCK_NONBLOCK
#  define SOCK_NONBLOCK 0x800
#  define SOCK_CLOEXEC  0x80000
#endif

#ifndef IPPROTO_TCP
#  define IPPROTO_TCP 6
#  define IPPROTO_UDP 17
#endif

#define SOL_SOCKET    1

#define SO_REUSEADDR  2
#define SO_KEEPALIVE  9
#define SO_BROADCAST  6
#define SO_LINGER     13
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_ERROR      4
#define SO_TYPE       3

#define MSG_DONTWAIT  0x40
#define MSG_NOSIGNAL  0x4000
#define MSG_PEEK      0x02
#define MSG_WAITALL   0x100
#define MSG_TRUNC     0x20
#define MSG_CTRUNC    0x08

#define SCM_RIGHTS    1

struct msghdr {
    void         *msg_name;
    socklen_t    msg_namelen;
    struct iovec *msg_iov;
    size_t       msg_iovlen;
    void         *msg_control;
    size_t       msg_controllen;
    int          msg_flags;
};

struct cmsghdr {
    size_t cmsg_len;
    int    cmsg_level;
    int    cmsg_type;
};

#define CMSG_ALIGN(len)   (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE(len)   (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len)     (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_DATA(cmsg)   ((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(msg) \
    (((msg)->msg_controllen >= sizeof(struct cmsghdr)) ? \
     (struct cmsghdr *)((msg)->msg_control) : (struct cmsghdr *)0)
#define CMSG_NXTHDR(msg, cmsg) \
    ((((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) + sizeof(struct cmsghdr)) > \
      ((unsigned char *)(msg)->msg_control + (msg)->msg_controllen)) ? \
     (struct cmsghdr *)0 : \
     (struct cmsghdr *)((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)))

/* -----------------------------------------------------------------------
 * Socket API
 * ----------------------------------------------------------------------- */

int     socket(int domain, int type, int protocol);
int     bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int     connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest, socklen_t addrlen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src, socklen_t *addrlen);
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
int     shutdown(int fd, int how);
int     getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int     setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int     getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
int     getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);
int     socketpair(int domain, int type, int protocol, int sv[2]);
int     accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags);

uint16_t htons(uint16_t hostshort);
uint16_t ntohs(uint16_t netshort);
uint32_t htonl(uint32_t hostlong);
uint32_t ntohl(uint32_t netlong);

#ifdef __cplusplus
}
#endif

#endif
