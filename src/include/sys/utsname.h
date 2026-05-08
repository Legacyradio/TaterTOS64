/*
 * TaterTOS64v3 — <sys/utsname.h>
 *
 * POSIX uname() — system identification.
 */

#ifndef _TATERTOS_SYS_UTSNAME_H
#define _TATERTOS_SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

#define _UTSNAME_LENGTH 65

struct utsname {
    char sysname[_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release[_UTSNAME_LENGTH];
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];
#ifdef _GNU_SOURCE
    char domainname[_UTSNAME_LENGTH];
#endif
};

int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif /* _TATERTOS_SYS_UTSNAME_H */
