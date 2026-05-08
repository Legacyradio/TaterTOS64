#ifndef _MALLOC_H
#define _MALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>

/* All malloc.h extension functions are declared in <stdlib.h> on TaterTOS.
   This header exists for compatibility with code that includes <malloc.h>
   directly (e.g. Chromium's partition_alloc allocator shim). */

void cfree(void *ptr);
size_t malloc_usable_size(void *ptr);
size_t malloc_size(const void *ptr);

/* Convenience alias — Chromium partition_alloc uses malloc_good_size */
#define malloc_good_size malloc_usable_size

/* Get number of available CPUs — bare-metal system, always 1 */
int malloc_get_cpu_count(void);

#ifdef __cplusplus
}
#endif

#endif /* _MALLOC_H */
