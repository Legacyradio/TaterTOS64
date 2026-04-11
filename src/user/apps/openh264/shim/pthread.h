#ifndef _TATER_OH264_PTHREAD_H
#define _TATER_OH264_PTHREAD_H
#include <stddef.h>
typedef unsigned long pthread_t;
typedef struct { int dummy; } pthread_mutex_t;
typedef struct { int dummy; } pthread_cond_t;
typedef struct { int dummy; } pthread_attr_t;
typedef struct { int dummy; } pthread_mutexattr_t;
#define PTHREAD_MUTEX_INITIALIZER {0}
#define PTHREAD_MUTEX_RECURSIVE 1
static inline int pthread_mutex_init(pthread_mutex_t *m, const void *a) { (void)m;(void)a; return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_cond_init(pthread_cond_t *c, const void *a) { (void)c;(void)a; return 0; }
static inline int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_signal(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_broadcast(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { (void)c;(void)m; return 0; }
static inline int pthread_create(pthread_t *t, const void *a, void *(*fn)(void *), void *arg) { (void)t;(void)a;(void)fn;(void)arg; return -1; }
static inline int pthread_join(pthread_t t, void **ret) { (void)t;(void)ret; return 0; }
static inline pthread_t pthread_self(void) { return 0; }
static inline int pthread_mutexattr_init(pthread_mutexattr_t *a) { (void)a; return 0; }
static inline int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t) { (void)a;(void)t; return 0; }
static inline int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
#endif
