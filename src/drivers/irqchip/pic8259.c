// PIC8259 irq_chip

#include <stdint.h>
#include "../../kernel/irq/irqdesc.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#define PIC1 0x20
#define PIC2 0xA0
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

static void pic_mask(uint32_t vector) {
    uint8_t irq = (uint8_t)(vector - 32);
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    mask |= (uint8_t)(1u << (irq & 7));
    outb(port, mask);
}

static void pic_unmask(uint32_t vector) {
    uint8_t irq = (uint8_t)(vector - 32);
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    mask &= (uint8_t)~(1u << (irq & 7));
    outb(port, mask);
}

static void pic_eoi(uint32_t vector) {
    uint8_t irq = (uint8_t)(vector - 32);
    if (irq >= 8) {
        outb(PIC2, 0x20);
    }
    outb(PIC1, 0x20);
}

static struct irq_chip pic_chip = {
    .name = "pic8259",
    .mask = pic_mask,
    .unmask = pic_unmask,
    .ack = pic_eoi,
    .eoi = pic_eoi,
    .set_affinity = 0,
};

void pic8259_init(void) {
    // Remap PIC to vectors 32-47
    outb(PIC1, 0x11);
    outb(PIC2, 0x11);
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    // Mask all IRQs initially
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    // Register chip for vectors 32-47
    for (uint32_t v = 32; v < 48; v++) {
        irq_set_chip(v, &pic_chip);
    }
}
