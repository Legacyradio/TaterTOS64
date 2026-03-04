// IRQ chip abstraction helpers

#include <stdint.h>
#include "chip.h"
#include "irqdesc.h"

void irq_chip_mask(uint32_t vector) {
    struct irq_desc *d = irq_get_desc(vector);
    if (d && d->chip && d->chip->mask) {
        d->chip->mask(vector);
    }
}

void irq_chip_unmask(uint32_t vector) {
    struct irq_desc *d = irq_get_desc(vector);
    if (d && d->chip && d->chip->unmask) {
        d->chip->unmask(vector);
    }
}

void irq_chip_ack(uint32_t vector) {
    struct irq_desc *d = irq_get_desc(vector);
    if (d && d->chip && d->chip->ack) {
        d->chip->ack(vector);
    }
}

void irq_chip_eoi(uint32_t vector) {
    struct irq_desc *d = irq_get_desc(vector);
    if (d && d->chip && d->chip->eoi) {
        d->chip->eoi(vector);
    }
}

void irq_chip_set_affinity(uint32_t vector, uint32_t apic_id) {
    struct irq_desc *d = irq_get_desc(vector);
    if (d && d->chip && d->chip->set_affinity) {
        d->chip->set_affinity(vector, apic_id);
    }
}
