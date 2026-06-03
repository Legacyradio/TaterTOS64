/*
 * TaterTOS64v3 — <linux/unistd.h>
 *
 * Linux x86_64 syscall numbers for abseil's direct_mmap.
 */

#ifndef _TATERTOS_LINUX_UNISTD_H
#define _TATERTOS_LINUX_UNISTD_H

#define __NR_mmap    9
#define __NR_munmap  11
#define __NR_mmap2   192  /* compat, not used on x86_64 */

#define SYS_mmap     __NR_mmap
#define SYS_munmap   __NR_munmap
#define SYS_mmap2    __NR_mmap2

#define SYS_rt_sigprocmask  14

#endif
