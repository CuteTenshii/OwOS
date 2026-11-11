/* pdfimage.c, PDF images: raw samples and JPEG into RGBA, soft and stencil masks, and the draw */
#include "jpeg.h"
#include "math.h"
#include "owos.h"
#include "pdfint.h"

static struct pobj ik(struct pdf *d, struct pobj dict, const char *a, const char *b)
{
  struct pobj o = pdf_get(d, dict, a);
  return o.t == PO_NULL ? pdf_get(d, dict, b) : o;
}

static unsigned get_bits(const unsigned char *row, long idx, int bpc)
{
  long bit = idx * bpc;
  switch (bpc) {
  case 8:
    return row[idx];
  case 16:
    return (unsigned)row[idx * 2] << 8 | row[idx * 2 + 1];
  default:
    return (row[bit >> 3] >> (8 - bpc - (int)(bit & 7))) & ((1u << bpc) - 1);
  }
}

struct sink {
  unsigned char *out;
  unsigned *acc;
  int ow, sx, sy;
};

static void put_px(struct sink *s, int x, int y, unsigned r, unsigned g, unsigned b, unsigned a)
{
  long i = (long)(y / s->sy) * s->ow + x / s->sx;
  if (!s->acc) {
    s->out[i * 4] = (unsigned char)r;
    s->out[i * 4 + 1] = (unsigned char)g;
    s->out[i * 4 + 2] = (unsigned char)b;
    s->out[i * 4 + 3] = (unsigned char)a;
  } else {
    s->acc[i * 4] += r * a;
    s->acc[i * 4 + 1] += g * a;
    s->acc[i * 4 + 2] += b * a;
    s->acc[i * 4 + 3] += a;
  }
}

static void finish(struct sink *s, int w, int h, int ow, int oh)
{
  if (!s->acc)
    return;
  for (int y = 0; y < oh; y++)
    for (int x = 0; x < ow; x++) {
      long i = (long)y * ow + x;
      int cx = w - x * s->sx < s->sx ? w - x * s->sx : s->sx, cy = h - y * s->sy < s->sy ? h - y * s->sy : s->sy;
      unsigned a = s->acc[i * 4 + 3];
      if (a) {
        s->out[i * 4] = (unsigned char)(s->acc[i * 4] / a);
        s->out[i * 4 + 1] = (unsigned char)(s->acc[i * 4 + 1] / a);
        s->out[i * 4 + 2] = (unsigned char)(s->acc[i * 4 + 2] / a);
      }
      s->out[i * 4 + 3] = (unsigned char)(a / (unsigned)(cx * cy));
    }
}

static unsigned char *image_rgba(struct rctx *rc, struct pobj dict, const unsigned char *data, int len,
                                 const char *left, double devw, double devh, int *pw, int *ph)
{
  struct pdf *d = rc->d;
  int w = (int)pdf_num(ik(d, dict, "Width", "W"), 0), h = (int)pdf_num(ik(d, dict, "Height", "H"), 0);
  struct pobj imo = ik(d, dict, "ImageMask", "IM");
  int mask = imo.t == PO_BOOL && imo.n;
  int bpc = mask ? 1 : (int)pdf_num(ik(d, dict, "BitsPerComponent", "BPC"), 8);
  struct pobj dec = ik(d, dict, "Decode", "D"), key = pdf_get(d, dict, "Mask");
  if (w <= 0 || h <= 0 || w > 65535 || h > 65535 || (long)w * h > (1L << 27))
    return 0;
  int tw = (int)(devw * 2) + 1, th = (int)(devh * 2) + 1;
  int sx = w / tw < 1 ? 1 : w / tw, sy = h / th < 1 ? 1 : h / th;
  while ((long)((w + sx - 1) / sx) * ((h + sy - 1) / sy) > (1L << 24)) {
    sx++;
    sy++;
  }
  unsigned char *out;
  if (left) {
    if (strcmp(left, "DCTDecode") && strcmp(left, "DCT"))
      return 0;
    unsigned jw, jh;
    int maxd = (int)(devw > devh ? devw : devh) * 2 + 1;
    if (jpeg_decode(data, len, maxd, &out, &jw, &jh) != JPEG_OK || !out)
      return 0;
    if (dec.t == PO_ARR && dec.n >= 2 && pdf_num(pdf_at(d, dec, 0), 0) > pdf_num(pdf_at(d, dec, 1), 1))
      for (unsigned long i = 0; i < (unsigned long)jw * jh; i++)
        for (int k = 0; k < 3; k++)
          out[i * 4 + k] = (unsigned char)(255 - out[i * 4 + k]);
    *pw = (int)jw;
    *ph = (int)jh;
    return out;
  }
  struct cspace *cs = mask ? 0 : pdf_cs_parse(rc, ik(d, dict, "ColorSpace", "CS"), 0);
  if (!mask && (cs->kind == CS_PATTERN || cs->n < 1))
    return 0;
  if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16)
    return 0;
  int nc = mask ? 1 : cs->n;
  long rowbytes = ((long)w * nc * bpc + 7) / 8;
  int ow = (w + sx - 1) / sx, oh = (h + sy - 1) / sy;
  out = calloc((unsigned long)ow * oh, 4);
  struct sink s = {out, 0, ow, sx, sy};
  if (!out)
    return 0;
  if (sx > 1 || sy > 1) {
    s.acc = calloc((unsigned long)ow * oh, 16);
    if (!s.acc) {
      free(out);
      return 0;
    }
  }
  double maxv = bpc == 16 ? 65535 : (double)((1 << bpc) - 1);
  double dmin[8], dmax[8];
  int has_dec = dec.t == PO_ARR && dec.n >= nc * 2;
  for (int k = 0; k < nc; k++) {
    dmin[k] = has_dec ? pdf_num(pdf_at(d, dec, k * 2), 0) : 0;
    dmax[k] = has_dec ? pdf_num(pdf_at(d, dec, k * 2 + 1), 1) : (!mask && cs->kind == CS_INDEXED ? maxv : 1);
  }
  const unsigned char *fill = rc->gs.fill.rgb;
  unsigned char lut[256][3];
  int use_lut = !mask && nc == 1 && bpc <= 8 && key.t != PO_ARR;
  if (use_lut)
    for (unsigned v = 0; v <= (unsigned)maxv; v++) {
      double c = dmin[0] + v * (dmax[0] - dmin[0]) / maxv;
      pdf_cs_rgb(cs, &c, lut[v]);
    }
  int fast_rgb = !mask && cs->kind == CS_RGB && bpc == 8 && !has_dec && key.t != PO_ARR;
  for (int y = 0; y < h; y++) {
    if ((long)(y + 1) * rowbytes > len)
      break;
    const unsigned char *row = data + (long)y * rowbytes;
    for (int x = 0; x < w; x++) {
      if (mask) {
        unsigned v = get_bits(row, x, 1);
        int paint = has_dec && dmin[0] > dmax[0] ? v == 1 : v == 0;
        put_px(&s, x, y, fill[0], fill[1], fill[2], paint ? 255 : 0);
      } else if (fast_rgb)
        put_px(&s, x, y, row[x * 3], row[x * 3 + 1], row[x * 3 + 2], 255);
      else if (use_lut) {
        unsigned v = get_bits(row, x, bpc);
        put_px(&s, x, y, lut[v][0], lut[v][1], lut[v][2], 255);
      } else {
        double comp[8];
        unsigned a = 255, hit = key.t == PO_ARR;
        for (int k = 0; k < nc && k < 8; k++) {
          unsigned v = get_bits(row, (long)x * nc + k, bpc);
          comp[k] = dmin[k] + v * (dmax[k] - dmin[k]) / maxv;
          if (hit && (key.n < (k + 1) * 2 || v < pdf_num(pdf_at(d, key, k * 2), 0) ||
                      v > pdf_num(pdf_at(d, key, k * 2 + 1), 0)))
            hit = 0;
        }
        if (hit)
          a = 0;
        unsigned char rgb[3];
        pdf_cs_rgb(cs, comp, rgb);
        put_px(&s, x, y, rgb[0], rgb[1], rgb[2], a);
      }
    }
  }
  finish(&s, w, h, ow, oh);
  free(s.acc);
  *pw = ow;
  *ph = oh;
  return out;
}

/* Alpha for an ow x oh image from a soft mask (luminance) or a stencil mask (painted = opaque). */
static void apply_mask(struct rctx *rc, struct pobj mk, int soft, unsigned char *rgba, int ow, int oh, double devw,
                       double devh)
{
  int len = 0, mw = 0, mh = 0;
  const char *left = 0;
  unsigned char *data = pdf_stream_data(rc->d, mk, &len, &left, 0);
  unsigned char *m = data ? image_rgba(rc, mk, data, len, left, devw, devh, &mw, &mh) : 0;
  free(data);
  if (!m)
    return;
  for (int y = 0; y < oh; y++)
    for (int x = 0; x < ow; x++) {
      const unsigned char *p = m + ((long)(y * mh / oh) * mw + x * mw / ow) * 4;
      unsigned a = soft ? p[0] : p[3];
      unsigned char *q = rgba + ((long)y * ow + x) * 4;
      q[3] = (unsigned char)(q[3] * a / 255);
    }
  free(m);
}

void pdf_draw_image(struct rctx *rc, struct pobj dict, unsigned char *data, int len, const char *left, struct pobj lp)
{
  (void)lp;
  struct pdf *d = rc->d;
  struct gstate *g = &rc->gs;
  double devw = hypot(g->ctm.a, g->ctm.b), devh = hypot(g->ctm.c, g->ctm.d);
  if (devw < 0.01 || devh < 0.01 || devw > 1e5 || devh > 1e5)
    return;
  int ow = 0, oh = 0;
  unsigned char *rgba = image_rgba(rc, dict, data, len, left, devw, devh, &ow, &oh);
  if (!rgba)
    return;
  struct pobj sm = pdf_get(d, dict, "SMask"), mk = pdf_get(d, dict, "Mask");
  if (sm.t == PO_STREAM)
    apply_mask(rc, sm, 1, rgba, ow, oh, devw, devh);
  else if (mk.t == PO_STREAM)
    apply_mask(rc, mk, 0, rgba, ow, oh, devw, devh);
  struct c2d_state *st = &rc->c->st[rc->c->depth];
  struct c2d_mat unit = {1.0f / ow, 0, 0, -1.0f / oh, 0, 1};
  st->m = c2d_mat_mul(g->ctm, unit);
  st->alpha = (float)g->fa;
  c2d_draw_image(rc->c, rgba, ow, oh, 0, 0, (float)ow, (float)oh, 0, 0, (float)ow, (float)oh);
  st->m = C2D_IDENTITY;
  st->alpha = 1;
  free(rgba);
}
