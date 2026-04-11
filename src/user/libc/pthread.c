/*
 * pthread.c — POSIX threads API wrapper over fry_thread/fry_mutex primitives
 *
 * Phase 8: pthread API needed by NSPR and NSS. Maps directly onto existing
 * TaterTOS threading primitives (fry_thread_create, fry_mutex, fry_cond, etc.)
 *
 * All implementations are original TaterTOS code.
 */

#include "libc.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * pthread_t — wraps fry_thread
 * ----------------------------------------------------------------------- */

struct pthread_start_ctx {
    void *(*fn)(void *);
    void *arg;
};

static void pthread_attr_apply_defaults(pthread_attr_t *attr) {
    memset(attr, 0, sizeof(*attr));
    attr->_detachstate = PTHREAD_CREATE_JOINABLE;
    attr->_stacksize = 2 * 1024 * 1024;
    attr->_stackaddr = 0;
    attr->_scope = PTHREAD_SCOPE_SYSTEM;
    attr->_inheritsched = PTHREAD_INHERIT_SCHED;
    attr->_schedpolicy = SCHED_OTHER;
    attr->_schedparam.sched_priority = 0;
}

static void pthread_trampoline(void *opaque) {
    struct pthread_start_ctx ctx = *(struct pthread_start_ctx *)opaque;
    free(opaque);
    ctx.fn(ctx.arg);
    /* Return value discarded — fry_thread_exit(0) is called by the
       fry_thread trampoline after this function returns. */
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;
    if (!thread || !start_routine) return EINVAL;

    struct pthread_start_ctx *ctx = (struct pthread_start_ctx *)malloc(sizeof(*ctx));
    if (!ctx) return ENOMEM;
    ctx->fn = start_routine;
    ctx->arg = arg;

    thread->_ctx = ctx;

    long rc = fry_thread_create(&thread->_thr, pthread_trampoline, ctx);
    if (rc < 0) {
        free(ctx);
        thread->_ctx = 0;
        return (int)(-rc);
    }
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    int exit_code = 0;
    long rc = fry_thread_join(&thread._thr, &exit_code);
    if (rc < 0) return (int)(-rc);

    if (retval) *retval = (void *)(intptr_t)exit_code;
    return 0;
}

int pthread_detach(pthread_t thread) {
    /* TaterTOS doesn't support detached threads yet; allow the call but no-op */
    (void)thread;
    return 0;
}

pthread_t pthread_self(void) {
    pthread_t self;
    memset(&self, 0, sizeof(self));
    if (fry_thread_current(&self._thr) < 0) {
        self._thr.tid = (uint32_t)fry_gettid();
    }
    return self;
}

int pthread_equal(pthread_t t1, pthread_t t2) {
    return t1._thr.tid == t2._thr.tid;
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
    long self_tid;

    if (!attr) return EINVAL;

    pthread_attr_apply_defaults(attr);
    self_tid = fry_gettid();
    if ((!thread._thr.stack_base || thread._thr.stack_len == 0) &&
        self_tid > 0 && thread._thr.tid == (uint32_t)self_tid) {
        thread = pthread_self();
    } else if (thread._thr.tid == 0) {
        thread = pthread_self();
    }

    attr->_stackaddr = thread._thr.stack_base;
    if (thread._thr.stack_len != 0) attr->_stacksize = thread._thr.stack_len;
    return 0;
}

void pthread_yield(void) {
    (void)sched_yield();
}

/* -----------------------------------------------------------------------
 * pthread_mutex — wraps fry_mutex
 * ----------------------------------------------------------------------- */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    if (!mutex) return EINVAL;
    mutex->_m = (fry_mutex_t)FRY_MUTEX_INIT;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    return 0; /* no resources to free */
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    int rc = fry_mutex_lock(&mutex->_m);
    return (rc < 0) ? -rc : 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    int rc = fry_mutex_trylock(&mutex->_m);
    if (rc == -EBUSY) return EBUSY;
    return (rc < 0) ? -rc : 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    int rc = fry_mutex_unlock(&mutex->_m);
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
    switch (type) {
        case PTHREAD_MUTEX_NORMAL:
        case PTHREAD_MUTEX_RECURSIVE:
        case PTHREAD_MUTEX_ERRORCHECK:
        case PTHREAD_MUTEX_ADAPTIVE_NP:
            attr->_type = type;
            return 0;
        default:
            return EINVAL;
    }
}

/* -----------------------------------------------------------------------
 * pthread_cond — wraps fry_cond
 * ----------------------------------------------------------------------- */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    if (!cond) return EINVAL;
    cond->_c = (fry_cond_t)FRY_COND_INIT;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (!cond || !mutex) return EINVAL;
    int rc = fry_cond_wait(&cond->_c, &mutex->_m);
    return (rc < 0) ? -rc : 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct fry_timespec *abstime) {
    if (!cond || !mutex) return EINVAL;

    /* Convert absolute time to relative timeout for futex */
    if (abstime) {
        struct fry_timespec now;
        fry_clock_gettime(FRY_CLOCK_MONOTONIC, &now);
        int64_t diff_ms = (abstime->tv_sec - now.tv_sec) * 1000 +
                          (abstime->tv_nsec - now.tv_nsec) / 1000000;
        if (diff_ms <= 0) return ETIMEDOUT;

        /* Manual cond wait with timeout */
        uint32_t seq = cond->_c.seq;
        int urc = fry_mutex_unlock(&mutex->_m);
        if (urc < 0) return -urc;

        long frc = fry_futex_wait(&cond->_c.seq, seq, (uint64_t)diff_ms);

        int lrc = fry_mutex_lock(&mutex->_m);
        if (lrc < 0) return -lrc;

        if (frc == -ETIMEDOUT) return ETIMEDOUT;
        return 0;
    }

    return pthread_cond_wait(cond, mutex);
}

int pthread_cond_signal(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    int rc = fry_cond_signal(&cond->_c);
    return (rc < 0) ? -rc : 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    int rc = fry_cond_broadcast(&cond->_c);
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
 * Uses a mutex + cond + reader count model.
 * ----------------------------------------------------------------------- */

int pthread_rwlock_init(pthread_rwlock_t *rwl, const pthread_rwlockattr_t *attr) {
    (void)attr;
    if (!rwl) return EINVAL;
    rwl->_m = (fry_mutex_t)FRY_MUTEX_INIT;
    rwl->_cond = (fry_cond_t)FRY_COND_INIT;
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
    fry_mutex_lock(&rwl->_m);
    while (rwl->_writer) {
        fry_cond_wait(&rwl->_cond, &rwl->_m);
    }
    rwl->_readers++;
    fry_mutex_unlock(&rwl->_m);
    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwl) {
    if (!rwl) return EINVAL;
    fry_mutex_lock(&rwl->_m);
    while (rwl->_writer || rwl->_readers > 0) {
        fry_cond_wait(&rwl->_cond, &rwl->_m);
    }
    rwl->_writer = 1;
    fry_mutex_unlock(&rwl->_m);
    return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwl) {
    if (!rwl) return EINVAL;
    fry_mutex_lock(&rwl->_m);
    if (rwl->_writer) {
        rwl->_writer = 0;
    } else if (rwl->_readers > 0) {
        rwl->_readers--;
    }
    fry_cond_broadcast(&rwl->_cond);
    fry_mutex_unlock(&rwl->_m);
    return 0;
}

/* -----------------------------------------------------------------------
 * pthread_once — wraps fry_once
 * ----------------------------------------------------------------------- */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) return EINVAL;
    int rc = fry_once(&once_control->_o, init_routine);
    return (rc < 0) ? -rc : 0;
}

/* -----------------------------------------------------------------------
 * pthread_key (TLS) — wraps fry_tls_key
 * ----------------------------------------------------------------------- */

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    (void)destructor; /* TaterTOS TLS doesn't support destructors yet */
    if (!key) return EINVAL;
    fry_tls_key_t fkey;
    int rc = fry_tls_key_create(&fkey);
    if (rc < 0) return -rc;
    *key = (pthread_key_t)fkey;
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    (void)key; /* no-op — TaterTOS TLS keys can't be freed */
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    int rc = fry_tls_set((fry_tls_key_t)key, (void *)value);
    return (rc < 0) ? -rc : 0;
}

void *pthread_getspecific(pthread_key_t key) {
    return fry_tls_get((fry_tls_key_t)key);
}

/* -----------------------------------------------------------------------
 * pthread_attr — minimal stubs
 * ----------------------------------------------------------------------- */

int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    pthread_attr_apply_defaults(attr);
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state) {
    if (!attr) return EINVAL;
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED) {
        return EINVAL;
    }
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
    *size = attr->_stacksize ? attr->_stacksize : (size_t)(2 * 1024 * 1024);
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr,
                          size_t *stacksize) {
    if (!attr || !stackaddr || !stacksize) return EINVAL;
    *stackaddr = attr->_stackaddr;
    *stacksize = attr->_stacksize ? attr->_stacksize : (size_t)(2 * 1024 * 1024);
    return 0;
}

int pthread_attr_setscope(pthread_attr_t *attr, int scope) {
    if (!attr) return EINVAL;
    if (scope != PTHREAD_SCOPE_SYSTEM && scope != PTHREAD_SCOPE_PROCESS) {
        return EINVAL;
    }
    attr->_scope = scope;
    return 0;
}

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched) {
    if (!attr) return EINVAL;
    if (inheritsched != PTHREAD_INHERIT_SCHED &&
        inheritsched != PTHREAD_EXPLICIT_SCHED) {
        return EINVAL;
    }
    attr->_inheritsched = inheritsched;
    return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy) {
    if (!attr) return EINVAL;
    switch (policy) {
        case SCHED_OTHER:
        case SCHED_FIFO:
        case SCHED_RR:
            attr->_schedpolicy = policy;
            return 0;
        default:
            return EINVAL;
    }
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
    (void)thread;
    if (!param) return EINVAL;
    switch (policy) {
        case SCHED_OTHER:
        case SCHED_FIFO:
        case SCHED_RR:
            return 0;
        default:
            return EINVAL;
    }
}

int pthread_getschedparam(pthread_t thread, int *policy,
                          struct sched_param *param) {
    (void)thread;
    if (!policy || !param) return EINVAL;
    *policy = SCHED_OTHER;
    param->sched_priority = 0;
    return 0;
}

int sched_get_priority_min(int policy) {
    switch (policy) {
        case SCHED_OTHER:
            return 0;
        case SCHED_FIFO:
        case SCHED_RR:
            return 1;
        default:
            errno = EINVAL;
            return -1;
    }
}

int sched_get_priority_max(int policy) {
    switch (policy) {
        case SCHED_OTHER:
            return 0;
        case SCHED_FIFO:
        case SCHED_RR:
            return 99;
        default:
            errno = EINVAL;
            return -1;
    }
}

int sched_yield(void) {
    (void)fry_sleep(0);
    return 0;
}
