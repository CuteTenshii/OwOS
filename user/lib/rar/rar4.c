/* rar4.c - the RAR 3.x/4.x decoder: the v2.9 LZ with its Huffman tables, PPMd variant H blocks, and the
 * standard VM filters (recognized by fingerprint and run natively). */
#include "crc32.h"
#include "owos.h"
#include "rar_int.h"

#define NC4 299
#define DC4 60
#define LDC4 17
#define RC4 28
#define BC4 20
#define TABLE4 (NC4 + DC4 + LDC4 + RC4)
#define LOW_DIST_REP 16
#define MAX_PROGS 1024
#define WIN4 0x400000u

static uint32_t dbase[DC4];
static uint8_t dbits[DC4];
static bool tabs_ready;

static const uint8_t lbits[28] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5};
static const uint16_t lbase[28] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10,  12,  14,  16,  20,
                                   24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224};
static const uint8_t sdbase[8] = {0, 4, 8, 16, 32, 64, 128, 192};
static const uint8_t sdbits[8] = {2, 2, 3, 4, 5, 6, 6, 6};

static void init_tabs(void)
{
  static const uint8_t counts[19] = {4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 14, 0, 12};
  uint32_t base = 0;
  unsigned k = 0;
  for (unsigned bits = 0; bits < 19; bits++)
    for (unsigned i = 0; i < counts[bits]; i++, k++) {
      dbase[k] = base;
      dbits[k] = (uint8_t)bits;
      base += 1u << bits;
    }
  tabs_ready = true;
}

void rar4_free(struct rar *r)
{
  struct lz4 *L = &r->l4;
  if (L->ppm) {
    ppmd_free(L->ppm);
    r->mem -= L->ppm_mem;
    L->ppm = NULL;
    L->ppm_mem = 0;
  }
  if (L->progs)
    rar_unalloc(r, L->progs, (size_t)L->pcap * sizeof *L->progs);
  L->progs = NULL;
  L->nprogs = L->pcap = 0;
}

static int ppm_getbyte(void *ctx)
{
  struct bitr *b = ctx;
  uint32_t v = br_bits(b, 8);
  return b->eof && b->pos > b->len * 8 + 64 ? -1 : (int)v;
}

static int ppm_char(struct rar *r)
{
  struct lz4 *L = &r->l4;
  int c = ppmd_decode(L->ppm, &L->rc.rc);
  if (r->br.eof && r->br.pos > r->br.len * 8 + 64)
    return -3;
  return c;
}

static int read_tables(struct rar *r)
{
  struct lz4 *L = &r->l4;
  struct bitr *b = &r->br;
  uint8_t bc_len[BC4], table[TABLE4];
  struct huff bc;
  br_align(b);
  uint32_t bf = br_peek(b, 16);
  if (bf & 0x8000) {
    uint32_t flags = br_bits(b, 8);
    uint32_t maxmb = 0;
    bool reset = (flags & 0x20) != 0;
    if (reset)
      maxmb = br_bits(b, 8);
    if (flags & 0x40)
      L->esc_char = (int)br_bits(b, 8);
    if (br_over(b))
      return b->err ? b->err : RAR_ECORRUPT;
    if (reset) {
      unsigned order = (flags & 0x1f) + 1;
      if (order > 16)
        order = 16 + (order - 16) * 3;
      if (order < 2)
        return RAR_ECORRUPT;
      uint64_t mem = ((uint64_t)maxmb + 1) << 20;
      if (!L->ppm || L->ppm_mem != mem) {
        if (L->ppm) {
          ppmd_free(L->ppm);
          r->mem -= L->ppm_mem;
          L->ppm = NULL;
          L->ppm_mem = 0;
        }
        if (mem > r->max_mem - r->mem)
          return RAR_ENOMEM;
        L->ppm = ppmd_new((uint32_t)mem);
        if (!L->ppm)
          return RAR_ENOMEM;
        L->ppm_mem = (uint32_t)mem;
        r->mem += mem;
      }
      ppmd_init(L->ppm, order);
    } else if (!L->ppm)
      return RAR_ECORRUPT;
    ppmd_rar_rc_init(&L->rc, ppm_getbyte, b);
    L->ppm_active = true;
    return br_over(b) ? RAR_ECORRUPT : RAR_OK;
  }
  L->ppm_active = false;
  L->prev_low = 0;
  L->low_rep = 0;
  if (!(bf & 0x4000))
    memset(L->old_table, 0, sizeof L->old_table);
  br_skip(b, 2);
  for (unsigned i = 0; i < BC4;) {
    unsigned n = br_bits(b, 4);
    if (n == 15) {
      unsigned z = br_bits(b, 4);
      if (z == 0)
        bc_len[i++] = 15;
      else
        for (z += 2; z && i < BC4; z--)
          bc_len[i++] = 0;
    } else
      bc_len[i++] = (uint8_t)n;
  }
  if (!huff_build(&bc, bc_len, BC4))
    return RAR_ECORRUPT;
  for (unsigned i = 0; i < TABLE4;) {
    int sym = huff_decode(b, &bc);
    if (sym < 0 || br_over(b))
      return b->err ? b->err : RAR_ECORRUPT;
    if (sym < 16) {
      table[i] = (uint8_t)((sym + L->old_table[i]) & 0xf);
      i++;
      continue;
    }
    unsigned n = (sym & 1) ? 11 + br_bits(b, 7) : 3 + br_bits(b, 3);
    uint8_t v = 0;
    if (sym < 18) {
      if (i == 0)
        return RAR_ECORRUPT;
      v = table[i - 1];
    }
    for (; n && i < TABLE4; n--)
      table[i++] = v;
  }
  if (br_over(b))
    return RAR_ECORRUPT;
  if (!huff_build(&L->main, table, NC4) || !huff_build(&L->dist, table + NC4, DC4) ||
      !huff_build(&L->low, table + NC4 + DC4, LDC4) || !huff_build(&L->rep, table + NC4 + DC4 + LDC4, RC4))
    return RAR_ECORRUPT;
  memcpy(L->old_table, table, sizeof table);
  L->tables_read = true;
  return RAR_OK;
}

struct vmin {
  const uint8_t *p;
  uint32_t nbits, pos;
};

static uint32_t vm_peek16(struct vmin *v)
{
  uint32_t x = 0;
  for (int i = 0; i < 3; i++) {
    uint32_t at = (v->pos >> 3) + (uint32_t)i;
    x = (x << 8) | (at * 8 < v->nbits ? v->p[at] : 0);
  }
  return (x >> (8 - (v->pos & 7))) & 0xffff;
}

static uint32_t vm_read_data(struct vmin *v)
{
  uint32_t d = vm_peek16(v);
  switch (d & 0xc000) {
  case 0:
    v->pos += 6;
    return (d >> 10) & 0x3f;
  case 0x4000:
    if ((d & 0x3c00) == 0) {
      v->pos += 14;
      return 0xffffff00u | ((d >> 2) & 0xff);
    }
    v->pos += 10;
    return (d >> 6) & 0xff;
  case 0x8000:
    v->pos += 2;
    d = vm_peek16(v);
    v->pos += 16;
    return d;
  default: {
    v->pos += 2;
    uint32_t hi = vm_peek16(v);
    v->pos += 16;
    uint32_t lo = vm_peek16(v);
    v->pos += 16;
    return (hi << 16) | lo;
  }
  }
}

/* Host tests set this to accept an extra fingerprint as a standard filter. */
struct rar4_fp {
  uint32_t len, crc;
  uint8_t type;
};
struct rar4_fp rar4_extra_fp;

static int find_std(const uint8_t *code, uint32_t len)
{
  static const struct {
    uint32_t len, crc;
    uint8_t type;
  } std[] = {
      {53, 0xad576887u, FT_E8},    {57, 0x3cd7e57eu, FT_E8E9}, {120, 0x3769893fu, FT_ITANIUM},
      {29, 0x0e06077du, FT_DELTA}, {149, 0x1c2c5dc8u, FT_RGB}, {216, 0xbc85e701u, FT_AUDIO},
  };
  uint8_t x = 0;
  for (uint32_t i = 1; i < len; i++)
    x ^= code[i];
  if (len == 0 || x != code[0])
    return -1;
  uint32_t crc = crc32(0, code, len);
  for (unsigned i = 0; i < sizeof std / sizeof std[0]; i++)
    if (std[i].len == len && std[i].crc == crc)
      return std[i].type;
  if (rar4_extra_fp.len == len && rar4_extra_fp.crc == crc)
    return rar4_extra_fp.type;
  return -1;
}

static int add_vm_code(struct rar *r, uint32_t first, const uint8_t *code, uint32_t len)
{
  struct lz4 *L = &r->l4;
  struct vmin v = {code, len * 8, 0};
  uint32_t fp;
  if (first & 0x80) {
    fp = vm_read_data(&v);
    if (fp == 0) {
      L->nprogs = 0;
      L->last_prog = 0;
    } else
      fp--;
  } else
    fp = L->last_prog;
  if (fp > L->nprogs)
    return RAR_ECORRUPT;
  L->last_prog = fp;
  bool fresh = fp == L->nprogs;
  if (fresh) {
    if (L->nprogs >= MAX_PROGS)
      return RAR_ECORRUPT;
    if (L->nprogs == L->pcap) {
      unsigned nc = L->pcap ? L->pcap * 2 : 16;
      struct vmprog *np = rar_realloc(r, L->progs, (size_t)L->pcap * sizeof *np, (size_t)nc * sizeof *np);
      if (!np)
        return RAR_ENOMEM;
      L->progs = np;
      L->pcap = nc;
    }
    L->progs[fp].type = 0xff;
    L->progs[fp].old_len = 0;
    L->nprogs++;
  }
  uint32_t bstart = vm_read_data(&v);
  if (first & 0x40)
    bstart += 258;
  uint32_t blen = (first & 0x20) ? vm_read_data(&v) : L->progs[fp].old_len;
  L->progs[fp].old_len = blen;
  struct lzfilter nf;
  memset(&nf, 0, sizeof nf);
  if (first & 0x10) {
    uint32_t mask = vm_peek16(&v) >> 9;
    v.pos += 7;
    for (unsigned i = 0; i < 7; i++)
      if (mask & (1u << i))
        nf.r[i] = vm_read_data(&v);
  }
  if (fresh) {
    uint32_t vsize = vm_read_data(&v);
    if (vsize == 0 || vsize >= 0x10000 || v.pos + (uint64_t)vsize * 8 > v.nbits)
      return RAR_ECORRUPT;
    uint8_t *tmp = rar_alloc(r, vsize);
    if (!tmp)
      return RAR_ENOMEM;
    for (uint32_t i = 0; i < vsize; i++) {
      struct vmin w = {code, len * 8, v.pos + i * 8};
      tmp[i] = (uint8_t)(vm_peek16(&w) >> 8);
    }
    int t = find_std(tmp, vsize);
    rar_unalloc(r, tmp, vsize);
    if (t < 0)
      return RAR_EUNSUPPORTED;
    L->progs[fp].type = (uint8_t)t;
  }
  if (v.pos > v.nbits + 16)
    return RAR_ECORRUPT;
  nf.type = L->progs[fp].type;
  if (nf.type == 0xff)
    return RAR_ECORRUPT;
  nf.start = r->win.abs + bstart;
  nf.len = blen;
  nf.v5 = false;
  return lzw_add_filter(r, &nf);
}

static int do_copy(struct rar *r, uint64_t produced, uint64_t target, uint32_t len, uint32_t dist)
{
  struct lzwin *w = &r->win;
  if (len > target - produced)
    return RAR_ECORRUPT;
  int rc = lzw_reserve(r, len);
  if (rc)
    return rc;
  if (dist == 0 || dist > w->abs || dist > w->size)
    return RAR_ECORRUPT;
  lzw_copy(w, dist, len);
  return RAR_OK;
}

static void insert_dist(struct lz4 *L, uint32_t d)
{
  L->old_dist[3] = L->old_dist[2];
  L->old_dist[2] = L->old_dist[1];
  L->old_dist[1] = L->old_dist[0];
  L->old_dist[0] = d;
}

static int read_vm_lz(struct rar *r)
{
  struct bitr *b = &r->br;
  uint32_t first = br_bits(b, 8);
  uint32_t len = (first & 7) + 1;
  if (len == 7)
    len = br_bits(b, 8) + 7;
  else if (len == 8)
    len = br_bits(b, 16);
  if (len == 0)
    return RAR_ECORRUPT;
  uint8_t *code = rar_alloc(r, len);
  if (!code)
    return RAR_ENOMEM;
  for (uint32_t i = 0; i < len; i++)
    code[i] = (uint8_t)br_bits(b, 8);
  int rc = br_over(b) ? RAR_ECORRUPT : add_vm_code(r, first, code, len);
  rar_unalloc(r, code, len);
  return rc;
}

static int read_vm_ppm(struct rar *r)
{
  int first = ppm_char(r);
  if (first < 0)
    return RAR_ECORRUPT;
  uint32_t len = ((uint32_t)first & 7) + 1;
  if (len == 7) {
    int c = ppm_char(r);
    if (c < 0)
      return RAR_ECORRUPT;
    len = (uint32_t)c + 7;
  } else if (len == 8) {
    int c1 = ppm_char(r), c2 = ppm_char(r);
    if (c1 < 0 || c2 < 0)
      return RAR_ECORRUPT;
    len = (uint32_t)c1 * 256 + (uint32_t)c2;
  }
  if (len == 0)
    return RAR_ECORRUPT;
  uint8_t *code = rar_alloc(r, len);
  if (!code)
    return RAR_ENOMEM;
  int rc = RAR_OK;
  for (uint32_t i = 0; i < len; i++) {
    int c = ppm_char(r);
    if (c < 0) {
      rc = RAR_ECORRUPT;
      break;
    }
    code[i] = (uint8_t)c;
  }
  if (rc == RAR_OK)
    rc = add_vm_code(r, (uint32_t)first, code, len);
  rar_unalloc(r, code, len);
  return rc;
}

int rar4_decode(struct rar *r)
{
  struct rar_file *f = &r->cur;
  struct lz4 *L = &r->l4;
  struct bitr *b = &r->br;
  struct lzwin *w = &r->win;
  int rc;
  if (!tabs_ready)
    init_tabs();
  if (f->comp_solid) {
    if (!w->buf || r->broken)
      return RAR_ESTATE;
  } else {
    rc = lzw_init(r, WIN4, f->size);
    if (rc)
      return rc;
    L->tables_read = false;
    L->ppm_active = false;
    L->esc_char = 2;
    memset(L->old_dist, 0, sizeof L->old_dist);
    memset(L->old_table, 0, sizeof L->old_table);
    L->last_len = 0;
    L->prev_low = 0;
    L->low_rep = 0;
    L->nprogs = 0;
    L->last_prog = 0;
  }
  w->file_base = w->abs;
  uint64_t start_abs = w->abs, target = f->size;
  br_reset(b);
  if (!L->tables_read || !f->comp_solid) {
    rc = read_tables(r);
    if (rc)
      return b->err ? b->err : rc;
  }
  for (;;) {
    uint64_t produced = w->abs - start_abs;
    bool done = produced >= target;
    if (done && b->eof && b->pos >= b->len * 8)
      break;
    if (L->ppm_active) {
      int ch = ppm_char(r);
      if (ch < 0) {
        if (done)
          break;
        return b->err ? b->err : RAR_ECORRUPT;
      }
      if (ch == L->esc_char) {
        int nx = ppm_char(r);
        if (nx < 0)
          return done ? lzw_flush(r, true) : (b->err ? b->err : RAR_ECORRUPT);
        if (nx == 0) {
          rc = read_tables(r);
          if (rc)
            return b->err ? b->err : rc;
          continue;
        }
        if (nx == 2) {
          L->tables_read = false;
          break;
        }
        if (done)
          break;
        if (nx == 3) {
          rc = read_vm_ppm(r);
          if (rc)
            return rc;
          continue;
        }
        if (nx == 4) {
          uint32_t dist = 0, len = 4;
          for (int i = 0; i < 4; i++) {
            int c = ppm_char(r);
            if (c < 0)
              return b->err ? b->err : RAR_ECORRUPT;
            if (i == 3)
              len = (uint32_t)c;
            else
              dist = (dist << 8) + (uint32_t)c;
          }
          rc = do_copy(r, produced, target, len + 32, dist + 2);
          if (rc)
            return rc;
        } else if (nx == 5) {
          int len = ppm_char(r);
          if (len < 0)
            return b->err ? b->err : RAR_ECORRUPT;
          rc = do_copy(r, produced, target, (uint32_t)len + 4, 1);
          if (rc)
            return rc;
        } else {
          rc = lzw_reserve(r, 1);
          if (rc)
            return rc;
          lzw_put(w, (uint8_t)ch);
        }
      } else {
        if (done)
          break;
        rc = lzw_reserve(r, 1);
        if (rc)
          return rc;
        lzw_put(w, (uint8_t)ch);
      }
    } else {
      int sym = huff_decode(b, &L->main);
      if (sym < 0 || br_over(b))
        return b->err ? b->err : RAR_ECORRUPT;
      if (done && sym != 256)
        break;
      if (sym < 256) {
        rc = lzw_reserve(r, 1);
        if (rc)
          return rc;
        lzw_put(w, (uint8_t)sym);
      } else if (sym >= 271) {
        uint32_t len = lbase[sym - 271] + 3u;
        unsigned nb = lbits[sym - 271];
        if (nb)
          len += br_bits(b, nb);
        int ds = huff_decode(b, &L->dist);
        if (ds < 0)
          return b->err ? b->err : RAR_ECORRUPT;
        uint32_t dist = dbase[ds] + 1;
        nb = dbits[ds];
        if (nb) {
          if (ds > 9) {
            if (nb > 4)
              dist += br_bits(b, nb - 4) << 4;
            if (L->low_rep > 0) {
              L->low_rep--;
              dist += L->prev_low;
            } else {
              int ld = huff_decode(b, &L->low);
              if (ld < 0)
                return b->err ? b->err : RAR_ECORRUPT;
              if (ld == 16) {
                L->low_rep = LOW_DIST_REP - 1;
                dist += L->prev_low;
              } else {
                dist += (uint32_t)ld;
                L->prev_low = (uint32_t)ld;
              }
            }
          } else
            dist += br_bits(b, nb);
        }
        if (dist >= 0x2000) {
          len++;
          if (dist >= 0x40000)
            len++;
        }
        insert_dist(L, dist);
        L->last_len = len;
        rc = do_copy(r, produced, target, len, dist);
        if (rc)
          return rc;
      } else if (sym == 256) {
        uint32_t bf = br_peek(b, 2);
        bool new_table, new_file = false;
        if (bf & 2) {
          new_table = true;
          br_skip(b, 1);
        } else {
          new_file = true;
          new_table = (bf & 1) != 0;
          br_skip(b, 2);
        }
        L->tables_read = !new_table;
        if (new_file || done)
          break;
        if (new_table) {
          rc = read_tables(r);
          if (rc)
            return b->err ? b->err : rc;
        }
      } else if (sym == 257) {
        rc = read_vm_lz(r);
        if (rc)
          return rc;
      } else if (sym == 258) {
        if (L->last_len) {
          rc = do_copy(r, produced, target, L->last_len, L->old_dist[0]);
          if (rc)
            return rc;
        }
      } else if (sym < 263) {
        unsigned idx = (unsigned)sym - 259;
        uint32_t dist = L->old_dist[idx];
        for (unsigned i = idx; i > 0; i--)
          L->old_dist[i] = L->old_dist[i - 1];
        L->old_dist[0] = dist;
        int ls = huff_decode(b, &L->rep);
        if (ls < 0)
          return b->err ? b->err : RAR_ECORRUPT;
        uint32_t len = lbase[ls] + 2u;
        unsigned nb = lbits[ls];
        if (nb)
          len += br_bits(b, nb);
        L->last_len = len;
        rc = do_copy(r, produced, target, len, dist);
        if (rc)
          return rc;
      } else {
        unsigned idx = (unsigned)sym - 263;
        uint32_t dist = sdbase[idx] + 1u;
        if (sdbits[idx])
          dist += br_bits(b, sdbits[idx]);
        insert_dist(L, dist);
        L->last_len = 2;
        rc = do_copy(r, produced, target, 2, dist);
        if (rc)
          return rc;
      }
    }
    if (br_over(b) && !L->ppm_active)
      return b->err ? b->err : RAR_ECORRUPT;
    if (w->abs >= w->next_flush) {
      rc = lzw_flush(r, false);
      if (rc)
        return rc;
      w->next_flush = w->abs + 0x40000;
    }
  }
  if (b->err)
    return b->err;
  if (w->abs - start_abs != target)
    return RAR_ECORRUPT;
  return lzw_flush(r, true);
}
