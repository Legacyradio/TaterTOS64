/* TaterTOS64 grp.h — POSIX group database
 *
 * TaterTOS64 has no Unix-style group database. getgrnam() and getgrgid()
 * always return NULL and set errno to ENOENT. Chromium handles this
 * gracefully via the NULL check in VerifyPathControlledByAdmin.
 */
#ifndef _GRP_H
#define _GRP_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct group {
    char   *gr_name;   /* group name */
    char   *gr_passwd; /* group password (unused) */
    gid_t   gr_gid;    /* group ID */
    char  **gr_mem;    /* NULL-terminated array of member names */
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);

#ifdef __cplusplus
}
#endif

#endif /* _GRP_H */
