/*
 * TaterTOS64v3 — <semaphore.h>
 *
 * POSIX semaphores for abseil synchronization.
 */

#ifndef _TATERTOS_SEMAPHORE_H
#define _TATERTOS_SEMAPHORE_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    unsigned int    value;
} sem_t;

int    sem_init(sem_t *sem, int pshared, unsigned int value);
int    sem_destroy(sem_t *sem);
int    sem_wait(sem_t *sem);
int    sem_timedwait(sem_t *sem, const struct timespec *abs_timeout);
int    sem_trywait(sem_t *sem);
int    sem_post(sem_t *sem);
int    sem_getvalue(sem_t *sem, int *sval);
int    sem_clockwait(sem_t *sem, clockid_t clock_id, const struct timespec *abs_timeout);

#ifdef __cplusplus
}
#endif

#endif
