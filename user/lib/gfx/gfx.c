/* gfx.c: user-space drawing into a window's shared pixel buffer. See gfx.h */
#include "gfx.h"
#include "font.h" /* scalable fonts, loaded at runtime (no baked atlas) */
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
#define FACE_CACHE 512
/* No member initialisers here */
struct gfx_face {
  struct font *font;
  int px, baseline, tried;
  struct gcache_e cache[FACE_CACHE];
};
static struct gfx_face g_sans, g_mono;

#define SANS_PATH "/etc/fonts/LiberationSans-Regular.ttf"
#define SANS_PX 15
#define MONO_PATH "/etc/fonts/LiberationMono-Regular.ttf"
#define MONO_PX 13

static int face_ready(struct gfx_face *f, const char *path, int px)
{
  if (!f->tried) {
    f->tried = 1;
    f->px = px;
    struct statinfo st;
    if (stat(path, &st) < 0 || st.is_dir || st.size == 0)
      return 0;
    unsigned char *buf = malloc(st.size); /* kept for the font's lifetime */
    if (!buf)
      return 0;
    if (readfile_at(path, 0, buf, st.size) != (int)st.size) {
      free(buf);
      return 0;
    }
    f->font = font_load(buf, (int)st.size);
    if (!f->font) {
      free(buf);
      return 0;
    }
    font_vmetrics(f->font, (float)px, &f->baseline, 0, 0);
  }
  return f->font != 0;
}

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

static struct gcache_e *face_glyph(struct gfx_face *f, unsigned cp)
{
  return cache_glyph(f->font, f->px, f->cache, FACE_CACHE, cp);
}

/* Per-size sans cache pool (e.g. headings), LRU-reclaimed; g_sans is pinned at SANS_PX */
#define SIZED_FACES 6
#define SIZED_CACHE 256
struct sized_face {
  int px, baseline;
  unsigned lru;
  struct gcache_e cache[SIZED_CACHE];
};
static struct sized_face g_sized[SIZED_FACES];
static unsigned g_lru_tick;

/* Return the pool slot serving `px` (g_sans.font assumed loaded) */
static struct sized_face *sized_slot(int px)
{
  int empty = -1, lru = 0;
  for (int i = 0; i < SIZED_FACES; i++) {
    if (g_sized[i].px == px) {
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
  s->px = px;
  font_vmetrics(g_sans.font, (float)px, &s->baseline, 0, 0);
  s->lru = ++g_lru_tick;
  return s;
}

/* A resolved sans face at some px: the shared font, its baseline, and the glyph cache to use */
struct face_view {
  struct font *font;
  int px, baseline;
  struct gcache_e *cache;
  int n;
};

static int sans_view(int px, struct face_view *v)
{
  if (px <= 0)
    px = SANS_PX;
  if (!face_ready(&g_sans, SANS_PATH, SANS_PX))
    return 0;
  if (px == SANS_PX) {
    v->font = g_sans.font;
    v->px = SANS_PX;
    v->baseline = g_sans.baseline;
    v->cache = g_sans.cache;
    v->n = FACE_CACHE;
  } else {
    struct sized_face *s = sized_slot(px);
    v->font = g_sans.font;
    v->px = px;
    v->baseline = s->baseline;
    v->cache = s->cache;
    v->n = SIZED_CACHE;
  }
  return 1;
}

/* Blit a cached glyph with its top-left at (x + ox, baseline + oy) */
/* Blit one glyph at `baseline`, additionally clipping rows to [ct,cb) */
static void face_blit_clip(int x, int baseline, const struct gcache_e *g, unsigned fg, int ct, int cb)
{
  if (!g->bm)
    return;
  for (int gy = 0; gy < g->h; gy++) {
    int sy = baseline + g->oy + gy;
    if ((unsigned)sy >= (unsigned)g_h)
      continue;
    if (sy < g_cy0 || sy >= g_cy1)
      continue;
    if (ct < cb && (sy < ct || sy >= cb))
      continue;
    const unsigned char *srow = g->bm + gy * g->w;
    for (int gx = 0; gx < g->w; gx++) {
      int sx = x + g->ox + gx;
      if ((unsigned)sx >= (unsigned)g_w)
        continue;
      if (sx < g_cx0 || sx >= g_cx1)
        continue;
      unsigned a = srow[gx];
      if (a) {
        unsigned *px = &g_fb[sy * g_w + sx];
        *px = gfx_blend(fg, *px, a);
      }
    }
  }
}

static void face_blit(int x, int baseline, const struct gcache_e *g, unsigned fg)
{
  face_blit_clip(x, baseline, g, fg, 0, 0);
}

/* One monospace cell: optional opaque background */
void gfx_glyph(int x, int y, unsigned char ch, unsigned fg, unsigned bg)
{
  if (!(bg & GFX_TRANSPARENT))
    gfx_fill_rect(x, y, GFX_FONT_W, GFX_FONT_H, bg);
  if (!face_ready(&g_mono, MONO_PATH, MONO_PX))
    return;
  face_blit(x, y + g_mono.baseline, face_glyph(&g_mono, ch), fg);
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

/* Proportional UI text (UTF-8) */
int gfx_ui_cw_cp_px(unsigned cp, int px)
{
  struct face_view v;
  if (!sans_view(px, &v))
    return GFX_FONT_W; /* sans missing: mono cell */
  return cache_glyph(v.font, v.px, v.cache, v.n, cp)->adv;
}
int gfx_ui_cw_cp(unsigned cp) { return gfx_ui_cw_cp_px(cp, SANS_PX); }
int gfx_ui_cw(unsigned char c) { return gfx_ui_cw_cp(c); }

int gfx_ui_width_px(const char *s, int px)
{
  const char *end = s + strlen(s);
  int w = 0;
  unsigned cp;
  while (s < end) {
    s += utf8_decode(s, end, &cp);
    w += gfx_ui_cw_cp_px(cp, px);
  }
  return w;
}
int gfx_ui_width(const char *s) { return gfx_ui_width_px(s, SANS_PX); }

#define CLIP_MAX 255 /* longest prefix kept before the ".." */

void gfx_ui_text_clip(int x, int y, const char *s, unsigned fg, int maxw)
{
  if (maxw <= 0)
    return;
  if (gfx_ui_width(s) <= maxw) {
    gfx_ui_text(x, y, s, fg);
    return;
  }
  char buf[CLIP_MAX + 3];
  const char *end = s + strlen(s);
  int n = 0, w = 0, dots = 2 * gfx_ui_cw('.');
  unsigned cp;
  for (;;) {
    int len = utf8_decode(s + n, end, &cp);
    int cw = gfx_ui_cw_cp(cp);
    if (n + len > CLIP_MAX || w + cw + dots > maxw)
      break;
    memcpy(buf + n, s + n, (unsigned)len);
    n += len;
    w += cw;
  }
  buf[n] = '.';
  buf[n + 1] = '.';
  buf[n + 2] = '\0';
  gfx_ui_text(x, y, buf, fg);
}

int gfx_ui_ascent_px(int px)
{
  struct face_view v;
  if (!sans_view(px, &v))
    return px <= 0 ? SANS_PX : px;
  return v.baseline;
}

/* Proportional UTF-8 text at any pixel size */
/* Sized UI text, rows clipped to [ct,cb); ct>=cb disables the vertical clip */
void gfx_ui_text_px_clip(int x, int y, const char *s, unsigned fg, int px, int ct, int cb)
{
  const char *end = s + strlen(s);
  unsigned cp;
  struct face_view v;
  if (sans_view(px, &v)) {
    while (s < end) {
      s += utf8_decode(s, end, &cp);
      struct gcache_e *g = cache_glyph(v.font, v.px, v.cache, v.n, cp);
      face_blit_clip(x, y + v.baseline, g, fg, ct, cb);
      x += g->adv;
    }
    return;
  }
  while (s < end) { /* sans missing: mono fallback */
    s += utf8_decode(s, end, &cp);
    gfx_glyph(x, y, cp < 0x80 ? (unsigned char)cp : '?', fg, GFX_TRANSPARENT);
    x += GFX_FONT_W;
  }
}

void gfx_ui_text_px(int x, int y, const char *s, unsigned fg, int px) { gfx_ui_text_px_clip(x, y, s, fg, px, 0, 0); }

void gfx_ui_text(int x, int y, const char *s, unsigned fg) { gfx_ui_text_px(x, y, s, fg, SANS_PX); }

/* GUI widgets */

void status_set(struct status_bar *s, const char *msg)
{
  int i = 0;
  for (; msg[i] && i < (int)sizeof(s->msg) - 1; i++)
    s->msg[i] = msg[i];
  s->msg[i] = '\0';
}

void status_draw(const struct status_bar *s, int x, int y, int w, unsigned fg, unsigned bg)
{
  gfx_fill_rect(x, y, w, GUI_BAR_H, bg);
  gfx_ui_text(x + 6, y + (GUI_BAR_H - UIFONT_H) / 2, s->msg, fg);
}

#define MENU_BG 0x2a3342u  /* dropdown background */
#define MENU_FG 0xe0e6eeu  /* enabled item text */
#define MENU_DIM 0x636b78u /* disabled item text / dimmed hint */

/* A small checkmark drawn in a row's left gutter (the cell is GUI_ROW_H tall) */
static void menu_check(int x, int y, unsigned c)
{
  gfx_fill_rect(x + 0, y + 8, 2, 2, c);
  gfx_fill_rect(x + 1, y + 9, 2, 2, c);
  gfx_fill_rect(x + 2, y + 10, 2, 2, c); /* vertex */
  gfx_fill_rect(x + 3, y + 8, 2, 2, c);
  gfx_fill_rect(x + 4, y + 6, 2, 2, c);
  gfx_fill_rect(x + 5, y + 4, 2, 2, c);
  gfx_fill_rect(x + 6, y + 2, 2, 2, c);
}

/* Draw a popup list (no title bar) at (px,py): one row per item, with the check gutter */
static void menu_popup(const struct menu *m, int px, int py)
{
  int gutter = m->flags ? 10 : 0; /* reserve a check column */
  gfx_fill_rect(px, py, GUI_MENU_W, m->n * GUI_ROW_H, MENU_BG);
  for (int i = 0; i < m->n; i++) {
    int ry = py + i * GUI_ROW_H;
    unsigned char fl = m->flags ? m->flags[i] : 0;
    int dis = (fl & MENU_DISABLED) != 0;
    unsigned fg = dis ? MENU_DIM : MENU_FG;
    if (fl & MENU_CHECKED)
      menu_check(px + 2, ry, dis ? MENU_DIM : 0x8fd98f);
    gfx_ui_text(px + 6 + gutter, ry, m->items[i], fg);
    if (m->subs && m->subs[i]) /* submenu: a right arrow (►) */
      gfx_glyph(px + GUI_MENU_W - 12, ry, 0x10, fg, MENU_BG);
    else if (m->hints && m->hints[i] && m->hints[i][0]) {
      int hw = gfx_ui_width(m->hints[i]);
      gfx_ui_text(px + GUI_MENU_W - 6 - hw, ry, m->hints[i], dis ? MENU_DIM : 0x8a93a0);
    }
  }
  gfx_rect(px, py, GUI_MENU_W, m->n * GUI_ROW_H, 0x05070a);
  for (int i = 0; i < m->n; i++)
    if (m->subs && m->subs[i] && m->subs[i]->open)
      menu_popup(m->subs[i], px + GUI_MENU_W - 2, py + i * GUI_ROW_H);
}

void menu_draw(const struct menu *m, int x, int y)
{
  unsigned bg = m->open ? 0x3a4a60 : 0x232a36;
  int tw = gfx_ui_width(m->title) + 6;
  if (m->open)
    gfx_fill_rect(x, y, tw, GUI_BAR_H, bg);
  gfx_ui_text(x + 3, y + (GUI_BAR_H - UIFONT_H) / 2, m->title, 0xe0e6ee);
  if (m->open)
    menu_popup(m, x, y + GUI_BAR_H);
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
static int menu_popup_hit(struct menu *m, int px, int py, int mx, int my, struct menu **fm, int *fi)
{
  for (int i = 0; i < m->n; i++)
    if (m->subs && m->subs[i] && m->subs[i]->open) {
      int r = menu_popup_hit(m->subs[i], px + GUI_MENU_W - 2, py + i * GUI_ROW_H, mx, my, fm, fi);
      if (r)
        return r;
    }
  if (mx < px || mx >= px + GUI_MENU_W || my < py || my >= py + m->n * GUI_ROW_H)
    return 0;
  int i = (my - py) / GUI_ROW_H;
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

int menu_click_ex(struct menu *m, int mx, int my, int x, int y, struct menu **leaf)
{
  if (leaf)
    *leaf = 0;
  int tw = gfx_ui_width(m->title) + 6;
  if (my >= y && my < y + GUI_BAR_H) { /* the title row */
    m->open = (mx >= x && mx < x + tw) ? !m->open : 0;
    if (!m->open)
      menu_subs_close(m);
    return -1;
  }
  if (m->open) {
    struct menu *fm = 0;
    int fi = -1;
    int r = menu_popup_hit(m, x, y + GUI_BAR_H, mx, my, &fm, &fi);
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

int menu_click(struct menu *m, int mx, int my, int x, int y)
{
  struct menu *leaf;
  int it = menu_click_ex(m, mx, my, x, y, &leaf);
  return (leaf == m) ? it : -1; /* back-compat: only this menu's items */
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
  return (leaf == m) ? it : -1; /* back-compat: only this menu's items */
}

/* menu bar */

#define MENUBAR_GAP 4 /* px between adjacent titles on the bar */

/* x of menu i's title: the bar origin plus the widths of the titles before it */
static int menubar_title_x(const struct menubar *mb, int i)
{
  int x = mb->x;
  for (int k = 0; k < i; k++)
    x += gfx_ui_width(mb->menus[k]->title) + 6 + MENUBAR_GAP;
  return x;
}

void menubar_close(struct menubar *mb)
{
  for (int i = 0; i < mb->n; i++) {
    mb->menus[i]->open = 0;
    menu_subs_close(mb->menus[i]);
  }
}

void menubar_draw(const struct menubar *mb)
{
  /* Closed titles first; the open one last so its dropdown overlays a neighbouring title */
  int openi = -1;
  for (int i = 0; i < mb->n; i++) {
    if (mb->menus[i]->open) {
      openi = i;
      continue;
    }
    menu_draw(mb->menus[i], menubar_title_x(mb, i), mb->y);
  }
  if (openi >= 0)
    menu_draw(mb->menus[openi], menubar_title_x(mb, openi), mb->y);
}

int menubar_click(struct menubar *mb, int mx, int my, struct menu **leaf, int *item)
{
  *leaf = 0;
  *item = -1;

  /* On the title row: toggle the clicked title and close the others */
  if (my >= mb->y && my < mb->y + GUI_BAR_H) {
    for (int i = 0; i < mb->n; i++) {
      int x = menubar_title_x(mb, i);
      int tw = gfx_ui_width(mb->menus[i]->title) + 6;
      if (mx >= x && mx < x + tw) {
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
    int it = menu_click_ex(mb->menus[i], mx, my, menubar_title_x(mb, i), mb->y, &lf);
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

/* text field */

#define TF_BG 0xffffffu     /* field background */
#define TF_FG 0x101418u     /* text colour */
#define TF_BORDER 0x3a3f48u /* idle border */
#define TF_FOCUS 0x2f6bd8u  /* focused border accent + caret */
#define TF_PAD 4            /* inner horizontal padding (px) */

void textfield_init(struct textfield *tf, char *buf, int cap)
{
  tf->buf = buf;
  tf->cap = cap;
  tf->len = 0;
  tf->caret = 0;
  tf->scroll = 0;
  if (cap > 0)
    buf[0] = '\0';
}

void textfield_set(struct textfield *tf, const char *s)
{
  int i = 0;
  for (; s[i] && i < tf->cap - 1; i++)
    tf->buf[i] = s[i];
  tf->buf[i] = '\0';
  tf->len = i;
  tf->caret = i;
  tf->scroll = 0;
}

/* Pixel width of buf[a..b) in the proportional UI font */
static int tf_span(const struct textfield *tf, int a, int b)
{
  int w = 0;
  for (int i = a; i < b; i++)
    w += gfx_ui_cw((unsigned char)tf->buf[i]);
  return w;
}

void textfield_draw(struct textfield *tf, int x, int y, int w, int h, int focused)
{
  gfx_fill_rect(x, y, w, h, TF_BG);
  gfx_rect(x, y, w, h, focused ? TF_FOCUS : TF_BORDER);

  int inner = x + TF_PAD;
  int avail = w - 2 * TF_PAD;
  if (avail < 1)
    avail = 1;
  int ty = y + (h - UIFONT_H) / 2;

  /* Scroll so the caret stays visible: never left of `scroll` */
  if (tf->scroll > tf->len)
    tf->scroll = tf->len;
  if (tf->caret < tf->scroll)
    tf->scroll = tf->caret;
  while (tf->scroll < tf->caret && tf_span(tf, tf->scroll, tf->caret) > avail - 2)
    tf->scroll++;
  /* After a deletion, pull the view left while all the text fits */
  while (tf->scroll > 0 && tf_span(tf, tf->scroll - 1, tf->len) <= avail - 2)
    tf->scroll--;

  int px = inner;
  int caret_px = -1;
  for (int i = tf->scroll; i <= tf->len; i++) {
    if (i == tf->caret)
      caret_px = px;
    if (i == tf->len)
      break;
    int cw = gfx_ui_cw((unsigned char)tf->buf[i]);
    if (px - inner + cw > avail)
      break;
    char one[2] = {tf->buf[i], '\0'};
    gfx_ui_text(px, ty, one, TF_FG);
    px += cw;
  }
  if (focused && caret_px >= 0) {
    if (caret_px > inner + avail - 1)
      caret_px = inner + avail - 1;
    gfx_fill_rect(caret_px, ty, 1, UIFONT_H, TF_FOCUS);
  }
}

int textfield_click(struct textfield *tf, int mx, int my, int x, int y, int w, int h)
{
  if (mx < x || mx >= x + w || my < y || my >= y + h)
    return 0;
  int inner = x + TF_PAD;
  int avail = w - 2 * TF_PAD;
  int px = inner;
  tf->caret = tf->len; /* default: past the text */
  for (int i = tf->scroll; i < tf->len; i++) {
    int cw = gfx_ui_cw((unsigned char)tf->buf[i]);
    if (px - inner + cw > avail) {
      tf->caret = i;
      break;
    }
    if (mx < px + cw / 2) {
      tf->caret = i;
      break;
    }
    px += cw;
  }
  return 1;
}

int textfield_key(struct textfield *tf, char c)
{
  unsigned char u = (unsigned char)c;
  switch (u) {
  case KEY_LEFT:
    if (tf->caret > 0)
      tf->caret--;
    return 1;
  case KEY_RIGHT:
    if (tf->caret < tf->len)
      tf->caret++;
    return 1;
  case KEY_HOME:
    tf->caret = 0;
    return 1;
  case KEY_END:
    tf->caret = tf->len;
    return 1;
  case '\b': /* delete left of caret */
    if (tf->caret > 0) {
      for (int i = tf->caret - 1; i < tf->len; i++)
        tf->buf[i] = tf->buf[i + 1];
      tf->caret--;
      tf->len--;
    }
    return 1;
  case KEY_DELETE: /* delete right of caret */
    if (tf->caret < tf->len) {
      for (int i = tf->caret; i < tf->len; i++)
        tf->buf[i] = tf->buf[i + 1];
      tf->len--;
    }
    return 1;
  default:
    if (u >= 0x20 && u < 0x7f && tf->len < tf->cap - 1) {
      for (int i = tf->len; i > tf->caret; i--) /* open a gap */
        tf->buf[i] = tf->buf[i - 1];
      tf->buf[tf->caret] = c;
      tf->caret++;
      tf->len++;
      tf->buf[tf->len] = '\0';
      return 1;
    }
    return 0; /* Enter/Esc/arrows: app handles */
  }
}

/* button */

/* Per-state face: fill, a 1px top highlight, label colour */
struct btn_face {
  unsigned bg, top, fg;
};
static const struct btn_face btn_faces[] = {
    [BTN_NORMAL] = {0x3a4658u, 0x4a5a70u, 0xe6ebf2u},
    [BTN_HOVER] = {0x46546au, 0x586a84u, 0xffffffu},
    [BTN_PRESSED] = {0x2a3342u, 0x2a3342u, 0xc8d0dcu},
    [BTN_DISABLED] = {0x2a3038u, 0x2a3038u, 0x636b78u},
};

void button_draw(int x, int y, int w, int h, const char *label, int state)
{
  if (state < 0 || state > BTN_DISABLED)
    state = BTN_NORMAL;
  const struct btn_face *f = &btn_faces[state];
  gfx_fill_rect(x, y, w, h, f->bg);
  if (f->top != f->bg) /* raised: lit top edge */
    gfx_fill_rect(x + 1, y + 1, w - 2, 1, f->top);
  gfx_rect(x, y, w, h, 0x05070a);

  /* Centre the label; nudge it down a pixel while pressed so it "sinks" */
  int tw = gfx_ui_width(label);
  int tx = x + (w - tw) / 2;
  int ty = y + (h - UIFONT_H) / 2 + (state == BTN_PRESSED ? 1 : 0);
  gfx_ui_text(tx, ty, label, f->fg);
}

int button_hit(int x, int y, int w, int h, int mx, int my) { return mx >= x && mx < x + w && my >= y && my < y + h; }

/* progress bar */

#define PROG_TROUGH 0x1c2330u /* sunken background */
#define PROG_FILL 0x2f6bd8u   /* accent-blue done portion */
#define PROG_TOP 0x5a8fe0u    /* lit top edge of the fill */
#define PROG_BORDER 0x05070au

void progress_draw(int x, int y, int w, int h, int value, int max, int show_pct)
{
  if (max < 0)
    max = 0;
  if (value < 0)
    value = 0;
  if (value > max)
    value = max;
  int pct = max > 0 ? value * 100 / max : 0;

  gfx_fill_rect(x, y, w, h, PROG_TROUGH);

  /* Fill the inner area (inside the 1px border) proportionally */
  int inner = w - 2;
  int fw = max > 0 && inner > 0 ? (int)((long)inner * value / max) : 0;
  if (fw > 0) {
    gfx_fill_rect(x + 1, y + 1, fw, h - 2, PROG_FILL);
    if (h > 3)
      gfx_fill_rect(x + 1, y + 1, fw, 1, PROG_TOP);
  }
  gfx_rect(x, y, w, h, PROG_BORDER);

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
    int tw = gfx_ui_width(buf);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - UIFONT_H) / 2;
    /* White over the blue done-portion, light grey over the trough */
    unsigned fg = (w / 2 < 1 + fw) ? 0xffffffu : 0xc8d0dcu;
    gfx_ui_text(tx, ty, buf, fg);
  }
}

/* slider */

#define SLIDER_THUMB_W 6
#define SLIDER_TRACK_H 6
#define SLIDER_GRAB 4 /* extra hit margin around the box */

static int slider_travel(int w) { return w - SLIDER_THUMB_W > 1 ? w - SLIDER_THUMB_W : 1; }

void slider_draw(int x, int y, int w, int value, int max)
{
  if (value < 0)
    value = 0;
  if (value > max)
    value = max;
  int tx = x + (max > 0 ? (int)((long)slider_travel(w) * value / max) : 0);
  int ty = y + (GUI_SLIDER_H - SLIDER_TRACK_H) / 2;
  gfx_fill_rect(x, ty, w, SLIDER_TRACK_H, PROG_TROUGH);
  gfx_fill_rect(x, ty, tx - x + SLIDER_THUMB_W / 2, SLIDER_TRACK_H, PROG_FILL);
  gfx_rect(x, ty, w, SLIDER_TRACK_H, PROG_BORDER);
  gfx_fill_rect(tx, y, SLIDER_THUMB_W, GUI_SLIDER_H, 0xffffffu);
}

int slider_hit(int x, int y, int w, int mx, int my)
{
  return button_hit(x - SLIDER_GRAB, y - SLIDER_GRAB, w + 2 * SLIDER_GRAB, GUI_SLIDER_H + 2 * SLIDER_GRAB, mx, my);
}

int slider_value(int x, int w, int mx, int max)
{
  long v = ((long)(mx - x - SLIDER_THUMB_W / 2) * max + slider_travel(w) / 2) / slider_travel(w);
  return v < 0 ? 0 : v > max ? max : (int)v;
}

/* scrollbar */

#define SCROLL_TRACK 0xc8ccd2u    /* light track groove */
#define SCROLL_THUMB 0x6b7686u    /* draggable slate thumb */
#define SCROLL_ARROW_BG 0xb4b9c2u /* arrow-button face */
#define SCROLL_ARROW_FG 0x4a5564u /* arrowhead */
#define SCROLL_MIN_THUMB 12       /* keep the thumb grabbable */

/* Square arrow-button height for a w-wide, h-tall bar */
static int scrollbar_arrow_h(int w, int h)
{
  int a = w;
  if (a < 4 || h < 2 * a + SCROLL_MIN_THUMB)
    return 0;
  return a;
}

/* Thumb height + top offset within a track of height `tr` for the current counts */
static void scrollbar_thumb(int tr, int scroll, int total, int visible, int *th, int *ty)
{
  int t = (int)((long)tr * visible / total);
  if (t < SCROLL_MIN_THUMB)
    t = SCROLL_MIN_THUMB;
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

void scrollbar_draw(int x, int y, int w, int h, int scroll, int total, int visible)
{
  int a = scrollbar_arrow_h(w, h);
  int tr = h - 2 * a; /* track region height */

  gfx_fill_rect(x, y, w, h, SCROLL_TRACK);
  if (a > 0) {
    gfx_fill_rect(x, y, w, a, SCROLL_ARROW_BG);         /* up box */
    gfx_fill_rect(x, y + h - a, w, a, SCROLL_ARROW_BG); /* down box */
    scrollbar_tri(x, y, w, a, 1, SCROLL_ARROW_FG);
    scrollbar_tri(x, y + h - a, w, a, 0, SCROLL_ARROW_FG);
  }
  if (total > visible && visible > 0 && tr > 0 && w > 2) {
    int th, ty;
    scrollbar_thumb(tr, scroll, total, visible, &th, &ty);
    gfx_fill_rect(x + 1, y + a + ty, w - 2, th, SCROLL_THUMB);
  }
}

int scrollbar_hit(int x, int y, int w, int h, int mx, int my)
{
  if (mx < x || mx >= x + w || my < y || my >= y + h)
    return SCROLLBAR_NONE;
  int a = scrollbar_arrow_h(w, h);
  if (a > 0) {
    if (my < y + a)
      return SCROLLBAR_UP;
    if (my >= y + h - a)
      return SCROLLBAR_DOWN;
  }
  return SCROLLBAR_TRACK;
}

int scrollbar_pos(int x, int y, int w, int h, int mx, int my, int scroll, int total, int visible)
{
  if (scrollbar_hit(x, y, w, h, mx, my) != SCROLLBAR_TRACK)
    return scroll;
  int maxs = total - visible;
  if (maxs <= 0)
    return 0;
  int a = scrollbar_arrow_h(w, h);
  int tr = h - 2 * a; /* track region height */
  if (tr <= 0)
    return scroll;
  int th, ty;
  scrollbar_thumb(tr, scroll, total, visible, &th, &ty);
  int span = tr - th;
  int rel = my - (y + a) - th / 2; /* centre the thumb on the cursor */
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
