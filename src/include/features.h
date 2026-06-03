/*
 * TaterTOS64v3 — <features.h>
 *
 * Glibc-compatible feature test macros stub.
 * TaterTOS is not glibc, but Chromium code checks __GLIBC__ etc.
 */

#ifndef _TATERTOS_FEATURES_H
#define _TATERTOS_FEATURES_H

/* Pretend to be a recent glibc for compatibility. */
#define __GLIBC__        2
#define __GLIBC_MINOR__  35

#define __USE_XOPEN2K8   1
#define __USE_POSIX      1
#define __USE_POSIX2     1
#define __USE_POSIX199309 1
#define __USE_POSIX199506 1
#define __USE_XOPEN      1
#define __USE_XOPEN_EXTENDED 1

#endif
