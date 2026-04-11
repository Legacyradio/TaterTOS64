/*
 * config.h — Opus configuration for TaterTOS64v3
 * Fixed-point build, no SIMD (for now), decoder-focused.
 */

#ifndef OPUS_CONFIG_H
#define OPUS_CONFIG_H

#define OPUS_BUILD 1
#define HAVE_CONFIG_H 1

/* Use fixed-point math (no floating-point dependency for decode) */
/* Actually, use float — our libc has full math library */
/* #define FIXED_POINT 1 */

/* Package info */
#define PACKAGE_VERSION "1.5.2"

/* Disable encoder-only features we don't use */
/* (but include source for link compatibility) */

/* Standard C features we have */
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_MEMORY_H 1

/* We don't have these */
/* #define HAVE_DLFCN_H 1 */
/* #define HAVE_ALLOCA_H 1 */
/* #define HAVE_LRINT 1 */
/* #define HAVE_LRINTF 1 */

/* Disable runtime CPU detection for now — plain C */
/* #define OPUS_HAVE_RTCD 1 */
/* #define OPUS_X86_MAY_HAVE_SSE 1 */
/* #define OPUS_X86_MAY_HAVE_SSE2 1 */
/* #define OPUS_X86_MAY_HAVE_SSE4_1 1 */
/* #define OPUS_X86_MAY_HAVE_AVX 1 */

/* Restrict keyword */
#define OPUS_RESTRICT __restrict__

/* Inline */
#define OPUS_INLINE inline

/* No custom modes (saves code size) */
/* #define CUSTOM_MODES 1 */

/* Enable assertions in debug, disable in release */
/* #define FUZZING 1 */
/* #define ENABLE_ASSERTIONS 1 */

/* VAR_ARRAYS: use variable-length arrays (C99 feature, GCC supports it) */
#define VAR_ARRAYS 1

#endif /* OPUS_CONFIG_H */
