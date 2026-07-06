/* bigint.c — arbitrary-precision integer arithmetic. See bigint.h. */
#include "bigint.h"
#include "owos.h"   /* malloc/free/realloc, mem* */
#include "math.h"   /* floor                                                  */

/* Ceiling on the limb count of any result, so a hostile `1n << 10000000000n`
 * or a runaway `**` raises RangeError instead of exhausting the heap. 16384
 * limbs is ~512 Kbit — far beyond any real BigInt while still bounding memory. */
#define BI_MAX_LIMBS 16384
#define LIMB_BASE    4294967296.0     /* 2^32 as a double (exact) */

/* ------------------------------------------------------- allocation -------- */

static bigint *bi_new(int cap) {
    bigint *b = (bigint *)malloc(sizeof *b);
    if (!b) return NULL;
    if (cap < 1) cap = 1;
    b->limb = (unsigned *)malloc((unsigned)cap * sizeof(unsigned));
    if (!b->limb) { free(b); return NULL; }
    b->cap = cap; b->len = 0; b->sign = 0;
    return b;
}

void bi_free(bigint *b) { if (b) { if (b->limb) free(b->limb); free(b); } }

static void norm(bigint *b) {
    while (b->len > 0 && b->limb[b->len - 1] == 0) b->len--;
    if (b->len == 0) b->sign = 0;
}

/* set a sign onto a freshly built magnitude (zero stays sign 0) */
static void set_sign(bigint *b, int s) { b->sign = (b->len == 0) ? 0 : s; }

bigint *bi_copy(const bigint *a) {
    bigint *b = bi_new(a->len ? a->len : 1);
    int i;
    if (!b) return NULL;
    for (i = 0; i < a->len; i++) b->limb[i] = a->limb[i];
    b->len = a->len; b->sign = a->sign;
    return b;
}

/* ------------------------------------------------------- magnitude core --- */

/* compare magnitudes only: -1/0/1 */
static int mag_cmp(const bigint *a, const bigint *b) {
    int i;
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    for (i = a->len - 1; i >= 0; i--)
        if (a->limb[i] != b->limb[i]) return a->limb[i] < b->limb[i] ? -1 : 1;
    return 0;
}

/* |a| + |b| */
static bigint *mag_add(const bigint *a, const bigint *b) {
    int n = (a->len > b->len ? a->len : b->len) + 1, i;
    unsigned long long carry = 0;
    bigint *r;
    if (n > BI_MAX_LIMBS) return NULL;
    r = bi_new(n);
    if (!r) return NULL;
    for (i = 0; i < n; i++) {
        unsigned long long s = carry;
        if (i < a->len) s += a->limb[i];
        if (i < b->len) s += b->limb[i];
        r->limb[i] = (unsigned)s;
        carry = s >> 32;
    }
    r->len = n; norm(r); set_sign(r, 1);
    return r;
}

/* |a| - |b|, requires |a| >= |b| */
static bigint *mag_sub(const bigint *a, const bigint *b) {
    int n = a->len, i;
    long long borrow = 0;
    bigint *r = bi_new(n ? n : 1);
    if (!r) return NULL;
    for (i = 0; i < n; i++) {
        long long d = (long long)a->limb[i] - borrow - (i < b->len ? (long long)b->limb[i] : 0);
        if (d < 0) { d += 0x100000000LL; borrow = 1; } else borrow = 0;
        r->limb[i] = (unsigned)d;
    }
    r->len = n; norm(r); set_sign(r, 1);
    return r;
}

/* r = |r| - |b| in place, requires |r| >= |b| */
static void mag_sub_inplace(bigint *r, const bigint *b) {
    long long borrow = 0;
    int i;
    for (i = 0; i < r->len; i++) {
        long long d = (long long)r->limb[i] - borrow - (i < b->len ? (long long)b->limb[i] : 0);
        if (d < 0) { d += 0x100000000LL; borrow = 1; } else borrow = 0;
        r->limb[i] = (unsigned)d;
    }
    norm(r);
}

/* r <<= 1 (magnitude); r->cap must have room for one more limb */
static void mag_shl1(bigint *r) {
    unsigned carry = 0;
    int i, n = r->len;
    for (i = 0; i < n; i++) {
        unsigned nv = (r->limb[i] << 1) | carry;
        carry = r->limb[i] >> 31;
        r->limb[i] = nv;
    }
    if (carry) { r->limb[n] = carry; r->len = n + 1; }
}

/* magnitude * small + add (add folded into the low limb); fresh result */
static bigint *mag_mul_add_small(const bigint *b, unsigned mul, unsigned add) {
    int n = b->len + 1, i;
    unsigned long long carry = add;
    bigint *r;
    if (n > BI_MAX_LIMBS) return NULL;
    r = bi_new(n);
    if (!r) return NULL;
    for (i = 0; i < b->len; i++) {
        unsigned long long cur = (unsigned long long)b->limb[i] * mul + carry;
        r->limb[i] = (unsigned)cur;
        carry = cur >> 32;
    }
    r->limb[b->len] = (unsigned)carry;
    r->len = n; norm(r); set_sign(r, 1);
    return r;
}

/* divide magnitude in place by a small divisor, return the remainder */
static unsigned mag_divmod_small(bigint *b, unsigned d) {
    unsigned long long rem = 0;
    int i;
    for (i = b->len - 1; i >= 0; i--) {
        unsigned long long cur = (rem << 32) | b->limb[i];
        b->limb[i] = (unsigned)(cur / d);
        rem = cur % d;
    }
    norm(b);
    return (unsigned)rem;
}

/* ------------------------------------------------------- constructors ----- */

bigint *bi_from_i64(long long v) {
    bigint *b = bi_new(2);
    unsigned long long m;
    if (!b) return NULL;
    if (v == 0) { b->len = 0; b->sign = 0; return b; }
    m = (v < 0) ? (~(unsigned long long)v + 1ULL) : (unsigned long long)v;
    b->limb[0] = (unsigned)m;
    b->limb[1] = (unsigned)(m >> 32);
    b->len = b->limb[1] ? 2 : 1;
    b->sign = (v < 0) ? -1 : 1;
    return b;
}

bigint *bi_from_double(double d, int *ok) {
    int neg, i = 0, cap = 4;
    unsigned *tmp;
    bigint *b;
    if (d != d || d == 1.0 / 0.0 || d == -1.0 / 0.0 || floor(d) != d) { if (ok) *ok = 0; return NULL; }
    if (ok) *ok = 1;
    if (d == 0.0) return bi_from_i64(0);
    neg = d < 0; if (neg) d = -d;
    tmp = (unsigned *)malloc((unsigned)cap * sizeof(unsigned));
    if (!tmp) { if (ok) *ok = 0; return NULL; }
    while (d >= 1.0) {
        double q = floor(d / LIMB_BASE);
        double rem = d - q * LIMB_BASE;
        if (i == cap) {
            unsigned *nt;
            cap *= 2;
            if (cap > BI_MAX_LIMBS) { free(tmp); if (ok) *ok = 0; return NULL; }
            nt = (unsigned *)realloc(tmp, (unsigned)cap * sizeof(unsigned));
            if (!nt) { free(tmp); if (ok) *ok = 0; return NULL; }
            tmp = nt;
        }
        tmp[i++] = (unsigned)rem;
        d = q;
    }
    b = bi_new(i ? i : 1);
    if (!b) { free(tmp); if (ok) *ok = 0; return NULL; }
    { int k; for (k = 0; k < i; k++) b->limb[k] = tmp[k]; }
    b->len = i; free(tmp);
    norm(b); set_sign(b, neg ? -1 : 1);
    return b;
}

bigint *bi_from_str(const char *s, int len, int *ok) {
    int i = 0, neg = 0, radix = 10, any = 0;
    bigint *b;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f' || s[i] == '\v')) i++;
    while (len > i && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == '\f' || s[len - 1] == '\v')) len--;
    if (i >= len) { if (ok) *ok = 1; return bi_from_i64(0); }   /* empty -> 0n */
    if (s[i] == '+') i++;
    else if (s[i] == '-') { neg = 1; i++; }
    if (i + 1 < len && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { radix = 16; i += 2; }
    else if (i + 1 < len && s[i] == '0' && (s[i + 1] == 'o' || s[i + 1] == 'O')) { radix = 8; i += 2; }
    else if (i + 1 < len && s[i] == '0' && (s[i + 1] == 'b' || s[i + 1] == 'B')) { radix = 2; i += 2; }
    b = bi_from_i64(0);
    if (!b) return NULL;
    for (; i < len; i++) {
        int c = (unsigned char)s[i], d;
        if (c == '_') continue;                    /* numeric separators */
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else { bi_free(b); if (ok) *ok = 0; return NULL; }
        if (d >= radix) { bi_free(b); if (ok) *ok = 0; return NULL; }
        { bigint *t = mag_mul_add_small(b, (unsigned)radix, (unsigned)d); bi_free(b); b = t; if (!b) return NULL; }
        any = 1;
    }
    if (!any) { bi_free(b); if (ok) *ok = 0; return NULL; }   /* lone sign / "0x" */
    if (ok) *ok = 1;
    set_sign(b, neg ? -1 : 1);
    return b;
}

/* ------------------------------------------------------- queries ---------- */

int    bi_sign(const bigint *a)    { return a->sign; }
int    bi_is_zero(const bigint *a) { return a->len == 0; }
int    bi_to_bool(const bigint *a) { return a->len != 0; }

int bi_cmp(const bigint *a, const bigint *b) {
    if (a->sign != b->sign) return a->sign < b->sign ? -1 : 1;
    if (a->sign == 0) return 0;
    { int c = mag_cmp(a, b); return a->sign < 0 ? -c : c; }
}

double bi_to_double(const bigint *a) {
    double r = 0;
    int i;
    for (i = a->len - 1; i >= 0; i--) r = r * LIMB_BASE + a->limb[i];
    return a->sign < 0 ? -r : r;
}

long long bi_to_i64(const bigint *a, int *ok) {
    unsigned long long m = 0;
    int i;
    if (a->len > 2) { if (ok) *ok = 0; return 0; }
    for (i = a->len - 1; i >= 0; i--) m = (m << 32) | a->limb[i];
    if (a->sign < 0) {
        if (m > 0x8000000000000000ULL) { if (ok) *ok = 0; return 0; }
        if (ok) *ok = 1;
        return (long long)(~m + 1ULL);
    }
    if (m > 0x7FFFFFFFFFFFFFFFULL) { if (ok) *ok = 0; return 0; }
    if (ok) *ok = 1;
    return (long long)m;
}

/* ------------------------------------------------------- arithmetic ------- */

bigint *bi_neg(const bigint *a) {
    bigint *r = bi_copy(a);
    if (r) set_sign(r, -a->sign);
    return r;
}

bigint *bi_add(const bigint *a, const bigint *b) {
    if (a->sign == 0) return bi_copy(b);
    if (b->sign == 0) return bi_copy(a);
    if (a->sign == b->sign) { bigint *r = mag_add(a, b); if (r) set_sign(r, a->sign); return r; }
    {
        int c = mag_cmp(a, b);
        if (c == 0) return bi_from_i64(0);
        if (c > 0) { bigint *r = mag_sub(a, b); if (r) set_sign(r, a->sign); return r; }
        { bigint *r = mag_sub(b, a); if (r) set_sign(r, b->sign); return r; }
    }
}

bigint *bi_sub(const bigint *a, const bigint *b) {
    bigint nb = *b;                 /* shallow negate; bi_add only reads b's limbs */
    nb.sign = -b->sign;
    return bi_add(a, &nb);
}

bigint *bi_mul(const bigint *a, const bigint *b) {
    int n, i, j;
    bigint *r;
    if (a->sign == 0 || b->sign == 0) return bi_from_i64(0);
    n = a->len + b->len;
    if (n > BI_MAX_LIMBS) return NULL;
    r = bi_new(n);
    if (!r) return NULL;
    for (i = 0; i < n; i++) r->limb[i] = 0;
    for (i = 0; i < a->len; i++) {
        unsigned long long carry = 0, av = a->limb[i];
        for (j = 0; j < b->len; j++) {
            unsigned long long cur = (unsigned long long)r->limb[i + j] + av * b->limb[j] + carry;
            r->limb[i + j] = (unsigned)cur;
            carry = cur >> 32;
        }
        {
            int k = i + b->len;
            while (carry && k < n) {
                unsigned long long cur = (unsigned long long)r->limb[k] + carry;
                r->limb[k] = (unsigned)cur;
                carry = cur >> 32;
                k++;
            }
        }
    }
    r->len = n; norm(r); set_sign(r, a->sign * b->sign);
    return r;
}

int bi_divmod(const bigint *a, const bigint *b, bigint **qo, bigint **ro) {
    bigint *q, *r;
    int i, bits;
    if (b->sign == 0) { if (qo) *qo = NULL; if (ro) *ro = NULL; return -1; }
    if (mag_cmp(a, b) < 0) {                 /* |a| < |b|: q = 0, r = a */
        q = bi_from_i64(0); r = bi_copy(a);
        if (!q || !r) { bi_free(q); bi_free(r); return 0; }
        set_sign(r, a->sign);
        if (qo) *qo = q; else bi_free(q);
        if (ro) *ro = r; else bi_free(r);
        return 1;
    }
    q = bi_new(a->len);
    r = bi_new(b->len + 1);
    if (!q || !r) { bi_free(q); bi_free(r); return 0; }
    for (i = 0; i < q->cap; i++) q->limb[i] = 0;
    for (i = 0; i < r->cap; i++) r->limb[i] = 0;
    q->len = a->len; q->sign = 1;
    r->len = 0; r->sign = 1;
    bits = a->len * 32;
    for (i = bits - 1; i >= 0; i--) {
        unsigned bit = (a->limb[i >> 5] >> (i & 31)) & 1u;
        mag_shl1(r);
        if (bit) {
            if (r->len == 0) { r->limb[0] = 1; r->len = 1; }
            else r->limb[0] |= 1u;
        }
        if (mag_cmp(r, b) >= 0) { mag_sub_inplace(r, b); q->limb[i >> 5] |= (1u << (i & 31)); }
    }
    norm(q); norm(r);
    set_sign(q, a->sign * b->sign);
    set_sign(r, a->sign);
    if (qo) *qo = q; else bi_free(q);
    if (ro) *ro = r; else bi_free(r);
    return 1;
}

bigint *bi_pow(const bigint *a, const bigint *e, int *rc) {
    long long ei;
    int ok;
    bigint *result, *base;
    if (e->sign < 0) { if (rc) *rc = -1; return NULL; }
    ei = bi_to_i64(e, &ok);
    if (!ok || ei < 0) { if (rc) *rc = -1; return NULL; }    /* absurdly large exponent */
    result = bi_from_i64(1);
    base = bi_copy(a);
    if (!result || !base) { bi_free(result); bi_free(base); if (rc) *rc = 0; return NULL; }
    while (ei > 0) {
        if (ei & 1) {
            bigint *t = bi_mul(result, base);
            bi_free(result); result = t;
            if (!result) { bi_free(base); if (rc) *rc = 0; return NULL; }
        }
        ei >>= 1;
        if (ei) {
            bigint *t = bi_mul(base, base);
            bi_free(base); base = t;
            if (!base) { bi_free(result); if (rc) *rc = 0; return NULL; }
        }
    }
    bi_free(base);
    if (rc) *rc = 1;
    return result;
}

/* ------------------------------------------------------- bitwise ---------- */

/* n-limb little-endian two's complement of a (n must exceed a's limb count) */
static unsigned *to_twos(const bigint *a, int n) {
    unsigned *t = (unsigned *)malloc((unsigned)n * sizeof(unsigned));
    int i;
    if (!t) return NULL;
    for (i = 0; i < n; i++) t[i] = (i < a->len) ? a->limb[i] : 0u;
    if (a->sign < 0) {
        unsigned long long carry = 1;
        for (i = 0; i < n; i++) { unsigned long long v = (unsigned long long)(~t[i]) + carry; t[i] = (unsigned)v; carry = v >> 32; }
    }
    return t;
}

/* interpret an n-limb two's-complement pattern (consumed/mutated) as a bigint */
static bigint *from_twos(unsigned *t, int n) {
    int neg = (t[n - 1] >> 31) & 1, i;
    bigint *b;
    if (neg) {
        unsigned long long carry = 1;                 /* magnitude = (~t)+1 */
        for (i = 0; i < n; i++) { unsigned long long v = (unsigned long long)(~t[i]) + carry; t[i] = (unsigned)v; carry = v >> 32; }
    }
    b = bi_new(n);
    if (!b) return NULL;
    for (i = 0; i < n; i++) b->limb[i] = t[i];
    b->len = n; b->sign = 1;
    norm(b); set_sign(b, neg ? -1 : 1);
    return b;
}

static bigint *bitwise(const bigint *a, const bigint *b, int op) {
    int n = (a->len > b->len ? a->len : b->len) + 1, i;
    unsigned *ta, *tb;
    bigint *r;
    if (n > BI_MAX_LIMBS) return NULL;
    ta = to_twos(a, n);
    tb = to_twos(b, n);
    if (!ta || !tb) { free(ta); free(tb); return NULL; }
    for (i = 0; i < n; i++) {
        switch (op) {
        case 0: ta[i] &= tb[i]; break;
        case 1: ta[i] |= tb[i]; break;
        default: ta[i] ^= tb[i]; break;
        }
    }
    r = from_twos(ta, n);
    free(ta); free(tb);
    return r;
}

bigint *bi_and(const bigint *a, const bigint *b) { return bitwise(a, b, 0); }
bigint *bi_or (const bigint *a, const bigint *b) { return bitwise(a, b, 1); }
bigint *bi_xor(const bigint *a, const bigint *b) { return bitwise(a, b, 2); }

bigint *bi_not(const bigint *a) {                 /* ~a == -(a + 1) */
    bigint *one = bi_from_i64(1), *s, *r;
    if (!one) return NULL;
    s = bi_add(a, one);
    bi_free(one);
    if (!s) return NULL;
    r = bi_neg(s);
    bi_free(s);
    return r;
}

/* 2^k as a magnitude, k >= 0 */
static bigint *bi_pow2(long long k) {
    int limbs = (int)(k / 32) + 1, i;
    bigint *b;
    if (limbs > BI_MAX_LIMBS) return NULL;
    b = bi_new(limbs);
    if (!b) return NULL;
    for (i = 0; i < limbs; i++) b->limb[i] = 0;
    b->limb[(int)(k / 32)] = 1u << (k % 32);
    b->len = limbs; b->sign = 1;
    norm(b);
    return b;
}

/* floor(a / b) — b must be positive (used with powers of two and moduli) */
static bigint *bi_floordiv(const bigint *a, const bigint *b) {
    bigint *q, *r;
    if (bi_divmod(a, b, &q, &r) != 1) return NULL;
    if (!bi_is_zero(r) && (a->sign < 0) != (b->sign < 0)) {   /* round toward -inf */
        bigint *one = bi_from_i64(1), *q2;
        if (!one) { bi_free(q); bi_free(r); return NULL; }
        q2 = bi_sub(q, one);
        bi_free(one); bi_free(q); bi_free(r);
        return q2;
    }
    bi_free(r);
    return q;
}

/* a mod m in [0, m), m positive */
static bigint *bi_floormod(const bigint *a, const bigint *m) {
    bigint *q, *r;
    if (bi_divmod(a, m, &q, &r) != 1) return NULL;
    bi_free(q);
    if (r->sign < 0) { bigint *r2 = bi_add(r, m); bi_free(r); return r2; }
    return r;
}

bigint *bi_shl(const bigint *a, long long n) {
    bigint *p, *r;
    if (n < 0) return bi_shr(a, -n);
    if (bi_is_zero(a)) return bi_from_i64(0);
    p = bi_pow2(n);
    if (!p) return NULL;
    r = bi_mul(a, p);
    bi_free(p);
    return r;
}

bigint *bi_shr(const bigint *a, long long n) {
    bigint *p, *r;
    if (n < 0) return bi_shl(a, -n);
    if (bi_is_zero(a)) return bi_from_i64(0);
    if (n >= (long long)a->len * 32 + 1)          /* shifted past the top bit */
        return bi_from_i64(a->sign < 0 ? -1 : 0);
    p = bi_pow2(n);
    if (!p) return NULL;
    r = bi_floordiv(a, p);
    bi_free(p);
    return r;
}

bigint *bi_as_uintn(int bits, const bigint *a) {
    bigint *m, *r;
    if (bits <= 0) return bi_from_i64(0);
    m = bi_pow2(bits);
    if (!m) return NULL;
    r = bi_floormod(a, m);
    bi_free(m);
    return r;
}

bigint *bi_as_intn(int bits, const bigint *a) {
    bigint *u, *half;
    if (bits <= 0) return bi_from_i64(0);
    u = bi_as_uintn(bits, a);
    if (!u) return NULL;
    half = bi_pow2(bits - 1);
    if (!half) { bi_free(u); return NULL; }
    if (bi_cmp(u, half) >= 0) {                    /* u >= 2^(bits-1) -> u - 2^bits */
        bigint *m = bi_pow2(bits), *r;
        if (!m) { bi_free(u); bi_free(half); return NULL; }
        r = bi_sub(u, m);
        bi_free(m); bi_free(u); bi_free(half);
        return r;
    }
    bi_free(half);
    return u;
}

/* ------------------------------------------------------- to string -------- */

char *bi_to_str(const bigint *a, int radix) {
    bigint *t;
    char *buf;
    int cap, k = 0, i, neg;
    if (radix < 2 || radix > 36) radix = 10;
    if (a->sign == 0) { buf = (char *)malloc(2); if (!buf) return NULL; buf[0] = '0'; buf[1] = 0; return buf; }
    t = bi_copy(a);
    if (!t) return NULL;
    neg = t->sign < 0; t->sign = 1;
    cap = a->len * 32 + 3;                          /* base-2 is the widest form */
    buf = (char *)malloc((unsigned)cap);
    if (!buf) { bi_free(t); return NULL; }
    while (t->len > 0) {
        unsigned rem = mag_divmod_small(t, (unsigned)radix);
        buf[k++] = (rem < 10) ? (char)('0' + rem) : (char)('a' + rem - 10);
    }
    if (neg) buf[k++] = '-';
    for (i = 0; i < k / 2; i++) { char c = buf[i]; buf[i] = buf[k - 1 - i]; buf[k - 1 - i] = c; }
    buf[k] = 0;
    bi_free(t);
    return buf;
}
