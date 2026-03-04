#ifndef TATER_SCHED_H
#define TATER_SCHED_H

#include <stdint.h>
#include "process.h"

int sched_init(void);
void sched_tick(void);
void sched_add(uint32_t pid);
void sched_remove(uint32_t pid);
void sched_yield(void);
void sched_sleep(uint32_t pid, uint64_t ms);
void sched_block(uint32_t pid);
void sched_wake(uint32_t pid);
struct fry_process *sched_current(void);
void sched_set_current(struct fry_process *p);

#endif
