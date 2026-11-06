#include "main_int.h"

/* realloc a typed array to hold `need` elements; NULL on failure (the old array stays valid). */
void *grow_arr(void *p, int *cap, int need, unsigned esz)
{
  if (need <= *cap)
    return p;
  int nc = browser_grow_cap(*cap, 64, need);
  void *np = realloc(p, (unsigned long)nc * esz);
  if (np)
    *cap = nc;
  return np;
}

static bool lt_ensure(struct ltab *t, int need)
{
  if (need <= t->cap)
    return true;
  int nc = browser_grow_cap(t->cap, LINES_INIT, need);
  int **col[7] = {&t->off, &t->len, &t->x, &t->w, &t->y, &t->h, &t->px};
  int *np[7];
  bool ok = true;
  for (int i = 0; i < 7; i++) {
    np[i] = realloc(*col[i], (unsigned long)nc * sizeof(int));
    ok = ok && np[i];
  }
  for (int i = 0; i < 7; i++)
    if (np[i])
      *col[i] = np[i];
  if (ok)
    t->cap = nc;
  return ok;
}

int lt_bottom(const struct ltab *t, int i) { return (i >= 0 && i < t->n) ? t->y[i] + t->h[i] : 0; }
int lt_doc_px(const struct ltab *t) { return t->n > 0 ? lt_bottom(t, t->n - 1) : 0; }

/* The line whose band contains document-pixel `y` (clamped to a real line). */
int lt_line_at(const struct ltab *t, int y)
{
  int lo = 0, hi = t->n - 1, ans = 0;
  if (hi < 0 || y <= 0)
    return 0;
  while (lo <= hi) { /* last line with y <= target */
    int mid = (lo + hi) / 2;
    if (t->y[mid] <= y) {
      ans = mid;
      lo = mid + 1;
    } else
      hi = mid - 1;
  }
  return ans;
}

/* --------------------------------------------------------------- word wrap */

/* The page body is drawn with the PROPORTIONAL sans-serif UI font (gfx_ui_*),
 * not the monospace cell grid: a glyph's pen advance is gfx_ui_cw(c), a line is
 * LINE_H tall, and wrapping/hit-testing measure in pixels. (The address bar,
 * tabs and menus already use this font.) */

/* --- body font: the baked proportional UI font, OR a page @font-face --------
 * When the page loads a usable @font-face and applies it to the body
 * (B->pg->body_font != NULL), body text is drawn with that custom face via
 * user/lib/font; otherwise it falls back to the baked gfx_ui font. B->pg->text
 * holds UTF-8; the renderer decodes a codepoint at a time via the shared
 * utf8_decode/utf8_seqlen (user/lib).
 *
 * Two contexts keep the layout thread and the paint thread off each other's
 * caches: g_ui (main thread: paint + hit-testing) and g_wk (worker: layout).
 * Each holds the block state set by set_block, an advance cache keyed by
 * (codepoint, px), and, for g_ui, a glyph bitmap cache keyed the same way. */
#define GT_BITMAP_BUDGET (3ul << 20)
#define GT_ADV_MAX 65536

struct bctx g_ui = {.gt = {.budget = GT_BITMAP_BUDGET}};
struct bctx g_wk = {.worker = true};
static struct ru_gent g_gscratch; /* stand-in glyph when the cache can't grow */

static void gcache_sync(struct bctx *c)
{
  if (c->gf == B->pg->body_font)
    return;
  ru_gtab_clear(&c->at);
  ru_gtab_clear(&c->gt);
  c->gf = B->pg->body_font;
}

/* Bitmap + advance of one glyph at `px` in the page font (g_ui only). */
static const struct ru_gent *gcache_get(struct bctx *c, unsigned cp, int px)
{
  struct ru_gent *g = ru_gtab_find(&c->gt, cp, (unsigned)px);
  if (g)
    return g;
  struct ru_gent n = {.cp = cp, .px = (unsigned)px};
  int gi = font_glyph_index(B->pg->body_font, (int)cp);
  n.bm = font_render_glyph(B->pg->body_font, gi, (float)px, &n.w, &n.h, &n.ox, &n.oy, &n.adv);
  if ((g = ru_gtab_put(&c->gt, &n)))
    return g;
  free(n.bm);
  n.bm = 0;
  g_gscratch = n;
  return &g_gscratch;
}

/* Pen advance of one codepoint, uncached. px 0 = the default body size. */
static int adv_measure(unsigned cp, int px)
{
  if (!B->pg->body_font) /* the gfx system-ui font */
    return px ? gfx_ui_cw_cp_px(cp, px) : gfx_ui_cw_cp(cp);
  int gi = font_glyph_index(B->pg->body_font, (int)cp);
  int adv = 0;
  if (px && px != (int)BODY_PX) { /* sized run: advance at px */
    font_glyph_hmetrics(B->pg->body_font, gi, (float)px, &adv, 0);
    return adv;
  }
  int w, h, ox, oy;
  free(font_render_glyph(B->pg->body_font, gi, BODY_PX, &w, &h, &ox, &oy, &adv));
  return adv;
}

/* Indent width in pixels for one nesting level. */

/* Block context for the paragraph about to be measured/drawn, decoded from a
 * glyph's BP_* byte: a heading px (0 = default UI font), monospace (<pre>),
 * centring, and an indent level (blockquote/list nesting). */
void set_block(struct bctx *c, int off)
{
  int inb = (off >= 0 && off < B->pg->text_len);
  unsigned char bp = inb ? B->pg->bp[off] : 0;
  c->bhr = ((bp & BP_SIZE) == 7);
  c->bpx = ru_glyph_px(inb ? B->pg->fsz[off] : 0, bp);
  c->bmono = (bp & BP_MONO) != 0;
  c->bcenter = inb && B->pg->align ? B->pg->align[off] : ((bp & BP_CENTER) ? 1 : 0);
  c->bindent = (bp & BP_INDENT) >> BP_INDENT_SH;
  c->blh = inb ? B->pg->lh[off] : 0;
}

/* Per-glyph resolved font-size px (0 = default body); like set_block's bpx
 * but for one glyph, so a line with mixed inline sizes measures/draws each run
 * right. */
int glyph_px(int off)
{
  if (off < 0 || off >= B->pg->text_len)
    return 0;
  return ru_glyph_px(B->pg->fsz[off], B->pg->bp[off]);
}

/* Raw, non-HTML content (text/plain, source, etc.) renders in the fixed-width
 * monospace cell font, not the proportional UI font — only HTML gets the
 * latter. A <pre> block (bmono) is monospace too, even inside an HTML page.
 */
static bool body_mono(const struct bctx *c) { return B->pg->mode == MODE_TEXT || c->bmono; }

/* Pen advance of one codepoint in the active body font (baked UI font when no
 * custom face: ASCII/Latin-1 by value, a '?'-width box for anything above it).
 */
int body_cp_adv(struct bctx *c, unsigned cp, int px)
{
  if (body_mono(c))
    return GLYPH_W; /* monospace cell */
  gcache_sync(c);
  struct ru_gent *a = ru_gtab_find(&c->at, cp, (unsigned)px);
  if (a)
    return a->adv;
  bool lock = c->worker && !c->held;
  if (lock)
    browser_buf_lock();
  int adv = adv_measure(cp, px);
  if (lock)
    browser_buf_unlock();
  if (c->at.n >= GT_ADV_MAX)
    ru_gtab_clear(&c->at);
  struct ru_gent n = {.cp = cp, .px = (unsigned)px, .adv = adv};
  ru_gtab_put(&c->at, &n);
  return adv;
}

/* Alpha-blend one cached glyph's coverage in colour fg, clipped to [ct,cb).
 * With `italic`, faux-slant: each row shifts horizontally by ~1/4 its height
 * above the baseline `y` (rows above the baseline lean right, below lean left).
 */
static void blit_glyph(int x, int y, const struct ru_gent *g, unsigned fg, int ct, int cb, int italic)
{
  if (!g->bm)
    return;
  unsigned *fb = browser_surface();
  unsigned fr = (fg >> 16) & 0xff, fgc = (fg >> 8) & 0xff, fbb = fg & 0xff;
  for (int gy = 0; gy < g->h; gy++) {
    int py = y + g->oy + gy;
    if (py < ct || py >= cb || py < 0 || py >= BH)
      continue;
    int shift = italic ? (y - py) / 4 : 0;
    const unsigned char *srow = g->bm + gy * g->w;
    unsigned *drow = fb + (unsigned long)py * BW;
    for (int gx = 0; gx < g->w; gx++) {
      int px = x + g->ox + gx + shift;
      if (px < 0 || px >= BW)
        continue;
      unsigned a = srow[gx];
      if (!a)
        continue;
      if (a == 255) {
        drow[px] = fg;
        continue;
      }
      unsigned dst = drow[px], ia = 255 - a;
      unsigned r = (fr * a + ((dst >> 16) & 0xff) * ia) / 255;
      unsigned gg = (fgc * a + ((dst >> 8) & 0xff) * ia) / 255;
      unsigned bb = (fbb * a + (dst & 0xff) * ia) / 255;
      drow[px] = (r << 16) | (gg << 8) | bb;
    }
  }
}

/* A "tofu" box for a codepoint the active font (or the ASCII baked font) can't
 * draw, so missing glyphs are visible rather than silently dropped. */
static void draw_tofu(int x, int y, int w, unsigned fg)
{
  if (w > 4)
    gfx_rect(x + 1, y + 3, w - 3, UIFONT_H - 6, fg);
}

/* Draw a UTF-8 run of font size `px` (0 = default body), its baseline
 * `baseline` px below line-top `y` so runs of different sizes on one line share
 * a baseline. Clipped to [ct,cb); returns the pen x after the run. With a custom
 * face, glyphs come from the (codepoint, px) cache; otherwise the baked UI font
 * draws ASCII/Latin-1 and a tofu box stands in for anything it lacks. */
int body_draw_run(int x, int y, const char *s, int len, int width, unsigned fg, int ct, int cb, int px, int baseline,
                  int italic)
{
  const char *end = s + len;
  unsigned cp;
  if (body_mono(&g_ui)) { /* raw content: monospace cells */
    gfx_text(x, y, s, fg, GFX_TRANSPARENT);
    return x + width;
  }
  if (B->pg->body_font) {
    int by = y + baseline; /* screen baseline shared by runs */
    int eff = px ? px : (int)BODY_PX;
    gcache_sync(&g_ui);
    while (s < end) {
      s += utf8_decode(s, end, &cp);
      const struct ru_gent *g = gcache_get(&g_ui, cp, eff);
      if (g->bm)
        blit_glyph(x, by, g, fg, ct, cb, italic);
      else if (cp > 0x20)
        draw_tofu(x, y, g->adv, fg); /* .notdef / empty */
      x += g->adv;
    }
    return x;
  }
  /* No @font-face: the gfx system-ui font (size-cached). gfx_ui_text_px puts
   * the baseline at y+ascent(px), so offset y to land it on the line's shared
   * baseline. */
  int eff = px ? px : (int)BODY_PX;
  gfx_ui_text_px_clip(x, y + baseline - gfx_ui_ascent_px(eff), s, fg, eff, ct, cb);
  return x + gfx_ui_width_px(s, eff);
}

/* Ascent (line-top → baseline) for a run of `px` in the active body font; a
 * line's baseline is the max over its runs, so mixed sizes share one baseline.
 */
int run_ascent(int px)
{
  if (px <= 0)
    px = (int)BODY_PX;
  if (B->pg->body_font) {
    int a = 0;
    font_vmetrics(B->pg->body_font, (float)px, &a, 0, 0);
    return a;
  }
  return gfx_ui_ascent_px(px);
}

/* Display height of a text line from its largest font px and largest CSS
 * line-height: an explicit line-height wins, else it's ~1.25 of the font. */
int line_height(int maxpx, int lhmax)
{
  if (lhmax > 0)
    return lhmax < 6 ? 6 : lhmax;
  if (maxpx <= (int)BODY_PX)
    return LINE_H;
  int lh = maxpx + (maxpx + 2) / 4;
  return lh < LINE_H ? LINE_H : lh;
}

/* While a flex-row item is being flowed, every wrapped line is captured into
 * the overlay on the band instead of appended to the main table (set by
 * flow_flex_row). */
bool g_ov_active; /* 1 = redirect add_line to the flex overlay */
int g_ov_base;    /* absolute band row that the item's first line maps to */
int g_ov_n;       /* lines emitted for the current item so far */

void add_line(int off, int len, int x, int w, int h, int maxpx)
{
  struct layout_out *o = g_bk;
  if (g_ov_active) {
    struct fslice *p = grow_arr(o->fs, &o->cap_fs, o->n_fs + 1, sizeof *p);
    if (p) {
      o->fs = p;
      p[o->n_fs++] = (struct fslice){g_ov_base + g_ov_n, off, len, x, w, h > 0 ? h : LINE_H, maxpx};
    }
    g_ov_n++;
    return;
  }
  struct ltab *t = &o->lt;
  if (!lt_ensure(t, t->n + 1))
    return; /* out of memory: stop adding lines */
  int n = t->n;
  t->off[n] = off;
  t->len[n] = len;
  t->x[n] = x;
  t->w[n] = w;
  t->h[n] = h > 0 ? h : LINE_H;
  t->px[n] = maxpx;
  t->y[n] = n > 0 ? t->y[n - 1] + t->h[n - 1] : 0;
  t->n++;
}

/* How many display rows an inline image reserves at the current pane width. */
/* Decide an inline image's drawn size (dw x dh px) within a pane `paw` px wide,
 * honouring the CSS/HTML width & height: both given pin the box (a single one
 * scales the other by the natural aspect ratio); with neither, the natural size
 * is used. The result is shrunk to fit the pane width, ratio preserved. dw/dh
 * are 0 when nothing can be sized yet (no explicit size and the image isn't
 * decoded), which the caller renders as a one-row alt box. Explicit sizes work
 * even before the image loads, so a lazy/slow image reserves its space and the
 * page doesn't reflow when it lands. */
void img_disp_size(const struct inl_img *im, int paw, int *dw, int *dh)
{
  int natural = im->ok && im->iw && im->ih;
  int w = 0, h = 0;
  if (im->cw && im->ch) {
    w = im->cw;
    h = im->ch;
  } else if (im->cw) {
    w = im->cw;
    h = natural ? (int)((unsigned long)im->cw * im->ih / im->iw) : im->cw;
  } else if (im->ch) {
    h = im->ch;
    w = natural ? (int)((unsigned long)im->ch * im->iw / im->ih) : im->ch;
  } else if (natural) {
    w = (int)im->iw;
    h = (int)im->ih;
  } else {
    *dw = 0;
    *dh = 0;
    return;
  }
  if (w > paw) {
    h = (int)((long)h * paw / (w ? w : 1));
    w = paw;
  } /* fit width */
  *dw = w < 1 ? 1 : w;
  *dh = h < 1 ? 1 : h;
}

/* Inline image anchored at text offset `pos` (lowest index), or -1. */
struct ru_imgidx g_imgix;
bool g_imgix_ok;
bool g_has_media; /* this page has <audio>/<video> to place */
int img_at_pos(int pos)
{
  if (g_imgix_ok)
    return ru_imgidx_find(&g_imgix, pos);
  for (int q = 0; q < B->pg->n_imgs; q++)
    if (B->pg->imgs[q].text_pos == pos)
      return q;
  return -1;
}

/* Highest valid scroll line: the last `rows` lines fill the pane exactly, so
 * the bottom stop can't scroll content up into blank space below it. */
int scroll_max(void)
{
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  int max = g_fr->doc_h - phh;
  return max < 0 ? 0 : max;
}
