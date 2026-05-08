/*
 * TaterTOS64v3 — <sys/mlock.h>
 *
 * POSIX memory locking.
 */

#ifndef _TATERTOS_SYS_MLOCK_H
#define _TATERTOS_SYS_MLOCK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCL_CURRENT 1
#define MCL_FUTURE  2

int mlock(const void *addr, size_t len);
int munlock(const void *addr, size_t len);
int mlockall(int flags);
int munlockall(void);

#ifdef __cplusplus
}
#endif

#endif /* _TATERTOS_SYS_MLOCK_H */
