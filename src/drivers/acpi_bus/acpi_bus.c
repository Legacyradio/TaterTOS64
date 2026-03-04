// ACPI bus driver

#include <stdint.h>
#include "../../kernel/acpi/namespace.h"
#include "../../kernel/acpi/aml_exec.h"
#include "../../kernel/drivers/bus.h"
#include "../../kernel/drivers/device.h"
#include "../../kernel/drivers/driver.h"

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
    // Build path like \_SB_.PCI0
    const struct acpi_node *stack[32];
    uint32_t depth = 0;
    struct acpi_node *cur = node;
    while (cur && cur->parent && depth < 32) {
        stack[depth++] = cur;
        cur = cur->parent;
    }

    uint32_t outp = 0;
    if (outp < max) out[outp++] = '\\';
    for (int i = (int)depth - 1; i >= 0; i--) {
        if (outp + 5 >= max) break;
        out[outp++] = stack[i]->name[0];
        out[outp++] = stack[i]->name[1];
        out[outp++] = stack[i]->name[2];
        out[outp++] = stack[i]->name[3];
        if (i != 0 && outp < max) out[outp++] = '.';
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
    if (dev_count >= 256) {
        return;
    }

    node_path(node, names[dev_count], sizeof(names[dev_count]));

    devs[dev_count].name = names[dev_count];
    devs[dev_count].bus = &acpi_bus;
    devs[dev_count].driver = 0;
    devs[dev_count].driver_data = node;
    device_register(&devs[dev_count]);

    // Try _HID
    char hid_path[80];
    uint32_t i = 0;
    while (names[dev_count][i] && i < sizeof(hid_path) - 6) {
        hid_path[i] = names[dev_count][i];
        i++;
    }
    hid_path[i++] = '.';
    hid_path[i++] = '_';
    hid_path[i++] = 'H';
    hid_path[i++] = 'I';
    hid_path[i++] = 'D';
    hid_path[i] = 0;

    struct acpi_object *hid = aml_eval(hid_path);
    if (hid && hid->type == AML_OBJ_STRING) {
        kprint("ACPI dev: %s HID=%s\n", names[dev_count], hid->v.string);
    } else {
        kprint("ACPI dev: %s\n", names[dev_count]);
    }

    // _CRS not evaluated here: executing it for every device causes real
    // hardware I/O (MMIO/IO/PCI) on each device, which is extremely slow.
    // IRQ resources are resolved via _PRT separately in acpi_extended_init.
    dev_count++;
}

void acpi_bus_init(void) {
    bus_register(&acpi_bus);
    ns_walk(create_acpi_device, 0);
}
