/*
 * TaterTOS64v3 — <endian.h>
 *
 * Byte-order conversion macros (glibc-compatible).
 * x86_64 is little-endian; all conversions are no-ops.
 */

#ifndef _TATERTOS_ENDIAN_H
#define _TATERTOS_ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412

#define __BYTE_ORDER __LITTLE_ENDIAN

#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#define PDP_ENDIAN    __PDP_ENDIAN
#define BYTE_ORDER    __BYTE_ORDER

#include <stdint.h>

static __inline__ uint16_t __bswap_16(uint16_t __x) {
    return (uint16_t)((__x >> 8) | (__x << 8));
}

static __inline__ uint32_t __bswap_32(uint32_t __x) {
    return __builtin_bswap32(__x);
}

static __inline__ uint64_t __bswap_64(uint64_t __x) {
    return __builtin_bswap64(__x);
}

/* Host-to-little / little-to-host (no-ops on x86_64) */
#define htobe16(x) __bswap_16(x)
#define htole16(x) (x)
#define be16toh(x) __bswap_16(x)
#define le16toh(x) (x)

#define htobe32(x) __bswap_32(x)
#define htole32(x) (x)
#define be32toh(x) __bswap_32(x)
#define le32toh(x) (x)

#define htobe64(x) __bswap_64(x)
#define htole64(x) (x)
#define be64toh(x) __bswap_64(x)
#define le64toh(x) (x)

#ifdef __cplusplus
}
#endif

#endif
