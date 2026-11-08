/* term.c, a terminal emulator: a resizable, scrollable window that draws what a program on a pty writes
 * (text with ANSI escapes) and sends it the keys. The program is /bin/sh. */

#include "gfx.h"
#include "owos.h"
#include <stdbool.h>

#define FONT_W 8
#define FONT_H 16
#define MAXCOLS 200
#define SBROWS 400 /* scrollback lines */

#define BG 0x0e1116u
#define FG 0xc8d0dcu

/* scrollback ring buffer */

static unsigned char gch[SBROWS][MAXCOLS];
static unsigned gfg[SBROWS][MAXCOLS];
static int top;        /* phys index of logical line 0 */
static int nlines = 1; /* logical lines in use */
static int cl, cc;     /* cursor: logical line, column */
static int view;       /* lines scrolled up from the bottom */
static unsigned cur_fg = FG;
static int cols = 90, rows = 27;   /* active grid (from window size) */
static int winw = 720, winh = 432; /* window client size in pixels */
static int win;
static int master = -1; /* our end of the shell's pty */
static bool dirty = true;

static int phys(int logical) { return (top + logical) % SBROWS; }

static void line_blank(int p)
{
  for (int x = 0; x < MAXCOLS; x++) {
    gch[p][x] = ' ';
    gfg[p][x] = FG;
  }
}

static void grid_reset(void)
{
  for (int p = 0; p < SBROWS; p++)
    line_blank(p);
  top = 0;
  nlines = 1;
  cl = cc = 0;
  view = 0;
  dirty = true;
}

/* First logical line of the screen (the last `rows` lines of the buffer) */
static int screen_base(void) { return nlines > rows ? nlines - rows : 0; }

/* Move the cursor to the next line, scrolling when the buffer is full */
static void linefeed(void)
{
  cl++;
  if (cl >= nlines) {
    if (nlines < SBROWS) {
      nlines = cl + 1;
    } else {
      top = (top + 1) % SBROWS; /* drop the oldest line */
      cl--;
      line_blank((top + nlines - 1) % SBROWS);
    }
  }
}

/* 16-colour ANSI palette */
static const unsigned ansi_rgb[16] = {
    0x1b1f26, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xc8d0dc,
    0x5c6370, 0xff7b86, 0xb5e890, 0xffd68a, 0x7ec5ff, 0xe0a0ff, 0x70d8e4, 0xffffff,
};

/* xterm 256-colour palette entry */
static unsigned palette256(int n)
{
  if (n < 16)
    return ansi_rgb[n];
  if (n >= 232) {
    unsigned v = (unsigned)(8 + (n - 232) * 10);
    return v << 16 | v << 8 | v;
  }
  n -= 16;
  static const unsigned level[6] = {0, 95, 135, 175, 215, 255};
  return level[n / 36] << 16 | level[n / 6 % 6] << 8 | level[n % 6];
}

/* Escape sequence parser state */
enum { ST_TEXT, ST_ESC, ST_CSI, ST_OSC };
static int state;
static int eparm[16], enp;
static bool eprivate;

static int param(int i, int dflt) { return i <= enp && eparm[i] > 0 ? eparm[i] : dflt; }

static void sgr_apply(void)
{
  for (int i = 0; i <= enp; i++) {
    int p = eparm[i];
    if (p == 0 || p == 39)
      cur_fg = FG;
    else if (p >= 30 && p <= 37)
      cur_fg = ansi_rgb[p - 30];
    else if (p >= 90 && p <= 97)
      cur_fg = ansi_rgb[8 + p - 90];
    else if (p == 38 && i + 2 <= enp && eparm[i + 1] == 5) {
      cur_fg = palette256(eparm[i + 2]);
      i += 2;
    } else if (p == 38 && i + 4 <= enp && eparm[i + 1] == 2) {
      cur_fg =
          (unsigned)(eparm[i + 2] & 255) << 16 | (unsigned)(eparm[i + 3] & 255) << 8 | (unsigned)(eparm[i + 4] & 255);
      i += 4;
    }
    /* background colours and text attributes are ignored: the grid background is fixed */
  }
}

static void erase_cells(int line, int from, int to)
{
  int p = phys(line);
  for (int x = from; x < to && x < MAXCOLS; x++) {
    gch[p][x] = ' ';
    gfg[p][x] = FG;
  }
}

/* ESC [ ... final byte */
static void csi_dispatch(char final)
{
  int base = screen_base();
  switch (final) {
  case 'm':
    sgr_apply();
    break;
  case 'A':
    cl -= param(0, 1);
    if (cl < base)
      cl = base;
    break;
  case 'B':
    cl += param(0, 1);
    if (cl >= nlines)
      cl = nlines - 1;
    break;
  case 'C':
    cc += param(0, 1);
    if (cc > cols - 1)
      cc = cols - 1;
    break;
  case 'D':
    cc -= param(0, 1);
    if (cc < 0)
      cc = 0;
    break;
  case 'G':
    cc = param(0, 1) - 1;
    if (cc > cols - 1)
      cc = cols - 1;
    break;
  case 'H':
  case 'f': {
    int row = param(0, 1) - 1;
    cc = param(1, 1) - 1;
    if (cc > cols - 1)
      cc = cols - 1;
    while (base + row >= nlines && nlines < SBROWS)
      nlines++;
    cl = base + row < nlines ? base + row : nlines - 1;
    break;
  }
  case 'K': {
    int mode = eparm[0];
    if (mode == 0)
      erase_cells(cl, cc, MAXCOLS);
    else if (mode == 1)
      erase_cells(cl, 0, cc + 1);
    else if (mode == 2)
      erase_cells(cl, 0, MAXCOLS);
    break;
  }
  case 'J': {
    int mode = eparm[0];
    if (mode == 3) {
      grid_reset();
    } else if (mode == 2) {
      for (int l = base; l < nlines; l++)
        erase_cells(l, 0, MAXCOLS);
    } else if (mode == 1) {
      for (int l = base; l < cl; l++)
        erase_cells(l, 0, MAXCOLS);
      erase_cells(cl, 0, cc + 1);
    } else {
      erase_cells(cl, cc, MAXCOLS);
      for (int l = cl + 1; l < nlines; l++)
        erase_cells(l, 0, MAXCOLS);
    }
    break;
  }
  }
  dirty = true;
}

static void put_char(unsigned char c)
{
  view = 0; /* any output snaps to the bottom */
  if (c == '\n') {
    linefeed();
  } else if (c == '\r') {
    cc = 0;
  } else if (c == '\b') {
    if (cc > 0)
      cc--;
  } else if (c == '\t') {
    cc = (cc / 8 + 1) * 8;
    if (cc > cols - 1)
      cc = cols - 1;
  } else if (c >= 0x20 && c != 0x7f) {
    if (cc >= cols) {
      cc = 0;
      linefeed();
    }
    int p = phys(cl);
    gch[p][cc] = c;
    gfg[p][cc] = cur_fg;
    cc++;
  }
  dirty = true;
}

/* Feed one byte the program wrote */
static void feed(unsigned char c)
{
  switch (state) {
  case ST_ESC:
    state = c == '[' ? ST_CSI : c == ']' ? ST_OSC : ST_TEXT;
    if (state == ST_CSI) {
      enp = 0;
      eparm[0] = 0;
      eprivate = false;
    }
    return;
  case ST_CSI:
    if (c >= '0' && c <= '9') {
      eparm[enp] = eparm[enp] * 10 + (c - '0');
    } else if (c == ';') {
      if (enp < 15)
        eparm[++enp] = 0;
    } else if (c == '?' || c == '>' || c == '=') {
      eprivate = true;
    } else if (c >= 0x40 && c <= 0x7e) {
      if (!eprivate)
        csi_dispatch((char)c);
      state = ST_TEXT;
    }
    return;
  case ST_OSC: /* window title etc.: skip to BEL or ST */
    if (c == 0x07 || c == 0x1b)
      state = c == 0x1b ? ST_ESC : ST_TEXT;
    return;
  }
  if (c == 0x1b)
    state = ST_ESC;
  else
    put_char(c);
}

static void redraw(void)
{
  gfx_fill_rect(0, 0, winw - GUI_SCROLL_W, winh, BG); /* text area */
  int bottom = nlines > rows ? nlines - rows : 0;
  int vt = bottom - view;
  if (vt < 0)
    vt = 0;
  for (int r = 0; r < rows; r++) {
    int L = vt + r;
    if (L >= nlines)
      break;
    int p = phys(L);
    for (int x = 0; x < cols; x++)
      if (gch[p][x] != ' ')
        gfx_glyph(x * FONT_W, r * FONT_H, gch[p][x], gfg[p][x], BG);
  }
  if (view == 0 && cl >= vt && cl < vt + rows) /* caret at bottom */
    gfx_fill_rect((cc < cols ? cc : cols - 1) * FONT_W, (cl - vt) * FONT_H + FONT_H - 2, FONT_W, 2, FG);

  /* Scrollbar down the right edge: `scroll` is the top visible line (vt) */
  scrollbar_draw(winw - GUI_SCROLL_W, 0, GUI_SCROLL_W, winh, bottom - view, nlines, rows);

  win_present(win);
  dirty = false;
}

/* Keys go to the program as the bytes a terminal sends */
static void send(const char *s) { write(master, s, (int)strlen(s)); }

static void on_key(unsigned char c)
{
  switch (c) {
  case KEY_UP:
    send("\x1b[A");
    break;
  case KEY_DOWN:
    send("\x1b[B");
    break;
  case KEY_RIGHT:
    send("\x1b[C");
    break;
  case KEY_LEFT:
    send("\x1b[D");
    break;
  case KEY_HOME:
    send("\x1b[H");
    break;
  case KEY_END:
    send("\x1b[F");
    break;
  case KEY_DELETE:
    send("\x1b[3~");
    break;
  case KEY_SELALL: /* Ctrl+A */
    send("\x01");
    break;
  case KEY_INTR: { /* Ctrl+C: interrupt the foreground program, or hand the shell a ^C at its prompt */
    int fg = tty_fg(master);
    if (fg > 0)
      kill(fg, SIGINT);
    else
      send("\x03");
    break;
  }
  case '\n':
  case '\r':
    send("\r");
    break;
  case '\b':
    send("\x7f");
    break;
  default:
    if (c >= 0x20 && c < 0x7f) {
      char b = (char)c;
      write(master, &b, 1);
    }
    break;
  }
}

static void on_scroll(int dz)
{
  int maxoff = nlines > rows ? nlines - rows : 0;
  view -= dz * 3; /* wheel up scrolls back to older lines */
  if (view < 0)
    view = 0;
  if (view > maxoff)
    view = maxoff;
  dirty = true;
}

/* The scrollbar widget works in top-line units (`scroll`, 0..bottom) */
static int sb_drag; /* 1 while the thumb is held */

static void sb_set(int scroll)
{
  int maxoff = nlines > rows ? nlines - rows : 0;
  view = maxoff - scroll;
  if (view < 0)
    view = 0;
  if (view > maxoff)
    view = maxoff;
  dirty = true;
}

/* Left-click on the bar: arrows step a line */
static void on_click(int mx, int my)
{
  sb_drag = 0;
  int maxoff = nlines > rows ? nlines - rows : 0;
  if (maxoff <= 0)
    return; /* nothing to scroll */
  int sbx = winw - GUI_SCROLL_W;
  int part = scrollbar_hit(sbx, 0, GUI_SCROLL_W, winh, mx, my);
  int scroll = maxoff - view;
  if (part == SCROLLBAR_UP)
    sb_set(scroll - 3);
  else if (part == SCROLLBAR_DOWN)
    sb_set(scroll + 3);
  else if (part == SCROLLBAR_TRACK) {
    sb_drag = 1;
    sb_set(scrollbar_pos(sbx, 0, GUI_SCROLL_W, winh, mx, my, scroll, nlines, rows));
  }
}

/* Drag while the thumb is held: track the cursor's y */
static void on_drag(int mx, int my)
{
  (void)mx;
  if (!sb_drag)
    return;
  int maxoff = nlines > rows ? nlines - rows : 0;
  int sbx = winw - GUI_SCROLL_W;
  sb_set(scrollbar_pos(sbx, 0, GUI_SCROLL_W, winh, sbx + GUI_SCROLL_W / 2, my, maxoff - view, nlines, rows));
}

static void on_resize(int w, int h)
{
  gfx_init(win_fb(), w, h);
  winw = w;
  winh = h;
  cols = (w - GUI_SCROLL_W) / FONT_W; /* reserve the right edge for the bar */
  if (cols > MAXCOLS)
    cols = MAXCOLS;
  if (cols < 1)
    cols = 1;
  rows = h / FONT_H;
  if (rows < 1)
    rows = 1;
  tty_set_size(master, cols, rows);
  dirty = true;
}

int main(void)
{
  win = win_create(720, 432, "Terminal");
  if (win < 0) {
    puts("term: no window\n");
    return 1;
  }
  gfx_init(win_fb(), 720, 432);
  win_resizable(win, 1);
  winw = 720;
  winh = 432;
  cols = (720 - GUI_SCROLL_W) / FONT_W;
  rows = 432 / FONT_H;
  grid_reset();

  setenv("TERM", "xterm", 1);
  char *argv[] = {"sh"};
  struct spawn_opts o = {.path = "/bin/sh", .argv = argv, .argc = 1, .flags = SPAWN_PTY, .rfd = &master};
  int pid = spawn_ex(&o);
  if (pid < 0) {
    win_close(win);
    puts("term: cannot start /bin/sh\n");
    return 1;
  }
  tty_set_size(master, cols, rows);

  char buf[4096];
  for (;;) {
    struct win_event ev;
    while (win_poll(win, &ev)) {
      switch (ev.type) {
      case WEV_CLOSE:
        kill(pid, SIGKILL);
        waitpid(pid);
        win_close(win);
        return 0;
      case WEV_KEY:
        on_key((unsigned char)ev.a);
        break;
      case WEV_SCROLL:
        on_scroll(ev.a);
        break;
      case WEV_MOUSE:
        on_click(ev.a, ev.b);
        break;
      case WEV_DRAG:
        on_drag(ev.a, ev.b);
        break;
      case WEV_RESIZE:
        on_resize(ev.a, ev.b);
        break;
      }
    }
    int n = read_nb(master, buf, sizeof buf);
    if (n < 0) { /* the shell exited */
      waitpid(pid);
      win_close(win);
      return 0;
    }
    for (int i = 0; i < n; i++)
      feed((unsigned char)buf[i]);
    if (dirty)
      redraw();
    if (n == 0)
      win_wait(win);
  }
}
