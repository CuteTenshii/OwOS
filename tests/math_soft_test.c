/* math_soft_test.c: the portable libm primitives (user/lib/math/math_soft.c, the AArch64 libm) against the host
 * libm. Exact operations must match bit for bit; the transcendental ones must stay within an ulp or two of the host's
 * extended-precision result over a sweep of each function's whole domain (the host's double versions are themselves
 * off by up to 1.5 ulp, log10 for one). The object is built with every symbol renamed soft_* (see
 * impl_math_soft in tests.mk), and with -ffp-contract=off as on the target, so the arithmetic is the target's. */
#define OWOS_SUITE_TEST
#include <math.h> /* user/lib/math's, on the test include path: its classifiers have no host definition */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define isnan __builtin_isnan
#define isinf __builtin_isinf
#define isfinite __builtin_isfinite
#define signbit __builtin_signbit

double soft_floor(double), soft_ceil(double), soft_trunc(double), soft_rint(double), soft_fmod(double, double);
double soft_scalbn(double, int), soft_sqrt(double), soft_exp(double), soft_exp2(double), soft_expm1(double);
double soft_log(double), soft_log2(double), soft_log10(double), soft_log1p(double);
double soft_sin(double), soft_cos(double), soft_tan(double), soft_atan2(double, double);

/* The reference: the host's long double functions, rounded once to double. */
long double expl(long double), exp2l(long double), expm1l(long double), logl(long double), log2l(long double);
long double log10l(long double), log1pl(long double), sinl(long double), cosl(long double), tanl(long double);
long double atan2l(long double, long double);
#define REF1(name)                                                                                                     \
  static double ref_##name(double x) { return (double)name##l(x); }
REF1(exp)
REF1(exp2)
REF1(expm1)
REF1(log)
REF1(log2)
REF1(log10)
REF1(log1p)
REF1(sin)
REF1(cos)
REF1(tan)
static double ref_atan2(double y, double x) { return (double)atan2l(y, x); }

static uint64_t bits(double x)
{
  uint64_t u;
  memcpy(&u, &x, sizeof u);
  return u;
}

static double from_bits(uint64_t u)
{
  double x;
  memcpy(&x, &u, sizeof x);
  return x;
}

/* Identical results: the same bits, or both NaN. */
static int same(double a, double b) { return (isnan(a) && isnan(b)) || bits(a) == bits(b); }

/* Distance in units in the last place, along the ordered line of doubles; huge when NaN-ness or infinity differs. */
static double ulps(double got, double want)
{
  if (isnan(got) || isnan(want))
    return isnan(got) && isnan(want) ? 0 : 1e300;
  if (isinf(got) || isinf(want))
    return got == want ? 0 : 1e300;
  int64_t a = (int64_t)bits(got), b = (int64_t)bits(want);
  a = a < 0 ? INT64_MIN - a : a;
  b = b < 0 ? INT64_MIN - b : b;
  __int128 d = (__int128)a - b; /* exact: near 2^62 a double would round the difference to a multiple of 512 */
  return (double)(d < 0 ? -d : d);
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rng(void)
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

/* Uniform in [lo, hi]. */
static double uniform(double lo, double hi) { return lo + (hi - lo) * ((double)(rng() >> 11) * 0x1p-53); }

/* Any finite double, every exponent equally likely. */
static double any_finite(void)
{
  for (;;) {
    double x = from_bits(rng());
    if (isfinite(x))
      return x;
  }
}

static const double SPECIAL[] = {
    0.0,      -0.0,      1.0,    -1.0,    0.5,       -0.5,       2.0,       1.5,
    -1.5,     2.5,       -2.5,   0x1p52,  -0x1p52,   0x1p52 + 1, 0x1p53,    4503599627370495.5,
    1e300,    -1e300,    1e-300, -1e-300, 0x1p-1074, -0x1p-1074, 0x1p-1022, 0x1.fffffffffffffp+1023,
    INFINITY, -INFINITY, NAN};

#define NSPECIAL ((int)(sizeof SPECIAL / sizeof *SPECIAL))
#define SWEEP 200000

static void check_rounding(void)
{
  for (int i = 0; i < NSPECIAL + SWEEP; i++) {
    double x = i < NSPECIAL ? SPECIAL[i] : i & 1 ? any_finite() : uniform(-1e6, 1e6);
    expect(same(soft_floor(x), floor(x)));
    expect(same(soft_ceil(x), ceil(x)));
    expect(same(soft_trunc(x), trunc(x)));
    expect(same(soft_rint(x), rint(x)));
  }
}

static void check_fmod(void)
{
  for (int i = 0; i < NSPECIAL; i++)
    for (int j = 0; j < NSPECIAL; j++)
      expect(same(soft_fmod(SPECIAL[i], SPECIAL[j]), fmod(SPECIAL[i], SPECIAL[j])));
  for (int i = 0; i < SWEEP; i++) {
    double x = any_finite(), y = i & 1 ? any_finite() : uniform(-10, 10);
    expect(same(soft_fmod(x, y), fmod(x, y)));
  }
}

static void check_scalbn_sqrt(void)
{
  static const int ns[] = {0, 1, -1, 52, -52, 1023, -1022, 1024, -1074, -1075, 2000, -2000, 4000, -4000};
  for (int i = 0; i < NSPECIAL + SWEEP; i++) {
    double x = i < NSPECIAL ? SPECIAL[i] : any_finite();
    for (unsigned k = 0; k < sizeof ns / sizeof *ns; k++)
      expect(same(soft_scalbn(x, ns[k]), scalbn(x, ns[k])));
    expect(same(soft_sqrt(x), sqrt(x)));
  }
}

/* Worst ulp error of soft against host over SPECIAL plus `n` samples from gen. */
static double sweep1(double (*soft)(double), double (*host)(double), double (*gen)(void), int n, double *worst_x)
{
  double worst = 0;
  for (int i = 0; i < NSPECIAL + n; i++) {
    double x = i < NSPECIAL ? SPECIAL[i] : gen(), e = ulps(soft(x), host(x));
    if (e > worst) {
      worst = e;
      *worst_x = x;
    }
  }
  return worst;
}

static double gen_exp(void) { return uniform(-760, 720); }
static double gen_exp_small(void) { return uniform(-1, 1); }
static double gen_exp2(void) { return uniform(-1100, 1030); }
static double gen_log(void) { return fabs(any_finite()); }
static double gen_log_near1(void) { return uniform(0.5, 2); }
static double gen_log1p(void) { return uniform(-1, 4); }
static double gen_trig(void) { return uniform(-10, 10); }
static double gen_trig_mid(void) { return uniform(-2e6, 2e6); }
static double gen_trig_any(void) { return any_finite(); }

/* One function's ulp bound, reporting the worst input when it is exceeded. */
#define BOUND(name, soft, host, gen, limit)                                                                            \
  do {                                                                                                                 \
    double wx = 0, e = sweep1(soft, host, gen, SWEEP, &wx);                                                            \
    if (e > (limit))                                                                                                   \
      printf("    %s: %.0f ulp at %a\n", name, e, wx);                                                                 \
    expect(e <= (limit));                                                                                              \
  } while (0)

static void check_exp(void)
{
  BOUND("exp", soft_exp, ref_exp, gen_exp, 1);
  BOUND("exp near 0", soft_exp, ref_exp, gen_exp_small, 1);
  BOUND("exp2", soft_exp2, ref_exp2, gen_exp2, 1);
  BOUND("expm1", soft_expm1, ref_expm1, gen_exp_small, 1);
  BOUND("expm1 wide", soft_expm1, ref_expm1, gen_exp, 1);
  for (int k = -1074; k <= 1023; k++)
    expect(soft_exp2(k) == ldexp(1.0, k));
  expect(soft_exp(0) == 1 && soft_expm1(-0.0) == 0 && signbit(soft_expm1(-0.0)));
}

static void check_log(void)
{
  BOUND("log", soft_log, ref_log, gen_log, 1);
  BOUND("log near 1", soft_log, ref_log, gen_log_near1, 1);
  BOUND("log2", soft_log2, ref_log2, gen_log, 1);
  BOUND("log2 near 1", soft_log2, ref_log2, gen_log_near1, 1);
  BOUND("log10", soft_log10, ref_log10, gen_log, 1);
  BOUND("log10 near 1", soft_log10, ref_log10, gen_log_near1, 1);
  BOUND("log1p", soft_log1p, ref_log1p, gen_log1p, 2);
  BOUND("log1p small", soft_log1p, ref_log1p, gen_exp_small, 2);
  for (int k = -1074; k <= 1023; k++)
    expect(soft_log2(ldexp(1.0, k)) == k);
  double p = 1;
  for (int k = 0; k <= 22; k++, p *= 10)
    expect(soft_log10(p) == k);
  expect(soft_log(1) == 0 && !signbit(soft_log(1)));
}

static void check_trig(void)
{
  BOUND("sin", soft_sin, ref_sin, gen_trig, 1);
  BOUND("cos", soft_cos, ref_cos, gen_trig, 1);
  BOUND("tan", soft_tan, ref_tan, gen_trig, 2);
  BOUND("sin mid", soft_sin, ref_sin, gen_trig_mid, 1);
  BOUND("cos mid", soft_cos, ref_cos, gen_trig_mid, 1);
  BOUND("tan mid", soft_tan, ref_tan, gen_trig_mid, 2);
  BOUND("sin any", soft_sin, ref_sin, gen_trig_any, 1);
  BOUND("cos any", soft_cos, ref_cos, gen_trig_any, 1);
  BOUND("tan any", soft_tan, ref_tan, gen_trig_any, 2);
  /* Doubles that lie closest to a multiple of pi/2, where reduction loses the most bits. */
  static const double hard[] = {0x1.6ac5b262ca1ffp+849,
                                0x1.6ac5b262ca1ffp+848,
                                6381956970095103.0 * 0x1p797,
                                1e22,
                                5e-324,
                                M_PI_2,
                                M_PI,
                                3 * M_PI_2,
                                2 * M_PI};
  for (unsigned i = 0; i < sizeof hard / sizeof *hard; i++) {
    expect(ulps(soft_sin(hard[i]), ref_sin(hard[i])) <= 1);
    expect(ulps(soft_cos(hard[i]), ref_cos(hard[i])) <= 1);
  }
}

static double atan2_soft_y1(double y) { return soft_atan2(y, 1.0); }
static double atan2_host_y1(double y) { return ref_atan2(y, 1.0); }

static void check_atan2(void)
{
  BOUND("atan", atan2_soft_y1, atan2_host_y1, gen_trig_any, 1);
  BOUND("atan small", atan2_soft_y1, atan2_host_y1, gen_trig, 1);
  double worst = 0;
  for (int i = 0; i < NSPECIAL; i++)
    for (int j = 0; j < NSPECIAL; j++) {
      double y = SPECIAL[i], x = SPECIAL[j];
      double e = ulps(soft_atan2(y, x), ref_atan2(y, x));
      expect(e <= 1);
      int sign_ok = signbit(soft_atan2(y, x)) == signbit(atan2(y, x)) || isnan(atan2(y, x));
      if (!sign_ok)
        printf("    atan2(%a, %a): %a, host %a\n", y, x, soft_atan2(y, x), atan2(y, x));
      expect(sign_ok);
    }
  for (int i = 0; i < SWEEP; i++) {
    double y = i & 1 ? any_finite() : uniform(-10, 10), x = i & 2 ? any_finite() : uniform(-10, 10);
    double e = ulps(soft_atan2(y, x), ref_atan2(y, x));
    if (e > worst)
      worst = e;
  }
  expect(worst <= 1);
}

int main(void)
{
  test("floor, ceil, trunc and rint are exact", check_rounding);
  test("fmod is exact", check_fmod);
  test("scalbn and sqrt are exact", check_scalbn_sqrt);
  test("exp, exp2 and expm1 within 1 ulp", check_exp);
  test("log, log2 and log10 within 1 ulp, log1p within 2", check_log);
  test("sin, cos and tan within 1-2 ulp, huge arguments too", check_trig);
  test("atan2 within 1 ulp, with every special quadrant", check_atan2);
  return 0;
}
