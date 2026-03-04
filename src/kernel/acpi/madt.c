// MADT parser

#include <stdint.h>
#include "madt.h"
#include "tables.h"
#include "../mm/vmm.h"

void kprint(const char *fmt, ...);

struct acpi_madt {
    struct acpi_sdt_header hdr;
    uint32_t lapic_addr;
    uint32_t flags;
} __attribute__((packed));

struct madt_entry_hdr {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct madt_lapic_entry {
    struct madt_entry_hdr h;
    uint8_t acpi_cpu_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

struct madt_ioapic_entry {
    struct madt_entry_hdr h;
    uint8_t id;
    uint8_t reserved;
    uint32_t addr;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_iso_entry {
    struct madt_entry_hdr h;
    uint8_t bus;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

struct madt_nmi_entry {
    struct madt_entry_hdr h;
    uint8_t proc_id;
    uint16_t flags;
    uint8_t lint;
} __attribute__((packed));

struct madt_lapic_override {
    struct madt_entry_hdr h;
    uint16_t reserved;
    uint64_t lapic_addr;
} __attribute__((packed));

static uint64_t lapic_phys;
static struct madt_lapic lapics[256];
static struct madt_ioapic ioapics[16];
static struct madt_iso isos[64];
static uint32_t lapic_count;
static uint32_t ioapic_count;
static uint32_t iso_count;

void madt_init(void) {
    lapic_count = 0;
    ioapic_count = 0;
    iso_count = 0;
    lapic_phys = 0;

    struct acpi_sdt_header *h = acpi_find_table("APIC");
    if (!h) {
        kprint("MADT: not found\n");
        return;
    }

    struct acpi_madt *m = (struct acpi_madt *)h;
    lapic_phys = m->lapic_addr;

    uint8_t *p = (uint8_t *)m + sizeof(struct acpi_madt);
    uint8_t *end = (uint8_t *)m + m->hdr.length;

    while (p + sizeof(struct madt_entry_hdr) <= end) {
        struct madt_entry_hdr *eh = (struct madt_entry_hdr *)p;
        if (eh->length < sizeof(struct madt_entry_hdr)) {
            break;
        }

        if (p + eh->length > end) {
            break;
        }

        switch (eh->type) {
            case 0: {
                struct madt_lapic_entry *e = (struct madt_lapic_entry *)p;
                if (lapic_count < 256) {
                    lapics[lapic_count].acpi_cpu_id = e->acpi_cpu_id;
                    lapics[lapic_count].apic_id = e->apic_id;
                    lapics[lapic_count].flags = e->flags;
                    lapic_count++;
                }
                break;
            }
            case 1: {
                struct madt_ioapic_entry *e = (struct madt_ioapic_entry *)p;
                if (ioapic_count < 16) {
                    ioapics[ioapic_count].id = e->id;
                    ioapics[ioapic_count].addr = e->addr;
                    ioapics[ioapic_count].gsi_base = e->gsi_base;
                    ioapic_count++;
                }
                break;
            }
            case 2: {
                struct madt_iso_entry *e = (struct madt_iso_entry *)p;
                if (iso_count < 64) {
                    isos[iso_count].bus = e->bus;
                    isos[iso_count].source_irq = e->source_irq;
                    isos[iso_count].gsi = e->gsi;
                    isos[iso_count].flags = e->flags;
                    iso_count++;
                }
                break;
            }
            case 4: {
                // NMI source (not stored yet)
                break;
            }
            case 5: {
                struct madt_lapic_override *e = (struct madt_lapic_override *)p;
                lapic_phys = e->lapic_addr;
                break;
            }
            default:
                break;
        }

        p += eh->length;
    }

    kprint("MADT: LAPICs=%u IOAPICs=%u ISOs=%u\n", lapic_count, ioapic_count, iso_count);
}

uint64_t madt_get_lapic_addr(void) {
    return lapic_phys;
}

uint32_t madt_get_lapic_count(void) {
    return lapic_count;
}

uint32_t madt_get_ioapic_count(void) {
    return ioapic_count;
}

uint32_t madt_get_iso_count(void) {
    return iso_count;
}

const struct madt_lapic *madt_get_lapic(uint32_t idx) {
    if (idx >= lapic_count) return 0;
    return &lapics[idx];
}

const struct madt_ioapic *madt_get_ioapic(uint32_t idx) {
    if (idx >= ioapic_count) return 0;
    return &ioapics[idx];
}

const struct madt_iso *madt_get_iso(uint8_t irq) {
    for (uint32_t i = 0; i < iso_count; i++) {
        if (isos[i].source_irq == irq) {
            return &isos[i];
        }
    }
    return 0;
}
