/*
 * limits.h shim — standard C limits for x86_64
 */
#ifndef _TATER_SHIM_LIMITS_H
#define _TATER_SHIM_LIMITS_H

#define CHAR_BIT     8
#define SCHAR_MIN    (-128)
#define SCHAR_MAX    127
#define UCHAR_MAX    255
#define CHAR_MIN     SCHAR_MIN
#define CHAR_MAX     SCHAR_MAX
#define SHRT_MIN     (-32768)
#define SHRT_MAX     32767
#define USHRT_MAX    65535
#define INT_MIN      (-2147483647 - 1)
#define INT_MAX      2147483647
#define UINT_MAX     4294967295U
#define LONG_MIN     (-9223372036854775807L - 1L)
#define LONG_MAX     9223372036854775807L
#define ULONG_MAX    18446744073709551615UL
#define LLONG_MIN    (-9223372036854775807LL - 1LL)
#define LLONG_MAX    9223372036854775807LL
#define ULLONG_MAX   18446744073709551615ULL

#ifndef SIZE_MAX
#define SIZE_MAX     ULONG_MAX
#endif

/* PATH_MAX — not a real POSIX value, but some code checks for it */
#define PATH_MAX     256

/* MB_LEN_MAX */
#define MB_LEN_MAX   4

#endif
