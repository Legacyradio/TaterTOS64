// ACPI events and SCI handler

#include <stdint.h>
#include "events.h"
#include "fadt.h"
#include "aml_exec.h"
#include "namespace.h"
#include "power.h"
#include "../irq/manage.h"
#include "../proc/process.h"
#include "../proc/sched.h"

void kprint(const char *fmt, ...);

static uint8_t sci_vector = 0;
static acpi_gpe_handler_t gpe_handlers[256];
static uint8_t gpe_levels[256];
static volatile uint64_t gpe_pending[4];
static int gpe_worker_started;
static int gpe_inited;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static int gpe_reg_for(uint32_t gpe, uint16_t *status_port, uint16_t *enable_port, uint8_t *bit) {
    uint32_t gpe0 = fadt_get_gpe0_blk();
    uint32_t gpe1 = fadt_get_gpe1_blk();
    uint8_t gpe0_len = fadt_get_gpe0_len();
    uint8_t gpe1_len = fadt_get_gpe1_len();
    uint8_t gpe1_base = fadt_get_gpe1_base();
    if (gpe0 && gpe0_len) {
        uint8_t half = (uint8_t)(gpe0_len / 2);
        uint32_t max = (uint32_t)half * 8;
        if (gpe < max) {
            *status_port = (uint16_t)(gpe0 + (gpe / 8));
            *enable_port = (uint16_t)(gpe0 + half + (gpe / 8));
            *bit = (uint8_t)(gpe & 7);
            return 0;
        }
    }
    if (gpe1 && gpe1_len) {
        uint8_t half = (uint8_t)(gpe1_len / 2);
        uint32_t max = (uint32_t)half * 8;
        if (gpe >= gpe1_base && (gpe - gpe1_base) < max) {
            uint32_t idx = gpe - gpe1_base;
            *status_port = (uint16_t)(gpe1 + (idx / 8));
            *enable_port = (uint16_t)(gpe1 + half + (idx / 8));
            *bit = (uint8_t)(idx & 7);
            return 0;
        }
    }
    return -1;
}

static inline void gpe_mark_pending(uint32_t gpe) {
    if (gpe >= 256) return;
    uint32_t w = gpe >> 6;
    uint64_t m = 1ULL << (gpe & 63u);
    __sync_fetch_and_or(&gpe_pending[w], m);
}

static inline int gpe_take_pending(uint32_t gpe) {
    if (gpe >= 256) return 0;
    uint32_t w = gpe >> 6;
    uint64_t m = 1ULL << (gpe & 63u);
    uint64_t old = __sync_fetch_and_and(&gpe_pending[w], ~m);
    return (old & m) != 0;
}

static void eval_gpe_method(uint32_t gpe, int level) {
    char name[16];
    name[0] = '\\';
    name[1] = '_';
    name[2] = 'G';
    name[3] = 'P';
    name[4] = 'E';
    name[5] = '.';
    name[6] = '_';
    name[7] = level ? 'L' : 'E';
    const char *hex = "0123456789ABCDEF";
    name[8] = hex[(gpe >> 4) & 0xF];
    name[9] = hex[gpe & 0xF];
    name[10] = 0;
    struct acpi_node *n = ns_lookup(ns_root(), name);
    if (n && n->type == ACPI_NODE_METHOD) {
        aml_eval(name);
    }
}

static void gpe_worker_thread(void *arg) {
    (void)arg;
    for (;;) {
        int did_work = 0;
        for (uint32_t gpe = 0; gpe < 256; gpe++) {
            if (gpe_take_pending(gpe)) {
                eval_gpe_method(gpe, gpe_levels[gpe] ? 1 : 0);
                did_work = 1;
            }
        }
        struct fry_process *cur = proc_current();
        if (!cur) continue;
        if (!did_work) {
            sched_sleep(cur->pid, 20);
        }
        sched_yield();
    }
}

static void handle_gpe_block(uint32_t base, uint8_t len, uint32_t gpe_base_index) {
    if (!base || len < 2) return;
    uint8_t half = (uint8_t)(len / 2);
    for (uint8_t i = 0; i < half; i++) {
        uint8_t sts = inb((uint16_t)(base + i));
        uint8_t en = inb((uint16_t)(base + half + i));
        uint8_t fired = (uint8_t)(sts & en);
        if (!fired) continue;
        for (uint8_t b = 0; b < 8; b++) {
            if (fired & (1u << b)) {
                uint32_t gpe = gpe_base_index + (uint32_t)i * 8 + b;
                if (gpe_handlers[gpe]) {
                    gpe_handlers[gpe](gpe);
                } else {
                    /* Defer AML method execution to process context. */
                    gpe_mark_pending(gpe);
                }
            }
        }
        outb((uint16_t)(base + i), fired); // clear after
    }
}

void acpi_events_start_worker(void) {
    if (gpe_worker_started) return;
    struct fry_process *p = process_create_kernel(gpe_worker_thread, 0, "acpi_gpe");
    if (p) {
        sched_add(p->pid);
        gpe_worker_started = 1;
    }
}

void acpi_sci_handler(uint32_t vector, void *ctx, void *dev_id, uint64_t error) {
    (void)ctx; (void)dev_id; (void)error;
    (void)vector;
    const struct fadt_info *f = fadt_get_info();
    if (f && f->pm1a_evt_blk) {
        uint16_t sts = inw((uint16_t)f->pm1a_evt_blk);
        if (sts & 0x0200) {
            kprint("ACPI: sleep button\n");
        }
        if (sts & 0x0400) {
            kprint("ACPI: RTC alarm\n");
        }
        if (sts & 0x8000) {
            kprint("ACPI: wake\n");
        }
        if (sts & 0x0100) {
            acpi_power_button_event();
        }
        outw((uint16_t)f->pm1a_evt_blk, sts);
    }
    if (f && f->pm1b_evt_blk) {
        uint16_t sts = inw((uint16_t)f->pm1b_evt_blk);
        outw((uint16_t)f->pm1b_evt_blk, sts);
    }
    handle_gpe_block(fadt_get_gpe0_blk(), fadt_get_gpe0_len(), 0);
    handle_gpe_block(fadt_get_gpe1_blk(), fadt_get_gpe1_len(), fadt_get_gpe1_base());
}

void acpi_events_init(void) {
    if (!gpe_inited) {
        for (uint32_t i = 0; i < 256; i++) gpe_levels[i] = 1;
        for (uint32_t i = 0; i < 4; i++) gpe_pending[i] = 0;
        gpe_inited = 1;
    }
    uint8_t sci_irq = fadt_get_sci_irq();
    if (sci_irq) {
        sci_vector = (uint8_t)(32 + sci_irq);
        request_irq(sci_vector, acpi_sci_handler, 0, "acpi_sci", 0);
    }
}

int acpi_install_gpe_handler(uint32_t gpe, acpi_gpe_handler_t handler, int level) {
    if (gpe >= 256) return -1;
    gpe_handlers[gpe] = handler;
    gpe_levels[gpe] = (uint8_t)(level ? 1 : 0);
    return 0;
}

void acpi_enable_gpe(uint32_t gpe) {
    uint16_t sts = 0, en = 0;
    uint8_t bit = 0;
    if (gpe_reg_for(gpe, &sts, &en, &bit) != 0) return;
    uint8_t v = inb(en);
    v |= (uint8_t)(1u << bit);
    outb(en, v);
}

void acpi_disable_gpe(uint32_t gpe) {
    uint16_t sts = 0, en = 0;
    uint8_t bit = 0;
    if (gpe_reg_for(gpe, &sts, &en, &bit) != 0) return;
    uint8_t v = inb(en);
    v &= (uint8_t)~(1u << bit);
    outb(en, v);
}
