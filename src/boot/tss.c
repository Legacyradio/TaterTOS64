// TSS setup for TaterTOS64v3

#include <stdint.h>
#include "tss.h"

struct tss64 g_tss;

__attribute__((aligned(16), section(".boot")))
static uint8_t boot_ist1_stack[16384];
__attribute__((aligned(16), section(".boot")))
static uint8_t boot_ist2_stack[16384];
__attribute__((aligned(16), section(".iststack")))
static uint8_t runtime_ist1_stack[16384];
__attribute__((aligned(16), section(".iststack")))
static uint8_t runtime_ist2_stack[16384];

void tss_load(uint16_t sel) {
    __asm__ volatile("ltr %0" : : "r"(sel));
}

void tss_init_local(struct tss64 *tss, uint64_t rsp0_top, uint64_t ist1_top, uint64_t ist2_top) {
    if (!tss) return;
    for (uint32_t i = 0; i < sizeof(*tss); i++) {
        ((volatile uint8_t *)tss)[i] = 0;
    }
    tss->rsp0 = rsp0_top;
    tss->ist1 = ist1_top;
    tss->ist2 = ist2_top;
    tss->iomap_base = sizeof(struct tss64);
}

void tss_init(uint64_t rsp0_top) {
    tss_init_local(&g_tss, rsp0_top,
                   (uint64_t)(boot_ist1_stack + sizeof(boot_ist1_stack)),
                   (uint64_t)(boot_ist2_stack + sizeof(boot_ist2_stack)));
    tss_load(0x30);
}

void tss_set_rsp0(uint64_t rsp0_top) {
    g_tss.rsp0 = rsp0_top;
}

void tss_set_rsp0_local(struct tss64 *tss, uint64_t rsp0_top) {
    if (tss) tss->rsp0 = rsp0_top;
}

void tss_use_runtime_ists(void) {
    g_tss.ist1 = (uint64_t)(runtime_ist1_stack + sizeof(runtime_ist1_stack));
    g_tss.ist2 = (uint64_t)(runtime_ist2_stack + sizeof(runtime_ist2_stack));
}
