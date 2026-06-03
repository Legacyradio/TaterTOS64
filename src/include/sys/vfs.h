/*
 * TaterTOS64v3 — <sys/vfs.h>
 *
 * POSIX filesystem statistics stub.
 */

#ifndef _TATERTOS_SYS_VFS_H
#define _TATERTOS_SYS_VFS_H

#include <sys/statfs.h>

/* If no statfs.h, provide minimal struct */
#ifndef _TATERTOS_SYS_STATFS_H
struct statfs {
    long f_type;
    long f_bsize;
    long f_blocks;
    long f_bfree;
    long f_bavail;
    long f_files;
    long f_ffree;
    long f_namelen;
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);
#endif

#endif
