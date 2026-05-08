/*
 * TaterTOS64v3 — <sys/statvfs.h>
 *
 * Filesystem space metadata surface for POSIX consumers. Backed by
 * statvfs() in posix.c.
 */

#ifndef _TATERTOS_SYS_STATVFS_H
#define _TATERTOS_SYS_STATVFS_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t    f_blocks;
    fsblkcnt_t    f_bfree;
    fsblkcnt_t    f_bavail;
    fsfilcnt_t    f_files;
    fsfilcnt_t    f_ffree;
    fsfilcnt_t    f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);

#ifdef __cplusplus
}
#endif

#endif /* _TATERTOS_SYS_STATVFS_H */
