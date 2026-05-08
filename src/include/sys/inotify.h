/*
 * TaterTOS64v3 — <sys/inotify.h>
 *
 * POSIX/Linux-compatible inotify declarations backed by TaterTOS syscalls
 * SYS_INOTIFY_INIT, SYS_INOTIFY_ADD_WATCH, SYS_INOTIFY_RM_WATCH.
 */

#ifndef _TATERTOS_SYS_INOTIFY_H
#define _TATERTOS_SYS_INOTIFY_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flag values for inotify_init1() */
#define IN_NONBLOCK   0x800
#define IN_CLOEXEC    0x80000

/* Supported event masks */
#define IN_ACCESS       0x00000001
#define IN_MODIFY       0x00000002
#define IN_ATTRIB       0x00000004
#define IN_CLOSE_WRITE  0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN         0x00000020
#define IN_MOVED_FROM   0x00000040
#define IN_MOVED_TO     0x00000080
#define IN_CREATE       0x00000100
#define IN_DELETE       0x00000200
#define IN_DELETE_SELF  0x00000400
#define IN_MOVE_SELF    0x00000800
#define IN_UNMOUNT      0x00002000
#define IN_Q_OVERFLOW   0x00004000
#define IN_IGNORED      0x00008000

/* Convenience masks */
#define IN_CLOSE        (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)
#define IN_MOVE         (IN_MOVED_FROM | IN_MOVED_TO)
#define IN_ALL_EVENTS   (IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | \
                         IN_CLOSE_NOWRITE | IN_OPEN | IN_MOVED_FROM | \
                         IN_MOVED_TO | IN_CREATE | IN_DELETE | \
                         IN_DELETE_SELF | IN_MOVE_SELF)

/* Special flags for inotify_add_watch */
#define IN_ONLYDIR      0x01000000
#define IN_DONT_FOLLOW  0x02000000
#define IN_EXCL_UNLINK  0x04000000
#define IN_MASK_ADD     0x20000000
#define IN_ISDIR        0x40000000
#define IN_ONESHOT      0x80000000

/* Structure read from an inotify file descriptor */
struct inotify_event {
    int      wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char     name[];  /* Variable-length */
};

int inotify_init(void);
int inotify_init1(int flags);
int inotify_add_watch(int fd, const char *pathname, uint32_t mask);
int inotify_rm_watch(int fd, int wd);

#ifdef __cplusplus
}
#endif

#endif /* _TATERTOS_SYS_INOTIFY_H */
