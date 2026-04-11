// IRQ descriptor table and dispatch

#include <stdint.h>
#include "irqdesc.h"
#include "../../boot/early_serial.h"
#include "../../drivers/irqchip/lapic.h"
#include "../../drivers/smp/smp.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/syscall.h"
#include "../mm/vmm.h"

void kprint(const char *fmt, ...);
void kernel_panic(const char *msg);

static struct irq_desc irq_descs[256];

/* Per-CPU exit stacks for killing user processes from exception handlers.
 * A single global stack was unsafe: concurrent user faults on different CPUs
 * would corrupt each other's stack frames, leading to kernel #UD. */
#define EXC_EXIT_STACK_SIZE 16384
#define EXC_MAX_CPUS 64
static uint8_t g_exc_exit_stacks[EXC_MAX_CPUS][EXC_EXIT_STACK_SIZE]
    __attribute__((aligned(16)));

static uint32_t exc_cpu_index(void) {
    uint8_t id = lapic_get_id();
    uint32_t count = smp_cpu_count();
    for (uint32_t i = 0; i < count && i < EXC_MAX_CPUS; i++) {
        if (smp_cpu_apic_id(i) == id) return i;
    }
    return 0;
}

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
    if (!p || !p->shared) return 0;
    for (uint32_t i = 0; i < PROC_VMREG_MAX; i++) {
        const struct fry_vm_region *r = &p->shared->vm_regions[i];
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
static void pf_kill_finish(uint32_t tgid, uint32_t code) {
    process_exit_group(tgid, code);
    sched_yield();
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void dump_pte_chain(uint64_t cr3_phys, uint64_t va) {
    if (!cr3_phys || va >= USER_VA_TOP) return;

    uint64_t pml4_i = (va >> 39) & 0x1FF;
    uint64_t pdpt_i = (va >> 30) & 0x1FF;
    uint64_t pd_i   = (va >> 21) & 0x1FF;
    uint64_t pt_i   = (va >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)vmm_phys_to_virt(cr3_phys);
    uint64_t pml4e = pml4[pml4_i];
    kprint("  PTE CHAIN va=0x%llx cr3=0x%llx pml4[%llu]=0x%llx\n",
           (unsigned long long)va, (unsigned long long)cr3_phys,
           (unsigned long long)pml4_i, (unsigned long long)pml4e);
    if (!(pml4e & 1ULL)) return;

    uint64_t *pdpt = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pml4e & 0x000FFFFFFFFFF000ULL);
    uint64_t pdpte = pdpt[pdpt_i];
    kprint("  pdpt[%llu]=0x%llx\n", (unsigned long long)pdpt_i, (unsigned long long)pdpte);
    if (!(pdpte & 1ULL) || (pdpte & 0x80ULL)) return;

    uint64_t *pd = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pdpte & 0x000FFFFFFFFFF000ULL);
    uint64_t pde = pd[pd_i];
    kprint("  pd[%llu]=0x%llx\n", (unsigned long long)pd_i, (unsigned long long)pde);
    if (!(pde & 1ULL) || (pde & 0x80ULL)) return;

    uint64_t *pt = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pde & 0x000FFFFFFFFFF000ULL);
    uint64_t pte = pt[pt_i];
    kprint("  pt[%llu]=0x%llx\n", (unsigned long long)pt_i, (unsigned long long)pte);
}

/* Kill a user process that triggered a CPU exception.
 * Handles ALL exception vectors (0-31), not just #PF (vec 14). */
__attribute__((noreturn))
static void exc_kill_current_user(uint64_t vector, uint64_t error, void *ctx) {
    struct fry_process *cur = proc_current();
    if (!cur || cur->is_kernel) {
        kernel_panic("cpu exception in invalid current context");
    }

    uint64_t *frame = (uint64_t *)ctx;
    uint64_t rip = frame[17];
    uint64_t cr2 = read_cr2_irq();
    uint32_t tgid = process_group_id(cur);
    uint32_t tid = cur->pid;

    kprint("USER FAULT: pid=%u tid=%u vec=%llu err=0x%llx rip=0x%llx cr2=0x%llx\n",
           (unsigned)tgid,
           (unsigned)tid,
           (unsigned long long)vector,
           (unsigned long long)error,
           (unsigned long long)rip,
           (unsigned long long)cr2);

    if (vector == 14) {
        pf_log_region_detail(cur, cr2, error);
        /* Dump actual PTE chain for the faulting address */
        dump_pte_chain(cur->cr3, cr2);
    }

    uint32_t cpu = exc_cpu_index();
    uint64_t kcr3 = irq_kernel_cr3 ? irq_kernel_cr3 : read_cr3_irq();
    uint64_t exit_sp = ((uint64_t)(uintptr_t)&g_exc_exit_stacks[cpu][EXC_EXIT_STACK_SIZE]) & ~0xFULL;
    __asm__ volatile(
        "mov %0, %%cr3\n"
        "mov %1, %%rsp\n"
        "mov %2, %%edi\n"
        "mov %3, %%esi\n"
        "call *%4\n"
        :
        : "r"(kcr3),
          "r"(exit_sp),
          "r"(tgid),
          "r"(139u),
          "r"(pf_kill_finish)
        : "rdi", "rsi", "memory");
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
     * Any CPU exception (vec 0-31) from user mode without a registered handler
     * terminates the process instead of iretq-ing back to the faulting
     * instruction (which would cause an infinite loop).
     * Previously only vec=14 (#PF) was handled; vec 0-13,15-31 fell through.
     */
    if (vector < 32 && !irq_descs[vector].handler) {
        uint64_t *frame = (uint64_t *)ctx;
        uint64_t cs = frame[18];
        if ((cs & 3ULL) == 3ULL) {
            exc_kill_current_user(vector, error, ctx);
        } else if (vector == 14) {
            kernel_panic("unhandled kernel page fault");
        }
        /* Kernel-mode non-#PF exceptions without handlers fall through
         * to the "EXC unhandled" log below and iretq. */
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
// SWAPGS on interrupt entry/exit:
// At common_isr entry, the stack is:
//   (%rsp)=vector  8(%rsp)=error  16(%rsp)=RIP  24(%rsp)=CS ...
// If CS & 3 != 0, we came from user mode and must SWAPGS to load kernel GS.
// On exit (after register+vector+error pops), CS is at 8(%rsp):
//   (%rsp)=RIP  8(%rsp)=CS ...
// If CS & 3 != 0, returning to user mode, do SWAPGS to restore user GS.
__asm__(
    ".global common_isr\n"
    "common_isr:\n"
    // SWAPGS if interrupted from user mode (CPL 3)
    "    testb $3, 24(%rsp)\n"                // test CS RPL bits
    "    jz .Lno_swapgs_entry\n"
    "    swapgs\n"
    ".Lno_swapgs_entry:\n"
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
    // SWAPGS if returning to user mode (CPL 3)
    "    testb $3, 8(%rsp)\n"                 // test CS RPL bits
    "    jz .Lno_swapgs_exit\n"
    "    swapgs\n"
    ".Lno_swapgs_exit:\n"
    "    iretq\n"
);
