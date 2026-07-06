/* bigint.h — arbitrary-precision signed integer (sign-magnitude, base 2^32).
 *
 * A standalone bignum with no engine or GC coupling: a `bigint` is a plain
 * malloc'd value the caller owns and releases with bi_free. The JS BigInt
 * primitive wraps one behind a GC cell (see js_mk_bigint in value.c); the
 * two's-complement bitwise ops and the radix string forms exist to serve that
 * binding but are generally useful.
 *
 * Every result-returning op allocates a fresh bigint and returns NULL on OOM or
 * when the result would exceed BI_MAX_LIMBS (the caller raises RangeError). The
 * magnitude is normalized: no leading zero limb, and len==0 iff the value is 0
 * (in which case sign==0 and limb may be unused).
 */
#ifndef OWOS_JS_BIGINT_H
#define OWOS_JS_BIGINT_H

typedef struct {
    int       sign;   /* -1, 0, +1; 0 iff the value is zero (len == 0)        */
    int       len;    /* significant limbs                                    */
    int       cap;    /* allocated limbs                                      */
    unsigned *limb;   /* little-endian base-2^32 magnitude                    */
} bigint;

void      bi_free(bigint *b);
bigint   *bi_copy(const bigint *a);

/* constructors (fresh, NULL on failure) */
bigint   *bi_from_i64(long long v);
bigint   *bi_from_double(double d, int *ok);            /* ok=0 unless d is a finite integer */
bigint   *bi_from_str(const char *s, int len, int *ok); /* radix from 0x/0o/0b prefix, else 10 */

/* queries */
int       bi_sign(const bigint *a);                     /* -1/0/1                        */
int       bi_is_zero(const bigint *a);
int       bi_cmp(const bigint *a, const bigint *b);     /* -1/0/1                        */
int       bi_to_bool(const bigint *a);
double    bi_to_double(const bigint *a);
long long bi_to_i64(const bigint *a, int *ok);          /* ok=0 if outside int64 range   */

/* arithmetic (fresh result; NULL on OOM / size cap, or as noted) */
bigint   *bi_neg(const bigint *a);
bigint   *bi_add(const bigint *a, const bigint *b);
bigint   *bi_sub(const bigint *a, const bigint *b);
bigint   *bi_mul(const bigint *a, const bigint *b);
/* truncated division: quotient toward zero, remainder takes the dividend's sign.
 * Returns 1 on success, 0 on OOM, -1 on divide-by-zero. q and/or r may be NULL. */
int       bi_divmod(const bigint *a, const bigint *b, bigint **q, bigint **r);
bigint   *bi_pow(const bigint *a, const bigint *e, int *rc); /* rc: 1 ok, 0 OOM, -1 negative/absurd exp */

/* bitwise, infinite-precision two's complement */
bigint   *bi_and(const bigint *a, const bigint *b);
bigint   *bi_or (const bigint *a, const bigint *b);
bigint   *bi_xor(const bigint *a, const bigint *b);
bigint   *bi_not(const bigint *a);
bigint   *bi_shl(const bigint *a, long long n);         /* n<0 shifts right (arithmetic) */
bigint   *bi_shr(const bigint *a, long long n);         /* arithmetic; n<0 shifts left   */

/* BigInt.asIntN / asUintN (bits >= 0) */
bigint   *bi_as_intn(int bits, const bigint *a);
bigint   *bi_as_uintn(int bits, const bigint *a);

/* base-`radix` (2..36) string, malloc'd NUL-terminated; NULL on OOM */
char     *bi_to_str(const bigint *a, int radix);

#endif /* OWOS_JS_BIGINT_H */
