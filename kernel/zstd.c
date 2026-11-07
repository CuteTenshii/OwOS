/* zstd.c: Zstandard decoder (RFC 8878): raw/RLE/compressed blocks, Huffman and FSE entropy coding, repeat offsets.
 * The whole frame is decoded into one flat output buffer, so matches may reach back to the start of the frame. */

#include <stdbool.h>
#include <stddef.h>

#include "endian.h"
#include "heap.h"
#include "kstring.h"
#include "zstd.h"

#define ZS_MAGIC 0xFD2FB528u
#define ZS_SKIP_MAGIC 0x184D2A50u
#define ZS_MAX_BLOCK 131072u
#define ZS_MAX_HUF_LOG 12u
#define ZS_LL_MAXSYM 35u
#define ZS_OF_MAXSYM 31u
#define ZS_ML_MAXSYM 52u
#define ZS_LL_MAXLOG 9u
#define ZS_OF_MAXLOG 8u
#define ZS_ML_MAXLOG 9u

struct zs_fse_entry {
  uint16_t newstate;
  uint8_t sym;
  uint8_t nbits;
};

struct zs_fse {
  uint8_t log;
  bool valid;
  struct zs_fse_entry e[1u << ZS_LL_MAXLOG];
};

struct zs_huf_entry {
  uint8_t sym;
  uint8_t nbits;
};

struct zs_ctx {
  struct zs_fse ll, of, ml, wt;
  bool huf_valid;
  uint8_t huf_log;
  struct zs_huf_entry huf[1u << ZS_MAX_HUF_LOG];
  uint32_t rep[3];
  uint32_t lit_len;
  uint8_t lit[ZS_MAX_BLOCK];
};

static const int16_t zs_ll_default[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,  1,  2,  2,
                                          2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const int16_t zs_ml_default[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1,  1,  1,  1,  1,  1,  1,  1, 1,
                                          1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,  1,  1, 1,
                                          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
static const int16_t zs_of_default[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1,  1,  1,  1, 1,
                                          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1};

static const uint32_t zs_ll_base[36] = {0,  1,  2,   3,   4,   5,    6,    7,    8,    9,     10,    11,
                                        12, 13, 14,  15,  16,  18,   20,   22,   24,   28,    32,    40,
                                        48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const uint8_t zs_ll_bits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  1,  1,
                                       1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint32_t zs_ml_base[53] = {3,  4,   5,   6,   7,    8,    9,    10,   11,    12,    13,   14, 15, 16,
                                        17, 18,  19,  20,  21,   22,   23,   24,   25,    26,    27,   28, 29, 30,
                                        31, 32,  33,  34,  35,   37,   39,   41,   43,    47,    51,   59, 67, 83,
                                        99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
static const uint8_t zs_ml_bits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  1,  1,  1, 1,
                                       2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

static unsigned zs_highbit(uint32_t v)
{
  unsigned n = 0;
  while (v >>= 1)
    n++;
  return n;
}

static uint32_t zs_bits_at(const uint8_t *buf, uint32_t len, uint64_t start, unsigned n)
{
  uint64_t byte = start >> 3, v = 0;
  for (unsigned i = 0; i < 5 && byte + i < len; i++)
    v |= (uint64_t)buf[byte + i] << (8 * i);
  return (uint32_t)((v >> (start & 7)) & ((1ull << n) - 1));
}

/* Backward bit reader: pos counts the unread bits below the end marker; reads past the start yield zeros. */
struct zs_rbits {
  const uint8_t *buf;
  uint32_t len;
  int64_t pos;
};

static int zs_rb_init(struct zs_rbits *b, const uint8_t *buf, uint32_t len)
{
  if (len == 0 || buf[len - 1] == 0)
    return -1;
  b->buf = buf;
  b->len = len;
  b->pos = (int64_t)(len - 1) * 8 + zs_highbit(buf[len - 1]);
  return 0;
}

static uint32_t zs_rb_peek(const struct zs_rbits *b, unsigned n)
{
  if (n == 0 || b->pos <= 0)
    return 0;
  int64_t start = b->pos - n;
  if (start >= 0)
    return zs_bits_at(b->buf, b->len, (uint64_t)start, n);
  return zs_bits_at(b->buf, b->len, 0, (unsigned)b->pos) << (unsigned)(-start);
}

static uint32_t zs_rb_read(struct zs_rbits *b, unsigned n)
{
  uint32_t v = zs_rb_peek(b, n);
  b->pos -= n;
  return v;
}

static int zs_fse_build(struct zs_fse *t, const int16_t *norm, unsigned nsyms, unsigned log)
{
  uint32_t size = 1u << log, high = size - 1, pos = 0;
  uint16_t next[ZS_ML_MAXSYM + 1];
  uint32_t step = (size >> 1) + (size >> 3) + 3;

  for (unsigned s = 0; s < nsyms; s++) {
    if (norm[s] == -1) {
      t->e[high--].sym = (uint8_t)s;
      next[s] = 1;
    } else {
      next[s] = (uint16_t)norm[s];
    }
  }
  for (unsigned s = 0; s < nsyms; s++) {
    for (int i = 0; i < norm[s]; i++) {
      t->e[pos].sym = (uint8_t)s;
      do {
        pos = (pos + step) & (size - 1);
      } while (pos > high);
    }
  }
  if (pos != 0)
    return -1;
  for (uint32_t u = 0; u < size; u++) {
    uint32_t ns = next[t->e[u].sym]++;
    unsigned nb = log - zs_highbit(ns);
    t->e[u].nbits = (uint8_t)nb;
    t->e[u].newstate = (uint16_t)((ns << nb) - size);
  }
  t->log = (uint8_t)log;
  t->valid = true;
  return 0;
}

static void zs_fse_rle(struct zs_fse *t, uint8_t sym)
{
  t->e[0].sym = sym;
  t->e[0].nbits = 0;
  t->e[0].newstate = 0;
  t->log = 0;
  t->valid = true;
}

static uint32_t zs_fb_peek(const uint8_t *buf, uint32_t len, uint64_t pos, unsigned n)
{
  return zs_bits_at(buf, len, pos, n);
}

/* Read an FSE normalized-count header. On success *used is the byte length of the header. */
static int zs_read_ncount(const uint8_t *src, uint32_t slen, int16_t *norm, unsigned maxsym, unsigned maxlog,
                          unsigned *nsyms, unsigned *log, uint32_t *used)
{
  uint64_t pos = 0;
  unsigned al = zs_fb_peek(src, slen, pos, 4) + 5;
  pos += 4;
  if (al > maxlog)
    return -1;
  int32_t remaining = (int32_t)(1u << al) + 1;
  int32_t threshold = (int32_t)(1u << al);
  unsigned nb = al + 1, sym = 0;
  bool prev0 = false;

  while (remaining > 1 && sym <= maxsym) {
    if (prev0) {
      unsigned n0 = sym, r;
      do {
        r = zs_fb_peek(src, slen, pos, 2);
        pos += 2;
        n0 += r;
      } while (r == 3 && n0 <= maxsym);
      if (n0 > maxsym)
        return -1;
      while (sym < n0)
        norm[sym++] = 0;
    }
    int32_t max = 2 * threshold - 1 - remaining;
    int32_t v = (int32_t)zs_fb_peek(src, slen, pos, nb), count;
    if ((v & (threshold - 1)) < max) {
      count = v & (threshold - 1);
      pos += nb - 1;
    } else {
      count = v & (2 * threshold - 1);
      if (count >= threshold)
        count -= max;
      pos += nb;
    }
    count--;
    remaining -= count < 0 ? -count : count;
    norm[sym++] = (int16_t)count;
    prev0 = count == 0;
    while (remaining < threshold) {
      nb--;
      threshold >>= 1;
    }
  }
  if (remaining != 1 || pos > (uint64_t)slen * 8)
    return -1;
  *nsyms = sym;
  *log = al;
  *used = (uint32_t)((pos + 7) >> 3);
  return 0;
}

/* Parse the Huffman tree description at src (hb is its first byte); fills c->huf. Returns bytes used or -1. */
static int zs_read_huf_tree(struct zs_ctx *c, const uint8_t *src, uint32_t slen)
{
  uint8_t w[256];
  uint32_t nw = 0, used;
  if (slen < 1)
    return -1;
  uint32_t hb = src[0];

  if (hb >= 128) {
    nw = hb - 127;
    used = 1 + (nw + 1) / 2;
    if (used > slen)
      return -1;
    for (uint32_t i = 0; i < nw; i++)
      w[i] = (i & 1) ? (src[1 + i / 2] & 15) : (src[1 + i / 2] >> 4);
  } else {
    int16_t norm[16];
    struct zs_fse *t = &c->wt;
    unsigned nsyms, log;
    uint32_t hdr;
    used = 1 + hb;
    if (hb == 0 || used > slen)
      return -1;
    if (zs_read_ncount(src + 1, hb, norm, 12, 6, &nsyms, &log, &hdr) != 0)
      return -1;
    if (zs_fse_build(t, norm, nsyms, log) != 0)
      return -1;
    struct zs_rbits b;
    if (zs_rb_init(&b, src + 1 + hdr, hb - hdr) != 0)
      return -1;
    uint32_t s1 = zs_rb_read(&b, log), s2 = zs_rb_read(&b, log);
    for (;;) {
      if (nw + 2 > 255)
        return -1;
      w[nw++] = t->e[s1].sym;
      s1 = t->e[s1].newstate + zs_rb_read(&b, t->e[s1].nbits);
      if (b.pos < 0) {
        w[nw++] = t->e[s2].sym;
        break;
      }
      w[nw++] = t->e[s2].sym;
      s2 = t->e[s2].newstate + zs_rb_read(&b, t->e[s2].nbits);
      if (b.pos < 0) {
        w[nw++] = t->e[s1].sym;
        break;
      }
    }
  }

  uint32_t total = 0;
  for (uint32_t i = 0; i < nw; i++) {
    if (w[i] > ZS_MAX_HUF_LOG)
      return -1;
    if (w[i])
      total += 1u << (w[i] - 1);
  }
  if (total == 0)
    return -1;
  unsigned maxbits = zs_highbit(total) + 1;
  if (maxbits > ZS_MAX_HUF_LOG)
    return -1;
  uint32_t rest = (1u << maxbits) - total;
  if (rest & (rest - 1))
    return -1;
  w[nw++] = (uint8_t)(zs_highbit(rest) + 1);

  uint32_t pos = 0;
  for (unsigned wt = 1; wt <= maxbits; wt++) {
    for (uint32_t s = 0; s < nw; s++) {
      if (w[s] != wt)
        continue;
      uint32_t n = 1u << (wt - 1);
      for (uint32_t i = 0; i < n; i++) {
        c->huf[pos + i].sym = (uint8_t)s;
        c->huf[pos + i].nbits = (uint8_t)(maxbits + 1 - wt);
      }
      pos += n;
    }
  }
  c->huf_log = (uint8_t)maxbits;
  c->huf_valid = true;
  return (int)used;
}

static int zs_huf_stream(const struct zs_ctx *c, const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t n)
{
  struct zs_rbits b;
  if (zs_rb_init(&b, src, slen) != 0)
    return -1;
  for (uint32_t i = 0; i < n; i++) {
    struct zs_huf_entry e = c->huf[zs_rb_peek(&b, c->huf_log)];
    dst[i] = e.sym;
    b.pos -= e.nbits;
  }
  return b.pos == 0 ? 0 : -1;
}

/* Decode the literals section into c->lit. Returns the section size or -1. */
static int zs_literals(struct zs_ctx *c, const uint8_t *blk, uint32_t bsz)
{
  if (bsz < 1)
    return -1;
  uint32_t type = blk[0] & 3, sf = (blk[0] >> 2) & 3, hdr, regen, comp = 0, v;

  if (type < 2) {
    hdr = sf == 3 ? 3 : sf == 1 ? 2 : 1;
    if (bsz < hdr)
      return -1;
    v = blk[0] | (hdr > 1 ? (uint32_t)blk[1] << 8 : 0) | (hdr > 2 ? (uint32_t)blk[2] << 16 : 0);
    regen = hdr == 1 ? v >> 3 : v >> 4;
    if (hdr == 2)
      regen &= 0xFFF;
    if (regen > ZS_MAX_BLOCK)
      return -1;
    uint32_t body = type == 0 ? regen : 1;
    if (hdr + body > bsz)
      return -1;
    if (type == 0)
      memcpy(c->lit, blk + hdr, regen);
    else
      memset(c->lit, blk[hdr], regen);
    c->lit_len = regen;
    return (int)(hdr + body);
  }

  hdr = sf == 0 || sf == 1 ? 3 : sf == 2 ? 4 : 5;
  if (bsz < hdr)
    return -1;
  uint64_t big = 0;
  for (uint32_t i = 0; i < hdr; i++)
    big |= (uint64_t)blk[i] << (8 * i);
  big >>= 4;
  unsigned rbits = sf <= 1 ? 10 : sf == 2 ? 14 : 18;
  regen = (uint32_t)(big & ((1u << rbits) - 1));
  comp = (uint32_t)((big >> rbits) & ((1u << rbits) - 1));
  if (regen > ZS_MAX_BLOCK || comp == 0 || (uint64_t)hdr + comp > bsz)
    return -1;

  const uint8_t *p = blk + hdr;
  uint32_t left = comp;
  if (type == 2) {
    int t = zs_read_huf_tree(c, p, left);
    if (t < 0)
      return -1;
    p += t;
    left -= (uint32_t)t;
  } else if (!c->huf_valid) {
    return -1;
  }

  if (sf == 0) {
    if (zs_huf_stream(c, p, left, c->lit, regen) != 0)
      return -1;
  } else {
    if (left < 6)
      return -1;
    uint32_t s1 = p[0] | ((uint32_t)p[1] << 8), s2 = p[2] | ((uint32_t)p[3] << 8), s3 = p[4] | ((uint32_t)p[5] << 8);
    uint32_t seg = (regen + 3) / 4;
    if (3 * seg > regen || (uint64_t)6 + s1 + s2 + s3 >= left)
      return -1;
    uint32_t s4 = left - 6 - s1 - s2 - s3;
    const uint8_t *q = p + 6;
    if (zs_huf_stream(c, q, s1, c->lit, seg) != 0 || zs_huf_stream(c, q + s1, s2, c->lit + seg, seg) != 0 ||
        zs_huf_stream(c, q + s1 + s2, s3, c->lit + 2 * seg, seg) != 0 ||
        zs_huf_stream(c, q + s1 + s2 + s3, s4, c->lit + 3 * seg, regen - 3 * seg) != 0)
      return -1;
  }
  c->lit_len = regen;
  return (int)(hdr + comp);
}

/* Set up one of the three sequence tables from its 2-bit mode. Returns bytes consumed or -1. */
static int zs_seq_table(struct zs_fse *t, unsigned mode, const uint8_t *src, uint32_t slen, const int16_t *def,
                        unsigned def_n, unsigned def_log, unsigned maxsym, unsigned maxlog)
{
  int16_t norm[ZS_ML_MAXSYM + 1];
  unsigned nsyms, log;
  uint32_t used;

  switch (mode) {
  case 0:
    return zs_fse_build(t, def, def_n, def_log) == 0 ? 0 : -1;
  case 1:
    if (slen < 1 || src[0] > maxsym)
      return -1;
    zs_fse_rle(t, src[0]);
    return 1;
  case 2:
    if (zs_read_ncount(src, slen, norm, maxsym, maxlog, &nsyms, &log, &used) != 0)
      return -1;
    if (zs_fse_build(t, norm, nsyms, log) != 0)
      return -1;
    return (int)used;
  default:
    return t->valid ? 0 : -1;
  }
}

static int zs_sequences(struct zs_ctx *c, const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap, uint32_t *opp)
{
  uint32_t ip = 0, op = *opp, nseq;

  if (slen < 1)
    return -1;
  if (src[0] < 128) {
    nseq = src[ip++];
  } else if (src[0] < 255) {
    if (slen < 2)
      return -1;
    nseq = ((uint32_t)(src[0] - 128) << 8) + src[1];
    ip = 2;
  } else {
    if (slen < 3)
      return -1;
    nseq = src[1] + ((uint32_t)src[2] << 8) + 0x7F00;
    ip = 3;
  }

  if (nseq == 0) {
    if (ip != slen || (uint64_t)op + c->lit_len > dcap)
      return -1;
    memcpy(dst + op, c->lit, c->lit_len);
    *opp = op + c->lit_len;
    return 0;
  }

  if (ip >= slen)
    return -1;
  uint32_t modes = src[ip++];
  if (modes & 3)
    return -1;
  int n = zs_seq_table(&c->ll, modes >> 6, src + ip, slen - ip, zs_ll_default, 36, 6, ZS_LL_MAXSYM, ZS_LL_MAXLOG);
  if (n < 0)
    return -1;
  ip += (uint32_t)n;
  n = zs_seq_table(&c->of, (modes >> 4) & 3, src + ip, slen - ip, zs_of_default, 29, 5, ZS_OF_MAXSYM, ZS_OF_MAXLOG);
  if (n < 0)
    return -1;
  ip += (uint32_t)n;
  n = zs_seq_table(&c->ml, (modes >> 2) & 3, src + ip, slen - ip, zs_ml_default, 53, 6, ZS_ML_MAXSYM, ZS_ML_MAXLOG);
  if (n < 0)
    return -1;
  ip += (uint32_t)n;

  struct zs_rbits b;
  if (zs_rb_init(&b, src + ip, slen - ip) != 0)
    return -1;
  uint32_t sl = zs_rb_read(&b, c->ll.log), so = zs_rb_read(&b, c->of.log), sm = zs_rb_read(&b, c->ml.log);
  uint32_t lp = 0;

  for (uint32_t i = 0; i < nseq; i++) {
    const struct zs_fse_entry *el = &c->ll.e[sl], *eo = &c->of.e[so], *em = &c->ml.e[sm];
    unsigned ocode = eo->sym;
    uint32_t ovalue = (1u << ocode) + zs_rb_read(&b, ocode);
    uint32_t mlen = zs_ml_base[em->sym] + zs_rb_read(&b, zs_ml_bits[em->sym]);
    uint32_t llen = zs_ll_base[el->sym] + zs_rb_read(&b, zs_ll_bits[el->sym]);
    if (b.pos < 0)
      return -1;
    if (i + 1 < nseq) {
      sl = el->newstate + zs_rb_read(&b, el->nbits);
      sm = em->newstate + zs_rb_read(&b, em->nbits);
      so = eo->newstate + zs_rb_read(&b, eo->nbits);
    }

    uint32_t off;
    if (ovalue > 3) {
      off = ovalue - 3;
      c->rep[2] = c->rep[1];
      c->rep[1] = c->rep[0];
      c->rep[0] = off;
    } else {
      uint32_t idx = ovalue - 1 + (llen == 0 ? 1 : 0);
      if (idx == 0) {
        off = c->rep[0];
      } else {
        off = idx == 3 ? c->rep[0] - 1 : c->rep[idx];
        if (off == 0)
          return -1;
        if (idx >= 2)
          c->rep[2] = c->rep[1];
        c->rep[1] = c->rep[0];
        c->rep[0] = off;
      }
    }

    if (llen > c->lit_len - lp || (uint64_t)op + llen > dcap)
      return -1;
    memcpy(dst + op, c->lit + lp, llen);
    lp += llen;
    op += llen;
    if (off > op || (uint64_t)op + mlen > dcap)
      return -1;
    for (uint32_t k = 0; k < mlen; k++, op++)
      dst[op] = dst[op - off];
  }
  if (b.pos != 0)
    return -1;

  uint32_t rest = c->lit_len - lp;
  if ((uint64_t)op + rest > dcap)
    return -1;
  memcpy(dst + op, c->lit + lp, rest);
  *opp = op + rest;
  return 0;
}

int zstd_decompress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap)
{
  uint32_t ip = 0, op = 0;

  for (;;) {
    if (slen - ip < 4)
      return -1;
    uint32_t magic = load_le32(src + ip);
    if ((magic & 0xFFFFFFF0u) != ZS_SKIP_MAGIC)
      break;
    if (slen - ip < 8)
      return -1;
    uint32_t skip = load_le32(src + ip + 4);
    if (skip > slen - ip - 8)
      return -1;
    ip += 8 + skip;
  }
  if (load_le32(src + ip) != ZS_MAGIC)
    return -1;
  ip += 4;

  if (ip >= slen)
    return -1;
  uint32_t fhd = src[ip++];
  bool single = fhd & 0x20, has_sum = fhd & 4;
  uint32_t fcs_flag = fhd >> 6, dict_flag = fhd & 3;
  if (fhd & 8)
    return -1;
  uint32_t need = (single ? 0 : 1) + (dict_flag == 3 ? 4 : dict_flag) +
                  (fcs_flag == 0   ? (single ? 1 : 0)
                   : fcs_flag == 1 ? 2
                   : fcs_flag == 2 ? 4
                                   : 8);
  if (slen - ip < need)
    return -1;
  if (!single)
    ip++;
  for (uint32_t i = 0, n = dict_flag == 3 ? 4 : dict_flag; i < n; i++)
    if (src[ip + i])
      return -1;
  ip += dict_flag == 3 ? 4 : dict_flag;
  if (fcs_flag == 1 || fcs_flag == 2 || fcs_flag == 3 || single) {
    uint64_t fcs = 0;
    uint32_t n = fcs_flag == 0 ? 1 : fcs_flag == 1 ? 2 : fcs_flag == 2 ? 4 : 8;
    for (uint32_t i = 0; i < n; i++)
      fcs |= (uint64_t)src[ip + i] << (8 * i);
    if (fcs_flag == 1)
      fcs += 256;
    if (fcs > dcap)
      return -1;
    ip += n;
  }

  struct zs_ctx *c = kmalloc(sizeof *c);
  if (!c)
    return -1;
  c->ll.valid = c->of.valid = c->ml.valid = false;
  c->huf_valid = false;
  c->rep[0] = 1;
  c->rep[1] = 4;
  c->rep[2] = 8;

  int ret = -1;
  for (bool last = false; !last;) {
    if (slen - ip < 3)
      goto out;
    uint32_t bh = src[ip] | ((uint32_t)src[ip + 1] << 8) | ((uint32_t)src[ip + 2] << 16);
    ip += 3;
    last = bh & 1;
    uint32_t btype = (bh >> 1) & 3, bsz = bh >> 3;
    if (bsz > ZS_MAX_BLOCK)
      goto out;
    if (btype == 1) {
      if (slen - ip < 1 || (uint64_t)op + bsz > dcap)
        goto out;
      memset(dst + op, src[ip], bsz);
      op += bsz;
      ip += 1;
    } else if (btype == 0) {
      if (slen - ip < bsz || (uint64_t)op + bsz > dcap)
        goto out;
      memcpy(dst + op, src + ip, bsz);
      op += bsz;
      ip += bsz;
    } else if (btype == 2) {
      if (slen - ip < bsz)
        goto out;
      int lit = zs_literals(c, src + ip, bsz);
      if (lit < 0 || zs_sequences(c, src + ip + lit, bsz - (uint32_t)lit, dst, dcap, &op) != 0)
        goto out;
      ip += bsz;
    } else {
      goto out;
    }
  }
  if (has_sum && slen - ip < 4)
    goto out;
  ret = (int)op;
out:
  kfree(c);
  return ret;
}
