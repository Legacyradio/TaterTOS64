/*
 * TaterTOS64v3 — <sys/stat.h>
 *
 * POSIX file metadata. struct stat + stat()/fstat()/lstat()/mkdir/
 * mknod/chmod. Backed by fry_stat / fry_fstat in libc.h, plus
 * mkdir_compat / stat_compat / fstat_compat / lstat_compat in posix.c.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_SYS_STAT_H
#define _TATERTOS_SYS_STAT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>           /* struct timespec */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mode bits. Match Linux exactly.
 */
#define S_IFMT     0170000
#define S_IFSOCK   0140000
#define S_IFLNK    0120000
#define S_IFREG    0100000
#define S_IFBLK    0060000
#define S_IFDIR    0040000
#define S_IFCHR    0020000
#define S_IFIFO    0010000

#define S_ISUID    0004000
#define S_ISGID    0002000
#define S_ISVTX    0001000

#define S_IRUSR    0000400
#define S_IWUSR    0000200
#define S_IXUSR    0000100
#define S_IRWXU    0000700
#define S_IRGRP    0000040
#define S_IWGRP    0000020
#define S_IXGRP    0000010
#define S_IRWXG    0000070
#define S_IROTH    0000004
#define S_IWOTH    0000002
#define S_IXOTH    0000001
#define S_IRWXO    0000007

#define S_ISREG(m)   (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)   (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)   (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)   (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)  (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)   (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m)  (((m) & S_IFMT) == S_IFSOCK)

/*
 * struct stat — POSIX layout.
 */
struct stat {
    dev_t      st_dev;
    ino_t      st_ino;
    mode_t     st_mode;
    nlink_t    st_nlink;
    uid_t      st_uid;
    gid_t      st_gid;
    dev_t      st_rdev;
    off_t      st_size;
    blksize_t  st_blksize;
    blkcnt_t   st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

/* Linux-style aliases. */
#define st_atime  st_atim.tv_sec
#define st_mtime  st_mtim.tv_sec
#define st_ctime  st_ctim.tv_sec

/*
 * Backed by libc.h fry_stat/fry_fstat (TaterTOS-native struct fry_stat)
 * and posix.c's *_compat shims. We need the canonical POSIX names.
 */
int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstatat(int dirfd, const char *path, struct stat *st, int flags);

int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);
int mkdir(const char *path, mode_t mode);
int mkdirat(int dirfd, const char *path, mode_t mode);
int mkfifo(const char *path, mode_t mode);
int mknod(const char *path, mode_t mode, dev_t dev);

mode_t umask(mode_t cmask);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_STAT_H */
