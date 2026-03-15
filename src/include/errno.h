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
#define EAGAIN  11   /* Resource temporarily unavailable */
#define EBUSY   16   /* Device or resource busy */

#endif /* _TATERTOS_ERRNO_H */
