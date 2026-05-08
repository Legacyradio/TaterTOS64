/*
 * TaterTOS64v3 — <sys/eventfd.h>
 *
 * Linux-compatible eventfd declarations backed by TaterTOS SYS_EVENTFD.
 */

#ifndef _TATERTOS_SYS_EVENTFD_H
#define _TATERTOS_SYS_EVENTFD_H

#include <stdint.h>
#include <fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE  0x00000001
#define EFD_NONBLOCK   O_NONBLOCK
#define EFD_CLOEXEC    O_CLOEXEC

int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#ifdef __cplusplus
}
#endif

#endif
