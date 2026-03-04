#ifndef TATER_ACPI_AML_EXEC_H
#define TATER_ACPI_AML_EXEC_H

#include <stdint.h>
#include "aml_types.h"

struct acpi_object *aml_eval(const char *path);
struct acpi_object *aml_eval_with_args(const char *path, struct acpi_object **args, uint32_t arg_count);

#endif
