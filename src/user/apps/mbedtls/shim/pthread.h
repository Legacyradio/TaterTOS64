/*
 * pthread.h shim — minimal stubs for QuickJS atomics (disabled)
 * QuickJS only uses pthreads for CONFIG_ATOMICS which we don't enable.
 * These stubs satisfy the header include.
 */
#ifndef _TATER_SHIM_PTHREAD_H
#define _TATER_SHIM_PTHREAD_H

#include <stddef.h>

typedef unsigned long pthread_t;
typedef struct { int dummy; } pthread_mutex_t;
typedef struct { int dummy; } pthread_cond_t;
typedef struct { int dummy; } pthread_attr_t;
typedef struct { int dummy; } pthread_mutexattr_t;
typedef struct { int dummy; } pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER {0}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
pthread_t pthread_self(void);

#endif
