//! TaterTOS native ABI bindings for the `libc` crate.
//!
//! TaterTOS is an independent x86-64 UEFI-native OS with its OWN kernel, ABI,
//! filesystem and syscall numbers. This module is NOT derived from Linux — it is
//! sourced directly from TaterTOS's own headers:
//!   - src/include/tatertos/syscall.h  (the SYS_* ABI, single source of truth)
//!   - src/include/sys/types.h, fry_types.h  (type widths)
//!   - src/include/sys/stat.h  (struct stat layout — TaterTOS's own, no glibc pad)
//!   - src/include/fry_fcntl.h, sys/mman.h, errno.h  (flags / errno)
//!
//! The extern "C" declarations resolve at link time to TaterTOS's own libc.a
//! (src/user/libc), which issues the native SYS_* syscalls. The Linux binary
//! personality (subsystem) is NOT involved.
//!
//! Type widths intentionally match common POSIX/Linux conventions because
//! TaterTOS's C headers deliberately chose those widths for POSIX source
//! compatibility — but the syscall numbers and runtime are 100% TaterTOS.

use crate::prelude::*;
// These are defined in the common unix layer (crate::unix); bring them into this
// module's scope (they are not in the prelude).
use crate::{
    gid_t, in6_addr, in_addr_t, in_port_t, iovec, pid_t, sigval, timespec, timeval, uid_t,
    DIR,
};

// c_char / wchar_t come from the crate prelude (arch-derived) — do NOT redefine
// here or they become ambiguous glob re-exports.

// ── POSIX scalar typedefs (src/include/fry_types.h, sys/types.h) ─────────
pub type off_t = i64;
pub type mode_t = u32;
pub type dev_t = u64;
pub type ino_t = u64;
pub type ino64_t = u64;
pub type nlink_t = u32;
pub type blkcnt_t = i64;
pub type blksize_t = i64;
pub type time_t = i64;
pub type clock_t = i64;
pub type suseconds_t = i64;
pub type useconds_t = u32;
pub type socklen_t = u32;
pub type sa_family_t = u16;
// in_port_t / in_addr_t are provided by the common unix layer (src/unix/mod.rs).
pub type id_t = i32;
pub type key_t = i32;
pub type clockid_t = i32;
pub type pthread_t = c_ulong;
pub type pthread_key_t = c_uint;
pub type nfds_t = c_ulong;
pub type wchar_t = i32;
pub type sigset_t = u64; // src/include/signal.h
pub type rlim_t = c_ulong; // src/include/sys/resource.h
pub type speed_t = c_uint; // src/include/termios.h
pub type tcflag_t = c_uint;
pub type cc_t = u8;
pub type fsblkcnt_t = c_ulong; // src/include/sys/statvfs.h
pub type fsfilcnt_t = c_ulong;
pub type sa_sigaction_t = size_t; // union of fn pointers (pointer-sized)

pub const NCCS: usize = 32; // src/include/termios.h
pub const FD_SETSIZE: usize = 1024; // src/include/sys/select.h

// ── struct timespec / timeval (src/include/time.h, sys/time.h) ───────────
// timespec / timeval are provided by the common unix layer (src/unix/mod.rs);
// their generic POSIX layout {time_t, c_long}/{time_t, suseconds_t} matches
// TaterTOS's time.h, so we reuse them rather than redefine (avoids type collision).
s! {
    // struct stat — EXACT TaterTOS layout (src/include/sys/stat.h).
    // No glibc __pad/reserved fields; field order is TaterTOS's own.
    pub struct stat {
        pub st_dev: dev_t,
        pub st_ino: ino_t,
        pub st_mode: mode_t,
        pub st_nlink: nlink_t,
        pub st_uid: uid_t,
        pub st_gid: gid_t,
        pub st_rdev: dev_t,
        pub st_size: off_t,
        pub st_blksize: blksize_t,
        pub st_blocks: blkcnt_t,
        pub st_atim: timespec,
        pub st_mtim: timespec,
        pub st_ctim: timespec,
    }
}

// ── pthread types — EXACT layouts (src/include/pthread.h, sched.h) ────────
// Byte-for-byte match to TaterTOS pthread.h or the runtime corrupts locks.
s! {
    pub struct sched_param {
        pub sched_priority: c_int,
    }

    pub struct pthread_attr_t {
        __stacksize: size_t,
        __stackaddr: *mut c_void,
        __guardsize: size_t,
        __detachstate: c_int,
        __inheritsched: c_int,
        __schedpolicy: c_int,
        __schedparam: sched_param,
        __scope: c_int,
    }

    pub struct pthread_mutex_t {
        __state: u32,
    }
    pub struct pthread_mutexattr_t {
        __type: c_int,
    }

    pub struct pthread_cond_t {
        __seq: u32,
    }
    pub struct pthread_condattr_t {
        __clock: c_int,
    }

    pub struct pthread_rwlock_t {
        __mutex: pthread_mutex_t,
        __cond: pthread_cond_t,
        __readers: u32,
        __writer: u32,
    }
    pub struct pthread_rwlockattr_t {
        __unused: c_int,
    }

    pub struct pthread_once_t {
        __state: u32,
    }

    pub struct pthread_barrier_t {
        __mutex: pthread_mutex_t,
        __cond: pthread_cond_t,
        __count: c_uint,
        __total: c_uint,
    }
    pub struct pthread_barrierattr_t {
        __unused: c_int,
    }
}

pub type pthread_spinlock_t = c_int;

// Static initializers (src/include/pthread.h: all-zero).
pub const PTHREAD_MUTEX_INITIALIZER: pthread_mutex_t = pthread_mutex_t { __state: 0 };
pub const PTHREAD_COND_INITIALIZER: pthread_cond_t = pthread_cond_t { __seq: 0 };
pub const PTHREAD_ONCE_INIT: pthread_once_t = pthread_once_t { __state: 0 };

pub const PTHREAD_CREATE_JOINABLE: c_int = 0;
pub const PTHREAD_CREATE_DETACHED: c_int = 1;
pub const PTHREAD_MUTEX_NORMAL: c_int = 0;
pub const PTHREAD_MUTEX_RECURSIVE: c_int = 1;
pub const PTHREAD_MUTEX_ERRORCHECK: c_int = 2;
pub const PTHREAD_MUTEX_DEFAULT: c_int = 0;
pub const PTHREAD_PROCESS_PRIVATE: c_int = 0;
pub const PTHREAD_PROCESS_SHARED: c_int = 1;

// ── core POSIX structs (sourced from src/include headers) ────────────────
s! {
    // src/include/sys/socket.h
    pub struct sockaddr {
        pub sa_family: sa_family_t,
        pub sa_data: [u8; 14],
    }

    // src/include/netdb.h
    pub struct addrinfo {
        pub ai_flags: c_int,
        pub ai_family: c_int,
        pub ai_socktype: c_int,
        pub ai_protocol: c_int,
        pub ai_addrlen: socklen_t,
        pub ai_addr: *mut sockaddr,
        pub ai_canonname: *mut c_char,
        pub ai_next: *mut addrinfo,
    }

    // src/include/dirent.h
    pub struct dirent {
        pub d_ino: ino_t,
        pub d_off: off_t,
        pub d_reclen: c_ushort,
        pub d_type: c_uchar,
        pub d_name: [c_char; 256],
    }

    // src/include/time.h
    pub struct tm {
        pub tm_sec: c_int,
        pub tm_min: c_int,
        pub tm_hour: c_int,
        pub tm_mday: c_int,
        pub tm_mon: c_int,
        pub tm_year: c_int,
        pub tm_wday: c_int,
        pub tm_yday: c_int,
        pub tm_isdst: c_int,
        pub tm_gmtoff: c_long,
        pub tm_zone: *const c_char,
    }

    // src/include/pwd.h
    pub struct passwd {
        pub pw_name: *mut c_char,
        pub pw_passwd: *mut c_char,
        pub pw_uid: uid_t,
        pub pw_gid: gid_t,
        pub pw_gecos: *mut c_char,
        pub pw_dir: *mut c_char,
        pub pw_shell: *mut c_char,
    }

    // (struct group is provided by the common unix layer — reuse it.)

    // src/include/termios.h
    pub struct termios {
        pub c_iflag: tcflag_t,
        pub c_oflag: tcflag_t,
        pub c_cflag: tcflag_t,
        pub c_lflag: tcflag_t,
        pub c_line: cc_t,
        pub c_cc: [cc_t; NCCS],
        pub c_ispeed: speed_t,
        pub c_ospeed: speed_t,
    }

    // src/include/locale.h
    pub struct lconv {
        pub decimal_point: *mut c_char,
        pub thousands_sep: *mut c_char,
        pub grouping: *mut c_char,
        pub mon_decimal_point: *mut c_char,
        pub mon_thousands_sep: *mut c_char,
        pub mon_grouping: *mut c_char,
        pub positive_sign: *mut c_char,
        pub negative_sign: *mut c_char,
        pub currency_symbol: *mut c_char,
        pub int_curr_symbol: *mut c_char,
        pub frac_digits: c_char,
        pub p_cs_precedes: c_char,
        pub n_cs_precedes: c_char,
        pub p_sep_by_space: c_char,
        pub n_sep_by_space: c_char,
        pub p_sign_posn: c_char,
        pub n_sign_posn: c_char,
        pub int_frac_digits: c_char,
        pub int_p_cs_precedes: c_char,
        pub int_n_cs_precedes: c_char,
        pub int_p_sep_by_space: c_char,
        pub int_n_sep_by_space: c_char,
        pub int_p_sign_posn: c_char,
        pub int_n_sign_posn: c_char,
    }

    // src/include/dlfcn.h (GNU extension)
    pub struct Dl_info {
        pub dli_fname: *const c_char,
        pub dli_fbase: *mut c_void,
        pub dli_sname: *const c_char,
        pub dli_saddr: *mut c_void,
    }

    // src/include/semaphore.h
    pub struct sem_t {
        pub mutex: pthread_mutex_t,
        pub cond: pthread_cond_t,
        pub value: c_uint,
    }

    // (struct rlimit is provided by the common unix layer — reuse it.)

    // src/include/sys/select.h  (1024 / 64 = 16 longs)
    pub struct fd_set {
        pub fds_bits: [c_ulong; 16],
    }

    // src/include/sys/statvfs.h
    pub struct statvfs {
        pub f_bsize: c_ulong,
        pub f_frsize: c_ulong,
        pub f_blocks: fsblkcnt_t,
        pub f_bfree: fsblkcnt_t,
        pub f_bavail: fsblkcnt_t,
        pub f_files: fsfilcnt_t,
        pub f_ffree: fsfilcnt_t,
        pub f_favail: fsfilcnt_t,
        pub f_fsid: c_ulong,
        pub f_flag: c_ulong,
        pub f_namemax: c_ulong,
    }

    // (union sigval is provided by the common unix layer — reuse it.)
    // src/include/signal.h
    pub struct siginfo_t {
        pub si_signo: c_int,
        pub si_errno: c_int,
        pub si_code: c_int,
        pub si_pid: pid_t,
        pub si_uid: uid_t,
        pub si_addr: *mut c_void,
        pub si_status: c_int,
        pub si_band: c_long,
        pub si_value: sigval,
    }

    pub struct sigaction {
        pub sa_sigaction: sa_sigaction_t,
        pub sa_mask: sigset_t,
        pub sa_flags: c_int,
        pub sa_restorer: Option<extern "C" fn()>,
    }
}

// ── errno (src/include/errno.h — full set) ───────────────────────────────
pub const EPERM: c_int = 1;
pub const ENOENT: c_int = 2;
pub const ESRCH: c_int = 3;
pub const EINTR: c_int = 4;
pub const EIO: c_int = 5;
pub const ENXIO: c_int = 6;
pub const E2BIG: c_int = 7;
pub const ENOEXEC: c_int = 8;
pub const EBADF: c_int = 9;
pub const ECHILD: c_int = 10;
pub const EAGAIN: c_int = 11;
pub const ENOMEM: c_int = 12;
pub const EACCES: c_int = 13;
pub const EFAULT: c_int = 14;
pub const EBUSY: c_int = 16;
pub const EEXIST: c_int = 17;
pub const EXDEV: c_int = 18;
pub const ENODEV: c_int = 19;
pub const ENOTDIR: c_int = 20;
pub const EISDIR: c_int = 21;
pub const EINVAL: c_int = 22;
pub const ENFILE: c_int = 23;
pub const EMFILE: c_int = 24;
pub const ENOTTY: c_int = 25;
pub const ETXTBSY: c_int = 26;
pub const EFBIG: c_int = 27;
pub const ENOSPC: c_int = 28;
pub const ESPIPE: c_int = 29;
pub const EROFS: c_int = 30;
pub const EMLINK: c_int = 31;
pub const EPIPE: c_int = 32;
pub const EDOM: c_int = 33;
pub const ERANGE: c_int = 34;
pub const EDEADLK: c_int = 35;
pub const ENAMETOOLONG: c_int = 36;
pub const ENOLCK: c_int = 37;
pub const ENOSYS: c_int = 38;
pub const ENOTEMPTY: c_int = 39;
pub const ELOOP: c_int = 40;
pub const ENOMSG: c_int = 42;
pub const EIDRM: c_int = 43;
pub const ENODATA: c_int = 61;
pub const ENOLINK: c_int = 67;
pub const EPROTO: c_int = 71;
pub const EBADMSG: c_int = 74;
pub const EOVERFLOW: c_int = 75;
pub const EILSEQ: c_int = 84;
pub const EUSERS: c_int = 87;
pub const ENOTSOCK: c_int = 88;
pub const EDESTADDRREQ: c_int = 89;
pub const EMSGSIZE: c_int = 90;
pub const EPROTOTYPE: c_int = 91;
pub const ENOPROTOOPT: c_int = 92;
pub const EPROTONOSUPPORT: c_int = 93;
pub const ESOCKTNOSUPPORT: c_int = 94;
pub const ENOTSUP: c_int = 95;
pub const EOPNOTSUPP: c_int = 95; // alias of ENOTSUP
pub const EAFNOSUPPORT: c_int = 97;
pub const EADDRINUSE: c_int = 98;
pub const EADDRNOTAVAIL: c_int = 99;
pub const ENETDOWN: c_int = 100;
pub const ENETUNREACH: c_int = 101;
pub const ENETRESET: c_int = 102;
pub const ECONNABORTED: c_int = 103;
pub const ECONNRESET: c_int = 104;
pub const ENOBUFS: c_int = 105;
pub const EISCONN: c_int = 106;
pub const ENOTCONN: c_int = 107;
pub const ESHUTDOWN: c_int = 108;
pub const ETOOMANYREFS: c_int = 109;
pub const ETIMEDOUT: c_int = 110;
pub const ECONNREFUSED: c_int = 111;
pub const EHOSTDOWN: c_int = 112;
pub const EHOSTUNREACH: c_int = 113;
pub const EALREADY: c_int = 114;
pub const EINPROGRESS: c_int = 115;
pub const ESTALE: c_int = 116;
pub const EDQUOT: c_int = 122;
pub const ECANCELED: c_int = 125;
pub const EOWNERDEAD: c_int = 130;
pub const ENOTRECOVERABLE: c_int = 131;

// ── open(2) flags (src/include/fry_fcntl.h) ──────────────────────────────
pub const O_RDONLY: c_int = 0x00;
pub const O_WRONLY: c_int = 0x01;
pub const O_RDWR: c_int = 0x02;
pub const O_CREAT: c_int = 0x40;
pub const O_TRUNC: c_int = 0x200;
pub const O_APPEND: c_int = 0x400;
pub const O_NONBLOCK: c_int = 0x800;
pub const O_CLOEXEC: c_int = 0x80000;

// ── lseek whence (src/include/fry_seek.h) ────────────────────────────────
pub const SEEK_SET: c_int = 0;
pub const SEEK_CUR: c_int = 1;
pub const SEEK_END: c_int = 2;

// ── mmap prot/flags (src/include/sys/mman.h) ─────────────────────────────
pub const PROT_NONE: c_int = 0x00;
pub const PROT_READ: c_int = 0x01;
pub const PROT_WRITE: c_int = 0x02;
pub const PROT_EXEC: c_int = 0x04;
pub const MAP_SHARED: c_int = 0x01;
pub const MAP_PRIVATE: c_int = 0x02;
pub const MAP_FIXED: c_int = 0x10;
pub const MAP_ANON: c_int = 0x20;
pub const MAP_ANONYMOUS: c_int = 0x20;
pub const MAP_FAILED: *mut c_void = !0 as *mut c_void;

// ── mode bits (src/include/sys/stat.h) ───────────────────────────────────
pub const S_IFMT: mode_t = 0o170000;
pub const S_IFSOCK: mode_t = 0o140000;
pub const S_IFLNK: mode_t = 0o120000;
pub const S_IFREG: mode_t = 0o100000;
pub const S_IFBLK: mode_t = 0o060000;
pub const S_IFDIR: mode_t = 0o040000;
pub const S_IFCHR: mode_t = 0o020000;
pub const S_IFIFO: mode_t = 0o010000;
// permission bits (src/include/sys/stat.h)
pub const S_IRWXU: mode_t = 0o700;
pub const S_IRUSR: mode_t = 0o400;
pub const S_IWUSR: mode_t = 0o200;
pub const S_IXUSR: mode_t = 0o100;
pub const S_IRWXG: mode_t = 0o070;
pub const S_IRGRP: mode_t = 0o040;
pub const S_IWGRP: mode_t = 0o020;
pub const S_IXGRP: mode_t = 0o010;
pub const S_IRWXO: mode_t = 0o007;
pub const S_IROTH: mode_t = 0o004;
pub const S_IWOTH: mode_t = 0o002;
pub const S_IXOTH: mode_t = 0o001;

// extra open(2) flags. O_ACCMODE/EXCL/DIRECTORY/NOFOLLOW not yet in TaterTOS
// fry_fcntl.h — binding-only standard values; back-port when implemented.
pub const O_ACCMODE: c_int = 0x3;
pub const O_EXCL: c_int = 0x80;
pub const O_DIRECTORY: c_int = 0x10000;
pub const O_NOFOLLOW: c_int = 0x20000;

// fcntl cmds (src/include/fcntl.h)
pub const F_GETFD: c_int = 1;
pub const F_SETFD: c_int = 2;
pub const F_GETFL: c_int = 3;
pub const F_SETFL: c_int = 4;
pub const F_DUPFD: c_int = 0;
pub const F_DUPFD_CLOEXEC: c_int = 1030; // binding-only
pub const FD_CLOEXEC: c_int = 1;
pub const FIOCLEX: c_ulong = 0x5451; // src/include (ioctl)
pub const FIONBIO: c_ulong = 0x5421;

// clocks (src/include/time.h) — match TaterTOS kernel ids
pub const CLOCK_REALTIME: clockid_t = 0;
pub const CLOCK_MONOTONIC: clockid_t = 1;
pub const CLOCK_BOOTTIME: clockid_t = 1;

// *at flags — not yet in TaterTOS headers; binding-only standard values.
pub const AT_FDCWD: c_int = -100;
pub const AT_SYMLINK_NOFOLLOW: c_int = 0x100;
pub const AT_REMOVEDIR: c_int = 0x200;

// utimensat special nsec — binding-only standard values.
pub const UTIME_NOW: c_long = 0x3fffffff;
pub const UTIME_OMIT: c_long = 0x3ffffffe;

// poll (src/include/poll.h)
pub const POLLIN: c_short = 0x0001;
pub const POLLPRI: c_short = 0x0002;
pub const POLLOUT: c_short = 0x0004;
pub const POLLERR: c_short = 0x0008;
pub const POLLHUP: c_short = 0x0010;
pub const POLLNVAL: c_short = 0x0020;

// sysconf names (src/include/unistd.h)
pub const _SC_PAGESIZE: c_int = 30;
pub const _SC_HOST_NAME_MAX: c_int = 180;
pub const _SC_GETPW_R_SIZE_MAX: c_int = 1024;

pub const SOMAXCONN: c_int = 128;
pub const EAI_SYSTEM: c_int = 9; // src/include/netdb.h
pub const EWOULDBLOCK: c_int = EAGAIN;
pub const EXIT_SUCCESS: c_int = 0;
pub const EXIT_FAILURE: c_int = 1;
pub const PTHREAD_STACK_MIN: size_t = 16384; // binding-only

// ── TaterTOS syscall numbers (src/include/tatertos/syscall.h) ────────────
// The ABI. Distinct from Linux. Single source of truth.
pub const SYS_WRITE: c_long = 0;
pub const SYS_READ: c_long = 1;
pub const SYS_EXIT: c_long = 2;
pub const SYS_OPEN: c_long = 5;
pub const SYS_CLOSE: c_long = 6;
pub const SYS_GETPID: c_long = 7;
pub const SYS_STAT: c_long = 8;
pub const SYS_MMAP: c_long = 52;
pub const SYS_MUNMAP: c_long = 53;
pub const SYS_MPROTECT: c_long = 54;
pub const SYS_GETTID: c_long = 67;
pub const SYS_FUTEX_WAIT: c_long = 68;
pub const SYS_FUTEX_WAKE: c_long = 69;
pub const SYS_GETRANDOM: c_long = 85;
pub const SYS_CLOCK_GETTIME: c_long = 86;
pub const SYS_NANOSLEEP: c_long = 87;
pub const SYS_LSEEK: c_long = 88;
pub const SYS_FSTAT: c_long = 91;

// ── signals (src/include/signal.h) ───────────────────────────────────────
pub const SIGHUP: c_int = 1;
pub const SIGINT: c_int = 2;
pub const SIGQUIT: c_int = 3;
pub const SIGILL: c_int = 4;
pub const SIGTRAP: c_int = 5;
pub const SIGABRT: c_int = 6;
pub const SIGBUS: c_int = 7;
pub const SIGFPE: c_int = 8;
pub const SIGKILL: c_int = 9;
pub const SIGUSR1: c_int = 10;
pub const SIGSEGV: c_int = 11;
pub const SIGUSR2: c_int = 12;
pub const SIGPIPE: c_int = 13;
pub const SIGALRM: c_int = 14;
pub const SIGTERM: c_int = 15;
pub const SIGCHLD: c_int = 17;
pub const SIGCONT: c_int = 18;
pub const SIGSTOP: c_int = 19;
pub const SIGTSTP: c_int = 20;
pub const SIGTTIN: c_int = 21;
pub const SIGTTOU: c_int = 22;
pub const SIGURG: c_int = 23;
pub const SIGXCPU: c_int = 24;
pub const SIGXFSZ: c_int = 25;
pub const SIGVTALRM: c_int = 26;
pub const SIGPROF: c_int = 27;
pub const SIGWINCH: c_int = 28;
pub const SIGIO: c_int = 29;
pub const SIGSYS: c_int = 31;
pub const SIG_SETMASK: c_int = 2;
pub const SIG_BLOCK: c_int = 0;
pub const SIG_UNBLOCK: c_int = 1;
pub const SA_SIGINFO: c_int = 0x00000004;
pub const SA_RESTART: c_int = 0x10000000;
pub const SIG_DFL: sighandler_t = 0;
pub const SIG_IGN: sighandler_t = 1;
pub type sighandler_t = size_t;

// ── sockets / net (src/include/sys/socket.h, netinet/in.h, sys/un.h) ─────
pub const AF_UNIX: c_int = 1;
pub const AF_INET: c_int = 2;
pub const AF_INET6: c_int = 10;
pub const SOCK_STREAM: c_int = 1;
pub const SOCK_DGRAM: c_int = 2;
pub const SOCK_RAW: c_int = 3;
pub const SOL_SOCKET: c_int = 1;
pub const SO_REUSEADDR: c_int = 2;
pub const SO_ERROR: c_int = 4;
pub const SO_BROADCAST: c_int = 6;
pub const SO_KEEPALIVE: c_int = 9;
pub const SO_LINGER: c_int = 13;
// Not yet in TaterTOS headers — binding-only, standard values; kernel may
// return ENOPROTOOPT. Back-port to src/include/sys/socket.h when implemented.
pub const SO_RCVTIMEO: c_int = 20;
pub const SO_SNDTIMEO: c_int = 21;
pub const SO_REUSEPORT: c_int = 15;
pub const MSG_PEEK: c_int = 0x02;
pub const IPPROTO_IP: c_int = 0;
pub const IPPROTO_TCP: c_int = 6;
pub const IPPROTO_UDP: c_int = 17;
pub const IPPROTO_IPV6: c_int = 41;
pub const TCP_NODELAY: c_int = 1;
pub const IP_TTL: c_int = 2; // binding-only (see note above)
pub const IP_MULTICAST_TTL: c_int = 33; // binding-only
pub const IP_MULTICAST_LOOP: c_int = 34; // binding-only
pub const IP_ADD_MEMBERSHIP: c_int = 35; // binding-only
pub const IP_DROP_MEMBERSHIP: c_int = 36; // binding-only
pub const IPV6_UNICAST_HOPS: c_int = 16;
pub const IPV6_MULTICAST_HOPS: c_int = 18;
pub const IPV6_MULTICAST_LOOP: c_int = 19;
pub const IPV6_ADD_MEMBERSHIP: c_int = 20;
pub const IPV6_DROP_MEMBERSHIP: c_int = 21;
pub const IPV6_V6ONLY: c_int = 26;
pub const SHUT_RD: c_int = 0;
pub const SHUT_WR: c_int = 1;
pub const SHUT_RDWR: c_int = 2;

// ── wait(2) status (src/include/sys/wait.h) ──────────────────────────────
pub const WNOHANG: c_int = 1;
pub const WUNTRACED: c_int = 2;
pub const WSTOPPED: c_int = 2;
pub const WEXITED: c_int = 4;
pub const WCONTINUED: c_int = 8;
pub const WNOWAIT: c_int = 0x01000000;

s! {
    // src/include/netinet/in.h
    pub struct in_addr {
        pub s_addr: in_addr_t,
    }
    pub struct sockaddr_in {
        pub sin_family: sa_family_t,
        pub sin_port: in_port_t,
        pub sin_addr: in_addr,
        pub sin_zero: [u8; 8],
    }
    // (in6_addr is provided by the common unix layer — reuse it.)
    pub struct sockaddr_in6 {
        pub sin6_family: sa_family_t,
        pub sin6_port: in_port_t,
        pub sin6_flowinfo: u32,
        pub sin6_addr: in6_addr,
        pub sin6_scope_id: u32,
    }
    // ip_mreq/ipv6_mreq: standard layout (not yet in TaterTOS headers).
    pub struct ip_mreq {
        pub imr_multiaddr: in_addr,
        pub imr_interface: in_addr,
    }
    // (ipv6_mreq is provided by the common unix layer — reuse it.)
    // src/include/sys/un.h (UNIX_PATH_MAX = 108)
    pub struct sockaddr_un {
        pub sun_family: sa_family_t,
        pub sun_path: [c_char; 108],
    }
    // Standard 128-byte storage (TaterTOS has no explicit struct; size per POSIX).
    pub struct sockaddr_storage {
        pub ss_family: sa_family_t,
        __ss_pad: [u8; 126],
    }

    // (iovec is provided by the common unix layer — reuse it.)

    // src/include/poll.h
    pub struct pollfd {
        pub fd: c_int,
        pub events: c_short,
        pub revents: c_short,
    }
}
// DIR (opaque) is provided by the common unix layer — reuse it.

// Wait-status macros (src/include/sys/wait.h) as safe const fns.
safe_f! {
    pub fn WIFEXITED(status: c_int) -> bool {
        (status & 0x7f) == 0
    }
    pub fn WEXITSTATUS(status: c_int) -> c_int {
        (status >> 8) & 0xff
    }
    pub fn WTERMSIG(status: c_int) -> c_int {
        status & 0x7f
    }
    pub fn WIFSIGNALED(status: c_int) -> bool {
        ((status & 0x7f) + 1) as i8 >= 2
    }
    pub fn WIFSTOPPED(status: c_int) -> bool {
        (status & 0xff) == 0x7f
    }
    pub fn WSTOPSIG(status: c_int) -> c_int {
        (status >> 8) & 0xff
    }
    pub fn WIFCONTINUED(status: c_int) -> bool {
        status == 0xffff
    }
    pub fn WCOREDUMP(status: c_int) -> bool {
        (status & 0x80) != 0
    }
}

// ── POSIX functions — resolve to TaterTOS libc.a (src/user/libc) ─────────
extern "C" {
    // process / wait
    pub fn waitpid(pid: pid_t, status: *mut c_int, options: c_int) -> pid_t;
    pub fn fork() -> pid_t;
    pub fn kill(pid: pid_t, sig: c_int) -> c_int;
    pub fn getppid() -> pid_t;
    pub fn getuid() -> uid_t;
    pub fn geteuid() -> uid_t;
    pub fn getgid() -> gid_t;
    pub fn getegid() -> gid_t;
    pub fn setgroups(size: size_t, list: *const gid_t) -> c_int;
    pub fn getpwuid_r(
        uid: uid_t,
        pwd: *mut passwd,
        buf: *mut c_char,
        buflen: size_t,
        result: *mut *mut passwd,
    ) -> c_int;

    // signals
    pub fn sigaction(sig: c_int, act: *const sigaction, oldact: *mut sigaction) -> c_int;
    pub fn sigaddset(set: *mut sigset_t, signum: c_int) -> c_int;
    pub fn sigemptyset(set: *mut sigset_t) -> c_int;
    pub fn pthread_sigmask(how: c_int, set: *const sigset_t, oldset: *mut sigset_t) -> c_int;

    // time
    pub fn clock_gettime(clk_id: clockid_t, tp: *mut timespec) -> c_int;
    pub fn nanosleep(req: *const timespec, rem: *mut timespec) -> c_int;
    pub fn gmtime_r(time: *const time_t, result: *mut tm) -> *mut tm;
    pub fn localtime_r(time: *const time_t, result: *mut tm) -> *mut tm;

    // fs
    pub fn lstat(path: *const c_char, buf: *mut stat) -> c_int;
    pub fn fstatat(dirfd: c_int, path: *const c_char, buf: *mut stat, flags: c_int) -> c_int;
    pub fn statvfs(path: *const c_char, buf: *mut statvfs) -> c_int;
    pub fn fstatvfs(fd: c_int, buf: *mut statvfs) -> c_int;
    pub fn ftruncate(fd: c_int, length: off_t) -> c_int;
    pub fn mkdir(path: *const c_char, mode: mode_t) -> c_int;
    pub fn rmdir(path: *const c_char) -> c_int;
    pub fn unlink(path: *const c_char) -> c_int;
    pub fn rename(old: *const c_char, new: *const c_char) -> c_int;
    pub fn readlink(path: *const c_char, buf: *mut c_char, bufsiz: size_t) -> ssize_t;
    pub fn symlink(target: *const c_char, linkpath: *const c_char) -> c_int;
    pub fn link(old: *const c_char, new: *const c_char) -> c_int;
    pub fn chmod(path: *const c_char, mode: mode_t) -> c_int;
    pub fn fchmod(fd: c_int, mode: mode_t) -> c_int;
    pub fn realpath(path: *const c_char, resolved: *mut c_char) -> *mut c_char;
    pub fn getcwd(buf: *mut c_char, size: size_t) -> *mut c_char;
    pub fn chdir(path: *const c_char) -> c_int;
    pub fn dup(fd: c_int) -> c_int;
    pub fn dup2(old: c_int, new: c_int) -> c_int;
    pub fn pipe(fds: *mut c_int) -> c_int;
    pub fn fcntl(fd: c_int, cmd: c_int, ...) -> c_int;
    pub fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
    pub fn fsync(fd: c_int) -> c_int;
    pub fn fdatasync(fd: c_int) -> c_int;
    pub fn readv(fd: c_int, iov: *const iovec, iovcnt: c_int) -> ssize_t;
    pub fn writev(fd: c_int, iov: *const iovec, iovcnt: c_int) -> ssize_t;
    pub fn pread(fd: c_int, buf: *mut c_void, count: size_t, offset: off_t) -> ssize_t;
    pub fn pwrite(fd: c_int, buf: *const c_void, count: size_t, offset: off_t) -> ssize_t;
    pub fn futimens(fd: c_int, times: *const timespec) -> c_int;
    pub fn utimensat(
        dirfd: c_int,
        path: *const c_char,
        times: *const timespec,
        flags: c_int,
    ) -> c_int;
    /* BSD-style time setters (used by the `filetime` crate). times = [atime, mtime]. */
    pub fn futimes(fd: c_int, times: *const timeval) -> c_int;
    pub fn lutimes(file: *const c_char, times: *const timeval) -> c_int;
    pub fn utimes(file: *const c_char, times: *const timeval) -> c_int;

    // dirent
    pub fn opendir(name: *const c_char) -> *mut DIR;
    pub fn readdir(dirp: *mut DIR) -> *mut dirent;
    pub fn closedir(dirp: *mut DIR) -> c_int;
    pub fn dirfd(dirp: *mut DIR) -> c_int;
    pub fn linkat(
        olddirfd: c_int,
        oldpath: *const c_char,
        newdirfd: c_int,
        newpath: *const c_char,
        flags: c_int,
    ) -> c_int;

    // sockets
    pub fn socket(domain: c_int, ty: c_int, protocol: c_int) -> c_int;
    pub fn bind(sockfd: c_int, addr: *const sockaddr, addrlen: socklen_t) -> c_int;
    pub fn connect(sockfd: c_int, addr: *const sockaddr, addrlen: socklen_t) -> c_int;
    pub fn listen(sockfd: c_int, backlog: c_int) -> c_int;
    pub fn accept(sockfd: c_int, addr: *mut sockaddr, addrlen: *mut socklen_t) -> c_int;
    pub fn send(sockfd: c_int, buf: *const c_void, len: size_t, flags: c_int) -> ssize_t;
    pub fn recv(sockfd: c_int, buf: *mut c_void, len: size_t, flags: c_int) -> ssize_t;
    pub fn sendto(
        sockfd: c_int,
        buf: *const c_void,
        len: size_t,
        flags: c_int,
        addr: *const sockaddr,
        addrlen: socklen_t,
    ) -> ssize_t;
    pub fn recvfrom(
        sockfd: c_int,
        buf: *mut c_void,
        len: size_t,
        flags: c_int,
        addr: *mut sockaddr,
        addrlen: *mut socklen_t,
    ) -> ssize_t;
    pub fn getsockopt(
        sockfd: c_int,
        level: c_int,
        optname: c_int,
        optval: *mut c_void,
        optlen: *mut socklen_t,
    ) -> c_int;
    pub fn setsockopt(
        sockfd: c_int,
        level: c_int,
        optname: c_int,
        optval: *const c_void,
        optlen: socklen_t,
    ) -> c_int;
    pub fn shutdown(sockfd: c_int, how: c_int) -> c_int;
    pub fn getsockname(sockfd: c_int, addr: *mut sockaddr, addrlen: *mut socklen_t) -> c_int;
    pub fn getpeername(sockfd: c_int, addr: *mut sockaddr, addrlen: *mut socklen_t) -> c_int;

    // pthread
    pub fn pthread_create(
        thread: *mut pthread_t,
        attr: *const pthread_attr_t,
        start_routine: extern "C" fn(*mut c_void) -> *mut c_void,
        arg: *mut c_void,
    ) -> c_int;
    pub fn pthread_join(thread: pthread_t, retval: *mut *mut c_void) -> c_int;
    pub fn pthread_detach(thread: pthread_t) -> c_int;
    pub fn pthread_self() -> pthread_t;
    pub fn pthread_attr_init(attr: *mut pthread_attr_t) -> c_int;
    pub fn pthread_attr_destroy(attr: *mut pthread_attr_t) -> c_int;
    pub fn pthread_attr_setstacksize(attr: *mut pthread_attr_t, stacksize: size_t) -> c_int;
    pub fn pthread_mutex_init(m: *mut pthread_mutex_t, a: *const pthread_mutexattr_t) -> c_int;
    pub fn pthread_mutex_lock(m: *mut pthread_mutex_t) -> c_int;
    pub fn pthread_mutex_trylock(m: *mut pthread_mutex_t) -> c_int;
    pub fn pthread_mutex_unlock(m: *mut pthread_mutex_t) -> c_int;
    pub fn pthread_mutex_destroy(m: *mut pthread_mutex_t) -> c_int;
    pub fn pthread_mutexattr_init(a: *mut pthread_mutexattr_t) -> c_int;
    pub fn pthread_mutexattr_settype(a: *mut pthread_mutexattr_t, ty: c_int) -> c_int;
    pub fn pthread_mutexattr_destroy(a: *mut pthread_mutexattr_t) -> c_int;
    pub fn pthread_cond_init(c: *mut pthread_cond_t, a: *const pthread_condattr_t) -> c_int;
    pub fn pthread_cond_wait(c: *mut pthread_cond_t, m: *mut pthread_mutex_t) -> c_int;
    pub fn pthread_cond_timedwait(
        c: *mut pthread_cond_t,
        m: *mut pthread_mutex_t,
        abstime: *const timespec,
    ) -> c_int;
    pub fn pthread_cond_signal(c: *mut pthread_cond_t) -> c_int;
    pub fn pthread_cond_broadcast(c: *mut pthread_cond_t) -> c_int;
    pub fn pthread_cond_destroy(c: *mut pthread_cond_t) -> c_int;
    pub fn pthread_condattr_init(a: *mut pthread_condattr_t) -> c_int;
    pub fn pthread_condattr_setclock(a: *mut pthread_condattr_t, clock_id: clockid_t) -> c_int;
    pub fn pthread_condattr_destroy(a: *mut pthread_condattr_t) -> c_int;
    pub fn pthread_key_create(key: *mut pthread_key_t, dtor: Option<extern "C" fn(*mut c_void)>) -> c_int;
    pub fn pthread_key_delete(key: pthread_key_t) -> c_int;
    pub fn pthread_setspecific(key: pthread_key_t, value: *const c_void) -> c_int;
    pub fn pthread_getspecific(key: pthread_key_t) -> *mut c_void;

    // env / misc
    pub fn getenv(name: *const c_char) -> *mut c_char;
    pub fn setenv(name: *const c_char, value: *const c_char, overwrite: c_int) -> c_int;
    pub fn unsetenv(name: *const c_char) -> c_int;
    pub fn strerror_r(errnum: c_int, buf: *mut c_char, buflen: size_t) -> c_int;
    pub fn sysconf(name: c_int) -> c_long;
    pub fn isatty(fd: c_int) -> c_int;
    pub fn getrandom(buf: *mut c_void, buflen: size_t, flags: c_uint) -> ssize_t;
    pub fn open(path: *const c_char, oflag: c_int, ...) -> c_int;
    pub fn close(fd: c_int) -> c_int;
    pub fn read(fd: c_int, buf: *mut c_void, count: size_t) -> ssize_t;
    pub fn write(fd: c_int, buf: *const c_void, count: size_t) -> ssize_t;
    pub fn lseek(fd: c_int, offset: off_t, whence: c_int) -> off_t;
    pub fn fstat(fd: c_int, buf: *mut stat) -> c_int;
    pub fn stat(path: *const c_char, buf: *mut stat) -> c_int;

    pub fn mmap(
        addr: *mut c_void,
        len: size_t,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: off_t,
    ) -> *mut c_void;
    pub fn munmap(addr: *mut c_void, len: size_t) -> c_int;
    pub fn mprotect(addr: *mut c_void, len: size_t, prot: c_int) -> c_int;

    pub fn malloc(size: size_t) -> *mut c_void;
    pub fn calloc(nmemb: size_t, size: size_t) -> *mut c_void;
    pub fn realloc(ptr: *mut c_void, size: size_t) -> *mut c_void;
    pub fn free(ptr: *mut c_void);

    pub fn abort() -> !;
    pub fn exit(code: c_int) -> !;
    pub fn getpid() -> pid_t;

    pub fn memcpy(dest: *mut c_void, src: *const c_void, n: size_t) -> *mut c_void;
    pub fn memmove(dest: *mut c_void, src: *const c_void, n: size_t) -> *mut c_void;
    pub fn memset(dest: *mut c_void, c: c_int, n: size_t) -> *mut c_void;
    pub fn memcmp(s1: *const c_void, s2: *const c_void, n: size_t) -> c_int;
    pub fn strlen(s: *const c_char) -> size_t;
}
