/* svg_text.c: <text> and <tspan> laid out with the shared TrueType rasterizer. */
#include "features/svg/svg_int.h"
#include "font.h"
#include "fontstore.h"

static struct font *g_fonts[2]; /* sans, mono */
static bool g_fonts_tried;

static struct font *load_font(const char *path)
{
  struct statinfo st;
  if (stat(path, &st) < 0 || st.is_dir || st.size == 0)
    return 0;
  unsigned char *buf = malloc(st.size);
  if (!buf)
    return 0;
  struct font *f = readfile_at(path, 0, buf, st.size) == (int)st.size ? font_load(buf, (int)st.size) : 0;
  if (!f)
    free(buf); /* a font keeps pointing into buf, so it stays allocated for the process lifetime */
  return f;
}

/* Fonts used for text; NULL keeps the installed sans and mono faces. */
void svg_set_fonts(struct font *sans, struct font *mono)
{
  g_fonts[0] = sans;
  g_fonts[1] = mono;
  g_fonts_tried = true;
}

static const struct font *pick_font(bool mono)
{
  if (!g_fonts_tried) {
    g_fonts_tried = true;
    char path[96];
    fontstore_path(FONT_SANS, path, sizeof path);
    g_fonts[0] = load_font(path);
    fontstore_path(FONT_MONO, path, sizeof path);
    g_fonts[1] = load_font(path);
  }
  return g_fonts[mono] ? g_fonts[mono] : g_fonts[!mono];
}

static unsigned next_cp(const char **s)
{
  const unsigned char *p = (const unsigned char *)*s;
  unsigned cp = *p;
  int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
  if (cp >= 0xf8 || (cp >= 0x80 && cp < 0xc0)) {
    extra = 0;
    cp = 0xfffd;
  } else if (extra) {
    cp &= 0x3fu >> extra;
    for (int i = 1; i <= extra; i++) {
      if ((p[i] & 0xc0) != 0x80) {
        extra = i - 1;
        cp = 0xfffd;
        break;
      }
      cp = (cp << 6) | (p[i] & 0x3f);
    }
  }
  *s += 1 + extra;
  return cp;
}

/* One text node's character data with the basic XML entities resolved; NULL on OOM. */
static char *entity_decoded(const char *src)
{
  char *out = malloc(strlen(src) + 1);
  if (!out)
    return 0;
  char *o = out;
  while (*src) {
    if (*src != '&') {
      *o++ = *src++;
      continue;
    }
    static const struct {
      const char *name;
      char ch;
    } named[] = {{"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''}, {"nbsp;", ' '}};
    bool done = false;
    for (unsigned i = 0; i < sizeof named / sizeof *named && !done; i++)
      if (starts_ci(src + 1, named[i].name)) {
        *o++ = named[i].ch;
        src += 1 + strlen(named[i].name);
        done = true;
      }
    if (!done && src[1] == '#') {
      const char *q = src + 2;
      unsigned cp = 0;
      bool hex = *q == 'x' || *q == 'X';
      q += hex;
      const char *d0 = q;
      for (; *q && *q != ';'; q++) {
        int v = hex ? ((*q >= '0' && *q <= '9')               ? *q - '0'
                       : (*q | 32) >= 'a' && (*q | 32) <= 'f' ? (*q | 32) - 'a' + 10
                                                              : -1)
                    : ((*q >= '0' && *q <= '9') ? *q - '0' : -1);
        if (v < 0)
          break;
        cp = cp * (hex ? 16 : 10) + (unsigned)v;
      }
      if (q > d0 && *q == ';' && cp > 0 && cp < 0x110000) {
        if (cp < 0x80)
          *o++ = (char)cp;
        else if (cp < 0x800) {
          *o++ = (char)(0xc0 | (cp >> 6));
          *o++ = (char)(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
          *o++ = (char)(0xe0 | (cp >> 12));
          *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
          *o++ = (char)(0x80 | (cp & 0x3f));
        } else {
          *o++ = (char)(0xf0 | (cp >> 18));
          *o++ = (char)(0x80 | ((cp >> 12) & 0x3f));
          *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
          *o++ = (char)(0x80 | (cp & 0x3f));
        }
        src = q + 1;
        done = true;
      }
    }
    if (!done)
      *o++ = *src++;
  }
  *o = '\0';
  return out;
}

struct tlay {
  struct mat m;
  float k;         /* device pixels per user unit */
  float x, y;      /* pen, user space */
  float x0, shift; /* pen at the start of the text, text-anchor offset */
  float width;     /* measured advance of the whole text */
  bool measure;    /* only advance the pen */
  bool started;    /* a visible character has been emitted */
  bool space;      /* collapsed whitespace pending */
  unsigned char *cov;
  struct ctx *cx;
};

static void blit_glyph(struct tlay *t, const unsigned char *g, int gw, int gh, int gx, int gy, float ux, float uy,
                       float px)
{
  struct ctx *cx = t->cx;
  struct mat m = t->m;
  bool axis = ABSF(m.b) < 1e-3f && ABSF(m.c) < 1e-3f && m.a > 0 && m.d > 0;
  if (axis) {
    float dx, dy;
    svg_mat_apply(m, ux, uy, &dx, &dy);
    int ox = (int)floor((double)dx + 0.5) + gx, oy = (int)floor((double)dy + 0.5) + gy;
    for (int y = 0; y < gh; y++)
      for (int x = 0; x < gw; x++) {
        int X = ox + x, Y = oy + y;
        if (X < 0 || Y < 0 || X >= cx->w || Y >= cx->h)
          continue;
        unsigned char *c = &t->cov[Y * cx->w + X];
        unsigned v = g[y * gw + x] + *c;
        *c = v > 255 ? 255 : (unsigned char)v;
      }
    return;
  }
  /* rotated or skewed: map device pixels back into the glyph bitmap */
  float cs[4][2] = {{(float)gx, (float)gy},
                    {(float)(gx + gw), (float)gy},
                    {(float)(gx + gw), (float)(gy + gh)},
                    {(float)gx, (float)(gy + gh)}};
  struct bb b = {0, 0, 0, 0, false};
  for (int i = 0; i < 4; i++) {
    float lx = ux + cs[i][0] / t->k, ly = uy + cs[i][1] / t->k, dx, dy;
    svg_mat_apply(m, lx, ly, &dx, &dy);
    svg_bb_add(&b, dx, dy);
  }
  struct mat inv;
  if (!svg_mat_inv(m, &inv))
    return;
  int x0 = (int)floor((double)b.x0), x1 = (int)ceil((double)b.x1), y0 = (int)floor((double)b.y0),
      y1 = (int)ceil((double)b.y1);
  for (int Y = y0 < 0 ? 0 : y0; Y < y1 && Y < cx->h; Y++)
    for (int X = x0 < 0 ? 0 : x0; X < x1 && X < cx->w; X++) {
      float lx, ly;
      svg_mat_apply(inv, (float)X + 0.5f, (float)Y + 0.5f, &lx, &ly);
      float gxf = (lx - ux) * t->k - (float)gx - 0.5f, gyf = (ly - uy) * t->k - (float)gy - 0.5f;
      int ix = (int)floor((double)gxf), iy = (int)floor((double)gyf);
      float fx = gxf - (float)ix, fy = gyf - (float)iy, v = 0;
      for (int k = 0; k < 4; k++) {
        int sx = ix + (k & 1), sy = iy + (k >> 1);
        if (sx < 0 || sy < 0 || sx >= gw || sy >= gh)
          continue;
        v += ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy) * (float)g[sy * gw + sx];
      }
      unsigned char *c = &t->cov[Y * cx->w + X];
      unsigned nv = (unsigned)(v + 0.5f) + *c;
      *c = nv > 255 ? 255 : (unsigned char)nv;
    }
  (void)px;
}

static void put_char(struct tlay *t, unsigned cp, const struct style *st)
{
  const struct font *f = pick_font(st->mono);
  if (!f)
    return;
  float px = st->font_size * t->k;
  if (px < 1)
    px = 1;
  int gi = font_glyph_index(f, (int)cp);
  int adv = 0, lsb = 0;
  font_glyph_hmetrics(f, gi, px, &adv, &lsb);
  if (!t->measure) {
    int gw, gh, gx, gy, a2;
    unsigned char *g = font_render_glyph(f, gi, px, &gw, &gh, &gx, &gy, &a2);
    if (g) {
      for (int pass = 0; pass < (st->bold ? 2 : 1); pass++)
        blit_glyph(t, g, gw, gh, gx + pass, gy, t->x, t->y, px);
      free(g);
    }
  }
  t->x += ((float)adv + (st->bold ? 1.0f : 0.0f)) / t->k + st->letter_spacing;
}

static void put_text(struct tlay *t, const char *text, const struct style *st)
{
  char *dec = entity_decoded(text);
  if (!dec)
    return;
  const char *p = dec;
  while (*p) {
    unsigned cp = next_cp(&p);
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
      t->space = t->started;
      continue;
    }
    if (t->space) {
      put_char(t, ' ', st);
      t->space = false;
    }
    put_char(t, cp, st);
    t->started = true;
  }
  free(dec);
}

/* First number of a (possibly list-valued) length attribute. */
static bool first_len(const struct dom_node *n, const char *name, float ref, float *out)
{
  const char *v = dom_get_attr(n, name);
  return v && svg_parse_len(v, ref, out);
}

static void walk(struct tlay *t, const struct dom_node *n, const struct style *in, int depth)
{
  struct style st = *in;
  if (depth > 0) {
    svg_resolve_style(t->cx, n, &st);
    if (svg_display_none(t->cx, n))
      return;
  }
  float v;
  if (first_len(n, "x", t->cx->vw, &v))
    t->x = v + t->shift;
  if (first_len(n, "y", t->cx->vh, &v))
    t->y = v;
  if (first_len(n, "dx", t->cx->vw, &v))
    t->x += v;
  if (first_len(n, "dy", t->cx->vh, &v))
    t->y += v;
  if (depth == 0)
    t->x0 = t->x;
  for (const struct dom_node *c = n->first_child; c && depth < 16; c = c->next) {
    if (c->type == DOM_TEXT) {
      if (c->text)
        put_text(t, c->text, &st);
    } else if (streq(c->tag, "tspan") || streq(c->tag, "a")) {
      walk(t, c, &st, depth + 1);
    }
  }
}

/* Fill a <text> element (stroke is not drawn). */
void svg_draw_text(struct ctx *cx, const struct dom_node *n, struct mat m, const struct style *st)
{
  if (st->hidden || st->fill.kind == PAINT_NONE)
    return;
  struct tlay t;
  memset(&t, 0, sizeof t);
  t.cx = cx;
  t.m = m;
  t.k = svg_mat_scale(m);
  if (!(t.k > 0.01f))
    return;
  t.measure = true;
  walk(&t, n, st, 0);
  t.shift = st->anchor ? -(t.x - t.x0) * (st->anchor == 1 ? 0.5f : 1.0f) : 0;
  t.cov = calloc((unsigned)cx->w * (unsigned)cx->h, 1);
  if (!t.cov)
    return;
  t.measure = false;
  t.started = t.space = false;
  t.x = t.shift;
  t.y = 0;
  walk(&t, n, st, 0);
  struct src s;
  struct bb tb = {0, 0, 1, 1, true};
  if (svg_make_src(cx, &st->fill, st->fill_op, st->color, &tb, m, &s)) {
    svg_blend_mask(cx, t.cov, 0, 0, cx->w, cx->h, &s);
    svg_src_free(&s);
  }
  free(t.cov);
}
