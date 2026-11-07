/* rar5.c - the RAR 5.x LZ decoder: block headers, Huffman tables, repeat distances and filters. */
#include "owos.h"
#include "rar_int.h"

#define NC5 306
#define DC5 64
#define DCX5 80
#define LDC5 16
#define RC5 44
#define MAX_FILTER_BLOCK 0x400000u

static uint32_t read_filter_data(struct bitr *b)
{
  unsigned n = br_bits(b, 2) + 1;
  uint32_t v = 0;
  for (unsigned i = 0; i < n; i++)
    v |= br_bits(b, 8) << (8 * i);
  return v;
}

static int read_tables(struct bitr *b, struct lz5 *L)
{
  uint8_t bc_len[20], lens[NC5 + DCX5 + LDC5 + RC5];
  struct huff bc;
  unsigned dc = L->filters_v7 ? DCX5 : DC5;
  unsigned total = NC5 + dc + LDC5 + RC5;
  for (unsigned i = 0; i < 20;) {
    unsigned n = br_bits(b, 4);
    if (n == 15) {
      unsigned z = br_bits(b, 4);
      if (z == 0)
        bc_len[i++] = 15;
      else
        for (z += 2; z && i < 20; z--)
          bc_len[i++] = 0;
    } else
      bc_len[i++] = (uint8_t)n;
  }
  if (!huff_build(&bc, bc_len, 20))
    return RAR_ECORRUPT;
  for (unsigned i = 0; i < total;) {
    int sym = huff_decode(b, &bc);
    if (sym < 0 || br_over(b))
      return RAR_ECORRUPT;
    if (sym < 16) {
      lens[i++] = (uint8_t)sym;
      continue;
    }
    unsigned n = (sym & 1) ? 11 + br_bits(b, 7) : 3 + br_bits(b, 3);
    uint8_t v = 0;
    if (sym < 18) {
      if (i == 0)
        return RAR_ECORRUPT;
      v = lens[i - 1];
    }
    for (; n && i < total; n--)
      lens[i++] = v;
  }
  if (br_over(b))
    return RAR_ECORRUPT;
  if (!huff_build(&L->main, lens, NC5) || !huff_build(&L->dist, lens + NC5, dc) ||
      !huff_build(&L->low, lens + NC5 + dc, LDC5) || !huff_build(&L->rep, lens + NC5 + dc + LDC5, RC5))
    return RAR_ECORRUPT;
  L->have_tables = true;
  return RAR_OK;
}

static uint32_t slot_to_len(struct bitr *b, unsigned slot)
{
  unsigned lbits = 0;
  uint32_t len = 2;
  if (slot < 8)
    len += slot;
  else {
    lbits = slot / 4 - 1;
    len += (4u | (slot & 3)) << lbits;
  }
  if (lbits)
    len += br_bits(b, lbits);
  return len;
}

static int do_copy(struct rar *r, uint64_t produced, uint64_t target, uint32_t len, uint64_t dist)
{
  struct lzwin *w = &r->win;
  if (target != UINT64_MAX && len > target - produced)
    return RAR_ECORRUPT;
  int rc = lzw_reserve(r, len);
  if (rc)
    return rc;
  if (dist == 0 || dist > w->abs || dist > w->size)
    return RAR_ECORRUPT;
  lzw_copy(w, (uint32_t)dist, len);
  return RAR_OK;
}

int rar5_decode(struct rar *r)
{
  struct rar_file *f = &r->cur;
  struct lz5 *L = &r->l5;
  struct bitr *b = &r->br;
  struct lzwin *w = &r->win;
  int rc;
  bool solid = f->comp_solid;
  uint64_t target = f->size_unknown ? UINT64_MAX : f->size;
  uint64_t cap64 = 0x20000;
  while (cap64 < f->dict && cap64 < 0x80000000u)
    cap64 <<= 1;
  uint32_t cap = (uint32_t)cap64;
  if (solid) {
    if (!w->buf || r->broken)
      return RAR_ESTATE;
    if (cap > w->cap)
      w->cap = cap;
  } else {
    rc = lzw_init(r, cap, target == UINT64_MAX ? cap : target);
    if (rc)
      return rc;
    memset(L->dcache, 0, sizeof L->dcache);
    L->last_len = 0;
    L->have_tables = false;
  }
  L->filters_v7 = f->v7;
  w->file_base = w->abs;
  uint64_t start_abs = w->abs;
  br_reset(b);
  bool last = false;
  while (!last) {
    if (target != UINT64_MAX && w->abs - start_abs >= target)
      break;
    br_align(b);
    uint32_t flags = br_bits(b, 8), chk = br_bits(b, 8);
    unsigned nb = ((flags >> 3) & 7) + 1;
    if (nb > 3)
      return RAR_ECORRUPT;
    uint32_t size = 0, sum = 0x5a ^ flags;
    for (unsigned i = 0; i < nb; i++) {
      uint32_t x = br_bits(b, 8);
      size |= x << (8 * i);
      sum ^= x;
    }
    if (br_over(b))
      return b->err ? b->err : RAR_ECORRUPT;
    if ((sum & 0xff) != chk || size == 0)
      return RAR_ECORRUPT;
    uint64_t end_bits = br_abs_bits(b) + (uint64_t)(size - 1) * 8 + (flags & 7) + 1;
    last = (flags & 0x40) != 0;
    if (flags & 0x80) {
      rc = read_tables(b, L);
      if (rc)
        return b->err ? b->err : rc;
    } else if (!L->have_tables)
      return RAR_ECORRUPT;
    while (br_abs_bits(b) < end_bits) {
      if (target != UINT64_MAX && w->abs - start_abs >= target)
        break;
      int sym = huff_decode(b, &L->main);
      if (sym < 0 || br_over(b))
        return b->err ? b->err : RAR_ECORRUPT;
      uint64_t produced = w->abs - start_abs;
      if (sym < 256) {
        rc = lzw_reserve(r, 1);
        if (rc)
          return rc;
        lzw_put(w, (uint8_t)sym);
      } else if (sym == 256) {
        uint32_t fs = read_filter_data(b), fl = read_filter_data(b);
        unsigned type = br_bits(b, 3), chan = 0;
        if (type == 0)
          chan = br_bits(b, 5) + 1;
        if (type > 3)
          return RAR_EUNSUPPORTED;
        if (fl > MAX_FILTER_BLOCK)
          fl = 0;
        struct lzfilter nf;
        memset(&nf, 0, sizeof nf);
        nf.start = w->abs + fs;
        nf.len = fl;
        nf.type = type == 0 ? FT_DELTA : type == 1 ? FT_E8 : type == 2 ? FT_E8E9 : FT_ARM;
        nf.v5 = true;
        nf.r[0] = chan;
        rc = lzw_add_filter(r, &nf);
        if (rc)
          return rc;
      } else if (sym == 257) {
        if (L->last_len) {
          rc = do_copy(r, produced, target, L->last_len, L->dcache[0]);
          if (rc)
            return rc;
        }
      } else if (sym < 262) {
        unsigned idx = (unsigned)sym - 258;
        uint32_t dist = L->dcache[idx];
        for (unsigned i = idx; i > 0; i--)
          L->dcache[i] = L->dcache[i - 1];
        L->dcache[0] = dist;
        int ls = huff_decode(b, &L->rep);
        if (ls < 0 || br_over(b))
          return b->err ? b->err : RAR_ECORRUPT;
        uint32_t len = slot_to_len(b, (unsigned)ls);
        L->last_len = len;
        rc = do_copy(r, produced, target, len, dist);
        if (rc)
          return rc;
      } else {
        uint32_t len = slot_to_len(b, (unsigned)sym - 262);
        int ds = huff_decode(b, &L->dist);
        if (ds < 0)
          return b->err ? b->err : RAR_ECORRUPT;
        uint64_t dist = 1;
        unsigned dbits = 0;
        if (ds < 4)
          dist += (unsigned)ds;
        else {
          dbits = (unsigned)ds / 2 - 1;
          dist += (uint64_t)(2u | ((unsigned)ds & 1)) << dbits;
        }
        if (dbits) {
          if (dbits >= 4) {
            if (dbits > 4) {
              unsigned hb = dbits - 4;
              uint64_t v = hb > 24 ? (uint64_t)br_bits(b, hb - 16) << 16 : 0;
              v |= br_bits(b, hb > 24 ? 16 : hb);
              dist += v << 4;
            }
            int ld = huff_decode(b, &L->low);
            if (ld < 0)
              return b->err ? b->err : RAR_ECORRUPT;
            dist += (unsigned)ld;
          } else
            dist += br_bits(b, dbits);
        }
        if (dist > 0x100) {
          len++;
          if (dist > 0x2000) {
            len++;
            if (dist > 0x40000)
              len++;
          }
        }
        if (br_over(b))
          return b->err ? b->err : RAR_ECORRUPT;
        if (dist > 0xFFFFFFFFu)
          return RAR_EUNSUPPORTED;
        for (int i = 3; i > 0; i--)
          L->dcache[i] = L->dcache[i - 1];
        L->dcache[0] = (uint32_t)dist;
        L->last_len = len;
        rc = do_copy(r, produced, target, len, dist);
        if (rc)
          return rc;
      }
      if (w->abs >= w->next_flush) {
        rc = lzw_flush(r, false);
        if (rc)
          return rc;
        w->next_flush = w->abs + 0x40000;
      }
    }
    if (br_abs_bits(b) > end_bits)
      return RAR_ECORRUPT;
  }
  if (target != UINT64_MAX && w->abs - start_abs != target)
    return RAR_ECORRUPT;
  return lzw_flush(r, true);
}
