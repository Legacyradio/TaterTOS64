// Platform bus

#include <stdint.h>
#include "platform.h"
#include "../acpi/tables.h"
#include "../../drivers/pci/pci.h"

void kprint(const char *fmt, ...);

static int platform_match(struct fry_device *dev, struct fry_driver *drv) {
    if (!dev || !drv || !dev->name || !drv->name) {
        return 0;
    }
    const char *a = dev->name;
    const char *b = drv->name;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static struct fry_bus_type platform_bus = {
    .name = "platform",
    .match = platform_match,
};

static platform_profile_t g_platform = {
    .id = PLATFORM_ID_UNKNOWN,
    .lpc_vendor = 0xFFFFu,
    .lpc_device = 0xFFFFu,
    .acpi_oem_dell = 0,
    .confidence = 0,
};

static int ascii_upper_eq(char a, char b) {
    if (a >= 'a' && a <= 'z') a = (char)(a - ('a' - 'A'));
    if (b >= 'a' && b <= 'z') b = (char)(b - ('a' - 'A'));
    return a == b;
}

static int oem_id_is_dell(const char oem_id[6]) {
    if (!oem_id) return 0;
    return ascii_upper_eq(oem_id[0], 'D') &&
           ascii_upper_eq(oem_id[1], 'E') &&
           ascii_upper_eq(oem_id[2], 'L') &&
           ascii_upper_eq(oem_id[3], 'L');
}

static int platform_detect_acpi_oem_dell(void) {
    uint32_t count = acpi_table_count();
    for (uint32_t i = 0; i < count; i++) {
        struct acpi_sdt_header *h = acpi_get_table(i);
        if (!h) continue;
        if (oem_id_is_dell(h->oem_id)) return 1;
    }
    return 0;
}

static int platform_find_lpc_id(uint16_t *vendor_out, uint16_t *device_out) {
    uint32_t count = 0;
    const struct pci_device_info *devs = pci_get_devices(&count);
    int found = 0;
    uint16_t vendor = 0xFFFFu;
    uint16_t device = 0xFFFFu;

    if (devs) {
        for (uint32_t i = 0; i < count; i++) {
            if (devs[i].class_code == 0x06u && devs[i].subclass == 0x01u) {
                vendor = devs[i].vendor_id;
                device = devs[i].device_id;
                found = 1;
                if (devs[i].bus == 0u && devs[i].slot == 0x1Fu && devs[i].func == 0u) {
                    break;
                }
            }
        }
    }

    /*
     * Fallback for early call sites: direct probe of canonical LPC/eSPI BDF.
     */
    if (!found) {
        uint32_t id = pci_ecam_read32(0, 0, 0x1F, 0, 0x00);
        if (id != 0xFFFFFFFFu) {
            vendor = (uint16_t)(id & 0xFFFFu);
            device = (uint16_t)((id >> 16) & 0xFFFFu);
            found = 1;
        }
    }

    if (!found) return 0;
    if (vendor_out) *vendor_out = vendor;
    if (device_out) *device_out = device;
    return 1;
}

const char *platform_id_name(platform_id_t id) {
    switch (id) {
    case PLATFORM_ID_DELL_PRECISION_7530:
        return "dell_precision_7530";
    default:
        return "unknown";
    }
}

void platform_detect(void) {
    platform_profile_t p = {
        .id = PLATFORM_ID_UNKNOWN,
        .lpc_vendor = 0xFFFFu,
        .lpc_device = 0xFFFFu,
        .acpi_oem_dell = 0,
        .confidence = 0,
    };

    p.acpi_oem_dell = platform_detect_acpi_oem_dell() ? 1u : 0u;
    (void)platform_find_lpc_id(&p.lpc_vendor, &p.lpc_device);

    /*
     * This is intentionally conservative: we do not have SMBIOS model strings
     * yet, so detection is based on ACPI OEM + LPC/eSPI device ID only.
     */
    if (p.acpi_oem_dell && p.lpc_vendor == 0x8086u && p.lpc_device == 0xA30Eu) {
        p.id = PLATFORM_ID_DELL_PRECISION_7530;
        p.confidence = 90;
    } else if (p.lpc_vendor == 0x8086u && p.lpc_device == 0xA30Eu) {
        p.confidence = 55;
    } else if (p.acpi_oem_dell) {
        p.confidence = 35;
    }

    g_platform = p;
    kprint("platform: profile=%s confidence=%u oem_dell=%u lpc=%04x:%04x\n",
           platform_id_name(g_platform.id),
           (uint32_t)g_platform.confidence,
           (uint32_t)g_platform.acpi_oem_dell,
           (uint32_t)g_platform.lpc_vendor,
           (uint32_t)g_platform.lpc_device);
}

void platform_init(void) {
    bus_register(&platform_bus);
}

int platform_device_add(struct fry_device *dev) {
    if (!dev) return -1;
    dev->bus = &platform_bus;
    return device_register(dev);
}

const platform_profile_t *platform_get_profile(void) {
    return &g_platform;
}
