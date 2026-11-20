/*
 * TaterTOS64v3 — <unistd.h>
 *
 * POSIX unistd.h surface for TaterTOS userland.
 */

#ifndef _TATERTOS_UNISTD_H
#define _TATERTOS_UNISTD_H

#include <stdint.h>
#include <sys/types.h>
#include <dirent.h>  /* DIR — for fdopendir */
#include <sys/uio.h>  /* struct iovec — for preadv/pwritev */
#include <stddef.h>
#include <time.h>
#include <sys/time.h>
#include <fry_types.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#define _SC_CLK_TCK             2
#define _SC_OPEN_MAX            11
#define _SC_PAGESIZE            30
#define _SC_PAGE_SIZE           _SC_PAGESIZE
#define _SC_NPROCESSORS_ONLN    84
#define _SC_PHYS_PAGES          85
#define _SC_AVPHYS_PAGES        86
#define _SC_NPROCESSORS_CONF    83
/* fry1806: asyncio needs this. selector_events.py does
 *     if _HAS_SENDMSG: try: SC_IOV_MAX = os.sysconf('SC_IOV_MAX')
 *                      except OSError: _HAS_SENDMSG = False
 * CPython builds os.sysconf_names from #ifdef _SC_* guards, so with this macro
 * undefined the NAME did not exist and os.sysconf() raised ValueError
 * ("unrecognized configuration name"). Upstream only catches OSError, so the
 * ValueError escaped and aborted the whole asyncio import — which then got
 * masked as a bogus "NameError: name 'base_events' is not defined" on retry
 * (fry1804). Value 60 is the Linux _SC_IOV_MAX. */
#define _SC_IOV_MAX             60
#define _SC_LINE_MAX           149
#define _SC_HOST_NAME_MAX      180

#ifndef SEEK_SET
#  define SEEK_SET  0
#  define SEEK_CUR  1
#  define SEEK_END  2
#endif

#define F_OK    0
#define X_OK    1
#define W_OK    2
#define R_OK    4

/* Standard POSIX declarations */
ssize_t pread(int fd, void *buf, size_t count, int64_t offset);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     pipe(int pipefd[2]);
int     pipe2(int pipefd[2], int flags);
int     dup3(int oldfd, int newfd, int flags);
pid_t   getpid(void);
pid_t   getppid(void);
uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
long    sysconf(int name);
int     getpagesize(void);
int     chdir(const char *path);
int     access(const char *path, int mode);
int     faccessat(int dirfd, const char *path, int mode, int flags);
int     unlink(const char *path);
int     unlinkat(int dirfd, const char *path, int flags);
int     rmdir(const char *path);
int     isatty(int fd);
int     usleep(unsigned int usec);
unsigned int sleep(unsigned int seconds);

ssize_t readlink(const char *path, char *buf, size_t bufsiz);
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz);
int     symlink(const char *target, const char *linkpath);
char    *getcwd(char *buf, size_t size);

int     ftruncate(int fd, off_t length);
int     truncate(const char *path, off_t length);

int     fsync(int fd);
int     fdatasync(int fd);

int     pause(void);

ssize_t splice(int fd_in, int64_t *off_in, int fd_out, int64_t *off_out, size_t len, unsigned int flags);
ssize_t tee(int fd_in, int fd_out, size_t len, unsigned int flags);

/* Forks/exec */
pid_t   vfork(void);
int     execv(const char *path, char *const argv[]);
int     execve(const char *path, char *const argv[], char *const envp[]);

/* Phase 2 gap matrix — wrappers + stubs for CPython posixmodule.c */

/* Wrappers (wrap existing syscalls) */
int     _exit(int status);
int     fchdir(int fd);
int     fchown(int fd, uid_t owner, gid_t group);
int     link(const char *oldpath, const char *newpath);
int     linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
int     sync(void);
int     chroot(const char *path);
int     chown(const char *path, uid_t owner, gid_t group);
int     lchown(const char *path, uid_t owner, gid_t group);
int     fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags);
int     fchmodat(int dirfd, const char *path, mode_t mode, int flags);
int     nice(int inc);
int     utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);
int     futimens(int fd, const struct timespec times[2]);
int futimes(int fd, const struct timeval times[2]);
int utimes(const char *path, const struct timeval times[2]);
int     lutimes(const char *path, const struct timeval times[2]);

/* Terminal names */
char   *ttyname(int fd);
int     ttyname_r(int fd, char *buf, size_t buflen);
char   *ctermid(char *s);
#define L_ctermid 9
#define _SC_TTY_NAME_MAX 132
#define _SC_GETPW_R_SIZE_MAX 1024

/* Directory by fd */
DIR    *fdopendir(int fd);

/* Process group stubs (single-user OS) */
pid_t   getpgrp(void);
pid_t   getpgid(pid_t pid);
int     setpgrp(void);
int     killpg(int pgrp, int sig);

/* Login stubs (single-user OS) */
int     getlogin_r(char *buf, size_t buflen);

/* Group stubs (single-user OS) */
int     getgroups(int size, gid_t list[]);
int     initgroups(const char *user, gid_t group);
int     getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups);

/* UID/GID setters (single-user OS — all no-ops returning 0) */
int     setuid(uid_t uid);
int     setgid(gid_t gid);
int     seteuid(uid_t euid);
int     setegid(gid_t egid);
int     setreuid(uid_t ruid, uid_t euid);
int     setregid(gid_t rgid, gid_t egid);
int     setgroups(size_t size, const gid_t *list);

/* Namespace stubs (TaterTOS has no namespaces) */
int     setns(int fd, int nstype);
int     unshare(int flags);

/* File operations */
int     symlinkat(const char *target, int newdirfd, const char *linkpath);

/* Session/terminal stubs (single-user OS) */
pid_t   getsid(pid_t pid);
pid_t   setsid(void);
int     setpgid(pid_t pid, pid_t pgrp);
pid_t   tcgetpgrp(int fd);
int     tcsetpgrp(int fd, pid_t pgrp);

/* File locking stub */
int     lockf(int fd, int op, off_t len);

/* posix_fallocate — stub (no-op, single user) */
int     posix_fallocate(int fd, off_t offset, off_t len);

/* Scatter/gather I/O (already have readv/writev) */
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);

/* Scatter/gather with offset */
ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);

/* Special file creation */
int     mkfifoat(int dirfd, const char *path, mode_t mode);
int     mknodat(int dirfd, const char *path, mode_t mode, dev_t dev);

/* File advice (stub) */
int     posix_fadvise(int fd, off_t offset, off_t len, int advice);

/* Path configuration (stubs) */
long    fpathconf(int fd, int name);
long    pathconf(const char *path, int name);
size_t  confstr(int name, char *buf, size_t len);

/* System info */
int     getloadavg(double loadavg[], int nelem);

/* Saved UID setters (single-user — no-ops) */
int     setresuid(uid_t ruid, uid_t euid, uid_t suid);
int     setresgid(gid_t rgid, gid_t egid, gid_t sgid);
int     getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
int     getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);

/* Hostname */
int     gethostname(char *name, size_t len);
int     sethostname(const char *name, size_t len);

/* Alarm */
unsigned int alarm(unsigned int seconds);

/* Signal string */
char   *strsignal(int sig);

#ifdef __cplusplus
}
#endif

#endif
/* getentropy (OpenBSD) — fallback to fry_random */
int getentropy(void *buffer, size_t length);
