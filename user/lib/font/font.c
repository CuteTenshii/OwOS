/* font.c — TrueType/OpenType (glyf) parser + AA glyph rasterizer. See font.h.
 *
 * SFNT is big-endian. We parse the table directory, pick a usable cmap, and
 * read head/maxp/hhea/hmtx/loca/glyf. A glyph's quadratic outline is flattened
 * to polygons (in font units, with composite-glyph transforms applied), then
 * scaled and scanline-filled (nonzero winding) at SS× resolution and
 * box-downsampled to an 8-bit coverage bitmap. Floats are fine in user space
 * (x87). */
#include "font.h"
#include "owos.h"

/* ------------------------------------------------ big-endian readers -------
 */
static unsigned rd16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static int rds16(const unsigned char *p)
{
  int v = (int)rd16(p);
  return v >= 0x8000 ? v - 0x10000 : v;
}
static unsigned rd32(const unsigned char *p)
{
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

struct font {
  const unsigned char *d;
  int len;
  unsigned char *owned; /* SFNT decoded from a WOFF container (freed here); else 0 */
  int num_glyphs, upem, loc_long;
  int n_hmetrics;
  int asc, desc, lgap;                         /* font units */
  unsigned head, maxp, hhea, hmtx, loca, glyf; /* table offsets (0 = absent) */
  unsigned cmap_sub;
  int cmap_fmt; /* chosen cmap subtable        */
  unsigned cff; /* CFF table offset; 0 = TrueType outlines */
  unsigned charstrings, gsubrs, lsubrs, fdselect;
  unsigned *fd_subrs; /* per-Font-DICT local subrs of a CID-keyed CFF */
  int n_fd;
};

/* WOFF/WOFF2 → SFNT decoder (user/lib/font/woff.c). Weak so a program that only
 * renders local SFNTs need not link it (and thus drag in brotli); when present,
 * font_load transparently decodes web-font containers. */
extern int woff_is_woff(const unsigned char *, int) __attribute__((weak));
extern unsigned char *woff_to_sfnt(const unsigned char *, int, int *) __attribute__((weak));

static unsigned find_table(const unsigned char *d, int len, const char *tag)
{
  if (len < 12)
    return 0;
  int n = (int)rd16(d + 4);
  for (int i = 0; i < n; i++) {
    const unsigned char *e = d + 12 + i * 16;
    if (e + 16 > d + len)
      break;
    if (e[0] == (unsigned char)tag[0] && e[1] == (unsigned char)tag[1] && e[2] == (unsigned char)tag[2] &&
        e[3] == (unsigned char)tag[3])
      return rd32(e + 8);
  }
  return 0;
}

/* True if reading `need` bytes starting at table offset `off` stays inside the
 * SFNT. All table offsets come straight from the (untrusted) directory, so
 * every fixed-field read below is gated through this. */
static int off_ok(unsigned off, int need, int len)
{
  return off <= (unsigned)len && (unsigned)len - off >= (unsigned)need;
}

/* Pick the most useful cmap subtable: Windows-Unicode-full (3,10), Windows-BMP
 * (3,1), Unicode (0,*), then symbol (3,0). Stores its offset + format. */
static void pick_cmap(struct font *f)
{
  unsigned cmap = find_table(f->d, f->len, "cmap");
  if (!cmap || cmap + 4 > (unsigned)f->len)
    return;
  const unsigned char *c = f->d + cmap;
  int n = (int)rd16(c + 2);
  int best = -1;
  unsigned best_off = 0;
  for (int i = 0; i < n; i++) {
    const unsigned char *e = c + 4 + i * 8;
    if (e + 8 > f->d + f->len)
      break;
    int plat = (int)rd16(e), enc = (int)rd16(e + 2);
    unsigned off = cmap + rd32(e + 4);
    int score = -1;
    if (plat == 3 && enc == 10)
      score = 4;
    else if (plat == 3 && enc == 1)
      score = 3;
    else if (plat == 0)
      score = 2;
    else if (plat == 3 && enc == 0)
      score = 1;
    if (score > best) {
      best = score;
      best_off = off;
    }
  }
  if (best < 0 || best_off + 2 > (unsigned)f->len)
    return;
  f->cmap_sub = best_off;
  f->cmap_fmt = (int)rd16(f->d + best_off);
}

static int cff_init(struct font *f);

struct font *font_load(const unsigned char *data, int len)
{
  if (!data || len < 12)
    return 0;

  /* Transparently decode a WOFF/WOFF2 web-font container to a raw SFNT, which
   * the font then owns (freed in font_free). The original `data` is untouched
   * and remains the caller's. */
  unsigned char *owned = 0;
  if (woff_is_woff && woff_to_sfnt && woff_is_woff(data, len)) {
    int sl = 0;
    owned = woff_to_sfnt(data, len, &sl);
    if (!owned)
      return 0;
    data = owned;
    len = sl;
  }

  unsigned ver = rd32(data);
  /* 0x00010000 = TrueType, 'true'/'typ1' = TT, 'OTTO' = CFF, 'ttcf' = collection (unsupported). */
  if (ver != 0x00010000 && ver != 0x74727565 /*true*/ && ver != 0x74797031 /*typ1*/ && ver != 0x4f54544f /*OTTO*/) {
    free(owned);
    return 0;
  }

  struct font *f = malloc(sizeof *f);
  if (!f) {
    free(owned);
    return 0;
  }
  for (unsigned i = 0; i < sizeof *f; i++)
    ((char *)f)[i] = 0;
  f->owned = owned;
  f->d = data;
  f->len = len;

  f->head = find_table(data, len, "head");
  f->maxp = find_table(data, len, "maxp");
  f->hhea = find_table(data, len, "hhea");
  f->hmtx = find_table(data, len, "hmtx");
  f->loca = find_table(data, len, "loca");
  f->glyf = find_table(data, len, "glyf");
  f->cff = ver == 0x4f54544f ? find_table(data, len, "CFF ") : 0;
  if (!f->head || !f->maxp || !f->hhea || !f->hmtx || (f->cff ? 0 : (!f->loca || !f->glyf))) {
    free(f->owned);
    free(f);
    return 0;
  }

  /* The directory offsets are untrusted: reject any that puts a fixed field we
   * read below (or a table base) outside the file before dereferencing. */
  if (!off_ok(f->head, 54, len) || !off_ok(f->maxp, 6, len) || !off_ok(f->hhea, 36, len) || f->hmtx > (unsigned)len ||
      f->loca > (unsigned)len || f->glyf > (unsigned)len) {
    free(f->owned);
    free(f);
    return 0;
  }

  f->upem = (int)rd16(data + f->head + 18);
  f->loc_long = rds16(data + f->head + 50);
  if (f->upem <= 0)
    f->upem = 1000;
  f->num_glyphs = (int)rd16(data + f->maxp + 4);
  f->asc = rds16(data + f->hhea + 4);
  f->desc = rds16(data + f->hhea + 6);
  f->lgap = rds16(data + f->hhea + 8);
  f->n_hmetrics = (int)rd16(data + f->hhea + 34);

  if (ver == 0x4f54544f && (!f->cff || !cff_init(f))) {
    font_free(f);
    return 0;
  }

  pick_cmap(f);
  return f;
}

void font_free(struct font *f)
{
  if (f) {
    free(f->owned);
    free(f->fd_subrs);
    free(f);
  }
}

/* ------------------------------------------------ cmap lookup --------------
 */
int font_glyph_index(const struct font *f, int cp)
{
  if (!f || !f->cmap_sub || cp < 0)
    return 0;
  const unsigned char *t = f->d + f->cmap_sub;
  const unsigned char *end = f->d + f->len;

  if (f->cmap_fmt == 4) {
    if (t + 14 > end)
      return 0;
    int segX2 = (int)rd16(t + 6);
    int segc = segX2 / 2;
    const unsigned char *endC = t + 14;
    const unsigned char *startC = endC + segX2 + 2;
    const unsigned char *idD = startC + segX2;
    const unsigned char *idR = idD + segX2;
    if (segc <= 0 || idR + segX2 > end)
      return 0; /* all four segment arrays must fit */
    for (int s = 0; s < segc; s++) {
      int end = (int)rd16(endC + s * 2);
      if (cp > end)
        continue;
      int start = (int)rd16(startC + s * 2);
      if (cp < start)
        return 0;
      int delta = rds16(idD + s * 2);
      int ro = (int)rd16(idR + s * 2);
      if (ro == 0)
        return (cp + delta) & 0xffff;
      const unsigned char *gp = idR + s * 2 + ro + (cp - start) * 2;
      if (gp + 2 > f->d + f->len)
        return 0;
      int g = (int)rd16(gp);
      return g ? (g + delta) & 0xffff : 0;
    }
    return 0;
  }
  if (f->cmap_fmt == 12) {
    if (t + 16 > end)
      return 0;
    unsigned ng = rd32(t + 12);
    const unsigned char *g = t + 16;
    for (unsigned i = 0; i < ng; i++, g += 12) {
      if (g + 12 > end)
        return 0;
      unsigned s = rd32(g), e = rd32(g + 4);
      if ((unsigned)cp < s)
        return 0;
      if ((unsigned)cp <= e)
        return rd32(g + 8) + ((unsigned)cp - s);
    }
    return 0;
  }
  if (f->cmap_fmt == 6) {
    if (t + 10 > end)
      return 0;
    int first = (int)rd16(t + 6), count = (int)rd16(t + 8);
    if (cp < first || cp >= first + count)
      return 0;
    if (t + 10 + (cp - first) * 2 + 2 > end)
      return 0;
    return (int)rd16(t + 10 + (cp - first) * 2);
  }
  if (f->cmap_fmt == 0) {
    if (cp < 0 || cp > 255)
      return 0;
    if (t + 6 + cp + 1 > end)
      return 0;
    return t[6 + cp];
  }
  return 0;
}

/* ------------------------------------------------ horizontal metrics -------
 */
static int glyph_advance_units(const struct font *f, int gi)
{
  if (gi < 0)
    return 0;
  int idx = gi < f->n_hmetrics ? gi : f->n_hmetrics - 1;
  if (idx < 0)
    return 0;
  if (!off_ok(f->hmtx, idx * 4 + 2, f->len))
    return 0;
  return (int)rd16(f->d + f->hmtx + idx * 4);
}

void font_glyph_hmetrics(const struct font *f, int gi, float px, int *advance, int *lsb)
{
  float s = px / f->upem;
  if (advance)
    *advance = (int)(glyph_advance_units(f, gi) * s + 0.5f);
  if (lsb) {
    int idx = gi < f->n_hmetrics ? gi : -1;
    *lsb = (idx >= 0 && off_ok(f->hmtx, idx * 4 + 4, f->len)) ? (int)(rds16(f->d + f->hmtx + idx * 4 + 2) * s) : 0;
  }
}

void font_vmetrics(const struct font *f, float px, int *a, int *d, int *l)
{
  float s = px / f->upem;
  if (a)
    *a = (int)(f->asc * s + 0.5f);
  if (d)
    *d = (int)(f->desc * s - 0.5f);
  if (l)
    *l = (int)(f->lgap * s + 0.5f);
}

/* ------------------------------------------------ outline -> polygons ------
 */
/* Contour points accumulate here in font units (composite transforms applied).
 */
struct ras {
  float *px, *py;
  int np, cap;
  int *sub;
  int ns, scap; /* contour start indices */
  int oom;
};

static void ras_pt(struct ras *r, float x, float y)
{
  if (r->np >= r->cap) {
    int nc = r->cap ? r->cap * 2 : 256;
    float *nx = realloc(r->px, (unsigned)nc * sizeof(float));
    float *ny = realloc(r->py, (unsigned)nc * sizeof(float));
    if (!nx || !ny) {
      if (nx)
        r->px = nx;
      if (ny)
        r->py = ny;
      r->oom = 1;
      return;
    }
    r->px = nx;
    r->py = ny;
    r->cap = nc;
  }
  r->px[r->np] = x;
  r->py[r->np] = y;
  r->np++;
}

static void ras_begin(struct ras *r)
{
  if (r->ns >= r->scap) {
    int nc = r->scap ? r->scap * 2 : 32;
    int *ns = realloc(r->sub, (unsigned)nc * sizeof(int));
    if (!ns) {
      r->oom = 1;
      return;
    }
    r->sub = ns;
    r->scap = nc;
  }
  r->sub[r->ns++] = r->np;
}

/* An affine in font units (for composite components): x' = a*x + c*y + e. */
struct xf {
  float a, b, c, d, e, f;
};
static void xf_apply(const struct xf *m, float x, float y, float *ox, float *oy)
{
  *ox = m->a * x + m->c * y + m->e;
  *oy = m->b * x + m->d * y + m->f;
}

/* Flatten a quadratic from on-curve p0 through control c to on-curve p1. */
static void quad(struct ras *r, const struct xf *m, float x0, float y0, float cx, float cy, float x1, float y1)
{
  const int N = 10;
  for (int i = 1; i <= N; i++) {
    float t = (float)i / N, u = 1 - t;
    float bx = u * u * x0 + 2 * u * t * cx + t * t * x1;
    float by = u * u * y0 + 2 * u * t * cy + t * t * y1;
    float dx, dy;
    xf_apply(m, bx, by, &dx, &dy);
    ras_pt(r, dx, dy);
  }
}

static void load_glyph(const struct font *f, int gi, struct ras *r, struct xf m, int depth);

/* Parse a simple glyph at glyf offset `g` (length `glen`) into r, transform m.
 */
static void load_simple(const struct font *f, const unsigned char *g, int glen, struct ras *r, struct xf m)
{
  int nc = rds16(g);
  if (nc <= 0 || 10 + nc * 2 > glen)
    return;
  const unsigned char *p = g + 10;
  int npts = (int)rd16(p + (nc - 1) * 2) + 1;
  if (npts <= 0 || npts > 20000)
    return;
  const int *endp = 0;
  (void)endp;
  int ends[256];
  int *ce = ends;
  int *ce_heap = 0;
  if (nc > 256) {
    ce_heap = malloc((unsigned)nc * sizeof(int));
    if (!ce_heap)
      return;
    ce = ce_heap;
  }
  for (int i = 0; i < nc; i++)
    ce[i] = (int)rd16(p + i * 2);
  p += nc * 2;
  int insLen = (p + 2 <= f->d + f->len) ? (int)rd16(p) : 0;
  p += 2 + insLen;

  unsigned char *flags = malloc((unsigned)npts);
  float *xs = malloc((unsigned)npts * sizeof(float));
  float *ys = malloc((unsigned)npts * sizeof(float));
  if (!flags || !xs || !ys) {
    free(flags);
    free(xs);
    free(ys);
    free(ce_heap);
    return;
  }

  const unsigned char *end = f->d + f->len;
  for (int i = 0; i < npts;) {
    if (p >= end) {
      npts = i;
      break;
    }
    unsigned char fl = *p++;
    flags[i++] = fl;
    if (fl & 0x08) { /* REPEAT */
      int rep = (p < end) ? *p++ : 0;
      while (rep-- > 0 && i < npts)
        flags[i++] = fl;
    }
  }
  int x = 0;
  for (int i = 0; i < npts; i++) {
    unsigned char fl = flags[i];
    if (fl & 0x02) {
      int dx = (p < end) ? *p++ : 0;
      x += (fl & 0x10) ? dx : -dx;
    } else if (!(fl & 0x10)) {
      if (p + 2 <= end) {
        x += rds16(p);
      }
      p += 2;
    }
    xs[i] = (float)x;
  }
  int y = 0;
  for (int i = 0; i < npts; i++) {
    unsigned char fl = flags[i];
    if (fl & 0x04) {
      int dy = (p < end) ? *p++ : 0;
      y += (fl & 0x20) ? dy : -dy;
    } else if (!(fl & 0x20)) {
      if (p + 2 <= end) {
        y += rds16(p);
      }
      p += 2;
    }
    ys[i] = (float)y;
  }

  /* Walk each contour, flattening quadratics (on/off-curve points). */
  int s = 0;
  for (int c = 0; c < nc; c++) {
    int e = ce[c];
    if (e < s || e >= npts) {
      s = e + 1;
      continue;
    }
    int n = e - s + 1;
    if (n < 2) {
      s = e + 1;
      continue;
    }

    /* find a starting on-curve point (or synthesize one) */
    float sx, sy;
    int start_on = -1;
    for (int i = 0; i < n; i++)
      if (flags[s + i] & 1) {
        start_on = i;
        break;
      }
    if (start_on >= 0) {
      sx = xs[s + start_on];
      sy = ys[s + start_on];
    } else { /* all off-curve: start at midpoint */
      sx = (xs[s] + xs[e]) * 0.5f;
      sy = (ys[s] + ys[e]) * 0.5f;
      start_on = 0;
    }

    ras_begin(r);
    {
      float dx, dy;
      xf_apply(&m, sx, sy, &dx, &dy);
      ras_pt(r, dx, dy);
    }

    float curx = sx, cury = sy;
    float cxp = 0, cyp = 0;
    int have_ctrl = 0;
    for (int k = 1; k <= n; k++) {
      int idx = s + (start_on + k) % n;
      int on = flags[idx] & 1;
      float px = xs[idx], py = ys[idx];
      if (on) {
        if (have_ctrl) {
          quad(r, &m, curx, cury, cxp, cyp, px, py);
          have_ctrl = 0;
        } else {
          float dx, dy;
          xf_apply(&m, px, py, &dx, &dy);
          ras_pt(r, dx, dy);
        }
        curx = px;
        cury = py;
      } else {
        if (have_ctrl) { /* two off-curve: implied on-curve midpoint */
          float mx = (cxp + px) * 0.5f, my = (cyp + py) * 0.5f;
          quad(r, &m, curx, cury, cxp, cyp, mx, my);
          curx = mx;
          cury = my;
        }
        cxp = px;
        cyp = py;
        have_ctrl = 1;
      }
    }
    s = e + 1;
  }

  free(flags);
  free(xs);
  free(ys);
  free(ce_heap);
}

/* Composite glyph: each component references another glyph with a transform. */
static void load_composite(const struct font *f, const unsigned char *g, int glen, struct ras *r, struct xf m,
                           int depth)
{
  const unsigned char *p = g + 10;
  const unsigned char *end = g + glen;
  for (;;) {
    if (p + 4 > end)
      break;
    int flags = (int)rd16(p);
    int cgi = (int)rd16(p + 2);
    p += 4;
    float dx, dy;
    if (flags & 0x0001) { /* ARG_1_AND_2_ARE_WORDS */
      if (p + 4 > end)
        break;
      dx = (float)rds16(p);
      dy = (float)rds16(p + 2);
      p += 4;
    } else {
      if (p + 2 > end)
        break;
      dx = (float)(signed char)p[0];
      dy = (float)(signed char)p[1];
      p += 2;
    }
    float a = 1, b = 0, c = 0, d = 1;
    if (flags & 0x0008) { /* WE_HAVE_A_SCALE */
      if (p + 2 > end)
        break;
      a = d = rds16(p) / 16384.0f;
      p += 2;
    } else if (flags & 0x0040) { /* X_AND_Y_SCALE */
      if (p + 4 > end)
        break;
      a = rds16(p) / 16384.0f;
      d = rds16(p + 2) / 16384.0f;
      p += 4;
    } else if (flags & 0x0080) { /* 2x2 */
      if (p + 8 > end)
        break;
      a = rds16(p) / 16384.0f;
      b = rds16(p + 2) / 16384.0f;
      c = rds16(p + 4) / 16384.0f;
      d = rds16(p + 6) / 16384.0f;
      p += 8;
    }
    /* Only the ARGS_ARE_XY_VALUES form (flags&2) is handled; point-matching
     * composites are rare and skipped (treated as 0 offset). */
    float ox = (flags & 0x0002) ? dx : 0, oy = (flags & 0x0002) ? dy : 0;
    /* compose: child transform then parent m */
    struct xf cm = {m.a * a + m.c * b, m.b * a + m.d * b,         m.a * c + m.c * d,
                    m.b * c + m.d * d, m.a * ox + m.c * oy + m.e, m.b * ox + m.d * oy + m.f};
    load_glyph(f, cgi, r, cm, depth + 1);
    if (!(flags & 0x0020))
      break; /* MORE_COMPONENTS */
  }
}

/* ------------------------------------------------ CFF outlines -------------
 * OpenType fonts with PostScript outlines ('OTTO'): a CFF table of Type 2 charstrings. Name-keyed and
 * CID-keyed fonts are handled; CFF2 (variable) is not. */
static unsigned rdn(const unsigned char *p, int n)
{
  unsigned v = 0;
  while (n-- > 0)
    v = (v << 8) | *p++;
  return v;
}

/* INDEX at offset `o`: its element count, or -1 if it does not fit in the file */
static int idx_count(const struct font *f, unsigned o) { return off_ok(o, 2, f->len) ? (int)rd16(f->d + o) : -1; }

/* Offset just past the INDEX at `o`, 0 if malformed */
static unsigned idx_end(const struct font *f, unsigned o)
{
  int cnt = idx_count(f, o);
  if (cnt < 0)
    return 0;
  if (cnt == 0)
    return o + 2;
  if (!off_ok(o, 3, f->len))
    return 0;
  int osz = f->d[o + 2];
  if (osz < 1 || osz > 4 || !off_ok(o + 3, (cnt + 1) * osz, f->len))
    return 0;
  unsigned data = o + 3 + (unsigned)(cnt + 1) * osz - 1;
  unsigned last = rdn(f->d + o + 3 + (unsigned)cnt * osz, osz);
  return last >= 1 && off_ok(data, (int)last, f->len) && last <= 0x7fffffff ? data + last : 0;
}

/* Element `i` of the INDEX at `o` */
static int idx_get(const struct font *f, unsigned o, int i, const unsigned char **p, int *n)
{
  int cnt = idx_count(f, o);
  if (!o || cnt <= 0 || i < 0 || i >= cnt || !off_ok(o, 3, f->len))
    return 0;
  int osz = f->d[o + 2];
  if (osz < 1 || osz > 4 || !off_ok(o + 3, (cnt + 1) * osz, f->len))
    return 0;
  unsigned data = o + 3 + (unsigned)(cnt + 1) * osz - 1;
  unsigned a = rdn(f->d + o + 3 + (unsigned)i * osz, osz), b = rdn(f->d + o + 3 + (unsigned)(i + 1) * osz, osz);
  if (a < 1 || b < a || b > (unsigned)f->len || !off_ok(data, (int)b, f->len))
    return 0;
  *p = f->d + data + a;
  *n = (int)(b - a);
  return 1;
}

/* Operands of operator `op` (escapes as 0x0c00|b) in a DICT; their count, or -1 if absent/malformed */
static int dict_find(const unsigned char *p, int n, int op, long *v, int maxv)
{
  long st[8];
  int ns = 0;
  for (int i = 0; i < n;) {
    int b = p[i++];
    if (b <= 21) {
      int o = b;
      if (b == 12) {
        if (i >= n)
          return -1;
        o = 0x0c00 | p[i++];
      }
      if (o == op) {
        for (int k = 0; k < ns && k < maxv; k++)
          v[k] = st[k];
        return ns;
      }
      ns = 0;
    } else if (b == 28) {
      if (i + 2 > n)
        return -1;
      if (ns < 8)
        st[ns++] = (short)((p[i] << 8) | p[i + 1]);
      i += 2;
    } else if (b == 29) {
      if (i + 4 > n)
        return -1;
      if (ns < 8)
        st[ns++] = (int)rd32(p + i);
      i += 4;
    } else if (b == 30) {
      while (i < n) {
        int c = p[i++];
        if ((c & 15) == 15 || (c >> 4) == 15)
          break;
      }
      if (ns < 8)
        st[ns++] = 0;
    } else if (b >= 32 && b <= 246) {
      if (ns < 8)
        st[ns++] = b - 139;
    } else if (b >= 247 && b <= 254) {
      if (i >= n)
        return -1;
      int w = p[i++];
      if (ns < 8)
        st[ns++] = b <= 250 ? (b - 247) * 256 + w + 108 : -(b - 251) * 256 - w - 108;
    } else
      return -1;
  }
  return -1;
}

/* Offset of the local Subrs INDEX named by a Private DICT reference ("size offset"), 0 if none */
static unsigned cff_private_subrs(const struct font *f, const unsigned char *dict, int dn)
{
  long pv[2], sv;
  if (dict_find(dict, dn, 18, pv, 2) != 2 || pv[0] < 0 || pv[1] < 0 ||
      !off_ok(f->cff + (unsigned)pv[1], (int)pv[0], f->len))
    return 0;
  const unsigned char *priv = f->d + f->cff + pv[1];
  if (dict_find(priv, (int)pv[0], 19, &sv, 1) != 1 || sv <= 0)
    return 0;
  return f->cff + (unsigned)pv[1] + (unsigned)sv;
}

/* Read the CFF header + Top DICT. Returns 0 if the table is unusable. */
static int cff_init(struct font *f)
{
  unsigned c = f->cff;
  if (!off_ok(c, 4, f->len))
    return 0;
  unsigned o = idx_end(f, c + f->d[c + 2]); /* Name INDEX */
  const unsigned char *top;
  int tn;
  if (!o || !idx_get(f, o, 0, &top, &tn))
    return 0;
  o = idx_end(f, o);         /* Top DICT INDEX */
  o = o ? idx_end(f, o) : 0; /* String INDEX */
  if (!o || !idx_end(f, o))
    return 0;
  f->gsubrs = o;

  long v;
  if (dict_find(top, tn, 0x0c06, &v, 1) == 1 && v != 2)
    return 0; /* Type 1 charstrings */
  if (dict_find(top, tn, 17, &v, 1) != 1 || v <= 0 || (unsigned long)v >= (unsigned)f->len)
    return 0;
  f->charstrings = c + (unsigned)v;
  if (idx_count(f, f->charstrings) < 0)
    return 0;

  if (dict_find(top, tn, 0x0c24, &v, 1) == 1) { /* CID-keyed: a Private DICT per Font DICT */
    unsigned fda = c + (unsigned)v;
    long fs;
    int n = v > 0 && v < f->len ? idx_count(f, fda) : -1;
    if (dict_find(top, tn, 0x0c25, &fs, 1) != 1 || fs <= 0 || fs >= f->len || n <= 0 || n > 256)
      return 0;
    f->fdselect = c + (unsigned)fs;
    f->fd_subrs = calloc((unsigned)n, sizeof *f->fd_subrs);
    if (!f->fd_subrs)
      return 0;
    f->n_fd = n;
    for (int i = 0; i < n; i++) {
      const unsigned char *fd;
      int fn;
      if (idx_get(f, fda, i, &fd, &fn))
        f->fd_subrs[i] = cff_private_subrs(f, fd, fn);
    }
  } else
    f->lsubrs = cff_private_subrs(f, top, tn);
  return 1;
}

/* Local subrs INDEX for glyph `gi` (through FDSelect for CID-keyed fonts) */
static unsigned cff_local_subrs(const struct font *f, int gi)
{
  if (!f->fd_subrs)
    return f->lsubrs;
  const unsigned char *s = f->d + f->fdselect;
  int fd = -1;
  if (!off_ok(f->fdselect, 1, f->len))
    return 0;
  if (s[0] == 0) {
    if (off_ok(f->fdselect, 1 + gi + 1, f->len))
      fd = s[1 + gi];
  } else if (s[0] == 3 && off_ok(f->fdselect, 3, f->len)) {
    int nr = (int)rd16(s + 1);
    if (off_ok(f->fdselect, 5 + nr * 3, f->len))
      for (int i = 0; i < nr; i++)
        if (gi >= (int)rd16(s + 3 + i * 3) && gi < (int)rd16(s + 3 + (i + 1) * 3)) {
          fd = s[5 + i * 3];
          break;
        }
  }
  return fd >= 0 && fd < f->n_fd ? f->fd_subrs[fd] : 0;
}

/* Type 2 charstring interpreter state */
struct cs {
  const struct font *f;
  struct ras *r;
  float st[48];
  int n, nstems, w_done, done, open, ops;
  float x, y;
  unsigned lsubrs;
};

#define CS_MAX_OPS 20000

static void cs_move(struct cs *c, float dx, float dy)
{
  c->x += dx;
  c->y += dy;
  ras_begin(c->r);
  ras_pt(c->r, c->x, c->y);
  c->open = 1;
}

static void cs_line(struct cs *c, float dx, float dy)
{
  if (!c->open)
    return;
  c->x += dx;
  c->y += dy;
  ras_pt(c->r, c->x, c->y);
}

static void cs_curve(struct cs *c, float dx1, float dy1, float dx2, float dy2, float dx3, float dy3)
{
  if (!c->open)
    return;
  float x0 = c->x, y0 = c->y;
  float x1 = x0 + dx1, y1 = y0 + dy1, x2 = x1 + dx2, y2 = y1 + dy2, x3 = x2 + dx3, y3 = y2 + dy3;
  const int N = 8;
  for (int i = 1; i <= N; i++) {
    float t = (float)i / N, u = 1 - t;
    float a = u * u * u, b = 3 * u * u * t, d = 3 * u * t * t, e = t * t * t;
    ras_pt(c->r, a * x0 + b * x1 + d * x2 + e * x3, a * y0 + b * y1 + d * y2 + e * y3);
  }
  c->x = x3;
  c->y = y3;
}

/* The first stack-clearing operator may carry the glyph width as an extra leading operand; drop it */
static void cs_width(struct cs *c, int has)
{
  if (!c->w_done && has && c->n > 0) {
    for (int i = 1; i < c->n; i++)
      c->st[i - 1] = c->st[i];
    c->n--;
  }
  c->w_done = 1;
}

static int cs_bias(const struct font *f, unsigned idx)
{
  int n = idx_count(f, idx);
  return n < 1240 ? 107 : n < 33900 ? 1131 : 32768;
}

static void cs_run(struct cs *c, const unsigned char *p, int len, int depth)
{
  float *s = c->st;
  for (int i = 0; i < len && !c->done;) {
    if (++c->ops > CS_MAX_OPS) {
      c->done = 1;
      return;
    }
    int b = p[i++];
    if (b >= 32 || b == 28) {
      float v;
      if (b == 28) {
        if (i + 2 > len)
          return;
        v = (float)(short)((p[i] << 8) | p[i + 1]);
        i += 2;
      } else if (b <= 246)
        v = (float)(b - 139);
      else if (b <= 250) {
        if (i >= len)
          return;
        v = (float)((b - 247) * 256 + p[i++] + 108);
      } else if (b <= 254) {
        if (i >= len)
          return;
        v = (float)(-(b - 251) * 256 - p[i++] - 108);
      } else {
        if (i + 4 > len)
          return;
        v = (float)(int)rd32(p + i) / 65536.0f;
        i += 4;
      }
      if (c->n >= 48) {
        c->done = 1;
        return;
      }
      s[c->n++] = v;
      continue;
    }
    int n = c->n;
    switch (b) {
    case 1:
    case 3:
    case 18:
    case 23: /* h/vstem(hm) */
      cs_width(c, n & 1);
      c->nstems += c->n / 2;
      break;
    case 19:
    case 20: /* hintmask, cntrmask: implicit vstem, then a mask byte per 8 stems */
      cs_width(c, n & 1);
      c->nstems += c->n / 2;
      i += (c->nstems + 7) / 8;
      break;
    case 21:
      cs_width(c, n > 2);
      if (c->n >= 2)
        cs_move(c, s[c->n - 2], s[c->n - 1]);
      break;
    case 22:
      cs_width(c, n > 1);
      if (c->n >= 1)
        cs_move(c, s[c->n - 1], 0);
      break;
    case 4:
      cs_width(c, n > 1);
      if (c->n >= 1)
        cs_move(c, 0, s[c->n - 1]);
      break;
    case 5:
      for (int k = 0; k + 2 <= n; k += 2)
        cs_line(c, s[k], s[k + 1]);
      break;
    case 6:
    case 7: { /* h/vlineto: alternating axes */
      int horiz = b == 6;
      for (int k = 0; k < n; k++, horiz = !horiz)
        horiz ? cs_line(c, s[k], 0) : cs_line(c, 0, s[k]);
      break;
    }
    case 8:
      for (int k = 0; k + 6 <= n; k += 6)
        cs_curve(c, s[k], s[k + 1], s[k + 2], s[k + 3], s[k + 4], s[k + 5]);
      break;
    case 24: {
      int k = 0;
      for (; k + 6 <= n - 2; k += 6)
        cs_curve(c, s[k], s[k + 1], s[k + 2], s[k + 3], s[k + 4], s[k + 5]);
      if (k + 2 <= n)
        cs_line(c, s[k], s[k + 1]);
      break;
    }
    case 25: {
      int k = 0;
      for (; k + 2 <= n - 6; k += 2)
        cs_line(c, s[k], s[k + 1]);
      if (k + 6 <= n)
        cs_curve(c, s[k], s[k + 1], s[k + 2], s[k + 3], s[k + 4], s[k + 5]);
      break;
    }
    case 26: {
      int k = 0;
      float dx1 = (n & 1) ? s[k++] : 0;
      for (; k + 4 <= n; k += 4, dx1 = 0)
        cs_curve(c, dx1, s[k], s[k + 1], s[k + 2], 0, s[k + 3]);
      break;
    }
    case 27: {
      int k = 0;
      float dy1 = (n & 1) ? s[k++] : 0;
      for (; k + 4 <= n; k += 4, dy1 = 0)
        cs_curve(c, s[k], dy1, s[k + 1], s[k + 2], s[k + 3], 0);
      break;
    }
    case 30:
    case 31: { /* vhcurveto / hvcurveto */
      int horiz = b == 31;
      for (int k = 0; k + 4 <= n; k += 4, horiz = !horiz) {
        float last = n - k == 5 ? s[k + 4] : 0;
        if (horiz)
          cs_curve(c, s[k], 0, s[k + 1], s[k + 2], last, s[k + 3]);
        else
          cs_curve(c, 0, s[k], s[k + 1], s[k + 2], s[k + 3], last);
      }
      break;
    }
    case 10:
    case 29: { /* callsubr / callgsubr */
      if (n < 1 || depth >= 10)
        return;
      unsigned idx = b == 10 ? c->lsubrs : c->f->gsubrs;
      int k = (int)s[--c->n] + cs_bias(c->f, idx);
      const unsigned char *sp;
      int sn;
      if (idx_get(c->f, idx, k, &sp, &sn))
        cs_run(c, sp, sn, depth + 1);
      continue; /* the subr's operands stay on the stack */
    }
    case 11:
      return;
    case 14:
      cs_width(c, n == 1 || n == 5);
      c->done = 1;
      return;
    case 12: {
      if (i >= len)
        return;
      int e = p[i++];
      if (e == 35 && n >= 12) {
        cs_curve(c, s[0], s[1], s[2], s[3], s[4], s[5]);
        cs_curve(c, s[6], s[7], s[8], s[9], s[10], s[11]);
      } else if (e == 34 && n >= 7) {
        cs_curve(c, s[0], 0, s[1], s[2], s[3], 0);
        cs_curve(c, s[4], 0, s[5], -s[2], s[6], 0);
      } else if (e == 36 && n >= 9) {
        cs_curve(c, s[0], s[1], s[2], s[3], s[4], 0);
        cs_curve(c, s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
      } else if (e == 37 && n >= 11) {
        float dx = s[0] + s[2] + s[4] + s[6] + s[8], dy = s[1] + s[3] + s[5] + s[7] + s[9];
        int xdom = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy);
        cs_curve(c, s[0], s[1], s[2], s[3], s[4], s[5]);
        cs_curve(c, s[6], s[7], s[8], s[9], xdom ? s[10] : -dx, xdom ? -dy : s[10]);
      }
      break;
    }
    default:
      break;
    }
    c->n = 0;
  }
}

static void cff_glyph(const struct font *f, int gi, struct ras *r)
{
  const unsigned char *p;
  int n;
  if (!idx_get(f, f->charstrings, gi, &p, &n))
    return;
  struct cs c;
  for (unsigned i = 0; i < sizeof c; i++)
    ((char *)&c)[i] = 0;
  c.f = f;
  c.r = r;
  c.lsubrs = cff_local_subrs(f, gi);
  cs_run(&c, p, n, 0);
}

static void load_glyph(const struct font *f, int gi, struct ras *r, struct xf m, int depth)
{
  if (depth > 5 || gi < 0 || gi >= f->num_glyphs)
    return;
  if (f->cff) {
    cff_glyph(f, gi, r);
    return;
  }
  unsigned o0, o1;
  if (f->loc_long) {
    if (!off_ok(f->loca, gi * 4 + 8, f->len))
      return;
    o0 = rd32(f->d + f->loca + gi * 4);
    o1 = rd32(f->d + f->loca + gi * 4 + 4);
  } else {
    if (!off_ok(f->loca, gi * 2 + 4, f->len))
      return;
    o0 = (unsigned)rd16(f->d + f->loca + gi * 2) * 2;
    o1 = (unsigned)rd16(f->d + f->loca + gi * 2 + 2) * 2;
  }
  if (o1 <= o0)
    return; /* empty glyph (e.g. space) */
  /* [o0,o1) is relative to the glyf table; require it to lie within glyf..EOF
   * so the record pointer + length can't run past the buffer (or overflow). */
  if (o1 > (unsigned)f->len - f->glyf)
    return;
  const unsigned char *g = f->d + f->glyf + o0;
  int glen = (int)(o1 - o0);
  if (glen < 10)
    return;
  int nc = rds16(g);
  if (nc >= 0)
    load_simple(f, g, glen, r, m);
  else
    load_composite(f, g, glen, r, m, depth);
}

/* ------------------------------------------------ rasterize ----------------
 */
#define SS 4 /* supersample factor */

unsigned char *font_render_glyph(const struct font *f, int gi, float px, int *w, int *h, int *ox, int *oy, int *adv)
{
  *w = *h = *ox = *oy = 0;
  if (adv)
    *adv = 0;
  float scale = px / f->upem;
  if (adv)
    *adv = (int)(glyph_advance_units(f, gi) * scale + 0.5f);

  struct ras r;
  r.px = r.py = 0;
  r.np = r.cap = 0;
  r.sub = 0;
  r.ns = r.scap = 0;
  r.oom = 0;
  struct xf id = {1, 0, 0, 1, 0, 0};
  load_glyph(f, gi, &r, id, 0);
  if (r.oom || r.np < 3 || r.ns == 0) {
    free(r.px);
    free(r.py);
    free(r.sub);
    return 0;
  }

  /* bbox in device pixels (y flipped: font is y-up, bitmap y-down) */
  float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
  for (int i = 0; i < r.np; i++) {
    float dx = r.px[i] * scale, dy = -r.py[i] * scale;
    if (dx < minx)
      minx = dx;
    if (dx > maxx)
      maxx = dx;
    if (dy < miny)
      miny = dy;
    if (dy > maxy)
      maxy = dy;
  }
  int x0 = (int)minx;
  if ((float)x0 > minx)
    x0--; /* floor */
  int y0 = (int)miny;
  if ((float)y0 > miny)
    y0--;
  int x1 = (int)maxx;
  if ((float)x1 < maxx)
    x1++; /* ceil  */
  int y1 = (int)maxy;
  if ((float)y1 < maxy)
    y1++;
  int bw = x1 - x0, bh = y1 - y0;
  if (bw <= 0 || bh <= 0 || bw > 4096 || bh > 4096) {
    free(r.px);
    free(r.py);
    free(r.sub);
    return 0;
  }

  int dw = bw * SS, dh = bh * SS;
  unsigned char *cov = malloc((unsigned)dw * dh); /* SS coverage (0/1) */
  unsigned char *out = malloc((unsigned)bw * bh); /* final 8-bit */
  if (!cov || !out) {
    free(cov);
    free(out);
    free(r.px);
    free(r.py);
    free(r.sub);
    return 0;
  }
  for (int i = 0; i < dw * dh; i++)
    cov[i] = 0;

  /* scale points into the SS device buffer */
  for (int i = 0; i < r.np; i++) {
    r.px[i] = (r.px[i] * scale - x0) * SS;
    r.py[i] = (-r.py[i] * scale - y0) * SS;
  }

  /* scanline nonzero fill (locals, not static: the browser rasterizes glyphs
   * from multiple worker threads and must not share scanline scratch) */
  float xs[1024];
  int wd[1024];
  for (int y = 0; y < dh; y++) {
    float yc = y + 0.5f;
    int nx = 0;
    for (int s = 0; s < r.ns && nx < 1024; s++) {
      int a = r.sub[s], b = (s + 1 < r.ns) ? r.sub[s + 1] : r.np;
      int cnt = b - a;
      if (cnt < 2)
        continue;
      for (int i = 0; i < cnt; i++) {
        int i0 = a + i, i1 = a + (i + 1) % cnt;
        float ya = r.py[i0], yb = r.py[i1];
        if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) {
          float t = (yc - ya) / (yb - ya);
          if (nx < 1024) {
            xs[nx] = r.px[i0] + t * (r.px[i1] - r.px[i0]);
            wd[nx] = (yb > ya) ? 1 : -1;
            nx++;
          }
        }
      }
    }
    for (int i = 1; i < nx; i++) { /* insertion sort by x */
      float kx = xs[i];
      int kw = wd[i];
      int j = i - 1;
      while (j >= 0 && xs[j] > kx) {
        xs[j + 1] = xs[j];
        wd[j + 1] = wd[j];
        j--;
      }
      xs[j + 1] = kx;
      wd[j + 1] = kw;
    }
    int wind = 0;
    unsigned char *row = cov + (unsigned)y * dw;
    for (int i = 0; i + 1 < nx; i++) {
      wind += wd[i];
      if (wind != 0) {
        int xa = (int)(xs[i] + 0.5f), xb = (int)(xs[i + 1] + 0.5f);
        if (xa < 0)
          xa = 0;
        if (xb > dw)
          xb = dw;
        for (int xx = xa; xx < xb; xx++)
          row[xx] = 1;
      }
    }
  }

  /* downsample SS×SS -> 0..255 coverage */
  for (int y = 0; y < bh; y++)
    for (int xx = 0; xx < bw; xx++) {
      int acc = 0;
      for (int sy = 0; sy < SS; sy++)
        for (int sx = 0; sx < SS; sx++)
          acc += cov[(unsigned)(y * SS + sy) * dw + (xx * SS + sx)];
      out[y * bw + xx] = (unsigned char)(acc * 255 / (SS * SS));
    }

  free(cov);
  free(r.px);
  free(r.py);
  free(r.sub);
  *w = bw;
  *h = bh;
  *ox = x0;
  *oy = y0;
  return out;
}

/* ------------------------------------------------ family name -------------- */
int font_family(const struct font *f, char *out, int n)
{
  unsigned nt = find_table(f->d, f->len, "name");
  if (n < 1)
    return 0;
  out[0] = '\0';
  if (!nt || !off_ok(nt, 6, f->len))
    return 0;
  int count = (int)rd16(f->d + nt + 2);
  unsigned strs = nt + rd16(f->d + nt + 4);
  int best = 0, bestscore = 0;
  for (int i = 0; i < count; i++) {
    unsigned r = nt + 6 + (unsigned)i * 12;
    if (!off_ok(r, 12, f->len))
      break;
    unsigned plat = rd16(f->d + r), id = rd16(f->d + r + 6);
    int score = (id == 16 ? 2 : id == 1 ? 1 : 0) * 2 + (plat == 3 ? 1 : 0);
    if (id != 1 && id != 16)
      continue;
    if ((plat != 3 && plat != 1) || score <= bestscore ||
        !off_ok(strs + rd16(f->d + r + 10), rd16(f->d + r + 8), f->len))
      continue;
    best = i + 1;
    bestscore = score;
  }
  if (!best)
    return 0;
  unsigned r = nt + 6 + (unsigned)(best - 1) * 12;
  const unsigned char *s = f->d + strs + rd16(f->d + r + 10);
  unsigned len = rd16(f->d + r + 8), step = rd16(f->d + r) == 3 ? 2 : 1;
  int o = 0;
  for (unsigned i = 0; i + step <= len && o < n - 1; i += step) {
    unsigned c = step == 2 ? rd16(s + i) : s[i];
    out[o++] = c < 0x80 ? (char)c : '?';
  }
  out[o] = '\0';
  return o;
}
