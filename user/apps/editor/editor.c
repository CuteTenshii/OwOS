/* editor.c, a windowed plain-text editor, running entirely in user space */
#include "clipboard.h"
#include "dialog.h"
#include "gfx.h"
#include "mime.h"
#include "owos.h"
#include <stdbool.h>

static int W = 560, H = 360; /* client size; grows/shrinks on resize */
#define MAXLINES 200
#define MAXCOLS 120
#define MENU_H 18
#define STATUS_H 18
#define ROW_H 16
#define COL_W 8
#define FNAME_MAX 256

static char lines[MAXLINES][MAXCOLS];
static int linelen[MAXLINES];
static int nlines = 1;
static int curx, cury, top_line;
static char fname[FNAME_MAX];
static bool has_name, modified;

static char io_buf[MAXLINES * (MAXCOLS + 1)];
static char clip_buf[8192]; /* scratch for Cut/Copy (text staged for clip_put) */

/* Selection: an anchor (sel_x,sel_y) plus the moving caret (curx,cury) */
static int sel_active;
static int sel_x, sel_y;

/* GUI widgets (shared gfx lib): a File + Edit menu bar and the status message */

/* File menu */
static const char *const file_items[] = {"New", "Open", "Save", "Save As"};
static const char *const file_hints[] = {"Ctrl+N", "Ctrl+O", "Ctrl+S", ""};
static const unsigned char file_keys[] = {KEY_NEW, KEY_OPEN, KEY_SAVE, 0};
static struct menu filemenu = {"File", file_items, 4, 0, file_hints, file_keys};

/* Edit menu */
static const char *const edit_items[] = {"Select All", "Cut", "Copy", "Paste", "Delete"};
static const char *const edit_hints[] = {"Ctrl+A", "Ctrl+X", "Ctrl+C", "Ctrl+V", "Del"};
static const unsigned char edit_keys[] = {KEY_SELALL, KEY_CUT, KEY_INTR, 0, 0};
static unsigned char edit_flags[5];
static struct menu editmenu = {"Edit", edit_items, 5, 0, edit_hints, edit_keys, edit_flags};

static struct menu *const bar_menus[] = {&filemenu, &editmenu};
static struct menubar mbar = {bar_menus, 2, 2, 0};
static struct status_bar sbar;

/* tiny helpers */

static void scopy(char *d, const char *s, int max)
{
  int i = 0;
  for (; s[i] && i < max - 1; i++)
    d[i] = s[i];
  d[i] = '\0';
}

static void set_status(const char *s) { status_set(&sbar, s); }

static bool has_high; /* true if the buffer holds any byte >= 0x80 (-> UTF-8) */

static void ed_clear(void)
{
  nlines = 1;
  linelen[0] = 0;
  lines[0][0] = 0;
  curx = cury = top_line = 0;
  modified = false;
  has_high = false;
  sel_active = 0;
}

/* selection */

/* Order the anchor and caret into top-left (*x0,*y0)  */
static void sel_bounds(int *x0, int *y0, int *x1, int *y1)
{
  if (sel_y < cury || (sel_y == cury && sel_x <= curx)) {
    *x0 = sel_x;
    *y0 = sel_y;
    *x1 = curx;
    *y1 = cury;
  } else {
    *x0 = curx;
    *y0 = cury;
    *x1 = sel_x;
    *y1 = sel_y;
  }
}

/* Begin a selection at the caret if none is active (called before a Shift+move grows it) */
static void sel_begin(void)
{
  if (!sel_active) {
    sel_active = 1;
    sel_x = curx;
    sel_y = cury;
  }
}
static void sel_clear(void) { sel_active = 0; }

/* Copy the selected text into out (with '\n' between lines); returns its length */
static int sel_extract(char *out, int max)
{
  int x0, y0, x1, y1, p = 0;
  sel_bounds(&x0, &y0, &x1, &y1);
  for (int y = y0; y <= y1; y++) {
    int from = (y == y0) ? x0 : 0;
    int to = (y == y1) ? x1 : linelen[y];
    for (int x = from; x < to && p < max; x++)
      out[p++] = lines[y][x];
    if (y < y1 && p < max)
      out[p++] = '\n';
  }
  return p;
}

/* Remove the selected span, leaving the caret where it began */
static void sel_delete(void)
{
  int x0, y0, x1, y1;
  if (!sel_active)
    return;
  sel_bounds(&x0, &y0, &x1, &y1);

  /* Splice the kept tail of the last line onto the kept head of the first */
  int tail = linelen[y1] - x1;
  for (int j = 0; j < tail; j++)
    lines[y0][x0 + j] = lines[y1][x1 + j];
  linelen[y0] = x0 + tail;

  int removed = y1 - y0;
  if (removed > 0) {
    for (int i = y0 + 1; i + removed < nlines; i++) {
      linelen[i] = linelen[i + removed];
      for (int j = 0; j < linelen[i]; j++)
        lines[i][j] = lines[i + removed][j];
    }
    nlines -= removed;
  }
  curx = x0;
  cury = y0;
  sel_active = 0;
  modified = true;
}

/* buffer edits */

static void ed_insert(char c)
{
  if (sel_active)
    sel_delete(); /* typing over a selection replaces it */
  if (linelen[cury] >= MAXCOLS - 1)
    return;
  for (int j = linelen[cury]; j > curx; j--)
    lines[cury][j] = lines[cury][j - 1];
  lines[cury][curx] = c;
  linelen[cury]++;
  curx++;
  modified = true;
  if ((unsigned char)c >= 0x80)
    has_high = true;
}

static void ed_newline(void)
{
  if (sel_active)
    sel_delete();
  if (nlines >= MAXLINES)
    return;
  for (int i = nlines; i > cury + 1; i--) {
    linelen[i] = linelen[i - 1];
    for (int j = 0; j < linelen[i]; j++)
      lines[i][j] = lines[i - 1][j];
  }
  int tail = linelen[cury] - curx;
  for (int j = 0; j < tail; j++)
    lines[cury + 1][j] = lines[cury][curx + j];
  linelen[cury + 1] = tail;
  linelen[cury] = curx;
  nlines++;
  cury++;
  curx = 0;
  modified = true;
}

static void ed_backspace(void)
{
  if (sel_active) {
    sel_delete();
    return;
  } /* Backspace deletes a selection */
  if (curx > 0) {
    for (int j = curx - 1; j < linelen[cury] - 1; j++)
      lines[cury][j] = lines[cury][j + 1];
    linelen[cury]--;
    curx--;
    modified = true;
  } else if (cury > 0) {
    int prev = cury - 1, pl = linelen[prev], cl = linelen[cury];
    if (pl + cl >= MAXCOLS)
      return;
    for (int j = 0; j < cl; j++)
      lines[prev][pl + j] = lines[cury][j];
    linelen[prev] = pl + cl;
    for (int i = cury; i < nlines - 1; i++) {
      linelen[i] = linelen[i + 1];
      for (int j = 0; j < linelen[i]; j++)
        lines[i][j] = lines[i + 1][j];
    }
    nlines--;
    cury = prev;
    curx = pl;
    modified = true;
  }
}

static void ed_delete(void)
{
  if (sel_active) {
    sel_delete();
    return;
  } /* Delete removes a selection */
  if (curx < linelen[cury]) {
    for (int j = curx; j < linelen[cury] - 1; j++)
      lines[cury][j] = lines[cury][j + 1];
    linelen[cury]--;
    modified = true;
  } else if (cury < nlines - 1) {
    cury++;
    curx = 0;
    ed_backspace();
  }
}

static void clamp_curx(void)
{
  if (curx > linelen[cury])
    curx = linelen[cury];
  if (curx < 0)
    curx = 0;
}

/* file I/O */

static void ed_load(const char *name)
{
  int n = readfile(name, io_buf, (int)sizeof(io_buf));
  if (n < 0) {
    set_status(n == -EACCES ? "open: permission denied" : "open: not found");
    return;
  }
  ed_clear();
  int li = 0, col = 0;
  for (int i = 0; i < n; i++) {
    char ch = io_buf[i];
    if (ch == '\n') {
      linelen[li] = col;
      if (li < MAXLINES - 1) {
        li++;
        col = 0;
      } else
        break;
    } else if (ch != '\r' && col < MAXCOLS - 1) {
      lines[li][col++] = ch;
      if ((unsigned char)ch >= 0x80)
        has_high = true;
    }
  }
  linelen[li] = col;
  nlines = li + 1;
  curx = cury = top_line = 0;
  scopy(fname, name, sizeof fname);
  has_name = true;
  modified = false;
  set_status("opened");
}

static void ed_save(const char *name)
{
  int p = 0;
  for (int i = 0; i < nlines; i++) {
    for (int j = 0; j < linelen[i] && p < (int)sizeof(io_buf); j++)
      io_buf[p++] = lines[i][j];
    if (i < nlines - 1 && p < (int)sizeof(io_buf))
      io_buf[p++] = '\n';
  }
  int wr = writefile(name, io_buf, p);
  if (wr == 0) {
    scopy(fname, name, sizeof fname);
    has_name = true;
    modified = false;
    set_status("saved");
  } else {
    set_status(wr == -EACCES ? "save: permission denied" : "save failed");
  }
}

/* menu */

static int g_win = -1; /* the editor's window id, for parenting modal dialogs */

/* Browse for a file to open or save; 1 if one was chosen */
static int pick_file(int mode, char *path, int n)
{
  char dir[FNAME_MAX];
  const char *base = "";
  scopy(dir, "/", sizeof dir);
  if (has_name) {
    scopy(dir, fname, sizeof dir);
    filelist_parent(dir);
    base = fname;
    for (const char *q = fname; *q; q++)
      if (*q == '/')
        base = q + 1;
  }
  struct filepicker_opts o = {.title = mode == FILEPICK_OPEN ? "Open" : "Save As",
                              .mode = mode,
                              .start = dir,
                              .name = mode == FILEPICK_SAVE ? base : NULL};
  return filepicker(g_win, &o, path, n);
}

/* 1 if it's safe to throw away the current buffer */
static int confirm_discard(void)
{
  if (!modified)
    return 1;
  return dialog_confirm(g_win, "Editor", "Discard unsaved changes?");
}

static void file_action(int item)
{
  char path[FNAME_MAX];
  switch (item) {
  case 0:
    if (!confirm_discard())
      break;
    ed_clear();
    has_name = false;
    fname[0] = 0;
    set_status("new file");
    break;
  case 1:
    if (!confirm_discard())
      break;
    if (pick_file(FILEPICK_OPEN, path, sizeof path))
      ed_load(path);
    break;
  case 2:
    if (has_name)
      ed_save(fname);
    else if (pick_file(FILEPICK_SAVE, path, sizeof path))
      ed_save(path);
    break;
  case 3:
    if (pick_file(FILEPICK_SAVE, path, sizeof path))
      ed_save(path);
    break;
  }
}

/* clipboard */

static void select_all(void)
{
  sel_active = 1;
  sel_x = 0;
  sel_y = 0;
  cury = nlines - 1;
  curx = linelen[cury];
}

static void do_copy(void)
{
  if (!sel_active) {
    set_status("nothing selected");
    return;
  }
  int n = sel_extract(clip_buf, sizeof clip_buf);
  if (clip_put_text(clip_buf, n) == 0)
    set_status("copied");
  else
    set_status("copy failed");
}

static void do_cut(void)
{
  if (!sel_active) {
    set_status("nothing selected");
    return;
  }
  do_copy();
  sel_delete();
  set_status("cut");

  int visrows = (H - MENU_H - STATUS_H) / ROW_H;
  if (cury < top_line)
    top_line = cury;
  if (top_line < 0)
    top_line = 0;
  (void)visrows;
}

static void do_paste(void)
{
  char mime[64];
  void *data = 0;
  int n = 0;
  if (clip_get(0, mime, sizeof mime, &data, &n) != 0 || n <= 0) {
    set_status("clipboard empty");
    return;
  }
  if (!mime_is_text(mime)) { /* can't insert an image/pdf/etc as text */
    free(data);
    set_status("clipboard is not text");
    return;
  }
  if (sel_active)
    sel_delete();
  const char *p = (const char *)data;
  for (int i = 0; i < n; i++) {
    char c = p[i];
    if (c == '\n')
      ed_newline();
    else if (c != '\r' && (unsigned char)c >= 0x20)
      ed_insert(c);
  }
  free(data);
  set_status("pasted");
}

static void edit_action(int item)
{
  switch (item) {
  case 0:
    select_all();
    break;
  case 1:
    do_cut();
    break;
  case 2:
    do_copy();
    break;
  case 3:
    do_paste();
    break;
  case 4:
    ed_delete();
    break; /* removes the selection, or the next char */
  }
}

static void menu_action(struct menu *leaf, int item)
{
  if (leaf == &editmenu)
    edit_action(item);
  else
    file_action(item);
}

/* input */

/* Caret moves, shared by the plain and Shift navigation keys */
static void move_left(void)
{
  if (curx > 0)
    curx--;
  else if (cury > 0) {
    cury--;
    curx = linelen[cury];
  }
}
static void move_right(void)
{
  if (curx < linelen[cury])
    curx++;
  else if (cury < nlines - 1) {
    cury++;
    curx = 0;
  }
}
static void move_up(void)
{
  if (cury > 0) {
    cury--;
    clamp_curx();
  }
}
static void move_down(void)
{
  if (cury < nlines - 1) {
    cury++;
    clamp_curx();
  }
}

/* Cut/Copy/Delete only apply with a selection -> grey them out otherwise */
static void sync_edit_flags(void)
{
  edit_flags[1] = sel_active ? 0 : MENU_DISABLED; /* Cut */
  edit_flags[2] = sel_active ? 0 : MENU_DISABLED; /* Copy */
}

static void key(int id, char c)
{
  /* Menu accelerators (Ctrl+N/O/S/A/X, and Ctrl+C as Copy): fire the item directly, like a click */
  sync_edit_flags();
  struct menu *leaf;
  int sc = menubar_shortcut(&mbar, (unsigned char)c, &leaf);
  if (sc >= 0) {
    menubar_close(&mbar);
    menu_action(leaf, sc);
    goto rescroll;
  }
  if (c == 27) {
    menubar_close(&mbar);
    sel_clear();
    goto rescroll;
  }

  switch ((unsigned char)c) {
  case '\n':
    ed_newline();
    break;
  case '\b':
    ed_backspace();
    break;
  case KEY_DELETE:
    ed_delete();
    break;

    /* Plain navigation: drop any selection, then move */
  case KEY_LEFT:
    sel_clear();
    move_left();
    break;
  case KEY_RIGHT:
    sel_clear();
    move_right();
    break;
  case KEY_UP:
    sel_clear();
    move_up();
    break;
  case KEY_DOWN:
    sel_clear();
    move_down();
    break;
  case KEY_HOME:
    sel_clear();
    curx = 0;
    break;
  case KEY_END:
    sel_clear();
    curx = linelen[cury];
    break;

    /* Shift+navigation: anchor a selection if needed, then move the caret */
  case KEY_SLEFT:
    sel_begin();
    move_left();
    break;
  case KEY_SRIGHT:
    sel_begin();
    move_right();
    break;
  case KEY_SUP:
    sel_begin();
    move_up();
    break;
  case KEY_SDOWN:
    sel_begin();
    move_down();
    break;
  case KEY_SHOME:
    sel_begin();
    curx = 0;
    break;
  case KEY_SEND:
    sel_begin();
    curx = linelen[cury];
    break;

  case KEY_F5: /* reload the file from disk, discarding edits */
    if (has_name)
      ed_load(fname);
    else
      set_status("nothing to reload");
    break;
  default:
    if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F)
      ed_insert(c);
    break;
  }
  (void)id;

rescroll:;
  int visrows = (H - MENU_H - STATUS_H) / ROW_H;
  if (cury < top_line)
    top_line = cury;
  if (cury >= top_line + visrows)
    top_line = cury - visrows + 1;
  if (top_line < 0)
    top_line = 0;
}

static void click(int mx, int my)
{
  if (my < MENU_H || filemenu.open || editmenu.open) { /* bar or open dropdown */
    struct menu *leaf;
    int item;
    if (menubar_click(&mbar, mx, my, &leaf, &item) && item >= 0)
      menu_action(leaf, item);
    return;
  }
  int ty0 = MENU_H, ty1 = H - STATUS_H;
  if (my >= ty0 && my < ty1) {
    int row = (my - ty0) / ROW_H + top_line;
    if (row >= nlines)
      row = nlines - 1;
    if (row < 0)
      row = 0;
    sel_clear(); /* a click places the caret, dropping any span */
    cury = row;
    curx = (mx - 2) / COL_W;
    clamp_curx();
  }
}

/* rendering */

static void apnd(char *buf, int *p, int cap, const char *s)
{
  for (int i = 0; s[i] && *p < cap - 1; i++)
    buf[(*p)++] = s[i];
  buf[*p] = 0;
}

static void apnd_uint(char *buf, int *p, int cap, unsigned v)
{
  char t[12];
  int n = 0;
  if (v == 0)
    t[n++] = '0';
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n && *p < cap - 1)
    buf[(*p)++] = t[--n];
  buf[*p] = 0;
}

static void render(void)
{
  unsigned txt_bg = 0x0d1117;

  /* Menu bar background (the File menu itself is drawn last, on top) */
  gfx_fill_rect(0, 0, W, MENU_H, 0x232a36);

  /* Text area */
  int ty0 = MENU_H, th = H - MENU_H - STATUS_H;
  gfx_fill_rect(0, ty0, W, th, txt_bg);
  int visrows = th / ROW_H;
  int maxcol = (W - 4) / COL_W;

  /* Selection bounds (in document order) for the highlight below */
  int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
  if (sel_active)
    sel_bounds(&sx0, &sy0, &sx1, &sy1);
  unsigned sel_bg = 0x2d4a78;

  for (int r = 0; r < visrows; r++) {
    int ln = top_line + r;
    if (ln >= nlines)
      break;
    int ry = ty0 + r * ROW_H;
    int cols = linelen[ln] < maxcol ? linelen[ln] : maxcol;

    /* This line's selected column span [c0,c1); -1 = not selected */
    int c0 = -1, c1 = -1;
    if (sel_active && ln >= sy0 && ln <= sy1) {
      c0 = (ln == sy0) ? sx0 : 0;
      c1 = (ln == sy1) ? sx1 : linelen[ln];
      if (c0 > maxcol)
        c0 = maxcol;
      if (c1 > maxcol)
        c1 = maxcol;
    }
    for (int j = 0; j < cols; j++) {
      unsigned bg = (c0 >= 0 && j >= c0 && j < c1) ? sel_bg : txt_bg;
      gfx_glyph(2 + j * COL_W, ry, (unsigned char)lines[ln][j], 0xc8d0dc, bg);
    }
    if (c0 >= 0 && ln < sy1)
      gfx_fill_rect(2 + cols * COL_W, ry, COL_W, ROW_H, sel_bg);
  }

  /* Caret */
  if (cury >= top_line && cury < top_line + visrows) {
    int cc = curx < maxcol ? curx : maxcol;
    gfx_fill_rect(2 + cc * COL_W, ty0 + (cury - top_line) * ROW_H, 2, ROW_H, 0x66ddff);
  }

  /* Status bar, rendered with the gfx status_bar widget */
  int sy = H - STATUS_H;
  struct status_bar bar;
  char line[64];
  int p = 0;
  line[0] = 0;
  apnd(line, &p, sizeof line, has_name ? fname : "[no name]");
  if (modified)
    apnd(line, &p, sizeof line, " *");
  apnd(line, &p, sizeof line, "  -  ");
  apnd(line, &p, sizeof line, sbar.msg);
  status_set(&bar, line);
  status_draw(&bar, 0, sy, W, 0x9fb0c4, 0x161d28);

  /* Cursor position + encoding, right-aligned over the bar the widget just drew */
  char rstat[40];
  int rp = 0;
  rstat[0] = 0;
  apnd(rstat, &rp, sizeof rstat, "Ln ");
  apnd_uint(rstat, &rp, sizeof rstat, (unsigned)(cury + 1));
  apnd(rstat, &rp, sizeof rstat, ", Col ");
  apnd_uint(rstat, &rp, sizeof rstat, (unsigned)(curx + 1));
  apnd(rstat, &rp, sizeof rstat, has_high ? "  UTF-8" : "  ASCII");
  (void)rp;
  int rx = W - 6 - gfx_ui_width(rstat);
  if (rx < 6)
    rx = 6;
  gfx_ui_text(rx, sy + (GUI_BAR_H - UIFONT_H) / 2, rstat, 0x9fb0c4);

  /* The menu bar, drawn last so an open dropdown overlays the text */
  sync_edit_flags();
  menubar_draw(&mbar);
}

int main(int argc, char **argv)
{
  ed_clear();
  set_status("new file");
  int id = win_create(W, H, "Editor");
  if (id < 0) {
    puts("editor: win_create failed\n");
    return 1;
  }
  g_win = id;
  gfx_init(win_fb(), W, H);
  win_cursor(id, WIN_CURSOR_TEXT); /* I-beam over the text area */

  /* Launched with a path: open it straight away instead of starting on a blank buffer */
  if (argc > 1 && argv[1] && argv[1][0])
    ed_load(argv[1]);

  int need = 1;
  for (;;) {
    struct win_event ev;
    while (win_poll(id, &ev)) {
      need = 1;
      if (ev.type == WEV_CLOSE) {
        if (confirm_discard()) {
          win_close(id);
          return 0;
        }
      } else if (ev.type == WEV_KEY)
        key(id, (char)ev.a);
      else if (ev.type == WEV_MOUSE)
        click(ev.a, ev.b);
      else if (ev.type == WEV_RESIZE) {
        W = ev.a;
        H = ev.b;
        gfx_init(win_fb(), W, H); /* new buffer + pitch */
      }
    }
    if (need) {
      render();
      win_present(id);
      need = 0;
    }
    win_wait(id);
  }
}
