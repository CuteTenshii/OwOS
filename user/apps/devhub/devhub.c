/* devhub.c, Device Hub: a read-only inventory of the hardware the kernel found */
#include "devhub.h"
#include "gfx.h"
#include "owos.h"

#define HEADER_H 22
#define CAT_ROW_H 26
#define SIDEBAR_W 140
#define SIDEBAR_MIN 110
#define SIDEBAR_MAX 220
#define PANE_TOP 44
#define LINE_H 20
#define HEAD_H 30
#define NOTE_GAP 6
#define PAD 16
#define INDENT_W 14
#define REFRESH_MS 2000

/* Same palette as Settings and Files, so the system apps read as one family */
#define BG_PANEL 0x12161eu
#define BG_HEADER 0x161d28u
#define BG_SEL 0x24405cu
#define LINE_RULE 0x232c3au
#define FG_TITLE 0x8aa0b8u
#define FG_TEXT 0xc8d0dcu
#define FG_DIM 0x8aa0b8u
#define FG_WHITE 0xffffffu
#define FG_ERROR 0xff9a8au

static int W = 720, H = 460;
static unsigned seq = 1;
static struct sidebar sb;
static struct scrollview sv;

static int visible[16]; /* indices into categories[], in sidebar order */
static int nvisible;
static int sel; /* index into categories[] */
static bool loading;

static struct page cur, scratch;

static int sidebar_w(void)
{
  int w = sb.w;
  if (w > W - 220)
    w = W - 220;
  return w < 0 ? 0 : w;
}

/* Re-check which categories have hardware behind them; true if the list changed. */
static bool refresh_visible(void)
{
  int v[16], n = 0;
  for (int i = 0; i < ncategories && n < 16; i++)
    if (categories[i].present())
      v[n++] = i;
  bool changed = n != nvisible;
  for (int i = 0; i < n && !changed; i++)
    changed = v[i] != visible[i];
  for (int i = 0; i < n; i++)
    visible[i] = v[i];
  nvisible = n;
  return changed;
}

static int visible_pos(int cat)
{
  for (int i = 0; i < nvisible; i++)
    if (visible[i] == cat)
      return i;
  return -1;
}

static void render(void);

/* Load the category into `pg` from scratch, zeroed so two loads of the same state compare equal. */
static void load_into(int cat, struct page *pg)
{
  memset(pg, 0, sizeof *pg);
  categories[cat].load(pg);
}

static void show(int id, int cat)
{
  if (cat != sel)
    sv.scroll = 0;
  sel = cat;
  if (categories[cat].costly) {
    /* The read probes hardware and can take a moment: say so before blocking */
    loading = true;
    render();
    win_present(id);
  }
  load_into(cat, &cur);
  loading = false;
  seq++;
}

static void step_category(int id, int dir)
{
  if (!nvisible)
    return;
  int p = visible_pos(sel);
  p = p < 0 ? 0 : (p + dir + nvisible) % nvisible;
  show(id, visible[p]);
}

/* Keep the selection on a category that still exists after hotplug. */
static void settle_selection(int id)
{
  if (visible_pos(sel) >= 0 || !nvisible)
    return;
  show(id, visible[0]);
}

static int row_h(const struct row *r)
{
  if (r->kind == ROW_HEAD)
    return HEAD_H;
  return r->kind == ROW_NOTE ? LINE_H + NOTE_GAP : LINE_H;
}

static int content_h(const struct page *pg)
{
  int h = 0;
  for (int i = 0; i < pg->n; i++)
    h += row_h(&pg->rows[i]);
  return h + PAD;
}

static void draw_rows(int x, int top, int w)
{
  int lw = w * 38 / 100;
  if (lw < 120)
    lw = 120;
  if (lw > 210)
    lw = 210;
  int y = top;
  for (int i = 0; i < cur.n; i++) {
    const struct row *r = &cur.rows[i];
    int ty = y + (LINE_H - UIFONT_H) / 2;
    switch (r->kind) {
    case ROW_HEAD:
      ty = y + HEAD_H - LINE_H + (LINE_H - UIFONT_H) / 2;
      gfx_ui_text_clip(x + PAD, ty, r->label, FG_WHITE, w - 2 * PAD);
      gfx_fill_rect(x + PAD, y + HEAD_H - 1, w - 2 * PAD, 1, LINE_RULE);
      break;
    case ROW_FIELD: {
      int ix = r->indent * INDENT_W;
      gfx_ui_text_clip(x + PAD + ix, ty, r->label, FG_DIM, lw - ix - 8);
      gfx_ui_text_clip(x + PAD + lw, ty, r->value, FG_TEXT, w - lw - 2 * PAD);
      break;
    }
    case ROW_NOTE:
      gfx_ui_text_clip(x + PAD, ty + NOTE_GAP, r->value, FG_DIM, w - 2 * PAD);
      break;
    default:
      gfx_ui_text_clip(x + PAD, ty, r->value, FG_ERROR, w - 2 * PAD);
      break;
    }
    y += row_h(r);
  }
}

static void render(void)
{
  int sbw = sidebar_w();
  gfx_clear(BG_PANEL);

  sidebar_draw(&sb, 0, 0, sbw, H, NULL);
  gfx_fill_rect(0, 0, sbw, HEADER_H, BG_HEADER);
  gfx_ui_text_clip(8, (HEADER_H - UIFONT_H) / 2, "Device Hub", FG_TITLE, sbw - 16);
  for (int i = 0; i < nvisible; i++) {
    int ry = HEADER_H + i * CAT_ROW_H;
    bool here = visible[i] == sel;
    if (here)
      gfx_fill_rect(0, ry, sbw, CAT_ROW_H, BG_SEL);
    gfx_ui_text_clip(12, ry + (CAT_ROW_H - UIFONT_H) / 2, categories[visible[i]].name, here ? FG_WHITE : FG_TEXT,
                     sbw - 20);
  }

  int cx = sbw + 1, cw = W - sbw - 1;
  const struct category *c = &categories[sel];
  gfx_ui_text_clip(cx + PAD, 14, c->name, FG_WHITE, cw / 2);
  const char *hint = c->live ? "Updates live" : "F5 to refresh";
  int hw = gfx_ui_width(hint);
  if (hw < cw / 2 - PAD)
    gfx_ui_text(cx + cw - PAD - hw, 14, hint, FG_DIM);

  int ph = H - PANE_TOP;
  if (loading) {
    gfx_ui_text_clip(cx + PAD, PANE_TOP + 4, "Reading devices...", FG_DIM, cw - 2 * PAD);
    return;
  }
  if (!cur.n) {
    gfx_ui_text_clip(cx + PAD, PANE_TOP + 4, "Nothing reported for this category.", FG_DIM, cw - 2 * PAD);
    return;
  }
  int top = scrollview_begin(&sv, cx, PANE_TOP, cw, ph, content_h(&cur));
  draw_rows(scrollview_content_x(&sv), top, scrollview_content_w(&sv));
  scrollview_end(&sv);
}

static void click(int id, int mx, int my)
{
  int sbw = sidebar_w();
  if (sidebar_press(&sb, 0, 0, sbw, H, mx, my)) {
    seq++;
    return;
  }
  if (mx < sbw) {
    if (my < HEADER_H)
      return;
    int i = (my - HEADER_H) / CAT_ROW_H;
    if (i >= 0 && i < nvisible && visible[i] != sel)
      show(id, visible[i]);
    return;
  }
  if (scrollview_press(&sv, mx, my))
    seq++;
}

static void drag(int mx, int my)
{
  if (sv.dragging) {
    if (scrollview_drag(&sv, mx, my))
      seq++;
    return;
  }
  if (sidebar_drag(&sb, 0, mx))
    seq++;
}

/* Up/Down pick a category like Settings; Shift+Up/Down, Home and End scroll the details */
static void key(int id, int k, int mods)
{
  if ((k == KEY_UP || k == KEY_DOWN) && !(mods & KMOD_SHIFT)) {
    step_category(id, k == KEY_DOWN ? 1 : -1);
    return;
  }
  if (k == '\t') {
    step_category(id, mods & KMOD_SHIFT ? -1 : 1);
    return;
  }
  if (k == KEY_F5) {
    if (refresh_visible())
      settle_selection(id);
    show(id, sel);
    return;
  }
  if (scrollview_key(&sv, k))
    seq++;
}

/* Re-read a live category and repaint only when something changed. */
static void poll(int id)
{
  if (refresh_visible()) {
    seq++;
    settle_selection(id);
  }
  if (!categories[sel].live)
    return;
  load_into(sel, &scratch);
  if (memcmp(&scratch, &cur, sizeof cur) != 0) {
    memcpy(&cur, &scratch, sizeof cur);
    seq++;
  }
}

int main(void)
{
  int id = win_create(W, H, "Device Hub");
  if (id < 0) {
    puts("devhub: no window\n");
    return 1;
  }
  gfx_init(win_fb(), W, H);
  win_resizable(id, 1);
  sidebar_init(&sb, SIDEBAR_W, SIDEBAR_MIN, SIDEBAR_MAX);
  scrollview_init(&sv, LINE_H, NULL);

  refresh_visible();
  show(id, nvisible ? visible[0] : 0);

  unsigned last_poll = msec();
  unsigned drawn = 0;
  for (;;) {
    struct win_event ev;
    while (win_poll(id, &ev)) {
      switch (ev.type) {
      case WEV_CLOSE:
        win_close(id);
        return 0;
      case WEV_MOUSE:
        click(id, ev.a, ev.b);
        break;
      case WEV_DRAG:
        drag(ev.a, ev.b);
        break;
      case WEV_SCROLL:
        if (scrollview_wheel(&sv, ev.a))
          seq++;
        break;
      case WEV_KEY:
        key(id, ev.a, ev.b);
        break;
      case WEV_RESIZE:
        W = ev.a;
        H = ev.b;
        gfx_init(win_fb(), W, H);
        seq++;
        break;
      default:
        break;
      }
    }
    if (msec() - last_poll >= REFRESH_MS) {
      last_poll = msec();
      poll(id);
    }
    if (seq != drawn) {
      render();
      win_present(id);
      drawn = seq;
    }
    win_wait(id);
  }
}
