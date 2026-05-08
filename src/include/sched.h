/*
 * TaterTOS64v3 — <sched.h>
 *
 * POSIX scheduling.
 */

#ifndef _TATERTOS_SCHED_H
#define _TATERTOS_SCHED_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * cpu_set_t — CPU affinity mask
 * ----------------------------------------------------------------------- */
#define __CPU_SETSIZE 1024
#define __CPU_BITS     (sizeof(unsigned long) * 8)

typedef struct {
    unsigned long __bits[__CPU_SETSIZE / __CPU_BITS];
} cpu_set_t;

#define CPU_ZERO(set) \
    do { \
        cpu_set_t *_s = (set); \
        for (int _i = 0; _i < (int)(__CPU_SETSIZE / __CPU_BITS); _i++) \
            _s->__bits[_i] = 0UL; \
    } while(0)

#define CPU_SET(cpu, set) \
    ((set)->__bits[(cpu) / __CPU_BITS] |= (1UL << ((cpu) % __CPU_BITS)))
#define CPU_CLR(cpu, set) \
    ((set)->__bits[(cpu) / __CPU_BITS] &= ~(1UL << ((cpu) % __CPU_BITS)))
#define CPU_ISSET(cpu, set) \
    (((set)->__bits[(cpu) / __CPU_BITS] & (1UL << ((cpu) % __CPU_BITS))) != 0)
#define CPU_COUNT(set) ({ \
    cpu_set_t *_s = (set); \
    int _cnt = 0; \
    for (int _i = 0; _i < (int)(__CPU_SETSIZE / __CPU_BITS); _i++) \
        _cnt += __builtin_popcountl(_s->__bits[_i]); \
    _cnt; \
})

struct sched_param {
    int sched_priority;
};

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

int sched_yield(void);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);

/* CPU affinity */
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);

#ifdef __cplusplus
}
#endif

#endif
