#ifndef TATER_PROCESS_H
#define TATER_PROCESS_H

#include <stdint.h>
#include <fry_limits.h>

enum proc_state {
    PROC_UNUSED = 0,
    PROC_RUNNING,
    PROC_WAITING,
    PROC_ZOMBIE,
    PROC_DEAD
};

#define PROC_MAX FRY_PROC_MAX
#define PROC_OUTBUF 512   /* per-process stdout ring buffer size */
#define PROC_INBUF 512    /* per-process stdin ring buffer size */
#define PROC_VMREG_MAX FRY_VMREG_MAX

#define TB_CLAUDE_WATCH_TGID 3u
#define TB_JSC_CAGE_SLOT_ARRAY_OFF 0x4480000ULL
#define TB_JSC_SLOT_WATCH0_OFF (0x3f8ULL * 8ULL)
#define TB_JSC_SLOT_WATCH1_OFF (0x3f9ULL * 8ULL)
#define TB_JSC_SLOT_WATCH2_OFF (0x7f8ULL * 8ULL)
#define TB_JSC_SLOT_WATCH3_OFF (0x7f9ULL * 8ULL)
#define TB_JSC_FUTEX_UADDR 0x63ad6e0ULL
#define TB_JSC_FUTEX_KEY 0x80030000063ad6e0ULL

enum process_launch_error {
    PROCESS_LAUNCH_OK = 0,
    PROCESS_LAUNCH_ERR_CREATE_USER = -200
};

/*
 * File descriptor kinds — each open fd carries a kind tag so the kernel
 * dispatches read/write/close to the correct subsystem (VFS, pipe, etc.).
 */
enum fry_fd_kind {
    FD_NONE       = 0,   /* slot is free */
    FD_FILE       = 1,   /* VFS file — fd_ptrs[fd] is struct vfs_file* */
    FD_PIPE_READ  = 2,   /* read end of a pipe — fd_ptrs[fd] is struct fry_pipe* */
    FD_PIPE_WRITE = 3,   /* write end of a pipe — fd_ptrs[fd] is struct fry_pipe* */
    FD_SOCKET     = 4,   /* socket — fd_ptrs[fd] is struct fry_socket* */
    FD_EPOLL      = 5,   /* epoll — fd_ptrs[fd] is struct epoll_cb* */
    FD_EVENTFD    = 6,   /* eventfd — fd_ptrs[fd] is struct eventfd_cb* */
    FD_DIR        = 7,    /* directory handle — fd_ptrs[fd] points at fd_paths[fd] */
    FD_TIMERFD    = 8,    /* timerfd — fd_ptrs[fd] is struct timerfd_cb* */
    FD_SIGNALFD   = 9,    /* signalfd — fd_ptrs[fd] is struct signalfd_cb* */
    FD_INOTIFY    = 10,    /* inotify — fd_ptrs[fd] is struct inotify_cb* */
    FD_MEMFD      = 11,    /* memfd — fd_ptrs[fd] is struct memfd_cb* */
    FD_STDIO      = 12     /* dup of stdin/stdout/stderr — fd_ptrs[fd] encodes fd+1 */
};

/*
 * Kernel pipe object — global pool of FRY_PIPE_MAX entries.
 * Ring buffer with reader/writer reference counts.
 * Blocking semantics: reader blocks when empty, writer blocks when full.
 * EOF: when last writer closes, readers get 0 (EOF).
 * EPIPE: when last reader closes, writers get -EPIPE.
 */
struct fry_pipe {
    uint8_t  buf[FRY_PIPE_BUFSZ];
    uint32_t head;        /* next read position */
    uint32_t tail;        /* next write position */
    uint32_t readers;     /* number of open read-end fds */
    uint32_t writers;     /* number of open write-end fds */
    uint8_t  used;
    uint8_t  _pad[3];
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

struct fry_linux_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct fry_process_shared {
    uint32_t owner_pid;
    uint32_t refcount;
    int fd_table[FRY_FD_MAX];
    void *fd_ptrs[FRY_FD_MAX];
    uint8_t  fd_kind[FRY_FD_MAX];   /* enum fry_fd_kind per fd */
    uint32_t fd_flags[FRY_FD_MAX];  /* per-fd flags (O_NONBLOCK etc.) */
    char     fd_paths[FRY_FD_MAX][FRY_PATH_MAX]; /* canonical path for *at() dirfd resolution */
    char     cwd[FRY_PATH_MAX];                   /* current working directory (chdir/getcwd) */
    uint16_t open_fds;
    uint16_t _res_pad;
    uint64_t heap_start;
    uint64_t heap_end;
    uint64_t linux_mmap_next;       /* shared Linux mmap() top-down cursor */
    /* stdout capture ring buffer — written by SYS_WRITE(fd=1),
       read by SYS_PROC_OUTPUT.  head==tail means empty. */
    uint8_t  outbuf[PROC_OUTBUF];
    uint32_t outbuf_head;
    uint32_t outbuf_tail;
    /* stdin input ring buffer — written by SYS_PROC_INPUT,
       read by SYS_READ(fd=0).  head==tail means empty. */
    uint8_t  inbuf[PROC_INBUF];
    uint32_t inbuf_head;
    uint32_t inbuf_tail;
    /* Process arguments and environment (Phase 3) */
    uint32_t argc;
    uint32_t envc;
    uint32_t argv_offsets[FRY_ARGV_MAX];  /* offset into args_buf for each arg */
    uint32_t env_offsets[FRY_ENV_MAX];    /* offset into args_buf for each env var */
    char     args_buf[FRY_ARGS_BUFSZ];   /* packed null-terminated strings */
    struct fry_vm_region vm_regions[PROC_VMREG_MAX];
    struct fry_linux_sigaction linux_sigact[64]; /* dispositions shared by tgid */
};

/*
 * Handle lifetime rules (Phase 0 ABI discipline):
 *
 * File descriptors (shared->fd_table/shared->fd_ptrs):
 *   - FDs 0/1/2 are stdin/stdout/stderr, managed by the kernel.
 *   - FDs 3-63 are available for user open/close.
 *   - Each open() allocates the lowest free FD slot.
 *   - close() frees the slot; the FD number may be reused by a later open().
 *   - On process exit, all open FDs are closed and VFS files released.
 *   - FDs are NOT inherited by child processes (spawn creates a fresh table).
 *
 * SHM handles:
 *   - Global pool of FRY_SHM_MAX regions.
 *   - owner_pid tracks who allocated; only owner can free.
 *   - Any process may map a valid SHM ID.
 *   - On owner exit, owned SHM regions are destroyed and all mappings torn down.
 *   - mapped_pids[] guards against stale slot reuse across pid recycling.
 *
 * VM regions (shared->vm_regions[]):
 *   - Per-process, up to PROC_VMREG_MAX tracked regions.
 *   - Cleaned up on process exit via syscall_vm_process_exit().
 *   - Shared anonymous VM objects are refcounted; last unmap frees backing.
 */

/*
 * Full user register snapshot for a clone()/clone3() child. On Linux a clone
 * child inherits a copy of the parent's registers at the syscall point (with
 * rax=0, rsp=new stack). glibc relies on this — it stashes the thread fn/arg
 * in callee-saved registers across the clone syscall. process_start restores
 * this whole set via iretq for clone children (is_clone_child).
 */
struct linux_clone_ctx {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rsp, rflags;
};

struct fry_process {
    uint32_t pid;
    uint32_t tgid;
    uint32_t wait_tid;
    uint32_t exit_code;
    uint64_t cr3;
    uint64_t saved_rsp;
    uint64_t saved_rip;
    uint64_t kernel_stack_top;
    uint8_t state;
    uint8_t cpu;
    struct fry_process *next;
    char name[32];
    struct fry_process_shared *shared;
    struct fry_process_shared owned_shared;
    uint64_t user_rsp;
    uint64_t user_rip;
    uint64_t user_arg;
    uint64_t user_rax;     /* value of RAX at user entry. 0 for _start and for
                              clone() children (clone returns 0 in the child).
                              Zero-initialized by proc_alloc for all processes. */
    uint64_t user_fs_base;
    uint64_t wake_time_ms;
    uint64_t wait_futex_key;
    uint32_t wait_futex_bitset;
    uint32_t wait_pid;
    int32_t wait_result;
    uint8_t is_kernel;
    uint8_t wait_poll;     /* 1 if blocked in poll() — wake on any fd event */
    uint8_t no_new_privs;
    uint8_t dumpable;
    uint8_t thp_disabled;
    uint8_t is_linux;      /* 1 = Linux-personality process: syscalls route
                              through linux_syscall_dispatch (Tater Bridge) */
    uint8_t is_clone_child; /* 1 = first run restores full GPRs from clone_ctx */
    uint8_t _prctl_pad[1];
    /* Linux-personality per-thread bookkeeping (set via Linux syscalls). */
    uint64_t linux_clear_child_tid; /* set_tid_address — futex-wake on exit */
    uint64_t linux_robust_list;     /* set_robust_list head */
    uint64_t linux_rseq_addr;       /* rseq registration area */
    uint32_t linux_rseq_len;
    uint32_t linux_rseq_sig;
    uint64_t linux_sig_blocked;     /* per-thread blocked signal mask */
    uint64_t linux_sig_pending;     /* per-thread pending signal mask */
    uint64_t linux_sigsuspend_saved_mask; /* mask restored by rt_sigreturn */
    uint64_t linux_sigalt_sp;       /* per-thread sigaltstack ss_sp */
    uint64_t linux_sigalt_size;     /* per-thread sigaltstack ss_size */
    uint32_t linux_sigalt_flags;    /* per-thread sigaltstack ss_flags */
    uint8_t linux_sigsuspend_active; /* 1 while rt_sigsuspend mask is active */
    uint8_t _linux_sig_pad[3];
    struct linux_clone_ctx clone_ctx; /* full register set for a clone child */
    uint64_t timer_slack_ns;
    uint64_t kernel_stack_phys;
    uint32_t kernel_stack_pages;
    void (*kentry)(void *arg);
    void *karg;
    /* Extended FPU/SSE/AVX state, saved/restored across context switches via
     * XSAVE/XRSTOR. 64-byte aligned (required by XSAVE). Sized for x87+SSE+AVX
     * (~832B); 1024 leaves headroom. The aligned(64) makes the whole PCB
     * 64-aligned so &procs[i].fpu_area stays aligned. */
    uint8_t fpu_area[1024] __attribute__((aligned(64)));
    /* fry1381: FPU/SSE/AVX state saved across SIGNAL delivery (separate from
     * fpu_area which is for context switches). Saved when a signal frame is
     * built, restored on rt_sigreturn, so handlers can't corrupt the
     * interrupted thread's vector registers. */
    uint8_t sig_fpu_area[1024] __attribute__((aligned(64)));
};

extern struct fry_process procs[PROC_MAX];
void lx_fpu_save_area(uint8_t *area);
void lx_fpu_restore_area(const uint8_t *area);
extern volatile uint64_t g_tb_jsc_slot_watch_base;
/* fry1379: absolute DR write-watchpoint addresses. Repurposed from the cage-
 * relative butterfly slots to the JSC hash-set bucket array (deterministic at
 * 0x6FBBFFF11020 across boots) to catch the -1 bulk-fill and any later zeroing
 * of empty buckets. Armed for the Claude process on every context switch. */
extern volatile uint64_t g_tb_jsc_slot_watch[4];

static inline void tb_program_jsc_slot_watchpoints_for(const struct fry_process *p) {
    uint64_t dr7 = 0;
    if (g_tb_jsc_slot_watch[0] && p && !p->is_kernel &&
        (p->pid == TB_CLAUDE_WATCH_TGID || p->tgid == TB_CLAUDE_WATCH_TGID)) {
        uint64_t dr0 = g_tb_jsc_slot_watch[0];
        uint64_t dr1 = g_tb_jsc_slot_watch[1];
        uint64_t dr2 = g_tb_jsc_slot_watch[2];
        uint64_t dr3 = g_tb_jsc_slot_watch[3];
        __asm__ volatile(
            "mov %0, %%dr0\n"
            "mov %1, %%dr1\n"
            "mov %2, %%dr2\n"
            "mov %3, %%dr3\n"
            :
            : "r"(dr0), "r"(dr1), "r"(dr2), "r"(dr3)
            : "memory");
        dr7 = (1ULL << 0) | (1ULL << 2) | (1ULL << 4) | (1ULL << 6) |
              (1ULL << 16) | (2ULL << 18) |
              (1ULL << 20) | (2ULL << 22) |
              (1ULL << 24) | (2ULL << 26) |
              (1ULL << 28) | (2ULL << 30);
    }
    __asm__ volatile("mov %0, %%dr7" : : "r"(dr7) : "memory");
}

int process_init(void);
struct fry_process *process_create_user(uint64_t cr3, uint64_t entry, uint64_t user_rsp, const char *name);
struct fry_process *process_create_user_thread(struct fry_process *parent, uint64_t entry,
                                               uint64_t arg, uint64_t user_rsp);
struct fry_process *process_clone_linux_thread(struct fry_process *parent,
                                                uint64_t user_rip, uint64_t user_rsp,
                                                uint64_t tls,
                                                uint64_t child_tid_addr,
                                                uint64_t parent_tid_addr);
struct fry_process *process_create_kernel(void (*entry)(void *), void *arg, const char *name);
struct fry_process *proc_current(void);
void proc_set_current(struct fry_process *p);
void proc_free(uint32_t pid);
void process_exit_group(uint32_t tgid, uint32_t code);
int process_thread_exit(uint32_t tid, uint32_t code);
int process_thread_join(uint32_t tid);
void process_reap_deferred_stacks(void);
int process_launch(const char *path);
int process_launch_args(const char *path, const char **argv, uint32_t argc,
                        const char **envp, uint32_t envc);
int process_last_launch_error(void);
int process_wait(uint32_t pid);
int process_wait_status(uint32_t pid, int *exit_code_out);
uint32_t process_count(void);
uint32_t process_group_id(const struct fry_process *p);

/* Pipe management — global pipe pool */
extern struct fry_pipe g_pipes[FRY_PIPE_MAX];

/*
 * Kernel socket object — global pool of FRY_SOCK_MAX entries.
 * Wraps the netcore TCP/UDP stack into the process FD model.
 */
enum fry_sock_state {
    SOCK_ST_CLOSED     = 0,
    SOCK_ST_CREATED    = 1,   /* socket() called, not bound/connected */
    SOCK_ST_BOUND      = 2,   /* bind() called */
    SOCK_ST_LISTENING  = 3,   /* listen() called (TCP only) */
    SOCK_ST_CONNECTING = 4,   /* non-blocking connect in progress */
    SOCK_ST_CONNECTED  = 5,   /* connect() completed or accept() returned */
    SOCK_ST_SHUTDOWN   = 6    /* shutdown() called */
};

struct fry_udp_pkt {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint8_t  data[FRY_SOCK_UDP_PKTSZ];
};

struct fry_socket {
    uint8_t  used;
    uint8_t  domain;       /* AF_INET */
    uint8_t  type;         /* SOCK_STREAM or SOCK_DGRAM */
    uint8_t  state;        /* enum fry_sock_state */
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_ip;
    uint32_t remote_ip;
    int      tcp_handle;   /* index into netcore tcp_conns[], -1 if not TCP */
    int      listen_handle;/* netcore tcp_listen handle for LISTENING sockets */
    uint32_t so_rcvtimeo;  /* receive timeout in ms (0 = infinite) */
    uint32_t so_sndtimeo;  /* send timeout in ms (0 = infinite) */
    uint8_t  reuseaddr;
    uint8_t  _pad[3];
    /* UDP receive queue */
    struct fry_udp_pkt udp_rxq[FRY_SOCK_UDP_RXMAX];
    uint8_t  udp_rx_head;
    uint8_t  udp_rx_tail;
    uint8_t  _pad2[2];
};

extern struct fry_socket g_sockets[FRY_SOCK_MAX];

#endif
