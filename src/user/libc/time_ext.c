/*
 * time_ext.c — Extended time functions for POSIX compatibility
 *
 * Phase 8: gmtime, localtime, mktime, strftime, gettimeofday, time()
 * needed by NSPR, NSS, and certificate validation.
 *
 * All implementations are original TaterTOS code.
 * TaterTOS uses UTC only (no timezone support yet).
 */

#include "libc.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * struct tm and time_t
 * ----------------------------------------------------------------------- */

/* Already defined in libc.h via forward declarations */

/* Days per month (non-leap, leap) */
static const int days_in_month[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};

static char g_tz_name_utc[] = "UTC";

long timezone = 0;
int daylight = 0;
char *tzname[2] = {g_tz_name_utc, g_tz_name_utc};

static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_year(int year) {
    return is_leap_year(year) ? 366 : 365;
}

void tzset(void) {
    timezone = 0;
    daylight = 0;
    tzname[0] = g_tz_name_utc;
    tzname[1] = g_tz_name_utc;
}

/* -----------------------------------------------------------------------
 * time() — seconds since epoch (1970-01-01 00:00:00 UTC)
 * ----------------------------------------------------------------------- */

time_t time_func(time_t *tloc) {
    struct fry_timespec ts;
    fry_clock_gettime(FRY_CLOCK_REALTIME, &ts);
    time_t t = (time_t)ts.tv_sec;
    if (tloc) *tloc = t;
    return t;
}

/* -----------------------------------------------------------------------
 * gmtime_r — reentrant UTC breakdown
 * ----------------------------------------------------------------------- */

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    if (!timep || !result) return 0;

    int64_t t = (int64_t)*timep;
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) { days--; rem += 86400; }

    result->tm_hour = (int)(rem / 3600);
    rem %= 3600;
    result->tm_min = (int)(rem / 60);
    result->tm_sec = (int)(rem % 60);

    /* Day of week: Jan 1 1970 was Thursday (4) */
    result->tm_wday = (int)((days + 4) % 7);
    if (result->tm_wday < 0) result->tm_wday += 7;

    /* Year and day-of-year */
    int year = 1970;
    if (days >= 0) {
        while (days >= days_in_year(year)) {
            days -= days_in_year(year);
            year++;
        }
    } else {
        while (days < 0) {
            year--;
            days += days_in_year(year);
        }
    }
    result->tm_year = year - 1900;
    result->tm_yday = (int)days;

    /* Month and day */
    int leap = is_leap_year(year);
    int mon = 0;
    while (mon < 11 && days >= days_in_month[leap][mon]) {
        days -= days_in_month[leap][mon];
        mon++;
    }
    result->tm_mon = mon;
    result->tm_mday = (int)days + 1;
    result->tm_isdst = 0; /* UTC, no DST */
    result->tm_gmtoff = 0;
    result->tm_zone = g_tz_name_utc;

    return result;
}

/* Non-reentrant version */
static struct tm g_tm_buf;

struct tm *gmtime_func(const time_t *timep) {
    return gmtime_r(timep, &g_tm_buf);
}

/* localtime — same as gmtime since TaterTOS is UTC-only */
struct tm *localtime_r(const time_t *timep, struct tm *result) {
    tzset();
    return gmtime_r(timep, result);
}

struct tm *localtime_func(const time_t *timep) {
    return gmtime_r(timep, &g_tm_buf);
}

/* -----------------------------------------------------------------------
 * mktime — convert struct tm to time_t
 * ----------------------------------------------------------------------- */

time_t mktime_func(struct tm *tm) {
    if (!tm) return (time_t)-1;

    /* Normalize month */
    int year = tm->tm_year + 1900;
    int mon = tm->tm_mon;
    while (mon < 0) { mon += 12; year--; }
    while (mon >= 12) { mon -= 12; year++; }

    /* Count days from epoch */
    int64_t days = 0;

    /* Years */
    if (year >= 1970) {
        for (int y = 1970; y < year; y++) days += days_in_year(y);
    } else {
        for (int y = year; y < 1970; y++) days -= days_in_year(y);
    }

    /* Months */
    int leap = is_leap_year(year);
    for (int m = 0; m < mon; m++) days += days_in_month[leap][m];

    /* Days */
    days += tm->tm_mday - 1;

    time_t result = (time_t)(days * 86400 + tm->tm_hour * 3600 +
                             tm->tm_min * 60 + tm->tm_sec);

    /* Fill in derived fields */
    struct tm check;
    gmtime_r(&result, &check);
    *tm = check;

    return result;
}

/* -----------------------------------------------------------------------
 * difftime
 * ----------------------------------------------------------------------- */

double difftime_func(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

/* -----------------------------------------------------------------------
 * gettimeofday — microsecond wall clock
 * ----------------------------------------------------------------------- */

int gettimeofday_func(struct timeval_compat *tv, void *tz) {
    (void)tz;
    if (!tv) return -1;

    struct fry_timespec ts;
    fry_clock_gettime(FRY_CLOCK_REALTIME, &ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

/* -----------------------------------------------------------------------
 * clock_gettime POSIX wrapper
 * ----------------------------------------------------------------------- */

int clock_gettime_compat(int clock_id, struct fry_timespec *ts) {
    if (!ts) return -1;
    long rc = fry_clock_gettime(clock_id, ts);
    return (rc < 0) ? -1 : 0;
}

/* Standard-name wrappers for QuickJS and other code that uses POSIX names */
int gettimeofday(void *tv, void *tz) {
    return gettimeofday_func((struct timeval_compat *)tv, tz);
}

int clock_gettime(int clock_id, void *ts) {
    return clock_gettime_compat(clock_id, (struct fry_timespec *)ts);
}

/* -----------------------------------------------------------------------
 * nanosleep POSIX wrapper
 * ----------------------------------------------------------------------- */

int nanosleep_compat(const struct fry_timespec *req, struct fry_timespec *rem) {
    if (!req) return -1;
    long rc = fry_nanosleep(req, rem);
    return (rc < 0) ? -1 : 0;
}

/* -----------------------------------------------------------------------
 * strftime — format time string
 * ----------------------------------------------------------------------- */

static const char *day_names[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

static const char *day_abbr[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *mon_names[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char *mon_abbr[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static size_t append_str_ft(char *buf, size_t max, size_t pos, const char *s) {
    while (*s && pos < max - 1) buf[pos++] = *s++;
    return pos;
}

static size_t append_num(char *buf, size_t max, size_t pos, int val, int width) {
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%0*d", width, val);
    return append_str_ft(buf, max, pos, tmp);
}

size_t strftime_func(char *buf, size_t maxsize, const char *fmt, const struct tm *tm) {
    if (!buf || maxsize == 0 || !fmt || !tm) return 0;

    size_t pos = 0;

    while (*fmt && pos < maxsize - 1) {
        if (*fmt != '%') {
            buf[pos++] = *fmt++;
            continue;
        }
        fmt++; /* skip '%' */

        switch (*fmt) {
        case 'a': /* Abbreviated weekday */
            pos = append_str_ft(buf, maxsize, pos, day_abbr[tm->tm_wday % 7]);
            break;
        case 'A': /* Full weekday */
            pos = append_str_ft(buf, maxsize, pos, day_names[tm->tm_wday % 7]);
            break;
        case 'b': case 'h': /* Abbreviated month */
            pos = append_str_ft(buf, maxsize, pos, mon_abbr[tm->tm_mon % 12]);
            break;
        case 'B': /* Full month */
            pos = append_str_ft(buf, maxsize, pos, mon_names[tm->tm_mon % 12]);
            break;
        case 'd': /* Day of month (01-31) */
            pos = append_num(buf, maxsize, pos, tm->tm_mday, 2);
            break;
        case 'e': /* Day of month ( 1-31) */
            if (tm->tm_mday < 10) {
                if (pos < maxsize - 1) buf[pos++] = ' ';
                pos = append_num(buf, maxsize, pos, tm->tm_mday, 1);
            } else {
                pos = append_num(buf, maxsize, pos, tm->tm_mday, 2);
            }
            break;
        case 'H': /* Hour (00-23) */
            pos = append_num(buf, maxsize, pos, tm->tm_hour, 2);
            break;
        case 'I': { /* Hour (01-12) */
            int h = tm->tm_hour % 12;
            if (h == 0) h = 12;
            pos = append_num(buf, maxsize, pos, h, 2);
            break;
        }
        case 'j': /* Day of year (001-366) */
            pos = append_num(buf, maxsize, pos, tm->tm_yday + 1, 3);
            break;
        case 'm': /* Month (01-12) */
            pos = append_num(buf, maxsize, pos, tm->tm_mon + 1, 2);
            break;
        case 'M': /* Minute (00-59) */
            pos = append_num(buf, maxsize, pos, tm->tm_min, 2);
            break;
        case 'p': /* AM/PM */
            pos = append_str_ft(buf, maxsize, pos, tm->tm_hour < 12 ? "AM" : "PM");
            break;
        case 'S': /* Second (00-60) */
            pos = append_num(buf, maxsize, pos, tm->tm_sec, 2);
            break;
        case 'w': /* Day of week (0=Sunday) */
            pos = append_num(buf, maxsize, pos, tm->tm_wday, 1);
            break;
        case 'y': /* Year without century (00-99) */
            pos = append_num(buf, maxsize, pos, (tm->tm_year + 1900) % 100, 2);
            break;
        case 'Y': /* Year with century */
            pos = append_num(buf, maxsize, pos, tm->tm_year + 1900, 4);
            break;
        case 'Z': /* Timezone */
            pos = append_str_ft(buf, maxsize, pos, "UTC");
            break;
        case 'z': /* Timezone offset */
            pos = append_str_ft(buf, maxsize, pos, "+0000");
            break;
        case '%':
            if (pos < maxsize - 1) buf[pos++] = '%';
            break;
        case 'n':
            if (pos < maxsize - 1) buf[pos++] = '\n';
            break;
        case 't':
            if (pos < maxsize - 1) buf[pos++] = '\t';
            break;
        default:
            break;
        }
        fmt++;
    }

    buf[pos] = '\0';
    return pos;
}

/* -----------------------------------------------------------------------
 * asctime / ctime
 * ----------------------------------------------------------------------- */

static char g_asctime_buf[64];

char *asctime_func(const struct tm *tm) {
    if (!tm) return 0;
    snprintf(g_asctime_buf, sizeof(g_asctime_buf),
             "%.3s %.3s %2d %02d:%02d:%02d %d\n",
             day_abbr[tm->tm_wday % 7],
             mon_abbr[tm->tm_mon % 12],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return g_asctime_buf;
}

char *ctime_func(const time_t *timep) {
    struct tm tm;
    if (!gmtime_r(timep, &tm)) return 0;
    return asctime_func(&tm);
}

/* -----------------------------------------------------------------------
 * clock() — process CPU time (stub: returns wall time in CLOCKS_PER_SEC)
 * ----------------------------------------------------------------------- */

long clock_func(void) {
    struct fry_timespec ts;
    fry_clock_gettime(FRY_CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}
