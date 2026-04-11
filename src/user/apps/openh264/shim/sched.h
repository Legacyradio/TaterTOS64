#ifndef _TATER_SCHED_H
#define _TATER_SCHED_H
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_OTHER 0
struct sched_param { int sched_priority; };
static inline int sched_yield(void) { return 0; }
#endif
