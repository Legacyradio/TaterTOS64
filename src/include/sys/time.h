/*
 * TaterTOS64v3 — <sys/time.h>
 *
 * POSIX sys/time.h surface. struct timeval, struct timezone,
 * gettimeofday, settimeofday, ITIMER_*, struct itimerval, plus
 * select-related decls historically here.
 *
 * Origin log: logs/fry834.txt
 * Triggered by: AK/Time.h:26 (Ladybird port).
 */

#ifndef _TATERTOS_SYS_TIME_H
#define _TATERTOS_SYS_TIME_H

#include <stdint.h>
#include <stddef.h>
#include <fry_types.h>          /* time_t (via sys/types.h chain) */
#include <sys/types.h>          /* time_t, suseconds_t */

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

/*
 * gettimeofday — backed by fry_clock_gettime(FRY_CLOCK_REALTIME).
 * settimeofday is a no-op on TaterTOS for now (no userland write
 * to RTC).
 */
int gettimeofday(struct timeval *tv, struct timezone *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);

/*
 * Interval timers. TaterTOS does not currently raise SIGALRM from
 * userland-set timers — these are declared so portable code that
 * sets them but doesn't depend on the signal compiles.
 */
#define ITIMER_REAL     0
#define ITIMER_VIRTUAL  1
#define ITIMER_PROF     2

struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

int getitimer(int which, struct itimerval *curr_value);
int setitimer(int which, const struct itimerval *new_value,
              struct itimerval *old_value);

/*
 * timercmp / timeradd / timersub — POSIX inline macros.
 */
#define timeradd(a, b, result) do { \
    (result)->tv_sec  = (a)->tv_sec  + (b)->tv_sec; \
    (result)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
    if ((result)->tv_usec >= 1000000) { \
        ++(result)->tv_sec; \
        (result)->tv_usec -= 1000000; \
    } \
} while (0)

#define timersub(a, b, result) do { \
    (result)->tv_sec  = (a)->tv_sec  - (b)->tv_sec; \
    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((result)->tv_usec < 0) { \
        --(result)->tv_sec; \
        (result)->tv_usec += 1000000; \
    } \
} while (0)

#define timerclear(t)    ((t)->tv_sec = (t)->tv_usec = 0)
#define timerisset(t)    ((t)->tv_sec || (t)->tv_usec)
#define timercmp(a, b, op) \
    (((a)->tv_sec == (b)->tv_sec) \
        ? ((a)->tv_usec op (b)->tv_usec) \
        : ((a)->tv_sec  op (b)->tv_sec))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_TIME_H */
