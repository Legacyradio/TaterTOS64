/*
 * TaterTOS64v3 — <fenv.h>
 *
 * POSIX floating-point environment.
 */

#ifndef _TATERTOS_FENV_H
#define _TATERTOS_FENV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t fexcept_t;

typedef struct {
    uint32_t __control_word;
    uint32_t __unused;
} fenv_t;

#define FE_INVALID      0x01
#define FE_DIVBYZERO    0x04
#define FE_OVERFLOW     0x08
#define FE_UNDERFLOW    0x10
#define FE_INEXACT      0x20
#define FE_ALL_EXCEPT   (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

#define FE_TONEAREST    0x000
#define FE_DOWNWARD     0x400
#define FE_UPWARD       0x800
#define FE_TOWARDZERO   0xC00

extern const fenv_t __fenv_dfl_env;
#define FE_DFL_ENV (&__fenv_dfl_env)

int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t *flagp, int excepts);
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t *flagp, int excepts);
int fetestexcept(int excepts);
int fegetround(void);
int fesetround(int round);
int fegetenv(fenv_t *envp);
int feholdexcept(fenv_t *envp);
int fesetenv(const fenv_t *envp);
int feupdateenv(const fenv_t *envp);

#ifdef __cplusplus
}
#endif

#endif
