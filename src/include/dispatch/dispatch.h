/*
 * TaterTOS64v3 — <dispatch/dispatch.h>
 *
 * Minimal Grand Central Dispatch stub for Skia.
 * Skia only uses dispatch_semaphore_t; provide enough for it to compile.
 */

#ifndef _TATERTOS_DISPATCH_H
#define _TATERTOS_DISPATCH_H

#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPATCH_TIME_FOREVER (~0ULL)

typedef struct dispatch_semaphore_s *dispatch_semaphore_t;

dispatch_semaphore_t dispatch_semaphore_create(long value);
void                 dispatch_release(dispatch_semaphore_t dsema);
void                 dispatch_semaphore_signal(dispatch_semaphore_t dsema);
long                 dispatch_semaphore_wait(dispatch_semaphore_t dsema,
                                              uint64_t timeout);

#ifdef __cplusplus
}
#endif

#endif
