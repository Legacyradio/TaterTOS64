#ifndef TATER_ACPI_TABLES_H
#define TATER_ACPI_TABLES_H

#include <stdint.h>

struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

void acpi_tables_init(uint64_t rsdp_phys);
struct acpi_sdt_header *acpi_find_table(const char sig[4]);
uint32_t acpi_table_count(void);
struct acpi_sdt_header *acpi_get_table(uint32_t idx);

#endif
