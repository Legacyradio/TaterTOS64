#ifndef TATER_FRY_TIME_H
#define TATER_FRY_TIME_H

#include <stdint.h>

/*
 * TaterTOS time types — shared between kernel and userspace.
 *
 * fry_timespec uses signed 64-bit fields to match POSIX semantics:
 *   tv_sec  = seconds
 *   tv_nsec = nanoseconds [0, 999999999]
 */
struct fry_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* Clock IDs for SYS_CLOCK_GETTIME */
#define FRY_CLOCK_MONOTONIC  0   /* HPET-based, never goes backward */
#define FRY_CLOCK_REALTIME   1   /* RTC-seeded wall-clock time       */
#define FRY_CLOCK_BOOTTIME   2   /* alias for monotonic (no suspend) */

#endif
