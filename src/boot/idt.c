// IDT setup for TaterTOS64v3

#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern void *isr_stub_table[];

static struct idt_entry idt[256];
static struct idt_ptr idtp;

static void set_idt_entry(int vec, void *isr, uint8_t type_attr) {
    uint64_t addr = (uint64_t)isr;
    idt[vec].offset_low = (uint16_t)(addr & 0xFFFF);
    idt[vec].selector = 0x08; // kernel code segment
    idt[vec].ist = 0;
    idt[vec].type_attr = type_attr;
    idt[vec].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vec].zero = 0;
}

static inline void lidt(struct idt_ptr *p) {
    __asm__ volatile("lidt (%0)" : : "r"(p));
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        set_idt_entry(i, isr_stub_table[i], 0x8E);
    }

    // Use IST stacks for critical faults (#DF = 8, #GP = 13, #PF = 14).
    idt[8].ist = 1;   // IST1
    idt[13].ist = 2;  // IST2
    idt[14].ist = 2;  // IST2

    idtp.base = (uint64_t)&idt[0];
    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    lidt(&idtp);
}
