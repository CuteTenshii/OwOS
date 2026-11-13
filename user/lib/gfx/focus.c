/* focus.c: keyboard focus order and the key handlers of the stateless widgets. See gfx.h */
#include "gfx.h"
#include "owos.h"

static int is_enter(int key) { return key == '\n' || key == '\r'; }

void gfx_focus_init(struct gfx_focus *f, int n, int cur, const unsigned char *disabled)
{
  f->n = n > 0 ? n : 0;
  f->disabled = disabled;
  f->cur = -1;
  if (f->n == 0)
    return;
  if (cur < 0 || cur >= f->n)
    cur = 0;
  if (disabled && disabled[cur]) {
    f->cur = cur;
    if (!gfx_focus_move(f, 1))
      f->cur = -1;
    return;
  }
  f->cur = cur;
}

int gfx_focus_move(struct gfx_focus *f, int dir)
{
  if (f->n <= 0 || dir == 0)
    return 0;
  dir = dir > 0 ? 1 : -1;
  int from = f->cur >= 0 && f->cur < f->n ? f->cur : (dir > 0 ? -1 : f->n);
  for (int step = 1; step <= f->n; step++) {
    int i = ((from + dir * step) % f->n + f->n) % f->n;
    if (f->disabled && f->disabled[i])
      continue;
    if (i == f->cur)
      return 0;
    f->cur = i;
    return 1;
  }
  return 0;
}

int gfx_focus_key(struct gfx_focus *f, int key, int mods)
{
  if (key != '\t' || (mods & (KMOD_CTRL | KMOD_ALT)))
    return 0;
  return gfx_focus_move(f, (mods & KMOD_SHIFT) ? -1 : 1);
}

int gfx_focus_is(const struct gfx_focus *f, int i) { return f->cur >= 0 && f->cur == i; }

int button_key(int key) { return key == ' ' || is_enter(key); }

int control_key(int kind, int key, int *value)
{
  if (key != ' ')
    return 0;
  if (kind == GFX_CTRL_RADIO) {
    if (*value)
      return 0;
    *value = 1;
    return 1;
  }
  *value = !*value;
  return 1;
}

int slider_key(int key, int *value, int max, int step)
{
  if (max < 0)
    max = 0;
  if (step <= 0)
    step = max / 10 > 0 ? max / 10 : 1;
  int v = *value;
  if (key == KEY_RIGHT || key == KEY_UP)
    v += step;
  else if (key == KEY_LEFT || key == KEY_DOWN)
    v -= step;
  else if (key == KEY_HOME)
    v = 0;
  else if (key == KEY_END)
    v = max;
  else
    return 0;
  v = v < 0 ? 0 : v > max ? max : v;
  if (v == *value)
    return 0;
  *value = v;
  return 1;
}

int tabbar_key(int key, int n, int *active)
{
  if (n <= 0)
    return 0;
  int a = *active;
  if (key == KEY_RIGHT)
    a = (a + 1) % n;
  else if (key == KEY_LEFT)
    a = (a - 1 + n) % n;
  else if (key == KEY_HOME)
    a = 0;
  else if (key == KEY_END)
    a = n - 1;
  else
    return 0;
  if (a == *active)
    return 0;
  *active = a;
  return 1;
}
