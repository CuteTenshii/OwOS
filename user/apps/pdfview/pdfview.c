/* pdfview.c, a windowed PDF viewer: one page at a time, zoom, scroll, drag to pan */
#include "canvas2d.h"
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
static char prompt_buf[16];
static int prompt_len;
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

static struct menu *const bar_menus[] = {&filemenu, &viewmenu, &gomenu};
static struct menubar mbar = {bar_menus, 3, 2, 0};

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
    else if (doc) {
      prompt_on = 1;
      prompt_len = 0;
      prompt_buf[0] = 0;
    }
  }
}

static void on_key(unsigned char c)
{
  if (prompt_on) {
    if (c == '\n') {
      int n = 0;
      for (int i = 0; i < prompt_len; i++)
        n = n * 10 + prompt_buf[i] - '0';
      prompt_on = 0;
      if (prompt_len)
        show_page(n - 1);
    } else if (c == 27)
      prompt_on = 0;
    else if (c == '\b') {
      if (prompt_len > 0)
        prompt_buf[--prompt_len] = 0;
    } else if (c >= '0' && c <= '9' && prompt_len < (int)sizeof prompt_buf - 1) {
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
  if (my < MENU_H || filemenu.open || viewmenu.open || gomenu.open) {
    struct menu *leaf;
    int item;
    if (menubar_click(&mbar, mx, my, &leaf, &item) && item >= 0)
      menu_action(leaf, item);
    return;
  }
  drag_x = mx;
  drag_y = my;
}

static void on_drag(int mx, int my)
{
  if (!doc || filemenu.open || viewmenu.open || gomenu.open)
    return;
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
  if (prompt_on)
    snprintf(line, sizeof line, "Go to page (1-%d): %s_", npages, prompt_buf);
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
