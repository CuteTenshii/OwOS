/* gif.c — GIF decoder with frame compositing. See gif.h.
 *
 * LZW is decoded with the usual prefix/suffix tables and an output stack; the code
 * table stops growing at 4096 entries and keeps decoding until the next clear code
 * (deferred clear). Every read is bounds-checked against the buffer, frame rectangles
 * are clipped to the logical screen, and decoding stops after fw*fh pixels, so hostile
 * input costs time linear in its size.
 */
#include "gif.h"
#include "owos.h"

#define GIF_MAX_DIM 16384

struct gif {
  const unsigned char *data;
  int len, pos, start;
  unsigned w, h;
  const unsigned char *gct; /* global palette, RGB triples */
  unsigned gct_n;
  int loops;
  int frames, done;

  int prev_disp;
  unsigned px, py, pw, ph; /* previous frame rectangle */
  unsigned char *backup;   /* canvas before a disposal-3 frame, allocated on first use */

  int blk;      /* bytes left in the current data sub-block */
  int lz_end;   /* the sub-block chain ended or ran off the buffer */
  unsigned acc; /* LZW bit accumulator */
  int nbits;
  unsigned short prefix[4096];
  unsigned char suffix[4096];
  unsigned char stack[4098];
};

/* Graphic control extension state for the next image descriptor. */
struct gce {
  int have, disp, delay_cs, tidx;
};

struct frame {
  unsigned left, top, fw, fh;
  const unsigned char *pal;
  unsigned npal;
  int interlace, tidx;
  unsigned x, row, remaining;
  int pass;
};

static int skip_blocks(struct gif *g)
{
  while (g->pos < g->len) {
    int n = g->data[g->pos++];
    if (!n)
      return 1;
    g->pos += n;
  }
  g->pos = g->len;
  return 0;
}

/* Extension body at g->pos (just after the label). Returns 0 if the buffer ended inside it. */
static int parse_extension(struct gif *g, int label, struct gce *gce)
{
  int first = 1, netscape = 0;
  while (g->pos < g->len) {
    int n = g->data[g->pos++];
    if (!n)
      return 1;
    if (n > g->len - g->pos) {
      g->pos = g->len;
      return 0;
    }
    const unsigned char *b = g->data + g->pos;
    if (label == 0xf9 && first && n >= 4) {
      gce->have = 1;
      gce->disp = (b[0] >> 2) & 7;
      gce->delay_cs = b[1] | (b[2] << 8);
      gce->tidx = (b[0] & 1) ? b[3] : -1;
    } else if (label == 0xff && first) {
      netscape = n == 11 && !memcmp(b, "NETSCAPE2.0", 11);
    } else if (label == 0xff && netscape && n >= 3 && b[0] == 1) {
      int count = b[1] | (b[2] << 8);
      g->loops = count ? count + 1 : 0;
    }
    first = 0;
    g->pos += n;
  }
  g->pos = g->len;
  return 0;
}

int gif_open(struct gif **out, const unsigned char *data, int len, unsigned *w, unsigned *h)
{
  *out = 0;
  if (len < 6 || memcmp(data, "GIF8", 4) || (data[4] != '7' && data[4] != '9') || data[5] != 'a')
    return GIF_EBADSIG;
  if (len < 13)
    return GIF_ETRUNC;
  unsigned sw = data[6] | (data[7] << 8), sh = data[8] | (data[9] << 8);
  if (!sw || !sh || sw > GIF_MAX_DIM || sh > GIF_MAX_DIM)
    return GIF_EFORMAT;
  int pos = 13;
  unsigned gct_n = 0;
  if (data[10] & 0x80) {
    gct_n = 2u << (data[10] & 7);
    if (len - pos < (int)(gct_n * 3))
      return GIF_ETRUNC;
  }
  struct gif *g = calloc(1, sizeof *g);
  if (!g)
    return GIF_ENOMEM;
  g->data = data;
  g->len = len;
  g->w = sw;
  g->h = sh;
  g->gct = data + pos;
  g->gct_n = gct_n;
  pos += (int)(gct_n * 3);
  g->start = g->pos = pos;
  g->loops = 1;

  /* The loop extension precedes the first frame: read it now so gif_loops is valid at once. */
  struct gce scratch = {0};
  while (g->pos < g->len && g->data[g->pos] == 0x21 && g->pos + 1 < g->len) {
    int label = g->data[g->pos + 1];
    g->pos += 2;
    if (!parse_extension(g, label, &scratch))
      break;
  }
  g->pos = g->start;
  *out = g;
  *w = sw;
  *h = sh;
  return GIF_OK;
}

/* Next LZW input byte, walking the data sub-block chain. */
static int lz_byte(struct gif *g)
{
  if (g->lz_end)
    return -1;
  if (g->blk == 0) {
    if (g->pos >= g->len || (g->blk = g->data[g->pos++]) == 0) {
      g->lz_end = 1;
      return -1;
    }
  }
  if (g->pos >= g->len) {
    g->lz_end = 1;
    return -1;
  }
  g->blk--;
  return g->data[g->pos++];
}

static int lz_bits(struct gif *g, int n)
{
  while (g->nbits < n) {
    int b = lz_byte(g);
    if (b < 0)
      return -1;
    g->acc |= (unsigned)b << g->nbits;
    g->nbits += 8;
  }
  int v = (int)(g->acc & ((1u << n) - 1));
  g->acc >>= n;
  g->nbits -= n;
  return v;
}

static void put_pixel(const struct gif *g, struct frame *f, unsigned char *canvas, unsigned idx)
{
  unsigned cx = f->left + f->x, cy = f->top + f->row;
  if (cx < g->w && cy < g->h && (int)idx != f->tidx) {
    unsigned char *p = canvas + (cy * g->w + cx) * 4;
    if (idx < f->npal) {
      p[0] = f->pal[idx * 3];
      p[1] = f->pal[idx * 3 + 1];
      p[2] = f->pal[idx * 3 + 2];
    } else {
      p[0] = p[1] = p[2] = 0;
    }
    p[3] = 255;
  }
  f->remaining--;
  if (++f->x < f->fw)
    return;
  f->x = 0;
  if (!f->interlace) {
    f->row++;
    return;
  }
  static const unsigned char step[4] = {8, 8, 4, 2}, first[4] = {0, 4, 2, 1};
  f->row += step[f->pass];
  while (f->row >= f->fh && f->pass < 3)
    f->row = first[++f->pass];
}

/* Decode one frame's LZW stream into the canvas. Stops quietly on corrupt or short data. */
static void decode_frame(struct gif *g, struct frame *f, unsigned char *canvas, int min_size)
{
  unsigned clear = 1u << min_size, eoi = clear + 1, next = clear + 2;
  int csize = min_size + 1, prev = -1, first = 0;
  g->blk = g->lz_end = g->nbits = 0;
  g->acc = 0;

  while (f->remaining) {
    int code = lz_bits(g, csize);
    if (code < 0 || (unsigned)code == eoi)
      break;
    if ((unsigned)code == clear) {
      next = clear + 2;
      csize = min_size + 1;
      prev = -1;
      continue;
    }
    if (prev < 0) {
      if ((unsigned)code >= clear)
        break;
      first = code;
      put_pixel(g, f, canvas, (unsigned)code);
      prev = code;
      if (next >= (1u << csize))
        csize++; /* only min_size 1: entry 4 already needs 3 bits */
      continue;
    }
    if ((unsigned)code > next)
      break;
    int sp = 0, c = code;
    if ((unsigned)code == next) {
      g->stack[sp++] = (unsigned char)first;
      c = prev;
    }
    while ((unsigned)c >= clear) {
      g->stack[sp++] = g->suffix[c];
      c = g->prefix[c];
    }
    first = c;
    g->stack[sp++] = (unsigned char)c;
    while (sp && f->remaining)
      put_pixel(g, f, canvas, g->stack[--sp]);
    if (next < 4096) {
      g->prefix[next] = (unsigned short)prev;
      g->suffix[next] = (unsigned char)first;
      next++;
      if (next >= (1u << csize) && csize < 12)
        csize++;
    }
    prev = code;
  }
  if (!g->lz_end) {
    g->pos += g->blk;
    if (g->pos > g->len)
      g->pos = g->len;
    skip_blocks(g);
  }
}

/* Copy the rows of rect (clipped to the screen) between canvas and backup. */
static void copy_rect(const struct gif *g, unsigned char *dst, const unsigned char *src, unsigned x, unsigned y,
                      unsigned w, unsigned h)
{
  if (x >= g->w || y >= g->h)
    return;
  if (w > g->w - x)
    w = g->w - x;
  if (h > g->h - y)
    h = g->h - y;
  for (unsigned r = 0; r < h; r++) {
    unsigned off = ((y + r) * g->w + x) * 4;
    memcpy(dst + off, src + off, w * 4);
  }
}

static void clear_rect(const struct gif *g, unsigned char *canvas, unsigned x, unsigned y, unsigned w, unsigned h)
{
  if (x >= g->w || y >= g->h)
    return;
  if (w > g->w - x)
    w = g->w - x;
  if (h > g->h - y)
    h = g->h - y;
  for (unsigned r = 0; r < h; r++)
    memset(canvas + ((y + r) * g->w + x) * 4, 0, w * 4);
}

int gif_next(struct gif *g, unsigned char *canvas, int *delay_ms)
{
  struct gce gce = {0, 0, 0, -1};
  if (delay_ms)
    *delay_ms = 0;
  if (g->done)
    return GIF_END;

  for (;;) {
    if (g->pos >= g->len) {
      g->done = 1;
      return g->frames ? GIF_END : GIF_ETRUNC;
    }
    int c = g->data[g->pos++];
    if (c == 0x3b) {
      g->done = 1;
      return g->frames ? GIF_END : GIF_EFORMAT;
    }
    if (c == 0x21) {
      if (g->pos >= g->len) {
        g->done = 1;
        return g->frames ? GIF_END : GIF_ETRUNC;
      }
      int label = g->data[g->pos++];
      parse_extension(g, label, &gce);
      continue;
    }
    if (c != 0x2c) {
      g->done = 1;
      return GIF_EFORMAT;
    }
    break;
  }

  if (g->len - g->pos < 9) {
    g->done = 1;
    g->pos = g->len;
    return g->frames ? GIF_END : GIF_ETRUNC;
  }
  const unsigned char *d = g->data + g->pos;
  struct frame f;
  f.left = d[0] | (d[1] << 8);
  f.top = d[2] | (d[3] << 8);
  f.fw = d[4] | (d[5] << 8);
  f.fh = d[6] | (d[7] << 8);
  f.interlace = (d[8] >> 6) & 1;
  f.pal = g->gct;
  f.npal = g->gct_n;
  g->pos += 9;
  if (d[8] & 0x80) {
    unsigned n = 2u << (d[8] & 7);
    if (g->len - g->pos < (int)(n * 3)) {
      g->done = 1;
      g->pos = g->len;
      return g->frames ? GIF_END : GIF_ETRUNC;
    }
    f.pal = g->data + g->pos;
    f.npal = n;
    g->pos += (int)(n * 3);
  }
  if (g->pos >= g->len) {
    g->done = 1;
    return g->frames ? GIF_END : GIF_ETRUNC;
  }
  int min_size = g->data[g->pos++];
  if (min_size < 1 || min_size > 8) {
    g->done = 1;
    return GIF_EFORMAT;
  }
  f.tidx = gce.tidx;
  f.x = f.row = 0;
  f.pass = 0;
  f.remaining = f.fw * f.fh;

  if (g->prev_disp == 2)
    clear_rect(g, canvas, g->px, g->py, g->pw, g->ph);
  else if (g->prev_disp == 3 && g->backup)
    copy_rect(g, canvas, g->backup, g->px, g->py, g->pw, g->ph);

  int disp = gce.disp;
  if (disp == 3) {
    if (!g->backup)
      g->backup = malloc(g->w * g->h * 4);
    if (g->backup)
      copy_rect(g, g->backup, canvas, f.left, f.top, f.fw, f.fh);
    else
      disp = 0;
  }

  decode_frame(g, &f, canvas, min_size);

  g->prev_disp = disp;
  g->px = f.left;
  g->py = f.top;
  g->pw = f.fw;
  g->ph = f.fh;
  g->frames++;
  if (delay_ms)
    *delay_ms = gce.delay_cs * 10;
  return GIF_FRAME;
}

void gif_rewind(struct gif *g, unsigned char *canvas)
{
  memset(canvas, 0, g->w * g->h * 4);
  g->pos = g->start;
  g->frames = g->done = g->prev_disp = 0;
}

int gif_loops(const struct gif *g) { return g->loops; }

void gif_close(struct gif *g)
{
  if (!g)
    return;
  free(g->backup);
  free(g);
}
