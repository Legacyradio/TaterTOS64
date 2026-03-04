#ifndef TATER_ACPI_MADT_H
#define TATER_ACPI_MADT_H

#include <stdint.h>

struct madt_lapic {
    uint8_t acpi_cpu_id;
    uint8_t apic_id;
    uint32_t flags;
};

struct madt_ioapic {
    uint8_t id;
    uint32_t addr;
    uint32_t gsi_base;
};

struct madt_iso {
    uint8_t bus;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
};

void madt_init(void);
uint64_t madt_get_lapic_addr(void);
uint32_t madt_get_lapic_count(void);
uint32_t madt_get_ioapic_count(void);
uint32_t madt_get_iso_count(void);
const struct madt_lapic *madt_get_lapic(uint32_t idx);
const struct madt_ioapic *madt_get_ioapic(uint32_t idx);
const struct madt_iso *madt_get_iso(uint8_t irq);

#endif
