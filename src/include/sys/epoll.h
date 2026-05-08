/*
 * TaterTOS64v3 — <sys/epoll.h>
 *
 * POSIX/Linux-compatible epoll declarations backed by TaterTOS syscalls
 * SYS_EPOLL_CREATE, SYS_EPOLL_CTL, and SYS_EPOLL_WAIT.
 */

#ifndef _TATERTOS_SYS_EPOLL_H
#define _TATERTOS_SYS_EPOLL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union epoll_data {
    void     *ptr;
    int       fd;
    uint32_t  u32;
    uint64_t  u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
};

#define EPOLLIN        0x00000001u
#define EPOLLPRI       0x00000002u
#define EPOLLOUT       0x00000004u
#define EPOLLERR       0x00000008u
#define EPOLLHUP       0x00000010u
#define EPOLLNVAL      0x00000020u
#define EPOLLRDNORM    0x00000040u
#define EPOLLRDBAND    0x00000080u
#define EPOLLWRNORM    0x00000100u
#define EPOLLWRBAND    0x00000200u
#define EPOLLMSG       0x00000400u
#define EPOLLRDHUP     0x00002000u
#define EPOLLONESHOT   0x40000000u
#define EPOLLET        0x80000000u

#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_DEL  2
#define EPOLL_CTL_MOD  3

#define EPOLL_CLOEXEC  0x80000

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

#ifdef __cplusplus
}
#endif

#endif
