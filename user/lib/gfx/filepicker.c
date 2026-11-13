/* filepicker.c, a modal open/save/choose-folder dialog. See gfx.h. */
#include "gfx.h"
#include "owos.h"

#define W 560
#define H 420
#define PAD 12
#define GAP 8
#define ROW_H 22
#define UP_W 44
#define FILTER_W 170
#define BTN_W 84
#define NAME_LABEL_W 52
#define CHK_W 28 /* checkbox column of a multiple-selection list */
#define PATH_MAX_LEN 256
#define PICK_A11Y_NODES 48 /* widgets plus the ~15 rows the list shows at once */

#define COL_BG 0x161d28
#define COL_LIST 0x0d1420
#define COL_BORDER 0x3a4658
#define COL_TEXT 0xd6deea
#define COL_DIM 0x9fb0c4
#define COL_SEL 0x2f6fb0
#define COL_DIR 0xe0b050
#define COL_ERR 0xe07060

struct picker {
  const struct filepicker_opts *o;
  char dir[PATH_MAX_LEN];
  char pathbuf[PATH_MAX_LEN];
  char namebuf[128];
  struct textfield pathtf, nametf;
  struct filelist_entry *ents;
  unsigned char *chk; /* per entry: ticked in a multiple-selection picker */
  int nents;
  int up;    /* 1 when a ".." row leads the list */
  int sel;   /* selected row, or -1 */
  int focus; /* 0 = name field, 1 = path field */
  int drag;  /* the field a left-drag is selecting in, or -1 */
  int hot;   /* hovered button: 0 cancel, 1 primary, or -1 */
  struct scrollview sv;
  struct select_box filter;
  char status[64];
  struct gfx_a11y a11y[PICK_A11Y_NODES];
};

static const char *const k_primary[] = {"Open", "Save", "Select"};

static int multi(const struct picker *p) { return p->o->multiple && p->o->mode == FILEPICK_OPEN; }
static int has_name(const struct picker *p) { return p->o->mode != FILEPICK_FOLDER && !multi(p); }
static int has_filter(const struct picker *p) { return p->o->exts && p->o->mode != FILEPICK_FOLDER; }

static void geom_top(const struct picker *p, int *pathw)
{
  *pathw = W - 2 * PAD - UP_W - GAP - (has_filter(p) ? FILTER_W + GAP : 0);
}

static int name_y(void) { return H - PAD - GUI_BTN_H - GAP - TEXTFIELD_H; }
static int list_y(void) { return PAD + TEXTFIELD_H + GAP; }
static int list_h(const struct picker *p) { return (has_name(p) ? name_y() : H - PAD - GUI_BTN_H) - GAP - list_y(); }
static int btn_y(void) { return H - PAD - GUI_BTN_H; }
static int cancel_x(void) { return W - PAD - 2 * BTN_W - GAP; }
static int primary_x(void) { return W - PAD - BTN_W; }

static const struct filelist_entry *row_entry(const struct picker *p, int row)
{
  int i = row - p->up;
  return i >= 0 && i < p->nents ? &p->ents[i] : NULL;
}

static int row_count(const struct picker *p) { return p->nents + p->up; }

static void reload(struct picker *p)
{
  free(p->ents);
  free(p->chk);
  p->ents = NULL;
  const char *const *exts = p->filter.selected == 0 ? p->o->exts : NULL;
  p->nents = filelist_read(p->dir, exts, p->o->mode == FILEPICK_FOLDER, &p->ents);
  if (p->nents < 0) {
    p->nents = 0;
    strlcpy(p->status, "Out of memory", sizeof p->status);
  }
  p->chk = calloc(p->nents + 1, 1);
  p->up = !streq(p->dir, "/");
  p->sel = -1;
  p->sv.scroll = 0;
  textfield_set(&p->pathtf, p->dir);
}

static void enter_dir(struct picker *p, const char *path)
{
  strlcpy(p->dir, path, sizeof p->dir);
  for (int n = strlen(p->dir); n > 1 && p->dir[n - 1] == '/'; n--)
    p->dir[n - 1] = 0;
  p->status[0] = 0;
  reload(p);
}

static void go_parent(struct picker *p)
{
  char path[PATH_MAX_LEN];
  strlcpy(path, p->dir, sizeof path);
  if (filelist_parent(path))
    enter_dir(p, path);
}

static int is_dir_path(const char *path)
{
  struct statinfo st;
  return stat(path, &st) == 0 && st.is_dir;
}

static void select_row(struct picker *p, int row)
{
  p->sel = row;
  scrollview_reveal(&p->sv, row * ROW_H, ROW_H);
  const struct filelist_entry *e = row_entry(p, row);
  if (!e || !has_name(p))
    return;
  if (!e->is_dir)
    textfield_set(&p->nametf, e->name);
  else if (p->o->mode == FILEPICK_OPEN)
    textfield_set(&p->nametf, "");
}

static void toggle_row(struct picker *p, int row)
{
  const struct filelist_entry *e = row_entry(p, row);
  if (e && !e->is_dir && p->chk)
    p->chk[row - p->up] ^= 1;
}

static void open_row(struct picker *p, int row)
{
  const struct filelist_entry *e = row_entry(p, row);
  if (!e) {
    go_parent(p);
    return;
  }
  char path[PATH_MAX_LEN];
  filelist_join(path, sizeof path, p->dir, e->name);
  enter_dir(p, path);
}

static void set_status(struct picker *p, const char *msg) { strlcpy(p->status, msg, sizeof p->status); }

/* 1 when `out` holds the chosen path; 0 when the key press only navigated or was refused */
static int accept(struct picker *p, char *out, int outsz)
{
  char path[PATH_MAX_LEN];
  if (p->o->mode == FILEPICK_FOLDER) {
    const struct filelist_entry *e = p->sel >= 0 ? row_entry(p, p->sel) : NULL;
    if (e)
      filelist_join(path, sizeof path, p->dir, e->name);
    else
      strlcpy(path, p->dir, sizeof path);
    strlcpy(out, path, outsz);
    return 1;
  }
  if (multi(p)) {
    int n = 0, off = 0;
    for (int i = 0; i < p->nents && p->chk; i++) {
      if (!p->chk[i])
        continue;
      filelist_join(path, sizeof path, p->dir, p->ents[i].name);
      int l = strlen(path);
      if (off + l + 2 > outsz)
        break;
      memcpy(out + off, path, l + 1);
      off += l + 1;
      n++;
    }
    const struct filelist_entry *cur = p->sel >= 0 ? row_entry(p, p->sel) : NULL;
    if (!n && cur && !cur->is_dir) {
      filelist_join(path, sizeof path, p->dir, cur->name);
      strlcpy(out, path, outsz - 1);
      off = strlen(out) + 1;
      n = 1;
    }
    if (!n) {
      set_status(p, "Tick at least one file");
      return 0;
    }
    out[off] = 0;
    return n;
  }
  const char *name = p->nametf.buf;
  if (!name[0]) {
    if (p->sel >= 0 && (p->sel < p->up || row_entry(p, p->sel)->is_dir))
      open_row(p, p->sel);
    else
      set_status(p, "Enter a file name");
    return 0;
  }
  filelist_join(path, sizeof path, p->dir, name);
  if (is_dir_path(path)) {
    enter_dir(p, path);
    textfield_set(&p->nametf, "");
    return 0;
  }
  if (p->o->mode == FILEPICK_OPEN && path_access(path, R_OK) == -1) {
    set_status(p, "File not found");
    return 0;
  }
  strlcpy(out, path, outsz);
  return 1;
}

static void fmt_size(char *buf, int n, unsigned size)
{
  if (size < 1024)
    snprintf(buf, n, "%u B", size);
  else if (size < 1024 * 1024)
    snprintf(buf, n, "%u KB", size >> 10);
  else
    snprintf(buf, n, "%u MB", size >> 20);
}

void filepicker_icon(int x, int y, int is_dir)
{
  if (is_dir) {
    gfx_fill_rect(x, y, 6, 3, COL_DIR);
    gfx_fill_rect(x, y + 2, 14, 10, COL_DIR);
  } else {
    gfx_fill_rect(x + 2, y, 10, 12, COL_DIM);
    gfx_fill_rect(x + 4, y + 3, 6, 1, COL_LIST);
    gfx_fill_rect(x + 4, y + 6, 6, 1, COL_LIST);
  }
}

static void draw_row(const struct picker *p, int row, int x, int y, int w)
{
  const struct filelist_entry *e = row_entry(p, row);
  int ticked = multi(p) && e && !e->is_dir && p->chk[row - p->up];
  unsigned st = (row == p->sel ? GFX_A11Y_SELECTED : 0) | (ticked ? GFX_A11Y_CHECKED : 0);
  struct gfx_a11y *a = gfx_a11y_add(GFX_ROLE_LISTITEM, e ? e->name : "..", st, x, y, w, ROW_H);
  gfx_a11y_set_text(a, !e ? "parent folder" : e->is_dir ? "folder" : 0);
  gfx_a11y_set_value(a, row + 1, 0, row_count(p));
  if (row == p->sel)
    gfx_fill_rect(x, y, w, ROW_H, COL_SEL);
  int ix = x + GAP + (multi(p) ? CHK_W - GAP : 0);
  if (multi(p) && e && !e->is_dir) {
    gfx_rect(x + GAP, y + (ROW_H - 12) / 2, 12, 12, COL_DIM);
    if (p->chk[row - p->up])
      gfx_fill_rect(x + GAP + 3, y + (ROW_H - 12) / 2 + 3, 6, 6, COL_TEXT);
  }
  filepicker_icon(ix, y + (ROW_H - 12) / 2, !e || e->is_dir);
  int tx = ix + 14 + GAP, right = x + w - GAP;
  if (e && !e->is_dir) {
    char sz[16];
    fmt_size(sz, sizeof sz, e->size);
    int sw = gfx_ui_width(sz);
    gfx_ui_text(right - sw, y + (ROW_H - UIFONT_H) / 2, sz, row == p->sel ? COL_TEXT : COL_DIM);
    right -= sw + GAP;
  }
  gfx_ui_text_clip(tx, y + (ROW_H - UIFONT_H) / 2, e ? e->name : "..", COL_TEXT, right - tx);
}

static void draw(struct picker *p)
{
  gfx_clear(COL_BG);
  int pathw;
  geom_top(p, &pathw);
  int fx = W - PAD - FILTER_W;

  button_draw(PAD, PAD, UP_W, TEXTFIELD_H, "Up", p->up ? p->hot == 2 ? BTN_HOVER : BTN_NORMAL : BTN_DISABLED, NULL);
  textfield_draw(&p->pathtf, PAD + UP_W + GAP, PAD, pathw, TEXTFIELD_H, p->focus == 1, NULL);
  gfx_a11y_set_name(gfx_a11y_last(), "Folder");

  int ly = list_y(), lh = list_h(p);
  gfx_fill_rect(PAD, ly, W - 2 * PAD, lh, COL_LIST);
  gfx_rect(PAD, ly, W - 2 * PAD, lh, COL_BORDER);
  int top = scrollview_begin(&p->sv, PAD + 1, ly + 1, W - 2 * PAD - 2, lh - 2, row_count(p) * ROW_H);
  int cw = scrollview_content_w(&p->sv);
  for (int r = 0; r < row_count(p); r++) {
    int y = top + r * ROW_H;
    if (y + ROW_H > ly && y < ly + lh)
      draw_row(p, r, PAD + 1, y, cw);
  }
  scrollview_end(&p->sv);

  if (has_name(p)) {
    gfx_ui_text(PAD, name_y() + (TEXTFIELD_H - UIFONT_H) / 2, "Name", COL_DIM);
    textfield_draw(&p->nametf, PAD + NAME_LABEL_W, name_y(), W - 2 * PAD - NAME_LABEL_W, TEXTFIELD_H, p->focus == 0,
                   NULL);
    gfx_a11y_set_name(gfx_a11y_last(), "Name");
  }

  int sw = cancel_x() - GAP - PAD;
  if (p->status[0])
    gfx_a11y_add(GFX_ROLE_LABEL, p->status, 0, PAD, btn_y(), sw, GUI_BTN_H);
  gfx_ui_text_clip(PAD, btn_y() + (GUI_BTN_H - UIFONT_H) / 2, p->status, COL_ERR, sw);
  button_draw(cancel_x(), btn_y(), BTN_W, GUI_BTN_H, "Cancel", p->hot == 0 ? BTN_HOVER : BTN_NORMAL, NULL);
  button_draw(primary_x(), btn_y(), BTN_W, GUI_BTN_H, k_primary[p->o->mode], p->hot == 1 ? BTN_HOVER : BTN_NORMAL,
              NULL);

  if (has_filter(p))
    select_draw(&p->filter, fx, PAD + (TEXTFIELD_H - GUI_SELECT_H) / 2, FILTER_W, GUI_SELECT_H, NULL);
}

/* The hovered button for a pointer at (mx,my): 0 cancel, 1 primary, 2 up, or -1 */
static int button_at(const struct picker *p, int mx, int my)
{
  if (button_hit(cancel_x(), btn_y(), BTN_W, GUI_BTN_H, mx, my, NULL))
    return 0;
  if (button_hit(primary_x(), btn_y(), BTN_W, GUI_BTN_H, mx, my, NULL))
    return 1;
  if (p->up && button_hit(PAD, PAD, UP_W, TEXTFIELD_H, mx, my, NULL))
    return 2;
  return -1;
}

/* 1 when the picker is done: *result = 1 with `out` filled, or 0 for cancel */
static int on_mouse(struct picker *p, int mx, int my, char *out, int outsz, int *result)
{
  int pathw;
  geom_top(p, &pathw);
  int fy = PAD + (TEXTFIELD_H - GUI_SELECT_H) / 2;
  if (has_filter(p)) {
    int r = select_click(&p->filter, W - PAD - FILTER_W, fy, FILTER_W, GUI_SELECT_H, mx, my, NULL);
    if (r == 1)
      reload(p);
    if (r || p->filter.open)
      return 0;
  }
  p->drag = -1;
  int b = button_at(p, mx, my);
  if (b == 0) {
    *result = 0;
    return 1;
  }
  if (b == 1) {
    *result = accept(p, out, outsz);
    return *result;
  }
  if (b == 2) {
    go_parent(p);
    return 0;
  }
  if (textfield_click(&p->pathtf, mx, my, PAD + UP_W + GAP, PAD, pathw, TEXTFIELD_H, NULL)) {
    p->focus = 1;
    p->drag = 1;
    return 0;
  }
  if (has_name(p) && textfield_click(&p->nametf, mx, my, PAD + NAME_LABEL_W, name_y(), W - 2 * PAD - NAME_LABEL_W,
                                     TEXTFIELD_H, NULL)) {
    p->focus = 0;
    p->drag = 0;
    return 0;
  }
  if (scrollview_press(&p->sv, mx, my))
    return 0;
  if (mx >= p->sv.x && mx < p->sv.x + scrollview_content_w(&p->sv) && my >= p->sv.y && my < p->sv.y + p->sv.h) {
    int row = (my - p->sv.y + p->sv.scroll) / ROW_H;
    if (row < row_count(p)) {
      select_row(p, row);
      if (multi(p) && mx < p->sv.x + CHK_W)
        toggle_row(p, row);
    }
  }
  return 0;
}

static int on_key(struct picker *p, int c, int mods, char *out, int outsz, int *result)
{
  if (c == 27) {
    if (p->filter.open)
      p->filter.open = 0;
    else {
      *result = 0;
      return 1;
    }
    return 0;
  }
  if (p->filter.open) {
    if (select_key(&p->filter, (char)c) == 1)
      reload(p);
    return 0;
  }
  if (c == '\t') {
    if (p->o->mode != FILEPICK_FOLDER)
      p->focus = !p->focus;
    return 0;
  }
  if (c == '\n') {
    if (p->focus == 1) {
      if (p->pathtf.buf[0] != '/' || !is_dir_path(p->pathtf.buf))
        set_status(p, "Not a folder");
      else if (p->o->mode == FILEPICK_FOLDER && streq(p->pathtf.buf, p->dir))
        *result = accept(p, out, outsz);
      else
        enter_dir(p, p->pathtf.buf);
      return *result;
    }
    *result = accept(p, out, outsz);
    return *result;
  }
  if (c == KEY_UP || c == KEY_DOWN) {
    int rows = row_count(p);
    int to = p->sel < 0 ? (c == KEY_DOWN ? 0 : rows - 1) : p->sel + (c == KEY_DOWN ? 1 : -1);
    if (to >= 0 && to < rows)
      select_row(p, to);
    return 0;
  }
  if (!p->focus && !has_name(p)) {
    if (c == ' ')
      toggle_row(p, p->sel);
    else if (c == 'a' && (mods & KMOD_CTRL) && p->chk)
      for (int i = 0; i < p->nents; i++)
        p->chk[i] = !p->ents[i].is_dir;
    return 0;
  }
  textfield_key(p->focus ? &p->pathtf : &p->nametf, (char)c, mods);
  return 0;
}

int filepicker(int parent, const struct filepicker_opts *o, char *out, int outsz)
{
  struct picker *p = calloc(1, sizeof *p);
  if (!p)
    return 0;
  p->o = o;
  p->sel = -1;
  p->drag = -1;
  p->hot = -1;
  p->focus = o->mode == FILEPICK_FOLDER ? 1 : 0;
  textfield_init(&p->pathtf, p->pathbuf, sizeof p->pathbuf);
  textfield_init(&p->nametf, p->namebuf, sizeof p->namebuf);
  scrollview_init(&p->sv, ROW_H, NULL);
  const char *const filters[2] = {o->filter_label ? o->filter_label : "Supported files", "All files"};
  select_init(&p->filter, filters, 2, 0);
  textfield_set(&p->nametf, o->name ? o->name : "");
  strlcpy(p->dir, o->start && is_dir_path(o->start) ? o->start : "/", sizeof p->dir);
  reload(p);

  unsigned *save_fb;
  int save_w, save_h;
  gfx_get(&save_fb, &save_w, &save_h);
  int id = win_create(W, H, o->title ? o->title : "");
  unsigned *fb = id < 0 ? NULL : win_fb_of(id);
  if (!fb) {
    if (id >= 0)
      win_close(id);
    free(p->ents);
    free(p->chk);
    free(p);
    return 0;
  }
  gfx_init(fb, W, H);
  win_resizable(id, 0);
  if (parent >= 0)
    win_set_parent(id, parent, 1);
  struct gfx_a11y_tree tree;
  gfx_a11y_tree_init(&tree, p->a11y, PICK_A11Y_NODES);
  struct gfx_a11y_tree *caller_tree = gfx_a11y_bind(&tree);

  int done = 0, result = 0, need = 1;
  while (!done) {
    struct win_event ev;
    while (!done && win_poll(id, &ev)) {
      need = 1;
      switch (ev.type) {
      case WEV_CLOSE:
        done = 1;
        break;
      case WEV_KEY:
        done = on_key(p, ev.a, ev.b, out, outsz, &result);
        break;
      case WEV_MOUSE:
        done = on_mouse(p, ev.a, ev.b, out, outsz, &result);
        break;
      case WEV_DBLCLICK:
        if (!p->filter.open && p->sel >= 0 && ev.a >= p->sv.x && ev.a < p->sv.x + scrollview_content_w(&p->sv) &&
            ev.b >= p->sv.y && ev.b < p->sv.y + p->sv.h && !(multi(p) && ev.a < p->sv.x + CHK_W)) {
          const struct filelist_entry *e = row_entry(p, p->sel);
          if (!e || e->is_dir)
            open_row(p, p->sel);
          else
            done = result = accept(p, out, outsz);
        }
        break;
      case WEV_DRAG:
        if (p->drag >= 0) {
          int pathw;
          geom_top(p, &pathw);
          if (p->drag == 1)
            textfield_drag(&p->pathtf, ev.a, PAD + UP_W + GAP, pathw, NULL);
          else
            textfield_drag(&p->nametf, ev.a, PAD + NAME_LABEL_W, W - 2 * PAD - NAME_LABEL_W, NULL);
        } else
          scrollview_drag(&p->sv, ev.a, ev.b);
        break;
      case WEV_SCROLL:
        scrollview_wheel(&p->sv, ev.a);
        break;
      case WEV_MOVE:
        p->hot = ev.a < 0 ? -1 : button_at(p, ev.a, ev.b);
        if (has_filter(p) && ev.a >= 0)
          select_motion(&p->filter, W - PAD - FILTER_W, PAD + (TEXTFIELD_H - GUI_SELECT_H) / 2, FILTER_W, GUI_SELECT_H,
                        ev.a, ev.b, NULL);
        break;
      }
    }
    if (done)
      break;
    if (need) {
      gfx_a11y_begin();
      draw(p);
      win_present(id);
      need = 0;
    }
    win_wait(id);
  }
  win_close(id);
  gfx_init(save_fb, save_w, save_h);
  gfx_a11y_bind(caller_tree);
  free(p->ents);
  free(p->chk);
  free(p);
  return result;
}
