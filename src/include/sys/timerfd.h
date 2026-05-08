/*
 * TaterTOS64v3 — <sys/timerfd.h>
 *
 * POSIX/Linux-compatible timerfd declarations backed by TaterTOS syscalls
 * SYS_TIMERFD_CREATE, SYS_TIMERFD_SETTIME, SYS_TIMERFD_GETTIME.
 */

#ifndef _TATERTOS_SYS_TIMERFD_H
#define _TATERTOS_SYS_TIMERFD_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flag values for timerfd_create() */
#define TFD_NONBLOCK   0x800
#define TFD_CLOEXEC    0x80000

/* Flag for timerfd_settime() */
#define TFD_TIMER_ABSTIME  0x01

/* Data structure for timerfd_settime/timerfd_gettime */
struct itimerspec {
    struct timespec it_interval;  /* Interval for periodic timer */
    struct timespec it_value;     /* Initial expiration */
};

/* Creation flags (Linux-style compat) */
#define TFD_TIMER_CANCEL_ON_SET 0x02

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);

#ifdef __cplusplus
}
#endif

#endif /* _TATERTOS_SYS_TIMERFD_H */
