// ACPI power management

#include <stdint.h>
#include "power.h"
#include "fadt.h"
#include "aml_exec.h"
#include "aml_types.h"
#include "../mm/vmm.h"
#include "../../drivers/pci/pci.h"

void kprint(const char *fmt, ...);
void kernel_panic(const char *msg);

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

__attribute__((noreturn))
static void halt_forever(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static struct acpi_object *eval_method1(const char *path, uint64_t arg) {
    struct acpi_object *args[1];
    args[0] = aml_obj_make_int(arg);
    return aml_eval_with_args(path, args, 1);
}

void acpi_power_init(void) {
    const struct fadt_info *f = fadt_get_info();
    if (!f) return;
    if (f->smi_cmd && f->acpi_enable) {
        outb((uint16_t)f->smi_cmd, f->acpi_enable);
        for (uint32_t i = 0; i < 100000; i++) {
            uint16_t pm1 = inw((uint16_t)f->pm1a_cnt_blk);
            if (pm1 & 0x0001) {
                break;
            }
        }
    }
}

void acpi_reset(void) {
    const struct fadt_info *f = fadt_get_info();
    if (!f || !f->reset_reg) return;
    if (f->reset_reg_space == 1) { // SystemIO
        outb((uint16_t)f->reset_reg, f->reset_val);
        return;
    }
    if (f->reset_reg_space == 0) { // SystemMemory
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)vmm_phys_to_virt(f->reset_reg);
        *p = f->reset_val;
        return;
    }
    if (f->reset_reg_space == 2) { // PCIConfig
        uint32_t addr = f->reset_reg;
        uint8_t bus = (addr >> 24) & 0xFF;
        uint8_t dev = (addr >> 16) & 0xFF;
        uint8_t fn = (addr >> 8) & 0xFF;
        uint8_t reg = addr & 0xFF;
        uint32_t val = (uint32_t)f->reset_val;
        pci_ecam_write32(0, bus, dev, fn, reg & ~3u, val);
    }
}

void acpi_shutdown(void) {
    const struct fadt_info *f = fadt_get_info();
    if (!f) {
        __asm__ volatile("cli" ::: "memory");
        halt_forever();
    }
    struct acpi_object *s5 = aml_eval("\\_S5");
    uint16_t slp_typa = 0;
    uint16_t slp_typb = 0;
    if (s5 && s5->type == AML_OBJ_PACKAGE && s5->v.package.count >= 2) {
        slp_typa = (uint16_t)aml_obj_to_int(s5->v.package.items[0]);
        slp_typb = (uint16_t)aml_obj_to_int(s5->v.package.items[1]);
    }

    // Shutdown is terminal: do not allow further interrupts/scheduling while
    // programming PM control blocks.
    __asm__ volatile("cli" ::: "memory");

    if (f->pm1a_cnt_blk) {
        uint16_t pm1a = inw((uint16_t)f->pm1a_cnt_blk);
        pm1a &= ~(7u << 10);
        pm1a |= (slp_typa & 0x7u) << 10;
        pm1a |= (1u << 13);
        outw((uint16_t)f->pm1a_cnt_blk, pm1a);
    }
    if (f->pm1b_cnt_blk) {
        uint16_t pm1b = inw((uint16_t)f->pm1b_cnt_blk);
        pm1b &= ~(7u << 10);
        pm1b |= (slp_typb & 0x7u) << 10;
        pm1b |= (1u << 13);
        outw((uint16_t)f->pm1b_cnt_blk, pm1b);
    }
    // QEMU/Bochs poweroff fallback port.
    outw(0x0604, 0x2000);

    // If platform did not power off, freeze safely instead of returning into
    // normal control flow.
    halt_forever();
}

void acpi_power_button_event(void) {
    kprint("ACPI: power button\n");
    acpi_shutdown();
}

void acpi_enter_sleep(uint8_t state) {
    const struct fadt_info *f = fadt_get_info();
    if (!f) return;

    char s_path[8] = "\\_Sx";
    s_path[3] = (char)('0' + state);

    struct acpi_object *sobj = aml_eval(s_path);
    if (!sobj || sobj->type != AML_OBJ_PACKAGE || sobj->v.package.count < 1) {
        kprint("ACPI: sleep S%u not supported\n", state);
        return;
    }

    uint16_t slp_typa = (uint16_t)aml_obj_to_int(sobj->v.package.items[0]);
    uint16_t slp_typb = slp_typa;
    if (sobj->v.package.count >= 2) {
        slp_typb = (uint16_t)aml_obj_to_int(sobj->v.package.items[1]);
    }

    eval_method1("\\_PTS", state);
    eval_method1("\\_TTS", state);
    eval_method1("\\_GTS", state);

    __asm__ volatile("cli");

    if (f->pm1a_cnt_blk) {
        uint16_t pm1a = inw((uint16_t)f->pm1a_cnt_blk);
        pm1a &= ~(7u << 10);
        pm1a |= (slp_typa & 0x7u) << 10;
        pm1a |= (1u << 13);
        outw((uint16_t)f->pm1a_cnt_blk, pm1a);
    }
    if (f->pm1b_cnt_blk) {
        uint16_t pm1b = inw((uint16_t)f->pm1b_cnt_blk);
        pm1b &= ~(7u << 10);
        pm1b |= (slp_typb & 0x7u) << 10;
        pm1b |= (1u << 13);
        outw((uint16_t)f->pm1b_cnt_blk, pm1b);
    }

    // If we return, call wake methods
    eval_method1("\\_WAK", state);
    eval_method1("\\_BFS", state);
}
