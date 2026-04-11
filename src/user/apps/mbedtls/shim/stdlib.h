/*
 * stdlib.h shim — maps to TaterTOS64v3 libc
 */
#ifndef _TATER_SHIM_STDLIB_H
#define _TATER_SHIM_STDLIB_H

#include <stddef.h>

typedef long ssize_t;

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
int atoi(const char *s);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

int abs(int x);
long labs(long x);

size_t malloc_usable_size(void *ptr);

/* alloca — stack allocation */
#define alloca(size) __builtin_alloca(size)

/* abort — print message and exit */
void __attribute__((noreturn)) abort(void);

/* exit — maps to fry_exit */
void __attribute__((noreturn)) exit(int status);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif
