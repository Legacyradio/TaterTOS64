#ifndef TATER_RTC_H
#define TATER_RTC_H

#include <stdint.h>

struct rtc_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
};

/* Read current wall-clock time from CMOS RTC.  Returns 0 on success. */
int rtc_read(struct rtc_time *t);

/* Convert RTC time to Unix epoch seconds (UTC). */
int64_t rtc_to_epoch(const struct rtc_time *t);

/* Initialize RTC subsystem — reads boot-time wall-clock for CLOCK_REALTIME. */
void rtc_init(void);

/* Get the epoch second captured at boot (for CLOCK_REALTIME base). */
int64_t rtc_boot_epoch_sec(void);

#endif
