/* out.c: buffered terminal output and error messages */

#include "sh.h"

/* Buffered output on stdout; flushed before anything that blocks or hands the terminal to another process */
static char obuf[4096];

static int olen;

/* While redirecting, output collects here instead of going to the terminal */
static char *cap;

static unsigned cap_len, cap_size;

bool out_capturing(void) { return cap_size != 0; }

void out_capture_begin(void)
{
  oflush();
  cap_len = 0;
  cap_size = 1;
}

void out_capture_put(const char *p, unsigned n)
{
  if (cap_len + n + 1 > cap_size) {
    unsigned size = (cap_len + n + 1) * 2;
    char *grown = realloc(cap, size);
    if (!grown)
      return;
    cap = grown;
    cap_size = size;
  }
  memcpy(cap + cap_len, p, n);
  cap_len += n;
}

/* Stop capturing; the text stays valid until the next capture */
const char *out_capture_end(unsigned *len)
{
  *len = cap_len;
  cap_size = 0;
  return cap;
}

void oflush(void)
{
  if (out_capturing())
    return;
  for (int off = 0; off < olen;) {
    int n = write(1, obuf + off, olen - off);
    if (n <= 0)
      break;
    off += n;
  }
  olen = 0;
}

void tputc(char c)
{
  if (out_capturing()) {
    out_capture_put(&c, 1);
    return;
  }
  if (olen == (int)sizeof obuf)
    oflush();
  obuf[olen++] = c;
}

void tputs(const char *s)
{
  while (*s)
    tputc(*s++);
}

void tdec(unsigned v)
{
  char b[12];
  int n = 0;
  if (!v) {
    tputc('0');
    return;
  }
  while (v) {
    b[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n--)
    tputc(b[n]);
}

void tputs_c(const char *s, unsigned color)
{
  if (color) {
    tputs("\x1b[");
    tdec(color);
    tputc('m');
  }
  tputs(s);
  if (color)
    tputs("\x1b[0m");
}

void err(const char *a, const char *b)
{
  tputs_c(a, FG_ERR);
  if (b)
    tputs_c(b, FG_ERR);
  tputc('\n');
}

/* "cmd: <msg>path", with "permission denied: " in place of msg when rc is -EACCES */
void errc(int rc, const char *cmd, const char *msg, const char *path)
{
  char b[96];
  strcpy(b, cmd);
  strcat(b, ": ");
  strcat(b, rc == -EACCES ? "permission denied: " : msg);
  err(b, path);
}
