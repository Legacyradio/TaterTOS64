/*
 * TaterTOS64v3 — <sys/file.h>
 *
 * BSD/POSIX advisory file locking interface used by portable POSIX code.
 */

#ifndef _TATERTOS_SYS_FILE_H
#define _TATERTOS_SYS_FILE_H

#include <fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

int flock(int fd, int operation);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_FILE_H */
