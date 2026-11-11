/* pdftext.c, PDF text showing: glyph bitmaps from the font rasterizer, placed by the text and page matrices */
#include "math.h"
#include "owos.h"
#include "pdfint.h"

static struct gcache_ent *glyph_get(struct rctx *rc, const struct font *f, int gid, int px8)
{
  unsigned h = ((unsigned)(unsigned long)f >> 4) * 31u + (unsigned)gid * 2654435761u + (unsigned)px8 * 40503u;
  struct gcache_ent *e = &rc->glyphs[(h >> 8) & 255];
  if (e->f == f && e->gid == gid && e->px8 == px8)
    return e;
  free(e->bmp);
  int adv;
  e->bmp = font_render_glyph(f, gid, px8 / 8.0f, &e->w, &e->h, &e->ox, &e->oy, &adv);
  e->f = f;
  e->gid = gid;
  e->px8 = px8;
  return e;
}

void pdf_glyphs_free(struct rctx *rc)
{
  for (int i = 0; i < 256; i++)
    free(rc->glyphs[i].bmp);
}

static void blit_upright(struct rctx *rc, const struct gcache_ent *e, int x0, int y0, const unsigned char *rgb,
                         double alpha)
{
  struct c2d *c = rc->c;
  const struct c2d_mask *clip = c->st[c->depth].clip;
  unsigned a255 = (unsigned)(alpha * 255 + 0.5);
  for (int j = 0; j < e->h; j++) {
    int y = y0 + j;
    if (y < 0 || y >= c->h)
      continue;
    for (int i = 0; i < e->w; i++) {
      int x = x0 + i;
      unsigned cov = e->bmp[j * e->w + i];
      if (!cov || x < 0 || x >= c->w)
        continue;
      unsigned long idx = (unsigned long)y * c->w + x;
      unsigned a = cov * a255 / 255;
      if (clip)
        a = a * clip->px[idx] / 255;
      unsigned char *p = c->px + idx * 4;
      for (int k = 0; k < 3; k++)
        p[k] = (unsigned char)((p[k] * (255 - a) + rgb[k] * a + 127) / 255);
      p[3] = 255;
    }
  }
}

static void draw_glyph(struct rctx *rc, const struct glyph_ref *gr)
{
  struct gstate *g = &rc->gs;
  if (g->tr == 3 || g->tr == 7)
    return;
  struct c2d_mat tm = {(float)rc->tm[0], (float)rc->tm[1], (float)rc->tm[2],
                       (float)rc->tm[3], (float)rc->tm[4], (float)rc->tm[5]};
  struct c2d_mat s = {(float)(g->fs * g->th), 0, 0, (float)g->fs, 0, (float)g->rise};
  struct c2d_mat m = c2d_mat_mul(c2d_mat_mul(g->ctm, tm), s);
  float ex = (float)hypot(m.a, m.b), ey = (float)hypot(m.c, m.d);
  if (ey < 0.8f || ey > 600 || ex - ex != 0 || ey - ey != 0)
    return;
  int px8 = (int)(ey * 8 + 0.5f);
  const struct gcache_ent *e = glyph_get(rc, gr->f, gr->gid, px8);
  if (!e->bmp)
    return;
  int stroke = g->tr == 1 || g->tr == 5;
  const unsigned char *rgb = stroke ? g->stroke.rgb : g->fill.rgb;
  double alpha = stroke ? g->sa : g->fa;
  int upright = fabs(m.b) < 0.02f * ey && fabs(m.c) < 0.02f * ey && m.a > 0 && m.d < 0 && fabs(ex - ey) < 0.03f * ey;
  if (upright) {
    blit_upright(rc, e, (int)floor(m.e + 0.5f) + e->ox, (int)floor(m.f + 0.5f) + e->oy, rgb, alpha);
    return;
  }
  unsigned char *tile = malloc((unsigned)e->w * e->h * 4);
  if (!tile)
    return;
  for (int i = 0; i < e->w * e->h; i++) {
    tile[i * 4] = rgb[0];
    tile[i * 4 + 1] = rgb[1];
    tile[i * 4 + 2] = rgb[2];
    tile[i * 4 + 3] = e->bmp[i];
  }
  float px = px8 / 8.0f;
  struct c2d_mat t = {1 / px, 0, 0, -1 / px, e->ox / px, -e->oy / px};
  struct c2d_state *st = &rc->c->st[rc->c->depth];
  st->m = c2d_mat_mul(m, t);
  st->alpha = (float)alpha;
  c2d_draw_image(rc->c, tile, e->w, e->h, 0, 0, (float)e->w, (float)e->h, 0, 0, (float)e->w, (float)e->h);
  st->m = C2D_IDENTITY;
  st->alpha = 1;
  free(tile);
}

static void record_glyph(struct rctx *rc, const struct glyph_ref *gr, double w)
{
  struct gstate *g = &rc->gs;
  if (!gr->uni || gr->uni == ' ' || gr->uni == 0xa0)
    return;
  struct c2d_mat tm = {(float)rc->tm[0], (float)rc->tm[1], (float)rc->tm[2],
                       (float)rc->tm[3], (float)rc->tm[4], (float)rc->tm[5]};
  struct c2d_mat s = {(float)(g->fs * g->th), 0, 0, (float)g->fs, 0, (float)g->rise};
  struct c2d_mat m = c2d_mat_mul(c2d_mat_mul(g->ctm, tm), s);
  float xs[4], ys[4], we = (float)(w / 1000);
  c2d_mat_apply(&m, 0, -0.2f, &xs[0], &ys[0]);
  c2d_mat_apply(&m, we, -0.2f, &xs[1], &ys[1]);
  c2d_mat_apply(&m, 0, 0.8f, &xs[2], &ys[2]);
  c2d_mat_apply(&m, we, 0.8f, &xs[3], &ys[3]);
  struct pdf_glyph gl = {xs[0], ys[0], xs[0], ys[0], (unsigned)gr->uni};
  for (int i = 1; i < 4; i++) {
    gl.x0 = xs[i] < gl.x0 ? xs[i] : gl.x0;
    gl.x1 = xs[i] > gl.x1 ? xs[i] : gl.x1;
    gl.y0 = ys[i] < gl.y0 ? ys[i] : gl.y0;
    gl.y1 = ys[i] > gl.y1 ? ys[i] : gl.y1;
  }
  if (gl.x0 - gl.x0 != 0 || gl.y0 - gl.y0 != 0 || gl.x1 - gl.x1 != 0 || gl.y1 - gl.y1 != 0)
    return;
  if (rc->ntext == rc->text_cap) {
    int cap = rc->text_cap ? rc->text_cap * 2 : 1024;
    struct pdf_glyph *nt = realloc(rc->text, (unsigned)cap * sizeof *nt);
    if (!nt || cap > (1 << 22))
      return;
    rc->text = nt;
    rc->text_cap = cap;
  }
  rc->text[rc->ntext++] = gl;
}

void pdf_show_text(struct rctx *rc, const unsigned char *s, int n)
{
  struct gstate *g = &rc->gs;
  struct pfont *pf = g->font;
  if (!pf)
    return;
  while (n > 0) {
    int code, space;
    double w;
    struct glyph_ref gr;
    int used = pdf_font_next(rc->d, pf, s, n, &code, &w, &gr, &space);
    s += used;
    n -= used;
    if (g->fs != 0) {
      if (rc->no_paint)
        record_glyph(rc, &gr, w);
      else if (gr.ok)
        draw_glyph(rc, &gr);
    }
    double tx = (w / 1000 * g->fs + g->tc + (space ? g->tw : 0)) * g->th;
    rc->tm[4] += tx * rc->tm[0];
    rc->tm[5] += tx * rc->tm[1];
  }
}
