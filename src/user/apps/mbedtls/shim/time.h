/*
 * time.h shim — minimal time types for mbedTLS
 * We don't define MBEDTLS_HAVE_TIME so most time code is disabled,
 * but some headers still reference time_t.
 */
#ifndef _TATER_SHIM_TIME_H
#define _TATER_SHIM_TIME_H

#include <stddef.h>
#include <stdint.h>

typedef long time_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;         /* seconds east of UTC */
    const char *tm_zone;    /* timezone abbreviation */
};

#ifndef _TATER_TIMESPEC_DEFINED
#define _TATER_TIMESPEC_DEFINED
struct timespec {
    long tv_sec;
    long tv_nsec;
};
#endif

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

int clock_gettime(int clk_id, struct timespec *tp);

time_t time(time_t *t);
struct tm *gmtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
struct tm *localtime(const time_t *timep);
struct tm *localtime_r(const time_t *timep, struct tm *result);
time_t mktime(struct tm *tm);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

/* mbedtls_time_t used internally */
typedef long mbedtls_time_t;
mbedtls_time_t mbedtls_time(mbedtls_time_t *timer);

#endif
