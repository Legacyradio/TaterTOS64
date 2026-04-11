#ifndef TATER_SYSCALL_H
#define TATER_SYSCALL_H

#include <stdint.h>

struct fry_process;

void syscall_init(void);
void syscall_init_ap(uint32_t cpu);
void syscall_shm_process_exit(uint32_t pid);
void syscall_vm_process_exit(struct fry_process *p);

#endif
