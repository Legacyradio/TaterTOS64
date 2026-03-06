// SMP init

#include <stdint.h>
#include "smp.h"
#include "../../kernel/acpi/madt.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/vmm.h"
#include "../../kernel/mm/heap.h"
#include "../irqchip/lapic.h"
#include "../../boot/gdt.h"
#include "../../boot/tss.h"
#include "../../boot/idt.h"

void kprint(const char *fmt, ...);
void lapic_timer_init(void);
void hpet_sleep_ms(uint64_t ms);

extern uint8_t smp_trampoline;
extern uint8_t smp_trampoline_data;
extern uint8_t smp_trampoline_end;

#define SMP_MAX_CPUS 64

struct smp_trampoline_data {
    uint64_t cr3;
    uint64_t stack_top;
    uint64_t entry;
    uint32_t cpu_index;
    uint32_t apic_id;
    uint64_t ready_flag;
} __attribute__((packed));

struct smp_cpu {
    uint8_t apic_id;
    uint8_t present;
    uint8_t is_bsp;
    uint8_t pad;
    uint64_t stack_top;
    uint64_t ist1_top;
    uint64_t ist2_top;
    struct tss64 tss;
    struct gdt_block gdt;
    struct gdt_ptr gdtp;
};

static struct smp_cpu cpus[SMP_MAX_CPUS];
static volatile uint32_t cpu_online[SMP_MAX_CPUS];
static uint32_t cpu_count;
static struct tss64 *cpu_tss[SMP_MAX_CPUS];

static void mem_copy(uint8_t *dst, const uint8_t *src, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
}

/*
 * Allocate stack memory directly from PMM instead of the bump-allocator heap.
 * The heap is a fixed-size bump allocator (~1MB) that doesn't free, so ACPI
 * namespace parsing (7000+ nodes) exhausts it before SMP can allocate stacks
 * for AP cores. PMM has plenty of physical pages available.
 */
static void *alloc_stack(uint32_t size) {
    uint64_t pages = (size + 4095ULL) / 4096ULL;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) return 0;
    return (void *)(uintptr_t)vmm_phys_to_virt(phys);
}

static void ap_entry(uint32_t cpu_index, uint32_t apic_id, uint64_t ready_flag) {
    if (cpu_index >= SMP_MAX_CPUS) {
        for (;;) __asm__ volatile("hlt");
    }
    struct smp_cpu *c = &cpus[cpu_index];
    (void)apic_id;

    gdt_load(&c->gdtp);
    gdt_reload_data_segments();
    tss_init_local(&c->tss, c->stack_top, c->ist1_top, c->ist2_top);
    tss_load(0x30);
    cpu_tss[cpu_index] = &c->tss;

    idt_init();
    lapic_init();
    lapic_timer_init();

    cpu_online[cpu_index] = 1;
    if (ready_flag) {
        *(volatile uint32_t *)(uintptr_t)ready_flag = 1;
    }
    for (;;) {
        __asm__ volatile("hlt");
    }
}

uint32_t smp_cpu_count(void) {
    return cpu_count;
}

uint8_t smp_cpu_apic_id(uint32_t idx) {
    if (idx >= cpu_count) return 0;
    return cpus[idx].apic_id;
}

uint32_t smp_bsp_index(void) {
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpus[i].is_bsp) return i;
    }
    return 0;
}

struct tss64 *smp_get_tss(uint32_t cpu) {
    if (cpu >= cpu_count) return 0;
    return cpu_tss[cpu];
}

void smp_init(void) {
    cpu_count = 0;

    uint32_t cnt = madt_get_lapic_count();
    uint8_t bsp_apic = lapic_get_id();

    for (uint32_t i = 0; i < cnt && cpu_count < SMP_MAX_CPUS; i++) {
        const struct madt_lapic *lap = madt_get_lapic(i);
        if (!lap) continue;
        if (lap->flags & 1) {
            cpus[cpu_count].apic_id = lap->apic_id;
            cpus[cpu_count].present = 1;
            cpus[cpu_count].is_bsp = (lap->apic_id == bsp_apic);
            cpu_count++;
        }
    }

    if (cpu_count == 0) {
        cpu_count = 1;
        cpus[0].apic_id = bsp_apic;
        cpus[0].present = 1;
        cpus[0].is_bsp = 1;
    }
    cpu_tss[0] = &g_tss;

    kprint("SMP: detected %u CPU(s)\n", cpu_count);

    uint32_t tramp_size = (uint32_t)(&smp_trampoline_end - &smp_trampoline);
    uint64_t tramp_phys = 0x00008000ULL;
    uint8_t *tramp = (uint8_t *)(uintptr_t)vmm_phys_to_virt(tramp_phys);
    mem_copy(tramp, &smp_trampoline, tramp_size);

    uint32_t data_off = (uint32_t)(&smp_trampoline_data - &smp_trampoline);
    struct smp_trampoline_data *td = (struct smp_trampoline_data *)(tramp + data_off);
    td->cr3 = vmm_get_kernel_pml4_phys();
    td->entry = (uint64_t)(uintptr_t)ap_entry;

    for (uint32_t i = 0; i < cpu_count; i++) {
        cpu_online[i] = cpus[i].is_bsp ? 1 : 0;
    }

    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpus[i].is_bsp) continue;

        uint8_t *kstack = (uint8_t *)alloc_stack(16384);
        uint8_t *ist1 = (uint8_t *)alloc_stack(8192);
        uint8_t *ist2 = (uint8_t *)alloc_stack(8192);
        if (!kstack || !ist1 || !ist2) {
            kprint("SMP: stack alloc failed for CPU%u\n", i);
            continue;
        }
        cpus[i].stack_top = (uint64_t)(uintptr_t)(kstack + 16384);
        cpus[i].ist1_top = (uint64_t)(uintptr_t)(ist1 + 8192);
        cpus[i].ist2_top = (uint64_t)(uintptr_t)(ist2 + 8192);
        gdt_build(&cpus[i].gdt, &cpus[i].tss, &cpus[i].gdtp);

        td->stack_top = cpus[i].stack_top;
        td->cpu_index = i;
        td->apic_id = cpus[i].apic_id;
        td->ready_flag = (uint64_t)(uintptr_t)&cpu_online[i];

        lapic_send_ipi(cpus[i].apic_id, 0, 0x5); // INIT
        hpet_sleep_ms(10);                         // 10ms INIT assert (spec minimum)
        uint8_t vector = (uint8_t)(tramp_phys >> 12);
        lapic_send_ipi(cpus[i].apic_id, vector, 0x6); // SIPI #1
        hpet_sleep_ms(2);                          // 2ms SIPI #1 settle

        // Intel MP spec: send a second SIPI in case the first was missed.
        // A second SIPI is ignored by an AP that already started executing.
        // Poll briefly first; skip second SIPI if AP is already online.
        uint32_t waited = 0;
        while (!cpu_online[i] && waited < 100) {
            hpet_sleep_ms(1);
            waited++;
        }
        if (!cpu_online[i]) {
            lapic_send_ipi(cpus[i].apic_id, vector, 0x6); // SIPI #2 (Intel spec)
            hpet_sleep_ms(2);                              // 2ms SIPI #2 settle
        }
        // Wait up to 2000ms total. Real hardware CPUs in deep C-states can take
        // longer than 500ms to start; the AP is known to be running at this point
        // (LAPIC timer message visible in serial) but the BSP was timing out
        // while the AP held kprint_lock inside lapic_timer_init.
        while (!cpu_online[i] && waited < 2000) {
            hpet_sleep_ms(1);
            waited++;
        }
        if (cpu_online[i]) {
            kprint("SMP: CPU%u online (APIC %u)\n", i, cpus[i].apic_id);
        } else {
            kprint("SMP: CPU%u failed to start\n", i);
        }
    }
}
