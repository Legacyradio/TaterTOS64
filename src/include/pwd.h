/*
 * TaterTOS64v3 — <pwd.h>
 *
 * POSIX password database. TaterTOS does not currently have a
 * multi-user system; getpwuid/getpwnam return a single hardcoded
 * "root" entry. The struct + API exist so portable code compiles.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_PWD_H
#define _TATERTOS_PWD_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
    char   *pw_name;
    char   *pw_passwd;
    uid_t   pw_uid;
    gid_t   pw_gid;
    char   *pw_gecos;
    char   *pw_dir;
    char   *pw_shell;
};

struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);
int            getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);
int            getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);

void           setpwent(void);
void           endpwent(void);
struct passwd *getpwent(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_PWD_H */
