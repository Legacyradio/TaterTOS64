#ifndef TATER_ACPI_AML_TYPES_H
#define TATER_ACPI_AML_TYPES_H

#include <stdint.h>

#define AML_OBJ_INTEGER 1
#define AML_OBJ_STRING  2
#define AML_OBJ_BUFFER  3
#define AML_OBJ_PACKAGE 4
#define AML_OBJ_REFERENCE 5
#define AML_OBJ_METHOD  6
#define AML_OBJ_DEVICE  7

struct acpi_object;

struct aml_package {
    struct acpi_object **items;
    uint32_t count;
};

struct aml_buffer {
    uint8_t *data;
    uint32_t length;
};

struct acpi_object {
    uint8_t type;
    union {
        uint64_t integer;
        const char *string;
        struct aml_buffer buffer;
        struct aml_package package;
        void *ref;
    } v;
};

struct acpi_object *aml_obj_alloc(uint8_t type);
struct acpi_object *aml_obj_make_int(uint64_t v);
struct acpi_object *aml_obj_make_str(const char *s);
struct acpi_object *aml_obj_make_buf(uint8_t *data, uint32_t len);
struct acpi_object *aml_obj_make_pkg(struct acpi_object **items, uint32_t count);
struct acpi_object *aml_obj_make_ref(void *ptr);
uint64_t aml_obj_to_int(struct acpi_object *obj);

#endif
