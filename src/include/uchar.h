/*
 * TaterTOS64v3 — <uchar.h>
 *
 * C11 Unicode utilities. Minimal stub — char16_t/char32_t
 * are GCC builtins in C11 mode.
 */

#ifndef _TATERTOS_UCHAR_H
#define _TATERTOS_UCHAR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <wchar.h>

/* char16_t and char32_t are built-in types in C11/C++11.
   Only declare the conversion function prototypes. */

#ifndef __cplusplus
typedef __CHAR16_TYPE__ char16_t;
typedef __CHAR32_TYPE__ char32_t;
#endif

size_t mbrtoc16(char16_t *__restrict pc16, const char *__restrict s,
                size_t n, mbstate_t *__restrict ps);
size_t c16rtomb(char *__restrict s, char16_t c16,
                mbstate_t *__restrict ps);
size_t mbrtoc32(char32_t *__restrict pc32, const char *__restrict s,
                size_t n, mbstate_t *__restrict ps);
size_t c32rtomb(char *__restrict s, char32_t c32,
                mbstate_t *__restrict ps);

#ifdef __cplusplus
}
#endif

#endif
