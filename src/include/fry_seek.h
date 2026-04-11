/*
 * fry_seek.h — Shared seek/whence constants for lseek ABI
 *
 * Used by kernel (syscall handler) and userspace (libc).
 * Values match Linux for compatibility with ported code.
 */

#ifndef FRY_SEEK_H
#define FRY_SEEK_H

#define FRY_SEEK_SET  0   /* Set position to offset */
#define FRY_SEEK_CUR  1   /* Set position to current + offset */
#define FRY_SEEK_END  2   /* Set position to file size + offset */

#endif
