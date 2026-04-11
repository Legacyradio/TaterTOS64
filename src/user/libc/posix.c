/*
 * posix.c — POSIX compatibility shims for NSPR/NSS porting
 *
 * Phase 8: Signal stubs, directory streams, environment helpers,
 * process control, file access checks, and miscellaneous POSIX
 * functions that browsers and their dependencies expect.
 *
 * All implementations are original TaterTOS code.
 */

#include "libc.h"
#include <stdint.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned long compat_rlim_t;

#define TATER_TERMIOS_NCCS 32

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[TATER_TERMIOS_NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

enum {
    TATER_VINTR = 0,
    TATER_VQUIT = 1,
    TATER_VERASE = 2,
    TATER_VKILL = 3,
    TATER_VEOF = 4,
    TATER_VTIME = 5,
    TATER_VMIN = 6
};

#define TATER_ECHO   0x0008u
#define TATER_ICANON 0x0002u
#define TATER_WNOHANG 1
#define TATER_EXEC_PATH_MAX 512

#define RLIM_INFINITY (~(compat_rlim_t)0)

#define RLIMIT_STACK  3
#define RLIMIT_NOFILE 7

#define RUSAGE_SELF      0
#define RUSAGE_CHILDREN -1
#define RUSAGE_THREAD    1

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct rlimit {
    compat_rlim_t rlim_cur;
    compat_rlim_t rlim_max;
};

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

/* -----------------------------------------------------------------------
 * errno — thread-local errno
 * We use a TLS slot for per-thread errno. Falls back to a global if
 * TLS is not yet initialized.
 * ----------------------------------------------------------------------- */

static int g_errno_fallback = 0;
static fry_tls_key_t g_errno_key;
static int g_errno_key_inited = 0;

static int compat_fail_errno(int err) {
    *__errno_location() = err;
    return -1;
}

static int compat_fail_sys(long rc) {
    if (rc >= 0) return 0;
    return compat_fail_errno((int)(-rc));
}

static uint32_t compat_count_vec(char *const vec[], uint32_t max_items) {
    uint32_t count = 0;

    if (!vec) return 0;
    while (count < max_items && vec[count]) count++;
    return count;
}

static void compat_fill_rlimit(int resource, struct rlimit *rlim) {
    if (!rlim) return;

    rlim->rlim_cur = RLIM_INFINITY;
    rlim->rlim_max = RLIM_INFINITY;

    switch (resource) {
    case RLIMIT_NOFILE:
        rlim->rlim_cur = 256;
        rlim->rlim_max = 256;
        break;
    case RLIMIT_STACK:
        rlim->rlim_cur = 8u * 1024u * 1024u;
        rlim->rlim_max = 8u * 1024u * 1024u;
        break;
    default:
        break;
    }
}

static void compat_fill_rusage(struct rusage *usage) {
    if (!usage) return;
    memset(usage, 0, sizeof(*usage));
}

static void errno_init_key(void) {
    if (!g_errno_key_inited) {
        if (fry_tls_key_create(&g_errno_key) == 0) {
            g_errno_key_inited = 1;
        }
    }
}

int *__errno_location(void) {
    errno_init_key();
    if (g_errno_key_inited) {
        int *p = (int *)fry_tls_get(g_errno_key);
        if (!p) {
            p = (int *)malloc(sizeof(int));
            if (p) {
                *p = 0;
                fry_tls_set(g_errno_key, p);
                return p;
            }
        }
        if (p) return p;
    }
    return &g_errno_fallback;
}

/* -----------------------------------------------------------------------
 * Signal stubs — NSPR uses signal() and sigaction() for SIGPIPE etc.
 * TaterTOS doesn't have signals yet, so we stub them safely.
 * ----------------------------------------------------------------------- */

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIGPIPE   13
#define SIGCHLD   17
#define SIGALRM   14
#define SIGUSR1   10
#define SIGUSR2   12
#define SIGHUP     1
#define SIGINT     2
#define SIGTERM   15
#define NSIG      32

static sighandler_t g_signal_handlers[NSIG];

sighandler_t signal(int sig, sighandler_t handler) {
    if (sig < 0 || sig >= NSIG) return SIG_ERR;
    sighandler_t old = g_signal_handlers[sig];
    g_signal_handlers[sig] = handler;
    return old;
}

/* Minimal sigset_t and sigaction for compilation */
typedef uint32_t sigset_t_compat;

struct sigaction_compat {
    sighandler_t sa_handler;
    uint32_t     sa_flags;
    sigset_t_compat sa_mask;
};

int sigaction_compat(int sig, const struct sigaction_compat *act,
                     struct sigaction_compat *oldact) {
    if (sig < 0 || sig >= NSIG) return -1;
    if (oldact) {
        oldact->sa_handler = g_signal_handlers[sig];
        oldact->sa_flags = 0;
        oldact->sa_mask = 0;
    }
    if (act) {
        g_signal_handlers[sig] = act->sa_handler;
    }
    return 0;
}

int sigemptyset_compat(sigset_t_compat *set) { if (set) *set = 0; return 0; }
int sigfillset_compat(sigset_t_compat *set) { if (set) *set = 0xFFFFFFFF; return 0; }
int sigaddset_compat(sigset_t_compat *set, int sig) {
    if (!set || sig < 0 || sig >= NSIG) return -1;
    *set |= (1u << sig);
    return 0;
}
int sigdelset_compat(sigset_t_compat *set, int sig) {
    if (!set || sig < 0 || sig >= NSIG) return -1;
    *set &= ~(1u << sig);
    return 0;
}
int sigismember_compat(const sigset_t_compat *set, int sig) {
    if (!set || sig < 0 || sig >= NSIG) return 0;
    return (*set & (1u << sig)) ? 1 : 0;
}
int sigprocmask_compat(int how, const sigset_t_compat *set, sigset_t_compat *oldset) {
    (void)how; (void)set;
    if (oldset) *oldset = 0;
    return 0;
}

int raise_compat(int sig) {
    if (sig < 0 || sig >= NSIG) return -1;
    sighandler_t h = g_signal_handlers[sig];
    if (h && h != SIG_DFL && h != SIG_IGN) h(sig);
    return 0;
}

int kill_compat(int pid, int sig) {
    (void)pid; (void)sig;
    return 0; /* stub */
}

/* -----------------------------------------------------------------------
 * Directory streams — opendir / readdir / closedir
 * ----------------------------------------------------------------------- */

#define DIR_BUF_SIZE 4096

struct _DIR {
    char   path[256];
    uint8_t buf[DIR_BUF_SIZE];
    size_t  buf_len;
    size_t  buf_pos;
    int     done;
};

DIR *opendir(const char *path) {
    if (!path) return 0;

    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    if (!d) return 0;

    strncpy(d->path, path, sizeof(d->path) - 1);

    /* Read directory entries into buffer */
    long rc = fry_readdir_ex(path, d->buf, DIR_BUF_SIZE);
    if (rc < 0) {
        free(d);
        return 0;
    }
    d->buf_len = (size_t)rc;
    d->buf_pos = 0;
    d->done = 0;
    return d;
}

static struct dirent_compat g_dirent_result;

struct dirent_compat *readdir_compat(DIR *dirp) {
    if (!dirp || dirp->done) return 0;

    while (dirp->buf_pos < dirp->buf_len) {
        struct fry_dirent *fde = (struct fry_dirent *)(dirp->buf + dirp->buf_pos);
        if (fde->rec_len == 0) { dirp->done = 1; return 0; }

        dirp->buf_pos += fde->rec_len;

        /* Convert to dirent_compat */
        g_dirent_result.d_ino = 1; /* fake inode */
        g_dirent_result.d_type = (fde->attr & 0x10) ? DT_DIR : DT_REG;

        size_t namelen = fde->name_len;
        if (namelen > 255) namelen = 255;
        memcpy(g_dirent_result.d_name, fde->name, namelen);
        g_dirent_result.d_name[namelen] = '\0';

        return &g_dirent_result;
    }

    dirp->done = 1;
    return 0;
}

int closedir(DIR *dirp) {
    if (!dirp) return -1;
    free(dirp);
    return 0;
}

/* -----------------------------------------------------------------------
 * File system helpers
 * ----------------------------------------------------------------------- */

char *getcwd(char *buf, size_t size) {
    /* TaterTOS has no per-process cwd yet; return "/" */
    if (!buf) {
        buf = (char *)malloc(size > 0 ? size : 2);
        if (!buf) return 0;
    }
    if (size < 2) return 0;
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}

int chdir(const char *path) {
    /* No per-process cwd support yet — always succeeds for "/" */
    if (!path) return -1;
    if (path[0] == '/' && path[1] == '\0') return 0;
    *__errno_location() = ENOSYS;
    return -1;
}

int access(const char *path, int mode) {
    (void)mode;
    if (!path) return -1;
    struct fry_stat st;
    long rc = fry_stat(path, &st);
    return (rc < 0) ? -1 : 0;
}

int unlink(const char *path) {
    return (fry_unlink(path) < 0) ? -1 : 0;
}

int rmdir(const char *path) {
    return (fry_unlink(path) < 0) ? -1 : 0;
}

int mkdir_compat(const char *path, uint32_t mode) {
    (void)mode;
    return (fry_mkdir(path) < 0) ? -1 : 0;
}

int stat_compat(const char *path, struct fry_stat *st) {
    return (fry_stat(path, st) < 0) ? -1 : 0;
}

int fstat_compat(int fd, struct fry_stat *st) {
    return (fry_fstat(fd, st) < 0) ? -1 : 0;
}

int lstat_compat(const char *path, struct fry_stat *st) {
    /* No symlinks in TaterTOS — same as stat */
    return stat_compat(path, st);
}

/* -----------------------------------------------------------------------
 * Process control
 * ----------------------------------------------------------------------- */

int getpid_compat(void) {
    return (int)fry_getpid();
}

int getppid_compat(void) {
    return 1; /* init is always parent for now */
}

int getuid_compat(void) { return 0; }
int geteuid_compat(void) { return 0; }
int getgid_compat(void) { return 0; }
int getegid_compat(void) { return 0; }

pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    const char *default_argv[2];
    const char **spawn_argv = (const char **)argv;
    const char **spawn_envp = (const char **)envp;
    uint32_t argc;
    uint32_t envc;
    long child_pid;

    if (!path || !path[0]) {
        errno = ENOENT;
        return -1;
    }

    if (!spawn_argv || !spawn_argv[0]) {
        default_argv[0] = path;
        default_argv[1] = 0;
        spawn_argv = default_argv;
    }

    argc = compat_count_vec((char *const *)spawn_argv, FRY_ARGV_MAX);
    envc = compat_count_vec((char *const *)spawn_envp, FRY_ENV_MAX);
    child_pid = fry_spawn_args(path, spawn_argv, argc, spawn_envp, envc);
    if (child_pid < 0) return compat_fail_sys(child_pid);

    /*
     * TaterTOS does not have a true exec-replace syscall yet. Best-effort
     * compatibility: run the requested image as a child, wait for it to
     * finish, then terminate the current process so exec* never returns
     * on success.
     */
    if (fry_wait((uint32_t)child_pid) < 0) {
        fry_exit(127);
        for (;;) {}
    }
    fry_exit(0);
    for (;;) {}
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, 0);
}

int execvp(const char *file, char *const argv[]) {
    const char *path_env;
    const char *cursor;
    struct fry_stat st;
    char candidate[TATER_EXEC_PATH_MAX];

    if (!file || !file[0]) {
        errno = ENOENT;
        return -1;
    }

    if (strchr(file, '/')) return execve(file, argv, 0);

    path_env = getenv_compat("PATH");
    if (!path_env || !path_env[0]) path_env = "/bin:/usr/bin:/apps:/system/bin";

    cursor = path_env;
    while (*cursor) {
        const char *entry = cursor;
        size_t entry_len = 0;
        size_t file_len = strlen(file);

        while (cursor[entry_len] && cursor[entry_len] != ':') entry_len++;

        if (entry_len == 0) {
            candidate[0] = '.';
            candidate[1] = '/';
            if (file_len + 3 > sizeof(candidate)) return compat_fail_errno(ENOENT);
            memcpy(candidate + 2, file, file_len + 1);
        } else {
            if (entry_len + 1 + file_len + 1 > sizeof(candidate)) {
                cursor += entry_len;
                if (*cursor == ':') cursor++;
                continue;
            }
            memcpy(candidate, entry, entry_len);
            candidate[entry_len] = '/';
            memcpy(candidate + entry_len + 1, file, file_len + 1);
        }

        if (fry_stat(candidate, &st) >= 0) return execve(candidate, argv, 0);

        cursor += entry_len;
        if (*cursor == ':') cursor++;
    }

    errno = ENOENT;
    return -1;
}

pid_t waitpid(pid_t pid, int *status, int options) {
    long rc;

    if (pid <= 0) {
        errno = ECHILD;
        return -1;
    }
    if (options & ~TATER_WNOHANG) {
        errno = EINVAL;
        return -1;
    }
    if (options & TATER_WNOHANG) {
        errno = ENOSYS;
        return -1;
    }

    rc = fry_wait((uint32_t)pid);
    if (rc < 0) return compat_fail_sys(rc);
    if (status) *status = 0;
    return pid;
}

pid_t wait(int *status) {
    if (status) *status = 0;
    errno = ENOSYS;
    return -1;
}

__attribute__((noreturn))
void _exit_compat(int status) {
    fry_exit(status);
    for (;;) {}
}

__attribute__((noreturn))
void abort_compat(void) {
    printf("abort() called\n");
    fry_exit(134);
    for (;;) {}
}

/* atexit — up to 32 handlers */
#define ATEXIT_MAX 32
static void (*g_atexit_fns[ATEXIT_MAX])(void);
static int g_atexit_count = 0;

int atexit_compat(void (*func)(void)) {
    if (!func || g_atexit_count >= ATEXIT_MAX) return -1;
    g_atexit_fns[g_atexit_count++] = func;
    return 0;
}

void exit_compat(int status) {
    /* Run atexit handlers in reverse order */
    for (int i = g_atexit_count - 1; i >= 0; i--) {
        if (g_atexit_fns[i]) g_atexit_fns[i]();
    }
    fry_exit(status);
    for (;;) {}
}

/* -----------------------------------------------------------------------
 * Environment helpers — backed by fry_getenv syscall
 * ----------------------------------------------------------------------- */

char *getenv_compat(const char *name) {
    static char env_buf[256];
    if (!name) return 0;
    long rc = fry_getenv(name, env_buf, sizeof(env_buf));
    if (rc < 0) return 0;
    return env_buf;
}

int setenv_compat(const char *name, const char *value, int overwrite) {
    (void)name; (void)value; (void)overwrite;
    return 0; /* stub — no persistent env yet */
}

int unsetenv_compat(const char *name) {
    (void)name;
    return 0; /* stub */
}

int putenv_compat(char *string) {
    (void)string;
    return 0; /* stub */
}

/* -----------------------------------------------------------------------
 * Miscellaneous POSIX
 * ----------------------------------------------------------------------- */

unsigned int sleep_compat(unsigned int seconds) {
    fry_sleep((uint64_t)seconds * 1000);
    return 0;
}

int usleep_compat(unsigned int usec) {
    uint64_t ms = usec / 1000;
    if (ms == 0 && usec > 0) ms = 1;
    fry_sleep(ms);
    return 0;
}

/* sysconf — return sensible defaults for commonly queried values */
long sysconf_compat(int name) {
    switch (name) {
    case 30: /* _SC_PAGESIZE / _SC_PAGE_SIZE */
        return 4096;
    case 84: /* _SC_NPROCESSORS_ONLN */
        return 1;
    case 11: /* _SC_OPEN_MAX */
        return 64;
    case  2: /* _SC_CLK_TCK */
        return 1000;
    default:
        return -1;
    }
}

/* getpagesize */
int getpagesize_compat(void) {
    return 4096;
}

/* pipe — wrapper */
int pipe_compat(int pipefd[2]) {
    return (fry_pipe(pipefd) < 0) ? -1 : 0;
}

/* dup / dup2 wrappers */
int dup_compat(int oldfd) {
    long rc = fry_dup(oldfd);
    return (rc < 0) ? -1 : (int)rc;
}

int dup2_compat(int oldfd, int newfd) {
    long rc = fry_dup2(oldfd, newfd);
    return (rc < 0) ? -1 : (int)rc;
}

/* close / read / write POSIX-style */
int close_compat(int fd) {
    return (fry_close(fd) < 0) ? -1 : 0;
}

long read_compat(int fd, void *buf, size_t count) {
    return fry_read(fd, buf, count);
}

long write_compat(int fd, const void *buf, size_t count) {
    return fry_write(fd, buf, count);
}

long lseek_compat(int fd, long offset, int whence) {
    return fry_lseek(fd, (int64_t)offset, whence);
}

int open_compat(const char *path, int flags) {
    long rc = fry_open(path, flags);
    return (rc < 0) ? -1 : (int)rc;
}

/* fcntl wrapper */
int fcntl_compat(int fd, int cmd, long arg) {
    return (int)fry_fcntl(fd, cmd, arg);
}

/* mmap / munmap / mprotect POSIX-style wrappers */
void *mmap_compat(void *addr, size_t length, int prot, int flags, int fd, long offset) {
    (void)offset; /* TaterTOS mmap doesn't support offset yet */
    if (fd >= 0) {
        return fry_mmap_fd(addr, length, (uint32_t)prot, (uint32_t)flags, fd);
    }
    return fry_mmap(addr, length, (uint32_t)prot, (uint32_t)flags);
}

int munmap_compat(void *addr, size_t length) {
    return (fry_munmap(addr, length) < 0) ? -1 : 0;
}

int mprotect_compat(void *addr, size_t length, int prot) {
    return (fry_mprotect(addr, length, (uint32_t)prot) < 0) ? -1 : 0;
}

int msync(void *addr, size_t length, int flags) {
    (void)addr;
    (void)length;
    (void)flags;
    return 0;
}

int madvise(void *addr, size_t length, int advice) {
    (void)addr;
    (void)length;
    (void)advice;
    return 0;
}

int posix_madvise(void *addr, size_t length, int advice) {
    return madvise(addr, length, advice);
}

int getrlimit(int resource, struct rlimit *rlim) {
    if (!rlim) {
        errno = EINVAL;
        return -1;
    }
    compat_fill_rlimit(resource, rlim);
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim) {
    (void)resource;
    (void)rlim;
    return 0;
}

int getrusage(int who, struct rusage *usage) {
    switch (who) {
    case RUSAGE_SELF:
    case RUSAGE_CHILDREN:
    case RUSAGE_THREAD:
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    if (!usage) {
        errno = EINVAL;
        return -1;
    }

    compat_fill_rusage(usage);
    return 0;
}

/* poll wrapper */
int poll_compat(struct fry_pollfd *fds, uint32_t nfds, int timeout) {
    return (int)fry_poll(fds, nfds, (timeout < 0) ? (uint64_t)-1 : (uint64_t)timeout);
}

/* gethostname */
int gethostname_compat(char *name, size_t len) {
    if (!name || len == 0) return -1;
    strncpy(name, "tatertos", len - 1);
    name[len - 1] = '\0';
    return 0;
}

int isatty(int fd) {
    return (fd >= 0 && fd <= 2) ? 1 : 0;
}

int tcgetattr(int fd, struct termios *termios_p) {
    if (fd < 0 || !termios_p) {
        errno = EINVAL;
        return -1;
    }

    memset(termios_p, 0, sizeof(*termios_p));
    termios_p->c_lflag = TATER_ECHO | TATER_ICANON;
    termios_p->c_cc[TATER_VINTR] = 3;
    termios_p->c_cc[TATER_VQUIT] = 28;
    termios_p->c_cc[TATER_VERASE] = 127;
    termios_p->c_cc[TATER_VKILL] = 21;
    termios_p->c_cc[TATER_VEOF] = 4;
    termios_p->c_cc[TATER_VMIN] = 1;
    termios_p->c_cc[TATER_VTIME] = 0;
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    (void)optional_actions;
    if (fd < 0 || !termios_p) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

speed_t cfgetispeed(const struct termios *termios_p) {
    return termios_p ? termios_p->c_ispeed : 0;
}

speed_t cfgetospeed(const struct termios *termios_p) {
    return termios_p ? termios_p->c_ospeed : 0;
}

int cfsetispeed(struct termios *termios_p, speed_t speed) {
    if (!termios_p) {
        errno = EINVAL;
        return -1;
    }
    termios_p->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios *termios_p, speed_t speed) {
    if (!termios_p) {
        errno = EINVAL;
        return -1;
    }
    termios_p->c_ospeed = speed;
    return 0;
}

int prctl(int option, ...) {
    va_list ap;
    unsigned long arg2 = 0;
    unsigned long arg3 = 0;
    unsigned long arg4 = 0;
    unsigned long arg5 = 0;

    va_start(ap, option);
    arg2 = va_arg(ap, unsigned long);
    arg3 = va_arg(ap, unsigned long);
    arg4 = va_arg(ap, unsigned long);
    arg5 = va_arg(ap, unsigned long);
    va_end(ap);

    (void)arg3;
    (void)arg4;
    (void)arg5;

    switch (option) {
        case 1:  /* PR_SET_PDEATHSIG */
        case 4:  /* PR_SET_DUMPABLE */
        case 15: /* PR_SET_NAME */
        case 22: /* PR_SET_SECCOMP */
        case 38: /* PR_SET_NO_NEW_PRIVS */
        case 0x59616d61: /* PR_SET_PTRACER */
        case 0x53564d41: /* PR_SET_VMA */
            return 0;
        case 2:  /* PR_GET_PDEATHSIG */
            if ((int *)arg2) {
                *(int *)arg2 = 0;
                return 0;
            }
            errno = EINVAL;
            return -1;
        case 3: /* PR_GET_DUMPABLE */
        case 21: /* PR_GET_SECCOMP */
            return 0;
        case 16: /* PR_GET_NAME */
            if ((char *)arg2) {
                strncpy((char *)arg2, "tatertos", 16);
                ((char *)arg2)[15] = '\0';
                return 0;
            }
            errno = EINVAL;
            return -1;
        default:
            return 0;
    }
}
