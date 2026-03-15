#ifndef TATER_PROCESS_H
#define TATER_PROCESS_H

#include <stdint.h>

enum proc_state {
    PROC_UNUSED = 0,
    PROC_RUNNING,
    PROC_WAITING,
    PROC_ZOMBIE,
    PROC_DEAD
};

#define PROC_MAX 256
#define PROC_OUTBUF 512   /* per-process stdout ring buffer size */
#define PROC_INBUF 512    /* per-process stdin ring buffer size */
#define PROC_VMREG_MAX 256

enum process_launch_error {
    PROCESS_LAUNCH_OK = 0,
    PROCESS_LAUNCH_ERR_CREATE_USER = -200
};

enum fry_vm_region_kind {
    FRY_VM_REGION_NONE = 0,
    FRY_VM_REGION_ANON_PRIVATE = 1,
    FRY_VM_REGION_ANON_SHARED = 2,
    FRY_VM_REGION_FILE_PRIVATE = 3,
    FRY_VM_REGION_GUARD = 4
};

struct fry_vm_region {
    uint64_t base;
    uint64_t length;
    uint32_t prot;
    uint32_t flags;
    uint32_t backing_id;
    uint32_t backing_page_start;
    uint16_t kind;
    uint8_t used;
    uint8_t committed;
    uint8_t _pad[4];
};

/*
 * Handle lifetime rules (Phase 0 ABI discipline):
 *
 * File descriptors (fd_table/fd_ptrs):
 *   - FDs 0/1/2 are stdin/stdout/stderr, managed by the kernel.
 *   - FDs 3-63 are available for user open/close.
 *   - Each open() allocates the lowest free FD slot.
 *   - close() frees the slot; the FD number may be reused by a later open().
 *   - On process exit, all open FDs are closed and VFS files released.
 *   - FDs are NOT inherited by child processes (spawn creates a fresh table).
 *
 * SHM handles:
 *   - Global pool of SHM_MAX (128) regions.
 *   - owner_pid tracks who allocated; only owner can free.
 *   - Any process may map a valid SHM ID.
 *   - On owner exit, owned SHM regions are destroyed and all mappings torn down.
 *   - mapped_pids[] guards against stale slot reuse across pid recycling.
 *
 * VM regions (vm_regions[]):
 *   - Per-process, up to PROC_VMREG_MAX (256) tracked regions.
 *   - Cleaned up on process exit via syscall_vm_process_exit().
 *   - Shared anonymous VM objects are refcounted; last unmap frees backing.
 */
struct fry_process {
    uint32_t pid;
    uint64_t cr3;
    uint64_t saved_rsp;
    uint64_t saved_rip;
    uint64_t kernel_stack_top;
    uint8_t state;
    uint8_t cpu;
    struct fry_process *next;
    char name[32];
    int fd_table[64];
    void *fd_ptrs[64];
    uint32_t exit_code;
    /* Resource accounting */
    uint16_t open_fds;       /* current count of open file descriptors */
    uint16_t _res_pad;
    uint64_t user_rsp;
    uint64_t user_rip;
    uint64_t wake_time_ms;
    uint32_t wait_pid;
    uint8_t is_kernel;
    uint64_t heap_start;
    uint64_t heap_end;
    uint64_t kernel_stack_phys;
    uint32_t kernel_stack_pages;
    void (*kentry)(void *arg);
    void *karg;
    /* stdout capture ring buffer — written by SYS_WRITE(fd=1),
       read by SYS_PROC_OUTPUT.  head==tail means empty. */
    uint8_t  outbuf[PROC_OUTBUF];
    uint32_t outbuf_head;   /* consumer read index */
    uint32_t outbuf_tail;   /* producer write index */
    /* stdin input ring buffer — written by SYS_PROC_INPUT,
       read by SYS_READ(fd=0).  head==tail means empty. */
    uint8_t  inbuf[PROC_INBUF];
    uint32_t inbuf_head;    /* consumer read index */
    uint32_t inbuf_tail;    /* producer write index */
    struct fry_vm_region vm_regions[PROC_VMREG_MAX];
};

extern struct fry_process procs[PROC_MAX];

int process_init(void);
struct fry_process *process_create_user(uint64_t cr3, uint64_t entry, uint64_t user_rsp, const char *name);
struct fry_process *process_create_kernel(void (*entry)(void *), void *arg, const char *name);
struct fry_process *proc_current(void);
void proc_set_current(struct fry_process *p);
void proc_free(uint32_t pid);
void process_reap_deferred_stacks(void);
int process_launch(const char *path);
int process_last_launch_error(void);
int process_wait(uint32_t pid);
uint32_t process_count(void);

#endif
