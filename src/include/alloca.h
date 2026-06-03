/*
 * TaterTOS64v3 — <alloca.h>
 *
 * Stack-based allocation (GCC builtin wrapper).
 */

#ifndef _TATERTOS_ALLOCA_H
#define _TATERTOS_ALLOCA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define alloca(size) __builtin_alloca(size)

#ifdef __cplusplus
}
#endif

#endif
