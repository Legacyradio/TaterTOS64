// AML opcode implementations (core + extended)

#include <stdint.h>
#include "aml_ops.h"
#include "namespace.h"
#include "aml_types.h"
#include "extended.h"

#include "ec.h"

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static uint8_t read_region8(struct acpi_node *region, uint64_t offset) {
    if (!region) return 0;
    uint64_t addr = region->u.op_region.offset + offset;
    if (region->u.op_region.space == 0x00) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)addr;
        return *p;
    }
    if (region->u.op_region.space == 0x01) {
        return inb((uint16_t)addr);
    }
    if (region->u.op_region.space == 0x03) {
        if (!ec_available()) return 0;
        uint8_t val = 0;
        if (ec_read((uint8_t)offset, &val)) return val;
    }
    return 0;
}

static void write_region8(struct acpi_node *region, uint64_t offset, uint8_t val) {
    if (!region) return;
    uint64_t addr = region->u.op_region.offset + offset;
    if (region->u.op_region.space == 0x00) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)addr;
        *p = val;
        return;
    }
    if (region->u.op_region.space == 0x01) {
        outb((uint16_t)addr, val);
    }
    if (region->u.op_region.space == 0x03) {
        if (!ec_available()) return;
        ec_write((uint8_t)offset, val);
    }
}

int aml_write_field(struct acpi_node *field, uint64_t value);

struct acpi_object *aml_read_field(struct acpi_node *field) {
    if (!field || field->type != ACPI_NODE_FIELD) return 0;
    struct acpi_node *region = field->u.field.region;
    if (field->u.field.field_type == 1 && field->u.field.bank_reg) {
        aml_write_field(field->u.field.bank_reg, field->u.field.bank_value);
    }
    if (field->u.field.field_type == 2 && field->u.field.index_reg && field->u.field.data_reg) {
        uint32_t bit_offset = field->u.field.bit_offset;
        uint32_t bit_length = field->u.field.bit_length;
        if (bit_length == 0 || bit_length > 64) return 0;

        uint32_t byte_off = bit_offset / 8;
        uint32_t bit_in_byte = bit_offset % 8;
        uint32_t bits_total = bit_in_byte + bit_length;
        uint32_t bytes = (bits_total + 7) / 8;
        uint64_t raw = 0;
        for (uint32_t i = 0; i < bytes; i++) {
            aml_write_field(field->u.field.index_reg, byte_off + i);
            struct acpi_object *b = aml_read_field(field->u.field.data_reg);
            uint64_t v = aml_obj_to_int(b) & 0xFF;
            raw |= v << (i * 8);
        }
        raw >>= bit_in_byte;
        if (bit_length < 64) {
            raw &= ((1ULL << bit_length) - 1ULL);
        }
        return aml_obj_make_int(raw);
    }
    if (!region || region->type != ACPI_NODE_OP_REGION) return 0;

    uint32_t bit_offset = field->u.field.bit_offset;
    uint32_t bit_length = field->u.field.bit_length;
    if (bit_length == 0 || bit_length > 64) return 0;

    uint32_t byte_off = bit_offset / 8;
    uint32_t bit_in_byte = bit_offset % 8;
    uint32_t bits_total = bit_in_byte + bit_length;
    uint32_t bytes = (bits_total + 7) / 8;
    uint64_t raw = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        raw |= ((uint64_t)read_region8(region, byte_off + i)) << (i * 8);
    }
    raw >>= bit_in_byte;
    if (bit_length < 64) {
        raw &= ((1ULL << bit_length) - 1ULL);
    }

    return aml_obj_make_int(raw);
}

int aml_write_field(struct acpi_node *field, uint64_t value) {
    if (!field || field->type != ACPI_NODE_FIELD) return -1;
    struct acpi_node *region = field->u.field.region;
    if (field->u.field.field_type == 1 && field->u.field.bank_reg) {
        aml_write_field(field->u.field.bank_reg, field->u.field.bank_value);
    }
    if (field->u.field.field_type == 2 && field->u.field.index_reg && field->u.field.data_reg) {
        uint32_t bit_offset = field->u.field.bit_offset;
        uint32_t bit_length = field->u.field.bit_length;
        if (bit_length == 0 || bit_length > 64) return -1;

        uint32_t byte_off = bit_offset / 8;
        uint32_t bit_in_byte = bit_offset % 8;
        uint32_t bits_total = bit_in_byte + bit_length;
        uint32_t bytes = (bits_total + 7) / 8;

        uint64_t raw = value;
        raw <<= bit_in_byte;
        for (uint32_t i = 0; i < bytes; i++) {
            uint8_t b = (uint8_t)((raw >> (i * 8)) & 0xFF);
            aml_write_field(field->u.field.index_reg, byte_off + i);
            aml_write_field(field->u.field.data_reg, b);
        }
        return 0;
    }
    if (!region || region->type != ACPI_NODE_OP_REGION) return -1;

    uint32_t bit_offset = field->u.field.bit_offset;
    uint32_t bit_length = field->u.field.bit_length;
    if (bit_length == 0 || bit_length > 64) return -1;

    uint32_t byte_off = bit_offset / 8;
    uint32_t bit_in_byte = bit_offset % 8;
    uint32_t bits_total = bit_in_byte + bit_length;
    uint32_t bytes = (bits_total + 7) / 8;

    uint64_t raw = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        raw |= ((uint64_t)read_region8(region, byte_off + i)) << (i * 8);
    }
    uint64_t mask = (bit_length == 64) ? ~0ULL : ((1ULL << bit_length) - 1ULL);
    raw &= ~(mask << bit_in_byte);
    raw |= (value & mask) << bit_in_byte;
    for (uint32_t i = 0; i < bytes; i++) {
        write_region8(region, byte_off + i, (uint8_t)((raw >> (i * 8)) & 0xFF));
    }
    return 0;
}

void aml_extended_init(void) {
    acpi_extended_init();
}
