#ifndef TATER_LIBC_H
#define TATER_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

/*
 * libc.h — TaterTOS C Library Internal Header
 *
 * This header contains declarations for standard C library functions
 * implemented in libc.c. It serves as the primary internal header for
 * the libc implementation itself.
 *
 * Public POSIX declarations belong in src/include/*.h.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Standard Library Functions (TaterTOS Implementation)
 * ----------------------------------------------------------------------- */

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
char *itoa(int value, char *buf, int base);
char *utoa(unsigned int value, char *buf, int base);
int atoi(const char *s);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *aligned_alloc(size_t alignment, size_t size);
int posix_memalign(void **memptr, size_t alignment, size_t size);
void *memalign(size_t alignment, size_t size);
void *valloc(size_t size);
void *pvalloc(size_t size);
size_t malloc_usable_size(void *ptr);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

int printf(const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);
int vsscanf(const char *str, const char *fmt, va_list ap);
int putchar(int c);
int puts(const char *s);
int getchar(void);
char *gets_bounded(char *buf, int max);

/* errno Support */
int *__errno_location(void);
#ifndef errno
#define errno (*__errno_location())
#endif

#ifdef __cplusplus
}
#endif

#endif
