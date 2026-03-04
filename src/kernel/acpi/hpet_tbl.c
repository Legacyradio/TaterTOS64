// HPET table parser

#include <stdint.h>
#include "tables.h"
#include "../mm/vmm.h"

void kprint(const char *fmt, ...);

struct acpi_hpet {
    struct acpi_sdt_header hdr;
    uint8_t hardware_rev_id;
    uint8_t info;
    uint16_t pci_id;
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((packed));

static uint64_t hpet_base;
static uint16_t hpet_min_tick;
static uint8_t hpet_comparators;

void hpet_tbl_init(void) {
    hpet_base = 0;
    hpet_min_tick = 0;
    hpet_comparators = 0;

    struct acpi_sdt_header *h = acpi_find_table("HPET");
    if (!h) {
        kprint("HPET: not found\n");
        return;
    }

    struct acpi_hpet *t = (struct acpi_hpet *)h;
    hpet_base = t->address;
    hpet_min_tick = t->minimum_tick;
    hpet_comparators = (t->info >> 3) & 0x1F;

    kprint("HPET: base=0x%llx min_tick=%u comps=%u\n", hpet_base, hpet_min_tick, hpet_comparators);
}

uint64_t hpet_tbl_get_base(void) {
    return hpet_base;
}
