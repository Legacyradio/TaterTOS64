#ifndef _TATER_OH264_SEMAPHORE_H
#define _TATER_OH264_SEMAPHORE_H
typedef struct { int dummy; } sem_t;
static inline int sem_init(sem_t *s, int p, unsigned int v) { (void)s;(void)p;(void)v; return 0; }
static inline int sem_destroy(sem_t *s) { (void)s; return 0; }
static inline int sem_wait(sem_t *s) { (void)s; return 0; }
static inline int sem_post(sem_t *s) { (void)s; return 0; }
static inline int sem_timedwait(sem_t *s, const void *t) { (void)s;(void)t; return 0; }
#endif
