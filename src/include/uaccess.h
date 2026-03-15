#ifndef _TATERTOS_UACCESS_H
#define _TATERTOS_UACCESS_H

/*
 * TaterTOS64v3 — User-space memory access helpers
 *
 * All kernel code that reads from or writes to user pointers must go
 * through these helpers so validation is never skipped.
 *
 * Return: 0 on success, -EFAULT on invalid pointer.
 */

#include <stdint.h>

struct fry_process; /* forward declaration */

/* Validate that [ptr, ptr+len) is within the user address space. */
int user_ptr_ok(uint64_t ptr, uint64_t len);

/* Check that [ptr, ptr+len) is mapped and accessible. */
int user_buf_mapped(struct fry_process *p, uint64_t ptr, uint64_t len);
int user_buf_writable(struct fry_process *p, uint64_t ptr, uint64_t len);

/* Copy len bytes from user address src to kernel buffer dst.
   Returns 0 on success, -EFAULT if the source range is invalid. */
int copyin(struct fry_process *p, uint64_t src_user, void *dst_kern, uint64_t len);

/* Copy len bytes from kernel buffer src to user address dst.
   Returns 0 on success, -EFAULT if the dest range is invalid. */
int copyout(struct fry_process *p, const void *src_kern, uint64_t dst_user, uint64_t len);

/* Copy a NUL-terminated string from user space. Returns 0 on success. */
int copy_user_string(struct fry_process *p, uint64_t uptr, char *dst, uint32_t max);

#endif /* _TATERTOS_UACCESS_H */
