/* ecdsa.c — ECDSA signature verification over NIST P-256 and P-384 (FIPS 186-4). */

#include "bn.h"
#include "pk.h"
#include <string.h>

#define EC_LIMBS 12 /* P-384 */

/* prime256v1 */
static const uint8_t P256_P[] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t P256_B[] = {
    0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
    0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b,
};
static const uint8_t P256_N[] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};
static const uint8_t P256_GX[] = {
    0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
    0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
};
static const uint8_t P256_GY[] = {
    0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
    0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5,
};
/* secp384r1 */
static const uint8_t P384_P[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t P384_B[] = {
    0xb3, 0x31, 0x2f, 0xa7, 0xe2, 0x3e, 0xe7, 0xe4, 0x98, 0x8e, 0x05, 0x6b, 0xe3, 0xf8, 0x2d, 0x19,
    0x18, 0x1d, 0x9c, 0x6e, 0xfe, 0x81, 0x41, 0x12, 0x03, 0x14, 0x08, 0x8f, 0x50, 0x13, 0x87, 0x5a,
    0xc6, 0x56, 0x39, 0x8d, 0x8a, 0x2e, 0xd1, 0x9d, 0x2a, 0x85, 0xc8, 0xed, 0xd3, 0xec, 0x2a, 0xef,
};
static const uint8_t P384_N[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc7, 0x63, 0x4d, 0x81, 0xf4, 0x37, 0x2d, 0xdf,
    0x58, 0x1a, 0x0d, 0xb2, 0x48, 0xb0, 0xa7, 0x7a, 0xec, 0xec, 0x19, 0x6a, 0xcc, 0xc5, 0x29, 0x73,
};
static const uint8_t P384_GX[] = {
    0xaa, 0x87, 0xca, 0x22, 0xbe, 0x8b, 0x05, 0x37, 0x8e, 0xb1, 0xc7, 0x1e, 0xf3, 0x20, 0xad, 0x74,
    0x6e, 0x1d, 0x3b, 0x62, 0x8b, 0xa7, 0x9b, 0x98, 0x59, 0xf7, 0x41, 0xe0, 0x82, 0x54, 0x2a, 0x38,
    0x55, 0x02, 0xf2, 0x5d, 0xbf, 0x55, 0x29, 0x6c, 0x3a, 0x54, 0x5e, 0x38, 0x72, 0x76, 0x0a, 0xb7,
};
static const uint8_t P384_GY[] = {
    0x36, 0x17, 0xde, 0x4a, 0x96, 0x26, 0x2c, 0x6f, 0x5d, 0x9e, 0x98, 0xbf, 0x92, 0x92, 0xdc, 0x29,
    0xf8, 0xf4, 0x1d, 0xbd, 0x28, 0x9a, 0x14, 0x7c, 0xe9, 0xda, 0x31, 0x13, 0xb5, 0xf0, 0xb8, 0xc0,
    0x0a, 0x60, 0xb1, 0xce, 0x1d, 0x7e, 0x81, 0x9d, 0x7a, 0x43, 0x1d, 0x7c, 0x90, 0xea, 0x0e, 0x5f,
};

struct curve {
  uint32_t size; /* bytes per coordinate */
  const uint8_t *p, *b, *n, *gx, *gy;
};

static const struct curve CURVES[] = {
    {32, P256_P, P256_B, P256_N, P256_GX, P256_GY},
    {48, P384_P, P384_B, P384_N, P384_GX, P384_GY},
};

/* Jacobian point in the Montgomery domain of the field; z == 0 is the point at infinity. */
struct pt {
  uint32_t x[EC_LIMBS], y[EC_LIMBS], z[EC_LIMBS];
};

static int is_inf(const struct bn_mont *f, const struct pt *p) { return bn_is_zero(f, p->z); }

static void set_inf(const struct bn_mont *f, struct pt *r) { memset(r->z, 0, f->len * 4); }

/* r = 2p for a = -3 (dbl-2001-b). r may alias p. */
static void pt_dbl(const struct bn_mont *f, struct pt *r, const struct pt *p)
{
  if (is_inf(f, p)) {
    *r = *p;
    return;
  }
  uint32_t delta[EC_LIMBS], gamma[EC_LIMBS], beta[EC_LIMBS], alpha[EC_LIMBS], t[EC_LIMBS], u[EC_LIMBS];
  uint32_t x3[EC_LIMBS], y3[EC_LIMBS], z3[EC_LIMBS];
  bn_mul(f, delta, p->z, p->z);
  bn_mul(f, gamma, p->y, p->y);
  bn_mul(f, beta, p->x, gamma);
  bn_sub(f, t, p->x, delta);
  bn_add(f, u, p->x, delta);
  bn_mul(f, alpha, t, u);
  bn_add(f, t, alpha, alpha);
  bn_add(f, alpha, t, alpha); /* 3(x - delta)(x + delta) */
  bn_add(f, t, beta, beta);
  bn_add(f, t, t, t); /* 4 beta */
  bn_add(f, u, t, t); /* 8 beta */
  bn_mul(f, x3, alpha, alpha);
  bn_sub(f, x3, x3, u);
  bn_add(f, z3, p->y, p->z);
  bn_mul(f, z3, z3, z3);
  bn_sub(f, z3, z3, gamma);
  bn_sub(f, z3, z3, delta);
  bn_sub(f, t, t, x3);
  bn_mul(f, y3, alpha, t);
  bn_mul(f, gamma, gamma, gamma);
  bn_add(f, gamma, gamma, gamma);
  bn_add(f, gamma, gamma, gamma);
  bn_add(f, gamma, gamma, gamma); /* 8 gamma^2 */
  bn_sub(f, y3, y3, gamma);
  memcpy(r->x, x3, f->len * 4);
  memcpy(r->y, y3, f->len * 4);
  memcpy(r->z, z3, f->len * 4);
}

/* r = p + q (add-2007-bl), falling back to doubling or infinity for equal or opposite points. */
static void pt_add(const struct bn_mont *f, struct pt *r, const struct pt *p, const struct pt *q)
{
  if (is_inf(f, p)) {
    *r = *q;
    return;
  }
  if (is_inf(f, q)) {
    *r = *p;
    return;
  }
  uint32_t z1z1[EC_LIMBS], z2z2[EC_LIMBS], u1[EC_LIMBS], u2[EC_LIMBS], s1[EC_LIMBS], s2[EC_LIMBS];
  uint32_t h[EC_LIMBS], i[EC_LIMBS], j[EC_LIMBS], rr[EC_LIMBS], v[EC_LIMBS], t[EC_LIMBS];
  uint32_t x3[EC_LIMBS], y3[EC_LIMBS], z3[EC_LIMBS];
  bn_mul(f, z1z1, p->z, p->z);
  bn_mul(f, z2z2, q->z, q->z);
  bn_mul(f, u1, p->x, z2z2);
  bn_mul(f, u2, q->x, z1z1);
  bn_mul(f, t, q->z, z2z2);
  bn_mul(f, s1, p->y, t);
  bn_mul(f, t, p->z, z1z1);
  bn_mul(f, s2, q->y, t);
  bn_sub(f, h, u2, u1);
  bn_sub(f, rr, s2, s1);
  if (bn_is_zero(f, h)) {
    if (bn_is_zero(f, rr))
      pt_dbl(f, r, p);
    else
      set_inf(f, r);
    return;
  }
  bn_add(f, rr, rr, rr);
  bn_add(f, i, h, h);
  bn_mul(f, i, i, i);
  bn_mul(f, j, h, i);
  bn_mul(f, v, u1, i);
  bn_mul(f, x3, rr, rr);
  bn_sub(f, x3, x3, j);
  bn_sub(f, x3, x3, v);
  bn_sub(f, x3, x3, v);
  bn_sub(f, t, v, x3);
  bn_mul(f, y3, rr, t);
  bn_mul(f, t, s1, j);
  bn_add(f, t, t, t);
  bn_sub(f, y3, y3, t);
  bn_add(f, z3, p->z, q->z);
  bn_mul(f, z3, z3, z3);
  bn_sub(f, z3, z3, z1z1);
  bn_sub(f, z3, z3, z2z2);
  bn_mul(f, z3, z3, h);
  memcpy(r->x, x3, f->len * 4);
  memcpy(r->y, y3, f->len * 4);
  memcpy(r->z, z3, f->len * 4);
}

static int load_coord(const struct bn_mont *f, uint32_t *out, const uint8_t *be, uint32_t len)
{
  if (bn_from_be(f, out, be, len) || bn_cmp(f, out, f->n) >= 0)
    return -1;
  bn_to_mont(f, out, out);
  return 0;
}

/* r, s: big-endian integers. digest is truncated to the curve size. 0 = valid. */
static int ecdsa_verify(const struct curve *c, const uint8_t *pub, uint32_t publen, const uint8_t *digest,
                        uint32_t dlen, const uint8_t *rb, uint32_t rlen, const uint8_t *sb, uint32_t slen)
{
  struct bn_mont f, o;
  struct pt g, q, gq, acc;
  uint32_t r[EC_LIMBS], s[EC_LIMBS], e[EC_LIMBS], w[EC_LIMBS], u1[EC_LIMBS], u2[EC_LIMBS], one[EC_LIMBS] = {1};
  uint32_t t[EC_LIMBS], rhs[EC_LIMBS], bm[EC_LIMBS];

  if (publen != 1 + 2 * c->size || pub[0] != 4 || bn_mont_init(&f, c->p, c->size) || bn_mont_init(&o, c->n, c->size))
    return -1;
  memset(&g, 0, sizeof g);
  bn_to_mont(&f, g.z, one);
  memcpy(q.z, g.z, sizeof g.z);
  if (load_coord(&f, g.x, c->gx, c->size) || load_coord(&f, g.y, c->gy, c->size) ||
      load_coord(&f, q.x, pub + 1, c->size) || load_coord(&f, q.y, pub + 1 + c->size, c->size))
    return -1;

  /* Q must satisfy y^2 = x^3 - 3x + b */
  bn_mul(&f, t, q.x, q.x);
  bn_mul(&f, t, t, q.x);
  bn_sub(&f, t, t, q.x);
  bn_sub(&f, t, t, q.x);
  bn_sub(&f, t, t, q.x);
  if (load_coord(&f, bm, c->b, c->size))
    return -1;
  bn_add(&f, rhs, t, bm);
  bn_mul(&f, t, q.y, q.y);
  if (bn_cmp(&f, t, rhs))
    return -1;

  if (bn_from_be(&o, r, rb, rlen) || bn_from_be(&o, s, sb, slen) || bn_is_zero(&o, r) || bn_is_zero(&o, s) ||
      bn_cmp(&o, r, o.n) >= 0 || bn_cmp(&o, s, o.n) >= 0)
    return -1;

  bn_from_be(&o, e, digest, dlen > c->size ? c->size : dlen);
  if (bn_cmp(&o, e, o.n) >= 0)
    bn_sub(&o, e, e, o.n);

  bn_to_mont(&o, s, s);
  bn_inv(&o, w, s);
  bn_to_mont(&o, e, e);
  bn_mul(&o, u1, e, w);
  bn_from_mont(&o, u1, u1);
  bn_to_mont(&o, t, r);
  bn_mul(&o, u2, t, w);
  bn_from_mont(&o, u2, u2);

  /* Shamir's trick: acc = u1*G + u2*Q */
  pt_add(&f, &gq, &g, &q);
  set_inf(&f, &acc);
  for (uint32_t i = o.len * 32; i-- > 0;) {
    pt_dbl(&f, &acc, &acc);
    unsigned sel = (u1[i / 32] >> (i % 32) & 1) | (u2[i / 32] >> (i % 32) & 1) << 1;
    if (sel)
      pt_add(&f, &acc, &acc, sel == 1 ? &g : sel == 2 ? &q : &gq);
  }
  if (is_inf(&f, &acc))
    return -1;

  uint32_t zi[EC_LIMBS], x[EC_LIMBS];
  bn_inv(&f, zi, acc.z);
  bn_mul(&f, zi, zi, zi);
  bn_mul(&f, x, acc.x, zi);
  bn_from_mont(&f, x, x);
  if (bn_cmp(&o, x, o.n) >= 0)
    bn_sub(&o, x, x, o.n);
  return bn_cmp(&o, x, r) ? -1 : 0;
}

/* Read one short-form DER INTEGER, leading zeros stripped. */
static int der_int(const uint8_t **p, const uint8_t *end, const uint8_t **v, uint32_t *vl)
{
  if (end - *p < 2 || (*p)[0] != 0x02 || (*p)[1] >= 0x80 || (*p)[1] == 0 || (*p)[1] > end - *p - 2 || ((*p)[2] & 0x80))
    return -1;
  *vl = (*p)[1];
  *v = *p + 2;
  *p += 2 + *vl;
  while (*vl > 1 && !**v) {
    (*v)++;
    (*vl)--;
  }
  return 0;
}

int ecdsa_verify_der(int curve, const uint8_t *pub, uint32_t publen, const uint8_t *digest, uint32_t dlen,
                     const uint8_t *sig, uint32_t siglen)
{
  if (curve < 0 || curve > 1 || siglen < 8 || sig[0] != 0x30)
    return -1;
  const uint8_t *p = sig + 2, *end = sig + siglen;
  if (sig[1] != siglen - 2) /* both integers fit in short-form lengths */
    return -1;
  const uint8_t *r, *s;
  uint32_t rl, sl;
  if (der_int(&p, end, &r, &rl) || der_int(&p, end, &s, &sl) || p != end)
    return -1;
  return ecdsa_verify(&CURVES[curve], pub, publen, digest, dlen, r, rl, s, sl);
}
