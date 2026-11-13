/* dialog.c — modal message-box + property-sheet dialogs. See dialog.h.
 *
 * Each dialog is a second window in the calling process: created transient +
 * modal on its parent, drawn with gfx into its own client buffer (win_fb_of),
 * and driven by a small blocking event loop. The compositor supplies the title
 * bar, border and close box; we draw only the client area (icon, text,
 * buttons).
 *
 * Keyboard: Tab / Shift+Tab walk the controls, Left/Right move along the
 * button row, Space or Enter presses the focused button, Esc cancels. Each
 * repaint records the dialog's accessibility nodes into a tree of its own.
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

/* The list dialog's rows plus the dialog, list and buttons */
#define DLG_A11Y_NODES 16

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
  int w = gfx_font_width(sym, &(struct gfx_font){.px = px});
  gfx_font_text(cx - w / 2, cy - px / 2, sym, 0xffffff, &(struct gfx_font){.px = px});
}

/* What the icon tells a sighted user, for the dialog's a11y node */
static const char *icon_kind(int type)
{
  switch (type) {
  case DLG_ICON_INFO:
    return "Information";
  case DLG_ICON_QUESTION:
    return "Question";
  case DLG_ICON_WARN:
    return "Warning";
  case DLG_ICON_ERROR:
    return "Error";
  default:
    return 0;
  }
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

static struct gfx_a11y dlg_nodes[DLG_A11Y_NODES];
static struct gfx_a11y_tree dlg_tree;

/* The caller's drawing surface and a11y tree, handed back by dlg_close */
struct dlg {
  int id;
  unsigned *save_fb;
  int save_w, save_h;
  struct gfx_a11y_tree *save_tree;
};

static int dlg_open(struct dlg *d, int parent, const char *title, int W, int H)
{
  gfx_get(&d->save_fb, &d->save_w, &d->save_h);
  d->id = win_create(W, H, title ? title : "");
  if (d->id < 0)
    return 0;
  unsigned *fb = win_fb_of(d->id);
  if (!fb) {
    win_close(d->id);
    return 0;
  }
  gfx_init(fb, W, H);
  win_resizable(d->id, 0);
  if (parent >= 0)
    win_set_parent(d->id, parent, 1);
  gfx_a11y_tree_init(&dlg_tree, dlg_nodes, DLG_A11Y_NODES);
  d->save_tree = gfx_a11y_bind(&dlg_tree);
  return 1;
}

static void dlg_close(struct dlg *d)
{
  win_close(d->id);
  gfx_init(d->save_fb, d->save_w, d->save_h);
  gfx_a11y_bind(d->save_tree);
}

/* Clear the client area and start this repaint's a11y nodes with the dialog itself */
static void dlg_frame(const char *title, const char *message, int icon, int W, int H)
{
  gfx_clear(DLG_BG);
  gfx_a11y_begin();
  struct gfx_a11y *a = gfx_a11y_add(GFX_ROLE_DIALOG, title, 0, 0, 0, W, H);
  gfx_a11y_set_text(a, icon_kind(icon));
  gfx_a11y_set_desc(a, message);
}

/* Tab order first, then Left/Right along the button row, which starts at focus entry first_btn and never wraps
 * into the input above it. Returns 1 if focus moved. */
static int dlg_nav(struct gfx_focus *f, int first_btn, int key, int mods)
{
  if (gfx_focus_key(f, key, mods))
    return 1;
  if (f->cur < first_btn || (key != KEY_LEFT && key != KEY_RIGHT))
    return 0;
  int to = f->cur + (key == KEY_RIGHT ? 1 : -1);
  if (to < first_btn || to >= f->n)
    return 0;
  f->cur = to;
  return 1;
}

static int is_enter(int key) { return key == '\n' || key == '\r'; }

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

static int button_w(const char *label)
{
  int w = gfx_ui_width(label) + 28;
  return w < BTN_MINW ? BTN_MINW : w;
}

/* Right-align the buttons in [0,W) with the primary (last) at the right edge.
 */
static void layout_buttons(struct dbtn *b, int n, int W, int H, int *bx, int *bw, int *by)
{
  *by = H - PAD - BTN_H;
  int x = W - PAD;
  for (int i = n - 1; i >= 0; i--) {
    bw[i] = button_w(b[i].label);
    x -= bw[i];
    bx[i] = x;
    x -= BTN_GAP;
  }
}

static void draw_button(int x, int y, int w, const char *label, int primary, int hot, int focused)
{
  unsigned st = (primary ? GFX_A11Y_DEFAULT : 0) | (hot ? GFX_A11Y_HOVER : 0) | (focused ? GFX_A11Y_FOCUSED : 0);
  gfx_a11y_add(GFX_ROLE_BUTTON, label, st, x, y, w, BTN_H);
  unsigned bg = primary ? (hot ? BTN_PRIMH : BTN_PRIM) : (hot ? BTN_HOT : BTN_NORM);
  if (focused)
    gfx_focus_ring(x, y, w, BTN_H, NULL);
  gfx_fill_rect(x, y, w, BTN_H, bg);
  gfx_rect(x, y, w, BTN_H, BORDER);
  int tx = x + (w - gfx_ui_width(label)) / 2;
  gfx_ui_text(tx, y + (BTN_H - UIFONT_H) / 2, label, primary ? 0xffffff : DLG_TEXT);
}

/* Draw the row; `focused` is the focused button's index, or -1 */
static void draw_buttons(struct dbtn *b, int n, int W, int H, int hot, int focused)
{
  int bx[4], bw[4], by;
  layout_buttons(b, n, W, H, bx, bw, &by);
  for (int i = 0; i < n; i++)
    draw_button(bx[i], by, bw[i], b[i].label, i == n - 1, i == hot, i == focused);
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

/* A message with 1..4 buttons; focus starts on the rightmost (primary) one. Returns the pressed button's result,
 * or `esc` for Esc, the close box, or a window that could not open. */
static int run_buttons(int parent, const char *title, const char *message, struct dbtn *b, int nb, int icon, int esc)
{
  int W = MSG_W;
  int row = 2 * PAD + (nb - 1) * BTN_GAP;
  for (int i = 0; i < nb; i++)
    row += button_w(b[i].label);
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

  struct dlg d;
  if (!dlg_open(&d, parent, title, W, H))
    return esc;
  struct gfx_focus focus;
  gfx_focus_init(&focus, nb, nb - 1, 0);

  int result = esc, done = 0, hot = -1, need = 1;
  while (!done) {
    struct win_event ev;
    while (!done && win_poll(d.id, &ev)) {
      need = 1;
      if (ev.type == WEV_CLOSE) {
        done = 1;
      } else if (ev.type == WEV_KEY) {
        if (ev.a == 27) {
          done = 1;
        } else if (button_key(ev.a)) {
          result = b[focus.cur].result;
          done = 1;
        } else {
          dlg_nav(&focus, 0, ev.a, ev.b);
        }
      } else if (ev.type == WEV_MOUSE) {
        int i = button_at(b, nb, W, H, ev.a, ev.b);
        if (i >= 0) {
          result = b[i].result;
          done = 1;
        }
      } else if (ev.type == WEV_MOVE) {
        hot = ev.a < 0 ? -1 : button_at(b, nb, W, H, ev.a, ev.b);
      }
    }
    if (done)
      break;
    if (need) {
      dlg_frame(title, message, icon, W, H);
      if (icon)
        draw_icon(icon, PAD, PAD);
      for (int i = 0; i < nl; i++)
        gfx_ui_text(textx, PAD + i * LINE_H + (LINE_H - UIFONT_H) / 2, lines[i], DLG_TEXT);
      draw_buttons(b, nb, W, H, hot, focus.cur);
      win_present(d.id);
      need = 0;
    }
    win_wait(d.id);
  }
  dlg_close(&d);
  return result;
}

int dialog_msg(int parent, const char *title, const char *message, unsigned flags)
{
  struct dbtn b[4];
  int nb = build_buttons(flags, b);
  return run_buttons(parent, title, message, b, nb, flags & DLG_ICON_MASK, b[0].result);
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
  return run_buttons(parent, title, message, b, nb, iconflags & DLG_ICON_MASK, -1);
}

/* ------------------------------------------------------------- text prompt */

#define PROMPT_W 360
#define INPUT_H 24

/* Focus entries of the prompt and list dialogs: the body, then Cancel and OK */
enum { FOCUS_BODY = 0, FOCUS_CANCEL, FOCUS_OK, FOCUS_COUNT };

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
  int iw = W - 2 * PAD;
  int H = inputy + INPUT_H + PAD + BTN_H + PAD;
  if (H < 130)
    H = 130;

  struct dlg d;
  if (!dlg_open(&d, parent, title, W, H))
    return 0;
  struct gfx_focus focus;
  gfx_focus_init(&focus, FOCUS_COUNT, FOCUS_BODY, 0);

  int result = -1, hot = -1, need = 1;
  while (result < 0) {
    struct win_event ev;
    while (result < 0 && win_poll(d.id, &ev)) {
      need = 1;
      if (ev.type == WEV_CLOSE) {
        result = DLG_R_CANCEL;
      } else if (ev.type == WEV_KEY) {
        int c = ev.a;
        int in_field = focus.cur == FOCUS_BODY;
        if (c == 27)
          result = DLG_R_CANCEL;
        else if (in_field && is_enter(c))
          result = DLG_R_OK;
        else if (!in_field && button_key(c))
          result = b[focus.cur - FOCUS_CANCEL].result;
        else if (dlg_nav(&focus, FOCUS_CANCEL, c, ev.b))
          ;
        else if (in_field && c == '\b') {
          if (len > 0)
            buf[--len] = 0;
        } else if (in_field && c >= 0x20 && c < 0x7f && len < (int)sizeof buf - 1) {
          buf[len++] = (char)c;
          buf[len] = 0;
        }
      } else if (ev.type == WEV_MOUSE) {
        int i = button_at(b, 2, W, H, ev.a, ev.b);
        if (i >= 0)
          result = b[i].result;
        else if (button_hit(PAD, inputy, iw, INPUT_H, ev.a, ev.b, NULL))
          focus.cur = FOCUS_BODY;
      } else if (ev.type == WEV_MOVE) {
        hot = ev.a < 0 ? -1 : button_at(b, 2, W, H, ev.a, ev.b);
      }
    }
    if (result >= 0)
      break;
    if (need) {
      int in_field = focus.cur == FOCUS_BODY;
      dlg_frame(title, message, DLG_ICON_NONE, W, H);
      for (int i = 0; i < nl; i++)
        gfx_ui_text(PAD, PAD + i * LINE_H + (LINE_H - UIFONT_H) / 2, lines[i], DLG_TEXT);
      struct gfx_a11y *field = gfx_a11y_add(GFX_ROLE_TEXTFIELD, nl ? message : title, in_field ? GFX_A11Y_FOCUSED : 0,
                                            PAD, inputy, iw, INPUT_H);
      gfx_a11y_set_text(field, buf);
      if (in_field)
        gfx_focus_ring(PAD, inputy, iw, INPUT_H, NULL);
      gfx_fill_rect(PAD, inputy, iw, INPUT_H, 0x0d1420);
      gfx_rect(PAD, inputy, iw, INPUT_H, 0x3a4658);
      /* Show the tail if the text overflows the box; caret sits at the end. */
      int avail = iw - 12;
      gfx_set_clip(PAD + 6, inputy, avail, INPUT_H);
      int tw = gfx_ui_width(buf);
      int tx = PAD + 6 + (tw > avail ? avail - tw : 0);
      gfx_ui_text(tx, inputy + (INPUT_H - UIFONT_H) / 2, buf, 0xffffff);
      if (in_field)
        gfx_fill_rect(tx + tw, inputy + 4, 1, INPUT_H - 8, 0x9ad0ff);
      gfx_clip_none();
      draw_buttons(b, 2, W, H, hot, in_field ? -1 : focus.cur - FOCUS_CANCEL);
      win_present(d.id);
      need = 0;
    }
    win_wait(d.id);
  }
  dlg_close(&d);
  if (result == DLG_R_OK && out && outsz > 0) {
    int i = 0;
    for (; buf[i] && i < outsz - 1; i++)
      out[i] = buf[i];
    out[i] = 0;
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------- pick-one list */

#define LIST_W 420
#define LIST_ROW_H 24
#define LIST_ROWS 8
#define LIST_BG 0x0d1420
#define LIST_SEL 0x2f6fb0

int dialog_list(int parent, const char *title, const char *message, const char *const *items, int nitems,
                const char *ok_label)
{
  struct dbtn b[2] = {{"Cancel", DLG_R_CANCEL}, {ok_label ? ok_label : "OK", DLG_R_OK}};
  int W = LIST_W;
  char lines[MAXLINES][LINEBUF];
  int nl = (message && message[0]) ? wrap(message, W - 2 * PAD, lines, MAXLINES) : 0;
  int rows = nitems < LIST_ROWS ? nitems : LIST_ROWS;
  if (rows < 1)
    rows = 1;
  int listy = PAD + nl * LINE_H + (nl ? 6 : 0);
  int lw = W - 2 * PAD;
  int H = listy + rows * LIST_ROW_H + PAD + BTN_H + PAD;

  struct dlg d;
  if (!dlg_open(&d, parent, title, W, H))
    return -1;
  const unsigned char skip[FOCUS_COUNT] = {[FOCUS_BODY] = nitems <= 0};
  struct gfx_focus focus;
  gfx_focus_init(&focus, FOCUS_COUNT, FOCUS_BODY, skip);

  int sel = nitems > 0 ? 0 : -1, top = 0, hot = -1, need = 1, result = -2;
  while (result == -2) {
    struct win_event ev;
    while (result == -2 && win_poll(d.id, &ev)) {
      need = 1;
      if (ev.type == WEV_CLOSE) {
        result = -1;
      } else if (ev.type == WEV_KEY) {
        int c = ev.a;
        if (c == 27)
          result = -1;
        else if (is_enter(c) || (c == ' ' && focus.cur != FOCUS_BODY))
          result = focus.cur == FOCUS_CANCEL ? -1 : sel;
        else if (dlg_nav(&focus, FOCUS_CANCEL, c, ev.b))
          ;
        else if (c == KEY_UP && sel > 0)
          sel--;
        else if (c == KEY_DOWN && sel + 1 < nitems)
          sel++;
        else if (c == KEY_HOME && nitems > 0)
          sel = 0;
        else if (c == KEY_END && nitems > 0)
          sel = nitems - 1;
      } else if (ev.type == WEV_SCROLL) {
        top -= ev.a > 0 ? 1 : -1;
      } else if (ev.type == WEV_MOUSE || ev.type == WEV_DBLCLICK) {
        int i = button_at(b, 2, W, H, ev.a, ev.b);
        int row = (ev.b - listy) / LIST_ROW_H;
        if (i >= 0)
          result = i == 0 ? -1 : sel;
        else if (ev.b >= listy && row < rows && top + row < nitems) {
          sel = top + row;
          focus.cur = FOCUS_BODY;
          if (ev.type == WEV_DBLCLICK)
            result = sel;
        }
      } else if (ev.type == WEV_MOVE) {
        hot = ev.a < 0 ? -1 : button_at(b, 2, W, H, ev.a, ev.b);
      }
    }
    if (sel >= 0 && sel < top)
      top = sel;
    if (sel >= top + rows)
      top = sel - rows + 1;
    if (top > nitems - rows)
      top = nitems - rows;
    if (top < 0)
      top = 0;
    if (result != -2)
      break;
    if (need) {
      int in_list = focus.cur == FOCUS_BODY;
      dlg_frame(title, message, DLG_ICON_NONE, W, H);
      for (int i = 0; i < nl; i++)
        gfx_ui_text(PAD, PAD + i * LINE_H + (LINE_H - UIFONT_H) / 2, lines[i], DLG_TEXT);
      gfx_a11y_add(GFX_ROLE_LIST, nl ? message : title, 0, PAD, listy, lw, rows * LIST_ROW_H);
      if (in_list)
        gfx_focus_ring(PAD, listy, lw, rows * LIST_ROW_H, NULL);
      gfx_fill_rect(PAD, listy, lw, rows * LIST_ROW_H, LIST_BG);
      gfx_rect(PAD, listy, lw, rows * LIST_ROW_H, 0x3a4658);
      for (int r = 0; r < rows && top + r < nitems; r++) {
        int y = listy + r * LIST_ROW_H, on = top + r == sel;
        unsigned st = (on ? GFX_A11Y_SELECTED : 0) | (on && in_list ? GFX_A11Y_FOCUSED : 0);
        gfx_a11y_set_value(gfx_a11y_add(GFX_ROLE_LISTITEM, items[top + r], st, PAD, y, lw, LIST_ROW_H), top + r + 1, 0,
                           nitems);
        if (on)
          gfx_fill_rect(PAD + 1, y, lw - 2, LIST_ROW_H, LIST_SEL);
        gfx_set_clip(PAD + 6, y, lw - 12, LIST_ROW_H);
        gfx_ui_text(PAD + 8, y + (LIST_ROW_H - UIFONT_H) / 2, items[top + r], on ? 0xffffff : DLG_TEXT);
        gfx_clip_none();
      }
      draw_buttons(b, 2, W, H, hot, in_list ? -1 : focus.cur - FOCUS_CANCEL);
      win_present(d.id);
      need = 0;
    }
    win_wait(d.id);
  }
  dlg_close(&d);
  return result;
}
