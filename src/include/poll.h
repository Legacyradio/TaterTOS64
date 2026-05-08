/*
 * TaterTOS64v3 — <poll.h>
 *
 * POSIX poll(). TaterTOS has fry_poll() in libc.h backed by the
 * kernel scheduler's poll-block primitive (sched_block_poll +
 * sched_wake_poll_waiters in src/kernel/proc/sched.c).
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_POLL_H
#define _TATERTOS_POLL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event/revents bits. Match Linux. */
#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020
#define POLLRDNORM  0x0040
#define POLLRDBAND  0x0080
#define POLLWRNORM  0x0100
#define POLLWRBAND  0x0200
#define POLLMSG     0x0400
#define POLLREMOVE  0x1000
#define POLLRDHUP   0x2000

/*
 * struct pollfd — POSIX layout. fry_pollfd in libc.h has the same
 * shape and layout.
 */
struct pollfd {
    int      fd;
    short    events;
    short    revents;
};

typedef unsigned long nfds_t;

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_POLL_H */
