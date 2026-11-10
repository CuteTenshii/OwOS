/* wm.c: window model, stacking, hit-testing and title/taskbar geometry */
#include "comp.h"

/* window model */
/* cur[] is enumerated in slot order, then zorder_apply() sorts it bottom first */
struct winrec cur[WM_LIST_MAX];
int nwin;

bool win_visible(const struct winrec *w) { return (w->flags & UWIN_F_VISIBLE) != 0; }
bool win_minimized(const struct winrec *w) { return (w->flags & UWIN_F_MINIMIZED) != 0; }

/* Compositor-tracked maximize state (see the maxrec table below): -1 if not */

/* wins index by z position i (bottom..top); cur[] is already in that order */
int slot_of(int id)
{
  for (int i = 0; i < nwin; i++)
    if (cur[i].id == id)
      return i;
  return -1;
}

/* Compositor-owned stacking order: window ids, bottom -> top */
static int zstack[WM_LIST_MAX];
static int nz;
bool taskbar_layout_dirty = true;
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
void z_lower(int id)
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
int parent_id_of(int id)
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
int root_id_of(int id)
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
void zorder_apply(void)
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
void put2(char *p, int v)
{
  p[0] = (char)('0' + v / 10);
  p[1] = (char)('0' + v % 10);
}

void disp_title(const struct winrec *w, char *buf)
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

/* z / hit-testing */

/* Topmost visible window's cur[] slot (holds focus), or -1 */
int topmost_visible(void)
{
  for (int i = nwin - 1; i >= 0; i--)
    if (win_visible(&cur[i]))
      return i;
  return -1;
}

/* Topmost visible window containing (px,py), or -1 */
int window_at(int px, int py)
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
void raise_family_id(int id)
{
  int root = root_id_of(id);
  z_raise(root);
  if (id != root)
    z_raise(id);
}
void raise_window(int slot)
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
int blocking_modal(int idx)
{
  if (idx < 0)
    return -1;
  int mid = family_active_modal(cur[idx].id);
  if (mid < 0 || is_descendant(cur[idx].id, mid))
    return -1;
  return mid;
}

/* A modal being flashed to signal it holds input; counts down in frames */
int flash_id = -1, flash_frames;
void flash_modal(int id)
{
  flash_id = id;
  flash_frames = 8;
  force_full = true;
}

/* Alt+Tab */
bool at_active;
int at_order[WM_LIST_MAX]; /* window ids, most-recent first */
int at_count;
int at_sel;

int alttab_build(void)
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
bool btn_present(const struct winrec *w, int kind)
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
int titlebtn_box(const struct winrec *w, int kind, int *bx, int *by)
{
  int slot = btn_slot(w, kind);
  if (slot < 0)
    return 0;
  *bx = w->x + w->w - 4 - (slot + 1) * TB_BTN_W - slot * TB_BTN_GAP;
  *by = w->y + 4;
  return 1;
}
int titlebtn_hit(const struct winrec *w, int mx, int my)
{
  if (fs_active(w->id))
    return -1;
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
static int tbstack[WM_LIST_MAX];
static int ntb;
static int taskbar_layout(int left, int right, int *idxs, int *bxs, int *bws, int max)
{
  int n = 0;
  for (int i = 0; i < ntb;) {
    if (slot_of(tbstack[i]) < 0) {
      memmove(&tbstack[i], &tbstack[i + 1], (unsigned)(ntb - i - 1) * sizeof tbstack[0]);
      ntb--;
    } else
      i++;
  }
  for (int i = 0; i < nwin && ntb < WM_LIST_MAX; i++) {
    int id = cur[i].id;
    bool known = false;
    for (int k = 0; k < ntb && !known; k++)
      known = tbstack[k] == id;
    if (!known)
      tbstack[ntb++] = id;
  }
  /* One button per top-level window, in creation order so focus changes don't reshuffle them */
  for (int k = 0; k < ntb && n < max; k++)
    if (parent_id_of(tbstack[k]) < 0)
      idxs[n++] = slot_of(tbstack[k]);
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

struct taskbar_cache tb_cache;

void taskbar_layout_cached(int right)
{
  if (!taskbar_layout_dirty && tb_cache.valid && tb_cache.right == right)
    return;
  tb_cache.right = right;
  tb_cache.n = taskbar_layout(tb_band_left(), right, tb_cache.idxs, tb_cache.bxs, tb_cache.bws, WM_LIST_MAX);
  tb_cache.valid = true;
  taskbar_layout_dirty = false;
}

int taskbar_hit(int mx, int my)
{
  if (my < bar_top())
    return -1;
  taskbar_layout_cached(tb_band_right);
  for (int k = 0; k < tb_cache.n; k++)
    if (mx >= tb_cache.bxs[k] && mx < tb_cache.bxs[k] + tb_cache.bws[k])
      return tb_cache.idxs[k];
  return -1;
}

/* cursor pick */

bool hover_resizing;
int rsz_edge_hint;

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

int resize_hit(const struct winrec *w, int mx, int my)
{
  if (!(w->flags & UWIN_F_RESIZABLE) || fs_active(w->id))
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
int pick_cursor(int mx, int my)
{
  if (pw_open) {
    int row = pw_row_at(mx, my);
    return row >= 0 && pw_enabled(row) ? CUR_HAND : CUR_ARROW;
  }
  if (hover_resizing)
    return edge_cursor(rsz_edge_hint ? rsz_edge_hint : (RZ_R | RZ_B));

  if (tb_menu_open && mx >= tb_menu_x && mx < tb_menu_x + TBM_W && my >= tb_menu_y && my < tb_menu_y + tbm_h())
    return CUR_HAND;
  if (tray_menu_inside(mx, my))
    return CUR_HAND;
  int bar_y = bar_top();
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
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    if (mx >= cx && mx < cx + cw && my >= cy && my < cy + ch && w->cursor >= 0 && w->cursor <= CUR_PROGRESS)
      return w->cursor;
  }
  return CUR_ARROW;
}

/* Maximize/restore tracked in the compositor (there is no cross-process maximize primitive) */
struct maxrec maxed[WM_LIST_MAX];
int is_maxed(int id)
{
  for (int i = 0; i < WM_LIST_MAX; i++)
    if (maxed[i].used && maxed[i].id == id)
      return i;
  return -1;
}
void maxed_forget_gone(void)
{
  for (int i = 0; i < WM_LIST_MAX; i++)
    if (maxed[i].used && slot_of(maxed[i].id) < 0)
      maxed[i].used = false;
}
void toggle_maximize(int slot)
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

/* Fullscreen: a client asks via UWIN_F_FULLSCREEN, the compositor covers the screen and drops the chrome */
static struct fsrec fsrecs[WM_LIST_MAX];
bool fs_active(int id)
{
  for (int i = 0; i < WM_LIST_MAX; i++)
    if (fsrecs[i].used && fsrecs[i].id == id)
      return true;
  return false;
}
bool fs_top(void)
{
  int t = topmost_visible();
  return t >= 0 && fs_active(cur[t].id);
}
int titlebar_h(const struct winrec *w) { return fs_active(w->id) ? 0 : WM_TITLE_H; }
int bar_top(void) { return fs_top() ? screen_h : screen_h - WM_BAR_H; }

void fullscreen_sync(void)
{
  for (int i = 0; i < WM_LIST_MAX; i++) {
    struct fsrec *f = &fsrecs[i];
    int s = f->used ? slot_of(f->id) : -1;
    if (f->used && s < 0)
      f->used = false;
    else if (f->used && !(cur[s].flags & UWIN_F_FULLSCREEN)) {
      win_set_geom(f->id, f->sx, f->sy, f->sw, f->sh);
      f->used = false;
      cur[s].x = f->sx;
      cur[s].y = f->sy;
      cur[s].w = f->sw;
      cur[s].h = f->sh;
    }
  }
  for (int s = 0; s < nwin; s++) {
    struct winrec *w = &cur[s];
    if (!(w->flags & UWIN_F_FULLSCREEN) || fs_active(w->id))
      continue;
    struct fsrec *f = 0;
    for (int i = 0; i < WM_LIST_MAX && !f; i++)
      if (!fsrecs[i].used)
        f = &fsrecs[i];
    if (!f)
      break;
    *f = (struct fsrec){w->id, true, w->x, w->y, w->w, w->h};
    win_set_geom(w->id, 0, 0, screen_w, screen_h);
    w->x = w->y = 0;
    w->w = screen_w;
    w->h = screen_h;
    raise_family_id(w->id);
    start_open = false;
    tb_menu_open = false;
    force_full = true;
  }
}

void fullscreen_refit(void)
{
  for (int i = 0; i < WM_LIST_MAX; i++)
    if (fsrecs[i].used)
      win_set_geom(fsrecs[i].id, 0, 0, screen_w, screen_h);
}
