/*
 * TaterTOS64v3 — Phase 2 gap matrix stubs for CPython
 * All functions declared in headers but never implemented.
 * Single-user OS — return 0 (success) or NULL.
 */
#include "libc.h"
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sched.h>
#include <pthread.h>
#include <locale.h>
#include <sys/wait.h>

/* GCC runtime */
const unsigned short **__ctype_b_loc(void) { static const unsigned short b[384]={0}; static const unsigned short *p=b; static const unsigned short **pp=&p; return pp; }
int __popcountdi2(unsigned long long x) { int c; for(c=0;x;c++) x&=x-1; return c; }

/* environ — fry1697: reassignable char** (was a fixed {NULL} array) so the
 * program entry can populate it from the kernel env (there is no crt0 to do it).
 * CPython convertenviron() reads this global to build os.environ. */
static char *fry_env_empty[1] = { 0 };
char **environ = fry_env_empty;

/* Process */
/* fry1758: TaterTOS spawns via posix_spawn (no fork). _posixsubprocess.c's
 * do_fork_exec is compiled unconditionally and references fork(); it is dead
 * code at runtime (subprocess uses the posix_spawn path) but must link. */
pid_t fork(void) { errno=ENOSYS; return -1; }
int execv(const char *p, char *const a[]) { (void)p;(void)a; errno=ENOSYS; return -1; }
int execve(const char *p, char *const a[], char *const e[]) { (void)p;(void)a;(void)e; errno=ENOSYS; return -1; }
int raise(int sig) { (void)sig; return 0; }
pid_t wait(int *s) { (void)s; errno=ECHILD; return -1; }
pid_t wait3(int *s, int o, struct rusage *ru) { (void)s;(void)o;(void)ru; errno=ECHILD; return -1; }
pid_t wait4(pid_t p, int *s, int o, struct rusage *ru) { (void)p;(void)s;(void)o;(void)ru; errno=ECHILD; return -1; }

/* UID/GID setters */
int chown(const char *p, uid_t o, gid_t g)  { (void)p;(void)o;(void)g; return 0; }
int chroot(const char *p) { (void)p; return 0; }
int fchmodat(int dfd, const char *p, mode_t m, int f) { (void)dfd;(void)p;(void)m;(void)f; return 0; }
int fchownat(int dfd, const char *p, uid_t o, gid_t g, int f) { (void)dfd;(void)p;(void)o;(void)g;(void)f; return 0; }
int nice(int inc) { (void)inc; return 0; }
int setegid(gid_t g) { (void)g; return 0; }
int seteuid(uid_t u) { (void)u; return 0; }
int setgid(gid_t g)  { (void)g; return 0; }
int setgroups(size_t s, const gid_t *l) { (void)s;(void)l; return 0; }
int setns(int fd, int nst) { (void)fd;(void)nst; errno=ENOSYS; return -1; }
int setpgid(pid_t p, pid_t pg) { (void)p;(void)pg; return 0; }
int setpgrp(void) { return 0; }
int setregid(gid_t r, gid_t e)  { (void)r;(void)e; return 0; }
int setresgid(gid_t r, gid_t e, gid_t s) { (void)r;(void)e;(void)s; return 0; }
int setresuid(uid_t r, uid_t e, uid_t s) { (void)r;(void)e;(void)s; return 0; }
int setreuid(uid_t r, uid_t e) { (void)r;(void)e; return 0; }
int setsid(void) { return 0; }
int setuid(uid_t u) { (void)u; return 0; }
int unshare(int f) { (void)f; errno=ENOSYS; return -1; }

/* UID/GID getters */
uid_t geteuid(void) { return 0; }
uid_t getegid(void) { return 0; }
uid_t getuid(void)  { return 0; }
uid_t getgid(void)  { return 0; }
pid_t getpgid(pid_t p) { (void)p; return 0; }
pid_t getpgrp(void) { return 0; }
pid_t getppid(void) { return 0; }
pid_t getsid(pid_t p) { (void)p; return 0; }
int getgroups(int s, gid_t l[]) { if(s>0&&l)l[0]=0; return 1; }
int getgrouplist(const char *u, gid_t g, gid_t *gr, int *ng) { (void)u;(void)g; if(gr&&ng){gr[0]=0;*ng=1;} return 1; }
int initgroups(const char *u, gid_t g) { (void)u;(void)g; return 0; }
int getresuid(uid_t *r, uid_t *e, uid_t *s) { if(r)*r=0; if(e)*e=0; if(s)*s=0; return 0; }
int getresgid(gid_t *r, gid_t *e, gid_t *s) { if(r)*r=0; if(e)*e=0; if(s)*s=0; return 0; }

/* Terminal */
char *ctermid(char *s) { if(s)s[0]='\0'; return s; }
int ttyname_r(int fd, char *b, size_t n) { (void)fd; if(b&&n)b[0]='\0'; errno=ENOTTY; return -1; }
pid_t tcgetpgrp(int fd) { (void)fd; return 0; }
int tcsetpgrp(int fd, pid_t p) { (void)fd;(void)p; errno=ENOTTY; return -1; }

/* Login stub */
int getlogin_r(char *b, size_t n) { if(b&&n)b[0]='\0'; return 0; }

/* Hostname */
int gethostname(char *n, size_t l) { if(n&&l)n[0]='\0'; return 0; }
int sethostname(const char *n, size_t l) { (void)n;(void)l; return 0; }

/* Time */

/* File ops */
int fchdir(int fd) { (void)fd; return 0; }
int futimens(int fd, const struct timespec t[2]) { (void)fd;(void)t; return 0; }
/* ftruncate: no longer a stub. fry1815 implements it for real in posix.c on top
 * of SYS_FTRUNCATE, which already existed. The stub here did nothing and
 * returned 0 — it reported success, so every caller believed the file had been
 * resized. Do not reinstate it. */
int lockf(int fd, int op, off_t l) { (void)fd;(void)op;(void)l; errno=ENOSYS; return -1; }
int mkfifo(const char *p, mode_t m) { (void)p;(void)m; errno=ENOSYS; return -1; }
int mkfifoat(int dfd, const char *p, mode_t m) { (void)dfd;(void)p;(void)m; errno=ENOSYS; return -1; }
int mknod(const char *p, mode_t m, dev_t d) { (void)p;(void)m;(void)d; errno=ENOSYS; return -1; }
int mknodat(int dfd, const char *p, mode_t m, dev_t d) { (void)dfd;(void)p;(void)m;(void)d; errno=ENOSYS; return -1; }
int symlinkat(const char *t, int ndfd, const char *lp) { (void)t;(void)ndfd;(void)lp; errno=ENOSYS; return -1; }
/* truncate: no longer a stub — see ftruncate above. Real implementation in
 * posix.c (open + ftruncate + close). Do not reinstate. */
int umask(mode_t cm) { return 0; }
int linkat(int odfd, const char *op, int ndfd, const char *np, int f) { (void)odfd;(void)op;(void)ndfd;(void)np;(void)f; errno=ENOSYS; return -1; }

/* I/O */

/* Directories */

/* Sync */

/* Path config */
long fpathconf(int fd, int n) { (void)fd;(void)n; errno=EINVAL; return -1; }
long pathconf(const char *p, int n) { (void)p;(void)n; errno=EINVAL; return -1; }
size_t confstr(int n, char *b, size_t l) { (void)n; if(b&&l)b[0]='\0'; return 0; }
int getloadavg(double l[], int n) { if(l&&n>0)l[0]=0.0; return 1; }

/* Timers */

/* File advice */
int posix_fadvise(int fd, off_t o, off_t l, int a) { (void)fd;(void)o;(void)l;(void)a; return ENOSYS; }
int posix_fallocate(int fd, off_t o, off_t l) { (void)fd;(void)o;(void)l; return 0; }

/* Random */
/* getentropy: no longer a stub. fry1816 implements it for real in posix.c on
 * top of SYS_GETRANDOM (85), which already existed and was already used by the
 * Rust path. The stub here ZERO-FILLED the buffer and returned success, and
 * CPython takes its entropy from getentropy() alone (pyconfig.h:493 defines
 * HAVE_GETENTROPY, :592 leaves HAVE_GETRANDOM undefined) — so os.urandom,
 * secrets, uuid4 and TLS randomness were all zeros. Do not reinstate it. */

/* Signal */
int sigemptyset(sigset_t *s) { if(s)*s=0; return 0; }
int sigfillset(sigset_t *s)  { if(s)*s=~(sigset_t)0; return 0; }
int sigaddset(sigset_t *s, int sig) { if(s&&sig>0&&(size_t)sig<sizeof(sigset_t)*8)*s|=(1ULL<<(sig-1)); return 0; }
int sigdelset(sigset_t *s, int sig) { if(s&&sig>0&&(size_t)sig<sizeof(sigset_t)*8)*s&=~(1ULL<<(sig-1)); return 0; }
int sigismember(const sigset_t *s, int sig) { if(s&&sig>0&&(size_t)sig<sizeof(sigset_t)*8)return!!(*s&(1ULL<<(sig-1))); return 0; }
int sigpending(sigset_t *s) { if(s)*s=0; return 0; }
int sigwait(const sigset_t *s, int *sig) { (void)s; if(sig)*sig=0; errno=ENOSYS; return -1; }
int pthread_sigmask(int h, const sigset_t *s, sigset_t *o) { (void)h;(void)s;(void)o; return 0; }
int killpg(int pg, int sig) { (void)pg;(void)sig; return 0; }
int sigaltstack(const stack_t *ss, stack_t *oss) { (void)ss;(void)oss; return 0; }

/* Scheduling */

/* Thread CPU clock */

/* Semaphores */

/* Wide char */
int wcscmp(const wchar_t *s1, const wchar_t *s2) {
    if(!s1&&!s2)return 0; if(!s1)return -1; if(!s2)return 1;
    while(*s1&&*s1==*s2){s1++;s2++;}
    return (int)*s1-(int)*s2;
}
int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
    if(!s1&&!s2)return 0;
    for(size_t i=0;i<n;i++){if(s1[i]!=s2[i])return(int)s1[i]-(int)s2[i]; if(!s1[i])return 0;}
    return 0;
}
wchar_t *wcscpy(wchar_t *d, const wchar_t *s) { wchar_t *o=d; while((*d++=*s++)); return o; }
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n) {
    size_t i; for(i=0;i<n&&s[i];i++) d[i]=s[i]; for(;i<n;i++) d[i]=0; return d;
}
wchar_t *wcschr(const wchar_t *s, wchar_t c) { while(*s){if(*s==c)return(wchar_t*)s; s++;} return c==0?(wchar_t*)s:NULL; }
wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    const wchar_t *last=NULL; while(*s){if(*s==c)last=s; s++;}
    return (wchar_t*)(c==0?s:last);
}
wchar_t *wcstok(wchar_t *s, const wchar_t *d, wchar_t **save) { (void)s;(void)d;(void)save; return NULL; }
/* wcsftime: no longer a stub. fry1814 implements it for real in
 * time_ext.c (wide -> UTF-8 -> the correct narrow strftime -> wide). The stub
 * here returned an empty string and 0, which silently emptied every Python
 * timestamp in the OS because CPython routes time.strftime() through wcsftime
 * when HAVE_WCSFTIME is defined. Do not reinstate it. */
size_t wcstombs(char *d, const wchar_t *s, size_t n) {
    if(!d||!s)return 0; size_t i; for(i=0;i<n-1&&s[i];i++) d[i]=(char)(s[i]&0x7F); if(i<n)d[i]=0; return i;
}
size_t mbstowcs(wchar_t *d, const char *s, size_t n) {
    if(!d||!s)return 0; size_t i; for(i=0;i<n&&s[i];i++) d[i]=(wchar_t)(unsigned char)s[i]; return i;
}
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) { for(size_t i=0;i<n;i++)if(s[i]==c)return(wchar_t*)(s+i); return NULL; }
int wcscoll(const wchar_t *s1, const wchar_t *s2) { return wcscmp(s1,s2); }

/* POSIX spawn attrs */

/* setlocale — stub returning "C" */
char *setlocale(int cat, const char *locale) { (void)cat; (void)locale; static char c[]="C"; return c; }

/* setenv/unsetenv (TaterTOS does not have process environment) */

/* localeconv stub */

/* PWD stubs */
void setpwent(void) {}
struct passwd *getpwent(void) { return NULL; }
void endpwent(void) {}
int getpwnam_r(const char *n, struct passwd *p, char *b, size_t s, struct passwd **r) { (void)n;(void)p;(void)b;(void)s; if(r)*r=NULL; return 0; }
int getpwuid_r(uid_t u, struct passwd *p, char *b, size_t s, struct passwd **r) { (void)u;(void)p;(void)b;(void)s; if(r)*r=NULL; return 0; }

/* CPython faulthandler stubs */
int _PyFaulthandler_Init(int e) { (void)e; return 0; }
void _PyFaulthandler_Fini(void) {}

/* fry1758: CPython fork-lifecycle hooks. Normally in posixmodule.c under
 * HAVE_FORK (undefined here), but _posixsubprocess.c references them from its
 * unconditional do_fork_exec. fork() always fails on TaterTOS, so these are
 * never load-bearing — they only need to link. */
void PyOS_BeforeFork(void) {}
void PyOS_AfterFork_Parent(void) {}
void PyOS_AfterFork_Child(void) {}

/* wcsxfrm */
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n) { if(s&&d&&n){size_t i; for(i=0;i<n-1&&s[i];i++)d[i]=s[i]; d[i]=0; return i;} return 0; }

/* I/O with offset: no longer stubs. fry1817 implements pread/pwrite/preadv/
 * pwritev for real in posix.c (save offset, seek, transfer, restore). The stubs
 * here discarded the offset with (void)off and forwarded to the sequential
 * call, so every positional transfer hit the file's CURRENT position and
 * reported success — silent corruption for random-access files, and CPython is
 * configured to use them (pyconfig.h HAVE_PREAD/HAVE_PWRITE et al).
 * Do not reinstate them. */

/* Timers */
int getitimer(int w, struct itimerval *c) { (void)w; if(c){memset(c,0,sizeof(*c));} return 0; }
int setitimer(int w, const struct itimerval *nv, struct itimerval *ov) { (void)w;(void)nv; if(ov){memset(ov,0,sizeof(*ov));} return 0; }
/* clock_nanosleep: no longer a stub — fry1818 implements it for real in
 * posix.c, including TIMER_ABSTIME. It used to delegate to the nanosleep stub
 * below, which always failed. Do not reinstate. */
int clock_settime(clockid_t c, const struct timespec *ts) { (void)c;(void)ts; return 0; }
clock_t times(struct tms *b) { if(b){memset(b,0,sizeof(*b));} return (clock_t)-1; }

/* Scheduling */
int sched_getparam(pid_t p, struct sched_param *sp) { (void)p; if(sp)memset(sp,0,sizeof(*sp)); return 0; }
int sched_setparam(pid_t p, const struct sched_param *sp) { (void)p;(void)sp; return 0; }
int sched_getscheduler(pid_t p) { (void)p; return SCHED_OTHER; }
int sched_setscheduler(pid_t p, int pol, const struct sched_param *sp) { (void)p;(void)pol;(void)sp; return 0; }
int sched_rr_get_interval(pid_t p, struct timespec *ts) { (void)p; if(ts){ts->tv_sec=0;ts->tv_nsec=10000000;} return 0; }

/* Signal advanced */
int sigwaitinfo(const sigset_t *s, siginfo_t *i) { (void)s;(void)i; errno=ENOSYS; return -1; }
int sigtimedwait(const sigset_t *s, siginfo_t *i, const struct timespec *t) { (void)s;(void)i;(void)t; errno=ENOSYS; return -1; }
char *strsignal(int sig) { static char b[32]; snprintf(b,sizeof(b),"Signal %d",sig); return b; }


/* Final missing stubs */
unsigned int alarm(unsigned int seconds) { (void)seconds; return 0; }
clock_t clock(void) { return 0; }
/* nanosleep: no longer a stub. fry1818 implements it for real in posix.c on top
 * of SYS_NANOSLEEP (87) and fry_nanosleep(), both of which already existed. The
 * stub here always returned -1/ENOSYS, so every bounded wait in the OS —
 * SQLite's busy handler, retry backoff, threading timeouts — either failed hard
 * or spun. Do not reinstate. */
int sync(void) { return 0; }

/* posix_spawn stubs — TaterTOS has spawn/p/spawnp but not the attr/fct variants yet */
int posix_spawnattr_init(void *a) { (void)a; return 0; }
int posix_spawnattr_destroy(void *a) { (void)a; return 0; }
int posix_spawnattr_setflags(void *a, short f) { (void)a;(void)f; return 0; }
int posix_spawnattr_setpgroup(void *a, pid_t p) { (void)a;(void)p; return 0; }
int posix_spawnattr_setschedparam(void *a, const struct sched_param *sp) { (void)a;(void)sp; return 0; }
int posix_spawnattr_setschedpolicy(void *a, int p) { (void)a;(void)p; return 0; }
int posix_spawnattr_setsigdefault(void *a, const sigset_t *s) { (void)a;(void)s; return 0; }
int posix_spawnattr_setsigmask(void *a, const sigset_t *s) { (void)a;(void)s; return 0; }
int posix_spawn_file_actions_init(void *fa) { (void)fa; return 0; }
int posix_spawn_file_actions_destroy(void *fa) { (void)fa; return 0; }
int posix_spawn_file_actions_addclose(void *fa, int fd) { (void)fa;(void)fd; return 0; }
int posix_spawn_file_actions_adddup2(void *fa, int fd, int nfd) { (void)fa;(void)fd;(void)nfd; return 0; }
int posix_spawn_file_actions_addopen(void *fa, int fd, const char *p, int f, mode_t m) { (void)fa;(void)fd;(void)p;(void)f;(void)m; return 0; }

/* pthread */
int pthread_getcpuclockid(pthread_t t, clockid_t *c) { (void)t; if(c)*c=0; return 0; }

/* sem */
int sem_init(void *s, int ps, unsigned v) { (void)s;(void)ps;(void)v; return 0; }
int sem_destroy(void *s) { (void)s; return 0; }
int sem_wait(void *s) { (void)s; return 0; }
int sem_trywait(void *s) { (void)s; return 0; }
int sem_post(void *s) { (void)s; return 0; }
int sem_clockwait(void *s, clockid_t c, const struct timespec *abst) { (void)s;(void)c;(void)abst; return 0; }


struct lconv *localeconv(void) { static struct lconv l; memset(&l,0,sizeof(l)); return &l; }
int setenv_compat(const char *name, const char *value, int overwrite) { (void)name;(void)value;(void)overwrite; return 0; }
int unsetenv_compat(const char *name) { (void)name; return 0; }
/* Must match unistd.h declarations */
int waitid(idtype_t idtype, id_t id, void *info, int options) { (void)idtype;(void)id;(void)info;(void)options; errno=ECHILD; return -1; }
/* _exit — must match unistd.h: int _exit(int status) */
int _exit(int status) { (void)status; abort(); /* should call fry_exit */ return 0; }
int utimes(const char *path, const struct timeval times[2]) { (void)path;(void)times; return 0; }
