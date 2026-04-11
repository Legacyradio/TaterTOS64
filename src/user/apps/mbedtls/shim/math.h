/*
 * math.h shim — maps to TaterTOS math.c functions
 */
#ifndef _TATER_SHIM_MATH_H
#define _TATER_SHIM_MATH_H

/* Special values */
#define INFINITY __builtin_inf()
#define NAN      __builtin_nan("")
#define HUGE_VAL __builtin_huge_val()

/* Classification macros */
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(0, 1, 4, 3, 2, x)
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_NORMAL    4
#define FP_SUBNORMAL 3
#define FP_ZERO      2

/* Basic math */
double fabs(double x);
float fabsf(float x);
double fmin(double x, double y);
double fmax(double x, double y);
float fminf(float x, float y);
float fmaxf(float x, float y);
double copysign(double x, double y);
double floor(double x);
float floorf(float x);
double ceil(double x);
float ceilf(float x);
double round(double x);
float roundf(float x);
long lround(double x);
long long llround(double x);
double trunc(double x);
float truncf(float x);
double fmod(double x, double y);
float fmodf(float x, float y);
double remainder(double x, double y);

/* Exponential / logarithmic */
double sqrt(double x);
float sqrtf(float x);
double cbrt(double x);
double hypot(double x, double y);
double exp(double x);
float expf(float x);
double exp2(double x);
double log(double x);
float logf(float x);
double log2(double x);
float log2f(float x);
double log10(double x);
float log10f(float x);
double log1p(double x);
double expm1(double x);
double pow(double x, double y);
float powf(float x, float y);

/* Trigonometric */
double sin(double x);
float sinf(float x);
double cos(double x);
float cosf(float x);
double tan(double x);
float tanf(float x);
double atan(double x);
float atanf(float x);
double atan2(double y, double x);
float atan2f(float y, float x);
double asin(double x);
float asinf(float x);
double acos(double x);
float acosf(float x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double acosh(double x);
double asinh(double x);
double atanh(double x);

/* Rounding to integer */
long lrint(double x);
long long llrint(double x);

/* Other */
double ldexp(double x, int exp);
double frexp(double x, int *exp);
double modf(double x, double *iptr);
double scalbn(double x, int n);
double nextafter(double x, double y);

#endif
