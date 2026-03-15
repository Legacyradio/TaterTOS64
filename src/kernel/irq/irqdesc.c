// IRQ descriptor table and dispatch

#include <stdint.h>
#include "irqdesc.h"
#include "../../boot/early_serial.h"
#include "../proc/process.h"
#include "../proc/sched.h"

void kprint(const char *fmt, ...);
void kernel_panic(const char *msg);

static struct irq_desc irq_descs[256];
static uint8_t g_pf_exit_stack[16384] __attribute__((aligned(16)));

// Kernel CR3 for interrupt handlers; set once by irq_cr3_init() before sti.
// common_isr saves the current (possibly user) CR3 in the callee-saved
// register %r15 before switching to the kernel page table.  Using a
// callee-saved register (not a global) is correct when a context switch can
// happen inside irq_dispatch: each process's kernel stack independently
// preserves its own saved CR3 through irq_dispatch's prologue/epilogue and
// through context_switch's register save/restore.
uint64_t irq_kernel_cr3 = 0;

void irq_cr3_init(uint64_t cr3) {
    irq_kernel_cr3 = cr3;
}

struct irq_desc *irq_get_desc(uint32_t vector) {
    if (vector < 256) {
        return &irq_descs[vector];
    }
    return 0;
}

void irq_desc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        irq_descs[i].handler = 0;
        irq_descs[i].dev_id = 0;
        irq_descs[i].chip = 0;
        irq_descs[i].flags = 0;
        irq_descs[i].count = 0;
        for (uint32_t j = 0; j < sizeof(irq_descs[i].name); j++) {
            irq_descs[i].name[j] = 0;
        }
    }
}

void irq_set_chip(uint32_t vector, struct irq_chip *chip) {
    if (vector < 256) {
        irq_descs[vector].chip = chip;
    }
}

void irq_set_handler(uint32_t vector, irq_handler_t handler, void *dev_id) {
    if (vector < 256) {
        irq_descs[vector].handler = handler;
        irq_descs[vector].dev_id = dev_id;
    }
}

static inline uint64_t read_cr2_irq(void) {
    uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v;
}
static inline uint64_t read_cr3_irq(void) {
    uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}

static const struct fry_vm_region *pf_find_vm_region(const struct fry_process *p,
                                                     uint64_t addr) {
    if (!p) return 0;
    for (uint32_t i = 0; i < PROC_VMREG_MAX; i++) {
        const struct fry_vm_region *r = &p->vm_regions[i];
        if (!r->used) continue;
        if (addr >= r->base && addr < r->base + r->length) return r;
    }
    return 0;
}

static const char *pf_region_kind_name(uint16_t kind) {
    switch (kind) {
        case FRY_VM_REGION_ANON_PRIVATE: return "anon-private";
        case FRY_VM_REGION_ANON_SHARED:  return "anon-shared";
        case FRY_VM_REGION_FILE_PRIVATE: return "file-private";
        case FRY_VM_REGION_GUARD:        return "guard";
        default:                         return "unknown";
    }
}

static void pf_log_region_detail(const struct fry_process *cur,
                                 uint64_t fault_addr,
                                 uint64_t error) {
    const struct fry_vm_region *r = pf_find_vm_region(cur, fault_addr);
    const char *access = (error & (1ULL << 4)) ? "exec"
                       : (error & (1ULL << 1)) ? "write"
                       : "read";
    if (!r) {
        kprint("USER VM: addr=0x%llx access=%s reason=unmapped\n",
               (unsigned long long)fault_addr, access);
        return;
    }

    const char *reason = ((error & 1ULL) != 0) ? "protection" : "not-present";
    if (!r->committed) {
        reason = (r->kind == FRY_VM_REGION_GUARD) ? "guard" : "reserved";
    }

    kprint("USER VM: addr=0x%llx access=%s reason=%s kind=%s committed=%u base=0x%llx len=0x%llx prot=0x%x flags=0x%x\n",
           (unsigned long long)fault_addr,
           access,
           reason,
           pf_region_kind_name(r->kind),
           (unsigned)r->committed,
           (unsigned long long)r->base,
           (unsigned long long)r->length,
           (unsigned)r->prot,
           (unsigned)r->flags);
}

__attribute__((noreturn))
static void pf_kill_finish(uint32_t pid) {
    proc_free(pid);
    sched_yield();
    for (;;) {
        __asm__ volatile("hlt");
    }
}

__attribute__((noreturn))
static void pf_kill_current_user(uint64_t vector, uint64_t error, void *ctx) {
    struct fry_process *cur = proc_current();
    if (!cur || cur->is_kernel) {
        kernel_panic("page fault in invalid current context");
    }

    uint64_t *frame = (uint64_t *)ctx;
    uint64_t rip = frame[17];
    uint64_t cr2 = read_cr2_irq();
    uint32_t pid = cur->pid;

    kprint("USER FAULT: pid=%u vec=%llu err=0x%llx rip=0x%llx cr2=0x%llx\n",
           (unsigned)pid,
           (unsigned long long)vector,
           (unsigned long long)error,
           (unsigned long long)rip,
           (unsigned long long)cr2);
    pf_log_region_detail(cur, cr2, error);

    cur->exit_code = 139; /* SIGSEGV-like exit code */

    uint64_t kcr3 = irq_kernel_cr3 ? irq_kernel_cr3 : read_cr3_irq();
    uint64_t exit_sp = ((uint64_t)(uintptr_t)&g_pf_exit_stack[sizeof(g_pf_exit_stack)]) & ~0xFULL;
    __asm__ volatile(
        "mov %0, %%cr3\n"
        "mov %1, %%rsp\n"
        "mov %2, %%edi\n"
        "call *%3\n"
        :
        : "r"(kcr3),
          "r"(exit_sp),
          "r"(pid),
          "r"(pf_kill_finish)
        : "rdi", "memory");
    __builtin_unreachable();
}

void irq_dispatch(uint64_t vector, uint64_t error, void *ctx) {
    // Early serial telemetry: always emitted for CPU exceptions (< 32).
    // Works before kprint_init because it uses only I/O port instructions.
    if (vector < 32) {
        uint64_t *frame = (uint64_t *)ctx;
        // Stack frame layout (see common_isr in irqdesc.c):
        // [0..14] = saved gp regs (rax..r15), [15]=vector, [16]=error,
        // [17]=RIP, [18]=CS, [19]=RFLAGS  (all 8-byte slots)
        uint64_t rip    = frame[17];
        uint64_t cs     = frame[18];
        uint64_t rflags = frame[19];
        uint64_t cr2    = read_cr2_irq();
        uint64_t cr3    = read_cr3_irq();
        early_serial_puts("!EXC vec=");
        early_serial_puthex64(vector);
        early_serial_puts(" err=");
        early_serial_puthex64(error);
        early_serial_puts(" RIP=");
        early_serial_puthex64(rip);
        early_serial_puts(" CS=");
        early_serial_puthex64(cs);
        early_serial_puts(" RF=");
        early_serial_puthex64(rflags);
        early_serial_puts(" CR2=");
        early_serial_puthex64(cr2);
        early_serial_puts(" CR3=");
        early_serial_puthex64(cr3);
        early_serial_puts("\n");
    }

    /*
     * If user mode triggers #PF and no dedicated page-fault handler is
     * installed, terminate only that process instead of stalling the kernel.
     */
    if (vector == 14 && !irq_descs[14].handler) {
        uint64_t *frame = (uint64_t *)ctx;
        uint64_t cs = frame[18];
        if ((cs & 3ULL) == 3ULL) {
            pf_kill_current_user(vector, error, ctx);
        } else {
            kernel_panic("unhandled kernel page fault");
        }
    }

    if (vector < 256 && irq_descs[vector].chip && irq_descs[vector].chip->ack) {
        irq_descs[vector].chip->ack((uint32_t)vector);
    }

    if (vector < 256 && irq_descs[vector].handler) {
        irq_descs[vector].handler((uint32_t)vector, ctx, irq_descs[vector].dev_id, error);
        irq_descs[vector].count++;
    } else {
        if (vector < 32) {
            early_serial_puts("EXC unhandled vec=");
            early_serial_puthex64(vector);
            early_serial_puts(" err=");
            early_serial_puthex64(error);
            early_serial_putc('\n');
        }
    }

    if (vector < 256 && irq_descs[vector].chip && irq_descs[vector].chip->eoi) {
        irq_descs[vector].chip->eoi((uint32_t)vector);
    }
}

// common_isr: called by IDT stubs
// Stack layout at entry (after IDT stub pushes vector+error):
//   0(%rsp)=rax 8=rbx 16=rcx 24=rdx 32=rbp 40=rsi 48=rdi
//   56=r8 64=r9 72=r10 80=r11 88=r12 96=r13 104=r14 112=r15
//   120=vector 128=error 136=RIP 144=CS 152=RFLAGS [160=RSP 168=SS if CPL3]
//
// CR3 save/restore: user process CR3 is saved in %r15 (callee-saved) and
// replaced with irq_kernel_cr3 before irq_dispatch so LAPIC/MMIO identity
// mappings are accessible.  The original %r15 sits on the stack (pushed
// first), so it is correctly restored at the end.  irq_dispatch's C prologue
// saves %r15 onto its own frame; if a context_switch happens inside,
// context_switch saves/restores it per-stack, so each process independently
// recovers its own saved CR3 when irq_dispatch eventually returns.
__asm__(
    ".global common_isr\n"
    "common_isr:\n"
    "    push %r15\n"
    "    push %r14\n"
    "    push %r13\n"
    "    push %r12\n"
    "    push %r11\n"
    "    push %r10\n"
    "    push %r9\n"
    "    push %r8\n"
    "    push %rdi\n"
    "    push %rsi\n"
    "    push %rbp\n"
    "    push %rdx\n"
    "    push %rcx\n"
    "    push %rbx\n"
    "    push %rax\n"
    "    mov 120(%rsp), %rdi\n"               // arg1 = vector
    "    mov 128(%rsp), %rsi\n"               // arg2 = error code
    "    mov %rsp, %rdx\n"                    // arg3 = ctx (frame base)
    // Save current (user) CR3 in %r15.  %r15 was already pushed above so its
    // original value is safe on the stack.  %r15 is callee-saved per SysV
    // ABI, so irq_dispatch (C) saves it in its own prologue and restores it
    // in its epilogue — including if a context_switch happens inside.
    "    mov %cr3, %r15\n"                    // r15 = saved user CR3
    "    mov irq_kernel_cr3(%rip), %rax\n"
    "    test %rax, %rax\n"
    "    jz .Lcommon_isr_no_cr3\n"
    "    mov %rax, %cr3\n"                    // switch to kernel page table
    ".Lcommon_isr_no_cr3:\n"
    "    call irq_dispatch\n"
    "    mov %r15, %cr3\n"                    // restore user CR3 from r15
    "    pop %rax\n"
    "    pop %rbx\n"
    "    pop %rcx\n"
    "    pop %rdx\n"
    "    pop %rbp\n"
    "    pop %rsi\n"
    "    pop %rdi\n"
    "    pop %r8\n"
    "    pop %r9\n"
    "    pop %r10\n"
    "    pop %r11\n"
    "    pop %r12\n"
    "    pop %r13\n"
    "    pop %r14\n"
    "    pop %r15\n"
    "    add $16, %rsp\n"                     // pop vector + error
    "    iretq\n"
);
