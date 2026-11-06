/* imgcarry.h  -  inline images keep their landed state across layout walks. */
#ifndef OWOS_BROWSER_IMGCARRY_H
#define OWOS_BROWSER_IMGCARRY_H

#include "browser.h"

struct img_carry {
  struct inl_img *prev;
  unsigned char *used;
  int n;
};

/* Snapshot cur[0..n). False when there is nothing to carry or no memory. */
static inline bool img_carry_begin(struct img_carry *c, const struct inl_img *cur, int n)
{
  c->prev = 0;
  c->used = 0;
  c->n = 0;
  if (n <= 0)
    return false;
  c->prev = malloc((unsigned)n * sizeof *c->prev);
  c->used = calloc((unsigned)n, 1);
  if (!c->prev || !c->used) {
    free(c->prev);
    free(c->used);
    c->prev = 0;
    c->used = 0;
    return false;
  }
  memcpy(c->prev, cur, (unsigned)n * sizeof *c->prev);
  c->n = n;
  return true;
}

static inline void img_carry_end(struct img_carry *c)
{
  free(c->prev);
  free(c->used);
  c->prev = 0;
  c->used = 0;
  c->n = 0;
}

/* Pool bytes survive a walk only while some entry still points into the pool. */
static inline int img_carry_pool_len(const struct inl_img *imgs, int n, int pool_len)
{
  for (int i = 0; i < n; i++)
    if (imgs[i].clen > 0)
      return pool_len;
  return 0;
}

static inline void img_clear(struct inl_img *im)
{
  im->ok = 0;
  im->iw = im->ih = 0;
  im->coff = im->clen = 0;
  im->is_argb = 0;
  im->uid = 0;
  im->fetched = 0;
  im->want = 0;
}

/* Adopt the first unclaimed previous entry with href `key` (scanning from slot idx) */
static inline bool img_carry_take(struct img_carry *c, struct inl_img *im, const char *key, int idx, bool need_ok,
                                  const struct inl_img *built)
{
  const struct inl_img *o = 0;
  int start = idx < c->n ? idx : 0;
  for (int j = 0; !o && j < c->n; j++) {
    int i = start + j >= c->n ? start + j - c->n : start + j;
    if (!c->used[i] && streq(c->prev[i].href, key) && (!need_ok || c->prev[i].ok)) {
      c->used[i] = 1;
      o = &c->prev[i];
    }
  }
  for (int i = 0; !o && need_ok && i < idx; i++)
    if (built[i].ok && streq(built[i].href, key))
      o = &built[i];
  if (!o)
    return false;
  im->ok = o->ok;
  im->iw = o->iw;
  im->ih = o->ih;
  im->coff = o->coff;
  im->clen = o->clen;
  im->is_argb = o->is_argb;
  im->uid = o->uid;
  im->fetched = o->fetched;
  im->want = o->want;
  return true;
}

#endif
