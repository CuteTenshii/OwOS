/* math_soft.c: the libm primitives in portable C, for targets without the x87 (AArch64). See math.h for the contract.
 *
 * The algorithms are fdlibm's: an argument reduction, then a minimax polynomial on the reduced range. Large trig
 * arguments are reduced exactly against 1216 bits of 2/pi (Payne-Hanek). Built with -ffp-contract=off, so the
 * arithmetic, and the host test that checks it against the host libm, is the same on every target.
 */

#include "math.h"
#include <stdint.h>

static uint64_t bits_of(double x)
{
  union {
    double d;
    uint64_t u;
  } b = {.d = x};
  return b.u;
}

static double from_bits(uint64_t u)
{
  union {
    uint64_t u;
    double d;
  } b = {.u = u};
  return b.d;
}

#define SIGN_BIT (1ull << 63)
#define EXP_OF(u) ((int)((u) >> 52 & 0x7ff))

static int is_nan(double x) { return (bits_of(x) & ~SIGN_BIT) > 0x7ff0000000000000ull; }

/* ---- rounding, remainder, scaling, square root: exact ---- */

double trunc(double x)
{
  uint64_t u = bits_of(x);
  int e = EXP_OF(u) - 0x3ff;
  if (e >= 52)
    return x; /* already integral, or inf/nan */
  if (e < 0)
    return from_bits(u & SIGN_BIT);
  return from_bits(u & ~((1ull << (52 - e)) - 1));
}

double floor(double x)
{
  double t = trunc(x);
  return t > x ? t - 1.0 : t;
}

double ceil(double x)
{
  double t = trunc(x);
  return t < x ? t + 1.0 : t;
}

/* Round to nearest, ties to even: adding and removing 2^52 leaves no fraction bits for the rounding to keep. */
double rint(double x)
{
  uint64_t u = bits_of(x);
  if (EXP_OF(u) >= 0x3ff + 52)
    return x;
  double y = u & SIGN_BIT ? x - 0x1p52 + 0x1p52 : x + 0x1p52 - 0x1p52;
  return y == 0.0 ? from_bits(u & SIGN_BIT) : y;
}

/* Long division on the integer significands: exact, like the hardware fprem. */
double fmod(double x, double y)
{
  uint64_t ux = bits_of(x), uy = bits_of(y), sx = ux & SIGN_BIT;
  int ex = EXP_OF(ux), ey = EXP_OF(uy);
  if (uy << 1 == 0 || is_nan(y) || ex == 0x7ff)
    return (x * y) / (x * y);
  if (ux << 1 <= uy << 1)
    return ux << 1 == uy << 1 ? 0.0 * x : x;

  if (ex == 0) { /* subnormal: normalize */
    for (uint64_t i = ux << 12; !(i >> 63); i <<= 1)
      ex--;
    ux <<= 1 - ex;
  } else {
    ux = (ux & (~0ull >> 12)) | 1ull << 52;
  }
  if (ey == 0) {
    for (uint64_t i = uy << 12; !(i >> 63); i <<= 1)
      ey--;
    uy <<= 1 - ey;
  } else {
    uy = (uy & (~0ull >> 12)) | 1ull << 52;
  }

  for (; ex > ey; ex--) {
    uint64_t i = ux - uy;
    if (!(i >> 63)) {
      if (i == 0)
        return 0.0 * x;
      ux = i;
    }
    ux <<= 1;
  }
  uint64_t i = ux - uy;
  if (!(i >> 63)) {
    if (i == 0)
      return 0.0 * x;
    ux = i;
  }
  for (; !(ux >> 52); ux <<= 1)
    ex--;

  if (ex > 0)
    ux = (ux - (1ull << 52)) | (uint64_t)ex << 52;
  else
    ux >>= 1 - ex;
  return from_bits(ux | sx);
}

/* x * 2^n in at most three exact steps; a subnormal result is rounded only once, by the last. */
double scalbn(double x, int n)
{
  double y = x;
  if (n > 1023) {
    y *= 0x1p1023;
    n -= 1023;
    if (n > 1023) {
      y *= 0x1p1023;
      n -= 1023;
      if (n > 1023)
        n = 1023;
    }
  } else if (n < -1022) {
    y *= 0x1p-1022 * 0x1p53;
    n += 1022 - 53;
    if (n < -1022) {
      y *= 0x1p-1022 * 0x1p53;
      n += 1022 - 53;
      if (n < -1022)
        n = -1022;
    }
  }
  return y * from_bits((uint64_t)(0x3ff + n) << 52);
}

double sqrt(double x)
{
  double r;
#if defined(__aarch64__)
  __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
#elif defined(__SSE2__)
  __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
#else
#error "math_soft.c needs a hardware square root for this target"
#endif
  return r;
}

/* ---- exponentials ---- */

static const double LN2_HI = 6.93147180369123816490e-01; /* ln 2, high 32 bits: k * LN2_HI is exact */
static const double LN2_LO = 1.90821492927058770002e-10;
static const double INV_LN2 = 1.44269504088896338700e+00;
static const double EP1 = 1.66666666666666019037e-01, EP2 = -2.77777777770155933842e-03,
                    EP3 = 6.61375632143793436117e-05, EP4 = -1.65339022054652515390e-06,
                    EP5 = 4.13813679705723846039e-08;

/* For r = hi - lo in [-ln2/2, ln2/2]: e^r as 1 + r + r*c/(2-c), with c from the Remez fit of r*(e^r+1)/(e^r-1). */
static double exp_reduced(double hi, double lo)
{
  double r = hi - lo, t = r * r;
  double c = r - t * (EP1 + t * (EP2 + t * (EP3 + t * (EP4 + t * EP5))));
  return 1.0 - ((lo - (r * c) / (2.0 - c)) - hi);
}

double exp(double x)
{
  if (is_nan(x))
    return x;
  if (x > 7.09782712893383973096e+02)
    return INFINITY;
  if (x < -7.45133219101941108420e+02)
    return 0.0;
  double ax = x < 0 ? -x : x;
  if (ax < 0x1p-28)
    return 1.0 + x;
  if (ax <= 0.5 * M_LN2)
    return exp_reduced(x, 0.0);
  int k = (int)(INV_LN2 * x + (x < 0 ? -0.5 : 0.5));
  return scalbn(exp_reduced(x - k * LN2_HI, k * LN2_LO), k);
}

double exp2(double x)
{
  if (is_nan(x))
    return x;
  if (x >= 1024.0)
    return INFINITY;
  if (x < -1075.0)
    return 0.0;
  double n = rint(x), f = x - n; /* exact, |f| <= 1/2 */
  return scalbn(exp_reduced(f * M_LN2, 0.0), (int)n);
}

static const double EQ1 = -3.33333333333331316428e-02, EQ2 = 1.58730158725481460165e-03,
                    EQ3 = -7.93650757867487942473e-05, EQ4 = 4.00821782732936239552e-06,
                    EQ5 = -2.01099218183624371326e-07;

/* fdlibm's expm1: x = k ln2 + r with r = hi - lo, a Remez fit of r/(e^r - 1) - 1 + r/2 on the reduced range, then
 * 2^k (e^r - 1) + (2^k - 1) assembled so neither the reduction error nor the 1 cancels away. */
double expm1(double x)
{
  double ax = x < 0 ? -x : x;
  if (is_nan(x) || ax < 0x1p-54)
    return x;
  if (x > 7.09782712893383973096e+02)
    return INFINITY;
  if (x < -56.0 * M_LN2)
    return -1.0; /* e^x is below half an ulp of 1 */

  int k = 0;
  double c = 0.0;
  if (ax > 0.5 * M_LN2) {
    double hi, lo;
    if (ax < 1.5 * M_LN2) {
      k = x > 0 ? 1 : -1;
      hi = x - k * LN2_HI;
      lo = k * LN2_LO;
    } else {
      k = (int)(INV_LN2 * x + (x < 0 ? -0.5 : 0.5));
      hi = x - k * LN2_HI;
      lo = k * LN2_LO;
    }
    x = hi - lo;
    c = (hi - x) - lo; /* what rounding hi - lo lost */
  }

  double hfx = 0.5 * x, hxs = x * hfx;
  double r1 = 1.0 + hxs * (EQ1 + hxs * (EQ2 + hxs * (EQ3 + hxs * (EQ4 + hxs * EQ5))));
  double t = 3.0 - r1 * hfx;
  double e = hxs * ((r1 - t) / (6.0 - x * t));
  if (k == 0)
    return x - (x * e - hxs);
  e = (x * (e - c) - c) - hxs;
  if (k == -1)
    return 0.5 * (x - e) - 0.5;
  if (k == 1)
    return x < -0.25 ? -2.0 * (e - (x + 0.5)) : 1.0 + 2.0 * (x - e);
  if (k <= -2 || k > 56)
    return scalbn(1.0 - (e - x), k) - 1.0;
  if (k < 20)
    return scalbn((1.0 - scalbn(1.0, -k)) - (e - x), k);
  return scalbn((x - (e + scalbn(1.0, -k))) + 1.0, k);
}

/* ---- logarithms ---- */

static const double LG1 = 6.666666666666735130e-01, LG2 = 3.999999999940941908e-01, LG3 = 2.857142874366239149e-01,
                    LG4 = 2.222219843214978396e-01, LG5 = 1.818357216161805012e-01, LG6 = 1.531383769920937332e-01,
                    LG7 = 1.479819860511658591e-01;

/* Split a finite x > 0 as 2^k * (1 + f) with 1 + f in [sqrt(2)/2, sqrt(2)); *hx keeps the high mantissa bits that
 * pick the polynomial's form. */
static double log_reduce(double x, int *k, uint32_t *hx)
{
  uint64_t u = bits_of(x);
  *k = 0;
  if (EXP_OF(u) == 0) { /* subnormal */
    *k = -54;
    u = bits_of(x * 0x1p54);
  }
  uint32_t h = (uint32_t)(u >> 32);
  *k += (int)(h >> 20) - 0x3ff;
  h &= 0x000fffff;
  uint32_t i = (h + 0x95f64) & 0x100000; /* set when 1 + f would be at or above sqrt(2): halve it */
  *k += (int)(i >> 20);
  *hx = h;
  return from_bits((uint64_t)(h | (i ^ 0x3ff00000)) << 32 | (u & 0xffffffffu)) - 1.0;
}

/* fdlibm's log: k ln2 folded into the last step, in two parts, so nothing rounds before the sum. */
static double log_positive(double x)
{
  int k;
  uint32_t hx;
  double f = log_reduce(x, &k, &hx), dk = k;
  if ((0x000fffff & (2 + hx)) < 3) { /* |f| < 2^-20 */
    if (f == 0.0)
      return dk * LN2_HI + dk * LN2_LO;
    double r = f * f * (0.5 - 0.33333333333333333 * f);
    return dk * LN2_HI - ((r - dk * LN2_LO) - f);
  }
  double s = f / (2.0 + f), z = s * s, w = z * z;
  double r = z * (LG1 + w * (LG3 + w * (LG5 + w * LG7))) + w * (LG2 + w * (LG4 + w * LG6));
  if ((int32_t)((hx - 0x6147a) | (0x6b851 - hx)) > 0) { /* 1 + f in the band around 1.4 */
    double hfsq = 0.5 * f * f;
    return dk * LN2_HI - ((hfsq - (s * (hfsq + r) + dk * LN2_LO)) - f);
  }
  return dk * LN2_HI - ((s * (f - r) - dk * LN2_LO) - f);
}

/* NaN, negative, zero and +inf share their answers across the logarithms; 1 means x is an ordinary positive value. */
static int log_special(double x, double *out)
{
  if (is_nan(x))
    *out = x;
  else if (x < 0.0)
    *out = NAN;
  else if (x == 0.0)
    *out = -INFINITY;
  else if (x == INFINITY)
    *out = INFINITY;
  else
    return 1;
  return 0;
}

double log(double x)
{
  double r;
  return log_special(x, &r) ? log_positive(x) : r;
}

/* ln(1 + f) for x = 2^k (1 + f), as hi + lo: hi is f - f^2/2 with its low 32 bits cleared, so multiplying it by the
 * high half of a constant is exact, and lo carries the rest (FreeBSD's split for log2 and log10). */
static void log_split(double x, int *k, double *hi, double *lo)
{
  uint32_t hx;
  double f = log_reduce(x, k, &hx);
  double hfsq = 0.5 * f * f, s = f / (2.0 + f), z = s * s, w = z * z;
  double r = s * (hfsq + z * (LG1 + w * (LG3 + w * (LG5 + w * LG7))) + w * (LG2 + w * (LG4 + w * LG6)));
  *hi = from_bits(bits_of(f - hfsq) & 0xffffffff00000000ull);
  *lo = (f - *hi) - hfsq + r;
}

static const double IVLN2_HI = 1.44269504072144627571e+00, IVLN2_LO = 1.67517131648865118353e-10;

/* k + ln(1 + f)/ln 2 in parts, so powers of two come out exact and the sum rounds once. */
double log2(double x)
{
  double r;
  if (!log_special(x, &r))
    return r;
  int k;
  double hi, lo;
  log_split(x, &k, &hi, &lo);
  double val_hi = hi * IVLN2_HI, val_lo = (lo + hi) * IVLN2_LO + lo * IVLN2_HI;
  double y = k, w = y + val_hi;
  val_lo += (y - w) + val_hi;
  return val_lo + w;
}

static const double IVLN10_HI = 4.34294481878168880939e-01, IVLN10_LO = 2.50829467116452752298e-11;
static const double LOG10_2HI = 3.01029995663611771306e-01, LOG10_2LO = 3.69423907715893078616e-13;

/* k log10(2) + ln(1 + f)/ln 10, both in parts, so powers of ten come out exact and the sum rounds once. */
double log10(double x)
{
  double r;
  if (!log_special(x, &r))
    return r;
  int k;
  double hi, lo;
  log_split(x, &k, &hi, &lo);
  double y = k, val_hi = hi * IVLN10_HI, y2 = y * LOG10_2HI;
  double val_lo = y * LOG10_2LO + (lo + hi) * IVLN10_LO + lo * IVLN10_HI;
  double w = y2 + val_hi;
  val_lo += (y2 - w) + val_hi;
  return val_lo + w;
}

/* log(u) * x / (u - 1) with u = 1 + x: the rounding error of u cancels to first order (Goldberg's method). */
double log1p(double x)
{
  if (is_nan(x))
    return x;
  if (x < -1.0)
    return NAN;
  if (x == -1.0)
    return -INFINITY;
  if (x == INFINITY)
    return INFINITY;
  double u = 1.0 + x;
  if (u == 1.0)
    return x;
  return log(u) * (x / (u - 1.0));
}

/* ---- trigonometry ---- */

static const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03, S3 = -1.98412698298579493134e-04,
                    S4 = 2.75573137070700676789e-06, S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
static const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03, C3 = 2.48015872894767294178e-05,
                    C4 = -2.75573143513906633035e-07, C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;

/* sin and cos of x + y on [-pi/4, pi/4], y being the tail of the reduced argument. */
static double ksin(double x, double y)
{
  double z = x * x, v = z * x;
  double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
  return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

static double kcos(double x, double y)
{
  double z = x * x, w = z * z;
  double r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
  double hz = 0.5 * z;
  w = 1.0 - hz;
  return w + (((1.0 - w) - hz) + (z * r - x * y));
}

/* floor(2/pi * 2^1216): the binary fraction of 2/pi, most significant bit first. */
static const uint64_t TWO_OVER_PI[19] = {
    0xa2f9836e4e441529ull, 0xfc2757d1f534ddc0ull, 0xdb6295993c439041ull, 0xfe5163abdebbc561ull, 0xb7246e3a424dd2e0ull,
    0x06492eea09d1921cull, 0xfe1deb1cb129a73eull, 0xe88235f52ebb4484ull, 0xe99c7026b45f7e41ull, 0x3991d639835339f4ull,
    0x9c845f8bbdf9283bull, 0x1ff897ffde05980full, 0xef2f118b5a0a6d1full, 0x6d367ecf27cb09b7ull, 0x4f463f669e5fea2dull,
    0x7527bac7ebe5f17bull, 0x3d0739f78a5292eaull, 0x6bfb5fb11f8d5d08ull, 0x56033046fc7b6babull,
};

/* 64 bits of 2/pi starting at fraction bit `t` (bit 1 is the first after the point); 2/pi < 1, so bits at or before
 * the point are 0. */
static uint64_t two_over_pi_bits(int t)
{
  int p = t - 1;
  if (p <= -64)
    return 0;
  if (p < 0)
    return TWO_OVER_PI[0] >> -p;
  int w = p / 64, s = p % 64;
  uint64_t hi = w < 19 ? TWO_OVER_PI[w] : 0, lo = w + 1 < 19 ? TWO_OVER_PI[w + 1] : 0;
  return s ? hi << s | lo >> (64 - s) : hi;
}

/* a * b as hi + lo exactly (Dekker), so no fused multiply-add is needed. */
static void two_product(double a, double b, double *hi, double *lo)
{
  const double split = 134217729.0; /* 2^27 + 1 */
  double ca = split * a, ah = ca - (ca - a), al = a - ah;
  double cb = split * b, bh = cb - (cb - b), bl = b - bh;
  *hi = a * b;
  *lo = ((ah * bh - *hi) + ah * bl + al * bh) + al * bl;
}

static const double PIO2_HI = 1.57079632679489655800e+00, PIO2_LO = 6.12323399573676603587e-17;

/* Payne-Hanek: for |x| = m * 2^(e-52), only 2/pi bits from e-53 on reach x * 2/pi mod 4, and 192 of them leave a
 * 190-bit fraction, ample for the worst case of a double lying near a multiple of pi/2. */
static int reduce_large(double x, double *y0, double *y1)
{
  uint64_t u = bits_of(x);
  int e = EXP_OF(u) - 0x3ff;
  uint64_t m = (u & (~0ull >> 12)) | 1ull << 52;
  int t = e - 53;
  uint64_t w0 = two_over_pi_bits(t), w1 = two_over_pi_bits(t + 64), w2 = two_over_pi_bits(t + 128);

  /* P = m * W mod 2^192 = r0:r1:r2; the top two bits are the quadrant, the rest the fraction. */
  unsigned __int128 p = (unsigned __int128)m * w2;
  uint64_t r2 = (uint64_t)p;
  p = (unsigned __int128)m * w1 + (uint64_t)(p >> 64);
  uint64_t r1 = (uint64_t)p;
  uint64_t r0 = (uint64_t)((unsigned __int128)m * w0 + (uint64_t)(p >> 64));

  int n = (int)(r0 >> 62), neg = 0;
  uint64_t a[3] = {r0 & ((1ull << 62) - 1), r1, r2}; /* the fraction, times 2^190 */
  if (a[0] >> 61) { /* past one half: round up to the next quadrant and take 1 - fraction, negated */
    int carry = a[2] == 0;
    a[2] = ~a[2] + 1;
    a[1] = ~a[1] + (uint64_t)carry;
    carry = carry && a[1] == 0;
    a[0] = (~a[0] + (uint64_t)carry) & ((1ull << 62) - 1);
    n++;
    neg = 1;
  }

  /* Normalize so a[0] carries the leading bit; the fraction is then (a[0] + a[1] / 2^64) * 2^(-62 - shift). */
  int shift = 0;
  while (a[0] == 0 && shift < 192) {
    a[0] = a[1];
    a[1] = a[2];
    a[2] = 0;
    shift += 64;
  }
  if (a[0] == 0) {
    *y0 = *y1 = 0.0;
    return n;
  }
  int lz = __builtin_clzll(a[0]);
  if (lz) {
    a[0] = a[0] << lz | a[1] >> (64 - lz);
    a[1] = a[1] << lz | a[2] >> (64 - lz);
  }
  shift += lz;
  double hi = scalbn((double)(a[0] & ~0x7ffull), -62 - shift); /* the top 53 bits, exactly */
  double lo = scalbn((double)(a[0] & 0x7ff) + (double)a[1] * 0x1p-64, -62 - shift);

  double ph, pl;
  two_product(hi, PIO2_HI, &ph, &pl);
  pl += hi * PIO2_LO + lo * PIO2_HI;
  double r = ph + pl;
  double tail = pl - (r - ph);
  if (neg) {
    r = -r;
    tail = -tail;
  }
  *y0 = r;
  *y1 = tail;
  return n;
}

static const double INV_PIO2 = 6.36619772367581382433e-01;
static const double PIO2_1 = 1.57079632673412561417e+00, PIO2_1T = 6.07710050650619224932e-11,
                    PIO2_2 = 6.07710050630396597660e-11, PIO2_2T = 2.02226624879595063154e-21,
                    PIO2_3 = 2.02226624871116645580e-21, PIO2_3T = 8.47842766036889956997e-32;

/* Reduce x to y0 + y1 in [-pi/4, pi/4] and return the quadrant: Cody-Waite with pi/2 in three parts up to
 * 2^20 * pi/2, where n * PIO2_k stays exact, Payne-Hanek beyond. */
static int rem_pio2(double x, double *y0, double *y1)
{
  double ax = x < 0 ? -x : x;
  if (ax <= M_PI_4) {
    *y0 = x;
    *y1 = 0.0;
    return 0;
  }
  if (ax >= 0x1p20 * PIO2_HI) {
    int n = reduce_large(ax, y0, y1);
    if (x < 0) {
      *y0 = -*y0;
      *y1 = -*y1;
      n = -n;
    }
    return n;
  }
  double fn = rint(x * INV_PIO2);
  int n = (int)fn, j = EXP_OF(bits_of(x));
  double r = x - fn * PIO2_1, w = fn * PIO2_1T;
  *y0 = r - w;
  if (j - EXP_OF(bits_of(*y0)) > 16) { /* cancellation ate the first part's 33 bits: subtract the next */
    double t = r;
    w = fn * PIO2_2;
    r = t - w;
    w = fn * PIO2_2T - ((t - r) - w);
    *y0 = r - w;
    if (j - EXP_OF(bits_of(*y0)) > 49) {
      t = r;
      w = fn * PIO2_3;
      r = t - w;
      w = fn * PIO2_3T - ((t - r) - w);
      *y0 = r - w;
    }
  }
  *y1 = (r - *y0) - w;
  return n;
}

double sin(double x)
{
  if (is_nan(x) || x == INFINITY || x == -INFINITY)
    return NAN;
  if ((x < 0 ? -x : x) < 0x1p-26)
    return x;
  double y0, y1;
  switch (rem_pio2(x, &y0, &y1) & 3) {
  case 0:
    return ksin(y0, y1);
  case 1:
    return kcos(y0, y1);
  case 2:
    return -ksin(y0, y1);
  default:
    return -kcos(y0, y1);
  }
}

double cos(double x)
{
  if (is_nan(x) || x == INFINITY || x == -INFINITY)
    return NAN;
  double y0, y1;
  switch (rem_pio2(x, &y0, &y1) & 3) {
  case 0:
    return kcos(y0, y1);
  case 1:
    return -ksin(y0, y1);
  case 2:
    return -kcos(y0, y1);
  default:
    return ksin(y0, y1);
  }
}

double tan(double x)
{
  if (is_nan(x) || x == INFINITY || x == -INFINITY)
    return NAN;
  if ((x < 0 ? -x : x) < 0x1p-27)
    return x;
  double y0, y1;
  int n = rem_pio2(x, &y0, &y1);
  double s = ksin(y0, y1), c = kcos(y0, y1);
  return n & 1 ? -c / s : s / c;
}

static const double ATAN_HI[4] = {4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01,
                                  1.57079632679489655800e+00};
static const double ATAN_LO[4] = {2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17,
                                  6.12323399573676603587e-17};
static const double AT[11] = {3.33333333333329318027e-01,  -1.99999999998764832476e-01, 1.42857142725034663711e-01,
                              -1.11111104054623557880e-01, 9.09088713343650656196e-02,  -7.69187620504482999495e-02,
                              6.66107313738753120669e-02,  -5.83357013379057348645e-02, 4.97687799461593236017e-02,
                              -3.65315727442169155270e-02, 1.62858201153657823623e-02};

/* atan by fdlibm's four breakpoints: atan(x) = atan(c) + atan((x - c) / (1 + c*x)), the rest by polynomial. */
static double atan_core(double x)
{
  uint32_t ix = (uint32_t)(bits_of(x) >> 32) & 0x7fffffff;
  int neg = x < 0, id;
  if (ix >= 0x44100000) /* |x| >= 2^66 */
    return is_nan(x) ? x : neg ? -ATAN_HI[3] - ATAN_LO[3] : ATAN_HI[3] + ATAN_LO[3];
  if (ix < 0x3fdc0000) { /* |x| < 7/16 */
    if (ix < 0x3e400000) /* |x| < 2^-27 */
      return x;
    id = -1;
  } else {
    x = neg ? -x : x;
    if (ix < 0x3ff30000) { /* |x| < 19/16 */
      if (ix < 0x3fe60000) {
        id = 0;
        x = (2.0 * x - 1.0) / (2.0 + x);
      } else {
        id = 1;
        x = (x - 1.0) / (x + 1.0);
      }
    } else if (ix < 0x40038000) { /* |x| < 39/16 */
      id = 2;
      x = (x - 1.5) / (1.0 + 1.5 * x);
    } else {
      id = 3;
      x = -1.0 / x;
    }
  }
  double z = x * x, w = z * z;
  double s1 = z * (AT[0] + w * (AT[2] + w * (AT[4] + w * (AT[6] + w * (AT[8] + w * AT[10])))));
  double s2 = w * (AT[1] + w * (AT[3] + w * (AT[5] + w * (AT[7] + w * AT[9]))));
  if (id < 0)
    return x - x * (s1 + s2);
  z = ATAN_HI[id] - ((x * (s1 + s2) - ATAN_LO[id]) - x);
  return neg ? -z : z;
}

static const double PI = 3.1415926535897931160e+00, PI_LO = 1.2246467991473531772e-16;

double atan2(double y, double x)
{
  if (is_nan(x) || is_nan(y))
    return x + y;
  if (x == 1.0)
    return atan_core(y);
  int m = (int)(bits_of(y) >> 63) | (int)(bits_of(x) >> 63) << 1; /* 2*sign(x) + sign(y) */
  if (y == 0.0)
    return m == 2 ? PI : m == 3 ? -PI : y;
  if (x == 0.0)
    return m & 1 ? -M_PI_2 : M_PI_2;
  if (x == INFINITY || x == -INFINITY) {
    if (y == INFINITY || y == -INFINITY) {
      static const double corner[4] = {M_PI_4, -M_PI_4, 3.0 * M_PI_4, -3.0 * M_PI_4};
      return corner[m];
    }
    static const double edge[4] = {0.0, -0.0, PI, -PI};
    return edge[m];
  }
  if (y == INFINITY || y == -INFINITY)
    return m & 1 ? -M_PI_2 : M_PI_2;

  int k = (EXP_OF(bits_of(y)) - EXP_OF(bits_of(x)));
  double z;
  if (k > 60) {
    z = M_PI_2 + 0.5 * PI_LO;
    m &= 1;
  } else if ((m & 2) && k < -60) {
    z = 0.0;
  } else {
    z = atan_core(from_bits(bits_of(y / x) & ~SIGN_BIT)); /* |y/x|, keeping a -0 quotient at +0 */
  }
  switch (m) {
  case 0:
    return z;
  case 1:
    return -z;
  case 2:
    return PI - (z - PI_LO);
  default:
    return (z - PI_LO) - PI;
  }
}
