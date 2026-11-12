#include "main_int.h"

/* --------------------------------------------------------------- drawing */

/* A line's baseline: the tallest ascent at the line's font size among the fonts its runs use. */
static int line_ascent(int off, int len, int maxpx)
{
  int best = run_ascent(maxpx, glyph_set(off)), seen = glyph_set(off);
  for (int p = off; p < off + len; p += utf8_seqlen((unsigned char)B->pg->text[p])) {
    int set = glyph_set(p);
    if (set == seen)
      continue;
    seen = set;
    int a = run_ascent(maxpx, set);
    if (a > best)
      best = a;
  }
  return best;
}

/* Draw one wrapped text run [off,off+len) at (x,y), clipped vertically to [ct,cb) */
int draw_slice_core(int x, int y, int off, int len, int paw, int ct, int cb, int line_h, int maxpx)
{
  set_block(&g_ui, off);

  /* A horizontal rule is a whole-line element: a centred grey bar, no text. */
  if (g_ui.bhr) {
    int m = g_ui.bindent * INDENT_PX;
    gfx_fill_rect(x + m, y + LINE_H / 2, paw - m, 1, 0xc4c8cf);
    return x + m;
  }

  int col_right = x + paw;                 /* the column's right edge, before indent and alignment */
  x += g_ui.bindent * INDENT_PX;           /* blockquote / list indent  */
  int base = line_ascent(off, len, maxpx); /* baseline shared by the line's runs */
  if (g_ui.bcenter == 1 || g_ui.bcenter == 2) {
    int lw = 0, p = off, end = off + len;
    unsigned cp;
    while (p < end) {
      int gp = p;
      p += utf8_decode(&B->pg->text[p], &B->pg->text[end], &cp);
      lw += glyph_adv(&g_ui, cp, gp);
    }
    int room = (paw - g_ui.bindent * INDENT_PX) - lw;
    if (room > 0)
      x += g_ui.bcenter == 1 ? room / 2 : room;
  }
  bool ellipsis = false;
  if (len > 0 && (B->pg->attr[off] & ST_CLIP)) { /* overflow:hidden: stop at the column edge, optionally with "..." */
    int room = col_right - x, used = 0, keep = 0;
    bool fits = true;
    for (int p = 0; p < len;) {
      unsigned cp;
      int nb = utf8_decode(&B->pg->text[off + p], &B->pg->text[off + len], &cp);
      used += glyph_adv(&g_ui, cp, off + p);
      p += nb;
      if (used > room) {
        fits = false;
        break;
      }
    }
    if (!fits) {
      bool dots = (B->pg->attr[off] & ST_ELLIPSIS) != 0;
      int avail = room - (dots ? 3 * body_cp_adv(&g_ui, '.', glyph_px(off), glyph_set(off)) : 0), w = 0;
      for (int p = 0; p < len;) {
        unsigned cp;
        int nb = utf8_decode(&B->pg->text[off + p], &B->pg->text[off + len], &cp);
        w += glyph_adv(&g_ui, cp, off + p);
        if (w > avail)
          break;
        p += nb;
        keep = p;
      }
      len = keep;
      ellipsis = dots;
    }
  }
  int x0 = x; /* line content origin (for selection) */
  int justify_extra = 0, justify_rem = 0, justify_spaces = 0;
  if (g_ui.bcenter == 3) {
    int lw = 0, p = off, end = off + len;
    unsigned cp;
    while (p < end) {
      int gp = p;
      p += utf8_decode(&B->pg->text[p], &B->pg->text[end], &cp);
      lw += glyph_adv(&g_ui, cp, gp);
      if (cp == ' ')
        justify_spaces++;
    }
    int room = paw - g_ui.bindent * INDENT_PX - lw;
    if (justify_spaces > 0 && room > 0) {
      justify_extra = room / justify_spaces;
      justify_rem = room % justify_spaces;
    }
  }

  int i = 0;
  while (i < len) {
    unsigned char a = B->pg->attr[off + i];
    unsigned fg = B->pg->fg[off + i];
    unsigned bgc = B->pg->bg[off + i];
    int rpx = glyph_px(off + i), rset = glyph_set(off + i);
    int rls = B->pg->ls[off + i], rva = B->pg->va[off + i];
    char run[260];
    int rl = 0, rw = 0;
    /* Accumulate codepoints sharing one (attr,fg,bg,size,font,spacing,shift) so any change starts a new run */
    while (i < len && B->pg->attr[off + i] == a && B->pg->fg[off + i] == fg && B->pg->bg[off + i] == bgc &&
           glyph_px(off + i) == rpx && glyph_set(off + i) == rset && B->pg->ls[off + i] == rls &&
           B->pg->va[off + i] == rva) {
      int nb = utf8_seqlen((unsigned char)B->pg->text[off + i]);
      if (i + nb > len)
        nb = 1;
      if (rl + nb > (int)sizeof run - 1)
        break;
      unsigned cp;
      utf8_decode(&B->pg->text[off + i], &B->pg->text[off + len], &cp);
      rw += body_cp_adv(&g_ui, cp, rpx, rset) + rls;
      for (int b = 0; b < nb; b++)
        run[rl++] = B->pg->text[off + i++];
    }
    run[rl] = '\0';
    if (bgc != BG_NONE && !(a & ST_HIDDEN))
      gfx_fill_rect(x, y, rw, line_h, bgc);
    int ital = (a & ST_ITALIC) ? 1 : 0, bold = (a & ST_BOLD) ? 1 : 0;
    if (!(a & ST_HIDDEN)) {
      if (rls) { /* letter-spaced: place each glyph on its own */
        int gx = x;
        for (int p = 0; p < rl;) {
          unsigned cp;
          int n = utf8_decode(&run[p], &run[rl], &cp);
          int adv = body_cp_adv(&g_ui, cp, rpx, rset);
          char one[5]; /* body_draw_run reads to the terminator */
          for (int q = 0; q < n; q++)
            one[q] = run[p + q];
          one[n] = '\0';
          body_draw_run(gx, y + rva, one, n, adv, fg, ct, cb, rpx, rset, base, ital, bold);
          gx += adv + rls;
          p += n;
        }
      } else {
        body_draw_run(x, y + rva, run, rl, rw, fg, ct, cb, rpx, rset, base, ital, bold);
      }
      if (a & ST_UNDER)
        gfx_fill_rect(x, y + base + 1, rw, 1, fg);
      if (a & ST_STRIKE)
        gfx_fill_rect(x, y + base - rpx / 3, rw, 1, fg);
    }
    x += rw;
    if (g_ui.bcenter == 3 && justify_spaces > 0) {
      int spaces = 0;
      for (int p = 0; p < rl;) {
        unsigned cp;
        int n = utf8_decode(&run[p], &run[rl], &cp);
        if (cp == ' ')
          spaces++;
        p += n;
      }
      for (int n = 0; n < spaces; n++) {
        x += justify_extra;
        if (justify_rem > 0) {
          x++;
          justify_rem--;
        }
      }
    }
  }
  if (ellipsis) {
    int rpx = glyph_px(off), rset = glyph_set(off);
    body_draw_run(x, y, "...", 3, 3 * body_cp_adv(&g_ui, '.', rpx, rset), B->pg->fg[off], ct, cb, rpx, rset, base, 0,
                  0);
  }
  return x0;
}

void draw_styled_line(int x, int y, int idx, int paw, int ct, int cb)
{
  int off = g_fr->lt.off[idx], len = g_fr->lt.len[idx];
  int x0 = draw_slice_core(x, y, off, len, paw, ct, cb, g_fr->lt.h[idx], g_fr->lt.px[idx]);
  if (g_ui.bhr)
    return; /* rule: nothing to select   */

  /* Text selection: tint the selected byte span [s,e) of this display line. */
  int l0, c0, l1, c1;
  if (gfx_sel_range(&g_sel, &l0, &c0, &l1, &c1) && idx >= l0 && idx <= l1) {
    int s = (idx == l0) ? c0 : 0, e = (idx == l1) ? c1 : len;
    if (s < 0)
      s = 0;
    if (e > len)
      e = len;
    if (e > s) {
      int xs = x0, xe;
      unsigned cp;
      for (int p = off; p < off + s;) {
        int gp = p;
        p += utf8_decode(&B->pg->text[p], &B->pg->text[off + len], &cp);
        xs += glyph_adv(&g_ui, cp, gp);
      }
      xe = xs;
      for (int p = off + s; p < off + e;) {
        int gp = p;
        p += utf8_decode(&B->pg->text[p], &B->pg->text[off + len], &cp);
        xe += glyph_adv(&g_ui, cp, gp);
      }
      sel_hl_rect(xs, y, xe - xs, g_fr->lt.h[idx], ct, cb, sel_color_at(off + s));
    }
  }
}

/* ---- CSS background gradients & url() images -------------------------------- Painted per-pixel */

static int isin90(int d) /* sin(d°), d in [0,90], scaled by 1024 */
{
  static const short t[19] = {0,   89,  178, 265, 350, 433, 512,  587,  658, 724,
                              784, 839, 887, 928, 962, 989, 1008, 1019, 1024};
  if (d < 0)
    d = 0;
  if (d > 90)
    d = 90;
  int i = d / 5, f = d % 5, a = t[i], b = t[i < 18 ? i + 1 : 18];
  return a + (b - a) * f / 5;
}
static int isin(int d)
{
  d %= 360;
  if (d < 0)
    d += 360;
  return d <= 90 ? isin90(d) : d <= 180 ? isin90(180 - d) : d <= 270 ? -isin90(d - 180) : -isin90(360 - d);
}
static int icos(int d) { return isin(d + 90); }

/* One colour table per distinct stop set, shared by every gradient box. */
static struct ru_glut g_glut;

/* Paint a linear/radial gradient into the box rect, clipped to the visible band [by,by+bh) */
static void paint_gradient(const struct css_bg_image *bi, int bx, int sy, int bw, int bh_full, int by, int bh,
                           int clipw)
{
  if (bw <= 0 || bh_full <= 0)
    return;
  struct ru_gstops g;
  ru_resolve_stops(bi, &g);
  if (g.n == 0)
    return;
  const struct ru_glut *lut = ru_glut_get(&g_glut, &g);
  unsigned *fb = browser_surface();
  int x0 = bx < 0 ? 0 : bx, x1 = bx + bw;
  if (x1 > clipw)
    x1 = clipw;
  if (x1 > BW)
    x1 = BW;
  int cx = bw / 2, cy = bh_full / 2;

  if (bi->kind == CSS_BGK_LINEAR) {
    int dx = isin(bi->angle), dy = -icos(bi->angle); /* unit dir, Q10 */
    long Lq = (long)(dx < 0 ? -dx : dx) * bw + (long)(dy < 0 ? -dy : dy) * bh_full;
    if (Lq < 1)
      Lq = 1; /* Q10 px projection */
    for (int yy = by; yy < by + bh; yy++) {
      int ry = yy - sy;
      long rowdot = (long)(ry - cy) * dy;
      ru_grad_linear_row(fb + (long)yy * BW, x0, x1, bx, cx, dx, rowdot, Lq, lut);
    }
  } else { /* radial (centred) */
    int hw = bw / 2 ? bw / 2 : 1, hh = bh_full / 2 ? bh_full / 2 : 1;
    unsigned rad = bi->rad_circle ? ru_isqrt((unsigned)(hw * hw + hh * hh)) : 0;
    for (int yy = by; yy < by + bh; yy++)
      ru_grad_radial_row(fb + (long)yy * BW, x0, x1, bx, cx, yy - sy - cy, bi->rad_circle, hw, hh, rad, lut);
  }
}

/* Paint a decoded url() background image into the box rect, honouring background- size */
static void paint_bg_image(const struct bg_img *im, const struct css_bg_image *bi, int bx, int sy, int bw, int bh_full,
                           int by, int bh, int clipw)
{
  if (!im->rgba || !im->iw || !im->ih || bw <= 0 || bh_full <= 0)
    return;
  int iw = (int)im->iw, ih = (int)im->ih;

  int dw, dh; /* displayed image size */
  switch (bi->size_kind) {
  case CSS_BGS_COVER: {
    int sw = bw * 1000 / iw, sh = bh_full * 1000 / ih, s = sw > sh ? sw : sh;
    dw = iw * s / 1000;
    dh = ih * s / 1000;
    break;
  }
  case CSS_BGS_CONTAIN: {
    int sw = bw * 1000 / iw, sh = bh_full * 1000 / ih, s = sw < sh ? sw : sh;
    dw = iw * s / 1000;
    dh = ih * s / 1000;
    break;
  }
  case CSS_BGS_LEN:
    dw = bi->size_w >= 0 ? bi->size_w : -1;
    dh = bi->size_h >= 0 ? bi->size_h : -1;
    if (dw < 0 && dh < 0) {
      dw = iw;
      dh = ih;
    } else if (dw < 0)
      dw = iw * dh / ih; /* auto keeps aspect */
    else if (dh < 0)
      dh = ih * dw / iw;
    break;
  default:
    dw = iw;
    dh = ih;
    break; /* auto = natural size */
  }
  if (dw < 1)
    dw = 1;
  if (dh < 1)
    dh = 1;

  int posx = bi->pos_x >= 0 ? bi->pos_x : 0, posy = bi->pos_y >= 0 ? bi->pos_y : 0;
  int offx = (bw - dw) * posx / 100, offy = (bh_full - dh) * posy / 100;
  int rep = bi->repeat < 0 ? CSS_BGR_REPEAT : bi->repeat;
  int tilex = (rep == CSS_BGR_REPEAT || rep == CSS_BGR_REPEAT_X);
  int tiley = (rep == CSS_BGR_REPEAT || rep == CSS_BGR_REPEAT_Y);

  unsigned *fb = browser_surface();
  const unsigned *xm = xmap_get((unsigned)dw, (unsigned)iw);
  int x0 = bx < 0 ? 0 : bx, x1 = bx + bw;
  if (x1 > clipw)
    x1 = clipw;
  if (x1 > BW)
    x1 = BW;
  for (int yy = by; yy < by + bh; yy++) {
    int ry = yy - sy - offy;
    if (tiley) {
      ry %= dh;
      if (ry < 0)
        ry += dh;
    } else if (ry < 0 || ry >= dh)
      continue;
    int syimg = ry * ih / dh;
    const unsigned char *srow = im->rgba + (long)syimg * iw * 4;
    unsigned *drow = fb + (long)yy * BW;
    for (int xx = x0; xx < x1; xx++) {
      int rx = xx - bx - offx;
      if (tilex) {
        rx %= dw;
        if (rx < 0)
          rx += dw;
      } else if (rx < 0 || rx >= dw)
        continue;
      const unsigned char *p = srow + (long)(xm ? xm[rx] : (unsigned)(rx * iw / dw)) * 4;
      drow[xx] = ru_blend_rgba(p, drow[xx]);
    }
  }
}

/* Paint visible block-box backgrounds and borders from the box tree's resolved geometry */
/* Integer sqrt for the rounded-corner geometry. */
static int isqrt_i(int n)
{
  if (n <= 0)
    return 0;
  int x = n, y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + n / x) / 2;
  }
  return x;
}
/* Fill rounded rect [x,y,w,h] radius r with `rgb` at `alpha` (0..255), clipped to the vertical */
static void fill_round_rect(int x, int y, int w, int h, int r, unsigned rgb, int alpha, int ct, int cb)
{
  if (w <= 0 || h <= 0 || alpha <= 0)
    return;
  if (r < 0)
    r = 0;
  if (r * 2 > w)
    r = w / 2;
  if (r * 2 > h)
    r = h / 2;
  unsigned *fb = browser_surface();
  unsigned sr = (rgb >> 16) & 0xff, sg = (rgb >> 8) & 0xff, sb = rgb & 0xff, ia = 255 - alpha;
  for (int yy = 0; yy < h; yy++) {
    int p_y = y + yy;
    if (p_y < ct || p_y >= cb || p_y < 0 || p_y >= BH)
      continue;
    int cut = 0, d = -1;
    if (yy < r)
      d = r - yy;
    else if (yy >= h - r)
      d = yy - (h - 1 - r);
    if (d >= 0 && d <= r)
      cut = r - isqrt_i(r * r - d * d);
    int xa = x + cut, xb = x + w - cut;
    if (xa < 0)
      xa = 0;
    if (xb > BW)
      xb = BW;
    unsigned *row = fb + (unsigned long)p_y * BW;
    if (alpha >= 255) {
      for (int px = xa; px < xb; px++)
        row[px] = rgb;
    } else
      for (int px = xa; px < xb; px++) {
        unsigned dd = row[px];
        unsigned rr = (sr * alpha + ((dd >> 16) & 0xff) * ia) / 255;
        unsigned gg = (sg * alpha + ((dd >> 8) & 0xff) * ia) / 255;
        unsigned bb = (sb * alpha + (dd & 0xff) * ia) / 255;
        row[px] = (rr << 16) | (gg << 8) | bb;
      }
  }
}
/* A box's drop shadow */
static void draw_box_shadow(const struct lbox *b, const struct lgeom *ge, int sy, int r, int ct, int cb)
{
  unsigned argb = b->sh_color;
  int alpha = (int)((argb >> 24) & 0xff);
  if (alpha == 0)
    alpha = 64;
  unsigned rgb = argb & 0xffffff;
  int sp = b->sh_spread, bl = b->sh_blur < 0 ? 0 : b->sh_blur;
  int bx = ge->x + b->sh_dx, by = sy + b->sh_dy;
  if (bl <= 0) {
    int g = sp, rr = r + g;
    if (rr < 0)
      rr = 0;
    fill_round_rect(bx - g, by - g, ge->w + 2 * g, ge->h + 2 * g, rr, rgb, alpha, ct, cb);
    return;
  }
  /* The blur fades across about `bl` px centred on the shadow's edge: stack thin rings, outermost first, each
   * adding its share of the opacity, so the falloff is smooth instead of stepped. */
  int n = bl / 2 < 3 ? 3 : bl / 2 > 16 ? 16 : bl / 2;
  int a = (alpha + n - 1) / n;
  for (int pass = 0; pass < n; pass++) {
    int g = sp + bl / 2 - bl * pass / n;
    int rr = r + g;
    if (rr < 0)
      rr = 0;
    fill_round_rect(bx - g, by - g, ge->w + 2 * g, ge->h + 2 * g, rr, rgb, a, ct, cb);
  }
}

/* Fill a rect clipped to the vertical pane band [ct,cb). */
static void fill_clip(int x, int y, int w, int h, unsigned rgb, int ct, int cb)
{
  int y0 = y < ct ? ct : y, y1 = y + h > cb ? cb : y + h;
  if (y1 > y0 && w > 0)
    gfx_fill_rect(x, y0, w, y1 - y0, rgb);
}

static void draw_border_h(int x, int y, int w, int h, int style, unsigned color, int ct, int cb)
{
  if (style <= CSS_BST_SOLID) {
    fill_clip(x, y, w, h, color, ct, cb);
    return;
  }
  int period = style == CSS_BST_DOTTED ? h * 2 : h * 5;
  int on = style == CSS_BST_DOTTED ? h : h * 3;
  if (period < 1)
    period = 1;
  if (on < 1)
    on = 1;
  for (int p = 0; p < w; p += period)
    fill_clip(x + p, y, on < w - p ? on : w - p, h, color, ct, cb);
}

static void draw_border_v(int x, int y, int w, int h, int style, unsigned color, int ct, int cb)
{
  if (style <= CSS_BST_SOLID) {
    fill_clip(x, y, w, h, color, ct, cb);
    return;
  }
  int period = style == CSS_BST_DOTTED ? w * 2 : w * 5;
  int on = style == CSS_BST_DOTTED ? w : w * 3;
  if (period < 1)
    period = 1;
  if (on < 1)
    on = 1;
  for (int p = 0; p < h; p += period)
    fill_clip(x, y + p, w, on < h - p ? on : h - p, color, ct, cb);
}

/* Paint one box's shadow, background (colour/gradient/image), border and outline into the band */
static void paint_one_box(const struct lbox *src, const struct lgeom *ge, int py, int pw, int ct, int cb, int top_doc)
{
  if (src->hidden)
    return;
  const struct lbox *b = src;
  struct lbox filtered;
  const struct lext *fx = src->ext;
  if (fx && (fx->flt_bright != 1000 || fx->flt_gray || fx->flt_invert || fx->flt_opacity != 100)) {
    filtered = *src;
    if (filtered.bg != BG_NONE)
      filtered.bg = ru_filter_rgb(fx, filtered.bg);
    if (filtered.border_color != BG_NONE)
      filtered.border_color = ru_filter_rgb(fx, filtered.border_color);
    for (int i = 0; i < 4; i++)
      filtered.border_side_color[i] = ru_filter_rgb(fx, filtered.border_side_color[i]);
    if (filtered.outline_color != BG_NONE)
      filtered.outline_color = ru_filter_rgb(fx, filtered.outline_color);
    if (filtered.sh_color != BG_NONE)
      filtered.sh_color = (filtered.sh_color & 0xff000000u) | ru_filter_rgb(fx, filtered.sh_color & 0xffffffu);
    filtered.opacity = (signed char)((filtered.opacity < 0 ? 100 : filtered.opacity) * fx->flt_opacity / 100);
    b = &filtered;
  }
  if (fx && fx->clip_kind > CSS_CLIP_NONE) { /* the shape clips the solid background only */
    if (b->bg == BG_NONE)
      return;
    int alpha = b->opacity < 0 ? 255 : b->opacity * 255 / 100;
    int sy = py + ge->y - top_doc;
    for (int row = 0; row < ge->h; row++) {
      int xs[2 * CSS_CLIP_PTS];
      int ns = ru_clip_spans(fx, ge->w, ge->h, row, xs);
      for (int k = 0; k < ns; k++) {
        int a = xs[2 * k] < 0 ? 0 : xs[2 * k], e = xs[2 * k + 1] > ge->w ? ge->w : xs[2 * k + 1];
        fill_round_rect(ge->x + a, sy + row, e - a, 1, 0, b->bg, alpha, ct, cb);
      }
    }
    return;
  }
  int bgk = b->bgi.kind; /* CSS_BGK_* image layer, <=0 = none */
  bool has_img = bgk == CSS_BGK_LINEAR || bgk == CSS_BGK_RADIAL ||
                 (bgk == CSS_BGK_URL && b->bgi.img_idx >= 0 && b->bgi.img_idx < B->pg->n_bgimgs &&
                  B->pg->bgimgs[b->bgi.img_idx].rgba);
  bool has_bg = b->bg != BG_NONE;
  bool has_bd = b->border_color != BG_NONE && (b->border.t || b->border.r || b->border.b || b->border.l);
  bool has_ol = b->outline_w > 0 && b->outline_color != BG_NONE;
  if (!has_bg && !has_bd && !has_img && !has_ol && b->sh_color == BG_NONE)
    return;
  int alpha = b->opacity < 0 ? 255 : b->opacity * 255 / 100;
  int sy = py + ge->y - top_doc; /* box top, screen y */
  int r = b->radius;
  if (b->sh_color != BG_NONE) /* drop shadow (clips itself to the band) */
    draw_box_shadow(b, ge, sy, r, ct, cb);
  int by = sy, bh = ge->h;
  if (by < ct) {
    bh -= ct - by;
    by = ct;
  }
  if (by + bh > cb)
    bh = cb - by;
  /* Rounded path only for solid backgrounds; gradients/images stay square. */
  if (bh > 0 && r > 0 && !has_img && has_bg) {
    if (has_bd) { /* border ring: outer colour, inner bg */
      fill_round_rect(ge->x, sy, ge->w, ge->h, r, b->border_color, alpha, ct, cb);
      int il = b->border.l, it = b->border.t, ir = b->border.r, ib = b->border.b;
      int bw = il > it ? il : it;
      if (ir > bw)
        bw = ir;
      if (ib > bw)
        bw = ib;
      int inr = r - bw;
      if (inr < 0)
        inr = 0;
      fill_round_rect(ge->x + il, sy + it, ge->w - il - ir, ge->h - it - ib, inr, b->bg, alpha, ct, cb);
    } else {
      fill_round_rect(ge->x, sy, ge->w, ge->h, r, b->bg, alpha, ct, cb);
    }
  } else if (bh > 0) {
    /* background-clip: the area the background covers, inside the border (and padding for content-box) */
    int cl = b->bg_clip ? b->border.l + (b->bg_clip > 1 ? b->padding.l : 0) : 0;
    int cr = b->bg_clip ? b->border.r + (b->bg_clip > 1 ? b->padding.r : 0) : 0;
    int ctp = b->bg_clip ? b->border.t + (b->bg_clip > 1 ? b->padding.t : 0) : 0;
    int cbt = b->bg_clip ? b->border.b + (b->bg_clip > 1 ? b->padding.b : 0) : 0;
    int gx = ge->x + cl, gw = ge->w - cl - cr, gy = sy + ctp, gh = ge->h - ctp - cbt;
    int gby = gy < ct ? ct : gy, gbh = (gy + gh > cb ? cb : gy + gh) - gby;
    if (has_bg && gw > 0 && gbh > 0) {
      if (alpha < 255 || b->bg_clip)
        fill_round_rect(gx, gby, gw, gbh, 0, b->bg, alpha, ct, cb);
      else
        fill_clip(gx, gby, gw, gbh, b->bg, ct, cb);
    }
    if (has_img && gw > 0 && gbh > 0) {
      if (bgk == CSS_BGK_URL)
        paint_bg_image(&B->pg->bgimgs[b->bgi.img_idx], &b->bgi, gx, gy, gw, gh, gby, gbh, pw);
      else
        paint_gradient(&b->bgi, gx, gy, gw, gh, gby, gbh, pw);
    }
    if (has_bd) { /* 4 edges, clipped to the visible band */
      if (b->border.t && sy >= ct && sy < cb)
        draw_border_h(ge->x, sy, ge->w, b->border.t, b->border_style[0], b->border_side_color[0], ct, cb);
      int bot = sy + ge->h - b->border.b;
      if (b->border.b && bot >= ct && bot < cb)
        draw_border_h(ge->x, bot, ge->w, b->border.b, b->border_style[2], b->border_side_color[2], ct, cb);
      if (b->border.l)
        draw_border_v(ge->x, by, b->border.l, bh, b->border_style[3], b->border_side_color[3], ct, cb);
      if (b->border.r)
        draw_border_v(ge->x + ge->w - b->border.r, by, b->border.r, bh, b->border_style[1], b->border_side_color[1], ct,
                      cb);
    }
  }
  if (has_ol) { /* outline: a ring just outside the border box */
    int ow = b->outline_w;
    unsigned oc = b->outline_color;
    fill_clip(ge->x - ow, sy - ow, ge->w + 2 * ow, ow, oc, ct, cb);    /* top    */
    fill_clip(ge->x - ow, sy + ge->h, ge->w + 2 * ow, ow, oc, ct, cb); /* bottom */
    fill_clip(ge->x - ow, sy, ow, ge->h, oc, ct, cb);                  /* left   */
    fill_clip(ge->x + ge->w, sy, ow, ge->h, oc, ct, cb);               /* right  */
  }
}

/* True if box `i` belongs to an out-of-flow overlay (painted separately) */
static bool in_overlay(const struct layout_out *o, const struct lbox *bx, int i)
{
  for (int k = 0; k < o->n_ov; k++)
    if (i >= o->ov[k].box && i < bx[o->ov[k].box].end)
      return true;
  return false;
}

/* Overlay indices sorted bottom to top by z-index, then document order */
int overlay_order(const struct layout_out *o, int *order)
{
  int n;
  const struct lbox *bx = layout_pool(&n);
  for (int k = 0; k < o->n_ov; k++) {
    int z = bx[o->ov[k].box].z_index == CSS_Z_AUTO ? 0 : bx[o->ov[k].box].z_index, j = k;
    while (j > 0 && (bx[o->ov[order[j - 1]].box].z_index == CSS_Z_AUTO ? 0 : bx[o->ov[order[j - 1]].box].z_index) > z) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = k;
  }
  return o->n_ov;
}

/* Pane-relative top of an overlay's border box in a pane `phh` tall, and the height to draw */
int overlay_place(const struct lovl *ov, int phh, int *h)
{
  int n;
  const struct lbox *b = &layout_pool(&n)[ov->box];
  int hh = ov->h, top = 0;
  if (ov->doc) {
    *h = hh;
    return ov->doc_top - B->pg->scroll;
  }
  if (b->off_t >= 0 && b->off_b >= 0 && b->spec_h < 0 && b->pct_h < 0) { /* top+bottom: stretch */
    if (phh - b->off_t - b->off_b > hh)
      hh = phh - b->off_t - b->off_b;
    top = b->off_t;
  } else if (b->off_t >= 0) {
    top = b->off_t;
  } else if (b->off_b >= 0) {
    top = phh - b->off_b - hh;
  } else if (b->vcenter) {
    top = (phh - hh) / 2;
  }
  if (top < 0 && b->vcenter)
    top = 0;
  *h = hh;
  return top;
}

/* Paint one overlay's box backgrounds and borders, its border box at pane-relative `top`, `h` tall */
void paint_overlay_boxes(const struct lovl *ov, int py, int pw, int ct, int cb, int top, int h)
{
  const struct layout_out *o = g_fr;
  int n;
  const struct lbox *bx = layout_pool(&n);
  if (n != o->n_geo)
    return;
  int shift = py + top - o->geo[ov->box].y; /* overlay-table y -> screen y */
  for (int i = ov->box; i < bx[ov->box].end; i++) {
    struct lgeom g = o->geo[i];
    if (i == ov->box)
      g.h = h;
    paint_one_box(&bx[i], &g, shift, pw, ct, cb, 0);
  }
}

struct paint_gate g_gate = {0, -1, {{0}}};

static bool gate_in(int k, int v, bool box)
{
  return box ? v >= g_gate.r[k].box0 && v < g_gate.r[k].box1 : v >= g_gate.r[k].t0 && v < g_gate.r[k].t1;
}

/* False for what the current pass must not paint: transformed subtrees on the page pass, everything else on
 * a subtree's own pass. */
static bool gate_pass(int v, bool box)
{
  if (g_gate.only >= 0)
    return gate_in(g_gate.only, v, box);
  for (int k = 0; k < g_gate.n; k++)
    if (gate_in(k, v, box))
      return false;
  return true;
}

bool gate_box(int i) { return gate_pass(i, true); }
bool gate_text(int off) { return gate_pass(off, false); }

/* Paint every box that touches the band [ct,cb) */
void paint_boxes(int py, int pw, int ct, int cb)
{
  const struct layout_out *o = g_fr;
  int n;
  const struct lbox *bx = layout_pool(&n);
  if (!g_box_layout || o->lt.n == 0 || n != o->n_geo || !o->bands.idx)
    return;
  int top_doc = B->pg->scroll; /* document px at the pane top */
  int vtop = top_doc + (ct - py), vbot = top_doc + (cb - py);
  for (int band = 0; band < 3; band++) {
    int lo, hi;
    ru_bands_span(&o->bands, band, vtop, vbot, &lo, &hi);
    for (int p = lo; p < hi; p++) {
      int i = o->bands.idx[p];
      if ((o->n_ov && in_overlay(o, bx, i)) || !gate_box(i))
        continue;
      int top, bot;
      box_extent(&bx[i], &o->geo[i], &top, &bot);
      if (bot <= vtop || top >= vbot)
        continue;
      paint_one_box(&bx[i], &o->geo[i], py, pw, ct, cb, top_doc);
    }
  }
}

/* Outline a devtools-selected element's box over the page */
void draw_dt_highlight(int x, int y, int w, int h, int clip_top, int clip_bot, int clip_right, const char *label)
{
  unsigned col = 0x4c8df6;
  int x0 = x < 0 ? 0 : x, x1 = x + w > clip_right ? clip_right : x + w;
  int y0 = y < clip_top ? clip_top : y, y1 = y + h > clip_bot ? clip_bot : y + h;
  if (x1 <= x0 || y1 <= y0)
    return;

  if (y >= clip_top && y < clip_bot)
    gfx_fill_rect(x0, y, x1 - x0, 2, col); /* top    */
  if (y + h - 2 >= clip_top && y + h <= clip_bot)
    gfx_fill_rect(x0, y + h - 2, x1 - x0, 2, col); /* bottom */
  if (x >= 0 && x < clip_right)
    gfx_fill_rect(x, y0, 2, y1 - y0, col); /* left   */
  if (x + w - 2 >= 0 && x + w <= clip_right)
    gfx_fill_rect(x + w - 2, y0, 2, y1 - y0, col); /* right  */

  if (label && label[0]) {
    int lw = gfx_ui_width(label) + 8, lh = UIFONT_H + 4;
    int ly = y0 - lh;
    if (ly < clip_top)
      ly = y0; /* below the top edge if no room above */
    int lx = x0;
    if (lx + lw > clip_right)
      lx = clip_right - lw;
    if (lx < 0)
      lx = 0;
    gfx_fill_rect(lx, ly, lw, lh, 0x2563eb);
    gfx_ui_text(lx + 4, ly + 2, label, 0xffffff);
  }
}

/* Human-readable cause for the "tab crashed" page, from the fault signal. */
static const char *crash_reason(int sig)
{
  switch (sig) {
  case SIGSEGV:
  case SIGBUS:
    return "It ran out of memory or accessed invalid memory.";
  case SIGILL:
    return "It executed an illegal instruction.";
  case SIGFPE:
    return "It hit an arithmetic error.";
  case SIGABRT:
    return "It aborted unexpectedly.";
  default:
    return "Something went wrong while displaying this page.";
  }
}

/* Reload button on the crash page, remembered so handle_click can hit-test */
struct rect crash_reload_rect;

/* Draw the "Aw, snap"-style crashed-tab page over the content pane (py..py+phh), replacing */
void draw_crash_page(int py, int pw, int phh)
{
  gfx_fill_rect(0, py, pw, phh, 0xf2f3f5); /* neutral canvas, not the dead page's */

  const char *reason = crash_reason(tabs[active_tab]->crash_sig);
  const char *head = "This tab has crashed";
  int cx = pw / 2;
  int cy = py + phh / 2;

  int face_px = 64;
  int fw = gfx_ui_width_px(":(", face_px);
  gfx_ui_text_px(cx - fw / 2, cy - 96, ":(", 0xb0b6be, face_px);

  int hw = gfx_ui_width(head);
  gfx_ui_text(cx - hw / 2, cy - 12, head, 0x303840);

  int rw = gfx_ui_width(reason);
  gfx_ui_text(cx - rw / 2, cy + 12, reason, 0x707880);

  int bw = 96, bh = 30;
  crash_reload_rect.x = cx - bw / 2;
  crash_reload_rect.y = cy + 44;
  crash_reload_rect.w = bw;
  crash_reload_rect.h = bh;
  button_draw(crash_reload_rect.x, crash_reload_rect.y, bw, bh, "Reload", BTN_NORMAL);
}
