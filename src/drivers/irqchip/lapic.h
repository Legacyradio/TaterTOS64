#ifndef TATER_LAPIC_H
#define TATER_LAPIC_H

#include <stdint.h>

uint64_t lapic_get_base_phys(void);
uint8_t lapic_get_id(void);
void lapic_init(void);
void lapic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint8_t delivery_mode);

#endif
