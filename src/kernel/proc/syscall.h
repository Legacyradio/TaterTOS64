#ifndef TATER_SYSCALL_H
#define TATER_SYSCALL_H

#include <stdint.h>

struct fry_process;

void syscall_init(void);
void syscall_init_ap(uint32_t cpu);
void syscall_shm_process_exit(uint32_t pid);
void syscall_vm_process_exit(struct fry_process *p);

/* Demand-page a not-present user fault inside a reserved anonymous mmap
 * region (lazy backing for MAP_NORESERVE, e.g. JSC's gigacage). Called from
 * the #PF handler. Returns 1 if a page was committed (retry the instruction),
 * 0 to fall through to the USER FAULT kill. */
int lx_try_demand_page(struct fry_process *p, uint64_t fault_va, uint64_t err);

/* fry1387: demand stack growth — extend a thread's stack VMA down to a fault
 * just below it (Linux GROWSDOWN emulation). rsp = faulting frame's RSP. */
int lx_try_grow_stack(struct fry_process *p, uint64_t fault_va, uint64_t rsp,
                      uint64_t err);

/* Try to deliver a Unix signal from an exception context (not syscall return).
 * Modifies the exception frame to jump to the user's signal handler instead
 * of killing the process. Returns 1 if signal was delivered (caller should
 * iretq and not kill), 0 if no handler exists (caller should kill). */
int lx_deliver_signal_from_exception(struct fry_process *cur, uint64_t vector,
                                     uint64_t *exc_frame);

#endif
