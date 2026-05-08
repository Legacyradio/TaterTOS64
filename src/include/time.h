/*
 * TaterTOS64v3 — <time.h>
 *
 * POSIX/C time.h surface.
 */

#ifndef _TATERTOS_TIME_H
#define _TATERTOS_TIME_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mbstate_t — multibyte conversion state, needed by libc++ */
typedef struct {
    uint64_t __state;
} mbstate_t;

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
    long tm_gmtoff;
    const char *tm_zone;
};

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_BOOTTIME           1
#define CLOCK_MONOTONIC_RAW      1
#define CLOCK_MONOTONIC_COARSE   1
#define CLOCK_REALTIME_COARSE    0
#define CLOCK_PROCESS_CPUTIME_ID 0
#define CLOCK_THREAD_CPUTIME_ID  0

#define CLOCKS_PER_SEC  1000000L

#ifndef _POSIX_MONOTONIC_CLOCK
#define _POSIX_MONOTONIC_CLOCK 1
#endif

#define TIME_UTC 1

typedef int32_t clockid_t;
typedef int32_t timer_t;

/* Standard POSIX declarations */
time_t time(time_t *tloc);
struct tm *gmtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
struct tm *localtime(const time_t *timep);
struct tm *localtime_r(const time_t *timep, struct tm *result);
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);
double difftime(time_t time1, time_t time0);
size_t strftime(char *s, size_t maxsize, const char *format, const struct tm *tm);
char *asctime(const struct tm *tm);
char *ctime(const time_t *timep);

int clock_gettime(clockid_t clk_id, struct timespec *ts);
int timespec_get(struct timespec *ts, int base);
int nanosleep(const struct timespec *req, struct timespec *rem);
clock_t clock(void);

void tzset(void);
extern long  timezone;
extern int   daylight;
extern char *tzname[2];

#ifdef __cplusplus
}
#endif

#endif
