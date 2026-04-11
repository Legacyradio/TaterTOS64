/*
 * math.c — Software math library for POSIX compatibility
 *
 * Phase 8: Functions needed by NSPR, NSS, and browser runtime.
 * All implementations are original TaterTOS code using software
 * floating-point algorithms. No hardware FPU instructions beyond
 * basic +, -, *, / are assumed.
 */

#include "libc.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_E         2.71828182845904523536
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_LOG2E     1.44269504088896340736
#define M_LOG10E    0.43429448190325182765
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440

static const double DBL_HUGE = 1e308;
const fenv_t __fenv_dfl_env = { FE_TONEAREST, 0u };

/* -----------------------------------------------------------------------
 * Floating-point environment
 *
 * TaterTOS does not yet expose hardware FP status/control state. Keep a
 * conservative C99-compatible surface so fdlibm/NSPR can build.
 * ----------------------------------------------------------------------- */

int feclearexcept(int excepts) {
    (void)excepts;
    return 0;
}

int fegetexceptflag(fexcept_t *flagp, int excepts) {
    if (!flagp) {
        errno = EINVAL;
        return -1;
    }
    *flagp = 0;
    (void)excepts;
    return 0;
}

int feraiseexcept(int excepts) {
    (void)excepts;
    return 0;
}

int fesetexceptflag(const fexcept_t *flagp, int excepts) {
    if (!flagp) {
        errno = EINVAL;
        return -1;
    }
    (void)excepts;
    return 0;
}

int fetestexcept(int excepts) {
    (void)excepts;
    return 0;
}

int fegetround(void) {
    return FE_TONEAREST;
}

int fesetround(int round) {
    switch (round) {
        case FE_TONEAREST:
        case FE_DOWNWARD:
        case FE_UPWARD:
        case FE_TOWARDZERO:
            return 0;
        default:
            errno = EINVAL;
            return -1;
    }
}

int fegetenv(fenv_t *envp) {
    if (!envp) {
        errno = EINVAL;
        return -1;
    }
    *envp = __fenv_dfl_env;
    return 0;
}

int feholdexcept(fenv_t *envp) {
    return fegetenv(envp);
}

int fesetenv(const fenv_t *envp) {
    if (!envp) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int feupdateenv(const fenv_t *envp) {
    return fesetenv(envp);
}

/* -----------------------------------------------------------------------
 * Basic: fabs, fmin, fmax, copysign
 * ----------------------------------------------------------------------- */

double fabs(double x) { return x < 0.0 ? -x : x; }
float  fabsf(float x) { return x < 0.0f ? -x : x; }

double fmin(double x, double y) { return x < y ? x : y; }
double fmax(double x, double y) { return x > y ? x : y; }
float  fminf(float x, float y) { return x < y ? x : y; }
float  fmaxf(float x, float y) { return x > y ? x : y; }

double copysign(double mag, double sgn) {
    union { double d; uint64_t u; } um, us;
    um.d = mag;
    us.d = sgn;
    um.u = (um.u & 0x7FFFFFFFFFFFFFFFULL) | (us.u & 0x8000000000000000ULL);
    return um.d;
}

/* -----------------------------------------------------------------------
 * floor / ceil / round / trunc / fmod / remainder
 * ----------------------------------------------------------------------- */

double floor(double x) {
    if (x >= 0.0) {
        return (double)(long long)x;
    } else {
        long long i = (long long)x;
        return (x == (double)i) ? x : (double)(i - 1);
    }
}

float floorf(float x) { return (float)floor((double)x); }

double ceil(double x) {
    if (x <= 0.0) {
        return (double)(long long)x;
    } else {
        long long i = (long long)x;
        return (x == (double)i) ? x : (double)(i + 1);
    }
}

float ceilf(float x) { return (float)ceil((double)x); }

double round(double x) {
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

float roundf(float x) { return (float)round((double)x); }

long lround(double x) { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }

long lrint(double x) { return (long)round(x); }
long long llrint(double x) { return (long long)round(x); }

double nextafter(double x, double y) {
    union { double d; uint64_t u; } ux, uy;
    ux.d = x; uy.d = y;
    if (__builtin_isnan(x) || __builtin_isnan(y)) return x + y;
    if (ux.u == uy.u) return y;
    if (x == 0.0) {
        ux.u = 1;
        if (__builtin_signbit(y)) ux.u |= ((uint64_t)1 << 63);
        return ux.d;
    }
    if ((x < y) == (x > 0.0)) ux.u++;
    else ux.u--;
    return ux.d;
}

double trunc(double x) {
    return (double)(long long)x;
}

float truncf(float x) { return (float)trunc((double)x); }

double fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    return x - (double)(long long)(x / y) * y;
}

float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }

double remainder(double x, double y) {
    if (y == 0.0) return 0.0;
    double n = round(x / y);
    return x - n * y;
}

/* -----------------------------------------------------------------------
 * sqrt — Newton-Raphson
 * ----------------------------------------------------------------------- */

double sqrt(double x) {
    if (x < 0.0) return -1.0; /* NaN not available; return -1 as error */
    if (x == 0.0) return 0.0;

    /* Initial guess using bit manipulation */
    union { double d; uint64_t u; } u;
    u.d = x;
    u.u = (u.u >> 1) + 0x1FF8000000000000ULL;
    double g = u.d;

    /* Newton-Raphson iterations */
    for (int i = 0; i < 8; i++) {
        g = 0.5 * (g + x / g);
    }
    return g;
}

float sqrtf(float x) { return (float)sqrt((double)x); }

double cbrt(double x) {
    if (x == 0.0) return 0.0;
    int neg = (x < 0.0);
    double a = neg ? -x : x;

    /* Initial guess */
    double g = a;
    if (g > 1.0) g = a / 3.0;

    for (int i = 0; i < 20; i++) {
        g = (2.0 * g + a / (g * g)) / 3.0;
    }
    return neg ? -g : g;
}

double hypot(double x, double y) {
    return sqrt(x * x + y * y);
}

/* -----------------------------------------------------------------------
 * exp / log / log2 / log10 / pow
 *
 * exp: Taylor series with range reduction
 * log: Halley's method
 * pow: exp(y * log(x))
 * ----------------------------------------------------------------------- */

double exp(double x) {
    /* Handle extremes */
    if (x > 709.0) return DBL_HUGE;
    if (x < -709.0) return 0.0;

    /* Range reduction: x = k*ln2 + r, |r| <= ln2/2 */
    int k = (int)(x / M_LN2 + (x >= 0.0 ? 0.5 : -0.5));
    double r = x - k * M_LN2;

    /* Taylor series for e^r */
    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i <= 20; i++) {
        term *= r / i;
        sum += term;
        if (fabs(term) < 1e-16 * fabs(sum)) break;
    }

    /* Multiply by 2^k */
    if (k > 0) {
        for (int i = 0; i < k; i++) sum *= 2.0;
    } else if (k < 0) {
        for (int i = 0; i < -k; i++) sum *= 0.5;
    }

    return sum;
}

float expf(float x) { return (float)exp((double)x); }

double exp10(double x) {
    return exp(x * M_LN10);
}

float exp10f(float x) {
    return (float)exp10((double)x);
}

double exp2(double x) {
    return exp(x * M_LN2);
}

double log(double x) {
    if (x <= 0.0) return -DBL_HUGE; /* -inf for 0, undefined for negative */
    if (x == 1.0) return 0.0;

    /* Range reduction: x = m * 2^e, 0.5 <= m < 1.0 */
    int e = 0;
    double m = x;
    while (m >= 2.0) { m *= 0.5; e++; }
    while (m < 0.5) { m *= 2.0; e--; }

    /* Use log(m) with the series: log((1+t)/(1-t)) = 2*(t + t^3/3 + t^5/5 + ...)
       where t = (m-1)/(m+1) */
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double sum = t;
    double power = t;

    for (int i = 3; i <= 41; i += 2) {
        power *= t2;
        sum += power / i;
    }

    return 2.0 * sum + e * M_LN2;
}

float logf(float x) { return (float)log((double)x); }

double log2(double x) {
    return log(x) * M_LOG2E;
}

double log10(double x) {
    return log(x) * M_LOG10E;
}

float log10f(float x) { return (float)log10((double)x); }
float log2f(float x) { return (float)log2((double)x); }

double log1p(double x) {
    /* For small x, use series directly to avoid cancellation */
    if (fabs(x) < 1e-4) {
        double sum = x;
        double term = x;
        for (int i = 2; i <= 20; i++) {
            term *= -x;
            sum += term / i;
        }
        return sum;
    }
    return log(1.0 + x);
}

double expm1(double x) {
    if (fabs(x) < 1e-4) {
        double sum = x;
        double term = x;
        for (int i = 2; i <= 20; i++) {
            term *= x / i;
            sum += term;
        }
        return sum;
    }
    return exp(x) - 1.0;
}

double pow(double base, double exponent) {
    if (exponent == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    if (base == 1.0) return 1.0;

    /* Integer exponent fast path */
    if (exponent == (double)(int)exponent && exponent > 0 && exponent < 64) {
        int n = (int)exponent;
        double result = 1.0;
        double b = base;
        while (n > 0) {
            if (n & 1) result *= b;
            b *= b;
            n >>= 1;
        }
        return result;
    }

    if (base < 0.0) {
        /* Negative base with non-integer exponent is undefined */
        return -1.0;
    }

    return exp(exponent * log(base));
}

float powf(float base, float exp_) { return (float)pow((double)base, (double)exp_); }

/* -----------------------------------------------------------------------
 * Trigonometric: sin, cos, tan, asin, acos, atan, atan2
 *
 * sin/cos: Taylor series with range reduction to [-pi, pi]
 * atan: Polynomial approximation
 * ----------------------------------------------------------------------- */

/* Reduce angle to [-pi, pi] */
static double reduce_angle(double x) {
    if (x >= -M_PI && x <= M_PI) return x;
    x = fmod(x, 2.0 * M_PI);
    if (x > M_PI) x -= 2.0 * M_PI;
    if (x < -M_PI) x += 2.0 * M_PI;
    return x;
}

double sin(double x) {
    x = reduce_angle(x);

    /* Taylor series: x - x^3/3! + x^5/5! - ... */
    double term = x;
    double sum = x;
    double x2 = x * x;

    for (int i = 1; i <= 12; i++) {
        term *= -x2 / (double)(2 * i * (2 * i + 1));
        sum += term;
    }
    return sum;
}

float sinf(float x) { return (float)sin((double)x); }

double cos(double x) {
    x = reduce_angle(x);

    /* Taylor series: 1 - x^2/2! + x^4/4! - ... */
    double term = 1.0;
    double sum = 1.0;
    double x2 = x * x;

    for (int i = 1; i <= 12; i++) {
        term *= -x2 / (double)((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}

float cosf(float x) { return (float)cos((double)x); }

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return DBL_HUGE;
    return sin(x) / c;
}

float tanf(float x) { return (float)tan((double)x); }

double atan(double x) {
    /* atan for all x using identity:
       atan(x) = pi/2 - atan(1/x) for |x| > 1
       atan(x) = x - x^3/3 + x^5/5 - ... for |x| <= 1 */
    int neg = 0;
    int recip = 0;

    if (x < 0.0) { neg = 1; x = -x; }
    if (x > 1.0) { recip = 1; x = 1.0 / x; }

    /* Reduce further: atan(x) = pi/6 + atan((x-1/sqrt(3))/(1+x/sqrt(3))) */
    double sum;
    double x2 = x * x;
    double term = x;
    sum = x;

    for (int i = 1; i <= 25; i++) {
        term *= -x2;
        sum += term / (double)(2 * i + 1);
    }

    if (recip) sum = M_PI_2 - sum;
    if (neg) sum = -sum;
    return sum;
}

float atanf(float x) { return (float)atan((double)x); }

double atan2(double y, double x) {
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) {
        if (y >= 0.0) return atan(y / x) + M_PI;
        return atan(y / x) - M_PI;
    }
    /* x == 0 */
    if (y > 0.0) return M_PI_2;
    if (y < 0.0) return -M_PI_2;
    return 0.0;
}

float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }

double asin(double x) {
    if (x < -1.0 || x > 1.0) return 0.0;
    if (x == 1.0) return M_PI_2;
    if (x == -1.0) return -M_PI_2;
    return atan2(x, sqrt(1.0 - x * x));
}

float asinf(float x) { return (float)asin((double)x); }

double acos(double x) {
    if (x < -1.0 || x > 1.0) return 0.0;
    return M_PI_2 - asin(x);
}

float acosf(float x) { return (float)acos((double)x); }

/* -----------------------------------------------------------------------
 * Hyperbolic: sinh, cosh, tanh
 * ----------------------------------------------------------------------- */

double sinh(double x) {
    double ex = exp(x);
    return (ex - 1.0 / ex) * 0.5;
}

double cosh(double x) {
    double ex = exp(x);
    return (ex + 1.0 / ex) * 0.5;
}

double tanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e2x = exp(2.0 * x);
    return (e2x - 1.0) / (e2x + 1.0);
}

/* Inverse hyperbolic functions */
double acosh(double x) {
    if (x < 1.0) return __builtin_nan("");
    return log(x + sqrt(x * x - 1.0));
}

double asinh(double x) {
    if (x >= 0.0)
        return log(x + sqrt(x * x + 1.0));
    return -log(-x + sqrt(x * x + 1.0));
}

double atanh(double x) {
    if (x <= -1.0 || x >= 1.0) return __builtin_nan("");
    return 0.5 * log((1.0 + x) / (1.0 - x));
}

/* -----------------------------------------------------------------------
 * ldexp / frexp / modf / scalbn
 * ----------------------------------------------------------------------- */

double ldexp(double x, int exp_) {
    while (exp_ > 0) { x *= 2.0; exp_--; }
    while (exp_ < 0) { x *= 0.5; exp_++; }
    return x;
}

double frexp(double x, int *exp_) {
    if (x == 0.0) { if (exp_) *exp_ = 0; return 0.0; }
    int e = 0;
    double m = (x < 0.0) ? -x : x;
    while (m >= 1.0) { m *= 0.5; e++; }
    while (m < 0.5) { m *= 2.0; e--; }
    if (exp_) *exp_ = e;
    return (x < 0.0) ? -m : m;
}

double modf(double x, double *iptr) {
    double i = trunc(x);
    if (iptr) *iptr = i;
    return x - i;
}

double scalbn(double x, int n) {
    return ldexp(x, n);
}

/* -----------------------------------------------------------------------
 * isinf / isnan / isfinite (software implementations)
 * ----------------------------------------------------------------------- */

int isinf_d(double x) {
    union { double d; uint64_t u; } u;
    u.d = x;
    return ((u.u & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL);
}

int isnan_d(double x) {
    union { double d; uint64_t u; } u;
    u.d = x;
    return ((u.u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) &&
           ((u.u & 0x000FFFFFFFFFFFFFULL) != 0);
}

int isfinite_d(double x) {
    union { double d; uint64_t u; } u;
    u.d = x;
    return ((u.u & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL);
}

long double exp10l(long double x) {
    return (long double)exp10((double)x);
}
