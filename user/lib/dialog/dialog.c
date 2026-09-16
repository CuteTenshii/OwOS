/* dialog.c — modal message-box + property-sheet dialogs. See dialog.h.
 *
 * Each dialog is a second window in the calling process: created transient +
 * modal on its parent, drawn with gfx into its own client buffer (win_fb_of),
 * and driven by a small blocking event loop. The compositor supplies the title
 * bar, border and close box; we draw only the client area (icon, text,
 * buttons).
 */
#include "dialog.h"
#include "gfx.h"
#include "owos.h"

/* Layout metrics + palette (kept close to the desktop chrome colours). */
#define PAD 16
#define LINE_H 20
#define BTN_H 26
#define BTN_GAP 10
#define BTN_MINW 74
#define ICON_SZ 32

#define DLG_BG 0x161d28
#define DLG_TEXT 0xd6deea
#define DLG_DIM 0x9fb0c4
#define BTN_NORM 0x2a3446
#define BTN_HOT 0x36435a
#define BTN_PRIM 0x2f6fb0
#define BTN_PRIMH 0x3f83c8
#define BORDER 0x05070a

#define MSG_W 360
#define MAXLINES 10
#define LINEBUF 160

/* ------------------------------------------------------------- small helpers
 */

static int sqrt_i(int v)
{
  int r = 0;
  while ((r + 1) * (r + 1) <= v)
    r++;
  return r;
}

static void fill_disc(int cx, int cy, int r, unsigned col)
{
  for (int dy = -r; dy <= r; dy++) {
    int dx = sqrt_i(r * r - dy * dy);
    gfx_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, col);
  }
}

/* A 32x32 icon: a coloured disc with a white symbol, at (x,y). */
static void draw_icon(int type, int x, int y)
{
  unsigned col;
  const char *sym;
  switch (type) {
  case DLG_ICON_INFO:
    col = 0x2f80d0;
    sym = "i";
    break;
  case DLG_ICON_QUESTION:
    col = 0x2f80d0;
    sym = "?";
    break;
  case DLG_ICON_WARN:
    col = 0xe0a020;
    sym = "!";
    break;
  case DLG_ICON_ERROR:
    col = 0xd04030;
    sym = "\xC3\x97";
    break; /* U+00D7 multiply */
  default:
    return;
  }
  int cx = x + ICON_SZ / 2, cy = y + ICON_SZ / 2;
  fill_disc(cx, cy, ICON_SZ / 2 - 1, col);
  int px = 24;
  int w = gfx_ui_width_px(sym, px);
  gfx_ui_text_px(cx - w / 2, cy - px / 2, sym, 0xffffff, px);
}

/* Word-wrap `msg` into `lines` (each < LINEBUF), breaking on spaces and
 * honouring explicit '\n'. Returns the line count (capped at MAXLINES). */
static int wrap(const char *msg, int maxw, char lines[][LINEBUF], int maxlines)
{
  int nl = 0, cur = 0;
  char cbuf[LINEBUF];
  cbuf[0] = 0;
  int i = 0;
  while (msg[i] && nl < maxlines) {
    /* pull one word (up to the next space/newline/end) */
    int ws = i;
    while (msg[i] && msg[i] != ' ' && msg[i] != '\n')
      i++;
    int wl = i - ws;
    char word[LINEBUF];
    if (wl > LINEBUF - 1)
      wl = LINEBUF - 1;
    for (int k = 0; k < wl; k++)
      word[k] = msg[ws + k];
    word[wl] = 0;

    char trial[LINEBUF];
    int t = 0;
    for (int k = 0; k < cur && t < LINEBUF - 1; k++)
      trial[t++] = cbuf[k];
    if (cur && t < LINEBUF - 1)
      trial[t++] = ' ';
    for (int k = 0; word[k] && t < LINEBUF - 1; k++)
      trial[t++] = word[k];
    trial[t] = 0;

    if (cur && gfx_ui_width(trial) > maxw) { /* word doesn't fit: flush line */
      for (int k = 0; k < cur; k++)
        lines[nl][k] = cbuf[k];
      lines[nl][cur] = 0;
      nl++;
      cur = 0;
      cbuf[0] = 0;
      if (nl >= maxlines)
        break;
      /* start the new line with this word */
      for (int k = 0; word[k] && cur < LINEBUF - 1; k++)
        cbuf[cur++] = word[k];
      cbuf[cur] = 0;
    } else {
      for (int k = 0; trial[k]; k++)
        cbuf[k] = trial[k];
      cur = t;
    }

    if (msg[i] == '\n') { /* forced break */
      for (int k = 0; k < cur; k++)
        lines[nl][k] = cbuf[k];
      lines[nl][cur] = 0;
      nl++;
      cur = 0;
      cbuf[0] = 0;
      i++;
    } else if (msg[i] == ' ') {
      i++;
    }
  }
  if (cur && nl < maxlines) {
    for (int k = 0; k < cur; k++)
      lines[nl][k] = cbuf[k];
    lines[nl][cur] = 0;
    nl++;
  }
  if (nl == 0) {
    lines[0][0] = 0;
    nl = 1;
  }
  return nl;
}

/* ------------------------------------------------------------- buttons */

struct dbtn {
  const char *label;
  int result;
};

static int build_buttons(unsigned flags, struct dbtn *b)
{
  int n = 0;
  switch (flags & DLG_BTN_MASK) {
  case DLG_OKCANCEL:
    b[n].label = "Cancel";
    b[n++].result = DLG_R_CANCEL;
    b[n].label = "OK";
    b[n++].result = DLG_R_OK;
    break;
  case DLG_YESNO:
    b[n].label = "No";
    b[n++].result = DLG_R_NO;
    b[n].label = "Yes";
    b[n++].result = DLG_R_YES;
    break;
  case DLG_YESNOCANCEL:
    b[n].label = "Cancel";
    b[n++].result = DLG_R_CANCEL;
    b[n].label = "No";
    b[n++].result = DLG_R_NO;
    b[n].label = "Yes";
    b[n++].result = DLG_R_YES;
    break;
  default:
    b[n].label = "OK";
    b[n++].result = DLG_R_OK;
    break;
  }
  return n;
}

/* Right-align the buttons in [0,W) with the primary (last) at the right edge.
 */
static void layout_buttons(struct dbtn *b, int n, int W, int H, int *bx, int *bw, int *by)
{
  *by = H - PAD - BTN_H;
  int x = W - PAD;
  for (int i = n - 1; i >= 0; i--) {
    int w = gfx_ui_width(b[i].label) + 28;
    if (w < BTN_MINW)
      w = BTN_MINW;
    bw[i] = w;
    x -= w;
    bx[i] = x;
    x -= BTN_GAP;
  }
}

static void draw_button(int x, int y, int w, const char *label, int primary, int hot)
{
  unsigned bg = primary ? (hot ? BTN_PRIMH : BTN_PRIM) : (hot ? BTN_HOT : BTN_NORM);
  gfx_fill_rect(x, y, w, BTN_H, bg);
  gfx_rect(x, y, w, BTN_H, BORDER);
  int tx = x + (w - gfx_ui_width(label)) / 2;
  gfx_ui_text(tx, y + (BTN_H - UIFONT_H) / 2, label, primary ? 0xffffff : DLG_TEXT);
}

/* Hit-test the button row; returns the button index under (mx,my) or -1. */
static int button_at(struct dbtn *b, int n, int W, int H, int mx, int my)
{
  int bx[4], bw[4], by;
  layout_buttons(b, n, W, H, bx, bw, &by);
  for (int i = 0; i < n; i++)
    if (mx >= bx[i] && mx < bx[i] + bw[i] && my >= by && my < by + BTN_H)
      return i;
  return -1;
}

/* ------------------------------------------------------------- message box */

int dialog_msg(int parent, const char *title, const char *message, unsigned flags)
{
  struct dbtn b[4];
  int nb = build_buttons(flags, b);
  int esc = b[0].result; /* safe/negative choice: Esc + close box */
  int prim = nb - 1;     /* affirmative: Enter + default highlight */
  int icon = flags & DLG_ICON_MASK;

  int W = MSG_W;
  int textx = PAD + (icon ? ICON_SZ + 12 : 0);
  int maxw = W - textx - PAD;
  char lines[MAXLINES][LINEBUF];
  int nl = wrap(message ? message : "", maxw, lines, MAXLINES);

  int body_h = nl * LINE_H;
  if (icon && ICON_SZ > body_h)
    body_h = ICON_SZ;
  int H = PAD + body_h + PAD + BTN_H + PAD;
  if (H < 120)
    H = 120;

  /* Save the caller's gfx surface so we can restore it before returning. */
  unsigned *save_fb;
  int save_w, save_h;
  gfx_get(&save_fb, &save_w, &save_h);

  int id = win_create(W, H, title ? title : "");
  if (id < 0)
    return esc;
  unsigned *fb = win_fb_of(id);
  if (!fb) {
    win_close(id);
    gfx_init(save_fb, save_w, save_h);
    return esc;
  }
  gfx_init(fb, W, H);
  win_resizable(id, 0);
  if (parent >= 0)
    win_set_parent(id, parent, 1);

  int result = -1, hot = -1, need = 1;
  while (result < 0) {
    struct win_event ev;
    while (win_poll(id, &ev)) {
      need = 1;
      if (ev.type == WEV_CLOSE) {
        result = esc;
        break;
      } else if (ev.type == WEV_KEY) {
        if (ev.a == '\n')
          result = b[prim].result;
        else if (ev.a == 27)
          result = esc;
      } else if (ev.type == WEV_MOUSE) {
        int i = button_at(b, nb, W, H, ev.a, ev.b);
        if (i >= 0)
          result = b[i].result;
      } else if (ev.type == WEV_MOVE) {
        int i = ev.a < 0 ? -1 : button_at(b, nb, W, H, ev.a, ev.b);
        if (i != hot) {
          hot = i;
          need = 1;
        }
      }
    }
    if (result >= 0)
      break;
    if (need) {
      gfx_clear(DLG_BG);
      if (icon)
        draw_icon(icon, PAD, PAD);
      for (int i = 0; i < nl; i++)
        gfx_ui_text(textx, PAD + i * LINE_H + (LINE_H - UIFONT_H) / 2, lines[i], DLG_TEXT);
      int bx[4], bw[4], by;
      layout_buttons(b, nb, W, H, bx, bw, &by);
      for (int i = 0; i < nb; i++)
        draw_button(bx[i], by, bw[i], b[i].label, i == prim, i == hot);
      win_present(id);
      need = 0;
    }
    win_wait(id);
  }
  win_close(id);
  gfx_init(save_fb, save_w, save_h); /* hand the surface back to the caller */
  return result;
}

int dialog_message(int parent, const char *title, const char *message)
{
  return dialog_msg(parent, title, message, DLG_ICON_INFO | DLG_OK);
}

int dialog_confirm(int parent, const char *title, const char *message)
{
  return dialog_msg(parent, title, message, DLG_ICON_QUESTION | DLG_YESNO) == DLG_R_YES;
}

int dialog_choose(int parent, const char *title, const char *message, const char *const *labels, int nlabels,
                  unsigned iconflags)
{
  struct dbtn b[4];
  int nb = nlabels;
  if (nb < 1)
    nb = 1;
  if (nb > 4)
    nb = 4;
  for (int i = 0; i < nb; i++) {
    b[i].label = labels[i] ? labels[i] : "";
    b[i].result = i;
  }
  int prim = nb - 1; /* rightmost = affirmative default (Enter) */
  int icon = iconflags & DLG_ICON_MASK;

  /* Widen past MSG_W if the custom labels need a longer button row. */
  int W = MSG_W;
  int row = 2 * PAD;
  for (int i = 0; i < nb; i++) {
    int w = gfx_ui_width(b[i].label) + 28;
    if (w < BTN_MINW)
      w = BTN_MINW;
    row += w + (i ? BTN_GAP : 0);
  }
  if (row > W)
    W = row;

  int textx = PAD + (icon ? ICON_SZ + 12 : 0);
  int maxw = W - textx - PAD;
  char lines[MAXLINES][LINEBUF];
  int nl = wrap(message ? message : "", maxw, lines, MAXLINES);

  int body_h = nl * LINE_H;
  if (icon && ICON_SZ > body_h)
    body_h = ICON_SZ;
  int H = PAD + body_h + PAD + BTN_H + PAD;
  if (H < 120)
    H = 120;

  unsigned *save_fb;
  int save_w, save_h;
  gfx_get(&save_fb, &save_w, &save_h);

  int id = win_create(W, H, title ? title : "");
  if (id < 0)
    return -1;
  unsigned *fb = win_fb_of(id);
  if (!fb) {
    win_close(id);
    gfx_init(save_fb, save_w, save_h);
    return -1;
  }
  gfx_init(fb, W, H);
  win_resizable(id, 0);
  if (parent >= 0)
    win_set_parent(id, parent, 1);

  int result = -2, hot = -1, need = 1; /* -2 = still open; -1 = dismissed */
  while (result == -2) {
    struct win_event ev;
    while (win_poll(id, &ev)) {
      need = 1;
      if (ev.type == WEV_CLOSE) {
        result = -1;
        break;
      } else if (ev.type == WEV_KEY) {
        if (ev.a == '\n')
          result = b[prim].result;
        else if (ev.a == 27)
          result = -1;
      } else if (ev.type == WEV_MOUSE) {
        int i = button_at(b, nb, W, H, ev.a, ev.b);
        if (i >= 0)
          result = b[i].result;
      } else if (ev.type == WEV_MOVE) {
        int i = ev.a < 0 ? -1 : button_at(b, nb, W, H, ev.a, ev.b);
        if (i != hot) {
          hot = i;
          need = 1;
        }
      }
    }
    if (result != -2)
      break;
    if (need) {
      gfx_clear(DLG_BG);
      if (icon)
        draw_icon(icon, PAD, PAD);
      for (int i = 0; i < nl; i++)
        gfx_ui_text(textx, PAD + i * LINE_H + (LINE_H - UIFONT_H) / 2, lines[i], DLG_TEXT);
      int bx[4], bw[4], by;
      layout_buttons(b, nb, W, H, bx, bw, &by);
      for (int i = 0; i < nb; i++)
        draw_button(bx[i], by, bw[i], b[i].label, i == prim, i == hot);
      win_present(id);
      need = 0;
    }
    win_wait(id);
  }
  win_close(id);
  gfx_init(save_fb, save_w, save_h);
  return result;
}

/* ------------------------------------------------------------- text prompt */

#define PROMPT_W 360
#define INPUT_H 24

int dialog_prompt(int parent, const char *title, const char *message, const char *initial, char *out, int outsz)
{
  struct dbtn b[2];
  b[0].label = "Cancel";
  b[0].result = DLG_R_CANCEL;
  b[1].label = "OK";
  b[1].result = DLG_R_OK;

  int W = PROMPT_W;
  int maxw = W - 2 * PAD;
  char lines[MAXLINES][LINEBUF];
  int nl = (message && message[0]) ? wrap(message, maxw, lines, MAXLINES) : 0;

  char buf[512];
  int len = 0;
  if (initial)
    for (; initial[len] && len < (int)sizeof buf - 1; len++)
      buf[len] = initial[len];
  buf[len] = 0;

  int inputy = PAD + nl * LINE_H + (nl ? 6 : 0);
  int H = inputy + INPUT_H + PAD + BTN_H + PAD;
  if (H < 130)
    H = 130;

  unsigned *save_fb;
  int save_w, save_h;
  gfx_get(&save_fb, &save_w, &save_h);

  int id = win_create(W, H, title ? title : "");
  if (id < 0)
    return 0;
  unsigned *fb = win_fb_of(id);
  if (!fb) {
    win_close(id);
    gfx_init(save_fb, save_w, save_h);
    return 0;
  }
  gfx_init(fb, W, H);
  win_resizable(id, 0);
  if (parent >= 0)
    win_set_parent(id, parent, 1);

  int result = -1, hot = -1, need = 1;
  while (result < 0) {
    struct win_event ev;
    while (win_poll(id, &ev)) {
      need = 1;
      if (ev.type == WEV_CLOSE) {
        result = DLG_R_CANCEL;
        break;
      } else if (ev.type == WEV_KEY) {
        int c = ev.a;
        if (c == '\n')
          result = DLG_R_OK;
        else if (c == 27)
          result = DLG_R_CANCEL;
        else if (c == '\b') {
          if (len > 0)
            buf[--len] = 0;
        } else if (c >= 0x20 && c < 0x7f && len < (int)sizeof buf - 1) {
          buf[len++] = (char)c;
          buf[len] = 0;
        }
      } else if (ev.type == WEV_MOUSE) {
        int i = button_at(b, 2, W, H, ev.a, ev.b);
        if (i >= 0)
          result = b[i].result;
      } else if (ev.type == WEV_MOVE) {
        int i = ev.a < 0 ? -1 : button_at(b, 2, W, H, ev.a, ev.b);
        if (i != hot) {
          hot = i;
          need = 1;
        }
      }
    }
    if (result >= 0)
      break;
    if (need) {
      gfx_clear(DLG_BG);
      for (int i = 0; i < nl; i++)
        gfx_ui_text(PAD, PAD + i * LINE_H + (LINE_H - UIFONT_H) / 2, lines[i], DLG_TEXT);
      int iw = W - 2 * PAD;
      gfx_fill_rect(PAD, inputy, iw, INPUT_H, 0x0d1420);
      gfx_rect(PAD, inputy, iw, INPUT_H, 0x3a4658);
      /* Show the tail if the text overflows the box; caret sits at the end. */
      int avail = iw - 12;
      gfx_set_clip(PAD + 6, inputy, avail, INPUT_H);
      int tw = gfx_ui_width(buf);
      int tx = PAD + 6 + (tw > avail ? avail - tw : 0);
      gfx_ui_text(tx, inputy + (INPUT_H - UIFONT_H) / 2, buf, 0xffffff);
      gfx_fill_rect(tx + tw, inputy + 4, 1, INPUT_H - 8, 0x9ad0ff);
      gfx_clip_none();
      int bx[2], bw[2], by;
      layout_buttons(b, 2, W, H, bx, bw, &by);
      for (int i = 0; i < 2; i++)
        draw_button(bx[i], by, bw[i], b[i].label, i == 1, i == hot);
      win_present(id);
      need = 0;
    }
    win_wait(id);
  }
  win_close(id);
  gfx_init(save_fb, save_w, save_h);
  if (result == DLG_R_OK && out && outsz > 0) {
    int i = 0;
    for (; buf[i] && i < outsz - 1; i++)
      out[i] = buf[i];
    out[i] = 0;
    return 1;
  }
  return 0;
}
