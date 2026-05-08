/*
 * TaterTOS64v3 — <dirent.h>
 *
 * POSIX directory iteration. Backed by closedir/opendir/readdir
 * shims in src/user/libc/posix.c, plus fry_readdir/_ex in libc.h.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_DIRENT_H
#define _TATERTOS_DIRENT_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * d_type values. Linux/POSIX subset.
 */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14    /* whiteout (BSD/Mac legacy) */

/*
 * struct dirent — POSIX layout. d_name is variable-length but most
 * codebases assume a 256-byte buffer.
 */
struct dirent {
    ino_t          d_ino;
    off_t          d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

/* Opaque DIR — implementation in src/user/libc/posix.c, where the
 * underlying tag is `struct _DIR`. Match libc.h's declaration. */
typedef struct _DIR DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int            readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result);
int            closedir(DIR *dirp);
void           rewinddir(DIR *dirp);
long           telldir(DIR *dirp);
void           seekdir(DIR *dirp, long loc);
int            dirfd(DIR *dirp);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_DIRENT_H */
