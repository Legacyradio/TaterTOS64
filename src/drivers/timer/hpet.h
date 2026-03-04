#ifndef TATER_HPET_H
#define TATER_HPET_H

#include <stdint.h>

void hpet_init(void);
uint64_t hpet_get_freq_hz(void);
uint64_t hpet_read_counter(void);
void hpet_sleep_ms(uint64_t ms);

#endif
