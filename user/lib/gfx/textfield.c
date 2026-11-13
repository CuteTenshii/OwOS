#include "gfx.h"
#include "owos.h" /* KEY_* editing codes delivered via WEV_KEY */

/* text field */

void textfield_init(struct textfield *tf, char *buf, int cap)
{
  tf->buf = buf;
  tf->cap = cap;
  tf->len = 0;
  tf->caret = 0;
  tf->anchor = 0;
  tf->scroll = 0;
  tf->nundo = tf->nredo = tf->last_edit = 0;
  if (cap > 0)
    buf[0] = '\0';
}

static void tf_drop(struct tf_snap *stack, int *n)
{
  while (*n > 0)
    free(stack[--*n].text);
}

static void tf_snap_push(struct tf_snap *stack, int *n, const struct textfield *tf)
{
  char *copy = malloc((unsigned)tf->len + 1);
  if (!copy)
    return;
  memcpy(copy, tf->buf, (unsigned)tf->len + 1);
  if (*n == TEXTFIELD_UNDO) {
    free(stack[0].text);
    memmove(stack, stack + 1, sizeof stack[0] * (TEXTFIELD_UNDO - 1));
    --*n;
  }
  stack[*n].text = copy;
  stack[*n].caret = tf->caret;
  ++*n;
}

enum { EDIT_NONE, EDIT_TYPE, EDIT_DELETE, EDIT_PASTE };

/* Record the state before an edit; a run of plain typing shares one step */
static void tf_begin_edit(struct textfield *tf, int kind)
{
  int a, b;
  if (kind != tf->last_edit || kind != EDIT_TYPE || textfield_selection(tf, &a, &b))
    tf_snap_push(tf->undo, &tf->nundo, tf);
  tf_drop(tf->redo, &tf->nredo);
  tf->last_edit = kind;
}

/* Swap the buffer with the top of `from`, saving the current state on `to` */
static void tf_travel(struct textfield *tf, struct tf_snap *from, int *nfrom, struct tf_snap *to, int *nto)
{
  if (*nfrom == 0)
    return;
  struct tf_snap *snap = &from[--*nfrom];
  tf_snap_push(to, nto, tf);
  int n = (int)strlen(snap->text);
  if (n > tf->cap - 1)
    n = tf->cap - 1;
  memcpy(tf->buf, snap->text, (unsigned)n);
  tf->buf[n] = '\0';
  tf->len = n;
  tf->caret = tf->anchor = snap->caret < n ? snap->caret : n;
  free(snap->text);
  tf->last_edit = EDIT_NONE;
}

void textfield_set(struct textfield *tf, const char *s)
{
  tf_drop(tf->undo, &tf->nundo);
  tf_drop(tf->redo, &tf->nredo);
  tf->last_edit = EDIT_NONE;
  int i = 0;
  for (; s[i] && i < tf->cap - 1; i++)
    tf->buf[i] = s[i];
  tf->buf[i] = '\0';
  tf->len = i;
  tf->caret = i;
  tf->anchor = i;
  tf->scroll = 0;
}

/* Pixel width of buf[a..b) in font f */
static int tf_span(const struct textfield *tf, int a, int b, const struct gfx_font *f)
{
  int w = 0;
  for (int i = a; i < b; i++)
    w += gfx_font_cw((unsigned char)tf->buf[i], f);
  return w;
}

/* The field's content box: where text sits and the caret moves */
static void tf_content(const struct gfx_style *st, int x, int y, int w, int h, struct gfx_box *b, struct gfx_box *c)
{
  gfx_style_box(st, x, y, w, h, b);
  gfx_style_content(st, b, c);
  if (c->w < 1)
    c->w = 1;
}

int textfield_selection(const struct textfield *tf, int *start, int *end)
{
  *start = tf->anchor < tf->caret ? tf->anchor : tf->caret;
  *end = tf->anchor < tf->caret ? tf->caret : tf->anchor;
  return *end > *start;
}

void textfield_select_all(struct textfield *tf)
{
  tf->anchor = 0;
  tf->caret = tf->len;
  tf->last_edit = EDIT_NONE;
}

/* Remove the selected text, leaving the caret at its start; 1 if anything was removed */
static int tf_delete_selection(struct textfield *tf)
{
  int a, b;
  if (!textfield_selection(tf, &a, &b))
    return 0;
  memmove(tf->buf + a, tf->buf + b, (unsigned)(tf->len - b + 1));
  tf->len -= b - a;
  tf->caret = tf->anchor = a;
  return 1;
}

void textfield_draw(struct textfield *tf, int x, int y, int w, int h, int focused, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_TEXTFIELD, st);
  struct gfx_box b, c;
  tf_content(st, x, y, w, h, &b, &c);
  struct gfx_font f;
  gfx_style_font(st, &f);
  const struct gfx_colors *field = &st->color[BTN_NORMAL], *acc = &st->accent[BTN_NORMAL];
  gfx_a11y_set_text(gfx_a11y_add(GFX_ROLE_TEXTFIELD, 0, focused ? GFX_A11Y_FOCUSED : 0, b.x, b.y, b.w, b.h), tf->buf);
  struct gfx_colors frame = *field;
  if (focused)
    frame.border = acc->border;
  gfx_style_paint(st, &b, &frame);

  int avail = c.w, fh = gfx_font_height(&f);
  int ty = c.y + (c.h - fh) / 2;

  /* Scroll so the caret stays visible: never left of `scroll` */
  if (tf->scroll > tf->len)
    tf->scroll = tf->len;
  if (tf->caret < tf->scroll)
    tf->scroll = tf->caret;
  while (tf->scroll < tf->caret && tf_span(tf, tf->scroll, tf->caret, &f) > avail - 2)
    tf->scroll++;
  /* After a deletion, pull the view left while all the text fits */
  while (tf->scroll > 0 && tf_span(tf, tf->scroll - 1, tf->len, &f) <= avail - 2)
    tf->scroll--;

  int s0, s1;
  int has_sel = textfield_selection(tf, &s0, &s1);
  int px = c.x;
  int caret_px = -1;
  for (int i = tf->scroll; i <= tf->len; i++) {
    if (i == tf->caret)
      caret_px = px;
    if (i == tf->len)
      break;
    int cw = gfx_font_cw((unsigned char)tf->buf[i], &f);
    if (px - c.x + cw > avail)
      break;
    char one[2] = {tf->buf[i], '\0'};
    int sel = has_sel && i >= s0 && i < s1;
    if (sel)
      gfx_fill_rect(px, ty, cw, fh, acc->bg);
    gfx_font_text(px, ty, one, sel ? acc->fg : field->fg, &f);
    px += cw;
  }
  if (focused && caret_px >= 0) {
    if (caret_px > c.x + avail - 1)
      caret_px = c.x + avail - 1;
    gfx_fill_rect(caret_px, ty, 1, fh, acc->bg);
  }
}

/* Character index nearest to pixel column mx, using the scroll from the last draw */
static int tf_index_at(const struct textfield *tf, int mx, const struct gfx_box *c, const struct gfx_font *f)
{
  int px = c->x;
  for (int i = tf->scroll; i < tf->len; i++) {
    int cw = gfx_font_cw((unsigned char)tf->buf[i], f);
    if (px - c->x + cw > c->w || mx < px + cw / 2)
      return i;
    px += cw;
  }
  return tf->len;
}

int textfield_click(struct textfield *tf, int mx, int my, int x, int y, int w, int h, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_TEXTFIELD, st);
  struct gfx_box b, c;
  tf_content(st, x, y, w, h, &b, &c);
  if (mx < b.x || mx >= b.x + b.w || my < b.y || my >= b.y + b.h)
    return 0;
  struct gfx_font f;
  gfx_style_font(st, &f);
  tf->caret = tf->anchor = tf_index_at(tf, mx, &c, &f);
  tf->last_edit = EDIT_NONE;
  return 1;
}

void textfield_drag(struct textfield *tf, int mx, int x, int w, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_TEXTFIELD, st);
  struct gfx_box b, c;
  tf_content(st, x, 0, w, TEXTFIELD_H, &b, &c);
  if (mx < b.x)
    tf->caret = tf->scroll > 0 ? tf->scroll - 1 : 0;
  else if (mx >= b.x + b.w)
    tf->caret = tf->caret < tf->len ? tf->caret + 1 : tf->len;
  else {
    struct gfx_font f;
    gfx_style_font(st, &f);
    tf->caret = tf_index_at(tf, mx, &c, &f);
  }
}

int textfield_key(struct textfield *tf, char c, int mods)
{
  unsigned char u = (unsigned char)c;
  int extend = mods & KMOD_SHIFT;
  if (mods & KMOD_CTRL) {
    if (c == 'a' || c == 'A') {
      textfield_select_all(tf);
      return 1;
    }
    if (c == 'z' || c == 'Z') {
      if (extend)
        tf_travel(tf, tf->redo, &tf->nredo, tf->undo, &tf->nundo);
      else
        tf_travel(tf, tf->undo, &tf->nundo, tf->redo, &tf->nredo);
      return 1;
    }
    if (c == 'y' || c == 'Y') {
      tf_travel(tf, tf->redo, &tf->nredo, tf->undo, &tf->nundo);
      return 1;
    }
    if (u >= 0x20 && u < 0x7f)
      return 0;
  }
  switch (u) {
  case KEY_LEFT:
  case KEY_RIGHT:
  case KEY_HOME:
  case KEY_END: {
    int a, b;
    int sel = textfield_selection(tf, &a, &b);
    tf->last_edit = EDIT_NONE;
    if (u == KEY_HOME)
      tf->caret = 0;
    else if (u == KEY_END)
      tf->caret = tf->len;
    else if (!extend && sel)
      tf->caret = u == KEY_LEFT ? a : b;
    else if (u == KEY_LEFT && tf->caret > 0)
      tf->caret--;
    else if (u == KEY_RIGHT && tf->caret < tf->len)
      tf->caret++;
    if (!extend)
      tf->anchor = tf->caret;
    return 1;
  }
  case '\b': /* delete the selection, else left of caret */
    if (tf->caret > 0 || tf->anchor != tf->caret)
      tf_begin_edit(tf, EDIT_DELETE);
    if (!tf_delete_selection(tf) && tf->caret > 0) {
      memmove(tf->buf + tf->caret - 1, tf->buf + tf->caret, (unsigned)(tf->len - tf->caret + 1));
      tf->caret--;
      tf->len--;
      tf->anchor = tf->caret;
    }
    return 1;
  case KEY_DELETE: /* delete the selection, else right of caret */
    if (tf->caret < tf->len || tf->anchor != tf->caret)
      tf_begin_edit(tf, EDIT_DELETE);
    if (!tf_delete_selection(tf) && tf->caret < tf->len) {
      memmove(tf->buf + tf->caret, tf->buf + tf->caret + 1, (unsigned)(tf->len - tf->caret));
      tf->len--;
    }
    return 1;
  default:
    if (u >= 0x20 && u < 0x7f) {
      tf_begin_edit(tf, EDIT_TYPE);
      tf_delete_selection(tf);
      if (tf->len >= tf->cap - 1)
        return 1;
      memmove(tf->buf + tf->caret + 1, tf->buf + tf->caret, (unsigned)(tf->len - tf->caret + 1));
      tf->buf[tf->caret] = c;
      tf->caret++;
      tf->anchor = tf->caret;
      tf->len++;
      return 1;
    }
    return 0; /* Enter/Esc/arrows: app handles */
  }
}

void textfield_insert(struct textfield *tf, const char *s, int n)
{
  tf_begin_edit(tf, EDIT_PASTE);
  tf_delete_selection(tf);
  for (int i = 0; i < n && tf->len < tf->cap - 1; i++) {
    unsigned char u = (unsigned char)s[i];
    if (u == '\n' || u == '\r' || u == '\t')
      u = ' ';
    if (u < 0x20 || u >= 0x7f)
      continue;
    memmove(tf->buf + tf->caret + 1, tf->buf + tf->caret, (unsigned)(tf->len - tf->caret + 1));
    tf->buf[tf->caret++] = (char)u;
    tf->len++;
  }
  tf->anchor = tf->caret;
}
