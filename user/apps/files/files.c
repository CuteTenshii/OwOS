/* files.c, a navigable file explorer for owofs */
#include "dialog.h"
#include "fontstore.h"
#include "gfx.h"
#include "owos.h"
#include <stdbool.h>

static int W = 560, H = 400; /* client size; changes on resize */
#define MENU_H 18            /* top menu bar */
#define TB_H 34              /* toolbar under it: history buttons and the path field */
#define HEADER_H 22          /* sidebar title and the list's column header */
#define ROW_H 22
#define STATUS_H 22
#define FMAX 512
#define SIDEBAR_W 120               /* default sidebar width (a customizable property) */
#define SIDEBAR_MIN 72              /* narrowest the divider drag may make it */
#define SIDEBAR_MAX 240             /* widest */
#define TOP_Y (MENU_H + TB_H)       /* y of the sidebar title and the column header */
#define LIST_TOP (TOP_Y + HEADER_H) /* y of the first places/list row */
#define HIST_N 16
#define TB_BTN_W 34
#define TB_PAD 6
#define COL_MOD_W 132 /* Modified column; hidden when the list is narrow */
#define COL_SIZE_W 76 /* Size column */

#define COL_BG 0x12161e
#define COL_SIDE 0x0e1218
#define COL_BAR 0x232a36
#define COL_HDR 0x1d2430
#define COL_DIV 0x05070a
#define COL_TEXT 0xc8d0dc
#define COL_DIM 0x8aa0b8
#define COL_SEL 0x2f6fb0
#define COL_SIDESEL 0x24405c
#define COL_HOVER 0x1a212c
#define COL_ERR 0xe07060

/* The left navigation pane is a gfx sidebar widget */
static struct sidebar sb;

struct fent {
  char name[64];
  bool is_dir;
  unsigned size, mtime;
}; /* name: owofs FS_NAME_LEN */

enum { SORT_NAME, SORT_MOD, SORT_SIZE };

static char path[256] = "/";
static int sel = -1, hover = -1, hot_btn = -1;
static unsigned seq;
static struct fent fent_buf[FMAX];
static int fent_count;
static char list_path[256]; /* path the cached listing reflects */
static int force_relist;    /* set by F5/Refresh to re-read in place */
static struct scrollview sv;
static int sort_col = SORT_NAME, sort_desc;
static int g_id;

static char hist[HIST_N][256]; /* visited folders, for Back and Forward */
static int hist_n, hist_i;

static char pathbuf[256]; /* the editable path field */
static struct textfield pathtf;
static int path_focus, path_drag;
static char status[64]; /* a message that replaces the status summary until the next navigation */

/* The "Go" menu */
static const char *const go_items[] = {"Back", "Forward", "Up", "Home", "Refresh", "Open"};
static const char *const go_hints[] = {"Alt+Left", "Alt+Right", "Backspace", "", "F5", "Ctrl+O"};
static const unsigned char go_keys[] = {0, 0, 0, 0, KEY_F5, KEY_OPEN};
static struct menu gomenu = {"Go", go_items, 6, 0, go_hints, go_keys};

static int flen(const char *s)
{
  int n = 0;
  while (s[n])
    n++;
  return n;
}
static void fcpy(char *d, const char *s)
{
  int i = 0;
  while ((d[i] = s[i]))
    i++;
}
static int feq(const char *a, const char *b)
{
  int i = 0;
  while (a[i] && a[i] == b[i])
    i++;
  return a[i] == b[i];
}

static void u32str(unsigned v, char *out)
{
  char t[12];
  int n = 0;
  if (v == 0) {
    out[0] = '0';
    out[1] = 0;
    return;
  }
  while (v) {
    t[n++] = '0' + v % 10;
    v /= 10;
  }
  int i = 0;
  while (n)
    out[i++] = t[--n];
  out[i] = 0;
}

/* Format a byte count as "N B" */
static void size_str(unsigned v, char *out)
{
  static const char *const unit[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  int u = 0;
  unsigned whole = v;
  while (whole >= 1024 && u < 4) {
    whole >>= 10;
    u++;
  }

  char t[12];
  u32str(whole, t);
  int i = 0;
  for (int j = 0; t[j]; j++)
    out[i++] = t[j];

  if (u > 0) { /* one fractional digit for scaled units */
    unsigned rem = v & ((1u << (10 * u)) - 1);
    out[i++] = '.';
    out[i++] = '0' + (((rem >> (10 * (u - 1))) * 10) >> 10);
  }
  out[i++] = ' ';
  for (const char *p = unit[u]; *p; p++)
    out[i++] = *p;
  out[i] = 0;
}

/* Build the absolute path of `name` under the current directory into out */
static void full_path(const char *name, char *out, int max) { filelist_join(out, max, path, name); }

static void set_title(void)
{
  char t[64];
  snprintf(t, sizeof t, "files  %s", path);
  win_settitle(g_id, t);
}

/* Show `p`; a user-driven move is recorded so Back and Forward can retrace it */
static void set_path(const char *p, bool record)
{
  strlcpy(path, p, sizeof path);
  for (int n = flen(path); n > 1 && path[n - 1] == '/'; n--)
    path[n - 1] = 0;
  if (record && !(hist_n && feq(hist[hist_i], path))) {
    if (hist_i + 1 < hist_n)
      hist_n = hist_i + 1;
    if (hist_n == HIST_N) {
      memmove(hist[0], hist[1], (HIST_N - 1) * sizeof hist[0]);
      hist_n--;
    }
    strlcpy(hist[hist_n++], path, sizeof hist[0]);
    hist_i = hist_n - 1;
  }
  sel = hover = -1;
  sv.scroll = 0;
  status[0] = 0;
  path_focus = 0;
  textfield_set(&pathtf, path);
  set_title();
  seq++;
}

static void go_up(void)
{
  char p[256];
  strlcpy(p, path, sizeof p);
  if (filelist_parent(p))
    set_path(p, true);
}

static void go_hist(int d)
{
  int i = hist_i + d;
  if (i < 0 || i >= hist_n)
    return;
  hist_i = i;
  set_path(hist[i], false);
}

static void enter(const char *name)
{
  char p[256];
  full_path(name, p, sizeof p);
  set_path(p, true);
}

static int ci_cmp(const char *a, const char *b)
{
  while (*a && lower(*a) == lower(*b)) {
    a++;
    b++;
  }
  return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}

/* Folders first; then the sort column, ties by name */
static bool before(const struct fent *a, const struct fent *b)
{
  if (a->is_dir != b->is_dir)
    return a->is_dir;
  int c = 0;
  if (sort_col == SORT_MOD)
    c = a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
  else if (sort_col == SORT_SIZE)
    c = a->size < b->size ? -1 : a->size > b->size;
  if (c)
    return sort_desc ? c > 0 : c < 0;
  c = ci_cmp(a->name, b->name);
  return (sort_col == SORT_NAME && sort_desc) ? c > 0 : c < 0;
}

/* Sort entries [first, n); ".." stays above them */
static void sort_entries(int first, int n)
{
  for (int i = first + 1; i < n; i++) {
    struct fent t = fent_buf[i];
    int j = i;
    while (j > first && before(&t, &fent_buf[j - 1])) {
      fent_buf[j] = fent_buf[j - 1];
      j--;
    }
    fent_buf[j] = t;
  }
}

static int collect(void)
{
  int n = 0;
  struct dirent_u d;
  if (!feq(path, "/")) {
    fcpy(fent_buf[n].name, "..");
    fent_buf[n].is_dir = true;
    fent_buf[n].size = fent_buf[n].mtime = 0;
    n++;
  }
  int first = n;
  for (int i = 0; n < FMAX && readdir(path, i, &d) == 1; i++) {
    struct fent *e = &fent_buf[n++];
    struct statinfo st;
    char fp[256];
    strlcpy(e->name, d.name, sizeof e->name);
    e->is_dir = d.is_dir;
    e->size = d.is_dir ? 0 : d.size;
    full_path(d.name, fp, sizeof fp);
    e->mtime = stat(fp, &st) == 0 ? st.mtime : 0;
  }
  sort_entries(first, n);
  return n;
}

/* Re-read the directory only when the path changed or an explicit refresh was asked for */
static void refresh(void)
{
  if (!force_relist && feq(list_path, path))
    return;
  fent_count = collect();
  fcpy(list_path, path);
  force_relist = 0;
}

/* Re-sort the shown listing, keeping the selection on the same entry */
static void resort(void)
{
  char keep[64];
  keep[0] = 0;
  if (sel >= 0 && sel < fent_count)
    fcpy(keep, fent_buf[sel].name);
  int first = (fent_count && feq(fent_buf[0].name, "..")) ? 1 : 0;
  sort_entries(first, fent_count);
  sel = -1;
  for (int i = 0; keep[0] && i < fent_count; i++)
    if (feq(fent_buf[i].name, keep))
      sel = i;
  seq++;
}

static const struct {
  const char *label, *path;
} places[] = {
    {"Root", "/"},
    {"bin", "/bin"},
    {"home", "/home"},
    {"root", "/home/root"},
};
#define NPLACES (int)(sizeof(places) / sizeof(places[0]))

/* Removable volumes (USB sticks, etc.) */
static struct volinfo vols[6];
static int nvols;
static void scan_vols(void)
{
  nvols = 0;
  while (nvols < 6 && volumes(nvols, &vols[nvols]) == 1)
    nvols++;
}

/* 1 if `p` is `base` or sits inside it (so a device stays highlighted while you browse into it) */
static int under_path(const char *p, const char *base)
{
  int i = 0;
  while (base[i] && base[i] == p[i])
    i++;
  if (base[i])
    return 0;
  return p[i] == '\0' || p[i] == '/';
}

/* Sidebar geometry for the Devices section (below Places, after a small gap) */
static int dev_header_y(void) { return LIST_TOP + NPLACES * ROW_H + 6; }
static int dev_row_y(int i) { return dev_header_y() + ROW_H + i * ROW_H; }
#define EJECT_W 18 /* right-edge hit zone for the eject button */

/* A tiny eject glyph: a triangle over a tray bar, drawn in a ~7x9 box */
static void draw_eject(int x, int y, unsigned c)
{
  gfx_fill_rect(x + 3, y + 3, 1, 1, c);
  gfx_fill_rect(x + 2, y + 4, 3, 1, c);
  gfx_fill_rect(x + 1, y + 5, 5, 1, c);
  gfx_fill_rect(x + 1, y + 7, 5, 1, c);
}

/* A "connect" glyph: a down-arrow into a tray bar (insert), the mirror of eject */
static void draw_connect(int x, int y, unsigned c)
{
  gfx_fill_rect(x + 1, y + 3, 5, 1, c);
  gfx_fill_rect(x + 2, y + 4, 3, 1, c);
  gfx_fill_rect(x + 3, y + 5, 1, 1, c);
  gfx_fill_rect(x + 1, y + 7, 5, 1, c);
}

/* The width actually rendered this frame: the widget's property width */
static int sidebar_w(void)
{
  int sbw = sb.w;
  if (sbw > W - 80)
    sbw = W - 80;
  if (sbw < 0)
    sbw = 0;
  return sbw;
}

/* Toolbar geometry: Back, Forward, Up, then the path field */
static int tb_btn_x(int i) { return TB_PAD + i * (TB_BTN_W + 4); }
static int tb_btn_y(void) { return MENU_H + (TB_H - GUI_BTN_H) / 2; }
static int path_x(void) { return tb_btn_x(3) + 4; }
static int path_w(void) { return W - TB_PAD - path_x() > 40 ? W - TB_PAD - path_x() : 40; }
static int path_y(void) { return MENU_H + (TB_H - TEXTFIELD_H) / 2; }

/* 0 Back, 1 Forward, 2 Up under (mx,my), else -1 */
static int tb_btn_at(int mx, int my)
{
  for (int i = 0; i < 3; i++)
    if (button_hit(tb_btn_x(i), tb_btn_y(), TB_BTN_W, GUI_BTN_H, mx, my))
      return i;
  return -1;
}

static bool tb_btn_enabled(int i) { return i == 0 ? hist_i > 0 : i == 1 ? hist_i + 1 < hist_n : !feq(path, "/"); }

/* List geometry: the viewport right of the sidebar, and which columns fit */
static int list_x(void) { return sidebar_w() + 1; }
static int list_w(void) { return W - sidebar_w() - 1; }
static int list_h(void) { return H - LIST_TOP - STATUS_H; }
static bool overflows(void) { return fent_count * ROW_H > list_h(); }
static int content_w(void) { return list_w() - (overflows() ? GUI_SCROLL_W : 0); }

struct columns {
  int mod_x, mod_w, size_x, size_w;
};
static struct columns columns(void)
{
  int cw = content_w();
  struct columns c = {0};
  c.size_w = cw >= 260 ? COL_SIZE_W : 0;
  c.mod_w = cw >= 400 ? COL_MOD_W : 0;
  c.size_x = list_x() + cw - c.size_w;
  c.mod_x = c.size_x - c.mod_w;
  return c;
}

/* The sort column whose header lies under mx, or -1 */
static int header_col(int mx)
{
  struct columns c = columns();
  if (c.size_w && mx >= c.size_x)
    return SORT_SIZE;
  if (c.mod_w && mx >= c.mod_x)
    return SORT_MOD;
  return mx >= list_x() ? SORT_NAME : -1;
}

/* A list entry's row, or -1 for the header, empty space and the scrollbar */
static int list_hit(int mx, int my)
{
  if (mx < list_x() || my < LIST_TOP || my >= H - STATUS_H || (overflows() && mx >= W - GUI_SCROLL_W))
    return -1;
  int e = (my - LIST_TOP + sv.scroll) / ROW_H;
  return (e >= 0 && e < fent_count) ? e : -1;
}

/* "YYYY-MM-DD HH:MM" from an owofs packed stamp, or "-" when unstamped */
static void fmt_mtime(unsigned m, char *out, int cap)
{
  if (!m)
    snprintf(out, cap, "-");
  else
    snprintf(out, cap, "%04u-%02u-%02u %02u:%02u", 2000 + (m >> 26), (m >> 22) & 0xF, (m >> 17) & 0x1F,
             (m >> 12) & 0x1F, (m >> 6) & 0x3F);
}

/* "Folder", "PNG file", or "File" for a name without an extension */
static void type_str(const struct fent *e, char *out, int cap)
{
  const char *dot = 0;
  for (const char *p = e->name; *p; p++)
    if (*p == '.')
      dot = p;
  if (e->is_dir) {
    snprintf(out, cap, "Folder");
  } else if (dot && dot[1] && flen(dot + 1) <= 5) {
    int i = 0;
    for (; dot[1 + i]; i++)
      out[i] = (dot[1 + i] >= 'a' && dot[1 + i] <= 'z') ? (char)(dot[1 + i] - 32) : dot[1 + i];
    snprintf(out + i, cap - i, " file");
  } else {
    snprintf(out, cap, "File");
  }
}

static void draw_sort_arrow(int x, int y)
{
  for (int r = 0; r < 4; r++)
    gfx_fill_rect(x + (sort_desc ? r : 3 - r), y + r, sort_desc ? 7 - 2 * r : 1 + 2 * r, 1, COL_DIM);
}

static void ctx_draw(void); /* right-click popup, painted last (see below) */

static void render(void)
{
  refresh();
  int sbw = sidebar_w(), lx = list_x(), lw = list_w(), lh = list_h();
  int sidebar_h = H - TOP_Y - STATUS_H;

  /* Sidebar: the widget paints the panel base, the right divider and the resize grip */
  sidebar_draw(&sb, 0, TOP_Y, sbw, sidebar_h, COL_SIDE, COL_DIV);
  gfx_fill_rect(0, TOP_Y, sbw, HEADER_H, 0x161d28);
  gfx_ui_text(8, TOP_Y + (HEADER_H - UIFONT_H) / 2, "Places", COL_DIM);
  int ty = (ROW_H - UIFONT_H) / 2;
  for (int i = 0; i < NPLACES; i++) {
    int ry = LIST_TOP + i * ROW_H;
    int here = feq(path, places[i].path);
    if (here)
      gfx_fill_rect(0, ry, sbw, ROW_H, COL_SIDESEL);
    gfx_ui_text_clip(8, ry + ty, places[i].label, here ? 0xffffff : COL_TEXT, sbw - 12);
  }

  /* Devices: every present removable device */
  scan_vols();
  if (nvols > 0) {
    gfx_ui_text(8, dev_header_y() + ty, "Devices", COL_DIM);
    for (int i = 0; i < nvols; i++) {
      int ry = dev_row_y(i);
      int mounted = vols[i].mounted;
      int here = mounted && under_path(path, vols[i].point);
      if (here)
        gfx_fill_rect(0, ry, sbw, ROW_H, COL_SIDESEL);
      unsigned fg = here ? 0xffffff : (mounted ? COL_TEXT : 0x8a96a4);
      gfx_ui_text_clip(8, ry + ty, vols[i].label, fg, sbw - 8 - EJECT_W);
      if (mounted)
        draw_eject(sbw - EJECT_W + 4, ry + 6, COL_DIM);
      else
        draw_connect(sbw - EJECT_W + 4, ry + 6, COL_DIM);
    }
  }

  /* Column header, then the rows inside a scroll view */
  gfx_fill_rect(lx, 0, lw, H, COL_BG);
  gfx_fill_rect(lx, TOP_Y, lw, HEADER_H, COL_HDR);
  struct columns c = columns();
  int hy = TOP_Y + (HEADER_H - UIFONT_H) / 2;
  static const char *const hname[] = {"Name", "Modified", "Size"};
  int hx[3] = {lx + 8 + 14 + 8, c.mod_x + 4, c.size_x + c.size_w - 4 - gfx_ui_width("Size")};
  for (int k = 0; k < 3; k++) {
    if ((k == SORT_MOD && !c.mod_w) || (k == SORT_SIZE && !c.size_w))
      continue;
    gfx_ui_text(hx[k], hy, hname[k], sort_col == k ? 0xffffff : COL_DIM);
    if (sort_col == k)
      draw_sort_arrow(hx[k] + gfx_ui_width(hname[k]) + 6, TOP_Y + (HEADER_H - 4) / 2);
  }
  gfx_fill_rect(lx, LIST_TOP - 1, lw, 1, COL_DIV);

  int top = scrollview_begin(&sv, lx, LIST_TOP, lw, lh, fent_count * ROW_H);
  int cw = scrollview_content_w(&sv);
  for (int e = 0; e < fent_count; e++) {
    int ry = top + e * ROW_H;
    if (ry + ROW_H <= LIST_TOP || ry >= LIST_TOP + lh)
      continue;
    struct fent *fe = &fent_buf[e];
    if (e == sel)
      gfx_fill_rect(lx, ry, cw, ROW_H, COL_SEL);
    else if (e == hover)
      gfx_fill_rect(lx, ry, cw, ROW_H, COL_HOVER);
    filepicker_icon(lx + 8, ry + (ROW_H - 12) / 2, fe->is_dir);
    int nx = lx + 8 + 14 + 8;
    int right = (c.mod_w ? c.mod_x : c.size_w ? c.size_x : lx + cw) - 8;
    gfx_ui_text_clip(nx, ry + ty, fe->name, e == sel ? 0xffffff : COL_TEXT, right - nx);
    unsigned dim = e == sel ? 0xd6deea : COL_DIM;
    if (c.mod_w && !feq(fe->name, "..")) {
      char mb[24];
      fmt_mtime(fe->mtime, mb, sizeof mb);
      gfx_ui_text(c.mod_x + 4, ry + ty, mb, dim);
    }
    if (c.size_w && !fe->is_dir) {
      char sz[16];
      size_str(fe->size, sz);
      gfx_ui_text(c.size_x + c.size_w - 4 - gfx_ui_width(sz), ry + ty, sz, dim);
    }
  }
  scrollview_end(&sv);

  /* Toolbar */
  gfx_fill_rect(0, MENU_H, W, TB_H, COL_BAR);
  gfx_fill_rect(0, TOP_Y - 1, W, 1, COL_DIV);
  static const char *const tb_label[] = {"<", ">", "Up"};
  for (int i = 0; i < 3; i++)
    button_draw(tb_btn_x(i), tb_btn_y(), TB_BTN_W, GUI_BTN_H, tb_label[i],
                !tb_btn_enabled(i) ? BTN_DISABLED
                : hot_btn == i     ? BTN_HOVER
                                   : BTN_NORMAL);
  textfield_draw(&pathtf, path_x(), path_y(), path_w(), TEXTFIELD_H, path_focus);

  /* Status bar: a message, else the selection, else the folder's counts */
  gfx_fill_rect(0, H - STATUS_H, W, STATUS_H, COL_BAR);
  gfx_fill_rect(0, H - STATUS_H, W, 1, COL_DIV);
  char line[160];
  unsigned fg = COL_DIM;
  if (status[0]) {
    snprintf(line, sizeof line, "%s", status);
    fg = COL_ERR;
  } else if (sel >= 0 && sel < fent_count && !feq(fent_buf[sel].name, "..")) {
    struct fent *fe = &fent_buf[sel];
    char sz[16], ty2[16];
    size_str(fe->size, sz);
    type_str(fe, ty2, sizeof ty2);
    if (fe->is_dir)
      snprintf(line, sizeof line, "%s  -  %s", fe->name, ty2);
    else
      snprintf(line, sizeof line, "%s  -  %s  -  %s", fe->name, ty2, sz);
  } else {
    int dirs = 0, files = 0;
    for (int i = 0; i < fent_count; i++) {
      if (feq(fent_buf[i].name, ".."))
        continue;
      if (fent_buf[i].is_dir)
        dirs++;
      else
        files++;
    }
    snprintf(line, sizeof line, "%d folder%s, %d file%s", dirs, dirs == 1 ? "" : "s", files, files == 1 ? "" : "s");
  }
  gfx_ui_text_clip(8, H - STATUS_H + (STATUS_H - UIFONT_H) / 2, line, fg, W - 16);

  /* Top menu bar over everything, then its dropdown drawn last so it overlays the listing */
  gfx_fill_rect(0, 0, W, MENU_H, COL_BAR);
  menu_draw(&gomenu, 2, 0);

  ctx_draw(); /* right-click popup overlays everything */
}
/* File-type associations: a filename extension picks the /bin program that opens it */
static const struct {
  const char *ext;
  const char *prog;
} assoc[] = {
    {"png", "imgview"}, {"webp", "imgview"}, {"pdf", "pdfview"}, {"wav", "player"},
    {"mp3", "player"},  {"caf", "player"},   {"zip", "archive"}, {"tar", "archive"},
    {"tgz", "archive"}, {"gz", "archive"},   {"zst", "archive"}, {"br", "archive"},
};

/* Case-insensitive compare of `a` against the lowercase literal `b` */
static int ext_eq(const char *a, const char *b)
{
  for (; *a && *b; a++, b++) {
    char c = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
    if (c != *b)
      return 0;
  }
  return *a == '\0' && *b == '\0';
}

/* The program to open `name` with: by its extension, else the editor */
static const char *app_for(const char *name)
{
  const char *dot = 0;
  for (const char *p = name; *p; p++)
    if (*p == '.')
      dot = p;
  if (dot)
    for (unsigned i = 0; i < sizeof assoc / sizeof assoc[0]; i++)
      if (ext_eq(dot + 1, assoc[i].ext))
        return assoc[i].prog;
  return "editor";
}

/* Open a file by launching its associated program on it */
static void open_with(const char *name, const char *prog)
{
  char fp[256];
  full_path(name, fp, sizeof fp);
  int pid = fork();
  if (pid == 0) {
    execp1(prog, fp);
    exit(1); /* exec failed: don't fall back into UI */
  }
  if (pid > 0)
    waitpid(pid);
}

static bool is_font_file(const char *name)
{
  const char *dot = 0;
  for (const char *p = name; *p; p++)
    if (*p == '.')
      dot = p;
  return dot &&
         (ext_eq(dot + 1, "ttf") || ext_eq(dot + 1, "otf") || ext_eq(dot + 1, "woff") || ext_eq(dot + 1, "woff2"));
}

static void install_font(int id, const char *name)
{
  char fp[256], msg[128], installed[64];
  full_path(name, fp, sizeof fp);
  snprintf(msg, sizeof msg, "Install font '%s' for all apps?", name);
  if (dialog_msg(id, "Install font", msg, DLG_ICON_QUESTION | DLG_YESNO) != DLG_R_YES)
    return;
  if (fontstore_install(fp, installed) == 0) {
    snprintf(msg, sizeof msg, "Installed %s. Choose it in Settings > Fonts.", installed);
    dialog_msg(id, "Install font", msg, DLG_ICON_INFO);
  } else
    dialog_msg(id, "Install font",
               "Not a usable font (TrueType or OpenType with CFF, WOFF/WOFF2), or /etc/fonts is not writable.",
               DLG_ICON_ERROR);
}

static void open_file(int id, const char *name)
{
  if (is_font_file(name))
    install_font(id, name);
  else
    open_with(name, app_for(name));
}

/* Confirm and remove the entry `name` in the current directory, a file via unlink */
static void delete_entry(int id, const char *name, bool is_dir)
{
  if (feq(name, ".."))
    return;
  char msg[128];
  snprintf(msg, sizeof msg, "Delete %s '%s'?", is_dir ? "folder" : "file", name);
  if (dialog_msg(id, "Delete", msg, DLG_ICON_WARN | DLG_YESNO) != DLG_R_YES)
    return;
  char fp[256];
  full_path(name, fp, sizeof fp);
  int ok = is_dir ? rmdir(fp) : unlink(fp);
  if (ok == 0) {
    sel = -1;
    force_relist = 1;
  } else
    dialog_message(id, "Delete failed",
                   ok == -EACCES ? "Permission denied."
                   : is_dir      ? "The folder could not be removed (is it empty?)."
                                 : "The file could not be removed.");
  seq++;
}

static void rename_entry(int id, const char *name)
{
  char out[96], msg[128];
  if (feq(name, ".."))
    return;
  if (dialog_prompt(id, "Rename", "New name:", name, out, sizeof out) != DLG_R_OK || !out[0] || feq(out, name))
    return;
  const char *err = NULL;
  for (const char *p = out; *p; p++)
    if (*p == '/')
      err = "The name cannot contain '/'.";
  char from[256], to[256];
  struct statinfo st;
  full_path(name, from, sizeof from);
  full_path(out, to, sizeof to);
  if (!err && stat(to, &st) != -1)
    err = "An entry with that name already exists.";
  if (!err) {
    int rc = rename(from, to);
    if (rc == 0) {
      force_relist = 1;
      sel = -1;
    } else
      err = rc == -EACCES ? "Permission denied." : "The entry could not be renamed.";
  }
  if (err) {
    snprintf(msg, sizeof msg, "%s", err);
    dialog_message(id, "Rename failed", msg);
  }
  seq++;
}

/* Properties window */
#define PROP_ROWH 22
#define PROP_PAD 14
#define PROP_LABELW 118
#define PROP_BTN_H 26

static int prop_id = -1; /* the Properties window, or -1 when closed */
static unsigned *prop_fb;
static int prop_w, prop_h, prop_scroll, prop_nrows;
static unsigned prop_seq, prop_last;
static struct {
  const char *label;
  char value[256];
} prop_rows[6];

static void prop_add(const char *label, const char *value)
{
  if (prop_nrows >= (int)(sizeof prop_rows / sizeof prop_rows[0]))
    return;
  prop_rows[prop_nrows].label = label;
  int i = 0;
  for (; value[i] && i < (int)sizeof prop_rows[0].value - 1; i++)
    prop_rows[prop_nrows].value[i] = value[i];
  prop_rows[prop_nrows].value[i] = 0;
  prop_nrows++;
}

static void prop_close_rect(int *bx, int *by, int *bw)
{
  *bw = 74;
  *bx = prop_w - PROP_PAD - *bw;
  *by = prop_h - PROP_PAD - PROP_BTN_H;
}

/* Open (or replace) the Properties window for `name`, built from its facts */
static void show_properties(int mainid, const char *name, bool is_dir, unsigned size)
{
  prop_nrows = 0;
  prop_add("Name", name);
  prop_add("Location", path);
  prop_add("Type", is_dir ? "Folder" : "File");
  if (!is_dir) {
    char b[24];
    size_str(size, b);
    prop_add("Size", b);
  }
  char fp[256];
  full_path(name, fp, sizeof fp);
  prop_add("Full path", fp);

  if (prop_id >= 0) {
    win_close(prop_id);
    prop_id = -1;
  }
  int h = PROP_PAD + prop_nrows * PROP_ROWH + (PROP_PAD + PROP_BTN_H + PROP_PAD);
  if (h > 420)
    h = 420;
  prop_w = 340;
  prop_h = h;
  prop_id = win_create(prop_w, prop_h, "Properties");
  if (prop_id < 0)
    return;
  prop_fb = win_fb_of(prop_id);
  win_set_parent(prop_id, mainid, 0); /* modeless: doesn't block the explorer */
  win_resizable(prop_id, 0);
  prop_scroll = 0;
  prop_seq = 1;
  prop_last = 0;
}

/* Repaint the Properties window into its own buffer */
static void render_props(void)
{
  gfx_init(prop_fb, prop_w, prop_h);
  gfx_fill_rect(0, 0, prop_w, prop_h, 0x161d28);

  int listy = PROP_PAD;
  int listh = prop_h - listy - (PROP_PAD + PROP_BTN_H + PROP_PAD);
  int vis = listh / PROP_ROWH;
  if (vis < 1)
    vis = 1;
  int maxscroll = prop_nrows > vis ? prop_nrows - vis : 0;
  if (prop_scroll > maxscroll)
    prop_scroll = maxscroll;
  if (prop_scroll < 0)
    prop_scroll = 0;

  for (int r = 0; r < vis; r++) {
    int e = prop_scroll + r;
    if (e >= prop_nrows)
      break;
    int y = listy + r * PROP_ROWH;
    if (r & 1)
      gfx_fill_rect(0, y, prop_w, PROP_ROWH, 0x1b2330);
    int ty = y + (PROP_ROWH - UIFONT_H) / 2;
    gfx_ui_text(PROP_PAD, ty, prop_rows[e].label, 0x9fb0c4);
    gfx_set_clip(PROP_PAD + PROP_LABELW, y, prop_w - PROP_PAD - (PROP_PAD + PROP_LABELW), PROP_ROWH);
    gfx_ui_text(PROP_PAD + PROP_LABELW, ty, prop_rows[e].value, 0xd6deea);
    gfx_clip_none();
  }
  if (maxscroll > 0) {
    int knob = listh * vis / prop_nrows;
    if (knob < 12)
      knob = 12;
    int ky = listy + (listh - knob) * prop_scroll / maxscroll;
    gfx_fill_rect(prop_w - 5, listy, 3, listh, 0x0e1420);
    gfx_fill_rect(prop_w - 5, ky, 3, knob, 0x3a4658);
  }
  int bx, by, bw;
  prop_close_rect(&bx, &by, &bw);
  gfx_fill_rect(bx, by, bw, PROP_BTN_H, 0x2f6fb0);
  gfx_rect(bx, by, bw, PROP_BTN_H, 0x05070a);
  gfx_ui_text(bx + (bw - gfx_ui_width("Close")) / 2, by + (PROP_BTN_H - UIFONT_H) / 2, "Close", 0xffffff);

  win_present(prop_id);
  gfx_init(win_fb(), W, H); /* hand the surface back to the explorer window */
}

/* Service the Properties window each frame: drain its events, repaint on change */
static void props_pump(void)
{
  if (prop_id < 0)
    return;
  struct win_event ev;
  while (win_poll(prop_id, &ev)) {
    if (ev.type == WEV_CLOSE) {
      win_close(prop_id);
      prop_id = -1;
      return;
    } else if (ev.type == WEV_KEY) {
      if (ev.a == 27 || ev.a == '\n') {
        win_close(prop_id);
        prop_id = -1;
        return;
      } else if (ev.a == KEY_UP) {
        prop_scroll--;
        prop_seq++;
      } else if (ev.a == KEY_DOWN) {
        prop_scroll++;
        prop_seq++;
      }
    } else if (ev.type == WEV_SCROLL) {
      prop_scroll -= ev.a;
      prop_seq++;
    } else if (ev.type == WEV_MOUSE) {
      int bx, by, bw;
      prop_close_rect(&bx, &by, &bw);
      if (ev.a >= bx && ev.a < bx + bw && ev.b >= by && ev.b < by + PROP_BTN_H) {
        win_close(prop_id);
        prop_id = -1;
        return;
      }
    }
  }
  if (prop_seq != prop_last) {
    render_props();
    prop_last = prop_seq;
  }
}

/* context menu */
/* A right-click popup whose items depend on what sits under the cursor: a file, a directory */
enum ctx_kind { CTX_BG, CTX_FILE, CTX_DIR, CTX_PLACE, CTX_DEVICE, CTX_DEVICE_OFF };
enum ctx_act { A_OPEN, A_EDIT, A_RENAME, A_DELETE, A_PROPS, A_EJECT, A_CONNECT, A_NEWDIR, A_REFRESH, A_UP, A_HOME };

static const char *const ci_file[] = {"Open", "Edit", "Rename", "Delete", "Properties"};
static const int ca_file[] = {A_OPEN, A_EDIT, A_RENAME, A_DELETE, A_PROPS};
static const char *const ci_dir[] = {"Open", "Rename", "Delete", "Properties"};
static const int ca_dir[] = {A_OPEN, A_RENAME, A_DELETE, A_PROPS};
static const char *const ci_place[] = {"Open"};
static const int ca_place[] = {A_OPEN};
static const char *const ci_device[] = {"Open", "Unmount"};
static const int ca_device[] = {A_OPEN, A_EJECT};
static const char *const ci_device_off[] = {"Connect"};
static const int ca_device_off[] = {A_CONNECT};
static const char *const ci_bg[] = {"New Folder", "Refresh", "Go Up", "Home"};
static const int ca_bg[] = {A_NEWDIR, A_REFRESH, A_UP, A_HOME};

static struct {
  bool open;
  int x, y; /* popup top-left (clamped into the window) */
  enum ctx_kind kind;
  int target;    /* place / device / entry index */
  char name[64]; /* file / dir entry name (owofs FS_NAME_LEN) */
  const int *acts;
  struct menu menu; /* the gfx popup list */
} ctx;

/* Open the popup at (mx,my) for the given target, clamped to stay on-screen */
static void ctx_open(enum ctx_kind kind, int target, const char *name, int mx, int my)
{
  static const struct {
    const char *const *items;
    const int *acts;
    int n;
  } kinds[] = {
      [CTX_BG] = {ci_bg, ca_bg, 4},
      [CTX_FILE] = {ci_file, ca_file, 5},
      [CTX_DIR] = {ci_dir, ca_dir, 4},
      [CTX_PLACE] = {ci_place, ca_place, 1},
      [CTX_DEVICE] = {ci_device, ca_device, 2},
      [CTX_DEVICE_OFF] = {ci_device_off, ca_device_off, 1},
  };
  if (kind == CTX_DEVICE && target >= 0 && target < nvols && !vols[target].mounted)
    kind = CTX_DEVICE_OFF;
  ctx.kind = kind;
  ctx.target = target;
  ctx.name[0] = 0;
  if (name)
    fcpy(ctx.name, name);
  ctx.acts = kinds[kind].acts;
  ctx.menu = (struct menu){.items = kinds[kind].items, .n = kinds[kind].n};
  int h = ctx.menu.n * GUI_ROW_H + 4;
  ctx.x = mx + GUI_MENU_W > W ? W - GUI_MENU_W : mx;
  ctx.y = my + h > H ? H - h : my;
  if (ctx.x < 0)
    ctx.x = 0;
  if (ctx.y < 0)
    ctx.y = 0;
  ctx.open = true;
}

static void ctx_draw(void)
{
  if (ctx.open)
    menu_popup_draw(&ctx.menu, ctx.x, ctx.y);
}

static void activate_entry(int e)
{
  struct fent *fe = &fent_buf[e];
  if (!fe->is_dir)
    open_file(g_id, fe->name);
  else if (feq(fe->name, ".."))
    go_up();
  else
    enter(fe->name);
  seq++;
}

static void new_folder(int id)
{
  char name[64], fp[256];
  if (dialog_prompt(id, "New Folder", "Folder name:", "", name, sizeof name) != DLG_R_OK || !name[0])
    return;
  const char *err = 0;
  for (const char *p = name; *p; p++)
    if (*p == '/')
      err = "The name cannot contain '/'.";
  struct statinfo st;
  full_path(name, fp, sizeof fp);
  if (!err && stat(fp, &st) == 0)
    err = "An entry with that name already exists.";
  if (!err) {
    int rc = mkdir(fp);
    if (rc == 0)
      force_relist = 1;
    else
      err = rc == -EACCES ? "Permission denied." : "The folder could not be created.";
  }
  if (err)
    dialog_message(id, "New folder failed", err);
  seq++;
}

/* Run the chosen context-menu action against the captured target */
static void ctx_run(int act, int id)
{
  switch (act) {
  case A_OPEN:
    if (ctx.kind == CTX_FILE || ctx.kind == CTX_DIR) {
      refresh();
      for (int e = 0; e < fent_count; e++)
        if (feq(fent_buf[e].name, ctx.name))
          activate_entry(e);
    } else if (ctx.kind == CTX_PLACE) {
      set_path(places[ctx.target].path, true);
    } else if (ctx.kind == CTX_DEVICE) {
      set_path(vols[ctx.target].point, true);
    }
    break;
  case A_EDIT:
    open_with(ctx.name, "editor");
    break;
  case A_RENAME:
    rename_entry(id, ctx.name);
    break;
  case A_DELETE:
    delete_entry(id, ctx.name, ctx.kind == CTX_DIR);
    break;
  case A_PROPS: {
    unsigned sz = 0;
    if (ctx.target >= 0 && ctx.target < fent_count && feq(fent_buf[ctx.target].name, ctx.name))
      sz = fent_buf[ctx.target].size;
    show_properties(id, ctx.name, ctx.kind == CTX_DIR, sz);
    break;
  }
  case A_CONNECT:
    if (ctx.target >= 0 && ctx.target < nvols) {
      char pt[40];
      strcpy(pt, "/media/");
      strcat(pt, vols[ctx.target].dev);
      mount(vols[ctx.target].dev, pt);
      set_title();
    }
    break;
  case A_EJECT:
    if (umount(vols[ctx.target].point) == 0 && under_path(path, vols[ctx.target].point))
      set_path("/", true);
    break;
  case A_NEWDIR:
    new_folder(id);
    break;
  case A_REFRESH:
    force_relist = 1;
    break;
  case A_UP:
    go_up();
    break;
  case A_HOME:
    set_path("/", true);
    break;
  }
}

/* Route a left-press while the popup is open: fire an item, or dismiss */
static void ctx_click(int id, int mx, int my)
{
  int i = menu_popup_click(&ctx.menu, mx, my, ctx.x, ctx.y);
  ctx.open = false;
  if (i >= 0)
    ctx_run(ctx.acts[i], id);
  seq++;
}

/* A right-press at: figure out what's under the cursor and pop up the matching context menu */
static void rclick(int id, int mx, int my)
{
  (void)id;
  int sbw = sidebar_w();
  refresh();
  scan_vols();

  if (my >= TOP_Y && mx < sbw) { /* sidebar */
    if (my >= LIST_TOP) {
      int i = (my - LIST_TOP) / ROW_H;
      if (i >= 0 && i < NPLACES) {
        ctx_open(CTX_PLACE, i, 0, mx, my);
        seq++;
        return;
      }
      for (int v = 0; v < nvols; v++) {
        int ry = dev_row_y(v);
        if (my >= ry && my < ry + ROW_H) {
          ctx_open(CTX_DEVICE, v, 0, mx, my);
          seq++;
          return;
        }
      }
    }
  }

  int e = list_hit(mx, my); /* file list */
  if (e >= 0 && !feq(fent_buf[e].name, "..")) {
    struct fent *fe = &fent_buf[e];
    sel = e; /* highlight the right-clicked row */
    ctx_open(fe->is_dir ? CTX_DIR : CTX_FILE, e, fe->name, mx, my);
  } else {
    ctx_open(CTX_BG, 0, 0, mx, my);
  }
  seq++;
}

/* Activate the current selection */
static void activate(void)
{
  refresh();
  if (sel >= 0 && sel < fent_count)
    activate_entry(sel);
}

/* Run a "Go" menu item (also reached via the F5/Ctrl+O accelerators) */
static void menu_action(int item)
{
  switch (item) {
  case 0:
    go_hist(-1);
    break;
  case 1:
    go_hist(1);
    break;
  case 2:
    go_up();
    break;
  case 3:
    set_path("/", true);
    break;
  case 4:
    force_relist = 1;
    break;
  case 5:
    activate();
    break;
  }
  seq++;
}

static void click(int id, int mx, int my)
{
  path_drag = 0;
  if (ctx.open) {
    ctx_click(id, mx, my);
    return;
  } /* popup eats the click */
  if (my < MENU_H || gomenu.open) { /* menu bar or open dropdown */
    int it = menu_click(&gomenu, mx, my, 2, 0);
    if (it >= 0)
      menu_action(it);
    seq++;
    return;
  }
  if (my < TOP_Y) { /* toolbar */
    int b = tb_btn_at(mx, my);
    if (b >= 0 && tb_btn_enabled(b)) {
      if (b == 2)
        go_up();
      else
        go_hist(b ? 1 : -1);
    }
    if (textfield_click(&pathtf, mx, my, path_x(), path_y(), path_w(), TEXTFIELD_H)) {
      path_focus = 1;
      path_drag = 1;
    } else if (path_focus) {
      path_focus = 0;
      textfield_set(&pathtf, path);
    }
    seq++;
    return;
  }
  if (path_focus) { /* a click elsewhere abandons an edit */
    path_focus = 0;
    textfield_set(&pathtf, path);
    seq++;
  }
  int sbw = sidebar_w();
  refresh();

  /* Divider grip: grab it to resize the sidebar */
  if (sidebar_press(&sb, 0, TOP_Y, sbw, H - TOP_Y - STATUS_H, mx, my)) {
    seq++;
    return;
  }

  if (mx < sbw) { /* sidebar shortcut */
    if (my < LIST_TOP)
      return;
    int i = (my - LIST_TOP) / ROW_H;
    if (i >= 0 && i < NPLACES) {
      set_path(places[i].path, true);
      return;
    }
    /* Devices region: an unmounted device connects (mounts) on any click */
    for (int v = 0; v < nvols; v++) {
      int ry = dev_row_y(v);
      if (my < ry || my >= ry + ROW_H)
        continue;
      if (!vols[v].mounted) { /* connect (mount) the device */
        char pt[40];
        strcpy(pt, "/media/");
        strcat(pt, vols[v].dev);
        mount(vols[v].dev, pt);
        set_title();
        seq++;
      } else if (mx >= sbw - EJECT_W) { /* eject button -> unmount */
        if (umount(vols[v].point) == 0) {
          if (under_path(path, vols[v].point))
            set_path("/", true);
          seq++;
        }
      } else { /* open the device */
        set_path(vols[v].point, true);
      }
      return;
    }
    return;
  }
  if (my < LIST_TOP) { /* column header: sort by it, again to reverse */
    int col = header_col(mx);
    if (col >= 0) {
      sort_desc = col == sort_col ? !sort_desc : 0;
      sort_col = col;
      resort();
    }
    return;
  }
  if (scrollview_press(&sv, mx, my)) {
    seq++;
    return;
  }
  int e = list_hit(mx, my);
  sel = e; /* single click selects/highlights; empty space clears */
  seq++;
}

/* Double-click: activate the entry, descend into a directory or open a file in its program */
static void dblclick(int id, int mx, int my)
{
  (void)id;
  refresh();
  int e = list_hit(mx, my);
  if (e >= 0)
    activate_entry(e);
}

/* Left-button motion: a path-field selection, a live sidebar resize or a scrollbar drag */
static void drag(int mx, int my)
{
  if (path_drag)
    textfield_drag(&pathtf, mx, path_x(), path_w());
  else if (sidebar_drag(&sb, 0, mx) | scrollview_drag(&sv, mx, my))
    seq++;
}

/* Pointer motion with no button: hover feedback for the toolbar buttons and list rows */
static void motion(int mx, int my)
{
  int hb = mx < 0 ? -1 : tb_btn_at(mx, my);
  int hr = mx < 0 ? -1 : list_hit(mx, my);
  if (hb != hot_btn || hr != hover) {
    hot_btn = hb;
    hover = hr;
    seq++;
  }
}

/* The path field is focused: Enter goes there, Esc abandons the edit */
static void path_key(int k, int mods)
{
  struct statinfo st;
  if (k == '\n') {
    if (pathbuf[0] == '/' && stat(pathbuf, &st) == 0 && st.is_dir) {
      set_path(pathbuf, true);
      return;
    }
    snprintf(status, sizeof status, "Not a folder: %s", pathbuf);
    path_focus = 0;
  } else if (k == 27) {
    path_focus = 0;
    textfield_set(&pathtf, path);
  } else if (!textfield_key(&pathtf, (char)k, mods)) {
    return;
  }
  seq++;
}

/* Keyboard navigation: arrow up/down move the highlighted entry, keeping it in view by scrolling */
static void key(int id, int k, int mods)
{
  if (path_focus && !gomenu.open) {
    path_key(k, mods);
    return;
  }
  /* Menu accelerators (F5 refresh, Ctrl+O open): fire the item like a click */
  int sc = menu_shortcut(&gomenu, k);
  if (sc >= 0) {
    gomenu.open = 0;
    menu_action(sc);
    return;
  }
  if (k == 27) {
    gomenu.open = 0;
    ctx.open = false;
    status[0] = 0;
    seq++;
    return;
  }
  if (mods & KMOD_CTRL && (k == 'l' || k == 'L')) {
    path_focus = 1;
    textfield_select_all(&pathtf);
    seq++;
    return;
  }
  if (mods & KMOD_ALT && (k == KEY_LEFT || k == KEY_RIGHT)) {
    go_hist(k == KEY_LEFT ? -1 : 1);
    return;
  }

  refresh();
  if (k == KEY_UP || k == KEY_DOWN || k == KEY_HOME || k == KEY_END) {
    if (fent_count == 0)
      return;
    if (k == KEY_HOME)
      sel = 0;
    else if (k == KEY_END)
      sel = fent_count - 1;
    else if (sel < 0)
      sel = (k == KEY_DOWN) ? 0 : fent_count - 1;
    else {
      sel += (k == KEY_DOWN) ? 1 : -1;
      if (sel < 0)
        sel = 0;
      if (sel >= fent_count)
        sel = fent_count - 1;
    }
    scrollview_reveal(&sv, sel * ROW_H, ROW_H);
    seq++;
  } else if (k == '\n') {
    activate();
  } else if (k == '\b') {
    go_up();
  } else if (k == KEY_F2) {
    if (sel >= 0 && sel < fent_count)
      rename_entry(id, fent_buf[sel].name);
  } else if (k == KEY_DELETE) {
    if (sel >= 0 && sel < fent_count) {
      struct fent *fe = &fent_buf[sel];
      delete_entry(id, fe->name, fe->is_dir);
    }
  }
}

int main(void)
{
  int id = g_id = win_create(W, H, "Files");
  if (id < 0) {
    puts("files: no window\n");
    return 1;
  }
  gfx_init(win_fb(), W, H);
  sidebar_init(&sb, SIDEBAR_W, SIDEBAR_MIN, SIDEBAR_MAX);
  scrollview_init(&sv, ROW_H);
  textfield_init(&pathtf, pathbuf, sizeof pathbuf);
  set_path("/", true);

  int last = -1;
  for (;;) {
    struct win_event ev;
    while (win_poll(id, &ev)) {
      if (ev.type == WEV_CLOSE) {
        win_close(id);
        return 0;
      } else if (ev.type == WEV_MOUSE)
        click(id, ev.a, ev.b);
      else if (ev.type == WEV_DRAG)
        drag(ev.a, ev.b);
      else if (ev.type == WEV_MOVE)
        motion(ev.a, ev.b);
      else if (ev.type == WEV_SCROLL) {
        if (scrollview_wheel(&sv, ev.a))
          seq++;
      } else if (ev.type == WEV_RCLICK)
        rclick(id, ev.a, ev.b);
      else if (ev.type == WEV_DBLCLICK)
        dblclick(id, ev.a, ev.b);
      else if (ev.type == WEV_KEY)
        key(id, ev.a, ev.b);
      else if (ev.type == WEV_RESIZE) {
        W = ev.a;
        H = ev.b;
        gfx_init(win_fb(), W, H);
        seq++; /* force a repaint at the new size */
      }
    }
    if ((int)seq != last) {
      render();
      win_present(id);
      last = seq;
    }
    props_pump(); /* service the modeless Properties window, if open */
    win_wait(id);
  }
}
