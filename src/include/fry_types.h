#ifndef _TATERTOS_TYPES_H
#define _TATERTOS_TYPES_H

/*
 * TaterTOS64v3 — Portable type definitions
 *
 * Canonical widths for syscall arguments and kernel/user shared structures.
 * stdint.h provides the raw integer types; this header provides the
 * semantic aliases that make ABI intent clear.
 */

#include <stdint.h>
#include <stddef.h>

typedef int32_t  pid_t;
typedef int64_t  ssize_t;
typedef int64_t  off_t;
typedef uint32_t mode_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int32_t  fd_t;

#endif /* _TATERTOS_TYPES_H */
