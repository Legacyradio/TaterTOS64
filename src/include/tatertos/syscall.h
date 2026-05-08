/*
 * TaterTOS syscall numbers — SINGLE SOURCE OF TRUTH
 *
 * Both the kernel dispatcher and the userspace libc include this file.
 * Every syscall gets a numeric ABI binding here.
 *
 * Numbering conventions:
 *   0-31     Core POSIX-like (STABLE)
 *   32-51    FS diagnostics / driver-specific
 *   52-54    VM syscalls (STABLE)
 *   55-63    IPC / descriptor model (STABLE)
 *   64-71    User threads / TLS / futex (STABLE)
 *   72-84    Socket ABI (STABLE)
 *   85-87    Randomness / Time / Runtime (STABLE)
 *   88-91    Filesystem expansion (STABLE)
 *   92-100   GUI / Audio / Chdir
 *   101-131  Chrome port POSIX expansion (Phase 1/2/9)
 *   132-144  Chrome/GN probe stubs (Phase 10)
 *
 * Adding a new syscall: edit this file, then add the dispatch case in
 * syscall.c and the wrapper in libc.c.  The number is THE ABI — do not
 * reuse or renumber existing entries.
 */

#ifndef TATERTOS_SYSCALL_H
#define TATERTOS_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* --- Core POSIX-like (0-31, STABLE) --- */
    SYS_WRITE = 0,
    SYS_READ = 1,
    SYS_EXIT = 2,
    SYS_SPAWN = 3,
    SYS_SLEEP = 4,
    SYS_OPEN = 5,
    SYS_CLOSE = 6,
    SYS_GETPID = 7,
    SYS_STAT = 8,
    SYS_READDIR = 9,
    SYS_GETTIME = 10,
    SYS_REBOOT = 11,
    SYS_SHUTDOWN = 12,
    SYS_WAIT = 13,
    SYS_PROCCOUNT = 14,
    SYS_SETBRIGHT = 15,
    SYS_GETBRIGHT = 16,
    SYS_GETBATTERY = 17,
    SYS_FB_INFO = 18,
    SYS_FB_MAP = 19,
    SYS_MOUSE_GET = 20,
    SYS_PROC_OUTPUT = 21,
    SYS_SBRK = 22,
    SYS_SHM_ALLOC = 23,
    SYS_SHM_MAP = 24,
    SYS_PROC_INPUT = 25,
    SYS_KILL = 26,
    SYS_SHM_FREE = 27,
    SYS_ACPI_DIAG = 28,
    SYS_CREATE = 29,
    SYS_MKDIR = 30,
    SYS_UNLINK = 31,

    /* --- FS diagnostics / extended (32-36) --- */
    SYS_STORAGE_INFO = 32,
    SYS_PATH_FS_INFO = 33,
    SYS_MOUNTS_INFO = 34,
    SYS_READDIR_EX = 35,
    SYS_MOUNTS_DEBUG = 36,

    /* --- Driver-specific (37-51) --- */
    SYS_WIFI_STATUS = 37,
    SYS_WIFI_SCAN = 38,
    SYS_WIFI_CONNECT = 39,
    SYS_WIFI_DEBUG = 40,
    SYS_WIFI_CPU_STATUS = 41,
    SYS_WIFI_INIT_LOG = 42,
    SYS_WIFI_DEBUG2 = 43,
    SYS_WIFI_HANDOFF = 44,
    SYS_WIFI_DEBUG3 = 45,
    SYS_WIFI_REINIT = 46,
    SYS_WIFI_CMD_TRACE = 47,
    SYS_WIFI_SRAM = 48,
    SYS_WIFI_DEEP_DIAG = 49,
    SYS_WIFI_VERIFY = 50,
    SYS_ETH_DIAG = 51,

    /* --- VM syscalls (STABLE) --- */
    SYS_MMAP = 52,
    SYS_MUNMAP = 53,
    SYS_MPROTECT = 54,

    /* --- IPC / descriptor model (Phase 3, STABLE) --- */
    SYS_PIPE = 55,
    SYS_DUP = 56,
    SYS_DUP2 = 57,
    SYS_POLL = 58,
    SYS_FCNTL = 59,
    SYS_SPAWN_ARGS = 60,
    SYS_GET_ARGC = 61,
    SYS_GET_ARGV = 62,
    SYS_GETENV = 63,
    SYS_GETENV_SYS = 63,  /* alias used by libc getenv wrapper */

    /* --- User threads (STABLE) --- */
    SYS_THREAD_CREATE = 64,
    SYS_THREAD_EXIT = 65,
    SYS_THREAD_JOIN = 66,
    SYS_GETTID = 67,

    /* --- Synchronization / TLS (STABLE) --- */
    SYS_FUTEX_WAIT = 68,
    SYS_FUTEX_WAKE = 69,
    SYS_SET_TLS_BASE = 70,
    SYS_GET_TLS_BASE = 71,

    /* --- Socket ABI (Phase 4, STABLE) --- */
    SYS_SOCKET = 72,
    SYS_CONNECT = 73,
    SYS_BIND = 74,
    SYS_LISTEN = 75,
    SYS_ACCEPT = 76,
    SYS_SEND = 77,
    SYS_RECV = 78,
    SYS_SHUTDOWN_SOCK = 79,
    SYS_GETSOCKOPT = 80,
    SYS_SETSOCKOPT = 81,
    SYS_SENDTO = 82,
    SYS_RECVFROM = 83,
    SYS_DNS_RESOLVE = 84,

    /* --- Randomness/Time/Runtime (Phase 5, STABLE) --- */
    SYS_GETRANDOM = 85,
    SYS_CLOCK_GETTIME = 86,
    SYS_NANOSLEEP = 87,

    /* --- Filesystem expansion (Phase 6, STABLE) --- */
    SYS_LSEEK = 88,
    SYS_FTRUNCATE = 89,
    SYS_RENAME = 90,
    SYS_FSTAT = 91,

    /* --- GUI/Input expansion (Phase 7) --- */
    SYS_KBD_EVENT = 92,
    SYS_MOUSE_GET_EXT = 93,
    SYS_CLIPBOARD_GET = 94,
    SYS_CLIPBOARD_SET = 95,

    /* Audio syscalls (TaterSurf Phase D) */
    SYS_AUDIO_OPEN  = 96,
    SYS_AUDIO_WRITE = 97,
    SYS_AUDIO_CLOSE = 98,
    SYS_AUDIO_INFO  = 99,
    SYS_CHDIR = 100,

    /* Chrome port — POSIX expansion (Phase 1/2) */
    SYS_EPOLL_CREATE = 101,
    SYS_EPOLL_CTL    = 102,
    SYS_EPOLL_WAIT   = 103,
    SYS_EVENTFD      = 104,
    SYS_OPENAT       = 105,
    SYS_READV        = 106,
    SYS_WRITEV       = 107,
    SYS_SENDMSG      = 108,
    SYS_RECVMSG      = 109,
    SYS_PRCTL        = 110,
    SYS_MADVISE      = 111,
    SYS_FACCESSAT    = 112,
    SYS_READLINKAT   = 113,
    SYS_FSTATAT      = 114,
    SYS_MKDIRAT      = 115,
    SYS_UNLINKAT     = 116,
    SYS_RENAMEAT     = 117,
    SYS_PIPE2        = 118,
    SYS_DUP3         = 119,
    SYS_SOCKETPAIR   = 120,
    SYS_GETCWD       = 121,

    /* --- Phase 9: Chrome port — timerfd/signalfd/inotify/accept4 --- */
    SYS_ACCEPT4      = 122,
    SYS_TIMERFD_CREATE  = 123,
    SYS_TIMERFD_SETTIME = 124,
    SYS_TIMERFD_GETTIME = 125,
    SYS_SIGNALFD        = 126,
    SYS_INOTIFY_INIT    = 127,
    SYS_INOTIFY_ADD_WATCH = 128,
    SYS_INOTIFY_RM_WATCH  = 129,
    SYS_MEMFD_CREATE      = 130,
    SYS_SENDFILE          = 131,

    /* --- Chrome/GN probe syscalls (Phase 10, STUBS) --- */
    SYS_UNAME             = 132,
    SYS_SYSINFO           = 133,
    SYS_GETRUSAGE         = 134,
    SYS_GETPRIORITY       = 135,
    SYS_SETPRIORITY       = 136,
    SYS_FSYNC             = 137,
    SYS_FDATASYNC         = 138,
    SYS_SCHED_GETAFFINITY = 139,
    SYS_SCHED_SETAFFINITY = 140,
    SYS_MLOCK             = 141,
    SYS_MUNLOCK           = 142,
    SYS_SPLICE            = 143,
    SYS_TEE               = 144,
};

#ifdef __cplusplus
}
#endif

#endif /* TATERTOS_SYSCALL_H */
