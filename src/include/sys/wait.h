/*
 * TaterTOS64v3 — <sys/wait.h>
 *
 * POSIX process wait. TaterTOS has fry_wait() in libc.h backed by
 * the kernel's process_wait machinery. This header exposes the
 * POSIX names plus the W* macros for status decoding.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_SYS_WAIT_H
#define _TATERTOS_SYS_WAIT_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/resource.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG    1
#define WUNTRACED  2
#define WSTOPPED   2
#define WEXITED    4
#define WCONTINUED 8
#define WNOWAIT    0x01000000

#define WEXITSTATUS(status)  (((status) & 0xff00) >> 8)
#define WTERMSIG(status)     ((status) & 0x7f)
#define WIFEXITED(status)    (WTERMSIG(status) == 0)
#define WIFSIGNALED(status)  (((status) & 0x7f) > 0 && (((status) & 0x7f) != 0x7f))
#define WIFSTOPPED(status)   (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status)     WEXITSTATUS(status)
#define WCOREDUMP(status)    ((status) & 0x80)

typedef enum {
    P_ALL  = 0,
    P_PID  = 1,
    P_PGID = 2,
} idtype_t;

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait3(int *status, int options, struct rusage *rusage);
pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage);
int   waitid(idtype_t idtype, id_t id, void *info, int options);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_WAIT_H */
