/* rar_win.c - the sliding window with deferred filters, the packed-stream reader and the output sink. */
#include "crc32.h"
#include "owos.h"
#include "rar_int.h"

#define MAX_PENDING_FILTERS 8192
#define FLUSH_STEP 0x40000u

int out_emit(struct rar *r, const void *buf, uint32_t n)
{
  struct rar_out *o = &r->out;
  if (n == 0)
    return RAR_OK;
  if (!o->discard && r->cfg.max_total && (uint64_t)n > r->cfg.max_total - r->total_out)
    return RAR_ELIMIT;
  o->crc = crc32(o->crc, buf, n);
  if (o->use_b2)
    b2sp_update(&o->b2, buf, n);
  o->produced += n;
  if (!o->discard)
    r->total_out += n;
  if (o->write && o->write(o->ctx, buf, n))
    return RAR_EWRITE;
  return RAR_OK;
}

static int data_vol(struct rar *r, int idx, struct rar_vol **out)
{
  if (r->hv.open && r->hv.idx == idx) {
    *out = &r->hv;
    return RAR_OK;
  }
  if (!(r->dv.open && r->dv.idx == idx)) {
    int rc = rar_open_volume(r, &r->dv, idx);
    if (rc)
      return rc;
  }
  *out = &r->dv;
  return RAR_OK;
}

static int pk_enter(struct rar *r)
{
  struct rar_pk *k = &r->pk;
  const struct rar_chunk *c = &r->cur.chunks[k->ci];
  k->left = c->size;
  k->off = c->off;
  k->remn = 0;
  k->dec = false;
  if (r->cur.encrypted && c->enc.ver) {
    int rc = rar_get_key(r, &c->enc, &r->kc);
    if (rc)
      return rc;
    rar_aes_init(&k->aes, r->kc.key, c->enc.ver == 5 ? 32 : 16);
    memcpy(k->iv, c->enc.ver == 5 ? c->enc.iv : r->kc.iv, 16);
    memcpy(r->cur.hashkey, r->kc.hashkey, 32);
    r->cur.hash_mac = c->enc.hash_mac;
    k->dec = true;
  }
  return RAR_OK;
}

int pk_start(struct rar *r)
{
  struct rar_pk *k = &r->pk;
  memset(k, 0, sizeof *k);
  if (r->cur.nchunks == 0)
    return RAR_OK;
  int rc = pk_enter(r);
  k->started = rc == RAR_OK;
  return rc;
}

int pk_read(struct rar *r, uint8_t *buf, uint32_t n)
{
  struct rar_pk *k = &r->pk;
  uint32_t got = 0;
  while (got < n) {
    if (k->remn) {
      uint32_t take = k->remn < n - got ? k->remn : n - got;
      memcpy(buf + got, k->rem + 16 - k->remn, take);
      k->remn -= take;
      got += take;
      continue;
    }
    while (k->left == 0) {
      if (k->ci + 1 >= r->cur.nchunks) {
        if (r->cur.incomplete)
          return RAR_EVOLUME;
        return (int)got;
      }
      k->ci++;
      int rc = pk_enter(r);
      if (rc)
        return rc;
    }
    struct rar_vol *v;
    int rc = data_vol(r, r->cur.chunks[k->ci].vol, &v);
    if (rc)
      return rc;
    uint32_t want = n - got;
    if (k->left < want)
      want = (uint32_t)k->left;
    if (k->dec) {
      uint32_t whole = want & ~15u;
      if (whole == 0) {
        if (k->left < 16)
          return RAR_ECORRUPT;
        rc = rar_vol_read(r, v, k->off, k->rem, 16);
        if (rc < 16)
          return rc < 0 ? rc : RAR_ECORRUPT;
        rar_aes_cbc_decrypt(&k->aes, k->iv, k->rem, 16);
        k->off += 16;
        k->left -= 16;
        k->remn = 16;
        continue;
      }
      rc = rar_vol_read(r, v, k->off, buf + got, whole);
      if (rc < (int)whole)
        return rc < 0 ? rc : RAR_ECORRUPT;
      rar_aes_cbc_decrypt(&k->aes, k->iv, buf + got, whole);
      k->off += whole;
      k->left -= whole;
      got += whole;
      continue;
    }
    rc = rar_vol_read(r, v, k->off, buf + got, want);
    if (rc < (int)want)
      return rc < 0 ? rc : RAR_ECORRUPT;
    k->off += want;
    k->left -= want;
    got += want;
  }
  return (int)got;
}

void lzw_free(struct rar *r)
{
  struct lzwin *w = &r->win;
  if (w->buf)
    rar_unalloc(r, w->buf, w->size);
  if (w->f)
    rar_unalloc(r, w->f, (size_t)w->fcap * sizeof *w->f);
  if (w->tmp)
    rar_unalloc(r, w->tmp, w->tmpcap);
  memset(w, 0, sizeof *w);
}

static uint32_t pow2_at_least(uint64_t v)
{
  uint32_t p = 4096;
  while (p < v && p < 0x80000000u)
    p <<= 1;
  return p;
}

int lzw_init(struct rar *r, uint32_t cap, uint64_t hint)
{
  struct lzwin *w = &r->win;
  uint32_t size = pow2_at_least(hint);
  if (size > cap)
    size = cap;
  lzw_free(r);
  uint8_t *b = rar_alloc(r, size);
  if (!b)
    return RAR_ENOMEM;
  w->buf = b;
  w->size = size;
  w->cap = cap;
  w->next_flush = FLUSH_STEP;
  return RAR_OK;
}

static int lzw_grow(struct rar *r)
{
  struct lzwin *w = &r->win;
  uint32_t ns = w->size * 2;
  uint8_t *nb = rar_alloc(r, ns);
  if (!nb)
    return RAR_ENOMEM;
  uint64_t start = w->abs > w->size ? w->abs - w->size : 0;
  for (uint64_t x = start; x < w->abs; x++)
    nb[(uint32_t)x & (ns - 1)] = w->buf[(uint32_t)x & (w->size - 1)];
  rar_unalloc(r, w->buf, w->size);
  w->buf = nb;
  w->size = ns;
  return RAR_OK;
}

int lzw_room(struct rar *r, uint32_t n)
{
  struct lzwin *w = &r->win;
  if (n > w->cap)
    return RAR_ECORRUPT;
  while (w->size < w->cap && w->abs + n > w->size) {
    int rc = lzw_grow(r);
    if (rc)
      return rc;
  }
  if (w->abs - w->out_abs + n > w->size) {
    int rc = lzw_flush(r, false);
    if (rc)
      return rc;
    if (w->abs - w->out_abs + n > w->size)
      return RAR_ECORRUPT;
  }
  return RAR_OK;
}

static int deliver(struct rar *r, uint64_t from, uint64_t to)
{
  struct lzwin *w = &r->win;
  while (from < to) {
    uint32_t idx = (uint32_t)from & (w->size - 1);
    uint64_t n = to - from;
    if (n > w->size - idx)
      n = w->size - idx;
    int rc = out_emit(r, w->buf + idx, (uint32_t)n);
    if (rc)
      return rc;
    from += n;
    w->out_abs = from;
  }
  return RAR_OK;
}

int lzw_add_filter(struct rar *r, const struct lzfilter *f)
{
  struct lzwin *w = &r->win;
  if (f->len == 0)
    return RAR_OK;
  if (f->len > rar_filter_max_len(f->type) || f->len > w->cap)
    return RAR_ECORRUPT;
  if (w->fhead == w->nf)
    w->fhead = w->nf = 0;
  if (w->nf > w->fhead) {
    const struct lzfilter *last = &w->f[w->nf - 1];
    bool same = f->start == last->start && f->len == last->len && !f->v5;
    if (!same && f->start < last->start + last->len)
      return RAR_ECORRUPT;
  }
  if (w->nf - w->fhead >= MAX_PENDING_FILTERS)
    return RAR_ECORRUPT;
  if (w->nf == w->fcap) {
    unsigned nc = w->fcap ? w->fcap * 2 : 16;
    struct lzfilter *nf = rar_realloc(r, w->f, (size_t)w->fcap * sizeof *nf, (size_t)nc * sizeof *nf);
    if (!nf)
      return RAR_ENOMEM;
    w->f = nf;
    w->fcap = nc;
  }
  w->f[w->nf++] = *f;
  return RAR_OK;
}

static int ensure_tmp(struct rar *r, size_t need)
{
  struct lzwin *w = &r->win;
  if (w->tmpcap >= need)
    return RAR_OK;
  uint8_t *t = rar_realloc(r, w->tmp, w->tmpcap, need);
  if (!t)
    return RAR_ENOMEM;
  w->tmp = t;
  w->tmpcap = need;
  return RAR_OK;
}

int lzw_flush(struct rar *r, bool final)
{
  struct lzwin *w = &r->win;
  int rc = RAR_OK;
  for (;;) {
    if (w->fhead == w->nf) {
      w->fhead = w->nf = 0;
      rc = deliver(r, w->out_abs, w->abs);
      break;
    }
    struct lzfilter *f = &w->f[w->fhead];
    if (f->start < w->out_abs)
      return RAR_ECORRUPT;
    if (f->start > w->out_abs) {
      uint64_t to = f->start < w->abs ? f->start : w->abs;
      rc = deliver(r, w->out_abs, to);
      if (rc)
        return rc;
      if (to < f->start)
        break;
    }
    if (f->start + f->len > w->abs)
      break;
    if (w->abs - f->start > w->size)
      return RAR_ECORRUPT;
    rc = ensure_tmp(r, (size_t)f->len * 2);
    if (rc)
      return rc;
    uint8_t *buf = w->tmp, *scratch = w->tmp + f->len;
    for (uint32_t i = 0; i < f->len;) {
      uint32_t idx = (uint32_t)(f->start + i) & (w->size - 1);
      uint32_t n = f->len - i;
      if (n > w->size - idx)
        n = w->size - idx;
      memcpy(buf + i, w->buf + idx, n);
      i += n;
    }
    uint64_t start = f->start;
    uint32_t len = f->len;
    for (;;) {
      f = &w->f[w->fhead];
      const uint8_t *res = rar_filter_run(f, buf, scratch, len, (uint32_t)(start - w->file_base));
      if (!res)
        return RAR_ECORRUPT;
      if (res != buf)
        memcpy(buf, res, len);
      w->fhead++;
      if (w->fhead < w->nf && w->f[w->fhead].start == start && w->f[w->fhead].len == len)
        continue;
      break;
    }
    rc = out_emit(r, buf, len);
    if (rc)
      return rc;
    w->out_abs = start + len;
  }
  if (rc == RAR_OK && final && (w->fhead != w->nf || w->out_abs != w->abs))
    return RAR_ECORRUPT;
  return rc;
}
