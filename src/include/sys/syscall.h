/*
 * TaterTOS64v3 — <sys/syscall.h>
 *
 * Stub for partition_alloc's random utilities.
 * TaterTOS has fry_getrandom() for entropy.
 * SYS_getrandom is not available as a syscall number;
 * getrandom() should be used instead.
 */

#ifndef _TATERTOS_SYS_SYSCALL_H
#define _TATERTOS_SYS_SYSCALL_H

/* Not needed — TaterTOS uses fry_getrandom() directly. */

#endif /* _TATERTOS_SYS_SYSCALL_H */
