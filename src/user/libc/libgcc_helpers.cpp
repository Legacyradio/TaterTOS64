/*
 * TaterTOS64v3 — libgcc helper stubs.
 *
 * GCC emits calls to libgcc helpers for some operations the freestanding
 * cross-compiler doesn't have a hardware instruction for. We provide
 * the minimum set AK + Ladybird need to link.
 *
 * Origin log: logs/fry842.txt
 *
 * Each helper is documented; most are one-liners that hand off to a
 * hardware operation or compiler builtin.
 */

#include <stdint.h>

extern "C" {

/*
 * __extendhfdf2 — convert _Float16 (IEEE binary16) to double.
 * Used by AK/Format.cpp's Formatter<_Float16>. We don't actually
 * use _Float16 at runtime (the formatter only gets instantiated
 * because it's a template; the real path is float/double), but
 * the linker still wants the symbol.
 *
 * Implementation: GCC's `__builtin_convertvector` doesn't help
 * for scalar; cast goes through a union to extract the 16-bit
 * pattern, then expand by hand. For our purposes we can approximate
 * via the compiler's own __extendhfsf2 helper into float and then
 * float→double, but the cross-compiler doesn't ship those either
 * in freestanding mode. Implement directly:
 */
double __extendhfdf2(_Float16 x) {
    return static_cast<double>(x);
}

float __extendhfsf2(_Float16 x) {
    return static_cast<float>(x);
}

_Float16 __truncdfhf2(double x) {
    return static_cast<_Float16>(x);
}

_Float16 __truncsfhf2(float x) {
    return static_cast<_Float16>(x);
}

/*
 * __bfloat16-related (gcc 15 added bfloat16 helper symbols even when
 * we don't use them). Provide stubs.
 */
double __extendbfdf2(__bf16 x) {
    return static_cast<double>(x);
}

float __extendbfsf2(__bf16 x) {
    return static_cast<float>(x);
}

}  // extern "C"
