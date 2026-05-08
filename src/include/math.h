#ifndef _TATERTOS_MATH_H
#define _TATERTOS_MATH_H

#include <float.h>

/* C99 floating-point classification constants */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

typedef float  float_t;
typedef double double_t;

#ifdef __cplusplus
/*
 * NOTE: libc++ provides its own std::isnan, std::isinf, etc.
 * We do not define them here to avoid overload ambiguity.
 * In C mode, use the macros below.
 */
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)
#ifndef NAN
#define NAN       __builtin_nanf("")
#endif
#ifndef INFINITY
#define INFINITY  __builtin_inff()
#endif

/* Math constants — POSIX/C99 */
#define M_E         2.7182818284590452354
#define M_LOG2E     1.4426950408889634074
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_2_SQRTPI  1.12837916709551257390
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440

extern "C" {
#else
#ifndef NAN
#define NAN       __builtin_nanf("")
#endif
#ifndef INFINITY
#define INFINITY  __builtin_inff()
#endif
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

#define M_E         2.7182818284590452354
#define M_LOG2E     1.4426950408889634074
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_2_SQRTPI  1.12837916709551257390
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440
#endif

double fabs(double x);       float  fabsf(float x);
double fmin(double x, double y);
double fmax(double x, double y);
float  fminf(float x, float y);
float  fmaxf(float x, float y);
double copysign(double mag, double sgn);
double floor(double x);      float  floorf(float x);
double ceil(double x);       float  ceilf(float x);
double round(double x);      float  roundf(float x);
long   lround(double x);     long   lroundf(float x);
long long llround(double x); long long llroundf(float x);
double trunc(double x);      float  truncf(float x);
double fmod(double x, double y);
float  fmodf(float x, float y);
double modf(double x, double *iptr);
float modff(float x, float *iptr);
double remainder(double x, double y);
double sqrt(double x);       float  sqrtf(float x);
double cbrt(double x);       float  cbrtf(float x);
double hypot(double x, double y);
double exp(double x);        float  expf(float x);
double exp2(double x);
double expm1(double x);
double ldexp(double x, int exp_);
double frexp(double x, int *exp_);
double scalbn(double x, int n);
double log(double x);        float  logf(float x);
double log2(double x);       float  log2f(float x);
double log10(double x);      float  log10f(float x);
double log1p(double x);
double pow(double base, double exponent);
float  powf(float base, float exp_);
double sin(double x);        float  sinf(float x);
double cos(double x);        float  cosf(float x);
double tan(double x);        float  tanf(float x);
double fma(double x, double y, double z);
float  fmaf(float x, float y, float z);
double asin(double x);
double acos(double x);
float acosf(float x);
double atan2(double y, double x);
float  atan2f(float y, float x);
long   lrint(double x);
long long llrint(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double nextafter(double x, double y);
float  nextafterf(float x, float y);
double nan(const char* tagp);
float nanf(const char* tagp);

/* long double variants needed by libc++ cmath */
long double acosl(long double x);
long double asinl(long double x);
long double atanl(long double x);
long double atan2l(long double y, long double x);
long double cosl(long double x);
long double sinl(long double x);
long double tanl(long double x);
long double acoshl(long double x);
long double asinhl(long double x);
long double atanhl(long double x);
long double coshl(long double x);
long double sinhl(long double x);
long double tanhl(long double x);
long double expl(long double x);
long double exp2l(long double x);
long double expm1l(long double x);
long double logl(long double x);
long double log2l(long double x);
long double log10l(long double x);
long double log1pl(long double x);
long double powl(long double x, long double y);
long double sqrtl(long double x);
long double cbrtl(long double x);
long double hypotl(long double x, long double y);
long double ceill(long double x);
long double floorl(long double x);
long double roundl(long double x);
long double truncl(long double x);
long double rintl(long double x);
long double nearbyintl(long double x);
float   nearbyintf(float x);
float   rintf(float x);
float   remainderf(float x, float y);
float   remquof(float x, float y, int *quo);
float   scalbnf(float x, int n);
float   scalblnf(float x, long n);
float   nexttowardf(float x, long double y);
long    lrintf(float x);
long    llrintf(float x);
long    lroundf(float x);  // already declared in the original block
long long llroundf(float x);  // already declared
float   log1pf(float x);
float   logbf(float x);
float   lgammaf(float x);
float   tanhf(float x);
float   acoshf(float x);
float   asinhf(float x);
float   atanhf(float x);
float   asinf(float x);
float   atanf(float x);
float   coshf(float x);
float   sinhf(float x);
float   copysignf(float x, float y);
float   exp2f(float x);
float   expm1f(float x);
float   hypotf(float x, float y);
float   fdimf(float x, float y);
float   frexpf(float x, int *exp_);
float   ldexpf(float x, int exp_);
float   ilogbf(float x);
float   erff(float x);
float   erfcf(float x);
float   scalblnf(float x, long n);  // already declared
float   scalbnf(float x, int n);   // already declared
long double remainderl(long double x, long double y);
long double fmodl(long double x, long double y);
long double copysignl(long double mag, long double sgn);
long double fmal(long double x, long double y, long double z);
long double fmaxl(long double x, long double y);
long double fminl(long double x, long double y);
long double fdiml(long double x, long double y);
long double nanl(const char *tagp);
long double nextafterl(long double x, long double y);
long double scalbnl(long double x, int n);
long double scalblnl(long double x, long n);
long double tgammal(long double x);
long double lgammal(long double x);
long double erfcl(long double x);
long double erfl(long double x);
long double remainderl(long double x, long double y);
int ilogbl(long double x);
long double logbl(long double x);
long double fabsl(long double x);
long double frexpl(long double x, int *exp_);
long double ldexpl(long double x, int exp_);
long double modfl(long double x, long double *iptr);
long double nexttowardl(long double x, long double y);
long double remquol(long double x, long double y, int *quo);
long double tgammal(long double x);
float tgammaf(float x);
long   lrintl(long double x);
long long llrintl(long double x);
long   lroundl(long double x);
long long llroundl(long double x);

#ifdef __cplusplus
}
#endif

#endif
