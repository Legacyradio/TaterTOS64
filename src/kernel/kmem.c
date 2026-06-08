/*
 * TaterTOS64v3 — freestanding kernel memory primitives.
 *
 * The kernel is built -ffreestanding and historically relied on GCC never
 * emitting libcalls to memcpy/memset/memmove. That is fragile: at -O2 the
 * compiler may lower aggregate copies or loop idioms into calls to these
 * symbols at any time (e.g. after an unrelated edit shifts inlining budgets in
 * a large translation unit such as proc/syscall.c). Providing the symbols once
 * makes the kernel link robust to those codegen decisions.
 *
 * Compiled with -fno-tree-loop-distribute-patterns (see Makefile rule) so the
 * loops below are NOT pattern-matched back into self-referential memcpy/memset
 * calls.
 *
 * Origin log: logs/fry1388.txt
 */

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i != 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}
