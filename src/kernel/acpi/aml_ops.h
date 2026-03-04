#ifndef TATER_ACPI_AML_OPS_H
#define TATER_ACPI_AML_OPS_H

// AML opcode implementations (core + extended)

#include <stdint.h>

struct acpi_node;
struct acpi_object;

void aml_extended_init(void);
struct acpi_object *aml_read_field(struct acpi_node *field);
int aml_write_field(struct acpi_node *field, uint64_t value);

#endif
