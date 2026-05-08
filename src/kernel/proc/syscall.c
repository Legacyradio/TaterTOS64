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
#include <sys/prctl.h>
#include <fry_random.h>
#include <fry_time.h>
#include "../entropy/entropy.h"
#include "../../drivers/timer/rtc.h"
#include "../../drivers/smp/smp.h"
#include <fry_seek.h>
#include <fry_input.h>

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

/* Phase 7: kernel clipboard buffer */
static char g_clipboard_buf[FRY_CLIPBOARD_MAX];
static uint32_t g_clipboard_len = 0;
/*
 * SYS_EXIT must not free the currently active process CR3/stack while still
 * executing on them.  Use a dedicated kernel-owned stack for exit teardown.
 */
static uint8_t g_sys_exit_stack[16384] __attribute__((aligned(16)));

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

    if (kind == FD_FILE) {
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
        if (tm->expirations > 0) {
            if (events & FRY_POLLIN) revents |= FRY_POLLIN;
        }
        if (events & FRY_POLLOUT) revents |= FRY_POLLOUT;
    } else if (kind == FD_SIGNALFD) {
        struct signalfd_cb *sf = (struct signalfd_cb *)shared->fd_ptrs[fd];
        if (!sf || !sf->used) return FRY_POLLNVAL;
        /* Check if any signals in the mask are pending for this process */
        struct fry_process *cp = proc_current();
        if (cp && cp->shared) {
            /* TaterTOS doesn't track async pending signals.  For now,
             * signalfd is always readable (signal-zero semantics) so that
             * poll()-based event loops calling signalfd don't hang. */
            if (events & FRY_POLLIN) revents |= FRY_POLLIN;
        }
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
                                       void (*finish)(uint32_t, uint32_t)) {
    uint64_t kcr3 = vmm_get_kernel_pml4_phys();
    uint64_t exit_sp = ((uint64_t)(uintptr_t)&g_sys_exit_stack[sizeof(g_sys_exit_stack)]) & ~0xFULL;

    __asm__ volatile(
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
    syscall_exit_on_safe_stack(process_group_id(cur), code, syscall_exit_group_finish);
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
    syscall_exit_on_safe_stack(cur->pid, code, syscall_thread_exit_finish);
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
        if (!PROC_VMREGS(p)[i].used) return i;
    }
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

static void vm_unmap_pages_only(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p) return;
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        if (!vmm_virt_to_phys_user(p->cr3, va)) continue;
        vmm_unmap_user(p->cr3, va);
    }
}

static void vm_release_private_pages(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p) return;
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
        if (prot == 0) return 0;
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

static int futex_key_for_user_word(struct fry_process *p, uint64_t uaddr, uint64_t *key_out) {
    uint64_t key;
    if (!p || !key_out) return -EINVAL;
    if ((uaddr & 3ULL) != 0) return -EINVAL;
    if (!user_buf_mapped(p, uaddr, sizeof(uint32_t))) return -EFAULT;
    key = vmm_virt_to_phys_user(p->cr3, uaddr);
    if (!key) return -EFAULT;
    *key_out = key;
    return 0;
}

static int futex_wait_begin(struct fry_process *cur, uint64_t uaddr,
                            uint32_t expected, uint64_t timeout_ms) {
    uint64_t key;
    volatile const uint32_t *word;
    uint64_t wake_time_ms;
    int rc;
    if (!cur) return -ESRCH;
    rc = futex_key_for_user_word(cur, uaddr, &key);
    if (rc < 0) return rc;

    word = (volatile const uint32_t *)(uintptr_t)uaddr;
    if (timeout_ms == 0) {
        wake_time_ms = UINT64_MAX;
    } else {
        wake_time_ms = sys_now_ms();
        if (wake_time_ms > UINT64_MAX - timeout_ms) {
            wake_time_ms = UINT64_MAX - 1ULL;
        } else {
            wake_time_ms += timeout_ms;
        }
    }

    rc = sched_block_futex(cur->pid, word, expected, key, wake_time_ms);
    if (rc < 0) return rc;
    sched_yield();

    cur = proc_current();
    if (!cur) return -ESRCH;
    rc = cur->wait_result;
    cur->wait_result = 0;
    cur->wait_futex_key = 0;
    cur->wake_time_ms = 0;
    return rc;
}

static int futex_wake_waiters(struct fry_process *cur, uint64_t uaddr, uint32_t max_wake) {
    uint64_t key;
    int rc;
    if (!cur || max_wake == 0) return 0;
    rc = futex_key_for_user_word(cur, uaddr, &key);
    if (rc < 0) return rc;
    return (int)sched_wake_futex(key, max_wake, 0);
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
    struct fry_process *cur = proc_current();
    note_user_boot_progress(cur);
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
                    uint64_t freq = hpet_get_freq_hz();
                    uint64_t now_ms = (hpet_read_counter() * 1000ULL) / freq;
                    while (tm->it_value_ms > 0 && tm->deadline_ms > 0 && now_ms >= tm->deadline_ms) {
                        tm->expirations++;
                        if (tm->it_interval_ms > 0) {
                            tm->deadline_ms += tm->it_interval_ms;
                        } else {
                            tm->it_value_ms = 0;
                            tm->deadline_ms = 0;
                        }
                    }
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
            uint64_t freq = hpet_get_freq_hz();
            if (freq == 0) return 0;
            uint64_t ms = (hpet_read_counter() * 1000ULL) / freq;
            return ms;
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
                rc = syscall_dispatch(SYS_READ, (uint64_t)fd, iov.iov_base, iov.iov_len, 0, 0);
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
                rc = syscall_dispatch(SYS_WRITE, (uint64_t)fd, iov.iov_base, iov.iov_len, 0, 0);
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
            if (!user_buf_writable(cur, a3, cpusetsize))
                return (uint64_t)-EFAULT;
            uint32_t ncpu = smp_cpu_count();
            if (ncpu == 0) ncpu = 1;
            uint8_t mask_buf[128];
            for (uint32_t i = 0; i < sizeof(mask_buf); i++) mask_buf[i] = 0;
            for (uint32_t i = 0; i < ncpu && i < cpusetsize * 8 && i < sizeof(mask_buf)*8; i++)
                mask_buf[i / 8] |= (uint8_t)(1u << (i % 8));
            uint64_t copy_sz = cpusetsize < sizeof(mask_buf) ? cpusetsize : (uint64_t)sizeof(mask_buf);
            copyout(cur, mask_buf, a3, copy_sz);
            return (int64_t)copy_sz;
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

    // 5. Shuffle registers into syscall_dispatch ABI (num,a1,a2,a3,a4,a5).
    "    movq %r8,  %r9\n"            // a5
    "    movq %r10, %r8\n"            // a4
    "    movq %rdx, %rcx\n"           // a3
    "    movq %rsi, %rdx\n"           // a2
    "    movq %rdi, %rsi\n"           // a1
    "    movq %rax, %rdi\n"           // num

    // Allow IRQ-driven timers/scheduler while syscall body runs.
    "    sti\n"
    "    call syscall_dispatch\n"
    "    cli\n"

    // 6. Restore frame and user state.
    "    movq %rbp, %rsp\n"
    "    popq %rbp\n"
    "    popq %r11\n"                  // user RFLAGS
    "    popq %rcx\n"                  // user RIP
    "    popq %rsp\n"                  // user RSP  (back to user stack)

    // 7. SWAPGS: restore user GS before returning to user mode.
    "    swapgs\n"
    "    sysretq\n"
);
