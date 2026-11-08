/* text.c - canvas2d text: glyph coverage from the font rasterizer, placed by align/baseline and the transform. */
#include "c2int.h"
#include "font.h"
#include "math.h"
#include "owos.h"

static unsigned next_cp(const char **s)
{
  const unsigned char *p = (const unsigned char *)*s;
  unsigned cp = *p;
  int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
  if (cp >= 0xf8 || (cp >= 0x80 && cp < 0xc0))
    extra = 0, cp = 0xfffd;
  else if (extra) {
    cp &= 0x3f >> extra;
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

static const struct font *pick_font(const struct c2d *c)
{
  const struct c2d_state *st = &c->st[c->depth];
  const struct font *want = st->font_mono ? c->mono : c->sans;
  return want ? want : (st->font_mono ? c->sans : c->mono);
}

float c2d_measure_text(const struct c2d *c, const char *utf8)
{
  const struct font *f = pick_font(c);
  float px = c->st[c->depth].font_px, w = 0;
  if (!f)
    return 0;
  while (*utf8) {
    int adv = 0, lsb = 0;
    font_glyph_hmetrics(f, font_glyph_index(f, (int)next_cp(&utf8)), px, &adv, &lsb);
    w += (float)adv;
  }
  return w;
}

/* Draw `utf8` into a bw*bh coverage buffer with the pen starting at (ox,oy) on the baseline. */
static void render_run(const struct font *f, const char *utf8, float px, bool bold, unsigned char *buf, int bw, int bh,
                       int ox, int oy)
{
  int pen = ox;
  while (*utf8) {
    int gi = font_glyph_index(f, (int)next_cp(&utf8));
    int gw, gh, gx, gy, adv;
    unsigned char *g = font_render_glyph(f, gi, px, &gw, &gh, &gx, &gy, &adv);
    for (int pass = 0; g && pass < (bold ? 2 : 1); pass++)
      for (int y = 0; y < gh; y++) {
        int ty = oy + gy + y;
        if (ty < 0 || ty >= bh)
          continue;
        for (int x = 0; x < gw; x++) {
          int tx = pen + gx + x + pass;
          unsigned char v = g[y * gw + x];
          if (tx >= 0 && tx < bw && v > buf[ty * bw + tx])
            buf[ty * bw + tx] = v;
        }
      }
    free(g);
    pen += adv;
  }
}

static float bilinear_cov(const unsigned char *t, int tw, int th, float u, float v)
{
  u -= 0.5f;
  v -= 0.5f;
  int x0 = (int)floor(u), y0 = (int)floor(v);
  float fx = u - (float)x0, fy = v - (float)y0, acc = 0;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      int x = x0 + i, y = y0 + j;
      if (x >= 0 && x < tw && y >= 0 && y < th)
        acc += (float)t[y * tw + x] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
    }
  return acc;
}

void c2d_fill_text(struct c2d *c, const char *utf8, float x, float y, bool stroke_paint)
{
  struct c2d_state *st = &c->st[c->depth];
  const struct font *f = pick_font(c);
  float px = st->font_px;
  if (!f || !*utf8 || px <= 0 || x - x != 0 || y - y != 0)
    return;
  float width = c2d_measure_text(c, utf8);
  int asc, desc, gap;
  font_vmetrics(f, px, &asc, &desc, &gap);
  float ax = (st->text_align == C2D_ALIGN_END || st->text_align == C2D_ALIGN_RIGHT) ? -width
             : st->text_align == C2D_ALIGN_CENTER                                   ? -width / 2
                                                                                    : 0;
  float shift = 0;
  switch (st->text_base) {
  case C2D_BASE_TOP:
    shift = (float)asc;
    break;
  case C2D_BASE_HANGING:
    shift = (float)asc * 0.8f;
    break;
  case C2D_BASE_MIDDLE:
    shift = (float)(asc + desc) / 2;
    break;
  case C2D_BASE_IDEO:
  case C2D_BASE_BOTTOM:
    shift = (float)desc;
    break;
  }
  const struct c2d_paint *paint = stroke_paint ? &st->stroke : &st->fill;
  struct c2d_src src;
  if (!c2d_src_from_paint(&src, paint, &st->m, st->smoothing))
    return;

  const struct c2d_mat *m = &st->m;
  bool plain = m->b == 0 && m->c == 0 && m->a == m->d && m->a > 0;
  int tw = (int)ceil(width) + (int)px * 2 + 4, th = asc - desc + 8;
  int top = asc + 4; /* baseline row inside the text buffer */
  int pad = (int)px + 2;
  if (plain) {
    float epx = px * m->a;
    int ebw, ebh, eoy;
    int easc, edesc, egap;
    font_vmetrics(f, epx, &easc, &edesc, &egap);
    float dx0, dy0;
    c2d_mat_apply(m, x + ax, y + shift, &dx0, &dy0);
    ebw = (int)ceil(width * m->a) + (int)epx * 2 + 4;
    ebh = easc - edesc + 8;
    eoy = easc + 4;
    int bx = (int)floor(dx0) - (int)epx - 2, by = (int)floor(dy0) - eoy;
    unsigned char *buf = calloc((unsigned)ebw, (unsigned)ebh);
    if (!buf)
      return;
    render_run(f, utf8, epx, st->font_bold, buf, ebw, ebh, (int)epx + 2, eoy);
    int cx0 = bx < 0 ? -bx : 0, cy0 = by < 0 ? -by : 0;
    int cx1 = bx + ebw > c->w ? c->w - bx : ebw, cy1 = by + ebh > c->h ? c->h - by : ebh;
    if (cx1 > cx0 && cy1 > cy0) {
      int cw = cx1 - cx0, ch = cy1 - cy0;
      unsigned char *cov = malloc((unsigned long)cw * ch);
      if (cov) {
        for (int j = 0; j < ch; j++)
          memcpy(cov + (unsigned long)j * cw, buf + (unsigned long)(cy0 + j) * ebw + cx0, (unsigned)cw);
        c2d_draw_cov(c, cov, bx + cx0, by + cy0, cw, ch, &src, st->alpha, st->comp, true);
        free(cov);
      }
    }
    free(buf);
    return;
  }

  unsigned char *buf = calloc((unsigned)tw, (unsigned)th);
  if (!buf)
    return;
  render_run(f, utf8, px, st->font_bold, buf, tw, th, pad, top);
  /* Text-buffer pixel (u,v) is user point (x + ax + u - pad, y + shift + v - top) */
  struct c2d_mat to_dev = c2d_mat_mul(*m, (struct c2d_mat){1, 0, 0, 1, x + ax - (float)pad, y + shift - (float)top});
  struct c2d_mat inv;
  struct c2d_path box;
  memset(&box, 0, sizeof box);
  c2d_path_rect(&box, &to_dev, 0, 0, (float)tw, (float)th);
  int x0, y0, x1, y1;
  if (c2d_mat_invert(&to_dev, &inv) && c2d_path_bounds(&box, 1, c->w, c->h, &x0, &y0, &x1, &y1)) {
    int cw = x1 - x0, ch = y1 - y0;
    unsigned char *cov = malloc((unsigned long)cw * ch);
    if (cov) {
      for (int j = 0; j < ch; j++)
        for (int i = 0; i < cw; i++) {
          float u, v;
          c2d_mat_apply(&inv, (float)(x0 + i) + 0.5f, (float)(y0 + j) + 0.5f, &u, &v);
          float a = bilinear_cov(buf, tw, th, u, v);
          cov[(unsigned long)j * cw + i] = a >= 255 ? 255 : (unsigned char)(a + 0.5f);
        }
      c2d_draw_cov(c, cov, x0, y0, cw, ch, &src, st->alpha, st->comp, true);
      free(cov);
    }
  }
  c2d_path_free(&box);
  free(buf);
}
