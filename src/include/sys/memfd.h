/*
 * TaterTOS64v3 — <sys/memfd.h>
 *
 * POSIX/Linux-compatible memfd_create() declaration.
 */

#ifndef _TATERTOS_SYS_MEMFD_H
#define _TATERTOS_SYS_MEMFD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flags for memfd_create */
#define MFD_CLOEXEC      0x0001U
#define MFD_ALLOW_SEALING 0x0002U
#define MFD_HUGETLB      0x0004U

int memfd_create(const char *name, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* _TATERTOS_SYS_MEMFD_H */
