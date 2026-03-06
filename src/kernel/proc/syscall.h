#ifndef TATER_SYSCALL_H
#define TATER_SYSCALL_H

#include <stdint.h>

void syscall_init(void);
void syscall_shm_process_exit(uint32_t pid);

#endif
