/* traymenu.c: the tray icon popup menu, with cascading submenus, check boxes, disabled and hidden entries */
#include "comp.h"
#include "tray.h"

#define MENU_W 190
#define MENU_PAD 22
#define MAX_LEVELS 8
#define SEL_BG 0x2f6fb0
#define DIM_FG 0x5a6578

struct panel {
  int parent; /* entry the panel lists the children of, -1 at the root */
  int x, y, w, nvis, top, sel;
  int *rows; /* visible entries, as indexes into ent */
  int nrows;
};

bool tray_menu_open;
static struct tray_entry *ent;
static int nent;
static char owner[TRAY_ID_MAX];
static struct panel lv[MAX_LEVELS];
static int nlv;

static bool has_children(int i) { return i + 1 < nent && ent[i + 1].depth > ent[i].depth; }

static void panel_free(struct panel *p) { free(p->rows); }

static void levels_truncate(int n)
{
  while (nlv > n)
    panel_free(&lv[--nlv]);
}

void tray_menu_close(void)
{
  if (!tray_menu_open && !ent)
    return;
  levels_truncate(0);
  free(ent);
  ent = 0;
  nent = 0;
  tray_menu_open = false;
  force_full = true;
}

static void panel_open(int parent, int x, int ytop)
{
  struct panel *p = &lv[nlv];
  int d = parent < 0 ? 0 : ent[parent].depth + 1;
  p->rows = malloc((nent ? nent : 1) * sizeof(int));
  if (!p->rows)
    return;
  p->nrows = 0;
  for (int i = parent + 1; i < nent && ent[i].depth >= d; i++)
    if (ent[i].depth == d && !(ent[i].flags & TRAY_ENTRY_HIDDEN))
      p->rows[p->nrows++] = i;
  if (!p->nrows) {
    free(p->rows);
    return;
  }
  int maxvis = (screen_h - WM_BAR_H - 8) / TBM_ITEM_H;
  p->parent = parent;
  p->w = MENU_W;
  p->nvis = p->nrows < maxvis ? p->nrows : maxvis;
  p->top = 0;
  p->sel = -1;
  int h = p->nvis * TBM_ITEM_H;
  p->x = x + p->w > screen_w ? screen_w - p->w : x < 0 ? 0 : x;
  p->y = ytop + h > screen_h - WM_BAR_H ? screen_h - WM_BAR_H - h : ytop;
  if (p->y < 0)
    p->y = 0;
  nlv++;
  force_full = true;
}

void tray_menu_open_at(int cx, const char *id, struct tray_entry *e, int n)
{
  tray_menu_close();
  ent = e;
  nent = n;
  strlcpy(owner, id, sizeof owner);
  panel_open(-1, cx - MENU_W / 2, 0);
  if (!nlv) {
    tray_menu_close();
    return;
  }
  lv[0].y = screen_h - WM_BAR_H - lv[0].nvis * TBM_ITEM_H;
  tray_menu_open = true;
}

static int panel_at(int mx, int my)
{
  for (int k = nlv - 1; k >= 0; k--) {
    const struct panel *p = &lv[k];
    if (mx >= p->x && mx < p->x + p->w && my >= p->y && my < p->y + p->nvis * TBM_ITEM_H)
      return k;
  }
  return -1;
}

bool tray_menu_inside(int mx, int my) { return tray_menu_open && panel_at(mx, my) >= 0; }

/* Open the submenu of entry e (listed in level k) beside its row */
static void submenu_open(int k, int row)
{
  const struct panel *p = &lv[k];
  int e = p->rows[row];
  if (nlv == k + 2 && lv[k + 1].parent == e)
    return;
  levels_truncate(k + 1);
  if (nlv == MAX_LEVELS)
    return;
  int ry = p->y + (row - p->top) * TBM_ITEM_H;
  int rx = p->x + p->w;
  panel_open(e, rx + p->w > screen_w ? p->x - p->w : rx, ry);
}

void tray_menu_motion(int mx, int my)
{
  int k = panel_at(mx, my);
  if (k < 0)
    return;
  struct panel *p = &lv[k];
  int row = p->top + (my - p->y) / TBM_ITEM_H;
  int e = p->rows[row];
  if (p->sel != row) {
    p->sel = row;
    force_full = true;
  }
  if (has_children(e) && !(ent[e].flags & (TRAY_ENTRY_DISABLED | TRAY_ENTRY_SEPARATOR)))
    submenu_open(k, row);
  else if (nlv > k + 1) {
    levels_truncate(k + 1);
    force_full = true;
  }
}

void tray_menu_scroll(int mx, int my, int dz)
{
  int k = panel_at(mx, my);
  if (k < 0)
    return;
  struct panel *p = &lv[k];
  int top = p->top - dz;
  top = top > p->nrows - p->nvis ? p->nrows - p->nvis : top;
  top = top < 0 ? 0 : top;
  if (top != p->top) {
    p->top = top;
    levels_truncate(k + 1);
    force_full = true;
  }
}

static void run_cmd(const char *cmd)
{
  char buf[sizeof ent->cmd + 8], *arg = 0, *c;
  strlcpy(buf, cmd, sizeof buf);
  for (c = buf; *c; c++)
    if (*c == ' ') {
      *c = '\0';
      arg = c + 1;
      break;
    }
  char path[sizeof buf + 8];
  strcpy(path, buf[0] == '/' ? "" : "/bin/");
  strcat(path, buf);
  kid_track(spawn(path, arg));
}

bool tray_menu_press(int mx, int my)
{
  int k = panel_at(mx, my);
  if (k < 0) {
    tray_menu_close();
    return false;
  }
  int row = lv[k].top + (my - lv[k].y) / TBM_ITEM_H;
  int e = lv[k].rows[row];
  if (ent[e].flags & (TRAY_ENTRY_DISABLED | TRAY_ENTRY_SEPARATOR))
    return true;
  if (has_children(e)) {
    submenu_open(k, row);
    return true;
  }
  char cmd[sizeof ent->cmd], id[sizeof owner];
  strcpy(cmd, ent[e].cmd);
  strcpy(id, owner);
  bool check = ent[e].flags & TRAY_ENTRY_CHECKABLE;
  tray_menu_close();
  if (check)
    tray_menu_toggle(id, e);
  if (cmd[0])
    run_cmd(cmd);
  return true;
}

static void draw_check(int x, int y, unsigned flags, unsigned fg)
{
  if (!(flags & TRAY_ENTRY_CHECKABLE))
    return;
  gfx_rect(x, y, 10, 10, fg);
  if (flags & TRAY_ENTRY_CHECKED)
    gfx_fill_rect(x + 3, y + 3, 4, 4, fg);
}

static void draw_arrow(int x, int y, unsigned fg)
{
  for (int i = 0; i < 4; i++)
    gfx_fill_rect(x + i, y + i, 1, 9 - 2 * i, fg);
}

void tray_menu_draw(void)
{
  for (int k = 0; k < nlv; k++) {
    const struct panel *p = &lv[k];
    int h = p->nvis * TBM_ITEM_H;
    gfx_fill_rect(p->x, p->y, p->w, h, 0x141a24);
    for (int r = 0; r < p->nvis; r++) {
      int row = p->top + r, e = p->rows[row], iy = p->y + r * TBM_ITEM_H;
      unsigned fl = ent[e].flags;
      bool open_child = k + 1 < nlv && lv[k + 1].parent == e;
      if (fl & TRAY_ENTRY_SEPARATOR) {
        gfx_fill_rect(p->x + 8, iy + TBM_ITEM_H / 2, p->w - 16, 1, 0x05070a);
        continue;
      }
      bool dim = fl & TRAY_ENTRY_DISABLED;
      if (!dim && (row == p->sel || open_child))
        gfx_fill_rect(p->x + 1, iy, p->w - 2, TBM_ITEM_H, SEL_BG);
      unsigned fg = dim ? DIM_FG : 0xc8d0dc;
      draw_check(p->x + 6, iy + (TBM_ITEM_H - 10) / 2, fl, fg);
      gfx_ui_text_clip(p->x + MENU_PAD, iy + (TBM_ITEM_H - UIFONT_H) / 2, ent[e].label, fg, p->w - MENU_PAD - 16);
      if (has_children(e))
        draw_arrow(p->x + p->w - 12, iy + (TBM_ITEM_H - 9) / 2, fg);
    }
    if (p->nrows > p->nvis) {
      int bh = h * p->nvis / p->nrows, by = p->y + (h - bh) * p->top / (p->nrows - p->nvis);
      gfx_fill_rect(p->x + p->w - 3, by, 2, bh, 0x3a4658);
    }
    gfx_rect(p->x, p->y, p->w, h, 0x05070a);
  }
}
