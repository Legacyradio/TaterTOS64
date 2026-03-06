#ifndef TATER_ACPI_EVENTS_H
#define TATER_ACPI_EVENTS_H

#include <stdint.h>

void acpi_events_init(void);
void acpi_events_start_worker(void);
void acpi_sci_handler(uint32_t vector, void *ctx, void *dev_id, uint64_t error);

typedef void (*acpi_gpe_handler_t)(uint32_t gpe);
int acpi_install_gpe_handler(uint32_t gpe, acpi_gpe_handler_t handler, int level);
void acpi_enable_gpe(uint32_t gpe);
void acpi_disable_gpe(uint32_t gpe);

#endif
