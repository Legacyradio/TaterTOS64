// Process management

#include "process.h"
#include "syscall.h"
#include "sched.h"
#include "elf.h"
#include "../fs/vfs.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
void kprint(const char *fmt, ...);

#define KSTACK_SIZE 16384
#define PAGE_SIZE   4096ULL

struct fry_process procs[PROC_MAX];
static uint32_t next_pid;
static int g_process_last_launch_error;

static void mem_zero(void *p, uint64_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
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

static void process_start(struct fry_process *p) {
    if (!p) for (;;) __asm__ volatile("hlt");

    if (p->is_kernel && p->kentry) {
        p->kentry(p->karg);
        for (;;) __asm__ volatile("hlt");
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"(p->cr3));

    __asm__ volatile(
        "cli\n"
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
        : "r"(p->user_rsp), "r"(p->user_rip)
        : "rax", "memory"
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
            procs[i].state = PROC_RUNNING;
            procs[i].wait_pid = 0;
            for (uint32_t f = 0; f < 64; f++) {
                procs[i].fd_table[f] = -1;
                procs[i].fd_ptrs[f] = 0;
            }
            return &procs[i];
        }
    }
    return 0;
}

struct fry_process *process_create_user(uint64_t cr3, uint64_t entry, uint64_t user_rsp, const char *name) {
    struct fry_process *p = proc_alloc();
    if (!p) return 0;
    p->cr3 = cr3;
    p->user_rip = entry;
    p->user_rsp = user_rsp;
    p->heap_start = 0x10000000000ULL;
    p->heap_end = p->heap_start;
    copy_name(p->name, name);

    if (alloc_kernel_stack(p) != 0) {
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

struct fry_process *proc_current(void) {
    return sched_current();
}

void proc_set_current(struct fry_process *p) {
    sched_set_current(p);
}

void proc_free(uint32_t pid) {
    struct fry_process *cur = proc_current();
    uint64_t active_rsp = read_rsp();
    uint64_t active_cr3 = read_cr3();

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid &&
            procs[i].state != PROC_UNUSED &&
            procs[i].state != PROC_DEAD) {
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

            /* Drop SHM ownership/mappings before tearing down the address space. */
            syscall_shm_process_exit(pid);

            if (!procs[i].is_kernel && procs[i].cr3 &&
                procs[i].cr3 != vmm_get_kernel_pml4_phys()) {
                vmm_destroy_address_space(procs[i].cr3);
            }
            sched_remove(pid);
            free_kernel_stack(&procs[i]);
            procs[i].cr3 = 0;
            procs[i].state = PROC_DEAD;
            procs[i].wait_pid = 0;

            /* Close all open file descriptors */
            for (uint32_t f = 0; f < 64; f++) {
                if (procs[i].fd_ptrs[f]) {
                    struct vfs_file *vf = (struct vfs_file *)procs[i].fd_ptrs[f];
                    vfs_close(vf);
                    procs[i].fd_ptrs[f] = 0;
                    procs[i].fd_table[f] = -1;
                }
            }

            for (uint32_t w = 0; w < PROC_MAX; w++) {
                if (procs[w].state == PROC_WAITING && procs[w].wait_pid == pid) {
                    procs[w].wait_pid = 0;
                    sched_wake(procs[w].pid);
                }
            }
            return;
        }
    }
}

int process_launch(const char *path) {
    struct fry_process *p = 0;
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
    g_process_last_launch_error = 0;
    sched_add(p->pid);
    return (int)p->pid;
}

int process_wait(uint32_t pid) {
    if (pid == 0) return -1;
    struct fry_process *cur = proc_current();
    if (!cur || cur->pid == pid) return -1;
    struct fry_process *target = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid && procs[i].state != PROC_UNUSED) {
            target = &procs[i];
            break;
        }
    }
    if (!target || target->state == PROC_DEAD) return 0;
    cur->wait_pid = pid;
    cur->wake_time_ms = UINT64_MAX;
    sched_block(cur->pid);
    return 1;
}

uint32_t process_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].state != PROC_UNUSED && procs[i].state != PROC_DEAD) {
            count++;
        }
    }
    return count;
}
