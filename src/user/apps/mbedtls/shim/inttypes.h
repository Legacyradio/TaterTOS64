/*
 * inttypes.h shim — format macros for fixed-width integers
 */
#ifndef _TATER_SHIM_INTTYPES_H
#define _TATER_SHIM_INTTYPES_H

#include <stdint.h>

/* Format macros for printf — x86_64 LP64 model */
#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "ld"

#define PRIi8   "i"
#define PRIi16  "i"
#define PRIi32  "i"
#define PRIi64  "li"

#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "lu"

#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  "lx"

#define PRIX8   "X"
#define PRIX16  "X"
#define PRIX32  "X"
#define PRIX64  "lX"

#define PRIo8   "o"
#define PRIo16  "o"
#define PRIo32  "o"
#define PRIo64  "lo"

/* Scan macros */
#define SCNd32  "d"
#define SCNd64  "ld"
#define SCNu32  "u"
#define SCNu64  "lu"
#define SCNx32  "x"
#define SCNx64  "lx"

/* intmax_t */
typedef long intmax_t;
typedef unsigned long uintmax_t;

#ifndef INTMAX_MAX
#define INTMAX_MAX   LONG_MAX
#endif
#ifndef INTMAX_MIN
#define INTMAX_MIN   LONG_MIN
#endif
#ifndef UINTMAX_MAX
#define UINTMAX_MAX  ULONG_MAX
#endif

intmax_t strtoimax(const char *nptr, char **endptr, int base);
uintmax_t strtoumax(const char *nptr, char **endptr, int base);

#endif
