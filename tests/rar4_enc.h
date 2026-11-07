/* rar4_enc.h - test-only RAR 3.x/4.x writer (stored, LZ, PPMd, encrypted, volumes); see rar_enc.h. */
#ifndef RAR4_ENC_H
#define RAR4_ENC_H

#include "rar_enc.h"

#define E4_NC 299
#define E4_DC 60
#define E4_LDC 17
#define E4_RC 28
#define E4_TOTAL (E4_NC + E4_DC + E4_LDC + E4_RC)

static const uint8_t e4_lbits[28] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
                                     2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5};
static const uint16_t e4_lbase[28] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10,  12,  14,  16,  20,
                                      24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224};
static const uint8_t e4_sdbase[8] = {0, 4, 8, 16, 32, 64, 128, 192};
static const uint8_t e4_sdbits[8] = {2, 2, 3, 4, 5, 6, 6, 6};

static uint32_t e4_dbase[E4_DC];
static uint8_t e4_dbits[E4_DC];

static void e4_init(void)
{
  static bool done;
  if (done)
    return;
  done = true;
  int k = 0;
  uint32_t base = 0;
  for (int bits = 0; bits <= 18; bits++) {
    int count = bits == 0 ? 4 : bits <= 15 ? 2 : bits == 16 ? 14 : bits == 17 ? 0 : 12;
    for (int i = 0; i < count; i++, k++) {
      e4_dbase[k] = base;
      e4_dbits[k] = (uint8_t)bits;
      base += 1u << bits;
    }
  }
}

struct r4st {
  uint32_t old_dist[4], last_len, prev_low;
  int low_rep;
  uint8_t old_table[E4_TOTAL];
  bool tables_valid;
  struct eb hist;
  unsigned counter;
  bool no16;
};

struct r4f {
  const char *name;
  const uint8_t *data;
  size_t len;
  int method; /* 0 store, else LZ */
  bool solid, is_dir, unicode, universal, end_keep;
  const uint8_t *raw_name;
  size_t raw_name_len;
  uint32_t dos_time, attr;
  int host;
  int block_tokens;
  const char *password;
  bool ppm;
  int ppm_order, ppm_mem_mb;
  bool ppm_reset;
  int unp_ver;
  const uint8_t *comp;      /* bytes to compress when filtered (data holds the original) */
  const struct ops *prefix; /* ops emitted before the data (VM records) */
  bool no16;                /* never use the low-distance repeat code */
  bool part_crc;            /* header CRC is the CRC of the packed part (non-final volumes) */
};

static unsigned e4_bonus(uint32_t d) { return d >= 0x2000 ? 1 + (d >= 0x40000) : 0; }

static unsigned e4_lslot(uint32_t x)
{
  unsigned j = 27;
  while (e4_lbase[j] > x)
    j--;
  return j;
}

static void e4_tokenize(struct r4st *st, const uint8_t *data, size_t len, struct ops *o, int block_tokens,
                        bool new_table)
{
  e4_init();
  size_t base = st->hist.n, total = base + len;
  uint8_t *buf = malloc(total + 8);
  memcpy(buf, st->hist.p, base);
  memcpy(buf + base, data, len);
  struct mf m;
  mf_init(&m, buf, total, base);
  int ntok = 0;
  if (new_table)
    st->prev_low = st->low_rep = 0;
  for (size_t pos = base; pos < total;) {
    mf_insert_to(&m, pos);
    size_t lim = total - pos;
    if (lim > 257)
      lim = 257;
    size_t rl = 0;
    int ri = -1;
    for (int k = 0; k < 4; k++) {
      uint32_t d = st->old_dist[k];
      if (d && d <= pos) {
        size_t l = common_len(buf + pos, buf + pos - d, lim);
        if (l > rl) {
          rl = l;
          ri = k;
        }
      }
    }
    uint32_t nd = 0;
    size_t nl = mf_best(&m, pos, lim, 0x3fffff, &nd);
    bool normal_ok = false;
    uint32_t ds = 0, extra = 0;
    if (nl) {
      unsigned bonus = e4_bonus(nd);
      if (nl < 3 + bonus)
        nl = 0;
      else {
        if (nl > 258 - 1)
          nl = 257;
        uint32_t y = nd - 1;
        ds = 59;
        while (e4_dbase[ds] > y)
          ds--;
        extra = y - e4_dbase[ds];
        normal_ok = !(e4_dbits[ds] && ds > 9 && st->low_rep > 0 && (extra & 15) != st->prev_low);
      }
    }
    size_t adv;
    if (rl >= 3 && (rl + 1 >= nl || !normal_ok)) {
      if (ri == 0 && rl == st->last_len)
        op_add(o, A_MAIN, 258, 0, 0);
      else {
        op_add(o, A_MAIN, 259 + (unsigned)ri, 0, 0);
        unsigned j = e4_lslot((uint32_t)rl - 2);
        op_add(o, A_REP, j, 0, 0);
        if (e4_lbits[j])
          op_add(o, A_RAW, 0, e4_lbits[j], ((uint32_t)rl - 2) - e4_lbase[j]);
        uint32_t d = st->old_dist[ri];
        for (int k = ri; k > 0; k--)
          st->old_dist[k] = st->old_dist[k - 1];
        st->old_dist[0] = d;
        st->last_len = (uint32_t)rl;
      }
      adv = rl;
    } else if (normal_ok) {
      unsigned bonus = e4_bonus(nd);
      uint32_t lp = (uint32_t)nl - bonus;
      unsigned j = e4_lslot(lp - 3);
      op_add(o, A_MAIN, 271 + j, 0, 0);
      if (e4_lbits[j])
        op_add(o, A_RAW, 0, e4_lbits[j], (lp - 3) - e4_lbase[j]);
      op_add(o, A_DIST, ds, 0, 0);
      unsigned nb = e4_dbits[ds];
      if (nb) {
        if (ds > 9) {
          if (nb > 4)
            op_add(o, A_RAW, 0, nb - 4, extra >> 4);
          uint32_t low = extra & 15;
          if (st->low_rep > 0)
            st->low_rep--;
          else if (low == st->prev_low && !st->no16 && (st->counter++ & 1)) {
            op_add(o, A_LOW, 16, 0, 0);
            st->low_rep = 15;
          } else {
            op_add(o, A_LOW, low, 0, 0);
            st->prev_low = low;
          }
        } else
          op_add(o, A_RAW, 0, nb, extra);
      }
      for (int k = 3; k > 0; k--)
        st->old_dist[k] = st->old_dist[k - 1];
      st->old_dist[0] = nd;
      st->last_len = (uint32_t)nl;
      adv = nl;
    } else {
      uint32_t sd = 0;
      if (pos + 2 <= total)
        for (uint32_t d = 1; d <= 256 && d <= pos; d++)
          if (buf[pos] == buf[pos - d] && buf[pos + 1] == buf[pos - d + 1]) {
            sd = d;
            break;
          }
      if (sd && (st->counter++ % 3 == 0)) {
        uint32_t y = sd - 1;
        unsigned k = 7;
        while (e4_sdbase[k] > y)
          k--;
        op_add(o, A_MAIN, 263 + k, 0, 0);
        op_add(o, A_RAW, 0, e4_sdbits[k], y - e4_sdbase[k]);
        for (int q = 3; q > 0; q--)
          st->old_dist[q] = st->old_dist[q - 1];
        st->old_dist[0] = sd;
        st->last_len = 2;
        adv = 2;
      } else {
        op_add(o, A_MAIN, buf[pos], 0, 0);
        adv = 1;
      }
    }
    pos += adv;
    if (block_tokens && ++ntok % block_tokens == 0 && pos < total) {
      op_split(o);
      st->prev_low = st->low_rep = 0;
    }
  }
  free(m.head);
  free(m.prev);
  free(buf);
  eb_put(&st->hist, data, len);
}

static void e4_universal(uint8_t *lens)
{
  memset(lens, 9, E4_NC);
  memset(lens + E4_NC, 6, E4_DC);
  memset(lens + E4_NC + E4_DC, 5, E4_LDC);
  memset(lens + E4_NC + E4_DC + E4_LDC, 5, E4_RC);
}

/* table: 2 flag bits then BC nibbles and the delta-coded lengths */
static void e4_write_table(struct bw *w, const uint8_t *lens, const uint8_t *old, bool keep)
{
  bw_put(w, 0, 1);
  bw_put(w, keep ? 1 : 0, 1);
  const uint8_t *o;
  uint8_t zero[E4_TOTAL];
  memset(zero, 0, sizeof zero);
  o = keep ? old : zero;
  struct {
    uint8_t sym, xb;
    uint32_t xv;
  } t[E4_TOTAL];
  int nt = 0;
  for (int i = 0; i < E4_TOTAL;) {
    int run = 1;
    if (lens[i] == 0) {
      while (i + run < E4_TOTAL && lens[i + run] == 0 && run < 138)
        run++;
      if (run >= 3) {
        t[nt++] = run >= 11 ? (typeof(t[0])){19, 7, (uint32_t)run - 11} : (typeof(t[0])){18, 3, (uint32_t)run - 3};
        i += run;
        continue;
      }
    } else if (i > 0 && lens[i] == lens[i - 1]) {
      while (i + run < E4_TOTAL && lens[i + run] == lens[i] && run < 138)
        run++;
      if (run >= 3) {
        t[nt++] = run >= 11 ? (typeof(t[0])){17, 7, (uint32_t)run - 11} : (typeof(t[0])){16, 3, (uint32_t)run - 3};
        i += run;
        continue;
      }
    }
    t[nt++] = (typeof(t[0])){(uint8_t)((lens[i] - o[i]) & 15), 0, 0};
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

static void e4_block_lens(const struct op *p, size_t n, uint8_t *lens)
{
  uint32_t f[4][E4_NC] = {{0}};
  for (size_t i = 0; i < n; i++)
    if (p[i].alpha != A_RAW)
      f[p[i].alpha][p[i].sym]++;
  f[0][256]++;
  hb_lens(f[0], E4_NC, lens);
  hb_lens(f[1], E4_DC, lens + E4_NC);
  hb_lens(f[2], E4_LDC, lens + E4_NC + E4_DC);
  hb_lens(f[3], E4_RC, lens + E4_NC + E4_DC + E4_LDC);
}

/* LZ stream for one file. `need_table` is false when the decoder keeps the previous file's tables. */
static void e4_write_blocks(struct bw *w, struct r4st *st, const struct ops *o, bool universal, bool need_table,
                            bool end_keep, unsigned *rng)
{
  size_t nb = o->nsplits + 1;
  uint8_t lens[E4_TOTAL];
  memcpy(lens, st->old_table, sizeof lens);
  for (size_t b = 0; b < nb; b++) {
    size_t from = b ? o->splits[b - 1] : 0, to = b < o->nsplits ? o->splits[b] : o->n;
    bool table = b > 0 || need_table;
    if (table) {
      if (universal)
        e4_universal(lens);
      else
        e4_block_lens(o->p + from, to - from, lens);
      bool keep = st->tables_valid && (enc_rng(rng) & 1);
      bw_align(w);
      e4_write_table(w, lens, st->old_table, keep);
      memcpy(st->old_table, lens, sizeof lens);
      st->tables_valid = true;
    }
    uint32_t codes[E4_TOTAL];
    hb_codes(lens, E4_NC, codes);
    hb_codes(lens + E4_NC, E4_DC, codes + E4_NC);
    hb_codes(lens + E4_NC + E4_DC, E4_LDC, codes + E4_NC + E4_DC);
    hb_codes(lens + E4_NC + E4_DC + E4_LDC, E4_RC, codes + E4_NC + E4_DC + E4_LDC);
    static const int off[4] = {0, E4_NC, E4_NC + E4_DC, E4_NC + E4_DC + E4_LDC};
    for (size_t i = from; i < to; i++) {
      const struct op *p = &o->p[i];
      if (p->alpha == A_RAW)
        bw_put(w, p->val, p->nbits);
      else
        bw_put(w, codes[off[p->alpha] + p->sym], lens[off[p->alpha] + p->sym]);
    }
    bw_put(w, codes[256], lens[256]);
    if (b + 1 < nb) {
      bw_put(w, 1, 1);
    } else {
      bw_put(w, 0, 1);
      bw_put(w, end_keep ? 0 : 1, 1);
    }
  }
}

/* VM record: bit-packed ReadData values behind a flag byte, as ops for the LZ stream */
struct vmw {
  struct bw w;
};

static void vm_data(struct vmw *v, uint32_t x)
{
  if (x < 16) {
    bw_put(&v->w, 0, 2);
    bw_put(&v->w, x, 4);
  } else if (x < 256) {
    bw_put(&v->w, 1, 2);
    bw_put(&v->w, x, 8);
  } else if ((x & 0xffffff00u) == 0xffffff00u) {
    bw_put(&v->w, 1, 2);
    bw_put(&v->w, 0, 4);
    bw_put(&v->w, x & 255, 8);
  } else if (x < 0x10000) {
    bw_put(&v->w, 2, 2);
    bw_put(&v->w, x, 16);
  } else {
    bw_put(&v->w, 3, 2);
    bw_put(&v->w, x >> 16, 16);
    bw_put(&v->w, x & 0xffff, 16);
  }
}

/* flags: 0x80 explicit program index (prog+1), 0x40 start +258, 0x20 explicit length, 0x10 register mask */
static void vm_record(struct ops *o, unsigned flags, uint32_t prog, uint32_t start, uint32_t len, unsigned regmask,
                      const uint32_t *regs, const uint8_t *code, size_t codelen)
{
  struct vmw v = {{{0}, 0}};
  if (flags & 0x80)
    vm_data(&v, prog + 1);
  vm_data(&v, (flags & 0x40) ? start - 258 : start);
  if (flags & 0x20)
    vm_data(&v, len);
  if (flags & 0x10) {
    bw_put(&v.w, regmask, 7);
    for (int i = 0; i < 7; i++)
      if (regmask & (1u << i))
        vm_data(&v, regs[i]);
  }
  if (code) {
    vm_data(&v, (uint32_t)codelen);
    for (size_t i = 0; i < codelen; i++)
      bw_put(&v.w, code[i], 8);
  }
  size_t plen = v.w.b.n;
  unsigned low3 = plen <= 6 ? (unsigned)plen - 1 : plen <= 262 ? 6 : 7;
  op_add(o, A_MAIN, 257, 0, 0);
  op_add(o, A_RAW, 0, 8, (flags & 0xf8) | low3);
  if (plen > 6 && plen <= 262)
    op_add(o, A_RAW, 0, 8, (uint32_t)plen - 7);
  else if (plen > 262)
    op_add(o, A_RAW, 0, 16, (uint32_t)plen);
  for (size_t i = 0; i < plen; i++)
    op_add(o, A_RAW, 0, 8, v.w.b.p[i]);
  eb_free(&v.w.b);
}

#ifdef RAR_ENC_CRYPTO
static void host_rar3_kdf(const char *pw, const uint8_t salt[8], uint8_t key[16], uint8_t iv[16])
{
  uint8_t raw[512];
  size_t rl = 0;
  for (const char *p = pw; *p && rl < 500; p++) {
    raw[rl++] = (uint8_t)*p;
    raw[rl++] = 0;
  }
  CC_SHA1_CTX c;
  CC_SHA1_Init(&c);
  for (uint32_t i = 0; i < 0x40000; i++) {
    CC_SHA1_Update(&c, raw, (CC_LONG)rl);
    CC_SHA1_Update(&c, salt, 8);
    uint8_t cnt[3] = {(uint8_t)i, (uint8_t)(i >> 8), (uint8_t)(i >> 16)};
    CC_SHA1_Update(&c, cnt, 3);
    if (i % 0x4000 == 0) {
      CC_SHA1_CTX t = c;
      uint8_t d[20];
      CC_SHA1_Final(d, &t);
      iv[i / 0x4000] = d[19];
    }
  }
  uint8_t d[20];
  CC_SHA1_Final(d, &c);
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      key[i * 4 + j] = d[i * 4 + 3 - j];
}
#endif

struct r4a {
  struct eb out;
  struct r4st st;
  bool hdr_enc;
  uint8_t hsalt[8], hkey[16], hiv[16];
  unsigned rng;
};

static void r4a_block(struct r4a *a, unsigned type, unsigned flags, const struct eb *body)
{
  struct eb h = {0};
  eb_le16(&h, 0);
  eb_u8(&h, type);
  eb_le16(&h, flags);
  eb_le16(&h, 7 + (body ? body->n : 0));
  if (body)
    eb_put(&h, body->p, body->n);
  uint32_t crc = crc32(0, h.p + 2, (uint32_t)h.n - 2) & 0xffff;
  h.p[0] = (uint8_t)crc;
  h.p[1] = (uint8_t)(crc >> 8);
  if (a->hdr_enc && type != 0x73) {
#ifdef RAR_ENC_CRYPTO
    size_t padded = (h.n + 15) & ~(size_t)15;
    h.p = realloc(h.p, padded);
    memset(h.p + h.n, 0, padded - h.n);
    uint8_t *ct = malloc(padded);
    host_aes_cbc(1, a->hkey, 16, a->hiv, h.p, ct, padded);
    eb_put(&a->out, a->hsalt, 8);
    eb_put(&a->out, ct, padded);
    free(ct);
#endif
  } else
    eb_put(&a->out, h.p, h.n);
  eb_free(&h);
}

static void r4a_begin(struct r4a *a, unsigned main_flags, const char *hdr_password)
{
  static const uint8_t sig[7] = {0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00};
  eb_put(&a->out, sig, 7);
  a->rng = a->rng ? a->rng : 777;
  struct eb body = {0};
  for (int i = 0; i < 6; i++)
    eb_u8(&body, 0);
#ifdef RAR_ENC_CRYPTO
  if (hdr_password) {
    main_flags |= 0x80;
    enc_rand(&a->rng, a->hsalt, 8);
    host_rar3_kdf(hdr_password, a->hsalt, a->hkey, a->hiv);
  }
#else
  (void)hdr_password;
#endif
  r4a_block(a, 0x73, main_flags, &body);
  eb_free(&body);
  a->hdr_enc = hdr_password != NULL;
}

static void r4a_end(struct r4a *a, bool more) { r4a_block(a, 0x7b, more ? 0x4001 : 0x4000, NULL); }

static void e4_utf8_to_enc(const char *name, struct eb *out)
{
  uint16_t w[512];
  int n = 0;
  for (const unsigned char *p = (const unsigned char *)name; *p && n < 500;) {
    uint32_t cp = *p++;
    if (cp >= 0xc0) {
      int ex = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : 1;
      cp &= 0x3f >> ex;
      while (ex--)
        cp = (cp << 6) | (*p++ & 0x3f);
    }
    w[n++] = (uint16_t)cp;
  }
  for (int i = 0; i < n; i++)
    eb_u8(out, w[i] < 256 ? w[i] : '_');
  eb_u8(out, 0);
  eb_u8(out, 0);
  for (int i = 0; i < n; i += 4) {
    unsigned flags = 0;
    int k = i + 4 < n ? 4 : n - i;
    for (int j = 0; j < 4; j++)
      flags = (flags << 2) | (j < k ? (w[i + j] < 256 ? 0u : 2u) : 0u);
    eb_u8(out, flags);
    for (int j = 0; j < k; j++) {
      if (w[i + j] < 256)
        eb_u8(out, w[i + j]);
      else
        eb_le16(out, w[i + j]);
    }
  }
}

static void r4_pack_lz(struct r4a *a, const struct r4f *f, struct eb *packed, bool need_table)
{
  struct ops o = {0};
  a->st.no16 = f->no16;
  if (f->prefix)
    for (size_t i = 0; i < f->prefix->n; i++)
      op_add(&o, f->prefix->p[i].alpha, f->prefix->p[i].sym, f->prefix->p[i].nbits, f->prefix->p[i].val);
  e4_tokenize(&a->st, f->comp ? f->comp : f->data, f->len, &o, f->block_tokens, need_table);
  struct bw w = {{0}, 0};
  e4_write_blocks(&w, &a->st, &o, f->universal, need_table, f->end_keep, &a->rng);
  bw_put(&w, 0, 32);
  eb_put(packed, w.b.p, w.b.n);
  eb_free(&w.b);
  ops_free(&o);
}

/* split_before/after select volume flags; crc covers the whole file. */
static void r4a_put_file(struct r4a *a, const struct r4f *f, const uint8_t *packed, size_t plen, bool sb, bool sa)
{
  struct eb body = {0}, data = {0}, name = {0};
  uint8_t salt[8] = {0};
  bool enc = f->password != NULL && plen > 0;
#ifdef RAR_ENC_CRYPTO
  if (enc) {
    uint8_t key[16], iv[16];
    enc_rand(&a->rng, salt, 8);
    host_rar3_kdf(f->password, salt, key, iv);
    size_t padded = (plen + 15) & ~(size_t)15;
    uint8_t *pt = calloc(padded, 1), *ct = malloc(padded);
    memcpy(pt, packed, plen);
    host_aes_cbc(1, key, 16, iv, pt, ct, padded);
    eb_put(&data, ct, padded);
    free(pt);
    free(ct);
  } else
#endif
    eb_put(&data, packed, plen);
  (void)salt;
  unsigned flags = 0x8000 | (sb ? 1 : 0) | (sa ? 2 : 0) | (enc ? 0x4 | 0x400 : 0) | (f->solid ? 0x10 : 0);
  if (f->raw_name) {
    eb_put(&name, f->raw_name, f->raw_name_len);
    flags |= 0x200;
  } else if (f->unicode) {
    e4_utf8_to_enc(f->name, &name);
    flags |= 0x200;
  } else
    eb_put(&name, f->name, strlen(f->name));
  for (size_t i = 0; i < name.n; i++)
    if (name.p[i] == '/')
      name.p[i] = '\\';
  flags |= f->is_dir ? 0xe0 : 6 << 5;
  eb_le32(&body, (uint32_t)data.n);
  eb_le32(&body, (uint32_t)f->len);
  eb_u8(&body, f->host ? f->host : 3);
  eb_le32(&body, f->part_crc ? crc32(0, packed, (uint32_t)plen) : crc32(0, f->data, (uint32_t)f->len));
  eb_le32(&body, f->dos_time ? f->dos_time : ((2023 - 1980) << 25 | 5 << 21 | 17 << 16 | 12 << 11 | 30 << 5 | 7));
  eb_u8(&body, f->unp_ver ? f->unp_ver : 29);
  eb_u8(&body, 0x30 + f->method);
  eb_le16(&body, (uint32_t)name.n);
  eb_le32(&body, f->attr ? f->attr : f->is_dir ? 040755 : 0100644);
  eb_put(&body, name.p, name.n);
  if (enc)
    eb_put(&body, salt, 8);
  r4a_block(a, 0x74, flags, &body);
  eb_put(&a->out, data.p, data.n);
  eb_free(&body);
  eb_free(&data);
  eb_free(&name);
}

static void r4a_add(struct r4a *a, const struct r4f *f, bool next_solid_reuses_tables)
{
  struct eb packed = {0};
  if (!f->is_dir && f->len) {
    if (f->method == 0)
      eb_put(&packed, f->data, f->len);
    else {
      bool first_needs = true;
      if (!f->solid) {
        memset(&a->st.old_dist, 0, sizeof a->st.old_dist);
        a->st.last_len = a->st.prev_low = 0;
        a->st.low_rep = 0;
        memset(a->st.old_table, 0, sizeof a->st.old_table);
        a->st.tables_valid = false;
        a->st.hist.n = 0;
      } else if (a->st.tables_valid && f->universal)
        first_needs = false;
      (void)next_solid_reuses_tables;
      r4_pack_lz(a, f, &packed, first_needs);
    }
  }
  r4a_put_file(a, f, packed.p, packed.n, false, false);
  eb_free(&packed);
}

#endif /* RAR4_ENC_H */
