/*
 * TaterTOS64v3 — <langinfo.h>
 *
 * Minimal langinfo.h for Chromium ICU and other POSIX-conformant code.
 * TaterTOS uses the C locale only. nl_langinfo() returns fixed values
 * appropriate for a minimal freestanding environment.
 */

#ifndef _TATERTOS_LANGINFO_H
#define _TATERTOS_LANGINFO_H

#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

/* nl_types.h constants — nl_item argument values */
#define CODESET      1    /* return codeset name */
#define D_T_FMT      2    /* date/time format */
#define D_FMT        3    /* date format */
#define T_FMT        4    /* time format */
#define T_FMT_AMPM   5    /* 12-hour time format */
#define AM_STR       6    /* AM string */
#define PM_STR       7    /* PM string */
#define DAY_1        8    /* Sunday */
#define DAY_2        9
#define DAY_3        10
#define DAY_4        11
#define DAY_5        12
#define DAY_6        13
#define DAY_7        14
#define ABDAY_1      15   /* Sun */
#define ABDAY_2      16
#define ABDAY_3      17
#define ABDAY_4      18
#define ABDAY_5      19
#define ABDAY_6      20
#define ABDAY_7      21
#define MON_1        22   /* January */
#define MON_2        23
#define MON_3        24
#define MON_4        25
#define MON_5        26
#define MON_6        27
#define MON_7        28
#define MON_8        29
#define MON_9        30
#define MON_10       31
#define MON_11       32
#define MON_12       33
#define ABMON_1      34   /* Jan */
#define ABMON_2      35
#define ABMON_3      36
#define ABMON_4      37
#define ABMON_5      38
#define ABMON_6      39
#define ABMON_7      40
#define ABMON_8      41
#define ABMON_9      42
#define ABMON_10     43
#define ABMON_11     44
#define ABMON_12     45
#define RADIXCHAR    46   /* radix character ('.') */
#define THOUSEP      47   /* thousands separator ('') */
#define YESSTR       48
#define NOSTR        49
#define CRNCYSTR     50

typedef int nl_item;

char *nl_langinfo(nl_item item);
char *nl_langinfo_l(nl_item item, locale_t locale);

#ifdef __cplusplus
}
#endif

#endif /* _TATERTOS_LANGINFO_H */
