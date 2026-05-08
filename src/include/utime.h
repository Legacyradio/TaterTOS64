/*
 * TaterTOS64v3 — <utime.h>
 *
 * POSIX file timestamps. TaterTOS does not currently track
 * per-file atime/mtime in TotFS, so utime() and utimes() are stubs.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_UTIME_H
#define _TATERTOS_UTIME_H

#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct utimbuf {
    time_t actime;
    time_t modtime;
};

int utime(const char *path, const struct utimbuf *times);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_UTIME_H */
