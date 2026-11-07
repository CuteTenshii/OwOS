/* rar_enc.h - test-only RAR writers (RAR5 and RAR 3/4) used to build archives for the host tests.
 * The bit layouts follow the published RAR format; nothing here is shared with the decoder
 * except the CRC and BLAKE2sp primitives, which the tests check separately. */
#ifndef RAR_ENC_H
#define RAR_ENC_H

#include "crc32.h"
#include "rar_int.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#define RAR_ENC_CRYPTO 1
#endif

struct eb {
  uint8_t *p;
  size_t n, cap;
};

static void eb_put(struct eb *b, const void *d, size_t n)
{
  if (b->n + n > b->cap) {
    b->cap = (b->n + n) * 2 + 64;
    b->p = realloc(b->p, b->cap);
  }
  if (n)
    memcpy(b->p + b->n, d, n);
  b->n += n;
}
static void eb_u8(struct eb *b, unsigned v)
{
  uint8_t c = (uint8_t)v;
  eb_put(b, &c, 1);
}
static void eb_le16(struct eb *b, uint32_t v)
{
  eb_u8(b, v);
  eb_u8(b, v >> 8);
}
static void eb_le32(struct eb *b, uint32_t v)
{
  eb_le16(b, v);
  eb_le16(b, v >> 16);
}
static void eb_vint(struct eb *b, uint64_t v)
{
  while (v >= 0x80) {
    eb_u8(b, (unsigned)(v & 0x7f) | 0x80);
    v >>= 7;
  }
  eb_u8(b, (unsigned)v);
}
static void eb_free(struct eb *b)
{
  free(b->p);
  memset(b, 0, sizeof *b);
}

/* MSB-first bit writer */
struct bw {
  struct eb b;
  uint64_t bits;
};
static void bw_put(struct bw *w, uint32_t v, unsigned n)
{
  for (int i = (int)n - 1; i >= 0; i--) {
    if ((w->bits & 7) == 0)
      eb_u8(&w->b, 0);
    if ((v >> i) & 1)
      w->b.p[w->b.n - 1] |= (uint8_t)(0x80 >> (w->bits & 7));
    w->bits++;
  }
}
static void bw_align(struct bw *w) { w->bits = (w->bits + 7) & ~7ull; }

/* ------------------------------------------------------------------ Huffman */

static void hb_lens(const uint32_t *freq0, int n, uint8_t *lens)
{
  uint32_t freq[512];
  for (int i = 0; i < n; i++)
    freq[i] = freq0[i];
  for (;;) {
    int used = 0;
    for (int i = 0; i < n; i++)
      used += freq[i] != 0;
    for (int i = 0; used < 2 && i < n; i++)
      if (!freq[i]) {
        freq[i] = 1;
        used++;
      }
    uint64_t w[1024];
    int parent[1024], act[1024], na = 0, nn = n;
    for (int i = 0; i < n; i++) {
      w[i] = freq[i];
      parent[i] = -1;
      if (freq[i])
        act[na++] = i;
    }
    while (na > 1) {
      int a = 0, b = 1;
      if (w[act[b]] < w[act[a]]) {
        a = 1;
        b = 0;
      }
      for (int k = 2; k < na; k++) {
        if (w[act[k]] < w[act[a]]) {
          b = a;
          a = k;
        } else if (w[act[k]] < w[act[b]])
          b = k;
      }
      w[nn] = w[act[a]] + w[act[b]];
      parent[nn] = -1;
      parent[act[a]] = nn;
      parent[act[b]] = nn;
      int hi = a > b ? a : b, lo = a > b ? b : a;
      act[lo] = nn++;
      act[hi] = act[--na];
    }
    int maxd = 0;
    for (int i = 0; i < n; i++) {
      lens[i] = 0;
      if (!freq[i])
        continue;
      int d = 0;
      for (int x = i; parent[x] >= 0; x = parent[x])
        d++;
      lens[i] = (uint8_t)d;
      if (d > maxd)
        maxd = d;
    }
    if (maxd <= 15)
      return;
    for (int i = 0; i < n; i++)
      if (freq[i])
        freq[i] = (freq[i] + 1) / 2;
  }
}

static void hb_codes(const uint8_t *lens, int n, uint32_t *codes)
{
  uint32_t code = 0;
  for (int len = 1; len <= 15; len++) {
    for (int i = 0; i < n; i++)
      if (lens[i] == len)
        codes[i] = code++;
    code <<= 1;
  }
}

/* ------------------------------------------------------------------ match finder and ops */

struct mf {
  const uint8_t *buf;
  size_t total, inserted;
  int32_t *head, *prev;
};

static void mf_init(struct mf *m, const uint8_t *buf, size_t total, size_t start)
{
  m->buf = buf;
  m->total = total;
  m->head = malloc(65536 * sizeof *m->head);
  m->prev = malloc((total + 1) * sizeof *m->prev);
  for (int i = 0; i < 65536; i++)
    m->head[i] = -1;
  m->inserted = 0;
  (void)start;
}
static uint32_t mf_hash(const uint8_t *p)
{
  return ((uint32_t)p[0] * 2654435761u ^ (uint32_t)p[1] * 40503u ^ (uint32_t)p[2] * 69069u) >> 8 & 0xffff;
}
static void mf_insert_to(struct mf *m, size_t pos)
{
  for (; m->inserted < pos; m->inserted++) {
    if (m->inserted + 3 > m->total)
      continue;
    uint32_t h = mf_hash(m->buf + m->inserted);
    m->prev[m->inserted] = m->head[h];
    m->head[h] = (int32_t)m->inserted;
  }
}
static size_t common_len(const uint8_t *a, const uint8_t *b, size_t max)
{
  size_t n = 0;
  while (n < max && a[n] == b[n])
    n++;
  return n;
}
static size_t mf_best(struct mf *m, size_t pos, size_t max_len, size_t max_dist, uint32_t *dist)
{
  size_t best = 0;
  if (pos + 3 > m->total || max_len < 3)
    return 0;
  mf_insert_to(m, pos);
  int32_t c = m->head[mf_hash(m->buf + pos)];
  for (int steps = 0; c >= 0 && steps < 256; steps++, c = m->prev[c]) {
    size_t d = pos - (size_t)c;
    if (d > max_dist)
      break;
    size_t l = common_len(m->buf + pos, m->buf + c, max_len);
    if (l > best) {
      best = l;
      *dist = (uint32_t)d;
      if (l == max_len)
        break;
    }
  }
  return best >= 3 ? best : 0;
}

enum { A_MAIN, A_DIST, A_LOW, A_REP, A_RAW };

struct op {
  uint8_t alpha;
  uint16_t sym;
  uint8_t nbits;
  uint32_t val;
};

struct ops {
  struct op *p;
  size_t n, cap;
  size_t *splits;
  size_t nsplits, capsplits;
};

static void op_add(struct ops *o, int alpha, unsigned sym, unsigned nbits, uint32_t val)
{
  if (o->n == o->cap) {
    o->cap = o->cap * 2 + 256;
    o->p = realloc(o->p, o->cap * sizeof *o->p);
  }
  o->p[o->n++] = (struct op){(uint8_t)alpha, (uint16_t)sym, (uint8_t)nbits, val};
}
static void op_split(struct ops *o)
{
  if (o->nsplits == o->capsplits) {
    o->capsplits = o->capsplits * 2 + 16;
    o->splits = realloc(o->splits, o->capsplits * sizeof *o->splits);
  }
  o->splits[o->nsplits++] = o->n;
}
static void ops_free(struct ops *o)
{
  free(o->p);
  free(o->splits);
  memset(o, 0, sizeof *o);
}

static unsigned ilog2(uint32_t v)
{
  unsigned n = 0;
  while (v >>= 1)
    n++;
  return n;
}

/* ------------------------------------------------------------------ RAR 5 */

#define E_NC 306
#define E_DC 64
#define E_LDC 16
#define E_RC 44
#define E_TOTAL (E_NC + E_DC + E_LDC + E_RC)

struct filt {
  uint32_t at, start, len;
  int type, ch;
};

struct r5st {
  uint32_t dc[4], last_len;
  uint8_t lens[E_TOTAL];
  bool have_lens;
  struct eb hist;
  size_t bends[8192];
  int nbends;
  size_t wrap; /* when set, matches never span a multiple of this window size */
};

static unsigned r5_bonus(uint32_t d) { return d > 0x100 ? 1 + (d > 0x2000) + (d > 0x40000) : 0; }

static void r5_len_ops(struct ops *o, int alpha, unsigned base, uint32_t len_minus2)
{
  uint32_t x = len_minus2;
  unsigned slot, lb = 0;
  if (x < 8)
    slot = x;
  else {
    lb = ilog2(x) - 2;
    slot = (lb + 1) * 4 + ((x >> lb) - 4);
  }
  op_add(o, alpha, base + slot, 0, 0);
  if (lb)
    op_add(o, A_RAW, 0, lb, x & ((1u << lb) - 1));
}

static void r5_filter_data(struct ops *o, uint32_t v)
{
  unsigned nb = v < 0x100 ? 1 : v < 0x10000 ? 2 : v < 0x1000000 ? 3 : 4;
  op_add(o, A_RAW, 0, 2, nb - 1);
  for (unsigned i = 0; i < nb; i++)
    op_add(o, A_RAW, 0, 8, (v >> (8 * i)) & 0xff);
}

static void r5_tokenize(struct r5st *st, const uint8_t *data, size_t len, const struct filt *f, int nf, struct ops *o,
                        int block_tokens, uint32_t max_dist)
{
  size_t base = st->hist.n, total = base + len;
  uint8_t *buf = malloc(total + 8);
  memcpy(buf, st->hist.p, base);
  memcpy(buf + base, data, len);
  struct mf m;
  mf_init(&m, buf, total, base);
  int ntok = 0;
  for (size_t pos = base; pos < total;) {
    mf_insert_to(&m, pos);
    for (int i = 0; i < nf; i++)
      if (f[i].at == pos - base) {
        op_add(o, A_MAIN, 256, 0, 0);
        r5_filter_data(o, f[i].start - f[i].at);
        r5_filter_data(o, f[i].len);
        op_add(o, A_RAW, 0, 3, (unsigned)f[i].type);
        if (f[i].type == 0)
          op_add(o, A_RAW, 0, 5, (unsigned)f[i].ch - 1);
      }
    size_t brk = total;
    for (int i = 0; i < nf; i++)
      if (f[i].at > pos - base && base + f[i].at < brk)
        brk = base + f[i].at;
    size_t lim = brk - pos;
    if (lim > 4000)
      lim = 4000;
    if (st->wrap && lim > st->wrap - pos % st->wrap)
      lim = st->wrap - pos % st->wrap;
    size_t rl = 0;
    int ri = -1;
    for (int k = 0; k < 4; k++) {
      uint32_t d = st->dc[k];
      if (d && d <= pos && d <= max_dist) {
        size_t l = common_len(buf + pos, buf + pos - d, lim);
        if (l > rl) {
          rl = l;
          ri = k;
        }
      }
    }
    uint32_t nd = 0;
    size_t nl = mf_best(&m, pos, lim, max_dist, &nd);
    size_t adv;
    if (rl >= 2 && (rl + 1 >= nl || nl < 4)) {
      if (ri == 0 && rl == st->last_len)
        op_add(o, A_MAIN, 257, 0, 0);
      else {
        op_add(o, A_MAIN, 258 + (unsigned)ri, 0, 0);
        r5_len_ops(o, A_REP, 0, (uint32_t)rl - 2);
        uint32_t d = st->dc[ri];
        for (int k = ri; k > 0; k--)
          st->dc[k] = st->dc[k - 1];
        st->dc[0] = d;
        st->last_len = (uint32_t)rl;
      }
      adv = rl;
    } else if (nl >= 4 && nl - r5_bonus(nd) >= 2) {
      uint32_t lp = (uint32_t)nl - r5_bonus(nd);
      r5_len_ops(o, A_MAIN, 262, lp - 2);
      uint32_t y = nd - 1, ds, dbits = 0;
      if (y < 4)
        ds = y;
      else {
        dbits = ilog2(y) - 1;
        ds = 2 * (dbits + 1) + ((y >> dbits) & 1);
      }
      op_add(o, A_DIST, ds, 0, 0);
      uint32_t extra = dbits ? y & ((1u << dbits) - 1) : 0;
      if (dbits >= 4) {
        if (dbits > 4)
          op_add(o, A_RAW, 0, dbits - 4, extra >> 4);
        op_add(o, A_LOW, extra & 15, 0, 0);
      } else if (dbits)
        op_add(o, A_RAW, 0, dbits, extra);
      for (int k = 3; k > 0; k--)
        st->dc[k] = st->dc[k - 1];
      st->dc[0] = nd;
      st->last_len = (uint32_t)nl;
      adv = nl;
    } else {
      op_add(o, A_MAIN, buf[pos], 0, 0);
      adv = 1;
    }
    pos += adv;
    if (block_tokens && ++ntok % block_tokens == 0 && pos < total)
      op_split(o);
  }
  free(m.head);
  free(m.prev);
  free(buf);
  eb_put(&st->hist, data, len);
}

static void r5_write_table(struct bw *w, const uint8_t *lens)
{
  struct {
    uint8_t sym, xb;
    uint32_t xv;
  } t[E_TOTAL];
  int nt = 0;
  for (int i = 0; i < E_TOTAL;) {
    int run = 1;
    if (lens[i] == 0) {
      while (i + run < E_TOTAL && lens[i + run] == 0 && run < 138)
        run++;
      if (run >= 3) {
        t[nt++] = run >= 11 ? (typeof(t[0])){19, 7, (uint32_t)run - 11} : (typeof(t[0])){18, 3, (uint32_t)run - 3};
        i += run;
        continue;
      }
    } else if (i > 0 && lens[i] == lens[i - 1]) {
      while (i + run < E_TOTAL && lens[i + run] == lens[i] && run < 138)
        run++;
      if (run >= 3) {
        t[nt++] = run >= 11 ? (typeof(t[0])){17, 7, (uint32_t)run - 11} : (typeof(t[0])){16, 3, (uint32_t)run - 3};
        i += run;
        continue;
      }
    }
    t[nt++] = (typeof(t[0])){lens[i], 0, 0};
    i++;
  }
  uint32_t freq[20] = {0};
  for (int i = 0; i < nt; i++)
    freq[t[i].sym]++;
  uint8_t bl[20];
  uint32_t bc[20];
  hb_lens(freq, 20, bl);
  hb_codes(bl, 20, bc);
  for (int i = 0; i < 20;) {
    if (bl[i] == 0) {
      int run = 1;
      while (i + run < 20 && bl[i + run] == 0 && run < 17)
        run++;
      if (run >= 3) {
        bw_put(w, 15, 4);
        bw_put(w, (uint32_t)run - 2, 4);
        i += run;
      } else {
        bw_put(w, 0, 4);
        i++;
      }
    } else if (bl[i] == 15) {
      bw_put(w, 15, 4);
      bw_put(w, 0, 4);
      i++;
    } else {
      bw_put(w, bl[i], 4);
      i++;
    }
  }
  for (int i = 0; i < nt; i++) {
    bw_put(w, bc[t[i].sym], bl[t[i].sym]);
    if (t[i].xb)
      bw_put(w, t[i].xv, t[i].xb);
  }
}

static void r5_universal(uint8_t *lens)
{
  memset(lens, 9, E_NC);
  memset(lens + E_NC, 6, E_DC);
  memset(lens + E_NC + E_DC, 4, E_LDC);
  memset(lens + E_NC + E_DC + E_LDC, 6, E_RC);
}

static void r5_block_lens(const struct op *p, size_t n, uint8_t *lens)
{
  uint32_t f[4][E_NC] = {{0}};
  for (size_t i = 0; i < n; i++)
    if (p[i].alpha != A_RAW)
      f[p[i].alpha][p[i].sym]++;
  hb_lens(f[0], E_NC, lens);
  hb_lens(f[1], E_DC, lens + E_NC);
  hb_lens(f[2], E_LDC, lens + E_NC + E_DC);
  hb_lens(f[3], E_RC, lens + E_NC + E_DC + E_LDC);
}

static void r5_emit_block(struct eb *packed, const struct bw *w, bool last, bool table)
{
  size_t size = w->b.n;
  unsigned bitsize = (unsigned)((w->bits - 1) & 7) + 1;
  unsigned nb = size < 256 ? 1 : size < 65536 ? 2 : 3;
  unsigned flags = (bitsize - 1) | ((nb - 1) << 3) | (last ? 0x40 : 0) | (table ? 0x80 : 0);
  unsigned sum = 0x5a ^ flags;
  eb_u8(packed, flags);
  size_t at = packed->n;
  eb_u8(packed, 0);
  for (unsigned i = 0; i < nb; i++) {
    unsigned b = (unsigned)(size >> (8 * i)) & 0xff;
    sum ^= b;
    eb_u8(packed, b);
  }
  packed->p[at] = (uint8_t)sum;
  eb_put(packed, w->b.p, size);
}

static void r5_write_blocks(struct eb *packed, struct r5st *st, const struct ops *o, bool universal)
{
  size_t nb = o->nsplits + 1;
  for (size_t b = 0; b < nb; b++) {
    size_t from = b ? o->splits[b - 1] : 0, to = b < o->nsplits ? o->splits[b] : o->n;
    bool table = !(universal && st->have_lens);
    if (table) {
      if (universal)
        r5_universal(st->lens);
      else
        r5_block_lens(o->p + from, to - from, st->lens);
      st->have_lens = true;
    }
    struct bw w = {{0}, 0};
    if (table)
      r5_write_table(&w, st->lens);
    uint32_t codes[E_TOTAL];
    hb_codes(st->lens, E_NC, codes);
    hb_codes(st->lens + E_NC, E_DC, codes + E_NC);
    hb_codes(st->lens + E_NC + E_DC, E_LDC, codes + E_NC + E_DC);
    hb_codes(st->lens + E_NC + E_DC + E_LDC, E_RC, codes + E_NC + E_DC + E_LDC);
    static const int off[4] = {0, E_NC, E_NC + E_DC, E_NC + E_DC + E_LDC};
    for (size_t i = from; i < to; i++) {
      const struct op *p = &o->p[i];
      if (p->alpha == A_RAW) {
        bw_put(&w, p->val, p->nbits);
        continue;
      }
      bw_put(&w, codes[off[p->alpha] + p->sym], st->lens[off[p->alpha] + p->sym]);
    }
    r5_emit_block(packed, &w, b + 1 == nb, table);
    if (st->nbends < 8192)
      st->bends[st->nbends++] = packed->n;
    eb_free(&w.b);
  }
}

/* forward filters (what the encoder applies before compressing) */
static void r5_forward(uint8_t *d, size_t len, const struct filt *f, int nf)
{
  for (int i = 0; i < nf; i++) {
    uint8_t *p = d + f[i].start;
    uint32_t n = f[i].len, off = f[i].start;
    if (f[i].type == 0) {
      uint8_t *src = malloc(n);
      memcpy(src, p, n);
      uint32_t sp = 0;
      for (int ch = 0; ch < f[i].ch; ch++) {
        uint8_t prev = 0;
        for (uint32_t dp = (uint32_t)ch; dp < n; dp += (uint32_t)f[i].ch) {
          p[sp++] = (uint8_t)(prev - src[dp]);
          prev = src[dp];
        }
      }
      free(src);
    } else if (f[i].type == 1 || f[i].type == 2) {
      uint8_t cmp2 = f[i].type == 2 ? 0xe9 : 0xe8;
      for (uint32_t cur = 0; cur + 4 < n;) {
        uint8_t b = p[cur++];
        if (b == 0xe8 || b == cmp2) {
          int64_t offset = (cur + off) % 0x1000000;
          uint32_t u = p[cur] | (uint32_t)p[cur + 1] << 8 | (uint32_t)p[cur + 2] << 16 | (uint32_t)p[cur + 3] << 24;
          int64_t rel = (int32_t)u, a = rel;
          if (rel >= -offset && rel < 0x1000000 - offset)
            a = rel + offset;
          else if (rel >= 0x1000000 - offset && rel < 0x1000000)
            a = rel - 0x1000000;
          u = (uint32_t)a;
          p[cur] = (uint8_t)u;
          p[cur + 1] = (uint8_t)(u >> 8);
          p[cur + 2] = (uint8_t)(u >> 16);
          p[cur + 3] = (uint8_t)(u >> 24);
          cur += 4;
        }
      }
    } else {
      for (uint32_t cur = 0; cur + 3 < n; cur += 4) {
        uint8_t *q = p + cur;
        if (q[3] == 0xeb) {
          uint32_t v = q[0] + (uint32_t)q[1] * 0x100 + (uint32_t)q[2] * 0x10000;
          v += (off + cur) / 4;
          q[0] = (uint8_t)v;
          q[1] = (uint8_t)(v >> 8);
          q[2] = (uint8_t)(v >> 16);
        }
      }
    }
  }
  (void)len;
}

/* ------------------------------------------------------------------ host crypto (independent of the library) */

#ifdef RAR_ENC_CRYPTO
static void host_pbkdf2(const char *pw, const uint8_t *salt, size_t sl, uint32_t iters, uint8_t *out, size_t ol)
{
  CCKeyDerivationPBKDF(kCCPBKDF2, pw, strlen(pw), salt, sl, kCCPRFHmacAlgSHA256, iters, out, ol);
}
static void host_aes_cbc(int encrypt, const uint8_t *key, size_t kl, const uint8_t iv[16], const uint8_t *in,
                         uint8_t *out, size_t len)
{
  size_t moved = 0;
  uint8_t *tmp = malloc(len + 16);
  CCCrypt(encrypt ? kCCEncrypt : kCCDecrypt, kCCAlgorithmAES, 0, key, kl, iv, in, len, tmp, len + 16, &moved);
  memcpy(out, tmp, len);
  free(tmp);
}
static void host_hmac256(const uint8_t *key, size_t kl, const uint8_t *d, size_t dl, uint8_t out[32])
{
  CCHmac(kCCHmacAlgSHA256, key, kl, d, dl, out);
}
static void host_sha256(const uint8_t *d, size_t dl, uint8_t out[32]) { CC_SHA256(d, (CC_LONG)dl, out); }
#endif

static uint32_t enc_rng(uint32_t *s)
{
  *s = *s * 1664525u + 1013904223u;
  return *s >> 8;
}
static void enc_rand(uint32_t *seed, uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
    p[i] = (uint8_t)enc_rng(seed);
}

/* ------------------------------------------------------------------ RAR5 archive writer */

struct r5a {
  struct eb out;
  struct r5st st;
  bool hdr_enc;
  uint8_t hkey[32];
  uint32_t seed;
};

struct r5f {
  const char *name;
  const uint8_t *data;
  size_t len;
  int method; /* 0 store, 1..5 compressed */
  bool solid;
  int dict_shift;
  bool is_dir;
  const char *link;
  int link_type;
  uint32_t mtime, attr;
  const struct filt *filters;
  int nfilters;
  int block_tokens;
  bool universal;
  const char *password;
  int lg2;
  bool pswcheck, tweak, b2hash, no_crc;
  uint32_t max_dist;
  bool crc_in_parts;
};

static void r5a_header(struct r5a *a, unsigned type, uint64_t flags, const struct eb *body, const struct eb *extra,
                       uint64_t data_size, bool has_data)
{
  struct eb h = {0}, pre = {0}, all = {0};
  if (extra && extra->n)
    flags |= 1;
  if (has_data)
    flags |= 2;
  eb_vint(&h, type);
  eb_vint(&h, flags);
  if (flags & 1)
    eb_vint(&h, extra->n);
  if (flags & 2)
    eb_vint(&h, data_size);
  if (body)
    eb_put(&h, body->p, body->n);
  if (extra)
    eb_put(&h, extra->p, extra->n);
  eb_vint(&pre, h.n);
  struct eb crcin = {0};
  eb_put(&crcin, pre.p, pre.n);
  eb_put(&crcin, h.p, h.n);
  eb_le32(&all, crc32(0, crcin.p, (uint32_t)crcin.n));
  eb_put(&all, crcin.p, crcin.n);
  if (!a->hdr_enc)
    eb_put(&a->out, all.p, all.n);
  else {
#ifdef RAR_ENC_CRYPTO
    size_t padded = (all.n + 15) & ~(size_t)15;
    all.p = realloc(all.p, padded);
    memset(all.p + all.n, 0, padded - all.n);
    uint8_t iv[16];
    enc_rand(&a->seed, iv, 16);
    eb_put(&a->out, iv, 16);
    uint8_t *ct = malloc(padded);
    host_aes_cbc(1, a->hkey, 32, iv, all.p, ct, padded);
    eb_put(&a->out, ct, padded);
    free(ct);
#endif
  }
  eb_free(&h);
  eb_free(&pre);
  eb_free(&all);
  eb_free(&crcin);
}

static void r5a_begin(struct r5a *a, unsigned arc_flags, unsigned volnum, const char *hdr_password, int lg2)
{
  static const uint8_t sig[8] = {0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x01, 0x00};
  eb_put(&a->out, sig, 8);
  a->seed = a->seed ? a->seed : 12345;
#ifdef RAR_ENC_CRYPTO
  if (hdr_password) {
    struct eb body = {0};
    uint8_t salt[16], k[96];
    enc_rand(&a->seed, salt, 16);
    host_pbkdf2(hdr_password, salt, 16, 1u << lg2, k, 32);
    memcpy(a->hkey, k, 32);
    uint8_t chk[32], chk8[8] = {0}, d[32];
    host_pbkdf2(hdr_password, salt, 16, (1u << lg2) + 32, chk, 32);
    for (int i = 0; i < 32; i++)
      chk8[i & 7] ^= chk[i];
    host_sha256(chk8, 8, d);
    eb_vint(&body, 0);
    eb_vint(&body, 1);
    eb_u8(&body, lg2);
    eb_put(&body, salt, 16);
    eb_put(&body, chk8, 8);
    eb_put(&body, d, 4);
    r5a_header(a, 4, 0, &body, NULL, 0, false);
    eb_free(&body);
    a->hdr_enc = true;
  }
#else
  (void)hdr_password;
  (void)lg2;
#endif
  struct eb body = {0};
  eb_vint(&body, arc_flags);
  if (arc_flags & 2)
    eb_vint(&body, volnum);
  r5a_header(a, 1, 0, &body, NULL, 0, false);
  eb_free(&body);
}

static void r5a_end(struct r5a *a, bool more)
{
  struct eb body = {0};
  eb_vint(&body, more ? 1 : 0);
  r5a_header(a, 5, 0, &body, NULL, 0, false);
  eb_free(&body);
}

static void r5_pack(struct r5a *a, const struct r5f *f, struct eb *packed)
{
  if (f->method == 0) {
    eb_put(packed, f->data, f->len);
    return;
  }
  a->st.nbends = 0;
  a->st.wrap = 0x20000u << f->dict_shift;
  if (!f->solid) {
    memset(a->st.dc, 0, sizeof a->st.dc);
    a->st.last_len = 0;
    a->st.have_lens = false;
    a->st.hist.n = 0;
  }
  uint8_t *xf = malloc(f->len + 1);
  memcpy(xf, f->data, f->len);
  r5_forward(xf, f->len, f->filters, f->nfilters);
  struct ops o = {0};
  r5_tokenize(&a->st, xf, f->len, f->filters, f->nfilters, &o, f->block_tokens,
              f->max_dist ? f->max_dist : (0x20000u << f->dict_shift) - 0x1000);
  r5_write_blocks(packed, &a->st, &o, f->universal);
  ops_free(&o);
  free(xf);
}

static void r5a_put_file(struct r5a *a, const struct r5f *f, const uint8_t *packed, size_t plen, bool sb, bool sa)
{
  struct eb body = {0}, extra = {0}, data = {0};
  uint32_t crc = crc32(0, f->data, (uint32_t)f->len);
  uint8_t b2[32];
  bool want_crc = !f->no_crc && (!sa || f->crc_in_parts) && !f->is_dir;
  uint8_t enc_salt[16], enc_iv[16], enc_chk[12], hashkey[32];
  bool encrypted = f->password != NULL && plen > 0;
#ifdef RAR_ENC_CRYPTO
  if (encrypted) {
    uint8_t key[32], chk[32], chk8[8] = {0}, d[32], t2[32];
    enc_rand(&a->seed, enc_salt, 16);
    enc_rand(&a->seed, enc_iv, 16);
    host_pbkdf2(f->password, enc_salt, 16, 1u << f->lg2, key, 32);
    host_pbkdf2(f->password, enc_salt, 16, (1u << f->lg2) + 16, t2, 32);
    memcpy(hashkey, t2, 32);
    host_pbkdf2(f->password, enc_salt, 16, (1u << f->lg2) + 32, chk, 32);
    for (int i = 0; i < 32; i++)
      chk8[i & 7] ^= chk[i];
    host_sha256(chk8, 8, d);
    memcpy(enc_chk, chk8, 8);
    memcpy(enc_chk + 8, d, 4);
    size_t padded = (plen + 15) & ~(size_t)15;
    uint8_t *pt = calloc(padded, 1);
    memcpy(pt, packed, plen);
    uint8_t *ct = malloc(padded);
    host_aes_cbc(1, key, 32, enc_iv, pt, ct, padded);
    eb_put(&data, ct, padded);
    free(pt);
    free(ct);
    if (f->tweak && want_crc) {
      uint8_t raw[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)}, dg[32];
      host_hmac256(hashkey, 32, raw, 4, dg);
      crc = 0;
      for (int i = 0; i < 32; i++)
        crc ^= (uint32_t)dg[i] << ((i & 3) * 8);
    }
  } else
#endif
    eb_put(&data, packed, plen);
  (void)hashkey;
  (void)enc_chk;
  (void)enc_salt;
  (void)enc_iv;
  unsigned fflags = (f->is_dir ? 1 : 0) | (f->mtime ? 2 : 0) | (want_crc ? 4 : 0);
  eb_vint(&body, fflags);
  eb_vint(&body, f->len);
  eb_vint(&body, f->attr ? f->attr : f->is_dir ? 040755 : 0100644);
  if (f->mtime)
    eb_le32(&body, f->mtime);
  if (want_crc)
    eb_le32(&body, crc);
  eb_vint(&body,
          (f->method ? ((uint64_t)f->method << 7) | ((uint64_t)f->dict_shift << 10) : 0) | (f->solid ? 1u << 6 : 0));
  eb_vint(&body, 1);
  eb_vint(&body, strlen(f->name));
  eb_put(&body, f->name, strlen(f->name));
#ifdef RAR_ENC_CRYPTO
  if (encrypted) {
    struct eb rec = {0};
    eb_vint(&rec, 1);
    eb_vint(&rec, 0);
    eb_vint(&rec, (f->pswcheck ? 1 : 0) | (f->tweak ? 2 : 0));
    eb_u8(&rec, f->lg2);
    eb_put(&rec, enc_salt, 16);
    eb_put(&rec, enc_iv, 16);
    if (f->pswcheck)
      eb_put(&rec, enc_chk, 12);
    eb_vint(&extra, rec.n);
    eb_put(&extra, rec.p, rec.n);
    eb_free(&rec);
  }
#endif
  if (f->b2hash && !sa) {
    struct b2sp s;
    b2sp_init(&s);
    b2sp_update(&s, f->data, f->len);
    b2sp_final(&s, b2);
#ifdef RAR_ENC_CRYPTO
    if (encrypted && f->tweak) {
      uint8_t m[32];
      host_hmac256(hashkey, 32, b2, 32, m);
      memcpy(b2, m, 32);
    }
#endif
    struct eb rec = {0};
    eb_vint(&rec, 2);
    eb_vint(&rec, 0);
    eb_put(&rec, b2, 32);
    eb_vint(&extra, rec.n);
    eb_put(&extra, rec.p, rec.n);
    eb_free(&rec);
  }
  if (f->link) {
    struct eb rec = {0};
    eb_vint(&rec, 5);
    eb_vint(&rec, f->link_type);
    eb_vint(&rec, 0);
    eb_vint(&rec, strlen(f->link));
    eb_put(&rec, f->link, strlen(f->link));
    eb_vint(&extra, rec.n);
    eb_put(&extra, rec.p, rec.n);
    eb_free(&rec);
  }
  r5a_header(a, 2, (sb ? 8 : 0) | (sa ? 0x10 : 0), &body, &extra, data.n, true);
  eb_put(&a->out, data.p, data.n);
  eb_free(&body);
  eb_free(&extra);
  eb_free(&data);
}

static void r5a_add(struct r5a *a, const struct r5f *f)
{
  struct eb packed = {0};
  if (!f->is_dir && !f->link)
    r5_pack(a, f, &packed);
  r5a_put_file(a, f, packed.p, packed.n, false, false);
  eb_free(&packed);
}

#endif /* RAR_ENC_H */
