#ifndef TATER_SCHED_H
#define TATER_SCHED_H

#include <stdint.h>
#include "process.h"

int sched_init(void);
void sched_tick(void);
void sched_add(uint32_t pid);
void sched_remove(uint32_t pid);
void sched_remove_with_state(uint32_t pid, enum proc_state state);
void sched_yield(void);
void sched_sleep(uint32_t pid, uint64_t ms);
void sched_block(uint32_t pid);
int sched_block_futex(uint32_t pid, volatile const uint32_t *word,
                      uint32_t expected, uint64_t key,
                      uint64_t wake_time_ms);
void sched_wake(uint32_t pid);
uint32_t sched_wake_futex(uint64_t key, uint32_t max_wake, int32_t result);
void sched_block_poll(uint32_t pid, uint64_t wake_time_ms);
uint32_t sched_wake_poll_waiters(void);
struct fry_process *sched_current(void);
void sched_set_current(struct fry_process *p);
void sched_ap_start(uint64_t stack_top);
void *sched_percpu_ptr(uint32_t cpu);
int sched_ap_ready(void);

#endif
