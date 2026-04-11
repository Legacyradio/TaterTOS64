/*
 * CMOS RTC driver for TaterTOS64v3.
 *
 * Reads the MC146818-compatible real-time clock via I/O ports 0x70/0x71.
 * Used to seed CLOCK_REALTIME at boot; HPET provides monotonic offset.
 */

#include "rtc.h"

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* CMOS register indices */
#define RTC_SEC   0x00
#define RTC_MIN   0x02
#define RTC_HOUR  0x04
#define RTC_DAY   0x07
#define RTC_MONTH 0x08
#define RTC_YEAR  0x09
#define RTC_CENTURY 0x32   /* may not exist on all CMOS chips */
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, (inb(0x70) & 0x80) | (reg & 0x7F));  /* preserve NMI disable */
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static int rtc_updating(void) {
    return cmos_read(RTC_STATUS_A) & 0x80;
}

int rtc_read(struct rtc_time *t) {
    if (!t) return -1;

    /* Wait for any in-progress update to finish */
    int timeout = 10000;
    while (rtc_updating() && --timeout > 0) {
        __asm__ volatile("pause");
    }

    uint8_t sec   = cmos_read(RTC_SEC);
    uint8_t min   = cmos_read(RTC_MIN);
    uint8_t hour  = cmos_read(RTC_HOUR);
    uint8_t day   = cmos_read(RTC_DAY);
    uint8_t month = cmos_read(RTC_MONTH);
    uint8_t year  = cmos_read(RTC_YEAR);

    /* Read a second time to ensure consistency (no mid-update race) */
    uint8_t sec2   = cmos_read(RTC_SEC);
    uint8_t min2   = cmos_read(RTC_MIN);
    uint8_t hour2  = cmos_read(RTC_HOUR);
    uint8_t day2   = cmos_read(RTC_DAY);
    uint8_t month2 = cmos_read(RTC_MONTH);
    uint8_t year2  = cmos_read(RTC_YEAR);

    if (sec != sec2 || min != min2 || hour != hour2 ||
        day != day2 || month != month2 || year != year2) {
        /* Re-read once more */
        sec   = cmos_read(RTC_SEC);
        min   = cmos_read(RTC_MIN);
        hour  = cmos_read(RTC_HOUR);
        day   = cmos_read(RTC_DAY);
        month = cmos_read(RTC_MONTH);
        year  = cmos_read(RTC_YEAR);
    }

    uint8_t status_b = cmos_read(RTC_STATUS_B);
    int bcd = !(status_b & 0x04);  /* bit 2 = binary mode if set */
    int h24 = (status_b & 0x02);   /* bit 1 = 24-hour if set */

    if (bcd) {
        sec   = bcd_to_bin(sec);
        min   = bcd_to_bin(min);
        hour  = bcd_to_bin(hour & 0x7F);
        day   = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year  = bcd_to_bin(year);
    } else {
        hour = hour & 0x7F;
    }

    /* Handle 12-hour mode PM flag */
    if (!h24 && (cmos_read(RTC_HOUR) & 0x80)) {
        hour = (hour % 12) + 12;
    }

    /* Century: try the century register, fall back to 20xx */
    uint8_t century_raw = cmos_read(RTC_CENTURY);
    uint16_t century = 20;
    if (century_raw >= 0x19 && century_raw <= 0x21) {
        century = bcd ? bcd_to_bin(century_raw) : century_raw;
    }

    t->year   = century * 100 + year;
    t->month  = month;
    t->day    = day;
    t->hour   = hour;
    t->minute = min;
    t->second = sec;

    return 0;
}

int64_t rtc_to_epoch(const struct rtc_time *t) {
    if (!t) return 0;

    /* Days from year 1970 to start of given year */
    int64_t y = (int64_t)t->year;
    int64_t m = (int64_t)t->month;
    int64_t d = (int64_t)t->day;

    /* Adjust for month indexing: treat Jan/Feb as months 13/14 of previous year */
    if (m <= 2) {
        y--;
        m += 12;
    }

    /* Days from epoch (1970-01-01) using Rata Die method */
    int64_t era_days = 365 * y + y / 4 - y / 100 + y / 400;
    int64_t month_days = (153 * (m - 3) + 2) / 5 + d;
    int64_t total_days = era_days + month_days - 719469;  /* 719469 = days from 0000 to 1970 */

    return total_days * 86400LL
         + (int64_t)t->hour * 3600LL
         + (int64_t)t->minute * 60LL
         + (int64_t)t->second;
}

/* Boot-time epoch second, set once by rtc_init() */
static int64_t g_boot_epoch_sec;

void rtc_init(void) {
    struct rtc_time t;
    if (rtc_read(&t) == 0) {
        g_boot_epoch_sec = rtc_to_epoch(&t);
    } else {
        g_boot_epoch_sec = 0;
    }
}

int64_t rtc_boot_epoch_sec(void) {
    return g_boot_epoch_sec;
}
