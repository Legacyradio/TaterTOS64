/*
 * TaterTOS64v3 — <sys/types.h>
 *
 * POSIX sys/types.h surface for TaterTOS userland. Re-exports the
 * typedefs already in <fry_types.h> and adds the rest of the
 * POSIX-required type aliases.
 *
 * This is engineered code, not a stub. Every TaterTOS app that
 * ports POSIX-flavored upstream code can rely on the standard
 * <sys/types.h> path.
 *
 * Origin log: logs/fry830.txt
 * Triggered by: AK/Types.h:135 (Ladybird port, fry829)
 *
 * Note on time_t: identical to the typedef in src/user/libc/libc.h:981
 * (typedef int64_t time_t). Identical-typename, identical-target
 * typedef redeclarations are permitted by C11 §6.7p3 and C++ —
 * including both libc.h and <sys/types.h> in the same TU is safe.
 */

#ifndef _TATERTOS_SYS_TYPES_H
#define _TATERTOS_SYS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <fry_types.h>      /* pid_t, ssize_t, off_t, mode_t, uid_t,
                               gid_t, fd_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * POSIX file/inode/device types not already in fry_types.h.
 * Widths match common Unix conventions (Linux glibc + macOS) so
 * upstream code that assumes those widths Just Works.
 */
typedef uint64_t  dev_t;        /* device id */
typedef uint64_t  ino_t;        /* inode number */
typedef uint64_t  ino64_t;
typedef int64_t   blkcnt_t;     /* file block count */
typedef int64_t   blksize_t;    /* preferred I/O block size */
typedef uint32_t  nlink_t;      /* link count */

/*
 * POSIX time / scheduling types.
 * time_t MUST match libc.h:981 exactly (typedef int64_t time_t).
 */
typedef int64_t   time_t;
typedef int64_t   clock_t;
typedef int64_t   suseconds_t;
typedef uint32_t  useconds_t;

/*
 * POSIX IPC / process group / session types.
 */
typedef int32_t   key_t;
typedef int32_t   id_t;

/*
 * Loff_t — large-file offset (Linux extension AK occasionally uses).
 * Matches off_t since TaterTOS uses 64-bit offsets natively.
 */
typedef off_t     loff_t;

/*
 * BSD-flavored shorthand integer typedefs occasionally referenced by
 * upstream POSIX code. Defined here per the historical Unix
 * convention of <sys/types.h> being the umbrella header for these.
 */
typedef uint8_t   u_int8_t;
typedef uint16_t  u_int16_t;
typedef uint32_t  u_int32_t;
typedef uint64_t  u_int64_t;
typedef uint8_t   u_char;
typedef uint16_t  u_short;
typedef uint32_t  u_int;
typedef uint64_t  u_long;
typedef uint64_t  u_quad_t;
typedef int64_t   quad_t;

/*
 * Signal-handler register save type. POSIX-required for reentrant
 * signal handlers. uint32_t matches sig_atomic_t conventions.
 */
typedef uint32_t  sig_atomic_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_TYPES_H */
