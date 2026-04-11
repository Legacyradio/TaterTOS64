/*
 * tatertos_rt.c — 128-bit division runtime stubs for freestanding x86_64
 *
 * GCC generates calls to __udivti3 / __udivmodti4 / __umodti3 when
 * using __int128 or unsigned __int128 types. These are normally in
 * libgcc.a, but we're freestanding (no libgcc). Provide them here.
 */

#include <stddef.h>
#include <stdint.h>

typedef unsigned __int128 uint128_t;

uint128_t __udivti3(uint128_t a, uint128_t b) {
    if (b == 0) return 0;
    uint128_t result = 0;
    uint128_t remainder = 0;
    for (int i = 127; i >= 0; i--) {
        remainder = (remainder << 1) | ((a >> i) & 1);
        if (remainder >= b) {
            remainder -= b;
            result |= ((uint128_t)1 << i);
        }
    }
    return result;
}

uint128_t __udivmodti4(uint128_t a, uint128_t b, uint128_t *rem) {
    if (b == 0) { if (rem) *rem = 0; return 0; }
    uint128_t result = 0;
    uint128_t remainder = 0;
    for (int i = 127; i >= 0; i--) {
        remainder = (remainder << 1) | ((a >> i) & 1);
        if (remainder >= b) {
            remainder -= b;
            result |= ((uint128_t)1 << i);
        }
    }
    if (rem) *rem = remainder;
    return result;
}

uint128_t __umodti3(uint128_t a, uint128_t b) {
    if (b == 0) return 0;
    uint128_t remainder = 0;
    for (int i = 127; i >= 0; i--) {
        remainder = (remainder << 1) | ((a >> i) & 1);
        if (remainder >= b) remainder -= b;
    }
    return remainder;
}
