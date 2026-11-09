/* bt_p256.c: NIST P-256 public key and ECDH for LE Secure Connections, for controllers that cannot do it themselves.
 * Montgomery field arithmetic on 8x32-bit limbs; scalar multiplication is a double-and-always-add ladder with
 * constant-time selection (only the position of the scalar's top bit is visible). */

#include "bt_int.h"

typedef uint32_t fe[8];

struct jac {
  fe x, y, z;
};

static const uint8_t be_p[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
                                 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t be_n[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
                                 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17,
                                 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51};
static const uint8_t be_b[32] = {0x5A, 0xC6, 0x35, 0xD8, 0xAA, 0x3A, 0x93, 0xE7, 0xB3, 0xEB, 0xBD,
                                 0x55, 0x76, 0x98, 0x86, 0xBC, 0x65, 0x1D, 0x06, 0xB0, 0xCC, 0x53,
                                 0xB0, 0xF6, 0x3B, 0xCE, 0x3C, 0x3E, 0x27, 0xD2, 0x60, 0x4B};
static const uint8_t be_gx[32] = {0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47, 0xF8, 0xBC, 0xE6,
                                  0xE5, 0x63, 0xA4, 0x40, 0xF2, 0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB,
                                  0x33, 0xA0, 0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96};
static const uint8_t be_gy[32] = {0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B, 0x8E, 0xE7, 0xEB,
                                  0x4A, 0x7C, 0x0F, 0x9E, 0x16, 0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31,
                                  0x5E, 0xCE, 0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5};

static fe P, one, r2, curve_b, gx, gy;
static bool ready;

static void load(fe r, const uint8_t *b)
{
  for (int i = 0; i < 8; i++)
    r[i] = (uint32_t)b[28 - 4 * i] << 24 | (uint32_t)b[29 - 4 * i] << 16 | (uint32_t)b[30 - 4 * i] << 8 | b[31 - 4 * i];
}

static void store(uint8_t *b, const fe a)
{
  for (int i = 0; i < 8; i++) {
    b[28 - 4 * i] = (uint8_t)(a[i] >> 24);
    b[29 - 4 * i] = (uint8_t)(a[i] >> 16);
    b[30 - 4 * i] = (uint8_t)(a[i] >> 8);
    b[31 - 4 * i] = (uint8_t)a[i];
  }
}

static uint32_t add_raw(fe r, const fe a, const fe b)
{
  uint64_t c = 0;
  for (int i = 0; i < 8; i++) {
    c += (uint64_t)a[i] + b[i];
    r[i] = (uint32_t)c;
    c >>= 32;
  }
  return (uint32_t)c;
}

static uint32_t sub_raw(fe r, const fe a, const fe b)
{
  uint64_t br = 0;
  for (int i = 0; i < 8; i++) {
    uint64_t d = (uint64_t)a[i] - b[i] - br;
    r[i] = (uint32_t)d;
    br = d >> 63;
  }
  return (uint32_t)br;
}

static void fe_select(fe r, uint32_t take_b, const fe a, const fe b)
{
  uint32_t m = 0 - take_b;
  for (int i = 0; i < 8; i++)
    r[i] = (a[i] & ~m) | (b[i] & m);
}

static void fe_add(fe r, const fe a, const fe b)
{
  fe t;
  uint32_t c = add_raw(r, a, b);
  uint32_t br = sub_raw(t, r, P);
  fe_select(r, c | (br ^ 1), r, t);
}

static void fe_sub(fe r, const fe a, const fe b)
{
  fe t;
  uint32_t br = sub_raw(r, a, b);
  add_raw(t, r, P);
  fe_select(r, br, r, t);
}

/* Montgomery product; -P^-1 mod 2^32 is 1 for this prime. */
static void fe_mul(fe r, const fe a, const fe b)
{
  uint32_t t[10] = {0};
  for (int i = 0; i < 8; i++) {
    uint64_t c = 0, s;
    for (int j = 0; j < 8; j++) {
      s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
      t[j] = (uint32_t)s;
      c = s >> 32;
    }
    s = (uint64_t)t[8] + c;
    t[8] = (uint32_t)s;
    t[9] = (uint32_t)(s >> 32);
    uint32_t m = t[0];
    c = ((uint64_t)t[0] + (uint64_t)m * P[0]) >> 32;
    for (int j = 1; j < 8; j++) {
      s = (uint64_t)t[j] + (uint64_t)m * P[j] + c;
      t[j - 1] = (uint32_t)s;
      c = s >> 32;
    }
    s = (uint64_t)t[8] + c;
    t[7] = (uint32_t)s;
    t[8] = t[9] + (uint32_t)(s >> 32);
  }
  fe d;
  uint32_t br = sub_raw(d, t, P);
  fe_select(r, t[8] | (br ^ 1), t, d);
}

static void fe_sqr(fe r, const fe a) { fe_mul(r, a, a); }

static void fe_inv(fe r, const fe a)
{
  fe x;
  uint8_t pm2[32];
  memcpy(pm2, be_p, 32);
  pm2[31] -= 2;
  memcpy(x, one, sizeof x);
  for (int i = 255; i >= 0; i--) {
    fe_sqr(x, x);
    if (pm2[31 - i / 8] >> (i % 8) & 1)
      fe_mul(x, x, a);
  }
  memcpy(r, x, sizeof x);
}

static void to_mont(fe r, const fe a) { fe_mul(r, a, r2); }

static void from_mont(fe r, const fe a)
{
  static const fe plain_one = {1};
  fe_mul(r, a, plain_one);
}

static void init(void)
{
  static const fe zero;
  load(P, be_p);
  sub_raw(one, zero, P); /* 2^256 mod P */
  memcpy(r2, one, sizeof r2);
  for (int i = 0; i < 256; i++)
    fe_add(r2, r2, r2);
  fe t;
  load(t, be_b);
  to_mont(curve_b, t);
  load(t, be_gx);
  to_mont(gx, t);
  load(t, be_gy);
  to_mont(gy, t);
  ready = true;
}

/* 2*A on y^2 = x^3 - 3x + b. */
static void point_dbl(struct jac *r, const struct jac *a)
{
  fe delta, gamma, beta, alpha, t1, t2, x3, y3, z3;
  fe_sqr(delta, a->z);
  fe_sqr(gamma, a->y);
  fe_mul(beta, a->x, gamma);
  fe_sub(t1, a->x, delta);
  fe_add(t2, a->x, delta);
  fe_mul(alpha, t1, t2);
  fe_add(t1, alpha, alpha);
  fe_add(alpha, t1, alpha);
  fe_sqr(x3, alpha);
  fe_add(t1, beta, beta);
  fe_add(t1, t1, t1);
  fe_add(t2, t1, t1);
  fe_sub(x3, x3, t2);
  fe_add(z3, a->y, a->z);
  fe_sqr(z3, z3);
  fe_sub(z3, z3, gamma);
  fe_sub(z3, z3, delta);
  fe_sub(t1, t1, x3);
  fe_mul(y3, alpha, t1);
  fe_sqr(t2, gamma);
  fe_add(t2, t2, t2);
  fe_add(t2, t2, t2);
  fe_add(t2, t2, t2);
  fe_sub(y3, y3, t2);
  memcpy(r->x, x3, sizeof x3);
  memcpy(r->y, y3, sizeof y3);
  memcpy(r->z, z3, sizeof z3);
}

/* A + (qx, qy) with the second point affine; A must not equal +-Q or be infinity. */
static void point_add(struct jac *r, const struct jac *a, const fe qx, const fe qy)
{
  fe z1z1, u2, s2, h, hh, i, j, rr, v, t, x3, y3, z3;
  fe_sqr(z1z1, a->z);
  fe_mul(u2, qx, z1z1);
  fe_mul(s2, qy, a->z);
  fe_mul(s2, s2, z1z1);
  fe_sub(h, u2, a->x);
  fe_sqr(hh, h);
  fe_add(i, hh, hh);
  fe_add(i, i, i);
  fe_mul(j, h, i);
  fe_sub(rr, s2, a->y);
  fe_add(rr, rr, rr);
  fe_mul(v, a->x, i);
  fe_sqr(x3, rr);
  fe_sub(x3, x3, j);
  fe_sub(x3, x3, v);
  fe_sub(x3, x3, v);
  fe_sub(t, v, x3);
  fe_mul(y3, rr, t);
  fe_mul(t, a->y, j);
  fe_add(t, t, t);
  fe_sub(y3, y3, t);
  fe_add(z3, a->z, h);
  fe_sqr(z3, z3);
  fe_sub(z3, z3, z1z1);
  fe_sub(z3, z3, hh);
  memcpy(r->x, x3, sizeof x3);
  memcpy(r->y, y3, sizeof y3);
  memcpy(r->z, z3, sizeof z3);
}

static bool scalar_ok(const uint8_t k[32])
{
  uint8_t any = 0;
  int cmp = 0;
  for (int i = 0; i < 32; i++) {
    any |= k[i];
    if (!cmp && k[i] != be_n[i])
      cmp = k[i] < be_n[i] ? -1 : 1;
  }
  return any && cmp < 0;
}

/* k * (qx, qy), affine in and out (Montgomery form); false for the point at infinity. */
static bool scalar_mul(fe ox, fe oy, const uint8_t k[32], const fe qx, const fe qy)
{
  struct jac r, t;
  int top = 255;
  while (top > 0 && !(k[31 - top / 8] >> (top % 8) & 1))
    top--;
  memcpy(r.x, qx, sizeof r.x);
  memcpy(r.y, qy, sizeof r.y);
  memcpy(r.z, one, sizeof r.z);
  for (int i = top - 1; i >= 0; i--) {
    point_dbl(&r, &r);
    point_add(&t, &r, qx, qy);
    uint32_t bit = k[31 - i / 8] >> (i % 8) & 1;
    fe_select(r.x, bit, r.x, t.x);
    fe_select(r.y, bit, r.y, t.y);
    fe_select(r.z, bit, r.z, t.z);
  }
  uint32_t zero = 0;
  for (int i = 0; i < 8; i++)
    zero |= r.z[i];
  if (!zero)
    return false;
  fe zi, zi2, zi3;
  fe_inv(zi, r.z);
  fe_sqr(zi2, zi);
  fe_mul(zi3, zi2, zi);
  fe_mul(ox, r.x, zi2);
  fe_mul(oy, r.y, zi3);
  return true;
}

static bool on_curve(const fe x, const fe y)
{
  fe l, r, t;
  fe_sqr(l, y);
  fe_sqr(r, x);
  fe_mul(r, r, x);
  fe_add(t, x, x);
  fe_add(t, t, x);
  fe_sub(r, r, t);
  fe_add(r, r, curve_b);
  return !memcmp(l, r, sizeof l);
}

bool p256_pub(const uint8_t priv[32], uint8_t pub[64])
{
  fe x, y;
  if (!ready)
    init();
  if (!scalar_ok(priv) || !scalar_mul(x, y, priv, gx, gy))
    return false;
  from_mont(x, x);
  from_mont(y, y);
  store(pub, x);
  store(pub + 32, y);
  return true;
}

bool p256_ecdh(const uint8_t priv[32], const uint8_t pub[64], uint8_t out[32])
{
  fe qx, qy, x, y;
  if (!ready)
    init();
  load(qx, pub);
  load(qy, pub + 32);
  fe t;
  if (sub_raw(t, qx, P) != 1 || sub_raw(t, qy, P) != 1)
    return false;
  to_mont(qx, qx);
  to_mont(qy, qy);
  if (!scalar_ok(priv) || !on_curve(qx, qy) || !scalar_mul(x, y, priv, qx, qy))
    return false;
  from_mont(x, x);
  store(out, x);
  return true;
}
