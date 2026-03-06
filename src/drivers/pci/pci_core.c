// PCI enumeration

#include <stdint.h>
#include "pci.h"
#include "../../kernel/drivers/bus.h"
#include "../../kernel/drivers/device.h"
#include "../../kernel/drivers/driver.h"
#include "../../kernel/acpi/mcfg.h"

void kprint(const char *fmt, ...);

static int pci_match(struct fry_device *dev, struct fry_driver *drv) {
    if (!dev || !drv || !dev->name || !drv->name) return 0;
    const char *a = dev->name;
    const char *b = drv->name;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static struct fry_bus_type pci_bus = {
    .name = "pci",
    .match = pci_match,
};

static struct fry_device pci_devs[256];
static char pci_names[256][16];
static uint32_t pci_dev_count;
static struct pci_device_info pci_info[256];

static void pci_add_device(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t vendor, uint16_t device,
                           uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    if (pci_dev_count >= 256) return;

    char *name = pci_names[pci_dev_count];
    // name like pci:bb:ss.f
    name[0] = 'p'; name[1] = 'c'; name[2] = 'i'; name[3] = ':';
    name[4] = "0123456789ABCDEF"[(bus >> 4) & 0xF];
    name[5] = "0123456789ABCDEF"[bus & 0xF];
    name[6] = ':';
    name[7] = "0123456789ABCDEF"[(slot >> 4) & 0xF];
    name[8] = "0123456789ABCDEF"[slot & 0xF];
    name[9] = '.';
    name[10] = "0123456789ABCDEF"[func & 0xF];
    name[11] = 0;

    struct fry_device *d = &pci_devs[pci_dev_count++];
    d->name = name;
    d->bus = &pci_bus;
    d->driver = 0;
    d->driver_data = 0;
    device_register(d);

    struct pci_device_info *info = &pci_info[pci_dev_count - 1];
    info->bus = bus;
    info->slot = slot;
    info->func = func;
    info->vendor_id = vendor;
    info->device_id = device;
    info->class_code = class_code;
    info->subclass = subclass;
    info->prog_if = prog_if;
    info->bar0 = pci_ecam_read32(0, bus, slot, func, 0x10);
    info->bar1 = pci_ecam_read32(0, bus, slot, func, 0x14);
    info->command = (uint16_t)(pci_ecam_read32(0, bus, slot, func, 0x04) & 0xFFFF);
    info->intx_pin = 0xFF;
    info->gsi = 0;

    kprint("PCI %u:%u.%u vid=%04x did=%04x class=%02x:%02x:%02x\n",
           bus, slot, func, vendor, device, class_code, subclass, prog_if);
}

static void pci_enum_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t v = pci_ecam_read32(0, bus, slot, func, 0x0);
            uint16_t vendor = (uint16_t)(v & 0xFFFF);
            if (vendor == 0xFFFF) {
                if (func == 0) break;
                continue;
            }
            uint16_t device = (uint16_t)((v >> 16) & 0xFFFF);
            uint32_t class_reg = pci_ecam_read32(0, bus, slot, func, 0x8);
            uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFF);
            uint8_t subclass = (uint8_t)((class_reg >> 16) & 0xFF);
            uint8_t prog_if = (uint8_t)((class_reg >> 8) & 0xFF);

            pci_add_device(bus, slot, func, vendor, device, class_code, subclass, prog_if);
        }
    }
}

void pci_enum_all(void) {
    pci_dev_count = 0;
    bus_register(&pci_bus);
    // Only scan buses that actually have ECAM entries — scanning all 256 buses
    // wastes time on holes and can trigger unnecessary vmm_ensure_physmap calls.
    uint32_t ecam_count = mcfg_get_ecam_count();
    for (uint32_t i = 0; i < ecam_count; i++) {
        const struct mcfg_ecam *e = mcfg_get_ecam(i);
        if (!e) continue;
        for (uint16_t bus = e->start_bus; bus <= e->end_bus; bus++) {
            pci_enum_bus((uint8_t)bus);
        }
    }
}

const struct pci_device_info *pci_get_devices(uint32_t *count) {
    if (count) *count = pci_dev_count;
    return pci_info;
}

void pci_inject_device(uint8_t bus, uint8_t slot, uint8_t func,
                       uint16_t vendor_id, uint16_t device_id,
                       uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                       uint32_t bar0, uint32_t bar1, uint16_t command) {
    if (pci_dev_count >= 256) return;

    char *name = pci_names[pci_dev_count];
    name[0] = 'v'; name[1] = 'm'; name[2] = 'd'; name[3] = ':';
    name[4] = "0123456789ABCDEF"[(bus >> 4) & 0xF];
    name[5] = "0123456789ABCDEF"[bus & 0xF];
    name[6] = ':';
    name[7] = "0123456789ABCDEF"[(slot >> 4) & 0xF];
    name[8] = "0123456789ABCDEF"[slot & 0xF];
    name[9] = '.';
    name[10] = "0123456789ABCDEF"[func & 0xF];
    name[11] = 0;

    struct fry_device *d = &pci_devs[pci_dev_count++];
    d->name = name;
    d->bus = &pci_bus;
    d->driver = 0;
    d->driver_data = 0;
    device_register(d);

    struct pci_device_info *info = &pci_info[pci_dev_count - 1];
    info->bus = bus;
    info->slot = slot;
    info->func = func;
    info->vendor_id = vendor_id;
    info->device_id = device_id;
    info->class_code = class_code;
    info->subclass = subclass;
    info->prog_if = prog_if;
    info->bar0 = bar0;
    info->bar1 = bar1;
    info->command = command;
    info->intx_pin = 0xFF;
    info->gsi = 0;

    kprint("PCI(VMD) %u:%u.%u vid=%04x did=%04x class=%02x:%02x:%02x\n",
           bus, slot, func, vendor_id, device_id, class_code, subclass, prog_if);
}

int pci_set_irq(uint8_t bus, uint8_t slot, uint8_t func, uint8_t pin, uint32_t gsi) {
    for (uint32_t i = 0; i < pci_dev_count; i++) {
        if (pci_info[i].bus == bus && pci_info[i].slot == slot && pci_info[i].func == func) {
            pci_info[i].intx_pin = pin;
            pci_info[i].gsi = gsi;
            return 0;
        }
    }
    return -1;
}
