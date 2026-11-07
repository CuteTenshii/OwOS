/* rar_ppmd_enc.h - test-only PPMd var.H encoder for RAR 3/4 blocks; included at the end of rar_test.c.
 * It compiles the library model with renamed symbols so it can drive the same update code. */
#define ppmd_new t_ppmd_new
#define ppmd_free t_ppmd_free
#define ppmd_init t_ppmd_init
#define ppmd_decode t_ppmd_decode
#define ppmd_rar_rc_init t_ppmd_rar_rc_init
#include "../user/lib/rar/ppmd.c"
#undef ppmd_new
#undef ppmd_free
#undef ppmd_init
#undef ppmd_decode
#undef ppmd_rar_rc_init

struct rcenc {
  uint32_t low, range;
  struct eb out;
};

static void rce_init(struct rcenc *c)
{
  c->low = 0;
  c->range = 0xFFFFFFFFu;
}
static void rce_encode(struct rcenc *c, uint32_t start, uint32_t size, uint32_t total)
{
  c->range /= total;
  c->low += c->range * start;
  c->range *= size;
  for (;;) {
    if ((c->low ^ (c->low + c->range)) >= RC_TOP) {
      if (c->range >= RC_BOT)
        break;
      c->range = (0 - c->low) & (RC_BOT - 1);
    }
    eb_u8(&c->out, c->low >> 24);
    c->range <<= 8;
    c->low <<= 8;
  }
}
static void rce_flush(struct rcenc *c)
{
  for (int i = 0; i < 4; i++) {
    eb_u8(&c->out, c->low >> 24);
    c->low <<= 8;
  }
}

static void ppm_encode_symbol(struct ppmd *p, struct rcenc *rc, int symbol)
{
  int8_t mask[256];
  if (p->min_ctx->num != 1) {
    struct pstate *s = STATS(p->min_ctx);
    uint32_t sum;
    unsigned i;
    if (s->sym == symbol) {
      rce_encode(rc, 0, s->freq, p->min_ctx->summ);
      p->found = s;
      update1_0(p);
      return;
    }
    p->prev_success = 0;
    sum = s->freq;
    i = p->min_ctx->num - 1u;
    do {
      if ((++s)->sym == symbol) {
        rce_encode(rc, sum, s->freq, p->min_ctx->summ);
        p->found = s;
        update1(p);
        return;
      }
      sum += s->freq;
    } while (--i);
    p->hi_bits_flag = p->hb2flag[p->found->sym];
    memset(mask, -1, sizeof mask);
    mask[s->sym] = 0;
    i = p->min_ctx->num - 1u;
    do {
      mask[(--s)->sym] = 0;
    } while (--i);
    rce_encode(rc, sum, p->min_ctx->summ - sum, p->min_ctx->summ);
  } else {
    p->hi_bits_flag = p->hb2flag[p->found->sym];
    uint16_t *prob = &p->bin_summ[ONE(p->min_ctx)->freq - 1][p->prev_success + p->ns2bs[SUFFIX(p->min_ctx)->num - 1] +
                                                             p->hi_bits_flag + 2 * p->hb2flag[ONE(p->min_ctx)->sym] +
                                                             ((p->run_length >> 26) & 0x20)];
    struct pstate *s = ONE(p->min_ctx);
    if (s->sym == symbol) {
      rce_encode(rc, 0, *prob, BIN_SCALE);
      *prob = (uint16_t)(*prob + (1 << INT_BITS) - MEAN(*prob));
      p->found = s;
      update_bin(p);
      return;
    }
    rce_encode(rc, *prob, BIN_SCALE - *prob, BIN_SCALE);
    *prob = (uint16_t)(*prob - MEAN(*prob));
    p->init_esc = exp_escape[*prob >> 10];
    memset(mask, -1, sizeof mask);
    mask[s->sym] = 0;
    p->prev_success = 0;
  }
  for (;;) {
    uint32_t esc_freq, sum;
    unsigned i, num_masked = p->min_ctx->num;
    do {
      p->order_fall++;
      if (!p->min_ctx->suffix)
        return;
      p->min_ctx = SUFFIX(p->min_ctx);
    } while (p->min_ctx->num == num_masked);
    struct psee *see = make_esc_freq(p, num_masked, &esc_freq);
    struct pstate *s = STATS(p->min_ctx);
    sum = 0;
    i = p->min_ctx->num;
    do {
      int cur = s->sym;
      if (cur == symbol) {
        uint32_t low = sum;
        struct pstate *s1 = s;
        do {
          sum += (s->freq & (unsigned)mask[s->sym]);
          s++;
        } while (--i);
        rce_encode(rc, low, s1->freq, sum + esc_freq);
        see_update(see);
        p->found = s1;
        update2(p);
        return;
      }
      sum += (s->freq & (unsigned)mask[cur]);
      mask[cur] = 0;
      s++;
    } while (--i);
    rce_encode(rc, sum, esc_freq, sum + esc_freq);
    see->summ = (uint16_t)(see->summ + sum + esc_freq);
  }
}

static void ppm_header(struct eb *out, const struct ppm_cfg *c, bool reset)
{
  eb_u8(out, 0x80 | (reset ? 0x20 : 0) | (c->esc >= 0 ? 0x40 : 0) |
                 ((c->order <= 16 ? (unsigned)c->order : 16 + ((unsigned)c->order - 16) / 3) - 1));
  if (reset)
    eb_u8(out, (unsigned)c->mem_mb - 1);
  if (c->esc >= 0)
    eb_u8(out, (unsigned)c->esc);
}

static void ppm_encode_stream(const uint8_t *d, size_t n, const struct ppm_cfg *c, struct eb *out)
{
  int esc = c->esc >= 0 ? c->esc : 2;
  struct ppmd *p = t_ppmd_new((uint32_t)c->mem_mb << 20);
  t_ppmd_init(p, (unsigned)c->order);
  struct rcenc rc = {0};
  rce_init(&rc);
  ppm_header(out, c, true);
  struct mf m;
  mf_init(&m, d, n, 0);
  size_t half = c->two_blocks ? n / 2 : n + 1;
  bool second = false;
  for (size_t pos = 0; pos < n;) {
    mf_insert_to(&m, pos);
    if (!second && pos >= half) {
      ppm_encode_symbol(p, &rc, esc);
      ppm_encode_symbol(p, &rc, 0);
      rce_flush(&rc);
      eb_put(out, rc.out.p, rc.out.n);
      eb_free(&rc.out);
      rce_init(&rc);
      ppm_header(out, c, false);
      second = true;
    }
    size_t run = 1;
    while (pos + run < n && run < 259 && pos > 0 && d[pos + run] == d[pos - 1] && d[pos] == d[pos - 1])
      run++;
    uint32_t nd = 0;
    size_t nl = c->matches ? mf_best(&m, pos, n - pos < 287 ? n - pos : 287, 0x3fffff, &nd) : 0;
    if (c->matches && pos > 0 && d[pos] == d[pos - 1] && run >= 6) {
      ppm_encode_symbol(p, &rc, esc);
      ppm_encode_symbol(p, &rc, 5);
      ppm_encode_symbol(p, &rc, (int)run - 4);
      pos += run;
    } else if (nl >= 40 && nd >= 2 && !(!second && pos < half && pos + nl > half && c->two_blocks)) {
      ppm_encode_symbol(p, &rc, esc);
      ppm_encode_symbol(p, &rc, 4);
      uint32_t dv = nd - 2;
      ppm_encode_symbol(p, &rc, (int)(dv >> 16) & 255);
      ppm_encode_symbol(p, &rc, (int)(dv >> 8) & 255);
      ppm_encode_symbol(p, &rc, (int)dv & 255);
      ppm_encode_symbol(p, &rc, (int)nl - 32);
      pos += nl;
    } else {
      ppm_encode_symbol(p, &rc, d[pos]);
      if (d[pos] == esc)
        ppm_encode_symbol(p, &rc, 1);
      pos++;
    }
  }
  ppm_encode_symbol(p, &rc, esc);
  ppm_encode_symbol(p, &rc, 2);
  rce_flush(&rc);
  eb_put(out, rc.out.p, rc.out.n);
  eb_free(&rc.out);
  free(m.head);
  free(m.prev);
  t_ppmd_free(p);
}
