#ifndef TATER_ACPI_RESOURCES_H
#define TATER_ACPI_RESOURCES_H

#include <stdint.h>

void acpi_decode_resources(const uint8_t *buf, uint32_t len);
int acpi_get_gsi_from_crs(const uint8_t *buf, uint32_t len, uint32_t *gsi_out);
int acpi_get_io_from_crs(const uint8_t *buf, uint32_t len,
                         uint16_t *ports, uint32_t max_ports);

#endif
