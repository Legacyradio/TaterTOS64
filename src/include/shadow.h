/*
 * TaterTOS64v3 — <shadow.h>
 *
 * POSIX shadow password file. TaterTOS does not currently have
 * multi-user authentication; this header exists so portable code
 * compiles. Lookups return null.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_SHADOW_H
#define _TATERTOS_SHADOW_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spwd {
    char *sp_namp;
    char *sp_pwdp;
    long  sp_lstchg;
    long  sp_min;
    long  sp_max;
    long  sp_warn;
    long  sp_inact;
    long  sp_expire;
    unsigned long sp_flag;
};

struct spwd *getspnam(const char *name);
int          getspnam_r(const char *name, struct spwd *spbuf,
                        char *buf, size_t buflen,
                        struct spwd **spbufp);

void         setspent(void);
void         endspent(void);
struct spwd *getspent(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SHADOW_H */
