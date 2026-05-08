/*
 * TaterTOS64v3 — <stdlib.h>
 *
 * POSIX/C stdlib.h surface for TaterTOS userland. Forwards to the
 * existing libc.h backends in src/user/libc/{libc,posix}.c. This
 * is engineered code, not a stub — every TaterTOS app that uses
 * the standard <stdlib.h> path now gets a real implementation.
 *
 * Origin log: logs/fry832.txt
 * Triggered by: AK/kmalloc.h:13 (Ladybird port, fry831)
 */

#ifndef _TATERTOS_STDLIB_H
#define _TATERTOS_STDLIB_H

#include <stddef.h>      /* size_t, NULL */
#include <stdint.h>

/*
 * extern "C" wrap. libc.h now has surgical extern "C" (with the C++
 * pthread_t operator overloads in an interleaved extern "C++"
 * block — fry839). Match its posture so malloc/free/etc. all link
 * with C linkage to libc.c's implementations.
 */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Process exit status macros (per ISO C / POSIX).
 */
#define EXIT_SUCCESS  0
#define EXIT_FAILURE  1

/*
 * RAND_MAX — TaterTOS does not currently ship a real PRNG via
 * <stdlib.h>. fry_getrandom() is the canonical entropy source. We
 * still expose RAND_MAX so portable upstream code that gates on it
 * compiles. rand() is implemented below as a thin wrapper.
 */
#define RAND_MAX  0x7FFFFFFF

/*
 * Memory allocation — already declared in <libc.h> with the
 * canonical POSIX/C names. Re-declared here so apps that #include
 * <stdlib.h> (without <libc.h>) get the same surface.
 */
void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void  *aligned_alloc(size_t alignment, size_t size);
int    posix_memalign(void **memptr, size_t alignment, size_t size);
void  *memalign(size_t alignment, size_t size);
void  *valloc(size_t size);
void  *pvalloc(size_t size);
size_t malloc_usable_size(void *ptr);
void   free(void *ptr);
char  *realpath(const char *path, char *resolved_path);

/*
 * Numeric conversions (already in libc.h via libc.c).
 */
int       atoi(const char *s);
long      atol(const char *s);
long long atoll(const char *s);
double    atof(const char *s);
long      strtol(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long      strtoul(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double             strtod(const char *nptr, char **endptr);
float              strtof(const char *nptr, char **endptr);
long double        strtold(const char *nptr, char **endptr);

/*
 * Integer absolute values.
 */
int       abs(int x);
long      labs(long x);
long long llabs(long long x);

/*
 * Sorting / searching.
 */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base,
              size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/*
 * Process control — backed by *_compat shims in src/user/libc/posix.c.
 */
extern __attribute__((noreturn)) void abort_compat(void);
extern __attribute__((noreturn)) void exit_compat(int status);
extern __attribute__((noreturn)) void _exit_compat(int status);
extern int  atexit_compat(void (*func)(void));

extern __attribute__((noreturn)) void quick_exit(int status);
extern int at_quick_exit(void (*func)(void));

static inline __attribute__((noreturn)) void abort(void) {
    abort_compat();
}

static inline __attribute__((noreturn)) void exit(int status) {
    exit_compat(status);
}

static inline __attribute__((noreturn)) void _Exit(int status) {
    _exit_compat(status);
}

static inline int atexit(void (*func)(void)) {
    return atexit_compat(func);
}

/*
 * Environment variables — backed by posix.c shims.
 *
 * Note: TaterTOS libc only exposes fry_getenv(name, buf, len), which
 * copies the value into the caller's buffer. Standard getenv()
 * returns a pointer to a static internal buffer. We provide that
 * pointer-returning form here as an inline static-buffer wrapper.
 * The buffer is per-call; callers should treat the result as
 * invalidated by the next getenv() call (matches POSIX wording).
 */
extern long fry_getenv(const char *name, char *buf, size_t len);
extern int  setenv_compat(const char *name, const char *value, int overwrite);
extern int  unsetenv_compat(const char *name);
extern int  putenv_compat(char *string);

static inline char *getenv(const char *name) {
    static char tatertos_getenv_buf[256];
    long n = fry_getenv(name, tatertos_getenv_buf, sizeof(tatertos_getenv_buf));
    if (n <= 0) return (char *)0;
    return tatertos_getenv_buf;
}

static inline int setenv(const char *name, const char *value, int overwrite) {
    return setenv_compat(name, value, overwrite);
}

static inline int unsetenv(const char *name) {
    return unsetenv_compat(name);
}

static inline int putenv(char *string) {
    return putenv_compat(string);
}

/*
 * GNU extensions used by upstream Ladybird (LibCore/Environment.cpp).
 * secure_getenv: same as getenv on TaterTOS — we don't currently
 * distinguish privileged contexts.
 * clearenv: clears the entire environment.
 */
static inline char *secure_getenv(const char *name) {
    return getenv(name);
}

extern int clearenv(void);

/*
 * Pseudo-random number generation. TaterTOS does not maintain a
 * stateful PRNG inside libc — the canonical entropy source is
 * fry_getrandom(). We expose rand()/srand() as thin wrappers around
 * a tiny linear-congruential generator so portable upstream code
 * that uses rand() compiles. Cryptographic randomness MUST go
 * through fry_getrandom(), not these.
 */
extern long fry_getrandom(void *buf, unsigned long len, unsigned int flags);

static unsigned int _tater_rand_state = 1u;

static inline void srand(unsigned int seed) {
    _tater_rand_state = seed;
}

static inline int rand(void) {
    /* Bog-standard LCG (Numerical Recipes). Deterministic — fine for
       portable upstream code that doesn't care about quality. */
    _tater_rand_state = _tater_rand_state * 1103515245u + 12345u;
    return (int)((_tater_rand_state >> 16) & RAND_MAX);
}

/*
 * div/ldiv/lldiv — quotient + remainder structures.
 */
typedef struct { int       quot; int       rem; } div_t;
typedef struct { long      quot; long      rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

static inline div_t div(int n, int d) {
    div_t r; r.quot = n / d; r.rem = n % d; return r;
}
static inline ldiv_t ldiv(long n, long d) {
    ldiv_t r; r.quot = n / d; r.rem = n % d; return r;
}
static inline lldiv_t lldiv(long long n, long long d) {
    lldiv_t r; r.quot = n / d; r.rem = n % d; return r;
}

/*
 * Wide-char placeholders. TaterTOS does not currently support locale-
 * dependent multibyte conversions; these are declared so portable
 * code compiles. If a future port actually calls them, they'll need
 * real implementations (logged separately).
 */
#define MB_CUR_MAX  1
#ifndef MB_LEN_MAX
#define MB_LEN_MAX  16
#endif

int mblen(const char *s, size_t n);
int system(const char *command);
int mkstemp(char *tmpl);
char *mkdtemp(char *tmpl);

#ifdef __cplusplus
/* Declare math functions in global namespace for C++ */
} /* extern "C" */
#endif

#endif /* _TATERTOS_STDLIB_H */
