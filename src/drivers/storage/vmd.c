// Intel VMD (Volume Management Device) driver
//
// Ported from TatertOS64-new and adapted for the v3 physmap model.
// Two discovery passes:
//   Pass 1: class=01:04 (VMD) with CFGBAR in BAR0+BAR1
//   Pass 2: class=08:80, DID=0x1911 using scratch register at MEMBAR0+0x6C0
//
// Exposes cfg-space accessors for NVMe driver:
//   vmd_cfg_read32 / vmd_cfg_write32

#include <stdint.h>
#include "vmd.h"
#include "../../drivers/pci/pci.h"
#include "../../kernel/mm/vmm.h"

void kprint(const char *fmt, ...);

#define PHYSMAP_BASE VMM_PHYSMAP_BASE
#define VMD_ECAM_STRIDE (1ULL << 20) // 1MB per bus

static int      g_ready = 0;
static uint64_t g_cfgbar_phys = 0;
static uint64_t g_cfgbar_size = 0;
static uint8_t  g_bus_start = 0;
static uint8_t  g_bus_count = 0;

static inline uint64_t phys_to_virt(uint64_t p) {
    return p + PHYSMAP_BASE;
}

// Ensure the physmap covers [phys, phys+len) with cache-disable (for MMIO).
static void vmd_ensure_mapped(uint64_t phys, uint64_t len) {
    uint64_t step = 2ULL * 1024ULL * 1024ULL;  // 2 MB granularity
    for (uint64_t off = 0; off < len; off += step)
        vmm_ensure_physmap_uc(phys + off + 1);
    vmm_ensure_physmap_uc(phys + len);
}

static uint64_t bar_probe_size(uint8_t bus, uint8_t slot, uint8_t func, uint16_t bar_off) {
    uint32_t cmd = pci_ecam_read32(0, bus, slot, func, 0x04);
    uint32_t cmd_disabled = cmd & ~(1u << 1); // disable MEM decode
    pci_ecam_write32(0, bus, slot, func, 0x04, cmd_disabled);

    uint32_t orig_lo = pci_ecam_read32(0, bus, slot, func, bar_off);
    uint32_t orig_hi = 0;
    pci_ecam_write32(0, bus, slot, func, bar_off, 0xFFFFFFFFu);
    uint32_t mask_lo = pci_ecam_read32(0, bus, slot, func, bar_off);
    pci_ecam_write32(0, bus, slot, func, bar_off, orig_lo);

    if ((orig_lo & 0x6u) == 0x4u) {
        // 64-bit BAR
        orig_hi = pci_ecam_read32(0, bus, slot, func, bar_off + 4);
        pci_ecam_write32(0, bus, slot, func, bar_off + 4, 0xFFFFFFFFu);
        uint32_t mask_hi = pci_ecam_read32(0, bus, slot, func, bar_off + 4);
        pci_ecam_write32(0, bus, slot, func, bar_off + 4, orig_hi);

        uint64_t mask = ((uint64_t)mask_hi << 32) | (mask_lo & ~0xFULL);
        uint64_t size = (~mask) + 1;
        pci_ecam_write32(0, bus, slot, func, 0x04, cmd);
        return size;
    }

    uint64_t size = (~(uint64_t)(mask_lo & ~0xFULL)) + 1;
    pci_ecam_write32(0, bus, slot, func, 0x04, cmd);
    return size;
}

static uint8_t vmd_calc_bus_start(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t vmcap = pci_ecam_read32(0, bus, slot, func, 0x40);
    uint32_t vmcfg = pci_ecam_read32(0, bus, slot, func, 0x44);
    if (!(vmcap & 0x1u)) {
        return 0;
    }
    uint32_t off = (vmcfg >> 8) & 0x3u;
    if (off == 1) return 128;
    if (off == 2) return 224;
    return 0;
}

static uint8_t vmd_calc_bus_count(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t vmcap = pci_ecam_read32(0, bus, slot, func, 0x40);
    return (vmcap & 0x1u) ? 32 : 256;
}

static int vmd_setup_cfgbar(uint64_t cfgbar_phys, uint64_t cfgbar_size,
                            uint8_t bus_start, uint8_t bus_count) {
    if (!cfgbar_phys || !cfgbar_size || !bus_count) return 0;
    if (cfgbar_size < VMD_ECAM_STRIDE) return 0;

    uint64_t max_buses = (cfgbar_size >> 20);
    if (max_buses < bus_count) {
        bus_count = (uint8_t)max_buses;
    }
    if (bus_count == 0) return 0;

    vmd_ensure_mapped(cfgbar_phys, cfgbar_size);

    g_cfgbar_phys = cfgbar_phys;
    g_cfgbar_size = cfgbar_size;
    g_bus_start = bus_start;
    g_bus_count = bus_count;
    g_ready = 1;
    return 1;
}

static int vmd_probe_class_0104(void) {
    uint32_t count = 0;
    struct pci_device_info *devs = pci_get_devices(&count);

    for (uint32_t i = 0; i < count; i++) {
        if (devs[i].vendor_id != 0x8086) continue;
        if (devs[i].class_code != 0x01 || devs[i].subclass != 0x04) continue;

        kprint("VMD: class 01:04 at %u:%u.%u did=0x%04x\n",
               devs[i].bus, devs[i].slot, devs[i].func, devs[i].device_id);

        uint32_t bar0 = pci_ecam_read32(0, devs[i].bus, devs[i].slot, devs[i].func, 0x10);
        uint32_t bar1 = pci_ecam_read32(0, devs[i].bus, devs[i].slot, devs[i].func, 0x14);

        uint64_t cfgbar;
        if ((bar0 & 0x6u) == 0x4u) {
            /* 64-bit memory BAR — combine BAR0+BAR1. */
            cfgbar = ((uint64_t)bar1 << 32) | (bar0 & ~0xFULL);
        } else if ((bar0 & 0x1u) == 0 && (bar0 & ~0xFULL) != 0) {
            /* 32-bit memory BAR with a valid sub-4GB address. */
            kprint("VMD: BAR0 is 32-bit, cfgbar=0x%x\n", (unsigned)(bar0 & ~0xFU));
            cfgbar = (uint64_t)(bar0 & ~0xFULL);
        } else {
            kprint("VMD: BAR0 not usable (val=0x%x), skipping\n", bar0);
            continue;
        }
        uint64_t cfgbar_size = bar_probe_size(devs[i].bus, devs[i].slot, devs[i].func, 0x10);
        if (cfgbar_size < (1ULL << 20)) {
            kprint("VMD: CFGBAR size too small: 0x%llx\n",
                   (unsigned long long)cfgbar_size);
            continue;
        }

        uint8_t bus_start = vmd_calc_bus_start(devs[i].bus, devs[i].slot, devs[i].func);
        uint8_t bus_count = vmd_calc_bus_count(devs[i].bus, devs[i].slot, devs[i].func);

        kprint("VMD: CFGBAR=0x%llx size=0x%llx bus_start=%u bus_count=%u\n",
               (unsigned long long)cfgbar,
               (unsigned long long)cfgbar_size,
               bus_start, bus_count);

        if (vmd_setup_cfgbar(cfgbar, cfgbar_size, bus_start, bus_count)) {
            return 1;
        }
    }

    return 0;
}

static int vmd_probe_1911(void) {
    uint32_t count = 0;
    struct pci_device_info *devs = pci_get_devices(&count);

    for (uint32_t i = 0; i < count; i++) {
        if (devs[i].vendor_id != 0x8086) continue;
        if (devs[i].class_code != 0x08 || devs[i].subclass != 0x80) continue;
        if (devs[i].device_id != 0x1911) continue;

        kprint("VMD: class 08:80 DID=0x1911 at %u:%u.%u\n",
               devs[i].bus, devs[i].slot, devs[i].func);

        uint32_t m0_lo = pci_ecam_read32(0, devs[i].bus, devs[i].slot, devs[i].func, 0x10);
        uint32_t m0_hi = pci_ecam_read32(0, devs[i].bus, devs[i].slot, devs[i].func, 0x14);
        uint64_t membar0 = ((uint64_t)m0_hi << 32) | (m0_lo & ~0xFULL);
        if (!membar0) {
            kprint("VMD: MEMBAR0 is 0, skipping\n");
            continue;
        }

        vmd_ensure_mapped(membar0, 0x800);
        volatile uint32_t *m0 = (volatile uint32_t *)(uintptr_t)phys_to_virt(membar0);
        uint32_t scr_lo = m0[0x6C0 / 4];
        uint32_t scr_hi = m0[0x6C4 / 4];
        uint64_t cfgbar = ((uint64_t)scr_hi << 32) | scr_lo;
        kprint("VMD: scratch CFGBAR=0x%llx\n", (unsigned long long)cfgbar);
        /* All-zeros = not programmed; all-ones = bus not responding (non-VMD device). */
        if (!cfgbar || cfgbar == 0xFFFFFFFFFFFFFFFFULL) continue;

        uint8_t bus_start = vmd_calc_bus_start(devs[i].bus, devs[i].slot, devs[i].func);
        uint8_t bus_count = 32;
        uint64_t cfgbar_size = 32ULL * 1024ULL * 1024ULL;

        kprint("VMD: CFGBAR=0x%llx size=0x%llx bus_start=%u bus_count=%u\n",
               (unsigned long long)cfgbar,
               (unsigned long long)cfgbar_size,
               bus_start, bus_count);

        if (vmd_setup_cfgbar(cfgbar, cfgbar_size, bus_start, bus_count)) {
            return 1;
        }
    }

    return 0;
}

void vmd_init(void) {
    g_ready = 0;
    g_cfgbar_phys = 0;
    g_cfgbar_size = 0;
    g_bus_start = 0;
    g_bus_count = 0;

    if (vmd_probe_class_0104()) return;
    vmd_probe_1911();
}

int vmd_ready(void) {
    return g_ready;
}

uint64_t vmd_cfgbar_base(void) {
    return g_cfgbar_phys;
}

uint64_t vmd_cfgbar_size(void) {
    return g_cfgbar_size;
}

uint8_t vmd_bus_start(void) {
    return g_bus_start;
}

uint8_t vmd_bus_count(void) {
    return g_bus_count;
}

uint32_t vmd_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    if (!g_ready) return 0xFFFFFFFFu;
    if (bus < g_bus_start) return 0xFFFFFFFFu;
    if (bus >= (uint8_t)(g_bus_start + g_bus_count)) return 0xFFFFFFFFu;

    uint64_t off = ((uint64_t)(bus - g_bus_start) << 20) |
                   ((uint64_t)slot << 15) |
                   ((uint64_t)func << 12) |
                   (uint64_t)(offset & 0xFFC);
    volatile uint32_t *p =
        (volatile uint32_t *)(uintptr_t)phys_to_virt(g_cfgbar_phys + off);
    return *p;
}

void vmd_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func,
                     uint8_t offset, uint32_t value) {
    if (!g_ready) return;
    if (bus < g_bus_start) return;
    if (bus >= (uint8_t)(g_bus_start + g_bus_count)) return;

    uint64_t off = ((uint64_t)(bus - g_bus_start) << 20) |
                   ((uint64_t)slot << 15) |
                   ((uint64_t)func << 12) |
                   (uint64_t)(offset & 0xFFC);
    volatile uint32_t *p =
        (volatile uint32_t *)(uintptr_t)phys_to_virt(g_cfgbar_phys + off);
    *p = value;
}
