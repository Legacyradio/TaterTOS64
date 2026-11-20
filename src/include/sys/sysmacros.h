/*
 * TaterTOS64v3 — <sys/sysmacros.h>
 *
 * BSD/GNU device number macros. CPython's posixmodule.c uses these
 * for os.major(), os.minor(), os.makedev().
 */
#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>  /* dev_t */

#define major(dev)  ((unsigned int)(((dev) >> 8) & 0xFF))
#define minor(dev)  ((unsigned int)((dev) & 0xFF))
#define makedev(maj, min)  ((dev_t)((((unsigned int)(maj) & 0xFF) << 8) | ((unsigned int)(min) & 0xFF)))

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SYSMACROS_H */
