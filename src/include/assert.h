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

/* C11 static_assert support */
#ifndef static_assert
#define static_assert _Static_assert
#endif

#endif /* _TATERTOS_ASSERT_H */
