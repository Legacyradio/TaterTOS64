// LAPIC timer driver

#include <stdint.h>
#include "../irqchip/lapic.h"
#include "../smp/smp.h"
#include "hpet.h"
#include "../../kernel/irq/irqdesc.h"
#include "../../kernel/mm/vmm.h"

void kprint(const char *fmt, ...);
void sched_tick(void);

static void lapic_timer_handler(uint32_t vector, void *ctx, void *dev_id, uint64_t error) {
    (void)vector; (void)ctx; (void)dev_id; (void)error;
    uint32_t count = smp_cpu_count();
    if (count == 0) {
        sched_tick();
        return;
    }
    if (lapic_get_id() == smp_cpu_apic_id(0)) {
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
    vmm_ensure_physmap(base + 0x1000);
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
