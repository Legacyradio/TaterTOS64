// ACPI notify dispatch

#include <stdint.h>
#include "notify.h"
#include "namespace.h"
#include "aml_exec.h"
#include "extended.h"

void kprint(const char *fmt, ...);

struct notify_entry {
    struct acpi_node *node;
    acpi_notify_handler_t handler;
};

static struct notify_entry notify_table[64];
static uint32_t notify_count;

int acpi_install_notify_handler(struct acpi_node *node, acpi_notify_handler_t handler) {
    if (!node || !handler) return -1;
    if (notify_count >= 64) return -1;
    notify_table[notify_count].node = node;
    notify_table[notify_count].handler = handler;
    notify_count++;
    return 0;
}

void acpi_notify_dispatch(struct acpi_node *node, uint32_t value) {
    if (!node) return;
    char path[256];
    ns_build_path(node, path, sizeof(path));
    kprint("ACPI: Notify %s val=0x%x\n", path, value);

    for (uint32_t i = 0; i < notify_count; i++) {
        if (notify_table[i].node == node && notify_table[i].handler) {
            notify_table[i].handler(node, value);
            return;
        }
    }

    if (node->type == ACPI_NODE_THERMAL) {
        acpi_thermal_poll_once();
        return;
    }

    if (ns_hid_match(node, "PNP0C0A")) {
        acpi_battery_refresh();
        return;
    }
    if (ns_hid_match(node, "ACPI0008")) {
        acpi_backlight_refresh();
        return;
    }
}
