/*
 * TaterTOS64v3 — <signal.h>
 *
 * POSIX signal handling. Backed by sigaction_compat etc. in
 * src/user/libc/posix.c. TaterTOS does not currently deliver async
 * signals to userland, but the API surface exists so portable
 * upstream code that registers handlers compiles. Signals raised
 * synchronously via raise() are delivered.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_SIGNAL_H
#define _TATERTOS_SIGNAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Signal numbers — POSIX subset matching Linux values.
 */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGIOT     SIGABRT
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31

#define NSIG 64

/*
 * sigaction flags.
 */
#define SA_NOCLDSTOP  0x00000001
#define SA_NOCLDWAIT  0x00000002
#define SA_SIGINFO    0x00000004
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000

/*
 * sigprocmask how.
 */
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2

/*
 * Special handler values.
 */
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

typedef void (*sighandler_t)(int);

typedef uint64_t sigset_t;

union sigval {
    int   sival_int;
    void *sival_ptr;
};

typedef struct {
    int          si_signo;
    int          si_errno;
    int          si_code;
    pid_t        si_pid;
    uid_t        si_uid;
    void        *si_addr;
    int          si_status;
    long         si_band;
    union sigval si_value;
} siginfo_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int      sa_flags;
    void   (*sa_restorer)(void);
};

/*
 * Backed by sigaction_compat etc. in posix.c (declared in libc.h).
 */
struct sigaction_compat;
extern int sigaction_compat(int sig, const struct sigaction_compat *act,
                            struct sigaction_compat *oldact);

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
int raise(int sig);
int kill(pid_t pid, int sig);
sighandler_t signal(int signum, sighandler_t handler);

/*
 * sigsetjmp / siglongjmp — defined as macros + sigjmp_buf typedef
 * in <setjmp.h>; just include it.
 */
#include <setjmp.h>

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SIGNAL_H */
