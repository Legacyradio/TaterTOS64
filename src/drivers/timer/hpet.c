// HPET driver

#include <stdint.h>
#include "../../kernel/acpi/hpet_tbl.h"
#include "../../kernel/mm/vmm.h"

void kprint(const char *fmt, ...);

struct hpet_regs {
    uint64_t cap_id;     // 0x000  GCAP_ID
    uint64_t rsvd0;      // 0x008  reserved
    uint64_t config;     // 0x010  GEN_CONF
    uint64_t rsvd1;      // 0x018  reserved  ← was missing, shifted counter to 0xE8
    uint64_t isr;        // 0x020  GINTR_STA
    uint64_t rsvd2[25];  // 0x028 - 0x0EF  reserved (25*8=200 bytes)
    uint64_t counter;    // 0x0F0  MAIN_CTR
} __attribute__((packed));

static volatile struct hpet_regs *hpet;
static uint64_t hpet_period_fs;
static uint64_t hpet_freq_hz;

static uint64_t read_counter(void) {
    return hpet ? hpet->counter : 0;
}

uint64_t hpet_get_freq_hz(void) {
    return hpet_freq_hz;
}

uint64_t hpet_read_counter(void) {
    return read_counter();
}

void hpet_sleep_ms(uint64_t ms) {
    if (!hpet || hpet_freq_hz == 0) return;
    uint64_t start = read_counter();
    uint64_t ticks = (hpet_freq_hz * ms) / 1000ULL;
    while ((read_counter() - start) < ticks) {
        __asm__ volatile("pause");
    }
}

void hpet_init(void) {
    uint64_t base = hpet_tbl_get_base();
    if (!base) {
        kprint("HPET: no table base\n");
        return;
    }
    vmm_ensure_physmap_uc(base + 0x1000);
    hpet = (volatile struct hpet_regs *)(uintptr_t)vmm_phys_to_virt(base);

    uint64_t cap = hpet->cap_id;
    hpet_period_fs = cap >> 32; // femtoseconds per tick
    if (hpet_period_fs == 0) {
        kprint("HPET: invalid period\n");
        return;
    }

    hpet_freq_hz = 1000000000000000ULL / hpet_period_fs;

    // Disable, reset counter, enable
    hpet->config &= ~1ULL;
    hpet->counter = 0;
    hpet->config |= 1ULL;

    kprint("HPET: enabled freq=%llu Hz\n", hpet_freq_hz);
}
