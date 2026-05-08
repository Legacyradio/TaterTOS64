/*
 * TaterTOS64v3 — <sys/un.h>
 *
 * POSIX Unix-domain socket address. TaterTOS does not yet support
 * AF_UNIX (planned in a future Phase). The struct + AF_UNIX constant
 * are provided so portable code compiles. socket(AF_UNIX, ...) returns
 * -1 with EAFNOSUPPORT until the kernel implements UDS.
 *
 * Origin log: logs/fry839.txt
 */

#ifndef _TATERTOS_SYS_UN_H
#define _TATERTOS_SYS_UN_H

#include <stddef.h>
#include <sys/socket.h>     /* sa_family_t, AF_UNIX */

#ifdef __cplusplus
extern "C" {
#endif

#define UNIX_PATH_MAX 108

struct sockaddr_un {
    sa_family_t sun_family;          /* AF_UNIX */
    char        sun_path[UNIX_PATH_MAX];
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_UN_H */
