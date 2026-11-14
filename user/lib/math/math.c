/* math.c: freestanding libm, the parts built on the arch primitives. See math.h for the contract; the primitives
 * (rounding, fmod, scalbn, sqrt, exp/log, trig) live in math_x87.c on x86 and math_soft.c elsewhere. */

#include "math.h"

typedef union {
  double d;
  struct {
    unsigned lo, hi;
  } w; /* little-endian: hi holds sign/exp */
  unsigned long long u;
} dbits;

int isnan(double x)
{
  dbits b;
  b.d = x;
  unsigned exp = (b.w.hi >> 20) & 0x7ff;
  return exp == 0x7ff && ((b.w.hi & 0xfffff) | b.w.lo);
}

int isinf(double x)
{
  dbits b;
  b.d = x;
  unsigned exp = (b.w.hi >> 20) & 0x7ff;
  return exp == 0x7ff && (b.w.hi & 0xfffff) == 0 && b.w.lo == 0;
}

int isfinite(double x)
{
  dbits b;
  b.d = x;
  return ((b.w.hi >> 20) & 0x7ff) != 0x7ff;
}

int signbit(double x)
{
  dbits b;
  b.d = x;
  return (int)(b.w.hi >> 31);
}

double fabs(double x)
{
  dbits b;
  b.d = x;
  b.w.hi &= 0x7fffffff; /* clear sign bit; no FPU needed   */
  return b.d;
}

double copysign(double x, double y)
{
  dbits a, b;
  a.d = x;
  b.d = y;
  a.w.hi = (a.w.hi & 0x7fffffff) | (b.w.hi & 0x80000000);
  return a.d;
}

double round(double x)
{
  if (!isfinite(x))
    return x;
  double t = trunc(x);
  double d = x - t;
  if (d >= 0.5)
    t += 1.0;
  else if (d <= -0.5)
    t -= 1.0;
  return t;
}

double fmin(double x, double y)
{
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  return x < y ? x : y;
}

double fmax(double x, double y)
{
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  return x > y ? x : y;
}

double ldexp(double x, int n) { return scalbn(x, n); }

double frexp(double x, int *e)
{
  dbits b;
  b.d = x;
  unsigned exp = (b.w.hi >> 20) & 0x7ff;
  if (exp == 0x7ff || x == 0.0) {
    *e = 0;
    return x;
  } /* 0, inf, nan        */
  if (exp == 0) {             /* subnormal: normalise */
    x *= 18014398509481984.0; /* 2^54               */
    b.d = x;
    exp = (b.w.hi >> 20) & 0x7ff;
    *e = (int)exp - 1022 - 54;
  } else {
    *e = (int)exp - 1022;
  }
  b.w.hi = (b.w.hi & 0x800fffff) | 0x3fe00000; /* force exponent to -1 */
  return b.d;                                  /* mantissa in [0.5,1) */
}

double modf(double x, double *iptr)
{
  if (!isfinite(x)) {
    *iptr = x;
    return isnan(x) ? x : copysign(0.0, x);
  }
  double ip = trunc(x);
  *iptr = ip;
  return copysign(x - ip, x); /* keep -0 for -0 < x < 1 */
}

double pow(double x, double y)
{
  if (y == 0.0)
    return 1.0; /* incl. pow(NaN,0)=1 */
  if (isnan(x) || isnan(y))
    return NAN;
  if (y == 1.0)
    return x;

  if (x == 1.0)
    return 1.0;
  if (x == 0.0) {
    if (y < 0.0)
      return (fmod(y, 2.0) != 0.0) ? copysign(INFINITY, x) : INFINITY;
    return (fmod(y, 2.0) != 0.0) ? x : 0.0; /* preserve -0 sign   */
  }

  if (x < 0.0) {
    double ay = fabs(y);
    if (trunc(ay) != ay)
      return NAN; /* non-integer exp     */
    double m = pow(-x, y);
    return (fmod(ay, 2.0) == 1.0) ? -m : m; /* odd exponent flips  */
  }

  if (x == INFINITY)
    return (y > 0.0) ? INFINITY : 0.0;
  /* general positive base */
  return exp2(y * log2(x));
}

double cbrt(double x)
{
  if (x == 0.0 || !isfinite(x))
    return x;
  double a = fabs(x);
  double r = exp2(log2(a) * (1.0 / 3.0));
  r = r - (r - a / (r * r)) / 3.0; /* one Newton step     */
  return copysign(r, x);
}

double hypot(double x, double y)
{
  x = fabs(x);
  y = fabs(y);
  if (isinf(x) || isinf(y))
    return INFINITY;
  if (x < y) {
    double t = x;
    x = y;
    y = t;
  }
  if (x == 0.0)
    return 0.0;
  double r = y / x;
  return x * sqrt(1.0 + r * r); /* scaled: avoids overflow */
}

double atan(double x) { return atan2(x, 1.0); }

double asin(double x)
{
  if (isnan(x))
    return x;
  if (x < -1.0 || x > 1.0)
    return NAN;
  return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x)
{
  if (isnan(x))
    return x;
  if (x < -1.0 || x > 1.0)
    return NAN;
  return atan2(sqrt(1.0 - x * x), x);
}

double sinh(double x)
{
  if (!isfinite(x))
    return x;
  double e = exp(x);
  return (e - 1.0 / e) * 0.5;
}

double cosh(double x)
{
  if (isnan(x))
    return x;
  if (isinf(x))
    return INFINITY;
  double e = exp(fabs(x));
  return (e + 1.0 / e) * 0.5;
}

double tanh(double x)
{
  if (isnan(x))
    return x;
  if (x > 20.0)
    return 1.0; /* saturates in double */
  if (x < -20.0)
    return -1.0;
  double e = exp(2.0 * x);
  return (e - 1.0) / (e + 1.0);
}

double asinh(double x)
{
  if (!isfinite(x))
    return x;
  double a = fabs(x);
  double r = log(a + sqrt(a * a + 1.0));
  return copysign(r, x);
}

double acosh(double x)
{
  if (isnan(x))
    return x;
  if (x < 1.0)
    return NAN;
  return log(x + sqrt(x * x - 1.0));
}

double atanh(double x)
{
  if (isnan(x))
    return x;
  if (x < -1.0 || x > 1.0)
    return NAN;
  if (x == 1.0)
    return INFINITY;
  if (x == -1.0)
    return -INFINITY;
  return 0.5 * log((1.0 + x) / (1.0 - x));
}
