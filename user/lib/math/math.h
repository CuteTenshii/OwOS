/* math.h — freestanding double-precision libm for OwOS user space.
 *
 * There is no host libm: user code is built -ffreestanding -nostdlib, and the
 * kernel saves no SSE/MMX state across a context switch, so every operation here
 * is x87 (the build forces -mno-sse -mfpmath=387). The surface is sized for a
 * JavaScript runtime — it covers the whole ECMAScript `Math` object plus the
 * IEEE-754 classification predicates the engine needs to implement Number
 * semantics — but nothing here is JS-specific, so it doubles as a general libm.
 *
 * JS-vs-C divergences (e.g. Math.pow(1, NaN) === NaN, Math.round half-up,
 * min/max -0 ordering) are NOT baked in here: these functions follow C99/IEEE,
 * and the engine's Math bindings apply the ECMAScript quirks on top.
 */
#ifndef OWOS_MATH_H
#define OWOS_MATH_H

/* IEEE-754 special values. gcc synthesises these as compile-time constants with
 * no library or SSE dependency. */
#define INFINITY  (__builtin_inf())
#define NAN       (__builtin_nan(""))
#define HUGE_VAL  (__builtin_inf())

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074   /* log2(e)  */
#define M_LOG10E   0.43429448190325182765   /* log10(e) */
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

/* Classification. isnan/isfinite/signbit return 0/1; isinf returns 1 for either
 * infinity, 0 otherwise. fpclassify-style helpers are not needed by the engine. */
int    isnan(double x);
int    isinf(double x);
int    isfinite(double x);
int    signbit(double x);

/* Sign / rounding / remainder. round() is C round-half-away-from-zero; the JS
 * Math.round (half toward +Infinity) is floor(x+0.5) in the engine. */
double fabs(double x);
double copysign(double x, double y);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double rint(double x);                 /* round to nearest, ties to even        */
double fmod(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);

/* Scaling — exact, no rounding error: x * 2^n. frexp/modf split a value. */
double scalbn(double x, int n);
double ldexp(double x, int n);
double frexp(double x, int *exp);
double modf(double x, double *iptr);

/* Powers, roots, exponentials, logs. */
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);
double pow(double x, double y);
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);

/* Trigonometric and hyperbolic. */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);

#endif /* OWOS_MATH_H */
