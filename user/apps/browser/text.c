#include "fontstore.h"
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

/* The page body is drawn with the PROPORTIONAL sans-serif UI font (gfx_ui_*), not the monospace */

/* --- page fonts: each glyph's face comes from its font set (family stack, weight, style) */
#define GT_BITMAP_BUDGET (3ul << 20)
#define GT_ADV_MAX 65536
#define PICK_MAX 65536

struct bctx g_ui = {.gt = {.budget = GT_BITMAP_BUDGET}};
struct bctx g_wk = {.worker = true};
static struct ru_gent g_gscratch; /* stand-in glyph when the cache can't grow */

/* Fonts from /etc/fonts shared by every page: the fixed roles, then installed families pages name. */
enum { LF_UI, LF_MONO, LF_EMOJI, LF_FIXED, LF_MAX = 16 };
static struct {
  char family[CSS_FAM_MAX];
  struct font *font;
  unsigned char *data;
  bool tried;
} g_local[LF_MAX];
static int g_nlocal = LF_FIXED;

/* A face id: >= 0 indexes the page's faces, < 0 is local font LF_* via FACE_LOCAL. */
#define FACE_LOCAL(k) (-1 - (k))

static struct font *load_font_file(const char *path, unsigned char **data)
{
  struct statinfo st;
  *data = 0;
  if (stat(path, &st) < 0 || st.is_dir || st.size == 0)
    return 0;
  unsigned char *buf = malloc(st.size);
  if (!buf)
    return 0;
  if (readfile_at(path, 0, buf, st.size) != (int)st.size) {
    free(buf);
    return 0;
  }
  struct font *f = font_load(buf, (int)st.size);
  if (!f) {
    free(buf);
    return 0;
  }
  *data = buf;
  return f;
}

static struct font *local_font(int k)
{
  if (k == LF_UI)
    return gfx_ui_font();
  if (!g_local[k].tried && (k == LF_MONO || k == LF_EMOJI)) {
    char path[96];
    g_local[k].tried = true;
    fontstore_path(k == LF_MONO ? FONT_MONO : FONT_EMOJI, path, sizeof path);
    g_local[k].font = load_font_file(path, &g_local[k].data);
  }
  return g_local[k].font;
}

/* Read the emoji font ahead of the first paint that needs it; the slow read runs outside the buffer lock. */
void fonts_preload_emoji(void)
{
  if (g_local[LF_EMOJI].tried)
    return;
  char path[96];
  unsigned char *data;
  fontstore_path(FONT_EMOJI, path, sizeof path);
  struct font *f = load_font_file(path, &data);
  browser_buf_lock();
  if (!g_local[LF_EMOJI].tried) {
    g_local[LF_EMOJI].tried = true;
    g_local[LF_EMOJI].font = f;
    g_local[LF_EMOJI].data = data;
    f = 0;
  }
  browser_buf_unlock();
  if (f) {
    font_free(f);
    free(data);
  }
}

/* The local slot of an installed family named `name`, loading it on first use; -1 if none is installed. */
static int local_named(const char *name)
{
  for (int k = LF_FIXED; k < g_nlocal; k++)
    if (streq(g_local[k].family, name))
      return g_local[k].font ? k : -1;
  if (g_nlocal == LF_MAX)
    return -1;
  int k = g_nlocal++;
  int i = 0;
  for (; name[i] && i < CSS_FAM_MAX - 1; i++)
    g_local[k].family[i] = name[i];
  g_local[k].family[i] = '\0';
  g_local[k].tried = true;
  char path[96];
  if (fontstore_find(name, path, sizeof path))
    g_local[k].font = load_font_file(path, &g_local[k].data);
  return g_local[k].font ? k : -1;
}

static bool fam_generic_sans(const char *n)
{
  return streq(n, "sans-serif") || streq(n, "serif") || streq(n, "system-ui") || streq(n, "ui-sans-serif") ||
         streq(n, "ui-serif") || streq(n, "ui-rounded") || streq(n, "cursive") || streq(n, "fantasy") ||
         streq(n, "-apple-system") || streq(n, "blinkmacsystemfont") || streq(n, "math");
}
static bool fam_generic_mono(const char *n) { return streq(n, "monospace") || streq(n, "ui-monospace"); }
static bool fam_generic_emoji(const char *n)
{
  return streq(n, "emoji") || streq(n, "noto color emoji") || streq(n, "apple color emoji") ||
         streq(n, "segoe ui emoji");
}

static const struct font *face_font(int face)
{
  if (face >= 0)
    return face < B->pg->n_faces ? B->pg->faces[face].font : 0;
  return local_font(-1 - face);
}

static bool font_has(const struct font *f, unsigned cp) { return f && font_glyph_index(f, (int)cp) > 0; }

/* Codepoints that only steer shaping (joiners, variation selectors, tags): no ink, no advance. */
static bool zero_width(unsigned cp)
{
  return (cp >= 0x200b && cp <= 0x200d) || cp == 0x2060 || cp == 0xfeff || (cp >= 0xfe00 && cp <= 0xfe0f) ||
         (cp >= 0xe0020 && cp <= 0xe007f);
}

/* How far a face's weight range is from `want`, ranked by the CSS font-matching order. */
static int weight_cost(int want, int lo, int hi)
{
  if (want >= lo && want <= hi)
    return 0;
  int near = want < lo ? lo : hi, d = near > want ? near - want : want - near;
  bool heavier = near > want;
  if (want > 500)
    return heavier ? d : 1000 + d;
  if (want < 400)
    return heavier ? 1000 + d : d;
  if (heavier && near <= 500)
    return d; /* 400..500 looks up to 500 first, then down, then above */
  return heavier ? 2000 + d : 1000 + d;
}

/* Bits stored with a pick: the face draws real bold / real italic, so neither is synthesised. */
#define PK_BOLD 1
#define PK_ITALIC 2

/* The face for `cp` in font set `set`: the first family in the stack that has the glyph (for @font-face
 * families, the face whose unicode-range covers it and whose weight and style fit best), then the UI font,
 * then the emoji font. */
static int pick_face_uncached(int set, unsigned cp, int *flags)
{
  const struct font_set *fs = set > 0 && set < B->pg->n_fsets ? &B->pg->fsets[set] : 0;
  int want = fs ? fs->weight : 400;
  bool ital = fs && fs->italic;
  *flags = 0;
  const char *p = fs ? fs->stack : "";
  while (*p) {
    char name[CSS_FAM_MAX];
    int n = 0;
    while (*p && *p != ',' && n < (int)sizeof name - 1)
      name[n++] = *p++;
    name[n] = '\0';
    while (*p && *p != ',')
      p++;
    if (*p == ',')
      p++;
    if (!name[0])
      continue;
    int best = -1, best_cost = 0;
    for (int i = 0; i < B->pg->n_faces; i++) {
      const struct loaded_face *lf = &B->pg->faces[i];
      if (!streq(lf->family, name) || !lf->font)
        continue;
      bool covered = !lf->nranges;
      for (int r = 0; r < lf->nranges && !covered; r++)
        covered = cp >= lf->ranges[2 * r] && cp <= lf->ranges[2 * r + 1];
      if (!covered || !font_has(lf->font, cp))
        continue;
      int cost = weight_cost(want, lf->wmin, lf->wmax) + (lf->italic != ital ? 5000 : 0);
      if (best < 0 || cost < best_cost) {
        best = i;
        best_cost = cost;
      }
    }
    if (best >= 0) {
      const struct loaded_face *lf = &B->pg->faces[best];
      int got = want < lf->wmin ? lf->wmin : want > lf->wmax ? lf->wmax : want;
      *flags = (got >= 600 ? PK_BOLD : 0) | (lf->italic ? PK_ITALIC : 0);
      return best;
    }
    int k = fam_generic_sans(name)    ? LF_UI
            : fam_generic_mono(name)  ? LF_MONO
            : fam_generic_emoji(name) ? LF_EMOJI
                                      : local_named(name);
    if (k >= 0 && font_has(local_font(k), cp)) {
      *flags = k == LF_EMOJI ? PK_BOLD | PK_ITALIC : 0;
      return FACE_LOCAL(k);
    }
  }
  if (font_has(local_font(LF_UI), cp) || !font_has(local_font(LF_EMOJI), cp))
    return FACE_LOCAL(LF_UI); /* the UI font draws tofu for what nothing has */
  *flags = PK_BOLD | PK_ITALIC;
  return FACE_LOCAL(LF_EMOJI);
}

/* Drop the caches when the page (or its faces) changed under this context. */
static void gcache_sync(struct bctx *c)
{
  if (c->pg == B->pg && c->gen == B->pg->font_gen)
    return;
  ru_gtab_clear(&c->at);
  ru_gtab_clear(&c->gt);
  ru_gtab_clear(&c->pk);
  c->pg = B->pg;
  c->gen = B->pg->font_gen;
}

static int pick_face(struct bctx *c, int set, unsigned cp, int *flags)
{
  struct ru_gent *e = ru_gtab_find(&c->pk, cp, (unsigned)set);
  if (e) {
    *flags = e->w;
    return e->adv;
  }
  int face = pick_face_uncached(set, cp, flags);
  if (c->pk.n >= PICK_MAX)
    ru_gtab_clear(&c->pk);
  struct ru_gent n = {.cp = cp, .px = (unsigned)set, .adv = face, .w = *flags};
  ru_gtab_put(&c->pk, &n);
  return face;
}

static unsigned glyph_key(int face, int px)
{
  return (unsigned)px | ((unsigned)(face >= 0 ? face + LF_MAX : -face) << 9);
}

struct memrd {
  const unsigned char *p;
  int n, i;
};
static int memrd_get(void *ctx)
{
  struct memrd *m = ctx;
  return m->i < m->n ? m->p[m->i++] : -1;
}
struct rgba_img {
  unsigned char *px;
  unsigned w, h;
};
static int rgba_row(void *ctx, unsigned y, const unsigned char *rgba, unsigned w)
{
  struct rgba_img *im = ctx;
  if (y < im->h && w == im->w)
    memcpy(im->px + (unsigned long)y * w * 4, rgba, (unsigned long)w * 4);
  return 0;
}

/* Colour glyph `gi` decoded and box-filtered to `px`, as straight-alpha ARGB in n->bm. */
static bool render_color(const struct font *f, int gi, int px, struct ru_gent *n)
{
  struct font_png g;
  if (!font_glyph_png(f, gi, (float)px, &g) || g.ppem <= 0)
    return false;
  struct memrd rd = {g.png, g.len, 0};
  struct rgba_img im = {0, 0, 0};
  if (png_dimensions(memrd_get, &rd, &im.w, &im.h) != PNG_OK || !im.w || !im.h || im.w > 512 || im.h > 512)
    return false;
  im.px = calloc((unsigned long)im.w * im.h, 4);
  if (!im.px)
    return false;
  rd.i = 0;
  if (png_decode(memrd_get, &rd, rgba_row, &im, 0, 0) != PNG_OK) {
    free(im.px);
    return false;
  }
  float s = (float)px / (float)g.ppem;
  int dw = (int)(im.w * s + 0.5f), dh = (int)(im.h * s + 0.5f);
  if (dw < 1)
    dw = 1;
  if (dh < 1)
    dh = 1;
  unsigned *out = malloc((unsigned long)dw * dh * sizeof *out);
  if (!out) {
    free(im.px);
    return false;
  }
  for (int dy = 0; dy < dh; dy++) {
    int y0 = (int)(dy / s), y1 = (int)((dy + 1) / s);
    if (y1 <= y0)
      y1 = y0 + 1;
    if (y1 > (int)im.h)
      y1 = (int)im.h;
    for (int dx = 0; dx < dw; dx++) {
      int x0 = (int)(dx / s), x1 = (int)((dx + 1) / s);
      if (x1 <= x0)
        x1 = x0 + 1;
      if (x1 > (int)im.w)
        x1 = (int)im.w;
      unsigned long sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
          const unsigned char *q = im.px + ((unsigned long)y * im.w + (unsigned)x) * 4;
          sr += (unsigned long)q[0] * q[3];
          sg += (unsigned long)q[1] * q[3];
          sb += (unsigned long)q[2] * q[3];
          sa += q[3];
          cnt++;
        }
      unsigned a = cnt ? (unsigned)(sa / cnt) : 0;
      unsigned r = sa ? (unsigned)(sr / sa) : 0, gg = sa ? (unsigned)(sg / sa) : 0, b = sa ? (unsigned)(sb / sa) : 0;
      out[dy * dw + dx] = a << 24 | r << 16 | gg << 8 | b;
    }
  }
  free(im.px);
  n->bm = (unsigned char *)out;
  n->color = true;
  n->w = dw;
  n->h = dh;
  n->ox = (int)(g.bx * s + (g.bx >= 0 ? 0.5f : -0.5f));
  n->oy = -(int)(g.by * s + 0.5f);
  return true;
}

/* Bitmap + advance of one glyph of `face` at `px` (g_ui only). */
static const struct ru_gent *gcache_get(struct bctx *c, int face, unsigned cp, int px)
{
  unsigned key = glyph_key(face, px);
  struct ru_gent *g = ru_gtab_find(&c->gt, cp, key);
  if (g)
    return g;
  struct ru_gent n = {.cp = cp, .px = key};
  const struct font *f = face_font(face);
  if (f) {
    int gi = font_glyph_index(f, (int)cp);
    if (font_has_color(f) && render_color(f, gi, px, &n))
      font_glyph_hmetrics(f, gi, (float)px, &n.adv, 0);
    else
      n.bm = font_render_glyph(f, gi, (float)px, &n.w, &n.h, &n.ox, &n.oy, &n.adv);
  } else {
    n.adv = gfx_ui_cw_cp_px(cp, px);
  }
  if ((g = ru_gtab_put(&c->gt, &n)))
    return g;
  free(n.bm);
  n.bm = 0;
  g_gscratch = n;
  return &g_gscratch;
}

/* Pen advance of one codepoint in `face`, uncached. */
static int adv_measure(int face, unsigned cp, int px)
{
  const struct font *f = face_font(face);
  if (!f)
    return gfx_ui_cw_cp_px(cp, px);
  int adv = 0;
  font_glyph_hmetrics(f, font_glyph_index(f, (int)cp), (float)px, &adv, 0);
  return adv;
}

/* Indent width in pixels for one nesting level. */

/* Block context for the paragraph about to be measured/drawn, decoded from a glyph's BP_* byte */
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

/* Per-glyph resolved font-size px (0 = default body) */
int glyph_px(int off)
{
  if (off < 0 || off >= B->pg->text_len)
    return 0;
  return ru_glyph_px(B->pg->fsz[off], B->pg->bp[off]);
}

int glyph_set(int off) { return off >= 0 && off < B->pg->text_len && B->pg->ff ? B->pg->ff[off] : 0; }

/* Raw, non-HTML content (text/plain, source, etc.) renders in the fixed-width monospace cell */
static bool body_mono(const struct bctx *c) { return B->pg->mode == MODE_TEXT || c->bmono; }

/* Pen advance of one codepoint in font set `set` at `px` (0 = the default body size) */
int body_cp_adv(struct bctx *c, unsigned cp, int px, int set)
{
  if (body_mono(c))
    return GLYPH_W; /* monospace cell */
  if (zero_width(cp))
    return 0;
  int eff = px ? px : (int)BODY_PX;
  bool lock = c->worker && !c->held;
  if (lock)
    browser_buf_lock();
  gcache_sync(c);
  unsigned key = (unsigned)eff | (unsigned)set << 9;
  struct ru_gent *a = ru_gtab_find(&c->at, cp, key);
  int adv;
  if (a) {
    adv = a->adv;
  } else {
    int flags;
    adv = adv_measure(pick_face(c, set, cp, &flags), cp, eff);
    if (c->at.n >= GT_ADV_MAX)
      ru_gtab_clear(&c->at);
    struct ru_gent n = {.cp = cp, .px = key, .adv = adv};
    ru_gtab_put(&c->at, &n);
  }
  if (lock)
    browser_buf_unlock();
  return adv;
}

/* Alpha-blend one cached glyph (coverage in colour fg, or its own ARGB colours) clipped to [ct,cb) */
static void blit_glyph(int x, int y, const struct ru_gent *g, unsigned fg, int ct, int cb, int italic)
{
  if (!g->bm)
    return;
  unsigned *fb = browser_surface();
  const unsigned *argb = g->color ? (const unsigned *)g->bm : 0;
  for (int gy = 0; gy < g->h; gy++) {
    int py = y + g->oy + gy;
    if (py < ct || py >= cb || py < 0 || py >= BH)
      continue;
    int shift = italic ? (y - py) / 4 : 0;
    unsigned *drow = fb + (unsigned long)py * BW;
    for (int gx = 0; gx < g->w; gx++) {
      int px = x + g->ox + gx + shift;
      if (px < 0 || px >= BW)
        continue;
      unsigned src = argb ? argb[gy * g->w + gx] : fg;
      unsigned a = argb ? src >> 24 : g->bm[gy * g->w + gx];
      if (!a)
        continue;
      if (a == 255) {
        drow[px] = src & 0xffffff;
        continue;
      }
      unsigned dst = drow[px], ia = 255 - a;
      unsigned r = (((src >> 16) & 0xff) * a + ((dst >> 16) & 0xff) * ia) / 255;
      unsigned gg = (((src >> 8) & 0xff) * a + ((dst >> 8) & 0xff) * ia) / 255;
      unsigned bb = ((src & 0xff) * a + (dst & 0xff) * ia) / 255;
      drow[px] = (r << 16) | (gg << 8) | bb;
    }
  }
}

/* A "tofu" box for a codepoint no font can draw, so missing glyphs stay visible */
static void draw_tofu(int x, int y, int w, unsigned fg)
{
  if (w > 4)
    gfx_rect(x + 1, y + 3, w - 3, UIFONT_H - 6, fg);
}

/* Draw a UTF-8 run of font size `px` (0 = default body) in font set `set`, its baseline `baseline` px below
 * `y`. `italic` and `bold` are what the text asks for; they are synthesised only where the face lacks them. */
int body_draw_run(int x, int y, const char *s, int len, int width, unsigned fg, int ct, int cb, int px, int set,
                  int baseline, int italic, int bold)
{
  const char *end = s + len;
  unsigned cp;
  if (body_mono(&g_ui)) { /* raw content: monospace cells */
    gfx_text(x, y, s, fg, GFX_TRANSPARENT);
    if (bold)
      gfx_text(x + 1, y, s, fg, GFX_TRANSPARENT);
    return x + width;
  }
  int by = y + baseline; /* screen baseline shared by runs */
  int eff = px ? px : (int)BODY_PX;
  gcache_sync(&g_ui);
  while (s < end) {
    s += utf8_decode(s, end, &cp);
    if (zero_width(cp))
      continue;
    int flags;
    int face = pick_face(&g_ui, set, cp, &flags);
    const struct ru_gent *g = gcache_get(&g_ui, face, cp, eff);
    int sk = italic && !(flags & PK_ITALIC);
    if (g->bm) {
      blit_glyph(x, by, g, fg, ct, cb, sk);
      if (bold && !(flags & PK_BOLD)) /* fake bold: smear the glyph about 1/24 em to the right */
        for (int k = 1; k <= (eff >= 48 ? eff / 24 : 1); k++)
          blit_glyph(x + k, by, g, fg, ct, cb, sk);
    } else if (cp > 0x20 && !font_has(face_font(face), cp)) {
      draw_tofu(x, y, g->adv, fg);
    }
    x += g->adv;
  }
  return x;
}

/* Ascent (line-top → baseline) of font set `set` at `px`: that of the first family in its stack the page has */
int run_ascent(int px, int set)
{
  if (px <= 0)
    px = (int)BODY_PX;
  const struct font *f = 0;
  const struct font_set *fs = set > 0 && set < B->pg->n_fsets ? &B->pg->fsets[set] : 0;
  for (const char *p = fs ? fs->stack : ""; *p && !f;) {
    char name[CSS_FAM_MAX];
    int n = 0;
    while (*p && *p != ',' && n < (int)sizeof name - 1)
      name[n++] = *p++;
    name[n] = '\0';
    while (*p && *p != ',')
      p++;
    if (*p == ',')
      p++;
    for (int i = 0; i < B->pg->n_faces && !f; i++)
      if (streq(B->pg->faces[i].family, name))
        f = B->pg->faces[i].font;
    if (!f && name[0]) {
      if (fam_generic_sans(name))
        break;
      int k = fam_generic_mono(name) ? LF_MONO : fam_generic_emoji(name) ? -1 : local_named(name);
      if (k >= 0)
        f = local_font(k);
    }
  }
  if (!f)
    return gfx_ui_ascent_px(px);
  int a = 0;
  font_vmetrics(f, (float)px, &a, 0, 0);
  return a;
}

/* Display height of a text line from its largest font px and largest CSS line-height */
int line_height(int maxpx, int lhmax)
{
  if (lhmax > 0)
    return lhmax < 6 ? 6 : lhmax;
  if (maxpx <= (int)BODY_PX)
    return LINE_H;
  int lh = maxpx + (maxpx + 2) / 4;
  return lh < LINE_H ? LINE_H : lh;
}

/* While a flex-row item is being flowed, every wrapped line is captured into the overlay */
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
/* Decide an inline image's drawn size (dw x dh px) within a pane `paw` px wide, honouring */
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

/* Highest valid scroll line */
int scroll_max(void)
{
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  int max = g_fr->doc_h - phh;
  return max < 0 ? 0 : max;
}
