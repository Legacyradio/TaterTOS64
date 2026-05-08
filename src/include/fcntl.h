/*
 * TaterTOS64v3 — <fcntl.h>
 *
 * POSIX file-control header. Re-exposes fry_fcntl.h's flag/command
 * constants and declares open()/fcntl()/openat()/etc.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_TOPLEVEL_FCNTL_H
#define _TATERTOS_TOPLEVEL_FCNTL_H

#include <stddef.h>
#include <fry_fcntl.h>      /* O_RDONLY/WRONLY/RDWR/CREAT/TRUNC/APPEND/
                               NONBLOCK + F_GET/SETFD/FL */
#include <sys/types.h>      /* off_t, mode_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Additional POSIX flags not in fry_fcntl.h. Match Linux.
 */
#ifndef O_EXCL
#  define O_EXCL      0x80
#  define O_NOCTTY    0x100
#  define O_DSYNC     0x1000
#  define O_DIRECTORY 0x10000
#  define O_NOFOLLOW  0x20000
#  define O_CLOEXEC   0x80000
#  define O_PATH      0x200000
#  define O_TMPFILE   0x410000
#endif

/*
 * Additional fcntl commands.
 */
#ifndef F_DUPFD
#  define F_DUPFD          0
#  define F_DUPFD_CLOEXEC  1030
#  define F_GETLK          5
#  define F_SETLK          6
#  define F_SETLKW         7
#endif

/*
 * Close-on-exec flag for F_SETFD.
 */
#ifndef FD_CLOEXEC
#  define FD_CLOEXEC 1
#endif

/*
 * AT_* flags for openat / faccessat / fstatat.
 */
#ifndef AT_FDCWD
#  define AT_FDCWD             (-100)
#  define AT_SYMLINK_NOFOLLOW  0x100
#  define AT_REMOVEDIR         0x200
#  define AT_SYMLINK_FOLLOW    0x400
#  define AT_NO_AUTOMOUNT      0x800
#  define AT_EMPTY_PATH        0x1000
#  define AT_EACCESS           0x200
#endif

/*
 * Backed by src/user/libc/{libc,posix}.c.
 */
int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_TOPLEVEL_FCNTL_H */
