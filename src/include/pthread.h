/*
 * TaterTOS64v3 — <pthread.h>
 *
 * POSIX threads.
 */

#ifndef _TATERTOS_PTHREAD_H
#define _TATERTOS_PTHREAD_H

#include <stdint.h>
#include <stddef.h>
#include <sched.h>
#include <time.h>
#include <fry_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Thread Types
 * ----------------------------------------------------------------------- */

/*
 * pthread_t is a scalar to allow 'pthread_t t = 0' initialization
 * in existing C++ code (like OpenH264).
 */
typedef unsigned long pthread_t;

typedef struct {
    size_t _stacksize;
    void  *_stackaddr;
    int    _detachstate;
    int    _inheritsched;
    int    _schedpolicy;
    struct sched_param _schedparam;
    int    _scope;
} pthread_attr_t;

typedef struct { volatile uint32_t state; } pthread_mutex_t;
typedef struct { int _type; } pthread_mutexattr_t;

typedef struct { volatile uint32_t seq; } pthread_cond_t;
typedef struct { int _clock; } pthread_condattr_t;

typedef struct {
    pthread_mutex_t _mutex;
    pthread_cond_t  _cond;
    uint32_t _readers;
    uint32_t _writer;
} pthread_rwlock_t;
typedef struct { int _unused; } pthread_rwlockattr_t;

typedef struct { volatile uint32_t state; } pthread_once_t;
typedef uint32_t pthread_key_t;

/* -----------------------------------------------------------------------
 * Barrier
 * ----------------------------------------------------------------------- */
typedef struct {
    pthread_mutex_t _mutex;
    pthread_cond_t  _cond;
    unsigned int    _count;
    unsigned int    _total;
} pthread_barrier_t;

typedef struct { int _unused; } pthread_barrierattr_t;

#define PTHREAD_BARRIER_SERIAL_THREAD 1

/* -----------------------------------------------------------------------
 * Spinlock
 * ----------------------------------------------------------------------- */
typedef volatile int pthread_spinlock_t;

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_INHERIT_SCHED   0
#define PTHREAD_EXPLICIT_SCHED  1

#define PTHREAD_SCOPE_SYSTEM    0
#define PTHREAD_SCOPE_PROCESS   1

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

#define PTHREAD_ONCE_INIT { 0u }
#define PTHREAD_COND_INITIALIZER { 0u }
#define PTHREAD_MUTEX_INITIALIZER { 0u }

/* -----------------------------------------------------------------------
 * Pthread API
 * ----------------------------------------------------------------------- */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t t1, pthread_t t2);
__attribute__((noreturn)) void pthread_exit(void *retval);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *rwlock);
int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize);
int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr, size_t *stacksize);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
int pthread_attr_setscope(pthread_attr_t *attr, int scope);

int pthread_setname_np(pthread_t thread, const char *name);
int pthread_getname_np(pthread_t thread, char *name, size_t len);
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr);

void pthread_yield(void);

/* -----------------------------------------------------------------------
 * Barrier API
 * ----------------------------------------------------------------------- */
int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attr,
                         unsigned int count);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int pthread_barrier_wait(pthread_barrier_t *barrier);

/* -----------------------------------------------------------------------
 * Spinlock API
 * ----------------------------------------------------------------------- */
int pthread_spin_init(pthread_spinlock_t *lock, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *lock);
int pthread_spin_lock(pthread_spinlock_t *lock);
int pthread_spin_trylock(pthread_spinlock_t *lock);
int pthread_spin_unlock(pthread_spinlock_t *lock);

#ifdef __cplusplus
}
#endif

#endif
