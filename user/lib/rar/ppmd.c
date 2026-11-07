/* ppmd.c - PPMd variant H model as used by RAR 3.x/4.x, with a pluggable range coder (RAR or 7z). */
#include "owos.h"
#include "rar_int.h"

#define MAX_ORDER 64
#define MAX_FREQ 124
#define UNIT_SIZE 12
#define N_INDEXES 38
#define INT_BITS 7
#define PERIOD_BITS 7
#define BIN_SCALE (1u << (INT_BITS + PERIOD_BITS))

struct pstate {
  uint8_t sym, freq;
  uint16_t slo, shi;
} __attribute__((packed));

struct pctx {
  uint16_t num, summ;
  uint32_t stats, suffix;
} __attribute__((packed));

struct pnode {
  uint16_t stamp, nu;
  uint32_t next, prev;
} __attribute__((packed));

struct psee {
  uint16_t summ;
  uint8_t shift, count;
};

struct ppmd {
  struct pctx *min_ctx, *max_ctx;
  struct pstate *found;
  unsigned order_fall, init_esc, prev_success, max_order, hi_bits_flag;
  int32_t run_length, init_rl;
  uint32_t size, glue_count;
  uint8_t *base, *lo_unit, *hi_unit, *text, *units_start;
  uint32_t align;
  uint8_t i2u[N_INDEXES], u2i[128], ns2indx[256], ns2bs[256], hb2flag[256];
  struct psee dummy_see, see[25][16];
  uint16_t bin_summ[128][64];
  uint32_t free_list[N_INDEXES];
};

static const uint16_t init_bin_esc[8] = {0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051};
static const uint8_t exp_escape[16] = {25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2};

#define CTX(ref) ((struct pctx *)(p->base + (ref)))
#define REF(ptr) ((uint32_t)((uint8_t *)(ptr) - p->base))
#define STATS(c) ((struct pstate *)(p->base + (c)->stats))
#define SUFFIX(c) CTX((c)->suffix)
#define ONE(c) ((struct pstate *)&(c)->summ)
#define NODE(ref) ((struct pnode *)(p->base + (ref)))
#define I2U(i) ((unsigned)p->i2u[i])
#define U2I(nu) ((unsigned)p->u2i[(nu) - 1])
#define U2B(nu) ((uint32_t)(nu) * UNIT_SIZE)

static uint32_t succ(const struct pstate *s) { return (uint32_t)s->slo | ((uint32_t)s->shi << 16); }

static void set_succ(struct pstate *s, uint32_t v)
{
  s->slo = (uint16_t)v;
  s->shi = (uint16_t)(v >> 16);
}

struct ppmd *ppmd_new(uint32_t mem)
{
  if (mem < (1u << 11) || mem > 0xFFFFFFFFu - 12 * 3)
    return NULL;
  struct ppmd *p = calloc(1, sizeof *p);
  if (!p)
    return NULL;
  p->align = 4 - (mem & 3);
  p->size = mem;
  p->base = calloc(1, (size_t)p->align + mem + UNIT_SIZE);
  if (!p->base) {
    free(p);
    return NULL;
  }
  unsigned k = 0;
  for (unsigned i = 0; i < N_INDEXES; i++) {
    unsigned step = i >= 12 ? 4 : (i >> 2) + 1;
    do {
      p->u2i[k++] = (uint8_t)i;
    } while (--step);
    p->i2u[i] = (uint8_t)k;
  }
  p->ns2bs[0] = 0;
  p->ns2bs[1] = 2;
  memset(p->ns2bs + 2, 4, 9);
  memset(p->ns2bs + 11, 6, 256 - 11);
  unsigned i;
  for (i = 0; i < 3; i++)
    p->ns2indx[i] = (uint8_t)i;
  unsigned m = i;
  k = 1;
  for (; i < 256; i++) {
    p->ns2indx[i] = (uint8_t)m;
    if (--k == 0)
      k = (++m) - 2;
  }
  memset(p->hb2flag, 0, 0x40);
  memset(p->hb2flag + 0x40, 8, 0x100 - 0x40);
  return p;
}

void ppmd_free(struct ppmd *p)
{
  if (p) {
    free(p->base);
    free(p);
  }
}

static void restart_model(struct ppmd *p)
{
  memset(p->free_list, 0, sizeof p->free_list);
  p->text = p->base + p->align;
  p->hi_unit = p->text + p->size;
  p->lo_unit = p->units_start = p->hi_unit - p->size / 8 / UNIT_SIZE * 7 * UNIT_SIZE;
  p->glue_count = 0;
  p->order_fall = p->max_order;
  p->run_length = p->init_rl = -(int32_t)(p->max_order < 12 ? p->max_order : 12) - 1;
  p->prev_success = 0;
  p->hi_unit -= UNIT_SIZE;
  p->min_ctx = p->max_ctx = (struct pctx *)p->hi_unit;
  p->min_ctx->suffix = 0;
  p->min_ctx->num = 256;
  p->min_ctx->summ = 256 + 1;
  p->found = (struct pstate *)p->lo_unit;
  p->lo_unit += U2B(256 / 2);
  p->min_ctx->stats = REF(p->found);
  for (unsigned i = 0; i < 256; i++) {
    struct pstate *s = &p->found[i];
    s->sym = (uint8_t)i;
    s->freq = 1;
    set_succ(s, 0);
  }
  for (unsigned i = 0; i < 128; i++)
    for (unsigned k = 0; k < 8; k++) {
      uint16_t *dest = p->bin_summ[i] + k;
      uint16_t val = (uint16_t)(BIN_SCALE - init_bin_esc[k] / (i + 2));
      for (unsigned m = 0; m < 64; m += 8)
        dest[m] = val;
    }
  for (unsigned i = 0; i < 25; i++)
    for (unsigned k = 0; k < 16; k++) {
      struct psee *s = &p->see[i][k];
      s->shift = PERIOD_BITS - 4;
      s->summ = (uint16_t)((5 * i + 10) << s->shift);
      s->count = 4;
    }
}

void ppmd_init(struct ppmd *p, unsigned max_order)
{
  p->max_order = max_order;
  restart_model(p);
  p->dummy_see.shift = PERIOD_BITS;
  p->dummy_see.summ = 0;
  p->dummy_see.count = 64;
}

static void insert_node(struct ppmd *p, void *node, unsigned indx)
{
  *(uint32_t *)node = p->free_list[indx];
  p->free_list[indx] = REF(node);
}

static void *remove_node(struct ppmd *p, unsigned indx)
{
  uint32_t *node = (uint32_t *)(p->base + p->free_list[indx]);
  p->free_list[indx] = *node;
  return node;
}

static void split_block(struct ppmd *p, void *ptr, unsigned old_indx, unsigned new_indx)
{
  unsigned nu = I2U(old_indx) - I2U(new_indx);
  unsigned i;
  ptr = (uint8_t *)ptr + U2B(I2U(new_indx));
  if (I2U(i = U2I(nu)) != nu) {
    unsigned k = I2U(--i);
    insert_node(p, (uint8_t *)ptr + U2B(k), nu - k - 1);
  }
  insert_node(p, ptr, i);
}

static void glue_free_blocks(struct ppmd *p)
{
  uint32_t head = p->align + p->size;
  uint32_t n = head;
  p->glue_count = 255;
  for (unsigned i = 0; i < N_INDEXES; i++) {
    uint16_t nu = (uint16_t)I2U(i);
    uint32_t next = p->free_list[i];
    p->free_list[i] = 0;
    while (next != 0) {
      struct pnode *node = NODE(next);
      node->next = n;
      NODE(n)->prev = next;
      n = next;
      next = *(const uint32_t *)node;
      node->stamp = 0;
      node->nu = nu;
    }
  }
  NODE(head)->stamp = 1;
  NODE(head)->next = n;
  NODE(n)->prev = head;
  if (p->lo_unit != p->hi_unit)
    ((struct pnode *)p->lo_unit)->stamp = 1;

  while (n != head) {
    struct pnode *node = NODE(n);
    uint32_t nu = node->nu;
    for (;;) {
      struct pnode *node2 = NODE(n) + nu;
      nu += node2->nu;
      if (node2->stamp != 0 || nu >= 0x10000)
        break;
      NODE(node2->prev)->next = node2->next;
      NODE(node2->next)->prev = node2->prev;
      node->nu = (uint16_t)nu;
    }
    n = node->next;
  }

  for (n = NODE(head)->next; n != head;) {
    struct pnode *node = NODE(n);
    uint32_t next = node->next;
    unsigned nu, i;
    for (nu = node->nu; nu > 128; nu -= 128, node += 128)
      insert_node(p, node, N_INDEXES - 1);
    if (I2U(i = U2I(nu)) != nu) {
      unsigned k = I2U(--i);
      insert_node(p, node + k, nu - k - 1);
    }
    insert_node(p, node, i);
    n = next;
  }
}

static void *alloc_units_rare(struct ppmd *p, unsigned indx)
{
  if (p->glue_count == 0) {
    glue_free_blocks(p);
    if (p->free_list[indx] != 0)
      return remove_node(p, indx);
  }
  unsigned i = indx;
  do {
    if (++i == N_INDEXES) {
      uint32_t num_bytes = U2B(I2U(indx));
      p->glue_count--;
      return ((uint32_t)(p->units_start - p->text) > num_bytes) ? (p->units_start -= num_bytes) : NULL;
    }
  } while (p->free_list[i] == 0);
  void *ret = remove_node(p, i);
  split_block(p, ret, i, indx);
  return ret;
}

static void *alloc_units(struct ppmd *p, unsigned indx)
{
  if (p->free_list[indx] != 0)
    return remove_node(p, indx);
  uint32_t num_bytes = U2B(I2U(indx));
  if (num_bytes <= (uint32_t)(p->hi_unit - p->lo_unit)) {
    void *ret = p->lo_unit;
    p->lo_unit += num_bytes;
    return ret;
  }
  return alloc_units_rare(p, indx);
}

static void *shrink_units(struct ppmd *p, void *old_ptr, unsigned old_nu, unsigned new_nu)
{
  unsigned i0 = U2I(old_nu), i1 = U2I(new_nu);
  if (i0 == i1)
    return old_ptr;
  if (p->free_list[i1] != 0) {
    void *ptr = remove_node(p, i1);
    memcpy(ptr, old_ptr, (size_t)new_nu * UNIT_SIZE);
    insert_node(p, old_ptr, i0);
    return ptr;
  }
  split_block(p, old_ptr, i0, i1);
  return old_ptr;
}

static void swap_states(struct pstate *a, struct pstate *b)
{
  struct pstate t = *a;
  *a = *b;
  *b = t;
}

static struct pctx *create_successors(struct ppmd *p, bool skip)
{
  struct pstate up_state;
  struct pctx *c = p->min_ctx;
  uint32_t up_branch = succ(p->found);
  struct pstate *ps[MAX_ORDER];
  unsigned num_ps = 0;
  if (!skip)
    ps[num_ps++] = p->found;
  while (c->suffix) {
    struct pstate *s;
    c = SUFFIX(c);
    if (c->num != 1) {
      for (s = STATS(c); s->sym != p->found->sym; s++)
        ;
    } else
      s = ONE(c);
    uint32_t successor = succ(s);
    if (successor != up_branch) {
      c = CTX(successor);
      if (num_ps == 0)
        return c;
      break;
    }
    if (num_ps >= MAX_ORDER)
      return NULL;
    ps[num_ps++] = s;
  }
  up_state.sym = *(p->base + up_branch);
  set_succ(&up_state, up_branch + 1);
  if (c->num == 1)
    up_state.freq = ONE(c)->freq;
  else {
    struct pstate *s;
    for (s = STATS(c); s->sym != up_state.sym; s++)
      ;
    uint32_t cf = (uint32_t)s->freq - 1;
    uint32_t s0 = (uint32_t)c->summ - c->num - cf;
    up_state.freq = (uint8_t)(1 + ((2 * cf <= s0) ? (5 * cf > s0) : ((2 * cf + 3 * s0 - 1) / (2 * s0))));
  }
  do {
    struct pctx *c1;
    if (p->hi_unit != p->lo_unit) {
      p->hi_unit -= UNIT_SIZE;
      c1 = (struct pctx *)p->hi_unit;
    } else if (p->free_list[0] != 0)
      c1 = remove_node(p, 0);
    else {
      c1 = alloc_units_rare(p, 0);
      if (!c1)
        return NULL;
    }
    c1->num = 1;
    *ONE(c1) = up_state;
    c1->suffix = REF(c);
    set_succ(ps[--num_ps], REF(c1));
    c = c1;
  } while (num_ps != 0);
  return c;
}

static void update_model(struct ppmd *p)
{
  uint32_t successor, f_successor = succ(p->found);
  struct pctx *c;
  unsigned s0, ns;
  if (p->found->freq < MAX_FREQ / 4 && p->min_ctx->suffix != 0) {
    c = SUFFIX(p->min_ctx);
    if (c->num == 1) {
      struct pstate *s = ONE(c);
      if (s->freq < 32)
        s->freq++;
    } else {
      struct pstate *s = STATS(c);
      if (s->sym != p->found->sym) {
        do {
          s++;
        } while (s->sym != p->found->sym);
        if (s[0].freq >= s[-1].freq) {
          swap_states(&s[0], &s[-1]);
          s--;
        }
      }
      if (s->freq < MAX_FREQ - 9) {
        s->freq += 2;
        c->summ += 2;
      }
    }
  }
  if (p->order_fall == 0) {
    p->min_ctx = p->max_ctx = create_successors(p, true);
    if (p->min_ctx == NULL) {
      restart_model(p);
      return;
    }
    set_succ(p->found, REF(p->min_ctx));
    return;
  }
  *p->text++ = p->found->sym;
  successor = REF(p->text);
  if (p->text >= p->units_start) {
    restart_model(p);
    return;
  }
  if (f_successor) {
    if (f_successor <= successor) {
      struct pctx *cs = create_successors(p, false);
      if (cs == NULL) {
        restart_model(p);
        return;
      }
      f_successor = REF(cs);
    }
    if (--p->order_fall == 0) {
      successor = f_successor;
      p->text -= (p->max_ctx != p->min_ctx);
    }
  } else {
    set_succ(p->found, successor);
    f_successor = REF(p->min_ctx);
  }
  ns = p->min_ctx->num;
  s0 = p->min_ctx->summ - ns - (p->found->freq - 1u);
  for (c = p->max_ctx; c != p->min_ctx; c = SUFFIX(c)) {
    unsigned ns1;
    uint32_t cf, sf;
    if ((ns1 = c->num) != 1) {
      if ((ns1 & 1) == 0) {
        unsigned old_nu = ns1 >> 1;
        unsigned i = U2I(old_nu);
        if (i != U2I(old_nu + 1)) {
          void *ptr = alloc_units(p, i + 1);
          if (!ptr) {
            restart_model(p);
            return;
          }
          void *old_ptr = STATS(c);
          memcpy(ptr, old_ptr, (size_t)old_nu * UNIT_SIZE);
          insert_node(p, old_ptr, i);
          c->stats = REF(ptr);
        }
      }
      c->summ = (uint16_t)(c->summ + (2 * ns1 < ns) + 2 * ((4 * ns1 <= ns) & (c->summ <= 8 * ns1)));
    } else {
      struct pstate *s = alloc_units(p, 0);
      if (!s) {
        restart_model(p);
        return;
      }
      *s = *ONE(c);
      c->stats = REF(s);
      if (s->freq < MAX_FREQ / 4 - 1)
        s->freq <<= 1;
      else
        s->freq = MAX_FREQ - 4;
      c->summ = (uint16_t)(s->freq + p->init_esc + (ns > 3));
    }
    cf = 2 * (uint32_t)p->found->freq * (c->summ + 6u);
    sf = (uint32_t)s0 + c->summ;
    if (cf < 6 * sf) {
      cf = 1 + (cf > sf) + (cf >= 4 * sf);
      c->summ += 3;
    } else {
      cf = 4 + (cf >= 9 * sf) + (cf >= 12 * sf) + (cf >= 15 * sf);
      c->summ = (uint16_t)(c->summ + cf);
    }
    struct pstate *s = STATS(c) + ns1;
    set_succ(s, successor);
    s->sym = p->found->sym;
    s->freq = (uint8_t)cf;
    c->num = (uint16_t)(ns1 + 1);
  }
  p->max_ctx = p->min_ctx = CTX(f_successor);
}

static void rescale(struct ppmd *p)
{
  unsigned i, adder, sum_freq, esc_freq;
  struct pstate *stats = STATS(p->min_ctx);
  struct pstate *s = p->found;
  if (s != stats) {
    struct pstate tmp = *s;
    do
      s[0] = s[-1];
    while (--s != stats);
    *s = tmp;
  }
  esc_freq = p->min_ctx->summ - s->freq;
  s->freq += 4;
  adder = (p->order_fall != 0);
  s->freq = (uint8_t)((s->freq + adder) >> 1);
  sum_freq = s->freq;
  i = p->min_ctx->num - 1u;
  do {
    esc_freq -= (++s)->freq;
    s->freq = (uint8_t)((s->freq + adder) >> 1);
    sum_freq += s->freq;
    if (s[0].freq > s[-1].freq) {
      struct pstate *s1 = s;
      struct pstate tmp = *s1;
      do
        s1[0] = s1[-1];
      while (--s1 != stats && tmp.freq > s1[-1].freq);
      *s1 = tmp;
    }
  } while (--i);
  if (s->freq == 0) {
    unsigned num_stats = p->min_ctx->num;
    unsigned n0, n1;
    do {
      i++;
    } while ((--s)->freq == 0);
    esc_freq += i;
    p->min_ctx->num = (uint16_t)(p->min_ctx->num - i);
    if (p->min_ctx->num == 1) {
      struct pstate tmp = *stats;
      do {
        tmp.freq = (uint8_t)(tmp.freq - (tmp.freq >> 1));
        esc_freq >>= 1;
      } while (esc_freq > 1);
      insert_node(p, stats, U2I((num_stats + 1) >> 1));
      *(p->found = ONE(p->min_ctx)) = tmp;
      return;
    }
    n0 = (num_stats + 1) >> 1;
    n1 = (p->min_ctx->num + 1u) >> 1;
    if (n0 != n1)
      p->min_ctx->stats = REF(shrink_units(p, stats, n0, n1));
  }
  p->min_ctx->summ = (uint16_t)(sum_freq + esc_freq - (esc_freq >> 1));
  p->found = STATS(p->min_ctx);
}

static struct psee *make_esc_freq(struct ppmd *p, unsigned num_masked, uint32_t *esc_freq)
{
  struct psee *see;
  unsigned non_masked = p->min_ctx->num - num_masked;
  if (p->min_ctx->num != 256) {
    see = p->see[p->ns2indx[non_masked - 1]] + (non_masked < (unsigned)SUFFIX(p->min_ctx)->num - p->min_ctx->num) +
          2 * (p->min_ctx->summ < 11u * p->min_ctx->num) + 4 * (num_masked > non_masked) + p->hi_bits_flag;
    unsigned r = see->summ >> see->shift;
    see->summ = (uint16_t)(see->summ - r);
    *esc_freq = r + (r == 0);
  } else {
    see = &p->dummy_see;
    *esc_freq = 1;
  }
  return see;
}

static void next_context(struct ppmd *p)
{
  struct pctx *c = CTX(succ(p->found));
  if (p->order_fall == 0 && (uint8_t *)c > p->text)
    p->min_ctx = p->max_ctx = c;
  else
    update_model(p);
}

static void update1(struct ppmd *p)
{
  struct pstate *s = p->found;
  s->freq += 4;
  p->min_ctx->summ += 4;
  if (s[0].freq > s[-1].freq) {
    swap_states(&s[0], &s[-1]);
    p->found = --s;
    if (s->freq > MAX_FREQ)
      rescale(p);
  }
  next_context(p);
}

static void update1_0(struct ppmd *p)
{
  p->prev_success = (2u * p->found->freq > p->min_ctx->summ);
  p->run_length += (int32_t)p->prev_success;
  p->min_ctx->summ += 4;
  if ((p->found->freq += 4) > MAX_FREQ)
    rescale(p);
  next_context(p);
}

static void update_bin(struct ppmd *p)
{
  p->found->freq = (uint8_t)(p->found->freq + (p->found->freq < 128 ? 1 : 0));
  p->prev_success = 1;
  p->run_length++;
  next_context(p);
}

static void update2(struct ppmd *p)
{
  p->min_ctx->summ += 4;
  if ((p->found->freq += 4) > MAX_FREQ)
    rescale(p);
  p->run_length = p->init_rl;
  update_model(p);
}

static void see_update(struct psee *see)
{
  if (see->shift < PERIOD_BITS && --see->count == 0) {
    see->summ = (uint16_t)(see->summ << 1);
    see->count = (uint8_t)(3 << see->shift++);
  }
}

#define MEAN(prob) (((prob) + (1u << (PERIOD_BITS - 2))) >> PERIOD_BITS)

int ppmd_decode(struct ppmd *p, struct ppmd_rc *rc)
{
  int8_t mask[256];
  if (p->min_ctx->num != 1) {
    struct pstate *s = STATS(p->min_ctx);
    uint32_t count = rc->threshold(rc, p->min_ctx->summ);
    uint32_t hi_cnt = s->freq;
    if (count == UINT32_MAX)
      return -2;
    if (count < hi_cnt) {
      rc->decode(rc, 0, s->freq);
      p->found = s;
      uint8_t symbol = s->sym;
      update1_0(p);
      return symbol;
    }
    p->prev_success = 0;
    unsigned i = p->min_ctx->num - 1u;
    do {
      if ((hi_cnt += (++s)->freq) > count) {
        rc->decode(rc, hi_cnt - s->freq, s->freq);
        p->found = s;
        uint8_t symbol = s->sym;
        update1(p);
        return symbol;
      }
    } while (--i);
    if (count >= p->min_ctx->summ)
      return -2;
    p->hi_bits_flag = p->hb2flag[p->found->sym];
    rc->decode(rc, hi_cnt, p->min_ctx->summ - hi_cnt);
    memset(mask, -1, sizeof mask);
    mask[s->sym] = 0;
    i = p->min_ctx->num - 1u;
    do {
      mask[(--s)->sym] = 0;
    } while (--i);
  } else {
    p->hi_bits_flag = p->hb2flag[p->found->sym];
    uint16_t *prob = &p->bin_summ[ONE(p->min_ctx)->freq - 1][p->prev_success + p->ns2bs[SUFFIX(p->min_ctx)->num - 1] +
                                                             p->hi_bits_flag + 2 * p->hb2flag[ONE(p->min_ctx)->sym] +
                                                             ((p->run_length >> 26) & 0x20)];
    uint32_t bit = rc->bit(rc, *prob, BIN_SCALE);
    if (bit == 2)
      return -2;
    if (bit == 0) {
      *prob = (uint16_t)(*prob + (1 << INT_BITS) - MEAN(*prob));
      p->found = ONE(p->min_ctx);
      uint8_t symbol = p->found->sym;
      update_bin(p);
      return symbol;
    }
    *prob = (uint16_t)(*prob - MEAN(*prob));
    p->init_esc = exp_escape[*prob >> 10];
    memset(mask, -1, sizeof mask);
    mask[ONE(p->min_ctx)->sym] = 0;
    p->prev_success = 0;
  }
  for (;;) {
    struct pstate *ps[256], *s;
    uint32_t freq_sum, count, hi_cnt;
    unsigned i, num, num_masked = p->min_ctx->num;
    do {
      p->order_fall++;
      if (!p->min_ctx->suffix)
        return -1;
      p->min_ctx = SUFFIX(p->min_ctx);
    } while (p->min_ctx->num == num_masked);
    hi_cnt = 0;
    s = STATS(p->min_ctx);
    i = 0;
    num = p->min_ctx->num - num_masked;
    do {
      int k = mask[s->sym];
      hi_cnt += (s->freq & (unsigned)k);
      ps[i] = s++;
      i -= (unsigned)k;
    } while (i != num);
    struct psee *see = make_esc_freq(p, num_masked, &freq_sum);
    freq_sum += hi_cnt;
    count = rc->threshold(rc, freq_sum);
    if (count == UINT32_MAX)
      return -2;
    if (count < hi_cnt) {
      struct pstate **pps = ps;
      for (hi_cnt = 0; (hi_cnt += (*pps)->freq) <= count; pps++)
        ;
      s = *pps;
      rc->decode(rc, hi_cnt - s->freq, s->freq);
      see_update(see);
      p->found = s;
      uint8_t symbol = s->sym;
      update2(p);
      return symbol;
    }
    if (count >= freq_sum)
      return -2;
    rc->decode(rc, hi_cnt, freq_sum - hi_cnt);
    see->summ = (uint16_t)(see->summ + freq_sum);
    do {
      mask[ps[--i]->sym] = 0;
    } while (i != 0);
  }
}

#define RC_TOP (1u << 24)
#define RC_BOT (1u << 15)

static void rar_rc_norm(struct ppmd_rar_rc *c)
{
  for (;;) {
    if ((c->low ^ (c->low + c->range)) >= RC_TOP) {
      if (c->range >= RC_BOT)
        break;
      c->range = (0 - c->low) & (RC_BOT - 1);
    }
    if (c->range == 0)
      return;
    int b = c->getbyte(c->ctx);
    c->code = (c->code << 8) | (uint32_t)(b < 0 ? 0 : b);
    c->range <<= 8;
    c->low <<= 8;
  }
}

static uint32_t rar_rc_threshold(struct ppmd_rc *rc, uint32_t total)
{
  struct ppmd_rar_rc *c = (struct ppmd_rar_rc *)rc;
  c->range /= total;
  if (c->range == 0)
    return UINT32_MAX;
  return (c->code - c->low) / c->range;
}

static void rar_rc_decode(struct ppmd_rc *rc, uint32_t start, uint32_t size)
{
  struct ppmd_rar_rc *c = (struct ppmd_rar_rc *)rc;
  c->low += c->range * start;
  c->range *= size;
  rar_rc_norm(c);
}

static uint32_t rar_rc_bit(struct ppmd_rc *rc, uint32_t size0, uint32_t total)
{
  uint32_t count = rar_rc_threshold(rc, total);
  if (count == UINT32_MAX)
    return 2;
  if (count < size0) {
    rar_rc_decode(rc, 0, size0);
    return 0;
  }
  rar_rc_decode(rc, size0, total - size0);
  return 1;
}

void ppmd_rar_rc_init(struct ppmd_rar_rc *c, int (*getbyte)(void *), void *ctx)
{
  c->rc.threshold = rar_rc_threshold;
  c->rc.decode = rar_rc_decode;
  c->rc.bit = rar_rc_bit;
  c->getbyte = getbyte;
  c->ctx = ctx;
  c->low = c->code = 0;
  c->range = 0xFFFFFFFFu;
  for (int i = 0; i < 4; i++) {
    int b = getbyte(ctx);
    c->code = (c->code << 8) | (uint32_t)(b < 0 ? 0 : b);
  }
}
