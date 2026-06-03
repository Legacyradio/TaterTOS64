/*
 * TaterTOS64v3 — <sys/random.h>
 *
 * Linux/BSD-compatible entropy entry points backed by SYS_GETRANDOM.
 */

#ifndef _TATERTOS_SYS_RANDOM_H
#define _TATERTOS_SYS_RANDOM_H

#include <stddef.h>
#include <sys/types.h>
#include <fry_random.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRND_NONBLOCK FRY_GRND_NONBLOCK
#define GRND_RANDOM   FRY_GRND_RANDOM
#define GRND_INSECURE FRY_GRND_INSECURE

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
int     getentropy(void *buf, size_t buflen);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_RANDOM_H */
