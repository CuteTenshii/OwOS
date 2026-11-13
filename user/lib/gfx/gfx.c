/* gfx.c: user-space drawing into a window's shared pixel buffer. See gfx.h */
#include "gfx.h"
#include "font.h" /* scalable fonts, loaded at runtime (no baked atlas) */
#include "fontstore.h"
#include "owos.h" /* KEY_* editing codes delivered via WEV_KEY */

/* Apps lay out the monospace grid on this fixed cell (terminals, the editor) */
#define GFX_FONT_W 8
#define GFX_FONT_H 16

static unsigned *g_fb;
static int g_w, g_h;

static int g_cx0, g_cy0, g_cx1, g_cy1;

void gfx_init(unsigned *fb, int w, int h)
{
  g_fb = fb;
  g_w = w;
  g_h = h;
  g_cx0 = 0;
  g_cy0 = 0;
  g_cx1 = w;
  g_cy1 = h;
}

void gfx_get(unsigned **fb, int *w, int *h)
{
  if (fb)
    *fb = g_fb;
  if (w)
    *w = g_w;
  if (h)
    *h = g_h;
}

void gfx_clip_none(void)
{
  g_cx0 = 0;
  g_cy0 = 0;
  g_cx1 = g_w;
  g_cy1 = g_h;
}

void gfx_set_clip(int x, int y, int w, int h)
{
  int x1 = x + w, y1 = y + h;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (x1 > g_w)
    x1 = g_w;
  if (y1 > g_h)
    y1 = g_h;
  g_cx0 = x;
  g_cy0 = y;
  g_cx1 = x1 > x ? x1 : x;
  g_cy1 = y1 > y ? y1 : y;
}

/* 32-bpp run fill (`rep stosl`) */
static inline void fill32(unsigned *d, unsigned v, int n)
{
  int d0, d1;
  __asm__ volatile("rep ; stosl" : "=&D"(d0), "=&c"(d1) : "a"(v), "0"(d), "1"(n) : "memory");
}

struct gfx_rect {
  int x0, y0, x1, y1;
};

static int clip_rect(int x, int y, int w, int h, struct gfx_rect *out)
{
  long long x1 = (long long)x + w, y1 = (long long)y + h;
  int x0 = x > g_cx0 ? x : g_cx0;
  int y0 = y > g_cy0 ? y : g_cy0;
  int x2 = x1 <= 0 ? 0 : x1 >= g_cx1 ? g_cx1 : (int)x1;
  int y2 = y1 <= 0 ? 0 : y1 >= g_cy1 ? g_cy1 : (int)y1;
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x2 > g_w)
    x2 = g_w;
  if (y2 > g_h)
    y2 = g_h;
  if (w <= 0 || h <= 0 || x2 <= x0 || y2 <= y0)
    return 0;
  out->x0 = x0;
  out->y0 = y0;
  out->x1 = x2;
  out->y1 = y2;
  return 1;
}

void gfx_pixel(int x, int y, unsigned color)
{
  if (x < g_cx0 || x >= g_cx1 || y < g_cy0 || y >= g_cy1)
    return;
  if ((unsigned)x < (unsigned)g_w && (unsigned)y < (unsigned)g_h)
    g_fb[y * g_w + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, unsigned color)
{
  if (w > 0 && h > 0 && x >= 0 && y >= 0 && x <= g_w - w && y <= g_h - h && g_cx0 == 0 && g_cy0 == 0 && g_cx1 == g_w &&
      g_cy1 == g_h) {
    for (int r = 0; r < h; r++)
      fill32(g_fb + (long)(y + r) * g_w + x, color, w);
    return;
  }

  struct gfx_rect r;
  if (!clip_rect(x, y, w, h, &r))
    return;
  int ww = r.x1 - r.x0;
  for (int row = r.y0; row < r.y1; row++)
    fill32(g_fb + (long)row * g_w + r.x0, color, ww);
}

void gfx_clear(unsigned color) { gfx_fill_rect(0, 0, g_w, g_h, color); }

void gfx_rect(int x, int y, int w, int h, unsigned color)
{
  if (w > 0 && h > 0 && x >= 0 && y >= 0 && x <= g_w - w && y <= g_h - h && g_cx0 == 0 && g_cy0 == 0 && g_cx1 == g_w &&
      g_cy1 == g_h) {
    fill32(g_fb + (long)y * g_w + x, color, w);
    if (h > 1)
      fill32(g_fb + (long)(y + h - 1) * g_w + x, color, w);
    if (w > 1)
      for (int row = 1; row < h - 1; row++) {
        g_fb[(long)(y + row) * g_w + x] = color;
        g_fb[(long)(y + row) * g_w + x + w - 1] = color;
      }
    return;
  }
  gfx_fill_rect(x, y, w, 1, color);
  gfx_fill_rect(x, y + h - 1, w, 1, color);
  gfx_fill_rect(x, y, 1, h, color);
  gfx_fill_rect(x + w - 1, y, 1, h, color);
}

/* Blend fg over bg by coverage a (0..255), per channel */
static unsigned gfx_blend(unsigned fg, unsigned bg, unsigned a)
{
  if (a == 0)
    return bg;
  if (a == 255)
    return fg;
  unsigned ia = 255 - a;
  unsigned r = ((fg >> 16) & 0xff) * a + ((bg >> 16) & 0xff) * ia + 128;
  unsigned g = ((fg >> 8) & 0xff) * a + ((bg >> 8) & 0xff) * ia + 128;
  unsigned b = (fg & 0xff) * a + (bg & 0xff) * ia + 128;
  return (((r + (r >> 8)) >> 8) << 16) | (((g + (g >> 8)) >> 8) << 8) | ((b + (b >> 8)) >> 8);
}

/* Blit a w*h ARGB (0xAARRGGBB) bitmap at (x,y), alpha-blending each pixel over the framebuffer */
void gfx_blit_argb(int x, int y, int w, int h, const unsigned *argb)
{
  struct gfx_rect r;
  if (!clip_rect(x, y, w, h, &r))
    return;

  int src_x = r.x0 - x;
  for (int dy = r.y0; dy < r.y1; dy++) {
    const unsigned *src = argb + (long)(dy - y) * w + src_x;
    unsigned *drow = g_fb + (long)dy * g_w + r.x0;
    for (int dx = r.x0; dx < r.x1; dx++, src++, drow++) {
      unsigned px = *src, a = px >> 24;
      if (a == 0)
        continue;
      *drow = (a == 255) ? (px & 0xffffff) : gfx_blend(px & 0xffffff, *drow, a);
    }
  }
}

/* fonts */
struct gcache_e {
  unsigned cp;
  unsigned char *bm;
  int w, h, ox, oy, adv;
  int valid;
};

/* Direct-mapped glyph cache lookup; rasterizes and evicts on a miss */
static struct gcache_e *cache_glyph(struct font *font, int px, struct gcache_e *cache, int n, unsigned cp)
{
  struct gcache_e *g = &cache[cp % n];
  if (g->valid && g->cp == cp)
    return g;
  if (g->valid) {
    free(g->bm);
    g->bm = 0;
  }
  int gi = font_glyph_index(font, (int)cp);
  g->bm = font_render_glyph(font, gi, (float)px, &g->w, &g->h, &g->ox, &g->oy, &g->adv);
  g->cp = cp;
  g->valid = 1;
  return g;
}

/* Font files loaded so far, shared by every size drawn from them; a file that failed to load keeps font = 0 */
#define FONT_FILES 8
struct font_file {
  char path[96];
  struct font *font;
  int weight, italic;
};
static struct font_file g_files[FONT_FILES];
static int g_nfiles;

/* Index of the loaded file at `path`, or -1 */
static int file_open(const char *path)
{
  for (int i = 0; i < g_nfiles; i++)
    if (strcmp(g_files[i].path, path) == 0)
      return g_files[i].font ? i : -1;
  if (g_nfiles == FONT_FILES)
    return -1;
  struct font_file *ff = &g_files[g_nfiles];
  snprintf(ff->path, sizeof ff->path, "%s", path);
  ff->font = 0;
  int i = g_nfiles++;
  struct statinfo st;
  if (stat(path, &st) < 0 || st.is_dir || st.size == 0)
    return -1;
  unsigned char *buf = malloc(st.size); /* kept for the font's lifetime */
  if (!buf)
    return -1;
  if (readfile_at(path, 0, buf, st.size) != (int)st.size) {
    free(buf);
    return -1;
  }
  ff->font = font_load(buf, (int)st.size);
  if (!ff->font) {
    free(buf);
    return -1;
  }
  font_style(ff->font, &ff->weight, &ff->italic);
  return i;
}

static int ieq(const char *a, const char *b)
{
  for (; *a && *b; a++, b++)
    if ((*a | 32) != (*b | 32))
      return 0;
  return !*a && !*b;
}

/* Which file serves a (family, weight, slant), and what has to be synthesised on top of it */
#define FONT_MEMO 12
struct font_memo {
  char family[48];
  int weight, italic;
  int file;
  int bold, slant;
  int used;
};
static struct font_memo g_memo[FONT_MEMO];
static int g_memo_next;

static int file_differs(int file, int weight, int italic)
{
  int d = g_files[file].weight - weight;
  return d >= 100 || d <= -100 || (g_files[file].italic != 0) != (italic != 0);
}

static int role_file(int role, int weight, int italic)
{
  char path[96];
  fontstore_path(role, path, sizeof path);
  int file = file_open(path);
  if (file < 0 || !file_differs(file, weight, italic))
    return file;
  /* The face chosen in Settings is one weight; another installed face of its family may fit better */
  char fam[64];
  if (font_family(g_files[file].font, fam, sizeof fam) &&
      fontstore_match(fam, weight, italic, path, sizeof path, 0, 0)) {
    int alt = file_open(path);
    if (alt >= 0)
      return alt;
  }
  return file;
}

static void font_lookup(const char *family, int weight, int italic, struct font_memo *m)
{
  int role = -1;
  if (!family || !*family || ieq(family, "sans-serif") || ieq(family, "sans") || ieq(family, "system-ui"))
    role = FONT_SANS;
  else if (ieq(family, "monospace") || ieq(family, "mono"))
    role = FONT_MONO;
  int file = -1;
  char path[96];
  if (role < 0 && fontstore_match(family, weight, italic, path, sizeof path, 0, 0))
    file = file_open(path);
  if (file < 0)
    file = role_file(role < 0 ? FONT_SANS : role, weight, italic);
  if (file < 0 && role == FONT_MONO)
    file = role_file(FONT_SANS, weight, italic);
  m->file = file;
  m->bold = file >= 0 && weight >= 600 && g_files[file].weight < 600;
  m->slant = file >= 0 && italic && !g_files[file].italic;
}

static const struct font_memo *font_memo(const char *family, int weight, int italic)
{
  const char *key = family ? family : "";
  for (int i = 0; i < FONT_MEMO; i++) {
    struct font_memo *m = &g_memo[i];
    if (m->used && m->weight == weight && m->italic == italic && strcmp(m->family, key) == 0)
      return m;
  }
  struct font_memo *m = &g_memo[g_memo_next];
  g_memo_next = (g_memo_next + 1) % FONT_MEMO;
  snprintf(m->family, sizeof m->family, "%s", key);
  m->weight = weight;
  m->italic = italic;
  m->used = 1;
  font_lookup(family, weight, italic, m);
  return m;
}

/* Glyph caches, one per (file, px), reclaimed least recently used first */
#define SIZED_FACES 10
#define SIZED_CACHE 384
struct sized_face {
  int file, px, baseline;
  unsigned lru;
  struct gcache_e cache[SIZED_CACHE];
};
static struct sized_face g_sized[SIZED_FACES];
static unsigned g_lru_tick;

static struct sized_face *sized_slot(int file, int px)
{
  int empty = -1, lru = 0;
  for (int i = 0; i < SIZED_FACES; i++) {
    if (g_sized[i].px == px && g_sized[i].file == file) {
      g_sized[i].lru = ++g_lru_tick;
      return &g_sized[i];
    }
    if (g_sized[i].px == 0 && empty < 0)
      empty = i;
    if (g_sized[i].lru < g_sized[lru].lru)
      lru = i;
  }
  struct sized_face *s = &g_sized[empty >= 0 ? empty : lru];
  for (int i = 0; i < SIZED_CACHE; i++)
    if (s->cache[i].valid) {
      free(s->cache[i].bm);
      s->cache[i].bm = 0;
      s->cache[i].valid = 0;
    }
  s->file = file;
  s->px = px;
  font_vmetrics(g_files[file].font, (float)px, &s->baseline, 0, 0);
  s->lru = ++g_lru_tick;
  return s;
}

/* A face ready to draw: the font, its glyph cache at px, and the synthetic bold (extra px) and slant to apply */
struct face_view {
  struct font *font;
  int px, baseline;
  int bold, slant;
  struct gcache_e *cache;
  int n;
};

static int font_view(const struct gfx_font *spec, struct face_view *v)
{
  struct gfx_font f;
  gfx_font_resolve(spec, &f);
  const struct font_memo *m = font_memo(f.family, f.weight, f.italic != 0);
  if (m->file < 0)
    return 0;
  struct sized_face *s = sized_slot(m->file, f.px);
  v->font = g_files[m->file].font;
  v->px = f.px;
  v->baseline = s->baseline;
  v->bold = m->bold ? (f.px + 12) / 24 : 0;
  v->slant = m->slant;
  v->cache = s->cache;
  v->n = SIZED_CACHE;
  return 1;
}

static struct gcache_e *view_glyph(const struct face_view *v, unsigned cp)
{
  return cache_glyph(v->font, v->px, v->cache, v->n, cp);
}

/* Blit a cached glyph with its top-left at (x + ox, baseline + oy). `bold` widens each row by that many px, taking the
 * strongest coverage under it; `slant` shears rows right in proportion to their height above the baseline. */
static void face_blit(int x, int baseline, const struct gcache_e *g, unsigned fg, int bold, int slant)
{
  if (!g->bm)
    return;
  for (int gy = 0; gy < g->h; gy++) {
    int sy = baseline + g->oy + gy;
    if ((unsigned)sy >= (unsigned)g_h || sy < g_cy0 || sy >= g_cy1)
      continue;
    int shift = slant ? -(g->oy + gy) * 13 / 64 : 0;
    const unsigned char *srow = g->bm + gy * g->w;
    for (int gx = 0; gx < g->w + bold; gx++) {
      int sx = x + g->ox + gx + shift;
      if ((unsigned)sx >= (unsigned)g_w || sx < g_cx0 || sx >= g_cx1)
        continue;
      unsigned a = 0;
      for (int k = 0; k <= bold; k++) {
        int c = gx - k;
        if (c >= 0 && c < g->w && srow[c] > a)
          a = srow[c];
      }
      if (a) {
        unsigned *px = &g_fb[sy * g_w + sx];
        *px = gfx_blend(fg, *px, a);
      }
    }
  }
}

/* The terminal and editor grid: the monospace face at a fixed cell */
#define MONO_PX 13
#define FACE_CACHE 512
static struct {
  int file, baseline, tried;
  struct gcache_e cache[FACE_CACHE];
} g_mono;

static int mono_ready(void)
{
  if (!g_mono.tried) {
    char path[96];
    g_mono.tried = 1;
    fontstore_path(FONT_MONO, path, sizeof path);
    g_mono.file = file_open(path);
    if (g_mono.file >= 0)
      font_vmetrics(g_files[g_mono.file].font, (float)MONO_PX, &g_mono.baseline, 0, 0);
  }
  return g_mono.file >= 0;
}

/* One monospace cell: optional opaque background */
void gfx_glyph(int x, int y, unsigned char ch, unsigned fg, unsigned bg)
{
  if (!(bg & GFX_TRANSPARENT))
    gfx_fill_rect(x, y, GFX_FONT_W, GFX_FONT_H, bg);
  if (!mono_ready())
    return;
  struct gcache_e *g = cache_glyph(g_files[g_mono.file].font, MONO_PX, g_mono.cache, FACE_CACHE, ch);
  face_blit(x, y + g_mono.baseline, g, fg, 0, 0);
}

void gfx_text(int x, int y, const char *s, unsigned fg, unsigned bg)
{
  int bulk_bg = 0;
  if (!(bg & GFX_TRANSPARENT)) {
    int n = 0;
    while (s[n])
      n++;
    if (n > 0 && n <= 2147483647 / GFX_FONT_W) {
      gfx_fill_rect(x, y, n * GFX_FONT_W, GFX_FONT_H, bg);
      bulk_bg = 1;
    }
  }
  for (; *s; s++, x += GFX_FONT_W)
    gfx_glyph(x, y, (unsigned char)*s, fg, bulk_bg ? GFX_TRANSPARENT : bg);
}

/* Map a Unicode codepoint to the cell font's CP437 glyph (0 = no glyph) */
static unsigned char uni_to_cp437(unsigned cp)
{
  if (cp < 0x80)
    return (unsigned char)cp;
  if (cp >= 0xA0 && cp <= 0xFF) {
    static const unsigned char t[96] = {
        0x20, 0xAD, 0x9B, 0x9C, 0x00, 0x9D, 0x00, 0x15, 0x00, 0x00, 0xA6, 0xAE, 0xAA, 0x2D, 0x00, 0x00,
        0xF8, 0xF1, 0xFD, 0x00, 0x00, 0xE6, 0x14, 0xFA, 0x00, 0x00, 0xA7, 0xAF, 0xAC, 0xAB, 0x00, 0xA8,
        0x00, 0x00, 0x00, 0x00, 0x8E, 0x8F, 0x92, 0x80, 0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xA5, 0x00, 0x00, 0x00, 0x00, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9A, 0x00, 0x00, 0xE1,
        0x85, 0xA0, 0x83, 0x00, 0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
        0x00, 0xA4, 0x95, 0xA2, 0x93, 0x00, 0x94, 0xF6, 0x00, 0x97, 0xA3, 0x96, 0x81, 0x00, 0x00, 0x98,
    };
    unsigned char c = t[cp - 0xA0];
    return c ? c : '?';
  }
  switch (cp) { /* common cp1252 punctuation */
  case 0x2018:
  case 0x2019:
    return '\'';
  case 0x201C:
  case 0x201D:
    return '"';
  case 0x2013:
  case 0x2014:
    return '-';
  case 0x2026:
    return '.';
    /* Geometric shapes that exist in the CP437 cell font (used for tree toggles) */
  case 0x25B6:
  case 0x25BA:
    return 0x10; /* ► right-pointing triangle */
  case 0x25C0:
  case 0x25C4:
    return 0x11; /* ◄ left-pointing triangle */
  case 0x25B2:
  case 0x25B4:
    return 0x1E; /* ▲ up-pointing triangle */
  case 0x25BC:
  case 0x25BE:
    return 0x1F; /* ▼ down-pointing triangle */
  }
  return '?';
}

/* Monospace text that decodes UTF-8 */
int gfx_text_utf8(int x, int y, const char *s, unsigned fg, unsigned bg)
{
  const char *end = s + strlen(s);
  unsigned cp;
  while (s < end) {
    s += utf8_decode(s, end, &cp);
    gfx_glyph(x, y, uni_to_cp437(cp), fg, bg);
    x += GFX_FONT_W;
  }
  return x;
}

/* Proportional text in any font */
int gfx_font_cw(unsigned cp, const struct gfx_font *f)
{
  struct face_view v;
  if (!font_view(f, &v))
    return GFX_FONT_W; /* no face: the mono cell */
  return view_glyph(&v, cp)->adv + v.bold;
}

int gfx_font_width(const char *s, const struct gfx_font *f)
{
  struct face_view v;
  int have = font_view(f, &v);
  const char *end = s + strlen(s);
  int w = 0;
  unsigned cp;
  while (s < end) {
    s += utf8_decode(s, end, &cp);
    w += have ? view_glyph(&v, cp)->adv + v.bold : GFX_FONT_W;
  }
  return w;
}

int gfx_font_ascent(const struct gfx_font *f)
{
  struct face_view v;
  if (font_view(f, &v))
    return v.baseline;
  struct gfx_font r;
  gfx_font_resolve(f, &r);
  return r.px;
}

int gfx_font_height(const struct gfx_font *f)
{
  struct gfx_font r;
  gfx_font_resolve(f, &r);
  return r.px * 6 / 5;
}

void gfx_font_text(int x, int y, const char *s, unsigned fg, const struct gfx_font *f)
{
  const char *end = s + strlen(s);
  unsigned cp;
  struct face_view v;
  if (font_view(f, &v)) {
    while (s < end) {
      s += utf8_decode(s, end, &cp);
      struct gcache_e *g = view_glyph(&v, cp);
      face_blit(x, y + v.baseline, g, fg, v.bold, v.slant);
      x += g->adv + v.bold;
    }
    return;
  }
  while (s < end) { /* no face: mono fallback */
    s += utf8_decode(s, end, &cp);
    gfx_glyph(x, y, cp < 0x80 ? (unsigned char)cp : '?', fg, GFX_TRANSPARENT);
    x += GFX_FONT_W;
  }
}

#define CLIP_MAX 255 /* longest prefix kept before the ".." */

void gfx_font_text_clip(int x, int y, const char *s, unsigned fg, const struct gfx_font *f, int maxw)
{
  if (maxw <= 0)
    return;
  if (gfx_font_width(s, f) <= maxw) {
    gfx_font_text(x, y, s, fg, f);
    return;
  }
  char buf[CLIP_MAX + 3];
  const char *end = s + strlen(s);
  int n = 0, w = 0, dots = 2 * gfx_font_cw('.', f);
  unsigned cp;
  while (s + n < end) {
    int len = utf8_decode(s + n, end, &cp);
    int cw = gfx_font_cw(cp, f);
    if (n + len > CLIP_MAX || w + cw + dots > maxw)
      break;
    memcpy(buf + n, s + n, (unsigned)len);
    n += len;
    w += cw;
  }
  buf[n] = '.';
  buf[n + 1] = '.';
  buf[n + 2] = '\0';
  gfx_font_text(x, y, buf, fg, f);
}

struct font *gfx_ui_font(void)
{
  struct face_view v;
  return font_view(0, &v) ? v.font : 0;
}

void gfx_ui_text(int x, int y, const char *s, unsigned fg) { gfx_font_text(x, y, s, fg, 0); }
void gfx_ui_text_clip(int x, int y, const char *s, unsigned fg, int maxw) { gfx_font_text_clip(x, y, s, fg, 0, maxw); }
int gfx_ui_width(const char *s) { return gfx_font_width(s, 0); }
int gfx_ui_cw_cp(unsigned cp) { return gfx_font_cw(cp, 0); }
int gfx_ui_cw(unsigned char c) { return gfx_font_cw(c, 0); }

/* GUI widgets */

void status_set(struct status_bar *s, const char *msg)
{
  int i = 0;
  for (; msg[i] && i < (int)sizeof(s->msg) - 1; i++)
    s->msg[i] = msg[i];
  s->msg[i] = '\0';
}

int status_height(const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_STATUS, st);
  return gfx_style_outer_h(st, gfx_style_text_h(st));
}

void status_draw(const struct status_bar *s, int x, int y, int w, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_STATUS, st);
  struct gfx_box b, c;
  gfx_style_box(st, x, y, w, status_height(st), &b);
  gfx_a11y_add(GFX_ROLE_LABEL, s->msg, 0, b.x, b.y, b.w, b.h);
  gfx_style_paint(st, &b, &st->color[BTN_NORMAL]);
  gfx_style_content(st, &b, &c);
  gfx_style_label(st, &c, s->msg, st->color[BTN_NORMAL].fg);
}

/* menus */

#define MENU_CHECK_W 8   /* the checkmark's width; the gutter adds pop->gap */
#define MENU_ARROW_W 8   /* a submenu's arrow cell */
#define MENU_HINT_GAP 16 /* least space between an item and its hint, when the popup fits its items */
#define MENU_OVERLAP 2   /* a submenu overlaps its parent by this much */

/* A small checkmark in a row's left gutter, centred in a row rh tall */
static void menu_check(int x, int y, int rh, unsigned c)
{
  y += (rh - GUI_ROW_H) / 2;
  gfx_fill_rect(x + 0, y + 8, 2, 2, c);
  gfx_fill_rect(x + 1, y + 9, 2, 2, c);
  gfx_fill_rect(x + 2, y + 10, 2, 2, c); /* vertex */
  gfx_fill_rect(x + 3, y + 8, 2, 2, c);
  gfx_fill_rect(x + 4, y + 6, 2, 2, c);
  gfx_fill_rect(x + 5, y + 4, 2, 2, c);
  gfx_fill_rect(x + 6, y + 2, 2, 2, c);
}

static int menu_border(const struct gfx_style *st) { return st->border > 0 ? st->border : 0; }

static int pop_row_h(const struct gfx_style *pop)
{
  if (pop->row_h > 0)
    return pop->row_h;
  struct gfx_font f;
  gfx_style_font(pop, &f);
  return gfx_font_height(&f);
}

static int pop_gutter(const struct menu *m, const struct gfx_style *pop)
{
  return m->flags ? MENU_CHECK_W + pop->gap : 0;
}

/* The popup's border box size */
static int pop_box_w(const struct menu *m, const struct gfx_style *pop)
{
  if (pop->width > 0)
    return pop->width;
  struct gfx_font f;
  gfx_style_font(pop, &f);
  int widest = 0;
  for (int i = 0; i < m->n; i++) {
    int w = gfx_font_width(m->items[i], &f);
    if (m->subs && m->subs[i])
      w += MENU_HINT_GAP + MENU_ARROW_W;
    else if (m->hints && m->hints[i] && m->hints[i][0])
      w += MENU_HINT_GAP + gfx_font_width(m->hints[i], &f);
    if (w > widest)
      widest = w;
  }
  return widest + pop_gutter(m, pop) + pop->padding[GFX_LEFT] + pop->padding[GFX_RIGHT] + 2 * menu_border(pop);
}

static int pop_box_h(const struct menu *m, const struct gfx_style *pop)
{
  if (pop->height > 0)
    return pop->height;
  return m->n * pop_row_h(pop) + pop->padding[GFX_TOP] + pop->padding[GFX_BOTTOM] + 2 * menu_border(pop);
}

int menu_popup_w(const struct menu *m, const struct gfx_style *pop)
{
  pop = gfx_style_of(GFX_W_MENU, pop);
  return gfx_style_outer_w(pop, pop_box_w(m, pop));
}

int menu_popup_h(const struct menu *m, const struct gfx_style *pop)
{
  pop = gfx_style_of(GFX_W_MENU, pop);
  return gfx_style_outer_h(pop, pop_box_h(m, pop));
}

/* The popup whose margin box starts at (x,y): its border and content boxes */
static void pop_boxes(const struct menu *m, const struct gfx_style *pop, int x, int y, struct gfx_box *b,
                      struct gfx_box *c)
{
  gfx_style_box(pop, x, y, gfx_style_outer_w(pop, pop_box_w(m, pop)), gfx_style_outer_h(pop, pop_box_h(m, pop)), b);
  gfx_style_content(pop, b, c);
}

/* Margin-box origin of item i's submenu, so its first row lines up with item i */
static void pop_sub_origin(const struct gfx_style *pop, const struct gfx_box *b, const struct gfx_box *c, int i,
                           int *sx, int *sy)
{
  *sx = b->x + b->w - MENU_OVERLAP - pop->margin[GFX_LEFT];
  *sy = c->y + i * pop_row_h(pop) - (c->y - b->y) - pop->margin[GFX_TOP];
}

/* Draw a popup list (no title bar) with its margin box at (x,y): one row per item, with the check gutter */
static void menu_popup(const struct menu *m, int x, int y, const struct gfx_style *pop)
{
  struct gfx_box b, c;
  pop_boxes(m, pop, x, y, &b, &c);
  int rh = pop_row_h(pop), gutter = pop_gutter(m, pop);
  struct gfx_font f;
  gfx_style_font(pop, &f);
  int fh = gfx_font_height(&f);
  gfx_style_paint(pop, &b, &pop->color[BTN_NORMAL]);
  for (int i = 0; i < m->n; i++) {
    int ry = c.y + i * rh;
    unsigned char fl = m->flags ? m->flags[i] : 0;
    int dis = (fl & MENU_DISABLED) != 0;
    const struct gfx_colors *cs = &pop->color[dis ? BTN_DISABLED : BTN_NORMAL];
    const struct menu *sub = m->subs ? m->subs[i] : 0;
    unsigned st = (dis ? GFX_A11Y_DISABLED : 0) | ((fl & MENU_CHECKED) ? GFX_A11Y_CHECKED : 0) |
                  (sub ? GFX_A11Y_HASPOPUP : 0) | (sub && sub->open ? GFX_A11Y_EXPANDED : 0);
    struct gfx_a11y *a = gfx_a11y_add(GFX_ROLE_MENUITEM, m->items[i], st, b.x, ry, b.w, rh);
    if (!sub && m->hints)
      gfx_a11y_set_shortcut(a, m->hints[i]);
    if (fl & MENU_CHECKED)
      menu_check(c.x, ry, rh, pop->accent[dis ? BTN_DISABLED : BTN_NORMAL].fg);
    int ty = ry + (rh - fh) / 2, right = c.x + c.w;
    if (sub) { /* a right arrow (►) */
      gfx_glyph(right - MENU_ARROW_W + 2, ry + (rh - GFX_FONT_H) / 2, 0x10, cs->fg, GFX_TRANSPARENT);
      right -= MENU_ARROW_W + pop->gap;
    } else if (m->hints && m->hints[i] && m->hints[i][0]) {
      int hw = gfx_font_width(m->hints[i], &f);
      gfx_font_text(right - hw, ty, m->hints[i], cs->detail, &f);
      right -= hw + pop->gap;
    }
    gfx_font_text_clip(c.x + gutter, ty, m->items[i], cs->fg, &f, right - c.x - gutter);
  }
  for (int i = 0; i < m->n; i++)
    if (m->subs && m->subs[i] && m->subs[i]->open) {
      int sx, sy;
      pop_sub_origin(pop, &b, &c, i, &sx, &sy);
      menu_popup(m->subs[i], sx, sy, pop);
    }
}

int menu_bar_h(const struct gfx_style *bar)
{
  bar = gfx_style_of(GFX_W_MENUBAR, bar);
  return gfx_style_outer_h(bar, gfx_style_text_h(bar));
}

/* A title's border box width */
static int title_box_w(const struct menu *m, const struct gfx_style *bar)
{
  if (bar->width > 0)
    return bar->width;
  struct gfx_font f;
  gfx_style_font(bar, &f);
  return gfx_font_width(m->title, &f) + bar->padding[GFX_LEFT] + bar->padding[GFX_RIGHT] + 2 * menu_border(bar);
}

int menu_title_w(const struct menu *m, const struct gfx_style *bar)
{
  bar = gfx_style_of(GFX_W_MENUBAR, bar);
  return gfx_style_outer_w(bar, title_box_w(m, bar));
}

void menu_draw(const struct menu *m, int x, int y, const struct gfx_style *bar, const struct gfx_style *pop)
{
  bar = gfx_style_of(GFX_W_MENUBAR, bar);
  pop = gfx_style_of(GFX_W_MENU, pop);
  struct gfx_box b, c;
  gfx_style_box(bar, x, y, menu_title_w(m, bar), menu_bar_h(bar), &b);
  gfx_a11y_add(GFX_ROLE_MENU, m->title, GFX_A11Y_HASPOPUP | (m->open ? GFX_A11Y_EXPANDED : 0), b.x, b.y, b.w, b.h);
  int state = m->open ? BTN_PRESSED : BTN_NORMAL;
  if (m->open) /* a closed title sits on the app's bar */
    gfx_style_paint(bar, &b, &bar->color[state]);
  gfx_style_content(bar, &b, &c);
  gfx_style_label(bar, &c, m->title, bar->color[state].fg);
  if (m->open)
    menu_popup(m, x, y + menu_bar_h(bar), pop);
}

void menu_popup_draw(const struct menu *m, int x, int y, const struct gfx_style *pop)
{
  menu_popup(m, x, y, gfx_style_of(GFX_W_MENU, pop));
}

/* Item index under (mx,my) in the popup at (x,y): -1 off the popup, -2 on it but between rows */
static int pop_row_at(const struct menu *m, const struct gfx_style *pop, int x, int y, int mx, int my)
{
  struct gfx_box b, c;
  pop_boxes(m, pop, x, y, &b, &c);
  if (mx < b.x || mx >= b.x + b.w || my < b.y || my >= b.y + b.h)
    return -1;
  if (my < c.y)
    return -2;
  int i = (my - c.y) / pop_row_h(pop);
  return i < m->n ? i : -2;
}

int menu_popup_click(const struct menu *m, int mx, int my, int x, int y, const struct gfx_style *pop)
{
  int i = pop_row_at(m, gfx_style_of(GFX_W_MENU, pop), x, y, mx, my);
  if (i < 0)
    return -1;
  return m->flags && (m->flags[i] & MENU_DISABLED) ? -1 : i;
}

/* Recursively close a menu's submenus (used when the menu itself closes) */
static void menu_subs_close(struct menu *m)
{
  if (!m->subs)
    return;
  for (int i = 0; i < m->n; i++)
    if (m->subs[i]) {
      m->subs[i]->open = 0;
      menu_subs_close(m->subs[i]);
    }
}

/* Route a click in m's popup, recursing into open submenus first */
static int menu_popup_hit(struct menu *m, int px, int py, int mx, int my, struct menu **fm, int *fi,
                          const struct gfx_style *pop)
{
  struct gfx_box b, c;
  pop_boxes(m, pop, px, py, &b, &c);
  for (int i = 0; i < m->n; i++)
    if (m->subs && m->subs[i] && m->subs[i]->open) {
      int sx, sy;
      pop_sub_origin(pop, &b, &c, i, &sx, &sy);
      int r = menu_popup_hit(m->subs[i], sx, sy, mx, my, fm, fi, pop);
      if (r)
        return r;
    }
  int i = pop_row_at(m, pop, px, py, mx, my);
  if (i == -1)
    return 0;
  if (i == -2)
    return 1; /* the popup's frame: swallow, stay open */
  unsigned char fl = m->flags ? m->flags[i] : 0;
  if (fl & MENU_DISABLED)
    return 1;                  /* inert: swallow, stay open */
  if (m->subs && m->subs[i]) { /* parent: toggle its child */
    int wasopen = m->subs[i]->open;
    for (int k = 0; k < m->n; k++)
      if (m->subs[k])
        m->subs[k]->open = 0;
    m->subs[i]->open = !wasopen;
    return 1;
  }
  *fm = m;
  *fi = i;
  return 2;
}

int menu_click_ex(struct menu *m, int mx, int my, int x, int y, struct menu **leaf, const struct gfx_style *bar,
                  const struct gfx_style *pop)
{
  bar = gfx_style_of(GFX_W_MENUBAR, bar);
  pop = gfx_style_of(GFX_W_MENU, pop);
  if (leaf)
    *leaf = 0;
  int bh = menu_bar_h(bar);
  if (my >= y && my < y + bh) { /* the title row */
    m->open = (mx >= x && mx < x + menu_title_w(m, bar)) ? !m->open : 0;
    if (!m->open)
      menu_subs_close(m);
    return -1;
  }
  if (m->open) {
    struct menu *fm = 0;
    int fi = -1;
    int r = menu_popup_hit(m, x, y + bh, mx, my, &fm, &fi, pop);
    if (r == 2) { /* a leaf fired -> close everything */
      m->open = 0;
      menu_subs_close(m);
      if (leaf)
        *leaf = fm;
      return fi;
    }
    if (r == 1) /* opened submenu / disabled: stay */
      return -1;
    m->open = 0; /* clicked outside -> dismiss */
    menu_subs_close(m);
  }
  return -1;
}

int menu_click(struct menu *m, int mx, int my, int x, int y, const struct gfx_style *bar, const struct gfx_style *pop)
{
  struct menu *leaf;
  int it = menu_click_ex(m, mx, my, x, y, &leaf, bar, pop);
  return (leaf == m) ? it : -1; /* only this menu's items */
}

/* Search m (and its submenus) for an enabled item whose accelerator is `key` */
static int menu_shortcut_walk(const struct menu *m, int key, struct menu **leaf)
{
  if (m->keys)
    for (int i = 0; i < m->n; i++) {
      unsigned char fl = m->flags ? m->flags[i] : 0;
      if (m->keys[i] == key && !(fl & MENU_DISABLED) && !(m->subs && m->subs[i])) {
        if (leaf)
          *leaf = (struct menu *)m;
        return i;
      }
    }
  if (m->subs)
    for (int i = 0; i < m->n; i++)
      if (m->subs[i]) {
        int it = menu_shortcut_walk(m->subs[i], key, leaf);
        if (it >= 0)
          return it;
      }
  return -1;
}

int menu_shortcut(const struct menu *m, int key)
{
  if (!key)
    return -1;
  struct menu *leaf = 0;
  int it = menu_shortcut_walk(m, key, &leaf);
  return (leaf == m) ? it : -1; /* only this menu's items */
}

/* menu bar */

/* x of menu i's title: the bar origin plus the titles before it and the gaps between them */
static int menubar_title_x(const struct menubar *mb, int i, const struct gfx_style *bar)
{
  int x = mb->x;
  for (int k = 0; k < i; k++)
    x += menu_title_w(mb->menus[k], bar) + bar->gap;
  return x;
}

void menubar_close(struct menubar *mb)
{
  for (int i = 0; i < mb->n; i++) {
    mb->menus[i]->open = 0;
    menu_subs_close(mb->menus[i]);
  }
}

void menubar_draw(const struct menubar *mb, const struct gfx_style *bar, const struct gfx_style *pop)
{
  bar = gfx_style_of(GFX_W_MENUBAR, bar);
  /* Closed titles first; the open one last so its dropdown overlays a neighbouring title */
  int openi = -1;
  for (int i = 0; i < mb->n; i++) {
    if (mb->menus[i]->open) {
      openi = i;
      continue;
    }
    menu_draw(mb->menus[i], menubar_title_x(mb, i, bar), mb->y, bar, pop);
  }
  if (openi >= 0)
    menu_draw(mb->menus[openi], menubar_title_x(mb, openi, bar), mb->y, bar, pop);
}

int menubar_click(struct menubar *mb, int mx, int my, struct menu **leaf, int *item, const struct gfx_style *bar,
                  const struct gfx_style *pop)
{
  bar = gfx_style_of(GFX_W_MENUBAR, bar);
  *leaf = 0;
  *item = -1;

  /* On the title row: toggle the clicked title and close the others */
  if (my >= mb->y && my < mb->y + menu_bar_h(bar)) {
    for (int i = 0; i < mb->n; i++) {
      int x = menubar_title_x(mb, i, bar);
      if (mx >= x && mx < x + menu_title_w(mb->menus[i], bar)) {
        int wasopen = mb->menus[i]->open;
        menubar_close(mb);
        mb->menus[i]->open = !wasopen;
        return 1;
      }
    }
    menubar_close(mb); /* empty bar area: dismiss any open menu */
    return 1;
  }

  /* Below the bar: hand the click to whichever menu is open */
  for (int i = 0; i < mb->n; i++) {
    if (!mb->menus[i]->open)
      continue;
    struct menu *lf = 0;
    int it = menu_click_ex(mb->menus[i], mx, my, menubar_title_x(mb, i, bar), mb->y, &lf, bar, pop);
    if (it >= 0) {
      *leaf = lf;
      *item = it;
    }
    return 1;
  }
  return 0;
}

int menubar_shortcut(const struct menubar *mb, int key, struct menu **leaf)
{
  if (!key) {
    if (leaf)
      *leaf = 0;
    return -1;
  }
  for (int i = 0; i < mb->n; i++) {
    struct menu *lf = 0;
    int it = menu_shortcut_walk(mb->menus[i], key, &lf);
    if (it >= 0) {
      if (leaf)
        *leaf = lf;
      return it;
    }
  }
  if (leaf)
    *leaf = 0;
  return -1;
}

/* button */

void button_draw(int x, int y, int w, int h, const char *label, int state, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_BUTTON, st);
  struct gfx_box b, c;
  gfx_style_box(st, x, y, w, h, &b);
  gfx_a11y_add(GFX_ROLE_BUTTON, label, gfx_a11y_state(state), b.x, b.y, b.w, b.h);
  int focused = state & GFX_FOCUSED;
  state = gfx_state_index(state);
  if (focused)
    gfx_focus_ring(b.x, b.y, b.w, b.h, st);
  gfx_style_paint(st, &b, &st->color[state]);
  gfx_style_content(st, &b, &c);
  if (state == BTN_PRESSED) /* the label sinks a pixel */
    c.y++;
  gfx_style_label(st, &c, label, st->color[state].fg);
}

int button_hit(int x, int y, int w, int h, int mx, int my, const struct gfx_style *st)
{
  struct gfx_box b;
  gfx_style_box(gfx_style_of(GFX_W_BUTTON, st), x, y, w, h, &b);
  return mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h;
}

/* progress bar */

void progress_draw(int x, int y, int w, int h, int value, int max, int show_pct, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_PROGRESS, st);
  if (max < 0)
    max = 0;
  if (value < 0)
    value = 0;
  if (value > max)
    value = max;
  int pct = max > 0 ? value * 100 / max : 0;
  struct gfx_box b, c;
  gfx_style_box(st, x, y, w, h, &b);
  gfx_style_content(st, &b, &c);
  gfx_a11y_set_value(gfx_a11y_add(GFX_ROLE_PROGRESS, 0, 0, b.x, b.y, b.w, b.h), value, 0, max);

  const struct gfx_colors *trough = &st->color[BTN_NORMAL], *fill = &st->accent[BTN_NORMAL];
  int bw = st->border > 0 ? st->border : 0;
  gfx_fill_round(b.x, b.y, b.w, b.h, st->radius, trough->bg);
  int fw = max > 0 && c.w > 0 ? (int)((long)c.w * value / max) : 0;
  if (fw > 0) {
    gfx_fill_round(c.x, c.y, fw, c.h, st->radius - bw, fill->bg);
    if (c.h > 1 && fill->detail != fill->bg) /* lit top edge */
      gfx_fill_rect(c.x, c.y, fw, 1, fill->detail);
  }
  if (bw)
    gfx_round_rect(b.x, b.y, b.w, b.h, st->radius, bw, trough->border);

  if (show_pct) {
    char buf[5]; /* "100%" + NUL */
    int p = pct, k = 0;
    if (p >= 100) {
      buf[k++] = '1';
      buf[k++] = '0';
      buf[k++] = '0';
    } else {
      if (p >= 10)
        buf[k++] = '0' + p / 10;
      buf[k++] = '0' + p % 10;
    }
    buf[k++] = '%';
    buf[k] = '\0';
    struct gfx_font f;
    gfx_style_font(st, &f);
    int tw = gfx_font_width(buf, &f);
    int tx = b.x + (b.w - tw) / 2;
    int ty = b.y + (b.h - gfx_font_height(&f)) / 2;
    /* The fill's colour once it reaches the middle, the trough's before */
    unsigned fg = (b.x + b.w / 2 < c.x + fw) ? fill->fg : trough->fg;
    gfx_font_text(tx, ty, buf, fg, &f);
  }
}

/* slider */

#define SLIDER_GRAB 4 /* extra hit margin around the box */

struct slider_geom {
  struct gfx_box b; /* border box */
  int thumb_w, thumb_h, track;
};

static void slider_geom(const struct gfx_style *st, int x, int y, int w, struct slider_geom *g)
{
  g->thumb_w = st->part_w > 0 ? st->part_w : 6;
  g->thumb_h = st->part_h > 0 ? st->part_h : GUI_SLIDER_H;
  g->track = st->track > 0 ? st->track : 6;
  gfx_style_box(st, x, y, w, slider_height(st), &g->b);
}

int slider_height(const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_SLIDER, st);
  int h = st->height > 0 ? st->height : st->part_h > 0 ? st->part_h : GUI_SLIDER_H;
  return gfx_style_outer_h(st, h);
}

static int slider_travel(const struct slider_geom *g) { return g->b.w - g->thumb_w > 1 ? g->b.w - g->thumb_w : 1; }

void slider_draw(int x, int y, int w, int value, int max, int focused, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_SLIDER, st);
  struct slider_geom g;
  slider_geom(st, x, y, w, &g);
  if (value < 0)
    value = 0;
  if (value > max)
    value = max;
  struct gfx_a11y *a = gfx_a11y_add(GFX_ROLE_SLIDER, 0, focused ? GFX_A11Y_FOCUSED : 0, g.b.x, g.b.y, g.b.w, g.b.h);
  gfx_a11y_set_value(a, value, 0, max);
  if (focused)
    gfx_focus_ring(g.b.x, g.b.y, g.b.w, g.b.h, st);
  const struct gfx_colors *track = &st->color[BTN_NORMAL], *on = &st->accent[BTN_NORMAL];
  int bw = st->border > 0 ? st->border : 0;
  int tx = g.b.x + (max > 0 ? (int)((long)slider_travel(&g) * value / max) : 0);
  int ty = g.b.y + (g.b.h - g.track) / 2;
  gfx_fill_round(g.b.x, ty, g.b.w, g.track, st->radius, track->bg);
  gfx_fill_round(g.b.x, ty, tx - g.b.x + g.thumb_w / 2, g.track, st->radius, on->bg);
  if (bw)
    gfx_round_rect(g.b.x, ty, g.b.w, g.track, st->radius, bw, track->border);
  gfx_fill_round(tx, g.b.y + (g.b.h - g.thumb_h) / 2, g.thumb_w, g.thumb_h, st->radius, on->fg);
}

int slider_hit(int x, int y, int w, int mx, int my, const struct gfx_style *st)
{
  struct slider_geom g;
  slider_geom(gfx_style_of(GFX_W_SLIDER, st), x, y, w, &g);
  return mx >= g.b.x - SLIDER_GRAB && mx < g.b.x + g.b.w + SLIDER_GRAB && my >= g.b.y - SLIDER_GRAB &&
         my < g.b.y + g.b.h + SLIDER_GRAB;
}

int slider_value(int x, int w, int mx, int max, const struct gfx_style *st)
{
  struct slider_geom g;
  slider_geom(gfx_style_of(GFX_W_SLIDER, st), x, 0, w, &g);
  int travel = slider_travel(&g);
  long v = ((long)(mx - g.b.x - g.thumb_w / 2) * max + travel / 2) / travel;
  return v < 0 ? 0 : v > max ? max : (int)v;
}

/* scrollbar */

static int scroll_min_thumb(const struct gfx_style *st) { return st->row_h > 0 ? st->row_h : 12; }

/* Square arrow-button height for a w-wide, h-tall bar */
static int scrollbar_arrow_h(int w, int h, const struct gfx_style *st)
{
  int a = w;
  if ((st->flags & GFX_STYLE_NO_ARROWS) || a < 4 || h < 2 * a + scroll_min_thumb(st))
    return 0;
  return a;
}

/* Thumb height + top offset within a track of height `tr` for the current counts */
static void scrollbar_thumb(int tr, int scroll, int total, int visible, int min_thumb, int *th, int *ty)
{
  int t = (int)((long)tr * visible / total);
  if (t < min_thumb)
    t = min_thumb;
  if (t > tr)
    t = tr;
  int maxs = total - visible;
  if (scroll < 0)
    scroll = 0;
  if (scroll > maxs)
    scroll = maxs;
  int span = tr - t;
  *th = t;
  *ty = (maxs > 0) ? (int)((long)span * scroll / maxs) : 0;
}

/* A filled triangle centred in the arrow box [bx,by,w,a], pointing up or down */
static void scrollbar_tri(int bx, int by, int w, int a, int up, unsigned color)
{
  int sz = (w - 2) / 2;
  if (sz < 3)
    sz = 3;
  if (sz > a / 2)
    sz = a / 2;
  if (sz < 1)
    sz = 1;
  int cx = bx + w / 2;
  int top = by + (a - sz) / 2;
  for (int r = 0; r < sz; r++) {
    int half = up ? r : (sz - 1 - r);
    gfx_fill_rect(cx - half, top + r, half * 2 + 1, 1, color);
  }
}

void scrollbar_draw(int x, int y, int w, int h, int scroll, int total, int visible, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_SCROLLBAR, st);
  struct gfx_box b;
  gfx_style_box(st, x, y, w, h, &b);
  int a = scrollbar_arrow_h(b.w, b.h, st);
  int tr = b.h - 2 * a; /* track region height */
  const struct gfx_colors *c = &st->color[BTN_NORMAL];
  struct gfx_a11y *node = gfx_a11y_add(GFX_ROLE_SCROLLBAR, 0, 0, b.x, b.y, b.w, b.h);
  gfx_a11y_set_value(node, scroll, 0, total > visible ? total - visible : 0);

  gfx_fill_rect(b.x, b.y, b.w, b.h, c->bg);
  if (a > 0) {
    gfx_fill_rect(b.x, b.y, b.w, a, c->detail);           /* up box */
    gfx_fill_rect(b.x, b.y + b.h - a, b.w, a, c->detail); /* down box */
    scrollbar_tri(b.x, b.y, b.w, a, 1, c->fg);
    scrollbar_tri(b.x, b.y + b.h - a, b.w, a, 0, c->fg);
  }
  int il = st->padding[GFX_LEFT], tw = b.w - il - st->padding[GFX_RIGHT];
  if (total > visible && visible > 0 && tr > 0 && tw > 0) {
    int th, ty;
    scrollbar_thumb(tr, scroll, total, visible, scroll_min_thumb(st), &th, &ty);
    gfx_fill_round(b.x + il, b.y + a + ty, tw, th, st->radius, st->accent[BTN_NORMAL].bg);
  }
}

int scrollbar_hit(int x, int y, int w, int h, int mx, int my, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_SCROLLBAR, st);
  struct gfx_box b;
  gfx_style_box(st, x, y, w, h, &b);
  if (mx < b.x || mx >= b.x + b.w || my < b.y || my >= b.y + b.h)
    return SCROLLBAR_NONE;
  int a = scrollbar_arrow_h(b.w, b.h, st);
  if (a > 0) {
    if (my < b.y + a)
      return SCROLLBAR_UP;
    if (my >= b.y + b.h - a)
      return SCROLLBAR_DOWN;
  }
  return SCROLLBAR_TRACK;
}

int scrollbar_pos(int x, int y, int w, int h, int mx, int my, int scroll, int total, int visible,
                  const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_SCROLLBAR, st);
  if (scrollbar_hit(x, y, w, h, mx, my, st) != SCROLLBAR_TRACK)
    return scroll;
  int maxs = total - visible;
  if (maxs <= 0)
    return 0;
  struct gfx_box b;
  gfx_style_box(st, x, y, w, h, &b);
  int a = scrollbar_arrow_h(b.w, b.h, st);
  int tr = b.h - 2 * a; /* track region height */
  if (tr <= 0)
    return scroll;
  int th, ty;
  scrollbar_thumb(tr, scroll, total, visible, scroll_min_thumb(st), &th, &ty);
  int span = tr - th;
  int rel = my - (b.y + a) - th / 2; /* centre the thumb on the cursor */
  int s = (span > 0) ? (int)((long)rel * maxs / span) : 0;
  if (s < 0)
    s = 0;
  if (s > maxs)
    s = maxs;
  return s;
}

void gfx_get_clip(int *x, int *y, int *w, int *h)
{
  *x = g_cx0;
  *y = g_cy0;
  *w = g_cx1 - g_cx0;
  *h = g_cy1 - g_cy0;
}

void gfx_cursor(int win, int shape)
{
  static int last_win = -1, last_shape = -1;
  if (win == last_win && shape == last_shape)
    return;
  if (win_cursor(win, shape) == 0) {
    last_win = win;
    last_shape = shape;
  }
}
