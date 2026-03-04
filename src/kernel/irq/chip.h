#ifndef TATER_IRQ_CHIP_H
#define TATER_IRQ_CHIP_H

#include <stdint.h>
#include "irqdesc.h"

void irq_chip_mask(uint32_t vector);
void irq_chip_unmask(uint32_t vector);
void irq_chip_ack(uint32_t vector);
void irq_chip_eoi(uint32_t vector);
void irq_chip_set_affinity(uint32_t vector, uint32_t apic_id);

#endif
