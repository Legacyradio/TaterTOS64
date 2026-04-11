#ifndef TATER_HPET_H
#define TATER_HPET_H

#include <stdint.h>

void hpet_init(void);
uint64_t hpet_get_freq_hz(void);
uint64_t hpet_read_counter(void);
void hpet_sleep_ms(uint64_t ms);

/* HPET period in femtoseconds per tick (for nanosecond-resolution time). */
uint64_t hpet_get_period_fs(void);

/* Get nanoseconds since HPET reset (boot).  Uses counter * period_fs / 1e6. */
void hpet_get_ns(int64_t *sec_out, int64_t *nsec_out);

#endif
