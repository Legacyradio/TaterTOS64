// Round-robin scheduler

#include <stdint.h>
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
extern struct fry_handoff *g_handoff;

// Tracks the kernel stack top of the currently running process on the BSP.
// syscall_entry reads this to switch off the user stack before any kernel work.
// Updated by sched_tick() every time a new process is scheduled on the BSP.
uint64_t g_syscall_kstack_top = 0;

#define MAX_CPUS 64

struct runqueue {
    struct fry_process *head;
    struct fry_process *tail;
    uint32_t count;
};

static struct runqueue rq[MAX_CPUS];
static struct fry_process *current[MAX_CPUS];
/*
 * The BSP boot thread starts life on the linker-defined bootstrap stack, not
 * on a scheduler-owned kernel stack. Keep a bookkeeping-only context for that
 * one-way handoff, but never enqueue it as a runnable task.
 */
static struct fry_process boot_contexts[MAX_CPUS];
static spinlock_t g_sched_lock = {0};
static uint8_t g_first_context_switch_seen;
static uint8_t g_first_user_switch_seen;

extern char __kernel_stack_top;

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

__attribute__((noinline))
static void context_switch(struct fry_process *from, struct fry_process *to) {
    if (from == to) return;
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
        "mov %%rsp, %0\n"
        "mov %1, %%rsp\n"
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
        : "=m"(from->saved_rsp)
        : "m"(to->saved_rsp)
        : "memory");
}

static void idle_loop(void *arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void init_boot_context(uint32_t cpu) {
    struct fry_process *boot = &boot_contexts[cpu];
    for (uint32_t i = 0; i < sizeof(*boot); i++) {
        ((uint8_t *)boot)[i] = 0;
    }
    boot->state = PROC_RUNNING;
    boot->cpu = (uint8_t)cpu;
    boot->is_kernel = 1;
    boot->cr3 = vmm_get_kernel_pml4_phys();
    boot->kernel_stack_top = (uint64_t)(uintptr_t)&__kernel_stack_top;
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
            rq_push(&rq[i], idle);
            current[i] = idle;
            spin_unlock(&g_sched_lock);
            irq_restore(irqf);
        }
    }

    {
        uint64_t irqf = irq_save_disable();
        spin_lock(&g_sched_lock);
        init_boot_context(bsp);
        current[bsp] = &boot_contexts[bsp];
        spin_unlock(&g_sched_lock);
        irq_restore(irqf);
    }
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

    /* BSP-only scheduling: lapic_timer currently advances runqueues only on the BSP.
       Keeping runnable tasks on AP queues causes app launches to stall forever. */
    p->cpu = (uint8_t)bsp;
    rq_push(&rq[bsp], p);
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
}

void sched_remove(uint32_t pid) {
    uint64_t irqf = irq_save_disable();
    spin_lock(&g_sched_lock);
    struct fry_process *p = remove_from_runqueue(pid);
    if (p) {
        p->state = PROC_DEAD;
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
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
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
    uint32_t count = smp_cpu_count();
    uint32_t bsp = smp_bsp_index();
    if (count == 0) count = 1;
    if (bsp >= count) bsp = 0;
    if (p->cpu >= count) p->cpu = (uint8_t)bsp;
    rq_push(&rq[p->cpu], p);
    spin_unlock(&g_sched_lock);
    irq_restore(irqf);
}

void sched_tick(void) {
    uint64_t irqf = irq_save_disable();
    uint8_t mark_context_switch = 0;
    uint8_t mark_user_switch = 0;
    spin_lock(&g_sched_lock);
    uint32_t cpu = cpu_index();
    uint32_t bsp = smp_bsp_index();
    struct runqueue *q = &rq[cpu];
    if (!q->head) {
        spin_unlock(&g_sched_lock);
        irq_restore(irqf);
        return;
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
            procs[i].state = PROC_RUNNING;
            if (procs[i].cpu >= count) {
                procs[i].cpu = (uint8_t)bsp;
            }
            rq_push(&rq[procs[i].cpu], &procs[i]);
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
    if (!next || next == cur) {
        spin_unlock(&g_sched_lock);
        irq_restore(irqf);
        return;
    }

    current[cpu] = next;
    struct tss64 *tss = smp_get_tss(cpu);
    if (tss) {
        tss_set_rsp0_local(tss, next->kernel_stack_top);
    }
    // Keep g_syscall_kstack_top in sync so syscall_entry can find the kernel
    // stack for the newly scheduled process.  Only the BSP handles syscalls.
    if (cpu == bsp) {
        g_syscall_kstack_top = next->kernel_stack_top;
    }
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
