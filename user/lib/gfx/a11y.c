/* a11y.c: the accessibility nodes widgets record as they draw. See gfx.h */
#include "gfx.h"
#include "owos.h"

static struct gfx_a11y_tree *bound;

/* Bytes of s that fit in `room` without splitting a UTF-8 sequence */
static int utf8_fit(const char *s, int room)
{
  int n = 0;
  while (n < room && s[n])
    n++;
  if (s[n])
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
      n--;
  return n;
}

static void copy_text(char *dst, int cap, const char *src)
{
  int n = src ? utf8_fit(src, cap - 1) : 0;
  for (int i = 0; i < n; i++)
    dst[i] = src[i];
  dst[n] = '\0';
}

void gfx_a11y_tree_init(struct gfx_a11y_tree *t, struct gfx_a11y *nodes, int cap)
{
  t->nodes = nodes;
  t->cap = nodes && cap > 0 ? cap : 0;
  t->n = 0;
}

struct gfx_a11y_tree *gfx_a11y_bind(struct gfx_a11y_tree *t)
{
  struct gfx_a11y_tree *prev = bound;
  bound = t;
  return prev;
}

void gfx_a11y_begin(void)
{
  if (bound)
    bound->n = 0;
}

struct gfx_a11y *gfx_a11y_add(int role, const char *name, unsigned state, int x, int y, int w, int h)
{
  if (!bound || bound->n >= bound->cap)
    return 0;
  struct gfx_a11y *a = &bound->nodes[bound->n++];
  a->role = role;
  a->state = state;
  a->x = x;
  a->y = y;
  a->w = w;
  a->h = h;
  a->value = a->min = a->max = 0;
  copy_text(a->name, sizeof a->name, name);
  a->text[0] = a->desc[0] = a->shortcut[0] = '\0';
  return a;
}

struct gfx_a11y *gfx_a11y_last(void) { return bound && bound->n > 0 ? &bound->nodes[bound->n - 1] : 0; }

void gfx_a11y_set_name(struct gfx_a11y *a, const char *s)
{
  if (a)
    copy_text(a->name, sizeof a->name, s);
}

void gfx_a11y_set_text(struct gfx_a11y *a, const char *s)
{
  if (a)
    copy_text(a->text, sizeof a->text, s);
}

void gfx_a11y_set_desc(struct gfx_a11y *a, const char *s)
{
  if (a)
    copy_text(a->desc, sizeof a->desc, s);
}

void gfx_a11y_set_shortcut(struct gfx_a11y *a, const char *s)
{
  if (a)
    copy_text(a->shortcut, sizeof a->shortcut, s);
}

void gfx_a11y_set_value(struct gfx_a11y *a, int value, int min, int max)
{
  if (!a)
    return;
  a->value = value;
  a->min = min;
  a->max = max;
}

unsigned gfx_a11y_state(int btn_state)
{
  unsigned s = (btn_state & GFX_FOCUSED) ? GFX_A11Y_FOCUSED : 0;
  switch (btn_state & ~GFX_FOCUSED) {
  case BTN_HOVER:
    return s | GFX_A11Y_HOVER;
  case BTN_PRESSED:
    return s | GFX_A11Y_PRESSED;
  case BTN_DISABLED:
    return s | GFX_A11Y_DISABLED;
  default:
    return s;
  }
}

const struct gfx_a11y *gfx_a11y_focused(const struct gfx_a11y_tree *t)
{
  if (!t)
    return 0;
  for (int i = 0; i < t->n; i++)
    if (t->nodes[i].state & GFX_A11Y_FOCUSED)
      return &t->nodes[i];
  return 0;
}

static const char *const role_names[GFX_ROLE_COUNT] = {
    [GFX_ROLE_NONE] = "",
    [GFX_ROLE_DIALOG] = "dialog",
    [GFX_ROLE_LABEL] = "",
    [GFX_ROLE_BUTTON] = "button",
    [GFX_ROLE_CHECKBOX] = "check box",
    [GFX_ROLE_RADIO] = "radio button",
    [GFX_ROLE_SWITCH] = "switch",
    [GFX_ROLE_SLIDER] = "slider",
    [GFX_ROLE_PROGRESS] = "progress bar",
    [GFX_ROLE_SCROLLBAR] = "scroll bar",
    [GFX_ROLE_TEXTFIELD] = "edit text",
    [GFX_ROLE_SELECT] = "combo box",
    [GFX_ROLE_OPTION] = "option",
    [GFX_ROLE_TAB] = "tab",
    [GFX_ROLE_MENU] = "menu",
    [GFX_ROLE_MENUITEM] = "menu item",
    [GFX_ROLE_LIST] = "list",
    [GFX_ROLE_LISTITEM] = "list item",
};

const char *gfx_a11y_role_name(int role) { return role >= 0 && role < GFX_ROLE_COUNT ? role_names[role] : ""; }

struct sbuf {
  char *p;
  int sz, len;
};

/* Append s as the next comma-separated part; empty parts are skipped */
static void part(struct sbuf *b, const char *s)
{
  if (!s || !*s || b->sz <= 0)
    return;
  const char *sep = ", ";
  if (b->len)
    for (; *sep && b->len < b->sz - 1; sep++)
      b->p[b->len++] = *sep;
  int n = utf8_fit(s, b->sz - 1 - b->len);
  for (int i = 0; i < n; i++)
    b->p[b->len++] = s[i];
  b->p[b->len] = '\0';
}

static int checkable(int role) { return role == GFX_ROLE_CHECKBOX || role == GFX_ROLE_RADIO; }

int gfx_a11y_describe(const struct gfx_a11y *a, char *buf, int sz)
{
  struct sbuf b = {buf, sz, 0};
  if (sz > 0)
    buf[0] = '\0';
  if (!a)
    return 0;
  unsigned st = a->state;
  part(&b, a->name);
  part(&b, gfx_a11y_role_name(a->role));
  if (a->role == GFX_ROLE_TEXTFIELD && !a->text[0])
    part(&b, "blank");
  part(&b, a->text);

  if (a->max > a->min) {
    char v[40];
    int span = a->max - a->min, off = a->value - a->min;
    if (a->role == GFX_ROLE_PROGRESS) /* off * 100 overflows int past ~21M */
      snprintf(v, sizeof v, "%d%%", span > 1000000 ? off / (span / 100) : off * 100 / span);
    else if (a->min == 0)
      snprintf(v, sizeof v, "%d of %d", a->value, a->max);
    else
      snprintf(v, sizeof v, "%d, %d to %d", a->value, a->min, a->max);
    part(&b, v);
  }

  if (checkable(a->role))
    part(&b, (st & GFX_A11Y_CHECKED) ? "checked" : "not checked");
  else if (a->role == GFX_ROLE_SWITCH)
    part(&b, (st & GFX_A11Y_CHECKED) ? "on" : "off");
  else if (st & GFX_A11Y_CHECKED)
    part(&b, "checked");
  if (st & GFX_A11Y_HASPOPUP)
    part(&b, (st & GFX_A11Y_EXPANDED) ? "expanded" : "collapsed");
  else if (st & GFX_A11Y_EXPANDED)
    part(&b, "expanded");
  if (st & GFX_A11Y_SELECTED)
    part(&b, "selected");
  if (st & GFX_A11Y_PRESSED)
    part(&b, "pressed");
  if (st & GFX_A11Y_DEFAULT)
    part(&b, "default");
  if (st & GFX_A11Y_DISABLED)
    part(&b, "unavailable");
  part(&b, a->shortcut);
  part(&b, a->desc);
  return b.len;
}
