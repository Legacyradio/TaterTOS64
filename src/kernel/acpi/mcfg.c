// MCFG parser (PCIe ECAM regions)

#include <stdint.h>
#include "mcfg.h"
#include "tables.h"
#include "../mm/vmm.h"

void kprint(const char *fmt, ...);

struct acpi_mcfg {
    struct acpi_sdt_header hdr;
    uint64_t reserved;
} __attribute__((packed));

struct acpi_mcfg_entry {
    uint64_t base_addr;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __attribute__((packed));

static struct mcfg_ecam ecams[16];
static uint32_t ecam_count;

void mcfg_init(void) {
    ecam_count = 0;

    struct acpi_sdt_header *h = acpi_find_table("MCFG");
    if (!h) {
        kprint("MCFG: not found\n");
        return;
    }

    struct acpi_mcfg *m = (struct acpi_mcfg *)h;
    uint8_t *p = (uint8_t *)m + sizeof(struct acpi_mcfg);
    uint8_t *end = (uint8_t *)m + m->hdr.length;

    while (p + sizeof(struct acpi_mcfg_entry) <= end) {
        struct acpi_mcfg_entry *e = (struct acpi_mcfg_entry *)p;
        if (ecam_count < 16) {
            ecams[ecam_count].base_addr = e->base_addr;
            ecams[ecam_count].segment = e->segment;
            ecams[ecam_count].start_bus = e->start_bus;
            ecams[ecam_count].end_bus = e->end_bus;
            ecam_count++;
        }
        p += sizeof(struct acpi_mcfg_entry);
    }

    kprint("MCFG: ECAMs=%u\n", ecam_count);
}

uint32_t mcfg_get_ecam_count(void) {
    return ecam_count;
}

const struct mcfg_ecam *mcfg_get_ecam(uint32_t idx) {
    if (idx >= ecam_count) return 0;
    return &ecams[idx];
}

uint64_t mcfg_get_ecam_base(uint16_t segment, uint8_t bus) {
    for (uint32_t i = 0; i < ecam_count; i++) {
        if (ecams[i].segment == segment && bus >= ecams[i].start_bus && bus <= ecams[i].end_bus) {
            uint64_t bus_offset = (uint64_t)(bus - ecams[i].start_bus) << 20;
            return ecams[i].base_addr + bus_offset;
        }
    }
    return 0;
}
