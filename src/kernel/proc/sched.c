// Round-robin scheduler

#include <stdint.h>
#include <errno.h>
#include "sched.h"
#include "process.h"
#include "../mm/vmm.h"
#include "../../drivers/smp/smp.h"
#include "../../drivers/smp/spinlock.h"
#include "../../drivers/irqchip/lapic.h"
#include "../../drivers/timer/hpet.h"
#include "../../boot/tss.h"
#include "../../boot/efi_handoff.h"
#include "../../boot/early_serial.h"
#include "../../include/tater_trace.h"

void kprint(const char *fmt, ...);
void kprint_serial_only(const char *fmt, ...);
extern struct fry_handoff *g_handoff;

/*
 * Per-CPU data for SWAPGS-based syscall entry.
 * syscall_entry uses SWAPGS to load GS.BASE → percpu, then accesses
 * kstack_top at offset 0 and user_rsp scratch at offset 8.
 */
struct percpu_data {
    uint64_t kstack_top;    /* offset 0: kernel stack for current process */
    uint64_t user_rsp;      /* offset 8: scratch for user RSP in syscall_entry */
    uint64_t linux_frame_rsp; /* offset 16: interrupt-frame RSP for clone3 */
    /* offsets 24..56: parent callee-saved regs captured in syscall_entry so
     * clone3 children can inherit them (rbx,r12,r13,r14,r15 in order). The
     * syscall_entry frame does not otherwise save these. */
    uint64_t linux_clone_saved[5];
} __attribute__((aligned(16)));

#define MAX_CPUS 64
#define MSR_FS_BASE 0xC0000100u
/* fry1568: was hardcoded 3u (= some other process, NOT claude) which made every
 * sched-side trace (TBCLK census, TBSCHED wakes) track the wrong tgid. Use the
 * dynamic claude tgid set in syscall.c (extern in process.h) so these actually
 * track claude (tgid=14). */
#define TB_TRACE_CLAUDE_TGID g_tb_trace_claude_tgid

static struct percpu_data percpu[MAX_CPUS];
static volatile uint32_t g_sched_ready;
static uint32_t g_sched_next_cpu;

struct runqueue {
    struct fry_process *head;
    struct fry_process *tail;
    uint32_t count;
};

static struct runqueue rq[MAX_CPUS];
static struct fry_process *current[MAX_CPUS];
/* Per-CPU idle task — the always-runnable fallback so a task that blocks
 * itself (PROC_WAITING) is never left running when its runqueue is empty. */
static struct fry_process *idle_task[MAX_CPUS];
/*
 * The BSP boot thread starts life on the linker-defined bootstrap stack, not
 * on a scheduler-owned kernel stack. Keep a bookkeeping-only context for that
 * one-way handoff, but never enqueue it as a runnable task.
 */
static struct fry_process boot_contexts[MAX_CPUS];
static spinlock_t g_sched_lock = {0};
static uint8_t g_first_context_switch_seen;
static uint8_t g_first_user_switch_seen;
static uint32_t g_tb_futex_timeout_logs;
static uint32_t g_tb_futex_wake_miss_logs;
static uint32_t g_tb_oncpu_defer_logs;
static uint32_t g_tb_requeue_logs;
/* fry1424 TBCLK clock-freeze tracer: per-CPU LAPIC tick counter. The 1422/
 * 1423 freezes left the LAPIC timer alive (per-vCPU host accrual) while
 * now_ms() stopped — heartbeat proves which: ticks rising + frozen hpet +
 * cfg bit0=0 => counter halted (stray MMIO write); cfg bit0=1 + frozen,
 * per-CPU DIVERGENT hpet => HPET page lost UC, cached reads; frozen and
 * identical => QEMU-side/deeper. Strip with the other beacons. */
static volatile uint64_t g_tbclk_ticks[MAX_CPUS];
static uint32_t g_tb_lost_wake_logs;

extern char __kernel_stack_top;

static void rq_push(struct runqueue *q, struct fry_process *p);

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

static inline uint64_t irq_save_disable(void) {
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n"
        "pop %0\n"
        "cli\n"
        : "=r"(flags)
        :
        : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static uint32_t cpu_index(void) {
    uint8_t id = lapic_get_id();
    uint32_t count = smp_cpu_count();
    for (uint32_t i = 0; i < count && i < MAX_CPUS; i++) {
        if (smp_cpu_apic_id(i) == id) return i;
    }
    return 0;
}

static uint64_t now_ms(void) {
    uint64_t freq = hpet_get_freq_hz();
    if (freq == 0) return 0;
    uint64_t cnt = hpet_read_counter();
    return (cnt * 1000ULL) / freq;
}

static void write_user_fs_base(uint64_t base) {
    uint32_t lo = (uint32_t)(base & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(base >> 32);
    __asm__ volatile("wrmsr" : : "c"(MSR_FS_BASE), "a"(lo), "d"(hi));
}

static void wake_runnable_locked(struct fry_process *p, uint32_t count,
                                 uint32_t bsp, int32_t result) {
    if (!p) return;
    p->state = PROC_RUNNING;
    p->wake_time_ms = 0;
    p->wait_poll = 0;
    p->wait_result = result;
    if (p->cpu >= count) p->cpu = (uint8_t)bsp;
    rq_push(&rq[p->cpu], p);
}

static void rq_push(struct runqueue *q, struct fry_process *p) {
    p->next = 0;
    if (!q->head) {
        q->head = q->tail = p;
    } else {
        q->tail->next = p;
        q->tail = p;
    }
    q->count++;
}

static struct fry_process *rq_pop(struct runqueue *q) {
    if (!q->head) return 0;
    struct fry_process *p = q->head;
    q->head = p->next;
    if (!q->head) q->tail = 0;
    p->next = 0;
    if (q->count) q->count--;
    return p;
}

/*
 * Keep the first BSP handoff deterministic: if early kernel workers ended up
 * queued before the boot user, promote the first runnable userspace task to
 * the front once so the initial switch reaches userspace before background
 * kernel threads.
 */
static void rq_promote_first_user(struct runqueue *q) {
    if (!q || !q->head || !q->head->is_kernel) return;

    struct fry_process *prev = q->head;
    struct fry_process *cur = q->head->next;
    while (cur) {
        if (!cur->is_kernel) {
            prev->next = cur->next;
            if (q->tail == cur) q->tail = prev;
            cur->next = q->head;
            q->head = cur;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static struct fry_process *remove_from_runqueue(uint32_t pid) {
    uint32_t count = smp_cpu_count();
    if (count == 0) count = 1;
    for (uint32_t c = 0; c < count && c < MAX_CPUS; c++) {
        struct fry_process *prev = 0;
        struct fry_process *cur = rq[c].head;
        while (cur) {
            if (cur->pid == pid) {
                if (prev) prev->next = cur->next;
                else rq[c].head = cur->next;
                if (rq[c].tail == cur) rq[c].tail = prev;
                if (rq[c].count) rq[c].count--;
                cur->next = 0;
                return cur;
            }
            prev = cur;
            cur = cur->next;
        }
    }
    return 0;
}

static uint64_t sched_deliverable_signal_mask(const struct fry_process *p) {
    if (!p || !p->is_linux || !p->shared) return 0;
    uint64_t mask = p->linux_sig_pending & ~p->linux_sig_blocked;
    mask &= ~(1ULL << (9 - 1));   /* SIGKILL is never delivered to a handler */
    mask &= ~(1ULL << (19 - 1));  /* SIGSTOP is never delivered to a handler */
    for (uint32_t sig = 1; sig < 64; sig++) {
        uint64_t bit = 1ULL << (sig - 1);
        if ((mask & bit) && p->shared->linux_sigact[sig].handler)
            return bit;
    }
    return 0;
}

/* ---- Extended FPU/SSE/AVX state (XSAVE) management ---------------------
 * Without saving vector state across context switches, concurrent glibc
 * processes corrupt each other's XMM/YMM registers -> intermittent crashes
 * in libc string/math code. Required for any real threaded Linux binary. */
static uint64_t g_xcr0_mask = 0;   /* 0 = XSAVE not enabled (graceful fallback) */

static void fpu_enable_cpu(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    if (!(c & (1u << 26))) return;     /* CPUID.1:ECX.XSAVE absent -> stay disabled */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1UL << 2) | (1UL << 3));  /* clear EM (no x87 emulation) + TS (no lazy) */
    cr0 |=  (1UL << 1);                 /* set MP */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9) | (1UL << 10) | (1UL << 18); /* OSFXSR | OSXMMEXCPT | OSXSAVE */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0xDu), "c"(0u));
    (void)b; (void)d;
    /* fry1395: AVX-masked bisect (0x3) did NOT change the numLocals=0 crash —
     * vector save/restore EXONERATED. Restored to full x87+SSE+AVX. */
    uint64_t mask = ((uint64_t)a & 0x7ULL) | 0x1ULL; /* x87 + SSE + AVX, as supported */
    __asm__ volatile("xsetbv" : : "a"((uint32_t)mask), "d"(0u), "c"(0u));
    g_xcr0_mask = mask;
}

static inline void fpu_save(struct fry_process *p) {
    if (!g_xcr0_mask) return;
    __asm__ volatile("xsave64 (%0)"
                     : : "r"(p->fpu_area), "a"((uint32_t)g_xcr0_mask), "d"(0u)
                     : "memory");
}

static inline void fpu_restore(struct fry_process *p) {
    if (!g_xcr0_mask) return;
    __asm__ volatile("xrstor64 (%0)"
                     : : "r"(p->fpu_area), "a"((uint32_t)g_xcr0_mask), "d"(0u)
                     : "memory");
}

/* fry1381: save/restore FPU+SSE+AVX state to an arbitrary area, for preserving
 * vector state ACROSS SIGNAL DELIVERY. Signal handlers (glibc/Bun) freely use
 * SSE/AVX; the kernel only stored GP regs in the sigframe, so a signal landing
 * mid-vector-op (e.g. JSC's movups table rehash) corrupted xmm/ymm and the
 * interrupted code resumed with garbage -> deterministic JSC #GP. */
void lx_fpu_save_area(uint8_t *area) {
    if (!g_xcr0_mask || !area) return;
    __asm__ volatile("xsave64 (%0)"
                     : : "r"(area), "a"((uint32_t)g_xcr0_mask), "d"(0u)
                     : "memory");
}
void lx_fpu_restore_area(const uint8_t *area) {
    if (!g_xcr0_mask || !area) return;
    __asm__ volatile("xrstor64 (%0)"
                     : : "r"(area), "a"((uint32_t)g_xcr0_mask), "d"(0u)
                     : "memory");
}

__attribute__((noinline))
static void context_switch(struct fry_process *from, struct fry_process *to) {
    if (from == to) return;
    /* Save outgoing vector state, load incoming — BEFORE the GPR/stack swap.
     * (Doing it after the swap would run in the wrong task's frame.) */
    fpu_save(from);
    fpu_restore(to);
    /* fry1423 SMP context-save publish protocol:
     *   1. save outgoing rsp        (mov %%rsp, (from_rsp_ptr))
     *   2. clear from->on_cpu      — PUBLISH: context fully saved. x86 TSO
     *      guarantees other CPUs see the rsp store before the clear.
     *   3. load the new rsp from a REGISTER (to->saved_rsp was read into a
     *      register before step 2). After step 2 another CPU may immediately
     *      resume `from` on its old stack, so nothing here may reference
     *      that stack again — no memory operands after the clear. IRQs are
     *      off across the switch, so no interrupt frame lands on the old
     *      stack in the one-instruction gap either. */
    __asm__ volatile(
        "pushfq\n"
        "push %%rax\n"
        "push %%rbx\n"
        "push %%rcx\n"
        "push %%rdx\n"
        "push %%rbp\n"
        "push %%rsi\n"
        "push %%rdi\n"
        "push %%r8\n"
        "push %%r9\n"
        "push %%r10\n"
        "push %%r11\n"
        "push %%r12\n"
        "push %%r13\n"
        "push %%r14\n"
        "push %%r15\n"
        "mov %%rsp, (%0)\n"
        "movb $0, (%1)\n"
        "mov %2, %%rsp\n"
        "pop %%r15\n"
        "pop %%r14\n"
        "pop %%r13\n"
        "pop %%r12\n"
        "pop %%r11\n"
        "pop %%r10\n"
        "pop %%r9\n"
        "pop %%r8\n"
        "pop %%rdi\n"
        "pop %%rsi\n"
        "pop %%rbp\n"
        "pop %%rdx\n"
        "pop %%rcx\n"
        "pop %%rbx\n"
        "pop %%rax\n"
        "popfq\n"
        "ret\n"
        :
        : "r"(&from->saved_rsp), "r"(&from->on_cpu), "r"(to->saved_rsp)
        : "memory");
}

static void idle_loop(void *arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void init_boot_context(uint32_t cpu, uint64_t stack_top) {
    /* Enable XSAVE/AVX state management on this CPU before it can schedule. */
    fpu_enable_cpu();
    struct fry_process *boot = &boot_contexts[cpu];
    for (uint32_t i = 0; i < sizeof(*boot); i++) {
        ((uint8_t *)boot)[i] = 0;
    }
    *(uint32_t *)(boot->fpu_area + 24) = 0x1f80u; /* MXCSR default (exceptions masked) */
    boot->state = PROC_RUNNING;
    boot->cpu = (uint8_t)cpu;
    boot->on_cpu = 1;  /* fry1423: this context IS running on this CPU now */
    boot->is_kernel = 1;
    boot->cr3 = vmm_get_kernel_pml4_phys();
    boot->kernel_stack_top = stack_top;
    boot->name[0] = 'b';
    boot->name[1] = 'o';
    boot->name[2] = 'o';
    boot->name[3] = 't';
    boot->name[4] = 'c';
    boot->name[5] = 't';
    boot->name[6] = 'x';
    boot->name[7] = 0;
}

int sched_init(void) {
    uint32_t count = smp_cpu_count();
    uint32_t bsp = smp_bsp_index();
    if (count == 0) count = 1;
    if (bsp >= count) bsp = 0;
    for (uint32_t i = 0; i < count && i < MAX_CPUS; i++) {
        uint64_t irqf = irq_save_disable();
        spin_lock(&g_sched_lock);
        rq[i].head = rq[i].tail = 0;
        rq[i].count = 0;
        current[i] = 0;
        spin_unlock(&g_sched_lock);
        irq_restore(irqf);
        struct fry_process *idle = process_create_kernel(idle_loop, 0, "idle");
        if (idle) {
            irqf = irq_save_disable();
            spin_lock(&g_sched_lock);
            idle->cpu = i;
            idle_task[i] = idle;
            rq_push(&rq[i], idle);
            current[i] = idle;
            spin_unlock(&g_sched_lock);
            irq_restore(irqf);
        }
    }

    {
        uint64_t irqf = irq_save_disable();
        spin_lock(&g_sched_lock);
        init_boot_context(bsp, (uint64_t)(uintptr_t)&__kernel_stack_top);
        current[bsp] = &boot_contexts[bsp];
        /* Seed BSP percpu kstack with current RSP as safe default */
        {
            uint64_t cur_rsp;
            __asm__ volatile("mov %%rsp, %0" : "=r"(cur_rsp));
            percpu[bsp].kstack_top = cur_rsp;
        }
        g_sched_next_cpu = 0;
        spin_unlock(&g_sched_lock);
        irq_restore(irqf);
    }
    /* Signal APs that the scheduler is initialized */
    __asm__ volatile("" ::: "memory");
    g_sched_ready = 1;
    return 0;
}

struct fry_process *sched_current(void) {
    return current[cpu_index()];
}

void sched_set_current(struct fry_process *p) {
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    current[cpu_index()] = p;
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
}

void *sched_percpu_ptr(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return &percpu[0];
    return &percpu[cpu];
}

int sched_ap_ready(void) {
    return g_sched_ready != 0;
}

void sched_ap_start(uint64_t stack_top) {
    uint32_t cpu = cpu_index();
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    init_boot_context(cpu, stack_top);
    current[cpu] = &boot_contexts[cpu];
    percpu[cpu].kstack_top = stack_top;
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
    early_serial_puts("K_AP_SCHED cpu=");
    {
        char c = '0' + (char)cpu;
        early_serial_putc(c);
    }
    early_serial_puts("\n");
    /* Enable interrupts — LAPIC timer drives sched_tick */
    __asm__ volatile("sti");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void sched_add(uint32_t pid) {
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    struct fry_process *p = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid && procs[i].state == PROC_RUNNING) {
            p = &procs[i];
            break;
        }
    }
    if (!p) {
        spin_unlock(&g_sched_lock);
        irq_restore(irqf);
        return;
    }

    uint32_t count = smp_cpu_count();
    uint32_t bsp = smp_bsp_index();
    if (count == 0) count = 1;
    if (bsp >= count) bsp = 0;

    /* Keep userspace on the creator CPU until user-mode SMP paths are proven. */
    {
        uint32_t target;
        if (!p->is_kernel && p->cpu < count) {
            target = p->cpu;
        } else {
            target = g_sched_next_cpu;
            if (target >= count) target = 0;
            g_sched_next_cpu = target + 1;
            if (g_sched_next_cpu >= count) g_sched_next_cpu = 0;
        }
        p->cpu = (uint8_t)target;
        rq_push(&rq[target], p);
    }
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
}

void sched_remove(uint32_t pid) {
    sched_remove_with_state(pid, PROC_DEAD);
}

void sched_remove_with_state(uint32_t pid, enum proc_state state) {
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    struct fry_process *p = remove_from_runqueue(pid);
    if (p) {
        p->state = state;
    }
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
}

void sched_yield(void) {
    sched_tick();
}

void sched_sleep(uint32_t pid, uint64_t ms) {
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    uint64_t wake = now_ms() + ms;
    struct fry_process *p = remove_from_runqueue(pid);
    if (!p) {
        spin_unlock(&g_sched_lock);
        irq_restore(irqf);
        return;
    }
    p->state = PROC_WAITING;
    p->wake_time_ms = wake;
    p->wait_pid = 0;
    p->wait_tid = 0;
    p->wait_futex_key = 0;
    p->wait_futex_bitset = 0;
    p->wait_result = 0;
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
}

void sched_block(uint32_t pid) {
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    struct fry_process *p = remove_from_runqueue(pid);
    if (!p) {
        spin_unlock(&g_sched_lock);
        irq_restore(irqf);
        return;
    }
    p->state = PROC_WAITING;
    p->wake_time_ms = UINT64_MAX;
    p->wait_result = 0;
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
}

int sched_block_futex(uint32_t pid, volatile const uint32_t *word,
                      uint32_t expected, uint64_t key,
                      uint64_t wake_time_ms, uint32_t bitset) {
    uint64_t irqf = irq_save_disable();
    int rc = 0;
    spin_lock(&g_sched_lock);
    if (!word || *word != expected) {
        rc = -EAGAIN;
    } else {
        struct fry_process *p = remove_from_runqueue(pid);
        if (!p) {
            /* The caller is the current (running) task. Accept it whether it
             * is marked RUNNING or transiently WAITING: under SMP futex
             * contention a concurrent wake on another CPU can flip the
             * caller's state to WAITING right as it re-enters here. We just
             * re-block it on the new key (a stale wait is superseded).
             * Without this, glibc gets -ESRCH from a wait that must block and
             * aborts with "futex facility returned an unexpected error". */
            for (uint32_t i = 0; i < PROC_MAX; i++) {
                if (procs[i].pid == pid &&
                    (procs[i].state == PROC_RUNNING ||
                     procs[i].state == PROC_WAITING)) {
                    p = &procs[i];
                    break;
                }
            }
        }
        if (!p) {
            rc = -ESRCH;
        } else if (sched_deliverable_signal_mask(p)) {
            uint32_t count = smp_cpu_count();
            uint32_t bsp = smp_bsp_index();
            if (count == 0) count = 1;
            if (bsp >= count) bsp = 0;
            wake_runnable_locked(p, count, bsp, 0);
            rc = -EINTR;
        } else {
            if (key == TB_JSC_FUTEX_KEY) {
                kprint_serial_only("TBSCHED jsc-futex-block pid=%u tgid=%u state=%u word=%p current=%u expected=%u wake_time=%llu bitset=%x\n",
                                   p->pid, p->tgid, p->state, word,
                                   word ? (unsigned)*word : 0u, expected,
                                   (unsigned long long)wake_time_ms, bitset);
            }
            if (p->pid == TB_TRACE_CLAUDE_TGID || p->tgid == TB_TRACE_CLAUDE_TGID) {
                kprint_serial_only("TBSCHED futex-block pid=%u tgid=%u state=%u word=%p actual=%u expected=%u key=%lx wake_time=%llu bitset=%x\n",
                                   p->pid, p->tgid, p->state, word,
                                   word ? (unsigned)*word : 0u, expected,
                                   (unsigned long)key,
                                   (unsigned long long)wake_time_ms, bitset);
            }
            p->state = PROC_WAITING;
            p->wait_pid = 0;
            p->wait_tid = 0;
            p->wait_futex_key = key;
            p->wait_futex_bitset = bitset;
            p->wait_result = 0;
            p->wake_time_ms = wake_time_ms;
            if (*word != expected) {
                p->state = PROC_RUNNING;
                p->wait_futex_key = 0;
                p->wait_futex_bitset = 0;
                p->wake_time_ms = 0;
                rc = -EAGAIN;
            }
        }
    }
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
    return rc;
}

void sched_wake(uint32_t pid) {
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    struct fry_process *p = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid && procs[i].state == PROC_WAITING) {
            p = &procs[i];
            break;
        }
    }
    if (!p) {
        spin_unlock(&g_sched_lock);
        irq_restore(irqf);
        return;
    }
    p->state = PROC_RUNNING;
    p->wake_time_ms = 0;
    p->wait_futex_bitset = 0;
    p->wait_result = 0;
    uint32_t count = smp_cpu_count();
    uint32_t bsp = smp_bsp_index();
    if (count == 0) count = 1;
    if (bsp >= count) bsp = 0;
    wake_runnable_locked(p, count, bsp, 0);
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
}

uint32_t sched_wake_signal(uint32_t pid) {
    uint64_t irqf = irq_save_disable();
    uint32_t woke = 0;
    uint32_t count;
    uint32_t bsp;
    spin_lock(&g_sched_lock);
    count = smp_cpu_count();
    bsp = smp_bsp_index();
    if (count == 0) count = 1;
    if (bsp >= count) bsp = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid != pid) continue;
        if (procs[i].state != PROC_WAITING) break;
        if (!sched_deliverable_signal_mask(&procs[i])) break;
        if (procs[i].pid == TB_TRACE_CLAUDE_TGID || procs[i].tgid == TB_TRACE_CLAUDE_TGID) {
            kprint_serial_only("TBSCHED signal-wake pid=%u tgid=%u pending=%lx blocked=%lx futex_key=%lx\n",
                               procs[i].pid, procs[i].tgid,
                               (unsigned long)procs[i].linux_sig_pending,
                               (unsigned long)procs[i].linux_sig_blocked,
                               (unsigned long)procs[i].wait_futex_key);
        }
        procs[i].wait_futex_key = 0;
        procs[i].wait_futex_bitset = 0;
        wake_runnable_locked(&procs[i], count, bsp, -EINTR);
        woke = 1;
        break;
    }
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
    return woke;
}

void sched_block_poll(uint32_t pid, uint64_t wake_time_ms) {
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    struct fry_process *p = remove_from_runqueue(pid);
    if (!p) {
        spin_unlock(&g_sched_lock);
        irq_restore(irqf);
        return;
    }
    p->state = PROC_WAITING;
    p->wait_pid = 0;
    p->wait_tid = 0;
    p->wait_futex_key = 0;
    p->wait_futex_bitset = 0;
    p->wait_poll = 1;
    p->wait_result = 0;
    p->wake_time_ms = wake_time_ms;
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
}

uint32_t sched_wake_poll_waiters(void) {
    uint64_t irqf = irq_save_disable();
    uint32_t woke = 0;
    uint32_t count, bsp;
    spin_lock(&g_sched_lock);
    count = smp_cpu_count();
    bsp = smp_bsp_index();
    if (count == 0) count = 1;
    if (bsp >= count) bsp = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].state != PROC_WAITING) continue;
        if (!procs[i].wait_poll) continue;
        procs[i].wait_poll = 0;
        wake_runnable_locked(&procs[i], count, bsp, 0);
        woke++;
    }
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
    return woke;
}

uint32_t sched_wake_futex(uint64_t key, uint32_t max_wake,
                          uint32_t bitset, int32_t result) {
    uint64_t irqf = irq_save_disable();
    uint32_t woke = 0;
    uint32_t count;
    uint32_t bsp;
    if (max_wake == 0 || key == 0) {
        irq_restore(irqf);
        return 0;
    }
    spin_lock(&g_sched_lock);
    count = smp_cpu_count();
    bsp = smp_bsp_index();
    if (count == 0) count = 1;
    if (bsp >= count) bsp = 0;
    for (uint32_t i = 0; i < PROC_MAX && woke < max_wake; i++) {
        if (procs[i].state != PROC_WAITING) continue;
        if (procs[i].wait_futex_key != key) continue;
        if ((procs[i].wait_futex_bitset & bitset) == 0) continue;
        if (key == TB_JSC_FUTEX_KEY) {
            kprint_serial_only("TBSCHED jsc-futex-wake target=%u tgid=%u key=%lx bitset=%x waiter_bitset=%x result=%d\n",
                               procs[i].pid, procs[i].tgid,
                               (unsigned long)key, bitset,
                               procs[i].wait_futex_bitset, result);
        }
        if (procs[i].pid == TB_TRACE_CLAUDE_TGID || procs[i].tgid == TB_TRACE_CLAUDE_TGID) {
            kprint_serial_only("TBSCHED futex-wake pid=%u tgid=%u key=%lx bitset=%x result=%d\n",
                               procs[i].pid, procs[i].tgid,
                               (unsigned long)key, bitset, result);
        }
        procs[i].wait_futex_key = 0;
        procs[i].wait_futex_bitset = 0;
        wake_runnable_locked(&procs[i], count, bsp, result);
        woke++;
    }
    if (woke == 0 && ((key >> 48) == 0x8003ULL) && g_tb_futex_wake_miss_logs < 80) {
        uint32_t seen = 0;
        g_tb_futex_wake_miss_logs++;
        kprint_serial_only("TBSCHED futex-wake-miss key=%lx bitset=%x max=%u\n",
                           (unsigned long)key, bitset, max_wake);
        for (uint32_t i = 0; i < PROC_MAX && seen < 4; i++) {
            if (procs[i].state != PROC_WAITING) continue;
            if (procs[i].pid != TB_TRACE_CLAUDE_TGID && procs[i].tgid != TB_TRACE_CLAUDE_TGID) continue;
            if (procs[i].wait_futex_key == 0) continue;
            kprint_serial_only("TBSCHED futex-waiter pid=%u tgid=%u key=%lx bitset=%x wake_time=%llu result=%d\n",
                               procs[i].pid, procs[i].tgid,
                               (unsigned long)procs[i].wait_futex_key,
                               procs[i].wait_futex_bitset,
                               (unsigned long long)procs[i].wake_time_ms,
                               procs[i].wait_result);
            seen++;
        }
    }
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
    return woke;
}

/*
 * fry1506: race-free pid-wait (wait4) primitives. See sched.h. The OLD path had
 * a lost-wakeup race — process_wait4 checked the child's ZOMBIE/DEAD state with
 * NO lock, then blocked on an INFINITE wait, while proc_free marked the child
 * DEAD + woke waiters also with NO lock (g_sched_lock is private to sched.c so
 * process.c could not synchronize). On -smp the child's exit could land between
 * the waiter's check and its block -> the wake hit no waiter and was lost ->
 * claude's main thread (reaping an rg/sh subprocess after a kill->ESRCH) hung
 * forever -> whole-process deadlock (fry1500-1505). Putting BOTH sides under
 * g_sched_lock makes the exit either visible to the arm (reap, no block) or land
 * after the arm (wake hits the PROC_WAITING waiter).
 */
int sched_pidwait_arm(uint32_t caller_pid, uint32_t target_pid) {
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    int reapable = 1;   /* default: target gone/not-found -> don't block, re-loop */
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == target_pid && procs[i].state != PROC_UNUSED) {
            reapable = (procs[i].state == PROC_ZOMBIE ||
                        procs[i].state == PROC_DEAD) ? 1 : 0;
            break;
        }
    }
    if (!reapable) {
        struct fry_process *p = remove_from_runqueue(caller_pid);
        if (!p) {
            for (uint32_t i = 0; i < PROC_MAX; i++) {
                if (procs[i].pid == caller_pid &&
                    (procs[i].state == PROC_RUNNING ||
                     procs[i].state == PROC_WAITING)) { p = &procs[i]; break; }
            }
        }
        if (p) {
            p->state = PROC_WAITING;
            p->wait_pid = target_pid;
            p->wait_tid = 0;
            p->wait_futex_key = 0;
            p->wait_futex_bitset = 0;
            p->wait_result = 0;
            p->wake_time_ms = UINT64_MAX;
        } else {
            reapable = 1;   /* could not arm -> re-loop rather than sleep untracked */
        }
    }
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
    return reapable;
}

void sched_pidwait_exit(uint32_t target_pid) {
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    uint32_t count = smp_cpu_count();
    uint32_t bsp = smp_bsp_index();
    if (count == 0) count = 1;
    if (bsp >= count) bsp = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == target_pid &&
            procs[i].state != PROC_UNUSED &&
            procs[i].state != PROC_DEAD) {
            procs[i].state = PROC_DEAD;
        }
    }
    for (uint32_t w = 0; w < PROC_MAX; w++) {
        if (procs[w].state == PROC_WAITING && procs[w].wait_pid == target_pid) {
            procs[w].wait_pid = 0;
            wake_runnable_locked(&procs[w], count, bsp, 0);
        }
    }
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
}

uint32_t sched_requeue_futex(uint64_t key_from, uint64_t key_to,
                             uint32_t max_requeue) {
    uint64_t irqf = irq_save_disable();
    uint32_t moved = 0;
    if (max_requeue == 0 || key_from == 0 || key_to == 0 || key_from == key_to) {
        irq_restore(irqf);
        return 0;
    }
    spin_lock(&g_sched_lock);
    for (uint32_t i = 0; i < PROC_MAX && moved < max_requeue; i++) {
        if (procs[i].state != PROC_WAITING) continue;
        if (procs[i].wait_futex_key != key_from) continue;
        procs[i].wait_futex_key = key_to;
        moved++;
    }
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
    return moved;
}

void sched_tick(void) {
    uint64_t irqf = irq_save_disable();
    uint8_t mark_context_switch = 0;
    uint8_t mark_user_switch = 0;
    spin_lock(&g_sched_lock);
    uint32_t cpu = cpu_index();
    uint32_t bsp = smp_bsp_index();
    struct runqueue *q = &rq[cpu];
    /* fry1424 TBCLK heartbeat — BEFORE any early return so it always fires.
     * Every 5000 ticks (~5s at 1kHz) per CPU. fry1425: + tgid-3 thread
     * census (run/wait/zombie/dead counts) so "claude went silent" resolves
     * to threads-exited vs threads-stuck; + TBLOST overdue-sleeper detector
     * (WAITING with a timed wake >5s past due while now advances == the
     * sleeper-wake path lost it — the smoking gun for a lost-wakeup bug). */
    g_tbclk_ticks[cpu]++;
    /* fry1811: gate the whole TBCLK/TBMAIN/TBLOST heartbeat on the tracer being
     * ENABLED. Without this guard, setting TB_TRACE_CLAUDE_TGID to 0 does not
     * disable the census — it INVERTS it: the filter below skips a process
     * unless its pid/tgid equals the traced tgid, so a tgid of 0 matches every
     * UNUSED proc-table slot (pid 0) and the census prints for all of them.
     * Measured: 42,804 TBMAIN lines during a single boot. Disable means
     * disable. */
    if (TB_TRACE_CLAUDE_TGID && (g_tbclk_ticks[cpu] % 5000ULL) == 0) {
        uint64_t hb_now = now_ms();
        uint32_t t3run = 0, t3wait = 0, t3zomb = 0, t3dead = 0;
        for (uint32_t i = 0; i < PROC_MAX; i++) {
            if (procs[i].tgid != TB_TRACE_CLAUDE_TGID &&
                procs[i].pid != TB_TRACE_CLAUDE_TGID) continue;
            switch (procs[i].state) {
            case PROC_RUNNING: t3run++; break;
            case PROC_WAITING: t3wait++; break;
            case PROC_ZOMBIE:  t3zomb++; break;
            case PROC_DEAD:    t3dead++; break;
            default: break;
            }
            /* fry1568: per-thread line for the GROUP LEADER (pid==tgid = the JS
             * main/event-loop thread). state=RUNNING+oncpu persistently => the
             * main thread is busy-spinning in userspace (the freeze); WAITING =>
             * blocked (futex_key/wake_time say where). Pins spin-vs-blocked for
             * the exact thread that stops polling stdin. REMOVE once fixed. */
            if (procs[i].pid == procs[i].tgid) {
                uint32_t lcpu = procs[i].cpu;
                kprint_serial_only("TBMAIN pid=%u tgid=%u state=%u cpu=%u oncpu=%d futex_key=%lx poll=%u wake=%llu rip=%lx\n",
                                   procs[i].pid, procs[i].tgid,
                                   (unsigned)procs[i].state, lcpu,
                                   (lcpu < MAX_CPUS && current[lcpu] == &procs[i]) ? 1 : 0,
                                   (unsigned long)procs[i].wait_futex_key,
                                   (unsigned)procs[i].wait_poll,
                                   (unsigned long long)procs[i].wake_time_ms,
                                   (unsigned long)procs[i].user_rip);
            }
            if (procs[i].state == PROC_WAITING &&
                procs[i].wait_pid == 0 &&
                procs[i].wake_time_ms != UINT64_MAX &&
                procs[i].wake_time_ms + 5000 < hb_now &&
                g_tb_lost_wake_logs < 32) {
                g_tb_lost_wake_logs++;
                kprint_serial_only("TBLOST pid=%u tgid=%u wake_time=%llu now=%llu overdue_ms=%llu futex_key=%lx poll=%u\n",
                                   procs[i].pid, procs[i].tgid,
                                   (unsigned long long)procs[i].wake_time_ms,
                                   (unsigned long long)hb_now,
                                   (unsigned long long)(hb_now - procs[i].wake_time_ms),
                                   (unsigned long)procs[i].wait_futex_key,
                                   procs[i].wait_poll);
            }
        }
        kprint_serial_only("TBCLK cpu=%u ticks=%llu hpet=%llu cfg=%llx now=%llu t3=%u/%u/%u/%u rwzd\n",
                           cpu,
                           (unsigned long long)g_tbclk_ticks[cpu],
                           (unsigned long long)hpet_read_counter(),
                           (unsigned long long)hpet_read_config(),
                           (unsigned long long)hb_now,
                           t3run, t3wait, t3zomb, t3dead);
    }
    if (!q->head) {
        /* Empty runqueue. If the current task blocked itself (PROC_WAITING,
         * e.g. a futex/poll wait), it must NOT keep running — make this CPU's
         * idle task runnable and fall through to switch to it. Otherwise the
         * blocked task continues and a later wait sees a non-RUNNING current
         * task -> -ESRCH (glibc futex: "unexpected error code"). */
        struct fry_process *cur0 = current[cpu];
        struct fry_process *idle = idle_task[cpu];
        if (cur0 && cur0->state != PROC_RUNNING && idle && idle != cur0) {
            idle->state = PROC_RUNNING;
            idle->cpu = (uint8_t)cpu;
            rq_push(q, idle);
        } else {
            spin_unlock(&g_sched_lock);
            irq_restore(irqf);
            return;
        }
    }

    // Wake sleepers
    uint64_t now = now_ms();
    uint32_t count = smp_cpu_count();
    if (count == 0) count = 1;
    if (bsp >= count) bsp = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_WAITING &&
            procs[i].wait_pid == 0 &&
            procs[i].wake_time_ms <= now) {
            if (procs[i].wait_futex_key != 0) {
                if (procs[i].wait_futex_key == TB_JSC_FUTEX_KEY) {
                    kprint_serial_only("TBSCHED jsc-futex-timeout pid=%u tgid=%u key=%lx bitset=%x now=%llu wake_time=%llu\n",
                                       procs[i].pid, procs[i].tgid,
                                       (unsigned long)procs[i].wait_futex_key,
                                       procs[i].wait_futex_bitset,
                                       (unsigned long long)now,
                                       (unsigned long long)procs[i].wake_time_ms);
                }
                if ((procs[i].pid == TB_TRACE_CLAUDE_TGID ||
                     procs[i].tgid == TB_TRACE_CLAUDE_TGID) &&
                    g_tb_futex_timeout_logs < 80) {
                    g_tb_futex_timeout_logs++;
                    kprint_serial_only("TBSCHED futex-timeout pid=%u tgid=%u key=%lx bitset=%x now=%llu wake_time=%llu\n",
                                       procs[i].pid, procs[i].tgid,
                                       (unsigned long)procs[i].wait_futex_key,
                                       procs[i].wait_futex_bitset,
                                       (unsigned long long)now,
                                       (unsigned long long)procs[i].wake_time_ms);
                }
                procs[i].wait_futex_key = 0;
                procs[i].wait_futex_bitset = 0;
                wake_runnable_locked(&procs[i], count, bsp, -ETIMEDOUT);
                continue;
            }
            wake_runnable_locked(&procs[i], count, bsp, 0);
        }
    }

    /* fry1575: self-heal a STRANDED RUNNING thread. Scheduler invariant: a
     * PROC_RUNNING thread must be current[its cpu] OR present on some runqueue.
     * The claude main/event-loop thread (pid==tgid) has been observed
     * PROC_RUNNING but NEITHER current NOR enqueued (fry1574: state=1, oncpu=0,
     * never caught running) -> never scheduled -> event loop frozen -> stdin
     * never read -> typing dead. Until the exact drop path is pinned, restore
     * the invariant: if the main thread is RUNNING, not mid-context-save, not
     * current on its cpu, and not on any runqueue, push it back so it runs
     * again. Scoped to the traced main thread to bound cost; the rq scan only
     * runs in the suspicious RUNNING-not-current case. Logged (TBREQUEUE). */
    if (g_tb_trace_claude_tgid) {
        for (uint32_t i = 0; i < PROC_MAX; i++) {
            if (procs[i].pid != g_tb_trace_claude_tgid ||
                procs[i].tgid != g_tb_trace_claude_tgid) continue;
            if (procs[i].state != PROC_RUNNING || procs[i].on_cpu) break;
            uint32_t mcpu = procs[i].cpu;
            if (mcpu >= count) { procs[i].cpu = (uint8_t)bsp; mcpu = bsp; }
            if (mcpu < MAX_CPUS && current[mcpu] == &procs[i]) break;
            int on_rq = 0;
            for (uint32_t c = 0; c < count && c < MAX_CPUS && !on_rq; c++) {
                for (struct fry_process *t = rq[c].head; t; t = t->next) {
                    if (t == &procs[i]) { on_rq = 1; break; }
                }
            }
            if (!on_rq) {
                if (g_tb_requeue_logs < 80) {
                    g_tb_requeue_logs++;
                    kprint_serial_only("TBREQUEUE pid=%u tgid=%u orphaned RUNNING (not current, not on rq) -> re-enqueue rq[%u]\n",
                                       procs[i].pid, procs[i].tgid, mcpu);
                }
                rq_push(&rq[mcpu], &procs[i]);
            }
            break;
        }
    }

    struct fry_process *cur = current[cpu];
    if (cur && cur->state == PROC_RUNNING) {
        rq_push(q, rq_pop(q));
    }

    if (cpu == bsp && !g_first_user_switch_seen) {
        rq_promote_first_user(q);
    }

    struct fry_process *next = q->head;
    /* fry1423 SMP context-save gate: never switch to a task whose on_cpu is
     * still set — its previous CPU has not finished saving its context
     * (saved_rsp is stale, that CPU is still ON its kernel stack; running it
     * here is the fry1422 freeze). Rotate busy candidates to the back; they
     * become eligible the moment their old CPU's context_switch publishes
     * the save (sub-microsecond), i.e. by the next tick at the latest. */
    if (next && next != cur && next->on_cpu) {
        uint32_t scan = q->count;
        while (scan-- > 0 && q->head && q->head != cur && q->head->on_cpu) {
            rq_push(q, rq_pop(q));
        }
        next = q->head;
        if (next && next != cur && next->on_cpu) {
            if (g_tb_oncpu_defer_logs < 40) {
                g_tb_oncpu_defer_logs++;
                kprint_serial_only("TBSCHED oncpu-defer cpu=%u pid=%u tgid=%u\n",
                                   cpu, next->pid, next->tgid);
            }
            if (cur && cur->state == PROC_RUNNING) {
                spin_unlock(&g_sched_lock);
                irq_restore(irqf);
                return;
            }
            next = 0;  /* nothing safely runnable — take the idle fallback */
        }
    }
    if (!next || next == cur) {
        /* No other runnable task queued. If the current task is still
         * RUNNING, just keep running it. But if it blocked itself
         * (PROC_WAITING etc., e.g. a futex/poll wait), it MUST NOT keep
         * running — fall back to this CPU's idle task. Otherwise the blocked
         * task continues and a later wait sees a non-RUNNING "current" and
         * fails with -ESRCH (glibc futex: "unexpected error code"). */
        if (cur && cur->state == PROC_RUNNING) {
            spin_unlock(&g_sched_lock);
            irq_restore(irqf);
            return;
        }
        struct fry_process *idle = idle_task[cpu];
        if (!idle || idle == cur) {
            spin_unlock(&g_sched_lock);
            irq_restore(irqf);
            return;
        }
        remove_from_runqueue(idle->pid);  /* ensure single rq presence */
        idle->state = PROC_RUNNING;
        idle->cpu = (uint8_t)cpu;
        rq_push(q, idle);
        /* This CPU's idle task can never be mid-save on another CPU, so it
         * is always a safe pick (also covers the gate's nothing-runnable
         * fallthrough, where q->head may still be an on_cpu-busy task). */
        next = idle;
    }

    current[cpu] = next;
    /* fry1423: claim next's stack before dropping the lock — no other CPU
     * may pick it until our context_switch publishes its next save. */
    next->on_cpu = 1;
    struct tss64 *tss = smp_get_tss(cpu);
    if (tss) {
        tss_set_rsp0_local(tss, next->kernel_stack_top);
    }
    /* Update per-CPU syscall kernel stack pointer for the newly scheduled
     * process.  syscall_entry reads this via SWAPGS + %gs:0 on all CPUs. */
    percpu[cpu].kstack_top = next->kernel_stack_top;
    write_user_fs_base(next->user_fs_base);
    tb_program_jsc_slot_watchpoints_for(next);
    if (cpu == bsp && !g_first_context_switch_seen) {
        g_first_context_switch_seen = 1;
        mark_context_switch = 1;
    }
    if (cpu == bsp && !next->is_kernel && !g_first_user_switch_seen) {
        g_first_user_switch_seen = 1;
        mark_user_switch = 1;
    }
    spin_unlock(&g_sched_lock);
    if (mark_context_switch) {
        boot_diag_stage(34);
        if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_FIRST_SWITCH\n");
    }
    if (mark_user_switch) {
        boot_diag_stage(35);
        if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_FIRST_USER\n");
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(next->cr3));
    context_switch(cur, next);
    process_reap_deferred_stacks();
    irq_restore(irqf);
}
