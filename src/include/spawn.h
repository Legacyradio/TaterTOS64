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
