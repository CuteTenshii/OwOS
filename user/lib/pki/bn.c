/* bn.c — see bn.h. */

#include "bn.h"
#include <string.h>

static uint32_t add_n(uint32_t *r, const uint32_t *a, const uint32_t *b, uint32_t len)
{
  uint64_t c = 0;
  for (uint32_t i = 0; i < len; i++) {
    c += (uint64_t)a[i] + b[i];
    r[i] = (uint32_t)c;
    c >>= 32;
  }
  return (uint32_t)c;
}

static uint32_t sub_n(uint32_t *r, const uint32_t *a, const uint32_t *b, uint32_t len)
{
  uint32_t borrow = 0;
  for (uint32_t i = 0; i < len; i++) {
    uint64_t d = (uint64_t)a[i] - b[i] - borrow;
    r[i] = (uint32_t)d;
    borrow = (uint32_t)(d >> 32) & 1;
  }
  return borrow;
}

static int cmp_n(const uint32_t *a, const uint32_t *b, uint32_t len)
{
  for (uint32_t i = len; i-- > 0;)
    if (a[i] != b[i])
      return a[i] > b[i] ? 1 : -1;
  return 0;
}

int bn_cmp(const struct bn_mont *m, const uint32_t *a, const uint32_t *b) { return cmp_n(a, b, m->len); }

int bn_is_zero(const struct bn_mont *m, const uint32_t *a)
{
  for (uint32_t i = 0; i < m->len; i++)
    if (a[i])
      return 0;
  return 1;
}

int bn_mont_init(struct bn_mont *m, const uint8_t *mod, uint32_t len)
{
  while (len && !*mod) {
    mod++;
    len--;
  }
  if (len < 1 || len > BN_MAX_LIMBS * 4 || !(mod[len - 1] & 1) || (len == 1 && mod[0] < 3))
    return -1;
  m->len = (len + 3) / 4;
  memset(m->n, 0, sizeof m->n);
  for (uint32_t i = 0; i < len; i++)
    m->n[i / 4] |= (uint32_t)mod[len - 1 - i] << (8 * (i % 4));

  uint32_t x = m->n[0]; /* Newton iteration for n^-1 mod 2^32 */
  for (int i = 0; i < 5; i++)
    x *= 2 - m->n[0] * x;
  m->n0 = 0 - x;

  /* R^2 mod n by 64*len doublings of 1 */
  uint32_t r[BN_MAX_LIMBS] = {1};
  for (uint32_t i = 0; i < 64 * m->len; i++) {
    uint32_t carry = 0;
    for (uint32_t j = 0; j < m->len; j++) {
      uint32_t next = r[j] >> 31;
      r[j] = (r[j] << 1) | carry;
      carry = next;
    }
    if (carry || cmp_n(r, m->n, m->len) >= 0)
      sub_n(r, r, m->n, m->len);
  }
  memcpy(m->rr, r, sizeof m->rr);
  return 0;
}

int bn_from_be(const struct bn_mont *m, uint32_t *x, const uint8_t *be, uint32_t len)
{
  while (len && !*be) {
    be++;
    len--;
  }
  if (len > m->len * 4)
    return -1;
  memset(x, 0, m->len * 4);
  for (uint32_t i = 0; i < len; i++)
    x[i / 4] |= (uint32_t)be[len - 1 - i] << (8 * (i % 4));
  return 0;
}

int bn_to_be(const struct bn_mont *m, uint8_t *out, uint32_t len, const uint32_t *x)
{
  for (uint32_t i = len; i < m->len * 4; i++)
    if (x[i / 4] >> (8 * (i % 4)) & 0xff)
      return -1;
  for (uint32_t i = 0; i < len; i++)
    out[len - 1 - i] = i < m->len * 4 ? (uint8_t)(x[i / 4] >> (8 * (i % 4))) : 0;
  return 0;
}

void bn_mul(const struct bn_mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
  uint32_t s = m->len, t[BN_MAX_LIMBS + 2] = {0};
  for (uint32_t i = 0; i < s; i++) {
    uint64_t c = 0, v;
    for (uint32_t j = 0; j < s; j++) {
      v = (uint64_t)a[j] * b[i] + t[j] + c;
      t[j] = (uint32_t)v;
      c = v >> 32;
    }
    v = (uint64_t)t[s] + c;
    t[s] = (uint32_t)v;
    t[s + 1] = (uint32_t)(v >> 32);

    uint32_t q = t[0] * m->n0;
    c = ((uint64_t)q * m->n[0] + t[0]) >> 32;
    for (uint32_t j = 1; j < s; j++) {
      v = (uint64_t)q * m->n[j] + t[j] + c;
      t[j - 1] = (uint32_t)v;
      c = v >> 32;
    }
    v = (uint64_t)t[s] + c;
    t[s - 1] = (uint32_t)v;
    t[s] = t[s + 1] + (uint32_t)(v >> 32);
  }
  if (t[s] || cmp_n(t, m->n, s) >= 0)
    sub_n(t, t, m->n, s);
  memcpy(r, t, s * 4);
}

void bn_add(const struct bn_mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
  uint32_t carry = add_n(r, a, b, m->len);
  if (carry || cmp_n(r, m->n, m->len) >= 0)
    sub_n(r, r, m->n, m->len);
}

void bn_sub(const struct bn_mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
  if (sub_n(r, a, b, m->len))
    add_n(r, r, m->n, m->len);
}

void bn_to_mont(const struct bn_mont *m, uint32_t *r, const uint32_t *x) { bn_mul(m, r, x, m->rr); }

void bn_from_mont(const struct bn_mont *m, uint32_t *r, const uint32_t *x)
{
  uint32_t one[BN_MAX_LIMBS] = {1};
  bn_mul(m, r, x, one);
}

void bn_pow(const struct bn_mont *m, uint32_t *r, const uint32_t *a, const uint32_t *e, uint32_t elen)
{
  uint32_t one[BN_MAX_LIMBS] = {1}, acc[BN_MAX_LIMBS], base[BN_MAX_LIMBS];
  memcpy(base, a, m->len * 4);
  bn_mul(m, acc, one, m->rr); /* 1 in the Montgomery domain */
  for (uint32_t i = elen * 32; i-- > 0;) {
    bn_mul(m, acc, acc, acc);
    if (e[i / 32] >> (i % 32) & 1)
      bn_mul(m, acc, acc, base);
  }
  memcpy(r, acc, m->len * 4);
}

void bn_inv(const struct bn_mont *m, uint32_t *r, const uint32_t *a)
{
  uint32_t e[BN_MAX_LIMBS], two[BN_MAX_LIMBS] = {2};
  sub_n(e, m->n, two, m->len); /* n - 2 */
  bn_pow(m, r, a, e, m->len);
}
