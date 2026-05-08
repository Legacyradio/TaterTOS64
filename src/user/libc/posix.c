/*
 * posix.c — POSIX compatibility layer for TaterTOS userspace
 *
 * This file provides standard POSIX function symbols (open, socket, etc.)
 * by wrapping the underlying TaterTOS fry_ syscalls.
 */

#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

/* Public POSIX headers */
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <sys/memfd.h>
#include <sys/sendfile.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#include <sys/mlock.h>
#include <sched.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fry_limits.h>

/* Private TaterTOS ABI */
#include "libc.h"
#include "fry.h"

/* -----------------------------------------------------------------------
 * Error Handling Helper
 * ----------------------------------------------------------------------- */

static long posix_error(long rc) {
    if (rc >= 0) return rc;
    errno = (int)(-rc);
    return -1;
}

static int posix_error_int(long rc) {
    return (int)posix_error(rc);
}

static void posix_stat_from_fry(struct stat *st, const struct fry_stat *fst) {
    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)fst->size;
    st->st_mode = (mode_t)((fst->attr & 0x10u) ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    st->st_nlink = (fst->attr & 0x10u) ? 2 : 1;
    st->st_blksize = 4096;
    st->st_blocks = (blkcnt_t)((fst->size + 511u) / 512u);
}

/* -----------------------------------------------------------------------
 * POSIX Wrappers
 * ----------------------------------------------------------------------- */

int open(const char *path, int flags, ...) {
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        (void)va_arg(ap, mode_t);
        va_end(ap);
    }
    return posix_error_int(fry_open(path, flags));
}

int close(int fd) {
    return posix_error_int(fry_close(fd));
}

ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)posix_error(fry_read(fd, buf, count));
}

ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)posix_error(fry_write(fd, buf, count));
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || (!iov && iovcnt > 0)) {
        errno = EINVAL;
        return -1;
    }
    return (ssize_t)posix_error(fry_readv(fd, (const struct fry_iovec *)iov, iovcnt));
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || (!iov && iovcnt > 0)) {
        errno = EINVAL;
        return -1;
    }
    return (ssize_t)posix_error(fry_writev(fd, (const struct fry_iovec *)iov, iovcnt));
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)posix_error(fry_lseek(fd, (int64_t)offset, whence));
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    return posix_error_int(fry_fcntl(fd, cmd, arg));
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    long arg = va_arg(ap, long);
    va_end(ap);
    return posix_error_int(fry_ioctl(fd, (uint32_t)request, arg));
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    uint64_t t = (timeout < 0) ? (uint64_t)-1 : (uint64_t)timeout;
    return posix_error_int(fry_poll((struct fry_pollfd *)fds, (uint32_t)nfds, t));
}

int epoll_create(int size) {
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }
    return posix_error_int(fry_epoll_create(size));
}

int epoll_create1(int flags) {
    if (flags & ~EPOLL_CLOEXEC) {
        errno = EINVAL;
        return -1;
    }
    return posix_error_int(fry_epoll_create(1));
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    if ((op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) && !event) {
        errno = EINVAL;
        return -1;
    }
    return posix_error_int(fry_epoll_ctl(epfd, op, fd, event));
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    if (!events || maxevents <= 0) {
        errno = EINVAL;
        return -1;
    }
    return posix_error_int(fry_epoll_wait(epfd, events, maxevents, timeout));
}

int eventfd(unsigned int initval, int flags) {
    if (flags & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    return posix_error_int(fry_eventfd(initval, flags));
}

int eventfd_read(int fd, eventfd_t *value) {
    if (!value) {
        errno = EINVAL;
        return -1;
    }
    return (read(fd, value, sizeof(*value)) == (ssize_t)sizeof(*value)) ? 0 : -1;
}

int eventfd_write(int fd, eventfd_t value) {
    return (write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) ? 0 : -1;
}

int socket(int domain, int type, int protocol) {
    return posix_error_int(fry_socket(domain, type, protocol));
}

int bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    return posix_error_int(fry_bind(fd, (const struct fry_sockaddr_in *)addr, (uint32_t)addrlen));
}

int listen(int fd, int backlog) {
    return posix_error_int(fry_listen(fd, backlog));
}

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return posix_error_int(fry_accept(fd, (struct fry_sockaddr_in *)addr, (uint32_t *)addrlen));
}

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    return posix_error_int(fry_connect(fd, (const struct fry_sockaddr_in *)addr, (uint32_t)addrlen));
}

ssize_t send(int fd, const void *buf, size_t len, int flags) {
    return (ssize_t)posix_error(fry_send(fd, buf, len, flags));
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    return (ssize_t)posix_error(fry_recv(fd, buf, len, flags));
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    return (ssize_t)posix_error(fry_sendto(fd, buf, len, flags, (const struct fry_sockaddr_in *)dest_addr, (uint32_t)addrlen));
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    return (ssize_t)posix_error(fry_recvfrom(fd, buf, len, flags, (struct fry_sockaddr_in *)src_addr, (uint32_t *)addrlen));
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
    if (!msg) {
        errno = EINVAL;
        return -1;
    }
    return (ssize_t)posix_error(fry_sendmsg(fd, (const struct fry_msghdr *)msg, flags));
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
    if (!msg) {
        errno = EINVAL;
        return -1;
    }
    return (ssize_t)posix_error(fry_recvmsg(fd, (struct fry_msghdr *)msg, flags));
}

int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen) {
    return posix_error_int(fry_getsockopt(fd, level, optname, optval, (uint32_t *)optlen));
}

int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
    return posix_error_int(fry_setsockopt(fd, level, optname, optval, (uint32_t)optlen));
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return posix_error_int(fry_getsockname(fd, (struct fry_sockaddr_in *)addr, (uint32_t *)addrlen));
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return posix_error_int(fry_getpeername(fd, (struct fry_sockaddr_in *)addr, (uint32_t *)addrlen));
}

int stat(const char *path, struct stat *st) {
    struct fry_stat fst;
    long rc = fry_stat(path, &fst);
    if (rc < 0) return posix_error_int(rc);
    posix_stat_from_fry(st, &fst);
    return 0;
}

int fstat(int fd, struct stat *st) {
    struct fry_stat fst;
    long rc = fry_fstat(fd, &fst);
    if (rc < 0) return posix_error_int(rc);
    posix_stat_from_fry(st, &fst);
    return 0;
}

int lstat(const char *path, struct stat *st) {
    return stat(path, st);
}

int fstatat(int dirfd, const char *path, struct stat *statbuf, int flags) {
    struct fry_stat fst;
    long rc = fry_fstatat(dirfd, path, &fst, flags);
    if (rc < 0) return posix_error_int(rc);
    posix_stat_from_fry(statbuf, &fst);
    return 0;
}

int statvfs(const char *path, struct statvfs *buf) {
    if (!path || !buf) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_namemax = 255;
    if (st.st_size > 0) {
        fsblkcnt_t blocks = (fsblkcnt_t)((st.st_size + 4095) / 4096);
        buf->f_blocks = blocks;
        buf->f_bfree = blocks;
        buf->f_bavail = blocks;
    }
    return 0;
}

int fstatvfs(int fd, struct statvfs *buf) {
    if (!buf) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;

    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_namemax = 255;
    if (st.st_size > 0) {
        fsblkcnt_t blocks = (fsblkcnt_t)((st.st_size + 4095) / 4096);
        buf->f_blocks = blocks;
        buf->f_bfree = blocks;
        buf->f_bavail = blocks;
    }
    return 0;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    void *p = fry_mmap(addr, length, (uint32_t)prot, (uint32_t)flags, fd, (int64_t)offset);
    if (FRY_IS_ERR(p)) {
        errno = FRY_PTR_ERR(p);
        return MAP_FAILED;
    }
    return p;
}

int munmap(void *addr, size_t length) {
    return posix_error_int(fry_munmap(addr, length));
}

int mprotect(void *addr, size_t length, int prot) {
    return posix_error_int(fry_mprotect(addr, length, (uint32_t)prot));
}

int madvise(void *addr, size_t length, int advice) {
    return posix_error_int(fry_madvise(addr, length, advice));
}

int posix_madvise(void *addr, size_t length, int advice) {
    if (advice < MADV_NORMAL || advice > MADV_DONTNEED) return EINVAL;
    long rc = fry_madvise(addr, length, advice);
    return (rc < 0) ? (int)-rc : 0;
}

int prctl(int option, ...) {
    va_list ap;
    unsigned long arg2 = 0;
    unsigned long arg3 = 0;
    unsigned long arg4 = 0;
    unsigned long arg5 = 0;

    va_start(ap, option);
    switch (option) {
        case PR_SET_NAME:
        case PR_GET_NAME:
        case PR_SET_DUMPABLE:
        case PR_SET_NO_NEW_PRIVS:
        case PR_SET_TIMERSLACK:
        case PR_SET_THP_DISABLE:
        case PR_SET_PTRACER:
        case PR_SET_PDEATHSIG:
        case PR_GET_PDEATHSIG:
            arg2 = va_arg(ap, unsigned long);
            break;
        case PR_SET_VMA:
            arg2 = va_arg(ap, unsigned long);
            arg3 = va_arg(ap, unsigned long);
            arg4 = va_arg(ap, unsigned long);
            arg5 = va_arg(ap, unsigned long);
            break;
        default:
            break;
    }
    va_end(ap);

    return posix_error_int(fry_prctl(option, arg2, arg3, arg4, arg5));
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    return posix_error_int(fry_sigaction(sig, (const struct fry_sigaction *)act, (struct fry_sigaction *)oldact));
}

int kill(pid_t pid, int sig) {
    return posix_error_int(fry_kill((int)pid, sig));
}

int mkdir(const char *path, mode_t mode) {
    return mkdirat(AT_FDCWD, path, mode);
}

int mkdirat(int dirfd, const char *path, mode_t mode) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    return posix_error_int(fry_mkdirat(dirfd, path, (uint32_t)mode));
}

int rename(const char *oldpath, const char *newpath) {
    return renameat(AT_FDCWD, oldpath, AT_FDCWD, newpath);
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    if (!oldpath || !newpath) {
        errno = EINVAL;
        return -1;
    }
    return posix_error_int(fry_renameat(olddirfd, oldpath, newdirfd, newpath));
}

int unlink(const char *path) {
    return unlinkat(AT_FDCWD, path, 0);
}

int unlinkat(int dirfd, const char *path, int flags) {
    if (!path || (flags & ~AT_REMOVEDIR)) {
        errno = EINVAL;
        return -1;
    }
    return posix_error_int(fry_unlinkat(dirfd, path, flags));
}

int rmdir(const char *path) {
    return unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
}

int access(const char *path, int mode) {
    return faccessat(AT_FDCWD, path, mode, 0);
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    if (!path || (mode & ~(R_OK | W_OK | X_OK))) {
        errno = EINVAL;
        return -1;
    }
    return posix_error_int(fry_faccessat(dirfd, path, mode, flags));
}

int isatty(int fd) {
    return (fd >= 0 && fd <= 2);
}

int usleep(unsigned int usec) {
    fry_sleep(usec / 1000);
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    fry_sleep((uint64_t)seconds * 1000);
    return 0;
}

long sysconf(int name) {
    if (name == 30) return 4096; /* _SC_PAGESIZE */
    if (name == 84) return 1;    /* _SC_NPROCESSORS_ONLN */
    return -1;
}

int dup(int oldfd) {
    return posix_error_int(fry_dup(oldfd));
}

int dup2(int oldfd, int newfd) {
    return posix_error_int(fry_dup2(oldfd, newfd));
}

int pipe(int pipefd[2]) {
    return posix_error_int(fry_pipe(pipefd));
}

int pipe2(int pipefd[2], int flags) {
    return posix_error_int(fry_pipe2(pipefd, flags));
}

int dup3(int oldfd, int newfd, int flags) {
    return posix_error_int(fry_dup3(oldfd, newfd, flags));
}

int accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    return posix_error_int(fry_accept4(fd, addr, addrlen, flags));
}

int timerfd_create(int clockid, int flags) {
    return posix_error_int(fry_timerfd_create(clockid, flags));
}

int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value) {
    return posix_error_int(fry_timerfd_settime(fd, flags, new_value, old_value));
}

int timerfd_gettime(int fd, struct itimerspec *curr_value) {
    return posix_error_int(fry_timerfd_gettime(fd, curr_value));
}

int signalfd(int fd, const sigset_t *mask, int flags) {
    return posix_error_int(fry_signalfd(fd, (const uint64_t *)mask, flags));
}

int inotify_init(void) {
    return posix_error_int(fry_inotify_init(0));
}

int inotify_init1(int flags) {
    return posix_error_int(fry_inotify_init(flags));
}

int inotify_add_watch(int fd, const char *pathname, uint32_t mask) {
    return posix_error_int(fry_inotify_add_watch(fd, pathname, mask));
}

int inotify_rm_watch(int fd, int wd) {
    return posix_error_int(fry_inotify_rm_watch(fd, wd));
}

int memfd_create(const char *name, unsigned int flags) {
    return posix_error_int(fry_memfd_create(name, flags));
}

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
    return (ssize_t)posix_error(fry_sendfile(out_fd, in_fd, offset, count));
}

pid_t getpid(void) {
    return (pid_t)fry_getpid();
}

char *getcwd(char *buf, size_t size) {
    long ret = fry_getcwd(buf, size);
    if (ret < 0) { errno = (int)-ret; return NULL; }
    return buf;
}

int chdir(const char *path) {
    return posix_error_int(fry_chdir(path));
}

char *realpath(const char *path, char *resolved_path) {
    if (!path || !*path) {
        errno = EINVAL;
        return NULL;
    }

    char scratch[FRY_PATH_MAX];
    if (path[0] == '/') {
        strncpy(scratch, path, sizeof(scratch) - 1);
        scratch[sizeof(scratch) - 1] = '\0';
    } else {
        char cwd[FRY_PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)))
            return NULL;
        int written = snprintf(scratch, sizeof(scratch), "%s/%s", cwd, path);
        if (written < 0 || (size_t)written >= sizeof(scratch)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
    }

    char normalized[FRY_PATH_MAX];
    size_t out = 0;
    normalized[out++] = '/';

    char *cursor = scratch;
    while (*cursor == '/')
        ++cursor;

    while (*cursor) {
        char *segment = cursor;
        while (*cursor && *cursor != '/')
            ++cursor;
        size_t len = (size_t)(cursor - segment);

        if (len == 1 && segment[0] == '.') {
            /* Skip current-directory segments. */
        } else if (len == 2 && segment[0] == '.' && segment[1] == '.') {
            if (out > 1) {
                --out;
                while (out > 1 && normalized[out - 1] != '/')
                    --out;
            }
        } else if (len > 0) {
            if (out > 1) {
                if (out + 1 >= sizeof(normalized)) {
                    errno = ENAMETOOLONG;
                    return NULL;
                }
                normalized[out++] = '/';
            }
            if (out + len >= sizeof(normalized)) {
                errno = ENAMETOOLONG;
                return NULL;
            }
            memcpy(&normalized[out], segment, len);
            out += len;
        }

        while (*cursor == '/')
            ++cursor;
    }

    normalized[out] = '\0';

    struct stat st;
    if (stat(normalized, &st) != 0)
        return NULL;

    char *result = resolved_path ? resolved_path : (char *)malloc(out + 1);
    if (!result) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(result, normalized, out + 1);
    return result;
}

int shutdown(int fd, int how) {
    return posix_error_int(fry_shutdown_sock(fd, how));
}

void (*signal(int sig, void (*func)(int)))(int) {
    (void)sig; (void)func;
    return 0;
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        (void)va_arg(ap, mode_t);
        va_end(ap);
    }
    return posix_error_int(fry_openat(dirfd, pathname, flags));
}

int waitpid(pid_t pid, int *status, int options) {
    (void)pid; (void)status; (void)options;
    return -1;
}

int chmod(const char *path, mode_t mode) {
    (void)path; (void)mode;
    return 0;
}

int fchmod(int fd, mode_t mode) {
    (void)fd; (void)mode;
    return 0;
}

int fchown(int fd, uid_t owner, gid_t group) {
    (void)fd; (void)owner; (void)group;
    return 0;
}

int lchown(const char *path, uid_t owner, gid_t group) {
    (void)path; (void)owner; (void)group;
    return 0;
}

int link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -1;
}

int symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath;
    return -1;
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    return readlinkat(AT_FDCWD, pathname, buf, bufsiz);
}

ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    if (!pathname || !buf || bufsiz == 0) {
        errno = EINVAL;
        return -1;
    }
    return (ssize_t)posix_error(fry_readlinkat(dirfd, pathname, buf, bufsiz));
}

int mkstemp(char *template) {
    (void)template;
    return -1;
}

/* strtold stub */
long double strtold(const char *nptr, char **endptr) {
    (void)nptr; (void)endptr; return 0.0;
}

int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags) {
    (void)dirfd; (void)pathname; (void)times; (void)flags;
    return 0;
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    return posix_error_int(fry_socketpair(domain, type, protocol, sv));
}

int getrlimit(int resource, struct rlimit *rlim) {
    (void)resource; (void)rlim;
    return -1;
}

int setrlimit(int resource, const struct rlimit *rlim) {
    (void)resource; (void)rlim;
    return -1;
}

/* -----------------------------------------------------------------------
 * Chrome/GN probe wrappers (Phase 10)
 * ----------------------------------------------------------------------- */
int uname(struct utsname *buf) {
    return posix_error_int(fry_uname(buf));
}
int sysinfo(struct sysinfo *info) {
    return posix_error_int(fry_sysinfo(info));
}
int getrusage(int who, struct rusage *usage) {
    return posix_error_int(fry_getrusage(who, usage));
}
int getpriority(int which, id_t who) {
    long rc = fry_getpriority(which, (int)who);
    if (rc < 0) { errno = (int)(-rc); return -1; }
    return (int)rc;
}
int setpriority(int which, id_t who, int prio) {
    return posix_error_int(fry_setpriority(which, (int)who, prio));
}
int fsync(int fd) {
    return posix_error_int(fry_fsync(fd));
}
int fdatasync(int fd) {
    return posix_error_int(fry_fdatasync(fd));
}
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask) {
    return posix_error_int(fry_sched_getaffinity((int)pid, cpusetsize, mask));
}
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
    return posix_error_int(fry_sched_setaffinity((int)pid, cpusetsize, (void*)mask));
}
int mlock(const void *addr, size_t len) {
    return posix_error_int(fry_mlock(addr, len));
}
int munlock(const void *addr, size_t len) {
    return posix_error_int(fry_munlock(addr, len));
}
int mlockall(int flags) {
    (void)flags; errno = ENOSYS; return -1;
}
int munlockall(void) {
    errno = ENOSYS; return -1;
}
ssize_t splice(int fd_in, int64_t *off_in, int fd_out, int64_t *off_out, size_t len, unsigned int flags) {
    return (ssize_t)posix_error(fry_splice(fd_in, off_in, fd_out, off_out, len, flags));
}
ssize_t tee(int fd_in, int fd_out, size_t len, unsigned int flags) {
    return (ssize_t)posix_error(fry_tee(fd_in, fd_out, len, flags));
}

int posix_spawn(pid_t *pid, const char *path, const void *file_actions, const void *attrp, char *const argv[], char *const envp[]) {
    (void)pid; (void)path; (void)file_actions; (void)attrp; (void)argv; (void)envp;
    return -1;
}

int posix_spawnp(pid_t *pid, const char *file, const void *file_actions, const void *attrp, char *const argv[], char *const envp[]) {
    (void)pid; (void)file; (void)file_actions; (void)attrp; (void)argv; (void)envp;
    return -1;
}

int tcgetattr(int fd, void *termios_p) {
    (void)fd; (void)termios_p;
    return -1;
}

int tcsetattr(int fd, int optional_actions, const void *termios_p) {
    (void)fd; (void)optional_actions; (void)termios_p;
    return -1;
}

/* -----------------------------------------------------------------------
 * Compat symbols needed by existing binary-only apps (linked against libc.o)
 * ----------------------------------------------------------------------- */

int close_compat(int fd) { return close(fd); }
ssize_t read_compat(int fd, void *buf, size_t count) { return read(fd, buf, count); }
ssize_t write_compat(int fd, const void *buf, size_t count) { return write(fd, buf, count); }
off_t lseek_compat(int fd, off_t offset, int whence) { return lseek(fd, offset, whence); }
int dup_compat(int oldfd) { return dup(oldfd); }
int dup2_compat(int oldfd, int newfd) { return dup2(oldfd, newfd); }
int pipe_compat(int pipefd[2]) { return pipe(pipefd); }
int usleep_compat(unsigned int usec) { return usleep(usec); }
int getpid_compat(void) { return (int)getpid(); }

int symlink_compat(const char *target, const char *linkpath) { return symlink(target, linkpath); }
ssize_t readlink_compat(const char *path, char *buf, size_t sz) { return readlink(path, buf, sz); }
long sysconf_compat(int name) { return sysconf(name); }

char *getcwd_compat(char *buf, size_t size) {
    return getcwd(buf, size);
}
