/*
 * TaterTOS64v3 — <unistd.h>
 *
 * POSIX unistd.h surface for TaterTOS userland.
 */

#ifndef _TATERTOS_UNISTD_H
#define _TATERTOS_UNISTD_H

#include <stdint.h>
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

ssize_t splice(int fd_in, int64_t *off_in, int fd_out, int64_t *off_out, size_t len, unsigned int flags);
ssize_t tee(int fd_in, int fd_out, size_t len, unsigned int flags);

/* Forks/exec */
pid_t   vfork(void);
int     execv(const char *path, char *const argv[]);
int     execve(const char *path, char *const argv[], char *const envp[]);

#ifdef __cplusplus
}
#endif

#endif
