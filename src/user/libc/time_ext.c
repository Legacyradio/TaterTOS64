/*
 * time_ext.c — Extended time functions for TaterTOS userspace
 *
 * Phase 5: time.h implementation for standard POSIX types and conversion.
 */

#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Public POSIX headers */
#include <time.h>
#include <sys/time.h>

/* Private TaterTOS ABI */
#include "libc.h"
#include "fry.h"

static const char *g_tz_name_utc = "UTC";

static int is_leap(int year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static int days_in_month(int year, int mon) {
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mon == 1 && is_leap(year)) return 29;
    if (mon < 0 || mon > 11) return 0;
    return days[mon];
}

static const char *day_abbr[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *mon_abbr[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *mon_names[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

static size_t append_str_ft(char *buf, size_t max, size_t pos, const char *s) {
    while (*s && pos < max - 1) {
        buf[pos++] = *s++;
    }
    buf[pos] = '\0';
    return pos;
}

static size_t append_num(char *buf, size_t max, size_t pos, int val, int digits) {
    char tmp[16];
    int i = 0;
    int v = val < 0 ? -val : val;
    do { tmp[i++] = (v % 10) + '0'; v /= 10; } while (v > 0);
    while (i < digits) tmp[i++] = '0';
    if (val < 0 && pos < max - 1) buf[pos++] = '-';
    while (i > 0 && pos < max - 1) buf[pos++] = tmp[--i];
    buf[pos] = '\0';
    return pos;
}

/* -----------------------------------------------------------------------
 * POSIX Time API
 * ----------------------------------------------------------------------- */

time_t time(time_t *tloc) {
    long ms = fry_gettime();
    time_t now = (time_t)(ms / 1000);
    if (tloc) *tloc = now;
    return now;
}

static struct tm g_tm_buf;

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    if (!timep || !result) return 0;
    time_t t = *timep;
    int64_t days = t / 86400;
    int64_t rem = t % 86400;

    result->tm_hour = (int)(rem / 3600);
    rem %= 3600;
    result->tm_min = (int)(rem / 60);
    result->tm_sec = (int)(rem % 60);

    /* Day of week (Jan 1 1970 was Thursday) */
    result->tm_wday = (int)((days + 4) % 7);
    if (result->tm_wday < 0) result->tm_wday += 7;

    int year = 1970;
    while (1) {
        int leap = is_leap(year);
        int ydays = leap ? 366 : 365;
        if (days < ydays) break;
        days -= ydays;
        year++;
    }

    result->tm_year = year - 1900;
    result->tm_yday = (int)days;

    int mon = 0;
    while (1) {
        int mdays = days_in_month(year, mon);
        if (days < mdays) break;
        days -= mdays;
        mon++;
    }

    result->tm_mon = mon;
    result->tm_mday = (int)days + 1;
    result->tm_isdst = 0; /* UTC, no DST */
    result->tm_gmtoff = 0;
    result->tm_zone = g_tz_name_utc;

    return result;
}

struct tm *gmtime(const time_t *timep) {
    return gmtime_r(timep, &g_tm_buf);
}

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    /* TaterTOS is UTC only for now */
    return gmtime_r(timep, result);
}

struct tm *localtime(const time_t *timep) {
    return gmtime_r(timep, &g_tm_buf);
}

time_t mktime(struct tm *tm) {
    if (!tm) return (time_t)-1;

    int year = tm->tm_year + 1900;
    int mon = tm->tm_mon;
    int64_t days = 0;

    for (int y = 1970; y < year; y++) days += is_leap(y) ? 366 : 365;
    for (int m = 0; m < mon; m++) days += days_in_month(year, m);
    days += tm->tm_mday - 1;

    time_t result = (time_t)(days * 86400 + tm->tm_hour * 3600 +
                             tm->tm_min * 60 + tm->tm_sec);

    /* Normalize tm by calling gmtime_r back */
    struct tm check;
    gmtime_r(&result, &check);
    *tm = check;

    return result;
}

time_t timegm(struct tm *tm) {
    return mktime(tm);
}

long timezone = 0;
int daylight = 0;
char *tzname[2] = { (char *)"UTC", (char *)"UTC" };

void tzset(void) {
    /* TaterTOS is UTC only */
}

int clock_gettime(clockid_t clk_id, struct timespec *ts) {
    struct fry_timespec fts;
    long rc = fry_clock_gettime((int)clk_id, &fts);
    if (rc < 0) {
        errno = (int)(-rc);
        return -1;
    }
    if (ts) {
        ts->tv_sec = (time_t)fts.tv_sec;
        ts->tv_nsec = (long)fts.tv_nsec;
    }
    return 0;
}

double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    (void)tz;
    if (!tv) return -1;
    struct fry_timespec ts;
    if (fry_clock_gettime(FRY_CLOCK_REALTIME, &ts) < 0) return -1;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int timespec_get(struct timespec *ts, int base) {
    if (!ts) return 0;
    if (base != TIME_UTC) return 0;
    struct fry_timespec fts;
    if (fry_clock_gettime(FRY_CLOCK_REALTIME, &fts) < 0) return 0;
    ts->tv_sec = fts.tv_sec;
    ts->tv_nsec = fts.tv_nsec;
    return TIME_UTC;
}

size_t strftime(char *s, size_t maxsize, const char *format, const struct tm *tm) {
    if (!s || maxsize == 0 || !format || !tm) return 0;
    size_t pos = 0;
    const char *f = format;

    while (*f && pos < maxsize - 1) {
        if (*f != '%') {
            s[pos++] = *f++;
            continue;
        }
        f++; /* skip % */
        if (!*f) break;

        switch (*f) {
        case 'a':
            pos = append_str_ft(s, maxsize, pos, day_abbr[tm->tm_wday % 7]);
            break;
        case 'A':
            pos = append_str_ft(s, maxsize, pos, day_names[tm->tm_wday % 7]);
            break;
        case 'b':
        case 'h':
            pos = append_str_ft(s, maxsize, pos, mon_abbr[tm->tm_mon % 12]);
            break;
        case 'B':
            pos = append_str_ft(s, maxsize, pos, mon_names[tm->tm_mon % 12]);
            break;
        case 'd':
            pos = append_num(s, maxsize, pos, tm->tm_mday, 2);
            break;
        case 'e':
            if (tm->tm_mday < 10) {
                if (pos < maxsize - 1) s[pos++] = ' ';
                pos = append_num(s, maxsize, pos, tm->tm_mday, 1);
            } else {
                pos = append_num(s, maxsize, pos, tm->tm_mday, 2);
            }
            break;
        case 'H':
            pos = append_num(s, maxsize, pos, tm->tm_hour, 2);
            break;
        case 'I': {
            int h = tm->tm_hour % 12;
            if (h == 0) h = 12;
            pos = append_num(s, maxsize, pos, h, 2);
            break;
        }
        case 'j':
            pos = append_num(s, maxsize, pos, tm->tm_yday + 1, 3);
            break;
        case 'm':
            pos = append_num(s, maxsize, pos, tm->tm_mon + 1, 2);
            break;
        case 'M':
            pos = append_num(s, maxsize, pos, tm->tm_min, 2);
            break;
        case 'p':
            pos = append_str_ft(s, maxsize, pos, tm->tm_hour < 12 ? "AM" : "PM");
            break;
        case 'S':
            pos = append_num(s, maxsize, pos, tm->tm_sec, 2);
            break;
        case 'w':
            pos = append_num(s, maxsize, pos, tm->tm_wday, 1);
            break;
        case 'y':
            pos = append_num(s, maxsize, pos, (tm->tm_year + 1900) % 100, 2);
            break;
        case 'Y':
            pos = append_num(s, maxsize, pos, tm->tm_year + 1900, 4);
            break;
        case '%':
            s[pos++] = '%';
            break;
        default:
            s[pos++] = *f;
            break;
        }
        f++;
    }

    s[pos] = '\0';
    return pos;
}

char *asctime(const struct tm *tm) {
    static char buf[32];
    if (!tm) return 0;
    snprintf(buf, sizeof(buf), "%s %s %2d %02d:%02d:%02d %d\n",
             day_abbr[tm->tm_wday % 7],
             mon_abbr[tm->tm_mon % 12],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *timep) {
    struct tm tm;
    if (!gmtime_r(timep, &tm)) return 0;
    return asctime(&tm);
}
