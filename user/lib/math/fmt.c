/* fmt.c — number <-> string. See fmt.h for the contract.
 *
 * dtoa is Dragon4 (Steele & White, "How to Print Floating-Point Numbers
 * Accurately", FPP2 free-format) over a small base-2^32 bignum: it emits the
 * shortest decimal digit string that rounds back to the exact input double,
 * correctly rounded, then a formatter places the decimal point / exponent. No
 * float tricks, no precision tables — just exact integer arithmetic, which is
 * why it is always right.
 */

#include "fmt.h"
#include "math.h"

typedef union {
  double d;
  unsigned long long u;
} dbits;

/* ----------------------------------------------------------------- bignum ---
 */
/* Little-endian base-2^32. The widest value Dragon4 forms is the scaling
 * denominator S ~ 2^1074 * 10^17, about 1140 bits, so 64 limbs is comfortable.
 */
#define BN_LIMBS 64
typedef struct {
  unsigned v[BN_LIMBS];
  int n;
} Bn;

static void bn_set_u64(Bn *a, unsigned long long x)
{
  a->v[0] = (unsigned)x;
  a->v[1] = (unsigned)(x >> 32);
  a->n = a->v[1] ? 2 : (a->v[0] ? 1 : 0);
}

static int bn_cmp(const Bn *a, const Bn *b)
{
  if (a->n != b->n)
    return a->n < b->n ? -1 : 1;
  for (int i = a->n - 1; i >= 0; i--)
    if (a->v[i] != b->v[i])
      return a->v[i] < b->v[i] ? -1 : 1;
  return 0;
}

static void bn_mul_small(Bn *a, unsigned m)
{
  unsigned long long carry = 0;
  for (int i = 0; i < a->n; i++) {
    unsigned long long cur = (unsigned long long)a->v[i] * m + carry;
    a->v[i] = (unsigned)cur;
    carry = cur >> 32;
  }
  while (carry) {
    a->v[a->n++] = (unsigned)carry;
    carry >>= 32;
  }
}

static void bn_add(Bn *a, const Bn *b)
{
  int n = a->n > b->n ? a->n : b->n;
  unsigned long long carry = 0;
  for (int i = 0; i < n; i++) {
    unsigned long long s = carry + (i < a->n ? a->v[i] : 0) + (i < b->n ? b->v[i] : 0);
    a->v[i] = (unsigned)s;
    carry = s >> 32;
  }
  a->n = n;
  if (carry)
    a->v[a->n++] = (unsigned)carry;
}

/* a -= b, assuming a >= b. */
static void bn_sub(Bn *a, const Bn *b)
{
  long long borrow = 0;
  for (int i = 0; i < a->n; i++) {
    long long d = (long long)a->v[i] - (i < b->n ? b->v[i] : 0) - borrow;
    if (d < 0) {
      d += 0x100000000LL;
      borrow = 1;
    } else
      borrow = 0;
    a->v[i] = (unsigned)d;
  }
  while (a->n > 0 && a->v[a->n - 1] == 0)
    a->n--;
}

static void bn_shl(Bn *a, int bits)
{
  if (a->n == 0 || bits == 0)
    return;
  int ls = bits / 32, bs = bits % 32;
  if (bs) {
    unsigned carry = 0;
    for (int i = 0; i < a->n; i++) {
      unsigned long long cur = ((unsigned long long)a->v[i] << bs) | carry;
      a->v[i] = (unsigned)cur;
      carry = (unsigned)(cur >> 32);
    }
    if (carry)
      a->v[a->n++] = carry;
  }
  if (ls) {
    for (int i = a->n - 1; i >= 0; i--)
      a->v[i + ls] = a->v[i];
    for (int i = 0; i < ls; i++)
      a->v[i] = 0;
    a->n += ls;
  }
}

/* -------------------------------------------------------------- Dragon4 -----
 */
/* For finite v > 0: write the shortest correctly-rounded decimal digits (no
 * trailing zeros) to `digits` and set *kout so that v = 0.<digits> *
 * 10^(*kout). Returns the digit count. */
static int dragon4(double v, char *digits, int *kout)
{
  dbits b;
  b.d = v;
  unsigned exp = (unsigned)((b.u >> 52) & 0x7ff);
  unsigned long long mant = b.u & 0xfffffffffffffULL;
  unsigned long long f;
  int e;
  if (exp == 0) {
    f = mant;
    e = -1074;
  } /* subnormal          */
  else {
    f = mant | 0x10000000000000ULL;
    e = (int)exp - 1075;
  }

  /* The gap to the next-smaller double is half-size when the significand is a
   * bare power of two (mantissa field 0) and we are not at the smallest normal.
   */
  int closer = (mant == 0 && exp > 1);

  Bn R, S, mp, mm;
  if (e >= 0) {
    bn_set_u64(&R, f);
    bn_shl(&R, e + (closer ? 2 : 1));
    bn_set_u64(&S, closer ? 4 : 2);
    bn_set_u64(&mp, 1);
    bn_shl(&mp, e + (closer ? 1 : 0));
    bn_set_u64(&mm, 1);
    bn_shl(&mm, e);
  } else {
    bn_set_u64(&R, f);
    bn_shl(&R, closer ? 2 : 1);
    bn_set_u64(&S, 1);
    bn_shl(&S, (-e) + (closer ? 2 : 1));
    bn_set_u64(&mp, closer ? 2 : 1);
    bn_set_u64(&mm, 1);
  }

  /* Scale into 0.1 <= R/S < 1 by comparison (no log estimate needed), tracking
   * the decimal exponent k. Boundaries ride on the R side. */
  int k = 0;
  while (bn_cmp(&R, &S) >= 0) {
    bn_mul_small(&S, 10);
    k++;
  }
  for (;;) {
    Bn t = R;
    bn_mul_small(&t, 10);
    if (bn_cmp(&t, &S) >= 0)
      break;
    bn_mul_small(&R, 10);
    bn_mul_small(&mp, 10);
    bn_mul_small(&mm, 10);
    k--;
  }

  /* With an even significand the rounding boundaries are inclusive (ties
   * resolve to this value), so the stop tests use <= / >=; with an odd one they
   * are strict. Getting this wrong rounds the last digit the wrong way on ~1e-4
   * of inputs — the classic Dragon4 even/odd subtlety. */
  int even = !(f & 1);

  int ndig = 0;
  for (;;) {
    bn_mul_small(&R, 10);
    bn_mul_small(&mp, 10);
    bn_mul_small(&mm, 10);
    int d = 0;
    while (bn_cmp(&R, &S) >= 0) {
      bn_sub(&R, &S);
      d++;
    }

    int cmm = bn_cmp(&R, &mm);
    int low = even ? cmm <= 0 : cmm < 0;
    Bn rp = R;
    bn_add(&rp, &mp);
    int crp = bn_cmp(&rp, &S);
    int high = even ? crp >= 0 : crp > 0;

    if (!low && !high) {
      digits[ndig++] = (char)('0' + d);
      continue;
    }

    int roundup;
    if (high && !low)
      roundup = 1;
    else if (low && !high)
      roundup = 0;
    else {
      Bn r2 = R;
      bn_mul_small(&r2, 2);
      roundup = bn_cmp(&r2, &S) > 0;
    }
    d += roundup;
    digits[ndig++] = (char)('0' + d);
    break;
  }

  /* A final 9->10 carry ripples left; if it falls off the front, prepend a 1
   * and shift the decimal exponent. Then drop any trailing zero the carry left.
   */
  int idx = ndig - 1;
  while (digits[idx] > '9') {
    digits[idx] = '0';
    if (idx == 0) {
      for (int t = ndig; t > 0; t--)
        digits[t] = digits[t - 1];
      digits[0] = '1';
      ndig++;
      k++;
      break;
    }
    digits[--idx]++;
  }
  while (ndig > 1 && digits[ndig - 1] == '0')
    ndig--;

  digits[ndig] = '\0';
  *kout = k;
  return ndig;
}

/* --------------------------------------------------- decimal point placement
 */
static int put(char *out, int i, char c)
{
  out[i] = c;
  return i + 1;
}

/* Append decimal of a non-negative int. */
static int put_uint(char *out, int i, int v)
{
  char t[12];
  int n = 0;
  if (v == 0)
    t[n++] = '0';
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n--)
    out[i++] = t[n];
  return i;
}

/* Shortest-form layout from (digits, ndig, k) where v = 0.digits * 10^k: plain
 * decimal within the usual magnitude band, exponential outside it. */
static int format_shortest(int neg, const char *s, int ndig, int k, char *out)
{
  int i = 0;
  if (neg)
    i = put(out, i, '-');
  int n = k; /* point position     */

  if (ndig <= n && n <= 21) { /* integer + zeros    */
    for (int j = 0; j < ndig; j++)
      i = put(out, i, s[j]);
    for (int j = 0; j < n - ndig; j++)
      i = put(out, i, '0');
  } else if (0 < n && n <= 21) { /* point inside digits */
    for (int j = 0; j < n; j++)
      i = put(out, i, s[j]);
    i = put(out, i, '.');
    for (int j = n; j < ndig; j++)
      i = put(out, i, s[j]);
  } else if (-6 < n && n <= 0) { /* 0.000digits         */
    i = put(out, i, '0');
    i = put(out, i, '.');
    for (int j = 0; j < -n; j++)
      i = put(out, i, '0');
    for (int j = 0; j < ndig; j++)
      i = put(out, i, s[j]);
  } else { /* exponential         */
    i = put(out, i, s[0]);
    if (ndig > 1) {
      i = put(out, i, '.');
      for (int j = 1; j < ndig; j++)
        i = put(out, i, s[j]);
    }
    i = put(out, i, 'e');
    int ex = n - 1;
    i = put(out, i, ex < 0 ? '-' : '+');
    i = put_uint(out, i, ex < 0 ? -ex : ex);
  }
  out[i] = '\0';
  return i;
}

int dtoa(double v, char *out)
{
  if (isnan(v)) {
    out[0] = 'N';
    out[1] = 'a';
    out[2] = 'N';
    out[3] = '\0';
    return 3;
  }
  int neg = signbit(v);
  if (isinf(v)) {
    const char *s = neg ? "-Infinity" : "Infinity";
    int i = 0;
    while (s[i]) {
      out[i] = s[i];
      i++;
    }
    out[i] = '\0';
    return i;
  }
  if (v == 0.0) {
    out[0] = '0';
    out[1] = '\0';
    return 1;
  }

  double a = neg ? -v : v;
  char digits[24];
  int k, ndig = dragon4(a, digits, &k);
  return format_shortest(neg, digits, ndig, k, out);
}

/* --------------------------------------------------------- radix conversion */
/* Fractional digits are capped: enough for normal use, bounded buffer. */
#define RADIX_FRAC_MAX 110

int dtoa_radix(double v, int radix, char *out)
{
  if (radix == 10)
    return dtoa(v, out);
  if (isnan(v)) {
    out[0] = 'N';
    out[1] = 'a';
    out[2] = 'N';
    out[3] = '\0';
    return 3;
  }
  int neg = signbit(v);
  if (isinf(v)) {
    const char *s = neg ? "-Infinity" : "Infinity";
    int i = 0;
    while (s[i]) {
      out[i] = s[i];
      i++;
    }
    out[i] = '\0';
    return i;
  }
  static const char dig[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  int i = 0;
  if (neg)
    out[i++] = '-';

  double a = neg ? -v : v;
  double ip = floor(a);
  double fp = a - ip;

  char tmp[1100];
  int tn = 0; /* integer part, reversed */
  if (ip == 0.0)
    tmp[tn++] = '0';
  while (ip >= 1.0) {
    double q = floor(ip / radix);
    int d = (int)(ip - q * radix);
    tmp[tn++] = dig[d];
    ip = q;
  }
  while (tn--)
    out[i++] = tmp[tn];

  if (fp > 0.0) {
    out[i++] = '.';
    for (int c = 0; c < RADIX_FRAC_MAX && fp > 0.0; c++) {
      fp *= radix;
      int d = (int)floor(fp);
      out[i++] = dig[d];
      fp -= d;
    }
  }
  out[i] = '\0';
  return i;
}

/* ------------------------------------------------------------- strtod -------
 */
static const double pow10tab[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                  1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

static int match_word(const char *s, const char *w)
{
  int i = 0;
  while (w[i]) {
    if (s[i] != w[i])
      return 0;
    i++;
  }
  return i;
}

double strtod(const char *s, char **end)
{
  const char *p = s;
  int neg = 0;
  if (*p == '+' || *p == '-') {
    neg = (*p == '-');
    p++;
  }

  int n = match_word(p, "Infinity");
  if (n) {
    if (end)
      *end = (char *)(p + n);
    return neg ? -INFINITY : INFINITY;
  }
  if (match_word(p, "NaN")) {
    if (end)
      *end = (char *)(p + 3);
    return NAN;
  }

  unsigned long long mant = 0;
  int sig = 0;   /* significant digits accumulated into mant           */
  int exp10 = 0; /* power-of-ten adjustment from the fractional point  */
  int any = 0;
  int dotseen = 0;

  for (;; p++) {
    if (*p >= '0' && *p <= '9') {
      any = 1;
      if (sig < 19) {                                /* u64 holds ~19 digits */
        if (!(mant == 0 && *p == '0' && !dotseen)) { /* skip leading zeros */
          mant = mant * 10 + (unsigned)(*p - '0');
          sig++;
          if (dotseen)
            exp10--;
        } else if (dotseen) {
          exp10--; /* 0.00x: count zeros    */
        }
      } else {
        if (!dotseen)
          exp10++; /* extra int digit       */
      }
    } else if (*p == '.' && !dotseen) {
      dotseen = 1;
    } else {
      break;
    }
  }

  if (!any) {
    if (end)
      *end = (char *)s;
    return NAN;
  } /* nothing numeric      */

  if (*p == 'e' || *p == 'E') {
    const char *q = p + 1;
    int esign = 0, edig = 0, eval = 0;
    if (*q == '+' || *q == '-') {
      esign = (*q == '-');
      q++;
    }
    while (*q >= '0' && *q <= '9') {
      eval = eval * 10 + (*q - '0');
      edig = 1;
      q++;
    }
    if (edig) {
      exp10 += esign ? -eval : eval;
      p = q;
    }
  }

  if (end)
    *end = (char *)p;
  if (mant == 0)
    return neg ? -0.0 : 0.0;

  double m = (double)mant;
  double r;
  /* Exact when the mantissa and 10^|exp10| are both exactly representable. */
  if (exp10 >= 0 && exp10 <= 22 && sig <= 15)
    r = m * pow10tab[exp10];
  else if (exp10 < 0 && -exp10 <= 22 && sig <= 15)
    r = m / pow10tab[-exp10];
  else {
    r = m;
    int e = exp10;
    while (e >= 22) {
      r *= 1e22;
      e -= 22;
    }
    while (e <= -22) {
      r /= 1e22;
      e += 22;
    }
    if (e > 0)
      r *= pow10tab[e];
    else if (e < 0)
      r /= pow10tab[-e];
  }
  return neg ? -r : r;
}

/* --------------------------------------------------------- integer to ASCII -
 */
int u64toa(unsigned long long v, char *out, int radix)
{
  static const char dig[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  char t[64];
  int n = 0;
  if (radix < 2 || radix > 36) {
    out[0] = '\0';
    return 0;
  }
  if (v == 0)
    t[n++] = '0';
  while (v) {
    t[n++] = dig[v % (unsigned)radix];
    v /= (unsigned)radix;
  }
  int i = 0;
  while (n--)
    out[i++] = t[n];
  out[i] = '\0';
  return i;
}

int i64toa(long long v, char *out, int radix)
{
  if (v < 0) {
    out[0] = '-';
    /* -(unsigned)v avoids overflow on LLONG_MIN. */
    return 1 + (int)u64toa((unsigned long long)(-(unsigned long long)v), out + 1, radix);
  }
  return u64toa((unsigned long long)v, out, radix);
}
