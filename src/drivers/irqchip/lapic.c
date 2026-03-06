// Local APIC irq_chip

#include <stdint.h>
#include "../../kernel/irq/irqdesc.h"
#include "../../kernel/acpi/madt.h"
#include "../../kernel/mm/vmm.h"

#define LAPIC_REG_ID         0x020
#define LAPIC_REG_TPR        0x080
#define LAPIC_REG_EOI        0x0B0
#define LAPIC_REG_SVR        0x0F0
#define LAPIC_REG_ICR_LOW    0x300
#define LAPIC_REG_ICR_HIGH   0x310
#define LAPIC_REG_LVT_TIMER  0x320
#define LAPIC_REG_LVT_LINT0  0x350
#define LAPIC_REG_LVT_LINT1  0x360
#define LAPIC_REG_LVT_ERROR  0x370

static volatile uint32_t *lapic_base;
static uint64_t lapic_phys;

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void write_msr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static void lapic_mask_lvt(uint32_t vector, int mask) {
    uint32_t regs[] = { LAPIC_REG_LVT_TIMER, LAPIC_REG_LVT_LINT0, LAPIC_REG_LVT_LINT1, LAPIC_REG_LVT_ERROR };
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t v = lapic_read(regs[i]);
        if ((v & 0xFF) == (vector & 0xFF)) {
            if (mask) {
                v |= (1u << 16);
            } else {
                v &= ~(1u << 16);
            }
            lapic_write(regs[i], v);
        }
    }
}

static void lapic_eoi(uint32_t vector) {
    (void)vector;
    if (lapic_base) {
        lapic_write(LAPIC_REG_EOI, 0);
    }
}

static void lapic_mask(uint32_t vector) {
    if (lapic_base) {
        lapic_mask_lvt(vector, 1);
    }
}

static void lapic_unmask(uint32_t vector) {
    if (lapic_base) {
        lapic_mask_lvt(vector, 0);
    }
}

static struct irq_chip lapic_chip = {
    .name = "lapic",
    .mask = lapic_mask,
    .unmask = lapic_unmask,
    .ack = lapic_eoi,
    .eoi = 0,
    .set_affinity = 0,
};

uint64_t lapic_get_base_phys(void) {
    return lapic_phys;
}

uint8_t lapic_get_id(void) {
    if (!lapic_base) return 0;
    return (uint8_t)(lapic_read(LAPIC_REG_ID) >> 24);
}

void lapic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint8_t delivery_mode) {
    if (!lapic_base) return;
    lapic_write(LAPIC_REG_ICR_HIGH, ((uint32_t)dest_apic_id) << 24);
    lapic_write(LAPIC_REG_ICR_LOW, (uint32_t)vector | ((uint32_t)delivery_mode << 8));
    while (lapic_read(LAPIC_REG_ICR_LOW) & (1u << 12)) {
    }
}

void lapic_init(void) {
    uint64_t phys = madt_get_lapic_addr();
    if (!phys) {
        uint64_t apic_base = read_msr(0x1B);
        phys = apic_base & 0xFFFFF000ULL;
        apic_base |= (1ULL << 11);
        write_msr(0x1B, apic_base);
    }
    if (!phys) return;

    lapic_phys = phys;
    vmm_ensure_physmap_uc(phys + 0x1000);
    lapic_base = (volatile uint32_t *)(uintptr_t)vmm_phys_to_virt(phys);

    // Enable LAPIC, spurious vector 0xFF
    lapic_write(LAPIC_REG_SVR, 0x100 | 0xFF);
    lapic_write(LAPIC_REG_LVT_LINT0, 1u << 16);
    lapic_write(LAPIC_REG_LVT_LINT1, 1u << 16);
    lapic_write(LAPIC_REG_LVT_ERROR, 1u << 16);
    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_EOI, 0);

    // Register chip for vectors 32-255 (APIC mode)
    for (uint32_t v = 32; v < 256; v++) {
        irq_set_chip(v, &lapic_chip);
    }

    (void)lapic_read(LAPIC_REG_ID);
}
