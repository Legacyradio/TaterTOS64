/*
 * TaterTOS64v3 — <string.h>
 *
 * POSIX/C string.h surface for TaterTOS userland. All symbols are
 * already implemented in src/user/libc/{libc,string_ext}.c — this
 * header just re-exposes them under the canonical names.
 *
 * Origin log: logs/fry832.txt
 * Triggered by: AK/Format.h:16 (Ladybird port).
 */

#ifndef _TATERTOS_STRING_H
#define _TATERTOS_STRING_H

#include <stddef.h>      /* size_t, NULL */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Memory operations.
 */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

/*
 * String length / comparison.
 */
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);

/*
 * String copy / concatenation.
 */
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);

/*
 * Search.
 */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);

/*
 * Tokenization.
 */
char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);

/*
 * Allocation-returning helpers.
 */
char *strdup(const char *s);
char *strndup(const char *s, size_t n);

/*
 * Error message strings.
 */
char *strerror(int errnum);
int strerror_r(int errnum, char *buf, size_t buflen);

/*
 * Locale-dependent string functions.
 */
int strcoll(const char *a, const char *b);
size_t strxfrm(char *d, const char *s, size_t n);

/*
 * Wide character operations (needed by libc++ char_traits<wchar_t>).
 */
int      wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n);
wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_STRING_H */
