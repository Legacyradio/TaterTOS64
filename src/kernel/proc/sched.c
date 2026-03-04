// Round-robin scheduler

#include <stdint.h>
#include "sched.h"
#include "process.h"
#include "../../drivers/smp/smp.h"
#include "../../drivers/irqchip/lapic.h"
#include "../../drivers/timer/hpet.h"
#include "../../boot/tss.h"

void kprint(const char *fmt, ...);

// Tracks the kernel stack top of the currently running process on BSP.
// syscall_entry reads this to switch off the user stack before any kernel work.
// Updated by sched_tick() every time a new process is scheduled on BSP (cpu 0).
uint64_t g_syscall_kstack_top = 0;

#define MAX_CPUS 64

struct runqueue {
    struct fry_process *head;
    struct fry_process *tail;
    uint32_t count;
};

static struct runqueue rq[MAX_CPUS];
static struct fry_process *current[MAX_CPUS];

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

int sched_init(void) {
    uint32_t count = smp_cpu_count();
    if (count == 0) count = 1;
    for (uint32_t i = 0; i < count && i < MAX_CPUS; i++) {
        rq[i].head = rq[i].tail = 0;
        rq[i].count = 0;
        current[i] = 0;
        struct fry_process *idle = process_create_kernel(idle_loop, 0, "idle");
        if (idle) {
            idle->cpu = i;
            rq_push(&rq[i], idle);
            current[i] = idle;
        }
    }
    return 0;
}

struct fry_process *sched_current(void) {
    return current[cpu_index()];
}

void sched_set_current(struct fry_process *p) {
    current[cpu_index()] = p;
}

void sched_add(uint32_t pid) {
    uint64_t irqf = irq_save_disable();
    struct fry_process *p = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid && procs[i].state == PROC_RUNNING) {
            p = &procs[i];
            break;
        }
    }
    if (!p) {
        irq_restore(irqf);
        return;
    }

    /* BSP-only scheduling: lapic_timer currently advances runqueues only on CPU0.
       Keeping runnable tasks on AP queues causes app launches to stall forever. */
    p->cpu = 0;
    rq_push(&rq[0], p);
    irq_restore(irqf);
}

void sched_remove(uint32_t pid) {
    uint64_t irqf = irq_save_disable();
    struct fry_process *p = remove_from_runqueue(pid);
    if (p) {
        p->state = PROC_DEAD;
    }
    irq_restore(irqf);
}

void sched_yield(void) {
    uint64_t irqf = irq_save_disable();
    sched_tick();
    irq_restore(irqf);
}

void sched_sleep(uint32_t pid, uint64_t ms) {
    uint64_t irqf = irq_save_disable();
    uint64_t wake = now_ms() + ms;
    struct fry_process *p = remove_from_runqueue(pid);
    if (!p) {
        irq_restore(irqf);
        return;
    }
    p->state = PROC_WAITING;
    p->wake_time_ms = wake;
    p->wait_pid = 0;
    irq_restore(irqf);
}

void sched_block(uint32_t pid) {
    uint64_t irqf = irq_save_disable();
    struct fry_process *p = remove_from_runqueue(pid);
    if (!p) {
        irq_restore(irqf);
        return;
    }
    p->state = PROC_WAITING;
    p->wake_time_ms = UINT64_MAX;
    irq_restore(irqf);
}

void sched_wake(uint32_t pid) {
    uint64_t irqf = irq_save_disable();
    struct fry_process *p = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid && procs[i].state == PROC_WAITING) {
            p = &procs[i];
            break;
        }
    }
    if (!p) {
        irq_restore(irqf);
        return;
    }
    p->state = PROC_RUNNING;
    p->wake_time_ms = 0;
    uint32_t count = smp_cpu_count();
    if (count == 0) count = 1;
    if (p->cpu >= count) p->cpu = 0;
    rq_push(&rq[p->cpu], p);
    irq_restore(irqf);
}

void sched_tick(void) {
    uint32_t cpu = cpu_index();
    struct runqueue *q = &rq[cpu];
    if (!q->head) return;

    // Wake sleepers
    uint64_t now = now_ms();
    uint32_t count = smp_cpu_count();
    if (count == 0) count = 1;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_WAITING &&
            procs[i].wait_pid == 0 &&
            procs[i].wake_time_ms <= now) {
            procs[i].state = PROC_RUNNING;
            if (procs[i].cpu >= count) {
                procs[i].cpu = 0;
            }
            rq_push(&rq[procs[i].cpu], &procs[i]);
        }
    }

    struct fry_process *cur = current[cpu];
    if (cur && cur->state == PROC_RUNNING) {
        rq_push(q, rq_pop(q));
    }

    struct fry_process *next = q->head;
    if (!next || next == cur) return;

    current[cpu] = next;
    struct tss64 *tss = smp_get_tss(cpu);
    if (tss) {
        tss_set_rsp0_local(tss, next->kernel_stack_top);
    }
    // Keep g_syscall_kstack_top in sync so syscall_entry can find the kernel
    // stack for the newly scheduled process.  Only BSP (cpu 0) handles syscalls.
    if (cpu == 0) {
        g_syscall_kstack_top = next->kernel_stack_top;
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(next->cr3));
    context_switch(cur, next);
}
