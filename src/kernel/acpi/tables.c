// ACPI table discovery and registry

#include <stdint.h>
#include "tables.h"
#include "../mm/vmm.h"
#include "../../boot/early_serial.h"

// Ensure every 2MB page covering [phys, phys+len) is mapped in the physmap.
static void acpi_ensure_mapped(uint64_t phys, uint32_t len) {
    if (!phys || !len) return;

    uint64_t step = 0x200000ULL;
    uint64_t first = phys & ~(step - 1ULL);
    uint64_t last_addr;
    if (phys > UINT64_MAX - ((uint64_t)len - 1ULL)) {
        last_addr = UINT64_MAX;
    } else {
        last_addr = phys + (uint64_t)len - 1ULL;
    }
    uint64_t last = last_addr & ~(step - 1ULL);

    for (uint64_t base = first;; base += step) {
        uint64_t phys_end = (base > UINT64_MAX - step) ? UINT64_MAX : (base + step);
        vmm_ensure_physmap(phys_end); // end-exclusive: maps chunk containing (phys_end - 1)
        if (base == last) break;
    }
}

void kprint(const char *fmt, ...);
void kernel_panic(const char *msg);
extern struct fry_handoff *g_handoff;

struct acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

static struct acpi_sdt_header *table_registry[64];
static uint32_t table_count;

static uint8_t checksum8(const void *ptr, uint32_t len) {
    const uint8_t *p = (const uint8_t *)ptr;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum;
}

static int sig_eq(const char a[4], const char b[4]) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

static void registry_add(struct acpi_sdt_header *h) {
    if (!h || table_count >= 64) {
        return;
    }
    table_registry[table_count++] = h;
}

void acpi_tables_init(uint64_t rsdp_phys) {
    table_count = 0;

    if (!rsdp_phys) {
        kprint("ACPI: no RSDP\n");
        return;
    }

    #define STAGE(c) do { early_debug_putc(c); early_serial_putc(c); } while (0)
    STAGE('R');
    if (g_handoff && g_handoff->boot_identity_limit && rsdp_phys >= g_handoff->boot_identity_limit) {
        kprint("ACPI: RSDP above boot identity map\n");
        kernel_panic("RSDP not mapped");
    }
    // RSDP should be reachable via physmap after VMM init; avoid extra page-table allocations here.
    struct acpi_rsdp *rsdp = (struct acpi_rsdp *)(uintptr_t)vmm_phys_to_virt(rsdp_phys);
    if (!(rsdp->signature[0] == 'R' && rsdp->signature[1] == 'S' &&
          rsdp->signature[2] == 'D' && rsdp->signature[3] == ' ' &&
          rsdp->signature[4] == 'P' && rsdp->signature[5] == 'T' &&
          rsdp->signature[6] == 'R' && rsdp->signature[7] == ' ')) {
        kprint("ACPI: bad RSDP signature\n");
        return;
    }
    STAGE('r');
    STAGE('r');
    kprint("ACPI: RSDP phys=0x%llx rev=%u\n", rsdp_phys, rsdp->revision);
    if (checksum8(rsdp, 20) != 0) {
        kprint("ACPI: RSDP checksum fail\n");
        return;
    }
    STAGE('c');
    STAGE('C');

    uint64_t sdt_phys = 0;
    int use_xsdt = 0;
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        if (checksum8(rsdp, rsdp->length) == 0) {
            sdt_phys = rsdp->xsdt_address;
            use_xsdt = 1;
        }
    }
    if (!sdt_phys && rsdp->rsdt_address) {
        sdt_phys = rsdp->rsdt_address;
        use_xsdt = 0;
    }

    if (!sdt_phys) {
        kprint("ACPI: no XSDT/RSDT\n");
        return;
    }
    STAGE(use_xsdt ? 'X' : 'x');

    STAGE('S');
    acpi_ensure_mapped(sdt_phys, sizeof(struct acpi_sdt_header));
    struct acpi_sdt_header *sdt = (struct acpi_sdt_header *)(uintptr_t)vmm_phys_to_virt(sdt_phys);
    uint32_t sdt_len = sdt->length;
    if (sdt_len < sizeof(struct acpi_sdt_header) || sdt_len > 0x100000) {
        kprint("ACPI: SDT bad length %u\n", sdt_len);
        return;
    }
    acpi_ensure_mapped(sdt_phys, sdt_len);
    if (checksum8(sdt, sdt_len) != 0) {
        kprint("ACPI: SDT checksum fail\n");
        return;
    }
    STAGE('s');

    // Add XSDT/RSDT itself
    registry_add(sdt);

    STAGE('E');
    uint32_t entry_count;
    if (use_xsdt) {
        entry_count = (sdt_len - sizeof(struct acpi_sdt_header)) / 8;
        uint64_t *entries = (uint64_t *)((uintptr_t)sdt + sizeof(struct acpi_sdt_header));
        for (uint32_t i = 0; i < entry_count; i++) {
            uint64_t phys = entries[i];
            if (!phys) continue;
            // Map header first so we can safely read h->length.
            acpi_ensure_mapped(phys, sizeof(struct acpi_sdt_header));
            struct acpi_sdt_header *h = (struct acpi_sdt_header *)(uintptr_t)vmm_phys_to_virt(phys);
            uint32_t len = h->length;
            if (len < sizeof(struct acpi_sdt_header) || len > 0x100000) continue;
            // Now map the full table and verify checksum.
            acpi_ensure_mapped(phys, len);
            if (checksum8(h, len) == 0) {
                registry_add(h);
            }
        }
    } else {
        entry_count = (sdt_len - sizeof(struct acpi_sdt_header)) / 4;
        uint32_t *entries = (uint32_t *)((uintptr_t)sdt + sizeof(struct acpi_sdt_header));
        for (uint32_t i = 0; i < entry_count; i++) {
            uint64_t phys = (uint64_t)entries[i];
            if (!phys) continue;
            acpi_ensure_mapped(phys, sizeof(struct acpi_sdt_header));
            struct acpi_sdt_header *h = (struct acpi_sdt_header *)(uintptr_t)vmm_phys_to_virt(phys);
            uint32_t len = h->length;
            if (len < sizeof(struct acpi_sdt_header) || len > 0x100000) continue;
            acpi_ensure_mapped(phys, len);
            if (checksum8(h, len) == 0) {
                registry_add(h);
            }
        }
    }
    STAGE('e');

    STAGE('L');
    for (uint32_t i = 0; i < table_count; i++) {
        struct acpi_sdt_header *h = table_registry[i];
        kprint("ACPI: %c%c%c%c len=%u\n", h->signature[0], h->signature[1], h->signature[2], h->signature[3], h->length);
    }
    STAGE('l');
    #undef STAGE
}

struct acpi_sdt_header *acpi_find_table(const char sig[4]) {
    for (uint32_t i = 0; i < table_count; i++) {
        if (sig_eq(table_registry[i]->signature, sig)) {
            return table_registry[i];
        }
    }
    return 0;
}

uint32_t acpi_table_count(void) {
    return table_count;
}

struct acpi_sdt_header *acpi_get_table(uint32_t idx) {
    if (idx >= table_count) {
        return 0;
    }
    return table_registry[idx];
}
