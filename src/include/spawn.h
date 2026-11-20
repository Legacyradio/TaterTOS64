/*
 * TaterTOS64v3 — <spawn.h>
 *
 * POSIX posix_spawn family. Backed by fry_spawn / fry_spawn_args
 * in libc.h.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_SPAWN_H
#define _TATERTOS_SPAWN_H

#include <stddef.h>
#include <sys/types.h>
#include <signal.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int        flags;
    pid_t      pgroup;
    sigset_t   sigmask;
    sigset_t   sigdefault;
    int        schedpolicy;
    struct sched_param schedparam;
} posix_spawnattr_t;

/*
 * posix_spawn_file_actions_t — opaque-ish struct. The standard says
 * applications should treat this as opaque and only manipulate it
 * through posix_spawn_file_actions_*() functions. We need a complete
 * type so it can be stack-allocated.
 */
typedef struct posix_spawn_file_actions {
    int    _count;
    int    _capacity;
    void  *_actions;       /* dynamically grown action list */
} posix_spawn_file_actions_t;

#define POSIX_SPAWN_RESETIDS         0x01
#define POSIX_SPAWN_SETPGROUP        0x02
#define POSIX_SPAWN_SETSIGDEF        0x04
#define POSIX_SPAWN_SETSIGMASK       0x08
#define POSIX_SPAWN_SETSCHEDPARAM    0x10
#define POSIX_SPAWN_SETSCHEDULER     0x20
#define POSIX_SPAWN_USEVFORK         0x40
#define POSIX_SPAWN_SETSID           0x80

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]);

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]);

int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);

/* Stubs — TaterTOS spawn doesn't support these attributes */
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags);
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup);
int posix_spawnattr_setschedparam(posix_spawnattr_t *attr, const struct sched_param *schedparam);
int posix_spawnattr_getschedparam(const posix_spawnattr_t *attr, struct sched_param *schedparam);
int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int schedpolicy);
int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *attr, int *schedpolicy);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *sigdefault);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *sigdefault);
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *sigmask);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *sigmask);

#define POSIX_SPAWN_RESETIDS       0x01
#define POSIX_SPAWN_SETPGROUP      0x02
#define POSIX_SPAWN_SETSIGDEF      0x04
#define POSIX_SPAWN_SETSIGMASK     0x08
#define POSIX_SPAWN_SETSCHEDULER   0x10
#define POSIX_SPAWN_SETSCHEDPARAM  0x20
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                     const char *path, int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SPAWN_H */
