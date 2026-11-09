/* jpeg.c, baseline and progressive JPEG decoder. All scans are decoded into coefficient planes, then each
 * block is inverse-transformed (integer IDCT) and box-averaged into a possibly downscaled sample plane. */
#include "jpeg.h"
#include "owos.h"

#define MAX_COMP 3
#define MAX_COEFS (12u << 20)

typedef unsigned char u8;
typedef unsigned int u32;

static const u8 zz[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                          41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                          30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct huff {
  unsigned short lut[256]; /* (len << 8) | symbol for codes of up to 8 bits */
  int maxcode[18];
  int delta[17];
  u8 vals[256];
  int set;
};

struct comp {
  int id, h, v, tq;
  int bw, bh; /* padded size in blocks */
  int cw, ch; /* real size in blocks, used by non-interleaved scans */
  int dc_pred;
  int td, ta;
  short *coef;
  u8 *plane;
  int pw, ph;
};

struct br {
  const u8 *p, *end;
  u32 buf;
  int nb;
  int marker;
};

struct jpeg {
  const u8 *buf;
  int len;
  int W, H, ncomp, hmax, vmax, mcux, mcuy;
  int progressive, ri, adobe_transform;
  int qt[4][64];
  struct huff dc[4], ac[4];
  struct comp c[MAX_COMP];
  int eobrun, err;
  struct br br;
};

static void br_fill(struct br *b)
{
  while (b->nb <= 24) {
    u32 v = 0;
    if (!b->marker && b->p < b->end) {
      v = *b->p;
      if (v == 0xFF) {
        if (b->p + 1 < b->end && b->p[1] == 0)
          b->p += 2;
        else
          b->marker = 1, v = 0;
      } else {
        b->p++;
      }
    } else {
      b->marker = 1;
    }
    b->buf |= v << (24 - b->nb);
    b->nb += 8;
  }
}

static int getbits(struct br *b, int n)
{
  if (!n)
    return 0;
  if (b->nb < n)
    br_fill(b);
  u32 v = b->buf >> (32 - n);
  b->buf <<= n;
  b->nb -= n;
  return (int)v;
}

static int extend(int v, int n) { return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

static int huff_decode(struct br *b, const struct huff *h)
{
  if (b->nb < 16)
    br_fill(b);
  unsigned e = h->lut[b->buf >> 24];
  if (e) {
    b->buf <<= e >> 8;
    b->nb -= e >> 8;
    return e & 255;
  }
  int code = 0;
  for (int l = 1; l <= 16; l++) {
    code = code << 1 | (int)((b->buf >> (32 - l)) & 1);
    if (code <= h->maxcode[l]) {
      b->buf <<= l;
      b->nb -= l;
      return h->vals[(code + h->delta[l]) & 255];
    }
  }
  return -1;
}

static void huff_build(struct huff *h, const u8 *counts, const u8 *vals, int nvals)
{
  int code = 0, k = 0;
  memset(h->lut, 0, sizeof h->lut);
  memcpy(h->vals, vals, (unsigned)nvals);
  for (int l = 1; l <= 16; l++) {
    int n = counts[l - 1];
    h->delta[l] = k - code;
    for (int i = 0; i < n && l <= 8 && k + i < nvals; i++) {
      int c = (code + i) << (8 - l);
      for (int j = 0; j < (1 << (8 - l)) && c + j < 256; j++)
        h->lut[c + j] = (unsigned short)(l << 8 | vals[k + i]);
    }
    code += n;
    k += n;
    h->maxcode[l] = n ? code - 1 : -1;
    code <<= 1;
  }
  h->set = 1;
}

static void restart(struct jpeg *j)
{
  struct br *b = &j->br;
  b->buf = 0;
  b->nb = 0;
  while (b->p + 1 < b->end && !(b->p[0] == 0xFF && b->p[1] >= 0xD0 && b->p[1] <= 0xD7))
    b->p++;
  if (b->p + 1 < b->end)
    b->p += 2;
  b->marker = 0;
  for (int i = 0; i < j->ncomp; i++)
    j->c[i].dc_pred = 0;
  j->eobrun = 0;
}

static void block_baseline(struct jpeg *j, struct comp *c, short *blk)
{
  struct br *b = &j->br;
  int t = huff_decode(b, &j->dc[c->td]);
  if (t < 0 || t > 15) {
    j->err = 1;
    return;
  }
  c->dc_pred += t ? extend(getbits(b, t), t) : 0;
  blk[0] = (short)c->dc_pred;
  for (int k = 1; k < 64;) {
    int rs = huff_decode(b, &j->ac[c->ta]);
    if (rs < 0) {
      j->err = 1;
      return;
    }
    int r = rs >> 4, s = rs & 15;
    if (!s) {
      if (r != 15)
        break;
      k += 16;
      continue;
    }
    k += r;
    if (k > 63) {
      j->err = 1;
      return;
    }
    blk[zz[k++]] = (short)extend(getbits(b, s), s);
  }
}

static void block_dc_first(struct jpeg *j, struct comp *c, short *blk, int al)
{
  int t = huff_decode(&j->br, &j->dc[c->td]);
  if (t < 0 || t > 15) {
    j->err = 1;
    return;
  }
  c->dc_pred += t ? extend(getbits(&j->br, t), t) : 0;
  blk[0] = (short)(c->dc_pred * (1 << al));
}

static void block_ac_first(struct jpeg *j, struct comp *c, short *blk, int ss, int se, int al)
{
  struct br *b = &j->br;
  if (j->eobrun > 0) {
    j->eobrun--;
    return;
  }
  for (int k = ss; k <= se;) {
    int rs = huff_decode(b, &j->ac[c->ta]);
    if (rs < 0) {
      j->err = 1;
      return;
    }
    int r = rs >> 4, s = rs & 15;
    if (!s) {
      if (r < 15) {
        j->eobrun = (1 << r) - 1;
        if (r)
          j->eobrun += getbits(b, r);
        return;
      }
      k += 16;
      continue;
    }
    k += r;
    if (k > 63) {
      j->err = 1;
      return;
    }
    blk[zz[k++]] = (short)(extend(getbits(b, s), s) * (1 << al));
  }
}

static void refine_nonzero(struct br *b, short *c, int p1)
{
  if (getbits(b, 1) && !(*c & p1))
    *c += *c >= 0 ? p1 : -p1;
}

static void block_ac_refine(struct jpeg *j, struct comp *c, short *blk, int ss, int se, int al)
{
  struct br *b = &j->br;
  int p1 = 1 << al, k = ss;
  if (j->eobrun == 0) {
    for (; k <= se; k++) {
      int rs = huff_decode(b, &j->ac[c->ta]);
      if (rs < 0) {
        j->err = 1;
        return;
      }
      int r = rs >> 4, s = rs & 15;
      if (s) {
        s = getbits(b, 1) ? p1 : -p1;
      } else if (r != 15) {
        j->eobrun = 1 << r;
        if (r)
          j->eobrun += getbits(b, r);
        break;
      }
      do {
        short *p = &blk[zz[k]];
        if (*p)
          refine_nonzero(b, p, p1);
        else if (--r < 0)
          break;
        k++;
      } while (k <= se);
      if (s && k <= se)
        blk[zz[k]] = (short)s;
    }
  }
  if (j->eobrun > 0) {
    for (; k <= se; k++)
      if (blk[zz[k]])
        refine_nonzero(b, &blk[zz[k]], p1);
    j->eobrun--;
  }
}

static int decode_scan(struct jpeg *j, const u8 *p, int ns, struct comp **sc, int ss, int se, int ah, int al)
{
  struct br *b = &j->br;
  memset(b, 0, sizeof *b);
  b->p = p;
  b->end = j->buf + j->len;
  for (int i = 0; i < j->ncomp; i++)
    j->c[i].dc_pred = 0;
  j->eobrun = 0;
  j->err = 0;

  int cols, rows;
  if (ns == 1) {
    cols = sc[0]->cw;
    rows = sc[0]->ch;
  } else {
    cols = j->mcux;
    rows = j->mcuy;
  }
  int count = 0;
  for (int my = 0; my < rows; my++) {
    for (int mx = 0; mx < cols; mx++) {
      if (j->ri && count && count % j->ri == 0)
        restart(j);
      count++;
      for (int i = 0; i < ns; i++) {
        struct comp *c = sc[i];
        int bh = ns == 1 ? 1 : c->v, bwid = ns == 1 ? 1 : c->h;
        for (int y = 0; y < bh; y++)
          for (int x = 0; x < bwid; x++) {
            int bx = ns == 1 ? mx : mx * c->h + x, by = ns == 1 ? my : my * c->v + y;
            short *blk = c->coef + ((unsigned)by * c->bw + bx) * 64;
            if (!j->progressive)
              block_baseline(j, c, blk);
            else if (ss == 0 && ah == 0)
              block_dc_first(j, c, blk, al);
            else if (ss == 0) {
              if (getbits(b, 1))
                blk[0] |= (short)(1 << al);
            } else if (ah == 0)
              block_ac_first(j, c, blk, ss, se, al);
            else
              block_ac_refine(j, c, blk, ss, se, al);
            if (j->err)
              return -1;
          }
      }
    }
  }
  return 0;
}

#define F2F(x) ((int)((x) * 4096 + 0.5))
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                                                        \
  int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                                                              \
  p2 = s2;                                                                                                             \
  p3 = s6;                                                                                                             \
  p1 = (p2 + p3) * F2F(0.5411961);                                                                                     \
  t2 = p1 + p3 * F2F(-1.847759065);                                                                                    \
  t3 = p1 + p2 * F2F(0.765366865);                                                                                     \
  p2 = s0;                                                                                                             \
  p3 = s4;                                                                                                             \
  t0 = (p2 + p3) * 4096;                                                                                               \
  t1 = (p2 - p3) * 4096;                                                                                               \
  x0 = t0 + t3;                                                                                                        \
  x3 = t0 - t3;                                                                                                        \
  x1 = t1 + t2;                                                                                                        \
  x2 = t1 - t2;                                                                                                        \
  t0 = s7;                                                                                                             \
  t1 = s5;                                                                                                             \
  t2 = s3;                                                                                                             \
  t3 = s1;                                                                                                             \
  p3 = t0 + t2;                                                                                                        \
  p4 = t1 + t3;                                                                                                        \
  p1 = t0 + t3;                                                                                                        \
  p2 = t1 + t2;                                                                                                        \
  p5 = (p3 + p4) * F2F(1.175875602);                                                                                   \
  t0 = t0 * F2F(0.298631336);                                                                                          \
  t1 = t1 * F2F(2.053119869);                                                                                          \
  t2 = t2 * F2F(3.072711026);                                                                                          \
  t3 = t3 * F2F(1.501321110);                                                                                          \
  p1 = p5 + p1 * F2F(-0.899976223);                                                                                    \
  p2 = p5 + p2 * F2F(-2.562915447);                                                                                    \
  p3 = p3 * F2F(-1.961570560);                                                                                         \
  p4 = p4 * F2F(-0.390180644);                                                                                         \
  t3 += p1 + p4;                                                                                                       \
  t2 += p2 + p3;                                                                                                       \
  t1 += p2 + p4;                                                                                                       \
  t0 += p1 + p3;

/* keeps corrupt coefficients from overflowing the fixed-point transform */
static int limit(int v, int lim) { return v < -lim ? -lim : v > lim ? lim : v; }

static u8 clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }

/* Dequantize and inverse-transform one block into 8x8 samples */
static void idct_block(const short *in, const int *q, u8 out[64])
{
  int val[64], d[64];
  for (int i = 0; i < 64; i++)
    d[i] = limit(in[i] * q[i], 8192);
  for (int i = 0; i < 8; i++) {
    int *c = d + i, *v = val + i;
    IDCT_1D(c[0], c[8], c[16], c[24], c[32], c[40], c[48], c[56])
    x0 += 512, x1 += 512, x2 += 512, x3 += 512;
    v[0] = limit((x0 + t3) >> 10, 16384);
    v[56] = limit((x0 - t3) >> 10, 16384);
    v[8] = limit((x1 + t2) >> 10, 16384);
    v[48] = limit((x1 - t2) >> 10, 16384);
    v[16] = limit((x2 + t1) >> 10, 16384);
    v[40] = limit((x2 - t1) >> 10, 16384);
    v[24] = limit((x3 + t0) >> 10, 16384);
    v[32] = limit((x3 - t0) >> 10, 16384);
  }
  for (int i = 0; i < 8; i++) {
    int *v = val + i * 8;
    u8 *o = out + i * 8;
    IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
    x0 += 65536 + (128 << 17), x1 += 65536 + (128 << 17), x2 += 65536 + (128 << 17), x3 += 65536 + (128 << 17);
    o[0] = clamp8((x0 + t3) >> 17);
    o[7] = clamp8((x0 - t3) >> 17);
    o[1] = clamp8((x1 + t2) >> 17);
    o[6] = clamp8((x1 - t2) >> 17);
    o[2] = clamp8((x2 + t1) >> 17);
    o[5] = clamp8((x2 - t1) >> 17);
    o[3] = clamp8((x3 + t0) >> 17);
    o[4] = clamp8((x3 - t0) >> 17);
  }
}

static void build_plane(const struct jpeg *j, struct comp *c, int s)
{
  int bs = 8 / s;
  u8 px[64];
  for (int by = 0; by < c->bh; by++)
    for (int bx = 0; bx < c->bw; bx++) {
      idct_block(c->coef + ((unsigned)by * c->bw + bx) * 64, j->qt[c->tq], px);
      for (int y = 0; y < bs; y++)
        for (int x = 0; x < bs; x++) {
          int sum = 0;
          for (int yy = 0; yy < s; yy++)
            for (int xx = 0; xx < s; xx++)
              sum += px[(y * s + yy) * 8 + x * s + xx];
          c->plane[(unsigned)(by * bs + y) * c->pw + bx * bs + x] = (u8)((sum + s * s / 2) / (s * s));
        }
    }
}

static int be16(const u8 *p) { return p[0] << 8 | p[1]; }

static int parse_sof(struct jpeg *j, const u8 *p, int n)
{
  if (n < 6 || p[0] != 8)
    return JPEG_EUNSUP;
  j->H = be16(p + 1);
  j->W = be16(p + 3);
  j->ncomp = p[5];
  if (j->W < 1 || j->H < 1 || n < 6 + 3 * j->ncomp)
    return JPEG_EFORMAT;
  if (j->ncomp != 1 && j->ncomp != 3)
    return JPEG_EUNSUP;
  j->hmax = j->vmax = 1;
  for (int i = 0; i < j->ncomp; i++) {
    struct comp *c = &j->c[i];
    c->id = p[6 + 3 * i];
    c->h = p[7 + 3 * i] >> 4;
    c->v = p[7 + 3 * i] & 15;
    c->tq = p[8 + 3 * i];
    if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3)
      return JPEG_EFORMAT;
    if (j->ncomp == 1)
      c->h = c->v = 1;
    if (c->h > j->hmax)
      j->hmax = c->h;
    if (c->v > j->vmax)
      j->vmax = c->v;
  }
  j->mcux = (j->W + 8 * j->hmax - 1) / (8 * j->hmax);
  j->mcuy = (j->H + 8 * j->vmax - 1) / (8 * j->vmax);
  unsigned long long total = 0;
  for (int i = 0; i < j->ncomp; i++) {
    struct comp *c = &j->c[i];
    c->bw = j->mcux * c->h;
    c->bh = j->mcuy * c->v;
    c->cw = (((j->W * c->h + j->hmax - 1) / j->hmax) + 7) / 8;
    c->ch = (((j->H * c->v + j->vmax - 1) / j->vmax) + 7) / 8;
    total += (unsigned long long)c->bw * c->bh * 64;
  }
  if (total > MAX_COEFS)
    return JPEG_EUNSUP;
  for (int i = 0; i < j->ncomp; i++) {
    struct comp *c = &j->c[i];
    c->coef = calloc((unsigned)c->bw * c->bh * 64, sizeof(short));
    if (!c->coef)
      return JPEG_ENOMEM;
  }
  return JPEG_OK;
}

static int parse_dht(struct jpeg *j, const u8 *p, int n)
{
  while (n >= 17) {
    int tc = p[0] >> 4, th = p[0] & 15, total = 0;
    if (tc > 1 || th > 3)
      return JPEG_EFORMAT;
    for (int i = 0; i < 16; i++)
      total += p[1 + i];
    if (total > 256 || n < 17 + total)
      return JPEG_EFORMAT;
    huff_build(tc ? &j->ac[th] : &j->dc[th], p + 1, p + 17, total);
    p += 17 + total;
    n -= 17 + total;
  }
  return JPEG_OK;
}

static int parse_dqt(struct jpeg *j, const u8 *p, int n)
{
  while (n >= 1) {
    int pq = p[0] >> 4, tq = p[0] & 15, sz = pq ? 128 : 64;
    if (tq > 3 || n < 1 + sz)
      return JPEG_EFORMAT;
    for (int i = 0; i < 64; i++)
      j->qt[tq][zz[i]] = pq ? be16(p + 1 + 2 * i) : p[1 + i];
    p += 1 + sz;
    n -= 1 + sz;
  }
  return JPEG_OK;
}

static int parse_sos(struct jpeg *j, const u8 *p, int n, int *scan_end)
{
  int ns = p[0];
  struct comp *sc[MAX_COMP];
  if (!j->ncomp || ns < 1 || ns > j->ncomp || n < 4 + 2 * ns)
    return JPEG_EFORMAT;
  for (int i = 0; i < ns; i++) {
    int k;
    for (k = 0; k < j->ncomp && j->c[k].id != p[1 + 2 * i]; k++)
      ;
    if (k == j->ncomp)
      return JPEG_EFORMAT;
    sc[i] = &j->c[k];
    sc[i]->td = p[2 + 2 * i] >> 4;
    sc[i]->ta = p[2 + 2 * i] & 15;
    if (sc[i]->td > 3 || sc[i]->ta > 3)
      return JPEG_EFORMAT;
  }
  int ss = p[1 + 2 * ns], se = p[2 + 2 * ns], ah = p[3 + 2 * ns] >> 4, al = p[3 + 2 * ns] & 15;
  if (!j->progressive) {
    ss = 0;
    se = 63;
    ah = al = 0;
  } else if (ss > se || se > 63 || al > 13 || (ss == 0 && se != 0) || (ss > 0 && ns != 1)) {
    return JPEG_EFORMAT;
  }
  for (int i = 0; i < ns; i++)
    if ((!j->progressive || (ss == 0 && ah == 0)) && !j->dc[sc[i]->td].set)
      return JPEG_EFORMAT;
  for (int i = 0; i < ns; i++)
    if ((!j->progressive || ss > 0) && !j->ac[sc[i]->ta].set)
      return JPEG_EFORMAT;
  decode_scan(j, j->buf + *scan_end, ns, sc, ss, se, ah, al); /* a damaged scan keeps what was decoded */
  *scan_end = (int)(j->br.p - j->buf);
  return JPEG_OK;
}

static void free_all(struct jpeg *j)
{
  for (int i = 0; i < MAX_COMP; i++) {
    free(j->c[i].coef);
    free(j->c[i].plane);
  }
}

int jpeg_decode(const unsigned char *buf, int len, int max_dim, unsigned char **out, unsigned *w, unsigned *h)
{
  struct jpeg *j = calloc(1, sizeof *j);
  int rc = JPEG_EFORMAT, e, pos = 2, scans = 0;
  *out = 0;
  if (!j)
    return JPEG_ENOMEM;
  j->buf = buf;
  j->len = len;
  j->adobe_transform = -1;
  if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8)
    goto done;
  while (pos + 4 <= len) {
    if (buf[pos] != 0xFF) {
      pos++;
      continue;
    }
    int m = buf[pos + 1];
    if (m == 0xFF || m == 0 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
      pos += m == 0xFF ? 1 : 2;
      continue;
    }
    if (m == 0xD9)
      break;
    int seg = be16(buf + pos + 2);
    if (seg < 2 || pos + 2 + seg > len)
      goto done;
    const u8 *p = buf + pos + 4;
    int n = seg - 2;
    pos += 2 + seg;
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
      if (j->ncomp)
        goto done;
      j->progressive = m == 0xC2;
      if ((e = parse_sof(j, p, n)) != JPEG_OK)
        goto fail;
    } else if ((m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)) {
      e = JPEG_EUNSUP;
      goto fail;
    } else if (m == 0xC4) {
      if ((e = parse_dht(j, p, n)) != JPEG_OK)
        goto fail;
    } else if (m == 0xDB) {
      if ((e = parse_dqt(j, p, n)) != JPEG_OK)
        goto fail;
    } else if (m == 0xDD && n >= 2) {
      j->ri = be16(p);
    } else if (m == 0xEE && n >= 12 && memcmp(p, "Adobe", 5) == 0) {
      j->adobe_transform = p[11];
    } else if (m == 0xDA) {
      if (!n)
        goto done;
      int end = pos;
      if ((e = parse_sos(j, p, n, &end)) != JPEG_OK)
        goto fail;
      scans++;
      pos = end;
    }
  }
  if (!scans || !j->ncomp)
    goto done;

  int s = 1;
  if (max_dim > 0)
    while (s < 8 && (j->W > j->H ? j->W : j->H) / (s * 2) >= max_dim)
      s *= 2;
  for (int i = 0; i < j->ncomp; i++) {
    struct comp *c = &j->c[i];
    c->pw = c->bw * 8 / s;
    c->ph = c->bh * 8 / s;
    c->plane = malloc((unsigned)c->pw * c->ph);
    if (!c->plane) {
      rc = JPEG_ENOMEM;
      goto done;
    }
    build_plane(j, c, s);
    free(c->coef);
    c->coef = 0;
  }
  unsigned ow = (unsigned)(j->W + s - 1) / s, oh = (unsigned)(j->H + s - 1) / s;
  u8 *rgba = malloc(ow * oh * 4);
  if (!rgba) {
    rc = JPEG_ENOMEM;
    goto done;
  }
  int rgb = j->ncomp == 3 && j->adobe_transform == 0;
  for (unsigned y = 0; y < oh; y++)
    for (unsigned x = 0; x < ow; x++) {
      u8 *o = rgba + (y * ow + x) * 4;
      int v[MAX_COMP];
      for (int i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->c[i];
        v[i] = c->plane[(unsigned)(y * c->v / j->vmax) * c->pw + x * c->h / j->hmax];
      }
      if (j->ncomp == 1) {
        o[0] = o[1] = o[2] = (u8)v[0];
      } else if (rgb) {
        o[0] = (u8)v[0], o[1] = (u8)v[1], o[2] = (u8)v[2];
      } else {
        int cb = v[1] - 128, cr = v[2] - 128;
        o[0] = clamp8(v[0] + ((91881 * cr + 32768) >> 16));
        o[1] = clamp8(v[0] - ((22554 * cb + 46802 * cr + 32768) >> 16));
        o[2] = clamp8(v[0] + ((116130 * cb + 32768) >> 16));
      }
      o[3] = 255;
    }
  *out = rgba;
  *w = ow;
  *h = oh;
  rc = JPEG_OK;
  goto done;
fail:
  rc = e;
done:
  free_all(j);
  free(j);
  return rc;
}
