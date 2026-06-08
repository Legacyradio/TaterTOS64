// Syscall layer

#include <stdint.h>
#include <errno.h>
#include <fry_limits.h>
#include <fry_fcntl.h>
#include "syscall.h"
#include "process.h"
#include "elf.h"
#include "sched.h"
#include "../fs/vfs.h"
#include "../fs/fat32.h"
#include "../acpi/extended.h"
#include "../mm/pmm.h"
#include "../../drivers/timer/hpet.h"
#include "../mm/vmm.h"
#include "../../boot/efi_handoff.h"
#include "../../boot/early_serial.h"
#include "../../include/tater_trace.h"
#include "../../shared/wifi_abi.h"
#include "../../drivers/net/netcore.h"
#include <fry_socket.h>
#include <linux/futex.h>
#include <sys/prctl.h>
#include <fry_random.h>
#include <fry_time.h>
#include "../entropy/entropy.h"
#include "../../drivers/timer/rtc.h"
#include "../../drivers/smp/smp.h"
#include <fry_seek.h>
#include <fry_input.h>
#include "linux_compat.h"

void kprint(const char *fmt, ...);
void kprint_write(const char *buf, uint64_t len);
void kprint_serial_only(const char *fmt, ...);
void kprint_serial_write(const char *buf, uint64_t len);
uint64_t kread_serial(char *buf, uint64_t len);
void *kmalloc(uint64_t size);
void kfree(void *ptr);
int ps2_kbd_read(char *buf, uint32_t len);
int ps2_kbd_read_event(struct fry_key_event *out);
uint8_t ps2_kbd_get_mods(void);
void ps2_mouse_get(int32_t *x, int32_t *y, uint8_t *btns,
                   int32_t *dx, int32_t *dy);
void ps2_mouse_get_ext(int32_t *x, int32_t *y, uint8_t *btns,
                       int32_t *dx, int32_t *dy, int32_t *wheel);
uint8_t ps2_mouse_has_wheel(void);
void acpi_reset(void);
void acpi_shutdown(void);
extern void syscall_entry(void);
extern struct fry_handoff *g_handoff;
int wifi_9260_get_user_status(struct fry_wifi_status *out);
int wifi_9260_get_scan_entries(struct fry_wifi_scan_entry *out,
                               uint32_t max_entries, uint32_t *count_out);
int wifi_9260_connect_user(const char *ssid, const char *passphrase);
int wifi_9260_get_debug_log(char *buf, uint32_t bufsz);

/* Per-CPU data is in sched.c — syscall_entry accesses it via SWAPGS + %gs:0/8 */
void *sched_percpu_ptr(uint32_t cpu);
static int32_t g_gui_slot_hint = -1;
static uint32_t g_gui_pid_hint = 0;
static uint8_t g_first_user_syscall_seen = 0;
static uint8_t g_first_init_syscall_seen = 0;
static uint8_t g_first_init_gui_spawn_seen = 0;
static uint8_t g_first_gui_fb_seen = 0;
static uint32_t g_spawn_attempt_count = 0;  /* visual spawn tracker */
volatile uint64_t g_tb_jsc_slot_watch_base = 0;
/* fry1379: absolute DR write-watchpoints on the JSC hash-set bucket array.
 * [0],[1],[2] = empty buckets (key=0, should become -1); [3] = an occupied
 * bucket (to see the insert write). Addresses observed deterministic across
 * boots. Catches who writes -1 (fill) vs 0 (zeroing) into the buckets. */
/* fry1389: armed on the corrupt CodeBlock numCalleeLocals field. The faulting
 * JIT prologue (RIP 0x3F4CFB6) reads numCalleeLocals=[CodeBlock+0x14]=0 for a
 * live CodeBlock at 0x6FBFF50F42C0 (deterministic across boots) -> 0-size frame
 * -> infinite stack-zero. 0x6FBFF50F42D0 is the 8-byte-aligned qword covering
 * +0x14. All 4 DRs alias it (avoids DR1-3 watching linear addr 0). Catches any
 * USER-mode write to the field with RIP. (Kernel physmap writes use a different
 * linear address -> see TBVMTOUCH page logging instead.) */
volatile uint64_t g_tb_jsc_slot_watch[4] = { 0ULL, 0ULL, 0ULL, 0ULL };
/* fry1389: kernel-side culprit catcher — any vm op (zero/unmap/demand) whose
 * range covers this page is logged with op + range for the Claude tgid. */
volatile uint64_t g_tb_target_page = 0x6FBFF50F4000ULL;

static int futex_key_for_user_word_ex(struct fry_process *p, uint64_t uaddr,
                                      int private_key, uint64_t *key_out);
__attribute__((noreturn))
static void syscall_exit_current(uint32_t code);

/* Phase 7: kernel clipboard buffer */
static char g_clipboard_buf[FRY_CLIPBOARD_MAX];
static uint32_t g_clipboard_len = 0;
/*
 * SYS_EXIT must not free the currently active process CR3/stack while still
 * executing on them.  Use dedicated kernel-owned stacks for exit teardown.
 * Keep one per CPU because exit teardown can be preempted or happen on more
 * than one CPU; a shared stack can corrupt the saved kernel context.
 */
#define SYS_EXIT_STACK_CPUS 64u
#define SYS_EXIT_STACK_BYTES 16384u
static uint8_t g_sys_exit_stacks[SYS_EXIT_STACK_CPUS][SYS_EXIT_STACK_BYTES]
    __attribute__((aligned(16)));

#define USER_TOP USER_VA_TOP
#define PAGE_SIZE 4096ULL
#define FB_USER_BASE 0x0000000100000000ULL
#define VM_USER_BASE FRY_VM_USER_BASE
#define VM_USER_LIMIT FRY_VM_USER_LIMIT
#define MSR_FS_BASE 0xC0000100u

#define FRY_PROT_READ  0x01u
#define FRY_PROT_WRITE 0x02u
#define FRY_PROT_EXEC  0x04u

#define FRY_MAP_SHARED  0x01u
#define FRY_MAP_PRIVATE 0x02u
#define FRY_MAP_FIXED   0x10u
#define FRY_MAP_ANON    0x20u
#define FRY_MAP_FILE    0x40u
#define FRY_MAP_RESERVE 0x80u
#define FRY_MAP_GUARD   0x100u

#define FRY_AT_FDCWD            (-100)
#define FRY_AT_SYMLINK_NOFOLLOW 0x100u
#define FRY_AT_REMOVEDIR        0x200u
#define FRY_AT_EACCESS          0x200u
#define FRY_AT_NO_AUTOMOUNT     0x800u
#define FRY_AT_EMPTY_PATH       0x1000u

#define FRY_O_ACCMODE   0x3u
#define FRY_O_DIRECTORY 0x10000u

#define FRY_X_OK 1
#define FRY_W_OK 2
#define FRY_R_OK 4

static uint32_t g_tb_trace_claude_tgid = 3u;
#define TB_TRACE_CLAUDE_TGID g_tb_trace_claude_tgid
/* Master switch for the high-volume per-syscall enter/ret/path trace. Default
 * OFF: emitting a serial line per syscall makes the kernel busy-wait on the
 * UART for every call and dominates runtime (fry1376). The targeted JSC
 * futex/skip/watch markers stay on independently. Flip to 1 for full tracing. */
static volatile int g_tb_trace_syscalls = 0;

#define LNX_SIGHUP   1
#define LNX_SIGINT   2
#define LNX_SIGSEGV  11
#define LNX_SIGKILL  9
#define LNX_SIGSTOP  19
#define LNX_SIGCHLD  17
#define LNX_SIGURG   23
#define LNX_SIGWINCH 28

#define LNX_SIG_BLOCK   0
#define LNX_SIG_UNBLOCK 1
#define LNX_SIG_SETMASK 2

#define LNX_SS_DISABLE  2u
#define LNX_SA_ONSTACK  0x08000000ULL
#define LNX_SA_RESTORER 0x04000000ULL
#define LX_SIGFRAME_MAGIC 0x5453494752544652ULL /* "RFTRGIST" */

#define LX_SYSFRAME_A6       0
#define LX_SYSFRAME_R9       2
#define LX_SYSFRAME_R8       3
#define LX_SYSFRAME_R10      4
#define LX_SYSFRAME_RDX      5
#define LX_SYSFRAME_RSI      6
#define LX_SYSFRAME_RDI      7
#define LX_SYSFRAME_RBP      8
#define LX_SYSFRAME_RFLAGS   9
#define LX_SYSFRAME_RIP      10
#define LX_SYSFRAME_RSP      11

struct lnx_user_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct lnx_user_stack {
    uint64_t sp;
    uint32_t flags;
    uint32_t pad;
    uint64_t size;
};

struct lx_sigrestore_regs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rsp, rflags;
};

struct lx_mcontext {
    uint64_t gregs[23];
    uint64_t fpregs;
    uint64_t reserved[8];
};

struct lx_ucontext {
    uint64_t flags;
    uint64_t link;
    struct lnx_user_stack stack;
    struct lx_mcontext mcontext;
    uint64_t sigmask;
};

struct lx_rt_sigframe {
    uint64_t restorer;
    uint64_t magic;
    uint64_t saved_mask;
    struct lx_sigrestore_regs regs;
    uint8_t siginfo[128];
    struct lx_ucontext ucontext;
};

static struct fry_process_shared *proc_shared_state(struct fry_process *p) {
    return p ? p->shared : 0;
}

static const struct fry_process_shared *proc_shared_state_const(const struct fry_process *p) {
    return p ? p->shared : 0;
}

static struct fry_process *proc_find_task(uint32_t pid) {
    if (pid == 0) return 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid &&
            procs[i].state != PROC_UNUSED &&
            procs[i].state != PROC_DEAD) {
            return &procs[i];
        }
    }
    return 0;
}

static struct fry_process *proc_find_task_any(uint32_t pid) {
    if (pid == 0) return 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid &&
            procs[i].state != PROC_UNUSED) {
            return &procs[i];
        }
    }
    return 0;
}

static struct fry_process *proc_find_group_leader(uint32_t tgid) {
    struct fry_process *p = proc_find_task(tgid);
    if (!p || p->pid != p->tgid) return 0;
    return p;
}

static struct fry_process *proc_find_group_leader_any(uint32_t tgid) {
    struct fry_process *p = proc_find_task_any(tgid);
    if (!p || p->pid != p->tgid) return 0;
    return p;
}

static inline void wrmsr(uint32_t msr, uint64_t val);
static uint64_t sys_now_ms(void);

static inline void write_user_fs_base(uint64_t base) {
    wrmsr(MSR_FS_BASE, base);
}

#define PROC_VMREGS(p) (proc_shared_state((p))->vm_regions)
#define PROC_VMREGS_CONST(p) (proc_shared_state_const((p))->vm_regions)
#define PROC_FD_PTRS(p) (proc_shared_state((p))->fd_ptrs)
#define PROC_FD_TABLE(p) (proc_shared_state((p))->fd_table)
#define PROC_FD_KIND(p) (proc_shared_state((p))->fd_kind)
#define PROC_FD_FLAGS(p) (proc_shared_state((p))->fd_flags)

static int vm_unmap_region_range(struct fry_process *p, uint64_t base, uint64_t length);

/* ====================================================================
 * Poll event definitions (matching POSIX numbering)
 * ==================================================================== */
struct fry_pollfd {
    int32_t  fd;
    uint16_t events;    /* requested events */
    uint16_t revents;   /* returned events */
};

#define FRY_POLLIN   0x0001u
#define FRY_POLLOUT  0x0002u
#define FRY_POLLERR  0x0008u
#define FRY_POLLHUP  0x0010u
#define FRY_POLLNVAL 0x0020u

/* ====================================================================
 * Pipe helpers
 * ==================================================================== */
static int pipe_data_avail(const struct fry_pipe *pp) {
    return pp->head != pp->tail;
}

static uint32_t pipe_bytes_avail(const struct fry_pipe *pp) {
    if (pp->tail >= pp->head)
        return pp->tail - pp->head;
    return FRY_PIPE_BUFSZ - pp->head + pp->tail;
}

static uint32_t pipe_space_avail(const struct fry_pipe *pp) {
    return FRY_PIPE_BUFSZ - 1u - pipe_bytes_avail(pp);
}

static int pipe_alloc(void) {
    for (int i = 0; i < FRY_PIPE_MAX; i++) {
        if (!g_pipes[i].used) {
            g_pipes[i].used = 1;
            g_pipes[i].head = 0;
            g_pipes[i].tail = 0;
            g_pipes[i].readers = 0;
            g_pipes[i].writers = 0;
            return i;
        }
    }
    return -1;
}

static int fd_alloc(struct fry_process *p) {
    struct fry_process_shared *shared = proc_shared_state(p);
    if (!shared) return -1;
    for (int fd = 3; fd < FRY_FD_MAX; fd++) {
        if (shared->fd_kind[fd] == FD_NONE && !shared->fd_ptrs[fd]) {
            return fd;
        }
    }
    return -1;
}

static void fd_install(struct fry_process *p, int fd, void *ptr, uint8_t kind, uint32_t flags) {
    struct fry_process_shared *shared = proc_shared_state(p);
    if (!shared || fd < 3 || fd >= FRY_FD_MAX) return;
    shared->fd_ptrs[fd] = ptr;
    shared->fd_table[fd] = 1;
    shared->fd_kind[fd] = kind;
    shared->fd_flags[fd] = flags;
    shared->open_fds++;
}

static void fd_release(struct fry_process *p, int fd);
static int fd_path_set(struct fry_process_shared *shared, int fd, const char *path);

static void *stdio_fd_ptr(int fd) {
    return (void *)(uintptr_t)(fd + 1);
}

static int stdio_fd_from_ptr(void *ptr) {
    uintptr_t v = (uintptr_t)ptr;
    if (v < 1 || v > 3) return -1;
    return (int)v - 1;
}

static int fd_is_open(struct fry_process_shared *shared, int fd) {
    if (!shared || fd < 0 || fd >= FRY_FD_MAX) return 0;
    if (fd >= 0 && fd <= 2) return 1;
    return shared->fd_kind[fd] != FD_NONE && shared->fd_ptrs[fd] != 0;
}

static int lx_dup_fd_at(struct fry_process *p, int oldfd, int minfd, uint32_t extra_flags) {
    struct fry_process_shared *shared = proc_shared_state(p);
    if (!shared) return -ESRCH;
    if (oldfd < 0 || oldfd >= FRY_FD_MAX || !fd_is_open(shared, oldfd))
        return -EBADF;
    if (minfd < 0 || minfd >= FRY_FD_MAX) return -EINVAL;

    int start = minfd < 3 ? 3 : minfd;
    int newfd = -1;
    for (int fd = start; fd < FRY_FD_MAX; fd++) {
        if (!fd_is_open(shared, fd)) {
            newfd = fd;
            break;
        }
    }
    if (newfd < 0) return -EMFILE;

    if (oldfd <= 2) {
        fd_install(p, newfd, stdio_fd_ptr(oldfd), FD_STDIO, extra_flags);
        return newfd;
    }

    void *optr = shared->fd_ptrs[oldfd];
    uint8_t okind = shared->fd_kind[oldfd];
    uint32_t flags = shared->fd_flags[oldfd] | extra_flags;
    fd_install(p, newfd, optr, okind, flags);
    shared->fd_table[newfd] = shared->fd_table[oldfd];
    if (shared->fd_paths[oldfd][0]) {
        int prc = fd_path_set(shared, newfd, shared->fd_paths[oldfd]);
        if (prc < 0) {
            fd_release(p, newfd);
            return prc;
        }
        if (okind == FD_DIR) shared->fd_ptrs[newfd] = shared->fd_paths[newfd];
    }
    if (okind == FD_PIPE_READ) {
        ((struct fry_pipe *)optr)->readers++;
    } else if (okind == FD_PIPE_WRITE) {
        ((struct fry_pipe *)optr)->writers++;
    }
    return newfd;
}

static void fd_release(struct fry_process *p, int fd) {
    struct fry_process_shared *shared = proc_shared_state(p);
    if (!shared || fd < 3 || fd >= FRY_FD_MAX) return;
    shared->fd_ptrs[fd] = 0;
    shared->fd_table[fd] = -1;
    shared->fd_kind[fd] = FD_NONE;
    shared->fd_flags[fd] = 0;
    shared->fd_paths[fd][0] = 0;
    if (shared->open_fds > 0) shared->open_fds--;
}

static uint32_t path_len(const char *s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int fd_path_set(struct fry_process_shared *shared, int fd, const char *path) {
    if (!shared || fd < 3 || fd >= FRY_FD_MAX || !path) return -EINVAL;
    uint32_t n = path_len(path);
    if (n == 0 || n >= FRY_PATH_MAX) return -ENAMETOOLONG;
    for (uint32_t i = 0; i <= n; i++) shared->fd_paths[fd][i] = path[i];
    return 0;
}

static int path_append_segment(char out[FRY_PATH_MAX], uint32_t *len,
                               const char *seg, uint32_t seg_len) {
    if (!out || !len || !seg || seg_len == 0) return -EINVAL;
    uint32_t need = *len + ((*len > 1) ? 1u : 0u) + seg_len;
    if (need >= FRY_PATH_MAX) return -ENAMETOOLONG;
    if (*len > 1) out[(*len)++] = '/';
    for (uint32_t i = 0; i < seg_len; i++) out[(*len)++] = seg[i];
    out[*len] = 0;
    return 0;
}

static void path_pop_segment(char out[FRY_PATH_MAX], uint32_t *len) {
    if (!out || !len || *len <= 1) {
        if (out) out[0] = '/', out[1] = 0;
        if (len) *len = 1;
        return;
    }
    while (*len > 1 && out[*len - 1] != '/') (*len)--;
    if (*len > 1) (*len)--;
    out[*len] = 0;
}

static int path_normalize_absolute(const char *in, char out[FRY_PATH_MAX]) {
    if (!in || !out || in[0] != '/') return -EINVAL;
    out[0] = '/';
    out[1] = 0;
    uint32_t out_len = 1;

    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        uint32_t seg_len = 0;
        while (p[seg_len] && p[seg_len] != '/') seg_len++;

        if (seg_len == 1 && seg[0] == '.') {
        } else if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            path_pop_segment(out, &out_len);
        } else {
            int rc = path_append_segment(out, &out_len, seg, seg_len);
            if (rc < 0) return rc;
        }
        p += seg_len;
    }

    return 0;
}

static int resolve_at_path(struct fry_process *cur, int dirfd,
                           const char *path, char out[FRY_PATH_MAX]) {
    if (!cur || !proc_shared_state(cur)) return -ESRCH;
    if (!path || !out) return -EINVAL;
    if (path[0] == '/') return path_normalize_absolute(path, out);
    if (path[0] == 0) return -ENOENT;

    struct fry_process_shared *shared = proc_shared_state(cur);
    const char *base = "/";
    if (dirfd != FRY_AT_FDCWD) {
        if (dirfd < 3 || dirfd >= FRY_FD_MAX) return -EBADF;
        if (!shared->fd_ptrs[dirfd] || shared->fd_kind[dirfd] == FD_NONE)
            return -EBADF;
        if (shared->fd_kind[dirfd] != FD_DIR) return -ENOTDIR;
        if (!shared->fd_paths[dirfd][0]) return -EBADF;
        base = shared->fd_paths[dirfd];
    }

    char joined[FRY_PATH_MAX * 2];
    uint32_t pos = 0;
    for (uint32_t i = 0; base[i]; i++) {
        if (pos + 1 >= sizeof(joined)) return -ENAMETOOLONG;
        joined[pos++] = base[i];
    }
    if (pos == 0) {
        joined[pos++] = '/';
    } else if (joined[pos - 1] != '/') {
        if (pos + 1 >= sizeof(joined)) return -ENAMETOOLONG;
        joined[pos++] = '/';
    }
    for (uint32_t i = 0; path[i]; i++) {
        if (pos + 1 >= sizeof(joined)) return -ENAMETOOLONG;
        joined[pos++] = path[i];
    }
    joined[pos] = 0;

    return path_normalize_absolute(joined, out);
}

static int install_fd_path(struct fry_process *cur, int fd, const char *path) {
    struct fry_process_shared *shared = proc_shared_state(cur);
    int rc = fd_path_set(shared, fd, path);
    if (rc < 0) {
        fd_release(cur, fd);
        return rc;
    }
    if (shared->fd_kind[fd] == FD_DIR) shared->fd_ptrs[fd] = shared->fd_paths[fd];
    return 0;
}

static int64_t pipe_read(struct fry_pipe *pp, char *buf, uint64_t len, uint32_t flags) {
    if (!pp || !buf || len == 0) return -EINVAL;

    /* No data available */
    if (!pipe_data_avail(pp)) {
        /* All writers closed → EOF */
        if (pp->writers == 0) return 0;
        /* Non-blocking → EAGAIN */
        if (flags & O_NONBLOCK) return -EAGAIN;
        /* Caller should block and retry */
        return -EAGAIN;
    }

    uint64_t nr = 0;
    while (nr < len && pp->head != pp->tail) {
        buf[nr++] = (char)pp->buf[pp->head];
        pp->head = (pp->head + 1u) % FRY_PIPE_BUFSZ;
    }
    /* Wake poll waiters — pipe became writable */
    sched_wake_poll_waiters();
    return (int64_t)nr;
}

static int64_t pipe_write(struct fry_pipe *pp, const char *buf, uint64_t len, uint32_t flags) {
    if (!pp || !buf || len == 0) return -EINVAL;

    /* No readers → broken pipe */
    if (pp->readers == 0) return -EPIPE;

    /* Buffer full */
    if (pipe_space_avail(pp) == 0) {
        if (flags & O_NONBLOCK) return -EAGAIN;
        return -EAGAIN; /* caller should block and retry */
    }

    uint64_t nw = 0;
    while (nw < len && pipe_space_avail(pp) > 0) {
        pp->buf[pp->tail] = (uint8_t)buf[nw++];
        pp->tail = (pp->tail + 1u) % FRY_PIPE_BUFSZ;
    }
    /* Wake poll waiters — pipe became readable */
    sched_wake_poll_waiters();
    return (int64_t)nw;
}

/* Chrome port — POSIX struct definitions */
struct epoll_item {
    int fd;
    uint32_t events;
    uint64_t data;
    struct epoll_item *next;
};

struct epoll_cb {
    struct epoll_item *items;
    int count;
    volatile int lock;
};

struct epoll_event {
    uint32_t events;
    uint64_t data;
};

struct fry_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

struct fry_msghdr {
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t _pad0;
    uint64_t msg_iov;
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    int32_t msg_flags;
    uint32_t _pad1;
};

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* eventfd — poll_check_fd needs this full type */
struct eventfd_cb {
    uint64_t counter;
    int semaphore;
    int nonblock;
    volatile int lock;
};

/*
 * Timerfd — file descriptor that becomes readable when a timer expires.
 * Uses kernel HPET-based ms clock.  it_value = initial expiration (relative),
 * it_interval = reload value for periodic timers.
 */
struct timerfd_cb {
    uint64_t it_value_ms;     /* ms remaining until next expiration (0 = disarmed) */
    uint64_t it_interval_ms;  /* periodic reload value (0 = one-shot) */
    uint64_t deadline_ms;     /* absolute deadline in ms (computed at arm time) */
    uint64_t expirations;     /* number of expirations that have occurred */
    int clockid;              /* CLOCK_REALTIME or CLOCK_MONOTONIC */
    int nonblock;
    uint8_t used;
};

static void timerfd_update_expirations(struct timerfd_cb *tm, uint64_t now_ms) {
    if (!tm || !tm->used || tm->it_value_ms == 0 || tm->deadline_ms == 0)
        return;
    if (now_ms < tm->deadline_ms)
        return;
    if (tm->it_interval_ms == 0) {
        tm->expirations++;
        tm->it_value_ms = 0;
        tm->deadline_ms = 0;
        return;
    }
    uint64_t count = 1 + ((now_ms - tm->deadline_ms) / tm->it_interval_ms);
    tm->expirations += count;
    tm->deadline_ms += count * tm->it_interval_ms;
}

/*
 * Signalfd — file descriptor that becomes readable when matching signals
 * are pending. TaterTOS does not deliver async signals to user space, but
 * the signalfd API surface lets Chromium/Ladybird code compile and link.
 * Pending signals are tracked as a bitmask; the fd becomes readable when
 * any bit in the mask is set in the process's pending signal set.
 */
struct signalfd_cb {
    uint64_t mask;            /* sigset_t of signals this fd watches */
    int nonblock;
    uint8_t used;
};

/*
 * Inotify — filesystem event monitoring.
 * Per-fd watch list and event queue.  TaterTOS VFS generates events
 * through vfs_inotify_notify() which walks all inotify fds, but only
 * for operations that pass through the VFS layer (open/create/delete/rename).
 */
#define INOTIFY_WATCH_MAX    16
#define INOTIFY_EVENT_MAX    32
#define INOTIFY_NAME_MAX     128

struct inotify_watch {
    int      wd;                 /* watch descriptor (index + 1) */
    uint32_t mask;               /* event mask being watched */
    char     path[FRY_PATH_MAX]; /* path being watched */
    uint8_t  used;
};

struct inotify_event_buf {
    int      wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;                /* length of name that follows */
    char     name[INOTIFY_NAME_MAX];
};

struct inotify_cb {
    struct inotify_watch  watches[INOTIFY_WATCH_MAX];
    int                   watch_count;
    int                   next_wd;
    struct inotify_event_buf events[INOTIFY_EVENT_MAX];
    int                    ev_head;
    int                    ev_tail;
    int                    nonblock;
    uint8_t                used;
};

/*
 * Memfd — memory-backed file descriptor.
 * Behaves like a regular file but backed by physical memory instead of
 * a filesystem on disk. Supports read, write, lseek, ftruncate, and
 * mmap (maps the backing pages directly into the process address space).
 *
 * Chromium uses memfd_create for: base::SharedMemory, ELF loading,
 * Mojo shared buffers, and graphics allocation.
 */
#define MEMFD_PAGE_CHUNK  4   /* grow by 4 pages (16KB) at a time */
struct memfd_cb {
    uint64_t  size;          /* logical file size */
    uint64_t  capacity;      /* allocated page count * PAGE_SIZE */
    uint64_t  pos;           /* current read/write position */
    uint32_t  page_count;    /* number of physical pages allocated */
    uint64_t *pages;         /* array of physical page addresses (page_count entries) */
    uint8_t   used;
    char      name[32];      /* name from memfd_create (for /proc visibility) */
};

/* Check pollability of a single fd. Returns revents. */
static uint16_t poll_check_fd(struct fry_process *p, int32_t fd, uint16_t events) {
    struct fry_process_shared *shared = proc_shared_state(p);
    if (!shared) return FRY_POLLNVAL;
    if (fd < 0 || fd >= FRY_FD_MAX) return FRY_POLLNVAL;

    /* stdin/stdout/stderr: always ready for their direction */
    if (fd == 0) return FRY_POLLIN;
    if (fd == 1 || fd == 2) return FRY_POLLOUT;

    uint8_t kind = shared->fd_kind[fd];
    if (kind == FD_NONE || !shared->fd_ptrs[fd]) return FRY_POLLNVAL;

    uint16_t revents = 0;

    if (kind == FD_STDIO) {
        int realfd = stdio_fd_from_ptr(shared->fd_ptrs[fd]);
        if (realfd == 0) revents |= FRY_POLLIN;
        else if (realfd == 1 || realfd == 2) revents |= FRY_POLLOUT;
    } else if (kind == FD_FILE) {
        /* Files are always ready for read and write */
        if (events & FRY_POLLIN) revents |= FRY_POLLIN;
        if (events & FRY_POLLOUT) revents |= FRY_POLLOUT;
    } else if (kind == FD_PIPE_READ) {
        struct fry_pipe *pp = (struct fry_pipe *)shared->fd_ptrs[fd];
        if (!pp || !pp->used) return FRY_POLLNVAL;
        if (pipe_data_avail(pp)) {
            revents |= FRY_POLLIN;
        }
        if (pp->writers == 0) {
            revents |= FRY_POLLHUP;  /* all writers closed — EOF pending */
        }
    } else if (kind == FD_PIPE_WRITE) {
        struct fry_pipe *pp = (struct fry_pipe *)shared->fd_ptrs[fd];
        if (!pp || !pp->used) return FRY_POLLNVAL;
        if (pipe_space_avail(pp) > 0) {
            revents |= FRY_POLLOUT;
        }
        if (pp->readers == 0) {
            revents |= FRY_POLLERR;  /* broken pipe */
        }
    } else if (kind == FD_SOCKET) {
        struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[fd];
        if (!sk || !sk->used) return FRY_POLLNVAL;
        if (sk->type == SOCK_STREAM) {
            /* AF_UNIX socketpair: poll via pipe buffers */
            if (sk->domain == 1 && sk->tcp_handle >= 0 && sk->tcp_handle < FRY_PIPE_MAX) {
                struct fry_pipe *wpp = &g_pipes[sk->tcp_handle];
                struct fry_pipe *rpp = &g_pipes[sk->listen_handle];
                if (!wpp->used || !rpp->used) {
                    revents |= FRY_POLLHUP;
                } else {
                    /* Writable if write pipe has space */
                    if (pipe_space_avail(wpp) > 0 || wpp->readers == 0)
                        revents |= FRY_POLLOUT;
                    /* Readable if read pipe has data */
                    if (pipe_data_avail(rpp))
                        revents |= FRY_POLLIN;
                    /* HUP when writers side is closed */
                    if (wpp->readers == 0)
                        revents |= FRY_POLLHUP;
                }
            } else if (sk->state == SOCK_ST_CONNECTED && sk->tcp_handle >= 0) {
                if (tcp_rx_available(sk->tcp_handle) > 0)
                    revents |= FRY_POLLIN;
                if (tcp_is_connected(sk->tcp_handle))
                    revents |= FRY_POLLOUT;
                else
                    revents |= FRY_POLLHUP;
            } else if (sk->state == SOCK_ST_LISTENING && sk->listen_handle >= 0) {
                /* Readable when accept() would succeed */
                if (tcp_accept(sk->listen_handle) >= 0) {
                    /* Undo the accept — we just peeked */
                    /* Can't easily undo, so don't call accept here.
                       Instead check for un-accepted ESTABLISHED children. */
                }
                /* Scan for pending connections */
                for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
                    int st = tcp_get_state(i);
                    if (st == TCP_ESTABLISHED || st == TCP_SYN_RECV) {
                        /* This is a simplified check */
                        revents |= FRY_POLLIN;
                        break;
                    }
                }
            } else if (sk->state == SOCK_ST_SHUTDOWN || sk->state == SOCK_ST_CLOSED) {
                revents |= FRY_POLLHUP;
            }
        } else if (sk->type == SOCK_DGRAM) {
            if (sk->udp_rx_head != sk->udp_rx_tail)
                revents |= FRY_POLLIN;
            revents |= FRY_POLLOUT; /* UDP is always writable */
        }
    } else if (kind == FD_EPOLL) {
        /* epoll fds are always readable (events may be ready) */
        if (events & FRY_POLLIN) revents |= FRY_POLLIN;
    } else if (kind == FD_EVENTFD) {
        struct eventfd_cb *ev = (struct eventfd_cb *)shared->fd_ptrs[fd];
        if (!ev) return FRY_POLLNVAL;
        if (ev->counter > 0) {
            if (events & FRY_POLLIN) revents |= FRY_POLLIN;
        }
        /* Always writable (counter can be incremented) */
        if (events & FRY_POLLOUT) revents |= FRY_POLLOUT;
    } else if (kind == FD_TIMERFD) {
        struct timerfd_cb *tm = (struct timerfd_cb *)shared->fd_ptrs[fd];
        if (!tm || !tm->used) return FRY_POLLNVAL;
        timerfd_update_expirations(tm, sys_now_ms());
        if (tm->expirations > 0) {
            if (events & FRY_POLLIN) revents |= FRY_POLLIN;
        }
        if (events & FRY_POLLOUT) revents |= FRY_POLLOUT;
    } else if (kind == FD_SIGNALFD) {
        struct signalfd_cb *sf = (struct signalfd_cb *)shared->fd_ptrs[fd];
        if (!sf || !sf->used) return FRY_POLLNVAL;
        /* No async signal delivery yet: a signalfd exists, but it is not
         * readable until real pending-signal tracking lands. Returning readable
         * here makes Linux epoll loops spin forever. */
        if (events & FRY_POLLOUT) revents |= FRY_POLLOUT;
    } else if (kind == FD_INOTIFY) {
        struct inotify_cb *in = (struct inotify_cb *)shared->fd_ptrs[fd];
        if (!in || !in->used) return FRY_POLLNVAL;
        if (in->ev_head != in->ev_tail) {
            if (events & FRY_POLLIN) revents |= FRY_POLLIN;
        }
        if (events & FRY_POLLOUT) revents |= FRY_POLLOUT;
    } else if (kind == FD_MEMFD) {
        struct memfd_cb *mf = (struct memfd_cb *)shared->fd_ptrs[fd];
        if (!mf || !mf->used) return FRY_POLLNVAL;
        if (events & FRY_POLLIN) revents |= FRY_POLLIN;
        if (events & FRY_POLLOUT) revents |= FRY_POLLOUT;
    }

    return revents & (events | FRY_POLLERR | FRY_POLLHUP | FRY_POLLNVAL);
}

/* ====================================================================
 * Socket helpers
 * ==================================================================== */
static int sock_alloc(void) {
    for (int i = 0; i < FRY_SOCK_MAX; i++) {
        if (!g_sockets[i].used) {
            uint8_t *p = (uint8_t *)&g_sockets[i];
            for (uint32_t j = 0; j < sizeof(struct fry_socket); j++) p[j] = 0;
            g_sockets[i].used = 1;
            g_sockets[i].tcp_handle = -1;
            g_sockets[i].listen_handle = -1;
            return i;
        }
    }
    return -1;
}

static uint16_t g_sock_next_port = 49200;

static uint16_t sock_ephemeral_port(void) {
    return g_sock_next_port++;
}

/* ====================================================================
 * Memfd helpers
 * ==================================================================== */

/*
 * Grow the memfd backing store to at least new_capacity bytes.
 * Pages are allocated from PMM and physically contiguous is preferred
 * but not required — we track a page list. Returns 0 on success.
 */
static int memfd_grow(struct memfd_cb *mf, uint64_t new_capacity) {
    if (!mf || new_capacity <= mf->capacity) return 0;
    uint32_t need_pages = (uint32_t)((new_capacity + PAGE_SIZE - 1ULL) / PAGE_SIZE);
    if (need_pages > 1024 * 1024) return -1; /* sanity cap: 4GB */
    uint32_t old_pages = mf->page_count;

    /* Allocate new page array */
    uint64_t *new_pages = (uint64_t *)kmalloc(need_pages * sizeof(uint64_t));
    if (!new_pages) return -1;

    /* Copy old page pointers */
    for (uint32_t i = 0; i < old_pages; i++)
        new_pages[i] = mf->pages[i];

    /* Allocate new pages */
    for (uint32_t i = old_pages; i < need_pages; i++) {
        new_pages[i] = pmm_alloc_page();
        if (!new_pages[i]) {
            /* Free what we allocated so far */
            for (uint32_t j = old_pages; j < i; j++)
                pmm_free_page(new_pages[j]);
            kfree(new_pages);
            return -1;
        }
        /* Zero the new page */
        uint8_t *virt = (uint8_t *)(uintptr_t)vmm_phys_to_virt(new_pages[i]);
        for (uint32_t z = 0; z < PAGE_SIZE; z++) virt[z] = 0;
    }

    kfree(mf->pages);
    mf->pages = new_pages;
    mf->page_count = need_pages;
    mf->capacity = (uint64_t)need_pages * PAGE_SIZE;
    return 0;
}

static void memfd_free_pages(struct memfd_cb *mf) {
    if (!mf || !mf->pages) return;
    for (uint32_t i = 0; i < mf->page_count; i++) {
        if (mf->pages[i])
            pmm_free_page(mf->pages[i]);
    }
    kfree(mf->pages);
    mf->pages = 0;
    mf->page_count = 0;
    mf->capacity = 0;
}

static int64_t memfd_read(struct memfd_cb *mf, void *buf, uint64_t len) {
    if (!mf || !buf || len == 0) return -EINVAL;
    if (mf->pos >= mf->size) return 0; /* EOF */
    uint64_t available = mf->size - mf->pos;
    if (len > available) len = available;
    if (len == 0) return 0;

    uint64_t offset_in_file = mf->pos;
    uint8_t *dst = (uint8_t *)buf;
    uint64_t remaining = len;

    while (remaining > 0) {
        uint32_t page_idx = (uint32_t)(offset_in_file / PAGE_SIZE);
        uint32_t page_off = (uint32_t)(offset_in_file % PAGE_SIZE);
        uint32_t chunk = (uint32_t)(PAGE_SIZE - page_off);
        if (chunk > remaining) chunk = (uint32_t)remaining;

        if (page_idx >= mf->page_count) break;
        uint8_t *virt = (uint8_t *)(uintptr_t)vmm_phys_to_virt(mf->pages[page_idx]);
        for (uint32_t i = 0; i < chunk; i++)
            dst[i] = virt[page_off + i];

        dst += chunk;
        offset_in_file += chunk;
        remaining -= chunk;
    }

    uint64_t done = len - remaining;
    mf->pos += done;
    return (int64_t)done;
}

static int64_t memfd_write(struct memfd_cb *mf, const void *buf, uint64_t len) {
    if (!mf || !buf || len == 0) return -EINVAL;

    uint64_t end_pos = mf->pos + len;
    if (end_pos > mf->capacity) {
        /* Grow the buffer */
        if (memfd_grow(mf, end_pos) != 0) return -ENOMEM;
    }

    uint64_t offset_in_file = mf->pos;
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t remaining = len;

    while (remaining > 0) {
        uint32_t page_idx = (uint32_t)(offset_in_file / PAGE_SIZE);
        uint32_t page_off = (uint32_t)(offset_in_file % PAGE_SIZE);
        uint32_t chunk = (uint32_t)(PAGE_SIZE - page_off);
        if (chunk > remaining) chunk = (uint32_t)remaining;

        if (page_idx >= mf->page_count) break;
        uint8_t *virt = (uint8_t *)(uintptr_t)vmm_phys_to_virt(mf->pages[page_idx]);
        for (uint32_t i = 0; i < chunk; i++)
            virt[page_off + i] = src[i];

        src += chunk;
        offset_in_file += chunk;
        remaining -= chunk;
    }

    uint64_t done = len - remaining;
    mf->pos += done;
    if (mf->pos > mf->size) mf->size = mf->pos;
    return (int64_t)done;
}

static int64_t memfd_lseek(struct memfd_cb *mf, int64_t offset, int whence) {
    if (!mf) return -EINVAL;
    uint64_t new_pos;
    switch (whence) {
        case FRY_SEEK_SET:
            if (offset < 0) return -EINVAL;
            new_pos = (uint64_t)offset;
            break;
        case FRY_SEEK_CUR:
            if ((int64_t)mf->pos + offset < 0) return -EINVAL;
            new_pos = (uint64_t)((int64_t)mf->pos + offset);
            break;
        case FRY_SEEK_END:
            if ((int64_t)mf->size + offset < 0) return -EINVAL;
            new_pos = (uint64_t)((int64_t)mf->size + offset);
            break;
        default:
            return -EINVAL;
    }
    mf->pos = new_pos;
    return (int64_t)new_pos;
}

static int memfd_truncate(struct memfd_cb *mf, uint64_t length) {
    if (!mf) return -EINVAL;
    if (length > mf->capacity) {
        if (memfd_grow(mf, length) != 0) return -ENOMEM;
    }
    mf->size = length;
    if (mf->pos > mf->size) mf->pos = mf->size;
    return 0;
}

/* UDP socket handler — called by netcore for unmatched UDP datagrams */
static void sock_udp_rx(uint16_t dst_port, uint32_t src_ip,
                         uint16_t src_port,
                         const uint8_t *data, uint16_t len) {
    for (int i = 0; i < FRY_SOCK_MAX; i++) {
        struct fry_socket *sk = &g_sockets[i];
        if (sk->used && sk->type == SOCK_DGRAM && sk->local_port == dst_port) {
            uint8_t next = (sk->udp_rx_head + 1) % FRY_SOCK_UDP_RXMAX;
            if (next == sk->udp_rx_tail) return; /* queue full, drop */
            struct fry_udp_pkt *pkt = &sk->udp_rxq[sk->udp_rx_head];
            pkt->src_ip = src_ip;
            pkt->src_port = src_port;
            uint16_t copylen = len;
            if (copylen > FRY_SOCK_UDP_PKTSZ) copylen = FRY_SOCK_UDP_PKTSZ;
            pkt->len = copylen;
            for (uint16_t j = 0; j < copylen; j++) pkt->data[j] = data[j];
            sk->udp_rx_head = next;
            sched_wake_poll_waiters();
            return;
        }
    }
}

static uint8_t g_sock_udp_handler_installed = 0;

static void sock_ensure_udp_handler(void) {
    if (!g_sock_udp_handler_installed) {
        udp_set_socket_handler(sock_udp_rx);
        g_sock_udp_handler_installed = 1;
    }
}

struct readdir_ctx {
    char *buf;
    uint32_t len;
    uint32_t pos;
};
struct readdir_ex_ctx {
    uint8_t *buf;
    uint32_t len;
    uint32_t pos;
};
struct lx_getdents64_ctx {
    uint8_t *buf;
    uint32_t len;
    uint32_t pos;
    uint32_t base_skip;
    uint32_t skip;
    uint32_t emitted;
    uint32_t overflow;
};
struct fry_dirent_hdr {
    uint16_t rec_len;
    uint16_t name_len;
    uint32_t attr;
    uint64_t size;
};

static uint32_t read_le32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

static int is_taterwin_msg(const char *buf, uint64_t len) {
    /* tw_msg_header_t: u32 type, u32 magic("TWIN"=0x5457494E). */
    if (!buf || len < 8) return 0;
    uint32_t type = read_le32(buf);
    uint32_t magic = read_le32(buf + 4);
    if (magic != 0x5457494EU) return 0;
    return type >= 1 && type <= 7;
}

static int user_ptr_ok(uint64_t ptr, uint64_t len) {
    if (ptr >= USER_TOP) return 0;
    if (len == 0) return 1;
    if (ptr + len < ptr) return 0;
    return (ptr + len) <= USER_TOP;
}

static int user_buf_accessible(struct fry_process *p, uint64_t ptr, uint64_t len, int want_write) {
    if (!p || !p->cr3) return 0;
    if (!user_ptr_ok(ptr, len)) return 0;
    if (len == 0) return 1;

    uint64_t va = ptr & ~(PAGE_SIZE - 1ULL);
    uint64_t last = (ptr + len - 1ULL) & ~(PAGE_SIZE - 1ULL);
    for (;;) {
        uint64_t flags = vmm_query_user_flags(p->cr3, va);
        if ((flags & VMM_FLAG_PRESENT) == 0) return 0;
        if ((flags & VMM_FLAG_USER) == 0) return 0;
        if (want_write && (flags & VMM_FLAG_WRITE) == 0) return 0;
        if (va == last) break;
        if (va > USER_TOP - PAGE_SIZE) return 0;
        va += PAGE_SIZE;
    }
    return 1;
}

static int user_buf_mapped(struct fry_process *p, uint64_t ptr, uint64_t len) {
    return user_buf_accessible(p, ptr, len, 0);
}

static int user_buf_writable(struct fry_process *p, uint64_t ptr, uint64_t len) {
    return user_buf_accessible(p, ptr, len, 1);
}

static int copy_user_string(struct fry_process *p, uint64_t uptr, char *dst, uint32_t max) {
    if (!p || !dst || max == 0) return -1;
    if (!user_ptr_ok(uptr, 1)) return -1;
    uint32_t i = 0;
    uint64_t cur_page = ~0ULL;
    while (i + 1 < max) {
        uint64_t va = uptr + i;
        if (!user_ptr_ok(va, 1)) return -1;
        uint64_t page = va & ~(PAGE_SIZE - 1ULL);
        if (page != cur_page) {
            uint64_t flags = vmm_query_user_flags(p->cr3, va);
            if ((flags & (VMM_FLAG_PRESENT | VMM_FLAG_USER)) != (VMM_FLAG_PRESENT | VMM_FLAG_USER)) {
                return -1;
            }
            cur_page = page;
        }
        char c = *(const char *)(uintptr_t)va;
        dst[i] = c;
        if (c == 0) return 0;
        i++;
    }
    dst[max - 1] = 0;
    return 0;
}

int copyin(struct fry_process *p, uint64_t src_user, void *dst_kern, uint64_t len) {
    if (!user_buf_mapped(p, src_user, len)) return -EFAULT;
    const uint8_t *src = (const uint8_t *)(uintptr_t)src_user;
    uint8_t *dst = (uint8_t *)dst_kern;
    for (uint64_t i = 0; i < len; i++) dst[i] = src[i];
    return 0;
}

int copyout(struct fry_process *p, const void *src_kern, uint64_t dst_user, uint64_t len) {
    if (!user_buf_writable(p, dst_user, len)) return -EFAULT;
    const uint8_t *src = (const uint8_t *)src_kern;
    uint8_t *dst = (uint8_t *)(uintptr_t)dst_user;
    for (uint64_t i = 0; i < len; i++) dst[i] = src[i];
    return 0;
}

static void lx_memzero(void *p, uint64_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

static void lx_store32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 0);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void lx_store64(uint8_t *p, uint64_t v) {
    for (uint32_t i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static void lx_fill_siginfo(uint8_t siginfo[128], int sig, int code, uint64_t addr) {
    lx_memzero(siginfo, 128);
    lx_store32(siginfo + 0, (uint32_t)sig);   /* si_signo */
    lx_store32(siginfo + 4, 0);               /* si_errno */
    lx_store32(siginfo + 8, (uint32_t)code);  /* si_code */
    lx_store64(siginfo + 16, addr);           /* si_addr for fault signals */
}

static inline uint64_t lx_read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline uint64_t lx_read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline void lx_write_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

static uint64_t lx_signal_bit(int sig) {
    if (sig <= 0 || sig >= 64) return 0;
    return 1ULL << (uint32_t)(sig - 1);
}

static int lx_signal_valid(int sig) {
    return sig > 0 && sig < 64;
}

static int lx_signal_default_ignored(int sig) {
    return sig == LNX_SIGCHLD || sig == LNX_SIGURG || sig == LNX_SIGWINCH;
}

static uint64_t lx_signal_unblockable_mask(void) {
    return lx_signal_bit(LNX_SIGKILL) | lx_signal_bit(LNX_SIGSTOP);
}

static uint64_t lx_pending_handled_mask(struct fry_process *p) {
    if (!p || !p->is_linux || !p->shared) return 0;
    uint64_t mask = p->linux_sig_pending & ~p->linux_sig_blocked;
    mask &= ~lx_signal_unblockable_mask();
    for (uint32_t sig = 1; sig < 64; sig++) {
        uint64_t bit = 1ULL << (sig - 1);
        if ((mask & bit) && p->shared->linux_sigact[sig].handler)
            return bit;
    }
    return 0;
}

static int lx_lowest_signal(uint64_t mask) {
    for (int sig = 1; sig < 64; sig++) {
        if (mask & lx_signal_bit(sig)) return sig;
    }
    return 0;
}

static int lx_rseq_update_cpu(struct fry_process *p) {
    if (!p || !p->is_linux || !p->linux_rseq_addr) return 0;
    if (p->linux_rseq_len < 32) return -EINVAL;
    if (!user_buf_writable(p, p->linux_rseq_addr, p->linux_rseq_len))
        return -EFAULT;

    uint32_t cpu = p->cpu;
    uint32_t ncpu = smp_cpu_count();
    if (ncpu == 0) ncpu = 1;
    if (cpu >= ncpu) cpu = 0;

    if (copyout(p, &cpu, p->linux_rseq_addr + 0, sizeof(cpu)) != 0)
        return -EFAULT;
    if (copyout(p, &cpu, p->linux_rseq_addr + 4, sizeof(cpu)) != 0)
        return -EFAULT;
    if (p->linux_rseq_len >= 28) {
        if (copyout(p, &cpu, p->linux_rseq_addr + 20, sizeof(cpu)) != 0)
            return -EFAULT;
        if (copyout(p, &cpu, p->linux_rseq_addr + 24, sizeof(cpu)) != 0)
            return -EFAULT;
    }
    return 0;
}

static void lx_sigaction_to_user(const struct fry_linux_sigaction *in,
                                 struct lnx_user_sigaction *out) {
    out->handler = in ? in->handler : 0;
    out->flags = in ? in->flags : 0;
    out->restorer = in ? in->restorer : 0;
    out->mask = in ? in->mask : 0;
}

static void lx_sigaction_from_user(const struct lnx_user_sigaction *in,
                                   struct fry_linux_sigaction *out) {
    out->handler = in ? in->handler : 0;
    out->flags = in ? in->flags : 0;
    out->restorer = in ? in->restorer : 0;
    out->mask = in ? in->mask : 0;
}

static uint64_t lx_read_gs_slot(uint32_t off) {
    uint64_t v = 0;
    switch (off) {
    case 24: __asm__ volatile("movq %%gs:24, %0" : "=r"(v)); break;
    case 32: __asm__ volatile("movq %%gs:32, %0" : "=r"(v)); break;
    case 40: __asm__ volatile("movq %%gs:40, %0" : "=r"(v)); break;
    case 48: __asm__ volatile("movq %%gs:48, %0" : "=r"(v)); break;
    case 56: __asm__ volatile("movq %%gs:56, %0" : "=r"(v)); break;
    default: break;
    }
    return v;
}

static void lx_write_gs_slot(uint32_t off, uint64_t v) {
    switch (off) {
    case 24: __asm__ volatile("movq %0, %%gs:24" : : "r"(v) : "memory"); break;
    case 32: __asm__ volatile("movq %0, %%gs:32" : : "r"(v) : "memory"); break;
    case 40: __asm__ volatile("movq %0, %%gs:40" : : "r"(v) : "memory"); break;
    case 48: __asm__ volatile("movq %0, %%gs:48" : : "r"(v) : "memory"); break;
    case 56: __asm__ volatile("movq %0, %%gs:56" : : "r"(v) : "memory"); break;
    default: break;
    }
}

static uint64_t *lx_syscall_frame(struct fry_process *cur) {
    if (!cur || !cur->kernel_stack_top) return 0;
    return (uint64_t *)(uintptr_t)(cur->kernel_stack_top - (12ULL * sizeof(uint64_t)));
}

static void lx_fill_ucontext(struct lx_rt_sigframe *sf, const struct lx_sigrestore_regs *r,
                             uint64_t old_mask, struct fry_process *cur) {
    lx_memzero(&sf->ucontext, sizeof(sf->ucontext));
    sf->ucontext.stack.sp = cur ? cur->linux_sigalt_sp : 0;
    sf->ucontext.stack.flags = cur ? cur->linux_sigalt_flags : 0;
    sf->ucontext.stack.size = cur ? cur->linux_sigalt_size : 0;
    sf->ucontext.sigmask = old_mask;

    sf->ucontext.mcontext.gregs[0] = r->r8;
    sf->ucontext.mcontext.gregs[1] = r->r9;
    sf->ucontext.mcontext.gregs[2] = r->r10;
    sf->ucontext.mcontext.gregs[3] = r->r11;
    sf->ucontext.mcontext.gregs[4] = r->r12;
    sf->ucontext.mcontext.gregs[5] = r->r13;
    sf->ucontext.mcontext.gregs[6] = r->r14;
    sf->ucontext.mcontext.gregs[7] = r->r15;
    sf->ucontext.mcontext.gregs[8] = r->rdi;
    sf->ucontext.mcontext.gregs[9] = r->rsi;
    sf->ucontext.mcontext.gregs[10] = r->rbp;
    sf->ucontext.mcontext.gregs[11] = r->rbx;
    sf->ucontext.mcontext.gregs[12] = r->rdx;
    sf->ucontext.mcontext.gregs[13] = r->rax;
    sf->ucontext.mcontext.gregs[14] = r->rcx;
    sf->ucontext.mcontext.gregs[15] = r->rsp;
    sf->ucontext.mcontext.gregs[16] = r->rip;
    sf->ucontext.mcontext.gregs[17] = r->rflags;
}

static uint64_t lx_rt_sigaction_sys(struct fry_process *cur, uint64_t sig_u,
                                    uint64_t act_u, uint64_t old_u,
                                    uint64_t sigset_size) {
    int sig = (int)sig_u;
    if (!cur || !cur->shared) return (uint64_t)-ESRCH;
    if (!lx_signal_valid(sig) || sig == LNX_SIGKILL || sig == LNX_SIGSTOP)
        return (uint64_t)-EINVAL;
    if (sigset_size != 8 && sigset_size != 128) return (uint64_t)-EINVAL;

    if (old_u) {
        struct lnx_user_sigaction old;
        lx_sigaction_to_user(&cur->shared->linux_sigact[sig], &old);
        if (copyout(cur, &old, old_u, sizeof(old)) != 0) return (uint64_t)-EFAULT;
    }
    if (act_u) {
        struct lnx_user_sigaction act;
        if (copyin(cur, act_u, &act, sizeof(act)) != 0) return (uint64_t)-EFAULT;
        lx_sigaction_from_user(&act, &cur->shared->linux_sigact[sig]);
    }
    return 0;
}

static uint64_t lx_rt_sigprocmask_sys(struct fry_process *cur, uint64_t how_u,
                                      uint64_t set_u, uint64_t old_u,
                                      uint64_t sigset_size) {
    if (!cur) return (uint64_t)-ESRCH;
    if (sigset_size != 8 && sigset_size != 128) return (uint64_t)-EINVAL;
    uint64_t old = cur->linux_sig_blocked;
    if (old_u && copyout(cur, &old, old_u, 8) != 0) return (uint64_t)-EFAULT;
    if (!set_u) return 0;

    uint64_t set = 0;
    if (copyin(cur, set_u, &set, 8) != 0) return (uint64_t)-EFAULT;
    set &= ~lx_signal_unblockable_mask();
    switch ((int)how_u) {
    case LNX_SIG_BLOCK:
        cur->linux_sig_blocked |= set;
        break;
    case LNX_SIG_UNBLOCK:
        cur->linux_sig_blocked &= ~set;
        break;
    case LNX_SIG_SETMASK:
        cur->linux_sig_blocked = set;
        break;
    default:
        return (uint64_t)-EINVAL;
    }
    return 0;
}

static uint64_t lx_rt_sigsuspend_sys(struct fry_process *cur, uint64_t mask_u,
                                     uint64_t sigset_size) {
    if (!cur) return (uint64_t)-ESRCH;
    if (sigset_size != 8 && sigset_size != 128) return (uint64_t)-EINVAL;

    uint64_t new_mask = 0;
    if (copyin(cur, mask_u, &new_mask, 8) != 0) return (uint64_t)-EFAULT;
    cur->linux_sigsuspend_saved_mask = cur->linux_sig_blocked;
    cur->linux_sigsuspend_active = 1;
    cur->linux_sig_blocked = new_mask & ~lx_signal_unblockable_mask();

    while (!lx_pending_handled_mask(cur)) {
        sched_block(cur->pid);
        sched_yield();
    }

    return (uint64_t)-EINTR;
}

static uint64_t lx_sigaltstack_sys(struct fry_process *cur, uint64_t ss_u, uint64_t old_u) {
    if (!cur) return (uint64_t)-ESRCH;
    if (old_u) {
        struct lnx_user_stack old;
        old.sp = cur->linux_sigalt_sp;
        old.flags = cur->linux_sigalt_flags;
        old.pad = 0;
        old.size = cur->linux_sigalt_size;
        if (copyout(cur, &old, old_u, sizeof(old)) != 0) return (uint64_t)-EFAULT;
    }
    if (ss_u) {
        struct lnx_user_stack ss;
        if (copyin(cur, ss_u, &ss, sizeof(ss)) != 0) return (uint64_t)-EFAULT;
        if (ss.flags & ~LNX_SS_DISABLE) return (uint64_t)-EINVAL;
        cur->linux_sigalt_sp = ss.sp;
        cur->linux_sigalt_size = ss.size;
        cur->linux_sigalt_flags = ss.flags;
    }
    return 0;
}

static uint64_t lx_send_signal(struct fry_process *sender, struct fry_process *target, int sig) {
    if (!target || !target->shared) return (uint64_t)-ESRCH;
    if (sig == 0) return 0;
    if (!lx_signal_valid(sig)) return (uint64_t)-EINVAL;

    struct fry_linux_sigaction *act = &target->shared->linux_sigact[sig];
    if (!act->handler) {
        if (lx_signal_default_ignored(sig)) return 0;
        if (sender && (sender->pid == TB_TRACE_CLAUDE_TGID || sender->tgid == TB_TRACE_CLAUDE_TGID)) {
            kprint_serial_only("TBSIG default-exit sender=%u target=%u tgid=%u sig=%d\n",
                               sender->pid, target->pid, target->tgid, sig);
        }
        if (sender && sender->tgid == target->tgid)
            syscall_exit_current(128u + (uint32_t)sig);
        process_exit_group(target->tgid, 128u + (uint32_t)sig);
        return 0;
    }

    target->linux_sig_pending |= lx_signal_bit(sig);
    if (sender && (sender->pid == TB_TRACE_CLAUDE_TGID || sender->tgid == TB_TRACE_CLAUDE_TGID)) {
        kprint_serial_only("TBSIG queue sender=%u target=%u tgid=%u sig=%d pending=%lx blocked=%lx handler=%lx restorer=%lx\n",
                           sender->pid, target->pid, target->tgid, sig,
                           (unsigned long)target->linux_sig_pending,
                           (unsigned long)target->linux_sig_blocked,
                           (unsigned long)act->handler,
                           (unsigned long)act->restorer);
    }
    sched_wake_signal(target->pid);
    return 0;
}

/* Deliver a Unix signal from an exception context (not a syscall return).
 * The exception frame layout (from common_isr push order):
 *   [0]=rax [1]=rbx [2]=rcx [3]=rdx [4]=rbp [5]=rsi [6]=rdi
 *   [7]=r8 [8]=r9 [9]=r10 [10]=r11 [11]=r12 [12]=r13 [13]=r14 [14]=r15
 *   [15]=vector [16]=error [17]=RIP [18]=CS [19]=RFLAGS [20]=RSP [21]=SS
 *
 * Returns 1 if a signal handler was invoked (modifies the frame), 0 if not
 * (caller should kill the process). */
int lx_deliver_signal_from_exception(struct fry_process *cur, uint64_t vector,
                                     uint64_t *exc_frame) {
    if (!cur || !cur->shared || !exc_frame) return 0;

    /* Map exception vector to Unix signal */
    int sig = 0;
    switch (vector) {
    case 0:  sig = 8;  break;  /* #DE  → SIGFPE  */
    case 1:  sig = 5;  break;  /* #DB  → SIGTRAP */
    case 3:  sig = 5;  break;  /* #BP  → SIGTRAP */
    case 4:  sig = 11; break;  /* #OF  → SIGSEGV */
    case 5:  sig = 11; break;  /* #BR  → SIGSEGV */
    case 6:  sig = 4;  break;  /* #UD  → SIGILL  */
    case 7:  sig = 8;  break;  /* #NM  → SIGFPE  */
    case 11: sig = 7;  break;  /* #NP  → SIGBUS  */
    default: sig = 11; break;  /* #GP #PF #SS → SIGSEGV */
    }
    if (sig == 0) return 0;

    int si_code = 0;
    uint64_t si_addr = exc_frame[17];
    if (sig == LNX_SIGSEGV) {
        si_code = 1; /* SEGV_MAPERR */
        si_addr = lx_read_cr2();
    } else if (sig == 4) {
        si_code = 2; /* ILL_ILLOPN */
    } else if (sig == 5) {
        si_code = 1; /* TRAP_BRKPT */
    } else if (sig == 7) {
        si_code = 2; /* BUS_ADRERR */
    } else if (sig == 8) {
        si_code = 1; /* FPE_INTDIV */
    }

    struct fry_linux_sigaction *act = &cur->shared->linux_sigact[sig];
    if (!act->handler || !act->restorer) {
        /* Fallback: #BP (int3) with no SIGTRAP handler →
         * try SIGSEGV for runtimes (Bun) that only install SIGSEGV. */
        if (vector == 3 && sig == 5) {
            act = &cur->shared->linux_sigact[11]; /* SIGSEGV */
            sig = 11;
            si_code = 2; /* SEGV_ACCERR */
            /* si_addr stays exc_frame[17] — the faulting RIP */
        }
        if (!act->handler || !act->restorer) {
            if (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID) {
                kprint_serial_only("TBSIG exc-miss pid=%u tgid=%u sig=%d vec=%llu handler=%lx restorer=%lx oldrip=%lx\n",
                                   cur->pid, cur->tgid, sig,
                                   (unsigned long long)vector,
                                   (unsigned long)act->handler,
                                   (unsigned long)act->restorer,
                                   (unsigned long)exc_frame[17]);
            }
            return 0;
        }
    }

    struct lx_rt_sigframe sf;
    lx_memzero(&sf, sizeof(sf));
    sf.restorer = act->restorer;
    sf.magic = LX_SIGFRAME_MAGIC;
    sf.saved_mask = cur->linux_sigsuspend_active ?
        cur->linux_sigsuspend_saved_mask : cur->linux_sig_blocked;

    /* Read saved registers from the exception frame */
    sf.regs.rax = exc_frame[0];
    sf.regs.rbx = exc_frame[1];
    sf.regs.rcx = exc_frame[2];
    sf.regs.rdx = exc_frame[3];
    sf.regs.rsi = exc_frame[5];
    sf.regs.rdi = exc_frame[6];
    sf.regs.rbp = exc_frame[4];
    sf.regs.r8  = exc_frame[7];
    sf.regs.r9  = exc_frame[8];
    sf.regs.r10 = exc_frame[9];
    sf.regs.r11 = exc_frame[10];
    sf.regs.r12 = exc_frame[11];
    sf.regs.r13 = exc_frame[12];
    sf.regs.r14 = exc_frame[13];
    sf.regs.r15 = exc_frame[14];
    sf.regs.rip = exc_frame[17];
    sf.regs.rsp = exc_frame[20];
    sf.regs.rflags = exc_frame[19];
    lx_fill_siginfo(sf.siginfo, sig, si_code, si_addr);
    lx_fill_ucontext(&sf, &sf.regs, sf.saved_mask, cur);

    uint64_t stack_top = exc_frame[20]; /* saved user RSP */
    if ((act->flags & LNX_SA_ONSTACK) &&
        cur->linux_sigalt_sp && cur->linux_sigalt_size &&
        !(cur->linux_sigalt_flags & LNX_SS_DISABLE)) {
        stack_top = cur->linux_sigalt_sp + cur->linux_sigalt_size;
    }
    uint64_t sp = (stack_top - sizeof(sf)) & ~0xFULL;
    sp -= 8ULL;
    uint64_t saved_cr3 = lx_read_cr3();
    int switched_cr3 = (cur->cr3 && saved_cr3 != cur->cr3);
    if (switched_cr3) lx_write_cr3(cur->cr3);
    int copy_rc = copyout(cur, &sf, sp, sizeof(sf));
    if (switched_cr3) lx_write_cr3(saved_cr3);
    if (copy_rc != 0) return 0;

    cur->linux_sig_blocked |= (lx_signal_bit(sig) | (act->mask & ~lx_signal_unblockable_mask()));

    /* fry1381: preserve the interrupted thread's FPU/SSE/AVX state across the
     * handler (restored on rt_sigreturn). Without this, a handler's vector use
     * corrupts an interrupted vector op (e.g. JSC movups rehash). */
    lx_fpu_save_area(cur->sig_fpu_area);

    /* Modify the exception frame to jump to the handler */
    exc_frame[17] = act->handler;   /* RIP → handler */
    exc_frame[20] = sp;             /* RSP → sigframe */
    exc_frame[6]  = (uint64_t)sig;  /* RDI → signal number */
    exc_frame[5]  = sp + (uint64_t)__builtin_offsetof(struct lx_rt_sigframe, siginfo);
    exc_frame[3]  = sp + (uint64_t)__builtin_offsetof(struct lx_rt_sigframe, ucontext);
    exc_frame[19] = exc_frame[19] | 0x202ULL; /* ensure IF=1 */

    if (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID) {
        kprint_serial_only("TBSIG exc-deliver pid=%u tgid=%u sig=%d vec=%llu handler=%lx sp=%lx oldrip=%lx\\n",
                           cur->pid, cur->tgid, sig,
                           (unsigned long long)vector,
                           (unsigned long)act->handler,
                           (unsigned long)sp,
                           (unsigned long)sf.regs.rip);
    }
    return 1;
}

static uint64_t lx_deliver_signal_on_sysret(struct fry_process *cur, uint64_t syscall_rc) {
    if (!cur || !cur->shared) return syscall_rc;
    uint64_t bit = lx_pending_handled_mask(cur);
    if (!bit) return syscall_rc;
    int sig = lx_lowest_signal(bit);
    if (!sig) return syscall_rc;
    struct fry_linux_sigaction *act = &cur->shared->linux_sigact[sig];
    if (!act->handler || !act->restorer) return syscall_rc;

    uint64_t *frame = lx_syscall_frame(cur);
    if (!frame) return syscall_rc;

    struct lx_rt_sigframe sf;
    lx_memzero(&sf, sizeof(sf));
    sf.restorer = act->restorer;
    sf.magic = LX_SIGFRAME_MAGIC;
    sf.saved_mask = cur->linux_sig_blocked;
    sf.regs.rax = syscall_rc;
    sf.regs.rbx = lx_read_gs_slot(24);
    sf.regs.rcx = frame[LX_SYSFRAME_RIP];
    sf.regs.rdx = frame[LX_SYSFRAME_RDX];
    sf.regs.rsi = frame[LX_SYSFRAME_RSI];
    sf.regs.rdi = frame[LX_SYSFRAME_RDI];
    sf.regs.rbp = frame[LX_SYSFRAME_RBP];
    sf.regs.r8 = frame[LX_SYSFRAME_R8];
    sf.regs.r9 = frame[LX_SYSFRAME_R9];
    sf.regs.r10 = frame[LX_SYSFRAME_R10];
    sf.regs.r11 = frame[LX_SYSFRAME_RFLAGS];
    sf.regs.r12 = lx_read_gs_slot(32);
    sf.regs.r13 = lx_read_gs_slot(40);
    sf.regs.r14 = lx_read_gs_slot(48);
    sf.regs.r15 = lx_read_gs_slot(56);
    sf.regs.rip = frame[LX_SYSFRAME_RIP];
    sf.regs.rsp = frame[LX_SYSFRAME_RSP];
    sf.regs.rflags = frame[LX_SYSFRAME_RFLAGS];
    lx_fill_siginfo(sf.siginfo, sig, 0, 0);
    lx_fill_ucontext(&sf, &sf.regs, cur->linux_sig_blocked, cur);

    uint64_t stack_top = frame[LX_SYSFRAME_RSP];
    if ((act->flags & LNX_SA_ONSTACK) &&
        cur->linux_sigalt_sp && cur->linux_sigalt_size &&
        !(cur->linux_sigalt_flags & LNX_SS_DISABLE)) {
        stack_top = cur->linux_sigalt_sp + cur->linux_sigalt_size;
    }
    uint64_t sp = (stack_top - sizeof(sf)) & ~0xFULL;
    sp -= 8ULL; /* handler entry RSP must look like a call: RSP % 16 == 8 */
    if (copyout(cur, &sf, sp, sizeof(sf)) != 0) return syscall_rc;

    cur->linux_sig_pending &= ~bit;
    cur->linux_sig_blocked |= (bit | (act->mask & ~lx_signal_unblockable_mask()));
    cur->linux_sigsuspend_active = 0;

    /* fry1381: preserve FPU/SSE/AVX across the handler (restored on sigreturn). */
    lx_fpu_save_area(cur->sig_fpu_area);

    frame[LX_SYSFRAME_RIP] = act->handler;
    frame[LX_SYSFRAME_RSP] = sp;
    frame[LX_SYSFRAME_RDI] = (uint64_t)sig;
    frame[LX_SYSFRAME_RSI] = sp + (uint64_t)__builtin_offsetof(struct lx_rt_sigframe, siginfo);
    frame[LX_SYSFRAME_RDX] = sp + (uint64_t)__builtin_offsetof(struct lx_rt_sigframe, ucontext);

    if (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID) {
        kprint_serial_only("TBSIG deliver pid=%u tgid=%u sig=%d handler=%lx restorer=%lx sp=%lx oldrip=%lx oldrsp=%lx rc=%lx\n",
                           cur->pid, cur->tgid, sig,
                           (unsigned long)act->handler,
                           (unsigned long)act->restorer,
                           (unsigned long)sp,
                           (unsigned long)sf.regs.rip,
                           (unsigned long)sf.regs.rsp,
                           (unsigned long)syscall_rc);
    }
    return syscall_rc;
}

static uint64_t lx_rt_sigreturn_sys(struct fry_process *cur) {
    uint64_t *frame = lx_syscall_frame(cur);
    if (!cur || !frame) return (uint64_t)-EFAULT;
    uint64_t user_sp = frame[LX_SYSFRAME_RSP];
    struct lx_rt_sigframe sf;
    uint64_t sf_base = user_sp - 8ULL;
    if (copyin(cur, sf_base, &sf, sizeof(sf)) != 0 || sf.magic != LX_SIGFRAME_MAGIC)
        return (uint64_t)-EFAULT;

    cur->linux_sig_blocked = sf.saved_mask & ~lx_signal_unblockable_mask();
    frame[LX_SYSFRAME_RIP] = sf.regs.rip;
    frame[LX_SYSFRAME_RSP] = sf.regs.rsp;
    frame[LX_SYSFRAME_RFLAGS] = sf.regs.rflags | 0x202ULL;
    frame[LX_SYSFRAME_RDI] = sf.regs.rdi;
    frame[LX_SYSFRAME_RSI] = sf.regs.rsi;
    frame[LX_SYSFRAME_RDX] = sf.regs.rdx;
    frame[LX_SYSFRAME_R10] = sf.regs.r10;
    frame[LX_SYSFRAME_R8] = sf.regs.r8;
    frame[LX_SYSFRAME_R9] = sf.regs.r9;
    frame[LX_SYSFRAME_RBP] = sf.regs.rbp;
    lx_write_gs_slot(24, sf.regs.rbx);
    lx_write_gs_slot(32, sf.regs.r12);
    lx_write_gs_slot(40, sf.regs.r13);
    lx_write_gs_slot(48, sf.regs.r14);
    lx_write_gs_slot(56, sf.regs.r15);

    /* fry1381: restore the FPU/SSE/AVX state saved at signal delivery, so the
     * interrupted thread resumes with its vector registers intact. */
    lx_fpu_restore_area(cur->sig_fpu_area);

    if (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID) {
        kprint_serial_only("TBSIG sigreturn pid=%u tgid=%u rip=%lx rsp=%lx rax=%lx mask=%lx\n",
                           cur->pid, cur->tgid,
                           (unsigned long)sf.regs.rip,
                           (unsigned long)sf.regs.rsp,
                           (unsigned long)sf.regs.rax,
                           (unsigned long)cur->linux_sig_blocked);
    }
    return sf.regs.rax;
}

static int streq_lit(const char *a, const char *b) {
    if (!a || !b) return 0;
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static void boot_diag_stage(uint64_t stage) {
    struct fry_handoff *handoff = g_handoff;
    if (!TATER_BOOT_VISUAL_DEBUG) return;
    if (!handoff) return;
    if (!handoff->fb_base || !handoff->fb_width || !handoff->fb_height || !handoff->fb_stride) return;

    uint64_t x0 = stage * 20ULL;
    if (x0 >= handoff->fb_width) return;

    uint64_t mw = 12ULL;
    uint64_t mh = 12ULL;
    uint64_t remain_w = handoff->fb_width - x0;
    if (remain_w < mw) mw = remain_w;
    if (handoff->fb_height < mh) mh = handoff->fb_height;

    /* Use VMM_FB_BASE (0xFFFFFFFFB0000000) — mapped during vmm_init, lives in
       PML4[511] which is copied to every user address space.  Safe to access
       from syscall context under user CR3. */
    volatile uint32_t *fb = (volatile uint32_t *)0xFFFFFFFFB0000000ULL;
    for (uint64_t y = 0; y < mh; y++) {
        uint64_t row = y * handoff->fb_stride + x0;
        for (uint64_t x = 0; x < mw; x++) {
            fb[row + x] = 0x00F0F0F0u;
        }
    }
}

/* Colored diagnostic square — same layout as boot_diag_stage but with
   a caller-chosen color.  Row 0 = white boot markers, row 1 (y offset 20)
   = spawn-attempt markers so they're visually separate. */
static void boot_diag_color(uint64_t col, uint64_t row_idx, uint32_t color) {
    struct fry_handoff *handoff = g_handoff;
    if (!TATER_BOOT_VISUAL_DEBUG) return;
    if (!handoff) return;
    if (!handoff->fb_base || !handoff->fb_width || !handoff->fb_height || !handoff->fb_stride) return;

    uint64_t x0 = col * 20ULL;
    uint64_t y0 = row_idx * 20ULL;
    if (x0 + 12 > handoff->fb_width) return;
    if (y0 + 12 > handoff->fb_height) return;

    volatile uint32_t *fb = (volatile uint32_t *)0xFFFFFFFFB0000000ULL;
    for (uint64_t y = 0; y < 12; y++) {
        uint64_t row = (y0 + y) * handoff->fb_stride + x0;
        for (uint64_t x = 0; x < 12; x++) {
            fb[row + x] = color;
        }
    }
}

/* Spawn-failure classifier for bare-metal debugging:
   blue   = open/path lookup failure
   magenta= corrupt/invalid container or ELF
   cyan   = memory / address-space / process allocation failure
   orange = short read / bounds / translation style failure
   white  = other / unexpected */
static uint32_t boot_diag_spawn_error_color(int rc) {
    switch (rc) {
        case ELF_LOAD_ERR_OPEN:
            return 0x000080FFu;
        case ELF_LOAD_ERR_BAD_MAGIC:
        case ELF_LOAD_ERR_BAD_CRC:
        case ELF_LOAD_ERR_BAD_ELF_HEADER:
        case ELF_LOAD_ERR_BAD_ELF_MAGIC:
            return 0x00FF00FFu;
        case ELF_LOAD_ERR_NOMEM:
        case ELF_LOAD_ERR_VMM_SPACE:
        case ELF_LOAD_ERR_SEG_ALLOC:
        case ELF_LOAD_ERR_SEG_TRANSLATE:
        case ELF_LOAD_ERR_STACK_ALLOC:
        case PROCESS_LAUNCH_ERR_CREATE_USER:
            return 0x0000FFFFu;
        case ELF_LOAD_ERR_SHORT_HEADER:
        case ELF_LOAD_ERR_READ:
        case ELF_LOAD_ERR_BOUNDS:
            return 0x00FF8000u;
        default:
            return 0x00FFFFFFu;
    }
}

static const char *path_basename_lit(const char *path) {
    const char *base = path;
    if (!path) return "";
    while (*path) {
        if (*path == '/') base = path + 1;
        path++;
    }
    return base;
}

static int process_name_is_init(const char *name) {
    return streq_lit(path_basename_lit(name), "INIT.FRY");
}

static int process_name_is_gui(const char *name) {
    return streq_lit(path_basename_lit(name), "GUI.FRY");
}

static void note_user_boot_progress(struct fry_process *cur) {
    if (!cur || cur->is_kernel) return;
    if (!g_first_user_syscall_seen) {
        g_first_user_syscall_seen = 1;
        boot_diag_stage(36);
        if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_FIRST_USER_SYSCALL\n");
    }
    if (!g_first_init_syscall_seen && process_name_is_init(cur->name)) {
        g_first_init_syscall_seen = 1;
        boot_diag_stage(37);
        if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_INIT_SYSCALL\n");
    }
}

static void sbrk_rollback_pages(struct fry_process *p, uint64_t va_start, uint64_t va_end) {
    if (!p || !p->cr3) return;
    for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
        uint64_t pa = vmm_virt_to_phys_user(p->cr3, va);
        if (!pa) continue;
        vmm_unmap_user(p->cr3, va);
        pmm_free_page(pa & 0x000FFFFFFFFFF000ULL);
    }
}

static int gui_process_running(void) {
    if (g_gui_slot_hint >= 0 && g_gui_slot_hint < (int32_t)PROC_MAX) {
        struct fry_process *hp = &procs[g_gui_slot_hint];
        if (hp->pid == g_gui_pid_hint &&
            hp->state != PROC_UNUSED &&
            hp->state != PROC_DEAD &&
            process_name_is_gui(hp->name)) {
            return 1;
        }
    }
    g_gui_slot_hint = -1;
    g_gui_pid_hint = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_UNUSED || procs[i].state == PROC_DEAD) continue;
        if (process_name_is_gui(procs[i].name)) {
            g_gui_slot_hint = (int32_t)i;
            g_gui_pid_hint = procs[i].pid;
            return 1;
        }
    }
    return 0;
}

static int readdir_cb(const char *name, void *c) {
    struct readdir_ctx *rc = (struct readdir_ctx *)c;
    uint32_t i = 0;
    while (name[i]) {
        if (rc->pos + 1 >= rc->len) return 1;
        rc->buf[rc->pos++] = name[i++];
    }
    if (rc->pos + 1 < rc->len) {
        rc->buf[rc->pos++] = '\n';
    }
    if (rc->pos + 1 >= rc->len) return 1;
    return 0;
}
static int readdir_ex_cb(const char *name, uint64_t size, uint32_t attr, void *c) {
    struct readdir_ex_ctx *rc = (struct readdir_ex_ctx *)c;
    uint32_t name_len = 0;
    while (name[name_len]) name_len++;
    uint32_t max_name = 0xFFFF - (uint32_t)sizeof(struct fry_dirent_hdr) - 1;
    if (name_len > max_name) name_len = max_name;
    uint32_t rec_len = (uint32_t)sizeof(struct fry_dirent_hdr) + name_len + 1;
    if (rec_len < sizeof(struct fry_dirent_hdr)) return 1;
    if (rc->pos + rec_len > rc->len) return 1;
    struct fry_dirent_hdr *h = (struct fry_dirent_hdr *)(rc->buf + rc->pos);
    h->rec_len = (uint16_t)rec_len;
    h->name_len = (uint16_t)name_len;
    h->attr = attr;
    h->size = size;
    uint8_t *dst = (uint8_t *)(h + 1);
    for (uint32_t i = 0; i < name_len; i++) dst[i] = (uint8_t)name[i];
    dst[name_len] = 0;
    rc->pos += rec_len;
    if (rc->pos >= rc->len) return 1;
    return 0;
}

static uint16_t lx_dirent64_reclen(uint32_t name_len) {
    uint32_t reclen = 19u + name_len + 1u; /* ino64 + off64 + reclen + type + NUL */
    reclen = (reclen + 7u) & ~7u;
    return (uint16_t)reclen;
}

static int lx_getdents64_cb(const char *name, uint64_t size, uint32_t attr, void *c) {
    (void)size;
    struct lx_getdents64_ctx *ctx = (struct lx_getdents64_ctx *)c;
    uint32_t name_len = 0;
    uint32_t index;
    uint16_t reclen;
    uint8_t *dst;

    if (!ctx || !name) return 1;
    if (ctx->skip > 0) {
        ctx->skip--;
        return 0;
    }

    while (name[name_len]) name_len++;
    if (name_len > 255u) name_len = 255u;
    reclen = lx_dirent64_reclen(name_len);
    if (ctx->pos + reclen > ctx->len) {
        ctx->overflow = 1;
        return 1;
    }

    dst = ctx->buf + ctx->pos;
    index = ctx->base_skip + ctx->emitted + 1u;
    *(uint64_t *)(uintptr_t)(dst + 0) = (uint64_t)index;              /* d_ino */
    *(int64_t *)(uintptr_t)(dst + 8) = (int64_t)(index + 1u);         /* d_off */
    *(uint16_t *)(uintptr_t)(dst + 16) = reclen;                     /* d_reclen */
    dst[18] = (attr & 0x10u) ? 4u : 8u;                              /* DT_DIR/DT_REG */
    for (uint32_t i = 0; i < name_len; i++) dst[19u + i] = (uint8_t)name[i];
    dst[19u + name_len] = 0;
    for (uint32_t i = 20u + name_len; i < reclen; i++) dst[i] = 0;

    ctx->pos += reclen;
    ctx->emitted++;
    return 0;
}

__attribute__((noreturn))
static void syscall_exit_group_finish(uint32_t tgid, uint32_t code) {
    process_exit_group(tgid, code);
    sched_yield();
    for (;;) {
        __asm__ volatile("hlt");
    }
}

__attribute__((noreturn))
static void syscall_thread_exit_finish(uint32_t tid, uint32_t code) {
    if (process_thread_exit(tid, code) < 0) {
        proc_free(tid);
    }
    sched_yield();
    for (;;) {
        __asm__ volatile("hlt");
    }
}

__attribute__((noreturn))
static void syscall_exit_on_safe_stack(uint32_t id, uint32_t code,
                                       void (*finish)(uint32_t, uint32_t),
                                       uint32_t cpu_slot) {
    uint64_t kcr3 = vmm_get_kernel_pml4_phys();
    if (cpu_slot >= SYS_EXIT_STACK_CPUS) cpu_slot = 0;
    uint64_t exit_sp = ((uint64_t)(uintptr_t)
        &g_sys_exit_stacks[cpu_slot][SYS_EXIT_STACK_BYTES]) & ~0xFULL;

    __asm__ volatile(
        "cli\n"
        "mov %0, %%cr3\n"
        "mov %1, %%rsp\n"
        "mov %2, %%edi\n"
        "mov %3, %%esi\n"
        "call *%4\n"
        :
        : "r"(kcr3),
          "r"(exit_sp),
          "r"(id),
          "r"(code),
          "r"(finish)
        : "rdi", "rsi", "memory");

    __builtin_unreachable();
}

__attribute__((noreturn))
static void syscall_exit_current(uint32_t code) {
    struct fry_process *cur = proc_current();
    if (!cur) {
        for (;;) __asm__ volatile("hlt");
    }
    syscall_exit_on_safe_stack(process_group_id(cur), code,
                               syscall_exit_group_finish, cur->cpu);
}

__attribute__((noreturn))
static void syscall_thread_exit_current(uint32_t code) {
    struct fry_process *cur = proc_current();
    if (!cur) {
        for (;;) __asm__ volatile("hlt");
    }
    if (cur->pid == cur->tgid) {
        syscall_exit_current(code);
    }
    if (cur->is_linux && cur->linux_clear_child_tid) {
        uint64_t private_key = 0;
        uint64_t shared_key = 0;
        uint64_t tid_addr = cur->linux_clear_child_tid;
        if (user_buf_writable(cur, tid_addr, sizeof(uint32_t)) &&
            futex_key_for_user_word_ex(cur, tid_addr, 1, &private_key) == 0) {
            *(uint32_t *)(uintptr_t)tid_addr = 0;
            uint32_t woke_private = sched_wake_futex(private_key, 0x7fffffffU,
                                                     FUTEX_BITSET_MATCH_ANY, 0);
            (void)woke_private;
            if (futex_key_for_user_word_ex(cur, tid_addr, 0, &shared_key) == 0) {
                sched_wake_futex(shared_key, 0x7fffffffU,
                                 FUTEX_BITSET_MATCH_ANY, 0);
            }
        }
        cur->linux_clear_child_tid = 0;
    }
    syscall_exit_on_safe_stack(cur->pid, code,
                               syscall_thread_exit_finish, cur->cpu);
}

/*
 * Syscall number allocation policy (Phase 0 ABI discipline):
 *
 *   0 -  31  Core POSIX-like syscalls (file I/O, process, memory).
 *             These numbers are STABLE and must never be renumbered.
 *  32 -  36  Filesystem diagnostics / extended readdir.
 *  37 -  50  Driver-specific syscalls (WiFi, Ethernet, etc.).
 *             Numbers in this range may be reclaimed when a driver
 *             is removed; the number itself must not be reused for
 *             an unrelated purpose within the same major release.
 *  51 -  63  Reserved for future driver syscalls.
 *  52 -  54  VM syscalls (mmap/munmap/mprotect) — STABLE.
 *  64 -  67  User thread syscalls — STABLE.
 *  68 -  71  Synchronization/TLS syscalls — STABLE.
 *  72 - 127  Reserved for future POSIX-compat expansion.
 * 128 - 255  Available for experimental / debug syscalls.
 * 256+       Undefined; returns -ENOSYS.
 *
 * Error convention: every syscall returns 0 on success or a positive
 * value (e.g. fd, pid, byte count), and a negative errno on failure.
 * Pointer-returning syscalls (mmap) encode the errno as (void *)-errno
 * and userspace checks with FRY_IS_ERR().
 */
#include <tatertos/syscall.h>


struct fry_fb_info {
    uint64_t phys;
    uint64_t size;
    uint64_t user_base;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};

#define SHM_USER_BASE 0x20000000000ULL
#define SHM_SLOT_STRIDE 0x10000000ULL
struct shm_region {
    uint64_t phys_base;
    uint32_t page_count;
    uint32_t owner_pid;
    uint32_t map_count;
    uint32_t mapped_pids[PROC_MAX]; /* slot-indexed pid guard against slot reuse */
    int used;
};
static struct shm_region shm_regions[FRY_SHM_MAX];

static int shm_proc_slot_by_pid(uint32_t pid) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid &&
            procs[i].state != PROC_UNUSED &&
            procs[i].state != PROC_DEAD) {
            return (int)i;
        }
    }
    return -1;
}

static void shm_untrack_slot(struct shm_region *r, int slot, uint32_t pid) {
    if (!r) return;
    if (slot < 0 || slot >= (int)PROC_MAX) return;
    if (r->mapped_pids[slot] != pid) return;
    r->mapped_pids[slot] = 0;
    if (r->map_count > 0) r->map_count--;
}

static void shm_release_pages(struct shm_region *r) {
    if (!r || !r->phys_base || !r->page_count) return;
    pmm_free_pages(r->phys_base, r->page_count);
}

static void shm_reset_region(struct shm_region *r) {
    if (!r) return;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        r->mapped_pids[i] = 0;
    }
    r->phys_base = 0;
    r->page_count = 0;
    r->owner_pid = 0;
    r->map_count = 0;
    r->used = 0;
}

static void shm_unmap_from_all_processes(int id, struct shm_region *r) {
    if (!r) return;
    uint64_t virt_base = SHM_USER_BASE + (uint64_t)id * SHM_SLOT_STRIDE;
    uint64_t kernel_cr3 = vmm_get_kernel_pml4_phys();
    for (uint32_t slot = 0; slot < PROC_MAX; slot++) {
        uint32_t pid = r->mapped_pids[slot];
        if (!pid) continue;
        if (procs[slot].pid == pid &&
            procs[slot].state != PROC_UNUSED &&
            procs[slot].state != PROC_DEAD &&
            procs[slot].cr3 &&
            procs[slot].cr3 != kernel_cr3) {
            for (uint32_t p = 0; p < r->page_count; p++) {
                vmm_unmap_user(procs[slot].cr3, virt_base + (uint64_t)p * PAGE_SIZE);
            }
        }
        shm_untrack_slot(r, (int)slot, pid);
    }
}

static void shm_destroy_region(int id) {
    if (id < 0 || id >= FRY_SHM_MAX) return;
    struct shm_region *r = &shm_regions[id];
    if (!r->used) return;
    shm_unmap_from_all_processes(id, r);
    shm_release_pages(r);
    shm_reset_region(r);
}

void syscall_shm_process_exit(uint32_t pid) {
    int slot = shm_proc_slot_by_pid(pid);
    for (int id = 0; id < FRY_SHM_MAX; id++) {
        struct shm_region *r = &shm_regions[id];
        if (!r->used) continue;
        if (r->owner_pid == pid) {
            shm_destroy_region(id);
            continue;
        }
        if (slot >= 0) {
            shm_untrack_slot(r, slot, pid);
        }
    }
}

#define VM_BACKING_NONE UINT32_MAX

struct vm_shared_object {
    uint64_t *pages;
    uint32_t *page_refs;
    uint32_t page_count;
    uint8_t used;
    uint8_t _pad[3];
};

static struct vm_shared_object vm_shared_objects[FRY_VM_SHARED_MAX];

static int vm_region_alloc_slot(struct fry_process *p) {
    if (!p || !proc_shared_state(p)) return -1;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        if (!PROC_VMREGS(p)[i].used) {
            /* fry1390: confirm the old 256-slot ceiling was being exceeded
             * (the aliasing trigger). Log once when we first use slot >= 256. */
            if (i >= 256 && (p->pid == 3u || p->tgid == 3u)) {
                static int g_vmreg_over256_logged = 0;
                if (!g_vmreg_over256_logged) {
                    g_vmreg_over256_logged = 1;
                    kprint_serial_only("TBVMREG exceeded old-256 ceiling: now using slot=%d (was the aliasing bug)\n", i);
                }
            }
            return i;
        }
    }
    if (p->pid == 3u || p->tgid == 3u)
        kprint_serial_only("TBVMREG TABLE FULL (%d) pid=%u -> mapping would be UNTRACKED\n",
                           PROC_VMREG_MAX, p->pid);
    return -1;
}

static int vm_region_alloc_slots(struct fry_process *p, int needed, int *slot1, int *slot2) {
    if (slot1) *slot1 = -1;
    if (slot2) *slot2 = -1;
    if (needed <= 0) return 0;
    if (!p || !proc_shared_state(p)) return -1;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        if (PROC_VMREGS(p)[i].used) continue;
        if (slot1 && *slot1 < 0) {
            *slot1 = i;
            needed--;
        } else if (slot2 && *slot2 < 0) {
            *slot2 = i;
            needed--;
        }
        if (needed == 0) return 0;
    }
    return -1;
}

static void vm_region_clear(struct fry_vm_region *r) {
    if (!r) return;
    r->base = 0;
    r->length = 0;
    r->prot = 0;
    r->flags = 0;
    r->backing_id = VM_BACKING_NONE;
    r->backing_page_start = 0;
    r->kind = FRY_VM_REGION_NONE;
    r->used = 0;
    r->committed = 0;
}

static void vm_region_fill(struct fry_vm_region *r, uint64_t base, uint64_t length,
                           uint32_t prot, uint32_t flags, uint16_t kind,
                           uint32_t backing_id, uint32_t backing_page_start,
                           uint8_t committed) {
    if (!r) return;
    r->base = base;
    r->length = length;
    r->prot = prot;
    r->flags = flags;
    r->backing_id = backing_id;
    r->backing_page_start = backing_page_start;
    r->kind = kind;
    r->used = 1;
    r->committed = committed;
}

static uint32_t vm_region_page_start_at(const struct fry_vm_region *r, uint64_t base) {
    if (!r || base < r->base) return 0;
    return r->backing_page_start + (uint32_t)((base - r->base) / PAGE_SIZE);
}

static void vm_region_fill_span_from_parent(struct fry_vm_region *dst,
                                            const struct fry_vm_region *src,
                                            uint64_t base, uint64_t length,
                                            uint32_t prot, uint32_t flags,
                                            uint8_t committed) {
    vm_region_fill(dst, base, length, prot, flags, src->kind, src->backing_id,
                   vm_region_page_start_at(src, base), committed);
}

static int vm_regions_can_merge(const struct fry_vm_region *a,
                                const struct fry_vm_region *b) {
    if (!a || !b || !a->used || !b->used) return 0;
    if (a->base + a->length != b->base) return 0;
    if (a->prot != b->prot || a->flags != b->flags) return 0;
    if (a->kind != b->kind || a->committed != b->committed) return 0;
    if (a->kind == FRY_VM_REGION_ANON_SHARED) {
        if (a->backing_id != b->backing_id) return 0;
        if (a->backing_page_start + (uint32_t)(a->length / PAGE_SIZE) != b->backing_page_start) {
            return 0;
        }
    }
    return 1;
}

static void vm_region_merge_neighbors(struct fry_process *p) {
    if (!p || !proc_shared_state(p)) return;
    int merged = 1;
    while (merged) {
        merged = 0;
        for (int i = 0; i < PROC_VMREG_MAX && !merged; i++) {
            if (!PROC_VMREGS(p)[i].used) continue;
            for (int j = 0; j < PROC_VMREG_MAX; j++) {
                if (i == j || !PROC_VMREGS(p)[j].used) continue;
                struct fry_vm_region *a = &PROC_VMREGS(p)[i];
                struct fry_vm_region *b = &PROC_VMREGS(p)[j];
                if (b->base < a->base) {
                    struct fry_vm_region *tmp = a;
                    a = b;
                    b = tmp;
                }
                if (!vm_regions_can_merge(a, b)) continue;
                a->length += b->length;
                vm_region_clear(b);
                merged = 1;
                break;
            }
        }
    }
}

static int vm_region_overlaps(const struct fry_vm_region *r, uint64_t base, uint64_t end) {
    if (!r || !r->used) return 0;
    uint64_t r_end = r->base + r->length;
    if (r_end < r->base) return 0;
    return !(end <= r->base || base >= r_end);
}

static int vm_any_region_overlap(const struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p || !proc_shared_state_const(p) || length == 0) return 0;
    if (base + length < base) return 1;
    uint64_t end = base + length;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        if (vm_region_overlaps(&PROC_VMREGS_CONST(p)[i], base, end)) return 1;
    }
    return 0;
}

static int vm_region_find_containing(const struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p || !proc_shared_state_const(p) || length == 0) return -1;
    if (base + length < base) return -1;
    uint64_t end = base + length;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        if (!PROC_VMREGS_CONST(p)[i].used) continue;
        uint64_t r_base = PROC_VMREGS_CONST(p)[i].base;
        uint64_t r_end = r_base + PROC_VMREGS_CONST(p)[i].length;
        if (r_end < r_base) continue;
        if (base >= r_base && end <= r_end) return i;
    }
    return -1;
}

static int vm_region_collect_covering_slots(const struct fry_process *p,
                                            uint64_t base, uint64_t length,
                                            int *slots, int max_slots) {
    if (!p || !proc_shared_state_const(p) || !slots || max_slots <= 0 || length == 0) return -1;
    if (base + length < base) return -1;

    uint64_t cursor = base;
    uint64_t end = base + length;
    int count = 0;

    while (cursor < end) {
        int slot = -1;
        uint64_t slot_end = 0;
        for (int i = 0; i < PROC_VMREG_MAX; i++) {
            const struct fry_vm_region *r = &PROC_VMREGS_CONST(p)[i];
            if (!r->used) continue;
            uint64_t r_end = r->base + r->length;
            if (r_end < r->base) continue;
            if (cursor < r->base || cursor >= r_end) continue;
            slot = i;
            slot_end = r_end;
            break;
        }
        if (slot < 0) return -1;
        if (count >= max_slots) return -1;
        slots[count++] = slot;
        cursor = (slot_end < end) ? slot_end : end;
    }

    return count;
}

static int vm_prot_supported(uint32_t prot) {
    if ((prot & ~(FRY_PROT_READ | FRY_PROT_WRITE | FRY_PROT_EXEC)) != 0) return 0;
    if (prot == 0) return 1;
    if ((prot & FRY_PROT_READ) == 0) return 0;
    if ((prot & (FRY_PROT_WRITE | FRY_PROT_EXEC)) == (FRY_PROT_WRITE | FRY_PROT_EXEC)) return 0;
    return 1;
}

static int vm_flags_supported(uint32_t flags) {
    if ((flags & ~(FRY_MAP_SHARED | FRY_MAP_PRIVATE | FRY_MAP_FIXED |
                   FRY_MAP_ANON | FRY_MAP_FILE | FRY_MAP_RESERVE |
                   FRY_MAP_GUARD)) != 0) return 0;
    if ((flags & FRY_MAP_GUARD) != 0) {
        /* guard: must be private+anon, nothing else */
        if ((flags & ~(FRY_MAP_GUARD | FRY_MAP_PRIVATE | FRY_MAP_ANON | FRY_MAP_FIXED)) != 0) return 0;
        if ((flags & (FRY_MAP_PRIVATE | FRY_MAP_ANON)) != (FRY_MAP_PRIVATE | FRY_MAP_ANON)) return 0;
        return 1;
    }
    if ((flags & (FRY_MAP_SHARED | FRY_MAP_PRIVATE)) == 0) return 0;
    if ((flags & (FRY_MAP_SHARED | FRY_MAP_PRIVATE)) == (FRY_MAP_SHARED | FRY_MAP_PRIVATE)) return 0;
    if ((flags & (FRY_MAP_ANON | FRY_MAP_FILE)) == 0) return 0;
    if ((flags & (FRY_MAP_ANON | FRY_MAP_FILE)) == (FRY_MAP_ANON | FRY_MAP_FILE)) return 0;
    if ((flags & FRY_MAP_FILE) != 0 && (flags & FRY_MAP_SHARED) != 0) return 0;
    if ((flags & FRY_MAP_RESERVE) != 0 && (flags & FRY_MAP_FILE) != 0) return 0;
    if ((flags & FRY_MAP_RESERVE) != 0 && (flags & FRY_MAP_SHARED) != 0) return 0;
    return 1;
}

static uint64_t vm_prot_to_pte_flags(uint32_t prot) {
    uint64_t flags = 0;
    if (prot != 0) flags |= VMM_FLAG_USER;
    if (prot & FRY_PROT_WRITE) flags |= VMM_FLAG_WRITE;
    if ((prot & FRY_PROT_EXEC) == 0) flags |= VMM_FLAG_NO_EXECUTE;
    return flags;
}

static int vm_range_mapped(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p || length == 0) return 0;
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        if (vmm_virt_to_phys_user(p->cr3, va) == 0) return 0;
    }
    return 1;
}

static int vm_range_available(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p || !p->cr3 || length == 0) return 0;
    if (base < VM_USER_BASE) return 0;
    if (base + length < base) return 0;
    if (base + length > VM_USER_LIMIT) return 0;
    if (vm_any_region_overlap(p, base, length)) return 0;
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        if (vmm_virt_to_phys_user(p->cr3, va) != 0) return 0;
    }
    return 1;
}

static uint64_t vm_find_free_range(struct fry_process *p, uint64_t length) {
    if (!p || length == 0) return 0;
    if (VM_USER_LIMIT <= VM_USER_BASE) return 0;
    if (length > VM_USER_LIMIT - VM_USER_BASE) return 0;

    uint64_t max_start = VM_USER_LIMIT - length;
    for (uint64_t base = VM_USER_BASE; base <= max_start; base += PAGE_SIZE) {
        if (vm_range_available(p, base, length)) return base;
    }
    return 0;
}

static void vm_zero_page(uint64_t phys) {
    uint8_t *dst = (uint8_t *)(uintptr_t)vmm_phys_to_virt(phys);
    for (uint64_t i = 0; i < PAGE_SIZE; i++) dst[i] = 0;
}

/* fry1389: log any vm op (for the Claude tgid) whose [base,base+len) range
 * covers the corrupt CodeBlock page (g_tb_target_page). Catches a kernel-side
 * zero/unmap/release of the live page that the user-VA DR watchpoint misses. */
static void tb_log_vm_touch(const char *op, struct fry_process *p,
                            uint64_t base, uint64_t len) {
    if (!g_tb_target_page || !p) return;
    if (p->pid != 3u && p->tgid != 3u) return;
    uint64_t end = base + len;
    if (end < base) return;
    if (g_tb_target_page < base || g_tb_target_page >= end) return;
    kprint_serial_only("TBVMTOUCH op=%s pid=%u base=0x%llx len=0x%llx end=0x%llx target=0x%llx\n",
                       op, p->pid, (unsigned long long)base,
                       (unsigned long long)len, (unsigned long long)end,
                       (unsigned long long)g_tb_target_page);
}

static int vm_madvise_dontneed(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p || !p->cr3 || length == 0) return -EINVAL;
    if (base + length < base) return -EINVAL;
    tb_log_vm_touch("dontneed", p, base, length);
    uint64_t end = base + length;

    if (proc_shared_state_const(p)) {
        int touched_region = 0;
        for (int i = 0; i < PROC_VMREG_MAX; i++) {
            const struct fry_vm_region *r = &PROC_VMREGS_CONST(p)[i];
            if (!r->used) continue;
            uint64_t r_end = r->base + r->length;
            if (r_end < r->base) continue;
            if (end <= r->base || base >= r_end) continue;
            touched_region = 1;
            /* fry1386: RE-APPLIED the fry1374 fix (the fry1378 revert was wrong).
             * Do NOT gate on r->committed. Sparse NORESERVE gigacage regions
             * register committed=0 yet have pages demand-faulted in. JSC issues
             * MADV_DONTNEED over such a region (e.g. the 1.5MB atom/compact table
             * at 0x6fbbff3a9000) to release it and RELIES on the Linux contract
             * that the next read returns ZERO. With the committed guard, DONTNEED
             * was a no-op for the sparse cage, so STALE data (an old 32-byte-record
             * structure's pointers) persisted; JSC reused the region as a 6-byte
             * packed table and read the stale bytes -> non-canonical pointer ->
             * #GP at 0x4C17293 (proven: DONTNEED at serial L3560 immediately
             * followed by the #GP at L3562). The fry1378 reasoning blamed this for
             * the hash-sentinel hang, but that hang was the AVX-clobber bug fixed
             * separately by -mgeneral-regs-only (fry1379). The per-page pa==0 check
             * below already skips genuinely uncommitted pages. */
            if (r->kind != FRY_VM_REGION_ANON_PRIVATE &&
                r->kind != FRY_VM_REGION_ANON_SHARED) {
                continue;
            }
            uint64_t from = (base > r->base) ? base : r->base;
            uint64_t to = (end < r_end) ? end : r_end;
            for (uint64_t va = from; va < to; va += PAGE_SIZE) {
                uint64_t pa = vmm_virt_to_phys_user(p->cr3, va) & 0x000FFFFFFFFFF000ULL;
                if (!pa) continue;
                vm_zero_page(pa);
            }
        }
        if (touched_region) return 0;
    }

    for (uint64_t va = base; va < end; va += PAGE_SIZE) {
        uint64_t pa = vmm_virt_to_phys_user(p->cr3, va) & 0x000FFFFFFFFFF000ULL;
        if (!pa) continue;
        vm_zero_page(pa);
    }
    return 0;
}

static void vm_unmap_pages_only(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p) return;
    tb_log_vm_touch("unmap", p, base, length);
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        if (!vmm_virt_to_phys_user(p->cr3, va)) continue;
        vmm_unmap_user(p->cr3, va);
    }
}

static void vm_release_private_pages(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p) return;
    tb_log_vm_touch("release", p, base, length);
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        uint64_t pa = vmm_virt_to_phys_user(p->cr3, va);
        if (!pa) continue;
        vmm_unmap_user(p->cr3, va);
        pmm_free_page(pa & 0x000FFFFFFFFFF000ULL);
    }
}

static int vm_commit_private_pages(struct fry_process *p, uint64_t base,
                                   uint64_t length, uint32_t prot) {
    if (!p || !p->cr3 || length == 0) return -1;
    uint64_t pte_flags = vm_prot_to_pte_flags(prot);
    uint64_t mapped = 0;
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) {
            vm_release_private_pages(p, base, mapped);
            return -1;
        }
        vmm_map_user(p->cr3, va, pa, pte_flags);
        uint64_t verify = vmm_virt_to_phys_user(p->cr3, va);
        if ((verify & 0x000FFFFFFFFFF000ULL) != pa) {
            kprint_serial_only("VM COMMIT FAIL: va=0x%llx pa=0x%llx verify=0x%llx cr3=0x%llx flags=0x%llx\n",
                   (unsigned long long)va, (unsigned long long)pa,
                   (unsigned long long)verify, (unsigned long long)p->cr3,
                   (unsigned long long)pte_flags);
            vmm_unmap_user(p->cr3, va);
            pmm_free_page(pa);
            vm_release_private_pages(p, base, mapped);
            return -1;
        }
        /* Diagnostic: log first page of each mmap commit for debugging */
        if (mapped == 0) {
            kprint_serial_only("VM COMMIT OK: pid=%u va=0x%llx pa=0x%llx cr3=0x%llx pte=0x%llx pages=%llu\n",
                   (unsigned)(p->pid), (unsigned long long)va, (unsigned long long)pa,
                   (unsigned long long)p->cr3, (unsigned long long)pte_flags,
                   (unsigned long long)(length / PAGE_SIZE));
        }
        vm_zero_page(pa);
        mapped += PAGE_SIZE;
    }
    return 0;
}

static void vm_shared_reset(struct vm_shared_object *obj) {
    if (!obj) return;
    obj->pages = 0;
    obj->page_refs = 0;
    obj->page_count = 0;
    obj->used = 0;
}

static int vm_shared_alloc_slot(void) {
    for (int i = 0; i < FRY_VM_SHARED_MAX; i++) {
        if (!vm_shared_objects[i].used) return i;
    }
    return -1;
}

static int vm_shared_all_released(const struct vm_shared_object *obj) {
    if (!obj || !obj->used) return 1;
    for (uint32_t i = 0; i < obj->page_count; i++) {
        if (obj->page_refs[i] != 0) return 0;
    }
    return 1;
}

static void vm_shared_destroy(int id) {
    if (id < 0 || id >= FRY_VM_SHARED_MAX) return;
    struct vm_shared_object *obj = &vm_shared_objects[id];
    if (!obj->used) return;
    if (obj->pages) {
        for (uint32_t i = 0; i < obj->page_count; i++) {
            if (obj->pages[i]) {
                pmm_free_page(obj->pages[i]);
                obj->pages[i] = 0;
            }
        }
    }
    if (obj->pages) kfree(obj->pages);
    if (obj->page_refs) kfree(obj->page_refs);
    vm_shared_reset(obj);
}

static int vm_shared_create(uint32_t page_count) {
    int id = vm_shared_alloc_slot();
    if (id < 0) return -1;

    struct vm_shared_object *obj = &vm_shared_objects[id];
    vm_shared_reset(obj);
    obj->pages = (uint64_t *)kmalloc((uint64_t)page_count * sizeof(uint64_t));
    if (!obj->pages) return -1;
    obj->page_refs = (uint32_t *)kmalloc((uint64_t)page_count * sizeof(uint32_t));
    if (!obj->page_refs) {
        kfree(obj->pages);
        obj->pages = 0;
        return -1;
    }
    obj->page_count = page_count;
    obj->used = 1;
    for (uint32_t i = 0; i < page_count; i++) {
        obj->pages[i] = 0;
        obj->page_refs[i] = 0;
    }
    for (uint32_t i = 0; i < page_count; i++) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) {
            vm_shared_destroy(id);
            return -1;
        }
        vm_zero_page(pa);
        obj->pages[i] = pa;
        obj->page_refs[i] = 1;
    }
    return id;
}

static void vm_shared_release_range(uint32_t id, uint32_t page_start,
                                    uint32_t page_count) {
    if (id >= FRY_VM_SHARED_MAX) return;
    struct vm_shared_object *obj = &vm_shared_objects[id];
    if (!obj->used || page_start > obj->page_count) return;
    if (page_count > obj->page_count - page_start) return;

    for (uint32_t i = 0; i < page_count; i++) {
        uint32_t idx = page_start + i;
        if (obj->page_refs[idx] == 0) continue;
        obj->page_refs[idx]--;
        if (obj->page_refs[idx] == 0 && obj->pages[idx]) {
            pmm_free_page(obj->pages[idx]);
            obj->pages[idx] = 0;
        }
    }
    if (vm_shared_all_released(obj)) vm_shared_destroy((int)id);
}

static int vm_map_shared_pages(struct fry_process *p, uint64_t base, uint64_t length,
                               uint32_t prot, uint32_t backing_id,
                               uint32_t page_start) {
    if (!p || backing_id >= FRY_VM_SHARED_MAX) return -1;
    struct vm_shared_object *obj = &vm_shared_objects[backing_id];
    if (!obj->used) return -1;
    uint32_t page_count = (uint32_t)(length / PAGE_SIZE);
    if (page_start > obj->page_count) return -1;
    if (page_count > obj->page_count - page_start) return -1;

    uint64_t pte_flags = vm_prot_to_pte_flags(prot) | VMM_FLAG_NOFREE;
    uint64_t mapped = 0;
    for (uint32_t i = 0; i < page_count; i++) {
        uint64_t pa = obj->pages[page_start + i];
        if (!pa) {
            vm_unmap_pages_only(p, base, mapped);
            return -1;
        }
        uint64_t va = base + (uint64_t)i * PAGE_SIZE;
        vmm_map_user(p->cr3, va, pa, pte_flags);
        if ((vmm_virt_to_phys_user(p->cr3, va) & 0x000FFFFFFFFFF000ULL) != pa) {
            vmm_unmap_user(p->cr3, va);
            vm_unmap_pages_only(p, base, mapped);
            return -1;
        }
        mapped += PAGE_SIZE;
    }
    return 0;
}

static int vm_map_anon_private_region(struct fry_process *p, uint64_t base, uint64_t length,
                                      uint32_t prot, uint32_t flags) {
    if (!p || !p->cr3 || length == 0) return -1;
    if (!vm_range_available(p, base, length)) return -1;

    if ((flags & FRY_MAP_RESERVE) == 0) {
        if (vm_commit_private_pages(p, base, length, prot) != 0) return -1;
    }

    int slot = vm_region_alloc_slot(p);
    if (slot < 0) {
        if ((flags & FRY_MAP_RESERVE) == 0) vm_release_private_pages(p, base, length);
        return -1;
    }

    vm_region_fill(&PROC_VMREGS(p)[slot], base, length,
                   ((flags & FRY_MAP_RESERVE) != 0) ? 0 : prot,
                   flags, FRY_VM_REGION_ANON_PRIVATE, VM_BACKING_NONE, 0,
                   ((flags & FRY_MAP_RESERVE) == 0));
    return 0;
}

static int vm_map_guard_region(struct fry_process *p, uint64_t base, uint64_t length,
                               uint32_t flags) {
    if (!p || !p->cr3 || length == 0) return -1;
    if (!vm_range_available(p, base, length)) return -1;
    int slot = vm_region_alloc_slot(p);
    if (slot < 0) return -1;
    vm_region_fill(&PROC_VMREGS(p)[slot], base, length, 0, flags,
                   FRY_VM_REGION_GUARD, VM_BACKING_NONE, 0, 0);
    return 0;
}

static int vm_map_anon_shared_region(struct fry_process *p, uint64_t base, uint64_t length,
                                     uint32_t prot, uint32_t flags) {
    if (!p || !p->cr3 || length == 0) return -1;
    if (!vm_range_available(p, base, length)) return -1;

    int backing_id = vm_shared_create((uint32_t)(length / PAGE_SIZE));
    if (backing_id < 0) return -1;
    if (vm_map_shared_pages(p, base, length, prot, (uint32_t)backing_id, 0) != 0) {
        vm_shared_destroy(backing_id);
        return -1;
    }

    int slot = vm_region_alloc_slot(p);
    if (slot < 0) {
        vm_unmap_pages_only(p, base, length);
        vm_shared_destroy(backing_id);
        return -1;
    }

    vm_region_fill(&PROC_VMREGS(p)[slot], base, length, prot, flags,
                   FRY_VM_REGION_ANON_SHARED, (uint32_t)backing_id, 0, 1);
    return 0;
}

static int vm_map_file_region(struct fry_process *p, uint64_t base, uint64_t length,
                              uint32_t prot, uint32_t flags, int fd) {
    if (!p || !proc_shared_state(p) || fd < 3 || fd >= FRY_FD_MAX || !PROC_FD_PTRS(p)[fd]) {
        return -1;
    }
    if (!vm_range_available(p, base, length)) return -1;
    if (vm_commit_private_pages(p, base, length, prot) != 0) return -1;

    struct vfs_file file = *(struct vfs_file *)PROC_FD_PTRS(p)[fd];
    uint64_t remaining = file.size;
    if (remaining > length) remaining = length;
    uint64_t va = base;

    while (remaining > 0) {
        uint64_t pa = vmm_virt_to_phys_user(p->cr3, va);
        if (!pa) {
            vm_release_private_pages(p, base, length);
            return -1;
        }
        uint32_t chunk = (remaining > PAGE_SIZE) ? (uint32_t)PAGE_SIZE : (uint32_t)remaining;
        int rd = vfs_read(&file, (void *)(uintptr_t)vmm_phys_to_virt(pa), chunk);
        if (rd < 0 || (uint32_t)rd != chunk) {
            vm_release_private_pages(p, base, length);
            return -1;
        }
        remaining -= chunk;
        va += PAGE_SIZE;
    }

    int slot = vm_region_alloc_slot(p);
    if (slot < 0) {
        vm_release_private_pages(p, base, length);
        return -1;
    }

    vm_region_fill(&PROC_VMREGS(p)[slot], base, length, prot, flags,
                   FRY_VM_REGION_FILE_PRIVATE, VM_BACKING_NONE, 0, 1);
    return 0;
}

static int vm_map_memfd_region(struct fry_process *p, uint64_t base, uint64_t length,
                                uint32_t prot, uint32_t flags, struct memfd_cb *mf) {
    if (!p || !mf || !mf->used) return -1;
    if (!vm_range_available(p, base, length)) return -1;

    uint64_t remaining = mf->size;
    if (remaining > length) remaining = length;
    uint64_t va = base;
    uint32_t page_idx = 0;

    while (remaining > 0 && page_idx < mf->page_count) {
        uint64_t pa = mf->pages[page_idx];
        if (pa == 0) break;

        uint32_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (prot & FRY_PROT_WRITE) vmm_flags |= VMM_FLAG_WRITE;
        if (!(prot & FRY_PROT_EXEC)) vmm_flags |= VMM_FLAG_NO_EXECUTE;

        vmm_map_user(p->cr3, va, pa, vmm_flags);
        va += PAGE_SIZE;
        page_idx++;
        if (remaining > PAGE_SIZE) remaining -= PAGE_SIZE;
        else remaining = 0;
    }

    int slot = vm_region_alloc_slot(p);
    if (slot < 0) return -1;

    vm_region_fill(&PROC_VMREGS(p)[slot], base, length, prot, flags,
                   FRY_VM_REGION_ANON_SHARED, VM_BACKING_NONE, 0, 1);
    return 0;
}

static int vm_release_region_pages(struct fry_process *p,
                                   const struct fry_vm_region *r,
                                   uint64_t base, uint64_t length) {
    if (!p || !r || length == 0) return -1;
    if (!r->committed) return 0;
    if (!vm_range_mapped(p, base, length)) return -1;

    if (r->kind == FRY_VM_REGION_ANON_SHARED) {
        vm_unmap_pages_only(p, base, length);
        vm_shared_release_range(r->backing_id,
                                vm_region_page_start_at(r, base),
                                (uint32_t)(length / PAGE_SIZE));
        return 0;
    }

    if (r->kind == FRY_VM_REGION_ANON_PRIVATE ||
        r->kind == FRY_VM_REGION_FILE_PRIVATE) {
        vm_release_private_pages(p, base, length);
        return 0;
    }

    return -1;
}

static int vm_unmap_region_range(struct fry_process *p, uint64_t base, uint64_t length) {
    uint64_t end = base + length;
    int slots[PROC_VMREG_MAX];
    int count = vm_region_collect_covering_slots(p, base, length, slots, PROC_VMREG_MAX);
    if (count < 0) return -1;

    int spill_slot = -1;
    if (count == 1) {
        const struct fry_vm_region *r = &PROC_VMREGS(p)[slots[0]];
        uint64_t r_end = r->base + r->length;
        if (base > r->base && end < r_end) {
            if (vm_region_alloc_slots(p, 1, &spill_slot, 0) != 0) return -1;
        }
    }

    for (int i = 0; i < count; i++) {
        const struct fry_vm_region *r = &PROC_VMREGS(p)[slots[i]];
        uint64_t overlap_base = (base > r->base) ? base : r->base;
        uint64_t r_end = r->base + r->length;
        uint64_t overlap_end = (end < r_end) ? end : r_end;
        if (overlap_end <= overlap_base) return -1;
        if (vm_release_region_pages(p, r, overlap_base, overlap_end - overlap_base) != 0) {
            return -1;
        }
    }

    if (count == 1) {
        int slot = slots[0];
        struct fry_vm_region r = PROC_VMREGS(p)[slot];
        uint64_t r_end = r.base + r.length;
        if (base == r.base && end == r_end) {
            vm_region_clear(&PROC_VMREGS(p)[slot]);
        } else if (base == r.base) {
            vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot], &r, end, r_end - end,
                                            r.prot, r.flags, r.committed);
        } else if (end == r_end) {
            vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot], &r, r.base,
                                            base - r.base, r.prot, r.flags, r.committed);
        } else {
            vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot], &r, r.base,
                                            base - r.base, r.prot, r.flags, r.committed);
            vm_region_fill_span_from_parent(&PROC_VMREGS(p)[spill_slot], &r, end,
                                            r_end - end, r.prot, r.flags, r.committed);
        }
    } else {
        int first_slot = slots[0];
        int last_slot = slots[count - 1];
        struct fry_vm_region first = PROC_VMREGS(p)[first_slot];
        struct fry_vm_region last = PROC_VMREGS(p)[last_slot];
        uint64_t last_end = last.base + last.length;

        if (base > first.base) {
            vm_region_fill_span_from_parent(&PROC_VMREGS(p)[first_slot], &first, first.base,
                                            base - first.base, first.prot, first.flags,
                                            first.committed);
        } else {
            vm_region_clear(&PROC_VMREGS(p)[first_slot]);
        }

        for (int i = 1; i < count - 1; i++) {
            vm_region_clear(&PROC_VMREGS(p)[slots[i]]);
        }

        if (end < last_end) {
            vm_region_fill_span_from_parent(&PROC_VMREGS(p)[last_slot], &last, end,
                                            last_end - end, last.prot, last.flags,
                                            last.committed);
        } else {
            vm_region_clear(&PROC_VMREGS(p)[last_slot]);
        }
    }
    vm_region_merge_neighbors(p);
    return 0;
}

static int vm_mprotect_region_range(struct fry_process *p, uint64_t base, uint64_t length,
                                    uint32_t prot) {
    int slot = vm_region_find_containing(p, base, length);
    if (slot < 0) return -1;
    struct fry_vm_region r = PROC_VMREGS(p)[slot];
    uint64_t end = base + length;
    uint64_t r_end = r.base + r.length;
    int slot1 = -1;
    int slot2 = -1;
    if (base == r.base && end == r_end) {
    } else if (base == r.base || end == r_end) {
        if (vm_region_alloc_slots(p, 1, &slot1, 0) != 0) return -1;
    } else {
        if (vm_region_alloc_slots(p, 2, &slot1, &slot2) != 0) return -1;
    }

    if (!r.committed) {
        if (r.kind != FRY_VM_REGION_ANON_PRIVATE && r.kind != FRY_VM_REGION_GUARD) return -1;
        if (prot == 0) {
            if (base == r.base && end == r_end) {
                vm_region_fill(&PROC_VMREGS(p)[slot], r.base, r.length, 0, r.flags,
                               r.kind, r.backing_id, r.backing_page_start, 0);
            } else if (base == r.base) {
                vm_region_fill(&PROC_VMREGS(p)[slot], base, length, 0, r.flags,
                               r.kind, r.backing_id, vm_region_page_start_at(&r, base), 0);
                vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot1], &r, end, r_end - end,
                                                r.prot, r.flags, 0);
            } else if (end == r_end) {
                vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot], &r, r.base,
                                                base - r.base, r.prot, r.flags, 0);
                vm_region_fill(&PROC_VMREGS(p)[slot1], base, length, 0, r.flags,
                               r.kind, r.backing_id, vm_region_page_start_at(&r, base), 0);
            } else {
                vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot], &r, r.base,
                                                base - r.base, r.prot, r.flags, 0);
                vm_region_fill(&PROC_VMREGS(p)[slot1], base, length, 0, r.flags,
                               r.kind, r.backing_id, vm_region_page_start_at(&r, base), 0);
                vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot2], &r, end, r_end - end,
                                                r.prot, r.flags, 0);
            }
            vm_region_merge_neighbors(p);
            return 0;
        }
        if (r.kind == FRY_VM_REGION_GUARD) return -1;
        if (vm_commit_private_pages(p, base, length, prot) != 0) return -1;

        uint32_t committed_flags = r.flags & ~FRY_MAP_RESERVE;
        if (base == r.base && end == r_end) {
            vm_region_fill(&PROC_VMREGS(p)[slot], base, length, prot, committed_flags,
                           r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
        } else if (base == r.base) {
            vm_region_fill(&PROC_VMREGS(p)[slot], base, length, prot, committed_flags,
                           r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
            vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot1], &r, end, r_end - end,
                                            0, r.flags, 0);
        } else if (end == r_end) {
            vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot], &r, r.base,
                                            base - r.base, 0, r.flags, 0);
            vm_region_fill(&PROC_VMREGS(p)[slot1], base, length, prot, committed_flags,
                           r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
        } else {
            vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot], &r, r.base,
                                            base - r.base, 0, r.flags, 0);
            vm_region_fill(&PROC_VMREGS(p)[slot1], base, length, prot, committed_flags,
                           r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
            vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot2], &r, end, r_end - end,
                                            0, r.flags, 0);
        }
        vm_region_merge_neighbors(p);
        return 0;
    }

    if (!vm_range_mapped(p, base, length)) return -1;

    uint64_t pte_flags = vm_prot_to_pte_flags(prot);
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        if (vmm_protect_user(p->cr3, va, pte_flags) != 0) return -1;
    }

    if (base == r.base && end == r_end) {
        vm_region_fill(&PROC_VMREGS(p)[slot], r.base, r.length, prot, r.flags,
                       r.kind, r.backing_id, r.backing_page_start, 1);
    } else if (base == r.base) {
        vm_region_fill(&PROC_VMREGS(p)[slot], base, length, prot, r.flags,
                       r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
        vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot1], &r, end, r_end - end,
                                        r.prot, r.flags, 1);
    } else if (end == r_end) {
        vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot], &r, r.base, base - r.base,
                                        r.prot, r.flags, 1);
        vm_region_fill(&PROC_VMREGS(p)[slot1], base, length, prot, r.flags,
                       r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
    } else {
        vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot], &r, r.base, base - r.base,
                                        r.prot, r.flags, 1);
        vm_region_fill(&PROC_VMREGS(p)[slot1], base, length, prot, r.flags,
                       r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
        vm_region_fill_span_from_parent(&PROC_VMREGS(p)[slot2], &r, end, r_end - end,
                                        r.prot, r.flags, 1);
    }
    vm_region_merge_neighbors(p);
    return 0;
}

void syscall_vm_process_exit(struct fry_process *p) {
    if (!p || !proc_shared_state(p)) return;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        struct fry_vm_region *r = &PROC_VMREGS(p)[i];
        if (!r->used) continue;
        if (r->committed && r->kind == FRY_VM_REGION_ANON_SHARED) {
            vm_shared_release_range(r->backing_id, r->backing_page_start,
                                    (uint32_t)(r->length / PAGE_SIZE));
        }
        vm_region_clear(r);
    }
}

static uint64_t sys_now_ms(void) {
    uint64_t freq = hpet_get_freq_hz();
    if (freq == 0) return 0;
    return (hpet_read_counter() * 1000ULL) / freq;
}

static int futex_key_for_user_word_ex(struct fry_process *p, uint64_t uaddr,
                                      int private_key, uint64_t *key_out) {
    uint64_t key;
    if (!p || !key_out) return -EINVAL;
    if ((uaddr & 3ULL) != 0) return -EINVAL;
    if (!user_buf_mapped(p, uaddr, sizeof(uint32_t))) return -EFAULT;
    if (private_key) {
        struct fry_process_shared *shared = proc_shared_state(p);
        if (!shared) return -ESRCH;
        /* Linux FUTEX_PRIVATE_FLAG keys are scoped to one process address
         * space. Keep the key collision-free for all in-tree process IDs:
         *   bit 63      = private futex namespace
         *   bits 62..48 = shared owner pid
         *   bits 47..2  = canonical user virtual word address
         */
        *key_out = 0x8000000000000000ULL |
                   (((uint64_t)shared->owner_pid & 0x7FFFULL) << 48) |
                   (uaddr & 0x0000FFFFFFFFFFFCULL);
        return 0;
    }
    key = vmm_virt_to_phys_user(p->cr3, uaddr);
    if (!key) return -EFAULT;
    *key_out = key;
    return 0;
}

static int futex_wait_begin_ex(struct fry_process *cur, uint64_t uaddr,
                               uint32_t expected, int has_timeout,
                               uint64_t timeout_ms, uint32_t bitset,
                               int private_key) {
    uint64_t key;
    volatile const uint32_t *word;
    uint64_t now_ms;
    uint64_t wake_time_ms;
    int rc;
    if (!cur) return -ESRCH;
    if (bitset == 0) return -EINVAL;
    rc = futex_key_for_user_word_ex(cur, uaddr, private_key, &key);
    if (rc < 0) return rc;

    word = (volatile const uint32_t *)(uintptr_t)uaddr;
    if (uaddr == TB_JSC_FUTEX_UADDR) {
        kprint_serial_only("TBFUTEX jsc-wait-enter pid=%u tgid=%u key=%lx expected=%u current=%u timeout=%u timeout_ms=%llu bitset=%x\n",
                           cur->pid, cur->tgid, (unsigned long)key,
                           expected, (unsigned)*word, has_timeout ? 1u : 0u,
                           (unsigned long long)timeout_ms, bitset);
    }
    if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
        kprint_serial_only("TBFUTEX wait-begin pid=%u tgid=%u uaddr=%lx key=%lx private=%u expected=%u actual=%u timeout=%u timeout_ms=%llu bitset=%x\n",
                           cur->pid, cur->tgid, (unsigned long)uaddr,
                           (unsigned long)key, private_key ? 1u : 0u,
                           expected, (unsigned)*word, has_timeout ? 1u : 0u,
                           (unsigned long long)timeout_ms, bitset);
    }
    now_ms = sys_now_ms();
    if (!has_timeout) {
        wake_time_ms = UINT64_MAX;
    } else if (timeout_ms == 0) {
        wake_time_ms = now_ms;
    } else {
        if (now_ms > UINT64_MAX - timeout_ms) {
            wake_time_ms = UINT64_MAX - 1ULL;
        } else {
            wake_time_ms = now_ms + timeout_ms;
        }
    }

    rc = sched_block_futex(cur->pid, word, expected, key, wake_time_ms, bitset);
    if (rc < 0) {
        if (uaddr == TB_JSC_FUTEX_UADDR) {
            kprint_serial_only("TBFUTEX jsc-wait-block-fail pid=%u tgid=%u key=%lx rc=%d expected=%u current=%u\n",
                               cur->pid, cur->tgid, (unsigned long)key,
                               rc, expected, (unsigned)*word);
        }
        if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
            kprint_serial_only("TBFUTEX wait-block-fail pid=%u tgid=%u uaddr=%lx key=%lx rc=%d actual=%u expected=%u\n",
                               cur->pid, cur->tgid, (unsigned long)uaddr,
                               (unsigned long)key, rc, (unsigned)*word, expected);
        }
        return rc;
    }
    if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
        kprint_serial_only("TBFUTEX wait-yield pid=%u tgid=%u uaddr=%lx key=%lx now=%llu wake_time=%llu delta=%llu\n",
                           cur->pid, cur->tgid, (unsigned long)uaddr,
                           (unsigned long)key, (unsigned long long)now_ms,
                           (unsigned long long)wake_time_ms,
                           (unsigned long long)((wake_time_ms == UINT64_MAX ||
                                                 wake_time_ms < now_ms) ? 0 : wake_time_ms - now_ms));
    }
    sched_yield();

    cur = proc_current();
    if (!cur) return -ESRCH;
    rc = cur->wait_result;
    if (uaddr == TB_JSC_FUTEX_UADDR) {
        uint64_t ret_now = sys_now_ms();
        kprint_serial_only("TBFUTEX jsc-wait-return pid=%u tgid=%u key=%lx rc=%d elapsed=%llu current=%u\n",
                           cur->pid, cur->tgid, (unsigned long)key, rc,
                           (unsigned long long)((ret_now >= now_ms) ? ret_now - now_ms : 0),
                           (unsigned)*word);
    }
    if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
        uint64_t ret_now = sys_now_ms();
        kprint_serial_only("TBFUTEX wait-return pid=%u tgid=%u uaddr=%lx key=%lx rc=%d elapsed=%llu actual=%u\n",
                           cur->pid, cur->tgid, (unsigned long)uaddr,
                           (unsigned long)key, rc,
                           (unsigned long long)((ret_now >= now_ms) ? ret_now - now_ms : 0),
                           (unsigned)*word);
    }
    cur->wait_result = 0;
    cur->wait_futex_key = 0;
    cur->wait_futex_bitset = 0;
    cur->wake_time_ms = 0;
    return rc;
}

static int futex_wait_begin(struct fry_process *cur, uint64_t uaddr,
                            uint32_t expected, uint64_t timeout_ms) {
    return futex_wait_begin_ex(cur, uaddr, expected, timeout_ms != 0,
                               timeout_ms, FUTEX_BITSET_MATCH_ANY, 0);
}

static int futex_wake_waiters_bitset(struct fry_process *cur, uint64_t uaddr,
                                     uint32_t max_wake, uint32_t bitset,
                                     int private_key) {
    uint64_t key;
    int rc;
    if (!cur || max_wake == 0) return 0;
    if (bitset == 0) return -EINVAL;
    rc = futex_key_for_user_word_ex(cur, uaddr, private_key, &key);
    if (rc < 0) return rc;
    if (uaddr == TB_JSC_FUTEX_UADDR) {
        volatile const uint32_t *word = (volatile const uint32_t *)(uintptr_t)uaddr;
        kprint_serial_only("TBFUTEX jsc-wake-call pid=%u tgid=%u key=%lx current=%u max=%u bitset=%x\n",
                           cur->pid, cur->tgid, (unsigned long)key,
                           (unsigned)*word, max_wake, bitset);
    }
    if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
        kprint_serial_only("TBFUTEX wake-call pid=%u tgid=%u uaddr=%lx key=%lx private=%u max=%u bitset=%x\n",
                           cur->pid, cur->tgid, (unsigned long)uaddr,
                           (unsigned long)key, private_key ? 1u : 0u,
                           max_wake, bitset);
    }
    rc = (int)sched_wake_futex(key, max_wake, bitset, 0);
    if (uaddr == TB_JSC_FUTEX_UADDR) {
        volatile const uint32_t *word = (volatile const uint32_t *)(uintptr_t)uaddr;
        kprint_serial_only("TBFUTEX jsc-wake-ret pid=%u tgid=%u key=%lx rc=%d current=%u\n",
                           cur->pid, cur->tgid, (unsigned long)key, rc,
                           (unsigned)*word);
    }
    if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
        kprint_serial_only("TBFUTEX wake-ret pid=%u tgid=%u uaddr=%lx key=%lx rc=%d\n",
                           cur->pid, cur->tgid, (unsigned long)uaddr,
                           (unsigned long)key, rc);
    }
    return rc;
}

static int futex_wake_waiters(struct fry_process *cur, uint64_t uaddr, uint32_t max_wake) {
    return futex_wake_waiters_bitset(cur, uaddr, max_wake,
                                     FUTEX_BITSET_MATCH_ANY, 0);
}

static uint64_t linux_affinity_mask_bytes(uint32_t ncpu) {
    uint64_t bytes;
    if (ncpu == 0) ncpu = 1;
    bytes = ((uint64_t)ncpu + 7ULL) / 8ULL;
    bytes = (bytes + 7ULL) & ~7ULL;
    if (bytes == 0) bytes = 8;
    if (bytes > 128) bytes = 128;
    return bytes;
}

static int futex_requeue_waiters(struct fry_process *cur, uint64_t uaddr,
                                 uint32_t wake_count, uint64_t uaddr2,
                                 uint32_t requeue_count, int do_cmp,
                                 uint32_t cmp_expected, int private_key) {
    uint64_t key1, key2;
    int rc;
    if (!cur) return -ESRCH;
    rc = futex_key_for_user_word_ex(cur, uaddr, private_key, &key1);
    if (rc < 0) return rc;
    rc = futex_key_for_user_word_ex(cur, uaddr2, private_key, &key2);
    if (rc < 0) return rc;
    if (do_cmp && *(volatile const uint32_t *)(uintptr_t)uaddr != cmp_expected)
        return -EAGAIN;
    uint32_t woke = sched_wake_futex(key1, wake_count,
                                     FUTEX_BITSET_MATCH_ANY, 0);
    uint32_t moved = sched_requeue_futex(key1, key2, requeue_count);
    return (int)(woke + moved);
}

static int futex_op_compare(int32_t oldval, int32_t cmpval, uint32_t cmp) {
    switch (cmp) {
    case FUTEX_OP_CMP_EQ: return oldval == cmpval;
    case FUTEX_OP_CMP_NE: return oldval != cmpval;
    case FUTEX_OP_CMP_LT: return oldval <  cmpval;
    case FUTEX_OP_CMP_LE: return oldval <= cmpval;
    case FUTEX_OP_CMP_GT: return oldval >  cmpval;
    case FUTEX_OP_CMP_GE: return oldval >= cmpval;
    default: return 0;
    }
}

static int futex_wake_op(struct fry_process *cur, uint64_t uaddr1,
                         uint32_t wake1, uint64_t uaddr2,
                         uint32_t wake2, uint32_t encoded_op,
                         int private_key) {
    uint64_t key1, key2;
    int rc;
    uint32_t op = (encoded_op >> 28) & 0xfU;
    uint32_t cmp = (encoded_op >> 24) & 0xfU;
    uint32_t oparg = (encoded_op >> 12) & 0xfffU;
    uint32_t cmparg = encoded_op & 0xfffU;
    volatile uint32_t *word2;
    uint32_t old;
    if (!cur) return -ESRCH;
    rc = futex_key_for_user_word_ex(cur, uaddr1, private_key, &key1);
    if (rc < 0) return rc;
    rc = futex_key_for_user_word_ex(cur, uaddr2, private_key, &key2);
    if (rc < 0) return rc;
    if (op & FUTEX_OP_OPARG_SHIFT) oparg = 1U << (oparg & 31U);
    op &= 0x7U;
    word2 = (volatile uint32_t *)(uintptr_t)uaddr2;
    switch (op) {
    case FUTEX_OP_SET:
        old = __sync_lock_test_and_set(word2, oparg);
        break;
    case FUTEX_OP_ADD:
        old = __sync_fetch_and_add(word2, oparg);
        break;
    case FUTEX_OP_OR:
        old = __sync_fetch_and_or(word2, oparg);
        break;
    case FUTEX_OP_ANDN:
        old = __sync_fetch_and_and(word2, ~oparg);
        break;
    case FUTEX_OP_XOR:
        old = __sync_fetch_and_xor(word2, oparg);
        break;
    default:
        return -ENOSYS;
    }
    uint32_t woke = sched_wake_futex(key1, wake1,
                                     FUTEX_BITSET_MATCH_ANY, 0);
    if (futex_op_compare((int32_t)old, (int32_t)cmparg, cmp)) {
        woke += sched_wake_futex(key2, wake2,
                                 FUTEX_BITSET_MATCH_ANY, 0);
    }
    return (int)woke;
}

/* =====================================================================
 * Tater Bridge — Linux-compatible syscall translation.
 *
 * Routed to from syscall_dispatch() when the current process has is_linux
 * set. `num` here is a LINUX x86_64 syscall number (a separate namespace
 * from native TaterTOS numbers). Reuses this file's validated helpers
 * (user_buf_mapped, syscall_exit_current, sys_now_ms, the page mappers).
 *
 * Phase 1 scope: enough mem/info/process syscalls to run a STATIC Linux
 * ELF (write/exit + memory + identity + time + randomness). File I/O,
 * threads/futex, and signals are honestly stubbed (-ENOSYS / no-op) and
 * land in later phases; the default case logs every unimplemented number
 * so expanding toward glibc/CPython/Bun is a measured iterate-to-green.
 * =================================================================== */

#define LX_FRAME_MASK 0x000FFFFFFFFFF000ULL
#define LX_PG 4096ULL

static int tb_trace_is_claude(struct fry_process *cur) {
    if (!cur) return 0;
    if (cur->name[0]) {
        for (uint32_t i = 0; cur->name[i]; i++) {
            uint32_t j = 0;
            const char *needle = "RCLAUDE";
            while (needle[j] && cur->name[i + j] == needle[j]) j++;
            if (!needle[j]) {
                g_tb_trace_claude_tgid = cur->tgid ? cur->tgid : cur->pid;
                return 1;
            }
            j = 0;
            needle = "claude";
            while (needle[j] && cur->name[i + j] == needle[j]) j++;
            if (!needle[j]) {
                g_tb_trace_claude_tgid = cur->tgid ? cur->tgid : cur->pid;
                return 1;
            }
        }
    }
    return cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID;
}

static const char *tb_trace_syscall_name(uint64_t num) {
    switch (num) {
    case LNX_read: return "read";
    case LNX_write: return "write";
    case LNX_readv: return "readv";
    case LNX_readlink: return "readlink";
    case LNX_access: return "access";
    case LNX_newfstatat: return "newfstatat";
    case LNX_pread64: return "pread64";
    case LNX_open: return "open";
    case LNX_openat: return "openat";
    case LNX_close: return "close";
    case LNX_mmap: return "mmap";
    case LNX_mprotect: return "mprotect";
    case LNX_munmap: return "munmap";
    case LNX_madvise: return "madvise";
    case LNX_rt_sigaction: return "rt_sigaction";
    case LNX_rt_sigprocmask: return "rt_sigprocmask";
    case LNX_rt_sigreturn: return "rt_sigreturn";
    case LNX_rt_sigsuspend: return "rt_sigsuspend";
    case LNX_sigaltstack: return "sigaltstack";
    case LNX_clone: return "clone";
    case LNX_clone3: return "clone3";
    case LNX_set_robust_list: return "set_robust_list";
    case LNX_rseq: return "rseq";
    case LNX_sched_setscheduler: return "sched_setscheduler";
    case LNX_futex: return "futex";
    case LNX_nanosleep: return "nanosleep";
    case LNX_clock_nanosleep: return "clock_nanosleep";
    case LNX_epoll_create: return "epoll_create";
    case LNX_epoll_create1: return "epoll_create1";
    case LNX_epoll_ctl: return "epoll_ctl";
    case LNX_epoll_wait: return "epoll_wait";
    case LNX_epoll_pwait: return "epoll_pwait";
    case LNX_eventfd: return "eventfd";
    case LNX_eventfd2: return "eventfd2";
    case LNX_timerfd_create: return "timerfd_create";
    case LNX_timerfd_settime: return "timerfd_settime";
    case LNX_timerfd_gettime: return "timerfd_gettime";
    case LNX_signalfd: return "signalfd";
    case LNX_signalfd4: return "signalfd4";
    case LNX_getdents64: return "getdents64";
    case LNX_getrandom: return "getrandom";
    case LNX_time: return "time";
    case LNX_fcntl: return "fcntl";
    case LNX_ioctl: return "ioctl";
    case LNX_socket: return "socket";
    case LNX_wait4: return "wait4";
    case LNX_kill: return "kill";
    case LNX_tkill: return "tkill";
    case LNX_tgkill: return "tgkill";
    case LNX_exit: return "exit";
    case LNX_exit_group: return "exit_group";
    default: return "unknown";
    }
}

static int tb_trace_selected_syscall(uint64_t num) {
    switch (num) {
    case LNX_read:
    case LNX_write:
    case LNX_readv:
    case LNX_readlink:
    case LNX_pread64:
    case LNX_open:
    case LNX_openat:
    case LNX_close:
    case LNX_mmap:
    case LNX_mprotect:
    case LNX_munmap:
    case LNX_madvise:
    case LNX_rt_sigaction:
    case LNX_rt_sigprocmask:
    case LNX_rt_sigreturn:
    case LNX_rt_sigsuspend:
    case LNX_sigaltstack:
    case LNX_clone:
    case LNX_clone3:
    case LNX_set_robust_list:
    case LNX_rseq:
    case LNX_futex:
    case LNX_nanosleep:
    case LNX_clock_nanosleep:
    case LNX_epoll_create:
    case LNX_epoll_create1:
    case LNX_epoll_ctl:
    case LNX_epoll_wait:
    case LNX_epoll_pwait:
    case LNX_eventfd:
    case LNX_eventfd2:
    case LNX_timerfd_create:
    case LNX_timerfd_settime:
    case LNX_timerfd_gettime:
    case LNX_signalfd:
    case LNX_signalfd4:
    case LNX_getdents64:
    case LNX_getrandom:
    case LNX_time:
    case LNX_fcntl:
    case LNX_ioctl:
    case LNX_socket:
    case LNX_wait4:
    case LNX_kill:
    case LNX_tkill:
    case LNX_tgkill:
    case LNX_exit:
    case LNX_exit_group:
    case LNX_access:
    case LNX_newfstatat:
        return 1;
    default:
        return 0;
    }
}

static int tb_trace_is_errno(uint64_t rc) {
    int64_t s = (int64_t)rc;
    return s < 0 && s >= -4096;
}

static void lx_setfield(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < 64) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Map one zeroed user page with the given VMM flags; returns 0 or -ENOMEM. */
static int lx_map_one(uint64_t cr3, uint64_t va, uint64_t flags) {
    uint64_t pa = pmm_alloc_page();
    if (!pa) return -ENOMEM;
    vmm_map_user(cr3, va, pa, flags);
    uint8_t *kv = (uint8_t *)(uintptr_t)vmm_phys_to_virt(pa);
    for (int i = 0; i < 4096; i++) kv[i] = 0;
    return 0;
}

/* Demand-paging for anonymous mmap regions (e.g. JSC's MAP_NORESERVE
 * gigacage: a huge virtual range reserved with no backing pages). The first
 * access to an un-backed page traps to the #PF handler, which calls this to
 * commit ONE zero page with the region's protection. This is how a multi-GB
 * reservation runs on a machine with only a few GB of RAM — physical pages
 * are allocated only for the pages actually touched.
 * Returns 1 if the fault was satisfied (retry the instruction), 0 to fall
 * through to the normal USER FAULT kill. Runs in #PF/exception context with
 * the faulting process's cr3 active. */
int lx_try_demand_page(struct fry_process *p, uint64_t fault_va, uint64_t err) {
    if (!p) return 0;
    /* Only NOT-present faults are demand-pageable. err bit0 set => the page
     * was present and this is a real protection violation -> let it kill. */
    if (err & 0x1ULL) return 0;
    uint64_t va = fault_va & ~0xFFFULL;
    if (g_tb_target_page && va == (g_tb_target_page & ~0xFFFULL))
        tb_log_vm_touch("demand-refault", p, va, LX_PG);
    int slot = vm_region_find_containing(p, va, LX_PG);
    if (slot < 0) return 0;
    struct fry_vm_region *r = &PROC_VMREGS(p)[slot];
    if (!r->used) return 0;
    if (r->kind != FRY_VM_REGION_ANON_PRIVATE &&
        r->kind != FRY_VM_REGION_ANON_SHARED) return 0;
    uint32_t prot = r->prot;
    if (prot == 0) return 0;                       /* PROT_NONE -> real fault */
    if ((err & 0x2ULL) && !(prot & FRY_PROT_WRITE)) /* write to read-only */
        return 0;
    /* Lost a race with another CPU that already committed this page? Fine. */
    if (vmm_virt_to_phys_user(p->cr3, va) & LX_FRAME_MASK) return 1;
    uint64_t mflags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (prot & FRY_PROT_WRITE) mflags |= VMM_FLAG_WRITE;
    if (!(prot & FRY_PROT_EXEC)) mflags |= VMM_FLAG_NO_EXECUTE;
    if (lx_map_one(p->cr3, va, mflags) != 0) return 0;  /* OOM -> kill */
    return 1;
}

/* fry1387: demand stack growth. Linux auto-grows a thread's stack when a fault
 * lands just below the stack VMA (within ~rsp). We don't track a GROWSDOWN flag,
 * so emulate it: if a not-present USER #PF is at/above (rsp - 64KB) and just
 * below the nearest anon-private writable region (the stack), extend that region
 * down to the fault page and commit it. Capped so runaway recursion still dies.
 * Returns 1 if grown+satisfied (retry), 0 to fall through to USER FAULT kill. */
int lx_try_grow_stack(struct fry_process *p, uint64_t fault_va, uint64_t rsp,
                      uint64_t err) {
    if (!p) return 0;
    if (err & 0x1ULL) return 0;                       /* not-present only */
    uint64_t va = fault_va & ~0xFFFULL;
    /* Main-stack window FIRST, with NO rsp-proximity requirement: JSC pre-zeroes
     * an entire multi-MB stack frame far below rsp (observed fault ~8MB below
     * rsp), so the Linux "within 64KB of rsp" heuristic wrongly rejects it. The
     * window [USER_VA_TOP-64MB, USER_VA_TOP) is exclusively the main thread's
     * stack (heap/cages live at 0x6fxx/0x26xx), so growing here is safe; the
     * 64MB cap still kills genuine runaway recursion. The ELF loader maps the
     * initial 8MB ([USER_VA_TOP-8MB, USER_VA_TOP)) with no PROC_VMREGS region. */
    /* DIAGNOSTIC (fry1388): cap raised 64MB -> 512MB to discriminate whether
     * JSC's giant single-frame stack zero is a LEGIT large reservation (will
     * terminate well under 512MB -> just needs a bigger stack) or GARBAGE from a
     * corrupted object (runs unbounded to the new cap). Bucketed depth logging
     * replaces the per-page TBSTACKGROW spam. */
    if (va < USER_VA_TOP && va >= (USER_VA_TOP - (512ULL << 20))) {
        if (vmm_virt_to_phys_user(p->cr3, va) & LX_FRAME_MASK) return 1;
        if (!vm_any_region_overlap(p, va, LX_PG)) {   /* not a tracked region */
            uint64_t sflags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITE |
                              VMM_FLAG_NO_EXECUTE;
            if (lx_map_one(p->cr3, va, sflags) != 0) return 0;
            if (p->pid == TB_TRACE_CLAUDE_TGID || p->tgid == TB_TRACE_CLAUDE_TGID) {
                static uint64_t g_deep_bucket = 0;     /* 8MB-depth buckets */
                uint64_t depth = USER_VA_TOP - va;
                uint64_t bucket = depth >> 23;         /* 8MB units */
                if (bucket != g_deep_bucket) {
                    g_deep_bucket = bucket;
                    kprint_serial_only("TBBIGFRAME pid=%u depth=%luMB va=%lx rsp=%lx below_rsp=%ldMB\n",
                                       p->pid, (unsigned long)(depth >> 20),
                                       (unsigned long)va, (unsigned long)rsp,
                                       (long)(((int64_t)rsp - (int64_t)va) >> 20));
                }
            }
            return 1;
        }
    }
    if (fault_va + 65536ULL < rsp) return 0;          /* generic: near rsp only */
    struct fry_vm_region *best = 0;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        struct fry_vm_region *r = &PROC_VMREGS(p)[i];
        if (!r->used) continue;
        if (r->kind != FRY_VM_REGION_ANON_PRIVATE) continue;
        if (!(r->prot & FRY_PROT_WRITE)) continue;
        if (r->base <= va) continue;                  /* want the region ABOVE */
        if (!best || r->base < best->base) best = r;
    }
    if (!best) return 0;
    uint64_t gap = best->base - va;
    if (gap > (1ULL << 20)) return 0;                 /* single grow <= 1MB */
    if ((best->base + best->length) - va > (64ULL << 20)) return 0; /* cap 64MB */
    if (vm_any_region_overlap(p, va, gap)) return 0;  /* would collide below */
    best->length += gap;
    best->base = va;
    if (vmm_virt_to_phys_user(p->cr3, va) & LX_FRAME_MASK) return 1;
    uint64_t mflags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITE;
    if (!(best->prot & FRY_PROT_EXEC)) mflags |= VMM_FLAG_NO_EXECUTE;
    if (lx_map_one(p->cr3, va, mflags) != 0) return 0;
    if (p->pid == TB_TRACE_CLAUDE_TGID || p->tgid == TB_TRACE_CLAUDE_TGID) {
        kprint_serial_only("TBSTACKGROW pid=%u va=%lx newbase=%lx len=%lx rsp=%lx\n",
                           p->pid, (unsigned long)va, (unsigned long)best->base,
                           (unsigned long)best->length, (unsigned long)rsp);
    }
    return 1;
}

static void lx_unmap_range(uint64_t cr3, uint64_t base, uint64_t npg) {
    for (uint64_t i = 0; i < npg; i++) {
        uint64_t va = base + i * LX_PG;
        uint64_t f = vmm_virt_to_phys_user(cr3, va) & LX_FRAME_MASK;
        if (f) { vmm_unmap_user(cr3, va); pmm_free_page(f); }
    }
}

#define LX_SPARSE_RESERVE_MIN (4ULL * 1024ULL * 1024ULL * 1024ULL)

static uint32_t lx_mmap_prot_to_fry(uint64_t prot) {
    uint32_t fprot = 0;
    if (prot & LNX_PROT_READ) fprot |= FRY_PROT_READ;
    if (prot & LNX_PROT_WRITE) fprot |= FRY_PROT_WRITE;
    if (prot & LNX_PROT_EXEC) fprot |= FRY_PROT_EXEC;
    if (fprot != 0) fprot |= FRY_PROT_READ;
    return fprot;
}

static int lx_mmap_prot_supported(uint64_t prot) {
    uint32_t fprot = lx_mmap_prot_to_fry(prot);
    if ((fprot & ~(FRY_PROT_READ | FRY_PROT_WRITE | FRY_PROT_EXEC)) != 0) return 0;
    if (fprot == 0) return 1;
    return (fprot & FRY_PROT_READ) != 0;
}

static int is_range_free_for_mmap_hint(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p || !p->cr3 || length == 0) return 0;
    if (base < 0x10000ULL) return 0;   /* Linux mmap_min_addr equivalent */
    if (base + length < base) return 0;
    if (base + length > VM_USER_LIMIT) return 0;
    if (vm_any_region_overlap(p, base, length)) return 0;

    if (vmm_virt_to_phys_user(p->cr3, base) != 0) return 0;
    uint64_t end_page = base + length - LX_PG;
    if (vmm_virt_to_phys_user(p->cr3, end_page) != 0) return 0;

    /* Full walk for ranges <=64MB (thorough for cage and sub-cage sizes;
     * init-time cost is acceptable and guarantees no hidden present pages
     * from direct ELF/brk/prior maps). Larger ranges use ends + VMA check. */
    if (length <= (64ULL * 1024 * 1024)) {
        for (uint64_t va = base; va < base + length; va += LX_PG) {
            if (vmm_virt_to_phys_user(p->cr3, va) != 0) return 0;
        }
    }
    return 1;
}

static int lx_sparse_range_available(struct fry_process *p, uint64_t base, uint64_t length) {
    /* Use the common thorough predicate (also used by hint logic). */
    if (!is_range_free_for_mmap_hint(p, base, length)) return 0;
    return 1;
}

static int lx_map_sparse_anon_private(struct fry_process *p, uint64_t base,
                                      uint64_t length, uint64_t prot) {
    if (!lx_sparse_range_available(p, base, length)) return -1;
    if (!lx_mmap_prot_supported(prot)) return -1;

    int slot = vm_region_alloc_slot(p);
    if (slot < 0) return -1;
    /* Store the REAL protection (not 0). MAP_NORESERVE means "back pages
     * lazily", NOT "inaccessible": the range is usable per its mmap prot and
     * pages are committed on first touch by lx_try_demand_page(), which needs
     * the prot to map them with correct permissions. (committed stays 0; no
     * physical pages are backed until touched.) */
    vm_region_fill(&PROC_VMREGS(p)[slot], base, length,
                   lx_mmap_prot_to_fry(prot),
                   FRY_MAP_PRIVATE | FRY_MAP_ANON | FRY_MAP_RESERVE,
                   FRY_VM_REGION_ANON_PRIVATE, VM_BACKING_NONE, 0, 0);
    vm_region_merge_neighbors(p);
    return 0;
}

static uint32_t lx_to_fry_open_flags(uint64_t lflags) {
    uint32_t f = (uint32_t)(lflags & LNX_O_ACCMODE);
    if (lflags & LNX_O_CREAT) f |= O_CREAT;
    if (lflags & LNX_O_TRUNC) f |= O_TRUNC;
    if (lflags & LNX_O_APPEND) f |= O_APPEND;
    if (lflags & LNX_O_NONBLOCK) f |= O_NONBLOCK;
    if (lflags & LNX_O_CLOEXEC) f |= O_CLOEXEC;
    if (lflags & LNX_O_DIRECTORY) f |= FRY_O_DIRECTORY;
    return f;
}

static uint32_t lx_mode_from_vfs_attr(uint32_t attr) {
    if (attr & 0x10u) return LNX_S_IFDIR | 0755u;
    return LNX_S_IFREG | 0644u;
}

static void lx_put64(uint8_t *p, uint32_t off, uint64_t v) {
    *(uint64_t *)(uintptr_t)(p + off) = v;
}

static void lx_put32(uint8_t *p, uint32_t off, uint32_t v) {
    *(uint32_t *)(uintptr_t)(p + off) = v;
}

static int lx_copy_stat(struct fry_process *cur, uint64_t dst, uint64_t size,
                        uint32_t mode) {
    uint8_t st[144];
    for (uint32_t i = 0; i < sizeof(st); i++) st[i] = 0;

    lx_put64(st, 0, 1);                                /* st_dev */
    lx_put64(st, 8, (size ^ ((uint64_t)mode << 32)) | 1); /* st_ino */
    lx_put64(st, 16, 1);                               /* st_nlink */
    lx_put32(st, 24, mode);                            /* st_mode */
    lx_put32(st, 28, 0);                               /* st_uid */
    lx_put32(st, 32, 0);                               /* st_gid */
    lx_put64(st, 40, 0);                               /* st_rdev */
    lx_put64(st, 48, size);                            /* st_size */
    lx_put64(st, 56, 4096);                            /* st_blksize */
    lx_put64(st, 64, (size + 511ULL) / 512ULL);        /* st_blocks */

    uint64_t now = sys_now_ms() / 1000ULL;
    lx_put64(st, 72, now);                             /* st_atim.tv_sec */
    lx_put64(st, 88, now);                             /* st_mtim.tv_sec */
    lx_put64(st, 104, now);                            /* st_ctim.tv_sec */

    return copyout(cur, st, dst, sizeof(st));
}

static int lx_fd_stat_to_user(struct fry_process *cur, int fd, uint64_t dst) {
    if (!cur) return -ESRCH;
    struct fry_process_shared *shared = proc_shared_state(cur);
    if (!shared) return -ESRCH;
    if (fd == 0 || fd == 1 || fd == 2)
        return lx_copy_stat(cur, dst, 0, LNX_S_IFCHR | 0666u);
    if (fd < 3 || fd >= FRY_FD_MAX) return -EBADF;
    if (!shared->fd_ptrs[fd] || shared->fd_kind[fd] == FD_NONE) return -EBADF;
    if (shared->fd_kind[fd] == FD_STDIO)
        return lx_copy_stat(cur, dst, 0, LNX_S_IFCHR | 0666u);
    if (shared->fd_kind[fd] == FD_DIR) {
        struct vfs_stat st;
        if (!shared->fd_paths[fd][0]) return -EBADF;
        if (vfs_stat(shared->fd_paths[fd], &st) != 0) return -ENOENT;
        return lx_copy_stat(cur, dst, st.size, lx_mode_from_vfs_attr(st.attr));
    }
    if (shared->fd_kind[fd] != FD_FILE) return -EBADF;

    struct vfs_stat st;
    if (shared->fd_paths[fd][0] && vfs_stat(shared->fd_paths[fd], &st) == 0)
        return lx_copy_stat(cur, dst, st.size, lx_mode_from_vfs_attr(st.attr));

    struct vfs_file *vf = (struct vfs_file *)shared->fd_ptrs[fd];
    return lx_copy_stat(cur, dst, vf ? vf->size : 0, LNX_S_IFREG | 0644u);
}

static int lx_path_stat_to_user(struct fry_process *cur, int dirfd,
                                const char *raw_path, uint64_t dst) {
    char path[FRY_PATH_MAX];
    struct vfs_stat st;
    const char *lookup_path = streq_lit(raw_path, "/etc/localtime")
                            ? "/usr/share/zoneinfo/UTC"
                            : raw_path;
    int rpath = resolve_at_path(cur, dirfd, lookup_path, path);
    if (rpath < 0) return rpath;
    if (vfs_stat(path, &st) != 0) return -ENOENT;
    return lx_copy_stat(cur, dst, st.size, lx_mode_from_vfs_attr(st.attr));
}

static int lx_open_path(struct fry_process *cur, int dirfd, const char *raw_path,
                        uint64_t lflags) {
    if (!cur) return -ESRCH;
    struct fry_process_shared *shared = proc_shared_state(cur);
    if (!shared) return -ESRCH;

    char path[FRY_PATH_MAX];
    const char *lookup_path = streq_lit(raw_path, "/etc/localtime")
                            ? "/usr/share/zoneinfo/UTC"
                            : raw_path;
    int rpath = resolve_at_path(cur, dirfd, lookup_path, path);
    if (rpath < 0) return rpath;

    uint32_t flags = lx_to_fry_open_flags(lflags);
    struct vfs_stat st;
    if (vfs_stat(path, &st) == 0 && (st.attr & 0x10u)) {
        if ((flags & FRY_O_ACCMODE) != O_RDONLY) return -EISDIR;
        int fd = fd_alloc(cur);
        if (fd < 0) return -EMFILE;
        fd_install(cur, fd, shared->fd_paths[fd], FD_DIR, flags & O_NONBLOCK);
        shared->fd_table[fd] = 0;
        int prc = install_fd_path(cur, fd, path);
        if (prc < 0) return prc;
        return fd;
    }
    if (flags & FRY_O_DIRECTORY) return -ENOTDIR;

    struct vfs_file *f = vfs_open(path);
    if (!f && (flags & O_CREAT)) {
        vfs_create(path, 1);
        f = vfs_open(path);
    }
    if (!f) return -ENOENT;

    int fd = fd_alloc(cur);
    if (fd < 0) {
        vfs_close(f);
        return -EMFILE;
    }
    fd_install(cur, fd, f, FD_FILE, flags & O_NONBLOCK);
    int prc = install_fd_path(cur, fd, path);
    if (prc < 0) {
        vfs_close(f);
        return prc;
    }
    return fd;
}

struct lx_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

#define LX_EPOLLIN   0x00000001u
#define LX_EPOLLOUT  0x00000004u
#define LX_EPOLLERR  0x00000008u
#define LX_EPOLLHUP  0x00000010u
#define LX_EPOLL_CTL_ADD 1
#define LX_EPOLL_CTL_DEL 2
#define LX_EPOLL_CTL_MOD 3
#define LX_EFD_SEMAPHORE 0x00000001u
#define LX_TFD_TIMER_ABSTIME 0x00000001u
#define LX_TFD_TIMER_CANCEL_ON_SET 0x00000002u

static uint32_t lx_epoll_to_fry(uint32_t events) {
    uint32_t out = 0;
    if (events & LX_EPOLLIN)  out |= FRY_POLLIN;
    if (events & LX_EPOLLOUT) out |= FRY_POLLOUT;
    if (events & LX_EPOLLERR) out |= FRY_POLLERR;
    if (events & LX_EPOLLHUP) out |= FRY_POLLHUP;
    return out;
}

static uint32_t lx_epoll_from_fry(uint32_t events) {
    uint32_t out = 0;
    if (events & FRY_POLLIN)  out |= LX_EPOLLIN;
    if (events & FRY_POLLOUT) out |= LX_EPOLLOUT;
    if (events & FRY_POLLERR) out |= LX_EPOLLERR;
    if (events & FRY_POLLHUP) out |= LX_EPOLLHUP;
    return out;
}

static int lx_epoll_create_fd(struct fry_process *cur, uint32_t flags) {
    if (!cur || !proc_shared_state(cur)) return -ESRCH;
    if (flags & ~LNX_O_CLOEXEC) return -EINVAL;
    int fd = fd_alloc(cur);
    if (fd < 0) return -EMFILE;
    struct epoll_cb *ep = (struct epoll_cb *)kmalloc(sizeof(struct epoll_cb));
    if (!ep) return -ENOMEM;
    ep->items = 0;
    ep->count = 0;
    ep->lock = 0;
    fd_install(cur, fd, ep, FD_EPOLL, (flags & LNX_O_CLOEXEC) ? O_CLOEXEC : 0);
    return fd;
}

static int lx_epoll_ctl_fd(struct fry_process *cur, int epfd, int op,
                           int target_fd, uint64_t event_ptr) {
    struct fry_process_shared *shared = proc_shared_state(cur);
    if (!shared) return -ESRCH;
    if (epfd < 3 || epfd >= FRY_FD_MAX || target_fd < 0 ||
        target_fd >= FRY_FD_MAX) return -EBADF;
    if (shared->fd_kind[epfd] != FD_EPOLL || !shared->fd_ptrs[epfd])
        return -EBADF;
    if (!shared->fd_ptrs[target_fd] || shared->fd_kind[target_fd] == FD_NONE)
        return -EBADF;

    struct epoll_cb *ep = (struct epoll_cb *)shared->fd_ptrs[epfd];
    struct lx_epoll_event lev;
    if (op != LX_EPOLL_CTL_DEL) {
        if (!event_ptr) return -EFAULT;
        if (!user_buf_mapped(cur, event_ptr, sizeof(lev))) return -EFAULT;
        if (copyin(cur, event_ptr, &lev, sizeof(lev)) != 0) return -EFAULT;
    } else {
        lev.events = 0;
        lev.data = 0;
    }

    while (__sync_lock_test_and_set(&ep->lock, 1)) {}
    struct epoll_item *item = ep->items;
    while (item && item->fd != target_fd) item = item->next;

    if (op == LX_EPOLL_CTL_ADD) {
        if (item) { ep->lock = 0; return -EEXIST; }
        item = (struct epoll_item *)kmalloc(sizeof(struct epoll_item));
        if (!item) { ep->lock = 0; return -ENOMEM; }
        item->fd = target_fd;
        item->events = lx_epoll_to_fry(lev.events);
        item->data = lev.data;
        item->next = ep->items;
        ep->items = item;
        ep->count++;
    } else if (op == LX_EPOLL_CTL_MOD) {
        if (!item) { ep->lock = 0; return -ENOENT; }
        item->events = lx_epoll_to_fry(lev.events);
        item->data = lev.data;
    } else if (op == LX_EPOLL_CTL_DEL) {
        struct epoll_item **pp = &ep->items;
        while (*pp && (*pp)->fd != target_fd) pp = &(*pp)->next;
        if (!*pp) { ep->lock = 0; return -ENOENT; }
        struct epoll_item *tmp = *pp;
        *pp = tmp->next;
        kfree(tmp);
        ep->count--;
    } else {
        ep->lock = 0;
        return -EINVAL;
    }
    ep->lock = 0;
    return 0;
}

static int lx_epoll_wait_fd(struct fry_process *cur, int epfd, uint64_t events_ptr,
                            int maxevents, int timeout_ms) {
    struct fry_process_shared *shared = proc_shared_state(cur);
    if (!shared) return -ESRCH;
    if (epfd < 3 || epfd >= FRY_FD_MAX) return -EBADF;
    if (maxevents <= 0 || maxevents > 64) return -EINVAL;
    if (shared->fd_kind[epfd] != FD_EPOLL || !shared->fd_ptrs[epfd])
        return -EBADF;
    if (!user_buf_writable(cur, events_ptr,
                           (uint64_t)maxevents * sizeof(struct lx_epoll_event)))
        return -EFAULT;

    struct epoll_cb *ep = (struct epoll_cb *)shared->fd_ptrs[epfd];
    struct lx_epoll_event out[64];
    int n = 0;
    uint64_t start = sys_now_ms();
    uint64_t wake = (timeout_ms < 0) ? UINT64_MAX : start + (uint64_t)timeout_ms;

    while (n == 0) {
        while (__sync_lock_test_and_set(&ep->lock, 1)) {}
        struct epoll_item *item = ep->items;
        while (item && n < maxevents) {
            uint16_t rev = poll_check_fd(cur, item->fd, (uint16_t)item->events);
            if (rev) {
                out[n].events = lx_epoll_from_fry((uint32_t)rev);
                out[n].data = item->data;
                n++;
            }
            item = item->next;
        }
        ep->lock = 0;
        if (n > 0 || timeout_ms == 0) break;
        uint64_t now = sys_now_ms();
        if (now >= wake) break;
        sched_block_poll(cur->pid, wake);
        sched_yield();
        cur = proc_current();
        if (!cur) return -ESRCH;
    }

    if (n > 0 && copyout(cur, out, events_ptr,
                         (uint64_t)n * sizeof(struct lx_epoll_event)) != 0)
        return -EFAULT;
    return n;
}

static int lx_eventfd_create_fd(struct fry_process *cur, uint64_t initval,
                                uint32_t flags) {
    if (!cur || !proc_shared_state(cur)) return -ESRCH;
    if (flags & ~(LX_EFD_SEMAPHORE | LNX_O_NONBLOCK | LNX_O_CLOEXEC))
        return -EINVAL;
    int fd = fd_alloc(cur);
    if (fd < 0) return -EMFILE;
    struct eventfd_cb *ev = (struct eventfd_cb *)kmalloc(sizeof(struct eventfd_cb));
    if (!ev) return -ENOMEM;
    ev->counter = initval;
    ev->semaphore = (flags & LX_EFD_SEMAPHORE) != 0;
    ev->nonblock = (flags & LNX_O_NONBLOCK) != 0;
    ev->lock = 0;
    fd_install(cur, fd, ev, FD_EVENTFD,
               ((flags & LNX_O_NONBLOCK) ? O_NONBLOCK : 0) |
               ((flags & LNX_O_CLOEXEC) ? O_CLOEXEC : 0));
    return fd;
}

static int lx_timerfd_create_fd(struct fry_process *cur, int clockid,
                                uint32_t flags) {
    if (!cur || !proc_shared_state(cur)) return -ESRCH;
    if (clockid != LNX_CLOCK_REALTIME && clockid != LNX_CLOCK_MONOTONIC)
        return -EINVAL;
    if (flags & ~(LNX_O_NONBLOCK | LNX_O_CLOEXEC)) return -EINVAL;
    int fd = fd_alloc(cur);
    if (fd < 0) return -EMFILE;
    struct timerfd_cb *tm = (struct timerfd_cb *)kmalloc(sizeof(struct timerfd_cb));
    if (!tm) return -ENOMEM;
    tm->used = 1;
    tm->clockid = clockid;
    tm->it_value_ms = 0;
    tm->it_interval_ms = 0;
    tm->deadline_ms = 0;
    tm->expirations = 0;
    tm->nonblock = (flags & LNX_O_NONBLOCK) ? 1 : 0;
    fd_install(cur, fd, tm, FD_TIMERFD,
               ((flags & LNX_O_NONBLOCK) ? O_NONBLOCK : 0) |
               ((flags & LNX_O_CLOEXEC) ? O_CLOEXEC : 0));
    return fd;
}

static void lx_timerfd_snapshot(struct timerfd_cb *tm, uint64_t out[4]) {
    uint64_t now = sys_now_ms();
    uint64_t remaining_ms = 0;
    if (tm->it_value_ms > 0 && tm->deadline_ms > now)
        remaining_ms = tm->deadline_ms - now;
    out[0] = tm->it_interval_ms / 1000ULL;
    out[1] = (tm->it_interval_ms % 1000ULL) * 1000000ULL;
    out[2] = remaining_ms / 1000ULL;
    out[3] = (remaining_ms % 1000ULL) * 1000000ULL;
}

static int lx_timerfd_settime_fd(struct fry_process *cur, int fd, uint32_t flags,
                                 uint64_t new_ptr, uint64_t old_ptr) {
    struct fry_process_shared *shared = proc_shared_state(cur);
    if (!shared) return -ESRCH;
    if (fd < 3 || fd >= FRY_FD_MAX || shared->fd_kind[fd] != FD_TIMERFD)
        return -EBADF;
    if (flags & ~(LX_TFD_TIMER_ABSTIME | LX_TFD_TIMER_CANCEL_ON_SET))
        return -EINVAL;
    struct timerfd_cb *tm = (struct timerfd_cb *)shared->fd_ptrs[fd];
    if (!tm || !tm->used) return -EBADF;
    if (old_ptr) {
        if (!user_buf_writable(cur, old_ptr, 32)) return -EFAULT;
        uint64_t oldv[4];
        lx_timerfd_snapshot(tm, oldv);
        if (copyout(cur, oldv, old_ptr, 32) != 0) return -EFAULT;
    }
    if (!new_ptr) return -EFAULT;
    uint64_t spec[4]; /* Linux: interval sec,nsec, value sec,nsec */
    if (!user_buf_mapped(cur, new_ptr, 32)) return -EFAULT;
    if (copyin(cur, new_ptr, spec, 32) != 0) return -EFAULT;
    if (spec[1] >= 1000000000ULL || spec[3] >= 1000000000ULL) return -EINVAL;

    uint64_t interval_ms = spec[0] * 1000ULL + spec[1] / 1000000ULL;
    uint64_t value_ms = spec[2] * 1000ULL + spec[3] / 1000000ULL;
    uint64_t now = sys_now_ms();
    tm->it_interval_ms = interval_ms;
    tm->expirations = 0;
    if (value_ms == 0) {
        tm->it_value_ms = 0;
        tm->deadline_ms = 0;
        return 0;
    }
    if (flags & LX_TFD_TIMER_ABSTIME) {
        tm->deadline_ms = value_ms;
        tm->it_value_ms = (tm->deadline_ms > now) ? (tm->deadline_ms - now) : 0;
        if (tm->deadline_ms <= now) tm->expirations = 1;
    } else {
        tm->it_value_ms = value_ms;
        tm->deadline_ms = now + value_ms;
    }
    sched_wake_poll_waiters();
    return 0;
}

static int lx_timerfd_gettime_fd(struct fry_process *cur, int fd, uint64_t out_ptr) {
    struct fry_process_shared *shared = proc_shared_state(cur);
    if (!shared) return -ESRCH;
    if (fd < 3 || fd >= FRY_FD_MAX || shared->fd_kind[fd] != FD_TIMERFD)
        return -EBADF;
    struct timerfd_cb *tm = (struct timerfd_cb *)shared->fd_ptrs[fd];
    if (!tm || !tm->used) return -EBADF;
    if (!user_buf_writable(cur, out_ptr, 32)) return -EFAULT;
    uint64_t out[4];
    lx_timerfd_snapshot(tm, out);
    return copyout(cur, out, out_ptr, 32);
}

static int lx_signalfd_fd(struct fry_process *cur, int fd, uint64_t mask_ptr,
                          uint64_t mask_size, uint32_t flags) {
    struct fry_process_shared *shared = proc_shared_state(cur);
    if (!shared) return -ESRCH;
    if (flags & ~(LNX_O_NONBLOCK | LNX_O_CLOEXEC)) return -EINVAL;
    if (mask_ptr && mask_size < 8) return -EINVAL;
    uint64_t mask = 0;
    if (mask_ptr) {
        if (!user_buf_mapped(cur, mask_ptr, 8)) return -EFAULT;
        if (copyin(cur, mask_ptr, &mask, 8) != 0) return -EFAULT;
    }
    if (fd == -1) {
        int nfd = fd_alloc(cur);
        if (nfd < 0) return -EMFILE;
        struct signalfd_cb *sf = (struct signalfd_cb *)kmalloc(sizeof(struct signalfd_cb));
        if (!sf) return -ENOMEM;
        sf->used = 1;
        sf->mask = mask;
        sf->nonblock = (flags & LNX_O_NONBLOCK) ? 1 : 0;
        fd_install(cur, nfd, sf, FD_SIGNALFD,
                   ((flags & LNX_O_NONBLOCK) ? O_NONBLOCK : 0) |
                   ((flags & LNX_O_CLOEXEC) ? O_CLOEXEC : 0));
        return nfd;
    }
    if (fd < 3 || fd >= FRY_FD_MAX || shared->fd_kind[fd] != FD_SIGNALFD)
        return -EBADF;
    struct signalfd_cb *sf = (struct signalfd_cb *)shared->fd_ptrs[fd];
    if (!sf || !sf->used) return -EBADF;
    sf->mask = mask;
    sf->nonblock = (flags & LNX_O_NONBLOCK) ? 1 : 0;
    return fd;
}

static int64_t lx_read_fd(struct fry_process *cur, int fd, uint64_t dst,
                          uint64_t len) {
    if (!cur) return -ESRCH;
    struct fry_process_shared *shared = proc_shared_state(cur);
    if (!shared) return -ESRCH;
    if (len && !user_buf_writable(cur, dst, len)) return -EFAULT;
    if (fd == 0) return 0;
    if (fd < 3 || fd >= FRY_FD_MAX) return -EBADF;
    if (!shared->fd_ptrs[fd] || shared->fd_kind[fd] == FD_NONE) return -EBADF;
    if (shared->fd_kind[fd] == FD_STDIO) {
        int realfd = stdio_fd_from_ptr(shared->fd_ptrs[fd]);
        if (realfd == 0) return 0;
        return -EBADF;
    }
    if (shared->fd_kind[fd] == FD_FILE)
        return vfs_read((struct vfs_file *)shared->fd_ptrs[fd],
                        (void *)(uintptr_t)dst, (uint32_t)len);
    if (shared->fd_kind[fd] == FD_EVENTFD) {
        if (len < 8) return -EINVAL;
        struct eventfd_cb *ev = (struct eventfd_cb *)shared->fd_ptrs[fd];
        if (!ev) return -EBADF;
        while (__sync_lock_test_and_set(&ev->lock, 1)) {}
        if (ev->counter == 0) {
            ev->lock = 0;
            return -EAGAIN;
        }
        uint64_t val = ev->semaphore ? 1 : ev->counter;
        if (ev->semaphore) ev->counter--;
        else ev->counter = 0;
        ev->lock = 0;
        if (copyout(cur, &val, dst, 8) != 0) return -EFAULT;
        sched_wake_poll_waiters();
        return 8;
    }
    if (shared->fd_kind[fd] == FD_TIMERFD) {
        if (len < 8) return -EINVAL;
        struct timerfd_cb *tm = (struct timerfd_cb *)shared->fd_ptrs[fd];
        if (!tm || !tm->used) return -EBADF;
        timerfd_update_expirations(tm, sys_now_ms());
        if (tm->expirations == 0) return -EAGAIN;
        uint64_t val = tm->expirations;
        tm->expirations = 0;
        if (copyout(cur, &val, dst, 8) != 0) return -EFAULT;
        return 8;
    }
    if (shared->fd_kind[fd] == FD_SIGNALFD) {
        return -EAGAIN;
    }
    if (shared->fd_kind[fd] == FD_INOTIFY) {
        struct inotify_cb *in = (struct inotify_cb *)shared->fd_ptrs[fd];
        if (!in || !in->used) return -EBADF;
        if (in->ev_head == in->ev_tail) return -EAGAIN;
        struct inotify_event_buf *ev = &in->events[in->ev_tail];
        uint32_t ev_size = 16 + ev->len;
        if (ev_size > len) ev_size = (uint32_t)len;
        uint8_t tmp[16 + INOTIFY_NAME_MAX];
        *(int32_t *)(tmp + 0) = ev->wd;
        *(uint32_t *)(tmp + 4) = ev->mask;
        *(uint32_t *)(tmp + 8) = ev->cookie;
        *(uint32_t *)(tmp + 12) = ev->len;
        for (uint32_t i = 0; i < ev->len && i < INOTIFY_NAME_MAX && (16 + i) < ev_size; i++)
            tmp[16 + i] = (uint8_t)ev->name[i];
        in->ev_tail = (in->ev_tail + 1) % INOTIFY_EVENT_MAX;
        if (copyout(cur, tmp, dst, ev_size) != 0) return -EFAULT;
        return ev_size;
    }
    return -EBADF;
}

static int64_t lx_write_fd(struct fry_process *cur, int fd, uint64_t src,
                           uint64_t len) {
    if (!cur) return -ESRCH;
    struct fry_process_shared *shared = proc_shared_state(cur);
    if (len && !user_buf_mapped(cur, src, len)) return -EFAULT;
    if (fd == 1 || fd == 2) {
        kprint_write((const char *)(uintptr_t)src, len);
        return (int64_t)len;
    }
    if (!shared || fd < 3 || fd >= FRY_FD_MAX ||
        !shared->fd_ptrs[fd] || shared->fd_kind[fd] == FD_NONE)
        return -EBADF;
    if (shared->fd_kind[fd] == FD_STDIO) {
        int realfd = stdio_fd_from_ptr(shared->fd_ptrs[fd]);
        if (realfd == 1 || realfd == 2) {
            kprint_write((const char *)(uintptr_t)src, len);
            return (int64_t)len;
        }
        return -EBADF;
    }
    if (shared->fd_kind[fd] == FD_FILE)
        return vfs_write((struct vfs_file *)shared->fd_ptrs[fd],
                         (const void *)(uintptr_t)src, (uint32_t)len);
    if (shared->fd_kind[fd] == FD_EVENTFD) {
        if (len < 8) return -EINVAL;
        struct eventfd_cb *ev = (struct eventfd_cb *)shared->fd_ptrs[fd];
        if (!ev) return -EBADF;
        uint64_t val;
        if (copyin(cur, src, &val, 8) != 0) return -EFAULT;
        if (val == UINT64_MAX) return -EINVAL;
        while (__sync_lock_test_and_set(&ev->lock, 1)) {}
        uint64_t newval = ev->counter + val;
        if (newval < ev->counter) newval = UINT64_MAX;
        ev->counter = newval;
        ev->lock = 0;
        sched_wake_poll_waiters();
        return 8;
    }
    return -EBADF;
}

static int lx_close_fd(struct fry_process *cur, int fd) {
    if (!cur) return -ESRCH;
    struct fry_process_shared *shared = proc_shared_state(cur);
    if (!shared) return -ESRCH;
    if (fd == 0 || fd == 1 || fd == 2) return 0;
    if (fd < 3 || fd >= FRY_FD_MAX) return -EBADF;
    if (!shared->fd_ptrs[fd] || shared->fd_kind[fd] == FD_NONE) return -EBADF;
    if (shared->fd_kind[fd] == FD_FILE)
        vfs_close((struct vfs_file *)shared->fd_ptrs[fd]);
    else if (shared->fd_kind[fd] == FD_EPOLL) {
        struct epoll_cb *ep = (struct epoll_cb *)shared->fd_ptrs[fd];
        if (ep) {
            struct epoll_item *item = ep->items;
            while (item) {
                struct epoll_item *next = item->next;
                kfree(item);
                item = next;
            }
            kfree(ep);
        }
    } else if (shared->fd_kind[fd] == FD_EVENTFD) {
        kfree(shared->fd_ptrs[fd]);
    } else if (shared->fd_kind[fd] == FD_TIMERFD) {
        struct timerfd_cb *tm = (struct timerfd_cb *)shared->fd_ptrs[fd];
        if (tm) { tm->used = 0; kfree(tm); }
    } else if (shared->fd_kind[fd] == FD_SIGNALFD) {
        struct signalfd_cb *sf = (struct signalfd_cb *)shared->fd_ptrs[fd];
        if (sf) { sf->used = 0; kfree(sf); }
    } else if (shared->fd_kind[fd] == FD_INOTIFY) {
        struct inotify_cb *in = (struct inotify_cb *)shared->fd_ptrs[fd];
        if (in) { in->used = 0; kfree(in); }
    }
    fd_release(cur, fd);
    return 0;
}

static uint64_t linux_syscall_dispatch_impl(uint64_t num, uint64_t a1, uint64_t a2,
                                            uint64_t a3, uint64_t a4, uint64_t a5,
                                            uint64_t a6,
                                            struct fry_process *cur) {
    if (!cur) return (uint64_t)-ESRCH;

    switch (num) {

    case LNX_write: {
        int fd = (int)a1;
        uint64_t len = a3;
        int64_t rc = lx_write_fd(cur, fd, a2, len);
        return (uint64_t)rc;
    }

    case LNX_writev: {
        int fd = (int)a1;
        int n = (int)a3;
        if (n < 0) return (uint64_t)-EINVAL;
        if (n == 0) return 0;
        if (!user_buf_mapped(cur, a2, (uint64_t)n * 16)) return (uint64_t)-EFAULT;
        const uint64_t *iov = (const uint64_t *)(uintptr_t)a2; /* {base,len} pairs */
        uint64_t total = 0;
        for (int i = 0; i < n; i++) {
            uint64_t base = iov[i * 2], l = iov[i * 2 + 1];
            if (!l) continue;
            int64_t rc = lx_write_fd(cur, fd, base, l);
            if (rc < 0) return total ? total : (uint64_t)rc;
            total += (uint64_t)rc;
            if ((uint64_t)rc < l) break;
        }
        return total;
    }

    case LNX_readv: {
        int fd = (int)a1;
        int n = (int)a3;
        if (n < 0) return (uint64_t)-EINVAL;
        if (n == 0) return 0;
        if (!user_buf_mapped(cur, a2, (uint64_t)n * 16)) return (uint64_t)-EFAULT;
        const uint64_t *iov = (const uint64_t *)(uintptr_t)a2;
        uint64_t total = 0;
        for (int i = 0; i < n; i++) {
            uint64_t base = iov[i * 2], l = iov[i * 2 + 1];
            if (!l) continue;
            int64_t rc = lx_read_fd(cur, fd, base, l);
            if (rc < 0) return (uint64_t)rc;
            total += (uint64_t)rc;
            if ((uint64_t)rc < l) break;
        }
        return total;
    }

    case LNX_read: {
        int fd = (int)a1;
        uint64_t len = a3;
        int64_t rc = lx_read_fd(cur, fd, a2, len);
        return (uint64_t)rc;
    }

    case LNX_pread64: {
        int fd = (int)a1;
        uint64_t len = a3;
        int64_t off = (int64_t)a4;
        struct fry_process_shared *shared = proc_shared_state(cur);
        if (off < 0) return (uint64_t)-EINVAL;
        if (!shared || fd < 3 || fd >= FRY_FD_MAX ||
            !shared->fd_ptrs[fd] || shared->fd_kind[fd] != FD_FILE)
            return (uint64_t)-EBADF;
        if (len && !user_buf_writable(cur, a2, len)) return (uint64_t)-EFAULT;
        struct vfs_file *vf = (struct vfs_file *)shared->fd_ptrs[fd];
        int64_t old = vfs_seek(vf, 0, FRY_SEEK_CUR);
        if (old < 0) return (uint64_t)-ESPIPE;
        if (vfs_seek(vf, off, FRY_SEEK_SET) < 0) return (uint64_t)-EINVAL;
        int rd = vfs_read(vf, (void *)(uintptr_t)a2, (uint32_t)len);
        vfs_seek(vf, old, FRY_SEEK_SET);
        return (uint64_t)rd;
    }

    case LNX_getdents64: {
        int fd = (int)a1;
        uint32_t count = (uint32_t)a3;
        struct fry_process_shared *shared = proc_shared_state(cur);
        if (count == 0) return (uint64_t)-EINVAL;
        if (!user_buf_writable(cur, a2, count)) return (uint64_t)-EFAULT;
        if (!shared || fd < 3 || fd >= FRY_FD_MAX ||
            !shared->fd_ptrs[fd] || shared->fd_kind[fd] == FD_NONE)
            return (uint64_t)-EBADF;
        if (shared->fd_kind[fd] != FD_DIR) return (uint64_t)-ENOTDIR;
        if (!shared->fd_paths[fd][0]) return (uint64_t)-EBADF;

        uint32_t skip = (shared->fd_table[fd] > 0) ? (uint32_t)shared->fd_table[fd] : 0;
        struct lx_getdents64_ctx ctx = {
            (uint8_t *)(uintptr_t)a2, count, 0, skip, skip, 0, 0
        };
        int rc = vfs_readdir_ex(shared->fd_paths[fd], lx_getdents64_cb, &ctx);
        if (rc < 0) return (uint64_t)-ENOENT;
        if (ctx.pos == 0 && ctx.overflow) return (uint64_t)-EINVAL;
        shared->fd_table[fd] = (int)(skip + ctx.emitted);
        return (uint64_t)ctx.pos;
    }

    case LNX_lseek: {
        int fd = (int)a1;
        int64_t off = (int64_t)a2;
        int whence = (int)a3;
        struct fry_process_shared *shared = proc_shared_state(cur);
        if (fd == 0 || fd == 1 || fd == 2) return (uint64_t)-ESPIPE;
        if (!shared || fd < 3 || fd >= FRY_FD_MAX ||
            !shared->fd_ptrs[fd] || shared->fd_kind[fd] != FD_FILE)
            return (uint64_t)-EBADF;
        if (whence != FRY_SEEK_SET && whence != FRY_SEEK_CUR && whence != FRY_SEEK_END)
            return (uint64_t)-EINVAL;
        int64_t pos = vfs_seek((struct vfs_file *)shared->fd_ptrs[fd], off, whence);
        if (pos < 0) return (uint64_t)-EINVAL;
        return (uint64_t)pos;
    }

    case LNX_fstat: {
        int rc = lx_fd_stat_to_user(cur, (int)a1, a2);
        return (uint64_t)rc;
    }

    case LNX_brk: {
        struct fry_process_shared *sh = proc_shared_state(cur);
        if (!sh) return (uint64_t)-ENOMEM;
        uint64_t cur_brk = sh->heap_end;
        uint64_t newbrk = a1;
        if (newbrk == 0) return cur_brk;
        if (newbrk < sh->heap_start || newbrk > USER_TOP) return cur_brk;
        uint64_t old_pg = (cur_brk + 0xFFF) & ~0xFFFULL;
        uint64_t new_pg = (newbrk + 0xFFF) & ~0xFFFULL;
        if (new_pg > old_pg) {
            for (uint64_t va = old_pg; va < new_pg; va += LX_PG) {
                if (vmm_virt_to_phys_user(cur->cr3, va) & LX_FRAME_MASK) continue;
                if (lx_map_one(cur->cr3, va,
                               VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE) != 0) {
                    lx_unmap_range(cur->cr3, old_pg, (va - old_pg) / LX_PG);
                    return cur_brk;
                }
            }
        } else if (new_pg < old_pg) {
            lx_unmap_range(cur->cr3, new_pg, (old_pg - new_pg) / LX_PG);
        }
        sh->heap_end = newbrk;
        return newbrk;
    }

    case LNX_mmap: {
        uint64_t addr = a1, len = a2, prot = a3, flags = a4;
        int fd = (int)a5;
        uint64_t off = a6;
        struct fry_process_shared *shared = proc_shared_state(cur);
        if (!shared) return (uint64_t)-ENOMEM;
        if (len == 0) return (uint64_t)-EINVAL;
        if (off & 0xFFFULL) return (uint64_t)-EINVAL;
        if (len > UINT64_MAX - 0xFFFULL) return (uint64_t)-ENOMEM;
        uint64_t npg = (len + 0xFFF) >> 12;
        uint64_t map_len = npg * LX_PG;
        uint64_t mflags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (prot & LNX_PROT_WRITE) mflags |= VMM_FLAG_WRITE;
        if (!(prot & LNX_PROT_EXEC)) mflags |= VMM_FLAG_NO_EXECUTE;

        uint64_t base = 0;
        if ((flags & LNX_MAP_FIXED) && addr) {
            base = addr & ~0xFFFULL;
            (void)vm_unmap_region_range(cur, base, map_len);
            lx_unmap_range(cur->cr3, base, npg);   /* replace any existing */
        } else if (addr) {
            /* Honor mmap hint when the range is usable. Force the supplied
             * hint (for JSC gigacages at 0x0120... etc) when the range is
             * demonstrably free. Never silently fall back to cursor for a
             * usable hint; the app will often dereference its chosen cage VA
             * regardless of the return value. */
            uint64_t hint_base = addr & ~0xFFFULL;
            if (is_range_free_for_mmap_hint(cur, hint_base, map_len)) {
                base = hint_base;
            }
        }

        if (!base) {
            /* Top-down cursor allocation.  Unlike Linux, our blind cursor used
             * to collide with hints; we now search downward using the hint-
             * check (which verifies against all tracked VM regions AND the
             * page tables) to find the first truly free hole. */
            uint64_t align = 0x1000ULL;
            if (map_len >= (1ULL * 1024 * 1024)) {
                uint64_t p2 = map_len;
                p2 |= p2 >> 1;  p2 |= p2 >> 2;
                p2 |= p2 >> 4;  p2 |= p2 >> 8;
                p2 |= p2 >> 16; p2 |= p2 >> 32;
                p2 = (p2 >> 1) + 1;
                align = p2;
            }
            while (shared->linux_mmap_next >= map_len + 0x10000ULL) {
                uint64_t cand = (shared->linux_mmap_next - map_len) & ~(align - 1);
                if (is_range_free_for_mmap_hint(cur, cand, map_len)) {
                    base = cand;
                    shared->linux_mmap_next = cand;
                    break;
                }
                /* Occupied. Round down to next aligned block and try again. */
                if (cand <= align) { shared->linux_mmap_next = 0; break; }
                shared->linux_mmap_next = cand;
            }
            if (!base) return (uint64_t)-ENOMEM;
        }

        int anon_private = ((flags & LNX_MAP_ANONYMOUS) != 0) &&
                           ((flags & LNX_MAP_PRIVATE) != 0) &&
                           ((flags & LNX_MAP_SHARED) == 0);
        /* Sparse/lazy reservation for large anon private regions.
         * Small NORESERVE mappings still get eager pages — only treat
         * regions >= 64 MiB as sparse when NORESERVE is set.  Gigacage-
         * sized regions (>= 4 GiB) are always sparse. */
        int sparse_reserve = anon_private &&
                             (map_len >= (64ULL * 1024 * 1024)) &&
                             ((flags & LNX_MAP_NORESERVE) != 0 ||
                              map_len >= LX_SPARSE_RESERVE_MIN);
        if (sparse_reserve) {
            if (lx_map_sparse_anon_private(cur, base, map_len, prot) != 0) {
                return (uint64_t)-ENOMEM;
            }
            if (tb_trace_is_claude(cur) && map_len == 0x40000000ULL) {
                g_tb_jsc_slot_watch_base = base + TB_JSC_CAGE_SLOT_ARRAY_OFF;
                tb_program_jsc_slot_watchpoints_for(cur);
                kprint_serial_only("TBWATCH arm-jsc-slots pid=%u tgid=%u cage=0x%llx arr=0x%llx slots=0x%llx,0x%llx,0x%llx,0x%llx\n",
                                   cur->pid, cur->tgid,
                                   (unsigned long long)base,
                                   (unsigned long long)g_tb_jsc_slot_watch_base,
                                   (unsigned long long)(g_tb_jsc_slot_watch_base + TB_JSC_SLOT_WATCH0_OFF),
                                   (unsigned long long)(g_tb_jsc_slot_watch_base + TB_JSC_SLOT_WATCH1_OFF),
                                   (unsigned long long)(g_tb_jsc_slot_watch_base + TB_JSC_SLOT_WATCH2_OFF),
                                   (unsigned long long)(g_tb_jsc_slot_watch_base + TB_JSC_SLOT_WATCH3_OFF));
            }
            if (tb_trace_is_claude(cur)) {
                kprint_serial_only("TBTRACE mmap detail pid=%u base=0x%llx len=0x%llx chosen=%s sparse=1\n",
                                   (unsigned)cur->pid, (unsigned long long)base, (unsigned long long)map_len,
                                   (base == (addr & ~0xFFFULL) ? "hint" : "cursor"));
            }
            return base;
        }

        /* eager path (non-sparse) */
        for (uint64_t i = 0; i < npg; i++) {
            uint64_t va = base + i * LX_PG;
            if (vmm_virt_to_phys_user(cur->cr3, va) & LX_FRAME_MASK) continue;
            if (lx_map_one(cur->cr3, va, mflags) != 0) {
                lx_unmap_range(cur->cr3, base, i);
                return (uint64_t)-ENOMEM;
            }
        }
        if (!(flags & LNX_MAP_ANONYMOUS)) {
            if (!shared || fd < 3 || fd >= FRY_FD_MAX ||
                !shared->fd_ptrs[fd] || shared->fd_kind[fd] != FD_FILE) {
                lx_unmap_range(cur->cr3, base, npg);
                return (uint64_t)-EBADF;
            }
            struct vfs_file *vf = (struct vfs_file *)shared->fd_ptrs[fd];
            int64_t old = vfs_seek(vf, 0, FRY_SEEK_CUR);
            if (old < 0) {
                lx_unmap_range(cur->cr3, base, npg);
                return (uint64_t)-ESPIPE;
            }
            if (vfs_seek(vf, (int64_t)off, FRY_SEEK_SET) < 0) {
                vfs_seek(vf, old, FRY_SEEK_SET);
                lx_unmap_range(cur->cr3, base, npg);
                return (uint64_t)-EINVAL;
            }
            uint64_t left = len;
            for (uint64_t i = 0; i < npg && left > 0; i++) {
                uint64_t va = base + i * LX_PG;
                uint64_t pa = vmm_virt_to_phys_user(cur->cr3, va) & LX_FRAME_MASK;
                if (!pa) {
                    vfs_seek(vf, old, FRY_SEEK_SET);
                    lx_unmap_range(cur->cr3, base, npg);
                    return (uint64_t)-ENOMEM;
                }
                uint8_t *kv = (uint8_t *)(uintptr_t)vmm_phys_to_virt(pa);
                uint32_t chunk = (left > LX_PG) ? (uint32_t)LX_PG : (uint32_t)left;
                int rd = vfs_read(vf, kv, chunk);
                if (rd < 0) {
                    vfs_seek(vf, old, FRY_SEEK_SET);
                    lx_unmap_range(cur->cr3, base, npg);
                    return (uint64_t)-EIO;
                }
                if ((uint32_t)rd < chunk) {
                    uint32_t got = (uint32_t)rd;
                    while (got < chunk) {
                        int rd2 = vfs_read(vf, kv + got, chunk - got);
                        if (rd2 <= 0) break;
                        got += (uint32_t)rd2;
                    }
                    if (got < chunk) {
                        for (uint32_t z = got; z < chunk; z++) kv[z] = 0;
                    }
                }
                left -= chunk;
            }
            vfs_seek(vf, old, FRY_SEEK_SET);
        }

        /* Track the successful mapping in the VM regions table. fry1383:
         * this is the fix for untracked eager mappings. */
        int slot = vm_region_alloc_slot(cur);
        if (slot >= 0) {
            uint32_t fprot = lx_mmap_prot_to_fry(prot);
            uint32_t fflags = FRY_MAP_PRIVATE | (anon_private ? FRY_MAP_ANON : 0);
            uint8_t kind = anon_private ? FRY_VM_REGION_ANON_PRIVATE : FRY_VM_REGION_FILE_PRIVATE;
            vm_region_fill(&PROC_VMREGS(cur)[slot], base, map_len, fprot, fflags, kind, VM_BACKING_NONE, 0, 1);
        }

        if (tb_trace_is_claude(cur)) {
            kprint_serial_only("TBTRACE mmap detail pid=%u base=0x%llx len=0x%llx chosen=%s sparse=0\n",
                               (unsigned)cur->pid, (unsigned long long)base, (unsigned long long)map_len,
                               (base == (addr & ~0xFFFULL) ? "hint" : "cursor"));
        }
        return base;
    }

    case LNX_munmap: {
        uint64_t base = a1 & ~0xFFFULL;
        uint64_t npg = (a2 + 0xFFF) >> 12;
        uint64_t map_len = npg * LX_PG;
        if (vm_unmap_region_range(cur, base, map_len) == 0) return 0;
        lx_unmap_range(cur->cr3, base, npg);
        return 0;
    }

    case LNX_mprotect: {
        uint64_t base = a1 & ~0xFFFULL, prot = a3;
        uint64_t npg = (a2 + 0xFFF) >> 12;
        uint64_t map_len = npg * LX_PG;
        uint32_t fry_prot = lx_mmap_prot_to_fry(prot);
        /* Always update the vm_region prot if a region covers this range.
         * Also update already-present page table entries — the old code
         * returned after vm_mprotect_region_range and never touched PTEs,
         * so protection changes on faulted-in pages were silently ignored. */
        int region_ok = (vm_mprotect_region_range(cur, base, map_len, fry_prot) == 0);
        uint64_t mflags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (prot & LNX_PROT_WRITE) mflags |= VMM_FLAG_WRITE;
        if (!(prot & LNX_PROT_EXEC)) mflags |= VMM_FLAG_NO_EXECUTE;
        for (uint64_t i = 0; i < npg; i++) {
            uint64_t va = base + i * LX_PG;
            uint64_t f = vmm_virt_to_phys_user(cur->cr3, va) & LX_FRAME_MASK;
            if (!f) continue;
            vmm_unmap_user(cur->cr3, va);
            vmm_map_user(cur->cr3, va, f, mflags);
        }
        return 0;
    }

    case LNX_madvise: {
        uint64_t addr = a1;
        uint64_t len = a2;
        uint64_t advice = a3;
        if (len == 0) return 0;
        if (addr > UINT64_MAX - len) return (uint64_t)-EINVAL;

        if (advice == 102) { /* MADV_GUARD_INSTALL */
            uint64_t base = addr & ~0xFFFULL;
            uint64_t end = (addr + len + 0xFFFULL) & ~0xFFFULL;
            if (end < base) return (uint64_t)-EINVAL;
            (void)vm_mprotect_region_range(cur, base, end - base, 0);
            for (uint64_t va = base; va < end; va += LX_PG) {
                uint64_t pa = vmm_virt_to_phys_user(cur->cr3, va) & LX_FRAME_MASK;
                if (!pa) continue;
                vmm_unmap_user(cur->cr3, va);
                __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
            }
            return 0;
        }

        /*
         * JSC/Bun uses MADV_DONTNEED while cycling heap pages. Linux promises
         * anonymous private pages read back as zero after the advice. Keeping
         * stale contents here can feed old tagged pointers back into JSC.
         */
        if (advice == 4) { /* MADV_DONTNEED */
            uint64_t base = addr & ~0xFFFULL;
            uint64_t end = (addr + len + 0xFFFULL) & ~0xFFFULL;
            if (end < base) return (uint64_t)-EINVAL;
            return (uint64_t)vm_madvise_dontneed(cur, base, end - base);
        }
        return 0;
    }

    case LNX_arch_prctl: {
        int code = (int)a1;
        uint64_t addr = a2;
        if (code == LNX_ARCH_SET_FS) {
            cur->user_fs_base = addr;
            write_user_fs_base(addr);
            /*
             * glibc's x86_64 loader expects tcbhead_t.tcb/self to point at
             * the thread pointer before it calls set_tid_address(). Linux
             * userland normally initializes these itself; normalize them here
             * as a compatibility guard for the early Tater Bridge bootstrap.
             */
            if (addr && user_buf_writable(cur, addr, 24)) {
                uint64_t *tcb = (uint64_t *)(uintptr_t)addr;
                tcb[0] = addr;  /* tcbhead_t.tcb */
                tcb[2] = addr;  /* tcbhead_t.self */
            }
            return 0;
        }
        if (code == LNX_ARCH_GET_FS) {
            if (!user_buf_mapped(cur, addr, 8)) return (uint64_t)-EFAULT;
            *(uint64_t *)(uintptr_t)addr = cur->user_fs_base; return 0;
        }
        if (code == LNX_ARCH_GET_GS) {
            if (!user_buf_mapped(cur, addr, 8)) return (uint64_t)-EFAULT;
            *(uint64_t *)(uintptr_t)addr = 0; return 0;
        }
        return (uint64_t)-EINVAL;
    }

    case LNX_set_tid_address: {
        cur->linux_clear_child_tid = a1;
        return cur->pid;
    }

    case LNX_set_robust_list:
        cur->linux_robust_list = a1;
        return 0;

    case LNX_getpid:
        return cur->tgid;
    case LNX_gettid:
        return cur->pid;
    case LNX_getppid:
        return 1;
    case LNX_getrusage: {
        /* Return zeroed rusage. JSC/Bun calls this for memory tracking
         * during initialization. A full implementation would track
         * ru_maxrss/ru_utime/ru_stime but returning zeros is sufficient
         * to unblock initialization. */
        uint64_t len = a2;
        if (len > 144) len = 144; /* sizeof(struct rusage) */
        if (a1 == 0 /* RUSAGE_SELF */ || a1 == (uint64_t)-1 /* RUSAGE_THREAD */) {
            char *dst = (char *)(uintptr_t)a2;
            if (user_buf_writable(cur, a2, (uint32_t)len)) {
                for (uint64_t i = 0; i < len; i++) dst[i] = 0;
                return 0;
            }
        }
        return (uint64_t)-EINVAL;
    }
    case LNX_getuid:
    case LNX_geteuid:
    case LNX_getgid:
    case LNX_getegid:
        return 0;

    case LNX_uname: {
        if (!user_buf_mapped(cur, a1, 6 * 65)) return (uint64_t)-EFAULT;
        char *u = (char *)(uintptr_t)a1;
        for (int i = 0; i < 6 * 65; i++) u[i] = 0;
        lx_setfield(u + 0 * 65, "Linux");
        lx_setfield(u + 1 * 65, "tatertos");
        lx_setfield(u + 2 * 65, "5.15.0-tatertos");
        lx_setfield(u + 3 * 65, "#1 TaterTOS64v3 Tater Bridge");
        lx_setfield(u + 4 * 65, "x86_64");
        lx_setfield(u + 5 * 65, "(none)");
        return 0;
    }

    case LNX_getrandom: {
        uint64_t buf = a1, len = a2;
        if (len && !user_buf_mapped(cur, buf, len)) return (uint64_t)-EFAULT;
        if (len) entropy_getbytes((void *)(uintptr_t)buf, (uint32_t)len);
        return len;
    }

    case LNX_clock_gettime: {
        int id = (int)a1;
        uint64_t ts = a2;
        if (!user_buf_mapped(cur, ts, 16)) return (uint64_t)-EFAULT;
        uint64_t ms;
        if (id == LNX_CLOCK_REALTIME) {
            int64_t rtc_boot_epoch_sec(void);
            uint64_t freq = hpet_get_freq_hz();
            uint64_t up = (freq == 0) ? 0 : (hpet_read_counter() * 1000ULL) / freq;
            ms = (uint64_t)(rtc_boot_epoch_sec() * 1000LL) + up;
        } else {
            ms = sys_now_ms();
        }
        uint64_t *t = (uint64_t *)(uintptr_t)ts;
        t[0] = ms / 1000ULL;
        t[1] = (ms % 1000ULL) * 1000000ULL;
        return 0;
    }

    case LNX_gettimeofday: {
        uint64_t tv = a1;
        uint64_t tz = a2;
        int64_t rtc_boot_epoch_sec(void);
        uint64_t freq = hpet_get_freq_hz();
        uint64_t up_ms = (freq == 0) ? 0 : (hpet_read_counter() * 1000ULL) / freq;
        uint64_t ms = (uint64_t)(rtc_boot_epoch_sec() * 1000LL) + up_ms;
        if (tv) {
            if (!user_buf_writable(cur, tv, 16)) return (uint64_t)-EFAULT;
            uint64_t out[2];
            out[0] = ms / 1000ULL;
            out[1] = (ms % 1000ULL) * 1000ULL;
            if (copyout(cur, out, tv, sizeof(out)) != 0) return (uint64_t)-EFAULT;
        }
        if (tz) {
            if (!user_buf_writable(cur, tz, 8)) return (uint64_t)-EFAULT;
            uint32_t out[2] = {0, 0};
            if (copyout(cur, out, tz, sizeof(out)) != 0) return (uint64_t)-EFAULT;
        }
        return 0;
    }

    case LNX_time: {
        int64_t rtc_boot_epoch_sec(void);
        uint64_t freq = hpet_get_freq_hz();
        uint64_t up_ms = (freq == 0) ? 0 : (hpet_read_counter() * 1000ULL) / freq;
        int64_t sec = rtc_boot_epoch_sec() + (int64_t)(up_ms / 1000ULL);
        if (a1) {
            if (!user_buf_writable(cur, a1, sizeof(sec))) return (uint64_t)-EFAULT;
            if (copyout(cur, &sec, a1, sizeof(sec)) != 0) return (uint64_t)-EFAULT;
        }
        return (uint64_t)sec;
    }

    case LNX_sysinfo: {
        if (!user_buf_writable(cur, a1, 112)) return (uint64_t)-EFAULT;
        uint8_t info[112];
        for (uint32_t i = 0; i < sizeof(info); i++) info[i] = 0;
        uint64_t freq = hpet_get_freq_hz();
        uint64_t uptime_ms = (freq > 0) ? ((hpet_read_counter() * 1000ULL) / freq) : 0;
        int64_t uptime = (int64_t)(uptime_ms / 1000ULL);
        uint64_t total = pmm_get_total_pages() * 4096ULL;
        uint64_t free = (pmm_get_total_pages() - pmm_get_used_pages()) * 4096ULL;
        uint16_t proc_count = 0;
        for (uint32_t i = 0; i < PROC_MAX; i++) {
            if (procs[i].state != PROC_UNUSED && procs[i].state != PROC_DEAD)
                proc_count++;
        }
        copyout(cur, &uptime, a1 + 0, sizeof(uptime));
        copyout(cur, &total, a1 + 32, sizeof(total));
        copyout(cur, &free, a1 + 40, sizeof(free));
        copyout(cur, &proc_count, a1 + 80, sizeof(proc_count));
        uint32_t mem_unit = 1;
        copyout(cur, &mem_unit, a1 + 104, sizeof(mem_unit));
        return 0;
    }

    case LNX_clock_getres: {
        int id = (int)a1;
        uint64_t ts = a2;
        (void)id;   /* 1ms resolution reported for all clocks */
        if (!user_buf_mapped(cur, ts, 16)) return (uint64_t)-EFAULT;
        uint64_t *t = (uint64_t *)(uintptr_t)ts;
        t[0] = 0;
        t[1] = 1000000ULL;  /* 1 ms resolution from HPET/scheduler tick */
        return 0;
    }

    case LNX_getcwd: {
        uint64_t buf = a1, sz = a2;
        struct fry_process_shared *sh = proc_shared_state(cur);
        const char *cwd = (sh && sh->cwd[0]) ? sh->cwd : "/";
        uint64_t n = 0; while (cwd[n]) n++; n++;
        if (sz < n) return (uint64_t)-ERANGE;
        if (!user_buf_mapped(cur, buf, n)) return (uint64_t)-EFAULT;
        char *d = (char *)(uintptr_t)buf;
        for (uint64_t i = 0; i < n; i++) d[i] = cwd[i];
        return n;
    }

    case LNX_ioctl:
        /* Honest: no real termios device yet → "not a tty". libc then uses
         * full buffering and flushes at exit. Real ioctls come with file I/O. */
        return (uint64_t)-ENOTTY;

    case LNX_prlimit64: {
        uint64_t old = a4;
        if (old && user_buf_mapped(cur, old, 16)) {
            uint64_t *o = (uint64_t *)(uintptr_t)old;
            o[0] = 0x800000ULL;            /* rlim_cur = 8 MiB stack */
            o[1] = ~0ULL;                  /* rlim_max = INFINITY */
        }
        return 0;
    }

    case LNX_prctl: {
        int option = (int)a1;
        switch (option) {
        case PR_SET_NAME: {
            if (!user_buf_mapped(cur, a2, 16)) return (uint64_t)-EFAULT;
            const char *src = (const char *)(uintptr_t)a2;
            uint32_t i = 0;
            for (; i < sizeof(cur->name) - 1 && i < 15; i++) {
                cur->name[i] = src[i];
                if (src[i] == '\0') break;
            }
            cur->name[sizeof(cur->name) - 1] = '\0';
            if (i >= sizeof(cur->name) - 1 || i >= 15) cur->name[i < sizeof(cur->name) ? i : sizeof(cur->name) - 1] = '\0';
            return 0;
        }
        case PR_GET_NAME: {
            if (!user_buf_writable(cur, a2, 16)) return (uint64_t)-EFAULT;
            char out[16];
            for (uint32_t i = 0; i < sizeof(out); i++) out[i] = 0;
            for (uint32_t i = 0; i < sizeof(out) - 1 && cur->name[i]; i++) out[i] = cur->name[i];
            return (uint64_t)copyout(cur, out, a2, sizeof(out));
        }
        case PR_SET_DUMPABLE:
            cur->dumpable = (uint8_t)(a2 != 0);
            return 0;
        case PR_GET_DUMPABLE:
            return cur->dumpable ? 1 : 0;
        case PR_SET_NO_NEW_PRIVS:
            if (a2 != 1 || a3 || a4 || a5) return (uint64_t)-EINVAL;
            cur->no_new_privs = 1;
            return 0;
        case PR_GET_NO_NEW_PRIVS:
            return cur->no_new_privs ? 1 : 0;
        case PR_SET_THP_DISABLE:
            cur->thp_disabled = (uint8_t)(a2 != 0);
            return 0;
        case PR_GET_THP_DISABLE:
            return cur->thp_disabled ? 1 : 0;
        case PR_SET_TIMERSLACK:
            cur->timer_slack_ns = a2;
            return 0;
        case PR_GET_TIMERSLACK:
            return cur->timer_slack_ns ? cur->timer_slack_ns : 50000ULL;
        case PR_GET_SECCOMP:
            return 0;
        case PR_SET_PDEATHSIG:
        case PR_SET_PTRACER:
        case PR_SET_VMA:
            return 0;
        case PR_GET_PDEATHSIG: {
            if (!user_buf_writable(cur, a2, sizeof(int))) return (uint64_t)-EFAULT;
            int zero = 0;
            return (uint64_t)copyout(cur, &zero, a2, sizeof(zero));
        }
        default:
            return (uint64_t)-EINVAL;
        }
    }

    case LNX_rt_sigaction:
        return lx_rt_sigaction_sys(cur, a1, a2, a3, a4);

    case LNX_rt_sigprocmask:
        return lx_rt_sigprocmask_sys(cur, a1, a2, a3, a4);

    case LNX_rt_sigreturn:
        return lx_rt_sigreturn_sys(cur);

    case LNX_rt_sigsuspend:
        return lx_rt_sigsuspend_sys(cur, a1, a2);

    case LNX_sigaltstack:
        return lx_sigaltstack_sys(cur, a1, a2);

    case LNX_sched_yield:
        sched_yield();
        return 0;

    case LNX_sched_setscheduler: {
        int policy = (int)a2;
        uint64_t param = a3;
        const int sched_reset_on_fork = 0x40000000;
        int base_policy = policy & ~sched_reset_on_fork;
        if (a1 != 0) {
            struct fry_process *target = 0;
            for (uint32_t i = 0; i < PROC_MAX; i++) {
                if (procs[i].pid == (uint32_t)a1 &&
                    procs[i].state != PROC_UNUSED &&
                    procs[i].state != PROC_DEAD) {
                    target = &procs[i];
                    break;
                }
            }
            if (!target || target->tgid != cur->tgid) return (uint64_t)-ESRCH;
        }
        if (param && !user_buf_mapped(cur, param, sizeof(int))) return (uint64_t)-EFAULT;
        if (base_policy != 0) return (uint64_t)-EINVAL; /* SCHED_OTHER only. */
        return 0;
    }

    case LNX_sched_getaffinity: {
        uint64_t cpusetsize = a2;
        uint32_t ncpu = smp_cpu_count();
        uint64_t mask_bytes = linux_affinity_mask_bytes(ncpu);
        if (cpusetsize < mask_bytes) return (uint64_t)-EINVAL;
        if (!user_buf_writable(cur, a3, mask_bytes)) return (uint64_t)-EFAULT;
        uint8_t mask[128];
        for (uint32_t i = 0; i < sizeof(mask); i++) mask[i] = 0;
        if (ncpu == 0) ncpu = 1;
        for (uint32_t i = 0; i < ncpu && i < sizeof(mask) * 8 && i < cpusetsize * 8; i++)
            mask[i / 8] |= (uint8_t)(1u << (i % 8));
        if (copyout(cur, mask, a3, mask_bytes) != 0) return (uint64_t)-EFAULT;
        return mask_bytes;
    }

    case LNX_close_range: {
        uint32_t first = (uint32_t)a1;
        uint32_t last = (uint32_t)a2;
        uint32_t flags = (uint32_t)a3;
        if (first > last) return (uint64_t)-EINVAL;
        if (flags & ~0x6u) return (uint64_t)-EINVAL; /* UNSHARE/CLOEXEC accepted as local no-ops. */
        if (first >= FRY_FD_MAX) return 0;
        if (last >= FRY_FD_MAX) last = FRY_FD_MAX - 1;
        if (flags & 0x4u) return 0; /* CLOEXEC: fd table has no exec transition yet. */
        for (uint32_t fd = first; fd <= last; fd++)
            (void)lx_close_fd(cur, (int)fd);
        return 0;
    }

    case LNX_kill: {
        int pid = (int)a1;
        int sig = (int)a2;
        struct fry_process *target = 0;
        if (pid > 0) {
            target = proc_find_group_leader((uint32_t)pid);
            if (!target) target = proc_find_task((uint32_t)pid);
        } else if (pid == 0) {
            target = proc_find_group_leader(cur->tgid);
            if (!target) target = cur;
        } else {
            return (uint64_t)-EINVAL;
        }
        return lx_send_signal(cur, target, sig);
    }

    case LNX_tkill: {
        uint32_t tid = (uint32_t)a1;
        int sig = (int)a2;
        struct fry_process *target = proc_find_task(tid);
        return lx_send_signal(cur, target, sig);
    }

    case LNX_tgkill: {
        uint32_t tgid = (uint32_t)a1;
        uint32_t tid = (uint32_t)a2;
        int sig = (int)a3;
        struct fry_process *target = proc_find_task(tid);
        if (!target || target->tgid != tgid) return (uint64_t)-ESRCH;
        return lx_send_signal(cur, target, sig);
    }

    case LNX_rseq:
        if (a1 == 0) {
            cur->linux_rseq_addr = 0;
            cur->linux_rseq_len = 0;
            cur->linux_rseq_sig = 0;
            return 0;
        }
        if (a2 < 32 || a2 > 4096) return (uint64_t)-EINVAL;
        if (!user_buf_writable(cur, a1, a2)) return (uint64_t)-EFAULT;
        cur->linux_rseq_addr = a1;
        cur->linux_rseq_len = (uint32_t)a2;
        cur->linux_rseq_sig = (uint32_t)a4;
        return (uint64_t)lx_rseq_update_cpu(cur);
    case LNX_get_robust_list:
        return (uint64_t)-ENOSYS;

    case LNX_futex: {
        int futex_op = (int)a2;
        uint32_t op = (uint32_t)futex_op;
        uint32_t cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
        int private_key = (op & FUTEX_PRIVATE_FLAG) != 0;
        int frc;
#define LX_FUTEX_RETURN(expr) do { \
            frc = (expr); \
            return (uint64_t)frc; \
        } while (0)

        if (cmd == FUTEX_WAIT) {
            int has_timeout = a4 != 0;
            uint64_t timeout_ms = 0;
            if (has_timeout) {
                if (!user_buf_mapped(cur, a4, 16)) return (uint64_t)-EFAULT;
                uint64_t *ts = (uint64_t *)(uintptr_t)a4;
                uint64_t sec = ts[0];
                uint64_t nsec = ts[1];
                if (nsec >= 1000000000ULL) return (uint64_t)-EINVAL;
                timeout_ms = sec * 1000ULL + nsec / 1000000ULL;
                if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
                    kprint_serial_only("TBFUTEX timeout-rel pid=%u tgid=%u cmd=WAIT sec=%llu nsec=%llu rel_ms=%llu\n",
                                       cur->pid, cur->tgid,
                                       (unsigned long long)sec,
                                       (unsigned long long)nsec,
                                       (unsigned long long)timeout_ms);
                }
            }
            LX_FUTEX_RETURN(futex_wait_begin_ex(cur, a1, (uint32_t)a3,
                                                has_timeout, timeout_ms,
                                                FUTEX_BITSET_MATCH_ANY,
                                                private_key));
        }

        if (cmd == FUTEX_WAIT_BITSET) {
            int has_timeout = a4 != 0;
            uint64_t timeout_ms = 0;
            uint32_t bitset = (uint32_t)a6;
            if (bitset == 0) return (uint64_t)-EINVAL;
            if (has_timeout) {
                if (!user_buf_mapped(cur, a4, 16)) return (uint64_t)-EFAULT;
                uint64_t *ts = (uint64_t *)(uintptr_t)a4;
                uint64_t sec = ts[0];
                uint64_t nsec = ts[1];
                uint64_t target_ms;
                uint64_t now_ms;
                if (nsec >= 1000000000ULL) return (uint64_t)-EINVAL;
                target_ms = sec * 1000ULL + nsec / 1000000ULL;
                if (op & FUTEX_CLOCK_REALTIME) {
                    int64_t rtc_boot_epoch_sec(void);
                    now_ms = (uint64_t)(rtc_boot_epoch_sec() * 1000LL) + sys_now_ms();
                } else {
                    now_ms = sys_now_ms();
                }
                timeout_ms = (target_ms > now_ms) ? (target_ms - now_ms) : 0;
                if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
                    kprint_serial_only("TBFUTEX timeout-abs pid=%u tgid=%u cmd=WAIT_BITSET realtime=%u sec=%llu nsec=%llu target_ms=%llu now_ms=%llu rel_ms=%llu\n",
                                       cur->pid, cur->tgid,
                                       (op & FUTEX_CLOCK_REALTIME) ? 1u : 0u,
                                       (unsigned long long)sec,
                                       (unsigned long long)nsec,
                                       (unsigned long long)target_ms,
                                       (unsigned long long)now_ms,
                                       (unsigned long long)timeout_ms);
                }
            }
            LX_FUTEX_RETURN(futex_wait_begin_ex(cur, a1, (uint32_t)a3,
                                                has_timeout, timeout_ms, bitset,
                                                private_key));
        }

        if (cmd == FUTEX_WAKE) {
            LX_FUTEX_RETURN(futex_wake_waiters_bitset(cur, a1, (uint32_t)a3,
                                                      FUTEX_BITSET_MATCH_ANY,
                                                      private_key));
        }

        if (cmd == FUTEX_WAKE_BITSET) {
            LX_FUTEX_RETURN(futex_wake_waiters_bitset(cur, a1, (uint32_t)a3,
                                                      (uint32_t)a6,
                                                      private_key));
        }

        if (cmd == FUTEX_REQUEUE) {
            LX_FUTEX_RETURN(futex_requeue_waiters(cur, a1, (uint32_t)a3,
                                                  a5, (uint32_t)a4, 0, 0,
                                                  private_key));
        }

        if (cmd == FUTEX_CMP_REQUEUE) {
            LX_FUTEX_RETURN(futex_requeue_waiters(cur, a1, (uint32_t)a3,
                                                  a5, (uint32_t)a4, 1,
                                                  (uint32_t)a6, private_key));
        }

        if (cmd == FUTEX_WAKE_OP) {
            LX_FUTEX_RETURN(futex_wake_op(cur, a1, (uint32_t)a3,
                                          a5, (uint32_t)a4, (uint32_t)a6,
                                          private_key));
        }

        kprint_serial_only("LINUXSYS: futex unsupported cmd=%u op=%x pid=%u\n",
                           cmd, op, cur->pid);
#undef LX_FUTEX_RETURN
        return (uint64_t)-ENOSYS;
    }

    case LNX_clock_nanosleep: {
        int clockid = (int)a1;
        int flags   = (int)a2;
        uint64_t req = a3;
        uint64_t rem = a4;
        if (!user_buf_mapped(cur, req, 16)) return (uint64_t)-EFAULT;
        if (rem && !user_buf_writable(cur, rem, 16)) return (uint64_t)-EFAULT;
        if (clockid != LNX_CLOCK_REALTIME && clockid != LNX_CLOCK_MONOTONIC)
            return (uint64_t)-EINVAL;
        uint64_t *r = (uint64_t *)(uintptr_t)req;
        uint64_t sec  = r[0];
        uint64_t nsec = r[1];
        if (nsec >= 1000000000ULL) return (uint64_t)-EINVAL;
        uint64_t now = sys_now_ms();
        uint64_t target = now + sec * 1000ULL + nsec / 1000000ULL;
        if (flags & 1 /* TIMER_ABSTIME */) {
            /* Absolute time — convert to relative for our scheduler */
            uint64_t ms_now = sec * 1000ULL + nsec / 1000000ULL;
            if (ms_now <= now) return 0;
            target = ms_now;
        }
        cur->wake_time_ms = target;
        if (target > now)
            sched_sleep(cur->pid, (uint64_t)(target - now));
        else
            sched_yield();
        if (rem) {
            uint64_t *rm = (uint64_t *)(uintptr_t)rem;
            rm[0] = 0; rm[1] = 0;
        }
        return 0;
    }

    case LNX_fcntl: {
        int fd = (int)a1;
        int cmd = (int)a2;
        uint64_t arg = a3;
        struct fry_process_shared *shared = proc_shared_state(cur);
        if (fd < 0 || fd >= FRY_FD_MAX || !shared) return (uint64_t)-EBADF;
        switch (cmd) {
        case 0:  /* F_DUPFD */
            return (uint64_t)lx_dup_fd_at(cur, fd, (int)arg, 0);
        case 1:  /* F_GETFD */
            return (shared->fd_flags[fd] & 1 /* FD_CLOEXEC */) ? 1 : 0;
        case 2:  /* F_SETFD */
            shared->fd_flags[fd] = (shared->fd_flags[fd] & ~1u) | ((uint32_t)arg & 1u);
            return 0;
        case 3:  /* F_GETFL */
            return (uint64_t)(int64_t)shared->fd_flags[fd];
        case 4:  /* F_SETFL */
            shared->fd_flags[fd] = (shared->fd_flags[fd] & ~0x3FFF) | ((uint32_t)arg & 0x3FFF);
            return 0;
        case 1030:  /* F_DUPFD_CLOEXEC */
            return (uint64_t)lx_dup_fd_at(cur, fd, (int)arg, 1u);
        default:
            return (uint64_t)-EINVAL;
        }
    }

    case LNX_epoll_create: {
        int size = (int)a1;
        if (size <= 0) return (uint64_t)-EINVAL;
        return (uint64_t)lx_epoll_create_fd(cur, 0);
    }

    case LNX_epoll_create1:
        return (uint64_t)lx_epoll_create_fd(cur, (uint32_t)a1);

    case LNX_epoll_ctl:
        return (uint64_t)lx_epoll_ctl_fd(cur, (int)a1, (int)a2, (int)a3, a4);

    case LNX_epoll_wait:
        return (uint64_t)lx_epoll_wait_fd(cur, (int)a1, a2, (int)a3, (int)a4);

    case LNX_epoll_pwait:
        /* Signal masks are a no-op until real Linux signal delivery exists. */
        return (uint64_t)lx_epoll_wait_fd(cur, (int)a1, a2, (int)a3, (int)a4);

    case LNX_eventfd:
        return (uint64_t)lx_eventfd_create_fd(cur, a1, 0);

    case LNX_eventfd2:
        return (uint64_t)lx_eventfd_create_fd(cur, a1, (uint32_t)a2);

    case LNX_timerfd_create:
        return (uint64_t)lx_timerfd_create_fd(cur, (int)a1, (uint32_t)a2);

    case LNX_timerfd_settime:
        return (uint64_t)lx_timerfd_settime_fd(cur, (int)a1, (uint32_t)a2, a3, a4);

    case LNX_timerfd_gettime:
        return (uint64_t)lx_timerfd_gettime_fd(cur, (int)a1, a2);

    case LNX_signalfd:
        return (uint64_t)lx_signalfd_fd(cur, (int)a1, a2, a3, 0);

    case LNX_signalfd4:
        return (uint64_t)lx_signalfd_fd(cur, (int)a1, a2, a3, (uint32_t)a4);

    case LNX_dup: {
        int oldfd = (int)a1;
        return (uint64_t)lx_dup_fd_at(cur, oldfd, 0, 0);
    }

    case LNX_dup2: {
        int oldfd = (int)a1, newfd = (int)a2;
        struct fry_process_shared *shared = proc_shared_state(cur);
        if (!shared || oldfd < 0 || oldfd >= FRY_FD_MAX ||
            newfd < 0 || newfd >= FRY_FD_MAX ||
            !fd_is_open(shared, oldfd)) return (uint64_t)-EBADF;
        if (oldfd == newfd) return newfd;
        if (shared->fd_ptrs[newfd]) lx_close_fd(cur, newfd);
        if (newfd < 3) return (uint64_t)-EBADF;
        if (oldfd <= 2) {
            fd_install(cur, newfd, stdio_fd_ptr(oldfd), FD_STDIO, 0);
            return newfd;
        }
        fd_install(cur, newfd, shared->fd_ptrs[oldfd],
                   shared->fd_kind[oldfd], shared->fd_flags[oldfd]);
        shared->fd_table[newfd] = shared->fd_table[oldfd];
        if (shared->fd_paths[oldfd][0]) {
            int prc = fd_path_set(shared, newfd, shared->fd_paths[oldfd]);
            if (prc < 0) {
                fd_release(cur, newfd);
                return (uint64_t)prc;
            }
            if (shared->fd_kind[oldfd] == FD_DIR) shared->fd_ptrs[newfd] = shared->fd_paths[newfd];
        }
        if (shared->fd_kind[oldfd] == FD_PIPE_READ)
            ((struct fry_pipe *)shared->fd_ptrs[oldfd])->readers++;
        else if (shared->fd_kind[oldfd] == FD_PIPE_WRITE)
            ((struct fry_pipe *)shared->fd_ptrs[oldfd])->writers++;
        return newfd;
    }

    case LNX_open: {
        char raw_path[FRY_PATH_MAX];
        if (copy_user_string(cur, a1, raw_path, sizeof(raw_path)) != 0)
            return (uint64_t)-EFAULT;
        return (uint64_t)lx_open_path(cur, FRY_AT_FDCWD, raw_path, a2);
    }

    case LNX_openat: {
        char raw_path[FRY_PATH_MAX];
        int dirfd = (int)a1;
        if (copy_user_string(cur, a2, raw_path, sizeof(raw_path)) != 0)
            return (uint64_t)-EFAULT;
        return (uint64_t)lx_open_path(cur, dirfd, raw_path, a3);
    }

    case LNX_stat:
    case LNX_lstat: {
        char raw_path[FRY_PATH_MAX];
        if (copy_user_string(cur, a1, raw_path, sizeof(raw_path)) != 0)
            return (uint64_t)-EFAULT;
        int rc = lx_path_stat_to_user(cur, FRY_AT_FDCWD, raw_path, a2);
        return (uint64_t)rc;
    }

    case LNX_newfstatat: {
        char raw_path[FRY_PATH_MAX];
        int dirfd = (int)a1;
        uint32_t flags = (uint32_t)a4;
        if (flags & ~(FRY_AT_SYMLINK_NOFOLLOW | FRY_AT_NO_AUTOMOUNT | FRY_AT_EMPTY_PATH))
            return (uint64_t)-EINVAL;
        if (copy_user_string(cur, a2, raw_path, sizeof(raw_path)) != 0)
            return (uint64_t)-EFAULT;
        if ((flags & FRY_AT_EMPTY_PATH) && raw_path[0] == '\0') {
            int rc = lx_fd_stat_to_user(cur, dirfd, a3);
            return (uint64_t)rc;
        }
        int rc = lx_path_stat_to_user(cur, dirfd, raw_path, a3);
        return (uint64_t)rc;
    }

    case LNX_access: {
        char raw_path[FRY_PATH_MAX];
        char path[FRY_PATH_MAX];
        struct vfs_stat st;
        if (copy_user_string(cur, a1, raw_path, sizeof(raw_path)) != 0)
            return (uint64_t)-EFAULT;
        int rpath = resolve_at_path(cur, FRY_AT_FDCWD, raw_path, path);
        if (rpath < 0) return (uint64_t)rpath;
        if (vfs_stat(path, &st) != 0) return (uint64_t)-ENOENT;
        return 0;
    }

    case LNX_readlink: {
        char raw_path[FRY_PATH_MAX];
        char path[FRY_PATH_MAX];
        struct vfs_stat st;
        if (a3 == 0) return (uint64_t)-EINVAL;
        if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
        if (copy_user_string(cur, a1, raw_path, sizeof(raw_path)) != 0)
            return (uint64_t)-EFAULT;
        if (streq_lit(raw_path, "/etc/localtime")) {
            const char *target = "/usr/share/zoneinfo/UTC";
            uint64_t n = 0;
            while (target[n]) n++;
            if (n > a3) n = a3;
            char *dst = (char *)(uintptr_t)a2;
            for (uint64_t i = 0; i < n; i++) dst[i] = target[i];
            return n;
        }
        if (streq_lit(raw_path, "/proc/self/exe")) {
            const char *target = "/nvme/RCLAUDE.LXE";
            uint64_t n = 0;
            while (target[n]) n++;
            if (n > a3) n = a3;
            char *dst = (char *)(uintptr_t)a2;
            for (uint64_t i = 0; i < n; i++) dst[i] = target[i];
            return n;
        }
        int rpath = resolve_at_path(cur, FRY_AT_FDCWD, raw_path, path);
        if (rpath < 0) return (uint64_t)rpath;
        if (vfs_stat(path, &st) != 0) return (uint64_t)-ENOENT;
        return (uint64_t)-EINVAL; /* Existing non-symlink. */
    }

    case LNX_close: {
        int rc = lx_close_fd(cur, (int)a1);
        return (uint64_t)rc;
    }

    case LNX_exit:
        syscall_thread_exit_current((uint32_t)a1);
        return 0;

    case LNX_exit_group:
        syscall_exit_current((uint32_t)a1);
        return 0;

    case LNX_clone3: {
        struct linux_clone_args {
            uint64_t flags;
            uint64_t pidfd;
            uint64_t child_tid;
            uint64_t parent_tid;
            uint64_t exit_signal;
            uint64_t stack;
            uint64_t stack_size;
            uint64_t tls;
            uint64_t set_tid;
            uint64_t set_tid_size;
            uint64_t cgroup;
        } args;
        uint64_t sz = a2;
        if (sz < 88 || sz > sizeof(args)) return (uint64_t)-EINVAL;
        if (!user_buf_mapped(cur, a1, sz)) return (uint64_t)-EFAULT;
        /* copy from user */
        const uint64_t *src = (const uint64_t *)(uintptr_t)a1;
        uint64_t *dst = (uint64_t *)&args;
        for (uint64_t i = 0; i < 11; i++) dst[i] = src[i];
        if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
            kprint_serial_only("TBCLONE3 args pid=%u tgid=%u flags=%lx pidfd=%lx child_tid=%lx parent_tid=%lx exit_signal=%lx stack=%lx stack_size=%lx tls=%lx set_tid=%lx set_tid_size=%lx cgroup=%lx\n",
                               cur->pid, cur->tgid,
                               (unsigned long)args.flags,
                               (unsigned long)args.pidfd,
                               (unsigned long)args.child_tid,
                               (unsigned long)args.parent_tid,
                               (unsigned long)args.exit_signal,
                               (unsigned long)args.stack,
                               (unsigned long)args.stack_size,
                               (unsigned long)args.tls,
                               (unsigned long)args.set_tid,
                               (unsigned long)args.set_tid_size,
                               (unsigned long)args.cgroup);
        }

        /* Only CLONE_THREAD for now */
        uint64_t flags = args.flags;
        if (!(flags & LNX_CLONE_THREAD))
            return (uint64_t)-EINVAL;
        if (flags & LNX_CLONE_PIDFD)
            return (uint64_t)-EINVAL;
        if ((flags & LNX_CLONE_PARENT_SETTID) && args.parent_tid &&
            !user_buf_writable(cur, args.parent_tid, sizeof(uint32_t)))
            return (uint64_t)-EFAULT;
        if ((flags & LNX_CLONE_CHILD_SETTID) && args.child_tid &&
            !user_buf_writable(cur, args.child_tid, sizeof(uint32_t)))
            return (uint64_t)-EFAULT;
        if ((flags & LNX_CLONE_CHILD_CLEARTID) && args.child_tid &&
            !user_buf_writable(cur, args.child_tid, sizeof(uint32_t)))
            return (uint64_t)-EFAULT;

        /* Read the syscall-entry frame from this task's kernel stack.
         * syscall_entry builds this fixed frame at kernel_stack_top - 96:
         *   [a6,pad,r9,r8,r10,rdx,rsi,rdi,rbp,r11,rcx,user_rsp]
         *
         * Do not read the frame through per-CPU scratch here. Interrupts are
         * enabled while syscall_dispatch runs, so another task on the same CPU
         * can overwrite percpu.linux_frame_rsp before clone3 snapshots it. */
        uint64_t frame_rsp = cur->kernel_stack_top - (12ULL * sizeof(uint64_t));
        uint64_t *frame = (uint64_t *)(uintptr_t)frame_rsp;
        uint64_t user_rip = frame[10];
        uint64_t user_rsp_saved = frame[11];

        /* clone3 semantics differ from legacy clone(2): cl_args.stack points
         * at the LOWEST address of the stack region and the kernel sets the
         * child SP to stack + stack_size. (Legacy clone() passed the top
         * directly.) Using args.stack as-is makes the child's first push land
         * one slot below the mapped region -> page fault at stack-8. */
        uint64_t child_rsp = args.stack ? (args.stack + args.stack_size)
                                        : user_rsp_saved;
        if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
            kprint_serial_only("TBCLONE3 frame pid=%u tgid=%u user_rip=%lx user_rsp=%lx child_rsp=%lx frame_rsp=%lx\n",
                               cur->pid, cur->tgid,
                               (unsigned long)user_rip,
                               (unsigned long)user_rsp_saved,
                               (unsigned long)child_rsp,
                               (unsigned long)frame_rsp);
        }
        struct fry_process *child = process_clone_linux_thread(
            cur, user_rip, child_rsp,
            args.tls, args.child_tid, args.parent_tid);
        if (!child) return (uint64_t)-ENOMEM;

        /* Snapshot the FULL parent register set into the child so its first
         * run (process_start clone path) iretqs with everything restored.
         * glibc carries the thread fn/arg in callee-saved regs across the
         * clone syscall. rbx/r12-r15 come from per-CPU scratch (gs:24..56)
         * captured in syscall_entry; the rest from the interrupt frame. */
        uint64_t sv_rbx, sv_r12, sv_r13, sv_r14, sv_r15;
        __asm__ volatile("movq %%gs:24, %0" : "=r"(sv_rbx));
        __asm__ volatile("movq %%gs:32, %0" : "=r"(sv_r12));
        __asm__ volatile("movq %%gs:40, %0" : "=r"(sv_r13));
        __asm__ volatile("movq %%gs:48, %0" : "=r"(sv_r14));
        __asm__ volatile("movq %%gs:56, %0" : "=r"(sv_r15));
        child->clone_ctx.rax = 0;            /* clone returns 0 in the child */
        child->clone_ctx.rbx = sv_rbx;
        child->clone_ctx.rcx = 0;
        child->clone_ctx.rdx = frame[5];
        child->clone_ctx.rsi = frame[6];
        child->clone_ctx.rdi = frame[7];
        child->clone_ctx.rbp = frame[8];
        child->clone_ctx.r8  = frame[3];
        child->clone_ctx.r9  = frame[2];
        child->clone_ctx.r10 = frame[4];
        child->clone_ctx.r11 = frame[9];
        child->clone_ctx.r12 = sv_r12;
        child->clone_ctx.r13 = sv_r13;
        child->clone_ctx.r14 = sv_r14;
        child->clone_ctx.r15 = sv_r15;
        child->clone_ctx.rip = user_rip;
        child->clone_ctx.rsp = child_rsp;
        child->clone_ctx.rflags = frame[9] | 0x202ULL;

        /* Write TID notifications only when clone flags request them. */
        if ((flags & LNX_CLONE_CHILD_SETTID) && args.child_tid)
            *(uint32_t *)(uintptr_t)args.child_tid = child->pid;
        if ((flags & LNX_CLONE_PARENT_SETTID) && args.parent_tid)
            *(uint32_t *)(uintptr_t)args.parent_tid = child->pid;
        child->linux_clear_child_tid =
            (flags & LNX_CLONE_CHILD_CLEARTID) ? args.child_tid : 0;
        if (cur && (cur->pid == TB_TRACE_CLAUDE_TGID || cur->tgid == TB_TRACE_CLAUDE_TGID)) {
            uint32_t child_tid_val = 0xffffffffU;
            uint32_t parent_tid_val = 0xffffffffU;
            if (args.child_tid && user_buf_mapped(cur, args.child_tid, 4))
                child_tid_val = *(uint32_t *)(uintptr_t)args.child_tid;
            if (args.parent_tid && user_buf_mapped(cur, args.parent_tid, 4))
                parent_tid_val = *(uint32_t *)(uintptr_t)args.parent_tid;
            kprint_serial_only("TBCLONE3 child pid=%u tgid=%u child_tid=%lx val=%u parent_tid=%lx val=%u clear_child_tid=%lx tls=%lx\n",
                               child->pid, child->tgid,
                               (unsigned long)args.child_tid, child_tid_val,
                               (unsigned long)args.parent_tid, parent_tid_val,
                               (unsigned long)child->linux_clear_child_tid,
                               (unsigned long)args.tls);
        }

        sched_add(child->pid);
        return child->pid;
    }

    default:
        kprint("LINUXSYS: unimplemented nr=%lu (a1=%lx a2=%lx a3=%lx) pid=%u\n",
               (unsigned long)num, (unsigned long)a1, (unsigned long)a2,
               (unsigned long)a3, cur->pid);
        return (uint64_t)-ENOSYS;
    }
}

uint64_t linux_syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5,
                                uint64_t a6,
                                struct fry_process *cur) {
    uint32_t trace_pid = cur ? cur->pid : 0;
    uint32_t trace_tgid = cur ? cur->tgid : 0;
    int is_claude = tb_trace_is_claude(cur);
    int trace = g_tb_trace_syscalls && is_claude && tb_trace_selected_syscall(num);
    if (trace) {
        kprint_serial_only("TBTRACE enter pid=%u tgid=%u nr=%lu %s a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx a6=%lx\n",
                           trace_pid, trace_tgid, (unsigned long)num,
                           tb_trace_syscall_name(num),
                           (unsigned long)a1, (unsigned long)a2,
                           (unsigned long)a3, (unsigned long)a4,
                           (unsigned long)a5, (unsigned long)a6);
        if (num == LNX_open || num == LNX_openat || num == LNX_access || num == LNX_newfstatat) {
            char tb_path[128];
            uint64_t path_ptr = (num == LNX_open || num == LNX_access) ? a1 : a2;
            if (copy_user_string(cur, path_ptr, tb_path, sizeof(tb_path)) == 0) {
                kprint_serial_only("TBTRACE path pid=%u tgid=%u nr=%lu %s path=\"%s\"\n",
                                   trace_pid, trace_tgid, (unsigned long)num,
                                   tb_trace_syscall_name(num), tb_path);
            } else {
                kprint_serial_only("TBTRACE path pid=%u tgid=%u nr=%lu %s path=<copy-failed>\n",
                                   trace_pid, trace_tgid, (unsigned long)num,
                                   tb_trace_syscall_name(num));
            }
        }
    }
    uint64_t rc = linux_syscall_dispatch_impl(num, a1, a2, a3, a4, a5, a6, cur);
    rc = lx_deliver_signal_on_sysret(cur, rc);
    if (cur && cur->linux_rseq_addr)
        (void)lx_rseq_update_cpu(cur);
    if (trace || (is_claude && tb_trace_is_errno(rc))) {
        kprint_serial_only("TBTRACE ret pid=%u tgid=%u nr=%lu %s rc=%lx\n",
                           trace_pid, trace_tgid,
                           (unsigned long)num, tb_trace_syscall_name(num),
                           (unsigned long)rc);
    }
    return rc;
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    struct fry_process *cur = proc_current();
    note_user_boot_progress(cur);
    if (cur && cur->is_linux)
        return linux_syscall_dispatch(num, a1, a2, a3, a4, a5, a6, cur);
    switch (num) {
        case SYS_WRITE: {
            int fd = (int)a1;
            struct fry_process_shared *shared = proc_shared_state(cur);
            if (!user_buf_mapped(cur, a2, a3)) {
                return (uint64_t)-EFAULT;
            }
            const char *buf = (const char *)(uintptr_t)a2;
            if (fd == 1) {
                int tw_msg = is_taterwin_msg(buf, a3);
                /* In GUI mode, user stdout should be window-routed only.
                   Keep direct console rendering only for no-GUI fallback mode. */
                if (!tw_msg && !gui_process_running()) {
                    kprint_write(buf, a3);
                }
                /* also capture into the per-process stdout ring buffer */
                if (shared) {
                    for (uint64_t _i = 0; _i < a3; _i++) {
                        uint32_t nt = (shared->outbuf_tail + 1u) % PROC_OUTBUF;
                        if (nt != shared->outbuf_head) {
                            shared->outbuf[shared->outbuf_tail] = (uint8_t)buf[_i];
                            shared->outbuf_tail = nt;
                        }
                        /* ring full: silently drop oldest byte */
                    }
                }
                return a3;
            }
            if (fd == 2) {
                kprint_serial_write(buf, a3);
                return a3;
            }
            if (fd >= 3 && fd < FRY_FD_MAX && shared && shared->fd_ptrs[fd]) {
                uint8_t kind = shared->fd_kind[fd];
                if (kind == FD_FILE) {
                    return (uint64_t)vfs_write((struct vfs_file *)shared->fd_ptrs[fd],
                                               (const void *)buf, (uint32_t)a3);
                }
                if (kind == FD_PIPE_WRITE) {
                    struct fry_pipe *pp = (struct fry_pipe *)shared->fd_ptrs[fd];
                    uint32_t wrflags = shared->fd_flags[fd];
                    int64_t ret = pipe_write(pp, buf, a3, wrflags);
                    if (ret == -EAGAIN && !(wrflags & O_NONBLOCK) && pp->readers > 0) {
                        /* Block until space available or readers close */
                        sched_block_poll(cur->pid, UINT64_MAX);
                        sched_yield();
                        cur = proc_current();
                        if (!cur) return (uint64_t)-ESRCH;
                        shared = proc_shared_state(cur);
                        if (!shared || !shared->fd_ptrs[fd]) return (uint64_t)-EBADF;
                        pp = (struct fry_pipe *)shared->fd_ptrs[fd];
                        ret = pipe_write(pp, buf, a3, wrflags | O_NONBLOCK);
                    }
                    return (uint64_t)ret;
                }
                if (kind == FD_PIPE_READ) return (uint64_t)-EBADF;
                if (kind == FD_EVENTFD) {
                    /* write to eventfd: add value to counter */
                    struct eventfd_cb *ev = (struct eventfd_cb *)shared->fd_ptrs[fd];
                    if (!ev) return (uint64_t)-EBADF;
                    uint64_t val;
                    if (a3 < 8) return (uint64_t)-EINVAL;
                    copyin(cur, a2, &val, 8);
                    while (__sync_lock_test_and_set(&ev->lock, 1)) {}
                    if (val == 0xFFFFFFFFFFFFFFFFULL && ev->counter == 0xFFFFFFFFFFFFFFFFULL) {
                        ev->lock = 0;
                        return (uint64_t)-EAGAIN;
                    }
                    uint64_t newval = ev->counter + val;
                    if (newval < ev->counter) newval = UINT64_MAX;
                    ev->counter = newval;
                    ev->lock = 0;
                    sched_wake_poll_waiters();
                    return 8;
                }
                if (kind == FD_MEMFD) {
                    struct memfd_cb *mf = (struct memfd_cb *)shared->fd_ptrs[fd];
                    if (!mf || !mf->used) return (uint64_t)-EBADF;
                    if (a3 == 0) return 0;
                    if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-EFAULT;
                    int64_t ret = memfd_write(mf, (const void *)(uintptr_t)a2, a3);
                    if (ret < 0) return (uint64_t)ret;
                    return (uint64_t)ret;
                }
                if (kind == FD_SOCKET) {
                    struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[fd];
                    if (!sk || !sk->used) return (uint64_t)-EBADF;
                    if (sk->type == SOCK_STREAM) {
                        /* AF_UNIX socketpair: route through pipe buffers */
                        if (sk->domain == 1 && sk->tcp_handle >= 0 && sk->tcp_handle < FRY_PIPE_MAX) {
                            struct fry_pipe *pp = &g_pipes[sk->tcp_handle];
                            if (!pp->used) return (uint64_t)-ENOTCONN;
                            uint32_t wrflags = shared->fd_flags[fd];
                            int64_t sent = pipe_write(pp, (const char *)buf, a3, wrflags);
                            if (sent < 0 && sent != -EAGAIN) return (uint64_t)sent;
                            if (sent == -EAGAIN) {
                                if (wrflags & O_NONBLOCK) return (uint64_t)-EAGAIN;
                                sched_block_poll(cur->pid, UINT64_MAX);
                                sched_yield();
                                cur = proc_current();
                                if (!cur) return (uint64_t)-ESRCH;
                                shared = proc_shared_state(cur);
                                if (!shared || !shared->fd_ptrs[fd]) return (uint64_t)-EBADF;
                                sk = (struct fry_socket *)shared->fd_ptrs[fd];
                                if (!sk || !sk->used || sk->tcp_handle < 0) return (uint64_t)-EBADF;
                                pp = &g_pipes[sk->tcp_handle];
                                sent = pipe_write(pp, (const char *)buf, a3, 0);
                            }
                            if (sent < 0) return (uint64_t)sent;
                            return (uint64_t)sent;
                        }
                        if (sk->state != SOCK_ST_CONNECTED) return (uint64_t)-ENOTCONN;
                        if (sk->tcp_handle < 0) return (uint64_t)-ENOTCONN;
                        int sent = tcp_send(sk->tcp_handle, (const uint8_t *)buf, (uint16_t)a3);
                        if (sent < 0) return (uint64_t)-EIO;
                        return (uint64_t)sent;
                    }
                    if (sk->type == SOCK_DGRAM) {
                        if (sk->remote_ip == 0 && sk->remote_port == 0)
                            return (uint64_t)-EDESTADDRREQ;
                        int r = udp_send(sk->remote_ip, sk->remote_port,
                                         sk->local_port, (const uint8_t *)buf, (uint16_t)a3);
                        return r == 0 ? (uint64_t)a3 : (uint64_t)-EIO;
                    }
                    return (uint64_t)-EBADF;
                }
            }
            return (uint64_t)-EBADF;
        }
        case SYS_READ: {
            int fd = (int)a1;
            struct fry_process_shared *shared = proc_shared_state(cur);
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
            char *buf = (char *)(uintptr_t)a2;
            if (fd == 0) {
                if (shared && shared->inbuf_head != shared->inbuf_tail) {
                    uint64_t nr = 0;
                    while (nr < a3 && shared->inbuf_head != shared->inbuf_tail) {
                        buf[nr++] = (char)shared->inbuf[shared->inbuf_head];
                        shared->inbuf_head = (shared->inbuf_head + 1u) % PROC_INBUF;
                    }
                    return nr;
                }
                int n = ps2_kbd_read(buf, (uint32_t)a3);
                if (n > 0) return (uint64_t)n;
                uint64_t sn = kread_serial(buf, a3);
                if (sn > 0) return sn;
                if (cur) sched_yield();
                return 0;
            }
            if (fd >= 3 && fd < FRY_FD_MAX && shared && shared->fd_ptrs[fd]) {
                uint8_t kind = shared->fd_kind[fd];
                if (kind == FD_FILE) {
                    return (uint64_t)vfs_read((struct vfs_file *)shared->fd_ptrs[fd],
                                              buf, (uint32_t)a3);
                }
                if (kind == FD_PIPE_READ) {
                    struct fry_pipe *pp = (struct fry_pipe *)shared->fd_ptrs[fd];
                    uint32_t rdflags = shared->fd_flags[fd];
                    int64_t ret = pipe_read(pp, buf, a3, rdflags);
                    if (ret == -EAGAIN && !(rdflags & O_NONBLOCK) && pp->writers > 0) {
                        /* Block until data available or writers close */
                        sched_block_poll(cur->pid, UINT64_MAX);
                        sched_yield();
                        /* Re-read after wake */
                        cur = proc_current();
                        if (!cur) return (uint64_t)-ESRCH;
                        shared = proc_shared_state(cur);
                        if (!shared || !shared->fd_ptrs[fd]) return (uint64_t)-EBADF;
                        pp = (struct fry_pipe *)shared->fd_ptrs[fd];
                        ret = pipe_read(pp, buf, a3, rdflags | O_NONBLOCK);
                        if (ret == -EAGAIN && pp->writers == 0) ret = 0; /* EOF */
                    }
                    return (uint64_t)ret;
                }
                if (kind == FD_PIPE_WRITE) return (uint64_t)-EBADF;
                if (kind == FD_EVENTFD) {
                    struct eventfd_cb *ev = (struct eventfd_cb *)shared->fd_ptrs[fd];
                    if (!ev) return (uint64_t)-EBADF;
                    while (__sync_lock_test_and_set(&ev->lock, 1)) {}
                    if (ev->counter == 0) {
                        ev->lock = 0;
                        if (ev->nonblock) return (uint64_t)-EAGAIN;
                        /* Block — yield and retry later */
                        ev->lock = 0;
                        return (uint64_t)-EAGAIN;
                    }
                    uint64_t val;
                    if (ev->semaphore) {
                        val = 1;
                        ev->counter--;
                    } else {
                        val = ev->counter;
                        ev->counter = 0;
                    }
                    ev->lock = 0;
                    copyout(cur, &val, a2, 8);
                    sched_wake_poll_waiters();
                    return 8;
                }
                if (kind == FD_TIMERFD) {
                    struct timerfd_cb *tm = (struct timerfd_cb *)shared->fd_ptrs[fd];
                    if (!tm || !tm->used) return (uint64_t)-EBADF;
                    timerfd_update_expirations(tm, sys_now_ms());
                    if (tm->expirations == 0) {
                        if (tm->nonblock) return (uint64_t)-EAGAIN;
                        return (uint64_t)-EAGAIN;
                    }
                    uint64_t val = tm->expirations;
                    tm->expirations = 0;
                    copyout(cur, &val, a2, 8);
                    return 8;
                }
                if (kind == FD_INOTIFY) {
                    struct inotify_cb *in = (struct inotify_cb *)shared->fd_ptrs[fd];
                    if (!in || !in->used) return (uint64_t)-EBADF;
                    if (in->ev_head == in->ev_tail) {
                        if (in->nonblock) return (uint64_t)-EAGAIN;
                        return (uint64_t)-EAGAIN;
                    }
                    struct inotify_event_buf *ev = &in->events[in->ev_tail];
                    uint32_t ev_size = sizeof(int32_t) * 3 + sizeof(uint32_t) + ev->len;
                    if (ev_size > (uint32_t)a3) ev_size = (uint32_t)a3;
                    uint8_t *out = (uint8_t *)buf;
                    *(int32_t *)(out + 0) = ev->wd;
                    *(uint32_t *)(out + 4) = ev->mask;
                    *(uint32_t *)(out + 8) = ev->cookie;
                    *(uint32_t *)(out + 12) = ev->len;
                    for (uint32_t i = 0; i < ev->len && i < (uint32_t)INOTIFY_NAME_MAX && (16 + i) < ev_size; i++)
                        out[16 + i] = (uint8_t)ev->name[i];
                    in->ev_tail = (in->ev_tail + 1) % INOTIFY_EVENT_MAX;
                    return (uint64_t)ev_size;
                }
                if (kind == FD_MEMFD) {
                    struct memfd_cb *mf = (struct memfd_cb *)shared->fd_ptrs[fd];
                    if (!mf || !mf->used) return (uint64_t)-EBADF;
                    int64_t ret = memfd_read(mf, buf, a3);
                    if (ret < 0) return (uint64_t)ret;
                    return (uint64_t)ret;
                }
                if (kind == FD_SOCKET) {
                    struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[fd];
                    if (!sk || !sk->used) return (uint64_t)-EBADF;
                    if (sk->type == SOCK_STREAM) {
                        /* AF_UNIX socketpair: route through pipe buffers */
                        if (sk->domain == 1 && sk->listen_handle >= 0 && sk->listen_handle < FRY_PIPE_MAX) {
                            struct fry_pipe *pp = &g_pipes[sk->listen_handle];
                            if (!pp->used) {
                                if (pp->writers == 0) return 0;
                                return (uint64_t)-ENOTCONN;
                            }
                            uint32_t rdflags = shared->fd_flags[fd];
                            int64_t nr = pipe_read(pp, buf, a3, rdflags);
                            if (nr > 0) return (uint64_t)nr;
                            if (pp->writers == 0) return 0;
                            if (nr == -EAGAIN) {
                                if (rdflags & O_NONBLOCK) return (uint64_t)-EAGAIN;
                                sched_block_poll(cur->pid, UINT64_MAX);
                                sched_yield();
                                cur = proc_current();
                                if (!cur) return (uint64_t)-ESRCH;
                                shared = proc_shared_state(cur);
                                if (!shared || !shared->fd_ptrs[fd]) return (uint64_t)-EBADF;
                                sk = (struct fry_socket *)shared->fd_ptrs[fd];
                                if (!sk || !sk->used) return (uint64_t)-EBADF;
                                pp = &g_pipes[sk->listen_handle];
                                nr = pipe_read(pp, buf, a3, 0);
                                if (nr > 0) return (uint64_t)nr;
                                if (pp->writers == 0) return 0;
                                return (uint64_t)-EAGAIN;
                            }
                            return 0;
                        }
                        if (sk->state != SOCK_ST_CONNECTED) return (uint64_t)-ENOTCONN;
                        if (sk->tcp_handle < 0) return (uint64_t)-ENOTCONN;
                        /* Poll network before reading */
                        net_poll();
                        int nr = tcp_recv(sk->tcp_handle, (uint8_t *)buf, (uint32_t)a3);
                        if (nr > 0) return (uint64_t)nr;
                        /* Check if connection closed */
                        if (!tcp_is_connected(sk->tcp_handle)) return 0; /* EOF */
                        uint32_t rdflags = shared->fd_flags[fd];
                        if (rdflags & O_NONBLOCK) return (uint64_t)-EAGAIN;
                        /* Block briefly and retry once */
                        sched_block_poll(cur->pid, UINT64_MAX);
                        sched_yield();
                        cur = proc_current();
                        if (!cur) return (uint64_t)-ESRCH;
                        shared = proc_shared_state(cur);
                        if (!shared || !shared->fd_ptrs[fd]) return (uint64_t)-EBADF;
                        sk = (struct fry_socket *)shared->fd_ptrs[fd];
                        if (!sk || !sk->used || sk->tcp_handle < 0) return (uint64_t)-EBADF;
                        net_poll();
                        nr = tcp_recv(sk->tcp_handle, (uint8_t *)buf, (uint32_t)a3);
                        if (nr > 0) return (uint64_t)nr;
                        if (!tcp_is_connected(sk->tcp_handle)) return 0;
                        return (uint64_t)-EAGAIN;
                    }
                    if (sk->type == SOCK_DGRAM) {
                        /* Dequeue from UDP receive buffer */
                        if (sk->udp_rx_head == sk->udp_rx_tail) {
                            uint32_t rdflags = shared->fd_flags[fd];
                            if (rdflags & O_NONBLOCK) return (uint64_t)-EAGAIN;
                            /* Block and retry once */
                            net_poll();
                            sched_block_poll(cur->pid, UINT64_MAX);
                            sched_yield();
                            cur = proc_current();
                            if (!cur) return (uint64_t)-ESRCH;
                            shared = proc_shared_state(cur);
                            if (!shared || !shared->fd_ptrs[fd]) return (uint64_t)-EBADF;
                            sk = (struct fry_socket *)shared->fd_ptrs[fd];
                            if (!sk || !sk->used) return (uint64_t)-EBADF;
                            net_poll();
                            if (sk->udp_rx_head == sk->udp_rx_tail)
                                return (uint64_t)-EAGAIN;
                        }
                        struct fry_udp_pkt *pkt = &sk->udp_rxq[sk->udp_rx_tail];
                        uint16_t copylen = pkt->len;
                        if (copylen > (uint16_t)a3) copylen = (uint16_t)a3;
                        for (uint16_t i = 0; i < copylen; i++)
                            buf[i] = (char)pkt->data[i];
                        sk->udp_rx_tail = (sk->udp_rx_tail + 1) % FRY_SOCK_UDP_RXMAX;
                        return (uint64_t)copylen;
                    }
                    return (uint64_t)-EBADF;
                }
            }
            return (uint64_t)-EBADF;
        }
        case SYS_EXIT:
            if (cur) {
                syscall_exit_current((uint32_t)a1);
            }
            return 0;
        case SYS_THREAD_EXIT:
            if (cur) {
                syscall_thread_exit_current((uint32_t)a1);
            }
            return 0;
        case SYS_SPAWN: {
            char path[FRY_PATH_MAX];
            /* Row 1: yellow = spawn entered */
            uint32_t col = g_spawn_attempt_count;
            if (col < 80) boot_diag_color(col, 1, 0x00FFFF00u);
            if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("SPAWN_ENTER path=");
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) {
                /* Row 1: red = copy_user_string failed */
                if (col < 80) boot_diag_color(col, 1, 0x00FF0000u);
                if (col < 80) boot_diag_color(col, 2, 0x00FFFFFFu);
                if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("(copy_fail)\n");
                kprint_serial_only("SPAWN_FAIL path=(copy_fail) rc=%d\n", -EFAULT);
                g_spawn_attempt_count++;
                return (uint64_t)-EFAULT;
            }
            if (TATER_BOOT_SERIAL_TRACE) {
                early_serial_puts(path);
                early_serial_puts("\n");
            }
            int rc = process_launch(path);
            if (rc >= 0) {
                /* Row 1: green = spawn succeeded */
                if (col < 80) boot_diag_color(col, 1, 0x0000FF00u);
                if (col < 80) boot_diag_color(col, 2, 0x0000FF00u);
                kprint_serial_only("SPAWN_OK path=%s pid=%d parent=%u\n",
                    path, rc, cur ? cur->pid : 0);
            } else {
                /* Row 1: red = spawn failed (file not found or ELF error) */
                if (col < 80) boot_diag_color(col, 1, 0x00FF0000u);
                if (col < 80) boot_diag_color(col, 2, boot_diag_spawn_error_color(rc));
                kprint_serial_only("SPAWN_FAIL path=%s rc=%d\n", path, rc);
            }
            g_spawn_attempt_count++;
            if (rc >= 0 &&
                cur &&
                !g_first_init_gui_spawn_seen &&
                process_name_is_init(cur->name) &&
                process_name_is_gui(path)) {
                g_first_init_gui_spawn_seen = 1;
                boot_diag_stage(38);
                if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_INIT_GUI_SPAWN\n");
            }
            return (uint64_t)rc;
        }
        case SYS_SLEEP:
            if (cur) {
                sched_sleep(cur->pid, a1);
                sched_yield();
            }
            return 0;
        case SYS_OPEN: {
            char raw_path[FRY_PATH_MAX];
            char path[FRY_PATH_MAX];
            struct fry_process_shared *shared;
            if (copy_user_string(cur, a1, raw_path, sizeof(raw_path)) != 0) return (uint64_t)-EFAULT;
            if (!cur) return (uint64_t)-ESRCH;
            shared = proc_shared_state(cur);
            if (!shared) return (uint64_t)-ESRCH;
            uint32_t flags = (uint32_t)a2;
            int rpath = resolve_at_path(cur, FRY_AT_FDCWD, raw_path, path);
            if (rpath < 0) return (uint64_t)rpath;

            struct vfs_stat st;
            if (vfs_stat(path, &st) == 0 && (st.attr & 0x10u)) {
                if ((flags & FRY_O_ACCMODE) != O_RDONLY) return (uint64_t)-EISDIR;
                int fd = fd_alloc(cur);
                if (fd < 0) return (uint64_t)-EMFILE;
                fd_install(cur, fd, shared->fd_paths[fd], FD_DIR, flags & O_NONBLOCK);
                shared->fd_table[fd] = 0;
                int prc = install_fd_path(cur, fd, path);
                if (prc < 0) return (uint64_t)prc;
                return (uint64_t)fd;
            }
            if (flags & FRY_O_DIRECTORY) return (uint64_t)-ENOTDIR;

            int fd = fd_alloc(cur);
            if (fd < 0) return (uint64_t)-EMFILE;
            struct vfs_file *f = vfs_open(path);
            if (!f && (flags & O_CREAT)) {
                vfs_create(path, 1);  /* TOTFS_TYPE_FILE */
                f = vfs_open(path);
            }
            if (!f) return (uint64_t)-ENOENT;
            fd_install(cur, fd, f, FD_FILE, flags & O_NONBLOCK);
            int prc = install_fd_path(cur, fd, path);
            if (prc < 0) {
                vfs_close(f);
                return (uint64_t)prc;
            }
            return (uint64_t)fd;
        }
        case SYS_CLOSE: {
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int cfd = (int)a1;
            if (cfd < 3 || cfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            uint8_t ckind = PROC_FD_KIND(cur)[cfd];
            void *cptr = PROC_FD_PTRS(cur)[cfd];
            if (ckind == FD_NONE || !cptr) return (uint64_t)-EBADF;
            if (ckind == FD_FILE) {
                vfs_close((struct vfs_file *)cptr);
            } else if (ckind == FD_PIPE_READ) {
                struct fry_pipe *pp = (struct fry_pipe *)cptr;
                if (pp->readers > 0) pp->readers--;
                if (pp->readers == 0 && pp->writers == 0) {
                    pp->used = 0;
                    pp->head = 0;
                    pp->tail = 0;
                }
                sched_wake_poll_waiters();
            } else if (ckind == FD_PIPE_WRITE) {
                struct fry_pipe *pp = (struct fry_pipe *)cptr;
                if (pp->writers > 0) pp->writers--;
                if (pp->readers == 0 && pp->writers == 0) {
                    pp->used = 0;
                    pp->head = 0;
                    pp->tail = 0;
                }
                sched_wake_poll_waiters();
            } else if (ckind == FD_SOCKET) {
                struct fry_socket *sk = (struct fry_socket *)cptr;
                if (sk && sk->used) {
                    if (sk->type == SOCK_STREAM) {
                        /* AF_UNIX socketpair: close pipe buffers instead of TCP */
                        if (sk->domain == 1 && sk->tcp_handle >= 0 && sk->tcp_handle < FRY_PIPE_MAX) {
                            struct fry_pipe *pp = &g_pipes[sk->tcp_handle];
                            if (pp->writers > 0) pp->writers--;
                            if (pp->readers == 0 && pp->writers == 0) { pp->used = 0; pp->head = 0; pp->tail = 0; }
                            pp = &g_pipes[sk->listen_handle];
                            if (pp->readers > 0) pp->readers--;
                            if (pp->readers == 0 && pp->writers == 0) { pp->used = 0; pp->head = 0; pp->tail = 0; }
                        } else {
                            if (sk->tcp_handle >= 0) tcp_close(sk->tcp_handle);
                            if (sk->listen_handle >= 0) tcp_close(sk->listen_handle);
                        }
                    }
                    sk->used = 0;
                    sk->state = SOCK_ST_CLOSED;
                    sk->tcp_handle = -1;
                    sk->listen_handle = -1;
                }
                sched_wake_poll_waiters();
            } else if (ckind == FD_TIMERFD) {
                struct timerfd_cb *tm = (struct timerfd_cb *)cptr;
                if (tm) { tm->used = 0; kfree(tm); }
            } else if (ckind == FD_SIGNALFD) {
                struct signalfd_cb *sf = (struct signalfd_cb *)cptr;
                if (sf) { sf->used = 0; kfree(sf); }
            } else if (ckind == FD_INOTIFY) {
                struct inotify_cb *in = (struct inotify_cb *)cptr;
                if (in) { in->used = 0; kfree(in); }
            } else if (ckind == FD_MEMFD) {
                struct memfd_cb *mf = (struct memfd_cb *)cptr;
                if (mf) { mf->used = 0; memfd_free_pages(mf); kfree(mf); }
            }
            fd_release(cur, cfd);
            return 0;
        }
        case SYS_GETPID:
            return cur ? process_group_id(cur) : 0;
        case SYS_GETTID:
            return cur ? cur->pid : 0;
        case SYS_SET_TLS_BASE:
            if (!cur || cur->is_kernel) return (uint64_t)-ESRCH;
            if (a1 != 0 && !user_ptr_ok(a1, 1)) return (uint64_t)-EFAULT;
            cur->user_fs_base = a1;
            write_user_fs_base(a1);
            return 0;
        case SYS_GET_TLS_BASE:
            if (!cur || cur->is_kernel) return 0;
            return cur->user_fs_base;
        case SYS_STAT:
            if (!user_buf_writable(cur, a2, sizeof(struct vfs_stat))) return (uint64_t)-EFAULT;
            {
                char path[FRY_PATH_MAX];
                if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
                struct vfs_stat st;
                if (vfs_stat(path, &st) != 0) return (uint64_t)-ENOENT;
                struct vfs_stat *u = (struct vfs_stat *)(uintptr_t)a2;
                *u = st;
                return 0;
        }
        case SYS_READDIR: {
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
            char path[FRY_PATH_MAX];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            struct readdir_ctx ctx = {(char *)(uintptr_t)a2, (uint32_t)a3, 0};
            if (vfs_readdir(path, readdir_cb, &ctx) != 0) return (uint64_t)-ENOENT;
            if (ctx.pos < ctx.len) ctx.buf[ctx.pos] = 0;
            return ctx.pos;
        }
        case SYS_READDIR_EX: {
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
            char path[FRY_PATH_MAX];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            struct readdir_ex_ctx ctx = {(uint8_t *)(uintptr_t)a2, (uint32_t)a3, 0};
            if (vfs_readdir_ex(path, readdir_ex_cb, &ctx) != 0) return (uint64_t)-ENOENT;
            return ctx.pos;
        }
        case SYS_GETTIME: {
            /* Wall-clock ms = RTC epoch captured at boot + HPET uptime since
             * boot. Previously returned uptime only, so libc time() read ~1970
             * and TLS cert validation failed (BR_ERR_X509_EXPIRED). */
            int64_t rtc_boot_epoch_sec(void);
            uint64_t freq = hpet_get_freq_hz();
            uint64_t uptime_ms = (freq == 0) ? 0
                               : (hpet_read_counter() * 1000ULL) / freq;
            return (uint64_t)(rtc_boot_epoch_sec() * 1000LL) + uptime_ms;
        }
        case SYS_REBOOT:
            acpi_reset();
            return 0;
        case SYS_SHUTDOWN:
            acpi_shutdown();
            return 0;
        case SYS_WAIT: {
            int ret = process_wait((uint32_t)a1);
            if (ret < 0) return (uint64_t)ret;
            if (ret > 0) {
                sched_yield();
            }
            return 0;
        }
        case SYS_PROCCOUNT:
            return (uint64_t)process_count();
        case SYS_SETBRIGHT:
            return (uint64_t)acpi_backlight_set((uint32_t)a1);
        case SYS_GETBRIGHT:
            return (uint64_t)acpi_backlight_get();
        case SYS_GETBATTERY:
            if (!user_buf_writable(cur, a1, sizeof(struct fry_battery_status))) return (uint64_t)-EFAULT;
            {
                struct fry_battery_status st;
                if (acpi_battery_get(&st) != 0) return (uint64_t)-EIO;
                struct fry_battery_status *u = (struct fry_battery_status *)(uintptr_t)a1;
                *u = st;
                return 0;
            }
        case SYS_FB_INFO: {
            if (cur && !g_first_gui_fb_seen && process_name_is_gui(cur->name)) {
                g_first_gui_fb_seen = 1;
                boot_diag_stage(39);
                if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_GUI_FB\n");
            }
            if (!user_buf_writable(cur, a1, sizeof(struct fry_fb_info))) return (uint64_t)-EFAULT;
            if (!g_handoff || !g_handoff->fb_base || !g_handoff->fb_width || !g_handoff->fb_height) {
                return (uint64_t)-ENXIO;
            }
            struct fry_fb_info *info = (struct fry_fb_info *)(uintptr_t)a1;
            uint64_t size = g_handoff->fb_stride * g_handoff->fb_height * 4ULL;
            info->phys = g_handoff->fb_base;
            info->size = size;
            info->user_base = FB_USER_BASE;
            info->width = (uint32_t)g_handoff->fb_width;
            info->height = (uint32_t)g_handoff->fb_height;
            info->stride = (uint32_t)g_handoff->fb_stride;
            info->format = g_handoff->fb_pixel_format;
            return 0;
        }
        case SYS_FB_MAP: {
            if (cur && !g_first_gui_fb_seen && process_name_is_gui(cur->name)) {
                g_first_gui_fb_seen = 1;
                boot_diag_stage(39);
                if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_GUI_FB\n");
            }
            if (!g_handoff || !g_handoff->fb_base || !g_handoff->fb_width || !g_handoff->fb_height) {
                return (uint64_t)-ENXIO;
            }
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t size = g_handoff->fb_stride * g_handoff->fb_height * 4ULL;
            uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
            uint64_t phys = g_handoff->fb_base;
            for (uint64_t i = 0; i < pages; i++) {
                vmm_map_user(cur->cr3,
                             FB_USER_BASE + i * PAGE_SIZE,
                             phys + i * PAGE_SIZE,
                             VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_CACHE_DISABLE | VMM_FLAG_NOFREE);
                __asm__ volatile("invlpg (%0)" : : "r"(FB_USER_BASE + i * PAGE_SIZE) : "memory");
            }
            return FB_USER_BASE;
        }
        case SYS_PROC_OUTPUT: {
            /* a1=pid, a2=user buf, a3=maxlen
               Returns: >0 bytes read, 0 alive/no data,
                        (uint64_t)-2 process dead+empty, (uint64_t)-1 bad ptr */
            uint32_t tpid = (uint32_t)a1;
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a2;
            struct fry_process *tp = proc_find_group_leader_any(tpid);
            struct fry_process_shared *shared;
            if (!tp) tp = proc_find_task_any(tpid);
            if (!tp) return (uint64_t)-ESRCH; /* never existed or fully freed */
            shared = proc_shared_state(tp);
            if (!shared) return (uint64_t)-ESRCH;
            uint64_t nr = 0;
            while (nr < a3 && shared->outbuf_head != shared->outbuf_tail) {
                ubuf[nr++] = (char)shared->outbuf[shared->outbuf_head];
                shared->outbuf_head = (shared->outbuf_head + 1u) % PROC_OUTBUF;
            }
            if (nr > 0) return nr;
            if (tp->state == PROC_DEAD) return (uint64_t)-ESRCH; /* dead + empty */
            return 0; /* alive, no output yet */
        }
        case SYS_MOUSE_GET: {
            // struct fry_mouse_state {
            //   int32_t x, y, dx, dy;   // offsets 0,4,8,12
            //   uint8_t btns, _pad[3];  // offset 16
            //   int32_t wheel;          // offset 20 (Phase 7)
            // }  size = 24 bytes
            if (!user_buf_writable(cur, a1, 24)) return (uint64_t)-EFAULT;
            int32_t mx, my, mdx, mdy, mwheel;
            uint8_t mb;
            ps2_mouse_get_ext(&mx, &my, &mb, &mdx, &mdy, &mwheel);
            int32_t *out = (int32_t *)(uintptr_t)a1;
            out[0] = mx;
            out[1] = my;
            out[2] = mdx;
            out[3] = mdy;
            *((uint8_t *)(out + 4)) = mb;
            out[5] = mwheel;
            return 0;
        }
        case SYS_PROC_INPUT: {
            /* a1=pid, a2=user buf, a3=len
               Returns: bytes written, -1 error */
            uint32_t tpid = (uint32_t)a1;
            if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-EFAULT;
            const uint8_t *ubuf = (const uint8_t *)(uintptr_t)a2;
            struct fry_process *tp = proc_find_group_leader(tpid);
            struct fry_process_shared *shared;
            if (!tp) tp = proc_find_task(tpid);
            if (!tp || tp->state == PROC_DEAD) return (uint64_t)-ESRCH;
            shared = proc_shared_state(tp);
            if (!shared) return (uint64_t)-ESRCH;
            uint64_t nw = 0;
            for (uint64_t _i = 0; _i < a3; _i++) {
                uint32_t nt = (shared->inbuf_tail + 1u) % PROC_INBUF;
                if (nt != shared->inbuf_head) {
                    shared->inbuf[shared->inbuf_tail] = ubuf[_i];
                    shared->inbuf_tail = nt;
                    nw++;
                } else {
                    break; // Buffer full
                }
            }
            return nw;
        }
        case SYS_SBRK: {
            struct fry_process_shared *shared;
            if (!cur) return (uint64_t)-ESRCH;
            shared = proc_shared_state(cur);
            if (!shared) return (uint64_t)-ESRCH;
            int64_t inc = (int64_t)a1;
            uint64_t old_end = shared->heap_end;
            if (inc == 0) return old_end;
            if (inc < 0) return (uint64_t)-EINVAL; // No shrinking for now
            uint64_t new_end = old_end + (uint64_t)inc;
            if (new_end < old_end || new_end > USER_TOP) return (uint64_t)-ENOMEM;
            uint64_t old_page_end = (old_end + 4095ULL) & ~4095ULL;
            uint64_t new_page_end = (new_end + 4095ULL) & ~4095ULL;
            if (new_page_end > USER_TOP) return (uint64_t)-ENOMEM;
            for (uint64_t va = old_page_end; va < new_page_end; va += 4096ULL) {
                /* Heap growth must only map fresh pages in this range. */
                if (vmm_virt_to_phys_user(cur->cr3, va) != 0) {
                    sbrk_rollback_pages(cur, old_page_end, va);
                    return (uint64_t)-ENOMEM;
                }
                uint64_t pa = pmm_alloc_page();
                if (!pa) {
                    sbrk_rollback_pages(cur, old_page_end, va);
                    return (uint64_t)-ENOMEM;
                }
                vmm_map_user(cur->cr3, va, pa, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE);
                uint64_t mapped_pa = vmm_virt_to_phys_user(cur->cr3, va);
                if ((mapped_pa & 0x000FFFFFFFFFF000ULL) != pa) {
                    if (mapped_pa) {
                        vmm_unmap_user(cur->cr3, va);
                    }
                    pmm_free_page(pa);
                    sbrk_rollback_pages(cur, old_page_end, va);
                    return (uint64_t)-ENOMEM;
                }
                uint8_t *kv = (uint8_t *)vmm_phys_to_virt(pa);
                for (int i = 0; i < 4096; i++) kv[i] = 0;
            }
            shared->heap_end = new_end;
            return old_end;
        }
        case SYS_SHM_ALLOC: {
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t size = (uint64_t)a1;
            uint32_t pages = (size + 4095ULL) / 4096ULL;
            if (pages == 0) return (uint64_t)-EINVAL;
            for (int i = 0; i < FRY_SHM_MAX; i++) {
                if (!shm_regions[i].used) {
                    uint64_t phys = 0;
                    // Allocate contiguous physical pages for simple mapping
                    // Actually, let's just allocate page by page and map them.
                    // But for SHM, we usually want a contiguous physical range or 
                    // a list of pages. Let's just do a simple contiguous allocation from PMM
                    // if possible, or just a linked list. 
                    // Actually, our PMM is a bitmap, we can do contiguous.
                    // TODO: contiguous PMM alloc. 
                    // For now, I'll just support 1 page or hack it.
                    // Wait, let's just allocate pages and store them.
                    // To keep it simple, I'll just allocate one big chunk from PMM.
                    phys = pmm_alloc_pages(pages);
                    if (!phys) return (uint64_t)-ENOMEM;
                    for (uint32_t s = 0; s < PROC_MAX; s++) {
                        shm_regions[i].mapped_pids[s] = 0;
                    }
                    shm_regions[i].phys_base = phys;
                    shm_regions[i].page_count = pages;
                    shm_regions[i].owner_pid = process_group_id(cur);
                    shm_regions[i].map_count = 0;
                    shm_regions[i].used = 1;
                    return (uint64_t)i;
                }
            }
            return (uint64_t)-ENFILE;
        }
        case SYS_SHM_MAP: {
            int id = (int)a1;
            if (id < 0 || id >= FRY_SHM_MAX || !shm_regions[id].used || !cur) return (uint64_t)-EINVAL;
            int slot = shm_proc_slot_by_pid(cur->pid);
            if (slot < 0) return (uint64_t)-ENOMEM;
            // Map at a high address
            uint64_t virt = SHM_USER_BASE + (uint64_t)id * SHM_SLOT_STRIDE; // 256MB apart
            for (uint32_t i = 0; i < shm_regions[id].page_count; i++) {
                vmm_map_user(cur->cr3, virt + i * 4096ULL, shm_regions[id].phys_base + i * 4096ULL,
                             VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE | VMM_FLAG_NOFREE);
            }
            if (shm_regions[id].mapped_pids[slot] != cur->pid) {
                shm_regions[id].mapped_pids[slot] = cur->pid;
                if (shm_regions[id].map_count != 0xFFFFFFFFU) {
                    shm_regions[id].map_count++;
                }
            }
            return virt;
        }
        case SYS_SHM_FREE: {
            int id = (int)a1;
            if (!cur || id < 0 || id >= FRY_SHM_MAX || !shm_regions[id].used) return (uint64_t)-EINVAL;
            if (shm_regions[id].owner_pid != process_group_id(cur)) return (uint64_t)-EPERM;
            shm_destroy_region(id);
            return 0;
        }
        case SYS_KILL: {
            uint32_t tpid = (uint32_t)a1;
            struct fry_process *tp = proc_find_task(tpid);
            uint32_t target_tgid;
            if (!tp) return (uint64_t)-ESRCH;
            target_tgid = process_group_id(tp);
            /* Don't allow killing pid 0, 1, or self group. */
            if (target_tgid <= 1 || (cur && target_tgid == process_group_id(cur))) {
                return (uint64_t)-EPERM;
            }
            if (tp->state == PROC_DEAD) return 0;
            process_exit_group(target_tgid, 0);
            sched_yield();
            return 0;
        }
        case SYS_THREAD_CREATE: {
            struct fry_process *thr;
            if (!cur || cur->is_kernel || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_ptr_ok(a1, 1) || a3 < 8 || !user_ptr_ok(a3 - 8, 8)) return (uint64_t)-EFAULT;
            if (!user_buf_mapped(cur, a1, 1) || !user_buf_writable(cur, a3 - 8, 8)) {
                return (uint64_t)-EFAULT;
            }
            thr = process_create_user_thread(cur, a1, a2, a3);
            if (!thr) return (uint64_t)-ENOMEM;
            sched_add(thr->pid);
            return (uint64_t)thr->pid;
        }
        case SYS_THREAD_JOIN: {
            int rc = process_thread_join((uint32_t)a1);
            return (uint64_t)rc;
        }
        case SYS_FUTEX_WAIT:
            return (uint64_t)futex_wait_begin(cur, a1, (uint32_t)a2, a3);
        case SYS_FUTEX_WAKE:
            return (uint64_t)futex_wake_waiters(cur, a1, (uint32_t)a2);
        case SYS_ACPI_DIAG: {
            if (!user_buf_writable(cur, a1, sizeof(struct fry_acpi_diag))) return (uint64_t)-EFAULT;
            struct fry_acpi_diag diag;
            if (acpi_get_diag(&diag) != 0) return (uint64_t)-EIO;
            struct fry_acpi_diag *u = (struct fry_acpi_diag *)(uintptr_t)a1;
            *u = diag;
            return 0;
        }
        case SYS_CREATE: {
            char path[FRY_PATH_MAX];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            uint16_t type = (uint16_t)a2;
            return (uint64_t)vfs_create(path, type);
        }
        case SYS_MKDIR: {
            char path[FRY_PATH_MAX];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            return (uint64_t)vfs_mkdir(path);
        }
        case SYS_UNLINK: {
            char path[FRY_PATH_MAX];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            return (uint64_t)vfs_unlink(path);
        }
        case SYS_STORAGE_INFO: {
            if (!user_buf_writable(cur, a1, sizeof(struct vfs_storage_info))) return (uint64_t)-EFAULT;
            struct vfs_storage_info info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_storage_info(&info) != 0) return (uint64_t)-EIO;
            struct vfs_storage_info *u = (struct vfs_storage_info *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        case SYS_PATH_FS_INFO: {
            if (!user_buf_writable(cur, a2, sizeof(struct vfs_path_fs_info))) return (uint64_t)-EFAULT;
            char path[192];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            struct vfs_path_fs_info info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_path_fs_info(path, &info) != 0) return (uint64_t)-ENOENT;
            struct vfs_path_fs_info *u = (struct vfs_path_fs_info *)(uintptr_t)a2;
            *u = info;
            return 0;
        }
        case SYS_MOUNTS_INFO: {
            if (!user_buf_writable(cur, a1, sizeof(struct vfs_mounts_info))) return (uint64_t)-EFAULT;
            struct vfs_mounts_info info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_mounts_info(&info) != 0) return (uint64_t)-EIO;
            struct vfs_mounts_info *u = (struct vfs_mounts_info *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        case SYS_MOUNTS_DEBUG: {
            if (!user_buf_writable(cur, a1, sizeof(struct vfs_mounts_dbg))) return (uint64_t)-EFAULT;
            struct vfs_mounts_dbg info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_mounts_dbg(&info) != 0) return (uint64_t)-EIO;
            struct vfs_mounts_dbg *u = (struct vfs_mounts_dbg *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        case SYS_WIFI_STATUS: {
            if (!user_buf_writable(cur, a1, sizeof(struct fry_wifi_status))) return (uint64_t)-EFAULT;
            struct fry_wifi_status info;
            if (wifi_9260_get_user_status(&info) != 0) return (uint64_t)-EIO;
            struct fry_wifi_status *u = (struct fry_wifi_status *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        case SYS_WIFI_SCAN: {
            if (!user_buf_writable(cur, a3, sizeof(uint32_t))) return (uint64_t)-EFAULT;
            if (a2 > FRY_WIFI_MAX_SCAN) a2 = FRY_WIFI_MAX_SCAN;
            if (a2 == 0) {
                *(uint32_t *)(uintptr_t)a3 = 0;
                return 0;
            }
            uint64_t bytes = a2 * (uint64_t)sizeof(struct fry_wifi_scan_entry);
            if (!user_buf_writable(cur, a1, bytes)) return (uint64_t)-EFAULT;
            uint32_t count = 0;
            int rc = wifi_9260_get_scan_entries((struct fry_wifi_scan_entry *)(uintptr_t)a1,
                                                (uint32_t)a2, &count);
            if (rc != 0) return (uint64_t)rc;
            *(uint32_t *)(uintptr_t)a3 = count;
            return 0;
        }
        case SYS_WIFI_CONNECT: {
            char ssid[FRY_WIFI_SSID_MAX + 1];
            char passphrase[96];
            if (copy_user_string(cur, a1, ssid, sizeof(ssid)) != 0) return (uint64_t)-EFAULT;
            if (copy_user_string(cur, a2, passphrase, sizeof(passphrase)) != 0) return (uint64_t)-EFAULT;
            return (uint64_t)wifi_9260_connect_user(ssid, passphrase);
        }
        case SYS_WIFI_DEBUG: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz == 0 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            int n = wifi_9260_get_debug_log(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_CPU_STATUS: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 128 || bufsz > 8192) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_cpu_status(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_cpu_status(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_INIT_LOG: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz == 0 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_init_log(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_init_log(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_DEBUG2: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz == 0 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_debug_log2(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_debug_log2(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_HANDOFF: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 192 || bufsz > 4096) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_handoff_status(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_handoff_status(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_DEBUG3: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz == 0 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_debug_log3(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_debug_log3(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_REINIT: {
            extern int wifi_9260_reinit_user(void);
            return (uint64_t)wifi_9260_reinit_user();
        }
        case SYS_WIFI_CMD_TRACE: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 128 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_cmd_trace(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_cmd_trace(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_SRAM: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 256 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_sram_dump(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_sram_dump(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_DEEP_DIAG: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 256 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_deep_diag(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_deep_diag(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_VERIFY: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 256 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_verify_result(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_verify_result(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_ETH_DIAG: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 256 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int i219_get_diag(char *buf, uint32_t bufsz);
            int n = i219_get_diag(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_MMAP: {
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t hint = a1;
            uint64_t length = a2;
            uint32_t prot = (uint32_t)a3;
            uint32_t flags = (uint32_t)a4;
            int fd = (int)a5;
            if (length == 0 || length > UINT64_MAX - (PAGE_SIZE - 1ULL)) return (uint64_t)-EINVAL;
            length = (length + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
            if (length == 0) return (uint64_t)-EINVAL;
            if (!vm_prot_supported(prot) || !vm_flags_supported(flags)) return (uint64_t)-EINVAL;
            if ((flags & FRY_MAP_RESERVE) != 0 && prot != 0) return (uint64_t)-EINVAL;
            if ((flags & FRY_MAP_GUARD) != 0 && prot != 0) return (uint64_t)-EINVAL;
            if (hint != 0 && (hint & (PAGE_SIZE - 1ULL)) != 0) return (uint64_t)-EINVAL;

            uint64_t base = 0;
            if ((flags & FRY_MAP_FIXED) != 0) {
                base = hint;
                if (!vm_range_available(cur, base, length)) return (uint64_t)-ENOMEM;
            } else if (hint != 0 && vm_range_available(cur, hint, length)) {
                base = hint;
            } else {
                base = vm_find_free_range(cur, length);
            }
            if (!base) return (uint64_t)-ENOMEM;
            if ((flags & FRY_MAP_GUARD) != 0) {
                if (vm_map_guard_region(cur, base, length, flags) != 0) return (uint64_t)-ENOMEM;
            } else if ((flags & FRY_MAP_ANON) != 0) {
                if ((flags & FRY_MAP_SHARED) != 0) {
                    if (vm_map_anon_shared_region(cur, base, length, prot, flags) != 0) return (uint64_t)-ENOMEM;
                } else {
                    if (vm_map_anon_private_region(cur, base, length, prot, flags) != 0) return (uint64_t)-ENOMEM;
                }
            } else {
                /* fd-backed mapping — check for FD_MEMFD vs FD_FILE */
                struct fry_process_shared *shared = proc_shared_state(cur);
                if (fd >= 3 && fd < FRY_FD_MAX && shared && shared->fd_ptrs[fd] &&
                    shared->fd_kind[fd] == FD_MEMFD) {
                    struct memfd_cb *mf = (struct memfd_cb *)shared->fd_ptrs[fd];
                    if (!mf || !mf->used) return (uint64_t)-EBADF;
                    if (vm_map_memfd_region(cur, base, length, prot, flags, mf) != 0)
                        return (uint64_t)-ENOMEM;
                } else {
                    if (vm_map_file_region(cur, base, length, prot, flags, fd) != 0)
                        return (uint64_t)-ENOMEM;
                }
            }
            return base;
        }
        case SYS_MUNMAP: {
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t base = a1;
            uint64_t length = a2;
            if ((base & (PAGE_SIZE - 1ULL)) != 0) return (uint64_t)-EINVAL;
            if (length == 0 || length > UINT64_MAX - (PAGE_SIZE - 1ULL)) return (uint64_t)-EINVAL;
            length = (length + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
            if (length == 0) return (uint64_t)-EINVAL;
            return (uint64_t)vm_unmap_region_range(cur, base, length);
        }
        case SYS_MPROTECT: {
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t base = a1;
            uint64_t length = a2;
            uint32_t prot = (uint32_t)a3;
            if ((base & (PAGE_SIZE - 1ULL)) != 0) return (uint64_t)-EINVAL;
            if (length == 0 || length > UINT64_MAX - (PAGE_SIZE - 1ULL)) return (uint64_t)-EINVAL;
            length = (length + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
            if (length == 0) return (uint64_t)-EINVAL;
            if (!vm_prot_supported(prot)) return (uint64_t)-EINVAL;
            return (uint64_t)vm_mprotect_region_range(cur, base, length, prot);
        }
        case SYS_PRCTL: {
            if (!cur) return (uint64_t)-ESRCH;
            int option = (int)a1;

            switch (option) {
                case PR_SET_NAME: {
                    char name[16];
                    if (copy_user_string(cur, a2, name, sizeof(name)) != 0)
                        return (uint64_t)-EFAULT;
                    for (uint32_t i = 0; i < sizeof(cur->name); i++) cur->name[i] = 0;
                    for (uint32_t i = 0; i < 15 && name[i]; i++) cur->name[i] = name[i];
                    return 0;
                }
                case PR_GET_NAME: {
                    char out[16];
                    if (!user_buf_writable(cur, a2, sizeof(out))) return (uint64_t)-EFAULT;
                    for (uint32_t i = 0; i < sizeof(out); i++) out[i] = 0;
                    for (uint32_t i = 0; i < 15 && cur->name[i]; i++) out[i] = cur->name[i];
                    return (uint64_t)copyout(cur, out, a2, sizeof(out));
                }
                case PR_SET_NO_NEW_PRIVS:
                    if (a2 != 1 || a3 || a4 || a5) return (uint64_t)-EINVAL;
                    cur->no_new_privs = 1;
                    return 0;
                case PR_GET_NO_NEW_PRIVS:
                    return cur->no_new_privs ? 1 : 0;
                case PR_SET_DUMPABLE:
                    if (a2 > 1 || a3 || a4 || a5) return (uint64_t)-EINVAL;
                    cur->dumpable = (uint8_t)a2;
                    return 0;
                case PR_GET_DUMPABLE:
                    return cur->dumpable ? 1 : 0;
                case PR_GET_SECCOMP:
                    return 0;
                case PR_SET_SECCOMP:
                    return (uint64_t)-ENOTSUP;
                case PR_SET_TIMERSLACK:
                    cur->timer_slack_ns = a2 ? a2 : 50000;
                    return 0;
                case PR_GET_TIMERSLACK:
                    return cur->timer_slack_ns;
                case PR_SET_THP_DISABLE:
                    if (a2 > 1 || a3 || a4 || a5) return (uint64_t)-EINVAL;
                    cur->thp_disabled = (uint8_t)a2;
                    return 0;
                case PR_GET_THP_DISABLE:
                    return cur->thp_disabled ? 1 : 0;
                case PR_SET_PTRACER:
                    return 0;
                case PR_GET_PDEATHSIG: {
                    uint32_t sig = 0;
                    if (!user_buf_writable(cur, a2, sizeof(sig))) return (uint64_t)-EFAULT;
                    return (uint64_t)copyout(cur, &sig, a2, sizeof(sig));
                }
                case PR_SET_PDEATHSIG:
                    return a2 ? (uint64_t)-ENOTSUP : 0;
                case PR_SET_VMA: {
                    char vma_name[80];
                    uint64_t base = a3;
                    uint64_t length = a4;
                    if (a2 != PR_SET_VMA_ANON_NAME) return (uint64_t)-EINVAL;
                    if (length == 0) return 0;
                    if ((base & (PAGE_SIZE - 1ULL)) != 0) return (uint64_t)-EINVAL;
                    if (length > UINT64_MAX - (PAGE_SIZE - 1ULL)) return (uint64_t)-EINVAL;
                    length = (length + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
                    if (vm_region_find_containing(cur, base, length) < 0)
                        return (uint64_t)-ENOMEM;
                    if (a5 && copy_user_string(cur, a5, vma_name, sizeof(vma_name)) != 0)
                        return (uint64_t)-EFAULT;
                    return 0;
                }
                default:
                    return (uint64_t)-EINVAL;
            }
        }
        case SYS_MADVISE: {
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t base = a1;
            uint64_t length = a2;
            int advice = (int)a3;

            switch (advice) {
                case 0:  /* MADV_NORMAL */
                case 1:  /* MADV_RANDOM */
                case 2:  /* MADV_SEQUENTIAL */
                case 3:  /* MADV_WILLNEED */
                case 4:  /* MADV_DONTNEED */
                case 8:  /* MADV_FREE */
                case 14: /* MADV_HUGEPAGE */
                    break;
                default:
                    return (uint64_t)-EINVAL;
            }

            if (length == 0) return 0;
            if ((base & (PAGE_SIZE - 1ULL)) != 0) return (uint64_t)-EINVAL;
            if (length > UINT64_MAX - (PAGE_SIZE - 1ULL)) return (uint64_t)-EINVAL;
            length = (length + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
            if (vm_region_find_containing(cur, base, length) < 0)
                return (uint64_t)-ENOMEM;
            return 0;
        }
        /* ================================================================
         * Phase 3: IPC / Descriptor model syscalls
         * ================================================================ */
        case SYS_PIPE: {
            /*
             * SYS_PIPE(a1=user_int_array_ptr)
             * Creates a pipe. Writes [read_fd, write_fd] to user buffer.
             * Returns 0 on success, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_buf_writable(cur, a1, 2 * sizeof(int32_t))) return (uint64_t)-EFAULT;
            int pidx = pipe_alloc();
            if (pidx < 0) return (uint64_t)-ENFILE;
            int rfd = fd_alloc(cur);
            if (rfd < 0) { g_pipes[pidx].used = 0; return (uint64_t)-EMFILE; }
            fd_install(cur, rfd, &g_pipes[pidx], FD_PIPE_READ, 0);
            g_pipes[pidx].readers++;
            int wfd = fd_alloc(cur);
            if (wfd < 0) {
                g_pipes[pidx].readers--;
                fd_release(cur, rfd);
                g_pipes[pidx].used = 0;
                return (uint64_t)-EMFILE;
            }
            fd_install(cur, wfd, &g_pipes[pidx], FD_PIPE_WRITE, 0);
            g_pipes[pidx].writers++;
            int32_t *ufds = (int32_t *)(uintptr_t)a1;
            ufds[0] = rfd;
            ufds[1] = wfd;
            return 0;
        }
        case SYS_DUP: {
            /*
             * SYS_DUP(a1=oldfd)
             * Returns new fd pointing to the same underlying object.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int oldfd = (int)a1;
            struct fry_process_shared *shared = proc_shared_state(cur);
            if (oldfd < 0 || oldfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (oldfd < 3) return (uint64_t)-EBADF; /* don't dup stdin/stdout/stderr for now */
            uint8_t okind = shared->fd_kind[oldfd];
            void *optr = shared->fd_ptrs[oldfd];
            if (okind == FD_NONE || !optr) return (uint64_t)-EBADF;
            int newfd = fd_alloc(cur);
            if (newfd < 0) return (uint64_t)-EMFILE;
            fd_install(cur, newfd, optr, okind, shared->fd_flags[oldfd]);
            shared->fd_table[newfd] = shared->fd_table[oldfd];
            if (shared->fd_paths[oldfd][0]) {
                int prc = fd_path_set(shared, newfd, shared->fd_paths[oldfd]);
                if (prc < 0) {
                    fd_release(cur, newfd);
                    return (uint64_t)prc;
                }
                if (okind == FD_DIR) shared->fd_ptrs[newfd] = shared->fd_paths[newfd];
            }
            /* Increment pipe refcount if duplicating a pipe end */
            if (okind == FD_PIPE_READ) {
                ((struct fry_pipe *)optr)->readers++;
            } else if (okind == FD_PIPE_WRITE) {
                ((struct fry_pipe *)optr)->writers++;
            }
            return (uint64_t)newfd;
        }
        case SYS_DUP2: {
            /*
             * SYS_DUP2(a1=oldfd, a2=newfd)
             * Forces newfd to refer to the same object as oldfd.
             * If newfd was open, it is closed first.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int oldfd2 = (int)a1;
            int newfd2 = (int)a2;
            struct fry_process_shared *shared = proc_shared_state(cur);
            if (oldfd2 < 0 || oldfd2 >= FRY_FD_MAX || newfd2 < 3 || newfd2 >= FRY_FD_MAX) {
                return (uint64_t)-EBADF;
            }
            uint8_t okind2 = shared->fd_kind[oldfd2];
            void *optr2 = shared->fd_ptrs[oldfd2];
            if (okind2 == FD_NONE || !optr2) return (uint64_t)-EBADF;
            if (oldfd2 == newfd2) return (uint64_t)newfd2;
            /* Close newfd if open */
            if (shared->fd_kind[newfd2] != FD_NONE && shared->fd_ptrs[newfd2]) {
                if (shared->fd_kind[newfd2] == FD_FILE) {
                    vfs_close((struct vfs_file *)shared->fd_ptrs[newfd2]);
                } else if (shared->fd_kind[newfd2] == FD_PIPE_READ) {
                    struct fry_pipe *pp = (struct fry_pipe *)shared->fd_ptrs[newfd2];
                    if (pp->readers > 0) pp->readers--;
                    if (pp->readers == 0 && pp->writers == 0) { pp->used = 0; pp->head = 0; pp->tail = 0; }
                } else if (shared->fd_kind[newfd2] == FD_PIPE_WRITE) {
                    struct fry_pipe *pp = (struct fry_pipe *)shared->fd_ptrs[newfd2];
                    if (pp->writers > 0) pp->writers--;
                    if (pp->readers == 0 && pp->writers == 0) { pp->used = 0; pp->head = 0; pp->tail = 0; }
                } else if (shared->fd_kind[newfd2] == FD_SOCKET) {
                    struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[newfd2];
                    if (sk && sk->used) {
                        if (sk->type == SOCK_STREAM) {
                            if (sk->domain == 1 && sk->tcp_handle >= 0 && sk->tcp_handle < FRY_PIPE_MAX) {
                                struct fry_pipe *pp = &g_pipes[sk->tcp_handle];
                                if (pp->writers > 0) pp->writers--;
                                if (pp->readers == 0 && pp->writers == 0) { pp->used = 0; pp->head = 0; pp->tail = 0; }
                                pp = &g_pipes[sk->listen_handle];
                                if (pp->readers > 0) pp->readers--;
                                if (pp->readers == 0 && pp->writers == 0) { pp->used = 0; pp->head = 0; pp->tail = 0; }
                            } else {
                                if (sk->tcp_handle >= 0) tcp_close(sk->tcp_handle);
                                if (sk->listen_handle >= 0) tcp_close(sk->listen_handle);
                            }
                        }
                        sk->used = 0;
                        sk->state = SOCK_ST_CLOSED;
                        sk->tcp_handle = -1;
                        sk->listen_handle = -1;
                    }
                } else if (shared->fd_kind[newfd2] == FD_TIMERFD) {
                    struct timerfd_cb *tm = (struct timerfd_cb *)shared->fd_ptrs[newfd2];
                    if (tm) { tm->used = 0; kfree(tm); }
                } else if (shared->fd_kind[newfd2] == FD_SIGNALFD) {
                    struct signalfd_cb *sf = (struct signalfd_cb *)shared->fd_ptrs[newfd2];
                    if (sf) { sf->used = 0; kfree(sf); }
                } else if (shared->fd_kind[newfd2] == FD_INOTIFY) {
                    struct inotify_cb *in = (struct inotify_cb *)shared->fd_ptrs[newfd2];
                    if (in) { in->used = 0; kfree(in); }
                } else if (shared->fd_kind[newfd2] == FD_MEMFD) {
                    struct memfd_cb *mf = (struct memfd_cb *)shared->fd_ptrs[newfd2];
                    if (mf) { mf->used = 0; memfd_free_pages(mf); kfree(mf); }
                }
                fd_release(cur, newfd2);
            }
            fd_install(cur, newfd2, optr2, okind2, shared->fd_flags[oldfd2]);
            shared->fd_table[newfd2] = shared->fd_table[oldfd2];
            if (shared->fd_paths[oldfd2][0]) {
                int prc = fd_path_set(shared, newfd2, shared->fd_paths[oldfd2]);
                if (prc < 0) {
                    fd_release(cur, newfd2);
                    return (uint64_t)prc;
                }
                if (okind2 == FD_DIR) shared->fd_ptrs[newfd2] = shared->fd_paths[newfd2];
            }
            if (okind2 == FD_PIPE_READ) ((struct fry_pipe *)optr2)->readers++;
            else if (okind2 == FD_PIPE_WRITE) ((struct fry_pipe *)optr2)->writers++;
            return (uint64_t)newfd2;
        }
        case SYS_POLL: {
            /*
             * SYS_POLL(a1=user_pollfd_array, a2=nfds, a3=timeout_ms)
             * Returns number of ready fds, 0 on timeout, -errno on error.
             * timeout_ms == 0: non-blocking poll
             * timeout_ms == UINT64_MAX: block indefinitely
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            uint32_t nfds = (uint32_t)a2;
            if (nfds == 0) return 0;
            if (nfds > FRY_POLL_MAX) return (uint64_t)-EINVAL;
            uint64_t pfdsz = nfds * sizeof(struct fry_pollfd);
            if (!user_buf_writable(cur, a1, pfdsz)) return (uint64_t)-EFAULT;
            struct fry_pollfd *ufds = (struct fry_pollfd *)(uintptr_t)a1;
            uint64_t timeout_ms = a3;

            /* First pass: drain NIC then check readiness */
            net_poll();
            uint32_t ready = 0;
            for (uint32_t i = 0; i < nfds; i++) {
                ufds[i].revents = poll_check_fd(cur, ufds[i].fd, ufds[i].events);
                if (ufds[i].revents != 0) ready++;
            }
            if (ready > 0 || timeout_ms == 0) return (uint64_t)ready;

            /* Block until event or timeout */
            uint64_t wake_ms = (timeout_ms == UINT64_MAX) ? UINT64_MAX :
                               (hpet_read_counter() * 1000ULL / hpet_get_freq_hz()) + timeout_ms;
            sched_block_poll(cur->pid, wake_ms);
            sched_yield();

            /* Re-check after wake */
            cur = proc_current();
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_buf_writable(cur, a1, pfdsz)) return (uint64_t)-EFAULT;
            ufds = (struct fry_pollfd *)(uintptr_t)a1;
            net_poll();
            ready = 0;
            for (uint32_t i = 0; i < nfds; i++) {
                ufds[i].revents = poll_check_fd(cur, ufds[i].fd, ufds[i].events);
                if (ufds[i].revents != 0) ready++;
            }
            return (uint64_t)ready;
        }
        case SYS_FCNTL: {
            /*
             * SYS_FCNTL(a1=fd, a2=cmd, a3=arg)
             * F_GETFL: return current flags
             * F_SETFL: set flags (only O_NONBLOCK is mutable)
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int ffd = (int)a1;
            int cmd = (int)a2;
            struct fry_process_shared *shared = proc_shared_state(cur);
            if (ffd < 3 || ffd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[ffd] == FD_NONE || !shared->fd_ptrs[ffd]) return (uint64_t)-EBADF;
            switch (cmd) {
                case F_GETFL:
                    return (uint64_t)shared->fd_flags[ffd];
                case F_SETFL:
                    /* Only O_NONBLOCK is user-settable after open */
                    shared->fd_flags[ffd] = (shared->fd_flags[ffd] & ~(uint32_t)O_NONBLOCK) |
                                             ((uint32_t)a3 & O_NONBLOCK);
                    return 0;
                case F_GETFD:
                    /* Return 0 or FD_CLOEXEC (1) based on O_CLOEXEC flag */
                    return (uint64_t)((shared->fd_flags[ffd] & O_CLOEXEC) ? 1 : 0);
                case F_SETFD:
                    /* Only FD_CLOEXEC is settable */
                    if (a3 & ~1) return (uint64_t)-EINVAL;
                    if (a3 & 1)
                        shared->fd_flags[ffd] |= O_CLOEXEC;
                    else
                        shared->fd_flags[ffd] &= ~(uint32_t)O_CLOEXEC;
                    return 0;
                default:
                    return (uint64_t)-EINVAL;
            }
        }
        case SYS_SPAWN_ARGS: {
            /*
             * SYS_SPAWN_ARGS(a1=path_ptr, a2=argv_ptr, a3=argc, a4=envp_ptr, a5=envc)
             * Spawn a new process with arguments and environment.
             * argv_ptr points to an array of user string pointers.
             * Returns pid on success, -errno on failure.
             */
            char spath[FRY_PATH_MAX];
            uint32_t col = g_spawn_attempt_count;
            if (copy_user_string(cur, a1, spath, sizeof(spath)) != 0) {
                g_spawn_attempt_count++;
                return (uint64_t)-EFAULT;
            }
            uint32_t sargc = (uint32_t)a3;
            uint32_t senvc = (uint32_t)a5;
            if (sargc > FRY_ARGV_MAX) sargc = FRY_ARGV_MAX;
            if (senvc > FRY_ENV_MAX) senvc = FRY_ENV_MAX;

            /* Copy argv strings from userspace */
            const char *kargv[FRY_ARGV_MAX];
            char kargv_buf[FRY_ARGS_BUFSZ];
            uint32_t abuf_pos = 0;
            uint32_t actual_argc = 0;

            if (sargc > 0 && a2 != 0) {
                if (!user_buf_mapped(cur, a2, sargc * sizeof(uint64_t))) {
                    g_spawn_attempt_count++;
                    return (uint64_t)-EFAULT;
                }
                uint64_t *uargv = (uint64_t *)(uintptr_t)a2;
                for (uint32_t i = 0; i < sargc; i++) {
                    char tmp[256];
                    if (copy_user_string(cur, uargv[i], tmp, sizeof(tmp)) != 0) break;
                    uint32_t len = 0;
                    while (tmp[len]) len++;
                    if (abuf_pos + len + 1 > FRY_ARGS_BUFSZ) break;
                    kargv[i] = &kargv_buf[abuf_pos];
                    for (uint32_t j = 0; j <= len; j++) kargv_buf[abuf_pos++] = tmp[j];
                    actual_argc++;
                }
            }

            /* Copy envp strings from userspace */
            const char *kenvp[FRY_ENV_MAX];
            uint32_t actual_envc = 0;

            if (senvc > 0 && a4 != 0) {
                if (!user_buf_mapped(cur, a4, senvc * sizeof(uint64_t))) {
                    g_spawn_attempt_count++;
                    return (uint64_t)-EFAULT;
                }
                uint64_t *uenvp = (uint64_t *)(uintptr_t)a4;
                for (uint32_t i = 0; i < senvc; i++) {
                    char tmp[256];
                    if (copy_user_string(cur, uenvp[i], tmp, sizeof(tmp)) != 0) break;
                    uint32_t len = 0;
                    while (tmp[len]) len++;
                    if (abuf_pos + len + 1 > FRY_ARGS_BUFSZ) break;
                    kenvp[i] = &kargv_buf[abuf_pos];
                    for (uint32_t j = 0; j <= len; j++) kargv_buf[abuf_pos++] = tmp[j];
                    actual_envc++;
                }
            }

            int rc = process_launch_args(spath, kargv, actual_argc, kenvp, actual_envc);
            if (rc >= 0) {
                if (col < 80) boot_diag_color(col, 1, 0x0000FF00u);
            } else {
                if (col < 80) boot_diag_color(col, 1, 0x00FF0000u);
            }
            g_spawn_attempt_count++;
            return (uint64_t)rc;
        }
        case SYS_GET_ARGC: {
            /*
             * SYS_GET_ARGC()
             * Returns the argument count for the current process.
             */
            if (!cur || !proc_shared_state(cur)) return 0;
            return (uint64_t)proc_shared_state(cur)->argc;
        }
        case SYS_GET_ARGV: {
            /*
             * SYS_GET_ARGV(a1=index, a2=user_buf, a3=buf_len)
             * Copies argv[index] into user buffer. Returns string length or -errno.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            uint32_t idx = (uint32_t)a1;
            if (idx >= shared->argc) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a2;
            uint32_t off = shared->argv_offsets[idx];
            if (off >= FRY_ARGS_BUFSZ) return (uint64_t)-EINVAL;
            const char *arg = &shared->args_buf[off];
            uint32_t len = 0;
            while (arg[len] && off + len < FRY_ARGS_BUFSZ) len++;
            if (len + 1 > (uint32_t)a3) return (uint64_t)-ERANGE;
            for (uint32_t i = 0; i <= len; i++) ubuf[i] = arg[i];
            return (uint64_t)len;
        }
        case SYS_GETENV: {
            /*
             * SYS_GETENV(a1=name_ptr, a2=value_buf, a3=buf_len)
             * Looks up NAME=VALUE in the process environment.
             * Copies VALUE into user buffer. Returns length or -ENOENT.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            char name[128];
            if (copy_user_string(cur, a1, name, sizeof(name)) != 0) return (uint64_t)-EFAULT;
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
            uint32_t nlen = 0;
            while (name[nlen]) nlen++;

            for (uint32_t i = 0; i < shared->envc; i++) {
                uint32_t off = shared->env_offsets[i];
                if (off >= FRY_ARGS_BUFSZ) continue;
                const char *env = &shared->args_buf[off];
                /* Check if env starts with "NAME=" */
                uint32_t match = 1;
                for (uint32_t j = 0; j < nlen; j++) {
                    if (off + j >= FRY_ARGS_BUFSZ || env[j] != name[j]) { match = 0; break; }
                }
                if (!match || off + nlen >= FRY_ARGS_BUFSZ || env[nlen] != '=') continue;
                /* Found it — copy value after '=' */
                const char *val = &env[nlen + 1];
                uint32_t vlen = 0;
                while (val[vlen] && off + nlen + 1 + vlen < FRY_ARGS_BUFSZ) vlen++;
                if (vlen + 1 > (uint32_t)a3) return (uint64_t)-ERANGE;
                char *ubuf = (char *)(uintptr_t)a2;
                for (uint32_t j = 0; j <= vlen; j++) ubuf[j] = val[j];
                return (uint64_t)vlen;
            }
            return (uint64_t)-ENOENT;
        }

        /* ============================================================
         * Socket syscalls (Phase 4)
         * ============================================================ */

        case SYS_SOCKET: {
            /*
             * SYS_SOCKET(a1=domain, a2=type, a3=protocol)
             * Returns: fd on success, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if ((int)a1 != AF_INET) return (uint64_t)-EAFNOSUPPORT;
            if ((int)a2 != SOCK_STREAM && (int)a2 != SOCK_DGRAM) return (uint64_t)-EPROTOTYPE;

            int si = sock_alloc();
            if (si < 0) return (uint64_t)-ENFILE;

            int sfd = fd_alloc(cur);
            if (sfd < 0) {
                g_sockets[si].used = 0;
                return (uint64_t)-EMFILE;
            }

            g_sockets[si].domain = (uint8_t)a1;
            g_sockets[si].type = (uint8_t)a2;
            g_sockets[si].state = SOCK_ST_CREATED;

            fd_install(cur, sfd, &g_sockets[si], FD_SOCKET, 0);

            if ((int)a2 == SOCK_DGRAM) sock_ensure_udp_handler();

            return (uint64_t)sfd;
        }

        case SYS_CONNECT: {
            /*
             * SYS_CONNECT(a1=fd, a2=sockaddr_in_ptr, a3=addrlen)
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int cfd = (int)a1;
            if (cfd < 3 || cfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[cfd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[cfd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;
            if (sk->state == SOCK_ST_CONNECTED) return (uint64_t)-EISCONN;

            if (a3 < sizeof(struct fry_sockaddr_in)) return (uint64_t)-EINVAL;
            if (!user_buf_mapped(cur, a2, sizeof(struct fry_sockaddr_in)))
                return (uint64_t)-EFAULT;

            struct fry_sockaddr_in addr;
            if (copyin(cur, a2, &addr, sizeof(addr)) != 0) return (uint64_t)-EFAULT;
            if (addr.sin_family != AF_INET) return (uint64_t)-EAFNOSUPPORT;

            uint32_t dst_ip = fry_ntohl(addr.sin_addr);
            uint16_t dst_port = fry_ntohs(addr.sin_port);

            sk->remote_ip = dst_ip;
            sk->remote_port = dst_port;

            if (sk->type == SOCK_STREAM) {
                kprint_serial_only("SYS_CONNECT: pid=%u tcp %u.%u.%u.%u:%u\n",
                    cur->pid,
                    (dst_ip >> 24) & 0xFF, (dst_ip >> 16) & 0xFF,
                    (dst_ip >> 8) & 0xFF, dst_ip & 0xFF, dst_port);
                tcp_conn_t tc = tcp_connect(dst_ip, dst_port);
                if (tc < 0) {
                    kprint_serial_only("SYS_CONNECT: FAIL rc=%d\n", tc);
                    sk->state = SOCK_ST_CREATED;
                    return (uint64_t)-ECONNREFUSED;
                }
                sk->tcp_handle = tc;
                sk->state = SOCK_ST_CONNECTED;
                kprint_serial_only("SYS_CONNECT: OK handle=%d\n", tc);
                return 0;
            }

            if (sk->type == SOCK_DGRAM) {
                /* UDP "connect" just sets default destination */
                if (sk->local_port == 0) sk->local_port = sock_ephemeral_port();
                sk->state = SOCK_ST_CONNECTED;
                return 0;
            }

            return (uint64_t)-EINVAL;
        }

        case SYS_BIND: {
            /*
             * SYS_BIND(a1=fd, a2=sockaddr_in_ptr, a3=addrlen)
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int bfd = (int)a1;
            if (bfd < 3 || bfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[bfd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[bfd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;
            if (sk->state != SOCK_ST_CREATED) return (uint64_t)-EINVAL;

            if (a3 < sizeof(struct fry_sockaddr_in)) return (uint64_t)-EINVAL;
            if (!user_buf_mapped(cur, a2, sizeof(struct fry_sockaddr_in)))
                return (uint64_t)-EFAULT;

            struct fry_sockaddr_in addr;
            if (copyin(cur, a2, &addr, sizeof(addr)) != 0) return (uint64_t)-EFAULT;
            if (addr.sin_family != AF_INET) return (uint64_t)-EAFNOSUPPORT;

            sk->local_ip = fry_ntohl(addr.sin_addr);
            sk->local_port = fry_ntohs(addr.sin_port);

            /* Check for port conflict (skip if reuseaddr) */
            if (!sk->reuseaddr) {
                for (int i = 0; i < FRY_SOCK_MAX; i++) {
                    if (g_sockets[i].used && &g_sockets[i] != sk &&
                        g_sockets[i].type == sk->type &&
                        g_sockets[i].local_port == sk->local_port) {
                        return (uint64_t)-EADDRINUSE;
                    }
                }
            }

            sk->state = SOCK_ST_BOUND;
            return 0;
        }

        case SYS_LISTEN: {
            /*
             * SYS_LISTEN(a1=fd, a2=backlog)
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int lfd = (int)a1;
            if (lfd < 3 || lfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[lfd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[lfd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;
            if (sk->type != SOCK_STREAM) return (uint64_t)-EINVAL;
            if (sk->state != SOCK_ST_BOUND) return (uint64_t)-EINVAL;

            tcp_conn_t lh = tcp_listen(sk->local_port);
            if (lh < 0) return (uint64_t)-ENOMEM;

            sk->listen_handle = lh;
            sk->state = SOCK_ST_LISTENING;
            return 0;
        }

        case SYS_ACCEPT: {
            /*
             * SYS_ACCEPT(a1=fd, a2=sockaddr_out_ptr (or 0), a3=addrlen_ptr (or 0))
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int afd = (int)a1;
            if (afd < 3 || afd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[afd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *lsk = (struct fry_socket *)shared->fd_ptrs[afd];
            if (!lsk || !lsk->used || lsk->state != SOCK_ST_LISTENING)
                return (uint64_t)-EINVAL;

            /* Poll network and try to accept */
            net_poll();
            tcp_conn_t nc = tcp_accept(lsk->listen_handle);
            if (nc < 0) {
                uint32_t aflags = shared->fd_flags[afd];
                if (aflags & O_NONBLOCK) return (uint64_t)-EAGAIN;
                /* Block and retry */
                sched_block_poll(cur->pid, UINT64_MAX);
                sched_yield();
                cur = proc_current();
                if (!cur) return (uint64_t)-ESRCH;
                shared = proc_shared_state(cur);
                if (!shared || !shared->fd_ptrs[afd]) return (uint64_t)-EBADF;
                lsk = (struct fry_socket *)shared->fd_ptrs[afd];
                if (!lsk || !lsk->used) return (uint64_t)-EBADF;
                net_poll();
                nc = tcp_accept(lsk->listen_handle);
                if (nc < 0) return (uint64_t)-EAGAIN;
            }

            /* Allocate new socket for the accepted connection */
            int nsi = sock_alloc();
            if (nsi < 0) { tcp_close(nc); return (uint64_t)-ENFILE; }

            int newfd = fd_alloc(cur);
            if (newfd < 0) {
                g_sockets[nsi].used = 0;
                tcp_close(nc);
                return (uint64_t)-EMFILE;
            }

            g_sockets[nsi].domain = AF_INET;
            g_sockets[nsi].type = SOCK_STREAM;
            g_sockets[nsi].state = SOCK_ST_CONNECTED;
            g_sockets[nsi].tcp_handle = nc;
            g_sockets[nsi].local_port = lsk->local_port;
            /* We don't have easy access to remote IP/port from tcp_conn here,
               but the connection is fully established in netcore. */

            fd_install(cur, newfd, &g_sockets[nsi], FD_SOCKET, 0);

            /* Fill in peer address if requested */
            if (a2 && a3) {
                /* Best effort — we'd need netcore to expose remote addr */
                struct fry_sockaddr_in peer;
                uint8_t *p = (uint8_t *)&peer;
                for (uint32_t j = 0; j < sizeof(peer); j++) p[j] = 0;
                peer.sin_family = AF_INET;
                if (user_buf_writable(cur, a2, sizeof(peer)))
                    copyout(cur, &peer, a2, sizeof(peer));
            }

            return (uint64_t)newfd;
        }

        case SYS_SEND: {
            /*
             * SYS_SEND(a1=fd, a2=buf, a3=len, a4=flags)
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int sfd = (int)a1;
            if (sfd < 3 || sfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[sfd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[sfd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;
            if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-EFAULT;

            const uint8_t *buf = (const uint8_t *)(uintptr_t)a2;

            if (sk->type == SOCK_STREAM) {
                /* AF_UNIX socketpair: route through pipe buffers */
                if (sk->domain == 1 && sk->tcp_handle >= 0 && sk->tcp_handle < FRY_PIPE_MAX) {
                    struct fry_pipe *pp = &g_pipes[sk->tcp_handle];
                    if (!pp->used) return (uint64_t)-ENOTCONN;
                    uint32_t fdflags = shared->fd_flags[sfd];
                    int sent = pipe_write(pp, (const char *)buf, a3, fdflags);
                    if (sent < 0 && sent != -EAGAIN) return (uint64_t)sent;
                    if (sent == -EAGAIN) {
                        if (fdflags & O_NONBLOCK) return (uint64_t)-EAGAIN;
                        sched_block_poll(cur->pid, UINT64_MAX);
                        sched_yield();
                        cur = proc_current();
                        if (!cur) return (uint64_t)-ESRCH;
                        shared = proc_shared_state(cur);
                        if (!shared || !shared->fd_ptrs[sfd]) return (uint64_t)-EBADF;
                        sk = (struct fry_socket *)shared->fd_ptrs[sfd];
                        if (!sk || !sk->used || sk->tcp_handle < 0) return (uint64_t)-EBADF;
                        pp = &g_pipes[sk->tcp_handle];
                        sent = pipe_write(pp, (const char *)buf, a3, 0);
                    }
                    if (sent < 0) return (uint64_t)sent;
                    return (uint64_t)sent;
                }
                if (sk->state != SOCK_ST_CONNECTED || sk->tcp_handle < 0)
                    return (uint64_t)-ENOTCONN;
                int sent = tcp_send(sk->tcp_handle, buf, (uint16_t)a3);
                kprint_serial_only("SYS_SEND: pid=%u len=%u sent=%d\n",
                    cur->pid, (uint32_t)a3, sent);
                if (sent < 0) return (uint64_t)-EIO;
                return (uint64_t)sent;
            }
            if (sk->type == SOCK_DGRAM) {
                if (sk->remote_ip == 0 && sk->remote_port == 0)
                    return (uint64_t)-EDESTADDRREQ;
                if (sk->local_port == 0) sk->local_port = sock_ephemeral_port();
                int r = udp_send(sk->remote_ip, sk->remote_port,
                                 sk->local_port, buf, (uint16_t)a3);
                return r == 0 ? (uint64_t)a3 : (uint64_t)-EIO;
            }
            return (uint64_t)-EINVAL;
        }

        case SYS_RECV: {
            /*
             * SYS_RECV(a1=fd, a2=buf, a3=len, a4=flags)
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int rfd = (int)a1;
            if (rfd < 3 || rfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[rfd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[rfd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;

            uint8_t *buf = (uint8_t *)(uintptr_t)a2;
            uint32_t rflags = (uint32_t)a4;
            uint32_t fdflags = shared->fd_flags[rfd];
            int nonblock = (fdflags & O_NONBLOCK) || (rflags & MSG_DONTWAIT);

            if (sk->type == SOCK_STREAM) {
                /* AF_UNIX socketpair: route through pipe buffers */
                if (sk->domain == 1 && sk->listen_handle >= 0 && sk->listen_handle < FRY_PIPE_MAX) {
                    struct fry_pipe *pp = &g_pipes[sk->listen_handle];
                    if (!pp->used) {
                        /* Both writers gone and pipe empty = EOF */
                        if (pp->writers == 0 && !pipe_data_avail(pp)) return 0;
                        return (uint64_t)-ENOTCONN;
                    }
                    int nr = pipe_read(pp, (char *)buf, a3, 0);
                    if (nr > 0) return (uint64_t)nr;
                    /* EOF: all writers closed and buffer empty */
                    if (pp->writers == 0 && !pipe_data_avail(pp)) return 0;
                    /* No data available */
                    if (nonblock) return (uint64_t)-EAGAIN;
                    /* Block and retry */
                    sched_block_poll(cur->pid, UINT64_MAX);
                    sched_yield();
                    cur = proc_current();
                    if (!cur) return (uint64_t)-ESRCH;
                    shared = proc_shared_state(cur);
                    if (!shared || !shared->fd_ptrs[rfd]) return (uint64_t)-EBADF;
                    sk = (struct fry_socket *)shared->fd_ptrs[rfd];
                    if (!sk || !sk->used || sk->listen_handle < 0) return (uint64_t)-EBADF;
                    pp = &g_pipes[sk->listen_handle];
                    nr = pipe_read(pp, (char *)buf, a3, 0);
                    if (nr > 0) return (uint64_t)nr;
                    if (pp->writers == 0 && !pipe_data_avail(pp)) return 0;
                    return (uint64_t)-EAGAIN;
                }
                if (sk->tcp_handle < 0)
                    return (uint64_t)-ENOTCONN;
                net_poll();
                /* Try to read buffered data first — even if the peer has
                 * closed the connection, there may be data in the TCP
                 * receive buffer that arrived before the FIN. */
                int nr = tcp_recv(sk->tcp_handle, buf, (uint32_t)a3);
                if (nr > 0) {
                    static int recv_trace_count;
                    if (recv_trace_count < 200) {
                        kprint_serial_only("SYS_RECV: pid=%u got %d bytes\n",
                            cur->pid, nr);
                        recv_trace_count++;
                    }
                    return (uint64_t)nr;
                }
                /* No buffered data — check if connection is still alive */
                if (sk->state != SOCK_ST_CONNECTED ||
                    !tcp_is_connected(sk->tcp_handle)) return 0; /* EOF */
                if (nonblock) return (uint64_t)-EAGAIN;
                /* Block and retry */
                sched_block_poll(cur->pid, UINT64_MAX);
                sched_yield();
                cur = proc_current();
                if (!cur) return (uint64_t)-ESRCH;
                shared = proc_shared_state(cur);
                if (!shared || !shared->fd_ptrs[rfd]) return (uint64_t)-EBADF;
                sk = (struct fry_socket *)shared->fd_ptrs[rfd];
                if (!sk || !sk->used || sk->tcp_handle < 0) return (uint64_t)-EBADF;
                net_poll();
                nr = tcp_recv(sk->tcp_handle, buf, (uint32_t)a3);
                if (nr > 0) return (uint64_t)nr;
                if (!tcp_is_connected(sk->tcp_handle)) return 0;
                return (uint64_t)-EAGAIN;
            }
            if (sk->type == SOCK_DGRAM) {
                net_poll();
                if (sk->udp_rx_head != sk->udp_rx_tail) {
                    struct fry_udp_pkt *pkt = &sk->udp_rxq[sk->udp_rx_tail];
                    uint16_t copylen = pkt->len;
                    if (copylen > (uint16_t)a3) copylen = (uint16_t)a3;
                    for (uint16_t i = 0; i < copylen; i++) buf[i] = pkt->data[i];
                    sk->udp_rx_tail = (sk->udp_rx_tail + 1) % FRY_SOCK_UDP_RXMAX;
                    return (uint64_t)copylen;
                }
                if (nonblock) return (uint64_t)-EAGAIN;
                sched_block_poll(cur->pid, UINT64_MAX);
                sched_yield();
                cur = proc_current();
                if (!cur) return (uint64_t)-ESRCH;
                shared = proc_shared_state(cur);
                if (!shared || !shared->fd_ptrs[rfd]) return (uint64_t)-EBADF;
                sk = (struct fry_socket *)shared->fd_ptrs[rfd];
                if (!sk || !sk->used) return (uint64_t)-EBADF;
                net_poll();
                if (sk->udp_rx_head != sk->udp_rx_tail) {
                    struct fry_udp_pkt *pkt = &sk->udp_rxq[sk->udp_rx_tail];
                    uint16_t copylen = pkt->len;
                    if (copylen > (uint16_t)a3) copylen = (uint16_t)a3;
                    for (uint16_t i = 0; i < copylen; i++) buf[i] = pkt->data[i];
                    sk->udp_rx_tail = (sk->udp_rx_tail + 1) % FRY_SOCK_UDP_RXMAX;
                    return (uint64_t)copylen;
                }
                return (uint64_t)-EAGAIN;
            }
            return (uint64_t)-EINVAL;
        }

        case SYS_SHUTDOWN_SOCK: {
            /*
             * SYS_SHUTDOWN_SOCK(a1=fd, a2=how)
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int sfd = (int)a1;
            if (sfd < 3 || sfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[sfd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[sfd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;

            if (sk->type == SOCK_STREAM && sk->tcp_handle >= 0) {
                /* AF_UNIX socketpair: close pipe writers instead of TCP */
                if (sk->domain == 1 && sk->tcp_handle < FRY_PIPE_MAX && sk->listen_handle < FRY_PIPE_MAX) {
                    struct fry_pipe *pp = &g_pipes[sk->tcp_handle];
                    if (pp->writers > 0) pp->writers--;
                    pp = &g_pipes[sk->listen_handle];
                    if (pp->readers > 0) pp->readers--;
                } else {
                    tcp_close(sk->tcp_handle);
                }
                sk->tcp_handle = -1;
            }
            sk->state = SOCK_ST_SHUTDOWN;
            sched_wake_poll_waiters();
            return 0;
        }

        case SYS_GETSOCKOPT: {
            /*
             * SYS_GETSOCKOPT(a1=fd, a2=level, a3=optname, a4=optval_ptr, a5=optlen_ptr)
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int sfd = (int)a1;
            if (sfd < 3 || sfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[sfd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[sfd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;
            if ((int)a2 != SOL_SOCKET) return (uint64_t)-ENOPROTOOPT;

            uint32_t val = 0;
            switch ((int)a3) {
                case SO_REUSEADDR: val = sk->reuseaddr; break;
                case SO_RCVTIMEO:  val = sk->so_rcvtimeo; break;
                case SO_SNDTIMEO:  val = sk->so_sndtimeo; break;
                case SO_ERROR:     val = 0; break;
                case SO_KEEPALIVE: val = 0; break;
                default: return (uint64_t)-ENOPROTOOPT;
            }
            if (user_buf_writable(cur, a4, 4))
                copyout(cur, &val, a4, 4);
            return 0;
        }

        case SYS_SETSOCKOPT: {
            /*
             * SYS_SETSOCKOPT(a1=fd, a2=level, a3=optname, a4=optval_ptr, a5=optlen)
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int sfd = (int)a1;
            if (sfd < 3 || sfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[sfd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[sfd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;
            if ((int)a2 != SOL_SOCKET) return (uint64_t)-ENOPROTOOPT;

            uint32_t val = 0;
            if (a5 >= 4 && user_buf_mapped(cur, a4, 4))
                copyin(cur, a4, &val, 4);

            switch ((int)a3) {
                case SO_REUSEADDR: sk->reuseaddr = val ? 1 : 0; break;
                case SO_RCVTIMEO:  sk->so_rcvtimeo = val; break;
                case SO_SNDTIMEO:  sk->so_sndtimeo = val; break;
                case SO_KEEPALIVE: break; /* accept but ignore */
                default: return (uint64_t)-ENOPROTOOPT;
            }
            return 0;
        }

        case SYS_SENDTO: {
            /*
             * SYS_SENDTO(a1=fd, a2=buf, a3=len, a4=flags, a5=dest_addr_ptr)
             * Note: addrlen is implicit (sizeof(fry_sockaddr_in))
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int sfd = (int)a1;
            if (sfd < 3 || sfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[sfd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[sfd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;
            if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-EFAULT;

            const uint8_t *buf = (const uint8_t *)(uintptr_t)a2;
            uint32_t dst_ip = sk->remote_ip;
            uint16_t dst_port = sk->remote_port;

            if (a5) {
                if (!user_buf_mapped(cur, a5, sizeof(struct fry_sockaddr_in)))
                    return (uint64_t)-EFAULT;
                struct fry_sockaddr_in daddr;
                if (copyin(cur, a5, &daddr, sizeof(daddr)) != 0) return (uint64_t)-EFAULT;
                dst_ip = fry_ntohl(daddr.sin_addr);
                dst_port = fry_ntohs(daddr.sin_port);
            }

            if (dst_ip == 0 || dst_port == 0) return (uint64_t)-EDESTADDRREQ;

            if (sk->type == SOCK_DGRAM) {
                sock_ensure_udp_handler();
                if (sk->local_port == 0) sk->local_port = sock_ephemeral_port();
                int r = udp_send(dst_ip, dst_port, sk->local_port, buf, (uint16_t)a3);
                return r == 0 ? (uint64_t)a3 : (uint64_t)-EIO;
            }
            if (sk->type == SOCK_STREAM) {
                /* TCP sendto ignores dest — same as send */
                if (sk->state != SOCK_ST_CONNECTED || sk->tcp_handle < 0)
                    return (uint64_t)-ENOTCONN;
                int sent = tcp_send(sk->tcp_handle, buf, (uint16_t)a3);
                return sent < 0 ? (uint64_t)-EIO : (uint64_t)sent;
            }
            return (uint64_t)-EINVAL;
        }

        case SYS_RECVFROM: {
            /*
             * SYS_RECVFROM(a1=fd, a2=buf, a3=len, a4=flags, a5=src_addr_ptr)
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int rfd = (int)a1;
            if (rfd < 3 || rfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[rfd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[rfd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;

            uint8_t *buf = (uint8_t *)(uintptr_t)a2;
            uint32_t rflags = (uint32_t)a4;
            uint32_t fdflags = shared->fd_flags[rfd];
            int nonblock = (fdflags & O_NONBLOCK) || (rflags & MSG_DONTWAIT);

            if (sk->type == SOCK_DGRAM) {
                net_poll();
                if (sk->udp_rx_head == sk->udp_rx_tail) {
                    if (nonblock) return (uint64_t)-EAGAIN;
                    sched_block_poll(cur->pid, UINT64_MAX);
                    sched_yield();
                    cur = proc_current();
                    if (!cur) return (uint64_t)-ESRCH;
                    shared = proc_shared_state(cur);
                    if (!shared || !shared->fd_ptrs[rfd]) return (uint64_t)-EBADF;
                    sk = (struct fry_socket *)shared->fd_ptrs[rfd];
                    if (!sk || !sk->used) return (uint64_t)-EBADF;
                    net_poll();
                    if (sk->udp_rx_head == sk->udp_rx_tail)
                        return (uint64_t)-EAGAIN;
                }
                struct fry_udp_pkt *pkt = &sk->udp_rxq[sk->udp_rx_tail];
                uint16_t copylen = pkt->len;
                if (copylen > (uint16_t)a3) copylen = (uint16_t)a3;
                for (uint16_t i = 0; i < copylen; i++) buf[i] = pkt->data[i];
                /* Fill in source address if requested */
                if (a5 && user_buf_writable(cur, a5, sizeof(struct fry_sockaddr_in))) {
                    struct fry_sockaddr_in src;
                    uint8_t *sp = (uint8_t *)&src;
                    for (uint32_t j = 0; j < sizeof(src); j++) sp[j] = 0;
                    src.sin_family = AF_INET;
                    src.sin_port = fry_htons(pkt->src_port);
                    src.sin_addr = fry_htonl(pkt->src_ip);
                    copyout(cur, &src, a5, sizeof(src));
                }
                sk->udp_rx_tail = (sk->udp_rx_tail + 1) % FRY_SOCK_UDP_RXMAX;
                return (uint64_t)copylen;
            }
            if (sk->type == SOCK_STREAM) {
                /* TCP recvfrom ignores src addr — same as recv */
                if (sk->state != SOCK_ST_CONNECTED || sk->tcp_handle < 0)
                    return (uint64_t)-ENOTCONN;
                net_poll();
                int nr = tcp_recv(sk->tcp_handle, buf, (uint32_t)a3);
                if (nr > 0) return (uint64_t)nr;
                if (!tcp_is_connected(sk->tcp_handle)) return 0;
                if (nonblock) return (uint64_t)-EAGAIN;
                sched_block_poll(cur->pid, UINT64_MAX);
                sched_yield();
                cur = proc_current();
                if (!cur) return (uint64_t)-ESRCH;
                shared = proc_shared_state(cur);
                if (!shared || !shared->fd_ptrs[rfd]) return (uint64_t)-EBADF;
                sk = (struct fry_socket *)shared->fd_ptrs[rfd];
                if (!sk || !sk->used || sk->tcp_handle < 0) return (uint64_t)-EBADF;
                net_poll();
                nr = tcp_recv(sk->tcp_handle, buf, (uint32_t)a3);
                if (nr > 0) return (uint64_t)nr;
                if (!tcp_is_connected(sk->tcp_handle)) return 0;
                return (uint64_t)-EAGAIN;
            }
            return (uint64_t)-EINVAL;
        }

        case SYS_SENDMSG: {
            struct fry_msghdr msg;
            struct fry_process_shared *shared;
            struct fry_socket *sk;
            uint64_t total_len = 0;
            uint64_t copied = 0;
            uint8_t *linear;
            uint32_t dst_ip = 0;
            uint16_t dst_port = 0;

            (void)a3;
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_buf_mapped(cur, a2, sizeof(msg))) return (uint64_t)-EFAULT;
            if (copyin(cur, a2, &msg, sizeof(msg)) != 0) return (uint64_t)-EFAULT;
            if (msg.msg_control && msg.msg_controllen) return (uint64_t)-ENOTSUP;
            if (msg.msg_iovlen > 1024) return (uint64_t)-EINVAL;
            if (msg.msg_iovlen && !user_buf_mapped(cur, msg.msg_iov,
                                                   msg.msg_iovlen * sizeof(struct fry_iovec)))
                return (uint64_t)-EFAULT;

            shared = proc_shared_state(cur);
            int fd = (int)a1;
            if (fd < 3 || fd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[fd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            sk = (struct fry_socket *)shared->fd_ptrs[fd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;

            for (uint64_t i = 0; i < msg.msg_iovlen; i++) {
                struct fry_iovec iov;
                if (copyin(cur, msg.msg_iov + i * sizeof(iov), &iov, sizeof(iov)) != 0)
                    return (uint64_t)-EFAULT;
                if (iov.iov_len && !user_buf_mapped(cur, iov.iov_base, iov.iov_len))
                    return (uint64_t)-EFAULT;
                if (iov.iov_len > 0x7fffffffffffffffULL - total_len)
                    return (uint64_t)-EINVAL;
                total_len += iov.iov_len;
            }

            if (sk->type == SOCK_DGRAM && total_len > 1472) return (uint64_t)-EMSGSIZE;
            if (sk->type == SOCK_STREAM && total_len > 65535) total_len = 65535;
            if (sk->type != SOCK_DGRAM && sk->type != SOCK_STREAM) return (uint64_t)-EINVAL;

            linear = (uint8_t *)kmalloc(total_len ? total_len : 1);
            if (!linear) return (uint64_t)-ENOMEM;

            for (uint64_t i = 0; i < msg.msg_iovlen && copied < total_len; i++) {
                struct fry_iovec iov;
                uint64_t chunk;
                copyin(cur, msg.msg_iov + i * sizeof(iov), &iov, sizeof(iov));
                chunk = iov.iov_len;
                if (chunk > total_len - copied) chunk = total_len - copied;
                if (chunk) {
                    if (copyin(cur, iov.iov_base, linear + copied, chunk) != 0) {
                        kfree(linear);
                        return (uint64_t)-EFAULT;
                    }
                    copied += chunk;
                }
            }

            if (msg.msg_name) {
                struct fry_sockaddr_in daddr;
                if (msg.msg_namelen < sizeof(daddr)) {
                    kfree(linear);
                    return (uint64_t)-EINVAL;
                }
                if (!user_buf_mapped(cur, msg.msg_name, sizeof(daddr))) {
                    kfree(linear);
                    return (uint64_t)-EFAULT;
                }
                if (copyin(cur, msg.msg_name, &daddr, sizeof(daddr)) != 0) {
                    kfree(linear);
                    return (uint64_t)-EFAULT;
                }
                if (daddr.sin_family != AF_INET) {
                    kfree(linear);
                    return (uint64_t)-EAFNOSUPPORT;
                }
                dst_ip = fry_ntohl(daddr.sin_addr);
                dst_port = fry_ntohs(daddr.sin_port);
            } else {
                dst_ip = sk->remote_ip;
                dst_port = sk->remote_port;
            }

            if (sk->type == SOCK_DGRAM) {
                int r;
                if (dst_ip == 0 || dst_port == 0) {
                    kfree(linear);
                    return (uint64_t)-EDESTADDRREQ;
                }
                sock_ensure_udp_handler();
                if (sk->local_port == 0) sk->local_port = sock_ephemeral_port();
                r = udp_send(dst_ip, dst_port, sk->local_port, linear, (uint16_t)total_len);
                kfree(linear);
                return r == 0 ? (uint64_t)total_len : (uint64_t)-EIO;
            }

            if (sk->state != SOCK_ST_CONNECTED || sk->tcp_handle < 0) {
                kfree(linear);
                return (uint64_t)-ENOTCONN;
            }
            int sent = tcp_send(sk->tcp_handle, linear, (uint16_t)total_len);
            kfree(linear);
            return sent < 0 ? (uint64_t)-EIO : (uint64_t)sent;
        }

        case SYS_RECVMSG: {
            struct fry_msghdr msg;
            struct fry_process_shared *shared;
            struct fry_socket *sk;
            uint64_t total_len = 0;
            uint8_t *linear;
            uint64_t got = 0;
            uint64_t scattered = 0;
            uint32_t flags = (uint32_t)a3;
            int control_buffer_present;

            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_buf_writable(cur, a2, sizeof(msg))) return (uint64_t)-EFAULT;
            if (copyin(cur, a2, &msg, sizeof(msg)) != 0) return (uint64_t)-EFAULT;
            control_buffer_present = msg.msg_control && msg.msg_controllen;
            if (msg.msg_iovlen > 1024) return (uint64_t)-EINVAL;
            if (msg.msg_iovlen && !user_buf_mapped(cur, msg.msg_iov,
                                                   msg.msg_iovlen * sizeof(struct fry_iovec)))
                return (uint64_t)-EFAULT;

            shared = proc_shared_state(cur);
            int fd = (int)a1;
            if (fd < 3 || fd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[fd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            sk = (struct fry_socket *)shared->fd_ptrs[fd];
            if (!sk || !sk->used) return (uint64_t)-EBADF;

            for (uint64_t i = 0; i < msg.msg_iovlen; i++) {
                struct fry_iovec iov;
                if (copyin(cur, msg.msg_iov + i * sizeof(iov), &iov, sizeof(iov)) != 0)
                    return (uint64_t)-EFAULT;
                if (iov.iov_len && !user_buf_writable(cur, iov.iov_base, iov.iov_len))
                    return (uint64_t)-EFAULT;
                if (iov.iov_len > 0x7fffffffffffffffULL - total_len)
                    return (uint64_t)-EINVAL;
                total_len += iov.iov_len;
            }

            if (msg.msg_name && msg.msg_namelen < sizeof(struct fry_sockaddr_in))
                return (uint64_t)-EINVAL;
            if (msg.msg_name && !user_buf_writable(cur, msg.msg_name, sizeof(struct fry_sockaddr_in)))
                return (uint64_t)-EFAULT;

            if (sk->type == SOCK_STREAM && total_len == 0) {
                msg.msg_controllen = 0;
                msg.msg_flags = 0;
                copyout(cur, &msg, a2, sizeof(msg));
                return 0;
            }

            if (sk->type == SOCK_STREAM && total_len > 65535) total_len = 65535;
            if (sk->type == SOCK_DGRAM && total_len > FRY_SOCK_UDP_PKTSZ) total_len = FRY_SOCK_UDP_PKTSZ;
            linear = (uint8_t *)kmalloc(total_len ? total_len : 1);
            if (!linear) return (uint64_t)-ENOMEM;

            if (sk->type == SOCK_DGRAM) {
                uint32_t fdflags = shared->fd_flags[fd];
                int nonblock = (fdflags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
                net_poll();
                if (sk->udp_rx_head == sk->udp_rx_tail) {
                    if (nonblock) {
                        kfree(linear);
                        return (uint64_t)-EAGAIN;
                    }
                    sched_block_poll(cur->pid, UINT64_MAX);
                    sched_yield();
                    cur = proc_current();
                    if (!cur) {
                        kfree(linear);
                        return (uint64_t)-ESRCH;
                    }
                    shared = proc_shared_state(cur);
                    if (!shared || !shared->fd_ptrs[fd]) {
                        kfree(linear);
                        return (uint64_t)-EBADF;
                    }
                    sk = (struct fry_socket *)shared->fd_ptrs[fd];
                    if (!sk || !sk->used) {
                        kfree(linear);
                        return (uint64_t)-EBADF;
                    }
                    net_poll();
                    if (sk->udp_rx_head == sk->udp_rx_tail) {
                        kfree(linear);
                        return (uint64_t)-EAGAIN;
                    }
                }
                struct fry_udp_pkt *pkt = &sk->udp_rxq[sk->udp_rx_tail];
                got = pkt->len;
                if (got > total_len) {
                    got = total_len;
                    msg.msg_flags = MSG_TRUNC;
                } else {
                    msg.msg_flags = 0;
                }
                for (uint64_t j = 0; j < got; j++) linear[j] = pkt->data[j];
                if (msg.msg_name) {
                    struct fry_sockaddr_in src;
                    uint8_t *sp = (uint8_t *)&src;
                    for (uint32_t j = 0; j < sizeof(src); j++) sp[j] = 0;
                    src.sin_family = AF_INET;
                    src.sin_port = fry_htons(pkt->src_port);
                    src.sin_addr = fry_htonl(pkt->src_ip);
                    copyout(cur, &src, msg.msg_name, sizeof(src));
                    msg.msg_namelen = sizeof(src);
                }
                sk->udp_rx_tail = (sk->udp_rx_tail + 1) % FRY_SOCK_UDP_RXMAX;
            } else if (sk->type == SOCK_STREAM) {
                uint32_t fdflags = shared->fd_flags[fd];
                int nonblock = (fdflags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
                int nr;
                if (sk->state != SOCK_ST_CONNECTED || sk->tcp_handle < 0) {
                    kfree(linear);
                    return (uint64_t)-ENOTCONN;
                }
                net_poll();
                nr = tcp_recv(sk->tcp_handle, linear, (uint32_t)total_len);
                if (nr <= 0 && tcp_is_connected(sk->tcp_handle) && !nonblock) {
                    sched_block_poll(cur->pid, UINT64_MAX);
                    sched_yield();
                    cur = proc_current();
                    if (!cur) {
                        kfree(linear);
                        return (uint64_t)-ESRCH;
                    }
                    shared = proc_shared_state(cur);
                    if (!shared || !shared->fd_ptrs[fd]) {
                        kfree(linear);
                        return (uint64_t)-EBADF;
                    }
                    sk = (struct fry_socket *)shared->fd_ptrs[fd];
                    if (!sk || !sk->used || sk->tcp_handle < 0) {
                        kfree(linear);
                        return (uint64_t)-EBADF;
                    }
                    net_poll();
                    nr = tcp_recv(sk->tcp_handle, linear, (uint32_t)total_len);
                }
                if (nr < 0) {
                    kfree(linear);
                    return nonblock ? (uint64_t)-EAGAIN : (uint64_t)-EIO;
                }
                if (nr == 0 && tcp_is_connected(sk->tcp_handle)) {
                    kfree(linear);
                    return nonblock ? (uint64_t)-EAGAIN : 0;
                }
                got = (uint64_t)nr;
                msg.msg_flags = 0;
                msg.msg_namelen = 0;
            } else {
                kfree(linear);
                return (uint64_t)-EINVAL;
            }

            msg.msg_controllen = 0;
            if (control_buffer_present) msg.msg_flags |= MSG_CTRUNC;
            for (uint64_t i = 0; i < msg.msg_iovlen && scattered < got; i++) {
                struct fry_iovec iov;
                uint64_t chunk;
                copyin(cur, msg.msg_iov + i * sizeof(iov), &iov, sizeof(iov));
                chunk = iov.iov_len;
                if (chunk > got - scattered) chunk = got - scattered;
                if (chunk) {
                    if (copyout(cur, linear + scattered, iov.iov_base, chunk) != 0) {
                        kfree(linear);
                        return (uint64_t)-EFAULT;
                    }
                    scattered += chunk;
                }
            }
            copyout(cur, &msg, a2, sizeof(msg));
            kfree(linear);
            return got;
        }

        case SYS_DNS_RESOLVE: {
            /*
             * SYS_DNS_RESOLVE(a1=hostname_ptr, a2=ip_out_ptr)
             * Returns: 0 on success, -errno on failure.
             * Writes resolved IP (host byte order) to *ip_out.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            char hostname[128];
            if (copy_user_string(cur, a1, hostname, sizeof(hostname)) != 0)
                return (uint64_t)-EFAULT;
            if (!user_buf_writable(cur, a2, 4)) return (uint64_t)-EFAULT;

            kprint_serial_only("SYS_DNS: pid=%u host=\"%s\"\n",
                   cur->pid, hostname);
            uint32_t ip = dns_resolve(hostname);
            if (ip == 0) {
                kprint_serial_only("SYS_DNS: failed for \"%s\"\n", hostname);
                return (uint64_t)-ENOENT;
            }

            copyout(cur, &ip, a2, 4);
            return 0;
        }

        /* ===== Phase 5: Randomness, Time, and Core Runtime ===== */

        case 85: /* SYS_GETRANDOM(buf, len, flags) */ {
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            uint32_t len = (uint32_t)a2;
            uint32_t flags = (uint32_t)a3;
            if (len == 0) return 0;
            if (len > FRY_RANDOM_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, len)) return (uint64_t)-EFAULT;

            if (!entropy_ready()) {
                if (flags & FRY_GRND_NONBLOCK) return (uint64_t)-EAGAIN;
                /* Entropy should always be ready after boot; if not, fail. */
                return (uint64_t)-EAGAIN;
            }

            uint8_t kbuf[FRY_RANDOM_MAX];
            int rc = entropy_getbytes(kbuf, len);
            if (rc < 0) return (uint64_t)rc;

            copyout(cur, kbuf, a1, len);
            return (uint64_t)len;
        }

        case 86: /* SYS_CLOCK_GETTIME(clock_id, timespec_ptr) */ {
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            uint32_t clock_id = (uint32_t)a1;
            if (!user_buf_writable(cur, a2, sizeof(struct fry_timespec)))
                return (uint64_t)-EFAULT;

            struct fry_timespec ts;
            int64_t sec, nsec;

            switch (clock_id) {
                case FRY_CLOCK_MONOTONIC:
                case FRY_CLOCK_BOOTTIME:
                    hpet_get_ns(&sec, &nsec);
                    ts.tv_sec = sec;
                    ts.tv_nsec = nsec;
                    break;
                case FRY_CLOCK_REALTIME: {
                    hpet_get_ns(&sec, &nsec);
                    int64_t boot_epoch = rtc_boot_epoch_sec();
                    ts.tv_sec = boot_epoch + sec;
                    ts.tv_nsec = nsec;
                    break;
                }
                default:
                    return (uint64_t)-EINVAL;
            }

            copyout(cur, &ts, a2, sizeof(struct fry_timespec));
            return 0;
        }

        case 87: /* SYS_NANOSLEEP(req_ptr, rem_ptr) */ {
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_ptr_ok(a1, sizeof(struct fry_timespec)))
                return (uint64_t)-EFAULT;

            struct fry_timespec req;
            {
                const uint8_t *src = (const uint8_t *)(uintptr_t)a1;
                uint8_t *dst = (uint8_t *)&req;
                for (uint64_t i = 0; i < sizeof(req); i++) dst[i] = src[i];
            }

            if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000LL)
                return (uint64_t)-EINVAL;

            /* Convert to milliseconds (minimum 1ms granularity for scheduler) */
            uint64_t ms = (uint64_t)req.tv_sec * 1000ULL
                        + (uint64_t)(req.tv_nsec + 999999LL) / 1000000ULL;
            if (ms == 0) ms = 1;  /* sub-millisecond sleeps round up to 1ms */

            sched_sleep(cur->pid, ms);
            sched_yield();

            /* Write zero remainder (we slept the full duration) */
            if (a2 && user_buf_writable(cur, a2, sizeof(struct fry_timespec))) {
                struct fry_timespec rem = {0, 0};
                copyout(cur, &rem, a2, sizeof(struct fry_timespec));
            }
            return 0;
        }

        /* ===== Phase 6: Filesystem and Runtime Expansion ===== */

        case 88: /* SYS_LSEEK(fd, offset, whence) */ {
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int fd = (int)a1;
            int64_t offset = (int64_t)a2;
            int whence = (int)a3;
            if (fd < 3 || fd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            struct fry_process_shared *shared = proc_shared_state(cur);
            if (!shared || !shared->fd_ptrs[fd]) return (uint64_t)-EBADF;
            uint8_t lkind = shared->fd_kind[fd];
            if (lkind == FD_FILE) {
                if (whence != FRY_SEEK_SET && whence != FRY_SEEK_CUR && whence != FRY_SEEK_END)
                    return (uint64_t)-EINVAL;
                int64_t result = vfs_seek((struct vfs_file *)shared->fd_ptrs[fd], offset, whence);
                if (result < 0) return (uint64_t)-EINVAL;
                return (uint64_t)result;
            }
            if (lkind == FD_MEMFD) {
                struct memfd_cb *mf = (struct memfd_cb *)shared->fd_ptrs[fd];
                if (!mf || !mf->used) return (uint64_t)-EBADF;
                int64_t result = memfd_lseek(mf, offset, whence);
                if (result < 0) return (uint64_t)(-result);
                return (uint64_t)result;
            }
            return (uint64_t)-EBADF;
        }

        case 89: /* SYS_FTRUNCATE(fd, length) */ {
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int fd = (int)a1;
            uint64_t length = (uint64_t)a2;
            if (fd < 3 || fd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            struct fry_process_shared *shared = proc_shared_state(cur);
            if (!shared || !shared->fd_ptrs[fd]) return (uint64_t)-EBADF;
            uint8_t tkind = shared->fd_kind[fd];
            if (tkind == FD_FILE) {
                int rc = vfs_truncate((struct vfs_file *)shared->fd_ptrs[fd], length);
                if (rc < 0) return (uint64_t)-EIO;
                return 0;
            }
            if (tkind == FD_MEMFD) {
                struct memfd_cb *mf = (struct memfd_cb *)shared->fd_ptrs[fd];
                if (!mf || !mf->used) return (uint64_t)-EBADF;
                int rc = memfd_truncate(mf, length);
                if (rc < 0) return (uint64_t)-ENOMEM;
                return 0;
            }
            return (uint64_t)-EBADF;
        }

        case 90: /* SYS_RENAME(old_path, new_path) */ {
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            char old_path[FRY_PATH_MAX], new_path[FRY_PATH_MAX];
            if (copy_user_string(cur, a1, old_path, sizeof(old_path)) != 0)
                return (uint64_t)-EFAULT;
            if (copy_user_string(cur, a2, new_path, sizeof(new_path)) != 0)
                return (uint64_t)-EFAULT;
            int rc = vfs_rename(old_path, new_path);
            if (rc < 0) return (uint64_t)-EIO;
            return 0;
        }

        case 91: /* SYS_FSTAT(fd, stat_buf) */ {
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int fd = (int)a1;
            if (fd < 3 || fd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            struct fry_process_shared *shared = proc_shared_state(cur);
            if (!shared || !shared->fd_ptrs[fd] || shared->fd_kind[fd] == FD_NONE)
                return (uint64_t)-EBADF;
            if (!user_buf_writable(cur, a2, sizeof(struct vfs_stat)))
                return (uint64_t)-EFAULT;
            if (shared->fd_kind[fd] == FD_DIR) {
                struct vfs_stat st;
                if (!shared->fd_paths[fd][0]) return (uint64_t)-EBADF;
                if (vfs_stat(shared->fd_paths[fd], &st) != 0) return (uint64_t)-ENOENT;
                copyout(cur, &st, a2, sizeof(struct vfs_stat));
                return 0;
            }
            if (shared->fd_kind[fd] != FD_FILE)
                return (uint64_t)-EBADF;
            struct vfs_file *vf = (struct vfs_file *)shared->fd_ptrs[fd];
            struct vfs_stat st;
            st.size = vf->size;
            /* Extract attr from the underlying fat32_file if available */
            st.attr = 0;
            if (sizeof(struct fat32_file) <= sizeof(vf->private)) {
                struct fat32_file *ff = (struct fat32_file *)vf->private;
                if (ff->fs) st.attr = (uint32_t)ff->attr;
            }
            copyout(cur, &st, a2, sizeof(struct vfs_stat));
            return 0;
        }

        /* ---- Phase 7: GUI/Input expansion ---- */

        case SYS_KBD_EVENT: /* SYS_KBD_EVENT(event_buf) */ {
            /* Returns one rich key event from the keyboard event ring buffer.
             * a1 = pointer to fry_key_event struct (8 bytes)
             * Returns: 1 if event was copied, 0 if no events pending */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_buf_writable(cur, a1, sizeof(struct fry_key_event)))
                return (uint64_t)-EFAULT;
            struct fry_key_event evt;
            int got = ps2_kbd_read_event(&evt);
            if (got) {
                copyout(cur, &evt, a1, sizeof(struct fry_key_event));
                return 1;
            }
            return 0;
        }

        case SYS_MOUSE_GET_EXT: /* SYS_MOUSE_GET_EXT(buf) */ {
            /* Extended mouse state including wheel.  Identical to
             * SYS_MOUSE_GET but explicit about being the 24-byte version.
             * Kept as a separate syscall for future extensibility. */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_buf_writable(cur, a1, 24)) return (uint64_t)-EFAULT;
            int32_t mx, my, mdx, mdy, mwheel;
            uint8_t mb;
            ps2_mouse_get_ext(&mx, &my, &mb, &mdx, &mdy, &mwheel);
            int32_t *out = (int32_t *)(uintptr_t)a1;
            out[0] = mx;
            out[1] = my;
            out[2] = mdx;
            out[3] = mdy;
            *((uint8_t *)(out + 4)) = mb;
            out[5] = mwheel;
            return 0;
        }

        case SYS_CLIPBOARD_GET: /* SYS_CLIPBOARD_GET(buf, maxlen) */ {
            /* Copy kernel clipboard to user buffer.
             * a1 = user buf ptr, a2 = max length
             * Returns: bytes copied (may be 0 if clipboard empty) */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            uint32_t maxlen = (uint32_t)a2;
            if (maxlen == 0) return 0;
            if (maxlen > FRY_CLIPBOARD_MAX) maxlen = FRY_CLIPBOARD_MAX;
            if (!user_buf_writable(cur, a1, maxlen)) return (uint64_t)-EFAULT;
            uint32_t copy = g_clipboard_len;
            if (copy > maxlen) copy = maxlen;
            if (copy > 0) copyout(cur, g_clipboard_buf, a1, copy);
            return (uint64_t)copy;
        }

        case SYS_CLIPBOARD_SET: /* SYS_CLIPBOARD_SET(buf, len) */ {
            /* Set kernel clipboard from user buffer.
             * a1 = user buf ptr, a2 = length
             * Returns: 0 on success */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            uint32_t len = (uint32_t)a2;
            if (len > FRY_CLIPBOARD_MAX) return (uint64_t)-EINVAL;
            if (len > 0 && !user_buf_mapped(cur, a1, len))
                return (uint64_t)-EFAULT;
            if (len > 0) {
                const uint8_t *src = (const uint8_t *)(uintptr_t)a1;
                for (uint32_t i = 0; i < len; i++)
                    g_clipboard_buf[i] = src[i];
            }
            g_clipboard_len = len;
            return 0;
        }

        /* ---- Audio syscalls (TaterSurf Phase D) ---- */

        case SYS_AUDIO_OPEN: {
            /* SYS_AUDIO_OPEN(sample_rate, channels, bits)
             * Opens audio output stream.
             * Returns: 0 on success, -errno on failure */
            int hda_is_ready(void);
            int hda_open_output(uint32_t sr, uint8_t ch, uint8_t b);
            if (!hda_is_ready()) return (uint64_t)-19; /* ENODEV */
            return (uint64_t)hda_open_output((uint32_t)a1, (uint8_t)a2, (uint8_t)a3);
        }

        case SYS_AUDIO_WRITE: {
            /* SYS_AUDIO_WRITE(buf, len)
             * Write PCM samples to audio output.
             * Returns: bytes written or -errno */
            int hda_write_pcm(const void *data, uint32_t len);
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (a2 == 0) return 0;
            if (!user_buf_mapped(cur, a1, (uint32_t)a2))
                return (uint64_t)-EFAULT;
            return (uint64_t)hda_write_pcm((const void *)(uintptr_t)a1, (uint32_t)a2);
        }

        case SYS_AUDIO_CLOSE: {
            /* SYS_AUDIO_CLOSE()
             * Close audio output stream.
             * Returns: 0 */
            void hda_close_output(void);
            hda_close_output();
            return 0;
        }

        case SYS_AUDIO_INFO: {
            /* SYS_AUDIO_INFO(info_buf)
             * Get audio stream info (8 bytes: rate(4) + channels(1) + bits(1) + active(1) + pad(1)).
             * Returns: 0 on success */
            int hda_get_stream_info(void *info);
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_buf_mapped(cur, a1, 8))
                return (uint64_t)-EFAULT;
            return (uint64_t)hda_get_stream_info((void *)(uintptr_t)a1);
        }

        /* ===== Chrome Port — POSIX Expansion (Phase 1/2) ===== */

        case SYS_OPENAT: {
            char raw_path[FRY_PATH_MAX];
            char path[FRY_PATH_MAX];
            struct fry_process_shared *shared;
            int dirfd = (int)a1;
            uint32_t flags = (uint32_t)a3;
            struct vfs_file *f;
            int fd;

            if (copy_user_string(cur, a2, raw_path, sizeof(raw_path)) != 0) return (uint64_t)-EFAULT;
            if (!cur) return (uint64_t)-ESRCH;
            shared = proc_shared_state(cur);
            if (!shared) return (uint64_t)-ESRCH;
            int rpath = resolve_at_path(cur, dirfd, raw_path, path);
            if (rpath < 0) return (uint64_t)rpath;

            struct vfs_stat st;
            if (vfs_stat(path, &st) == 0 && (st.attr & 0x10u)) {
                if ((flags & FRY_O_ACCMODE) != O_RDONLY) return (uint64_t)-EISDIR;
                fd = fd_alloc(cur);
                if (fd < 0) return (uint64_t)-EMFILE;
                fd_install(cur, fd, shared->fd_paths[fd], FD_DIR, flags & O_NONBLOCK);
                shared->fd_table[fd] = 0;
                int prc = install_fd_path(cur, fd, path);
                if (prc < 0) return (uint64_t)prc;
                return (uint64_t)fd;
            }
            if (flags & FRY_O_DIRECTORY) return (uint64_t)-ENOTDIR;

            f = vfs_open(path);
            if (!f && (flags & O_CREAT)) {
                vfs_create(path, 1);  /* TOTFS_TYPE_FILE */
                f = vfs_open(path);
            }
            if (!f) return (uint64_t)-ENOENT;

            fd = fd_alloc(cur);
            if (fd < 0) {
                vfs_close(f);
                return (uint64_t)-EMFILE;
            }
            fd_install(cur, fd, f, FD_FILE, flags & O_NONBLOCK);
            int prc = install_fd_path(cur, fd, path);
            if (prc < 0) {
                vfs_close(f);
                return (uint64_t)prc;
            }
            return (uint64_t)fd;
        }

        case SYS_READV: {
            int fd = (int)a1;
            uint64_t iov_user = a2;
            int iovcnt;
            uint64_t total_len = 0;
            uint64_t done = 0;

            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if ((int64_t)a3 < 0 || a3 > 1024) return (uint64_t)-EINVAL;
            iovcnt = (int)a3;
            if (iovcnt == 0) return 0;
            if (!user_buf_mapped(cur, iov_user, (uint64_t)iovcnt * sizeof(struct fry_iovec)))
                return (uint64_t)-EFAULT;

            for (int i = 0; i < iovcnt; i++) {
                struct fry_iovec iov;
                if (copyin(cur, iov_user + (uint64_t)i * sizeof(iov), &iov, sizeof(iov)) != 0)
                    return (uint64_t)-EFAULT;
                if (iov.iov_len > 0 && !user_buf_writable(cur, iov.iov_base, iov.iov_len))
                    return (uint64_t)-EFAULT;
                if (iov.iov_len > 0x7fffffffffffffffULL - total_len)
                    return (uint64_t)-EINVAL;
                total_len += iov.iov_len;
            }

            for (int i = 0; i < iovcnt; i++) {
                struct fry_iovec iov;
                uint64_t rc;
                copyin(cur, iov_user + (uint64_t)i * sizeof(iov), &iov, sizeof(iov));
                if (iov.iov_len == 0) continue;
                rc = syscall_dispatch(SYS_READ, (uint64_t)fd, iov.iov_base, iov.iov_len, 0, 0, 0);
                if ((int64_t)rc < 0) return done ? done : rc;
                done += rc;
                if (rc < iov.iov_len) break;
            }
            return done;
        }

        case SYS_WRITEV: {
            int fd = (int)a1;
            uint64_t iov_user = a2;
            int iovcnt;
            uint64_t total_len = 0;
            uint64_t done = 0;

            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if ((int64_t)a3 < 0 || a3 > 1024) return (uint64_t)-EINVAL;
            iovcnt = (int)a3;
            if (iovcnt == 0) return 0;
            if (!user_buf_mapped(cur, iov_user, (uint64_t)iovcnt * sizeof(struct fry_iovec)))
                return (uint64_t)-EFAULT;

            for (int i = 0; i < iovcnt; i++) {
                struct fry_iovec iov;
                if (copyin(cur, iov_user + (uint64_t)i * sizeof(iov), &iov, sizeof(iov)) != 0)
                    return (uint64_t)-EFAULT;
                if (iov.iov_len > 0 && !user_buf_mapped(cur, iov.iov_base, iov.iov_len))
                    return (uint64_t)-EFAULT;
                if (iov.iov_len > 0x7fffffffffffffffULL - total_len)
                    return (uint64_t)-EINVAL;
                total_len += iov.iov_len;
            }

            for (int i = 0; i < iovcnt; i++) {
                struct fry_iovec iov;
                uint64_t rc;
                copyin(cur, iov_user + (uint64_t)i * sizeof(iov), &iov, sizeof(iov));
                if (iov.iov_len == 0) continue;
                rc = syscall_dispatch(SYS_WRITE, (uint64_t)fd, iov.iov_base, iov.iov_len, 0, 0, 0);
                if ((int64_t)rc < 0) return done ? done : rc;
                done += rc;
                if (rc < iov.iov_len) break;
            }
            return done;
        }

        case 101: { /* SYS_EPOLL_CREATE(size) → fd */
            if (!cur) return (uint64_t)-ESRCH;
            (void)a1;
            int fd = fd_alloc(cur);
            if (fd < 0) return (uint64_t)-EMFILE;
            struct epoll_cb *ep = (struct epoll_cb *)kmalloc(sizeof(struct epoll_cb));
            if (!ep) return (uint64_t)-ENOMEM;
            ep->count = 0;
            ep->items = 0;
            ep->lock = 0;
            fd_install(cur, fd, ep, FD_EPOLL, 0);
            return (uint64_t)fd;
        }

        case 102: { /* SYS_EPOLL_CTL(epfd, op, fd, event_ptr) → 0 */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int epfd = (int)a1, op = (int)a2, target_fd = (int)a3;
            if (epfd < 3 || (uint32_t)epfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            struct fry_process_shared *shr = proc_shared_state(cur);
            if (shr->fd_kind[epfd] != FD_EPOLL || !shr->fd_ptrs[epfd])
                return (uint64_t)-EBADF;
            struct epoll_cb *ep = (struct epoll_cb *)shr->fd_ptrs[epfd];
            struct epoll_event ev;
            if (a4 && copyin(cur, a4, &ev, sizeof(ev)) != 0)
                return (uint64_t)-EFAULT;
            while (__sync_lock_test_and_set(&ep->lock, 1)) {}
            if (op == EPOLL_CTL_ADD) {
                struct epoll_item *item = (struct epoll_item *)kmalloc(sizeof(struct epoll_item));
                if (!item) { ep->lock = 0; return (uint64_t)-ENOMEM; }
                item->fd = target_fd;
                item->events = ev.events;
                item->data = ev.data;
                item->next = ep->items;
                ep->items = item;
                ep->count++;
            } else if (op == EPOLL_CTL_DEL) {
                struct epoll_item **pp = &ep->items;
                while (*pp) {
                    if ((*pp)->fd == target_fd) {
                        struct epoll_item *tmp = *pp;
                        *pp = tmp->next;
                        kfree(tmp);
                        ep->count--;
                        break;
                    }
                    pp = &(*pp)->next;
                }
            } else if (op == EPOLL_CTL_MOD) {
                struct epoll_item *item = ep->items;
                while (item) {
                    if (item->fd == target_fd) {
                        item->events = ev.events;
                        item->data = ev.data;
                        break;
                    }
                    item = item->next;
                }
            }
            ep->lock = 0;
            return 0;
        }

        case 103: { /* SYS_EPOLL_WAIT(epfd, events, maxevents, timeout_ms) → count */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int epfd = (int)a1, maxevents = (int)a3;
            int64_t timeout = (int64_t)a4;
            if (epfd < 3 || (uint32_t)epfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            struct fry_process_shared *shr2 = proc_shared_state(cur);
            if (shr2->fd_kind[epfd] != FD_EPOLL || !shr2->fd_ptrs[epfd])
                return (uint64_t)-EBADF;
            if (!user_buf_writable(cur, a2, (uint64_t)maxevents * sizeof(struct epoll_event)))
                return (uint64_t)-EFAULT;
            struct epoll_cb *ep = (struct epoll_cb *)shr2->fd_ptrs[epfd];
            struct epoll_event results[64];
            int n = 0;
            uint64_t now_ms = hpet_read_counter() * 1000ULL / hpet_get_freq_hz();
            uint64_t wake_ms = (timeout >= 0) ? now_ms + (uint64_t)timeout : UINT64_MAX;
            while (n == 0) {
                while (__sync_lock_test_and_set(&ep->lock, 1)) {}
                struct epoll_item *item = ep->items;
                while (item && n < maxevents && n < 64) {
                    uint16_t rev = poll_check_fd(cur, item->fd,
                        (uint16_t)(item->events & (FRY_POLLIN | FRY_POLLOUT | FRY_POLLERR)));
                    if (rev) {
                        results[n].events = rev;
                        results[n].data = item->data;
                        n++;
                    }
                    item = item->next;
                }
                ep->lock = 0;
                if (n > 0) break;
                uint64_t cur_ms = hpet_read_counter() * 1000ULL / hpet_get_freq_hz();
                if (cur_ms >= wake_ms) break;
                sched_block_poll(cur->pid, wake_ms);
                sched_yield();
                cur = proc_current();
                if (!cur) return (uint64_t)-ESRCH;
            }
            if (n > 0)
                copyout(cur, results, a2, (uint64_t)n * sizeof(struct epoll_event));
            return (uint64_t)n;
        }

        case 104: { /* SYS_EVENTFD(initval, flags) → fd */
            if (!cur) return (uint64_t)-ESRCH;
            int fd = fd_alloc(cur);
            if (fd < 0) return (uint64_t)-EMFILE;
            struct eventfd_cb *ev = (struct eventfd_cb *)kmalloc(sizeof(struct eventfd_cb));
            if (!ev) return (uint64_t)-ENOMEM;
            uint64_t valid_flags = 0x1ULL | (uint64_t)O_NONBLOCK | (uint64_t)O_CLOEXEC;
            if (a2 & ~valid_flags) return (uint64_t)-EINVAL;
            ev->counter = (uint64_t)a1;
            ev->semaphore = (a2 & 0x1) != 0;
            ev->nonblock = (a2 & O_NONBLOCK) != 0;
            ev->lock = 0;
            fd_install(cur, fd, ev, FD_EVENTFD, (a2 & O_NONBLOCK) ? O_NONBLOCK : 0);
            return (uint64_t)fd;
        }

        case SYS_FACCESSAT: {
            char raw_path[FRY_PATH_MAX];
            char path[FRY_PATH_MAX];
            struct vfs_stat st;
            int dirfd = (int)a1;
            int mode = (int)a3;
            uint32_t flags = (uint32_t)a4;

            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (mode & ~(FRY_R_OK | FRY_W_OK | FRY_X_OK)) return (uint64_t)-EINVAL;
            if (flags & ~(FRY_AT_EACCESS | FRY_AT_SYMLINK_NOFOLLOW | FRY_AT_EMPTY_PATH))
                return (uint64_t)-EINVAL;
            if (copy_user_string(cur, a2, raw_path, sizeof(raw_path)) != 0) return (uint64_t)-EFAULT;

            if ((flags & FRY_AT_EMPTY_PATH) && raw_path[0] == '\0') {
                if (dirfd < 0 || dirfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
                struct fry_process_shared *shared = proc_shared_state(cur);
                if (!shared || shared->fd_kind[dirfd] == FD_NONE || !shared->fd_ptrs[dirfd])
                    return (uint64_t)-EBADF;
                return 0;
            }

            int rpath = resolve_at_path(cur, dirfd, raw_path, path);
            if (rpath < 0) return (uint64_t)rpath;
            if (vfs_stat(path, &st) != 0) return (uint64_t)-ENOENT;

            /*
             * TaterTOS currently has no uid/gid permission enforcement, so
             * an existing node is readable/writable/executable to callers.
             */
            (void)st;
            return 0;
        }

        case SYS_READLINKAT: {
            char raw_path[FRY_PATH_MAX];
            char path[FRY_PATH_MAX];
            struct vfs_stat st;
            int dirfd = (int)a1;
            uint64_t bufsiz = a4;

            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (bufsiz == 0) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a3, bufsiz)) return (uint64_t)-EFAULT;
            if (copy_user_string(cur, a2, raw_path, sizeof(raw_path)) != 0) return (uint64_t)-EFAULT;
            int rpath = resolve_at_path(cur, dirfd, raw_path, path);
            if (rpath < 0) return (uint64_t)rpath;
            if (vfs_stat(path, &st) != 0) return (uint64_t)-ENOENT;

            /*
             * No mounted TaterTOS filesystem exposes symbolic-link metadata
             * yet. Return Linux-compatible EINVAL for existing non-symlinks
             * rather than manufacturing a fake target.
             */
            (void)st;
            return (uint64_t)-EINVAL;
        }

        case SYS_FSTATAT: {
            char raw_path[FRY_PATH_MAX];
            char path[FRY_PATH_MAX];
            struct vfs_stat st;
            int dirfd = (int)a1;
            uint32_t flags = (uint32_t)a4;

            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_buf_writable(cur, a3, sizeof(struct vfs_stat))) return (uint64_t)-EFAULT;
            if (flags & ~(FRY_AT_SYMLINK_NOFOLLOW | FRY_AT_NO_AUTOMOUNT | FRY_AT_EMPTY_PATH))
                return (uint64_t)-EINVAL;
            if (copy_user_string(cur, a2, raw_path, sizeof(raw_path)) != 0) return (uint64_t)-EFAULT;

            if ((flags & FRY_AT_EMPTY_PATH) && raw_path[0] == '\0') {
                if (dirfd < 3 || dirfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
                struct fry_process_shared *shared = proc_shared_state(cur);
                if (!shared || !shared->fd_ptrs[dirfd] || shared->fd_kind[dirfd] == FD_NONE)
                    return (uint64_t)-EBADF;
                if (shared->fd_kind[dirfd] == FD_DIR) {
                    if (!shared->fd_paths[dirfd][0]) return (uint64_t)-EBADF;
                    if (vfs_stat(shared->fd_paths[dirfd], &st) != 0) return (uint64_t)-ENOENT;
                    copyout(cur, &st, a3, sizeof(st));
                    return 0;
                }
                if (shared->fd_kind[dirfd] != FD_FILE) return (uint64_t)-EBADF;
                struct vfs_file *vf = (struct vfs_file *)shared->fd_ptrs[dirfd];
                st.size = vf->size;
                st.attr = 0;
                if (sizeof(struct fat32_file) <= sizeof(vf->private)) {
                    struct fat32_file *ff = (struct fat32_file *)vf->private;
                    if (ff->fs) st.attr = (uint32_t)ff->attr;
                }
                copyout(cur, &st, a3, sizeof(st));
                return 0;
            }

            int rpath = resolve_at_path(cur, dirfd, raw_path, path);
            if (rpath < 0) return (uint64_t)rpath;
            if (vfs_stat(path, &st) != 0) return (uint64_t)-ENOENT;
            copyout(cur, &st, a3, sizeof(st));
            return 0;
        }

        case SYS_MKDIRAT: {
            char raw_path[FRY_PATH_MAX];
            char path[FRY_PATH_MAX];
            int dirfd = (int)a1;

            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (copy_user_string(cur, a2, raw_path, sizeof(raw_path)) != 0) return (uint64_t)-EFAULT;
            int rpath = resolve_at_path(cur, dirfd, raw_path, path);
            if (rpath < 0) return (uint64_t)rpath;

            struct vfs_stat st;
            if (vfs_stat(path, &st) == 0) return (uint64_t)-EEXIST;
            int rc = vfs_mkdir(path);
            return (rc < 0) ? (uint64_t)-EIO : 0;
        }

        case SYS_UNLINKAT: {
            char raw_path[FRY_PATH_MAX];
            char path[FRY_PATH_MAX];
            int dirfd = (int)a1;
            uint32_t flags = (uint32_t)a3;

            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (flags & ~FRY_AT_REMOVEDIR) return (uint64_t)-EINVAL;
            if (copy_user_string(cur, a2, raw_path, sizeof(raw_path)) != 0) return (uint64_t)-EFAULT;
            int rpath = resolve_at_path(cur, dirfd, raw_path, path);
            if (rpath < 0) return (uint64_t)rpath;

            struct vfs_stat st;
            if (vfs_stat(path, &st) != 0) return (uint64_t)-ENOENT;
            if ((st.attr & 0x10u) && !(flags & FRY_AT_REMOVEDIR)) return (uint64_t)-EISDIR;
            int rc = vfs_unlink(path);
            return (rc < 0) ? (uint64_t)-EIO : 0;
        }

        case SYS_RENAMEAT: {
            char old_raw[FRY_PATH_MAX];
            char new_raw[FRY_PATH_MAX];
            char old_path[FRY_PATH_MAX];
            char new_path[FRY_PATH_MAX];
            int olddirfd = (int)a1;
            int newdirfd = (int)a3;

            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (copy_user_string(cur, a2, old_raw, sizeof(old_raw)) != 0) return (uint64_t)-EFAULT;
            if (copy_user_string(cur, a4, new_raw, sizeof(new_raw)) != 0) return (uint64_t)-EFAULT;
            int orc = resolve_at_path(cur, olddirfd, old_raw, old_path);
            if (orc < 0) return (uint64_t)orc;
            int nrc = resolve_at_path(cur, newdirfd, new_raw, new_path);
            if (nrc < 0) return (uint64_t)nrc;

            struct vfs_stat st;
            if (vfs_stat(old_path, &st) != 0) return (uint64_t)-ENOENT;
            int rc = vfs_rename(old_path, new_path);
            return (rc < 0) ? (uint64_t)-EIO : 0;
        }

        /* ================================================================
         * Phase 8: getcwd/chdir cleanup, pipe2, dup3, socketpair
         * ================================================================ */
        case SYS_CHDIR: {
            /*
             * SYS_CHDIR(a1=path_ptr)
             * Changes the process's current working directory.
             * The provided path is resolved through the dirfd resolver using
             * AT_FDCWD, then normalized and stored in shared->cwd.
             * Returns 0 on success, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            char raw[FRY_PATH_MAX];
            if (copy_user_string(cur, a1, raw, sizeof(raw)) != 0)
                return (uint64_t)-EFAULT;

            /* Resolve relative/absolute path through the dirfd resolver */
            char resolved[FRY_PATH_MAX];
            int rc = resolve_at_path(cur, FRY_AT_FDCWD, raw, resolved);
            if (rc < 0) return (uint64_t)rc;

            /* Verify the target is actually a directory */
            struct vfs_stat st;
            if (vfs_stat(resolved, &st) != 0) return (uint64_t)-ENOENT;
            if (!(st.attr & 0x10)) return (uint64_t)-ENOTDIR;

            /* Store normalized path */
            uint32_t i;
            for (i = 0; i < FRY_PATH_MAX - 1 && resolved[i]; i++)
                shared->cwd[i] = resolved[i];
            shared->cwd[i] = 0;
            return 0;
        }

        case SYS_GETCWD: {
            /*
             * SYS_GETCWD(a1=user_buf_ptr, a2=buf_size)
             * Copies the current working directory path to user buffer.
             * Returns number of bytes copied (including NUL) on success,
             * -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            uint64_t sz = a2;
            if (sz == 0) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, sz)) return (uint64_t)-EFAULT;

            uint32_t plen = 0;
            while (plen < FRY_PATH_MAX && shared->cwd[plen]) plen++;
            uint64_t copy = (plen + 1 < sz) ? plen + 1 : sz;
            uint64_t i;
            char *ubuf = (char *)(uintptr_t)a1;
            for (i = 0; i < copy; i++)
                ubuf[i] = (i < FRY_PATH_MAX) ? shared->cwd[i] : 0;
            ubuf[copy - 1] = 0;
            return (int64_t)plen;
        }

        case SYS_PIPE2: {
            /*
             * SYS_PIPE2(a1=user_int_array_ptr, a2=flags)
             * Creates a pipe with flags (O_NONBLOCK, O_CLOEXEC).
             * Writes [read_fd, write_fd] to user buffer.
             * Returns 0 on success, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            if (!user_buf_writable(cur, a1, 2 * sizeof(int32_t))) return (uint64_t)-EFAULT;
            uint32_t flags = (uint32_t)a2;

            /* Only O_NONBLOCK and O_CLOEXEC are valid */
            if (flags & ~(O_NONBLOCK | O_CLOEXEC)) return (uint64_t)-EINVAL;

            int pidx = pipe_alloc();
            if (pidx < 0) return (uint64_t)-ENFILE;
            int rfd = fd_alloc(cur);
            if (rfd < 0) { g_pipes[pidx].used = 0; return (uint64_t)-EMFILE; }
            fd_install(cur, rfd, &g_pipes[pidx], FD_PIPE_READ, flags);
            g_pipes[pidx].readers++;
            int wfd = fd_alloc(cur);
            if (wfd < 0) {
                g_pipes[pidx].readers--;
                fd_release(cur, rfd);
                g_pipes[pidx].used = 0;
                return (uint64_t)-EMFILE;
            }
            fd_install(cur, wfd, &g_pipes[pidx], FD_PIPE_WRITE, flags);
            g_pipes[pidx].writers++;
            int32_t *ufds = (int32_t *)(uintptr_t)a1;
            ufds[0] = rfd;
            ufds[1] = wfd;
            return 0;
        }

        case SYS_DUP3: {
            /*
             * SYS_DUP3(a1=oldfd, a2=newfd, a3=flags)
             * Like dup2 but accepts flags (only O_CLOEXEC).
             * If oldfd == newfd, returns newfd without closing.
             * Returns newfd on success, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int oldfd = (int)a1;
            int newfd = (int)a2;
            uint32_t flags = (uint32_t)a3;
            struct fry_process_shared *shared = proc_shared_state(cur);

            if (oldfd < 0 || oldfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (newfd < 3 || newfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (flags & ~(uint32_t)O_CLOEXEC) return (uint64_t)-EINVAL;

            uint8_t okind = shared->fd_kind[oldfd];
            void *optr = shared->fd_ptrs[oldfd];
            if (okind == FD_NONE || !optr) return (uint64_t)-EBADF;

            if (oldfd == newfd) {
                /* dup3 with oldfd==newfd does not close the fd */
                return (uint64_t)newfd;
            }

            /* Close newfd if open (same as dup2) */
            if (shared->fd_kind[newfd] != FD_NONE && shared->fd_ptrs[newfd]) {
                if (shared->fd_kind[newfd] == FD_FILE) {
                    vfs_close((struct vfs_file *)shared->fd_ptrs[newfd]);
                } else if (shared->fd_kind[newfd] == FD_PIPE_READ) {
                    struct fry_pipe *pp = (struct fry_pipe *)shared->fd_ptrs[newfd];
                    if (pp->readers > 0) pp->readers--;
                    if (pp->readers == 0 && pp->writers == 0) { pp->used = 0; pp->head = 0; pp->tail = 0; }
                } else if (shared->fd_kind[newfd] == FD_PIPE_WRITE) {
                    struct fry_pipe *pp = (struct fry_pipe *)shared->fd_ptrs[newfd];
                    if (pp->writers > 0) pp->writers--;
                    if (pp->readers == 0 && pp->writers == 0) { pp->used = 0; pp->head = 0; pp->tail = 0; }
                } else if (shared->fd_kind[newfd] == FD_SOCKET) {
                    struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[newfd];
                    if (sk && sk->used) {
                        if (sk->type == SOCK_STREAM) {
                            if (sk->domain == 1 && sk->tcp_handle >= 0 && sk->tcp_handle < FRY_PIPE_MAX) {
                                struct fry_pipe *pp = &g_pipes[sk->tcp_handle];
                                if (pp->writers > 0) pp->writers--;
                                if (pp->readers == 0 && pp->writers == 0) { pp->used = 0; pp->head = 0; pp->tail = 0; }
                                pp = &g_pipes[sk->listen_handle];
                                if (pp->readers > 0) pp->readers--;
                                if (pp->readers == 0 && pp->writers == 0) { pp->used = 0; pp->head = 0; pp->tail = 0; }
                            } else {
                                if (sk->tcp_handle >= 0) tcp_close(sk->tcp_handle);
                                if (sk->listen_handle >= 0) tcp_close(sk->listen_handle);
                            }
                        }
                        sk->used = 0;
                        sk->state = SOCK_ST_CLOSED;
                        sk->tcp_handle = -1;
                        sk->listen_handle = -1;
                    }
                } else if (shared->fd_kind[newfd] == FD_TIMERFD) {
                    struct timerfd_cb *tm = (struct timerfd_cb *)shared->fd_ptrs[newfd];
                    if (tm) { tm->used = 0; kfree(tm); }
                } else if (shared->fd_kind[newfd] == FD_SIGNALFD) {
                    struct signalfd_cb *sf = (struct signalfd_cb *)shared->fd_ptrs[newfd];
                    if (sf) { sf->used = 0; kfree(sf); }
                } else if (shared->fd_kind[newfd] == FD_INOTIFY) {
                    struct inotify_cb *in = (struct inotify_cb *)shared->fd_ptrs[newfd];
                    if (in) { in->used = 0; kfree(in); }
                } else if (shared->fd_kind[newfd] == FD_MEMFD) {
                    struct memfd_cb *mf = (struct memfd_cb *)shared->fd_ptrs[newfd];
                    if (mf) { mf->used = 0; memfd_free_pages(mf); kfree(mf); }
                }
                fd_release(cur, newfd);
            }

            fd_install(cur, newfd, optr, okind, flags);
            shared->fd_table[newfd] = shared->fd_table[oldfd];
            if (shared->fd_paths[oldfd][0]) {
                int prc = fd_path_set(shared, newfd, shared->fd_paths[oldfd]);
                if (prc < 0) {
                    fd_release(cur, newfd);
                    return (uint64_t)prc;
                }
                if (okind == FD_DIR) shared->fd_ptrs[newfd] = shared->fd_paths[newfd];
            }
            if (okind == FD_PIPE_READ) ((struct fry_pipe *)optr)->readers++;
            else if (okind == FD_PIPE_WRITE) ((struct fry_pipe *)optr)->writers++;
            return (uint64_t)newfd;
        }

        case SYS_SOCKETPAIR: {
            /*
             * SYS_SOCKETPAIR(a1=domain, a2=type, a3=protocol, a4=user_sv_array)
             * Creates a pair of connected sockets (AF_UNIX only, SOCK_STREAM or SOCK_DGRAM).
             * Writes [fd0, fd1] to user buffer.
             * Returns 0 on success, -errno on failure.
             *
             * Implementation: a bidirectional pipe-like connection over shared buffers.
             * Each end of the pair is a struct fry_socket that points to a shared
             * ring buffer pair. For SOCK_STREAM, data written to one end is readable
             * from the other. For SOCK_DGRAM, messages are preserved.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int sdomain = (int)a1;
            int stype = (int)a2;
            int sproto = (int)a3;
            (void)sproto;

            if (!user_buf_writable(cur, a4, sizeof(int32_t) * 2)) return (uint64_t)-EFAULT;

            /* Only AF_UNIX (AF_LOCAL = 1) is supported for socketpair */
            if (sdomain != 1) return (uint64_t)-EAFNOSUPPORT;
            /* Only SOCK_STREAM and SOCK_DGRAM */
            if (stype != SOCK_STREAM && stype != SOCK_DGRAM) return (uint64_t)-EINVAL;

            /* Allocate two pipe buffers (one for each direction) */
            int pidx_a = pipe_alloc();
            if (pidx_a < 0) return (uint64_t)-ENFILE;
            int pidx_b = pipe_alloc();
            if (pidx_b < 0) {
                g_pipes[pidx_a].used = 0;
                return (uint64_t)-ENFILE;
            }

            /* Set pipe reader/writer counts for socketpair:
             * Pipe A (pidx_a): socket 0 writes, socket 1 reads
             * Pipe B (pidx_b): socket 1 writes, socket 0 reads
             * Each pipe has 1 writer and 1 reader. */
            g_pipes[pidx_a].writers = 1;
            g_pipes[pidx_a].readers = 1;
            g_pipes[pidx_b].writers = 1;
            g_pipes[pidx_b].readers = 1;

            /* Allocate two socket objects */
            int sidx0 = -1, sidx1 = -1;
            for (int i = 0; i < FRY_SOCK_MAX; i++) {
                if (!g_sockets[i].used) { sidx0 = i; break; }
            }
            if (sidx0 < 0) {
                g_pipes[pidx_a].used = 0;
                g_pipes[pidx_b].used = 0;
                return (uint64_t)-ENFILE;
            }
            for (int i = sidx0 + 1; i < FRY_SOCK_MAX; i++) {
                if (!g_sockets[i].used) { sidx1 = i; break; }
            }
            if (sidx1 < 0) {
                g_pipes[pidx_a].used = 0;
                g_pipes[pidx_b].used = 0;
                g_sockets[sidx0].used = 0;
                return (uint64_t)-ENFILE;
            }

            /* Set up socket 0 */
            struct fry_socket *sk0 = &g_sockets[sidx0];
            sk0->used = 1;
            sk0->domain = (uint8_t)sdomain;
            sk0->type = (uint8_t)stype;
            sk0->state = SOCK_ST_CONNECTED;
            sk0->tcp_handle = pidx_a;  /* store pipe index A for outgoing writes */
            sk0->listen_handle = pidx_b; /* store pipe index B for incoming reads */
            sk0->local_port = 0;
            sk0->remote_port = 0;
            sk0->local_ip = 0;
            sk0->remote_ip = 0;
            sk0->so_rcvtimeo = 0;
            sk0->so_sndtimeo = 0;
            sk0->reuseaddr = 0;
            sk0->udp_rx_head = 0;
            sk0->udp_rx_tail = 0;

            /* Set up socket 1 (reversed pipes) */
            struct fry_socket *sk1 = &g_sockets[sidx1];
            sk1->used = 1;
            sk1->domain = (uint8_t)sdomain;
            sk1->type = (uint8_t)stype;
            sk1->state = SOCK_ST_CONNECTED;
            sk1->tcp_handle = pidx_b;  /* writes go to pipe B */
            sk1->listen_handle = pidx_a; /* reads come from pipe A */
            sk1->local_port = 0;
            sk1->remote_port = 0;
            sk1->local_ip = 0;
            sk1->remote_ip = 0;
            sk1->so_rcvtimeo = 0;
            sk1->so_sndtimeo = 0;
            sk1->reuseaddr = 0;
            sk1->udp_rx_head = 0;
            sk1->udp_rx_tail = 0;

            /*
             * Allocate two distinct FD slots before installing either socket.
             * Calling fd_alloc() twice here can return the same slot because
             * fd_alloc() only observes already-installed descriptors.
             */
            int fd0 = -1;
            int fd1 = -1;
            for (int fd = 3; fd < FRY_FD_MAX; fd++) {
                if (shared->fd_kind[fd] != FD_NONE || shared->fd_ptrs[fd]) continue;
                if (fd0 < 0) fd0 = fd;
                else { fd1 = fd; break; }
            }
            if (fd0 < 0 || fd1 < 0) {
                sk0->used = 0; sk1->used = 0;
                g_pipes[pidx_a].used = 0; g_pipes[pidx_b].used = 0;
                return (uint64_t)-EMFILE;
            }

            fd_install(cur, fd0, sk0, FD_SOCKET, 0);
            fd_install(cur, fd1, sk1, FD_SOCKET, 0);

            int32_t *usv = (int32_t *)(uintptr_t)a4;
            usv[0] = fd0;
            usv[1] = fd1;
            return 0;
        }

        /* ================================================================
         * Phase 9: Chrome port — accept4, timerfd, signalfd, inotify
         * ================================================================ */
        case SYS_ACCEPT4: {
            /*
             * SYS_ACCEPT4(a1=fd, a2=sockaddr_out_ptr, a3=addrlen_ptr, a4=flags)
             * Like accept() but applies SOCK_NONBLOCK and SOCK_CLOEXEC flags
             * atomically on the new fd.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int afd = (int)a1;
            uint32_t aflags = (uint32_t)a4;

            /* Validate flags: only SOCK_NONBLOCK and SOCK_CLOEXEC */
            if (aflags & ~(0x800u | 0x80000u)) return (uint64_t)-EINVAL;

            /* Re-dispatch to the accept handler internally.
             * We copy the argument layout and let accept do the work,
             * then apply flags to the returned fd. */
            /* Re-invoke the same logic as SYS_ACCEPT but we can't goto,
             * so we inline the same accept logic with flag application. */
            if (afd < 3 || afd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[afd] != FD_SOCKET) return (uint64_t)-ENOTSOCK;
            struct fry_socket *lsk = (struct fry_socket *)shared->fd_ptrs[afd];
            if (!lsk || !lsk->used || lsk->state != SOCK_ST_LISTENING)
                return (uint64_t)-EINVAL;

            net_poll();
            tcp_conn_t nc = tcp_accept(lsk->listen_handle);
            if (nc < 0) {
                uint32_t fflags = shared->fd_flags[afd];
                if (fflags & O_NONBLOCK) return (uint64_t)-EAGAIN;
                sched_block_poll(cur->pid, UINT64_MAX);
                sched_yield();
                cur = proc_current();
                if (!cur) return (uint64_t)-ESRCH;
                shared = proc_shared_state(cur);
                if (!shared || !shared->fd_ptrs[afd]) return (uint64_t)-EBADF;
                lsk = (struct fry_socket *)shared->fd_ptrs[afd];
                if (!lsk || !lsk->used) return (uint64_t)-EBADF;
                net_poll();
                nc = tcp_accept(lsk->listen_handle);
                if (nc < 0) return (uint64_t)-EAGAIN;
            }

            int nsi = sock_alloc();
            if (nsi < 0) { tcp_close(nc); return (uint64_t)-ENFILE; }

            int newfd = fd_alloc(cur);
            if (newfd < 0) {
                g_sockets[nsi].used = 0;
                tcp_close(nc);
                return (uint64_t)-EMFILE;
            }

            g_sockets[nsi].domain = AF_INET;
            g_sockets[nsi].type = SOCK_STREAM;
            g_sockets[nsi].state = SOCK_ST_CONNECTED;
            g_sockets[nsi].tcp_handle = nc;
            g_sockets[nsi].local_port = lsk->local_port;

            /* Apply flags atomically at fd creation */
            uint32_t fd_flags = 0;
            if (aflags & 0x800u)   fd_flags |= O_NONBLOCK;   /* SOCK_NONBLOCK */
            if (aflags & 0x80000u) fd_flags |= O_CLOEXEC;    /* SOCK_CLOEXEC */
            fd_install(cur, newfd, &g_sockets[nsi], FD_SOCKET, fd_flags);

            /* Fill in peer address if requested */
            if (a2 && a3) {
                struct fry_sockaddr_in peer;
                uint8_t *p = (uint8_t *)&peer;
                for (uint32_t j = 0; j < sizeof(peer); j++) p[j] = 0;
                peer.sin_family = AF_INET;
                if (user_buf_writable(cur, a2, sizeof(peer)))
                    copyout(cur, &peer, a2, sizeof(peer));
            }

            return (uint64_t)newfd;
        }

        case SYS_TIMERFD_CREATE: {
            /*
             * SYS_TIMERFD_CREATE(a1=clockid, a2=flags)
             * Creates a timerfd. clockid: FRY_CLOCK_MONOTONIC or FRY_CLOCK_REALTIME.
             * flags: TFD_NONBLOCK (0x800) or TFD_CLOEXEC (0x80000).
             * Returns fd on success, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int clockid = (int)a1;
            uint32_t flags = (uint32_t)a2;

            if (clockid != 0 && clockid != 1)
                return (uint64_t)-EINVAL;
            if (flags & ~(0x800u | 0x80000u)) return (uint64_t)-EINVAL;

            struct timerfd_cb *tm = (struct timerfd_cb *)kmalloc(sizeof(struct timerfd_cb));
            if (!tm) return (uint64_t)-ENOMEM;

            int fd = fd_alloc(cur);
            if (fd < 0) {
                kfree(tm);
                return (uint64_t)-EMFILE;
            }

            tm->used = 1;
            tm->clockid = clockid;
            tm->it_value_ms = 0;
            tm->it_interval_ms = 0;
            tm->deadline_ms = 0;
            tm->expirations = 0;
            tm->nonblock = (flags & 0x800u) ? 1 : 0;

            uint32_t fd_flags = 0;
            if (flags & 0x800u)   fd_flags |= O_NONBLOCK;
            if (flags & 0x80000u) fd_flags |= O_CLOEXEC;
            fd_install(cur, fd, tm, FD_TIMERFD, fd_flags);
            return (uint64_t)fd;
        }

        case SYS_TIMERFD_SETTIME: {
            /*
             * SYS_TIMERFD_SETTIME(a1=fd, a2=flags, a3=user_new_value, a4=user_old_value)
             * Arms or disarms the timer.
             * new_value: pointer to struct { uint64_t it_value_sec; uint64_t it_value_nsec;
             *                             uint64_t it_interval_sec; uint64_t it_interval_nsec; }
             * TFD_TIMER_ABSTIME in flags means it_value is absolute.
             * old_value: optional output (can be 0).
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int tfd = (int)a1;
            uint32_t tflags = (uint32_t)a2;
            struct fry_process_shared *shared = proc_shared_state(cur);

            if (tfd < 3 || tfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[tfd] != FD_TIMERFD) return (uint64_t)-EINVAL;
            struct timerfd_cb *tm = (struct timerfd_cb *)shared->fd_ptrs[tfd];
            if (!tm || !tm->used) return (uint64_t)-EBADF;

            if (a4 != 0) {
                /* Save old value first — use 64-bit inline struct layout */
                /* We'll just store zero-old for simplicity; Chromium rarely uses old_value */
                if (user_buf_writable(cur, a4, 32)) {
                    uint64_t zero[4] = {0, 0, 0, 0};
                    copyout(cur, zero, a4, 32);
                }
            }

            if (a3 == 0) {
                /* Disarm */
                tm->it_value_ms = 0;
                tm->deadline_ms = 0;
                tm->expirations = 0;
                return 0;
            }

            /* Read new timer spec from user space */
            uint64_t new_val[4]; /* it_value_sec, it_value_nsec, it_interval_sec, it_interval_nsec */
            if (!user_buf_mapped(cur, a3, 32)) return (uint64_t)-EFAULT;
            if (copyin(cur, a3, new_val, 32) != 0) return (uint64_t)-EFAULT;

            uint64_t it_value_sec = new_val[0];
            uint64_t it_value_nsec = new_val[1];
            uint64_t it_interval_sec = new_val[2];
            uint64_t it_interval_nsec = new_val[3];

            /* Convert to ms */
            uint64_t new_it_value_ms = it_value_sec * 1000ULL + it_value_nsec / 1000000ULL;
            uint64_t new_it_interval_ms = it_interval_sec * 1000ULL + it_interval_nsec / 1000000ULL;

            tm->it_interval_ms = new_it_interval_ms;
            tm->expirations = 0;

            if (new_it_value_ms == 0) {
                /* Disarm */
                tm->it_value_ms = 0;
                tm->deadline_ms = 0;
                return 0;
            }

            /* Get current time in ms */
            uint64_t freq = hpet_get_freq_hz();
            uint64_t now_ms = (hpet_read_counter() * 1000ULL) / freq;

            if (tflags & 0x1u) {
                /* TFD_TIMER_ABSTIME — value is absolute */
                tm->deadline_ms = new_it_value_ms;
                if (tm->deadline_ms <= now_ms) {
                    /* Already expired */
                    tm->expirations = 1;
                    if (tm->it_interval_ms > 0) {
                        /* Schedule next periodic */
                        tm->deadline_ms = now_ms + tm->it_interval_ms;
                    }
                }
                tm->it_value_ms = (tm->deadline_ms > now_ms) ? (tm->deadline_ms - now_ms) : 0;
            } else {
                /* Relative timeout */
                tm->it_value_ms = new_it_value_ms;
                tm->deadline_ms = now_ms + new_it_value_ms;
            }

            return 0;
        }

        case SYS_TIMERFD_GETTIME: {
            /*
             * SYS_TIMERFD_GETTIME(a1=fd, a2=user_curr_value)
             * Returns current timer settings (remaining time + interval).
             * Output: { it_value_sec, it_value_nsec, it_interval_sec, it_interval_nsec }
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            int tfd = (int)a1;
            struct fry_process_shared *shared = proc_shared_state(cur);

            if (tfd < 3 || tfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[tfd] != FD_TIMERFD) return (uint64_t)-EINVAL;
            struct timerfd_cb *tm = (struct timerfd_cb *)shared->fd_ptrs[tfd];
            if (!tm || !tm->used) return (uint64_t)-EBADF;
            if (a2 == 0) return (uint64_t)-EFAULT;
            if (!user_buf_writable(cur, a2, 32)) return (uint64_t)-EFAULT;

            uint64_t freq = hpet_get_freq_hz();
            uint64_t now_ms = (hpet_read_counter() * 1000ULL) / freq;
            uint64_t remaining_ms = 0;
            if (tm->it_value_ms > 0 && tm->deadline_ms > now_ms) {
                remaining_ms = tm->deadline_ms - now_ms;
            }

            uint64_t out[4];
            out[0] = remaining_ms / 1000ULL;          /* it_value_sec */
            out[1] = (remaining_ms % 1000ULL) * 1000000ULL; /* it_value_nsec */
            out[2] = tm->it_interval_ms / 1000ULL;    /* it_interval_sec */
            out[3] = (tm->it_interval_ms % 1000ULL) * 1000000ULL; /* it_interval_nsec */

            copyout(cur, out, a2, 32);
            return 0;
        }

        case SYS_SIGNALFD: {
            /*
             * SYS_SIGNALFD(a1=fd, a2=user_mask_ptr (sigset_t), a3=flags)
             * If fd == -1: create a new signalfd.
             * If fd >= 0: modify existing signalfd's mask.
             * flags: SFD_NONBLOCK (0x800) or SFD_CLOEXEC (0x80000).
             * Returns fd on success, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int sfd = (int)a1;
            uint32_t sflags = (uint32_t)a3;

            if (sflags & ~(0x800u | 0x80000u)) return (uint64_t)-EINVAL;

            uint64_t mask = 0;
            if (a2 != 0) {
                if (!user_buf_mapped(cur, a2, 8)) return (uint64_t)-EFAULT;
                if (copyin(cur, a2, &mask, 8) != 0) return (uint64_t)-EFAULT;
            }

            if (sfd == -1) {
                /* Create new signalfd */
                struct signalfd_cb *sf = (struct signalfd_cb *)kmalloc(sizeof(struct signalfd_cb));
                if (!sf) return (uint64_t)-ENOMEM;

                int nfd = fd_alloc(cur);
                if (nfd < 0) {
                    kfree(sf);
                    return (uint64_t)-EMFILE;
                }

                sf->used = 1;
                sf->mask = mask;
                sf->nonblock = (sflags & 0x800u) ? 1 : 0;

                uint32_t fd_flags = 0;
                if (sflags & 0x800u)   fd_flags |= O_NONBLOCK;
                if (sflags & 0x80000u) fd_flags |= O_CLOEXEC;
                fd_install(cur, nfd, sf, FD_SIGNALFD, fd_flags);
                return (uint64_t)nfd;
            } else {
                /* Modify existing signalfd mask */
                if (sfd < 3 || sfd >= FRY_FD_MAX) return (uint64_t)-EBADF;
                if (shared->fd_kind[sfd] != FD_SIGNALFD) return (uint64_t)-EINVAL;
                struct signalfd_cb *sf = (struct signalfd_cb *)shared->fd_ptrs[sfd];
                if (!sf || !sf->used) return (uint64_t)-EBADF;
                sf->mask = mask;
                return (uint64_t)sfd;
            }
        }

        case SYS_INOTIFY_INIT: {
            /*
             * SYS_INOTIFY_INIT(a1=flags)
             * Creates an inotify instance.
             * flags: IN_NONBLOCK (0x800) or IN_CLOEXEC (0x80000).
             * Returns fd on success, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            uint32_t iflags = (uint32_t)a1;

            if (iflags & ~(0x800u | 0x80000u)) return (uint64_t)-EINVAL;

            struct inotify_cb *in = (struct inotify_cb *)kmalloc(sizeof(struct inotify_cb));
            if (!in) return (uint64_t)-ENOMEM;

            int fd = fd_alloc(cur);
            if (fd < 0) {
                kfree(in);
                return (uint64_t)-EMFILE;
            }

            in->used = 1;
            in->watch_count = 0;
            in->next_wd = 1;
            in->ev_head = 0;
            in->ev_tail = 0;
            in->nonblock = (iflags & 0x800u) ? 1 : 0;
            for (int i = 0; i < INOTIFY_WATCH_MAX; i++) {
                in->watches[i].used = 0;
            }

            uint32_t fd_flags = 0;
            if (iflags & 0x800u)   fd_flags |= O_NONBLOCK;
            if (iflags & 0x80000u) fd_flags |= O_CLOEXEC;
            fd_install(cur, fd, in, FD_INOTIFY, fd_flags);
            return (uint64_t)fd;
        }

        case SYS_INOTIFY_ADD_WATCH: {
            /*
             * SYS_INOTIFY_ADD_WATCH(a1=fd, a2=path_ptr, a3=mask)
             * Adds a watch on a path.
             * Returns watch descriptor (wd) on success, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int ifd = (int)a1;
            uint32_t imask = (uint32_t)a3;

            if (ifd < 3 || ifd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[ifd] != FD_INOTIFY) return (uint64_t)-EINVAL;
            struct inotify_cb *in = (struct inotify_cb *)shared->fd_ptrs[ifd];
            if (!in || !in->used) return (uint64_t)-EBADF;

            char raw_path[FRY_PATH_MAX];
            if (copy_user_string(cur, a2, raw_path, sizeof(raw_path)) != 0)
                return (uint64_t)-EFAULT;

            char resolved[FRY_PATH_MAX];
            int rc = resolve_at_path(cur, FRY_AT_FDCWD, raw_path, resolved);
            if (rc < 0) return (uint64_t)rc;

            /* Check if path exists */
            struct vfs_stat st;
            if (vfs_stat(resolved, &st) != 0) return (uint64_t)-ENOENT;

            /* Find an empty watch slot or reuse existing watch on same path */
            int slot = -1;
            for (int i = 0; i < INOTIFY_WATCH_MAX; i++) {
                if (!in->watches[i].used) {
                    if (slot < 0) slot = i;
                } else {
                    /* Check if this path is already watched */
                    uint32_t j;
                    for (j = 0; resolved[j] && in->watches[i].path[j]; j++) {
                        if (resolved[j] != in->watches[i].path[j]) break;
                    }
                    if (resolved[j] == 0 && in->watches[i].path[j] == 0) {
                        /* Same path — return existing wd */
                        return (uint64_t)in->watches[i].wd;
                    }
                }
            }

            if (slot < 0) return (uint64_t)-ENOSPC;

            /* Copy resolved path */
            uint32_t pi;
            for (pi = 0; pi < FRY_PATH_MAX - 1 && resolved[pi]; pi++)
                in->watches[slot].path[pi] = resolved[pi];
            in->watches[slot].path[pi] = 0;

            in->watches[slot].mask = imask;
            in->watches[slot].used = 1;
            in->watches[slot].wd = in->next_wd++;
            in->watch_count++;
            return (uint64_t)in->watches[slot].wd;
        }

        case SYS_INOTIFY_RM_WATCH: {
            /*
             * SYS_INOTIFY_RM_WATCH(a1=fd, a2=wd)
             * Removes a watch by watch descriptor.
             * Returns 0 on success, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int ifd = (int)a1;
            int wd = (int)a2;

            if (ifd < 3 || ifd >= FRY_FD_MAX) return (uint64_t)-EBADF;
            if (shared->fd_kind[ifd] != FD_INOTIFY) return (uint64_t)-EINVAL;
            struct inotify_cb *in = (struct inotify_cb *)shared->fd_ptrs[ifd];
            if (!in || !in->used) return (uint64_t)-EBADF;

            for (int i = 0; i < INOTIFY_WATCH_MAX; i++) {
                if (in->watches[i].used && in->watches[i].wd == wd) {
                    in->watches[i].used = 0;
                    in->watch_count--;
                    return 0;
                }
            }
            return (uint64_t)-EINVAL;
        }

        case SYS_MEMFD_CREATE: {
            /*
             * SYS_MEMFD_CREATE(a1=name_ptr, a2=flags)
             * Creates an anonymous memory-backed file descriptor.
             * Returns fd on success, -errno on failure.
             * flags: MFD_CLOEXEC (0x80000).
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            uint32_t mflags = (uint32_t)a2;

            if (mflags & ~(0x80000u)) return (uint64_t)-EINVAL;

            char name[32];
            if (a1 != 0) {
                if (copy_user_string(cur, a1, name, sizeof(name)) != 0)
                    return (uint64_t)-EFAULT;
            } else {
                name[0] = 0;
            }

            struct memfd_cb *mf = (struct memfd_cb *)kmalloc(sizeof(struct memfd_cb));
            if (!mf) return (uint64_t)-ENOMEM;
            for (uint32_t z = 0; z < sizeof(struct memfd_cb); z++)
                ((uint8_t *)mf)[z] = 0;

            mf->used = 1;
            mf->size = 0;
            mf->capacity = 0;
            mf->pos = 0;
            mf->page_count = 0;
            mf->pages = 0;
            for (uint32_t i = 0; i < 32 && name[i]; i++)
                mf->name[i] = name[i];
            mf->name[31] = 0;

            int fd = fd_alloc(cur);
            if (fd < 0) {
                kfree(mf);
                return (uint64_t)-EMFILE;
            }

            uint32_t fd_flags = 0;
            if (mflags & 0x80000u) fd_flags |= O_CLOEXEC;
            fd_install(cur, fd, mf, FD_MEMFD, fd_flags);
            return (uint64_t)fd;
        }

        case SYS_SENDFILE: {
            /*
             * SYS_SENDFILE(a1=out_fd, a2=in_fd, a3=offset_ptr, a4=count)
             * Copies data from in_fd to out_fd without userspace buffer.
             * If offset_ptr != 0, uses the pointed-to value as the starting
             * offset in in_fd and updates it.
             * Returns number of bytes transferred, -errno on failure.
             */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int out_fd = (int)a1;
            int in_fd = (int)a2;
            uint64_t count = (uint64_t)a4;

            if (out_fd < 3 || out_fd >= FRY_FD_MAX || in_fd < 3 || in_fd >= FRY_FD_MAX)
                return (uint64_t)-EBADF;
            if (!shared->fd_ptrs[out_fd] || !shared->fd_ptrs[in_fd])
                return (uint64_t)-EBADF;

            uint8_t in_kind = shared->fd_kind[in_fd];
            uint8_t out_kind = shared->fd_kind[out_fd];

            /* Determine read offset */
            uint64_t in_offset = UINT64_MAX; /* UINT64_MAX = use current pos */
            if (a3 != 0) {
                if (!user_buf_mapped(cur, a3, 8)) return (uint64_t)-EFAULT;
                uint64_t off_val;
                if (copyin(cur, a3, &off_val, 8) != 0) return (uint64_t)-EFAULT;
                in_offset = off_val;
            }

            if (count == 0) return 0;
            if (count > 1024 * 1024) count = 1024 * 1024; /* cap at 1MB per call */

            /* Use a small kernel bounce buffer for the transfer */
            uint8_t bounce[4096];
            uint64_t total = 0;

            while (total < count) {
                uint64_t chunk = count - total;
                if (chunk > sizeof(bounce)) chunk = sizeof(bounce);

                /* Read from in_fd */
                int64_t nread = 0;
                if (in_kind == FD_MEMFD) {
                    struct memfd_cb *mf = (struct memfd_cb *)shared->fd_ptrs[in_fd];
                    if (in_offset != UINT64_MAX) {
                        memfd_lseek(mf, (int64_t)in_offset, FRY_SEEK_SET);
                    }
                    nread = memfd_read(mf, bounce, chunk);
                    if (in_offset != UINT64_MAX) {
                        in_offset = (uint64_t)memfd_lseek(mf, 0, FRY_SEEK_CUR);
                    }
                } else if (in_kind == FD_FILE) {
                    struct vfs_file *vf = (struct vfs_file *)shared->fd_ptrs[in_fd];
                    if (in_offset != UINT64_MAX) {
                        vfs_seek(vf, (int64_t)in_offset, FRY_SEEK_SET);
                    }
                    nread = vfs_read(vf, bounce, (uint32_t)chunk);
                    if (in_offset != UINT64_MAX) {
                        in_offset = (uint64_t)vfs_seek(vf, 0, FRY_SEEK_CUR);
                    }
                } else {
                    return (uint64_t)-EBADF;
                }

                if (nread <= 0) break;

                /* Write to out_fd */
                int64_t nwritten = 0;
                if (out_kind == FD_MEMFD) {
                    struct memfd_cb *mf = (struct memfd_cb *)shared->fd_ptrs[out_fd];
                    nwritten = memfd_write(mf, bounce, (uint64_t)nread);
                } else if (out_kind == FD_FILE) {
                    struct vfs_file *vf = (struct vfs_file *)shared->fd_ptrs[out_fd];
                    nwritten = vfs_write(vf, bounce, (uint32_t)nread);
                } else if (out_kind == FD_SOCKET) {
                    struct fry_socket *sk = (struct fry_socket *)shared->fd_ptrs[out_fd];
                    if (sk->type == SOCK_STREAM) {
                        nwritten = tcp_send(sk->tcp_handle, bounce, (uint16_t)nread);
                    } else {
                        return (uint64_t)-EBADF;
                    }
                } else if (out_kind == FD_PIPE_WRITE) {
                    struct fry_pipe *pp = (struct fry_pipe *)shared->fd_ptrs[out_fd];
                    nwritten = pipe_write(pp, (const char *)bounce, (uint64_t)nread, shared->fd_flags[out_fd]);
                } else {
                    return (uint64_t)-EBADF;
                }

                if (nwritten <= 0) break;
                total += (uint64_t)nwritten;

                /* Update user offset */
                if (a3 != 0 && in_offset != UINT64_MAX) {
                    if (user_buf_writable(cur, a3, 8))
                        copyout(cur, &in_offset, a3, 8);
                }

                if ((uint64_t)nwritten < (uint64_t)nread) break;
            }

            return (uint64_t)total;
        }

        /* --- Chrome/GN probe stubs (Phase 10) --- */
        case SYS_UNAME: {
            /* uname(buf) — fill struct utsname (5x65 byte fields) */
            if (!cur || !user_buf_writable(cur, a1, 65*5))
                return (uint64_t)-EFAULT;
            uint8_t buf[65*5];
            for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
            /* sysname at +0 */
            { const char *s = "TaterTOS"; uint32_t i = 0; while (s[i]) { buf[i] = (uint8_t)s[i]; i++; } }
            /* nodename at +65 */
            { const char *s = "taterbox"; uint32_t i = 0; while (s[i]) { buf[65+i] = (uint8_t)s[i]; i++; } }
            /* release at +130 */
            { const char *s = "1.0.0"; uint32_t i = 0; while (s[i]) { buf[130+i] = (uint8_t)s[i]; i++; } }
            /* version at +195 */
            { const char *s = "TaterTOS64v3 #1 SMP"; uint32_t i = 0; while (s[i]) { buf[195+i] = (uint8_t)s[i]; i++; } }
            /* machine at +260 */
            { const char *s = "x86_64"; uint32_t i = 0; while (s[i]) { buf[260+i] = (uint8_t)s[i]; i++; } }
            copyout(cur, buf, a1, sizeof(buf));
            return 0;
        }
        case SYS_SYSINFO: {
            /* sysinfo(info) — provide uptime + memory + process count */
            if (!cur || !user_buf_writable(cur, a1, 64))
                return (uint64_t)-EFAULT;
            uint64_t freq = hpet_get_freq_hz();
            uint64_t uptime_ms = (freq > 0) ? ((hpet_read_counter() * 1000ULL) / freq) : 0;
            uint8_t info[64];
            for (uint32_t i = 0; i < sizeof(info); i++) info[i] = 0;
            /* offset 0: int64_t uptime */
            uint64_t uptime_sec = uptime_ms / 1000ULL;
            info[0] = (uint8_t)(uptime_sec >> 0); info[1] = (uint8_t)(uptime_sec >> 8);
            info[2] = (uint8_t)(uptime_sec >> 16); info[3] = (uint8_t)(uptime_sec >> 24);
            info[4] = (uint8_t)(uptime_sec >> 32); info[5] = (uint8_t)(uptime_sec >> 40);
            info[6] = (uint8_t)(uptime_sec >> 48); info[7] = (uint8_t)(uptime_sec >> 56);
            /* offset 32: uint64_t totalram */
            { uint64_t v = pmm_get_total_pages() * 4096ULL;
              for (uint32_t i = 0; i < 8; i++) info[32+i] = (uint8_t)(v >> (i*8)); }
            /* offset 40: uint64_t freeram */
            { uint64_t v = (pmm_get_total_pages() - pmm_get_used_pages()) * 4096ULL;
              for (uint32_t i = 0; i < 8; i++) info[40+i] = (uint8_t)(v >> (i*8)); }
            /* offset 60: uint16_t procs */
            { uint32_t pc = 0;
              for (uint32_t i = 0; i < PROC_MAX; i++)
                  if (procs[i].state != PROC_UNUSED && procs[i].state != PROC_DEAD) pc++;
              uint16_t pv = (uint16_t)(pc > 0xFFFF ? 0xFFFF : pc);
              info[60] = (uint8_t)(pv >> 0); info[61] = (uint8_t)(pv >> 8); }
            copyout(cur, info, a1, sizeof(info));
            return 0;
        }
        case SYS_GETRUSAGE: {
            /* getrusage(who, usage) — return zeros */
            if (!cur) return (uint64_t)-ESRCH;
            int who = (int)a1;
            if (who != 0) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a2, 144))
                return (uint64_t)-EFAULT;
            uint8_t zeros[144];
            for (uint32_t i = 0; i < sizeof(zeros); i++) zeros[i] = 0;
            copyout(cur, zeros, a2, sizeof(zeros));
            return 0;
        }
        case SYS_GETPRIORITY: {
            int which = (int)a1;
            if (which != 0) return (uint64_t)-EINVAL;
            return 0;
        }
        case SYS_SETPRIORITY: {
            int which = (int)a1;
            if (which != 0) return (uint64_t)-EINVAL;
            return 0;
        }
        case SYS_FSYNC:
        case SYS_FDATASYNC: {
            /* ramdisk is always synced */
            if (!cur || !proc_shared_state(cur)) return (uint64_t)-ESRCH;
            struct fry_process_shared *shared = proc_shared_state(cur);
            int fd = (int)a1;
            if (fd < 3 || fd >= FRY_FD_MAX || !shared->fd_ptrs[fd])
                return (uint64_t)-EBADF;
            return 0;
        }
        case SYS_SCHED_GETAFFINITY: {
            /* sched_getaffinity(pid, cpusetsize, mask) */
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t cpusetsize = a2;
            uint32_t ncpu = smp_cpu_count();
            uint64_t mask_bytes = linux_affinity_mask_bytes(ncpu);
            if (cpusetsize < mask_bytes)
                return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a3, mask_bytes))
                return (uint64_t)-EFAULT;
            if (ncpu == 0) ncpu = 1;
            uint8_t mask_buf[128];
            for (uint32_t i = 0; i < sizeof(mask_buf); i++) mask_buf[i] = 0;
            for (uint32_t i = 0; i < ncpu && i < cpusetsize * 8 && i < sizeof(mask_buf)*8; i++)
                mask_buf[i / 8] |= (uint8_t)(1u << (i % 8));
            copyout(cur, mask_buf, a3, mask_bytes);
            return (int64_t)mask_bytes;
        }
        case SYS_SCHED_SETAFFINITY: {
            return 0;
        }
        case SYS_MLOCK:
        case SYS_MUNLOCK:
            /* TaterTOS does not swap — all memory is permanently resident.
             * These operations are no-ops that succeed immediately. */
            return 0;
        case SYS_SPLICE:
        case SYS_TEE:
            /* splice/tee require pipe buffer manipulation and a VFS
             * splice callback. Not implemented on TaterTOS. */
            return (uint64_t)-ENOSYS;

        default:
            return (uint64_t)-ENOSYS;
    }
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

uint32_t smp_bsp_index(void);

/*
 * Set SYSCALL MSRs + SWAPGS per-CPU data for one CPU.
 * Called once per CPU: syscall_init() for BSP, syscall_init_ap() for APs.
 */
static void syscall_setup_cpu(uint32_t cpu) {
    /* Enable SYSCALL/SYSRET: set EFER.SCE (bit 0) */
    uint32_t efer_lo, efer_hi;
    __asm__ volatile("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080u));
    wrmsr(0xC0000080, ((uint64_t)efer_hi << 32 | efer_lo) | 1ULL);

    /* IA32_STAR: SYSCALL -> CS=0x08 SS=0x10; SYSRETQ -> CS=0x2B SS=0x23 */
    uint64_t star = ((uint64_t)0x18 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(0xC0000081, star);
    wrmsr(0xC0000082, (uint64_t)(uintptr_t)&syscall_entry);
    /* IA32_FMASK: clear IF(9), TF(8), DF(10) on SYSCALL entry */
    wrmsr(0xC0000084, 0x700);

    /* Set up SWAPGS per-CPU state:
     * GS.BASE    = percpu pointer (kernel GS — active during kernel mode)
     * KERNEL_GS  = 0 (user GS — swapped in by SWAPGS on syscall/interrupt exit)
     */
    uint64_t pcpu = (uint64_t)(uintptr_t)sched_percpu_ptr(cpu);
    wrmsr(0xC0000101, pcpu);   /* MSR_GS_BASE */
    wrmsr(0xC0000102, 0);      /* MSR_KERNEL_GS_BASE (user GS = 0) */
}

void syscall_init(void) {
    uint32_t bsp = smp_bsp_index();
    syscall_setup_cpu(bsp);
}

void syscall_init_ap(uint32_t cpu) {
    syscall_setup_cpu(cpu);
}

// syscall entry — SMP-safe via SWAPGS + per-CPU data
//
// The SYSCALL instruction does NOT switch the stack pointer; RSP remains the
// user-space RSP.  We use SWAPGS to load the per-CPU percpu_data struct into
// GS.BASE, which gives us a per-CPU kernel stack pointer at %gs:0 and a
// per-CPU scratch slot at %gs:8 for the user RSP.
//
// GS invariant (maintained by syscall_entry/exit and common_isr):
//   Kernel mode: GS.BASE = percpu pointer, KERNEL_GS = user GS (0)
//   User mode:   GS.BASE = user GS (0),    KERNEL_GS = percpu pointer
// Every user↔kernel transition does SWAPGS to flip between these.
//
// SFMASK clears IF on SYSCALL entry; SYSRET restores user RFLAGS from R11.
__asm__(
    ".global syscall_entry\n"
    "syscall_entry:\n"
    // IF is already 0 (SFMASK clears it at SYSCALL instruction).
    // RSP is still the user RSP.  GS.BASE = user GS (0).

    // 1. SWAPGS: load per-CPU kernel data into GS.BASE.
    "    swapgs\n"

    // 2. Save user RSP to per-CPU scratch, switch to kernel stack.
    "    movq %rsp, %gs:8\n"          // percpu.user_rsp = user RSP
    "    movq %gs:0, %rsp\n"          // RSP = percpu.kstack_top

    // 2b. Snapshot the parent's callee-saved registers into per-CPU scratch
    //     (gs:24..56) BEFORE they can be touched. The frame below does not
    //     save rbx/r12-r15, but clone3 children must inherit them (glibc
    //     stashes the thread fn/arg there across the clone syscall).
    "    movq %rbx, %gs:24\n"
    "    movq %r12, %gs:32\n"
    "    movq %r13, %gs:40\n"
    "    movq %r14, %gs:48\n"
    "    movq %r15, %gs:56\n"

    // 3. Push user RSP from percpu scratch onto kernel stack.
    //    Uses RAX as temporary (syscall number), recovered afterward.
    "    pushq %rax\n"                 // save syscall number
    "    movq %gs:8, %rax\n"           // RAX = user RSP
    "    xchgq %rax, (%rsp)\n"         // stack = user RSP, RAX = syscall number

    // 4. Save sysret frame values.
    "    pushq %rcx\n"                 // user RIP (return address)
    "    pushq %r11\n"                 // user RFLAGS
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"

    // 5. Preserve user-visible syscall registers before shuffling into the
    //    C ABI. Linux userland expects syscall to return with all GPRs except
    //    RAX/RCX/R11 intact; ld-linux relies on RDX surviving set_tid_address.
    "    pushq %rdi\n"
    "    pushq %rsi\n"
    "    pushq %rdx\n"
    "    pushq %r10\n"
    "    pushq %r8\n"
    "    pushq %r9\n"

    // 6. Shuffle registers into syscall_dispatch ABI (num,a1,a2,a3,a4,a5,a6).
    //    Linux x86_64 passes arg6 in user R9. Put it in the first stack
    //    argument slot before reusing R9 for the C ABI's sixth register arg.
    "    subq $16, %rsp\n"
    "    movq %r9, (%rsp)\n"            // a6 stack arg, 8-byte padding above it
    "    movq %r8,  %r9\n"            // a5
    "    movq %r10, %r8\n"            // a4
    "    movq %rdx, %rcx\n"           // a3
    "    movq %rsi, %rdx\n"           // a2
    "    movq %rdi, %rsi\n"           // a1
    "    movq %rax, %rdi\n"           // num

    // Allow IRQ-driven timers/scheduler while syscall body runs.
    "    sti\n"
    "    movq %rsp, %gs:16\n"          // percpu.linux_frame_rsp for clone3
    "    call syscall_dispatch\n"
    "    cli\n"

    // 7. Restore the saved user-visible registers. Leave RAX as the syscall
    //    return value from syscall_dispatch.
    "    addq $16, %rsp\n"
    "    popq %r9\n"
    "    popq %r8\n"
    "    popq %r10\n"
    "    popq %rdx\n"
    "    popq %rsi\n"
    "    popq %rdi\n"

    // 8. Restore frame and user state.
    "    movq %rbp, %rsp\n"
    "    popq %rbp\n"
    "    popq %r11\n"                  // user RFLAGS
    "    popq %rcx\n"                  // user RIP
    "    popq %rsp\n"                  // user RSP  (back to user stack)

    // 9. SWAPGS: restore user GS before returning to user mode.
    "    swapgs\n"
    "    sysretq\n"
);
