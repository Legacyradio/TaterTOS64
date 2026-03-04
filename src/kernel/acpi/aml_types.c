// AML object types

#include <stdint.h>
#include "aml_types.h"
#include "../../drivers/smp/spinlock.h"

/* Monotonic arena for AML runtime objects.
   4 MiB gives headroom for full ACPI namespace + device enumeration growth.
   Kernel load address is at 32 MB (BOOT_LMA=0x2000000) so BSS growth
   does not collide with firmware ACPI tables in low memory. */
static uint8_t aml_obj_arena[4 * 1024 * 1024];
static uint32_t aml_obj_off;
static spinlock_t aml_obj_lock = {0};

struct acpi_object *aml_obj_alloc(uint8_t type) {
    uint64_t flags = spin_lock_irqsave(&aml_obj_lock);
    uint32_t size = (uint32_t)sizeof(struct acpi_object);
    size = (size + 7u) & ~7u;
    if (aml_obj_off + size > sizeof(aml_obj_arena)) {
        spin_unlock_irqrestore(&aml_obj_lock, flags);
        return 0;
    }
    struct acpi_object *o = (struct acpi_object *)(aml_obj_arena + aml_obj_off);
    aml_obj_off += size;
    for (uint32_t i = 0; i < sizeof(struct acpi_object); i++) {
        ((uint8_t *)o)[i] = 0;
    }
    o->type = type;
    spin_unlock_irqrestore(&aml_obj_lock, flags);
    return o;
}

struct acpi_object *aml_obj_make_int(uint64_t v) {
    struct acpi_object *o = aml_obj_alloc(AML_OBJ_INTEGER);
    if (o) o->v.integer = v;
    return o;
}

struct acpi_object *aml_obj_make_str(const char *s) {
    struct acpi_object *o = aml_obj_alloc(AML_OBJ_STRING);
    if (o) o->v.string = s;
    return o;
}

struct acpi_object *aml_obj_make_buf(uint8_t *data, uint32_t len) {
    struct acpi_object *o = aml_obj_alloc(AML_OBJ_BUFFER);
    if (o) {
        o->v.buffer.data = data;
        o->v.buffer.length = len;
    }
    return o;
}

struct acpi_object *aml_obj_make_pkg(struct acpi_object **items, uint32_t count) {
    struct acpi_object *o = aml_obj_alloc(AML_OBJ_PACKAGE);
    if (o) {
        o->v.package.items = items;
        o->v.package.count = count;
    }
    return o;
}

struct acpi_object *aml_obj_make_ref(void *ptr) {
    struct acpi_object *o = aml_obj_alloc(AML_OBJ_REFERENCE);
    if (o) o->v.ref = ptr;
    return o;
}

uint64_t aml_obj_to_int(struct acpi_object *obj) {
    if (!obj) return 0;
    if (obj->type == AML_OBJ_INTEGER) return obj->v.integer;
    return 0;
}
