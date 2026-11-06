/* notifyd.c — the notification daemon.
 *
 * Owns the "notify" IPC port, keeps the notification history, and draws the
 * popups + the Notification Center in its own window. The compositor just shows
 * that window like any other app's — nothing notification-specific lives in the
 * kernel (the freedesktop / KDE Plasma model).
 *
 * Two threads share the address space: a server thread blocks on port_accept
 * and decodes incoming notifications; the main thread runs the window UI. They
 * hand off through a single-producer/single-consumer inbox ring (no lock needed
 * — only the server writes `head`, only the UI writes `tail`). The UI thread
 * owns the history and the window outright.
 *
 * A new notification pops the window up (the "toast"); the window persists as
 * the scrollable history until the user closes it, and reopens on the next
 * arrival. Launching notifyd a second time just asks the running one to show
 * itself.
 */
#include "gfx.h"
#include "notify.h"
#include "owos.h"
#include "upng.h"
#include <stdbool.h>

/* ---- layout + theme -------------------------------------------------------
 */
#define NW 384 /* default window width                          */
#define NH 492 /* default window height                         */
#define PAD 12
#define GAP 10
#define HDR_H 36
#define THUMB 56 /* max image thumbnail edge                      */
#define BODY_MAXLINES 4
#define LINE_H 19
#define BTN_GAP 8
#define CARD_BOT 12
#define SBW GUI_SCROLL_W

#define C_BG 0x141a24u
#define C_HEADER 0x0c0e14u
#define C_CARD 0x1d2533u
#define C_TXT 0xc8d0dcu
#define C_BRIGHT 0xffffffu
#define C_DIM 0x8b94a4u
#define C_SEP 0x05070au
#define C_ACCENT 0x2f6fb0u

struct note {
  int id;
  char app[NOTIFY_APP_MAX];
  char title[NOTIFY_TITLE_MAX];
  char subtitle[NOTIFY_SUB_MAX];
  char body[NOTIFY_BODY_MAX];
  char btn_label[NOTIFY_MAX_BUTTONS][NOTIFY_BLABEL_MAX];
  char btn_cmd[NOTIFY_MAX_BUTTONS][NOTIFY_BCMD_MAX];
  int nbtn;
  unsigned *img; /* ARGB thumbnail, or NULL                     */
  int iw, ih;
};

#define HIST_MAX 64
static struct note *hist[HIST_MAX]; /* newest first; UI-thread owned        */
static int nhist;

#define INBOX_N 32
static struct note *volatile inbox[INBOX_N];
static volatile int in_head, in_tail; /* SPSC: server head / UI tail */
static volatile bool show_req;        /* set by server, cleared by UI */
static int g_next_id = 1;

static void free_note(struct note *n)
{
  if (!n)
    return;
  if (n->img)
    free(n->img);
  free(n);
}

/* ---- framed I/O over a channel fd -----------------------------------------
 */
static int read_full(int fd, void *buf, int n)
{
  char *p = (char *)buf;
  int got = 0;
  while (got < n) {
    int r = read(fd, p + got, n - got);
    if (r <= 0)
      return 0;
    got += r;
  }
  return 1;
}

static int write_full(int fd, const void *buf, int n)
{
  const char *p = (const char *)buf;
  int put = 0;
  while (put < n) {
    int w = write(fd, p + put, n - put);
    if (w <= 0)
      return 0;
    put += w;
  }
  return 1;
}

/* Read a length-prefixed string (lenbytes = 1 or 2, LE) into dst[cap], NUL-
 * terminating and discarding any bytes past the cap so the framing stays
 * aligned. */
static int rd_str(int fd, int lenbytes, char *dst, int cap)
{
  unsigned char b[2] = {0, 0};
  if (!read_full(fd, b, lenbytes))
    return 0;
  unsigned len = lenbytes == 1 ? b[0] : (unsigned)b[0] | ((unsigned)b[1] << 8);
  int di = 0;
  while (len) {
    char c;
    if (!read_full(fd, &c, 1))
      return 0;
    if (di < cap - 1)
      dst[di++] = c;
    len--;
  }
  dst[di] = '\0';
  return 1;
}

/* ---- image decode: PNG path -> a small ARGB thumbnail ---------------------
 */
#define FR_BUF 8192
static unsigned char fr_buffer[FR_BUF]; /* server thread only          */

struct freader {
  const char *name;
  unsigned off, len, pos;
  bool eof;
};

static void fr_init(struct freader *f, const char *name)
{
  f->name = name;
  f->off = 0;
  f->len = 0;
  f->pos = 0;
  f->eof = false;
}

static int fr_get(void *c)
{
  struct freader *f = c;
  if (f->pos >= f->len) {
    if (f->eof)
      return -1;
    int n = readfile_at(f->name, f->off, fr_buffer, FR_BUF);
    if (n <= 0) {
      f->eof = true;
      return -1;
    }
    f->off += (unsigned)n;
    f->len = (unsigned)n;
    f->pos = 0;
  }
  return fr_buffer[f->pos++];
}

/* Nearest-neighbour downscale, streamed one source row at a time: each source
 * row paints the target rows it covers, so the whole frame is never resident.
 */
struct dctx {
  unsigned *out;
  int tw, th;
  unsigned sw, sh;
};

static int dec_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  struct dctx *d = c;
  int ty0 = (int)(y * (unsigned)d->th / d->sh);
  int ty1 = (int)((y + 1) * (unsigned)d->th / d->sh);
  if (ty1 <= ty0)
    ty1 = ty0 + 1;
  for (int ty = ty0; ty < ty1 && ty < d->th; ty++)
    for (int tx = 0; tx < d->tw; tx++) {
      unsigned sx = (unsigned)tx * w / (unsigned)d->tw;
      if (sx >= w)
        sx = w - 1;
      const unsigned char *p = rgba + sx * 4;
      d->out[ty * d->tw + tx] = ((unsigned)p[3] << 24) | ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
    }
  return 0;
}

static void load_image(struct note *n, const char *path)
{
  struct freader fr;
  unsigned sw, sh;
  fr_init(&fr, path);
  if (upng_dimensions(fr_get, &fr, &sw, &sh) != 0 || !sw || !sh)
    return;
  int tw, th;
  if (sw >= sh) {
    tw = THUMB;
    th = (int)((unsigned)THUMB * sh / sw);
  } else {
    th = THUMB;
    tw = (int)((unsigned)THUMB * sw / sh);
  }
  if (tw < 1)
    tw = 1;
  if (th < 1)
    th = 1;
  unsigned *out = calloc((unsigned)(tw * th), 4);
  if (!out)
    return;
  struct dctx d = {out, tw, th, sw, sh};
  fr_init(&fr, path);
  if (upng_decode(fr_get, &fr, dec_row, &d, 0, 0) != UPNG_OK) {
    free(out);
    return;
  }
  n->img = out;
  n->iw = tw;
  n->ih = th;
}

/* ---- inbox (server -> UI) -------------------------------------------------
 */
static void inbox_push(struct note *n)
{
  int nh = (in_head + 1) % INBOX_N;
  if (nh == in_tail) {
    free_note(n);
    return;
  } /* full: drop */
  inbox[in_head] = n;
  in_head = nh;
}

/* ---- the server thread ----------------------------------------------------
 */
static void serve(int ch)
{
  char op;
  if (read(ch, &op, 1) != 1)
    return;
  if (op == NOTIFY_OP_SHOW) {
    show_req = true;
    write_full(ch, "K", 1);
    return;
  }
  if (op != NOTIFY_OP_NOTIFY)
    return;

  struct note *n = calloc(1, sizeof *n);
  if (!n)
    return;
  char img[NOTIFY_IMG_MAX];
  if (!rd_str(ch, 1, n->app, NOTIFY_APP_MAX) || !rd_str(ch, 2, n->title, NOTIFY_TITLE_MAX) ||
      !rd_str(ch, 2, n->subtitle, NOTIFY_SUB_MAX) || !rd_str(ch, 2, n->body, NOTIFY_BODY_MAX) ||
      !rd_str(ch, 2, img, NOTIFY_IMG_MAX)) {
    free_note(n);
    return;
  }
  unsigned char nb;
  if (!read_full(ch, &nb, 1)) {
    free_note(n);
    return;
  }
  if (nb > NOTIFY_MAX_BUTTONS)
    nb = NOTIFY_MAX_BUTTONS;
  n->nbtn = nb;
  for (int i = 0; i < (int)nb; i++)
    if (!rd_str(ch, 1, n->btn_label[i], NOTIFY_BLABEL_MAX) || !rd_str(ch, 2, n->btn_cmd[i], NOTIFY_BCMD_MAX)) {
      free_note(n);
      return;
    }

  if (!n->app[0])
    strcpy(n->app, "App");
  n->id = g_next_id++;
  /* Ack before decoding the image so the sender doesn't block on a slow PNG;
   * the notification simply appears once the thumbnail is ready. */
  write_full(ch, "K", 1);
  if (img[0])
    load_image(n, img);
  inbox_push(n);
}

/* ---- history --------------------------------------------------------------
 */
static void hist_prepend(struct note *n)
{
  if (nhist == HIST_MAX) {
    free_note(hist[HIST_MAX - 1]);
    nhist--;
  }
  for (int i = nhist; i > 0; i--)
    hist[i] = hist[i - 1];
  hist[0] = n;
  nhist++;
}

static void hist_remove(int idx)
{
  if (idx < 0 || idx >= nhist)
    return;
  free_note(hist[idx]);
  for (int i = idx; i < nhist - 1; i++)
    hist[i] = hist[i + 1];
  nhist--;
}

static void hist_clear(void)
{
  for (int i = 0; i < nhist; i++)
    free_note(hist[i]);
  nhist = 0;
}

/* Drain everything the server queued; returns how many arrived. */
static int inbox_drain(void)
{
  int got = 0;
  while (in_tail != in_head) {
    struct note *n = inbox[in_tail];
    in_tail = (in_tail + 1) % INBOX_N;
    hist_prepend(n);
    got++;
  }
  return got;
}

/* ---- word-wrap (measure or draw) ------------------------------------------
 */
static int wrap_text(const char *s, int maxw, int maxlines, int draw, int x, int y, unsigned col)
{
  int lines = 0, i = 0;
  char line[192];
  int ll = 0;
  while (s[i] && lines < maxlines) {
    if (s[i] == '\n') {
      line[ll] = '\0';
      if (draw)
        gfx_ui_text(x, y + lines * LINE_H, line, col);
      lines++;
      ll = 0;
      i++;
      continue;
    }
    while (s[i] == ' ')
      i++;
    char word[160];
    int wl = 0;
    while (s[i] && s[i] != ' ' && s[i] != '\n' && wl < (int)sizeof word - 1)
      word[wl++] = s[i++];
    word[wl] = '\0';
    if (!wl)
      continue;

    char trial[192];
    int tl = 0;
    for (int k = 0; k < ll; k++)
      trial[tl++] = line[k];
    if (ll > 0)
      trial[tl++] = ' ';
    for (int k = 0; k < wl && tl < (int)sizeof trial - 1; k++)
      trial[tl++] = word[k];
    trial[tl] = '\0';

    if (ll == 0 || gfx_ui_width(trial) <= maxw) {
      for (int k = 0; k <= tl; k++)
        line[k] = trial[k];
      ll = tl;
    } else {
      line[ll] = '\0';
      if (draw)
        gfx_ui_text(x, y + lines * LINE_H, line, col);
      lines++;
      if (lines >= maxlines)
        break;
      for (int k = 0; k < wl; k++)
        line[k] = word[k];
      line[wl] = '\0';
      ll = wl;
    }
  }
  if (ll > 0 && lines < maxlines) {
    line[ll] = '\0';
    if (draw)
      gfx_ui_text(x, y + lines * LINE_H, line, col);
    lines++;
  }
  return lines;
}

/* UI text truncated with ".." to fit `maxw`. */
static void ui_clip(int x, int y, const char *s, unsigned col, int maxw)
{
  if (maxw <= 0)
    return;
  char buf[NOTIFY_TITLE_MAX + 4];
  int n = 0;
  for (; s[n] && n < (int)sizeof buf - 4; n++)
    buf[n] = s[n];
  buf[n] = '\0';
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
    buf[n--] = '\0';
  }
  if (n > 0)
    gfx_ui_text(x, y, buf, col);
}

/* ---- card geometry (shared by draw + hit-test) ----------------------------
 */
struct cardgeom {
  int textx, textw, sub_y, body_y, btn_y, height, close_x, close_y;
};

static void card_geom(const struct note *n, int x, int y, int w, struct cardgeom *g)
{
  int tx = n->img ? x + 12 + THUMB + 12 : x + 12;
  g->textx = tx;
  g->textw = (x + w - 12) - tx;

  int after = y + 10 + 22; /* below the title row           */
  g->sub_y = n->subtitle[0] ? after : -1;
  if (n->subtitle[0])
    after += LINE_H;
  g->body_y = n->body[0] ? after : -1;
  if (n->body[0])
    after += wrap_text(n->body, g->textw, BODY_MAXLINES, 0, 0, 0, 0) * LINE_H;
  g->btn_y = n->nbtn > 0 ? after + BTN_GAP : -1;

  int bottom = g->btn_y >= 0 ? g->btn_y + GUI_BTN_H : after;
  int h = (bottom - y) + CARD_BOT;
  if (n->img) {
    int min = 10 + n->ih + 10;
    if (h < min)
      h = min;
  }
  if (h < 64)
    h = 64;
  g->height = h;
  g->close_x = x + w - 22;
  g->close_y = y + 8;
}

/* Button rects within a card, laid out left to right from the text column. */
static int button_layout(const struct note *n, const struct cardgeom *g, int *bx, int *bw)
{
  int x = g->textx;
  for (int k = 0; k < n->nbtn; k++) {
    int lw = gfx_ui_width(n->btn_label[k]) + 24;
    if (lw < 64)
      lw = 64;
    bx[k] = x;
    bw[k] = lw;
    x += lw + BTN_GAP;
  }
  return n->nbtn;
}

static int cardw_for(int win_w) { return win_w - 2 * PAD - SBW - 6; }

static int content_height(int win_w)
{
  int total = 0, cw = cardw_for(win_w);
  for (int i = 0; i < nhist; i++) {
    struct cardgeom g;
    card_geom(hist[i], PAD, 0, cw, &g);
    total += g.height + GAP;
  }
  return total;
}

/* ---- rendering ------------------------------------------------------------
 */
static void draw_x(int x, int y, unsigned col)
{
  for (int d = 0; d < 9; d++) {
    gfx_pixel(x + d, y + d, col);
    gfx_pixel(x + 8 - d, y + d, col);
  }
}

static void draw_card(const struct note *n, int x, int y, int w, int newest)
{
  struct cardgeom g;
  card_geom(n, x, y, w, &g);

  gfx_fill_rect(x, y, w, g.height, C_CARD);
  gfx_rect(x, y, w, g.height, C_SEP);
  if (newest)
    gfx_fill_rect(x, y, 3, g.height, C_ACCENT); /* unread accent */

  if (n->img)
    gfx_blit_argb(x + 12, y + 12, n->iw, n->ih, n->img);

  int title_w = (g.close_x - 8) - g.textx;
  ui_clip(g.textx, y + 10, n->title, C_BRIGHT, title_w);
  if (g.sub_y >= 0)
    gfx_ui_text(g.textx, g.sub_y, n->subtitle, C_DIM);
  if (g.body_y >= 0)
    wrap_text(n->body, g.textw, BODY_MAXLINES, 1, g.textx, g.body_y, C_TXT);

  if (g.btn_y >= 0) {
    int bx[NOTIFY_MAX_BUTTONS], bw[NOTIFY_MAX_BUTTONS];
    int nb = button_layout(n, &g, bx, bw);
    for (int k = 0; k < nb; k++)
      button_draw(bx[k], g.btn_y, bw[k], GUI_BTN_H, n->btn_label[k], BTN_NORMAL);
  }

  draw_x(g.close_x, g.close_y, C_DIM);
}

static int g_scroll; /* list scroll, in pixels                         */

static void clamp_scroll(int win_w, int win_h)
{
  int max = content_height(win_w) - (win_h - HDR_H);
  if (max < 0)
    max = 0;
  if (g_scroll > max)
    g_scroll = max;
  if (g_scroll < 0)
    g_scroll = 0;
}

static void render(int win_w, int win_h)
{
  gfx_clear(C_BG);

  /* Cards first; the header is painted over their top so a scrolled-up card
   * slides cleanly under it. */
  int cw = cardw_for(win_w);
  int y = HDR_H - g_scroll;
  for (int i = 0; i < nhist; i++) {
    struct cardgeom g;
    card_geom(hist[i], PAD, y, cw, &g);
    if (y < win_h && y + g.height > HDR_H)
      draw_card(hist[i], PAD, y, cw, i == 0);
    y += g.height + GAP;
    if (y >= win_h)
      break;
  }

  gfx_fill_rect(0, 0, win_w, HDR_H, C_HEADER);
  gfx_fill_rect(0, HDR_H, win_w, 1, C_SEP);
  gfx_ui_text(PAD, (HDR_H - UIFONT_H) / 2, "Notifications", C_TXT);
  if (nhist)
    button_draw(win_w - PAD - 84, (HDR_H - 22) / 2, 84, 22, "Clear all", BTN_NORMAL);
  else
    gfx_ui_text((win_w - gfx_ui_width("No notifications")) / 2, HDR_H + (win_h - HDR_H - UIFONT_H) / 2,
                "No notifications", C_DIM);

  int visible = win_h - HDR_H;
  int total = content_height(win_w);
  scrollbar_draw(win_w - SBW, HDR_H, SBW, visible, g_scroll, total, visible);
}

/* ---- input ----------------------------------------------------------------
 */
static int g_sb_drag; /* dragging the scrollbar thumb                   */
static void run_command(const char *cmd);

/* Returns 1 if the click changed anything needing a repaint. */
static int handle_click(int mx, int my, int win_w, int win_h)
{
  if (my < HDR_H) { /* header                  */
    if (nhist && button_hit(win_w - PAD - 84, (HDR_H - 22) / 2, 84, 22, mx, my)) {
      hist_clear();
      g_scroll = 0;
      return 1;
    }
    return 0;
  }

  int visible = win_h - HDR_H;
  int total = content_height(win_w);
  if (mx >= win_w - SBW) { /* scrollbar               */
    int reg = scrollbar_hit(win_w - SBW, HDR_H, SBW, visible, mx, my);
    if (reg == SCROLLBAR_UP)
      g_scroll -= 40;
    else if (reg == SCROLLBAR_DOWN)
      g_scroll += 40;
    else if (reg == SCROLLBAR_TRACK) {
      g_sb_drag = 1;
      g_scroll = scrollbar_pos(win_w - SBW, HDR_H, SBW, visible, mx, my, g_scroll, total, visible);
    }
    clamp_scroll(win_w, win_h);
    return 1;
  }
  g_sb_drag = 0;

  int cw = cardw_for(win_w);
  int y = HDR_H - g_scroll;
  for (int i = 0; i < nhist; i++) {
    struct cardgeom g;
    card_geom(hist[i], PAD, y, cw, &g);
    if (my >= y && my < y + g.height && mx >= PAD && mx < PAD + cw) {
      if (mx >= g.close_x - 2 && mx <= g.close_x + 11 && my >= g.close_y - 2 && my <= g.close_y + 11) {
        hist_remove(i);
        clamp_scroll(win_w, win_h);
        return 1;
      }
      if (g.btn_y >= 0) {
        int bx[NOTIFY_MAX_BUTTONS], bw[NOTIFY_MAX_BUTTONS];
        int nb = button_layout(hist[i], &g, bx, bw);
        for (int k = 0; k < nb; k++)
          if (button_hit(bx[k], g.btn_y, bw[k], GUI_BTN_H, mx, my)) {
            char cmd[NOTIFY_BCMD_MAX];
            strcpy(cmd, hist[i]->btn_cmd[k]);
            hist_remove(i); /* dismiss on action (Plasma-style) */
            run_command(cmd);
            clamp_scroll(win_w, win_h);
            return 1;
          }
      }
      return 0;
    }
    y += g.height + GAP;
  }
  return 0;
}

/* Spawn a button's action: "prog [arg]" — bare names resolve under /bin. */
static void run_command(const char *cmd)
{
  if (!cmd || !cmd[0])
    return;
  char path[NOTIFY_BCMD_MAX], arg[NOTIFY_BCMD_MAX];
  int i = 0, pi = 0, ai = 0;
  while (cmd[i] == ' ')
    i++;
  while (cmd[i] && cmd[i] != ' ' && pi < (int)sizeof path - 1)
    path[pi++] = cmd[i++];
  path[pi] = '\0';
  while (cmd[i] == ' ')
    i++;
  while (cmd[i] && ai < (int)sizeof arg - 1)
    arg[ai++] = cmd[i++];
  arg[ai] = '\0';
  if (!path[0])
    return;

  char full[NOTIFY_BCMD_MAX + 8];
  if (path[0] != '/') {
    strcpy(full, "/bin/");
    strcat(full, path);
  } else
    strcpy(full, path);
  spawn(full, ai ? arg : 0);
}

/* ---- window lifecycle + main loop -----------------------------------------
 */
static int g_win = -1;
static int g_w = NW, g_h = NH;

static void open_window(void)
{
  if (g_win >= 0)
    return;
  g_win = win_create(NW, NH, "Notifications");
  if (g_win < 0)
    return;
  g_w = NW;
  g_h = NH;
  gfx_init(win_fb(), g_w, g_h);
  g_scroll = 0;
}

/* The server runs in its own thread, not main, for two reasons: a uwin window
 * can only be created/owned by the task that draws it (so the window stays in
 * main), and file descriptors are per-task — so this thread opens the listen fd
 * itself and blocks in port_accept. It shares only memory with main: the inbox
 * ring, the history, and show_req. */
static char server_stack[32768];
static void server_fn(void)
{
  int lst = port_open(NOTIFY_PORT, 1);
  if (lst < 0) {
    for (;;)
      msleep(1000);
  } /* lost a startup race: park */
  for (;;) {
    int ch = port_accept(lst);
    if (ch < 0)
      continue;
    serve(ch);
    close(ch);
  }
}

int main(void)
{
  /* If a notifyd is already serving, a second launch just asks it to show. */
  int probe = port_open(NOTIFY_PORT, 0);
  if (probe >= 0) {
    char op = NOTIFY_OP_SHOW, r = 0;
    write(probe, &op, 1);
    read(probe, &r, 1);
    close(probe);
    return 0;
  }
  thread_create(server_fn, &server_stack[sizeof server_stack]);

  bool dirty = true;
  for (;;) {
    if (inbox_drain()) {
      open_window();
      g_scroll = 0;
      dirty = true;
    }
    if (show_req) {
      show_req = false;
      open_window();
      dirty = true;
    }

    if (g_win < 0) {
      msleep(60);
      continue;
    }

    struct win_event ev;
    while (win_poll(g_win, &ev)) {
      if (ev.type == WEV_CLOSE) {
        win_close(g_win);
        g_win = -1;
        break;
      } else if (ev.type == WEV_RESIZE) {
        g_w = ev.a;
        g_h = ev.b;
        gfx_init(win_fb(), g_w, g_h);
        clamp_scroll(g_w, g_h);
        dirty = true;
      } else if (ev.type == WEV_SCROLL) {
        g_scroll -= ev.a * 40;
        clamp_scroll(g_w, g_h);
        dirty = true;
      } else if (ev.type == WEV_MOUSE) {
        if (handle_click(ev.a, ev.b, g_w, g_h))
          dirty = true;
      } else if (ev.type == WEV_DRAG) {
        if (g_sb_drag) {
          int visible = g_h - HDR_H;
          g_scroll = scrollbar_pos(g_w - SBW, HDR_H, SBW, visible, ev.a, ev.b, g_scroll, content_height(g_w), visible);
          clamp_scroll(g_w, g_h);
          dirty = true;
        }
      }
    }
    if (g_win < 0)
      continue;

    if (dirty) {
      render(g_w, g_h);
      win_present(g_win);
      dirty = false;
    }
    win_wait(g_win);
  }
}
