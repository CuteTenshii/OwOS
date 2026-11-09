/* svg_tex.c: textured paint: <pattern> tiles and <image>. */
#include "base64.h"
#include "features/svg/svg_int.h"
#include "imgdec.h"

#define MAX_TILE 2048

/* Render a pattern's content into a tile bitmap and describe it as a repeating texture source. */
bool svg_pattern_src(struct ctx *cx, const struct dom_node *pat, const struct bb *bb, struct mat m, float opacity,
                     int color, struct src *s)
{
  if (cx->sub_depth >= 3)
    return false;
  const char *u = svg_chain_attr(cx, pat, "patternunits", false);
  const char *cu = svg_chain_attr(cx, pat, "patterncontentunits", false);
  bool obb = !(u && starts_ci(u, "userSpaceOnUse"));
  bool content_obb = cu && starts_ci(cu, "objectBoundingBox");
  if ((obb || content_obb) && (!bb->ok || bb->x1 <= bb->x0 || bb->y1 <= bb->y0))
    return false;
  float bw = bb->x1 - bb->x0, bh = bb->y1 - bb->y0;
  float px = svg_grad_coord(svg_chain_attr(cx, pat, "x", true), 0, obb, cx->vw);
  float py = svg_grad_coord(svg_chain_attr(cx, pat, "y", true), 0, obb, cx->vh);
  float pw = svg_grad_coord(svg_chain_attr(cx, pat, "width", true), 0, obb, cx->vw);
  float ph = svg_grad_coord(svg_chain_attr(cx, pat, "height", true), 0, obb, cx->vh);
  if (obb) {
    px = bb->x0 + px * bw;
    py = bb->y0 + py * bh;
    pw *= bw;
    ph *= bh;
  }
  if (!(pw > 0 && ph > 0))
    return false;

  struct mat pm = svg_mat_mul(m, svg_parse_transform(svg_chain_attr(cx, pat, "patterntransform", false)));
  pm = svg_mat_mul(pm, svg_mat_translate(px, py));
  struct mat pinv;
  if (!svg_mat_inv(pm, &pinv))
    return false;
  float ts = svg_mat_scale(pm);
  int tw = (int)(pw * ts + 0.999f), th = (int)(ph * ts + 0.999f);
  tw = tw < 1 ? 1 : tw > MAX_TILE ? MAX_TILE : tw;
  th = th < 1 ? 1 : th > MAX_TILE ? MAX_TILE : th;
  float sx = (float)tw / pw, sy = (float)th / ph;

  const struct dom_node *content = pat;
  for (int hop = 0; content && hop < 8; hop++) {
    bool any = false;
    for (const struct dom_node *c = content->first_child; c && !any; c = c->next)
      any = c->type == DOM_ELEMENT;
    if (any)
      break;
    content = svg_href_target(cx, content);
  }
  unsigned *tile = calloc((unsigned)tw * (unsigned)th, sizeof *tile);
  if (!tile)
    return false;

  if (content) {
    struct mat cm = {sx, 0, 0, sy, 0, 0};
    float vb[4];
    struct ctx sub = *cx;
    if (svg_parse_viewbox(svg_chain_attr(cx, pat, "viewbox", false), vb)) {
      cm = svg_mat_mul(
          cm, svg_viewbox_mat(vb, svg_parse_par(svg_chain_attr(cx, pat, "preserveaspectratio", false)), 0, 0, pw, ph));
      sub.vw = vb[2];
      sub.vh = vb[3];
    } else if (content_obb) {
      cm = svg_mat_mul(cm, (struct mat){bw, 0, 0, bh, 0, 0});
      sub.vw = sub.vh = 1;
    }
    sub.w = tw;
    sub.h = th;
    sub.buf = tile;
    sub.clip = 0;
    sub.nlayers = 0;
    sub.xs = 0;
    sub.wd = 0;
    sub.xcap = 0;
    memset(sub.tp, 0, sizeof sub.tp);
    memset(sub.aux, 0, sizeof sub.aux);
    sub.sub_depth++;
    sub.acc = malloc((unsigned)(tw + 2) * sizeof(float));
    if (sub.acc) {
      struct style bs;
      svg_style_base(&bs, color);
      svg_resolve_style(&sub, pat, &bs);
      svg_render_children(&sub, content, cm, &bs, 1, false);
    }
    cx->budget = sub.budget;
    free(sub.acc);
    free(sub.xs);
    free(sub.wd);
    for (int i = 0; i < 3; i++)
      svg_path_free(&sub.tp[i]);
    svg_path_free(&sub.aux[0]);
    svg_path_free(&sub.aux[1]);
  }

  s->grad = false;
  s->tex = true;
  s->repeat = true;
  s->pix = tile;
  s->tw = tw;
  s->th = th;
  s->alpha = (int)(opacity * 255 + 0.5f);
  s->inv = svg_mat_mul((struct mat){sx, 0, 0, sy, 0, 0}, pinv);
  return s->alpha > 0;
}

static int hexval(char c)
{
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

/* Payload of a data: URI, decoded into a fresh buffer. */
static unsigned char *data_uri(const char *href, int *len)
{
  if (!starts_ci(href, "data:"))
    return 0;
  const char *comma = strchr(href, ',');
  if (!comma)
    return 0;
  bool b64 = false;
  for (const char *q = href; q < comma; q++)
    if (starts_ci(q, ";base64"))
      b64 = true;
  const char *pay = comma + 1;
  int n = (int)strlen(pay);
  unsigned char *out = malloc((unsigned)(b64 ? base64_decoded_cap(n) : n) + 1);
  if (!out)
    return 0;
  if (b64) {
    *len = base64_decode(pay, n, out);
    if (*len < 0) {
      free(out);
      return 0;
    }
    return out;
  }
  int k = 0;
  for (int i = 0; i < n; i++) {
    if (pay[i] == '%' && i + 2 < n + 0 && hexval(pay[i + 1]) >= 0 && hexval(pay[i + 2]) >= 0) {
      out[k++] = (unsigned char)(hexval(pay[i + 1]) * 16 + hexval(pay[i + 2]));
      i += 2;
    } else {
      out[k++] = (unsigned char)pay[i];
    }
  }
  *len = k;
  return out;
}

/* <image> with an inline (data:) source. */
void svg_draw_image(struct ctx *cx, const struct dom_node *n, struct mat m, const struct style *st)
{
  const char *href = dom_get_attr(n, "href");
  if (!href)
    href = dom_get_attr(n, "xlink:href");
  if (!href || st->hidden || cx->sub_depth >= 3)
    return;
  int len = 0;
  unsigned char *data = data_uri(href, &len);
  if (!data)
    return;
  unsigned iw = 0, ih = 0;
  unsigned char *rgba = len > 0 ? image_decode_rgba(data, len, (unsigned)st->color, 2048, &iw, &ih) : 0;
  free(data);
  if (!rgba)
    return;
  unsigned *pix = malloc(iw * ih * sizeof *pix);
  if (!pix) {
    free(rgba);
    return;
  }
  for (unsigned i = 0; i < iw * ih; i++) {
    unsigned r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
    pix[i] = (a << 24) | (((r * a + 127) / 255) << 16) | (((g * a + 127) / 255) << 8) | ((b * a + 127) / 255);
  }
  free(rgba);

  float x = svg_attr_len(n, "x", 0, cx->vw), y = svg_attr_len(n, "y", 0, cx->vh);
  float w = svg_attr_len(n, "width", (float)iw, cx->vw), h = svg_attr_len(n, "height", (float)ih, cx->vh);
  if (w > 0 && h > 0) {
    struct par par = svg_parse_par(dom_get_attr(n, "preserveaspectratio"));
    float vb[4] = {0, 0, (float)iw, (float)ih};
    struct mat t = svg_viewbox_mat(vb, par, x, y, w, h), full = svg_mat_mul(m, t), inv;
    if (svg_mat_inv(full, &inv)) {
      struct src s;
      memset(&s, 0, sizeof s);
      s.tex = true;
      s.pix = pix;
      s.tw = (int)iw;
      s.th = (int)ih;
      s.inv = inv;
      s.alpha = (int)(st->fill_op * 255 + 0.5f);
      struct path *up = &cx->tp[0];
      svg_path_reset(up);
      float rx0 = 0, ry0 = 0, rx1 = (float)iw, ry1 = (float)ih;
      struct mat u2 = t;
      if (par.slice || par.none) { /* fill the whole viewport box */
        u2 = svg_mat_id();
        rx0 = x;
        ry0 = y;
        rx1 = x + w;
        ry1 = y + h;
      }
      float qx[4] = {rx0, rx1, rx1, rx0}, qy[4] = {ry0, ry0, ry1, ry1};
      for (int i = 0; i < 4; i++) {
        float ux, uy;
        svg_mat_apply(u2, qx[i], qy[i], &ux, &uy);
        if (i == 0)
          svg_path_move(up, ux, uy);
        else
          svg_path_line(up, ux, uy);
      }
      svg_path_xform(&cx->tp[1], up, m);
      svg_raster_fill(cx, &cx->tp[1], false, &s, 0);
    }
  }
  free(pix);
}
