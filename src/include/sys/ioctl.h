/*
 * TaterTOS64v3 — <sys/ioctl.h>
 *
 * Minimal ioctl surface. TaterTOS userland's ioctl story is
 * fd-kind-specific (TTY via ps2_kbd line discipline, SHM via
 * fry_shm_*, etc.). This header declares ioctl() so portable code
 * compiles — actual semantics depend on which fd kind is targeted.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_SYS_IOCTL_H
#define _TATERTOS_SYS_IOCTL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Common ioctl request numbers. Match Linux for portability of
 * upstream code that uses them.
 */
#define TIOCGWINSZ   0x5413
#define TIOCSWINSZ   0x5414
#define TIOCGPGRP    0x540F
#define TIOCSPGRP    0x5410
#define FIONREAD     0x541B
#define FIONBIO      0x5421
#define FIOCLEX      0x5451
#define FIONCLEX     0x5450

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_IOCTL_H */
