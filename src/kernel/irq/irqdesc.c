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

static void exc_early_serial_dump_regs(const uint64_t *frame) {
    early_serial_puts("!REG rax=");
    early_serial_puthex64(frame[0]);
    early_serial_puts(" rbx=");
    early_serial_puthex64(frame[1]);
    early_serial_puts(" rcx=");
    early_serial_puthex64(frame[2]);
    early_serial_puts(" rdx=");
    early_serial_puthex64(frame[3]);
    early_serial_puts(" rbp=");
    early_serial_puthex64(frame[4]);
    early_serial_puts("\n");

    early_serial_puts("!REG rsi=");
    early_serial_puthex64(frame[5]);
    early_serial_puts(" rdi=");
    early_serial_puthex64(frame[6]);
    early_serial_puts(" r8=");
    early_serial_puthex64(frame[7]);
    early_serial_puts(" r9=");
    early_serial_puthex64(frame[8]);
    early_serial_puts(" r10=");
    early_serial_puthex64(frame[9]);
    early_serial_puts("\n");

    early_serial_puts("!REG r11=");
    early_serial_puthex64(frame[10]);
    early_serial_puts(" r12=");
    early_serial_puthex64(frame[11]);
    early_serial_puts(" r13=");
    early_serial_puthex64(frame[12]);
    early_serial_puts(" r14=");
    early_serial_puthex64(frame[13]);
    early_serial_puts(" r15=");
    early_serial_puthex64(frame[14]);
    early_serial_puts("\n");
}

static int read_user_u64_for_exc(const struct fry_process *p, uint64_t addr, uint64_t *out) {
    if (!p || !p->cr3 || !out) return -1;
    if (addr > USER_VA_TOP - sizeof(uint64_t)) return -1;
    uint64_t pa = vmm_virt_to_phys_user(p->cr3, addr);
    if (!pa) return -1;
    uint64_t phys = pa & 0x000FFFFFFFFFF000ULL;
    uint64_t off = addr & 0xFFFULL;
    if (off > 0x1000ULL - sizeof(uint64_t)) return -1;
    const uint8_t *kv = (const uint8_t *)(uintptr_t)vmm_phys_to_virt(phys);
    *out = *(const uint64_t *)(const void *)(kv + off);
    return 0;
}

static int write_user_u64_for_exc(const struct fry_process *p, uint64_t addr, uint64_t val) {
    if (!p || !p->cr3) return -1;
    if (addr > USER_VA_TOP - sizeof(uint64_t)) return -1;
    uint64_t pa = vmm_virt_to_phys_user(p->cr3, addr);
    if (!pa) return -1;
    uint64_t phys = pa & 0x000FFFFFFFFFF000ULL;
    uint64_t off = addr & 0xFFFULL;
    if (off > 0x1000ULL - sizeof(uint64_t)) return -1;
    uint8_t *kv = (uint8_t *)(uintptr_t)vmm_phys_to_virt(phys);
    *(uint64_t *)(void *)(kv + off) = val;
    return 0;
}

static uint64_t tb_watch_addr_from_dr6(uint64_t dr6) {
    if (dr6 & 0x1ULL) return g_tb_jsc_slot_watch[0];
    if (dr6 & 0x2ULL) return g_tb_jsc_slot_watch[1];
    if (dr6 & 0x4ULL) return g_tb_jsc_slot_watch[2];
    if (dr6 & 0x8ULL) return g_tb_jsc_slot_watch[3];
    return 0;
}

static int tb_handle_jsc_slot_watchpoint(uint64_t vector, const uint64_t *frame) {
    if (vector != 1 || !frame) return 0;
    struct fry_process *cur = proc_current();
    if (!cur || cur->is_kernel ||
        (cur->pid != TB_CLAUDE_WATCH_TGID && cur->tgid != TB_CLAUDE_WATCH_TGID))
        return 0;

    uint64_t dr6 = 0;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    uint64_t watch = tb_watch_addr_from_dr6(dr6);
    if (!watch) return 0;

    uint64_t val = 0;
    int have_val = read_user_u64_for_exc(cur, watch, &val) == 0;
    early_serial_puts("TBWATCH jsc-slot pid=");
    early_serial_puthex64(cur->pid);
    early_serial_puts(" tgid=");
    early_serial_puthex64(cur->tgid);
    early_serial_puts(" dr6=");
    early_serial_puthex64(dr6);
    early_serial_puts(" rip=");
    early_serial_puthex64(frame[17]);
    early_serial_puts(" rsp=");
    early_serial_puthex64(frame[20]);
    early_serial_puts(" watch=");
    early_serial_puthex64(watch);
    early_serial_puts(" val=");
    if (have_val) early_serial_puthex64(val); else early_serial_puts("NA");
    early_serial_puts(" rax=");
    early_serial_puthex64(frame[0]);
    early_serial_puts(" rbx=");
    early_serial_puthex64(frame[1]);
    early_serial_puts(" rcx=");
    early_serial_puthex64(frame[2]);
    early_serial_puts(" rdx=");
    early_serial_puthex64(frame[3]);
    early_serial_puts(" rsi=");
    early_serial_puthex64(frame[5]);
    early_serial_puts(" rdi=");
    early_serial_puthex64(frame[6]);
    early_serial_puts(" r14=");
    early_serial_puthex64(frame[13]);
    early_serial_puts("\n");

    dr6 = 0;
    __asm__ volatile("mov %0, %%dr6" : : "r"(dr6) : "memory");
    return 1;
}

static void exc_early_serial_dump_trap_stack(const struct fry_process *cur,
                                             const uint64_t *frame) {
    if (!cur || (frame[18] & 3ULL) != 3ULL) return;
    uint32_t tgid = process_group_id(cur);
    if (tgid != 3u) return;

    uint64_t rsp = frame[20];
    uint64_t rbp = frame[4];
    uint64_t stack0 = 0, stack1 = 0, saved_rbp = 0, caller = 0;
    int have_stack0 = read_user_u64_for_exc(cur, rsp, &stack0) == 0;
    int have_stack1 = read_user_u64_for_exc(cur, rsp + 8, &stack1) == 0;
    int have_saved_rbp = read_user_u64_for_exc(cur, rbp, &saved_rbp) == 0;
    int have_caller = read_user_u64_for_exc(cur, rbp + 8, &caller) == 0;

    early_serial_puts("!TRAPSTACK pid=");
    early_serial_puthex64(cur->pid);
    early_serial_puts(" tgid=");
    early_serial_puthex64(tgid);
    early_serial_puts(" rsp=");
    early_serial_puthex64(rsp);
    early_serial_puts(" rbp=");
    early_serial_puthex64(rbp);
    early_serial_puts(" stack0=");
    if (have_stack0) early_serial_puthex64(stack0); else early_serial_puts("NA");
    early_serial_puts(" stack1=");
    if (have_stack1) early_serial_puthex64(stack1); else early_serial_puts("NA");
    early_serial_puts(" saved_rbp=");
    if (have_saved_rbp) early_serial_puthex64(saved_rbp); else early_serial_puts("NA");
    early_serial_puts(" caller=");
    if (have_caller) early_serial_puthex64(caller); else early_serial_puts("NA");
    early_serial_puts("\n");
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

/* Map user CPU exceptions to the Unix signal delivered/killed for that fault.
 * Handles ALL exception vectors (0-31), not just #PF (vec 14). */
static uint32_t exc_unix_signal(uint64_t vector) {
    switch (vector) {
    case 0:  return 8;   /* #DE  → SIGFPE  */
    case 1:  return 5;   /* #DB  → SIGTRAP */
    case 3:  return 5;   /* #BP  → SIGTRAP */
    case 4:  return 11;  /* #OF  → SIGSEGV */
    case 5:  return 11;  /* #BR  → SIGSEGV */
    case 6:  return 4;   /* #UD  → SIGILL  */
    case 7:  return 8;   /* #NM  → SIGFPE  */
    case 11: return 7;   /* #NP  → SIGBUS  */
    default: return 11;  /* #GP #PF #SS #TS etc → SIGSEGV */
    }
}

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
        if (cur->cr3) {
            dump_pte_chain(cur->cr3, cr2);
        } else {
            kprint("USER VM: skipped PTE dump for pid=%u tid=%u because cr3=0\n",
                   (unsigned)tgid, (unsigned)tid);
        }
    }

    uint32_t unix_sig = exc_unix_signal(vector);
    uint32_t cpu = exc_cpu_index();
    uint64_t kcr3 = irq_kernel_cr3 ? irq_kernel_cr3 : read_cr3_irq();
    uint64_t exit_sp = ((uint64_t)(uintptr_t)&g_exc_exit_stacks[cpu][EXC_EXIT_STACK_SIZE]) & ~0xFULL;
    __asm__ volatile(
        "mov %0, %%cr3\n"
        "mov %1, %%rsp\n"
        "movl %k2, %%edi\n"
        "movl %k3, %%esi\n"
        "call *%4\n"
        :
        : "r"(kcr3),
          "r"(exit_sp),
          "r"(tgid),
          "r"(128u + unix_sig),
          "r"(pf_kill_finish)
        : "rdi", "rsi", "memory");
    __builtin_unreachable();
}

/* fry1377: userspace-RIP sampler. The Claude main thread (pid=3) wedges in a
 * pure-userspace spin (no syscalls, no faults) holding the JSC heap lock. Sample
 * its RIP from the LAPIC timer IRQ (vector 0x40) to locate the spin site. Rate-
 * limited so IRQ-context serial output stays bounded. */
static uint64_t g_tb_rip_samples = 0;
static uint32_t g_tb_rip_logs = 0;
static uint32_t g_tb_rip_detail_logs = 0;

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
        /* Demand-page a not-present USER #PF inside a reserved anon mmap
         * region (e.g. JSC's MAP_NORESERVE gigacage). Commit one zero page
         * and iretq to retry. Done before any logging/kill so it is fast and
         * silent — a running runtime faults in thousands of pages this way. */
        if (vector == 14 && (cs & 3ULL) == 3ULL &&
            lx_try_demand_page(proc_current(), cr2, error)) {
            return;
        }
        /* fry1389: ONE-TIME dump of the JSC JIT-prologue CodeBlock chain on the
         * first DEEP main-stack fault (below USER_VA_TOP-8MB). The faulting code
         * (RIP 0x3F4CFB6) computes frame size = CodeBlock->m_numCalleeLocals
         * ([CodeBlock+0x14]) * 8, sets rsp = rbp - size, then zeroes locals. A
         * garbage-huge size walks the stack off the cap. This tells us whether
         * the CodeBlock POINTER ([rbp+0x10]) is garbage (stack/frame corruption)
         * or its CONTENTS are garbage (heap corruption / UAF). */
        if (vector == 14 && (cs & 3ULL) == 3ULL &&
            cr2 < (0x800000000000ULL - (8ULL << 20)) &&
            cr2 >= (0x800000000000ULL - (512ULL << 20))) {
            static int g_bf_dumped = 0;
            if (!g_bf_dumped) {
                g_bf_dumped = 1;
                struct fry_process *bc = proc_current();
                uint64_t rbp = frame[4];
                uint64_t rsi = frame[5];        /* CodeBlock the JIT actually used */
                uint64_t cb = 0, retaddr = 0, callerfp = 0;
                uint64_t cnt = 0, vm = 0, lim = 0;
                uint64_t rsicnt = 0, cb0 = 0, cb8 = 0, cb10 = 0;
                int hb  = read_user_u64_for_exc(bc, rbp + 0x10, &cb) == 0;
                (void)read_user_u64_for_exc(bc, rbp + 0x08, &retaddr);
                (void)read_user_u64_for_exc(bc, rbp + 0x00, &callerfp);
                int hc  = hb && read_user_u64_for_exc(bc, cb + 0x14, &cnt) == 0;
                int hvm = hb && read_user_u64_for_exc(bc, cb + 0x48, &vm) == 0;
                int hl  = hvm && read_user_u64_for_exc(bc, vm + 0x60, &lim) == 0;
                int hrc = read_user_u64_for_exc(bc, rsi + 0x14, &rsicnt) == 0;
                (void)read_user_u64_for_exc(bc, cb + 0x00, &cb0);
                (void)read_user_u64_for_exc(bc, cb + 0x08, &cb8);
                (void)read_user_u64_for_exc(bc, cb + 0x10, &cb10);
                early_serial_puts("TBBFDUMP rip="); early_serial_puthex64(rip);
                early_serial_puts(" rbp="); early_serial_puthex64(rbp);
                early_serial_puts(" rsi="); early_serial_puthex64(rsi);
                early_serial_puts(" cb=");
                if (hb) early_serial_puthex64(cb); else early_serial_puts("FAULT");
                early_serial_puts(" rsi.numLocals=");
                if (hrc) early_serial_puthex64(rsicnt & 0xFFFFFFFFULL); else early_serial_puts("FAULT");
                early_serial_puts(" cb.numLocals=");
                if (hc) early_serial_puthex64(cnt & 0xFFFFFFFFULL); else early_serial_puts("FAULT");
                early_serial_puts(" cb+0=");  early_serial_puthex64(cb0);
                early_serial_puts(" cb+8=");  early_serial_puthex64(cb8);
                early_serial_puts(" cb+10="); early_serial_puthex64(cb10);
                early_serial_puts(" vm=");
                if (hvm) early_serial_puthex64(vm); else early_serial_puts("FAULT");
                early_serial_puts(" softLimit=");
                if (hl) early_serial_puthex64(lim); else early_serial_puts("FAULT");
                early_serial_puts(" ret="); early_serial_puthex64(retaddr);
                early_serial_puts(" callerfp="); early_serial_puthex64(callerfp);
                early_serial_puts("\n");
            }
        }
        /* fry1387: demand stack growth — fault just below the stack VMA. */
        if (vector == 14 && (cs & 3ULL) == 3ULL &&
            lx_try_grow_stack(proc_current(), cr2, frame[20], error)) {
            return;
        }
        if ((cs & 3ULL) == 3ULL && tb_handle_jsc_slot_watchpoint(vector, frame)) {
            return;
        }
        /* Work around Bun/JSC libuv array corruption: 0x40000000 (1 GiB =
         * gigacage size) leaks into a pointer slot in a vtable dispatch loop
         * at RIP 0x6056F06.  Skip the bogus entry by advancing RIP to the
         * loop's next-iteration label so Bun can continue initialization. */
        if (vector == 14 && (cs & 3ULL) == 3ULL &&
            rip >= 0x6056F06 && rip <= 0x6056F08 &&
            (cr2 == 0x40000000 || cr2 == 0xFFFFFFFFFFFFFFFF)) {
            struct fry_process *cur = proc_current();
            uint64_t rbx = frame[1];
            uint64_t rdi = frame[6];
            uint64_t r14 = frame[13];
            uint64_t count_word = 0;
            uint64_t array_ptr = 0;
            uint64_t slot_addr = 0;
            uint64_t slot_val = 0;
            int have_count = read_user_u64_for_exc(cur, rbx + 0x58, &count_word) == 0;
            int have_array = read_user_u64_for_exc(cur, rbx + 0x60, &array_ptr) == 0;
            if (have_array && r14 < 0x100000ULL) {
                slot_addr = array_ptr + r14 * 8ULL;
                (void)read_user_u64_for_exc(cur, slot_addr, &slot_val);
            }
            /* fry1378: do NOT zero the slot. 0xffffffffffffffff is a legitimate
             * JSC hash-table empty sentinel (-1), not corruption. Writing 0 over
             * it converts the empty sentinel to 0, which later breaks the JSC
             * hash-probe at 0x60CCF10 (it stops only on -1) -> infinite spin and
             * the main-thread hang. Keep only the RIP skip so the process
             * survives this deref; stop destroying sentinels. */
            early_serial_puts("TBSKIP bad-ptr pid=");
            early_serial_puthex64(cur ? cur->pid : 0);
            early_serial_puts(" rip=");
            early_serial_puthex64(rip);
            early_serial_puts(" cr2=");
            early_serial_puthex64(cr2);
            early_serial_puts(" rbx=");
            early_serial_puthex64(rbx);
            early_serial_puts(" r14=");
            early_serial_puthex64(r14);
            early_serial_puts(" rdi=");
            early_serial_puthex64(rdi);
            early_serial_puts(" count=");
            if (have_count) early_serial_puthex64(count_word & 0xFFFFFFFFULL); else early_serial_puts("NA");
            early_serial_puts(" arr=");
            if (have_array) early_serial_puthex64(array_ptr); else early_serial_puts("NA");
            early_serial_puts(" slot=");
            if (slot_addr) early_serial_puthex64(slot_addr); else early_serial_puts("NA");
            early_serial_puts(" slotval=");
            if (slot_addr) early_serial_puthex64(slot_val); else early_serial_puts("NA");
            early_serial_puts("\n");
            frame[17] = 0x6056F0F;  /* skip to inc r14 + loop */
            return;
        }
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
        exc_early_serial_dump_regs(frame);
        if (vector == 3) {
            exc_early_serial_dump_trap_stack(proc_current(), frame);
        }
        /* fry1380: on a user #GP/#PF in the Claude thread, walk the rbp chain to
         * recover the caller return-address chain (the bad-pointer deref's
         * callers), so we can locate where a non-canonical pointer is produced. */
        if ((vector == 13 || vector == 14) && (cs & 3ULL) == 3ULL) {
            struct fry_process *gc = proc_current();
            if (gc && (gc->pid == TB_CLAUDE_WATCH_TGID || gc->tgid == TB_CLAUDE_WATCH_TGID)) {
                uint64_t fp = frame[4];   /* rbp */
                for (int d = 0; d < 12 && fp >= 0x1000ULL; d++) {
                    uint64_t ret = 0, nextfp = 0;
                    if (read_user_u64_for_exc(gc, fp + 8, &ret) != 0) break;
                    early_serial_puts("  TBBT ret=");
                    early_serial_puthex64(ret);
                    early_serial_puts(" fp=");
                    early_serial_puthex64(fp);
                    early_serial_puts("\n");
                    if (read_user_u64_for_exc(gc, fp, &nextfp) != 0) break;
                    if (nextfp <= fp) break;   /* stop on non-ascending / corrupt */
                    fp = nextfp;
                }
            }
        }
    }

    /*
     * Any CPU exception (vec 0-31) from user mode without a registered handler
     * tries to deliver the appropriate Unix signal first. If the process has
     * a handler, the exception frame is modified to jump there on iretq.
     * Otherwise the process is killed.
     */
    if (vector < 32 && !irq_descs[vector].handler) {
        uint64_t *frame = (uint64_t *)ctx;
        uint64_t cs = frame[18];
        if ((cs & 3ULL) == 3ULL) {
            if (lx_deliver_signal_from_exception(proc_current(), vector, frame)) {
                return;  /* signal delivered, iretq will jump to handler */
            }
            exc_kill_current_user(vector, error, ctx);
        } else if (vector == 14) {
            kernel_panic("unhandled kernel page fault");
        }
        /* Kernel-mode non-#PF exceptions without handlers fall through
         * to the "EXC unhandled" log below and iretq. */
    }

    if (vector == 0x40 && ctx) {
        uint64_t *tframe = (uint64_t *)ctx;
        if ((tframe[18] & 3ULL) == 3ULL) {   /* interrupted in user mode */
            struct fry_process *tc = proc_current();
            if (tc && (tc->pid == TB_CLAUDE_WATCH_TGID ||
                       tc->tgid == TB_CLAUDE_WATCH_TGID)) {
                g_tb_rip_samples++;
                uint64_t srip = tframe[17];
                /* Detail dump when caught in the JSC hash-probe spin
                 * (0x60CCF00..0x60CCF30): registers + table header + slots, to
                 * see whether the mask/capacity is bogus or the table is full of
                 * garbage with no -1 empty sentinel (fry1377/1378). */
                if (srip >= 0x60CCF00ULL && srip <= 0x60CCF30ULL &&
                    g_tb_rip_detail_logs < 2) {
                    g_tb_rip_detail_logs++;
                    uint64_t base = tframe[0];   /* rax = table base */
                    uint64_t key  = tframe[1];   /* rbx = target key */
                    uint64_t prc  = tframe[2];   /* rcx = hash/probe */
                    uint64_t msk  = tframe[5];   /* rsi = mask (low 32) */
                    uint64_t slot = tframe[7];   /* r8  = current slot val */
                    early_serial_puts("TBPROBE rip=");
                    early_serial_puthex64(srip);
                    early_serial_puts(" base=");
                    early_serial_puthex64(base);
                    early_serial_puts(" key=");
                    early_serial_puthex64(key);
                    early_serial_puts(" rcx=");
                    early_serial_puthex64(prc);
                    early_serial_puts(" mask=");
                    early_serial_puthex64(msk & 0xFFFFFFFFULL);
                    early_serial_puts(" slot=");
                    early_serial_puthex64(slot);
                    early_serial_puts("\n");
                    /* header qwords (capacity/count/etc.) */
                    for (int h = 0; h < 4; h++) {
                        uint64_t hv = 0;
                        int ok = read_user_u64_for_exc(tc, base + (uint64_t)h * 8, &hv) == 0;
                        early_serial_puts("  TBPROBE hdr+");
                        early_serial_puthex64((uint64_t)h * 8);
                        early_serial_puts("=");
                        if (ok) early_serial_puthex64(hv); else early_serial_puts("NA");
                        early_serial_puts("\n");
                    }
                    /* ALL 32 entry keys (16-byte entries, data at base+0x20):
                     * count zeros, pointers, and any -1, to tell "full table
                     * needs rehash" from "empties wrongly 0" (fry1378). */
                    for (int e = 0; e <= (int)(msk & 0xFFFFFFFFULL); e++) {
                        uint64_t v = 0;
                        int ok = read_user_u64_for_exc(tc, base + 0x20 + (uint64_t)e * 0x10, &v) == 0;
                        early_serial_puts("  TBPROBE slot[");
                        early_serial_puthex64((uint64_t)e);
                        early_serial_puts("]=");
                        if (ok) early_serial_puthex64(v); else early_serial_puts("NA");
                        early_serial_puts("\n");
                    }
                }
                if ((g_tb_rip_samples & 0x3FULL) == 0 && g_tb_rip_logs < 400) {
                    g_tb_rip_logs++;
                    early_serial_puts("TBRIP pid=");
                    early_serial_puthex64(tc->pid);
                    early_serial_puts(" rip=");
                    early_serial_puthex64(srip);
                    early_serial_puts(" rsp=");
                    early_serial_puthex64(tframe[20]);
                    early_serial_puts("\n");
                }
            }
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
