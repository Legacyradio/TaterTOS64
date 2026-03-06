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
#define VMD_MAX_CONTROLLERS 8

struct vmd_controller {
    uint64_t cfgbar_phys;
    uint64_t cfgbar_size;
    uint8_t bus_start;
    uint16_t bus_count;
};

static struct vmd_controller g_ctrls[VMD_MAX_CONTROLLERS];
static uint8_t g_ctrl_count = 0;
static uint8_t g_active_ctrl = 0;

static inline uint64_t phys_to_virt(uint64_t p) {
    return p + PHYSMAP_BASE;
}

// Ensure the physmap covers [phys, phys+len) with cache-disable (for MMIO).
static void vmd_ensure_mapped(uint64_t phys, uint64_t len) {
    if (!phys || !len) return;

    uint64_t step = 2ULL * 1024ULL * 1024ULL; // 2 MB granularity
    uint64_t first = phys & ~(step - 1ULL);
    uint64_t last_addr;
    if (phys > UINT64_MAX - (len - 1ULL)) {
        last_addr = UINT64_MAX;
    } else {
        last_addr = phys + len - 1ULL;
    }
    uint64_t last = last_addr & ~(step - 1ULL);

    for (uint64_t base = first;; base += step) {
        uint64_t phys_end = (base > UINT64_MAX - step) ? UINT64_MAX : (base + step);
        vmm_ensure_physmap_uc(phys_end); // end-exclusive: maps chunk containing (phys_end - 1)
        if (base == last) break;
    }
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

static uint16_t vmd_calc_bus_count(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t vmcap = pci_ecam_read32(0, bus, slot, func, 0x40);
    return (vmcap & 0x1u) ? 32 : 256;
}

static int vmd_add_controller(uint64_t cfgbar_phys, uint64_t cfgbar_size,
                              uint8_t bus_start, uint16_t bus_count) {
    if (!cfgbar_phys || !cfgbar_size || !bus_count) return 0;
    if (cfgbar_size < VMD_ECAM_STRIDE) return 0;

    uint64_t max_buses = (cfgbar_size >> 20);
    if (max_buses < bus_count) {
        bus_count = (uint16_t)max_buses;
    }
    if (bus_count == 0) return 0;

    for (uint8_t i = 0; i < g_ctrl_count; i++) {
        if (g_ctrls[i].cfgbar_phys == cfgbar_phys &&
            g_ctrls[i].bus_start == bus_start &&
            g_ctrls[i].bus_count == bus_count) {
            return 1;
        }
    }
    if (g_ctrl_count >= VMD_MAX_CONTROLLERS) {
        kprint("VMD: controller table full, dropping cfgbar=0x%llx\n",
               (unsigned long long)cfgbar_phys);
        return 0;
    }

    vmd_ensure_mapped(cfgbar_phys, cfgbar_size);

    g_ctrls[g_ctrl_count].cfgbar_phys = cfgbar_phys;
    g_ctrls[g_ctrl_count].cfgbar_size = cfgbar_size;
    g_ctrls[g_ctrl_count].bus_start = bus_start;
    g_ctrls[g_ctrl_count].bus_count = bus_count;
    g_ctrl_count++;
    return 1;
}

static struct vmd_controller *vmd_active(void) {
    if (g_ctrl_count == 0) return 0;
    if (g_active_ctrl >= g_ctrl_count) g_active_ctrl = 0;
    return &g_ctrls[g_active_ctrl];
}

static int vmd_probe_class_0104(void) {
    uint32_t count = 0;
    const struct pci_device_info *devs = pci_get_devices(&count);
    int found = 0;

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
        uint8_t bus_start = vmd_calc_bus_start(devs[i].bus, devs[i].slot, devs[i].func);
        uint16_t bus_count = vmd_calc_bus_count(devs[i].bus, devs[i].slot, devs[i].func);
        uint64_t cfgbar_size = (uint64_t)bus_count * VMD_ECAM_STRIDE;
        if (cfgbar_size < (1ULL << 20)) {
            cfgbar_size = 32ULL * 1024ULL * 1024ULL;
        }

        kprint("VMD: CFGBAR=0x%llx size=0x%llx bus_start=%u bus_count=%u\n",
               (unsigned long long)cfgbar,
               (unsigned long long)cfgbar_size,
               bus_start, bus_count);

        if (vmd_add_controller(cfgbar, cfgbar_size, bus_start, bus_count)) {
            found = 1;
        }
    }

    return found;
}

static int vmd_probe_1911(void) {
    uint32_t count = 0;
    const struct pci_device_info *devs = pci_get_devices(&count);
    int found = 0;

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
        uint16_t bus_count = 32;
        uint64_t cfgbar_size = 32ULL * 1024ULL * 1024ULL;

        kprint("VMD: CFGBAR=0x%llx size=0x%llx bus_start=%u bus_count=%u\n",
               (unsigned long long)cfgbar,
               (unsigned long long)cfgbar_size,
               bus_start, bus_count);

        if (vmd_add_controller(cfgbar, cfgbar_size, bus_start, bus_count)) {
            found = 1;
        }
    }

    return found;
}

void vmd_init(void) {
    for (uint8_t i = 0; i < VMD_MAX_CONTROLLERS; i++) {
        g_ctrls[i].cfgbar_phys = 0;
        g_ctrls[i].cfgbar_size = 0;
        g_ctrls[i].bus_start = 0;
        g_ctrls[i].bus_count = 0;
    }
    g_ctrl_count = 0;
    g_active_ctrl = 0;

    kprint("VMD: init begin\n");
    int found_0104 = vmd_probe_class_0104();
    if (!found_0104) {
        (void)vmd_probe_1911();
    } else {
        kprint("VMD: skipping 1911 probe (01:04 controller present)\n");
    }
    kprint("VMD: init done controllers=%u\n", g_ctrl_count);
}

int vmd_ready(void) {
    return g_ctrl_count > 0;
}

uint8_t vmd_controller_count(void) {
    return g_ctrl_count;
}

int vmd_select_controller(uint8_t index) {
    if (index >= g_ctrl_count) return 0;
    g_active_ctrl = index;
    return 1;
}

uint8_t vmd_selected_controller(void) {
    return g_active_ctrl;
}

uint64_t vmd_cfgbar_base(void) {
    struct vmd_controller *c = vmd_active();
    return c ? c->cfgbar_phys : 0;
}

uint64_t vmd_cfgbar_size(void) {
    struct vmd_controller *c = vmd_active();
    return c ? c->cfgbar_size : 0;
}

uint8_t vmd_bus_start(void) {
    struct vmd_controller *c = vmd_active();
    return c ? c->bus_start : 0;
}

uint16_t vmd_bus_count(void) {
    struct vmd_controller *c = vmd_active();
    return c ? c->bus_count : 0;
}

uint32_t vmd_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    struct vmd_controller *c = vmd_active();
    uint16_t bus_limit = 0;
    if (!c) return 0xFFFFFFFFu;
    if (bus < c->bus_start) return 0xFFFFFFFFu;
    bus_limit = (uint16_t)c->bus_start + (uint16_t)c->bus_count;
    if ((uint16_t)bus >= bus_limit) return 0xFFFFFFFFu;

    uint64_t off = ((uint64_t)(bus - c->bus_start) << 20) |
                   ((uint64_t)slot << 15) |
                   ((uint64_t)func << 12) |
                   (uint64_t)(offset & 0xFFC);
    volatile uint32_t *p =
        (volatile uint32_t *)(uintptr_t)phys_to_virt(c->cfgbar_phys + off);
    return *p;
}

void vmd_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func,
                     uint8_t offset, uint32_t value) {
    struct vmd_controller *c = vmd_active();
    uint16_t bus_limit = 0;
    if (!c) return;
    if (bus < c->bus_start) return;
    bus_limit = (uint16_t)c->bus_start + (uint16_t)c->bus_count;
    if ((uint16_t)bus >= bus_limit) return;

    uint64_t off = ((uint64_t)(bus - c->bus_start) << 20) |
                   ((uint64_t)slot << 15) |
                   ((uint64_t)func << 12) |
                   (uint64_t)(offset & 0xFFC);
    volatile uint32_t *p =
        (volatile uint32_t *)(uintptr_t)phys_to_virt(c->cfgbar_phys + off);
    *p = value;
}
