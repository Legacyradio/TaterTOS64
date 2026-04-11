// Process management

#include "process.h"
#include "syscall.h"
#include "sched.h"
#include "elf.h"
#include <errno.h>
#include "../fs/vfs.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../../drivers/net/netcore.h"
void kprint(const char *fmt, ...);

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
    for (uint32_t f = 0; f < FRY_FD_MAX; f++) {
        shared->fd_table[f] = -1;
        shared->fd_ptrs[f] = 0;
        shared->fd_kind[f] = FD_NONE;
        shared->fd_flags[f] = 0;
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
        "iretq\n"
        :
        : "r"(p->user_rsp), "r"(p->user_rip), "r"(p->user_arg)
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
    p->user_fs_base = 0;
    p->cpu = parent->cpu;
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
    syscall_vm_process_exit(p);

    if (!p->is_kernel && p->cr3 &&
        p->cr3 != vmm_get_kernel_pml4_phys()) {
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
                    if (sk->type == 1 /* SOCK_STREAM */) {
                        if (sk->tcp_handle >= 0)
                            tcp_close(sk->tcp_handle);
                        if (sk->listen_handle >= 0)
                            tcp_close(sk->listen_handle);
                    }
                    sk->used = 0;
                    sk->state = 0;
                    sk->tcp_handle = -1;
                    sk->listen_handle = -1;
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
            procs[i].wait_result = 0;

            for (uint32_t w = 0; w < PROC_MAX; w++) {
                if (procs[w].state == PROC_WAITING && procs[w].wait_pid == pid) {
                    procs[w].wait_pid = 0;
                    sched_wake(procs[w].pid);
                }
            }
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
    uint64_t cr3 = 0, entry = 0, user_rsp = 0;
    g_process_last_launch_error = 0;
    int elf_rc = elf_load_fry(path, &cr3, &entry, &user_rsp);
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

int process_launch_args(const char *path, const char **argv, uint32_t argc,
                        const char **envp, uint32_t envc) {
    struct fry_process *p = 0;
    struct fry_process *cur = proc_current();
    uint64_t cr3 = 0, entry = 0, user_rsp = 0;
    g_process_last_launch_error = 0;
    int elf_rc = elf_load_fry(path, &cr3, &entry, &user_rsp);
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
    if (cur) p->cpu = cur->cpu;

    /* Copy arguments and environment into the new process */
    if (argv && argc > 0) {
        setup_process_args(p, argv, argc, envp, envc);
    }

    g_process_last_launch_error = 0;
    sched_add(p->pid);
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
        cur->wait_result = 0;
        cur->wake_time_ms = UINT64_MAX;
        sched_block(cur->pid);
        sched_yield();
        cur = proc_current();
        if (!cur) return -ESRCH;
        cur->wait_tid = 0;
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
