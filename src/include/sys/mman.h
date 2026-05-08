/*
 * TaterTOS64v3 — <sys/mman.h>
 *
 * POSIX memory mapping. Backed by fry_mmap / fry_munmap / fry_mprotect
 * etc. in libc.h.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_SYS_MMAN_H
#define _TATERTOS_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* prot bits */
#define PROT_NONE   0x00
#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04

/* mmap flags */
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANON        0x20
#define MAP_ANONYMOUS   MAP_ANON
#define MAP_GROWSDOWN   0x100
#define MAP_NORESERVE   0x4000
#define MAP_LOCKED      0x2000
#define MAP_POPULATE    0x8000
#define MAP_NONBLOCK    0x10000
#define MAP_STACK       0x20000

#define MAP_FAILED      ((void *)-1)

/* msync flags */
#define MS_ASYNC        1
#define MS_INVALIDATE   2
#define MS_SYNC         4

/* madvise advice */
#define MADV_NORMAL      0
#define MADV_RANDOM      1
#define MADV_SEQUENTIAL  2
#define MADV_WILLNEED    3
#define MADV_DONTNEED    4
#define MADV_FREE        8
#define MADV_HUGEPAGE   14

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t length, int prot);
int   msync(void *addr, size_t length, int flags);
int   madvise(void *addr, size_t length, int advice);
int   posix_madvise(void *addr, size_t length, int advice);
int   mlock(const void *addr, size_t length);
int   munlock(const void *addr, size_t length);

int   shm_open(const char *name, int oflag, mode_t mode);
int   shm_unlink(const char *name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_MMAN_H */
