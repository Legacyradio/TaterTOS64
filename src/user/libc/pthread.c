/*
 * pthread.c — POSIX threads API wrapper over fry_thread/fry_mutex primitives
 *
 * Phase 8: pthread API needed by NSPR and NSS. Maps directly onto existing
 * TaterTOS threading primitives (fry_thread_create, fry_mutex, fry_cond, etc.)
 *
 * All implementations are original TaterTOS code.
 */

#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Public POSIX headers */
#include <pthread.h>
#include <sched.h>
#include <time.h>

/* Private TaterTOS ABI */
#include "libc.h"
#include "fry.h"

/* -----------------------------------------------------------------------
 * Internal Thread Tracking
 * ----------------------------------------------------------------------- */

#define MAX_PTHREADS 256
struct pthread_info {
    uint32_t tid;
    struct fry_thread thr;
    void *(*fn)(void *);
    void *arg;
    int in_use;
};

static struct pthread_info g_pthreads[MAX_PTHREADS];
static fry_mutex_t g_pth_lock = FRY_MUTEX_INIT;

static struct pthread_info *alloc_pth(void) {
    fry_mutex_lock(&g_pth_lock);
    for (int i = 0; i < MAX_PTHREADS; i++) {
        if (!g_pthreads[i].in_use) {
            g_pthreads[i].in_use = 1;
            fry_mutex_unlock(&g_pth_lock);
            return &g_pthreads[i];
        }
    }
    fry_mutex_unlock(&g_pth_lock);
    return 0;
}

static struct pthread_info *find_pth(pthread_t t) {
    if (t == 0 || t > MAX_PTHREADS) return 0;
    struct pthread_info *pi = &g_pthreads[t - 1];
    return pi->in_use ? pi : 0;
}

static void pthread_trampoline(void *opaque) {
    struct pthread_info *pi = (struct pthread_info *)opaque;
    pi->fn(pi->arg);
}

/* -----------------------------------------------------------------------
 * Pthread API
 * ----------------------------------------------------------------------- */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;
    if (!thread || !start_routine) return EINVAL;

    struct pthread_info *pi = alloc_pth();
    if (!pi) return ENOMEM;

    pi->fn = start_routine;
    pi->arg = arg;

    long rc = fry_thread_create(&pi->thr, pthread_trampoline, pi);
    if (rc < 0) {
        pi->in_use = 0;
        return (int)(-rc);
    }
    pi->tid = pi->thr.tid;
    *thread = (pthread_t)(pi - g_pthreads + 1);
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    struct pthread_info *pi = find_pth(thread);
    if (!pi) return ESRCH;

    int exit_code = 0;
    long rc = fry_thread_join(&pi->thr, &exit_code);
    if (rc < 0) return (int)(-rc);

    if (retval) *retval = (void *)(intptr_t)exit_code;
    pi->in_use = 0;
    return 0;
}

int pthread_detach(pthread_t thread) {
    struct pthread_info *pi = find_pth(thread);
    if (!pi) return ESRCH;
    return 0;
}

pthread_t pthread_self(void) {
    uint32_t tid = (uint32_t)fry_gettid();
    fry_mutex_lock(&g_pth_lock);
    for (int i = 0; i < MAX_PTHREADS; i++) {
        if (g_pthreads[i].in_use && g_pthreads[i].tid == tid) {
            pthread_t t = (pthread_t)(i + 1);
            fry_mutex_unlock(&g_pth_lock);
            return t;
        }
    }
    fry_mutex_unlock(&g_pth_lock);
    return 0;
}

int pthread_equal(pthread_t t1, pthread_t t2) {
    return t1 == t2;
}

void pthread_exit(void *retval) {
    fry_thread_exit((int)(intptr_t)retval);
}

int pthread_kill(pthread_t thread, int sig) {
    (void)thread;
    return (sig == 0) ? 0 : ENOSYS;
}

int pthread_setname_np(pthread_t thread, const char *name) {
    (void)thread;
    if (!name) return EINVAL;
    return 0;
}

int pthread_getname_np(pthread_t thread, char *name, size_t len) {
    (void)thread;
    if (!name || len == 0) return EINVAL;
    name[0] = '\0';
    return 0;
}

int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    struct pthread_info *pi = find_pth(thread);
    if (!pi) return ESRCH;

    memset(attr, 0, sizeof(*attr));
    attr->_stackaddr = pi->thr.stack_base;
    attr->_stacksize = pi->thr.stack_len;
    return 0;
}

void pthread_yield(void) {
    (void)sched_yield();
}

int sched_yield(void) {
    fry_sleep(0);
    return 0;
}

/* -----------------------------------------------------------------------
 * pthread_mutex — wraps fry_mutex
 * ----------------------------------------------------------------------- */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    if (!mutex) return EINVAL;
    mutex->state = 0u;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    int rc = fry_mutex_lock((fry_mutex_t *)mutex);
    return (rc < 0) ? -rc : 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    int rc = fry_mutex_trylock((fry_mutex_t *)mutex);
    if (rc == -EBUSY) return EBUSY;
    return (rc < 0) ? -rc : 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    int rc = fry_mutex_unlock((fry_mutex_t *)mutex);
    return (rc < 0) ? -rc : 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    attr->_type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr) return EINVAL;
    attr->_type = type;
    return 0;
}

/* -----------------------------------------------------------------------
 * pthread_cond — wraps fry_cond
 * ----------------------------------------------------------------------- */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    if (!cond) return EINVAL;
    cond->seq = 0u;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (!cond || !mutex) return EINVAL;
    int rc = fry_cond_wait((fry_cond_t *)cond, (fry_mutex_t *)mutex);
    return (rc < 0) ? -rc : 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
    if (!cond || !mutex) return EINVAL;

    if (abstime) {
        struct timespec now;
        struct fry_timespec fts;
        fry_clock_gettime(FRY_CLOCK_MONOTONIC, &fts);
        now.tv_sec = (time_t)fts.tv_sec;
        now.tv_nsec = (long)fts.tv_nsec;

        int64_t diff_ms = (abstime->tv_sec - now.tv_sec) * 1000 +
                          (abstime->tv_nsec - now.tv_nsec) / 1000000;
        if (diff_ms <= 0) return ETIMEDOUT;

        uint32_t seq = cond->seq;
        int urc = fry_mutex_unlock((fry_mutex_t *)mutex);
        if (urc < 0) return -urc;

        long frc = fry_futex_wait(&cond->seq, seq, (uint64_t)diff_ms);

        int lrc = fry_mutex_lock((fry_mutex_t *)mutex);
        if (lrc < 0) return -lrc;

        if (frc == -ETIMEDOUT) return ETIMEDOUT;
        return 0;
    }

    return pthread_cond_wait(cond, mutex);
}

int pthread_cond_signal(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    int rc = fry_cond_signal((fry_cond_t *)cond);
    return (rc < 0) ? -rc : 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    int rc = fry_cond_broadcast((fry_cond_t *)cond);
    return (rc < 0) ? -rc : 0;
}

int pthread_condattr_init(pthread_condattr_t *attr) {
    if (!attr) return EINVAL;
    attr->_clock = 0;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
    if (!attr) return EINVAL;
    return 0;
}

/* -----------------------------------------------------------------------
 * pthread_rwlock — simple readers-writer lock
 * ----------------------------------------------------------------------- */

int pthread_rwlock_init(pthread_rwlock_t *rwl, const pthread_rwlockattr_t *attr) {
    (void)attr;
    if (!rwl) return EINVAL;
    rwl->_mutex.state = 0u;
    rwl->_cond.seq = 0u;
    rwl->_readers = 0;
    rwl->_writer = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwl) {
    (void)rwl;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwl) {
    if (!rwl) return EINVAL;
    fry_mutex_lock((fry_mutex_t *)&rwl->_mutex);
    while (rwl->_writer) {
        fry_cond_wait((fry_cond_t *)&rwl->_cond, (fry_mutex_t *)&rwl->_mutex);
    }
    rwl->_readers++;
    fry_mutex_unlock((fry_mutex_t *)&rwl->_mutex);
    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwl) {
    if (!rwl) return EINVAL;
    fry_mutex_lock((fry_mutex_t *)&rwl->_mutex);
    while (rwl->_writer || rwl->_readers > 0) {
        fry_cond_wait((fry_cond_t *)&rwl->_cond, (fry_mutex_t *)&rwl->_mutex);
    }
    rwl->_writer = 1;
    fry_mutex_unlock((fry_mutex_t *)&rwl->_mutex);
    return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwl) {
    if (!rwl) return EINVAL;
    fry_mutex_lock((fry_mutex_t *)&rwl->_mutex);
    if (rwl->_writer) {
        rwl->_writer = 0;
    } else if (rwl->_readers > 0) {
        rwl->_readers--;
    }
    fry_cond_broadcast((fry_cond_t *)&rwl->_cond);
    fry_mutex_unlock((fry_mutex_t *)&rwl->_mutex);
    return 0;
}

/* -----------------------------------------------------------------------
 * pthread_once — wraps fry_once
 * ----------------------------------------------------------------------- */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) return EINVAL;
    int rc = fry_once((fry_once_t *)once_control, init_routine);
    return (rc < 0) ? -rc : 0;
}

/* -----------------------------------------------------------------------
 * pthread_key (TLS) — wraps fry_tls_key
 * ----------------------------------------------------------------------- */

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    (void)destructor;
    if (!key) return EINVAL;
    uint32_t fkey;
    int rc = fry_tls_key_create(&fkey);
    if (rc < 0) return -rc;
    *key = (pthread_key_t)fkey;
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    (void)key;
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    int rc = fry_tls_set((uint32_t)key, (void *)value);
    return (rc < 0) ? -rc : 0;
}

void *pthread_getspecific(pthread_key_t key) {
    return fry_tls_get((uint32_t)key);
}

/* -----------------------------------------------------------------------
 * pthread_attr — minimal stubs
 * ----------------------------------------------------------------------- */

int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    memset(attr, 0, sizeof(*attr));
    attr->_stacksize = 2 * 1024 * 1024;
    attr->_detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state) {
    if (!attr) return EINVAL;
    attr->_detachstate = state;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size) {
    if (!attr || size == 0) return EINVAL;
    attr->_stacksize = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size) {
    if (!attr || !size) return EINVAL;
    *size = attr->_stacksize;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr,
                          size_t *stacksize) {
    if (!attr || !stackaddr || !stacksize) return EINVAL;
    *stackaddr = attr->_stackaddr;
    *stacksize = attr->_stacksize;
    return 0;
}

int pthread_attr_setscope(pthread_attr_t *attr, int scope) {
    if (!attr) return EINVAL;
    attr->_scope = scope;
    return 0;
}

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched) {
    if (!attr) return EINVAL;
    attr->_inheritsched = inheritsched;
    return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy) {
    if (!attr) return EINVAL;
    attr->_schedpolicy = policy;
    return 0;
}

int pthread_attr_setschedparam(pthread_attr_t *attr,
                               const struct sched_param *param) {
    if (!attr || !param) return EINVAL;
    attr->_schedparam = *param;
    return 0;
}

int pthread_attr_getschedparam(const pthread_attr_t *attr,
                               struct sched_param *param) {
    if (!attr || !param) return EINVAL;
    *param = attr->_schedparam;
    return 0;
}

int pthread_attr_getschedpolicy(const pthread_attr_t *attr, int *policy) {
    if (!attr || !policy) return EINVAL;
    *policy = attr->_schedpolicy;
    return 0;
}

int pthread_setschedparam(pthread_t thread, int policy,
                          const struct sched_param *param) {
    (void)thread; (void)policy; (void)param;
    return 0;
}

int pthread_getschedparam(pthread_t thread, int *policy,
                          struct sched_param *param) {
    (void)thread;
    if (policy) *policy = SCHED_OTHER;
    if (param) param->sched_priority = 0;
    return 0;
}

int sched_get_priority_min(int policy) {
    (void)policy; return 0;
}

int sched_get_priority_max(int policy) {
    (void)policy; return 99;
}

/* -----------------------------------------------------------------------
 * Barrier (pthread_barrier_*)
 * ----------------------------------------------------------------------- */
int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attr,
                         unsigned int count) {
    if (!barrier || count == 0) return EINVAL;
    (void)attr;
    pthread_mutex_init(&barrier->_mutex, NULL);
    pthread_cond_init(&barrier->_cond, NULL);
    barrier->_count = 0;
    barrier->_total = count;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    if (!barrier) return EINVAL;
    pthread_mutex_destroy(&barrier->_mutex);
    pthread_cond_destroy(&barrier->_cond);
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
    if (!barrier) return EINVAL;
    pthread_mutex_lock(&barrier->_mutex);
    barrier->_count++;
    if (barrier->_count >= barrier->_total) {
        barrier->_count = 0;
        pthread_cond_broadcast(&barrier->_cond);
        pthread_mutex_unlock(&barrier->_mutex);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    pthread_cond_wait(&barrier->_cond, &barrier->_mutex);
    pthread_mutex_unlock(&barrier->_mutex);
    return 0;
}

/* -----------------------------------------------------------------------
 * Spinlock (pthread_spin_*)
 * ----------------------------------------------------------------------- */
int pthread_spin_init(pthread_spinlock_t *lock, int pshared) {
    if (!lock) return EINVAL;
    (void)pshared;
    *lock = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;
    return 0;
}

int pthread_spin_lock(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) __builtin_ia32_pause();
    }
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;
    return __sync_lock_test_and_set(lock, 1) ? EBUSY : 0;
}

int pthread_spin_unlock(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;
    __sync_lock_release(lock);
    return 0;
}
