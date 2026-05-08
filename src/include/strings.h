/*
 * TaterTOS64v3 — <strings.h>
 *
 * POSIX strings.h surface. Case-insensitive string operations and
 * legacy BSD memory ops. The canonical impls live in
 * src/user/libc/{libc,string_ext}.c.
 *
 * Origin log: logs/fry833.txt
 * Triggered by: simdutf/portability.h:12 (Ladybird port).
 */

#ifndef _TATERTOS_STRINGS_H
#define _TATERTOS_STRINGS_H

#include <stddef.h>      /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

int   strcasecmp(const char *a, const char *b);
int   strncasecmp(const char *a, const char *b, size_t n);

/*
 * Legacy BSD memory ops. POSIX-2008 marks these obsolete in favour
 * of memset/memcpy/memcmp from <string.h>, but upstream code still
 * occasionally references them. Implemented as inline forwarders.
 */
#include <string.h>

static inline void bzero(void *s, size_t n) {
    (void)memset(s, 0, n);
}

static inline void bcopy(const void *src, void *dst, size_t n) {
    (void)memmove(dst, src, n);
}

static inline int bcmp(const void *a, const void *b, size_t n) {
    return memcmp(a, b, n);
}

static inline char *index(const char *s, int c) {
    return strchr(s, c);
}

static inline char *rindex(const char *s, int c) {
    return strrchr(s, c);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_STRINGS_H */
