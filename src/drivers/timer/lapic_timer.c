// LAPIC timer driver

#include <stdint.h>
#include "../irqchip/lapic.h"
#include "../smp/smp.h"
#include "hpet.h"
#include "../../kernel/irq/irqdesc.h"
#include "../../kernel/mm/vmm.h"
#include "../../boot/efi_handoff.h"
#include "../../boot/early_serial.h"
#include "../../include/tater_trace.h"

void kprint(const char *fmt, ...);
void sched_tick(void);
extern struct fry_handoff *g_handoff;

static uint8_t g_first_bsp_tick_seen;

static void boot_diag_stage(uint64_t stage) {
    struct fry_handoff *handoff = g_handoff;
    if (!TATER_BOOT_VISUAL_DEBUG) return;
    if (!handoff) return;
    if (!handoff->fb_base || !handoff->fb_width || !handoff->fb_height || !handoff->fb_stride) return;
    if (!handoff->boot_identity_limit || handoff->fb_base >= handoff->boot_identity_limit) return;

    uint64_t x0 = stage * 20ULL;
    if (x0 >= handoff->fb_width) return;

    uint64_t mw = 12ULL;
    uint64_t mh = 12ULL;
    uint64_t remain_w = handoff->fb_width - x0;
    if (remain_w < mw) mw = remain_w;
    if (handoff->fb_height < mh) mh = handoff->fb_height;

    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)handoff->fb_base;
    for (uint64_t y = 0; y < mh; y++) {
        uint64_t row = y * handoff->fb_stride + x0;
        for (uint64_t x = 0; x < mw; x++) {
            fb[row + x] = 0x00F0F0F0u;
        }
    }
}

static void lapic_timer_handler(uint32_t vector, void *ctx, void *dev_id, uint64_t error) {
    (void)vector; (void)ctx; (void)dev_id; (void)error;
    uint32_t count = smp_cpu_count();
    uint32_t bsp = smp_bsp_index();
    if (count == 0) {
        if (!g_first_bsp_tick_seen) {
            g_first_bsp_tick_seen = 1;
            boot_diag_stage(33);
            if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_FIRST_TICK\n");
        }
        sched_tick();
        return;
    }
    if (bsp < count && lapic_get_id() == smp_cpu_apic_id(bsp)) {
        if (!g_first_bsp_tick_seen) {
            g_first_bsp_tick_seen = 1;
            boot_diag_stage(33);
            if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_FIRST_TICK\n");
        }
        sched_tick();
    }
}

#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR  0x390
#define LAPIC_REG_TIMER_DIV  0x3E0

static volatile uint32_t *lapic;
static uint32_t lapic_ticks_per_ms;

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
}

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}

static void lapic_timer_calibrate(void) {
    uint64_t freq = hpet_get_freq_hz();
    if (freq == 0) return;

    // Set divide by 16
    lapic_write(LAPIC_REG_TIMER_DIV, 0x3);
    // Mask LVT timer
    lapic_write(LAPIC_REG_LVT_TIMER, 1 << 16);

    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);

    uint64_t start = hpet_read_counter();
    uint64_t target = start + freq / 100; // 10ms
    while (hpet_read_counter() < target) {
    }

    uint32_t cur = lapic_read(LAPIC_REG_TIMER_CUR);
    uint32_t elapsed = 0xFFFFFFFFu - cur;
    lapic_ticks_per_ms = elapsed / 10;
}

void lapic_timer_init(void) {
    uint64_t base = lapic_get_base_phys();
    if (!base) {
        kprint("LAPIC timer: no LAPIC\n");
        return;
    }
    vmm_ensure_physmap_uc(base + 0x1000);
    lapic = (volatile uint32_t *)(uintptr_t)vmm_phys_to_virt(base);

    lapic_timer_calibrate();
    if (lapic_ticks_per_ms == 0) {
        kprint("LAPIC timer: calibrate failed\n");
        return;
    }

    // Periodic mode, vector 0x40
    lapic_write(LAPIC_REG_LVT_TIMER, 0x40 | (1 << 17));
    lapic_write(LAPIC_REG_TIMER_DIV, 0x3);
    lapic_write(LAPIC_REG_TIMER_INIT, lapic_ticks_per_ms);

    irq_set_handler(0x40, lapic_timer_handler, 0);

    kprint("LAPIC timer: %u ticks/ms\n", lapic_ticks_per_ms);
}
