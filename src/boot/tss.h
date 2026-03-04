#ifndef TATER_TSS_H
#define TATER_TSS_H

#include <stdint.h>

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

extern struct tss64 g_tss;

void tss_init(uint64_t rsp0_top);
void tss_init_local(struct tss64 *tss, uint64_t rsp0_top, uint64_t ist1_top, uint64_t ist2_top);
void tss_set_rsp0(uint64_t rsp0_top);
void tss_set_rsp0_local(struct tss64 *tss, uint64_t rsp0_top);
void tss_load(uint16_t sel);

#endif
