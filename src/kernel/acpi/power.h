#ifndef TATER_ACPI_POWER_H
#define TATER_ACPI_POWER_H

#include <stdint.h>

void acpi_power_init(void);
void acpi_shutdown(void);
void acpi_reset(void);
void acpi_power_button_event(void);
void acpi_enter_sleep(uint8_t state);

#endif
