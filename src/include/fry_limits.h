#ifndef _TATERTOS_LIMITS_H
#define _TATERTOS_LIMITS_H

/*
 * TaterTOS64v3 — System-wide resource limits
 *
 * These values define hard ceilings enforced by the kernel.
 * Userspace can query them but cannot raise them.
 */

#define FRY_FD_MAX          64    /* max open file descriptors per process */
#define FRY_PROC_MAX       256    /* max simultaneous processes */
#define FRY_VMREG_MAX      256    /* max VM regions per process */
#define FRY_SHM_MAX        128    /* max global shared-memory objects */
#define FRY_VM_SHARED_MAX  128    /* max global anonymous shared VM objects */
#define FRY_PATH_MAX       128    /* max path length in bytes */
#define FRY_NAME_MAX       127    /* max file name component length */
#define FRY_PAGE_SIZE     4096    /* page size in bytes */

/* Pipe limits */
#define FRY_PIPE_MAX       128    /* max global kernel pipe objects */
#define FRY_PIPE_BUFSZ    4096    /* per-pipe ring buffer size */

/* Process argument/environment limits */
#define FRY_ARGV_MAX        32    /* max arguments per process */
#define FRY_ENV_MAX         32    /* max environment variables per process */
#define FRY_ARGS_BUFSZ    2048    /* total args+env string buffer per process */

/* Poll limits */
#define FRY_POLL_MAX        64    /* max fds in a single poll() call */

/* Socket limits */
#define FRY_SOCK_MAX        16    /* max global kernel socket objects */
#define FRY_SOCK_UDP_RXMAX   4    /* max queued UDP datagrams per socket */
#define FRY_SOCK_UDP_PKTSZ 512    /* max UDP datagram payload in queue */

/* User virtual address space boundaries */
#define FRY_VM_USER_BASE   0x0000100000000000ULL
#define FRY_VM_USER_LIMIT  0x00007FFF00000000ULL

#endif /* _TATERTOS_LIMITS_H */
