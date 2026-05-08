#ifndef TATER_FRY_H
#define TATER_FRY_H

#include <stddef.h>
#include <stdint.h>
#include <fry_input.h>

/*
 * fry.h — Private TaterTOS Syscall ABI
 *
 * This header contains the underlying fry_ prefixed syscall wrappers
 * and ABI structs used by the C library. It should not be included by
 * standard POSIX applications.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * TaterTOS ABI Structs
 * ----------------------------------------------------------------------- */

struct fry_stat {
    uint64_t size;
    uint32_t attr;
};

struct fry_pollfd {
    int32_t  fd;
    uint16_t events;
    uint16_t revents;
};

struct fry_iovec {
    void *iov_base;
    size_t iov_len;
};

struct fry_msghdr {
    void *msg_name;
    uint32_t msg_namelen;
    struct fry_iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct epoll_event;

struct fry_sigaction {
    void (*sa_handler)(int);
    uint32_t sa_flags;
    uint32_t sa_mask;
};

struct fry_mouse_state {
    int32_t x; int32_t y; int32_t dx; int32_t dy;
    uint8_t btns; uint8_t _pad[3]; int32_t wheel;
};

struct fry_fb_info {
    uint64_t phys;
    uint64_t size;
    uint64_t user_base;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};

struct fry_battery_status {
    uint32_t state;
    uint32_t present_rate;
    uint32_t remaining_capacity;
    uint32_t present_voltage;
};

struct fry_acpi_diag {
    uint32_t ns_nodes;
    uint8_t  ec_ok;
    uint8_t  ec_node_found;
    uint8_t  ec_ports_source;
    uint8_t  ec_probe_step;
    uint8_t  ec_ibf_seen;
    uint8_t  ec_probe_status;
    uint8_t  ec_probe_attempts;
    uint8_t  ec_recovery_method;
    uint8_t  espi_probed;
    uint8_t  espi_en;
    uint8_t  espi_clear_run;
    uint8_t  espi_clear_found;
    uint8_t  espi_clear_ok;
    uint8_t  espi_slave_read_ok;
    uint8_t  espi_slave_pc_en;
    uint8_t  espi_slave_vw_en;
    uint8_t  pcr_mirror_done;
    uint8_t  ec_events_frozen;
    uint8_t  batt_count;
    uint8_t  bl_found;
    uint32_t lpc_ioe_before;
    uint32_t lpc_ioe_after;
    uint32_t espi_pid;
    uint32_t espi_raw[8];
    uint32_t espi_pre_clear[4];
    uint32_t espi_post_clear[4];
    uint32_t espi_gen_chan_sup;
    uint32_t espi_slave_pc_cap;
    uint32_t espi_slave_vw_cap;
    uint32_t pcr_pid;
    uint32_t pcr_ioe_before;
    uint32_t pcr_ioe_after;
    uint32_t ec_queries_dispatched;
    uint32_t ec_queries_dropped;
    uint32_t ec_storm_count;
    uint32_t ec_cand_count;
    uint32_t ec_policy_timeout;
    uint32_t ec_policy_retries;
    uint32_t ec_policy_max_fail;
    uint32_t ec_policy_flags;
    uint32_t batt_sta;
    uint32_t bl_sta;
};

struct fry_storage_info {
    uint8_t  nvme_detected;
    uint8_t  root_fs_type;
    uint8_t  secondary_fs_type;
    uint8_t  flags;
    uint32_t sector_size;
    uint64_t total_sectors;
    char     root_mount[16];
    char     secondary_mount[16];
};

#define FRY_STORAGE_FLAG_ROOT_RAMDISK_SOURCE 0x01u
#define FRY_STORAGE_FLAG_USB_BOOT            0x02u

struct fry_path_fs_info {
    uint8_t  fs_type;
    uint8_t  pad[3];
    char     mount[16];
};

#define FRY_MAX_MOUNT_INFO 16u
struct fry_mount_info {
    uint8_t  fs_type;
    uint8_t  pad[3];
    char     mount[64];
};
struct fry_mounts_info {
    uint32_t count;
    struct fry_mount_info entries[FRY_MAX_MOUNT_INFO];
};

struct fry_mount_dbg {
    char     mount[64];
    uint8_t  fs_type;
    uint64_t part_lba;
    uint32_t sector_size;
    uint32_t block_size;
};
struct fry_mounts_dbg {
    uint32_t count;
    struct fry_mount_dbg entries[FRY_MAX_MOUNT_INFO];
};

struct fry_dirent {
    uint32_t rec_len;
    uint32_t name_len;
    uint64_t size;
    uint32_t attr;
    char     name[0];
};

struct fry_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
};

struct fry_wifi_status {
    uint8_t  connected;
    uint8_t  ready;
    uint8_t  configured;
    uint8_t  secure;
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    char     ssid[33];
    uint8_t  pad[3];
};

#define FRY_WIFI_MAX_SCAN 32u
struct fry_wifi_scan_entry {
    char     ssid[33];
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  secure;
    uint8_t  connected;
    uint8_t  pad[1];
};

struct fry_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

#define FRY_CLOCK_REALTIME  0
#define FRY_CLOCK_MONOTONIC 1

/* -----------------------------------------------------------------------
 * Syscall Wrappers
 * ----------------------------------------------------------------------- */

long fry_write(int fd, const void *buf, size_t len);
long fry_read(int fd, void *buf, size_t len);
long fry_readv(int fd, const struct fry_iovec *iov, int iovcnt);
long fry_writev(int fd, const struct fry_iovec *iov, int iovcnt);
long fry_spawn(const char *path);
long fry_spawn_args(const char *path, const char **argv, uint32_t argc, const char **envp, uint32_t envc);
long fry_exit(int code);
__attribute__((noreturn)) void fry_thread_exit(int code);
long fry_gettid(void);
long fry_getpid(void);
long fry_gettime(void);
long fry_sleep(uint64_t ms);
long fry_open(const char *path, int flags);
long fry_openat(int dirfd, const char *path, int flags);
long fry_faccessat(int dirfd, const char *path, int mode, int flags);
long fry_readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz);
long fry_fstatat(int dirfd, const char *path, struct fry_stat *st, int flags);
long fry_mkdirat(int dirfd, const char *path, uint32_t mode);
long fry_unlinkat(int dirfd, const char *path, int flags);
long fry_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
long fry_create(const char *path, uint16_t type);
long fry_close(int fd);
long fry_wait(uint32_t pid);
long fry_kill(int pid, int sig);
long fry_mkdir(const char *path);
long fry_unlink(const char *path);
long fry_rename(const char *old, const char *newp);
long fry_chdir(const char *path);
long fry_lseek(int fd, int64_t offset, int whence);
long fry_ftruncate(int fd, uint64_t length);
long fry_mprotect(void *addr, size_t len, uint32_t prot);
long fry_madvise(void *addr, size_t len, int advice);
void *fry_mmap(void *addr, size_t len, uint32_t prot, uint32_t flags, int fd, int64_t offset);
void *fry_mmap_fd(void *addr, size_t len, uint32_t prot, uint32_t flags, int fd);
long fry_munmap(void *addr, size_t len);
void *fry_mreserve(void *addr, size_t len, uint32_t flags);
long fry_mcommit(void *addr, size_t len, uint32_t prot);
void *fry_mguard(void *addr, size_t len);
long fry_shm_alloc(size_t size);
long fry_shm_free(int shm_id);
long fry_shm_map(int shm_id);

long fry_pipe(int fds[2]);
long fry_pipe2(int fds[2], int flags);
long fry_dup(int oldfd);
long fry_dup2(int oldfd, int newfd);
long fry_dup3(int oldfd, int newfd, int flags);
long fry_socketpair(int domain, int type, int protocol, int sv[2]);
long fry_getcwd(char *buf, size_t size);
long fry_accept4(int fd, void *addr, uint32_t *addrlen, int flags);
long fry_timerfd_create(int clockid, int flags);
long fry_timerfd_settime(int fd, int flags, const void *new_value, void *old_value);
long fry_timerfd_gettime(int fd, void *curr_value);
long fry_signalfd(int fd, const uint64_t *mask, int flags);
long fry_inotify_init(int flags);
long fry_inotify_add_watch(int fd, const char *path, uint32_t mask);
long fry_inotify_rm_watch(int fd, int wd);
long fry_memfd_create(const char *name, unsigned int flags);
long fry_sendfile(int out_fd, int in_fd, int64_t *offset, size_t count);
/* --- Chrome/GN probe wrappers (Phase 10) --- */
long fry_uname(void *buf);
long fry_sysinfo(void *info);
long fry_getrusage(int who, void *usage);
long fry_getpriority(int which, int who);
long fry_setpriority(int which, int who, int prio);
long fry_fsync(int fd);
long fry_fdatasync(int fd);
long fry_sched_getaffinity(int pid, size_t cpusetsize, void *mask);
long fry_sched_setaffinity(int pid, size_t cpusetsize, const void *mask);
long fry_mlock(const void *addr, size_t len);
long fry_munlock(const void *addr, size_t len);
long fry_splice(int fd_in, int64_t *off_in, int fd_out, int64_t *off_out, size_t len, unsigned int flags);
long fry_tee(int fd_in, int fd_out, size_t len, unsigned int flags);
long fry_poll(struct fry_pollfd *fds, uint32_t nfds, uint64_t timeout_ms);
long fry_epoll_create(int size);
long fry_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
long fry_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout_ms);
long fry_eventfd(uint32_t initval, int flags);
long fry_fcntl(int fd, int cmd, long arg);
long fry_ioctl(int fd, uint32_t request, long arg);

struct fry_sockaddr_in;
long fry_socket(int domain, int type, int protocol);
long fry_connect(int fd, const struct fry_sockaddr_in *addr, uint32_t addrlen);
long fry_bind(int fd, const struct fry_sockaddr_in *addr, uint32_t addrlen);
long fry_listen(int fd, int backlog);
long fry_accept(int fd, struct fry_sockaddr_in *addr, uint32_t *addrlen);
long fry_send(int fd, const void *buf, size_t len, int flags);
long fry_recv(int fd, void *buf, size_t len, int flags);
long fry_sendto(int fd, const void *buf, size_t len, int flags, const struct fry_sockaddr_in *dest, uint32_t addrlen);
long fry_recvfrom(int fd, void *buf, size_t len, int flags, struct fry_sockaddr_in *src, uint32_t *addrlen);
long fry_sendmsg(int fd, const struct fry_msghdr *msg, int flags);
long fry_recvmsg(int fd, struct fry_msghdr *msg, int flags);
long fry_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen);
long fry_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen);
long fry_getsockname(int fd, struct fry_sockaddr_in *addr, uint32_t *addrlen);
long fry_getpeername(int fd, struct fry_sockaddr_in *addr, uint32_t *addrlen);
long fry_prctl(int option, unsigned long arg2, unsigned long arg3,
               unsigned long arg4, unsigned long arg5);

long fry_sigaction(int sig, const struct fry_sigaction *act, struct fry_sigaction *oldact);
int fry_sigemptyset(uint32_t *set);
int fry_sigfillset(uint32_t *set);
int fry_sigaddset(uint32_t *set, int signum);
int fry_sigdelset(uint32_t *set, int signum);
int fry_sigismember(const uint32_t *set, int signum);

struct fry_timespec;
long fry_clock_gettime(int clock_id, struct fry_timespec *ts);
long fry_nanosleep(const struct fry_timespec *req, struct fry_timespec *rem);
long fry_getrandom(void *buf, unsigned long len, unsigned int flags);
long fry_sbrk(intptr_t increment);
long fry_syscall_raw(long num, long arg0);

long fry_proc_output(uint32_t pid, void *buf, size_t len);
long fry_proc_input(uint32_t pid, const void *buf, size_t len);
long fry_proc_count(void);
long fry_getbattery(struct fry_battery_status *out);
long fry_setbrightness(uint32_t percent);
long fry_getbrightness(void);
long fry_fb_info(struct fry_fb_info *info);
long fry_fb_map(void);
long fry_mouse_get(struct fry_mouse_state *ms);
long fry_mouse_get_ext(struct fry_mouse_state *ms);
struct fry_key_event;
long fry_kbd_event(struct fry_key_event *out);
long fry_clipboard_get(char *buf, size_t maxlen);
long fry_clipboard_set(const char *buf, size_t len);
long fry_acpi_diag(struct fry_acpi_diag *out);
long fry_storage_info(struct fry_storage_info *out);
long fry_path_fs_info(const char *path, struct fry_path_fs_info *out);
long fry_mounts_info(struct fry_mounts_info *out);
long fry_mounts_dbg(struct fry_mounts_dbg *out);

long fry_stat(const char *path, struct fry_stat *st);
long fry_fstat(int fd, struct fry_stat *st);
long fry_readdir(const char *path, void *buf, size_t len);
long fry_readdir_ex(const char *path, void *buf, size_t len);
long fry_reboot(void);
long fry_shutdown(void);
long fry_dns_resolve(const char *node, uint32_t *ip_net);
long fry_shutdown_sock(int fd, int how);

struct fry_wifi_status;
struct fry_wifi_scan_entry;
long fry_wifi_status(struct fry_wifi_status *out);
long fry_wifi_scan(struct fry_wifi_scan_entry *out, uint32_t max, uint32_t *count);
long fry_wifi_connect(const char *ssid, const char *pass);
long fry_wifi_init_log(char *buf, uint32_t bufsz);
long fry_wifi_handoff(char *buf, uint32_t bufsz);
long fry_wifi_debug(char *buf, uint32_t bufsz);
long fry_wifi_debug2(char *buf, uint32_t bufsz);
long fry_wifi_debug3(char *buf, uint32_t bufsz);
long fry_wifi_cpu_status(char *buf, uint32_t bufsz);
long fry_wifi_cmd_trace(char *buf, uint32_t bufsz);
long fry_wifi_sram(char *buf, uint32_t bufsz);
long fry_wifi_deep_diag(char *buf, uint32_t bufsz);
long fry_wifi_verify(char *buf, uint32_t bufsz);
long fry_eth_diag(char *buf, uint32_t bufsz);
long fry_wifi_reinit(void);

/* -----------------------------------------------------------------------
 * Threading Primitives
 * ----------------------------------------------------------------------- */

typedef void (*fry_thread_start_t)(void *arg);
struct fry_thread {
    uint32_t tid;
    void *stack_base;
    size_t stack_len;
    void *tls_base;
};

long fry_thread_create(struct fry_thread *thr, fry_thread_start_t start, void *arg);
long fry_thread_join(struct fry_thread *thr, int *exit_code);
int fry_thread_current(struct fry_thread *thr);

typedef struct { volatile uint32_t state; } fry_mutex_t;
typedef struct { volatile uint32_t seq; } fry_cond_t;
typedef struct { volatile uint32_t count; } fry_sem_t;
typedef struct { volatile uint32_t state; } fry_once_t;

#define FRY_MUTEX_INIT {0u}
#define FRY_COND_INIT  {0u}
#define FRY_ONCE_INIT  {0u}

long fry_futex_wait(volatile uint32_t *addr, uint32_t expected, uint64_t timeout_ms);
long fry_futex_wake(volatile uint32_t *addr, uint32_t count);
long fry_tls_set_base(void *base);
void *fry_tls_get_base(void);
typedef uint32_t fry_tls_key_t;
int fry_tls_key_create(fry_tls_key_t *out_key);
void *fry_tls_get(fry_tls_key_t key);
int fry_tls_set(fry_tls_key_t key, void *value);

int fry_mutex_lock(fry_mutex_t *mutex);
int fry_mutex_trylock(fry_mutex_t *mutex);
int fry_mutex_unlock(fry_mutex_t *mutex);
int fry_cond_wait(fry_cond_t *cond, fry_mutex_t *mutex);
int fry_cond_signal(fry_cond_t *cond);
int fry_cond_broadcast(fry_cond_t *cond);
int fry_sem_init(fry_sem_t *sem, uint32_t value);
int fry_sem_wait(fry_sem_t *sem);
int fry_sem_post(fry_sem_t *sem);
int fry_once(fry_once_t *once, void (*init_fn)(void));

#define FRY_POLLIN   0x0001u
#define FRY_POLLPRI  0x0002u
#define FRY_POLLOUT  0x0004u
#define FRY_POLLERR  0x0008u
#define FRY_POLLHUP  0x0010u
#define FRY_POLLNVAL 0x0020u

uint16_t fry_htons(uint16_t h);
uint16_t fry_ntohs(uint16_t n);
uint32_t fry_htonl(uint32_t h);
uint32_t fry_ntohl(uint32_t n);

/* -----------------------------------------------------------------------
 * Memory/Mapping Constants
 * ----------------------------------------------------------------------- */

#define FRY_PROT_READ   0x01u
#define FRY_PROT_WRITE  0x02u
#define FRY_PROT_EXEC   0x04u

#define FRY_MAP_SHARED  0x01u
#define FRY_MAP_PRIVATE 0x02u
#define FRY_MAP_FIXED   0x10u
#define FRY_MAP_ANON    0x20u
#define FRY_MAP_FILE    0x40u
#define FRY_MAP_RESERVE 0x80u
#define FRY_MAP_GUARD   0x100u

#define FRY_MAP_FAILED ((void *)(intptr_t)-1)
#define FRY_IS_ERR(p) ((intptr_t)(p) < 0)
#define FRY_PTR_ERR(p) (-(int)(intptr_t)(p))

#ifdef __cplusplus
}
#endif

#endif
