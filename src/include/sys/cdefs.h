/*
 * TaterTOS64v3 — <sys/cdefs.h>
 *
 * Stub for partition_alloc shim layer.
 */

#ifndef _TATERTOS_SYS_CDEFS_H
#define _TATERTOS_SYS_CDEFS_H

#ifdef __THROW
#  undef __THROW
#endif
#define __THROW

#ifndef __attribute_pure__
#define __attribute_pure__ __attribute__((__pure__))
#endif

#ifndef __attribute_malloc__
#define __attribute_malloc__ __attribute__((__malloc__))
#endif

#ifndef __attribute_alloc_size__
#define __attribute_alloc_size__(...) __attribute__((__alloc_size__(__VA_ARGS__)))
#endif

#ifndef __attribute_alloc_align__
#define __attribute_alloc_align__(x) __attribute__((__alloc_align__(x)))
#endif

#endif /* _TATERTOS_SYS_CDEFS_H */
