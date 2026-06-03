/*
 * TaterTOS64v3 — <byteswap.h>
 *
 * Byte-swap functions (glibc-compatible).
 * Thin wrapper; the implementations live in <endian.h>.
 */

#ifndef _TATERTOS_BYTESWAP_H
#define _TATERTOS_BYTESWAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define bswap_16(x) __builtin_bswap16(x)
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)

#ifdef __cplusplus
}
#endif

#endif
