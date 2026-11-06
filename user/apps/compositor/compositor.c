/* compositor.c: the user-space window manager and desktop */
#include "clipboard.h"
#include "compositor_damage.h"
#include "gfx.h"
#include "owos.h"
#include "pngenc.h"

#include <stdbool.h>

/* WM metrics (mirror kernel/wm.h) */
#define WM_TITLE_H 22
#define WM_BORDER 2
#define WM_BAR_H 28
#define WM_TITLE_MAX 64
#define WM_GRIP 14
#define WM_EDGE 5
#define WM_MIN_W 140
#define WM_MIN_H 120

/* Cursor shapes (mirror enum wm_cursor / WIN_CURSOR_*) */
enum { CUR_ARROW = 0, CUR_HAND, CUR_TEXT, CUR_BUSY, CUR_RESIZE_NWSE, CUR_RESIZE_NESW, CUR_RESIZE_NS, CUR_RESIZE_EW };

/* Global keys the kernel routes through wm_feed_key (mirror keyboard.h) */
#define KEY_TAB '\t'
#define KEY_ALT_UP 0x1B /* internal: "Alt released" marker (kernel wm KEY_ALT_UP) */

#define WM_LIST_MAX 64

/* screen + back buffer */
static unsigned *back;         /* mapped back buffer (COMPOSITOR_FB_ADDR) */
static int screen_w, screen_h; /* visible dimensions */
static int fb_pitch;           /* back-buffer stride in pixels (== gfx surface w) */

static int work_bottom(void) { return screen_h - WM_BAR_H; }

static void clamp_frame_position(int *x, int *y, int w, int h)
{
  if (*x + w > screen_w)
    *x = screen_w - w;
  if (*y + h > work_bottom())
    *y = work_bottom() - h;
  if (*x < 0)
    *x = 0;
  if (*y < 0)
    *y = 0;
}

static void fit_frame_to_work_area(int *x, int *y, int *w, int *h)
{
  if (*w > screen_w)
    *w = screen_w;
  if (*h > work_bottom())
    *h = work_bottom();
  clamp_frame_position(x, y, *w, *h);
}

/* Mirror of the active gfx clip rect */
static int clip_x0, clip_y0, clip_x1, clip_y1;
static void set_clip(int x, int y, int w, int h)
{
  int x1 = x + w, y1 = y + h;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (x1 > screen_w)
    x1 = screen_w;
  if (y1 > screen_h)
    y1 = screen_h;
  clip_x0 = x;
  clip_y0 = y;
  clip_x1 = x1 > x ? x1 : x;
  clip_y1 = y1 > y ? y1 : y;
  gfx_set_clip(x, y, w, h);
}
static void clip_full(void)
{
  clip_x0 = 0;
  clip_y0 = 0;
  clip_x1 = screen_w;
  clip_y1 = screen_h;
  gfx_clip_none();
}

/* window model */
/* cur[] is enumerated in slot order, then zorder_apply() sorts it bottom first */
static struct winrec cur[WM_LIST_MAX];
static int nwin;

static bool win_visible(const struct winrec *w) { return (w->flags & UWIN_F_VISIBLE) != 0; }
static bool win_minimized(const struct winrec *w) { return (w->flags & UWIN_F_MINIMIZED) != 0; }

/* Compositor-tracked maximize state (see the maxrec table below): -1 if not */
static int is_maxed(int id);
static void maxed_forget_gone(void);

/* wins index by z position i (bottom..top); cur[] is already in that order */
static int slot_of(int id)
{
  for (int i = 0; i < nwin; i++)
    if (cur[i].id == id)
      return i;
  return -1;
}

/* Compositor-owned stacking order: window ids, bottom -> top */
static int zstack[WM_LIST_MAX];
static int nz;
static bool taskbar_layout_dirty = true;
static int z_index(int id)
{
  for (int i = 0; i < nz; i++)
    if (zstack[i] == id)
      return i;
  return -1;
}
static void z_raise(int id)
{
  int p = z_index(id);
  if (p < 0)
    return;
  for (int i = p; i < nz - 1; i++)
    zstack[i] = zstack[i + 1];
  zstack[nz - 1] = id;
  taskbar_layout_dirty = true;
}
static void z_lower(int id)
{
  int p = z_index(id);
  if (p < 0)
    return;
  for (int i = p; i > 0; i--)
    zstack[i] = zstack[i - 1];
  zstack[0] = id;
  taskbar_layout_dirty = true;
}

/* window families */

/* The valid transient parent id of window `id`, or -1 */
static int parent_id_of(int id)
{
  int s = slot_of(id);
  if (s < 0)
    return -1;
  int p = cur[s].parent;
  if (p < 0 || p == id)
    return -1;
  int ps = slot_of(p);
  if (ps < 0 || cur[ps].owner_pid != cur[s].owner_pid)
    return -1;
  return p;
}

/* The top ancestor of `id` (itself if it has no valid parent) */
static int root_id_of(int id)
{
  for (int guard = 0; guard < WM_LIST_MAX; guard++) {
    int p = parent_id_of(id);
    if (p < 0)
      return id;
    id = p;
  }
  return id;
}

/* 1 if `id` is `anc` or descends from it through the parent chain */
static bool is_descendant(int id, int anc)
{
  for (int guard = 0; id >= 0 && guard < WM_LIST_MAX; guard++) {
    if (id == anc)
      return true;
    id = parent_id_of(id);
  }
  return false;
}

/* Drop closed ids, append new ones on top, then order cur[] bottom to top */
static char fam_emitted[WM_LIST_MAX];
static void emit_family(int rootslot, struct winrec *ord, int *n)
{
  ord[(*n)++] = cur[rootslot];
  fam_emitted[rootslot] = 1;
  for (int zi = 0; zi < nz; zi++) {
    int cs = slot_of(zstack[zi]);
    if (cs < 0 || fam_emitted[cs])
      continue;
    if (parent_id_of(zstack[zi]) == cur[rootslot].id)
      emit_family(cs, ord, n);
  }
}
static void zorder_apply(void)
{
  for (int i = 0; i < nz;) {
    if (slot_of(zstack[i]) < 0) {
      for (int j = i; j < nz - 1; j++)
        zstack[j] = zstack[j + 1];
      nz--;
    } else
      i++;
  }
  for (int i = 0; i < nwin; i++)
    if (z_index(cur[i].id) < 0 && nz < WM_LIST_MAX)
      zstack[nz++] = cur[i].id;

  for (int i = 0; i < nwin; i++)
    fam_emitted[i] = 0;
  struct winrec ord[WM_LIST_MAX];
  int n = 0;
  for (int zi = 0; zi < nz; zi++) {
    int s = slot_of(zstack[zi]);
    if (s < 0 || fam_emitted[s])
      continue;
    if (parent_id_of(zstack[zi]) < 0)
      emit_family(s, ord, &n);
  }
  /* Any window not reached above still gets emitted, in zstack order, so nothing vanishes */
  for (int zi = 0; zi < nz; zi++) {
    int s = slot_of(zstack[zi]);
    if (s >= 0 && !fam_emitted[s]) {
      ord[n++] = cur[s];
      fam_emitted[s] = 1;
    }
  }
  for (int i = 0; i < n; i++)
    cur[i] = ord[i];
  nwin = n;
}

/* "Not Responding": no kernel signal yet, so every window counts as responsive */
static bool not_responding(const struct winrec *w)
{
  (void)w;
  return false;
}

/* title helpers */
static void put2(char *p, int v)
{
  p[0] = (char)('0' + v / 10);
  p[1] = (char)('0' + v % 10);
}

#define WM_TITLE_DISP (WM_TITLE_MAX + 18)
static void disp_title(const struct winrec *w, char *buf)
{
  int n = 0;
  for (; w->title[n] && n < WM_TITLE_MAX - 1; n++)
    buf[n] = w->title[n];
  buf[n] = '\0';
  if (not_responding(w)) {
    static const char sfx[] = " (Not Responding)";
    for (int j = 0; sfx[j]; j++)
      buf[n++] = sfx[j];
    buf[n] = '\0';
  }
}

/* taskbar / menus */
#define START_W 70
#define SM_ITEM_H 20
#define SM_W 150
#define SM_SEP_H 9
static bool start_open;

#define TBM_W 120
#define TBM_ITEM_H 22
#define TBM_N 2
static bool tb_menu_open;
static int tb_menu_win = -1; /* targeted cur[] id */
static int tb_menu_x, tb_menu_y;
static int tbm_h(void) { return TBM_N * TBM_ITEM_H; }

#define TB_BTN_W 14
#define TB_BTN_GAP 3
#define TB_WIN_GAP 4
static int tb_band_right;
static unsigned frame_clock;
static int tb_band_left(void) { return START_W + 6; }

/* Start menu apps */
enum {
  APP_TERM,
  APP_FILES,
  APP_EDITOR,
  APP_BROWSER,
  APP_PLAYER,
  APP_CLOCK,
  APP_CALC,
  APP_NOTIFYD,
  APP_SETTINGS,
  APP_ABOUT
};
static const struct {
  const char *name;
  const char *bin;
} app_menu[] = {
    [APP_TERM] = {"Terminal", "term"},         [APP_FILES] = {"Files", "files"},
    [APP_EDITOR] = {"Editor", "editor"},       [APP_BROWSER] = {"Browser", "browser"},
    [APP_PLAYER] = {"Media Player", "player"}, [APP_CLOCK] = {"Clock", "clock"},
    [APP_CALC] = {"Calculator", "calc"},       [APP_NOTIFYD] = {"Notifications", "notifyd"},
    [APP_SETTINGS] = {"Settings", "settings"}, [APP_ABOUT] = {"About", "about"},
};
static int app_pid[sizeof app_menu / sizeof app_menu[0]];
static int apps_menu_count(void) { return (int)(sizeof app_menu / sizeof app_menu[0]); }

/* Power actions */
enum { PA_SLEEP, PA_RESTART, PA_SHUTDOWN };
static const char *const power_label[] = {"Sleep", "Restart", "Shut Down"};
static bool can_sleep, resumed;

static void power_run(int action)
{
  if (action == PA_SLEEP) {
    power(2);
    resumed = true; /* main loop resyncs input and pacing */
  } else {
    power(action == PA_RESTART ? 1 : 0);
  }
}

static int sm_power_n(void) { return can_sleep ? 3 : 2; }
static int sm_power_action(int row) { return can_sleep ? row : row + 1; }

static int sm_apps_h(void) { return apps_menu_count() * SM_ITEM_H; }
static int sm_h(void) { return sm_apps_h() + SM_SEP_H + sm_power_n() * SM_ITEM_H; }
static int sm_top(void) { return screen_h - WM_BAR_H - sm_h(); }

/* Power overlay: Sleep, Restart, Shut Down, Cancel */
#define PW_W 240
#define PW_TITLE_H 40
#define PW_ROW_H 32
#define PW_PAD 8
#define PW_ROWS 4
#define PW_CANCEL 3
static bool pw_open;
static int pw_sel;
static int pw_x(void) { return (screen_w - PW_W) / 2; }
static int pw_h(void) { return PW_TITLE_H + PW_ROWS * PW_ROW_H + PW_PAD; }
static int pw_y(void) { return (screen_h - pw_h()) / 2; }
static bool pw_enabled(int row) { return row != PA_SLEEP || can_sleep; }
static bool pw_inside(int mx, int my)
{
  return mx >= pw_x() && mx < pw_x() + PW_W && my >= pw_y() && my < pw_y() + pw_h();
}
static int pw_row_at(int mx, int my)
{
  int rel = my - pw_y() - PW_TITLE_H;
  if (mx < pw_x() + PW_PAD || mx >= pw_x() + PW_W - PW_PAD || rel < 0 || rel >= PW_ROWS * PW_ROW_H)
    return -1;
  return rel / PW_ROW_H;
}

static bool force_full;

/* child reaping */
#define MAXKIDS 64
static int kids[MAXKIDS];
static int nkids;

static void kid_track(int pid)
{
  if (pid > 0 && nkids < MAXKIDS)
    kids[nkids++] = pid;
}

/* Reap exited children non-blocking so zombies don't accumulate */
static void reap_kids(void)
{
  int st;
  for (int i = 0; i < nkids;) {
    if (waitpid_nb(kids[i], &st))
      kids[i] = kids[--nkids];
    else
      i++;
  }
}

static void launch_app(int i)
{
  if (i < 0 || i >= apps_menu_count())
    return;
  char path[64];
  int j = 0;
  const char *pfx = "/bin/";
  for (int k = 0; pfx[k]; k++)
    path[j++] = pfx[k];
  for (int k = 0; app_menu[i].bin[k] && j < (int)sizeof path - 1; k++)
    path[j++] = app_menu[i].bin[k];
  path[j] = '\0';
  int pid = spawn(path, 0);
  app_pid[i] = pid;
  kid_track(pid);
}

/* cursors */
static const char *const arrow_glyph[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ",
    "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ",
    "X    X..X   ", "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};
static const char *const hand_glyph[] = {
    "  XX     ", "  X.X    ", "  X.X    ", "  X.X    ", "  X.XXXX ", "  X.X..X ", "XXX...X.X",
    "X.......X", "X.......X", "X.......X", " X......X", " X......X", "  X.....X", "  XXXXXXX",
};
static const char *const text_glyph[] = {
    "X...X", "XX.XX", " X.X ", " X.X ", " X.X ", " X.X ", " X.X ",
    " X.X ", " X.X ", " X.X ", " X.X ", " X.X ", "XX.XX", "X...X",
};
struct cur_shape {
  const char *const *rows;
  int nrows, hotx, hoty;
};
static const struct cur_shape cur_shapes[] = {
    [CUR_ARROW] = {arrow_glyph, 19, 0, 0},
    [CUR_HAND] = {hand_glyph, 14, 2, 0},
    [CUR_TEXT] = {text_glyph, 14, 2, 7},
};
#define CUR_DMG_PAD 9
#define CUR_DMG_W 24
#define CUR_DMG_H 30
static unsigned cursor_frame;

/* z / hit-testing */

/* Topmost visible window's cur[] slot (holds focus), or -1 */
static int topmost_visible(void)
{
  for (int i = nwin - 1; i >= 0; i--)
    if (win_visible(&cur[i]))
      return i;
  return -1;
}

/* Topmost visible window containing (px,py), or -1 */
static int window_at(int px, int py)
{
  for (int i = nwin - 1; i >= 0; i--) {
    struct winrec *w = &cur[i];
    if (!win_visible(w))
      continue;
    if (px >= w->x && px < w->x + w->w && py >= w->y && py < w->y + w->h)
      return i;
  }
  return -1;
}

/* Raise a window's whole family to the front: the root comes forward */
static void raise_family_id(int id)
{
  int root = root_id_of(id);
  z_raise(root);
  if (id != root)
    z_raise(id);
}
static void raise_window(int slot)
{
  if (slot >= 0 && slot < nwin)
    raise_family_id(cur[slot].id);
}

/* modality */

/* Topmost visible modal window in `id`'s family, or -1 */
static int family_active_modal(int id)
{
  int root = root_id_of(id);
  int mid = -1;
  for (int i = 0; i < nwin; i++) {
    if (!(cur[i].flags & UWIN_F_MODAL) || !win_visible(&cur[i]))
      continue;
    if (root_id_of(cur[i].id) == root)
      mid = cur[i].id;
  }
  return mid;
}

/* If interacting with window slot `idx` is blocked by a modal dialog, the modal's id; else -1 */
static int blocking_modal(int idx)
{
  if (idx < 0)
    return -1;
  int mid = family_active_modal(cur[idx].id);
  if (mid < 0 || is_descendant(cur[idx].id, mid))
    return -1;
  return mid;
}

/* A modal being flashed to signal it holds input; counts down in frames */
static int flash_id = -1, flash_frames;
static void flash_modal(int id)
{
  flash_id = id;
  flash_frames = 8;
  force_full = true;
}

/* Alt+Tab */
static bool at_active;
static int at_order[WM_LIST_MAX]; /* window ids, most-recent first */
static int at_count;
static int at_sel;

static int alttab_build(void)
{
  int n = 0;
  for (int i = nwin - 1; i >= 0 && n < WM_LIST_MAX; i--) {
    struct winrec *w = &cur[i];
    if (parent_id_of(w->id) >= 0)
      continue; /* dialogs switch with their app */
    if (win_visible(w) || win_minimized(w))
      at_order[n++] = w->id;
  }
  return n;
}

/* title buttons */
static bool btn_present(const struct winrec *w, int kind)
{
  /* Dialogs (transient children) get only a close box and stay movable */
  bool is_dialog = parent_id_of(w->id) >= 0;
  if (kind == 0)
    return true; /* close: always */
  if (kind == 1)
    return (w->flags & UWIN_F_RESIZABLE) != 0; /* maximize: resizable */
  return !is_dialog;                           /* minimize: not dialogs */
}
static int btn_slot(const struct winrec *w, int kind)
{
  if (!btn_present(w, kind))
    return -1;
  int slot = 0;
  if (kind > 0 && btn_present(w, 0))
    slot++;
  if (kind > 1 && btn_present(w, 1))
    slot++;
  return slot;
}
static int titlebtn_box(const struct winrec *w, int kind, int *bx, int *by)
{
  int slot = btn_slot(w, kind);
  if (slot < 0)
    return 0;
  *bx = w->x + w->w - 4 - (slot + 1) * TB_BTN_W - slot * TB_BTN_GAP;
  *by = w->y + 4;
  return 1;
}
static int titlebtn_hit(const struct winrec *w, int mx, int my)
{
  for (int k = 0; k < 3; k++) {
    int bx, by;
    if (!titlebtn_box(w, k, &bx, &by))
      continue;
    if (mx >= bx && mx < bx + TB_BTN_W && my >= by && my < by + TB_BTN_W)
      return k;
  }
  return -1;
}

/* taskbar layout */
static int taskbar_layout(int left, int right, int *idxs, int *bxs, int *bws, int max)
{
  int n = 0;
  /* One button per top-level window */
  for (int i = 0; i < nwin && n < max; i++)
    if (parent_id_of(cur[i].id) < 0)
      idxs[n++] = i;
  if (n == 0)
    return 0;
  int avail = right - left;
  if (avail < n)
    return 0;
  int bw = avail / n;
  if (bw > 168)
    bw = 168;
  for (int k = 0; k < n; k++) {
    bxs[k] = left + k * bw;
    bws[k] = bw - TB_WIN_GAP;
    if (bws[k] < 1)
      bws[k] = 1;
  }
  return n;
}

struct taskbar_cache {
  bool valid;
  int right, n;
  int idxs[WM_LIST_MAX], bxs[WM_LIST_MAX], bws[WM_LIST_MAX];
};
static struct taskbar_cache tb_cache;

static void taskbar_layout_cached(int right)
{
  if (!taskbar_layout_dirty && tb_cache.valid && tb_cache.right == right)
    return;
  tb_cache.right = right;
  tb_cache.n = taskbar_layout(tb_band_left(), right, tb_cache.idxs, tb_cache.bxs, tb_cache.bws, WM_LIST_MAX);
  tb_cache.valid = true;
  taskbar_layout_dirty = false;
}

static int taskbar_hit(int mx, int my)
{
  if (my < screen_h - WM_BAR_H)
    return -1;
  taskbar_layout_cached(tb_band_right);
  for (int k = 0; k < tb_cache.n; k++)
    if (mx >= tb_cache.bxs[k] && mx < tb_cache.bxs[k] + tb_cache.bws[k])
      return tb_cache.idxs[k];
  return -1;
}

/* rendering */

static void ui_text_clip(int x, int y, const char *s, unsigned col, int maxw);

/* Vertical gradient (deep indigo -> slate), a function of y only: one colour per screen row */
static unsigned *bg_rows;
static int bg_rows_h;

static void bg_rows_build(void)
{
  free(bg_rows);
  bg_rows = malloc((unsigned)screen_h * sizeof *bg_rows);
  bg_rows_h = bg_rows ? screen_h : 0;
  for (int y = 0; y < bg_rows_h; y++) {
    int t = y * 255 / screen_h;
    unsigned r = (unsigned)(24 + t * 40 / 255);
    unsigned g = (unsigned)(26 + t * 56 / 255);
    unsigned b = (unsigned)(54 + t * 70 / 255);
    bg_rows[y] = (r << 16) | (g << 8) | b;
  }
}

static void draw_background(void)
{
  if (bg_rows_h != screen_h)
    bg_rows_build();
  if (!bg_rows_h)
    return;
  for (int y = clip_y0; y < clip_y1; y++)
    gfx_fill_rect(0, y, screen_w, 1, bg_rows[y]);
}

/* Blit a window's client buffer into the back buffer at the client rect */
static void blit_client(int dx, int dy, const unsigned *src, int sw, int sh, int sstride)
{
  long long x1 = (long long)dx + sw, y1 = (long long)dy + sh;
  int x0 = dx > clip_x0 ? dx : clip_x0;
  int y0 = dy > clip_y0 ? dy : clip_y0;
  int x2 = x1 <= 0 ? 0 : x1 >= clip_x1 ? clip_x1 : (int)x1;
  int y2 = y1 <= 0 ? 0 : y1 >= clip_y1 ? clip_y1 : (int)y1;
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x2 > screen_w)
    x2 = screen_w;
  if (y2 > screen_h)
    y2 = screen_h;
  if (sw <= 0 || sh <= 0 || x2 <= x0 || y2 <= y0)
    return;

  int src_x = x0 - dx;
  for (int y = y0; y < y2; y++)
    memcpy(back + (long)y * fb_pitch + x0, src + (long)(y - dy) * sstride + src_x, (unsigned)(x2 - x0) * 4);
}

/* the per-window client buffer */
struct clientmap {
  int id;
  bool valid;
  struct winmap m;
};
static struct clientmap cmap[WM_LIST_MAX];
static int ncmap;

static const struct winmap *client_for(int id)
{
  for (int i = 0; i < ncmap; i++)
    if (cmap[i].id == id)
      return cmap[i].valid ? &cmap[i].m : 0;
  return 0;
}

/* Remap every enumerated window and unmap ids that are gone so the kernel can reclaim frames */
static void refresh_maps(void)
{
  /* drop maps for gone windows */
  for (int i = 0; i < ncmap;) {
    if (slot_of(cmap[i].id) < 0) {
      win_unmap(cmap[i].id);
      cmap[i] = cmap[--ncmap];
    } else
      i++;
  }
  /* (re)map every enumerated window */
  for (int s = 0; s < nwin; s++) {
    int id = cur[s].id, found = -1;
    for (int i = 0; i < ncmap; i++)
      if (cmap[i].id == id) {
        found = i;
        break;
      }
    if (found < 0) {
      if (ncmap >= WM_LIST_MAX)
        continue;
      found = ncmap++;
      cmap[found].id = id;
    }
    if (!cmap[found].valid || cmap[found].m.w != cur[s].buf_w || cmap[found].m.h != cur[s].buf_h)
      cmap[found].valid = (win_map(id, &cmap[found].m) == 0);
  }
  maxed_forget_gone(); /* clear maximize state for closed windows */
}

static void draw_close_button(int bx, int by, unsigned bg)
{
  gfx_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, bg);
  for (int d = 0; d < 7; d++) {
    gfx_pixel(bx + 4 + d, by + 4 + d, 0xffffff);
    gfx_pixel(bx + 10 - d, by + 4 + d, 0xffffff);
  }
}

static void client_rect(const struct winrec *w, int *x, int *y, int *width, int *height)
{
  *x = w->x + WM_BORDER;
  *y = w->y + WM_TITLE_H;
  if (width)
    *width = w->w - 2 * WM_BORDER;
  if (height)
    *height = w->h - WM_TITLE_H - WM_BORDER;
}

static void draw_window(const struct winrec *w, bool focused)
{
  unsigned title_bg = focused ? 0x2f6fb0 : 0x55606e;
  unsigned frame = 0x10141a;
  if (w->id == flash_id && flash_frames > 0 && ((flash_frames >> 1) & 1))
    title_bg = 0xf0a020;

  gfx_fill_rect(w->x, w->y, w->w, w->h, frame);
  gfx_fill_rect(w->x, w->y, w->w, WM_TITLE_H, title_bg);

  int title_x = w->x + 8;
  int title_right = w->x + w->w - 4;
  for (int k = 0; k < 3; k++) {
    int bbx, bby;
    if (titlebtn_box(w, k, &bbx, &bby) && bbx < title_right)
      title_right = bbx;
  }
  char dt[WM_TITLE_DISP];
  disp_title(w, dt);
  ui_text_clip(title_x, w->y + (WM_TITLE_H - UIFONT_H) / 2, dt, 0xffffff, title_right - title_x - 6);

  int bx, by;
  if (titlebtn_box(w, 2, &bx, &by)) { /* minimize */
    gfx_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0x3a4658);
    gfx_fill_rect(bx + 3, by + 9, 8, 2, 0xffffff);
  }
  if (titlebtn_box(w, 1, &bx, &by)) { /* maximize / restore */
    gfx_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0x3a4658);
    if (is_maxed(w->id) >= 0) {
      gfx_rect(bx + 4, by + 2, 6, 6, 0xffffff);
      gfx_fill_rect(bx + 2, by + 5, 6, 6, 0x3a4658);
      gfx_rect(bx + 2, by + 5, 6, 6, 0xffffff);
    } else {
      gfx_rect(bx + 3, by + 3, 8, 8, 0xffffff);
    }
  }
  if (titlebtn_box(w, 0, &bx, &by)) { /* close */
    draw_close_button(bx, by, 0xc04030);
  }

  /* Client area: blit the app's pixel buffer, or fill the chrome bg if unmapped */
  int cx, cy, cw, ch;
  client_rect(w, &cx, &cy, &cw, &ch);
  const struct winmap *cm = client_for(w->id);
  if (cm && cm->vaddr) {
    int bw = cm->w < cw ? cm->w : cw;
    int bh = cm->h < ch ? cm->h : ch;
    blit_client(cx, cy, (const unsigned *)cm->vaddr, bw, bh, cm->stride);
    if (bw < cw)
      gfx_fill_rect(cx + bw, cy, cw - bw, ch, 0x10141a);
    if (bh < ch)
      gfx_fill_rect(cx, cy + bh, cw, ch - bh, 0x10141a);
  } else {
    gfx_fill_rect(cx, cy, cw, ch, 0x10141a);
  }

  gfx_rect(w->x, w->y, w->w, w->h, 0x05070a);
}

/* cursor drawing (ported from wm.c) */
static void draw_glyph(const struct cur_shape *s, int x, int y)
{
  for (int row = 0; row < s->nrows; row++) {
    const char *line = s->rows[row];
    for (int col = 0; line[col]; col++) {
      if (line[col] == 'X')
        gfx_pixel(x + col, y + row, 0x000000);
      else if (line[col] == '.')
        gfx_pixel(x + col, y + row, 0xffffff);
    }
  }
}
static void cur_dot(int cx, int cy, unsigned inner)
{
  gfx_fill_rect(cx - 2, cy - 2, 4, 4, 0x000000);
  gfx_fill_rect(cx - 1, cy - 1, 2, 2, inner);
}
static void draw_busy(int mx, int my)
{
  static const int dx[8] = {0, 4, 6, 4, 0, -4, -6, -4};
  static const int dy[8] = {-6, -4, 0, 4, 6, 4, 0, -4};
  int head = (int)((cursor_frame / 3) & 7);
  for (int i = 0; i < 8; i++) {
    int dist = (head - i + 8) & 7;
    int b = 40 + (7 - dist) * 30;
    if (b > 255)
      b = 255;
    cur_dot(mx + dx[i], my + dy[i], (unsigned)((b << 16) | (b << 8) | b));
  }
}
static void cur_stroke(int x, int y, int halo)
{
  if (halo) {
    gfx_pixel(x, y, 0xffffff);
    gfx_pixel(x + 1, y, 0xffffff);
    gfx_pixel(x - 1, y, 0xffffff);
    gfx_pixel(x, y + 1, 0xffffff);
    gfx_pixel(x, y - 1, 0xffffff);
  } else
    gfx_pixel(x, y, 0x000000);
}
static void draw_resize_nwse(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--) {
    for (int t = -5; t <= 5; t++)
      cur_stroke(mx + t, my + t, halo);
    for (int k = 0; k <= 4; k++) {
      cur_stroke(mx - 5 + k, my - 5, halo);
      cur_stroke(mx - 5, my - 5 + k, halo);
      cur_stroke(mx + 5 - k, my + 5, halo);
      cur_stroke(mx + 5, my + 5 - k, halo);
    }
  }
}
static void draw_resize_nesw(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--) {
    for (int t = -5; t <= 5; t++)
      cur_stroke(mx + t, my - t, halo);
    for (int k = 0; k <= 4; k++) {
      cur_stroke(mx + 5 - k, my - 5, halo);
      cur_stroke(mx + 5, my - 5 + k, halo);
      cur_stroke(mx - 5 + k, my + 5, halo);
      cur_stroke(mx - 5, my + 5 - k, halo);
    }
  }
}
static void draw_resize_ns(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--) {
    for (int t = -6; t <= 6; t++)
      cur_stroke(mx, my + t, halo);
    for (int k = 0; k <= 4; k++) {
      cur_stroke(mx - k, my - 6 + k, halo);
      cur_stroke(mx + k, my - 6 + k, halo);
      cur_stroke(mx - k, my + 6 - k, halo);
      cur_stroke(mx + k, my + 6 - k, halo);
    }
  }
}
static void draw_resize_ew(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--) {
    for (int t = -6; t <= 6; t++)
      cur_stroke(mx + t, my, halo);
    for (int k = 0; k <= 4; k++) {
      cur_stroke(mx - 6 + k, my - k, halo);
      cur_stroke(mx - 6 + k, my + k, halo);
      cur_stroke(mx + 6 - k, my - k, halo);
      cur_stroke(mx + 6 - k, my + k, halo);
    }
  }
}
static void draw_cursor(int kind, int mx, int my)
{
  switch (kind) {
  case CUR_BUSY:
    draw_busy(mx, my);
    return;
  case CUR_RESIZE_NWSE:
    draw_resize_nwse(mx, my);
    return;
  case CUR_RESIZE_NESW:
    draw_resize_nesw(mx, my);
    return;
  case CUR_RESIZE_NS:
    draw_resize_ns(mx, my);
    return;
  case CUR_RESIZE_EW:
    draw_resize_ew(mx, my);
    return;
  default:
    if (kind < 0 || kind >= (int)(sizeof cur_shapes / sizeof cur_shapes[0]) || !cur_shapes[kind].rows)
      kind = CUR_ARROW;
    const struct cur_shape *s = &cur_shapes[kind];
    draw_glyph(s, mx - s->hotx, my - s->hoty);
    return;
  }
}

static void ui_text_clip(int x, int y, const char *s, unsigned col, int maxw)
{
  char buf[WM_TITLE_MAX + 2];
  int n = 0;
  for (; s[n] && n < WM_TITLE_MAX; n++)
    buf[n] = s[n];
  buf[n] = '\0';
  if (maxw <= 0)
    return;
  if (gfx_ui_width(buf) <= maxw) {
    gfx_ui_text(x, y, buf, col);
    return;
  }
  while (n > 0) {
    buf[n] = '.';
    buf[n + 1] = '.';
    buf[n + 2] = '\0';
    if (gfx_ui_width(buf) <= maxw)
      break;
    buf[n] = '\0';
    n--;
  }
  if (n > 0)
    gfx_ui_text(x, y, buf, col);
}

static void draw_taskbar_windows(int y, int uy, int right)
{
  tb_band_right = right;
  taskbar_layout_cached(right);
  int focus = topmost_visible();
  int focus_root = focus >= 0 ? root_id_of(cur[focus].id) : -1;
  int by = y + 3, bh = WM_BAR_H - 6;
  for (int k = 0; k < tb_cache.n; k++) {
    struct winrec *w = &cur[tb_cache.idxs[k]];
    bool active = (w->id == focus_root);
    unsigned bg = active ? 0x2f6fb0 : win_minimized(w) ? 0x12161e : 0x1d2533;
    gfx_fill_rect(tb_cache.bxs[k], by, tb_cache.bws[k], bh, bg);
    gfx_fill_rect(tb_cache.bxs[k], by, tb_cache.bws[k], 1, active ? 0x4f8fd0 : 0x2a3550);
    unsigned fg = win_minimized(w) ? 0x8b94a4 : 0xc8d0dc;
    char dt[WM_TITLE_DISP];
    disp_title(w, dt);
    ui_text_clip(tb_cache.bxs[k] + 8, uy, dt, fg, tb_cache.bws[k] - 12);
  }
}

static void draw_taskbar(void)
{
  int y = screen_h - WM_BAR_H;
  gfx_fill_rect(0, y, screen_w, WM_BAR_H, 0x0c0e14);
  gfx_fill_rect(0, y, screen_w, 1, 0x2a3550);
  unsigned btn = start_open ? 0x2f6fb0 : 0x0c0e14;
  int uy = y + (WM_BAR_H - UIFONT_H) / 2;
  gfx_fill_rect(0, y + 1, START_W, WM_BAR_H - 1, btn);
  gfx_ui_text(10, uy, "OwOS", 0x9ad0ff);

  unsigned t = frame_clock;
  char s[9] = "00:00:00";
  put2(s + 0, (int)((t >> 16) & 0xFF));
  put2(s + 3, (int)((t >> 8) & 0xFF));
  put2(s + 6, (int)(t & 0xFF));
  int clock_x = screen_w - gfx_ui_width(s) - 12;
  gfx_ui_text(clock_x, uy, s, 0xc8d0dc);

  draw_taskbar_windows(y, uy, clock_x - 10);
}

static void draw_start_menu(void)
{
  int n = apps_menu_count();
  int top = sm_top();
  gfx_fill_rect(0, top, SM_W, sm_h(), 0x141a24);
  for (int i = 0; i < n; i++) {
    int iy = top + i * SM_ITEM_H;
    gfx_ui_text(10, iy + (SM_ITEM_H - UIFONT_H) / 2, app_menu[i].name, 0xc8d0dc);
  }
  int sep_y = top + sm_apps_h() + SM_SEP_H / 2;
  gfx_fill_rect(8, sep_y, SM_W - 16, 1, 0x05070a);
  int py = top + sm_apps_h() + SM_SEP_H;
  for (int i = 0; i < sm_power_n(); i++) {
    int iy = py + i * SM_ITEM_H;
    gfx_ui_text(10, iy + (SM_ITEM_H - UIFONT_H) / 2, power_label[sm_power_action(i)], 0xa8b4c4);
  }
  gfx_rect(0, top, SM_W, sm_h(), 0x05070a);
}

static void draw_tb_menu(void)
{
  static const char *const items[TBM_N] = {"Close", "Force Kill"};
  int x = tb_menu_x, y = tb_menu_y;
  gfx_fill_rect(x, y, TBM_W, tbm_h(), 0x141a24);
  for (int i = 0; i < TBM_N; i++) {
    int iy = y + i * TBM_ITEM_H;
    unsigned fg = (i == 1) ? 0xe08070 : 0xc8d0dc;
    gfx_ui_text(x + 12, iy + (TBM_ITEM_H - UIFONT_H) / 2, items[i], fg);
  }
  gfx_rect(x, y, TBM_W, tbm_h(), 0x05070a);
}

/* Alt+Tab overlay */
#define AT_ROW_H 24
#define AT_PAD 12
#define AT_PANEL_W 340
static void switcher_box(int *px, int *py, int *pw, int *ph)
{
  int h = AT_PAD * 2 + at_count * AT_ROW_H;
  int maxh = screen_h - WM_BAR_H - 20;
  if (h > maxh)
    h = maxh;
  *pw = AT_PANEL_W;
  *ph = h;
  *px = (screen_w - AT_PANEL_W) / 2;
  *py = (screen_h - h) / 2;
}
static void draw_switcher(void)
{
  int x, y, pw, ph;
  switcher_box(&x, &y, &pw, &ph);
  gfx_fill_rect(x, y, pw, ph, 0x141a24);
  gfx_rect(x, y, pw, ph, 0x05070a);
  gfx_rect(x + 1, y + 1, pw - 2, ph - 2, 0x2a3550);
  int rows = (ph - 2 * AT_PAD) / AT_ROW_H;
  if (rows > at_count)
    rows = at_count;
  for (int i = 0; i < rows; i++) {
    int ry = y + AT_PAD + i * AT_ROW_H;
    if (i == at_sel)
      gfx_fill_rect(x + 4, ry, pw - 8, AT_ROW_H, 0x2f6fb0);
    int s = slot_of(at_order[i]);
    if (s < 0)
      continue;
    struct winrec *w = &cur[s];
    unsigned fg = (i == at_sel) ? 0xffffff : 0xc8d0dc;
    ui_text_clip(x + 14, ry + (AT_ROW_H - UIFONT_H) / 2, w->title, fg, pw - 28);
  }
}

/* crash windows: rendered directly (no kernel window) */
struct crashwin {
  bool used;
  int x, y, w, h;
  struct crash_rec rec;
};
#define MAXCRASHWIN 4
static struct crashwin crashwins[MAXCRASHWIN];

static void crash_ingest(void)
{
  struct crash_rec recs[MAXCRASHWIN];
  int n = crash_poll(recs, MAXCRASHWIN);
  for (int i = 0; i < n; i++) {
    for (int s = 0; s < MAXCRASHWIN; s++) {
      if (crashwins[s].used)
        continue;
      crashwins[s].used = true;
      crashwins[s].x = 140 + s * 22;
      crashwins[s].y = 110 + s * 22;
      crashwins[s].w = 460;
      crashwins[s].h = 300;
      crashwins[s].rec = recs[i];
      force_full = true;
      break;
    }
  }
}

static const char *hex8(unsigned v, char *out)
{
  static const char d[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++)
    out[i] = d[(v >> ((7 - i) * 4)) & 0xF];
  out[8] = '\0';
  return out;
}
static void draw_crashwin(struct crashwin *c)
{
  unsigned bg = 0x1a1020;
  int x0 = c->x, y0 = c->y;
  gfx_fill_rect(x0, y0, c->w, WM_TITLE_H, 0xc04030);
  gfx_ui_text(x0 + 8, y0 + (WM_TITLE_H - UIFONT_H) / 2, "crash", 0xffffff);
  /* close box (X) top-right */
  int bx = x0 + c->w - 4 - TB_BTN_W, by = y0 + 4;
  draw_close_button(bx, by, 0x902020);
  int cy = y0 + WM_TITLE_H;
  gfx_fill_rect(x0, cy, c->w, c->h - WM_TITLE_H, bg);

  struct crash_rec *r = &c->rec;
  char hx[12];
  int x = x0 + 12, y = cy + 10, lh = 16;
  char title[48];
  int t = 0;
  for (; r->name[t] && t < 23; t++)
    title[t] = r->name[t];
  const char *suf = " has crashed.";
  for (int k = 0; suf[k] && t < 47; k++)
    title[t++] = suf[k];
  title[t] = '\0';
  gfx_ui_text(x, y, title, 0xff8080);
  y += lh + 8;

  gfx_ui_text(x, y, "fault vector:", 0x9098a0);
  char vs[4];
  put2(vs, (int)(r->vector % 100));
  vs[2] = 0;
  gfx_ui_text(x + 120, y, vs, 0xe0e6ee);
  y += lh;
  gfx_ui_text(x, y, "eip=", 0x9098a0);
  gfx_ui_text(x + 40, y, hex8(r->eip, hx), 0x66ddff);
  if (r->vector == 14) {
    gfx_ui_text(x + 170, y, "cr2=", 0x9098a0);
    gfx_ui_text(x + 210, y, hex8(r->cr2, hx), 0xffe08a);
  }
  y += lh;
  gfx_ui_text(x, y, "err=", 0x9098a0);
  gfx_ui_text(x + 40, y, hex8(r->err, hx), 0xc8d0dc);
  y += lh + 8;
  gfx_ui_text(x, y, "stack trace:", 0x9098a0);
  y += lh;
  for (int i = 0; i < r->ntrace && i < CRASH_TRACE_MAX; i++) {
    gfx_ui_text(x + 12, y, hex8(r->trace[i], hx), 0xb0c0d0);
    if (i == 0)
      gfx_ui_text(x + 12 + 90, y, "<- fault", 0x707880);
    y += lh;
  }
}

/* cursor pick */
enum { RZ_L = 1, RZ_R = 2, RZ_T = 4, RZ_B = 8 };

static bool hover_resizing;
static int rsz_edge_hint;

static int resize_edge(const struct winrec *w, int mx, int my)
{
  if (is_maxed(w->id) >= 0)
    return 0;
  int e = 0;
  if (mx < w->x + WM_EDGE)
    e |= RZ_L;
  else if (mx >= w->x + w->w - WM_EDGE)
    e |= RZ_R;
  if (my < w->y + WM_EDGE)
    e |= RZ_T;
  else if (my >= w->y + w->h - WM_EDGE)
    e |= RZ_B;
  return e;
}

static int resize_hit(const struct winrec *w, int mx, int my)
{
  if (!(w->flags & UWIN_F_RESIZABLE))
    return 0;
  if (mx >= w->x + w->w - WM_GRIP && mx < w->x + w->w && my >= w->y + w->h - WM_GRIP && my < w->y + w->h)
    return RZ_R | RZ_B;
  return resize_edge(w, mx, my);
}

static int edge_cursor(int e)
{
  switch (e) {
  case RZ_L:
  case RZ_R:
    return CUR_RESIZE_EW;
  case RZ_T:
  case RZ_B:
    return CUR_RESIZE_NS;
  case RZ_L | RZ_T:
  case RZ_R | RZ_B:
    return CUR_RESIZE_NWSE;
  case RZ_R | RZ_T:
  case RZ_L | RZ_B:
    return CUR_RESIZE_NESW;
  }
  return CUR_ARROW;
}
static int pick_cursor(int mx, int my)
{
  if (pw_open) {
    int row = pw_row_at(mx, my);
    return row >= 0 && pw_enabled(row) ? CUR_HAND : CUR_ARROW;
  }
  if (hover_resizing)
    return edge_cursor(rsz_edge_hint ? rsz_edge_hint : (RZ_R | RZ_B));

  if (tb_menu_open && mx >= tb_menu_x && mx < tb_menu_x + TBM_W && my >= tb_menu_y && my < tb_menu_y + tbm_h())
    return CUR_HAND;
  int bar_y = screen_h - WM_BAR_H;
  if (my >= bar_y) {
    if (mx >= 0 && mx < START_W)
      return CUR_HAND;
    return taskbar_hit(mx, my) >= 0 ? CUR_HAND : CUR_ARROW;
  }
  if (start_open && mx >= 0 && mx < SM_W && my >= sm_top())
    return CUR_HAND;
  int idx = window_at(mx, my);
  if (idx >= 0) {
    struct winrec *w = &cur[idx];
    if (titlebtn_hit(w, mx, my) >= 0)
      return CUR_HAND;
    int e = resize_hit(w, mx, my);
    if (e)
      return edge_cursor(e);
  }
  return CUR_ARROW;
}

/* compose */
static bool clip_hits(int x, int y, int w, int h)
{
  return x < clip_x1 && x + w > clip_x0 && y < clip_y1 && y + h > clip_y0;
}

static bool occluded(int i)
{
  const struct winrec *w = &cur[i];
  int x0 = w->x > clip_x0 ? w->x : clip_x0, y0 = w->y > clip_y0 ? w->y : clip_y0;
  int x1 = w->x + w->w < clip_x1 ? w->x + w->w : clip_x1;
  int y1 = w->y + w->h < clip_y1 ? w->y + w->h : clip_y1;
  for (int j = i + 1; j < nwin; j++) {
    const struct winrec *o = &cur[j];
    if (win_visible(o) && o->x <= x0 && o->y <= y0 && o->x + o->w >= x1 && o->y + o->h >= y1)
      return true;
  }
  return false;
}

/* volume / brightness on-screen display */
#define OSD_W 220
#define OSD_H 44
#define OSD_MS 1500
#define OSD_BG 0x141a24
#define OSD_FG 0xe6ebf2
#define OSD_DIM 0x3a4458
#define OSD_ACCENT 0x5aa0ff
enum { OSD_NONE, OSD_VOLUME, OSD_BRIGHTNESS };
static int osd_kind, osd_level = -1; /* level 0..100, -1 = unreadable */
static bool osd_muted, osd_dirty;
static unsigned osd_until;

static int osd_x(void) { return (screen_w - OSD_W) / 2; }
static int osd_y(void) { return screen_h - WM_BAR_H - OSD_H - 24; }

static int sysfs_int(const char *path)
{
  char b[16];
  int n = readfile(path, b, sizeof b - 1);
  if (n <= 0)
    return -1;
  int v = 0, digits = 0;
  for (int i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++, digits++)
    v = v * 10 + (b[i] - '0');
  return digits ? v : -1;
}

static void osd_show(int kind)
{
  if (kind == OSD_VOLUME) {
    osd_level = sysfs_int("/sys/class/sound/card0/volume");
    osd_muted = sysfs_int("/sys/class/sound/card0/muted") > 0;
  } else {
    osd_level = sysfs_int("/sys/class/graphics/fb0/brightness");
    osd_muted = false;
  }
  if (osd_level > 100)
    osd_level = 100;
  osd_kind = kind;
  osd_until = msec() + OSD_MS;
  osd_dirty = true;
}

/* True when the overlay appeared, changed or expired since the last call */
static bool osd_tick(void)
{
  if (osd_kind != OSD_NONE && (int)(msec() - osd_until) >= 0) {
    osd_kind = OSD_NONE;
    osd_dirty = true;
  }
  bool d = osd_dirty;
  osd_dirty = false;
  return d;
}

static void fill_round(int x, int y, int w, int h, int r, unsigned c)
{
  for (int row = 0; row < h; row++) {
    int dy = row < r ? r - row : (row >= h - r ? row - (h - r) + 1 : 0);
    int inset = 0;
    if (dy)
      while ((r - inset) * (r - inset) + dy * dy > r * r + r)
        inset++;
    gfx_fill_rect(x + inset, y + row, w - 2 * inset, 1, c);
  }
}

static void osd_speaker(int x, int y, unsigned c)
{
  gfx_fill_rect(x, y + 6, 5, 8, c);
  for (int i = 0; i < 6; i++)
    gfx_fill_rect(x + 5 + i, y + 6 - i, 1, 8 + 2 * i, c);
}

static void osd_sun(int cx, int cy, unsigned c)
{
  static const signed char dir[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
  for (int dy = -4; dy <= 4; dy++)
    for (int dx = -4; dx <= 4; dx++)
      if (dx * dx + dy * dy <= 17)
        gfx_pixel(cx + dx, cy + dy, c);
  for (int i = 0; i < 8; i++)
    for (int k = 7; k <= 9; k++) {
      int m = i < 4 ? k : k * 5 / 7;
      gfx_pixel(cx + dir[i][0] * m, cy + dir[i][1] * m, c);
    }
}

static void draw_osd(void)
{
  int x = osd_x(), y = osd_y();
  fill_round(x, y, OSD_W, OSD_H, 10, OSD_BG);
  int ix = x + 14, iy = y + (OSD_H - 20) / 2, cy = y + OSD_H / 2;
  bool off = osd_kind == OSD_VOLUME && (osd_muted || osd_level == 0);
  if (osd_kind == OSD_VOLUME) {
    osd_speaker(ix, iy, OSD_FG);
    if (off) {
      for (int i = 0; i < 8; i++) {
        gfx_pixel(ix + 14 + i, cy - 4 + i, 0xff6060);
        gfx_pixel(ix + 14 + i, cy + 3 - i, 0xff6060);
      }
    }
  } else {
    osd_sun(ix + 8, cy, OSD_FG);
  }
  if (osd_level < 0)
    return;
  int bx = x + 54, bw = OSD_W - 54 - 52, by = cy - 3;
  fill_round(bx, by, bw, 6, 3, OSD_DIM);
  int fw = osd_muted ? 0 : bw * osd_level / 100;
  if (fw >= 6)
    fill_round(bx, by, fw, 6, 3, OSD_ACCENT);
  else if (fw > 0)
    gfx_fill_rect(bx, by + 1, fw, 4, OSD_ACCENT);
  char t[5];
  int n = 0;
  if (osd_muted) {
    gfx_ui_text(x + OSD_W - 46, cy - UIFONT_H / 2, "Mute", 0xff8080);
    return;
  }
  if (osd_level >= 100)
    t[n++] = '1';
  if (osd_level >= 10)
    t[n++] = (char)('0' + osd_level / 10 % 10);
  t[n++] = (char)('0' + osd_level % 10);
  t[n++] = '%';
  t[n] = '\0';
  gfx_ui_text(x + OSD_W - 14 - gfx_ui_width(t), cy - UIFONT_H / 2, t, OSD_FG);
}

#define PW_BG 0x141a24
#define PW_FG 0xe6ebf2
#define PW_OFF 0x5a6578
#define PW_SEL 0x2f6fb0

static void draw_power(void)
{
  int x = pw_x(), y = pw_y();
  fill_round(x, y, PW_W, pw_h(), 10, PW_BG);
  gfx_ui_text(x + PW_PAD + 6, y + (PW_TITLE_H - UIFONT_H) / 2, "Power", PW_FG);
  for (int i = 0; i < PW_ROWS; i++) {
    int ry = y + PW_TITLE_H + i * PW_ROW_H;
    if (i == pw_sel)
      fill_round(x + PW_PAD, ry, PW_W - 2 * PW_PAD, PW_ROW_H - 2, 6, PW_SEL);
    const char *label = i == PW_CANCEL ? "Cancel" : power_label[i];
    gfx_ui_text(x + PW_PAD + 12, ry + (PW_ROW_H - 2 - UIFONT_H) / 2, label, pw_enabled(i) ? PW_FG : PW_OFF);
  }
}

static void pw_show(void)
{
  pw_open = true;
  pw_sel = can_sleep ? PA_SLEEP : PA_RESTART;
  start_open = false;
  tb_menu_open = false;
  at_active = false;
}

static void pw_activate(int row)
{
  pw_open = false;
  if (row != PW_CANCEL)
    power_run(row);
}

static void pw_key(int c)
{
  if (c == 27 || c == KEY_POWER) {
    pw_open = false;
  } else if (c == '\n' || c == '\r') {
    pw_activate(pw_sel);
  } else if (c == KEY_UP || c == KEY_DOWN) {
    int step = c == KEY_UP ? PW_ROWS - 1 : 1;
    do
      pw_sel = (pw_sel + step) % PW_ROWS;
    while (!pw_enabled(pw_sel));
  }
}

static void compose(int mx, int my)
{
  draw_background();
  int top = topmost_visible();
  for (int i = 0; i < nwin; i++)
    if (win_visible(&cur[i]) && clip_hits(cur[i].x, cur[i].y, cur[i].w, cur[i].h) && !occluded(i))
      draw_window(&cur[i], i == top);
  if (clip_hits(0, screen_h - WM_BAR_H, screen_w, WM_BAR_H))
    draw_taskbar();
  if (start_open)
    draw_start_menu();
  if (tb_menu_open)
    draw_tb_menu();
  for (int i = 0; i < MAXCRASHWIN; i++)
    if (crashwins[i].used)
      draw_crashwin(&crashwins[i]);
  if (at_active)
    draw_switcher();
  if (pw_open && clip_hits(pw_x(), pw_y(), PW_W, pw_h()))
    draw_power();
  if (osd_kind != OSD_NONE && clip_hits(osd_x(), osd_y(), OSD_W, OSD_H))
    draw_osd();
  draw_cursor(pick_cursor(mx, my), mx, my);
}

/* input */
static void deliver_client(int slot, unsigned type, int mx, int my)
{
  struct winrec *w = &cur[slot];
  int cx, cy;
  client_rect(w, &cx, &cy, 0, 0);
  struct winevent ev = {type, mx - cx, my - cy};
  win_deliver(w->id, &ev);
}

/* Live-drag / resize state */
static int drag_id = -1, resize_id = -1;
static int drag_dx, drag_dy, drag_mx0, drag_my0;
static int rsz_edge;
static int rsz_ox, rsz_oy, rsz_ow, rsz_oh, rsz_mx0, rsz_my0;
static int app_drag_id = -1;

#define DBLCLICK_MS 400
#define DBLCLICK_SLOP 6

/* Maximize/restore tracked in the compositor (there is no cross-process maximize primitive) */
struct maxrec {
  int id;
  bool used;
  int sx, sy, sw, sh;
};
static struct maxrec maxed[WM_LIST_MAX];
static int is_maxed(int id)
{
  for (int i = 0; i < WM_LIST_MAX; i++)
    if (maxed[i].used && maxed[i].id == id)
      return i;
  return -1;
}
static void maxed_forget_gone(void)
{
  for (int i = 0; i < WM_LIST_MAX; i++)
    if (maxed[i].used && slot_of(maxed[i].id) < 0)
      maxed[i].used = false;
}
static void toggle_maximize(int slot)
{
  struct winrec *w = &cur[slot];
  int m = is_maxed(w->id);
  if (m >= 0) { /* restore */
    win_set_geom(w->id, maxed[m].sx, maxed[m].sy, maxed[m].sw, maxed[m].sh);
    maxed[m].used = false;
  } else { /* maximize: fill the work area */
    for (int i = 0; i < WM_LIST_MAX; i++)
      if (!maxed[i].used) {
        maxed[i].used = true;
        maxed[i].id = w->id;
        maxed[i].sx = w->x;
        maxed[i].sy = w->y;
        maxed[i].sw = w->w;
        maxed[i].sh = w->h;
        break;
      }
    win_set_geom(w->id, 0, 0, screen_w, screen_h - WM_BAR_H);
  }
}

static bool prev_down, prev_rdown;
static int prev_wheel;

static void handle_input(int mx, int my, int buttons, int dz)
{
  static int dbl_id = -1, dbl_x, dbl_y; /* last title-bar press, for double-click */
  static unsigned dbl_ms;
  bool down = buttons & 1;
  bool rdown = buttons & 2;

  if (pw_open) {
    static int last_mx, last_my;
    int row = pw_row_at(mx, my);
    if (down && !prev_down) {
      if (row >= 0 && pw_enabled(row))
        pw_activate(row);
      else if (!pw_inside(mx, my))
        pw_open = false;
    } else if ((mx != last_mx || my != last_my) && row >= 0 && pw_enabled(row)) {
      pw_sel = row;
    }
    last_mx = mx;
    last_my = my;
    prev_down = down;
    prev_rdown = rdown;
    return;
  }

  /* Wheel -> focused window under cursor (dropped if a modal blocks it) */
  if (dz) {
    int idx = window_at(mx, my);
    if (idx >= 0 && blocking_modal(idx) < 0) {
      struct winevent ev = {WEV_SCROLL, dz, 0};
      win_deliver(cur[idx].id, &ev);
    }
  }

  /* Right-press */
  if (rdown && !prev_rdown) {
    int bar_y = screen_h - WM_BAR_H;
    int tbw = (my >= bar_y) ? taskbar_hit(mx, my) : -1;
    if (tbw >= 0) {
      tb_menu_open = true;
      tb_menu_win = cur[tbw].id;
      start_open = false;
      tb_menu_x = mx;
      if (tb_menu_x + TBM_W > screen_w)
        tb_menu_x = screen_w - TBM_W;
      if (tb_menu_x < 0)
        tb_menu_x = 0;
      tb_menu_y = bar_y - tbm_h();
    } else {
      tb_menu_open = false;
      int idx = window_at(mx, my);
      if (idx >= 0) {
        int mb = blocking_modal(idx);
        if (mb >= 0) {
          raise_family_id(mb);
          flash_modal(mb);
        } else {
          raise_window(idx);
          if (my >= cur[idx].y + WM_TITLE_H)
            deliver_client(idx, WEV_RCLICK, mx, my);
        }
      }
    }
  }
  prev_rdown = rdown;

  if (down && !prev_down) { /* left press */
                            /* crash windows sit above everything but the menus: close box / dismiss */
    for (int i = MAXCRASHWIN - 1; i >= 0; i--) {
      struct crashwin *c = &crashwins[i];
      if (!c->used)
        continue;
      if (mx >= c->x && mx < c->x + c->w && my >= c->y && my < c->y + c->h) {
        int bx = c->x + c->w - 4 - TB_BTN_W, by = c->y + 4;
        if (mx >= bx && mx < bx + TB_BTN_W && my >= by && my < by + TB_BTN_W) {
          c->used = false;
          force_full = true;
        }
        prev_down = down;
        return; /* clicks over a crash panel stop here */
      }
    }
    if (tb_menu_open) {
      int inside = mx >= tb_menu_x && mx < tb_menu_x + TBM_W && my >= tb_menu_y && my < tb_menu_y + tbm_h();
      if (inside) {
        int item = (my - tb_menu_y) / TBM_ITEM_H;
        int id = tb_menu_win;
        tb_menu_open = false;
        int s = slot_of(id);
        if (s >= 0) {
          if (item == 0) { /* Close: ask the app */
            struct winevent ev = {WEV_CLOSE, 0, 0};
            win_deliver(id, &ev);
          } else { /* Force Kill */
            if (cur[s].owner_pid > 0)
              kill(cur[s].owner_pid, SIGKILL);
          }
        }
        prev_down = down;
        return;
      }
      tb_menu_open = false;
    }
    int bar_y = screen_h - WM_BAR_H;
    if (my >= bar_y && mx >= 0 && mx < START_W) { /* Start button */
      start_open = !start_open;
      prev_down = down;
      return;
    }
    int tbw = (my >= bar_y) ? taskbar_hit(mx, my) : -1;
    if (tbw >= 0) {
      start_open = false;
      /* restore/minimize/raise via focus + geom */
      raise_window(tbw);
      prev_down = down;
      return;
    }
    if (start_open) {
      int top = sm_top();
      if (mx >= 0 && mx < SM_W && my >= top && my < bar_y) {
        int rel = my - top;
        if (rel < sm_apps_h()) {
          launch_app(rel / SM_ITEM_H);
          start_open = false;
        } else {
          int prel = rel - sm_apps_h() - SM_SEP_H;
          if (prel >= 0 && prel / SM_ITEM_H < sm_power_n()) {
            start_open = false;
            power_run(sm_power_action(prel / SM_ITEM_H));
          }
        }
        prev_down = down;
        return;
      }
      start_open = false;
    }
    int idx = window_at(mx, my);
    if (idx >= 0) {
      /* A modal dialog holds its family */
      int mb = blocking_modal(idx);
      if (mb >= 0) {
        raise_family_id(mb);
        flash_modal(mb);
        prev_down = down;
        return;
      }
      raise_window(idx);
      struct winrec *w = &cur[idx];
      bool resizable = (w->flags & UWIN_F_RESIZABLE) != 0; /* dialogs opt out */
      int tb = titlebtn_hit(w, mx, my);
      int resize = resize_hit(w, mx, my);
      if (tb == 0) { /* close */
        struct winevent ev = {WEV_CLOSE, 0, 0};
        win_deliver(w->id, &ev);
      } else if (tb == 1) { /* maximize / restore */
        toggle_maximize(idx);
      } else if (tb == 2) { /* minimize: no cross-process minimize primitive yet */
        z_lower(w->id);
      } else if (resizable && is_maxed(w->id) < 0 && resize) {
        int e = resize;
        resize_id = w->id;
        rsz_edge = e;
        rsz_ox = w->x;
        rsz_oy = w->y;
        rsz_ow = w->w;
        rsz_oh = w->h;
        rsz_mx0 = mx;
        rsz_my0 = my;
      } else if (my < w->y + WM_TITLE_H) { /* title bar */
        unsigned t = msec();
        int dx = mx - dbl_x, dy = my - dbl_y;
        if (dx < 0)
          dx = -dx;
        if (dy < 0)
          dy = -dy;
        if (btn_present(w, 1) && dbl_id == w->id && (unsigned)(t - dbl_ms) <= DBLCLICK_MS && dx <= DBLCLICK_SLOP &&
            dy <= DBLCLICK_SLOP) {
          toggle_maximize(idx); /* double-click: [un-]maximize */
          dbl_id = -1;          /* consume, so a triple-click isn't two toggles */
        } else {                /* single click: title-bar drag */
          drag_id = w->id;
          drag_dx = mx - w->x;
          drag_dy = my - w->y;
          drag_mx0 = mx;
          drag_my0 = my;
          dbl_id = w->id;
          dbl_ms = t;
          dbl_x = mx;
          dbl_y = my;
        }
      } else { /* client press */
        deliver_client(idx, WEV_MOUSE, mx, my);
        app_drag_id = w->id;
      }
    }
  } else if (down && app_drag_id >= 0) { /* app-captured drag */
    int s = slot_of(app_drag_id);
    if (s >= 0)
      deliver_client(s, WEV_DRAG, mx, my);
  } else if (down && drag_id >= 0) { /* title drag */
    int s = slot_of(drag_id);
    if (s >= 0) {
      struct winrec *w = &cur[s];
      int rw = w->w, rh = w->h;
      int m = is_maxed(w->id);
      /* Drag off a maximized window */
      if (m >= 0 && (mx - drag_mx0 > DBLCLICK_SLOP || drag_mx0 - mx > DBLCLICK_SLOP || my - drag_my0 > DBLCLICK_SLOP ||
                     drag_my0 - my > DBLCLICK_SLOP)) {
        rw = maxed[m].sw;
        rh = maxed[m].sh;
        maxed[m].used = false;
        drag_dx = (screen_w > 0) ? mx * rw / screen_w : rw / 2;
      }
      int nx = mx - drag_dx, ny = my - drag_dy;
      clamp_frame_position(&nx, &ny, rw, WM_TITLE_H);
      win_set_geom(w->id, nx, ny, rw, rh);
    }
  } else if (down && resize_id >= 0) { /* resize */
    int s = slot_of(resize_id);
    if (s >= 0) {
      int dx = mx - rsz_mx0, dy = my - rsz_my0;
      int nx = rsz_ox, ny = rsz_oy, nw = rsz_ow, nh = rsz_oh;
      if (rsz_edge & RZ_R)
        nw = rsz_ow + dx;
      if (rsz_edge & RZ_B)
        nh = rsz_oh + dy;
      if (rsz_edge & RZ_L) {
        nw = rsz_ow - dx;
        nx = rsz_ox + dx;
      }
      if (rsz_edge & RZ_T) {
        nh = rsz_oh - dy;
        ny = rsz_oy + dy;
      }
      if (nw < WM_MIN_W) {
        if (rsz_edge & RZ_L)
          nx -= WM_MIN_W - nw;
        nw = WM_MIN_W;
      }
      if (nh < WM_MIN_H) {
        if (rsz_edge & RZ_T)
          ny -= WM_MIN_H - nh;
        nh = WM_MIN_H;
      }
      if (nx < 0) {
        nw += nx;
        nx = 0;
      }
      if (ny < 0) {
        nh += ny;
        ny = 0;
      }
      if (nx + nw > screen_w)
        nw = screen_w - nx;
      if (ny + nh > screen_h - WM_BAR_H)
        nh = screen_h - WM_BAR_H - ny;
      win_set_geom(cur[s].id, nx, ny, nw, nh);
    }
  } else if (!down) { /* release */
    drag_id = resize_id = app_drag_id = -1;
  }

  /* Hover: no button held -> WEV_MOVE to the window under the cursor */
  static int hover_id = -1, hover_px = -1, hover_py = -1;
  if (!down && !rdown && drag_id < 0 && resize_id < 0 && app_drag_id < 0 && (mx != hover_px || my != hover_py)) {
    hover_px = mx;
    hover_py = my;
    int idx = window_at(mx, my);
    struct winrec *w = idx >= 0 ? &cur[idx] : 0;
    int cx, cy, cw, ch;
    if (w)
      client_rect(w, &cx, &cy, &cw, &ch);
    bool in_client = w && mx >= cx && mx < cx + cw && my >= cy && my < cy + ch;
    if (in_client && blocking_modal(idx) >= 0)
      in_client = false; /* modal blocks hover */
    if (hover_id >= 0 && (idx < 0 || cur[idx].id != hover_id || !in_client)) {
      struct winevent ev = {WEV_MOVE, -1, -1};
      win_deliver(hover_id, &ev);
      hover_id = -1;
    }
    if (in_client) {
      deliver_client(idx, WEV_MOVE, mx, my);
      hover_id = w->id;
    }
  }

  hover_resizing = (resize_id >= 0);
  rsz_edge_hint = rsz_edge;
  prev_down = down;
}

/* global key routing (Super -> Start menu; Alt+Tab -> switcher; else the focused window) */

/* Clipboard-history viewer (Super+V): launch /bin/cliphist */
static void paste_begin(void);
static int cliphist_pid;
static void open_cliphist(void)
{
  if (cliphist_pid > 0)
    return; /* one at a time */
  cliphist_pid = spawn("/bin/cliphist", 0);
  /* Reaped exclusively by cliphist_reap, so it is deliberately NOT added to kids[] */
}
static void cliphist_reap(void)
{
  if (cliphist_pid <= 0)
    return;
  int st;
  if (waitpid_nb(cliphist_pid, &st)) {
    int picked = (st == 0);
    cliphist_pid = 0;
    if (picked)
      paste_begin(); /* paste the promoted clip into focus */
  }
}

#define PENDING_MAX 256
static struct {
  int c, mods;
} pending[PENDING_MAX];
static int pend_head, pend_tail;
static bool pend_empty(void) { return pend_head == pend_tail; }
static void pend_push(int c, int mods)
{
  int next = (pend_head + 1) % PENDING_MAX;
  if (next == pend_tail)
    return; /* full: drop (paste larger than 255 backlog) */
  pending[pend_head].c = c;
  pending[pend_head].mods = mods;
  pend_head = next;
}

/* Print Screen */
static unsigned *shot_pixels; /* tightly-packed w*h capture (grown on demand) */
static unsigned shot_cap;     /* capacity of shot_pixels, in pixels */
static int shot_tid;          /* encode worker, 0 = idle */
static void *shot_stack;      /* worker stack (allocated once, reused) */
static char shot_path[64];
static unsigned shot_w, shot_h;

static void shot_worker(void)
{
  png_write(shot_path, shot_pixels, shot_w, shot_h, shot_w);
  exit(0);
}

static void take_screenshot(void)
{
  if (shot_tid) { /* previous encode still running? */
    int st;
    if (!waitpid_nb(shot_tid, &st))
      return; /* busy: drop this shot */
    shot_tid = 0;
  }
  unsigned w = screen_w, h = screen_h;
  if (!w || !h || !back)
    return;

  if (shot_cap < w * h) { /* (re)size the capture buffer */
    free(shot_pixels);
    shot_pixels = malloc(w * h * 4);
    shot_cap = shot_pixels ? w * h : 0;
  }
  if (!shot_pixels)
    return;
  for (unsigned y = 0; y < h; y++) /* fast capture: strip the fb stride */
    memcpy(shot_pixels + y * w, back + y * fb_pitch, w * 4);
  shot_w = w;
  shot_h = h;

  /* First free /home/root/screenshotN.png */
  struct statinfo si;
  for (unsigned idx = 0; idx <= 9999; idx++) {
    int p = 0;
    for (const char *s = "/home/root/screenshot"; *s; s++)
      shot_path[p++] = *s;
    char t[6];
    int n = 0;
    unsigned v = idx;
    if (!v)
      t[n++] = '0';
    while (v) {
      t[n++] = (char)('0' + v % 10);
      v /= 10;
    }
    while (n)
      shot_path[p++] = t[--n];
    for (const char *s = ".png"; *s; s++)
      shot_path[p++] = *s;
    shot_path[p] = '\0';
    if (stat(shot_path, &si) != 0)
      break; /* name is free */
    if (idx == 9999)
      return;
  }

  if (!shot_stack)
    shot_stack = thread_stack_alloc(32 * 1024);
  if (!shot_stack)
    return;
  shot_tid = thread_create(shot_worker, shot_stack);
}

static void key_to_focus(int c, int mods)
{
  int t = topmost_visible();
  if (t < 0)
    return;
  /* Flow control: if the focused window's event ring is full */
  if (win_key_space(cur[t].id) <= 0) {
    pend_push(c, mods);
    return;
  }
  struct winevent ev = {WEV_KEY, c, mods};
  win_deliver(cur[t].id, &ev);
}

static bool title_is(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

/* cur[] slot of an app's window: spawned by us, or (player) known by title */
static int app_window(int app)
{
  for (int i = nwin - 1; i >= 0; i--) {
    if (app_pid[app] > 0 && cur[i].owner_pid == app_pid[app])
      return i;
    if (app == APP_PLAYER && (title_is(cur[i].title, "Media Player") || title_is(cur[i].title, "Video Player")))
      return i;
  }
  return -1;
}

static void app_raise_or_launch(int app)
{
  int s = app_window(app);
  if (s >= 0)
    raise_window(s);
  else
    launch_app(app);
}

/* Media transport reaches the player even when it is not focused */
static void media_key(int c)
{
  int s = app_window(APP_PLAYER);
  if (s < 0) {
    if (c == KEY_MEDIA_PLAY)
      launch_app(APP_PLAYER);
    return;
  }
  if (win_key_space(cur[s].id) <= 0)
    return;
  struct winevent ev = {WEV_KEY, c, 0};
  win_deliver(cur[s].id, &ev);
}

/* Global special keys; true when consumed (apps never see them) */
static bool special_key(int c)
{
  switch (c) {
  case KEY_VOL_UP:
  case KEY_VOL_DOWN:
  case KEY_MUTE:
    osd_show(OSD_VOLUME);
    return true;
  case KEY_BRIGHT_UP:
  case KEY_BRIGHT_DOWN:
    osd_show(OSD_BRIGHTNESS);
    return true;
  case KEY_MIC_MUTE:    /* no mic state is exposed: swallowed */
  case KEY_LAUNCH_MAIL: /* no mail app */
    return true;
  case KEY_MEDIA_PLAY:
  case KEY_MEDIA_STOP:
  case KEY_MEDIA_NEXT:
  case KEY_MEDIA_PREV:
    media_key(c);
    return true;
  case KEY_LAUNCH_CALC:
    app_raise_or_launch(APP_CALC);
    return true;
  case KEY_LAUNCH_BROWSER:
    app_raise_or_launch(APP_BROWSER);
    return true;
  case KEY_LAUNCH_FILES:
    app_raise_or_launch(APP_FILES);
    return true;
  case KEY_SEARCH:
    start_open = true;
    return true;
  case KEY_POWER:
    pw_show();
    return true;
  case KEY_SLEEP:
    if (can_sleep)
      power_run(PA_SLEEP);
    else
      pw_show();
    return true;
  case KEY_WWW_BACK:
    key_to_focus(KEY_LEFT, KMOD_ALT);
    return true;
  case KEY_WWW_FORWARD:
    key_to_focus(KEY_RIGHT, KMOD_ALT);
    return true;
  case KEY_WWW_REFRESH:
    key_to_focus(KEY_F5, 0);
    return true;
  }
  return false;
}

static void feed_key(int c, int mods)
{
  if (pw_open) {
    pw_key(c);
    return;
  }
  if (special_key(c))
    return;
  if (c == KEY_PRTSC) {
    take_screenshot();
    return;
  }
  if (c == 'v' && (mods & KMOD_SUPER)) {
    open_cliphist();
    return;
  } /* Super+V */
  if (c == 'v' && (mods & KMOD_CTRL)) {
    paste_begin();
    return;
  } /* Ctrl+V */
  if (c == KEY_SUPER) {
    start_open = !start_open;
    return;
  }
  if (c == KEY_TAB && (mods & KMOD_ALT)) {
    if (!at_active) {
      at_count = alttab_build();
      if (at_count <= 1) {
        at_count = 0;
        return;
      }
      at_active = true;
      at_sel = 0;
    }
    if (mods & KMOD_SHIFT)
      at_sel = (at_sel - 1 + at_count) % at_count;
    else
      at_sel = (at_sel + 1) % at_count;
    return;
  }
  /* We don't get an "Alt released" event from input_poll */
  if (at_active && !(mods & KMOD_ALT)) {
    int s = slot_of(at_order[at_sel]);
    at_active = false;
    if (s >= 0)
      raise_window(s);
    return;
  }
  key_to_focus(c, mods);
}

/* Drain parked keys before pulling fresh input; stop as soon as the target fills again */
static void pending_flush(void)
{
  while (!pend_empty()) {
    int c = pending[pend_tail].c, mods = pending[pend_tail].mods;
    int t = topmost_visible();
    if (t < 0) {
      pend_tail = pend_head;
      return;
    } /* nothing focused: discard */
    if (win_key_space(cur[t].id) <= 0)
      return; /* still full: try next frame */
    pend_tail = (pend_tail + 1) % PENDING_MAX;
    struct winevent ev = {WEV_KEY, c, mods};
    win_deliver(cur[t].id, &ev);
  }
}

#define PASTE_MAX 8192
static char paste_buf[PASTE_MAX];
static int paste_len, paste_pos;

static void paste_begin(void)
{
  if (paste_pos < paste_len)
    return; /* a paste is still draining */
  char mime[32];
  void *data = 0;
  int len = 0;
  if (clip_get(0, mime, sizeof mime, &data, &len) < 0)
    return;
  int out = 0;
  if (mime[0] == 't' && mime[1] == 'e' && mime[2] == 'x' && mime[3] == 't' && mime[4] == '/') {
    const char *raw = data;
    for (int i = 0; i < len && out < PASTE_MAX; i++) {
      char c = raw[i];
      if (c == '\r') { /* fold CR and CRLF down to one LF */
        if (i + 1 < len && raw[i + 1] == '\n')
          i++;
        c = '\n';
      }
      if (c != '\n' && c != '\t' && (unsigned char)c < 0x20)
        continue; /* drop stray control bytes */
      paste_buf[out++] = c;
    }
  }
  free(data);
  paste_pos = 0;
  paste_len = out; /* publish last */
}

/* Drain the pending paste into the focused window */
static void paste_flush(void)
{
  while (paste_pos < paste_len) {
    int t = topmost_visible();
    if (t < 0) {
      paste_pos = paste_len;
      return;
    } /* nothing focused: discard */
    if (win_key_space(cur[t].id) <= 0)
      return; /* full: try next frame */
    struct winevent ev = {WEV_KEY, (unsigned char)paste_buf[paste_pos++], 0};
    win_deliver(cur[t].id, &ev);
  }
}
/* Active-frame target rate (Hz) */
static int target_hz = 60;
static unsigned active_period(void) { return target_hz > 0 ? 1000u / (unsigned)target_hz : 16u; }

/* Frame pacing: sleep only the slack to a running deadline */
static unsigned frame_deadline;

static void frame_pace(unsigned period)
{
  unsigned now = msec();
  int slack = (int)(frame_deadline - now); /* signed: tolerates wrap + overrun */
  if (slack < 0 || slack > (int)period) {
    frame_deadline = now + period;
    return;
  }
  frame_deadline += period;
  msleep((unsigned)slack);
}

/* Persisted desktop tunables */
#define CONF_DISPLAY "/etc/display.conf"
#define CONF_INPUT "/etc/input.conf"
#define CONF_SOUND "/etc/sound.conf"
#define DISPLAY_RATE "/sys/class/graphics/fb0/rate"

/* One row per tunable: config file + key <-> sysfs attribute */
struct tunable {
  const char *file;
  const char *key;
  const char *node;
  char cur[32];
  bool known; /* cur holds a real value */
};
static struct tunable tun[] = {
    {CONF_DISPLAY, "mode", "/sys/class/graphics/fb0/mode", "", 0},
    {CONF_DISPLAY, "rate", DISPLAY_RATE, "", 0},
    {CONF_INPUT, "layout", "/sys/class/input/kbd0/layout", "", 0},
    {CONF_INPUT, "mouse", "/sys/class/input/mouse0/speed", "", 0},
    {CONF_SOUND, "sound", "/sys/class/sound/card0/output", "", 0},
};
#define NTUN ((int)(sizeof tun / sizeof tun[0]))
static const char *const conf_files[] = {CONF_DISPLAY, CONF_INPUT, CONF_SOUND};
#define NCONF ((int)(sizeof conf_files / sizeof conf_files[0]))

static int scan_uint(const char *p)
{
  int v = 0;
  while (*p >= '0' && *p <= '9')
    v = v * 10 + (*p++ - '0');
  return v;
}

/* Read a sysfs attribute into `out`, trimming trailing whitespace */
static int node_read(const char *path, char *out, int max)
{
  int n = readfile(path, out, max - 1);
  if (n < 0)
    n = 0;
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
    n--;
  out[n] = 0;
  return n;
}

/* Fill each tunable's `cur` from its /etc config ("key=value" lines) */
static void conf_load(void)
{
  for (int f = 0; f < NCONF; f++) {
    char buf[256];
    int n = readfile(conf_files[f], buf, sizeof buf - 1);
    if (n <= 0)
      continue;
    buf[n] = 0;
    for (char *p = buf; *p;) {
      char *line = p;
      while (*p && *p != '\n')
        p++;
      if (*p == '\n')
        *p++ = 0;
      char *eq = line;
      while (*eq && *eq != '=')
        eq++;
      if (*eq != '=')
        continue;
      *eq = 0;
      char *val = eq + 1;
      for (int m = strlen(val); m > 0 && (val[m - 1] == '\r' || val[m - 1] == ' ');)
        val[--m] = 0;
      for (int i = 0; i < NTUN; i++)
        if (streq(line, tun[i].key)) {
          int j = 0;
          while (val[j] && j < (int)sizeof tun[i].cur - 1) {
            tun[i].cur[j] = val[j];
            j++;
          }
          tun[i].cur[j] = 0;
          tun[i].known = tun[i].cur[0] != 0;
          break;
        }
    }
  }
}

/* Rewrite `file` from the known tunables that belong to it */
static void conf_save_file(const char *file)
{
  char buf[256], *o = buf, *end = buf + sizeof buf;
  for (int i = 0; i < NTUN; i++) {
    if (tun[i].file != file || !tun[i].known)
      continue;
    const char *k = tun[i].key, *v = tun[i].cur;
    if (o + strlen(k) + strlen(v) + 2 >= end)
      break;
    while (*k)
      *o++ = *k++;
    *o++ = '=';
    while (*v)
      *o++ = *v++;
    *o++ = '\n';
  }
  if (o > buf)
    writefile(file, buf, (int)(o - buf));
}

/* Persist every domain whose tunable changed */
static void conf_save(void)
{
  for (int f = 0; f < NCONF; f++)
    conf_save_file(conf_files[f]);
}

/* Push saved values into kernel nodes */
static void tun_restore(void)
{
  for (int i = 0; i < NTUN; i++)
    if (tun[i].known)
      writefile(tun[i].node, tun[i].cur, strlen(tun[i].cur));
  for (int i = 0; i < NTUN; i++) {
    char v[32];
    if (node_read(tun[i].node, v, sizeof v) > 0) {
      strcpy(tun[i].cur, v);
      tun[i].known = true;
    }
  }
}

/* Poll the live nodes; update any that changed */
static bool tun_poll(void)
{
  bool changed = false;
  char v[32];
  for (int i = 0; i < NTUN; i++) {
    if (node_read(tun[i].node, v, sizeof v) <= 0)
      continue;
    if (!streq(v, tun[i].cur)) {
      strcpy(tun[i].cur, v);
      tun[i].known = true;
      changed = true;
    }
  }
  return changed;
}

/* The live frame-pacing target (Hz) from the kernel */
static int rate_now(void)
{
  char v[16];
  if (node_read(DISPLAY_RATE, v, sizeof v) <= 0)
    return 0;
  return scan_uint(v);
}

/* prev-frame */
struct pfsnap {
  int id, x, y, w, h;
  bool vis, min;
  unsigned seq;
};
static struct pfsnap pf[WM_LIST_MAX];
static int npf;

/* Rebind to the framebuffer's current geometry: remap the back buffer */
static int bind_mode(void)
{
  struct fbinfo fi;
  if (fb_info(&fi) < 0)
    return -1;
  back = fb_map();
  if (!back)
    return -1;
  screen_w = fi.w;
  screen_h = fi.h;
  fb_pitch = fi.pitch;
  gfx_init(back, fb_pitch, screen_h); /* surface stride = pitch */
  input_set_bounds(screen_w - 1, screen_h - 1);
  clip_full();
  force_full = true;
  return 0;
}

/* Detect a resolution change via fb_info each frame */
/* Center each transient dialog over its parent once, so it stays draggable */
static int centered[WM_LIST_MAX], ncentered;
static bool was_centered(int id)
{
  for (int i = 0; i < ncentered; i++)
    if (centered[i] == id)
      return true;
  return false;
}
static void center_new_dialogs(void)
{
  for (int i = 0; i < ncentered;) { /* prune closed dialogs */
    if (slot_of(centered[i]) < 0)
      centered[i] = centered[--ncentered];
    else
      i++;
  }
  for (int i = 0; i < nwin; i++) {
    int pid = parent_id_of(cur[i].id);
    if (pid < 0 || was_centered(cur[i].id))
      continue;
    int ps = slot_of(pid);
    if (ps < 0)
      continue;
    struct winrec *w = &cur[i], *p = &cur[ps];
    int nx = p->x + (p->w - w->w) / 2;
    int ny = p->y + (p->h - w->h) / 2;
    clamp_frame_position(&nx, &ny, w->w, w->h);
    win_set_geom(w->id, nx, ny, w->w, w->h);
    w->x = nx;
    w->y = ny; /* reflect this frame's compose */
    if (ncentered < WM_LIST_MAX)
      centered[ncentered++] = w->id;
  }
}

static void check_resolution(void)
{
  struct fbinfo fi;
  if (fb_info(&fi) < 0)
    return;
  if (fi.w == screen_w && fi.h == screen_h && fi.pitch == fb_pitch)
    return;

  bind_mode();
  for (int i = 0; i < nwin; i++) {
    struct winrec *w = &cur[i];
    int m = is_maxed(w->id);
    if (m >= 0) {
      win_set_geom(w->id, 0, 0, screen_w, work_bottom());
      continue;
    }
    int nx = w->x, ny = w->y, nw = w->w, nh = w->h;
    fit_frame_to_work_area(&nx, &ny, &nw, &nh);
    if (nx != w->x || ny != w->y || nw != w->w || nh != w->h)
      win_set_geom(w->id, nx, ny, nw, nh);
  }
}

int main(void)
{
  if (getuid() != 0) {
    puts("compositor: must run as root\n");
    return 1;
  }

  if (bind_mode() < 0) {
    puts("compositor: no framebuffer\n");
    return 1;
  }

  /* Session autostart: clipboard server, notification daemon, terminal */
  kid_track(spawn("/bin/clipd", 0));
  kid_track(spawn("/bin/vmclipd", 0)); /* VMware host clipboard bridge; self-exits off VMware */
  kid_track(spawn("/bin/notifyd", 0));
  kid_track(spawn("/bin/term", 0));

  int prev_mx = -9999, prev_my = -9999, prev_cur = -1;
  bool prev_start = false, prev_tb_menu = false, prev_at_active = false, prev_pw_open = false;
  int prev_pw_sel = -1;
  int prev_at_sel = -1;
  int prev_focus = -1, last_sec = -1;
  bool inited = false;

  /* Restore saved tunables, then adopt the live refresh rate as the pacing target */
  conf_load();
  tun_restore();
  target_hz = rate_now();
  if (target_hz <= 0)
    target_hz = 60;
  unsigned conf_poll_at = 0;
  can_sleep = power(3) > 0;

  for (;;) {
    struct inputstate in;
    if (input_poll(&in) < 0) {
      msleep(16);
      continue;
    }
    if (resumed) {
      resumed = false;
      in.nkeys = 0;
      prev_wheel = in.wheel;
      prev_down = in.buttons & 1;
      prev_rdown = in.buttons & 2;
      osd_kind = OSD_NONE;
      force_full = true;
      frame_deadline = msec();
    }
    frame_clock = now_hms();

    cliphist_reap(); /* Super+V viewer exited -> maybe paste */
    reap_kids();
    crash_ingest();

    /* Refresh the window set, then (re)pin client buffers to match it */
    nwin = win_enum(cur, WM_LIST_MAX);
    if (nwin < 0)
      nwin = 0;
    refresh_maps(); /* maps must be current before compose */
    zorder_apply(); /* reconcile z-stack; reorder cur[] bottom->top */
    taskbar_layout_dirty = true;
    center_new_dialogs(); /* place a fresh dialog over its parent */

    check_resolution(); /* Settings may have changed the fb mode */

    /* Pick up a Settings change to any tunable ~1x/sec */
    unsigned nowms = msec();
    if ((int)(nowms - conf_poll_at) >= 0) {
      conf_poll_at = nowms + 1000;
      if (tun_poll()) {
        conf_save();
        int r = rate_now();
        if (r > 0)
          target_hz = r;
      }
    }

    if (flash_frames > 0) {
      flash_frames--;
      force_full = true;
    } /* pulse a modal */

    /* Drain any keys parked by paste back-pressure before pulling new input */
    pending_flush();
    paste_flush(); /* inject any in-progress clipboard paste */

    /* Keys first (may open menus / switch focus) */
    for (int i = 0; i < in.nkeys && !resumed; i++)
      feed_key(in.keys[i].c, in.keys[i].mods);

    int dz = in.wheel - prev_wheel; /* wheel is a running total; delta = this frame */
    prev_wheel = in.wheel;

    handle_input(in.mx, in.my, in.buttons, dz);

    int mx = in.mx, my = in.my;
    ndmg = 0;
    bool osd = osd_tick();
    bool full = !inited || force_full;
    force_full = false;

    /* Focus / z-order / visibility / add / remove change -> full repaint */
    if (!full) {
      int focus = topmost_visible();
      int focus_id = focus >= 0 ? cur[focus].id : -1;
      int zvis = (focus_id != prev_focus);
      if (nwin != npf)
        zvis = 1; /* window added or removed */
      for (int i = 0; i < nwin && !zvis; i++)
        if (pf[i].id != cur[i].id || pf[i].vis != win_visible(&cur[i]) || pf[i].min != win_minimized(&cur[i]))
          zvis = 1;
      if (zvis)
        full = true;
      prev_focus = focus_id;

      if (at_active != prev_at_active || at_sel != prev_at_sel)
        full = true;
      prev_at_active = at_active;
      prev_at_sel = at_sel;
      if (tb_menu_open != prev_tb_menu)
        full = true;
      prev_tb_menu = tb_menu_open;

      /* moved/resized windows: old ∪ new as one region */
      for (int i = 0; i < nwin; i++) {
        const struct pfsnap *p = &pf[i];
        if ((p->x != cur[i].x || p->y != cur[i].y || p->w != cur[i].w || p->h != cur[i].h))
          damage_add_union(p->x, p->y, p->w, p->h, cur[i].x, cur[i].y, cur[i].w, cur[i].h);
      }
      /* cursor moved/changed/animating */
      int cur_kind = pick_cursor(mx, my);
      if (mx != prev_mx || my != prev_my || cur_kind != prev_cur || cur_kind == CUR_BUSY) {
        damage_add(prev_mx - CUR_DMG_PAD, prev_my - CUR_DMG_PAD, CUR_DMG_W, CUR_DMG_H);
        damage_add(mx - CUR_DMG_PAD, my - CUR_DMG_PAD, CUR_DMG_W, CUR_DMG_H);
      }
      prev_cur = cur_kind;
      /* content changed: only the client rect the app reported, or the whole frame */
      for (int i = 0; i < nwin; i++) {
        const struct winrec *w = &cur[i];
        if (!win_visible(w) || pf[i].seq == w->present_seq)
          continue;
        if (w->dirty_w < 0) {
          damage_add(w->x, w->y, w->w, w->h);
        } else if (w->dirty_w > 0 && w->dirty_h > 0) {
          int cx, cy;
          client_rect(w, &cx, &cy, 0, 0);
          damage_add(cx + w->dirty_x, cy + w->dirty_y, w->dirty_w, w->dirty_h);
        }
      }
      /* clock tick */
      int sec = (int)(frame_clock & 0xFF);
      if (sec != last_sec)
        damage_add(0, screen_h - WM_BAR_H, screen_w, WM_BAR_H);
      if (osd)
        damage_add(osd_x(), osd_y(), OSD_W, OSD_H);
      /* Start menu toggle */
      if (start_open != prev_start)
        damage_add(0, sm_top(), SM_W, sm_h() + WM_BAR_H);
      if (pw_open != prev_pw_open || (pw_open && pw_sel != prev_pw_sel))
        damage_add(pw_x(), pw_y(), PW_W, pw_h());
    }

    if (full) {
      clip_full();
      compose(mx, my);
      struct fbrect all = {0, 0, screen_w, screen_h};
      fb_present_rects(&all, 1);
    } else if (ndmg > 0) {
      struct fbrect rects[MAXDMG];
      int n = 0;
      for (int k = 0; k < ndmg; k++) {
        set_clip(dmg[k].x, dmg[k].y, dmg[k].w, dmg[k].h);
        if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0)
          continue;
        compose(mx, my);
        rects[n++] = (struct fbrect){clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0};
      }
      clip_full();
      if (n)
        fb_present_rects(rects, n);
    }

    /* snapshot for next frame */
    npf = 0;
    for (int i = 0; i < nwin && npf < WM_LIST_MAX; i++) {
      pf[npf].id = cur[i].id;
      pf[npf].x = cur[i].x;
      pf[npf].y = cur[i].y;
      pf[npf].w = cur[i].w;
      pf[npf].h = cur[i].h;
      pf[npf].vis = win_visible(&cur[i]);
      pf[npf].min = win_minimized(&cur[i]);
      pf[npf].seq = cur[i].present_seq;
      npf++;
    }
    prev_mx = mx;
    prev_my = my;
    prev_start = start_open;
    prev_pw_open = pw_open;
    prev_pw_sel = pw_sel;
    last_sec = (int)(frame_clock & 0xFF);
    inited = true;
    cursor_frame++;

    int idle =
        in.nkeys == 0 && in.buttons == 0 && dz == 0 && !full && ndmg == 0 && pend_empty() && paste_pos >= paste_len;
    frame_pace(idle ? 40 : active_period()); /* target_hz while active; ~25 fps when static */
  }
}
