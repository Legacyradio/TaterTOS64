#ifndef TATER_ACPI_MCFG_H
#define TATER_ACPI_MCFG_H

#include <stdint.h>

struct mcfg_ecam {
    uint64_t base_addr;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
};

void mcfg_init(void);
uint32_t mcfg_get_ecam_count(void);
const struct mcfg_ecam *mcfg_get_ecam(uint32_t idx);
uint64_t mcfg_get_ecam_base(uint16_t segment, uint8_t bus);

#endif
