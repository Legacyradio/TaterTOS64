// GDT setup for TaterTOS64v3

#include <stdint.h>
#include "gdt.h"
#include "tss.h"

static struct gdt_block gdt_block;
static struct gdt_ptr gdtp;

static void set_gdt_entry(struct gdt_entry *e, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
    e->limit_low = (uint16_t)(limit & 0xFFFF);
    e->base_low = (uint16_t)(base & 0xFFFF);
    e->base_mid = (uint8_t)((base >> 16) & 0xFF);
    e->access = access;
    e->gran = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    e->base_high = (uint8_t)((base >> 24) & 0xFF);
}

static void set_tss_descriptor(struct gdt_tss_entry *tss, uint64_t base, uint32_t limit) {
    tss->limit_low = (uint16_t)(limit & 0xFFFF);
    tss->base_low = (uint16_t)(base & 0xFFFF);
    tss->base_mid = (uint8_t)((base >> 16) & 0xFF);
    tss->access = 0x89; // present, type=64-bit TSS (available)
    tss->gran = (uint8_t)(((limit >> 16) & 0x0F));
    tss->base_high = (uint8_t)((base >> 24) & 0xFF);
    tss->base_upper = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    tss->reserved = 0;
}

void gdt_build(struct gdt_block *blk, struct tss64 *tss, struct gdt_ptr *out) {
    if (!blk || !tss || !out) return;
    set_gdt_entry(&blk->gdt[0], 0, 0, 0, 0);
    set_gdt_entry(&blk->gdt[1], 0, 0, 0x9A, 0x20);
    set_gdt_entry(&blk->gdt[2], 0, 0, 0x92, 0x00);
    set_gdt_entry(&blk->gdt[3], 0, 0, 0xFA, 0x20);
    set_gdt_entry(&blk->gdt[4], 0, 0, 0xF2, 0x00);
    // gdt[5] at offset 0x28: user 64-bit code (sel 0x2B = 0x28|RPL3)
    // Required for sysretq: CS = STAR[63:48]+16|3 = (0x18+16)|3 = 0x2B
    set_gdt_entry(&blk->gdt[5], 0, 0, 0xFA, 0x20);

    set_tss_descriptor(&blk->tss, (uint64_t)tss, sizeof(struct tss64) - 1);

    out->base = (uint64_t)blk;
    out->limit = (uint16_t)(sizeof(*blk) - 1);
}

void gdt_load(struct gdt_ptr *p) {
    __asm__ volatile("lgdt (%0)" : : "r"(p));
}

void gdt_reload_data_segments(void) {
    __asm__ volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        :
        :
        : "rax");
}

void gdt_init(void) {
    gdt_build(&gdt_block, &g_tss, &gdtp);
    gdt_load(&gdtp);

    // Reload segment selectors (including CS)
    __asm__ volatile(
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        :
        :
        : "rax");
}

// Expose the TSS descriptor in memory so the linker doesn't drop it
__attribute__((used))
static struct gdt_tss_entry *gdt_tss_ref = &gdt_block.tss;
