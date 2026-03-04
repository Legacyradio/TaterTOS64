#ifndef TATER_GDT_H
#define TATER_GDT_H

#include <stdint.h>
#include "tss.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t gran;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t gran;
    uint8_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct gdt_block {
    struct gdt_entry gdt[6];
    struct gdt_tss_entry tss;
} __attribute__((packed));

void gdt_init(void);
void gdt_build(struct gdt_block *blk, struct tss64 *tss, struct gdt_ptr *out);
void gdt_load(struct gdt_ptr *p);
void gdt_reload_data_segments(void);

#endif
