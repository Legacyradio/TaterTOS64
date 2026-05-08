/*
 * assert.h — TaterTOS64v3 assertion macro
 */

#ifndef _TATERTOS_ASSERT_H
#define _TATERTOS_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else

#ifdef __cplusplus
extern "C"
#endif
void __assert_fail(const char *expr, const char *file,
                   unsigned int line, const char *func);

#define assert(expr) \
    ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__, __func__))

#endif /* NDEBUG */

/* C11 static_assert support — C-only. In C++ static_assert is a
 * keyword and must NOT be macro-defined. (Hit at fry835: fast_float
 * tripped on `_Static_assert was not declared` because the macro
 * leaked into C++ TUs that included assert.h transitively.)
 */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

#endif /* _TATERTOS_ASSERT_H */
