/* svg_marker.c: marker-start / marker-mid / marker-end on paths, lines, polylines and polygons. */
#include "features/svg/svg_int.h"

/* Direction of the polyline at flattened point k (inside subpath [a, b)): incoming and outgoing segment angles. */
static bool seg_angle(const struct path *p, int from, int to, float *ang)
{
  float dx = p->x[to] - p->x[from], dy = p->y[to] - p->y[from];
  if (dx * dx + dy * dy < 1e-12f)
    return false;
  *ang = (float)atan2((double)dy, (double)dx);
  return true;
}

static float vertex_angle(const struct path *p, int k, int a, int b, bool closed)
{
  float in = 0, out = 0;
  bool hi = false, ho = false;
  for (int i = k - 1; i >= a && !hi; i--)
    hi = seg_angle(p, i, k, &in);
  for (int i = k + 1; i < b && !ho; i++)
    ho = seg_angle(p, k, i, &out);
  if (closed && !hi && ho)
    for (int i = b - 1; i > k && !hi; i--)
      hi = seg_angle(p, i, a, &in);
  if (closed && !ho && hi)
    for (int i = a + 1; i < k && !ho; i++)
      ho = seg_angle(p, a, i, &out);
  if (hi && ho) {
    float d = out - in;
    while (d > SVG_PI)
      d -= 2 * SVG_PI;
    while (d < -SVG_PI)
      d += 2 * SVG_PI;
    return in + d * 0.5f;
  }
  return hi ? in : ho ? out : 0;
}

static void draw_one(struct ctx *cx, const struct dom_node *mk, float vx, float vy, float angle, struct mat m,
                     const struct style *st)
{
  const char *u = dom_get_attr(mk, "markerunits");
  float scale = u && starts_ci(u, "userSpaceOnUse") ? 1.0f : st->sw;
  float mw = svg_attr_len(mk, "markerwidth", 3, cx->vw), mh = svg_attr_len(mk, "markerheight", 3, cx->vh);
  if (!(mw > 0 && mh > 0 && scale > 0))
    return;
  float vb[4];
  struct mat vm = svg_mat_id();
  float vw = mw, vh = mh;
  if (svg_parse_viewbox(dom_get_attr(mk, "viewbox"), vb)) {
    vm = svg_viewbox_mat(vb, svg_parse_par(dom_get_attr(mk, "preserveaspectratio")), 0, 0, mw, mh);
    vw = vb[2];
    vh = vb[3];
  }
  float rx, ry;
  svg_mat_apply(vm, svg_attr_len(mk, "refx", 0, vw), svg_attr_len(mk, "refy", 0, vh), &rx, &ry);
  float cs = (float)cos((double)angle), sn = (float)sin((double)angle);
  struct mat rot = {cs, sn, -sn, cs, 0, 0};
  struct mat box = svg_mat_mul(m, svg_mat_translate(vx, vy));
  box = svg_mat_mul(box, rot);
  box = svg_mat_mul(box, (struct mat){scale, 0, 0, scale, 0, 0});
  box = svg_mat_mul(box, svg_mat_translate(-rx, -ry));

  float svw = cx->vw, svh = cx->vh;
  cx->vw = vw;
  cx->vh = vh;
  unsigned char *saved = cx->clip, *own = 0;
  const char *ov = dom_get_attr(mk, "overflow");
  if (!(ov && (starts_ci(ov, "visible") || starts_ci(ov, "auto")))) {
    own = svg_rect_clip(cx, box, 0, 0, mw, mh);
    if (own)
      cx->clip = own;
  }
  struct style bs;
  svg_style_base(&bs, st->color);
  svg_resolve_style(cx, mk, &bs);
  svg_render_children(cx, mk, svg_mat_mul(box, vm), &bs, 1, false);
  cx->clip = saved;
  free(own);
  cx->vw = svw;
  cx->vh = svh;
}

/* Draw the markers of the shape whose user-space path is `up` (vertices recorded by svg_path_mark). */
void svg_draw_markers(struct ctx *cx, const struct path *up, struct mat m, const struct style *st)
{
  if ((!st->marker[0] && !st->marker[1] && !st->marker[2]) || up->nv < 1 || cx->use_depth >= 8 || st->hidden)
    return;
  cx->use_depth++;
  int sub = 0;
  for (int i = 0; i < up->nv && cx->budget > 0; i++) {
    int which = i == 0 ? 0 : i == up->nv - 1 ? 2 : 1;
    int k = up->vtx[i];
    if (k < 0 || k >= up->n)
      continue;
    while (sub + 1 < up->ns && up->sub[sub + 1] <= k)
      sub++;
    int a = up->sub[sub], b = svg_sub_end(up, sub);
    for (int pass = 0; pass < (up->nv == 1 ? 2 : 1); pass++) {
      int w = up->nv == 1 ? (pass ? 2 : 0) : which;
      const struct dom_node *mk = st->marker[w];
      if (!mk || !streq(mk->tag, "marker"))
        continue;
      float ang = 0;
      const char *o = dom_get_attr(mk, "orient");
      if (o && starts_ci(o, "auto")) {
        ang = vertex_angle(up, k, a, b, up->closed[sub]);
        if (w == 0 && starts_ci(o, "auto-start-reverse"))
          ang += SVG_PI;
      } else if (o) {
        float deg;
        const char *q = o;
        if (svg_parse_num(&q, o + strlen(o), &deg))
          ang = deg * SVG_PI / 180.0f;
      }
      draw_one(cx, mk, up->x[k], up->y[k], ang, m, st);
    }
  }
  cx->use_depth--;
}
