// PCI ECAM access

#include <stdint.h>
#include "pci.h"
#include "../../kernel/acpi/mcfg.h"
#include "../../kernel/mm/vmm.h"

static inline volatile uint32_t *pci_ecam_addr(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    uint64_t base = mcfg_get_ecam_base(segment, bus);
    if (!base) {
        return 0;
    }
    uint64_t addr = base
        + ((uint64_t)slot << 15)
        + ((uint64_t)func << 12)
        + (offset & 0xFFF);

    vmm_ensure_physmap(addr + 0x1000);
    return (volatile uint32_t *)(uintptr_t)vmm_phys_to_virt(addr);
}

uint32_t pci_ecam_read32(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    volatile uint32_t *reg = pci_ecam_addr(segment, bus, slot, func, offset);
    if (!reg) return 0xFFFFFFFFu;
    return *reg;
}

void pci_ecam_write32(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t val) {
    volatile uint32_t *reg = pci_ecam_addr(segment, bus, slot, func, offset);
    if (!reg) return;
    *reg = val;
}

uint8_t pci_ecam_read8(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    /* ECAM maps all 4096 bytes linearly; use dword-aligned base + byte offset */
    volatile uint32_t *base = pci_ecam_addr(segment, bus, slot, func, offset & ~3u);
    if (!base) return 0xFFu;
    volatile uint8_t *byte_ptr = (volatile uint8_t *)base + (offset & 3u);
    return *byte_ptr;
}

void pci_ecam_write8(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint8_t val) {
    volatile uint32_t *base = pci_ecam_addr(segment, bus, slot, func, offset & ~3u);
    if (!base) return;
    volatile uint8_t *byte_ptr = (volatile uint8_t *)base + (offset & 3u);
    *byte_ptr = val;
}
