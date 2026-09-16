/* bigint_test.c — host unit test for the arbitrary-precision integer core.
 *
 * The bignum in user/lib/js/bigint.c is pure computation, so it is verified
 * natively (no QEMU boot). Moderate values are cross-checked against the host's
 * __int128; large values against known decimal strings.
 *
 *   cc -std=c11 -Iscripts -Iuser/lib -Iuser/lib/math \
 *      tests/bigint_test.c user/lib/js/bigint.c -lm \
 *      -o /tmp/bigint_test && /tmp/bigint_test
 */
#include "bigint.h"
#define OWOS_SUITE_TEST
#include "test.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void expect_str(bigint *b, int radix, const char *want, const char *what) {
    char *s;
    if (!b) { printf("  %s: null result\n", what); expect(0); return; }
    s = bi_to_str(b, radix);
    if (!s) { printf("  %s: out of memory\n", what); expect(0); return; }
    owos_expect_string(s, want, __FILE__, __LINE__, what);
    free(s);
}

static char *i128_str(__int128 v) {
    static char buf[64];
    char tmp[64];
    int i = 0, neg = 0;
    unsigned __int128 m;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return buf; }
    if (v < 0) { neg = 1; m = (unsigned __int128)(-(v + 1)) + 1; } else m = (unsigned __int128)v;
    while (m) { tmp[i++] = (char)('0' + (int)(m % 10)); m /= 10; }
    { int k = 0; if (neg) buf[k++] = '-'; while (i) buf[k++] = tmp[--i]; buf[k] = 0; }
    return buf;
}

/* verify add/sub/mul/div/mod of two int64 operands against __int128 */
static void check_pair(long long x, long long y) {
    bigint *a = bi_from_i64(x), *b = bi_from_i64(y), *r, *q;
    expect_str(bi_add(a, b), 10, i128_str((__int128)x + y), "add");
    expect_str(bi_sub(a, b), 10, i128_str((__int128)x - y), "sub");
    expect_str(bi_mul(a, b), 10, i128_str((__int128)x * y), "mul");
    if (y != 0) {
        if (bi_divmod(a, b, &q, &r) == 1) {
            expect_str(q, 10, i128_str((__int128)x / y), "div");
            expect_str(r, 10, i128_str((__int128)x % y), "mod");
        } else { printf("  divmod-rc: error\n"); expect(0); }
    }
    bi_free(a); bi_free(b);
}

int main(void) {
	/* round trips */
	test("integer round-trips");
    expect_str(bi_from_i64(0), 10, "0", "zero");
    expect_str(bi_from_i64(-1), 10, "-1", "neg1");
    expect_str(bi_from_i64(9223372036854775807LL), 10, "9223372036854775807", "i64max");
    expect_str(bi_from_i64(-9223372036854775807LL - 1), 10, "-9223372036854775808", "i64min");

	/* arithmetic vs __int128 over a deterministic grid */
	test("arithmetic and division grid");
    {
        long long v[] = { 0, 1, -1, 2, 7, -7, 255, 65536, -65536,
                          2147483647LL, -2147483648LL, 4294967296LL,
                          1000000007LL, -1000000007LL, 9999999999LL,
                          9223372036854775807LL, -9223372036854775807LL };
        int n = (int)(sizeof v / sizeof v[0]), i, j;
        for (i = 0; i < n; i++) for (j = 0; j < n; j++) check_pair(v[i], v[j]);
    }

	/* large exact values */
	test("large exact values");
    {
        bigint *two = bi_from_i64(2), *e64 = bi_from_i64(64), *ten = bi_from_i64(10), *e20 = bi_from_i64(20);
        int rc;
        expect_str(bi_pow(two, e64, &rc), 10, "18446744073709551616", "2^64");
        expect_str(bi_pow(ten, e20, &rc), 10, "100000000000000000000", "10^20");
        bi_free(two); bi_free(e64); bi_free(ten); bi_free(e20);
    }

	/* factorial 25! = 15511210043330985984000000 */
	test("factorial");
    {
        bigint *acc = bi_from_i64(1);
        int i;
        for (i = 2; i <= 25; i++) { bigint *m = bi_from_i64(i), *t = bi_mul(acc, m); bi_free(acc); bi_free(m); acc = t; }
        expect_str(acc, 10, "15511210043330985984000000", "25!");
        bi_free(acc);
    }

	/* shifts */
	test("bit shifts");
    {
        bigint *one = bi_from_i64(1), *r;
        r = bi_shl(one, 64); expect_str(r, 10, "18446744073709551616", "1<<64"); bi_free(r);
        r = bi_shl(one, 100); expect_str(r, 16, "10000000000000000000000000", "1<<100 hex"); bi_free(r);
        bi_free(one);
        {
            bigint *big = bi_from_str("18446744073709551616", (int)strlen("18446744073709551616"), 0), *s;
            s = bi_shr(big, 1); expect_str(s, 10, "9223372036854775808", "(2^64)>>1"); bi_free(s);
            bi_free(big);
        }
        {   /* arithmetic right shift of a negative: -5 >> 1 == -3 (floor) */
            bigint *neg = bi_from_i64(-5), *s;
            s = bi_shr(neg, 1); expect_str(s, 10, "-3", "-5>>1"); bi_free(s);
            bi_free(neg);
        }
    }

	/* bitwise two's complement */
	test("bitwise operations");
    {
        bigint *a = bi_from_i64(0xF0), *b = bi_from_i64(0x0F), *r;
        r = bi_and(a, b); expect_str(r, 10, "0", "0xF0 & 0x0F"); bi_free(r);
        r = bi_or(a, b);  expect_str(r, 10, "255", "0xF0 | 0x0F"); bi_free(r);
        r = bi_xor(a, b); expect_str(r, 10, "255", "0xF0 ^ 0x0F"); bi_free(r);
        bi_free(a); bi_free(b);
        {   /* ~5 == -6 ; -1 & 5 == 5 ; -2 | 1 == -1 */
            bigint *x = bi_from_i64(5), *y = bi_from_i64(-1), *m2 = bi_from_i64(-2), *one = bi_from_i64(1), *t;
            t = bi_not(x); expect_str(t, 10, "-6", "~5"); bi_free(t);
            t = bi_and(y, x); expect_str(t, 10, "5", "-1 & 5"); bi_free(t);
            t = bi_or(m2, one); expect_str(t, 10, "-1", "-2 | 1"); bi_free(t);
            bi_free(x); bi_free(y); bi_free(m2); bi_free(one);
        }
    }

	/* asUintN / asIntN — snowflake-style wrapping */
	test("fixed-width wrapping");
    {
        bigint *v = bi_from_i64(-1), *r;
        r = bi_as_uintn(64, v); expect_str(r, 10, "18446744073709551615", "asUintN(64,-1)"); bi_free(r);
        r = bi_as_intn(64, v);  expect_str(r, 10, "-1", "asIntN(64,-1)"); bi_free(r);
        bi_free(v);
        {
            bigint *big = bi_from_str("18446744073709551618", (int)strlen("18446744073709551618"), 0), *q;   /* 2^64 + 2 */
            q = bi_as_uintn(64, big); expect_str(q, 10, "2", "asUintN(64, 2^64+2)"); bi_free(q);
            q = bi_as_intn(8, bi_from_i64(255)); expect_str(q, 10, "-1", "asIntN(8,255)"); bi_free(q);
            bi_free(big);
        }
    }

    /* string parsing: radices, sign, separators, whitespace, empty */
    {
        int ok;
        expect_str(bi_from_str("0xff", (int)strlen("0xff"), &ok), 10, "255", "0xff");
        expect_str(bi_from_str("0b1010", (int)strlen("0b1010"), &ok), 10, "10", "0b1010");
        expect_str(bi_from_str("0o17", (int)strlen("0o17"), &ok), 10, "15", "0o17");
        expect_str(bi_from_str("1_000_000", (int)strlen("1_000_000"), &ok), 10, "1000000", "1_000_000");
        expect_str(bi_from_str("  -42  ", (int)strlen("  -42  "), &ok), 10, "-42", "trim -42");
        expect_str(bi_from_str("", 0, &ok), 10, "0", "empty->0");
        expect(ok == 1);
        bi_from_str("12x", (int)strlen("12x"), &ok); expect(ok == 0);
        bi_from_str("0x", (int)strlen("0x"), &ok);  expect(ok == 0);
    }

    /* from_double */
    {
        int ok;
        expect_str(bi_from_double(9007199254740992.0, &ok), 10, "9007199254740992", "double 2^53");
        expect_str(bi_from_double(1e21, &ok), 10, "1000000000000000000000", "double 1e21");
        bi_from_double(1.5, &ok); expect(ok == 0);
    }

    /* divide by zero + cmp */
    {
        bigint *a = bi_from_i64(5), *z = bi_from_i64(0), *q, *r;
        expect(bi_divmod(a, z, &q, &r) == -1);
        expect(bi_cmp(bi_from_i64(-3), bi_from_i64(2)) == -1);
        expect(bi_cmp(bi_from_i64(100), bi_from_i64(100)) == 0);
        bi_free(a); bi_free(z);
    }

    return 0;
}
