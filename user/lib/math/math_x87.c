/* math_x87.c: the libm primitives as x87 instructions (i386 and x86_64). See math.h for the contract.
 *
 * x87 inline-asm stack discipline (the thing that bites): with constraints
 * "=t"(out), "0"(x), "u"(y), gcc loads x into st0 and y into st1, and assumes
 * the asm *pops* every pure input (the "u" operand) while leaving the tied
 * output in st0. So a two-operand instruction that does NOT pop st1 itself
 * (fscale, fprem) must drop it with `fstp %st(1)`; one that does pop (fyl2x,
 * fpatan) must not. Get this wrong and the x87 stack slowly overflows into a
 * fault. Every helper below is annotated with which case it is.
 */

#include "math.h"

/* frndint under an explicit rounding mode: rc selects the control-word RC field
 * (00 nearest, 01 down, 10 up, 11 toward zero). Restores the prior CW. */
static double rndint_rc(double x, unsigned rc)
{
  unsigned short cw, ncw;
  double r;
  __asm__ volatile("fnstcw %0" : "=m"(cw));
  ncw = (unsigned short)((cw & ~0x0C00) | (rc << 10));
  __asm__ volatile("fldcw %0" ::"m"(ncw));
  __asm__ volatile("frndint" : "=t"(r) : "0"(x));
  __asm__ volatile("fldcw %0" ::"m"(cw));
  return r;
}
double floor(double x) { return rndint_rc(x, 1); }
double ceil(double x) { return rndint_rc(x, 2); }
double trunc(double x) { return rndint_rc(x, 3); }
double rint(double x)
{
  double r;
  __asm__("frndint" : "=t"(r) : "0"(x));
  return r;
}
/* fprem leaves st1 in place, so drop it. The loop reruns while C2 (bit 2 of AH
 * from fnstsw) is set, meaning the reduction was partial. */
double fmod(double x, double y)
{
  double r;
  __asm__ volatile("1:\n\t"
                   "fprem\n\t"
                   "fnstsw %%ax\n\t"
                   "testb $0x04, %%ah\n\t"
                   "jnz 1b\n\t"
                   "fstp %%st(1)\n\t"
                   : "=t"(r)
                   : "0"(x), "u"(y)
                   : "ax");
  return r;
}
/* fscale = x * 2^trunc(n); does not pop st1, so drop n. */
double scalbn(double x, int n)
{
  double r, nn = (double)n;
  __asm__ volatile("fscale\n\t fstp %%st(1)\n\t" : "=t"(r) : "0"(x), "u"(nn));
  return r;
}
double sqrt(double x)
{
  double r;
  __asm__("fsqrt" : "=t"(r) : "0"(x));
  return r;
}
/* 2^x = 2^trunc(x) * 2^frac, with 2^frac via f2xm1 over [-0.5,0.5]. */
double exp2(double x)
{
  if (isnan(x))
    return x;
  if (x == INFINITY)
    return INFINITY;
  if (x == -INFINITY)
    return 0.0;
  double n = round(x);
  double f = x - n; /* f in [-0.5, 0.5]   */
  double m;
  __asm__("f2xm1" : "=t"(m) : "0"(f));
  return scalbn(m + 1.0, (int)n);
}
double exp(double x) { return exp2(x * M_LOG2E); }
/* expm1: for small x compute e^x-1 = 2^(x·log2e)-1 directly with f2xm1, which
 * is exact near 0 (no cancellation); otherwise fall back to exp(x)-1. */
double expm1(double x)
{
  double t = x * M_LOG2E;
  if (fabs(t) <= 0.5) {
    double m;
    __asm__("f2xm1" : "=t"(m) : "0"(t));
    return m;
  }
  return exp(x) - 1.0;
}
/* fyl2x = st1 * log2(st0), then pops — balanced, no cleanup. */
static double fyl2x(double x, double y)
{
  double r;
  __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y));
  return r;
}
double log2(double x)
{
  if (isnan(x))
    return x;
  if (x < 0.0)
    return NAN;
  if (x == 0.0)
    return -INFINITY;
  if (x == INFINITY)
    return INFINITY;
  return fyl2x(x, 1.0);
}
double log(double x)
{
  if (isnan(x))
    return x;
  if (x < 0.0)
    return NAN;
  if (x == 0.0)
    return -INFINITY;
  if (x == INFINITY)
    return INFINITY;
  return fyl2x(x, M_LN2);
}
double log10(double x)
{
  if (isnan(x))
    return x;
  if (x < 0.0)
    return NAN;
  if (x == 0.0)
    return -INFINITY;
  if (x == INFINITY)
    return INFINITY;
  return fyl2x(x, M_LOG10E * M_LN2); /* = log10(2)         */
}
/* fyl2xp1 = st1 * log2(st0 + 1), valid for |st0| < 1-sqrt(1/2); exact near 0.
 */
double log1p(double x)
{
  if (isnan(x))
    return x;
  if (x < -1.0)
    return NAN;
  if (x == -1.0)
    return -INFINITY;
  if (fabs(x) < 0.29) {
    double r;
    __asm__("fyl2xp1" : "=t"(r) : "0"(x), "u"(M_LN2));
    return r;
  }
  return log(1.0 + x);
}
/* Trig: x87 fsin/fcos are only accurate for |arg| < 2^63 and reduce internally
 * with a coarse pi, so reduce mod 2pi first. Good to ~1 ulp for typical inputs;
 * extreme arguments are not a goal of a basic runtime. */
double sin(double x)
{
  if (!isfinite(x))
    return NAN;
  x = fmod(x, 2.0 * M_PI);
  double r;
  __asm__("fsin" : "=t"(r) : "0"(x));
  return r;
}
double cos(double x)
{
  if (!isfinite(x))
    return NAN;
  x = fmod(x, 2.0 * M_PI);
  double r;
  __asm__("fcos" : "=t"(r) : "0"(x));
  return r;
}
/* fptan leaves tan(x) in st1 and 1.0 in st0; popping the 1.0 leaves the result in st0, balanced. (sin(x) / cos(x)
 * does not work: gcc inlines both with their fmod, and the two inline sequences leave the x87 stack out of step.) */
double tan(double x)
{
  if (!isfinite(x))
    return NAN;
  x = fmod(x, 2.0 * M_PI);
  double r;
  __asm__("fptan\n\tfstp %%st(0)" : "=t"(r) : "0"(x));
  return r;
}
/* fpatan = atan(st1/st0), then pops — balanced. */
double atan2(double y, double x)
{
  if (isnan(x) || isnan(y))
    return NAN;
  double r;
  __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y));
  return r;
}
