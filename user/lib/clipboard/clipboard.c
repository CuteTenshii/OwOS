/* clipboard.c — client side of the clipd protocol (see clipboard.h and
 * user/clipd.c). Every call opens its own short-lived connection to the
 * "clipboard" port, so callers never hold the single connection slot open. */
#include "clipboard.h"
#include "owos.h"

#define MIME_MAX 64

/* ---- framed I/O (read/write may transfer partial over the pipe rings) -----
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

static int rd_u32(int fd, unsigned *v)
{
  unsigned char b[4];
  if (!read_full(fd, b, 4))
    return 0;
  *v = (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
  return 1;
}

static int wr_u32(int fd, unsigned v)
{
  unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
  return write_full(fd, b, 4);
}

/* Read a length(1)-prefixed MIME string into out[max] (NUL-terminated). */
static int rd_mime(int fd, char *out, int max)
{
  unsigned char n;
  char tmp[MIME_MAX];
  if (!read_full(fd, &n, 1))
    return 0;
  if (n && !read_full(fd, tmp, n))
    return 0;
  int j = 0;
  for (; j < n && j < max - 1; j++)
    out[j] = tmp[j];
  out[j] = '\0';
  return 1;
}

static int wr_mime(int fd, const char *mime)
{
  int n = 0;
  while (mime[n] && n < MIME_MAX - 1)
    n++;
  unsigned char nb = (unsigned char)n;
  return write_full(fd, &nb, 1) && (n == 0 || write_full(fd, mime, n));
}

/* ---- put ----------------------------------------------------------------- */
int clip_put(const char *mime, const void *data, int len)
{
  int ch = port_open("clipboard", 0);
  if (ch < 0)
    return -1;
  int ok = write_full(ch, "P", 1) && wr_mime(ch, mime) && wr_u32(ch, (unsigned)len) &&
           (len == 0 || write_full(ch, data, len));
  char r = 0;
  if (ok)
    ok = read_full(ch, &r, 1) && r == 'K';
  close(ch);
  return ok ? 0 : -1;
}

int clip_put_text(const char *text, int len) { return clip_put("text/plain", text, len); }

/* ---- get ----------------------------------------------------------------- */
int clip_get(int idx, char *mime, int mimemax, void **out, int *len)
{
  int ch = port_open("clipboard", 0);
  if (ch < 0)
    return -1;
  char m[MIME_MAX];
  unsigned l = 0;
  int ok = write_full(ch, "G", 1) && wr_u32(ch, (unsigned)idx) && rd_mime(ch, m, sizeof m) && rd_u32(ch, &l);
  void *buf = 0;
  if (ok && m[0] && l > 0) {
    buf = malloc(l);
    if (!buf || !read_full(ch, buf, (int)l)) {
      free(buf);
      buf = 0;
      ok = 0;
    }
  }
  close(ch);
  if (!ok || !m[0]) {
    free(buf);
    return -1;
  }
  for (int i = 0; i < mimemax; i++) {
    mime[i] = m[i];
    if (!m[i])
      break;
  }
  mime[mimemax - 1] = '\0';
  *out = buf; /* 0 for a zero-length clip */
  *len = (int)l;
  return 0;
}

/* ---- count --------------------------------------------------------------- */
int clip_count(void)
{
  int ch = port_open("clipboard", 0);
  if (ch < 0)
    return 0;
  unsigned n = 0;
  int ok = write_full(ch, "N", 1) && rd_u32(ch, &n);
  close(ch);
  return ok ? (int)n : 0;
}

/* ---- list ---------------------------------------------------------------- */
int clip_list(int idx, char *mime, int mimemax, int *full_len, char **preview)
{
  int ch = port_open("clipboard", 0);
  if (ch < 0)
    return -1;
  char m[MIME_MAX];
  unsigned fl = 0;
  unsigned char pb[2] = {0, 0};
  int ok = write_full(ch, "L", 1) && wr_u32(ch, (unsigned)idx) && rd_mime(ch, m, sizeof m) && rd_u32(ch, &fl) &&
           read_full(ch, pb, 2);
  int plen = (int)pb[0] | ((int)pb[1] << 8);
  char *pv = 0;
  if (ok && m[0]) {
    pv = malloc(plen + 1);
    if (!pv || (plen && !read_full(ch, pv, plen))) {
      free(pv);
      pv = 0;
      ok = 0;
    } else
      pv[plen] = '\0';
  }
  close(ch);
  if (!ok || !m[0]) {
    free(pv);
    return -1;
  }
  for (int i = 0; i < mimemax; i++) {
    mime[i] = m[i];
    if (!m[i])
      break;
  }
  mime[mimemax - 1] = '\0';
  *full_len = (int)fl;
  *preview = pv;
  return 0;
}
