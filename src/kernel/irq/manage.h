#ifndef TATER_IRQ_MANAGE_H
#define TATER_IRQ_MANAGE_H

#include <stdint.h>
#include "irqdesc.h"

int request_irq(uint32_t vector, irq_handler_t handler, uint32_t flags, const char *name, void *dev_id);
void free_irq(uint32_t vector, void *dev_id);
void enable_irq(uint32_t vector);
void disable_irq(uint32_t vector);

#endif
