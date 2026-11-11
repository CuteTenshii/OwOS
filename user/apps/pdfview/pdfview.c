/* pdfview.c, a windowed PDF viewer: one page at a time, zoom, scroll, drag to pan */
#include "canvas2d.h"
#include "clipboard.h"
#include "dialog.h"
#include "font.h"
#include "fontstore.h"
#include "gfx.h"
#include "owos.h"
#include "pdf.h"
#include <stdbool.h>

#define MENU_H GUI_BAR_H
#define STATUS_H GUI_BAR_H
#define MARGIN 12
#define SURF_MAX_W 2400
#define SURF_MAX_PX (3 * 1024 * 1024)
#define COL_BG 0x2b303b
#define COL_BAR 0x232a36
#define COL_TEXT 0x9fb0c4

static int W = 600, H = 440;
static int wid = -1;

static unsigned char *file_data;
static struct pdf *doc;
static char curpath[256];
static int npages, page;
static const char *load_error;

static const int zoom_steps[] = {10, 17, 25, 33, 50, 67, 75, 100, 125, 150, 200, 300, 400, 600, 800};
#define NSTEPS ((int)(sizeof zoom_steps / sizeof *zoom_steps))
enum { FIT_WIDTH, FIT_PAGE, FIT_NONE };
static int fit = FIT_WIDTH;
static float scale = 1.0f;
static float page_w, page_h;
static int sx, sy;

static struct c2d *surf;
static int surf_page = -1, surf_ox, surf_oy;
static float surf_scale;

static const struct font *sans, *mono;
static unsigned char *sans_data, *mono_data;

static int prompt_on;
static char prompt_buf[48];
static int prompt_len, prompt_kind; /* 0: go to page, 1: find */
static struct pdf_glyph *gl;
static int ngl, gl_page = -1;
static int sel_a = -1, sel_b = -1, selecting;
static char query[48];
static int qlen;
static const char *note;
static struct status_bar sbar;
static int drag_x, drag_y;

static int view_h(void) { return H - MENU_H - STATUS_H; }
static int content_w(void) { return (int)(page_w * scale + 0.999f); }
static int content_h(void) { return (int)(page_h * scale + 0.999f); }

static void set_status(const char *s) { status_set(&sbar, s); }

static unsigned char *read_whole(const char *path, int *len)
{
  struct statinfo st;
  if (stat(path, &st) != 0 || st.size == 0 || st.is_dir)
    return 0;
  unsigned char *buf = malloc(st.size);
  if (!buf)
    return 0;
  unsigned off = 0;
  while (off < st.size) {
    int n = readfile_at(path, off, buf + off, st.size - off);
    if (n <= 0)
      break;
    off += (unsigned)n;
  }
  if (off != st.size) {
    free(buf);
    return 0;
  }
  *len = (int)st.size;
  return buf;
}

static const struct font *load_font(int role, unsigned char **data)
{
  char path[96];
  int len = 0;
  fontstore_path(role, path, sizeof path);
  *data = read_whole(path, &len);
  return *data ? font_load(*data, len) : 0;
}

static const char *basename_of(const char *p)
{
  const char *b = p;
  for (; *p; p++)
    if (*p == '/')
      b = p + 1;
  return b;
}

static void clamp_scroll(void)
{
  int mx = content_w() - W, my = content_h() - view_h();
  if (sx > mx)
    sx = mx;
  if (sy > my)
    sy = my;
  if (sx < 0)
    sx = 0;
  if (sy < 0)
    sy = 0;
}

static void apply_fit(void)
{
  if (!doc)
    return;
  float fw = (W - 2 * MARGIN) / page_w, fh = (view_h() - 2 * MARGIN) / page_h;
  if (fit == FIT_WIDTH)
    scale = fw;
  else if (fit == FIT_PAGE)
    scale = fw < fh ? fw : fh;
  if (scale < 0.1f)
    scale = 0.1f;
  if (scale > 12.0f)
    scale = 12.0f;
  clamp_scroll();
}

static int zoom_percent(void) { return (int)(scale * 75.0f + 0.5f); }

static void close_doc(void)
{
  pdf_close(doc);
  free(file_data);
  doc = 0;
  file_data = 0;
  npages = 0;
  surf_page = -1;
  free(gl);
  gl = 0;
  ngl = 0;
  gl_page = -1;
  sel_a = sel_b = -1;
}

static void show_page(int n)
{
  if (!doc)
    return;
  if (n < 0)
    n = 0;
  if (n >= npages)
    n = npages - 1;
  page = n;
  sel_a = sel_b = -1;
  pdf_page_size(doc, page, &page_w, &page_h);
  if (page_w < 1 || page_h < 1)
    page_w = page_h = 1;
  apply_fit();
  sx = sy = 0;
}

static void load(const char *path)
{
  int len = 0;
  unsigned char *data = read_whole(path, &len);
  struct pdf *d = 0;
  int rc = data ? pdf_open(data, len, &d) : PDF_EFORMAT;
  if (rc != PDF_OK) {
    free(data);
    load_error = !data                ? "could not read the file"
                 : rc == PDF_EENCRYPT ? "password-protected PDFs are not supported"
                 : rc == PDF_ENOMEM   ? "out of memory"
                                      : "not a readable PDF";
    snprintf(curpath, sizeof curpath, "%s", path);
    set_status(load_error);
    return;
  }
  close_doc();
  load_error = 0;
  file_data = data;
  doc = d;
  pdf_set_fonts(doc, sans, mono);
  npages = pdf_page_count(doc);
  snprintf(curpath, sizeof curpath, "%s", path);
  fit = FIT_WIDTH;
  show_page(0);
  char title[160];
  snprintf(title, sizeof title, "%s - PDF Viewer", basename_of(curpath));
  win_settitle(wid, title);
  set_status("opened");
}

/* Render the part of the page the view needs into surf, unless the cached surface already covers it. */
static void ensure_surface(void)
{
  if (!doc)
    return;
  int cw = content_w(), ch = content_h();
  int vis_w = cw < W ? cw : W, vis_h = ch < view_h() ? ch : view_h();
  if (surf && surf_page == page && surf_scale == scale && sx >= surf_ox && sy >= surf_oy &&
      sx + vis_w <= surf_ox + surf->w && sy + vis_h <= surf_oy + surf->h)
    return;
  int w = cw < SURF_MAX_W ? cw : SURF_MAX_W;
  if (w < vis_w)
    w = vis_w;
  int h = SURF_MAX_PX / w;
  if (h > ch)
    h = ch;
  if (h < vis_h)
    h = vis_h;
  int ox = sx - (w - vis_w) / 2, oy = sy - (h - vis_h) / 2;
  if (ox > cw - w)
    ox = cw - w;
  if (oy > ch - h)
    oy = ch - h;
  if (ox < 0)
    ox = 0;
  if (oy < 0)
    oy = 0;
  if (!surf)
    surf = c2d_new(w, h);
  else if (surf->w != w || surf->h != h)
    c2d_resize(surf, w, h);
  if (!surf || surf->w != w) {
    surf_page = -1;
    return;
  }
  win_cursor(wid, WIN_CURSOR_BUSY);
  pdf_render_page(doc, page, surf, scale, ox, oy);
  win_cursor(wid, WIN_CURSOR_ARROW);
  surf_page = page;
  surf_scale = scale;
  surf_ox = ox;
  surf_oy = oy;
}

static void ensure_text(int p)
{
  if (!doc || gl_page == p)
    return;
  free(gl);
  gl = 0;
  ngl = 0;
  if (pdf_page_text(doc, p, &gl, &ngl) != 0) {
    gl = 0;
    ngl = 0;
  }
  gl_page = p;
}

static unsigned fold(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c == '\n' ? ' ' : c; }

/* End index (exclusive) of a match of the query starting at glyph i, or -1 */
static int match_at(int i)
{
  int k = 0, j = i;
  while (k < qlen && j < ngl) {
    if (fold(gl[j].ch) != fold((unsigned char)query[k]))
      return -1;
    k++;
    j++;
  }
  return k == qlen ? j : -1;
}

/* Where the page sits in the view: top-left of the page and the scroll offset into it */
static void page_origin(int *px0, int *py0, int *ox, int *oy)
{
  int cw = content_w(), ch = content_h();
  *px0 = cw < W ? (W - cw) / 2 : 0;
  *py0 = ch < view_h() ? (view_h() - ch) / 2 : 0;
  *ox = cw < W ? 0 : sx;
  *oy = ch < view_h() ? 0 : sy;
}

/* Glyph nearest the view point (mx,my); *exact says whether the point is inside its box */
static int glyph_at(int mx, int my, int *exact)
{
  int px0, py0, ox, oy, best = -1;
  float bd = 1e30f;
  page_origin(&px0, &py0, &ox, &oy);
  float gx = (mx - px0 + ox) / scale, gy = (my - MENU_H - py0 + oy) / scale;
  *exact = 0;
  ensure_text(page);
  for (int i = 0; i < ngl; i++) {
    if (gl[i].ch == '\n')
      continue;
    float dx = gx < gl[i].x0 ? gl[i].x0 - gx : gx > gl[i].x1 ? gx - gl[i].x1 : 0;
    float dy = gy < gl[i].y0 ? gl[i].y0 - gy : gy > gl[i].y1 ? gy - gl[i].y1 : 0;
    if (dx * dx + dy * dy < bd) {
      bd = dx * dx + dy * dy;
      best = i;
      *exact = bd == 0 && gl[i].ch != ' ';
    }
  }
  return best;
}

static void tint(int x0, int y0, int x1, int y1, unsigned rgb, unsigned a)
{
  unsigned *fb = win_fb();
  int px0, py0, ox, oy;
  page_origin(&px0, &py0, &ox, &oy);
  int lx = px0, ly = MENU_H + py0, hx = px0 + (content_w() < W ? content_w() : W),
      hy = ly + (content_h() < view_h() ? content_h() : view_h());
  x0 = x0 < lx ? lx : x0;
  y0 = y0 < ly ? ly : y0;
  x1 = x1 > hx ? hx : x1;
  y1 = y1 > hy ? hy : y1;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) {
      unsigned d = fb[(unsigned long)y * W + x], o = 0;
      for (int sh = 0; sh < 24; sh += 8)
        o |= ((((d >> sh) & 255) * (255 - a) + ((rgb >> sh) & 255) * a) / 255) << sh;
      fb[(unsigned long)y * W + x] = o;
    }
}

static void tint_glyph(int i, unsigned rgb, unsigned a)
{
  int px0, py0, ox, oy;
  page_origin(&px0, &py0, &ox, &oy);
  if (gl[i].ch == '\n')
    return;
  tint((int)(px0 + gl[i].x0 * scale - ox), (int)(MENU_H + py0 + gl[i].y0 * scale - oy),
       (int)(px0 + gl[i].x1 * scale - ox + 0.999f), (int)(MENU_H + py0 + gl[i].y1 * scale - oy + 0.999f), rgb, a);
}

static void draw_marks(void)
{
  ensure_text(page);
  int lo = sel_a < sel_b ? sel_a : sel_b, hi = sel_a < sel_b ? sel_b : sel_a;
  for (int i = 0; qlen && i < ngl; i++) {
    int e = match_at(i);
    if (e < 0)
      continue;
    for (int j = i; j < e; j++)
      tint_glyph(j, 0xffd000, 110);
    i = e - 1;
  }
  for (int i = lo; lo >= 0 && i <= hi && i < ngl; i++)
    tint_glyph(i, 0x3070e0, 110);
}

static void draw_page(void)
{
  unsigned *fb = win_fb();
  int vw = W, vh = view_h();
  gfx_fill_rect(0, MENU_H, vw, vh, COL_BG);
  if (!doc) {
    const char *m = load_error ? load_error : "Open a PDF:  File > Open";
    gfx_ui_text((W - gfx_ui_width(m)) / 2, MENU_H + vh / 2 - UIFONT_H / 2, m, COL_TEXT);
    return;
  }
  ensure_surface();
  if (!surf || surf_page != page)
    return;
  int cw = content_w(), ch = content_h();
  int px0 = cw < vw ? (vw - cw) / 2 : 0, py0 = ch < vh ? (vh - ch) / 2 : 0;
  int ox = cw < vw ? 0 : sx, oy = ch < vh ? 0 : sy;
  int dw = cw < vw ? cw : vw, dh = ch < vh ? ch : vh;
  gfx_rect(px0 - 1, MENU_H + py0 - 1, dw + 2, dh + 2, 0x151a22);
  for (int y = 0; y < dh; y++) {
    const unsigned char *src = surf->px + ((unsigned long)(oy + y - surf_oy) * surf->w + (ox - surf_ox)) * 4;
    unsigned *dst = fb + (unsigned long)(MENU_H + py0 + y) * W + px0;
    for (int x = 0; x < dw; x++, src += 4)
      dst[x] = (unsigned)src[0] << 16 | (unsigned)src[1] << 8 | src[2];
  }
  draw_marks();
  if (ch > vh) {
    int th = vh * vh / ch;
    if (th < 12)
      th = 12;
    int ty = MENU_H + (vh - th) * sy / (ch - vh);
    gfx_fill_rect(W - 6, ty, 4, th, 0x6b7a94);
  }
  if (cw > vw) {
    int tw = vw * vw / cw;
    if (tw < 12)
      tw = 12;
    int tx = (vw - tw) * sx / (cw - vw);
    gfx_fill_rect(tx, MENU_H + vh - 6, tw, 4, 0x6b7a94);
  }
}

static void scroll_by(int dy)
{
  if (!doc)
    return;
  int old = sy;
  sy += dy;
  clamp_scroll();
  if (sy == old && dy > 0 && page + 1 < npages) {
    show_page(page + 1);
  } else if (sy == old && dy < 0 && page > 0) {
    show_page(page - 1);
    sy = content_h();
    clamp_scroll();
  }
}

static void zoom_to(float s)
{
  if (!doc)
    return;
  float fx = (sx + W / 2.0f) / content_w(), fy = (sy + view_h() / 2.0f) / content_h();
  fit = FIT_NONE;
  scale = s < 0.1f ? 0.1f : s > 12.0f ? 12.0f : s;
  sx = (int)(fx * content_w() - W / 2.0f);
  sy = (int)(fy * content_h() - view_h() / 2.0f);
  clamp_scroll();
}

static void zoom_step(int dir)
{
  int pct = zoom_percent();
  int i = dir > 0 ? 0 : NSTEPS - 1;
  if (dir > 0) {
    while (i < NSTEPS - 1 && zoom_steps[i] <= pct)
      i++;
  } else {
    while (i > 0 && zoom_steps[i] >= pct)
      i--;
  }
  zoom_to(zoom_steps[i] / 75.0f);
}

static void reveal(int i)
{
  float cx = (gl[i].x0 + gl[i].x1) / 2 * scale, cy = (gl[i].y0 + gl[i].y1) / 2 * scale;
  sx = (int)(cx - W / 2.0f);
  sy = (int)(cy - view_h() / 2.0f);
  clamp_scroll();
}

/* Select the next (dir 1) or previous (dir -1) match of the query, across pages, wrapping once around */
static void find_step(int dir)
{
  note = 0;
  if (!doc || !qlen)
    return;
  ensure_text(page);
  int lo = sel_a < sel_b ? sel_a : sel_b, hi = sel_a < sel_b ? sel_b : sel_a;
  int p = page, i = lo < 0 ? (dir > 0 ? 0 : ngl - 1) : (dir > 0 ? hi + 1 : lo - 1);
  for (int tries = 0; tries <= npages; tries++) {
    ensure_text(p);
    for (; i >= 0 && i < ngl; i += dir) {
      int e = match_at(i);
      if (e < 0)
        continue;
      if (p != page)
        show_page(p);
      sel_a = i;
      sel_b = e - 1;
      reveal(i);
      return;
    }
    p = (p + dir + npages) % npages;
    ensure_text(p);
    i = dir > 0 ? 0 : ngl - 1;
  }
  note = "not found";
}

static void copy_selection(void)
{
  int lo = sel_a < sel_b ? sel_a : sel_b, hi = sel_a < sel_b ? sel_b : sel_a;
  if (lo < 0 || lo >= ngl) {
    note = "nothing selected";
    return;
  }
  char *buf = malloc((unsigned)(hi - lo + 1) * 4 + 1);
  int n = 0;
  for (int i = lo; buf && i <= hi && i < ngl; i++)
    n += utf8_encode(gl[i].ch, buf + n);
  if (buf) {
    buf[n] = 0;
    note = clip_put_text(buf, n) == 0 ? "copied" : "clipboard unavailable";
  }
  free(buf);
}

static void start_prompt(int kind)
{
  prompt_on = 1;
  prompt_kind = kind;
  prompt_len = 0;
  prompt_buf[0] = 0;
}

static void open_dialog(void)
{
  static const char *const exts[] = {"pdf", NULL};
  char path[256];
  struct filepicker_opts o = {
      .title = "Open PDF", .mode = FILEPICK_OPEN, .start = "/", .filter_label = "PDF files", .exts = exts};
  if (filepicker(wid, &o, path, sizeof path))
    load(path);
  gfx_init(win_fb(), W, H);
}

static const char *const file_items[] = {"Open...", "Quit"};
static const char *const file_hints[] = {"Ctrl+O", ""};
static const unsigned char file_keys[] = {KEY_OPEN, 0};
static struct menu filemenu = {"File", file_items, 2, 0, file_hints, file_keys};

static const char *const view_items[] = {"Zoom In", "Zoom Out", "Actual Size", "Fit Width", "Fit Page"};
static const char *const view_hints[] = {"+", "-", "1", "W", "F"};
static const unsigned char view_keys[] = {'+', '-', '1', 'w', 'f'};
static struct menu viewmenu = {"View", view_items, 5, 0, view_hints, view_keys};

static const char *const go_items[] = {"Previous Page", "Next Page", "First Page", "Last Page", "Go to Page..."};
static const char *const go_hints[] = {"Left", "Right", "Home", "End", "G"};
static const unsigned char go_keys[] = {KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, 'g'};
static struct menu gomenu = {"Go", go_items, 5, 0, go_hints, go_keys};

static const char *const edit_items[] = {"Find...", "Find Next", "Find Previous", "Copy"};
static const char *const edit_hints[] = {"/", "N", "Shift+N", "Ctrl+C"};
static const unsigned char edit_keys[] = {'/', 'n', 'N', KEY_INTR};
static struct menu editmenu = {"Edit", edit_items, 4, 0, edit_hints, edit_keys};

static struct menu *const bar_menus[] = {&filemenu, &editmenu, &viewmenu, &gomenu};
static struct menubar mbar = {bar_menus, 4, 2, 0};

static int menu_is_open(void) { return filemenu.open || editmenu.open || viewmenu.open || gomenu.open; }

static void menu_action(struct menu *leaf, int item)
{
  if (leaf == &filemenu) {
    if (item == 0)
      open_dialog();
    else {
      win_close(wid);
      exit(0);
    }
  } else if (leaf == &viewmenu) {
    if (item == 0)
      zoom_step(1);
    else if (item == 1)
      zoom_step(-1);
    else if (item == 2)
      zoom_to(100.0f / 75.0f);
    else {
      fit = item == 3 ? FIT_WIDTH : FIT_PAGE;
      apply_fit();
    }
  } else if (leaf == &gomenu) {
    if (item == 0)
      show_page(page - 1);
    else if (item == 1)
      show_page(page + 1);
    else if (item == 2)
      show_page(0);
    else if (item == 3)
      show_page(npages - 1);
    else if (doc)
      start_prompt(0);
  } else if (leaf == &editmenu && doc) {
    if (item == 0)
      start_prompt(1);
    else if (item == 3)
      copy_selection();
    else
      find_step(item == 1 ? 1 : -1);
  }
}

static void on_key(unsigned char c)
{
  note = 0;
  if (prompt_on) {
    if (c == '\n') {
      int n = 0;
      prompt_on = 0;
      if (prompt_kind == 1 && prompt_len) {
        strcpy(query, prompt_buf);
        qlen = prompt_len;
        sel_a = sel_b = -1;
        find_step(1);
      } else if (prompt_kind == 0 && prompt_len) {
        for (int i = 0; i < prompt_len; i++)
          n = n * 10 + prompt_buf[i] - '0';
        show_page(n - 1);
      }
    } else if (c == 27)
      prompt_on = 0;
    else if (c == '\b') {
      if (prompt_len > 0)
        prompt_buf[--prompt_len] = 0;
    } else if (((prompt_kind == 0 && c >= '0' && c <= '9') || (prompt_kind == 1 && c >= ' ' && c < 127)) &&
               prompt_len < (int)sizeof prompt_buf - 1) {
      prompt_buf[prompt_len++] = (char)c;
      prompt_buf[prompt_len] = 0;
    }
    return;
  }
  struct menu *leaf;
  int item = menubar_shortcut(&mbar, c, &leaf);
  if (item >= 0) {
    menubar_close(&mbar);
    menu_action(leaf, item);
    return;
  }
  switch (c) {
  case 27:
    menubar_close(&mbar);
    sel_a = sel_b = -1;
    qlen = 0;
    break;
  case KEY_UP:
    scroll_by(-48);
    break;
  case KEY_DOWN:
    scroll_by(48);
    break;
  case ' ':
    scroll_by(view_h() - 32);
    break;
  case '\b':
    scroll_by(-(view_h() - 32));
    break;
  case '=':
    zoom_step(1);
    break;
  case 'q':
    win_close(wid);
    exit(0);
  }
}

static void on_click(int mx, int my)
{
  note = 0;
  if (my < MENU_H || menu_is_open()) {
    struct menu *leaf;
    int item;
    if (menubar_click(&mbar, mx, my, &leaf, &item) && item >= 0)
      menu_action(leaf, item);
    return;
  }
  drag_x = mx;
  drag_y = my;
  selecting = 0;
  if (!doc || my >= H - STATUS_H)
    return;
  int exact, i = glyph_at(mx, my, &exact);
  sel_a = sel_b = -1;
  if (exact) {
    sel_a = sel_b = i;
    selecting = 1;
  }
}

static void on_drag(int mx, int my)
{
  if (!doc || menu_is_open())
    return;
  if (selecting) {
    int exact, i = glyph_at(mx, my, &exact);
    if (i >= 0)
      sel_b = i;
    return;
  }
  sx -= mx - drag_x;
  sy -= my - drag_y;
  drag_x = mx;
  drag_y = my;
  clamp_scroll();
}

static void render(void)
{
  draw_page();
  gfx_fill_rect(0, 0, W, MENU_H, COL_BAR);
  char line[160];
  if (prompt_on && prompt_kind == 1)
    snprintf(line, sizeof line, "Find: %s_", prompt_buf);
  else if (prompt_on)
    snprintf(line, sizeof line, "Go to page (1-%d): %s_", npages, prompt_buf);
  else if (note)
    snprintf(line, sizeof line, "%s", note);
  else if (doc)
    snprintf(line, sizeof line, "%s   page %d / %d   %d%%", basename_of(curpath), page + 1, npages, zoom_percent());
  else
    snprintf(line, sizeof line, "%s", load_error ? load_error : "[no document]");
  status_set(&sbar, line);
  status_draw(&sbar, 0, H - STATUS_H, W, prompt_on ? 0xffe08a : COL_TEXT, 0x161d28);
  menubar_draw(&mbar);
}

int main(int argc, char **argv)
{
  wid = win_create(W, H, "PDF Viewer");
  if (wid < 0) {
    puts("pdfview: could not open a window\n");
    return 1;
  }
  gfx_init(win_fb(), W, H);
  sans = load_font(FONT_SANS, &sans_data);
  mono = load_font(FONT_MONO, &mono_data);
  if (argc > 1 && argv[1] && argv[1][0])
    load(argv[1]);
  else
    set_status("no document");
  bool need = true;
  for (;;) {
    struct win_event ev;
    while (win_poll(wid, &ev)) {
      need = true;
      if (ev.type == WEV_CLOSE) {
        win_close(wid);
        return 0;
      } else if (ev.type == WEV_KEY)
        on_key((unsigned char)ev.a);
      else if (ev.type == WEV_MOUSE)
        on_click(ev.a, ev.b);
      else if (ev.type == WEV_DRAG)
        on_drag(ev.a, ev.b);
      else if (ev.type == WEV_SCROLL)
        scroll_by(-ev.a * 48);
      else if (ev.type == WEV_RESIZE) {
        W = ev.a;
        H = ev.b;
        gfx_init(win_fb(), W, H);
        apply_fit();
      }
    }
    if (need) {
      render();
      win_present(wid);
      need = false;
    }
    win_wait(wid);
  }
}
