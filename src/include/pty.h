/*
 * TaterTOS64v3 — <pty.h>
 *
 * POSIX pseudo-terminal functions. CPython's posixmodule.c uses these
 * for os.openpty() and os.forkpty(). TaterTOS doesn't have PTY support,
 * so these are stubs that return ENOSYS.
 */
#ifndef _PTY_H
#define _PTY_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int  openpty(int *master, int *slave, char *name, void *termp, void *winp);
int  forkpty(int *master, char *name, void *termp, void *winp);

/* POSIX 2008 pseudo-terminal functions */
int  posix_openpt(int flags);
int  grantpt(int fd);
int  unlockpt(int fd);
char *ptsname(int fd);
int  ptsname_r(int fd, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* _PTY_H */
