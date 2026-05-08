/*
 * TaterTOS64v3 — <sys/uio.h>
 *
 * Scatter/gather I/O declarations backed by SYS_READV and SYS_WRITEV.
 */

#ifndef _TATERTOS_SYS_UIO_H
#define _TATERTOS_SYS_UIO_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#endif
