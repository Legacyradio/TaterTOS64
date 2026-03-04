#ifndef TATER_ACPI_NOTIFY_H
#define TATER_ACPI_NOTIFY_H

#include <stdint.h>
#include "namespace.h"

typedef void (*acpi_notify_handler_t)(struct acpi_node *node, uint32_t value);
int acpi_install_notify_handler(struct acpi_node *node, acpi_notify_handler_t handler);
void acpi_notify_dispatch(struct acpi_node *node, uint32_t value);

#endif
