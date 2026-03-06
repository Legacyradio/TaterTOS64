#ifndef TATER_SMP_H
#define TATER_SMP_H

#include <stdint.h>

void smp_init(void);
uint32_t smp_cpu_count(void);
uint8_t smp_cpu_apic_id(uint32_t idx);
uint32_t smp_bsp_index(void);
struct tss64;
struct tss64 *smp_get_tss(uint32_t cpu);

#endif
