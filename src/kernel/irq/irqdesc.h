#ifndef TATER_IRQDESC_H
#define TATER_IRQDESC_H

#include <stdint.h>

struct irq_desc;

typedef void (*irq_handler_t)(uint32_t vector, void *ctx, void *dev_id, uint64_t error);

struct irq_chip {
    const char *name;
    void (*mask)(uint32_t vector);
    void (*unmask)(uint32_t vector);
    void (*ack)(uint32_t vector);
    void (*eoi)(uint32_t vector);
    void (*set_affinity)(uint32_t vector, uint32_t apic_id);
};

struct irq_desc {
    irq_handler_t handler;
    void *dev_id;
    struct irq_chip *chip;
    char name[32];
    uint32_t flags;
    uint32_t count;
};

struct irq_desc *irq_get_desc(uint32_t vector);
void irq_desc_init(void);
void irq_cr3_init(uint64_t cr3);
void irq_set_chip(uint32_t vector, struct irq_chip *chip);
void irq_set_handler(uint32_t vector, irq_handler_t handler, void *dev_id);
void irq_dispatch(uint64_t vector, uint64_t error, void *ctx);

#endif
