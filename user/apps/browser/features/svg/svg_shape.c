/* svg_shape.c: shape geometry, bounding boxes and clip masks. */
#include "features/svg/svg_int.h"

int svg_elem_kind(const char *t)
{
  if (streq(t, "g") || streq(t, "a"))
    return K_GROUP;
  if (streq(t, "svg"))
    return K_SVG;
  if (streq(t, "switch"))
    return K_SWITCH;
  if (streq(t, "use"))
    return K_USE;
  if (streq(t, "text"))
    return K_TEXT;
  if (streq(t, "image"))
    return K_IMAGE;
  if (streq(t, "rect") || streq(t, "circle") || streq(t, "ellipse") || streq(t, "line") || streq(t, "polyline") ||
      streq(t, "polygon") || streq(t, "path"))
    return K_SHAPE;
  return K_NONE;
}

static void add_ellipse(struct path *p, float scale, float cx, float cy, float rx, float ry)
{
  svg_path_move(p, cx + rx, cy);
  svg_path_arc(p, scale, cx + rx, cy, rx, ry, 0, false, true, cx, cy + ry);
  svg_path_arc(p, scale, cx, cy + ry, rx, ry, 0, false, true, cx - rx, cy);
  svg_path_arc(p, scale, cx - rx, cy, rx, ry, 0, false, true, cx, cy - ry);
  svg_path_arc(p, scale, cx, cy - ry, rx, ry, 0, false, true, cx + rx, cy);
  svg_path_close(p);
}

/* Shape element to a user-space path; false when it has nothing to draw. */
bool svg_build_shape(const struct ctx *cx, const struct dom_node *n, struct path *p, float scale)
{
  const char *t = n->tag;
  float vw = cx->vw, vh = cx->vh, vd = (float)sqrt((double)((vw * vw + vh * vh) * 0.5f));
  if (streq(t, "path")) {
    const char *d = dom_get_attr(n, "d");
    if (d)
      svg_parse_path(p, scale, d);
  } else if (streq(t, "rect")) {
    float x = svg_attr_len(n, "x", 0, vw), y = svg_attr_len(n, "y", 0, vh);
    float w = svg_attr_len(n, "width", 0, vw), h = svg_attr_len(n, "height", 0, vh);
    if (w <= 0 || h <= 0)
      return false;
    float rx = svg_attr_len(n, "rx", -1, vw), ry = svg_attr_len(n, "ry", -1, vh);
    if (rx < 0)
      rx = ry;
    if (ry < 0)
      ry = rx;
    rx = rx < 0 ? 0 : rx > w / 2 ? w / 2 : rx;
    ry = ry < 0 ? 0 : ry > h / 2 ? h / 2 : ry;
    if (rx <= 0 || ry <= 0) {
      svg_path_move(p, x, y);
      svg_path_line(p, x + w, y);
      svg_path_line(p, x + w, y + h);
      svg_path_line(p, x, y + h);
    } else {
      svg_path_move(p, x + rx, y);
      svg_path_line(p, x + w - rx, y);
      svg_path_arc(p, scale, x + w - rx, y, rx, ry, 0, false, true, x + w, y + ry);
      svg_path_line(p, x + w, y + h - ry);
      svg_path_arc(p, scale, x + w, y + h - ry, rx, ry, 0, false, true, x + w - rx, y + h);
      svg_path_line(p, x + rx, y + h);
      svg_path_arc(p, scale, x + rx, y + h, rx, ry, 0, false, true, x, y + h - ry);
      svg_path_line(p, x, y + ry);
      svg_path_arc(p, scale, x, y + ry, rx, ry, 0, false, true, x + rx, y);
    }
    svg_path_close(p);
  } else if (streq(t, "circle")) {
    float r = svg_attr_len(n, "r", 0, vd);
    if (r <= 0)
      return false;
    add_ellipse(p, scale, svg_attr_len(n, "cx", 0, vw), svg_attr_len(n, "cy", 0, vh), r, r);
  } else if (streq(t, "ellipse")) {
    float rx = svg_attr_len(n, "rx", 0, vw), ry = svg_attr_len(n, "ry", 0, vh);
    if (rx <= 0 || ry <= 0)
      return false;
    add_ellipse(p, scale, svg_attr_len(n, "cx", 0, vw), svg_attr_len(n, "cy", 0, vh), rx, ry);
  } else if (streq(t, "line")) {
    svg_path_move(p, svg_attr_len(n, "x1", 0, vw), svg_attr_len(n, "y1", 0, vh));
    svg_path_mark(p);
    svg_path_line(p, svg_attr_len(n, "x2", 0, vw), svg_attr_len(n, "y2", 0, vh));
    svg_path_mark(p);
  } else if (streq(t, "polyline") || streq(t, "polygon")) {
    const char *pts = dom_get_attr(n, "points");
    if (!pts)
      return false;
    const char *q = pts, *e = pts + strlen(pts);
    float x, y;
    while (svg_parse_num(&q, e, &x) && svg_parse_num(&q, e, &y)) {
      if (p->ns)
        svg_path_line(p, x, y);
      else
        svg_path_move(p, x, y);
      svg_path_mark(p);
    }
    if (streq(t, "polygon"))
      svg_path_close(p);
  }
  return p->n > 0;
}

/* Bounding box of an element's geometry in its own user space (for objectBoundingBox units). */
void svg_node_bbox(const struct ctx *cx, const struct dom_node *n, struct mat m, struct bb *b, int depth)
{
  if (depth > MAX_DEPTH)
    return;
  int k = svg_elem_kind(n->tag);
  if (k == K_SHAPE) {
    struct path p;
    memset(&p, 0, sizeof p);
    if (svg_build_shape(cx, n, &p, 1.0f)) {
      struct bb sb = svg_path_bbox(&p, m);
      if (sb.ok) {
        svg_bb_add(b, sb.x0, sb.y0);
        svg_bb_add(b, sb.x1, sb.y1);
      }
    }
    svg_path_free(&p);
  } else if (k == K_GROUP || k == K_SVG) {
    for (const struct dom_node *c = n->first_child; c; c = c->next)
      if (c->type == DOM_ELEMENT)
        svg_node_bbox(cx, c, svg_mat_mul(m, svg_parse_transform(dom_get_attr(c, "transform"))), b, depth + 1);
  }
}

/* clipping */
static unsigned char *mask_new(const struct ctx *cx) { return calloc((unsigned)cx->w * (unsigned)cx->h, 1); }

/* Fold the active clip into `mask` and return it. */
static unsigned char *mask_and_clip(const struct ctx *cx, unsigned char *mask)
{
  if (mask && cx->clip)
    for (int i = 0; i < cx->w * cx->h; i++)
      mask[i] = (unsigned char)(mask[i] * cx->clip[i] / 255);
  return mask;
}

static void fill_mask(struct ctx *cx, struct mat m, bool evenodd, unsigned char *mask)
{
  struct src s;
  memset(&s, 0, sizeof s);
  s.alpha = 255;
  svg_path_xform(&cx->tp[1], &cx->tp[0], m);
  svg_raster_fill(cx, &cx->tp[1], evenodd, &s, mask);
}

unsigned char *svg_rect_clip(struct ctx *cx, struct mat m, float x, float y, float w, float h)
{
  unsigned char *mask = mask_new(cx);
  if (!mask)
    return 0;
  struct path *p = &cx->tp[0];
  svg_path_reset(p);
  svg_path_move(p, x, y);
  svg_path_line(p, x + w, y);
  svg_path_line(p, x + w, y + h);
  svg_path_line(p, x, y + h);
  fill_mask(cx, m, false, mask);
  return mask_and_clip(cx, mask);
}

unsigned char *svg_make_clip(struct ctx *cx, const struct dom_node *cp, struct mat m, const struct dom_node *target,
                             int depth)
{
  if (depth > 8)
    return 0;
  unsigned char *mask = mask_new(cx);
  if (!mask)
    return 0;
  m = svg_mat_mul(m, svg_parse_transform(dom_get_attr(cp, "transform")));
  const char *u = dom_get_attr(cp, "clippathunits");
  if (u && starts_ci(u, "objectBoundingBox")) {
    struct bb b = {0, 0, 0, 0, false};
    svg_node_bbox(cx, target, svg_mat_id(), &b, 0);
    if (!b.ok) {
      free(mask);
      return 0;
    }
    m = svg_mat_mul(m, (struct mat){b.x1 - b.x0, 0, 0, b.y1 - b.y0, b.x0, b.y0});
  }
  char buf[32];
  bool cp_eo = svg_get_prop(cx, cp, "clip-rule", buf, sizeof buf) && starts_ci(buf, "evenodd");
  for (const struct dom_node *c = cp->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT || svg_display_none(cx, c))
      continue;
    const struct dom_node *shape = c;
    struct mat mm = svg_mat_mul(m, svg_parse_transform(dom_get_attr(c, "transform")));
    if (streq(c->tag, "use")) {
      shape = svg_href_target(cx, c);
      if (!shape)
        continue;
      mm = svg_mat_mul(mm, svg_mat_translate(svg_attr_len(c, "x", 0, cx->vw), svg_attr_len(c, "y", 0, cx->vh)));
      mm = svg_mat_mul(mm, svg_parse_transform(dom_get_attr(shape, "transform")));
    }
    if (svg_elem_kind(shape->tag) != K_SHAPE)
      continue;
    svg_path_reset(&cx->tp[0]);
    if (!svg_build_shape(cx, shape, &cx->tp[0], svg_mat_scale(mm)))
      continue;
    bool eo = cp_eo;
    if (svg_get_prop(cx, c, "clip-rule", buf, sizeof buf))
      eo = starts_ci(buf, "evenodd");
    fill_mask(cx, mm, eo, mask);
  }
  if (svg_get_prop(cx, cp, "clip-path", buf, sizeof buf) && starts_ci(buf, "url(")) {
    const struct dom_node *inner = svg_ref_target(cx, buf);
    if (inner && streq(inner->tag, "clippath") && inner != cp) {
      unsigned char *im = svg_make_clip(cx, inner, m, target, depth + 1);
      if (im) {
        for (int i = 0; i < cx->w * cx->h; i++)
          mask[i] = (unsigned char)(mask[i] * im[i] / 255);
        free(im);
      }
    }
  }
  return mask_and_clip(cx, mask);
}

/* Luminance (or alpha) mask of a <mask> element, ready to multiply into the active clip. */
unsigned char *svg_make_mask(struct ctx *cx, const struct dom_node *mk, struct mat m, const struct dom_node *target,
                             int depth)
{
  if (depth > 4 || cx->sub_depth >= 3)
    return 0;
  unsigned char *out = mask_new(cx);
  if (!out)
    return 0;
  const char *mu = dom_get_attr(mk, "maskunits"), *cu = dom_get_attr(mk, "maskcontentunits");
  bool obb = !(mu && starts_ci(mu, "userSpaceOnUse")), cobb = cu && starts_ci(cu, "objectBoundingBox");
  struct bb b = {0, 0, 0, 0, false};
  if (obb || cobb) {
    svg_node_bbox(cx, target, svg_mat_id(), &b, 0);
    if (!b.ok || b.x1 <= b.x0 || b.y1 <= b.y0)
      return out; /* nothing to mask against: the element disappears */
  }
  float bw = b.x1 - b.x0, bh = b.y1 - b.y0;
  float x = svg_grad_coord(dom_get_attr(mk, "x"), obb ? -0.1f : -0.1f * cx->vw, obb, cx->vw);
  float y = svg_grad_coord(dom_get_attr(mk, "y"), obb ? -0.1f : -0.1f * cx->vh, obb, cx->vh);
  float w = svg_grad_coord(dom_get_attr(mk, "width"), obb ? 1.2f : 1.2f * cx->vw, obb, cx->vw);
  float h = svg_grad_coord(dom_get_attr(mk, "height"), obb ? 1.2f : 1.2f * cx->vh, obb, cx->vh);
  if (obb) {
    x = b.x0 + x * bw;
    y = b.y0 + y * bh;
    w *= bw;
    h *= bh;
  }
  unsigned *layer = calloc((unsigned)cx->w * (unsigned)cx->h, sizeof *layer);
  if (!layer || !(w > 0 && h > 0)) {
    free(layer);
    return out;
  }
  unsigned *saved_buf = cx->buf;
  unsigned char *saved_clip = cx->clip;
  cx->buf = layer;
  cx->clip = 0;
  cx->sub_depth++;
  struct style bs;
  svg_style_base(&bs, 0);
  svg_resolve_style(cx, mk, &bs);
  struct mat cm = cobb ? svg_mat_mul(m, (struct mat){bw, 0, 0, bh, b.x0, b.y0}) : m;
  svg_render_children(cx, mk, cm, &bs, depth + 1, false);
  cx->sub_depth--;
  cx->buf = saved_buf;

  char tb[32];
  bool alpha_only = svg_get_prop(cx, mk, "mask-type", tb, sizeof tb) && starts_ci(tb, "alpha");
  unsigned char *region = svg_rect_clip(cx, m, x, y, w, h); /* cx->clip is still NULL here */
  cx->clip = saved_clip;
  for (int i = 0; i < cx->w * cx->h; i++) {
    unsigned p = layer[i];
    unsigned v = alpha_only ? p >> 24 : ((((p >> 16) & 255) * 54 + ((p >> 8) & 255) * 183 + (p & 255) * 19) >> 8);
    if (v > 255)
      v = 255;
    out[i] = region ? (unsigned char)(v * region[i] / 255) : (unsigned char)v;
  }
  free(layer);
  free(region);
  return mask_and_clip(cx, out);
}
