#ifndef _TATERTOS_ERRNO_H
#define _TATERTOS_ERRNO_H

/*
 * TaterTOS64v3 — Standard error codes
 *
 * POSIX-compatible subset. Syscalls return negative errno on failure
 * (e.g. -EINVAL). Userspace wrappers may translate to a positive errno
 * stored in a thread-local variable later; for now the raw negative
 * value is returned directly.
 *
 * Values match Linux where possible so ported code sees familiar numbers.
 */

#define EPERM    1   /* Operation not permitted */
#define ENOENT   2   /* No such file or directory */
#define ESRCH    3   /* No such process */
#define EINTR    4   /* Interrupted system call */
#define EIO      5   /* I/O error */
#define ENXIO    6   /* No such device or address */
#define EBADF    9   /* Bad file descriptor */
#define ENOMEM  12   /* Out of memory */
#define EACCES  13   /* Permission denied */
#define EFAULT  14   /* Bad address (invalid user pointer) */
#define EEXIST  17   /* File exists */
#define ENOTDIR 20   /* Not a directory */
#define EISDIR  21   /* Is a directory */
#define EINVAL  22   /* Invalid argument */
#define ENFILE  23   /* Too many open files in system */
#define EMFILE  24   /* Too many open files per process */
#define ENOSPC  28   /* No space left on device */
#define ERANGE  34   /* Result too large / out of range */
#define ENAMETOOLONG 36 /* File name too long */
#define ENOSYS  38   /* Function not implemented */
#define ENOTEMPTY 39 /* Directory not empty */
#define ETIMEDOUT 110 /* Connection timed out / wait timed out */
#define EAGAIN  11   /* Resource temporarily unavailable */
#define EBUSY   16   /* Device or resource busy */
#define EPIPE   32   /* Broken pipe */
#define ESPIPE  29   /* Illegal seek (on pipe/socket) */
#define E2BIG    7   /* Argument list too long */
#define ECHILD  10   /* No child processes */
#define EWOULDBLOCK EAGAIN  /* Operation would block */

/*
 * Additional POSIX values added in fry834 (Ladybird port) — match
 * Linux numbers so ported code sees familiar values.
 */
#define EDEADLK       35  /* Resource deadlock would occur */
#define EDEADLOCK     EDEADLK
#define ENOLCK        37  /* No record locks available */
#define ELOOP         40  /* Too many levels of symbolic links */
#define ENOMSG        42  /* No message of desired type */
#define EIDRM         43  /* Identifier removed */
#define ENODATA       61  /* No data available */
#define EBADMSG       74  /* Not a data message */
#define EOVERFLOW     75  /* Value too large for defined data type */
#define ENOTUNIQ      76  /* Name not unique on network */
#define EILSEQ        84  /* Invalid or incomplete multibyte/wide character */
#define ENOTSUP       95  /* Operation not supported */
#define EOPNOTSUPP    ENOTSUP
#define EHOSTDOWN    112  /* Host is down */
#define ESTALE       116  /* Stale file handle */
#define EDQUOT       122  /* Disk quota exceeded */
#define ECANCELED    125  /* Operation canceled */
#define EOWNERDEAD   130  /* Owner died (mutex robust attribute) */
#define ENOTRECOVERABLE 131 /* State not recoverable */
#define ENODEV       19  /* No such device */
#define EPROTO       71  /* Protocol error */
#define ETXTBSY      26  /* Text file busy */
#define ENOTTY       25  /* Inappropriate ioctl for device */
#define ENOLINK      67  /* Link has been severed */
#define ENXIO         6
#define EXDEV        18  /* Cross-device link */
#define EROFS        30  /* Read-only file system */
#define EMLINK       31  /* Too many links */
#define EDOM         33  /* Math argument out of domain */
#define ENOEXEC       8  /* Exec format error */
#define EFBIG        27  /* File too large */

/*
 * errno — POSIX requires this be a per-thread modifiable lvalue.
 * TaterTOS implements it via __errno_location() in src/user/libc/libc.c
 * (declared in libc.h:677). Make it accessible from <errno.h> too.
 */
#ifdef __cplusplus
extern "C" {
#endif
int *__errno_location(void);
#ifdef __cplusplus
} /* extern "C" */
#endif

#ifndef errno
#define errno (*__errno_location())
#endif

/* Network / socket errors */
#define ENOTSOCK      88  /* Socket operation on non-socket */
#define EDESTADDRREQ  89  /* Destination address required */
#define EPROTOTYPE    91  /* Protocol wrong type for socket */
#define ENOPROTOOPT   92  /* Protocol not available */
#define EAFNOSUPPORT  97  /* Address family not supported */
#define EADDRINUSE    98  /* Address already in use */
#define EADDRNOTAVAIL 99  /* Cannot assign requested address */
#define ENETUNREACH  101  /* Network is unreachable */
#define ECONNABORTED 103  /* Connection aborted */
#define ECONNRESET   104  /* Connection reset by peer */
#define EISCONN      106  /* Transport endpoint is already connected */
#define ENOTCONN     107  /* Transport endpoint is not connected */
#define ECONNREFUSED 111  /* Connection refused */
#define EHOSTUNREACH 113  /* No route to host */
#define EALREADY     114  /* Operation already in progress */
#define EINPROGRESS  115  /* Operation now in progress */
#define EMSGSIZE      90  /* Message too long */
#define ENOBUFS      105  /* No buffer space available */
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define ENETDOWN     100
#define ENETRESET    102
#define ESHUTDOWN    108
#define ETOOMANYREFS 109
#define EUSERS        87

#endif /* _TATERTOS_ERRNO_H */
