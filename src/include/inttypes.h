/*
 * TaterTOS64v3 — <inttypes.h>
 *
 * POSIX/C inttypes.h. Mostly format-string macros (PRId64 etc.) used
 * with printf/scanf for the fixed-width integer types. Plus
 * imaxabs/imaxdiv/strtoimax/strtoumax which we forward to libc
 * implementations.
 *
 * Origin log: logs/fry835.txt
 */

#ifndef _TATERTOS_INTTYPES_H
#define _TATERTOS_INTTYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * On x86_64 GNU userland: int32_t is `int`, int64_t is `long`.
 * These format strings match the canonical glibc spellings.
 */
#define __PRI64_PREFIX  "l"
#define __PRIPTR_PREFIX "l"

/* signed decimal */
#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  __PRI64_PREFIX "d"
#define PRIdMAX __PRI64_PREFIX "d"
#define PRIdPTR __PRIPTR_PREFIX "d"

#define PRIi8   "i"
#define PRIi16  "i"
#define PRIi32  "i"
#define PRIi64  __PRI64_PREFIX "i"
#define PRIiMAX __PRI64_PREFIX "i"
#define PRIiPTR __PRIPTR_PREFIX "i"

/* unsigned decimal */
#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  __PRI64_PREFIX "u"
#define PRIuMAX __PRI64_PREFIX "u"
#define PRIuPTR __PRIPTR_PREFIX "u"

/* hex */
#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  __PRI64_PREFIX "x"
#define PRIxMAX __PRI64_PREFIX "x"
#define PRIxPTR __PRIPTR_PREFIX "x"

#define PRIX8   "X"
#define PRIX16  "X"
#define PRIX32  "X"
#define PRIX64  __PRI64_PREFIX "X"
#define PRIXMAX __PRI64_PREFIX "X"
#define PRIXPTR __PRIPTR_PREFIX "X"

/* octal */
#define PRIo8   "o"
#define PRIo16  "o"
#define PRIo32  "o"
#define PRIo64  __PRI64_PREFIX "o"
#define PRIoMAX __PRI64_PREFIX "o"

/* SCN macros for scanf */
#define SCNd8   "hhd"
#define SCNd16  "hd"
#define SCNd32  "d"
#define SCNd64  __PRI64_PREFIX "d"
#define SCNu8   "hhu"
#define SCNu16  "hu"
#define SCNu32  "u"
#define SCNu64  __PRI64_PREFIX "u"
#define SCNx8   "hhx"
#define SCNx16  "hx"
#define SCNx32  "x"
#define SCNx64  __PRI64_PREFIX "x"

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

intmax_t imaxabs(intmax_t i);
imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom);
intmax_t  strtoimax(const char *nptr, char **endptr, int base);
uintmax_t strtoumax(const char *nptr, char **endptr, int base);
intmax_t  wcstoimax(const wchar_t *nptr, wchar_t **endptr, int base);
uintmax_t wcstoumax(const wchar_t *nptr, wchar_t **endptr, int base);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_INTTYPES_H */
