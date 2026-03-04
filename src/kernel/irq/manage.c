// IRQ request/free management

#include <stdint.h>
#include "manage.h"
#include "irqdesc.h"
#include "chip.h"

static void copy_name(char *dst, const char *src) {
    if (!dst) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    for (uint32_t i = 0; i < 31; i++) {
        char c = src[i];
        dst[i] = c;
        if (c == 0) {
            return;
        }
    }
    dst[31] = 0;
}

int request_irq(uint32_t vector, irq_handler_t handler, uint32_t flags, const char *name, void *dev_id) {
    if (vector >= 256 || !handler) {
        return -1;
    }
    struct irq_desc *d = irq_get_desc(vector);
    if (!d) {
        return -1;
    }
    if (d->handler) {
        return -1;
    }
    d->handler = handler;
    d->dev_id = dev_id;
    d->flags = flags;
    copy_name(d->name, name);
    irq_chip_unmask(vector);
    return 0;
}

void free_irq(uint32_t vector, void *dev_id) {
    struct irq_desc *d = irq_get_desc(vector);
    if (!d) return;
    if (d->dev_id != dev_id) return;
    d->handler = 0;
    d->dev_id = 0;
    d->flags = 0;
    d->count = 0;
    d->name[0] = 0;
    irq_chip_mask(vector);
}

void enable_irq(uint32_t vector) {
    irq_chip_unmask(vector);
}

void disable_irq(uint32_t vector) {
    irq_chip_mask(vector);
}
