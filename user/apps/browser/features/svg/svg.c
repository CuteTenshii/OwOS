/* svg.c: element rendering and the public entry points. */
#include "features/svg/svg.h"
#include "features/svg/svg_int.h"

/* drawing */
static void draw_shape(struct ctx *cx, const struct dom_node *n, struct mat m, const struct style *st)
{
  if (st->hidden)
    return;
  struct path *up = &cx->tp[0], *dp = &cx->tp[1];
  float scale = svg_mat_scale(m);
  svg_path_reset(up);
  if (!svg_build_shape(cx, n, up, scale))
    return;
  struct bb bb = svg_path_bbox(up, svg_mat_id());
  struct src s;
  if (st->fill.kind != PAINT_NONE && svg_make_src(cx, &st->fill, st->fill_op, st->color, &bb, m, &s)) {
    svg_path_xform(dp, up, m);
    svg_raster_fill(cx, dp, st->evenodd, &s, 0);
    svg_src_free(&s);
  }
  if (st->stroke.kind != PAINT_NONE && st->sw > 0 &&
      svg_make_src(cx, &st->stroke, st->stroke_op, st->color, &bb, m, &s)) {
    svg_stroke_build(cx, &cx->tp[2], up, st, scale);
    svg_path_xform(dp, &cx->tp[2], m);
    svg_raster_fill(cx, dp, false, &s, 0);
    svg_src_free(&s);
  }
  if (up->nv && (st->marker[0] || st->marker[1] || st->marker[2]) && !streq(n->tag, "rect") &&
      !streq(n->tag, "circle") && !streq(n->tag, "ellipse"))
    svg_draw_markers(cx, up, m, st);
}

static void render_element(struct ctx *cx, const struct dom_node *n, struct mat ctm, const struct style *in, int depth);

void svg_render_children(struct ctx *cx, const struct dom_node *n, struct mat m, const struct style *st, int depth,
                         bool first_only)
{
  for (const struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    if (first_only && svg_elem_kind(c->tag) == K_NONE)
      continue;
    render_element(cx, c, m, st, depth + 1);
    if (first_only)
      break;
  }
}

/* Nested <svg> or <symbol>: establishes a viewport, a viewBox mapping and an overflow clip. */
static void render_viewport(struct ctx *cx, const struct dom_node *n, struct mat m, const struct style *st, float x,
                            float y, float w, float h, int depth)
{
  if (w <= 0 || h <= 0)
    return;
  float vb[4];
  struct mat mm = m;
  float vw = cx->vw, vh = cx->vh;
  if (svg_parse_viewbox(dom_get_attr(n, "viewbox"), vb)) {
    mm = svg_mat_mul(m, svg_viewbox_mat(vb, svg_parse_par(dom_get_attr(n, "preserveaspectratio")), x, y, w, h));
    cx->vw = vb[2];
    cx->vh = vb[3];
  } else {
    mm = svg_mat_mul(m, svg_mat_translate(x, y));
    cx->vw = w;
    cx->vh = h;
  }
  unsigned char *saved = cx->clip, *own = 0;
  const char *ov = dom_get_attr(n, "overflow");
  if (!(ov && (starts_ci(ov, "visible") || starts_ci(ov, "auto")))) {
    own = svg_rect_clip(cx, m, x, y, w, h);
    if (own)
      cx->clip = own;
  }
  svg_render_children(cx, n, mm, st, depth, false);
  cx->clip = saved;
  free(own);
  cx->vw = vw;
  cx->vh = vh;
}

static void render_use(struct ctx *cx, const struct dom_node *n, struct mat m, const struct style *st, int depth)
{
  const struct dom_node *t = svg_href_target(cx, n);
  if (!t || cx->use_depth >= 12)
    return;
  float x = svg_attr_len(n, "x", 0, cx->vw), y = svg_attr_len(n, "y", 0, cx->vh);
  struct mat m2 = svg_mat_mul(m, svg_mat_translate(x, y));
  cx->use_depth++;
  if (streq(t->tag, "symbol") || streq(t->tag, "svg")) {
    float w = svg_attr_len(n, "width", svg_attr_len(t, "width", cx->vw, cx->vw), cx->vw);
    float h = svg_attr_len(n, "height", svg_attr_len(t, "height", cx->vh, cx->vh), cx->vh);
    struct style ts = *st;
    svg_resolve_style(cx, t, &ts);
    render_viewport(cx, t, m2, &ts, 0, 0, w, h, depth + 1);
  } else {
    render_element(cx, t, m2, st, depth + 1);
  }
  cx->use_depth--;
}

static void render_element(struct ctx *cx, const struct dom_node *n, struct mat ctm, const struct style *in, int depth)
{
  if (depth > MAX_DEPTH || --cx->budget < 0)
    return;
  int k = svg_elem_kind(n->tag);
  if (k == K_NONE || svg_display_none(cx, n))
    return;
  char tf[256];
  struct mat m = svg_mat_mul(ctm, svg_parse_transform(svg_get_prop(cx, n, "transform", tf, sizeof tf) ? tf : 0));
  struct style st = *in;
  svg_resolve_style(cx, n, &st);
  float op = svg_elem_opacity(cx, n);

  unsigned char *saved_clip = cx->clip, *own_clip = 0;
  char cpv[128];
  if (svg_get_prop(cx, n, "clip-path", cpv, sizeof cpv) && starts_ci(cpv, "url(")) {
    const struct dom_node *cp = svg_ref_target(cx, cpv);
    if (cp && streq(cp->tag, "clippath")) {
      own_clip = svg_make_clip(cx, cp, m, n, 0);
      if (own_clip)
        cx->clip = own_clip;
    }
  }

  char mv[128];
  if (svg_get_prop(cx, n, "mask", mv, sizeof mv) && starts_ci(mv, "url(")) {
    const struct dom_node *mk = svg_ref_target(cx, mv);
    if (mk && streq(mk->tag, "mask")) {
      unsigned char *mm = svg_make_mask(cx, mk, m, n, 0);
      if (mm) {
        free(own_clip);
        own_clip = mm;
        cx->clip = mm;
      }
    }
  }

  const struct dom_node *filter = 0;
  if (svg_get_prop(cx, n, "filter", mv, sizeof mv) && starts_ci(mv, "url(")) {
    filter = svg_ref_target(cx, mv);
    if (filter && !streq(filter->tag, "filter"))
      filter = 0;
  }

  unsigned *layer = 0, *prev = cx->buf;
  if (op < 0.999f || filter) {
    bool both = st.fill.kind != PAINT_NONE && st.stroke.kind != PAINT_NONE && st.sw > 0;
    if ((k != K_SHAPE || both || filter) && cx->nlayers < MAX_LAYERS) {
      layer = calloc((unsigned)cx->w * (unsigned)cx->h, sizeof *layer);
      if (layer) {
        cx->layers[cx->nlayers++] = layer;
        cx->buf = layer;
      }
    }
    if (!layer) {
      st.fill_op *= op;
      st.stroke_op *= op;
    }
  }

  if (k == K_SHAPE) {
    draw_shape(cx, n, m, &st);
  } else if (k == K_TEXT) {
    svg_draw_text(cx, n, m, &st);
  } else if (k == K_IMAGE) {
    svg_draw_image(cx, n, m, &st);
  } else if (k == K_USE) {
    render_use(cx, n, m, &st, depth);
  } else if (k == K_SVG && depth > 0) {
    float x = svg_attr_len(n, "x", 0, cx->vw), y = svg_attr_len(n, "y", 0, cx->vh);
    render_viewport(cx, n, m, &st, x, y, svg_attr_len(n, "width", cx->vw, cx->vw),
                    svg_attr_len(n, "height", cx->vh, cx->vh), depth);
  } else {
    svg_render_children(cx, n, m, &st, depth, k == K_SWITCH);
  }

  if (layer) {
    cx->buf = prev;
    cx->nlayers--;
    if (filter)
      svg_apply_filter(cx, filter, layer, m, n);
    svg_composite_layer(cx, prev, layer, op);
    free(layer);
  }
  cx->clip = saved_clip;
  free(own_clip);
}

/* entry */
static const struct dom_node *find_svg(const struct dom_node *n)
{
  for (const struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    if (streq(c->tag, "svg"))
      return c;
    const struct dom_node *r = find_svg(c);
    if (r)
      return r;
  }
  return 0;
}

int svg_probe(const char *data, int len)
{
  const char *p = data, *e = data + (len < 2048 ? len : 2048);
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\xef' || *p == '\xbb' || *p == '\xbf'))
    p++; /* skip ws + UTF-8 BOM */
  for (; p < e; p++) {
    if (p + 4 < e && p[0] == '<' && starts_ci(p + 1, "svg"))
      return 1;
    if (p + 5 < e && p[0] == '<' && p[1] == '?' && starts_ci(p + 2, "xml"))
      continue;
    if (p[0] == '<' && p[1] != '?' && p[1] != '!')
      break; /* a non-svg root tag */
  }
  return 0;
}

/* width/height as absolute lengths; 0 when missing or relative. */
static void root_size(const struct dom_node *svg, float *w, float *h)
{
  *w = svg_attr_len(svg, "width", 0, 0);
  *h = svg_attr_len(svg, "height", 0, 0);
  if (*w < 0)
    *w = 0;
  if (*h < 0)
    *h = 0;
}

int svg_size(const char *data, int len, int *w, int *h)
{
  struct dom_node *root = dom_parse(data, len);
  if (!root)
    return 0;
  const struct dom_node *svg = find_svg(root);
  bool ok = false;
  if (svg) {
    float vb[4], sw, sh;
    bool has_vb = svg_parse_viewbox(dom_get_attr(svg, "viewbox"), vb);
    root_size(svg, &sw, &sh);
    if (has_vb) {
      if (sw <= 0 && sh <= 0) {
        sw = vb[2];
        sh = vb[3];
      } else if (sw <= 0) {
        sw = sh * vb[2] / vb[3];
      } else if (sh <= 0) {
        sh = sw * vb[3] / vb[2];
      }
    }
    if (sw > 0 && sh > 0) {
      *w = (int)(sw + 0.5f);
      *h = (int)(sh + 0.5f);
      if (*w < 1)
        *w = 1;
      if (*h < 1)
        *h = 1;
      ok = true;
    }
  }
  dom_free(root);
  return ok;
}

int svg_render(const char *data, int len, int w, int h, unsigned *out, unsigned current_color)
{
  if (w <= 0 || h <= 0 || !out || (long)w * h > MAX_PIXELS)
    return 0;
  struct dom_node *root = dom_parse(data, len);
  if (!root)
    return 0;
  const struct dom_node *svg = find_svg(root);
  if (!svg) {
    dom_free(root);
    return 0;
  }

  struct ctx cx;
  memset(&cx, 0, sizeof cx);
  cx.root = svg;
  cx.w = w;
  cx.h = h;
  cx.budget = ELEM_BUDGET;
  cx.buf = calloc((unsigned)w * (unsigned)h, sizeof(unsigned));
  cx.acc = malloc((unsigned)(w + 2) * sizeof(float));
  if (!cx.buf || !cx.acc) {
    free(cx.buf);
    free(cx.acc);
    dom_free(root);
    return 0;
  }
  svg_collect(&cx, root, 0);

  /* viewBox, else width/height, else the output size, fitted into the output with preserveAspectRatio */
  float vb[4], sw, sh;
  if (!svg_parse_viewbox(dom_get_attr(svg, "viewbox"), vb)) {
    root_size(svg, &sw, &sh);
    vb[0] = vb[1] = 0;
    vb[2] = sw > 0 && sh > 0 ? sw : (float)w;
    vb[3] = sw > 0 && sh > 0 ? sh : (float)h;
  }
  cx.vw = vb[2];
  cx.vh = vb[3];
  struct mat ctm =
      svg_viewbox_mat(vb, svg_parse_par(dom_get_attr(svg, "preserveaspectratio")), 0, 0, (float)w, (float)h);

  struct style base;
  svg_style_base(&base, (int)(current_color & 0xFFFFFF));
  render_element(&cx, svg, ctm, &base, 0);

  for (int i = 0; i < w * h; i++) { /* premultiplied -> straight ARGB */
    unsigned p = cx.buf[i], a = p >> 24;
    if (a == 0 || a == 255) {
      out[i] = a ? p : 0;
      continue;
    }
    unsigned r = (((p >> 16) & 255) * 255 + a / 2) / a, g = (((p >> 8) & 255) * 255 + a / 2) / a,
             b = ((p & 255) * 255 + a / 2) / a;
    out[i] = (a << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
  }

  free(cx.buf);
  free(cx.acc);
  free(cx.xs);
  free(cx.wd);
  free(cx.rules);
  free(cx.ids);
  for (int i = 0; i < 3; i++)
    svg_path_free(&cx.tp[i]);
  svg_path_free(&cx.aux[0]);
  svg_path_free(&cx.aux[1]);
  dom_free(root);
  return 1;
}
