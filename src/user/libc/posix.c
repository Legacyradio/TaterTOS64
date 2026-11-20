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
#include <sys/select.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
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

struct _DIR {
    char path[FRY_PATH_MAX];
    unsigned char *buf;
    size_t len;
    size_t pos;
    struct dirent ent;
};

struct fry_readdir_ex_hdr {
    uint16_t rec_len;
    uint16_t name_len;
    uint32_t attr;
    uint64_t size;
};

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

/* fry1817: the positional I/O family, for real. All four were gap stubs
 * (gap_stubs.c:220-223) that CAST THE OFFSET AWAY and forwarded to the
 * sequential call:
 *     ssize_t pread(int fd, void *b, size_t c, off_t off)
 *     { (void)off; return read(fd,b,c); }
 * So every positional read or write landed at the file's CURRENT position
 * instead of the requested one, and returned a success count for it. That is
 * silent data corruption for any random-access file, and CPython is configured
 * to use these: pyconfig.h:982/985/1055/1058 define HAVE_PREAD, HAVE_PREADV,
 * HAVE_PWRITE and HAVE_PWRITEV. SQLite's unix VFS reads and writes its database
 * pages through exactly this family.
 * There is no SYS_PREAD/SYS_PWRITE in the native ABI (checked against
 * src/include/tatertos/syscall.h), so these are implemented the way a libc
 * without those syscalls does it: save the current offset, seek, transfer,
 * restore. The transfer's errno is preserved across the restoring seek so the
 * caller sees the real failure.
 * KNOWN LIMITATION, stated rather than hidden: seek+transfer+seek is NOT atomic
 * with respect to other threads sharing the same file description, which real
 * pread/pwrite are. Making it atomic requires SYS_PREAD/SYS_PWRITE in the
 * kernel — scoped as its own phase, not faked here. */
static ssize_t pio_prologue(int fd, off_t offset, off_t *saved_pos) {
    if (offset < 0) { errno = EINVAL; return -1; }
    *saved_pos = lseek(fd, 0, SEEK_CUR);
    if (*saved_pos == (off_t)-1) return -1;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;
    return 0;
}

static ssize_t pio_epilogue(int fd, off_t saved_pos, ssize_t n) {
    int saved_errno = errno;
    if (lseek(fd, saved_pos, SEEK_SET) == (off_t)-1) {
        /* The transfer may have succeeded, but the file position is now wrong
         * and the caller has no way to learn that. Report the failure. */
        return -1;
    }
    if (n < 0) errno = saved_errno;
    return n;
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    off_t pos;
    if (pio_prologue(fd, offset, &pos) < 0) return -1;
    return pio_epilogue(fd, pos, read(fd, buf, count));
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    off_t pos;
    if (pio_prologue(fd, offset, &pos) < 0) return -1;
    return pio_epilogue(fd, pos, write(fd, buf, count));
}

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    off_t pos;
    if (pio_prologue(fd, offset, &pos) < 0) return -1;
    return pio_epilogue(fd, pos, readv(fd, iov, iovcnt));
}

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    off_t pos;
    if (pio_prologue(fd, offset, &pos) < 0) return -1;
    return pio_epilogue(fd, pos, writev(fd, iov, iovcnt));
}

/* fry1815: real ftruncate()/truncate(). Both were gap stubs (gap_stubs.c:98 and
 * :105) that did nothing and returned 0 — they REPORTED SUCCESS, which is the
 * most dangerous shape a stub can have: every caller believed the file had been
 * resized. SQLite uses ftruncate to size the database and to truncate journals,
 * and /.hermes/state.db has sat at 0 bytes on the image since 2026-08-11
 * (logs/fry1812.txt FINDING 2, logs/fry1813.txt DEFECT 2).
 * Nothing new was needed underneath: the whole path already existed and was
 * real — SYS_FTRUNCATE = 89 (src/include/tatertos/syscall.h:143), the kernel
 * handler (src/kernel/proc/syscall.c:10770) covering FD_FILE via vfs_truncate()
 * and FD_MEMFD via memfd_truncate(), the VFS op (vfs.h:68) and the ToTFS
 * implementation totfs_truncate_vfs (wired at vfs.c:990/1016, committed in
 * cf5777f2). Only the POSIX wrapper was missing, so the syscall was never
 * issued. */
int ftruncate(int fd, off_t length) {
    if (length < 0) { errno = EINVAL; return -1; }
    return posix_error_int(fry_ftruncate(fd, (uint64_t)length));
}

/* There is no path-based truncate syscall in the native ABI, so this is done
 * the way a libc without one does it: open, ftruncate, close. The open errno is
 * preserved across close() so the caller sees the real failure. */
int truncate(const char *path, off_t length) {
    int fd, rc, saved;

    if (!path) { errno = EINVAL; return -1; }
    if (length < 0) { errno = EINVAL; return -1; }

    fd = open(path, O_WRONLY);
    if (fd < 0) return -1;

    rc = ftruncate(fd, length);
    saved = errno;
    close(fd);
    if (rc < 0) errno = saved;
    return rc;
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    return posix_error_int(fry_fcntl(fd, cmd, arg));
}

int ioctl(int fd, unsigned long request, ...) {
    /* fry1672: There is NO SYS_IOCTL in the native ABI. The previous wrapper
     * funnelled every ioctl() into fry_ioctl() -> raw syscall 11, which is
     * SYS_REBOOT -> calling ioctl() rebooted the machine (CPython's
     * ioctl(fd, FIOCLEX) during startup is what wedged `hermes`). Handle the
     * requests upstream code actually uses by translating to existing
     * mechanisms; never issue a raw ioctl syscall. */
    va_list ap;
    va_start(ap, request);
    void *argp = va_arg(ap, void *);
    va_end(ap);

    switch (request) {
    case FIOCLEX:   /* set close-on-exec */
        return fcntl(fd, F_SETFD, FD_CLOEXEC);
    case FIONCLEX:  /* clear close-on-exec */
        return fcntl(fd, F_SETFD, 0);
    case FIONBIO: { /* toggle non-blocking from *(int*)argp */
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl < 0) return -1;
        int on = argp ? *(int *)argp : 0;
        fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        return fcntl(fd, F_SETFL, fl);
    }
    case TIOCGWINSZ: { /* report a sane default size for tty fds */
        if (!isatty(fd) || !argp) { errno = ENOTTY; return -1; }
        struct winsize *ws = (struct winsize *)argp;
        ws->ws_row = 24; ws->ws_col = 80;
        ws->ws_xpixel = 0; ws->ws_ypixel = 0;
        return 0;
    }
    default:
        errno = ENOTTY;
        return -1;
    }
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    uint64_t t = (timeout < 0) ? (uint64_t)-1 : (uint64_t)timeout;
    return posix_error_int(fry_poll((struct fry_pollfd *)fds, (uint32_t)nfds, t));
}

/* fry1759: POSIX select() implemented over poll() (the kernel's poll-block
 * primitive). CPython's selectmodule.c calls select() unconditionally, and the
 * `selectors`/`asyncio` stack reaches it via select.select. Bits ready for more
 * than one set are counted once per set (glibc semantics). */
int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout) {
    if (nfds < 0 || nfds > FD_SETSIZE) { errno = EINVAL; return -1; }

    struct pollfd pfds[FD_SETSIZE];
    int slot[FD_SETSIZE];
    int npfds = 0;

    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (readfds   && FD_ISSET(fd, readfds))   ev |= POLLIN;
        if (writefds  && FD_ISSET(fd, writefds))  ev |= POLLOUT;
        if (exceptfds && FD_ISSET(fd, exceptfds)) ev |= POLLPRI;
        if (ev) {
            pfds[npfds].fd = fd;
            pfds[npfds].events = ev;
            pfds[npfds].revents = 0;
            slot[npfds] = fd;
            npfds++;
        }
    }

    int ms = -1;
    if (timeout)
        ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);

    int r = poll(pfds, (nfds_t)npfds, ms);
    if (r < 0) return -1;

    if (readfds)   FD_ZERO(readfds);
    if (writefds)  FD_ZERO(writefds);
    if (exceptfds) FD_ZERO(exceptfds);

    int count = 0;
    for (int i = 0; i < npfds; i++) {
        short re = pfds[i].revents;
        int fd = slot[i];
        if (readfds   && (re & (POLLIN  | POLLHUP | POLLERR))) { FD_SET(fd, readfds);   count++; }
        if (writefds  && (re & (POLLOUT | POLLERR)))           { FD_SET(fd, writefds);  count++; }
        if (exceptfds && (re &  POLLPRI))                      { FD_SET(fd, exceptfds); count++; }
    }
    return count;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask) {
    (void)sigmask; /* TaterTOS does not deliver signals during the poll-block */
    struct timeval tv;
    struct timeval *tvp = NULL;
    if (timeout) {
        tv.tv_sec  = timeout->tv_sec;
        tv.tv_usec = timeout->tv_nsec / 1000;
        tvp = &tv;
    }
    return select(nfds, readfds, writefds, exceptfds, tvp);
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
    /* fry1691: the kernel SYS_FSTAT only stats regular files (fd>=3) and returns
     * EBADF for the stdio fds 0/1/2. CPython's _io.FileIO treats EBADF on a std
     * fd as fatal and leaves sys.stdout/stderr/stdin = None -> print() is a
     * silent no-op and every traceback vanishes. The stdio fds are character
     * devices (console/GUI tty); report them as such so the streams initialize. */
    if (fd >= 0 && fd <= 2) {
        memset(st, 0, sizeof(*st));
        st->st_mode = (mode_t)(S_IFCHR | 0666);
        st->st_nlink = 1;
        st->st_blksize = 4096;
        return 0;
    }
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

/* fry1816: real getentropy(). This was a gap stub (gap_stubs.c:131) that
 * ZERO-FILLED the caller's buffer and returned 0 for success:
 *     int getentropy(void *b, size_t l)
 *     { if(b&&l){for(size_t i=0;i<l;i++)((unsigned char*)b)[i]=0;} return 0; }
 * external/Python-3.14.5/pyconfig.h:493 defines HAVE_GETENTROPY while line 592
 * leaves HAVE_GETRANDOM undefined, so CPython's entropy comes through THIS
 * function and nothing else. Every os.urandom(), secrets token, uuid4(), hash
 * seed and TLS random in the OS was therefore a run of zero bytes that the
 * caller believed was random.
 * A real entropy source already existed and was reachable: SYS_GETRANDOM = 85
 * (src/include/tatertos/syscall.h:137), the kernel handler
 * (src/kernel/proc/syscall.c:10658) and fry_getrandom() (libc.c:788). Only the
 * Rust path used it (rust_compat.c:89); the C path did not.
 * POSIX: getentropy() fills exactly len bytes or fails; len > 256 is EIO. It
 * returns 0 or -1, never a short count, so a partial fill is looped. */
int getentropy(void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;

    if (!buf && len) { errno = EFAULT; return -1; }
    if (len > 256) { errno = EIO; return -1; }

    while (got < len) {
        long rc = fry_getrandom(p + got, (unsigned long)(len - got), 0u);
        if (rc < 0) { errno = (int)(-rc); return -1; }
        if (rc == 0) { errno = EIO; return -1; }  /* no progress: do not spin */
        got += (size_t)rc;
    }
    return 0;
}

/* fry1818: real nanosleep()/clock_nanosleep(). nanosleep was a gap stub
 * (gap_stubs.c:247) that did nothing and FAILED:
 *     int nanosleep(const struct timespec *req, struct timespec *rem)
 *     { (void)req;(void)rem; errno=ENOSYS; return -1; }
 * and clock_nanosleep (gap_stubs.c:228) delegated straight to it, so both
 * always failed. Meanwhile SYS_NANOSLEEP = 87 exists
 * (src/include/tatertos/syscall.h:139), the kernel implements it
 * (src/kernel/proc/syscall.c, case 87: validates the request, converts to
 * milliseconds, sched_sleep + sched_yield, writes a zero remainder) and
 * fry_nanosleep() has been sitting in libc.c:796 unused by anything.
 * Everything that backs off, retries or waits a bounded time goes through this:
 * SQLite's busy handler, threading waits, retry loops with jitter — including
 * hermes_state.SessionDB's _WRITE_RETRY_MIN_S/_MAX_S jitter. A sleep primitive
 * that returns ENOSYS turns a polite retry loop into either a hard failure or
 * a hot spin, depending on how the caller handles it. */
int nanosleep(const struct timespec *req, struct timespec *rem) {
    struct fry_timespec freq, frem;
    long rc;

    if (!req) { errno = EFAULT; return -1; }
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }

    freq.tv_sec  = (int64_t)req->tv_sec;
    freq.tv_nsec = (int64_t)req->tv_nsec;
    frem.tv_sec  = 0;
    frem.tv_nsec = 0;

    rc = fry_nanosleep(&freq, &frem);
    if (rem) {
        rem->tv_sec  = (time_t)frem.tv_sec;
        rem->tv_nsec = (long)frem.tv_nsec;
    }
    if (rc < 0) { errno = (int)(-rc); return -1; }
    return 0;
}

/* clock_nanosleep() returns its error directly rather than via errno, per
 * POSIX. TIMER_ABSTIME is honoured by converting the absolute deadline into a
 * relative interval against the same clock; a deadline already in the past
 * returns success immediately, as POSIX requires. */
int clock_nanosleep(clockid_t clock_id, int flags,
                    const struct timespec *req, struct timespec *rem) {
    struct timespec rel;

    if (!req) return EFAULT;
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L)
        return EINVAL;

    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        if (clock_gettime(clock_id, &now) != 0) return errno;

        rel.tv_sec  = req->tv_sec  - now.tv_sec;
        rel.tv_nsec = req->tv_nsec - now.tv_nsec;
        if (rel.tv_nsec < 0) {
            rel.tv_nsec += 1000000000L;
            rel.tv_sec  -= 1;
        }
        /* Deadline already passed: nothing to wait for. POSIX also specifies
         * that rem is NOT written in the absolute case. */
        if (rel.tv_sec < 0) return 0;
        if (nanosleep(&rel, NULL) != 0) return errno;
        return 0;
    }

    if (nanosleep(req, rem) != 0) return errno;
    return 0;
}

int usleep(unsigned int usec) {
    fry_sleep(usec / 1000);
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    fry_sleep((uint64_t)seconds * 1000);
    return 0;
}

int pause(void) {
    /* POSIX pause(): suspend until a signal is delivered.
     * TaterTOS doesn't deliver async signals, so this blocks forever.
     * Used by CPython's PyThread_hang_thread() during finalization. */
    while (1) {
        fry_sleep(0);
    }
    return -1;  /* unreachable */
}

long sysconf(int name) {
    if (name == _SC_CLK_TCK) return 100;
    if (name == _SC_PAGESIZE) return 4096;
    if (name == _SC_NPROCESSORS_ONLN) return 1;
    /* fry1806: max iovec count for a single sendmsg/writev. 1024 matches Linux
     * IOV_MAX; asyncio uses it to cap how many buffers it batches per sendmsg
     * (selector_events.py). Reporting a real value rather than -1 keeps the
     * batching path enabled instead of silently degrading to send(). */
    if (name == _SC_IOV_MAX) return 1024;
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

DIR *opendir(const char *path) {
    if (!path || !*path) {
        errno = EINVAL;
        return NULL;
    }

    struct stat st;
    if (stat(path, &st) != 0)
        return NULL;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }

    struct _DIR *dir = (struct _DIR *)calloc(1, sizeof(*dir));
    if (!dir) {
        errno = ENOMEM;
        return NULL;
    }

    size_t cap = 128u * 1024u;
    dir->buf = (unsigned char *)malloc(cap);
    if (!dir->buf) {
        free(dir);
        errno = ENOMEM;
        return NULL;
    }

    strncpy(dir->path, path, sizeof(dir->path) - 1);
    long n = fry_readdir_ex(path, dir->buf, cap);
    if (n < 0) {
        int err = (int)-n;
        free(dir->buf);
        free(dir);
        errno = err;
        return NULL;
    }

    dir->len = (size_t)n;
    dir->pos = 0;
    return (DIR *)dir;
}

DIR *fdopendir(int fd) {
    (void)fd;
    errno = ENOSYS;
    return NULL;
}

struct dirent *readdir(DIR *dirp) {
    struct _DIR *dir = (struct _DIR *)dirp;
    if (!dir) {
        errno = EBADF;
        return NULL;
    }

    while (dir->pos + sizeof(struct fry_readdir_ex_hdr) <= dir->len) {
        struct fry_readdir_ex_hdr *h =
            (struct fry_readdir_ex_hdr *)(void *)(dir->buf + dir->pos);
        if (h->rec_len < sizeof(*h) || dir->pos + h->rec_len > dir->len) {
            errno = EIO;
            return NULL;
        }

        const char *name = (const char *)(h + 1);
        dir->pos += h->rec_len;
        if (h->name_len == 0)
            continue;

        memset(&dir->ent, 0, sizeof(dir->ent));
        dir->ent.d_ino = (ino_t)dir->pos;
        dir->ent.d_off = (off_t)dir->pos;
        dir->ent.d_reclen = (unsigned short)sizeof(dir->ent);
        dir->ent.d_type = (h->attr & 0x10u) ? DT_DIR : DT_REG;

        size_t copy_len = h->name_len;
        if (copy_len >= sizeof(dir->ent.d_name))
            copy_len = sizeof(dir->ent.d_name) - 1;
        memcpy(dir->ent.d_name, name, copy_len);
        dir->ent.d_name[copy_len] = '\0';
        return &dir->ent;
    }

    errno = 0;
    return NULL;
}

int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result) {
    if (!entry || !result)
        return EINVAL;
    struct dirent *next = readdir(dirp);
    if (!next) {
        *result = NULL;
        return errno;
    }
    memcpy(entry, next, sizeof(*entry));
    *result = entry;
    return 0;
}

int closedir(DIR *dirp) {
    struct _DIR *dir = (struct _DIR *)dirp;
    if (!dir) {
        errno = EBADF;
        return -1;
    }
    free(dir->buf);
    free(dir);
    return 0;
}

void rewinddir(DIR *dirp) {
    struct _DIR *dir = (struct _DIR *)dirp;
    if (dir)
        dir->pos = 0;
}

long telldir(DIR *dirp) {
    struct _DIR *dir = (struct _DIR *)dirp;
    if (!dir) {
        errno = EBADF;
        return -1;
    }
    return (long)dir->pos;
}

void seekdir(DIR *dirp, long loc) {
    struct _DIR *dir = (struct _DIR *)dirp;
    if (!dir || loc < 0 || (size_t)loc > dir->len)
        return;
    dir->pos = (size_t)loc;
}

int dirfd(DIR *dirp) {
    (void)dirp;
    errno = ENOSYS;
    return -1;
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
