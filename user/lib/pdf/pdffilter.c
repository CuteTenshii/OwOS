/* pdffilter.c, PDF stream filters: Flate, LZW, ASCII85, ASCIIHex, RunLength and the PNG/TIFF predictors */
#include "inflate.h"
#include "owos.h"
#include "pdfint.h"

#define MAX_OUT (96 << 20)

struct buf {
  unsigned char *p;
  int n, cap;
  int full;
};

static int buf_put(struct buf *b, const unsigned char *s, int n)
{
  if (b->n + n > MAX_OUT) {
    b->full = 1;
    return -1;
  }
  if (b->n + n > b->cap) {
    int cap = b->cap ? b->cap : 4096;
    while (cap < b->n + n)
      cap *= 2;
    unsigned char *p = realloc(b->p, (unsigned)cap);
    if (!p) {
      b->full = 1;
      return -1;
    }
    b->p = p;
    b->cap = cap;
  }
  memcpy(b->p + b->n, s, (unsigned)n);
  b->n += n;
  return 0;
}

static int buf_byte(struct buf *b, unsigned char c) { return buf_put(b, &c, 1); }

struct src {
  const unsigned char *p;
  int n, pos;
};

static int src_get(void *c)
{
  struct src *s = c;
  return s->pos < s->n ? s->p[s->pos++] : -1;
}

static int buf_write(void *c, const unsigned char *s, unsigned n) { return buf_put(c, s, (int)n); }

static int is_ws(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }

static void flate(struct buf *out, const unsigned char *in, int n)
{
  struct src s = {in, n, 0};
  inflate_http(src_get, &s, buf_write, out);
}

static void lzw_run(struct buf *out, const unsigned char *in, int n, int early, unsigned short *prefix,
                    unsigned char *suffix, unsigned char *stack)
{
  int next = 258, bits = 9, prev = -1;
  unsigned acc = 0;
  int nacc = 0;
  for (int i = 0; i < n;) {
    while (nacc < bits && i < n) {
      acc = (acc << 8) | in[i++];
      nacc += 8;
    }
    if (nacc < bits)
      break;
    int code = (int)((acc >> (nacc - bits)) & ((1u << bits) - 1));
    nacc -= bits;
    if (code == 256) {
      next = 258;
      bits = 9;
      prev = -1;
      continue;
    }
    if (code == 257)
      break;
    int sp = 0, cur = code;
    if (code >= next) {
      if (prev < 0 || code > next)
        break;
      cur = prev;
    }
    while (cur >= 258 && sp < 4096) {
      stack[sp++] = suffix[cur];
      cur = prefix[cur];
    }
    stack[sp++] = (unsigned char)cur;
    unsigned char first = stack[sp - 1];
    for (int k = sp - 1; k >= 0; k--)
      if (buf_byte(out, stack[k]))
        return;
    if (code >= next && buf_byte(out, first))
      return;
    if (prev >= 0 && next < 4096) {
      prefix[next] = (unsigned short)prev;
      suffix[next] = first;
      next++;
    }
    prev = code;
    int lim = next + (early ? 1 : 0);
    bits = lim >= 2048 ? 12 : lim >= 1024 ? 11 : lim >= 512 ? 10 : 9;
  }
}

static void lzw(struct buf *out, const unsigned char *in, int n, int early)
{
  unsigned short *prefix = malloc(4096 * sizeof *prefix);
  unsigned char *suffix = malloc(4096 + 4097);
  if (prefix && suffix)
    lzw_run(out, in, n, early, prefix, suffix, suffix + 4096);
  free(prefix);
  free(suffix);
}

static void a85(struct buf *out, const unsigned char *in, int n)
{
  unsigned long long v = 0;
  int k = 0;
  for (int i = 0; i < n; i++) {
    int c = in[i];
    if (is_ws(c))
      continue;
    if (c == '~')
      break;
    if (c == 'z' && k == 0) {
      unsigned char z[4] = {0, 0, 0, 0};
      buf_put(out, z, 4);
      continue;
    }
    if (c < '!' || c > 'u')
      continue;
    v = v * 85 + (unsigned)(c - '!');
    if (++k == 5) {
      unsigned char q[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8),
                            (unsigned char)v};
      buf_put(out, q, 4);
      v = 0;
      k = 0;
    }
  }
  if (k > 1) {
    for (int j = k; j < 5; j++)
      v = v * 85 + 84;
    unsigned char q[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8),
                          (unsigned char)v};
    buf_put(out, q, k - 1);
  }
}

static void ahex(struct buf *out, const unsigned char *in, int n)
{
  int hi = -1;
  for (int i = 0; i < n && in[i] != '>'; i++) {
    int c = in[i], h = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
    if (h < 0)
      continue;
    if (hi < 0)
      hi = h;
    else {
      buf_byte(out, (unsigned char)(hi * 16 + h));
      hi = -1;
    }
  }
  if (hi >= 0)
    buf_byte(out, (unsigned char)(hi * 16));
}

static void runlen(struct buf *out, const unsigned char *in, int n)
{
  for (int i = 0; i < n;) {
    int l = in[i++];
    if (l == 128)
      break;
    if (l < 128) {
      int c = l + 1 < n - i ? l + 1 : n - i;
      buf_put(out, in + i, c);
      i += c;
    } else if (i < n) {
      for (int k = 0; k < 257 - l; k++)
        buf_byte(out, in[i]);
      i++;
    }
  }
}

static int paeth(int a, int b, int c)
{
  int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static void predict(struct buf *b, int pred, int colors, int bpc, int cols)
{
  if (colors < 1 || bpc < 1 || cols < 1 || colors > 64 || bpc > 16 || cols > (1 << 24))
    return;
  int bpp = (colors * bpc + 7) / 8;
  long row = ((long)colors * bpc * cols + 7) / 8;
  if (pred == 2) {
    if (bpc != 8)
      return;
    for (int y = 0; (long)(y + 1) * row <= b->n; y++) {
      unsigned char *r = b->p + (long)y * row;
      for (long x = bpp; x < row; x++)
        r[x] = (unsigned char)(r[x] + r[x - bpp]);
    }
    return;
  }
  if (pred < 10)
    return;
  long in_row = row + 1;
  int rows = (int)((b->n + in_row - 1) / in_row);
  unsigned char *prev = calloc(1, (unsigned)row), *o = b->p;
  if (!prev)
    return;
  for (int y = 0; y < rows; y++) {
    const unsigned char *r = b->p + (long)y * in_row;
    long avail = b->n - (long)y * in_row - 1;
    long cnt = avail < row ? (avail < 0 ? 0 : avail) : row;
    int ft = r[0];
    unsigned char *dst = o + (long)y * row;
    for (long x = 0; x < cnt; x++) {
      int a = x >= bpp ? dst[x - bpp] : 0, up = prev[x], c = x >= bpp ? prev[x - bpp] : 0, v = r[1 + x];
      switch (ft) {
      case 1:
        v += a;
        break;
      case 2:
        v += up;
        break;
      case 3:
        v += (a + up) / 2;
        break;
      case 4:
        v += paeth(a, up, c);
        break;
      }
      dst[x] = (unsigned char)v;
    }
    for (long x = cnt; x < row; x++)
      dst[x] = 0;
    memcpy(prev, dst, (unsigned)row);
  }
  free(prev);
  b->n = (int)((long)rows * row);
}

static int name_in(struct pobj nm, const char *a, const char *b)
{
  return nm.t == PO_NAME && (!strcmp(nm.u.s, a) || !strcmp(nm.u.s, b));
}

unsigned char *pdf_decode(struct pdf *d, const unsigned char *in, int n, struct pobj filter, struct pobj parms,
                          int *outlen, const char **left, struct pobj *left_parms)
{
  struct pobj fl[16];
  struct pobj pm[16];
  int nf = 0;
  if (left)
    *left = 0;
  if (filter.t == PO_NAME) {
    fl[nf] = filter;
    pm[nf++] = parms.t == PO_ARR ? pdf_at(d, parms, 0) : parms;
  } else if (filter.t == PO_ARR)
    for (int i = 0; i < filter.n && nf < 16; i++) {
      fl[nf] = pdf_at(d, filter, i);
      pm[nf++] = parms.t == PO_ARR ? pdf_at(d, parms, i) : (i == 0 ? parms : pdf_null());
    }
  unsigned char *cur = malloc((unsigned)n + 1);
  if (!cur)
    return 0;
  memcpy(cur, in, (unsigned)n);
  int len = n;
  for (int i = 0; i < nf; i++) {
    struct pobj f = fl[i];
    struct buf out = {0, 0, 0, 0};
    if (name_in(f, "FlateDecode", "Fl") || name_in(f, "LZWDecode", "LZW")) {
      int isflate = name_in(f, "FlateDecode", "Fl");
      if (isflate)
        flate(&out, cur, len);
      else
        lzw(&out, cur, len, pdf_is_dict(pm[i]) ? (int)pdf_num(pdf_get(d, pm[i], "EarlyChange"), 1) : 1);
      if (pdf_is_dict(pm[i])) {
        int pred = (int)pdf_num(pdf_get(d, pm[i], "Predictor"), 1);
        if (pred > 1)
          predict(&out, pred, (int)pdf_num(pdf_get(d, pm[i], "Colors"), 1),
                  (int)pdf_num(pdf_get(d, pm[i], "BitsPerComponent"), 8),
                  (int)pdf_num(pdf_get(d, pm[i], "Columns"), 1));
      }
    } else if (name_in(f, "ASCII85Decode", "A85"))
      a85(&out, cur, len);
    else if (name_in(f, "ASCIIHexDecode", "AHx"))
      ahex(&out, cur, len);
    else if (name_in(f, "RunLengthDecode", "RL"))
      runlen(&out, cur, len);
    else if (pdf_name_is(f, "Crypt"))
      continue;
    else {
      if (left)
        *left = f.t == PO_NAME ? f.u.s : "?";
      if (left_parms)
        *left_parms = pm[i];
      break;
    }
    free(cur);
    cur = out.p ? out.p : malloc(1);
    len = out.n;
    if (!cur)
      return 0;
  }
  *outlen = len;
  return cur;
}
