/*
 * TaterTOS64v3 — <sys/select.h>
 *
 * POSIX select(). TaterTOS implements this via fry_poll under the
 * hood (the kernel scheduler's poll-block primitive). This header
 * provides the canonical names + fd_set + select prototypes.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_SYS_SELECT_H
#define _TATERTOS_SYS_SELECT_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/time.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FD_SETSIZE
#  define FD_SETSIZE 1024
#endif

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(set) do { \
    fd_set *__s = (set); \
    for (size_t __i = 0; __i < sizeof(__s->fds_bits) / sizeof(__s->fds_bits[0]); ++__i) \
        __s->fds_bits[__i] = 0; \
} while (0)

#define FD_SET(fd, set)   ((set)->fds_bits[(fd) / (8*sizeof(unsigned long))] |=  (1UL << ((fd) % (8*sizeof(unsigned long)))))
#define FD_CLR(fd, set)   ((set)->fds_bits[(fd) / (8*sizeof(unsigned long))] &= ~(1UL << ((fd) % (8*sizeof(unsigned long)))))
#define FD_ISSET(fd, set) (((set)->fds_bits[(fd) / (8*sizeof(unsigned long))] >> ((fd) % (8*sizeof(unsigned long)))) & 1UL)

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);
int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_SELECT_H */
