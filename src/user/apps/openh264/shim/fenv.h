/*
 * fenv.h shim — floating-point environment for QuickJS
 * Maps to TaterTOS math.c fenv implementations.
 */
#ifndef _TATER_SHIM_FENV_H
#define _TATER_SHIM_FENV_H

typedef unsigned int fenv_t;
typedef unsigned int fexcept_t;

#define FE_TONEAREST  0x000
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xC00

#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

#define FE_DFL_ENV    ((const fenv_t *)0)

int fegetround(void);
int fesetround(int round);
int feclearexcept(int excepts);
int feraiseexcept(int excepts);
int fetestexcept(int excepts);
int fegetenv(fenv_t *envp);
int fesetenv(const fenv_t *envp);
int feholdexcept(fenv_t *envp);
int feupdateenv(const fenv_t *envp);
int fegetexceptflag(fexcept_t *flagp, int excepts);
int fesetexceptflag(const fexcept_t *flagp, int excepts);

#endif
