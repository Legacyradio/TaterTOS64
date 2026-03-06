// ACPI bus driver

#include <stddef.h>
#include <stdint.h>
#include "../../kernel/acpi/namespace.h"
#include "../../kernel/acpi/aml_exec.h"
#include "../../kernel/drivers/bus.h"
#include "../../kernel/drivers/device.h"
#include "../../kernel/drivers/driver.h"
#include "../../drivers/smp/spinlock.h"

void kprint(const char *fmt, ...);

static int acpi_match(struct fry_device *dev, struct fry_driver *drv) {
    if (!dev || !drv || !dev->name || !drv->name) return 0;
    const char *a = dev->name;
    const char *b = drv->name;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static struct fry_bus_type acpi_bus = {
    .name = "acpi",
    .match = acpi_match,
};

static void node_path(struct acpi_node *node, char *out, uint32_t max) {
    if (!out || max == 0) return;
    if (!node) {
        out[0] = 0;
        return;
    }

    // Build path like \_SB_.PCI0
    const struct acpi_node *stack[32];
    uint32_t depth = 0;
    struct acpi_node *cur = node;
    while (cur && cur->parent) {
        if (depth >= 32) break;
        stack[depth++] = cur;
        cur = cur->parent;
    }

    uint32_t outp = 0;
    if (outp < max) out[outp++] = '\\';
    for (size_t i = (size_t)depth; i-- > 0;) {
        if (outp + 4 >= max) break;
        out[outp++] = stack[i]->name[0];
        out[outp++] = stack[i]->name[1];
        out[outp++] = stack[i]->name[2];
        out[outp++] = stack[i]->name[3];
        if (i != 0) {
            if (outp + 1 >= max) break;
            out[outp++] = '.';
        }
    }
    out[outp] = 0;
}

static void create_acpi_device(struct acpi_node *node, void *ctx) {
    (void)ctx;
    if (!node || node->type != ACPI_NODE_DEVICE) {
        return;
    }

    static struct fry_device devs[256];
    static char names[256][64];
    static uint32_t dev_count = 0;
    static spinlock_t dev_lock = {0};
    uint32_t idx = 0;
    char path_buf[64];
    node_path(node, path_buf, sizeof(path_buf));

    uint64_t flags = spin_lock_irqsave(&dev_lock);
    if (dev_count < 256) {
        idx = dev_count++;
    } else {
        spin_unlock_irqrestore(&dev_lock, flags);
        return;
    }

    uint32_t n = 0;
    while (n + 1 < sizeof(names[idx]) && path_buf[n]) {
        names[idx][n] = path_buf[n];
        n++;
    }
    names[idx][n] = 0;

    devs[idx].name = names[idx];
    devs[idx].bus = &acpi_bus;
    devs[idx].driver = 0;
    devs[idx].driver_data = node;
    spin_unlock_irqrestore(&dev_lock, flags);
    device_register(&devs[idx]);

    // Try _HID
    static const char hid_name[4] = {'_','H','I','D'};
    struct acpi_object *hid = 0;
    struct acpi_node *hid_node = ns_find_child(node, hid_name);
    if (hid_node && hid_node->object) {
        hid = (struct acpi_object *)hid_node->object;
    } else {
        char hid_path[80];
        uint32_t i = 0;
        uint32_t suffix_bytes = 6; // "._HID" + NUL
        uint32_t dst_cap = (uint32_t)sizeof(hid_path) - suffix_bytes;
        uint32_t src_cap = (uint32_t)sizeof(names[idx]) - 1;
        while (i < dst_cap && i < src_cap && names[idx][i]) {
            hid_path[i] = names[idx][i];
            i++;
        }
        hid_path[i++] = '.';
        hid_path[i++] = '_';
        hid_path[i++] = 'H';
        hid_path[i++] = 'I';
        hid_path[i++] = 'D';
        hid_path[i] = 0;
        hid = aml_eval(hid_path);
    }

    if (hid && hid->type == AML_OBJ_STRING) {
        kprint("ACPI dev: %s HID=%s\n", names[idx], hid->v.string);
    } else {
        kprint("ACPI dev: %s\n", names[idx]);
    }

    // _CRS not evaluated here: executing it for every device causes real
    // hardware I/O (MMIO/IO/PCI) on each device, which is extremely slow.
    // IRQ resources are resolved via _PRT separately in acpi_extended_init.
}

void acpi_bus_init(void) {
    bus_register(&acpi_bus);
    ns_walk(create_acpi_device, 0);
}
