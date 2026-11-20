// Process management

#include "process.h"
#include "syscall.h"
#include "sched.h"
#include "elf.h"
#include "linux_compat.h"
#include <errno.h>
#include "../fs/vfs.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../../drivers/net/netcore.h"
void kprint(const char *fmt, ...);
void kprint_serial_only(const char *fmt, ...);

#define KSTACK_SIZE 32768
#define PAGE_SIZE   4096ULL
#define MSR_FS_BASE 0xC0000100u

struct fry_process procs[PROC_MAX];
static uint32_t next_pid;
static int g_process_last_launch_error;
static struct {
    uint64_t phys;
    uint32_t pages;
} g_deferred_kstacks[PROC_MAX];

static void mem_zero(void *p, uint64_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

static struct fry_process_shared *proc_shared(struct fry_process *p) {
    return p ? p->shared : 0;
}

static const struct fry_process_shared *proc_shared_const(const struct fry_process *p) {
    return p ? p->shared : 0;
}

static void shared_release(struct fry_process *p);

static int proc_is_user_group_leader(const struct fry_process *p) {
    return p && !p->is_kernel && p->shared && p->pid == p->tgid;
}

/* Global pipe pool */
struct fry_pipe g_pipes[FRY_PIPE_MAX];

/* Global socket pool */
struct fry_socket g_sockets[FRY_SOCK_MAX];

static void shared_init(struct fry_process *p) {
    struct fry_process_shared *shared = &p->owned_shared;
    mem_zero(shared, sizeof(*shared));
    shared->owner_pid = p->pid;
    shared->refcount = 1;
    shared->cwd[0] = '/';
    shared->cwd[1] = 0;
    for (uint32_t f = 0; f < FRY_FD_MAX; f++) {
        shared->fd_table[f] = -1;
        shared->fd_ptrs[f] = 0;
        shared->fd_kind[f] = FD_NONE;
        shared->fd_flags[f] = 0;
        shared->fd_paths[f][0] = 0;
    }
    shared->heap_start = 0x10000000000ULL;
    shared->heap_end = shared->heap_start;
    shared->argc = 0;
    shared->envc = 0;
    p->shared = shared;
}

static void copy_name(char dst[32], const char *src) {
    if (!dst) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    uint32_t i = 0;
    while (src[i] && i < 31) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static uint64_t read_rsp(void) {
    uint64_t v;
    __asm__ volatile("mov %%rsp, %0" : "=r"(v));
    return v;
}

static uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void write_user_fs_base(uint64_t base) {
    uint32_t lo = (uint32_t)(base & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(base >> 32);
    __asm__ volatile("wrmsr" : : "c"(MSR_FS_BASE), "a"(lo), "d"(hi));
}

static int alloc_kernel_stack(struct fry_process *p) {
    if (!p) return -1;
    uint32_t pages = (uint32_t)(KSTACK_SIZE / PAGE_SIZE);
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) return -1;
    p->kernel_stack_phys = phys;
    p->kernel_stack_pages = pages;
    p->kernel_stack_top = vmm_phys_to_virt(phys) + KSTACK_SIZE;
    return 0;
}

static void free_kernel_stack(struct fry_process *p) {
    if (!p || !p->kernel_stack_phys || !p->kernel_stack_pages) return;
    pmm_free_pages(p->kernel_stack_phys, p->kernel_stack_pages);
    p->kernel_stack_phys = 0;
    p->kernel_stack_pages = 0;
    p->kernel_stack_top = 0;
}

static void defer_kernel_stack_free(struct fry_process *p) {
    if (!p || !p->kernel_stack_phys || !p->kernel_stack_pages) return;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (g_deferred_kstacks[i].phys == 0) {
            g_deferred_kstacks[i].phys = p->kernel_stack_phys;
            g_deferred_kstacks[i].pages = p->kernel_stack_pages;
            p->kernel_stack_phys = 0;
            p->kernel_stack_pages = 0;
            p->kernel_stack_top = 0;
            return;
        }
    }

    kprint("proc_free: deferred stack queue full, leaking stack pid=%u\n", p->pid);
    p->kernel_stack_phys = 0;
    p->kernel_stack_pages = 0;
    p->kernel_stack_top = 0;
}

void process_reap_deferred_stacks(void) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (!g_deferred_kstacks[i].phys || !g_deferred_kstacks[i].pages) continue;
        pmm_free_pages(g_deferred_kstacks[i].phys, g_deferred_kstacks[i].pages);
        g_deferred_kstacks[i].phys = 0;
        g_deferred_kstacks[i].pages = 0;
    }
}

static void process_start(struct fry_process *p) {
    if (!p) for (;;) __asm__ volatile("hlt");

    if (p->is_kernel && p->kentry) {
        p->kentry(p->karg);
        for (;;) __asm__ volatile("hlt");
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"(p->cr3));
    write_user_fs_base(p->user_fs_base);
    tb_program_jsc_slot_watchpoints_for(p);

    if (p->is_linux && p->is_clone_child) {
        /*
         * clone()/clone3() child: enter user mode with the FULL parent
         * register set (rax forced to 0, rsp=new stack), so glibc's thread
         * bootstrap finds fn/arg in the callee-saved registers it stashed
         * across the clone syscall. cr3 + FS base are already set above.
         *
         * Build a [15 GPRs][iret frame] block on the kernel stack from
         * clone_ctx, then pop the GPRs and iretq. The ctx base register is
         * only READ during the pushes, so it can be safely overwritten by
         * the later pops.
         */
        struct linux_clone_ctx *ctx = &p->clone_ctx;
        __asm__ volatile(
            "cli\n"
            "swapgs\n"
            "movw $0x23, %%ax\n"
            "movw %%ax, %%ds\n"
            "movw %%ax, %%es\n"
            "movw %%ax, %%fs\n"
            "movw %%ax, %%gs\n"
            /* The %fs selector load just zeroed FS.BASE — restore the TLS
             * base (%1) via wrmsr so %fs:0x28 (stack canary) etc. work.
             * Clobbers rax/rcx/rdx, all reloaded from clone_ctx by the pops. */
            "movq %1, %%rdx\n"
            "movq %%rdx, %%rax\n"
            "shrq $32, %%rdx\n"
            "movl $0xC0000100, %%ecx\n"
            "wrmsr\n"
            /* iret frame: ss, rsp, rflags, cs, rip (pushed high->low) */
            "pushq $0x23\n"
            "pushq 16*8(%0)\n"
            "pushq 17*8(%0)\n"
            "pushq $0x2b\n"
            "pushq 15*8(%0)\n"
            /* GPRs in reverse pop order: r15..rax (rax ends on top) */
            "pushq 14*8(%0)\n"
            "pushq 13*8(%0)\n"
            "pushq 12*8(%0)\n"
            "pushq 11*8(%0)\n"
            "pushq 10*8(%0)\n"
            "pushq 9*8(%0)\n"
            "pushq 8*8(%0)\n"
            "pushq 7*8(%0)\n"
            "pushq 6*8(%0)\n"
            "pushq 5*8(%0)\n"
            "pushq 4*8(%0)\n"
            "pushq 3*8(%0)\n"
            "pushq 2*8(%0)\n"
            "pushq 1*8(%0)\n"
            "pushq 0*8(%0)\n"
            "popq %%rax\n"
            "popq %%rbx\n"
            "popq %%rcx\n"
            "popq %%rdx\n"
            "popq %%rsi\n"
            "popq %%rdi\n"
            "popq %%rbp\n"
            "popq %%r8\n"
            "popq %%r9\n"
            "popq %%r10\n"
            "popq %%r11\n"
            "popq %%r12\n"
            "popq %%r13\n"
            "popq %%r14\n"
            "popq %%r15\n"
            "iretq\n"
            :
            : "r"(ctx), "r"(p->user_fs_base)
            : "rax", "rcx", "rdx", "memory"
        );
    }

    __asm__ volatile(
        "cli\n"
        "mov %2, %%rdi\n"
        // SWAPGS first: save percpu GS.BASE to KERNEL_GS_BASE before
        // loading user segment selectors.  On Intel/QEMU, mov to %gs
        // zeroes MSR_GS_BASE (loads base from descriptor), so SWAPGS
        // must run while GS.BASE still holds the percpu pointer.
        // Before: GS.BASE = percpu (kernel), KERNEL_GS = 0 (user)
        // After:  GS.BASE = 0 (user),        KERNEL_GS = percpu (kernel)
        "swapgs\n"
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "pushq $0x23\n"
        "pushq %0\n"
        "pushq $0x202\n"
        "pushq $0x2B\n"
        "pushq %1\n"
        // Set user RAX last (segment loads above clobber AX). Normal _start
        // ignores RAX; clone() children require RAX=0 to take the child path.
        "mov %3, %%rax\n"
        "iretq\n"
        :
        : "r"(p->user_rsp), "r"(p->user_rip), "r"(p->user_arg), "r"(p->user_rax)
        : "rax", "rdi", "memory"
    );
}

static void setup_initial_stack(struct fry_process *p) {
    uint64_t *sp = (uint64_t *)(uintptr_t)p->kernel_stack_top;
    /*
     * Context switch restores regs + rflags, then uses `ret` into process_start.
     * Add one padding slot above that return target so process_start starts with
     * SysV-conformant stack alignment (%rsp % 16 == 8 at function entry).
     */
    *(--sp) = 0; // padding/sentinel (unused unless process_start returns)
    *(--sp) = (uint64_t)(uintptr_t)process_start; // ret target
    *(--sp) = 0x202; // rflags
    *(--sp) = 0; // rax
    *(--sp) = 0; // rbx
    *(--sp) = 0; // rcx
    *(--sp) = 0; // rdx
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rsi
    *(--sp) = (uint64_t)(uintptr_t)p; // rdi
    *(--sp) = 0; // r8
    *(--sp) = 0; // r9
    *(--sp) = 0; // r10
    *(--sp) = 0; // r11
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15
    p->saved_rsp = (uint64_t)(uintptr_t)sp;
}

int process_init(void) {
    next_pid = 1;
    g_process_last_launch_error = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        mem_zero(&procs[i], sizeof(procs[i]));
        procs[i].state = PROC_UNUSED;
    }
    return 0;
}

int process_last_launch_error(void) {
    return g_process_last_launch_error;
}

static struct fry_process *proc_alloc(void) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_UNUSED || procs[i].state == PROC_DEAD) {
            mem_zero(&procs[i], sizeof(procs[i]));
            procs[i].pid = next_pid++;
            procs[i].tgid = procs[i].pid;
            procs[i].state = PROC_RUNNING;
            procs[i].cpu = 0xFF;
            procs[i].wait_pid = 0;
            procs[i].wait_tid = 0;
            procs[i].dumpable = 1;
            procs[i].timer_slack_ns = 50000;
            /* Seed MXCSR so the first XRSTOR (zeroed XSAVE area) yields a clean,
             * exception-masked FPU init state instead of MXCSR=0. */
            *(uint32_t *)(procs[i].fpu_area + 24) = 0x1f80u;
            return &procs[i];
        }
    }
    return 0;
}

struct fry_process *process_create_user(uint64_t cr3, uint64_t entry, uint64_t user_rsp, const char *name) {
    struct fry_process *p = proc_alloc();
    if (!p) return 0;
    p->cr3 = cr3;
    p->tgid = p->pid;
    p->user_arg = 0;
    p->user_fs_base = 0;
    p->user_rip = entry;
    p->user_rsp = user_rsp;
    shared_init(p);
    copy_name(p->name, name);

    if (alloc_kernel_stack(p) != 0) {
        p->shared = 0;
        p->state = PROC_DEAD;
        return 0;
    }

    setup_initial_stack(p);
    return p;
}

struct fry_process *process_create_kernel(void (*entry)(void *), void *arg, const char *name) {
    struct fry_process *p = proc_alloc();
    if (!p) return 0;
    p->cr3 = vmm_get_kernel_pml4_phys();
    p->tgid = p->pid;
    p->is_kernel = 1;
    p->kentry = entry;
    p->karg = arg;
    copy_name(p->name, name);

    if (alloc_kernel_stack(p) != 0) {
        p->state = PROC_DEAD;
        return 0;
    }

    setup_initial_stack(p);
    return p;
}

/* fry1790: per-thread TLS support. The ELF loader builds variant-II TLS only for
 * the MAIN thread; spawned native threads got user_fs_base=0 and shared the main
 * thread's __thread storage -> CPython's _PyThreadState_Attach saw a non-NULL old
 * tstate and fatally aborted. These give each spawned thread its OWN TLS block. */
#define FRY_TLS_REGION_BASE_P       0x00007FFF80000000ULL  /* must match elf.c */
#define FRY_TLS_TCB_RESERVE_P       0x1000ULL
#define FRY_TLS_THREAD_ARENA_BASE   0x00007FFF80100000ULL  /* 1MB above main TLS */
#define FRY_TLS_THREAD_ARENA_TOP    0x00007FFFFE000000ULL  /* below the 16MB stack */

void process_tls_stash_template(struct fry_process *p, const struct fry_tls_template *tmpl) {
    struct fry_process_shared *shared = proc_shared(p);
    if (!shared || !tmpl) return;
    shared->tls_tmpl = *tmpl;
    shared->tls_thread_next = FRY_TLS_THREAD_ARENA_BASE;
}

/* Build a private variant-II TLS block for a spawned thread and set its %fs base.
 * Layout mirrors elf.c: [.tdata | .tbss | TCB], tp = base + tls_alloc, the word
 * at tp (fs:0) holds tp (self-pointer), .tdata copied pristine from the template
 * vaddr (read-only in the image). Returns 0 ok (incl. "no PT_TLS"), -1 on
 * OOM / arena exhaustion. Runs with the parent's cr3 active (same address space).*/
/* fry1799: SEPARATE budgets per probe class. fry1798 used ONE global budget and
 * the main thread's high-frequency SYS_GET_TLS_BASE calls burned all 96 lines by
 * serial line 171 of 981 — the decisive spawn/build events at line ~975 printed
 * NOTHING. Rare events must not compete with hot ones for the same budget.
 * Diagnostic only, no logic change. */
int g_tlsdbg_spawn_budget = 64;   /* build/spawn/set: rare, bounded by threads */
int g_tlsdbg_get_budget   = 64;   /* get: hot, and gated to spawned threads */

static int process_build_thread_tls(struct fry_process *p, struct fry_process_shared *shared) {
    const struct fry_tls_template *t = &shared->tls_tmpl;
    if (!t->has_tls || t->memsz == 0) {
        p->user_fs_base = 0;
        if (g_tlsdbg_spawn_budget > 0) {
            g_tlsdbg_spawn_budget--;
            kprint_serial_only("TLSDBG build tid=%u NO_PT_TLS early_out fs=0 has_tls=%u memsz=%lu\n",
                               p->pid, (unsigned)t->has_tls, (unsigned long)t->memsz);
        }
        return 0;
    }

    uint64_t align = t->align < 1 ? 1 : t->align;
    uint64_t bbase = (shared->tls_thread_next + (align - 1)) & ~(align - 1);
    uint64_t tp    = bbase + t->tls_alloc;
    uint64_t map_start = bbase & ~0xFFFULL;
    uint64_t map_end   = (tp + FRY_TLS_TCB_RESERVE_P + 0xFFFULL) & ~0xFFFULL;
    if (map_end >= FRY_TLS_THREAD_ARENA_TOP) return -1;

    for (uint64_t va = map_start; va < map_end; va += 4096) {
        if (vmm_virt_to_phys_user(p->cr3, va)) continue;
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -1;
        vmm_map_user(p->cr3, va, phys, VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE);
        mem_zero((uint8_t *)(uintptr_t)vmm_phys_to_virt(phys), 4096);
    }

    /* copy pristine .tdata (small: ~121B for python); byte-wise across pages */
    for (uint64_t off = 0; off < t->filesz; off++) {
        uint64_t svp = t->vaddr + off, dvp = bbase + off;
        uint64_t sph = vmm_virt_to_phys_user(p->cr3, svp & ~0xFFFULL) & ~0xFFFULL;
        uint64_t dph = vmm_virt_to_phys_user(p->cr3, dvp & ~0xFFFULL) & ~0xFFFULL;
        if (!sph || !dph) return -1;
        *(uint8_t *)(uintptr_t)(vmm_phys_to_virt(dph) + (dvp & 0xFFF)) =
            *(uint8_t *)(uintptr_t)(vmm_phys_to_virt(sph) + (svp & 0xFFF));
    }

    /* variant-II self-pointer at fs:0 */
    uint64_t tph = vmm_virt_to_phys_user(p->cr3, tp & ~0xFFFULL) & ~0xFFFULL;
    if (!tph) return -1;
    *(uint64_t *)(uintptr_t)(vmm_phys_to_virt(tph) + (tp & 0xFFF)) = tp;

    shared->tls_thread_next = map_end;
    p->user_fs_base = tp;
    if (g_tlsdbg_spawn_budget > 0) {
        g_tlsdbg_spawn_budget--;
        kprint_serial_only("TLSDBG build tid=%u OK fs=%lx bbase=%lx alloc=%lu next=%lx\n",
                           p->pid, (unsigned long)tp, (unsigned long)bbase,
                           (unsigned long)t->tls_alloc, (unsigned long)map_end);
    }
    return 0;
}

struct fry_process *process_create_user_thread(struct fry_process *parent, uint64_t entry,
                                               uint64_t arg, uint64_t user_rsp) {
    struct fry_process_shared *shared = proc_shared(parent);
    struct fry_process *p;
    if (!parent || parent->is_kernel || !shared) return 0;

    p = proc_alloc();
    if (!p) return 0;
    p->cr3 = parent->cr3;
    p->tgid = parent->tgid;
    p->shared = shared;
    p->user_rip = entry;
    p->user_rsp = user_rsp;
    p->user_arg = arg;
    /* fry1790: own TLS block + %fs base (sets p->user_fs_base; 0 if no PT_TLS).
     * Fail the spawn rather than run with a colliding %fs. */
    if (process_build_thread_tls(p, shared) != 0) {
        p->shared = 0;
        p->state = PROC_DEAD;
        return 0;
    }
    if (g_tlsdbg_spawn_budget > 0) {
        g_tlsdbg_spawn_budget--;
        kprint_serial_only("TLSDBG spawn path=NATIVE tid=%u parent=%u fs=%lx entry=%lx\n",
                           p->pid, parent->pid,
                           (unsigned long)p->user_fs_base, (unsigned long)entry);
    }
    p->cpu = parent->cpu;
    p->no_new_privs = parent->no_new_privs;
    p->dumpable = parent->dumpable;
    p->thp_disabled = parent->thp_disabled;
    p->timer_slack_ns = parent->timer_slack_ns;
    p->linux_sig_blocked = parent->linux_sig_blocked;
    copy_name(p->name, parent->name);

    if (alloc_kernel_stack(p) != 0) {
        p->shared = 0;
        p->state = PROC_DEAD;
        return 0;
    }

    if (shared->refcount != 0xFFFFFFFFU) shared->refcount++;
    setup_initial_stack(p);
    return p;
}

struct fry_process *process_clone_linux_thread(struct fry_process *parent,
                                                uint64_t user_rip,
                                                uint64_t user_rsp,
                                                uint64_t tls,
                                                uint64_t child_tid_addr,
                                                uint64_t parent_tid_addr) {
    struct fry_process_shared *shared = proc_shared(parent);
    struct fry_process *p;
    (void)child_tid_addr;   /* TID notifications are written by the clone3 handler */
    (void)parent_tid_addr;
    if (!parent || parent->is_kernel || !shared) return 0;

    p = proc_alloc();
    if (!p) return 0;
    p->cr3           = parent->cr3;
    p->tgid          = parent->tgid;
    p->shared        = shared;
    p->user_rip      = user_rip;
    p->user_rsp      = user_rsp;
    p->user_arg      = 0;
    p->user_fs_base  = tls;
    /* fry1797: NOTE this path takes %fs verbatim from the clone arg and never
     * calls process_build_thread_tls — fry1790's per-thread TLS does not apply
     * here at all. If CPython's threads arrive this way, that is the answer. */
    if (g_tlsdbg_spawn_budget > 0) {
        g_tlsdbg_spawn_budget--;
        kprint_serial_only("TLSDBG spawn path=LINUX_CLONE tid=%u parent=%u fs=%lx(from clone arg) rip=%lx\n",
                           p->pid, parent->pid,
                           (unsigned long)tls, (unsigned long)user_rip);
    }
    p->cpu           = parent->cpu;
    p->no_new_privs  = parent->no_new_privs;
    p->dumpable      = parent->dumpable;
    p->thp_disabled  = parent->thp_disabled;
    p->timer_slack_ns= parent->timer_slack_ns;
    p->is_linux      = parent->is_linux;
    p->lx_img_nsegs  = parent->lx_img_nsegs;
    for (uint8_t si = 0; si < parent->lx_img_nsegs && si < LX_IMG_SEGS_MAX; si++)
        p->lx_img_segs[si] = parent->lx_img_segs[si];
    p->is_clone_child = 1;   /* first run restores full GPRs from clone_ctx */
    p->linux_sig_blocked = parent->linux_sig_blocked;
    copy_name(p->name, parent->name);

    if (alloc_kernel_stack(p) != 0) {
        p->shared = 0;
        p->state  = PROC_DEAD;
        return 0;
    }

    if (shared->refcount != 0xFFFFFFFFU) shared->refcount++;
    setup_initial_stack(p);
    return p;
}

static void copy_vfork_fd_table(struct fry_process *child,
                                struct fry_process_shared *dst,
                                const struct fry_process_shared *src) {
    if (!child || !dst || !src) return;
    /* fry1490: trace installed child fds, not just parent candidates. The
     * earlier beacon ran before allocation/copy and made skipped AF_UNIX fds
     * look inherited, hiding dup2(oldfd, stdio) EBADF failures. */
    static uint32_t tb_cv;
    uint8_t trace_child = 0;
    if (tb_cv < 128) {
        tb_cv++;
        trace_child = 1;
    }
    for (uint32_t f = 3; f < FRY_FD_MAX; f++) {
        void *ptr = src->fd_ptrs[f];
        uint8_t kind = src->fd_kind[f];
        if (!ptr || kind == FD_NONE) continue;

        /*
         * This is a deliberately narrow fd-table clone for Linux vfork/
         * posix_spawn bring-up. Pipes and stdio are the handles subprocess
         * launch needs for child setup and output capture. VFS files and
         * heap-owned fd objects need real refcounts before they can be copied
         * safely across process-owned tables.
         */
        if (kind == FD_PIPE_READ) {
            struct fry_pipe *pp = (struct fry_pipe *)ptr;
            if (!pp || !pp->used) continue;
            if (pp->readers != 0xFFFFFFFFU) pp->readers++;
        } else if (kind == FD_PIPE_WRITE) {
            struct fry_pipe *pp = (struct fry_pipe *)ptr;
            if (!pp || !pp->used) continue;
            if (pp->writers != 0xFFFFFFFFU) pp->writers++;
        } else if (kind == FD_STDIO) {
            /* encoded integer, no lifetime work */
        } else if (kind == FD_FILE) {
            /*
             * Give the child its OWN vfs_file by re-opening the recorded
             * path. vfs_file is pool-allocated with no refcount, so sharing
             * the parent's pointer would double-free on close. posix_spawn's
             * file actions dup2(/dev/null, 0/1/2) to redirect child stdio —
             * the source FD_FILE must exist in the child table or dup2
             * returns EBADF and the spawn fails (child _exit 127).
             */
            if (!src->fd_paths[f][0]) continue;
            struct vfs_file *nf = vfs_open(src->fd_paths[f]);
            if (!nf) continue;
            ptr = nf;
        } else if (kind == FD_SOCKET) {
            /*
             * fry1474: Bun uses AF_UNIX socketpairs (pipe-backed) for child
             * stdio; posix_spawn dup2's a socket end onto the child's 0/1/2.
             * Give the child its OWN fry_socket struct over the SAME backing
             * pipes (do NOT share the parent's struct — that mutated state
             * claude reads and caused a SIGSEGV loop in fry1471/1472). Bump
             * the backing pipe refcounts (this end writes tcp_handle, reads
             * listen_handle) so the child closing its copy doesn't free the
             * pipes the parent still uses. Only AF_UNIX (domain==1): never
             * touch claude's live AF_INET/netlink connections.
             */
            struct fry_socket *osk = (struct fry_socket *)ptr;
            if (!osk || !osk->used || osk->domain != 1) continue;
            int nsi = -1;
            for (uint32_t s = 0; s < FRY_SOCK_MAX; s++) {
                if (!g_sockets[s].used) { nsi = (int)s; break; }
            }
            if (nsi < 0) {
                if (trace_child) {
                    kprint_serial_only("TBCOPYFD child=%u fd=%u kind=%u dom=%u skip=sockfull\n",
                                       child->pid, f, (unsigned)kind,
                                       (unsigned)osk->domain);
                }
                continue;
            }
            struct fry_socket *nsk = &g_sockets[nsi];
            *nsk = *osk;                      /* same pipe handles, fields */
            nsk->used = 1;
            nsk->refs = 0;                    /* child's single fd owns it */
            if (nsk->tcp_handle >= 0 && nsk->tcp_handle < (int)FRY_PIPE_MAX)
                g_pipes[nsk->tcp_handle].writers++;
            if (nsk->listen_handle >= 0 && nsk->listen_handle < (int)FRY_PIPE_MAX)
                g_pipes[nsk->listen_handle].readers++;
            ptr = nsk;
        } else if (kind == FD_DIR) {
            ptr = dst->fd_paths[f];
        } else {
            continue;
        }

        dst->fd_ptrs[f] = ptr;
        dst->fd_table[f] = src->fd_table[f];
        dst->fd_kind[f] = kind;
        dst->fd_flags[f] = src->fd_flags[f];
        if (src->fd_paths[f][0]) {
            uint32_t i = 0;
            for (; i < FRY_PATH_MAX - 1 && src->fd_paths[f][i]; i++)
                dst->fd_paths[f][i] = src->fd_paths[f][i];
            dst->fd_paths[f][i] = 0;
            if (kind == FD_DIR) dst->fd_ptrs[f] = dst->fd_paths[f];
        }
        dst->open_fds++;
        if (trace_child) {
            unsigned dom = (kind == FD_SOCKET && dst->fd_ptrs[f])
                           ? ((struct fry_socket *)dst->fd_ptrs[f])->domain : 0;
            kprint_serial_only("TBCOPYFD child=%u fd=%u kind=%u dom=%u ok=1\n",
                               child->pid, f, (unsigned)kind, dom);
        }
    }
    (void)child;
}

struct fry_process *process_clone_linux_vfork_child(struct fry_process *parent,
                                                    uint64_t user_rip,
                                                    uint64_t user_rsp) {
    struct fry_process_shared *pshared = proc_shared(parent);
    struct fry_process *p;
    if (!parent || parent->is_kernel || !pshared) return 0;

    p = proc_alloc();
    if (!p) return 0;
    p->cr3           = parent->cr3;
    p->tgid          = p->pid;       /* wait4(pid) requires a group leader */
    p->user_rip      = user_rip;
    p->user_rsp      = user_rsp;
    p->user_arg      = 0;
    p->user_fs_base  = parent->user_fs_base;
    p->cpu           = parent->cpu;
    p->no_new_privs  = parent->no_new_privs;
    p->dumpable      = parent->dumpable;
    p->thp_disabled  = parent->thp_disabled;
    p->timer_slack_ns= parent->timer_slack_ns;
    p->is_linux      = parent->is_linux;
    p->lx_img_nsegs  = parent->lx_img_nsegs;
    for (uint8_t si = 0; si < parent->lx_img_nsegs && si < LX_IMG_SEGS_MAX; si++)
        p->lx_img_segs[si] = parent->lx_img_segs[si];
    p->is_clone_child = 1;
    p->linux_vfork_child = 1;
    p->linux_sig_blocked = parent->linux_sig_blocked;
    copy_name(p->name, parent->name);

    shared_init(p);
    if (pshared->cwd[0]) {
        uint32_t i = 0;
        for (; i < FRY_PATH_MAX - 1 && pshared->cwd[i]; i++)
            p->shared->cwd[i] = pshared->cwd[i];
        p->shared->cwd[i] = 0;
    }
    copy_vfork_fd_table(p, p->shared, pshared);

    if (alloc_kernel_stack(p) != 0) {
        shared_release(p);
        p->shared = 0;
        p->state  = PROC_DEAD;
        return 0;
    }

    setup_initial_stack(p);
    return p;
}

struct fry_process *proc_current(void) {
    return sched_current();
}

void proc_set_current(struct fry_process *p) {
    sched_set_current(p);
}

uint32_t process_group_id(const struct fry_process *p) {
    return p ? p->tgid : 0;
}

static void wake_joiners(uint32_t tid) {
    for (uint32_t w = 0; w < PROC_MAX; w++) {
        if (procs[w].state == PROC_WAITING && procs[w].wait_tid == tid) {
            procs[w].wait_tid = 0;
            sched_wake(procs[w].pid);
        }
    }
}

static void shared_release(struct fry_process *p) {
    struct fry_process_shared *shared = proc_shared(p);
    if (!shared) return;

    if (shared->refcount > 0) shared->refcount--;
    if (shared->refcount != 0) return;

    syscall_shm_process_exit(shared->owner_pid);
    if (!p->linux_vfork_child)
        syscall_vm_process_exit(p);

    if (!p->is_kernel && p->cr3 &&
        p->cr3 != vmm_get_kernel_pml4_phys() &&
        !p->linux_vfork_child) {
        vmm_destroy_address_space(p->cr3);
    }

    for (uint32_t f = 0; f < FRY_FD_MAX; f++) {
        if (shared->fd_ptrs[f]) {
            if (shared->fd_kind[f] == FD_PIPE_READ || shared->fd_kind[f] == FD_PIPE_WRITE) {
                struct fry_pipe *pp = (struct fry_pipe *)shared->fd_ptrs[f];
                if (pp) {
                    if (shared->fd_kind[f] == FD_PIPE_READ) {
                        if (pp->readers > 0) pp->readers--;
                    } else {
                        if (pp->writers > 0) pp->writers--;
                    }
                    /* Free pipe object if both ends are gone */
                    if (pp->readers == 0 && pp->writers == 0) {
                        pp->used = 0;
                        pp->head = 0;
                        pp->tail = 0;
                    }
                    /* Wake any poll waiters — peer may have been blocked */
                    sched_wake_poll_waiters();
                }
            } else if (shared->fd_kind[f] == FD_SOCKET) {
                struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[f];
                if (sk && sk->used) {
                    if (sk->refs > 0) {
                        sk->refs--;
                    } else {
                        if (sk->domain == 1 && sk->tcp_handle >= 0 &&
                            sk->tcp_handle < (int)FRY_PIPE_MAX) {
                            struct fry_pipe *pp = &g_pipes[sk->tcp_handle];
                            if (pp->writers > 0) pp->writers--;
                            if (pp->readers == 0 && pp->writers == 0) {
                                pp->used = 0;
                                pp->head = 0;
                                pp->tail = 0;
                            }
                            if (sk->listen_handle >= 0 &&
                                sk->listen_handle < (int)FRY_PIPE_MAX) {
                                pp = &g_pipes[sk->listen_handle];
                                if (pp->readers > 0) pp->readers--;
                                if (pp->readers == 0 && pp->writers == 0) {
                                    pp->used = 0;
                                    pp->head = 0;
                                    pp->tail = 0;
                                }
                            }
                        } else if (sk->type == 1 /* SOCK_STREAM */) {
                            /* Use tcp_close (now refcounted release) instead of direct close.
                             * WHY for fry1543: the conn now has its own refs; tcp_close safely
                             * decrements and only performs real TCP close when last owner.
                             * This prevents UAF / double-close / bad-handle when proc_free walks
                             * fds of a process with many real TCP sockets (Claude + helpers)
                             * during abrupt exit. See netcore.c tcp_conn_release. */
                            if (sk->tcp_handle >= 0)
                                tcp_close(sk->tcp_handle);
                            if (sk->listen_handle >= 0)
                                tcp_close(sk->listen_handle);
                        }
                        uint8_t sp_was_unix = (sk->domain == 1);
                        sk->used = 0;
                        sk->state = 0;
                        sk->tcp_handle = -1;
                        sk->listen_handle = -1;
                        /* fry1482: census on socketpair slot reclaim at process
                         * teardown — pairs with the SPCEN new/exhaust/close
                         * beacons in syscall.c to settle the rg/Grep spawn
                         * ENFILE as leak (slots pinned) vs need (>8 concurrent
                         * pairs). Serial-only; strip with the SPCEN beacons. */
                        if (sp_was_unix) {
                            static uint32_t sp_exit;
                            if (sp_exit < 500) {
                                sp_exit++;
                                int us = 0, up = 0;
                                for (int i = 0; i < FRY_SOCK_MAX; i++) if (g_sockets[i].used) us++;
                                for (int i = 0; i < FRY_PIPE_MAX; i++) if (g_pipes[i].used) up++;
                                kprint_serial_only("SPCEN exit pid=%u sock=%d/%d pipe=%d/%d\n",
                                                   p->pid, us, FRY_SOCK_MAX, up, FRY_PIPE_MAX);
                            }
                        }
                    }
                }
                sched_wake_poll_waiters();
            } else if (shared->fd_kind[f] == FD_FILE) {
                struct vfs_file *vf = (struct vfs_file *)shared->fd_ptrs[f];
                vfs_close(vf);
            }
            shared->fd_ptrs[f] = 0;
            shared->fd_table[f] = -1;
            shared->fd_kind[f] = FD_NONE;
            shared->fd_flags[f] = 0;
            shared->fd_paths[f][0] = 0;
        }
    }
    shared->open_fds = 0;
}

void proc_free(uint32_t pid) {
    struct fry_process *cur = proc_current();
    uint64_t active_rsp = read_rsp();
    uint64_t active_cr3 = read_cr3();

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid &&
            procs[i].state != PROC_UNUSED &&
            procs[i].state != PROC_DEAD) {
            int keep_exit_output = proc_is_user_group_leader(&procs[i]);
            if (cur && cur->pid == pid) {
                uint64_t stack_bytes = (uint64_t)procs[i].kernel_stack_pages * PAGE_SIZE;
                uint64_t stack_lo = (stack_bytes > 0 && procs[i].kernel_stack_top >= stack_bytes)
                                  ? (procs[i].kernel_stack_top - stack_bytes)
                                  : 0;
                if (stack_bytes > 0 &&
                    active_rsp >= stack_lo &&
                    active_rsp < procs[i].kernel_stack_top) {
                    kprint("proc_free: blocked unsafe self-free on active stack pid=%u\n", pid);
                    return;
                }
                if (!procs[i].is_kernel &&
                    procs[i].cr3 &&
                    procs[i].cr3 != vmm_get_kernel_pml4_phys() &&
                    active_cr3 == procs[i].cr3) {
                    kprint("proc_free: blocked unsafe self-free on active CR3 pid=%u\n", pid);
                    return;
                }
            }

            sched_remove(pid);
            if (cur && cur->pid == pid) {
                defer_kernel_stack_free(&procs[i]);
            } else {
                free_kernel_stack(&procs[i]);
            }
            shared_release(&procs[i]);
            /*
             * Keep the exited userspace leader's stdout ring available until the
             * slot is reused so GUI pollers can still drain short-lived app
             * output after exit.  Shells stay alive, but fast tests can finish
             * before the compositor's next poll tick.
             */
            if (!keep_exit_output) procs[i].shared = 0;
            procs[i].cr3 = 0;
            procs[i].state = PROC_DEAD;
            procs[i].wait_pid = 0;
            procs[i].wait_tid = 0;
            procs[i].wait_futex_key = 0;
            procs[i].wait_futex_bitset = 0;
            procs[i].wait_result = 0;

            /* fry1506: mark DEAD + wake pid-waiters atomically under g_sched_lock
             * (was an unlocked scan here, racing process_wait4's unlocked check
             * -> lost wakeup -> wait4 hung forever). sched_pidwait_arm is the
             * matching locked check-and-arm on the waiter side. */
            sched_pidwait_exit(pid);
            wake_joiners(pid);
            return;
        }
    }
}

void process_exit_group(uint32_t tgid, uint32_t code) {
    if (tgid == 0) return;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_UNUSED || procs[i].state == PROC_DEAD) continue;
        if (procs[i].tgid == tgid) procs[i].exit_code = code;
    }
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_UNUSED || procs[i].state == PROC_DEAD) continue;
        if (procs[i].tgid == tgid && procs[i].pid != tgid) {
            proc_free(procs[i].pid);
        }
    }
    proc_free(tgid);
}

int process_thread_exit(uint32_t tid, uint32_t code) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid != tid) continue;
        if (procs[i].state == PROC_UNUSED || procs[i].state == PROC_DEAD) return -ESRCH;
        if (procs[i].is_kernel || procs[i].pid == procs[i].tgid) return -EINVAL;
        procs[i].exit_code = code;
        procs[i].wait_pid = 0;
        procs[i].wait_tid = 0;
        /* Remove from the runqueue as a zombie so joiners never observe a
         * transient PROC_DEAD and misreport ESRCH while the thread exits. */
        sched_remove_with_state(tid, PROC_ZOMBIE);
        procs[i].state = PROC_ZOMBIE;
        wake_joiners(tid);
        return 0;
    }
    return -ESRCH;
}

int process_launch(const char *path) {
    struct fry_process *p = 0;
    struct fry_process *cur = proc_current();
    uint64_t cr3 = 0, entry = 0, user_rsp = 0, tls_base = 0;
    struct fry_tls_template tmpl;
    g_process_last_launch_error = 0;
    int elf_rc = elf_load_fry(path, &cr3, &entry, &user_rsp, &tls_base, &tmpl);
    if (elf_rc != 0) {
        g_process_last_launch_error = elf_rc;
        kprint("PROC: launch fail path=%s elf_rc=%d\n", path ? path : "(null)", elf_rc);
        return elf_rc;
    }
    p = process_create_user(cr3, entry, user_rsp, path);
    if (!p) {
        vmm_destroy_address_space(cr3);
        g_process_last_launch_error = PROCESS_LAUNCH_ERR_CREATE_USER;
        return PROCESS_LAUNCH_ERR_CREATE_USER;
    }
    /* PT_TLS images: program the main-thread %fs base so __thread vars resolve
     * before _start runs (process_start -> write_user_fs_base). 0 = no PT_TLS. */
    p->user_fs_base = tls_base;
    process_tls_stash_template(p, &tmpl);   /* fry1790: for spawned threads */
    if (cur) p->cpu = cur->cpu;
    g_process_last_launch_error = 0;
    sched_add(p->pid);
    return (int)p->pid;
}

static uint32_t kstrlen(const char *s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void setup_process_args(struct fry_process *p, const char **argv, uint32_t argc,
                                const char **envp, uint32_t envc) {
    struct fry_process_shared *shared = proc_shared(p);
    if (!shared) return;

    shared->argc = 0;
    shared->envc = 0;
    uint32_t pos = 0;

    /* Copy argv strings into args_buf */
    for (uint32_t i = 0; i < argc && i < FRY_ARGV_MAX; i++) {
        uint32_t len = kstrlen(argv[i]);
        if (pos + len + 1 > FRY_ARGS_BUFSZ) break;
        shared->argv_offsets[i] = pos;
        for (uint32_t j = 0; j <= len; j++) {
            shared->args_buf[pos++] = argv[i][j];
        }
        shared->argc++;
    }

    /* Copy envp strings into args_buf (after argv) */
    for (uint32_t i = 0; i < envc && i < FRY_ENV_MAX; i++) {
        uint32_t len = kstrlen(envp[i]);
        if (pos + len + 1 > FRY_ARGS_BUFSZ) break;
        shared->env_offsets[i] = pos;
        for (uint32_t j = 0; j <= len; j++) {
            shared->args_buf[pos++] = envp[i][j];
        }
        shared->envc++;
    }
}

/* fry1821: interpreter ("#!") support.
 * Reads the first line of a program file. If it begins with "#!", the rest of
 * the line is an interpreter path plus at most ONE optional argument, which is
 * how POSIX and Linux both define it — everything after the first space is a
 * single argument, not a tokenised list.
 * Returns 1 when a shebang was parsed, 0 when the file is not a script, and a
 * negative value on a read failure. A file that is too short, or whose shebang
 * line is empty, is reported as "not a script" so the normal ELF path produces
 * the error rather than this helper inventing one.
 * Without this, only compiled .fry binaries could ever be launched, which is
 * why the shell carried hardcoded cmd_python3/cmd_hermes/cmd_claude builtins
 * instead of just running a file. */
static int shebang_parse(const char *path,
                         char *interp, uint32_t interp_sz,
                         char *iarg, uint32_t iarg_sz) {
    uint8_t line[256];
    struct vfs_file *f;
    uint32_t n, i, o;

    if (!path || !interp || interp_sz == 0 || !iarg || iarg_sz == 0) return -1;
    interp[0] = '\0';
    iarg[0] = '\0';

    f = vfs_open(path);
    if (!f) return -1;
    n = (uint32_t)vfs_read(f, line, sizeof(line));
    vfs_close(f);
    if ((int32_t)n < 2) return 0;
    if (line[0] != '#' || line[1] != '!') return 0;

    i = 2;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;

    /* interpreter path: up to whitespace or end of line */
    o = 0;
    while (i < n && line[i] != '\n' && line[i] != '\r' &&
           line[i] != ' ' && line[i] != '\t') {
        if (o + 1 >= interp_sz) return 0;   /* too long to be usable */
        interp[o++] = (char)line[i++];
    }
    interp[o] = '\0';
    if (o == 0) return 0;                   /* "#!" with nothing after it */

    /* optional single argument: the remainder of the line, trimmed */
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    o = 0;
    while (i < n && line[i] != '\n' && line[i] != '\r') {
        if (o + 1 >= iarg_sz) break;
        iarg[o++] = (char)line[i++];
    }
    while (o > 0 && (iarg[o - 1] == ' ' || iarg[o - 1] == '\t')) o--;
    iarg[o] = '\0';

    return 1;
}

int process_launch_args(const char *path, const char **argv, uint32_t argc,
                        const char **envp, uint32_t envc) {
    struct fry_process *p = 0;
    struct fry_process *cur = proc_current();
    uint64_t cr3 = 0, entry = 0, user_rsp = 0, tls_base = 0;
    struct fry_tls_template tmpl;
    char sb_interp[FRY_PATH_MAX];
    char sb_arg[FRY_PATH_MAX];
    const char *sb_argv[FRY_ARGV_MAX];
    g_process_last_launch_error = 0;

    /* Resolve a "#!" script to its interpreter before loading. Exactly ONE
     * level is followed, matching Linux: a script whose interpreter is itself a
     * script is an error, not a recursion. The rewritten argv is
     *     interp [iarg] <script path> <original argv[1..]>
     * and these buffers stay live until setup_process_args() copies them below. */
    if (shebang_parse(path, sb_interp, sizeof(sb_interp),
                      sb_arg, sizeof(sb_arg)) == 1) {
        uint32_t n = 0;
        sb_argv[n++] = sb_interp;
        if (sb_arg[0] && n < FRY_ARGV_MAX) sb_argv[n++] = sb_arg;
        if (n < FRY_ARGV_MAX) sb_argv[n++] = path;
        for (uint32_t i = 1; i < argc && n < FRY_ARGV_MAX; i++) {
            sb_argv[n++] = argv[i];
        }
        kprint("PROC: shebang %s -> %s\n", path, sb_interp);
        path = sb_interp;
        argv = sb_argv;
        argc = n;
    }

    int elf_rc = elf_load_fry(path, &cr3, &entry, &user_rsp, &tls_base, &tmpl);
    if (elf_rc != 0) {
        g_process_last_launch_error = elf_rc;
        kprint("PROC: launch fail path=%s elf_rc=%d\n", path ? path : "(null)", elf_rc);
        return elf_rc;
    }
    p = process_create_user(cr3, entry, user_rsp, path);
    if (!p) {
        vmm_destroy_address_space(cr3);
        g_process_last_launch_error = PROCESS_LAUNCH_ERR_CREATE_USER;
        return PROCESS_LAUNCH_ERR_CREATE_USER;
    }
    /* PT_TLS images: program the main-thread %fs base so __thread vars resolve
     * before _start runs (process_start -> write_user_fs_base). 0 = no PT_TLS. */
    p->user_fs_base = tls_base;
    process_tls_stash_template(p, &tmpl);   /* fry1790: for spawned threads */
    if (cur) p->cpu = cur->cpu;

    /* Copy arguments and environment into the new process */
    if (argv && argc > 0) {
        setup_process_args(p, argv, argc, envp, envc);
    }

    g_process_last_launch_error = 0;
    sched_add(p->pid);
    return (int)p->pid;
}

/*
 * Launch an UNMODIFIED Linux x86_64 ELF as a Linux-personality process.
 * Loads via the Tater Bridge ELF loader (bare ELF + SysV stack), tags the
 * process is_linux so its syscalls route through linux_syscall_dispatch,
 * and seeds the program break + anonymous-mmap arena.
 */
int process_launch_linux(const char *path,
                         const char *const *argv, int argc,
                         const char *const *envp, int envc) {
    struct fry_process *cur = proc_current();
    uint64_t cr3 = 0, entry = 0, rsp = 0, brk = 0;
    struct lx_img_seg img_segs[LX_IMG_SEGS_MAX];
    uint8_t img_nsegs = 0;
    g_process_last_launch_error = 0;

    int rc = elf_load_linux(path, argv, argc,
                            envp, envc,
                            &cr3, &entry, &rsp, &brk,
                            img_segs, &img_nsegs,
                            0, 0, 0);
    if (rc != 0) {
        g_process_last_launch_error = rc;
        kprint("PROC: linux launch fail path=%s rc=%d\n", path ? path : "(null)", rc);
        return rc;
    }

    struct fry_process *p = process_create_user(cr3, entry, rsp, path);
    if (!p) {
        vmm_destroy_address_space(cr3);
        g_process_last_launch_error = PROCESS_LAUNCH_ERR_CREATE_USER;
        return PROCESS_LAUNCH_ERR_CREATE_USER;
    }

    p->is_linux = 1;
    p->lx_img_nsegs = img_nsegs;
    for (uint8_t si = 0; si < img_nsegs && si < LX_IMG_SEGS_MAX; si++)
        p->lx_img_segs[si] = img_segs[si];
    {
        struct fry_process_shared *sh = proc_shared(p);
        if (sh) {
            sh->heap_start = brk;
            sh->heap_end = brk;
            sh->linux_mmap_next = LX_MMAP_BASE;
        }
    }
    if (cur) p->cpu = cur->cpu;

    g_process_last_launch_error = 0;
    sched_add(p->pid);
    kprint("PROC: linux launch ok path=%s pid=%u entry=%lx rsp=%lx brk=%lx\n",
           path ? path : "(null)", p->pid,
           (unsigned long)entry, (unsigned long)rsp, (unsigned long)brk);
    return (int)p->pid;
}

int process_wait(uint32_t pid) {
    if (pid == 0) return -EINVAL;
    struct fry_process *cur = proc_current();
    if (!cur || cur->pid == pid) return -EINVAL;
    struct fry_process *target = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid && procs[i].state != PROC_UNUSED) {
            target = &procs[i];
            break;
        }
    }
    if (!target) return -ESRCH;
    if (!target->is_kernel && target->pid != target->tgid) return -EINVAL;
    if (target->state == PROC_DEAD) return 0;
    cur->wait_pid = pid;
    cur->wait_tid = 0;
    cur->wait_futex_key = 0;
    cur->wait_futex_bitset = 0;
    cur->wait_result = 0;
    cur->wake_time_ms = UINT64_MAX;
    sched_block(cur->pid);
    return 1;
}

int process_wait_status(uint32_t pid, int *exit_code_out) {
    if (pid == 0) return -EINVAL;
    struct fry_process *cur = proc_current();
    if (!cur || cur->pid == pid) return -EINVAL;
    struct fry_process *target = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid && procs[i].state != PROC_UNUSED) {
            target = &procs[i];
            break;
        }
    }
    if (!target) return -ESRCH;
    if (!target->is_kernel && target->pid != target->tgid) return -EINVAL;
    if (target->state == PROC_DEAD) {
        if (exit_code_out) *exit_code_out = (int)target->exit_code;
        return 0;
    }
    cur->wait_pid = pid;
    cur->wait_tid = 0;
    cur->wait_futex_key = 0;
    cur->wait_futex_bitset = 0;
    cur->wait_result = 0;
    cur->wake_time_ms = UINT64_MAX;
    sched_block(cur->pid);
    return 1; /* caller should yield and re-check */
}

int process_thread_join(uint32_t tid) {
    struct fry_process *cur = proc_current();
    if (!cur || cur->is_kernel || tid == 0 || cur->pid == tid) return -EINVAL;

    for (;;) {
        struct fry_process *target = 0;
        for (uint32_t i = 0; i < PROC_MAX; i++) {
            if (procs[i].pid == tid && procs[i].state != PROC_UNUSED && procs[i].state != PROC_DEAD) {
                target = &procs[i];
                break;
            }
        }
        if (!target) return -ESRCH;
        if (target->is_kernel || target->tgid != cur->tgid || target->pid == target->tgid) {
            return -EINVAL;
        }
        if (target->state == PROC_ZOMBIE) {
            int code = (int)target->exit_code;
            proc_free(tid);
            return code;
        }
        cur->wait_pid = 0;
        cur->wait_tid = tid;
        cur->wait_futex_key = 0;
        cur->wait_futex_bitset = 0;
        cur->wait_result = 0;
        cur->wake_time_ms = UINT64_MAX;
        sched_block(cur->pid);
        sched_yield();
        cur = proc_current();
        if (!cur) return -ESRCH;
        cur->wait_tid = 0;
    }
}

/*
 * Linux wait4(pid>0) for a child PROCESS leader (Bash/rg/hook subprocesses
 * spawned via vfork+execve). Mirrors process_thread_join's block/recheck
 * loop, but the target is a process leader, not a sibling thread.
 *
 * An exited process is left in PROC_DEAD by proc_free with exit_code
 * preserved and the slot retained until proc_alloc reuses it, so a child
 * that has already exited is still collectable here.
 *
 * Returns:
 *   >0  reaped child pid; *status_out gets the wait(2)-encoded status
 *    0  nohang requested and the child is still alive (no status written)
 *   <0  -errno (-ECHILD: no such pid, or pid is a thread, not a leader)
 */
int process_wait4(uint32_t pid, int *status_out, int nohang) {
    struct fry_process *cur = proc_current();
    if (!cur || cur->is_kernel || pid == 0 || cur->pid == pid) return -ECHILD;

    for (;;) {
        struct fry_process *target = 0;
        for (uint32_t i = 0; i < PROC_MAX; i++) {
            if (procs[i].pid == pid && procs[i].state != PROC_UNUSED) {
                target = &procs[i];
                break;
            }
        }
        if (!target) return -ECHILD;
        /* wait4 collects process leaders only — a TID inside a group is not
         * a waitable child here. */
        if (target->is_kernel || target->pid != target->tgid) return -ECHILD;

        if (target->state == PROC_ZOMBIE) {
            if (status_out) *status_out = ((int)target->exit_code & 0xff) << 8;
            proc_free(pid);            /* not yet freed → release properly */
            return (int)pid;
        }
        if (target->state == PROC_DEAD) {
            if (status_out) *status_out = ((int)target->exit_code & 0xff) << 8;
            /* Final reap: proc_free already released resources and left the
             * slot DEAD; mark it UNUSED so the pid is reusable and the same
             * child is never collected twice. */
            target->state = PROC_UNUSED;
            target->pid = 0;
            return (int)pid;
        }
        if (nohang) return 0;

        /* fry1506: race-free arm. Re-check the child and arm PROC_WAITING
         * ATOMICALLY under g_sched_lock (matches sched_pidwait_exit on the
         * proc_free side). The old code checked above without a lock then
         * sched_block'd on an infinite wait — an exit landing in that window
         * lost the wake and hung this thread forever. If the child became
         * reapable during the locked re-check, loop back and reap it. */
        if (sched_pidwait_arm(cur->pid, pid)) {
            cur = proc_current();
            if (!cur) return -ECHILD;
            continue;
        }
        sched_yield();
        cur = proc_current();
        if (!cur) return -ECHILD;
        cur->wait_pid = 0;
    }
}

/*
 * vfork support. A vfork child runs in the PARENT's address space, on the
 * parent's stack, until it execve's or exits. Real vfork therefore SUSPENDS
 * the parent for that window — letting both run concurrently corrupts the
 * shared stack (observed as glibc "stack smashing detected" aborts in the
 * posix_spawn child). The parent blocks until this returns 0.
 *
 * Returns 1 while the child still shares our address space (pre-exec, alive),
 * 0 once it has execve'd (linux_vfork_child cleared) or exited (slot gone).
 */
int process_vfork_child_active(uint32_t pid) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid &&
            procs[i].state != PROC_UNUSED &&
            procs[i].state != PROC_DEAD) {
            return procs[i].linux_vfork_child ? 1 : 0;
        }
    }
    return 0;
}

/* Wake any process blocked in wait_pid on this pid. The exit path (proc_free)
 * already does this for child death; execve calls it explicitly to release a
 * vfork parent once the child has switched to its own address space. */
void process_wake_pid_waiters(uint32_t pid) {
    for (uint32_t w = 0; w < PROC_MAX; w++) {
        if (procs[w].state == PROC_WAITING && procs[w].wait_pid == pid) {
            procs[w].wait_pid = 0;
            sched_wake(procs[w].pid);
        }
    }
}

uint32_t process_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].state != PROC_UNUSED &&
            procs[i].state != PROC_DEAD &&
            (!proc_shared_const(&procs[i]) || proc_is_user_group_leader(&procs[i]))) {
            count++;
        }
    }
    return count;
}
