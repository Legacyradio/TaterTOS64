#ifndef TATER_LINUX_COMPAT_H
#define TATER_LINUX_COMPAT_H

/*
 * TaterTOS64v3 — Tater Bridge Linux-compatible ABI layer.
 *
 * Original TaterTOS code that lets the kernel host UNMODIFIED Linux
 * x86_64 ELF binaries by (a) loading a bare Linux ELF and building a
 * SysV/Linux initial process stack, and (b) translating Linux syscalls
 * into TaterTOS kernel operations. No Linux source lives in the kernel;
 * the Linux binaries themselves live in the filesystem as data. Same
 * posture as a userspace ABI personality, not a
 * conversion of TaterTOS into Linux.
 */

#include <stdint.h>

struct fry_process;

/* ---- Linux x86_64 syscall numbers (curated subset we translate) ---- */
#define LNX_read              0
#define LNX_write             1
#define LNX_open              2
#define LNX_close             3
#define LNX_stat              4
#define LNX_fstat             5
#define LNX_lstat             6
#define LNX_lseek             8
#define LNX_mmap              9
#define LNX_mprotect          10
#define LNX_munmap            11
#define LNX_brk               12
#define LNX_rt_sigaction      13
#define LNX_rt_sigprocmask    14
#define LNX_rt_sigreturn      15
#define LNX_ioctl             16
#define LNX_pread64           17
#define LNX_readv             19
#define LNX_writev            20
#define LNX_access            21
#define LNX_sched_yield       24
#define LNX_madvise           28
#define LNX_dup               32
#define LNX_dup2              33
#define LNX_nanosleep         35
#define LNX_epoll_create      213
#define LNX_getdents64        217
#define LNX_getpid            39
#define LNX_socket            41
#define LNX_clone             56
#define LNX_fork              57
#define LNX_exit              60
#define LNX_wait4             61
#define LNX_kill              62
#define LNX_uname             63
#define LNX_fcntl             72
#define LNX_getcwd            79
#define LNX_readlink          89
#define LNX_gettimeofday      96
#define LNX_getrusage         98
#define LNX_sysinfo           99
#define LNX_getuid            102
#define LNX_getgid            104
#define LNX_geteuid           107
#define LNX_getegid           108
#define LNX_getppid           110
#define LNX_rt_sigsuspend     130
#define LNX_sigaltstack       131
#define LNX_sched_setscheduler 144
#define LNX_prctl             157
#define LNX_arch_prctl        158
#define LNX_gettid            186
#define LNX_time              201
#define LNX_tkill             200
#define LNX_futex             202
#define LNX_sched_getaffinity 204
#define LNX_set_tid_address   218
#define LNX_clock_gettime     228
#define LNX_clock_getres      229
#define LNX_clock_nanosleep   230
#define LNX_epoll_wait        232
#define LNX_epoll_ctl         233
#define LNX_exit_group        231
#define LNX_tgkill            234
#define LNX_inotify_init      253
#define LNX_inotify_add_watch 254
#define LNX_inotify_rm_watch  255
#define LNX_openat            257
#define LNX_newfstatat        262
#define LNX_set_robust_list   273
#define LNX_get_robust_list   274
#define LNX_epoll_pwait       281
#define LNX_signalfd          282
#define LNX_timerfd_create    283
#define LNX_eventfd           284
#define LNX_timerfd_settime   286
#define LNX_timerfd_gettime   287
#define LNX_signalfd4         289
#define LNX_eventfd2          290
#define LNX_epoll_create1     291
#define LNX_inotify_init1     294
#define LNX_prlimit64         302
#define LNX_getrandom         318
#define LNX_rseq              334
#define LNX_clone3            435
#define LNX_close_range       436

/* ---- arch_prctl subfunctions ---- */
#define LNX_ARCH_SET_GS  0x1001
#define LNX_ARCH_SET_FS  0x1002
#define LNX_ARCH_GET_FS  0x1003
#define LNX_ARCH_GET_GS  0x1004

/* ---- Linux mmap prot/flags ---- */
#define LNX_PROT_READ   0x1
#define LNX_PROT_WRITE  0x2
#define LNX_PROT_EXEC   0x4
#define LNX_MAP_SHARED      0x01
#define LNX_MAP_PRIVATE     0x02
#define LNX_MAP_FIXED       0x10
#define LNX_MAP_ANONYMOUS   0x20
#define LNX_MAP_NORESERVE   0x4000

/* ---- Linux clone/clone3 flags used by Tater Bridge ---- */
#define LNX_CLONE_VM              0x00000100ULL
#define LNX_CLONE_FS              0x00000200ULL
#define LNX_CLONE_FILES           0x00000400ULL
#define LNX_CLONE_SIGHAND         0x00000800ULL
#define LNX_CLONE_PIDFD           0x00001000ULL
#define LNX_CLONE_THREAD          0x00010000ULL
#define LNX_CLONE_SYSVSEM         0x00040000ULL
#define LNX_CLONE_SETTLS          0x00080000ULL
#define LNX_CLONE_PARENT_SETTID   0x00100000ULL
#define LNX_CLONE_CHILD_CLEARTID  0x00200000ULL
#define LNX_CLONE_CHILD_SETTID    0x01000000ULL

/* ---- Linux open/stat/at constants used by the translator ---- */
#define LNX_AT_FDCWD       (-100)
#define LNX_AT_EMPTY_PATH  0x1000u

#define LNX_O_ACCMODE    0x3u
#define LNX_O_CREAT      0x40u
#define LNX_O_EXCL       0x80u
#define LNX_O_TRUNC      0x200u
#define LNX_O_APPEND     0x400u
#define LNX_O_NONBLOCK   0x800u
#define LNX_O_DIRECTORY  0x10000u
#define LNX_O_CLOEXEC    0x80000u

#define LNX_S_IFMT   00170000u
#define LNX_S_IFDIR  0040000u
#define LNX_S_IFCHR  0020000u
#define LNX_S_IFREG  0100000u

/* ---- Linux clock ids ---- */
#define LNX_CLOCK_REALTIME   0
#define LNX_CLOCK_MONOTONIC  1

/* Anonymous mmap() arena for Linux processes. Grows DOWNWARD from here,
 * sitting well below the 8 MiB initial stack (top of user VA) and above
 * the brk heap, so the three never collide. */
#define LX_MMAP_BASE  0x0000700000000000ULL

/*
 * Linux ELF loader. Loads a bare (non-FRY) static ET_EXEC x86_64 ELF,
 * maps its PT_LOAD segments into a fresh address space, and builds a
 * conforming SysV/Linux initial stack (argc/argv/envp/auxv + AT_RANDOM).
 *
 * Returns 0 on success; negative errno on failure. brk_out receives the
 * initial program break (page-aligned end of the highest PT_LOAD).
 */
int elf_load_linux(const char *path,
                   const char *const *argv, int argc,
                   const char *const *envp, int envc,
                   uint64_t *cr3_out, uint64_t *entry_out,
                   uint64_t *rsp_out, uint64_t *brk_out);

/* Create + enqueue a Linux process. Returns pid (>0) or negative errno. */
int process_launch_linux(const char *path,
                         const char *const *argv, int argc,
                         const char *const *envp, int envc);

/* Linux syscall translation. Defined in syscall.c so it can reuse the
 * validated static helpers there. Routed to from syscall_dispatch when
 * the current process has is_linux set. */
uint64_t linux_syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5,
                                uint64_t a6, struct fry_process *cur);

#endif /* TATER_LINUX_COMPAT_H */
