#include "gfx.h"
#include "owos.h"

static int overflows(const struct scrollview *sv) { return sv->content_h > sv->h; }

static int max_scroll(const struct scrollview *sv) { return overflows(sv) ? sv->content_h - sv->h : 0; }

static int set_scroll(struct scrollview *sv, int pos)
{
  int max = max_scroll(sv);
  pos = pos < 0 ? 0 : pos > max ? max : pos;
  int changed = pos != sv->scroll;
  sv->scroll = pos;
  return changed;
}

void scrollview_init(struct scrollview *sv, int step)
{
  *sv = (struct scrollview){0};
  sv->step = step > 0 ? step : SCROLLVIEW_STEP;
}

int scrollview_begin(struct scrollview *sv, int x, int y, int w, int h, int content_h)
{
  sv->x = x;
  sv->y = y;
  sv->w = w;
  sv->h = h;
  sv->content_h = content_h;
  set_scroll(sv, sv->scroll);

  gfx_get_clip(&sv->saved_clip[0], &sv->saved_clip[1], &sv->saved_clip[2], &sv->saved_clip[3]);
  int x0 = x > sv->saved_clip[0] ? x : sv->saved_clip[0];
  int y0 = y > sv->saved_clip[1] ? y : sv->saved_clip[1];
  int x1 = x + scrollview_content_w(sv), y1 = y + h;
  int cx1 = sv->saved_clip[0] + sv->saved_clip[2], cy1 = sv->saved_clip[1] + sv->saved_clip[3];
  x1 = x1 < cx1 ? x1 : cx1;
  y1 = y1 < cy1 ? y1 : cy1;
  gfx_set_clip(x0, y0, x1 - x0, y1 - y0);
  return y - sv->scroll;
}

void scrollview_end(struct scrollview *sv)
{
  gfx_set_clip(sv->saved_clip[0], sv->saved_clip[1], sv->saved_clip[2], sv->saved_clip[3]);
  if (overflows(sv))
    scrollbar_draw(sv->x + sv->w - GUI_SCROLL_W, sv->y, GUI_SCROLL_W, sv->h, sv->scroll, sv->content_h, sv->h);
}

int scrollview_content_x(const struct scrollview *sv) { return sv->x; }

int scrollview_content_w(const struct scrollview *sv) { return overflows(sv) ? sv->w - GUI_SCROLL_W : sv->w; }

int scrollview_wheel(struct scrollview *sv, int dz) { return set_scroll(sv, sv->scroll - dz * sv->step); }

int scrollview_key(struct scrollview *sv, int key)
{
  switch (key) {
  case KEY_UP:
    set_scroll(sv, sv->scroll - sv->step);
    return 1;
  case KEY_DOWN:
    set_scroll(sv, sv->scroll + sv->step);
    return 1;
  case KEY_HOME:
    set_scroll(sv, 0);
    return 1;
  case KEY_END:
    set_scroll(sv, max_scroll(sv));
    return 1;
  }
  return 0;
}

int scrollview_press(struct scrollview *sv, int mx, int my)
{
  sv->dragging = 0;
  if (!overflows(sv))
    return 0;
  int bx = sv->x + sv->w - GUI_SCROLL_W;
  switch (scrollbar_hit(bx, sv->y, GUI_SCROLL_W, sv->h, mx, my)) {
  case SCROLLBAR_UP:
    set_scroll(sv, sv->scroll - sv->step);
    return 1;
  case SCROLLBAR_DOWN:
    set_scroll(sv, sv->scroll + sv->step);
    return 1;
  case SCROLLBAR_TRACK:
    sv->dragging = 1;
    set_scroll(sv, scrollbar_pos(bx, sv->y, GUI_SCROLL_W, sv->h, mx, my, sv->scroll, sv->content_h, sv->h));
    return 1;
  }
  return 0;
}

int scrollview_drag(struct scrollview *sv, int mx, int my)
{
  if (!sv->dragging)
    return 0;
  int bx = sv->x + sv->w - GUI_SCROLL_W;
  /* Keep following the pointer when it strays sideways off the bar */
  return set_scroll(sv, scrollbar_pos(bx, sv->y, GUI_SCROLL_W, sv->h, bx, my, sv->scroll, sv->content_h, sv->h));
}

int scrollview_reveal(struct scrollview *sv, int y, int h)
{
  if (y < sv->scroll)
    return set_scroll(sv, y);
  if (y + h > sv->scroll + sv->h)
    return set_scroll(sv, y + h - sv->h);
  return 0;
}
