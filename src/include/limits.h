/*
 * TaterTOS64v3 — <limits.h>
 *
 * ISO C integer limits plus the POSIX path/process limits that map to
 * the kernel-enforced FRY ceilings. Values are for the x86_64 LP64 ABI.
 */

#ifndef _TATERTOS_LIMITS_H_STD
#define _TATERTOS_LIMITS_H_STD

#include <fry_limits.h>

#ifndef CHAR_BIT
#  define CHAR_BIT __CHAR_BIT__
#endif

#ifndef SCHAR_MAX
#  define SCHAR_MAX __SCHAR_MAX__
#endif
#ifndef SCHAR_MIN
#  define SCHAR_MIN (-SCHAR_MAX - 1)
#endif
#ifndef UCHAR_MAX
#  define UCHAR_MAX (SCHAR_MAX * 2U + 1U)
#endif

#ifndef CHAR_MIN
#  ifdef __CHAR_UNSIGNED__
#    define CHAR_MIN 0
#  else
#    define CHAR_MIN SCHAR_MIN
#  endif
#endif
#ifndef CHAR_MAX
#  ifdef __CHAR_UNSIGNED__
#    define CHAR_MAX UCHAR_MAX
#  else
#    define CHAR_MAX SCHAR_MAX
#  endif
#endif

#ifndef SHRT_MAX
#  define SHRT_MAX __SHRT_MAX__
#endif
#ifndef SHRT_MIN
#  define SHRT_MIN (-SHRT_MAX - 1)
#endif
#ifndef USHRT_MAX
#  define USHRT_MAX (SHRT_MAX * 2U + 1U)
#endif

#ifndef INT_MAX
#  define INT_MAX __INT_MAX__
#endif
#ifndef INT_MIN
#  define INT_MIN (-INT_MAX - 1)
#endif
#ifndef UINT_MAX
#  define UINT_MAX (INT_MAX * 2U + 1U)
#endif

#ifndef LONG_MAX
#  define LONG_MAX __LONG_MAX__
#endif
#ifndef LONG_MIN
#  define LONG_MIN (-LONG_MAX - 1L)
#endif
#ifndef ULONG_MAX
#  define ULONG_MAX (LONG_MAX * 2UL + 1UL)
#endif

#ifndef LLONG_MAX
#  define LLONG_MAX __LONG_LONG_MAX__
#endif
#ifndef LLONG_MIN
#  define LLONG_MIN (-LLONG_MAX - 1LL)
#endif
#ifndef ULLONG_MAX
#  define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#endif

/*
 * TaterTOS libc currently implements UTF-8 multibyte conversion. Keep
 * MB_LEN_MAX aligned with MB_CUR_MAX in <stdlib.h>.
 */
#ifndef MB_LEN_MAX
#  define MB_LEN_MAX 4
#endif

#ifndef PATH_MAX
#  define PATH_MAX FRY_PATH_MAX
#endif
#ifndef NAME_MAX
#  define NAME_MAX FRY_NAME_MAX
#endif
#ifndef OPEN_MAX
#  define OPEN_MAX FRY_FD_MAX
#endif
#ifndef ARG_MAX
#  define ARG_MAX FRY_ARGS_BUFSZ
#endif
#ifndef PIPE_BUF
#  define PIPE_BUF FRY_PIPE_BUFSZ
#endif
#ifndef SSIZE_MAX
#  define SSIZE_MAX LONG_MAX
#endif

/* Default nice value (process priority baseline) */
#ifndef NZERO
#  define NZERO 20
#endif

#ifndef IOV_MAX
#  define IOV_MAX 1024
#endif

#ifndef _POSIX_ARG_MAX
#  define _POSIX_ARG_MAX 4096
#endif
#ifndef _POSIX_NAME_MAX
#  define _POSIX_NAME_MAX 14
#endif
#ifndef _POSIX_PATH_MAX
#  define _POSIX_PATH_MAX 256
#endif
#ifndef _POSIX_OPEN_MAX
#  define _POSIX_OPEN_MAX 20
#endif
#ifndef _POSIX_PIPE_BUF
#  define _POSIX_PIPE_BUF 512
#endif
#ifndef _POSIX_SSIZE_MAX
#  define _POSIX_SSIZE_MAX 32767
#endif

#endif /* _TATERTOS_LIMITS_H_STD */
