#include "gfx.h"
#include "owos.h"

#define SEL_CHEVRON_W 8   /* the chevron's width */
#define SEL_CHEVRON_GAP 4 /* between the chevron and the content's right edge */

void select_init(struct select_box *s, const char *const *options, int count, int selected)
{
  s->options = options;
  s->count = count;
  s->selected = (selected >= 0 && selected < count) ? selected : 0;
  s->open = 0;
  s->hover = -1;
  s->focused = 0;
}

static int sel_row_h(const struct gfx_style *st)
{
  if (st->row_h > 0)
    return st->row_h;
  struct gfx_font f;
  gfx_style_font(st, &f);
  return gfx_font_height(&f);
}

/* The closed box's border and content boxes, and the open list's border box below it */
static void sel_geom(const struct select_box *s, const struct gfx_style *st, int x, int y, int w, int h,
                     struct gfx_box *b, struct gfx_box *c, struct gfx_box *list)
{
  gfx_style_box(st, x, y, w, h, b);
  gfx_style_content(st, b, c);
  int bw = st->border > 0 ? st->border : 0;
  list->x = b->x;
  list->y = b->y + b->h;
  list->w = b->w;
  list->h = s->count * sel_row_h(st) + 2 * bw;
}

/* The list row under (mx,my), or -1 */
static int sel_row_at(const struct select_box *s, const struct gfx_style *st, const struct gfx_box *list, int mx,
                      int my)
{
  int bw = st->border > 0 ? st->border : 0;
  if (mx < list->x || mx >= list->x + list->w || my < list->y + bw || my >= list->y + list->h - bw)
    return -1;
  int row = (my - list->y - bw) / sel_row_h(st);
  return row < s->count ? row : -1;
}

void select_draw(struct select_box *s, int x, int y, int w, int h, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_SELECT, st);
  struct gfx_box b, c, list;
  sel_geom(s, st, x, y, w, h, &b, &c, &list);
  const char *lbl = (s->selected >= 0 && s->selected < s->count) ? s->options[s->selected] : "";
  unsigned a11y = GFX_A11Y_HASPOPUP | (s->open ? GFX_A11Y_EXPANDED : 0) | (s->focused ? GFX_A11Y_FOCUSED : 0);
  gfx_a11y_set_text(gfx_a11y_add(GFX_ROLE_SELECT, 0, a11y, b.x, b.y, b.w, b.h), lbl);

  const struct gfx_colors *box = &st->color[BTN_NORMAL], *acc = &st->accent[BTN_NORMAL];
  if (s->focused)
    gfx_focus_ring(b.x, b.y, b.w, b.h, st);
  struct gfx_colors frame = *box;
  frame.detail = frame.bg; /* detail is the chevron here, not a lit edge */
  if (s->open)
    frame.border = acc->border;
  gfx_style_paint(st, &b, &frame);

  struct gfx_box text = c;
  text.w -= SEL_CHEVRON_W + SEL_CHEVRON_GAP + st->gap;
  gfx_style_label(st, &text, lbl, box->fg);

  int tx = c.x + c.w - SEL_CHEVRON_W - SEL_CHEVRON_GAP, ty = b.y + (b.h - 4) / 2;
  for (int r = 0; r < 4; r++)
    gfx_fill_rect(tx + r, ty + r, SEL_CHEVRON_W - 2 * r, 1, box->detail);

  if (s->open && s->count > 0) {
    int bw = st->border > 0 ? st->border : 0, rh = sel_row_h(st);
    gfx_fill_rect(list.x, list.y, list.w, list.h, box->bg);
    for (int i = 0; i < s->count; i++) {
      int ry = list.y + bw + i * rh;
      int lit = i == s->hover || (s->hover < 0 && i == s->selected);
      unsigned ost = (i == s->selected ? GFX_A11Y_SELECTED : 0) | (lit ? GFX_A11Y_HOVER : 0);
      gfx_a11y_set_value(gfx_a11y_add(GFX_ROLE_OPTION, s->options[i], ost, list.x, ry, list.w, rh), i + 1, 0, s->count);
      if (lit)
        gfx_fill_rect(list.x + bw, ry, list.w - 2 * bw, rh, acc->bg);
      struct gfx_box row = {c.x, ry, c.w, rh};
      gfx_style_label(st, &row, s->options[i], lit ? acc->fg : box->fg);
    }
    if (bw)
      gfx_round_rect(list.x, list.y, list.w, list.h, 0, bw, box->border);
  }
}

int select_click(struct select_box *s, int x, int y, int w, int h, int mx, int my, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_SELECT, st);
  struct gfx_box b, c, list;
  sel_geom(s, st, x, y, w, h, &b, &c, &list);
  int in_box = mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h;
  if (!s->open) {
    if (in_box) {
      s->open = 1;
      s->hover = s->selected;
      return -1;
    }
    return 0;
  }
  if (in_box) {
    s->open = 0;
    return -1;
  }
  int row = sel_row_at(s, st, &list, mx, my);
  if (row >= 0) {
    s->selected = row;
    s->open = 0;
    return 1;
  }
  s->open = 0;
  return 0;
}

int select_motion(struct select_box *s, int x, int y, int w, int h, int mx, int my, const struct gfx_style *st)
{
  if (!s->open)
    return 0;
  st = gfx_style_of(GFX_W_SELECT, st);
  struct gfx_box b, c, list;
  sel_geom(s, st, x, y, w, h, &b, &c, &list);
  int nh = sel_row_at(s, st, &list, mx, my);
  if (nh != s->hover) {
    s->hover = nh;
    return 1;
  }
  return 0;
}

int select_key(struct select_box *s, char key)
{
  unsigned char u = (unsigned char)key;
  if (s->count <= 0)
    return 0;
  if (u == KEY_UP || u == KEY_DOWN) {
    int dir = (u == KEY_DOWN) ? 1 : -1;
    if (s->open) {
      int h = (s->hover < 0) ? s->selected : s->hover;
      h += dir;
      if (h < 0)
        h = 0;
      if (h >= s->count)
        h = s->count - 1;
      s->hover = h;
      return -1;
    }
    int n = s->selected + dir;
    if (n < 0)
      n = 0;
    if (n >= s->count)
      n = s->count - 1;
    if (n != s->selected) {
      s->selected = n;
      return 1;
    }
    return -1;
  }
  if (u == KEY_HOME || u == KEY_END) {
    int n = (u == KEY_HOME) ? 0 : s->count - 1;
    if (s->open) {
      s->hover = n;
      return -1;
    }
    if (n != s->selected) {
      s->selected = n;
      return 1;
    }
    return -1;
  }
  if (key == '\n' || key == '\r' || key == ' ') {
    if (!s->open) {
      s->open = 1;
      s->hover = s->selected;
      return -1;
    }
    int h = (s->hover >= 0 && s->hover < s->count) ? s->hover : s->selected;
    s->selected = h;
    s->open = 0;
    return 1;
  }
  if (u == 27) {
    if (s->open) {
      s->open = 0;
      return -1;
    }
    return 0;
  }
  return 0;
}
