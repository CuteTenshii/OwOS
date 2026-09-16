#include "gfx.h"

static int selection_le(int al, int ac, int bl, int bc) { return al < bl || (al == bl && ac <= bc); }

void gfx_sel_clear(struct gfx_sel *s)
{
  s->active = s->has = 0;
  s->al = s->ac = s->el = s->ec = 0;
}

void gfx_sel_start(struct gfx_sel *s, int line, int col)
{
  s->active = 1;
  s->has = 0;
  s->al = s->el = line;
  s->ac = s->ec = col;
}

void gfx_sel_extend(struct gfx_sel *s, int line, int col)
{
  if (!s->active)
    return;
  s->el = line;
  s->ec = col;
  s->has = s->al != s->el || s->ac != s->ec;
}

void gfx_sel_end(struct gfx_sel *s) { s->active = 0; }
int gfx_sel_active(const struct gfx_sel *s) { return s->active; }
int gfx_sel_has(const struct gfx_sel *s) { return s->has; }

int gfx_sel_range(const struct gfx_sel *s, int *l0, int *c0, int *l1, int *c1)
{
  if (!s->has)
    return 0;
  if (selection_le(s->al, s->ac, s->el, s->ec)) {
    *l0 = s->al;
    *c0 = s->ac;
    *l1 = s->el;
    *c1 = s->ec;
  } else {
    *l0 = s->el;
    *c0 = s->ec;
    *l1 = s->al;
    *c1 = s->ac;
  }
  return 1;
}

int gfx_sel_contains(const struct gfx_sel *s, int line, int col)
{
  int l0, c0, l1, c1;
  if (!gfx_sel_range(s, &l0, &c0, &l1, &c1))
    return 0;
  return selection_le(l0, c0, line, col) && (line < l1 || (line == l1 && col < c1));
}
