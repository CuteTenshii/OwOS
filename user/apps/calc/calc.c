/* calc.c: a four-function desktop calculator with immediate-execution semantics. */
#include "fmt.h"
#include "gfx.h"
#include "owos.h"
#include <stdbool.h>

#define W 248
#define H 336
#define PAD 10
#define DISP_H 64
#define GAP 8
#define COLS 4
#define ROWS 5

#define BG 0x1b1f27u      /* window background          */
#define DISP_BG 0x10131au /* sunken display panel       */
#define DISP_FG 0xf2f5fau /* display digits             */
#define KEY_DIG 0x2c313cu /* digit / dot keys           */
#define KEY_FN 0x3a4150u  /* C / backspace / % / +/-    */
#define KEY_OP 0x2f6df0u  /* operator column            */
#define KEY_EQ 0x2f8f4fu  /* equals                     */
#define KEY_FG 0xeef1f6u  /* key label                  */
#define KEY_BD 0x12151cu  /* key border                 */
#define KEY_HL 0xc9d4e6u  /* hover outline              */

/* A keypad cell: a UTF-8 label and the single-byte action code the engine acts on. */
enum { ACT_CLEAR = 'C', ACT_BACK = 'B', ACT_NEG = 'N', ACT_PCT = '%', ACT_EQ = '=' };

static const struct {
  const char *label;
  unsigned char act;
  unsigned color;
} keys[COLS * ROWS] = {
    {"C", ACT_CLEAR, KEY_FN},      {"\xE2\x86\x90", ACT_BACK, KEY_FN},
    {"%", ACT_PCT, KEY_FN},        {"\xC3\xB7", '/', KEY_OP},
    {"7", '7', KEY_DIG},           {"8", '8', KEY_DIG},
    {"9", '9', KEY_DIG},           {"\xC3\x97", '*', KEY_OP},
    {"4", '4', KEY_DIG},           {"5", '5', KEY_DIG},
    {"6", '6', KEY_DIG},           {"\xE2\x88\x92", '-', KEY_OP},
    {"1", '1', KEY_DIG},           {"2", '2', KEY_DIG},
    {"3", '3', KEY_DIG},           {"+", '+', KEY_OP},
    {"\xC2\xB1", ACT_NEG, KEY_FN}, {"0", '0', KEY_DIG},
    {".", '.', KEY_DIG},           {"=", ACT_EQ, KEY_EQ},
};

/* Engine state: `buf` entry, `acc` left operand, `op` pending operator (0 = none), `fresh` = next digit starts a new
 * entry. */
static char buf[40] = "0";
static double acc = 0.0;
static char op = 0;
static int fresh = 1;
static bool error;

static int buf_len(void)
{
  int n = 0;
  while (buf[n])
    n++;
  return n;
}
static int buf_has_dot(void)
{
  for (int i = 0; buf[i]; i++)
    if (buf[i] == '.')
      return 1;
  return 0;
}

static void show(double v) { dtoa(v, buf); }

static void clear_all(void)
{
  buf[0] = '0';
  buf[1] = 0;
  acc = 0.0;
  op = 0;
  fresh = 1;
  error = false;
}

/* Fold the current entry into `acc` under the pending operator; clears `op`, flags divide-by-zero. */
static void fold(void)
{
  double v = strtod(buf, 0);
  if (op) {
    switch (op) {
    case '+':
      acc = acc + v;
      break;
    case '-':
      acc = acc - v;
      break;
    case '*':
      acc = acc * v;
      break;
    case '/':
      if (v == 0.0) {
        error = true;
        op = 0;
        return;
      }
      acc = acc / v;
      break;
    }
    op = 0;
  } else {
    acc = v;
  }
  show(acc);
}

static void input_digit(char d)
{
  if (fresh) {
    buf[0] = 0;
    fresh = 0;
  }
  int n = buf_len();
  if (n == 1 && buf[0] == '0') {
    buf[0] = d;
    return;
  } /* drop a lone leading zero */
  if (n < (int)sizeof(buf) - 1) {
    buf[n] = d;
    buf[n + 1] = 0;
  }
}

static void input_dot(void)
{
  if (fresh) {
    buf[0] = '0';
    buf[1] = 0;
    fresh = 0;
  }
  if (!buf_has_dot()) {
    int n = buf_len();
    buf[n] = '.';
    buf[n + 1] = 0;
  }
}

static void set_op(char newop)
{
  fold(); /* commit the entry / any prior op into acc */
  if (error)
    return;
  op = newop;
  fresh = 1;
}

static void backspace(void)
{
  if (fresh)
    return; /* can't edit a freshly shown result */
  int n = buf_len();
  if (n <= 1 || (n == 2 && buf[0] == '-')) {
    buf[0] = '0';
    buf[1] = 0;
    fresh = 1;
    return;
  }
  buf[n - 1] = 0;
}

static void negate(void)
{
  if (buf[0] == '0' && buf[1] == 0)
    return;
  if (buf[0] == '-') {
    int i = 0;
    do {
      buf[i] = buf[i + 1];
      i++;
    } while (buf[i - 1]);
  } else {
    int n = buf_len();
    for (int i = n; i >= 0; i--)
      buf[i + 1] = buf[i];
    buf[0] = '-';
  }
}

static void percent(void)
{
  show(strtod(buf, 0) / 100.0);
  fresh = 0;
}

/* Apply one action code (shared by the keypad and the keyboard). */
static void act(unsigned char a)
{
  if (error && a != ACT_CLEAR) { /* any key but C clears the error first */
    clear_all();
    if (a == ACT_EQ || a == '+' || a == '-' || a == '*' || a == '/')
      return;
  }
  if (a >= '0' && a <= '9') {
    input_digit((char)a);
    return;
  }
  switch (a) {
  case '.':
    input_dot();
    break;
  case '+':
  case '-':
  case '*':
  case '/':
    set_op((char)a);
    break;
  case ACT_EQ:
    fold();
    fresh = 1;
    break;
  case ACT_CLEAR:
    clear_all();
    break;
  case ACT_BACK:
    backspace();
    break;
  case ACT_NEG:
    negate();
    break;
  case ACT_PCT:
    percent();
    break;
  }
}

/* Map a physical-keyboard byte to an action code, or 0 if it isn't a calc key. */
static unsigned char key_to_act(char c)
{
  if (c >= '0' && c <= '9')
    return (unsigned char)c;
  switch (c) {
  case '.':
  case '+':
  case '-':
  case '*':
  case '/':
  case '%':
    return (unsigned char)c;
  case '=':
  case '\n':
  case '\r':
    return ACT_EQ;
  case '\b':
  case 0x7f:
    return ACT_BACK;
  case 'c':
  case 'C':
  case 27:
    return ACT_CLEAR;
  case 'n':
  case 'N':
    return ACT_NEG;
  }
  return 0;
}

/* Cell rectangle for keypad index i (row-major). */
static void cell_rect(int i, int *x, int *y, int *w, int *h)
{
  int gx = PAD, gy = PAD + DISP_H + PAD;
  int gw = W - 2 * PAD, gh = H - gy - PAD;
  int cw = (gw - (COLS - 1) * GAP) / COLS;
  int ch = (gh - (ROWS - 1) * GAP) / ROWS;
  int col = i % COLS, row = i / COLS;
  *x = gx + col * (cw + GAP);
  *y = gy + row * (ch + GAP);
  *w = cw;
  *h = ch;
}

static int cell_at(int mx, int my)
{
  for (int i = 0; i < COLS * ROWS; i++) {
    int x, y, w, h;
    cell_rect(i, &x, &y, &w, &h);
    if (mx >= x && mx < x + w && my >= y && my < y + h)
      return i;
  }
  return -1;
}

static void draw(int hover)
{
  gfx_clear(BG);

  /* Display panel: sunken box, the entry right-aligned, big sans face. */
  gfx_fill_rect(PAD, PAD, W - 2 * PAD, DISP_H, DISP_BG);
  gfx_rect(PAD, PAD, W - 2 * PAD, DISP_H, 0x000000u);
  const char *txt = error ? "Error" : buf;
  int px = 30;
  int tw = gfx_ui_width_px(txt, px);
  while (tw > W - 2 * PAD - 16 && px > 14) {
    px -= 2;
    tw = gfx_ui_width_px(txt, px);
  }
  int ty = PAD + (DISP_H - px) / 2;
  gfx_ui_text_px(W - PAD - 8 - tw, ty, txt, error ? 0xff6b6bu : DISP_FG, px);

  /* Keypad. */
  for (int i = 0; i < COLS * ROWS; i++) {
    int x, y, w, h;
    cell_rect(i, &x, &y, &w, &h);
    gfx_fill_rect(x, y, w, h, keys[i].color);
    gfx_rect(x, y, w, h, KEY_BD);
    if (i == hover)
      gfx_rect(x + 1, y + 1, w - 2, h - 2, KEY_HL);
    int lw = gfx_ui_width(keys[i].label);
    gfx_ui_text(x + (w - lw) / 2, y + (h - UIFONT_H) / 2, keys[i].label, KEY_FG);
  }
}

int main(void)
{
  int id = win_create(W, H, "calc");
  if (id < 0) {
    puts("calc: no window\n");
    return 1;
  }
  gfx_init(win_fb(), W, H);
  win_resizable(id, 0);

  int hover = -1;
  bool dirty = true;
  for (;;) {
    struct win_event ev;
    while (win_poll(id, &ev)) {
      switch (ev.type) {
      case WEV_CLOSE:
        win_close(id);
        return 0;
      case WEV_MOUSE: {
        int i = cell_at(ev.a, ev.b);
        if (i >= 0) {
          act(keys[i].act);
          dirty = true;
        }
        break;
      }
      case WEV_MOVE: {
        int i = cell_at(ev.a, ev.b);
        if (i != hover) {
          hover = i;
          dirty = true;
        }
        break;
      }
      case WEV_KEY: {
        unsigned char a = key_to_act((char)ev.a);
        if (a) {
          act(a);
          dirty = true;
        }
        break;
      }
      }
    }
    if (dirty) {
      draw(hover);
      win_present(id);
      dirty = false;
    }
    win_wait(id);
  }
}
