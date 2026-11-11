/* pdfrender.c, the PDF content stream interpreter: graphics state, paths, colors, forms, shadings, pages */
#include "math.h"
#include "owos.h"
#include "pdfint.h"

#define MAX_OPS 4000000L
#define MAX_FORM_DEPTH 12
#define MAX_OPERANDS 40

static struct cspace cs_gray = {.kind = CS_GRAY, .n = 1};
static struct cspace cs_rgb = {.kind = CS_RGB, .n = 3};
static struct cspace cs_cmyk = {.kind = CS_CMYK, .n = 4};
static struct cspace cs_pattern = {.kind = CS_PATTERN, .n = 0};

static struct c2d_mat mat_mul(struct c2d_mat a, struct c2d_mat b) { return c2d_mat_mul(a, b); }

static struct c2d_mat mat_of(double a, double b, double c, double dd, double e, double f)
{
  struct c2d_mat m = {(float)a, (float)b, (float)c, (float)dd, (float)e, (float)f};
  return m;
}

static struct c2d_mat mat_from_arr(struct pdf *d, struct pobj a)
{
  if (a.t != PO_ARR || a.n < 6)
    return C2D_IDENTITY;
  return mat_of(pdf_num(pdf_at(d, a, 0), 1), pdf_num(pdf_at(d, a, 1), 0), pdf_num(pdf_at(d, a, 2), 0),
                pdf_num(pdf_at(d, a, 3), 1), pdf_num(pdf_at(d, a, 4), 0), pdf_num(pdf_at(d, a, 5), 0));
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* color spaces */

static struct cspace *cs_alloc(struct rctx *rc, int kind, int n)
{
  struct cspace *cs = arena_alloc(&rc->ar, sizeof *cs);
  if (cs) {
    cs->kind = kind;
    cs->n = n;
  }
  return cs;
}

struct cspace *pdf_cs_parse(struct rctx *rc, struct pobj o, int depth)
{
  struct pdf *d = rc->d;
  if (depth > 6)
    return &cs_gray;
  o = pdf_deref(d, o);
  if (o.t == PO_NAME) {
    const char *n = o.u.s;
    if (!strcmp(n, "DeviceGray") || !strcmp(n, "G") || !strcmp(n, "CalGray"))
      return &cs_gray;
    if (!strcmp(n, "DeviceRGB") || !strcmp(n, "RGB") || !strcmp(n, "CalRGB"))
      return &cs_rgb;
    if (!strcmp(n, "DeviceCMYK") || !strcmp(n, "CMYK"))
      return &cs_cmyk;
    if (!strcmp(n, "Pattern"))
      return &cs_pattern;
    int named = strlen(n) < (int)sizeof rc->cs_cache[0].name;
    for (int i = 0; named && i < 16; i++)
      if (rc->cs_cache[i].cs && rc->cs_cache[i].res == rc->res.u.kv && !strcmp(rc->cs_cache[i].name, n))
        return rc->cs_cache[i].cs;
    struct pobj r = pdf_get(d, pdf_get(d, rc->res, "ColorSpace"), n);
    struct cspace *cs = r.t == PO_NULL ? &cs_gray : pdf_cs_parse(rc, r, depth + 1);
    if (named) {
      int slot = rc->cs_next++ & 15;
      rc->cs_cache[slot].res = rc->res.u.kv;
      rc->cs_cache[slot].cs = cs;
      strcpy(rc->cs_cache[slot].name, n);
    }
    return cs;
  }
  if (o.t != PO_ARR || o.n < 1)
    return &cs_gray;
  struct pobj k = pdf_at(d, o, 0);
  if (k.t != PO_NAME)
    return &cs_gray;
  const char *n = k.u.s;
  if (!strcmp(n, "ICCBased")) {
    int cnt = (int)pdf_num(pdf_get(d, pdf_at(d, o, 1), "N"), 3);
    return cnt == 1 ? &cs_gray : cnt == 4 ? &cs_cmyk : &cs_rgb;
  }
  if (!strcmp(n, "CalGray"))
    return &cs_gray;
  if (!strcmp(n, "CalRGB"))
    return &cs_rgb;
  if (!strcmp(n, "Lab")) {
    struct cspace *cs = cs_alloc(rc, CS_LAB, 3);
    if (!cs)
      return &cs_rgb;
    struct pobj r = pdf_get(d, pdf_at(d, o, 1), "Range");
    for (int i = 0; i < 4; i++)
      cs->range[i] = pdf_num(pdf_at(d, r, i), i & 1 ? 100 : -100);
    return cs;
  }
  if (!strcmp(n, "Indexed") || !strcmp(n, "I")) {
    struct cspace *cs = cs_alloc(rc, CS_INDEXED, 1);
    if (!cs)
      return &cs_gray;
    cs->alt = pdf_cs_parse(rc, pdf_at(d, o, 1), depth + 1);
    cs->hival = (int)pdf_num(pdf_at(d, o, 2), 0);
    struct pobj lk = pdf_at(d, o, 3);
    if (lk.t == PO_STR) {
      cs->lookup = (const unsigned char *)lk.u.s;
      cs->nlookup = lk.n;
    } else if (lk.t == PO_STREAM) {
      int len = 0;
      unsigned char *data = pdf_stream_data(d, lk, &len, 0, 0),
                    *copy = data ? arena_alloc(&rc->ar, (unsigned)len + 1) : 0;
      if (copy) {
        memcpy(copy, data, (unsigned)len);
        cs->lookup = copy;
        cs->nlookup = len;
      }
      free(data);
    }
    return cs;
  }
  if (!strcmp(n, "Separation") || !strcmp(n, "DeviceN")) {
    struct pobj names = pdf_at(d, o, 1);
    int cnt = !strcmp(n, "DeviceN") && names.t == PO_ARR ? names.n : 1;
    if (cnt < 1 || cnt > 8)
      return &cs_gray;
    struct cspace *cs = cs_alloc(rc, CS_TINT, cnt);
    if (!cs)
      return &cs_gray;
    cs->alt = pdf_cs_parse(rc, pdf_at(d, o, 2), depth + 1);
    cs->fn = pdf_func_load(d, &rc->ar, pdf_at(d, o, 3));
    return cs;
  }
  if (!strcmp(n, "Pattern")) {
    struct cspace *cs = cs_alloc(rc, CS_PATTERN, 0);
    if (cs && o.n > 1)
      cs->alt = pdf_cs_parse(rc, pdf_at(d, o, 1), depth + 1);
    return cs ? cs : &cs_pattern;
  }
  return pdf_cs_parse(rc, k, depth + 1);
}

static double lab_gamma(double v)
{
  v = clamp01(v);
  return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
}

static double lab_inv(double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); }

void pdf_cs_rgb(const struct cspace *cs, const double *c, unsigned char *rgb)
{
  double r = 0, g = 0, b = 0;
  switch (cs->kind) {
  case CS_GRAY:
    r = g = b = clamp01(c[0]);
    break;
  case CS_RGB:
    r = clamp01(c[0]);
    g = clamp01(c[1]);
    b = clamp01(c[2]);
    break;
  case CS_CMYK: {
    double k = 1 - clamp01(c[3]);
    r = (1 - clamp01(c[0])) * k;
    g = (1 - clamp01(c[1])) * k;
    b = (1 - clamp01(c[2])) * k;
    break;
  }
  case CS_LAB: {
    double L = c[0], a = c[1], bb = c[2];
    double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - bb / 200;
    double X = 0.9642 * lab_inv(fx), Y = lab_inv(fy), Z = 0.8249 * lab_inv(fz);
    r = lab_gamma(3.1339 * X - 1.6169 * Y - 0.4906 * Z);
    g = lab_gamma(-0.9788 * X + 1.9161 * Y + 0.0335 * Z);
    b = lab_gamma(0.0719 * X - 0.2290 * Y + 1.4052 * Z);
    break;
  }
  case CS_INDEXED: {
    int i = (int)c[0], bn = cs->alt ? cs->alt->n : 3;
    double comp[8] = {0};
    if (i < 0)
      i = 0;
    if (i > cs->hival)
      i = cs->hival;
    for (int k = 0; k < bn && k < 8; k++)
      comp[k] = i * bn + k < cs->nlookup ? cs->lookup[i * bn + k] / 255.0 : 0;
    if (cs->alt && cs->alt->kind == CS_LAB) {
      comp[0] *= 100;
      comp[1] = cs->alt->range[0] + comp[1] * (cs->alt->range[1] - cs->alt->range[0]);
      comp[2] = cs->alt->range[2] + comp[2] * (cs->alt->range[3] - cs->alt->range[2]);
    }
    pdf_cs_rgb(cs->alt ? cs->alt : &cs_rgb, comp, rgb);
    return;
  }
  case CS_TINT: {
    double out[8] = {0};
    if (cs->fn && cs->alt && pdf_func_eval(cs->fn, c, cs->n, out, 8) > 0) {
      pdf_cs_rgb(cs->alt, out, rgb);
      return;
    }
    r = g = b = 1 - clamp01(c[0]);
    break;
  }
  default:
    break;
  }
  rgb[0] = (unsigned char)(r * 255 + 0.5);
  rgb[1] = (unsigned char)(g * 255 + 0.5);
  rgb[2] = (unsigned char)(b * 255 + 0.5);
}

void pdf_color_update(struct color *c) { pdf_cs_rgb(c->cs, c->c, c->rgb); }

static void color_reset(struct color *col, struct cspace *cs)
{
  col->cs = cs;
  memset(col->c, 0, sizeof col->c);
  if (cs->kind == CS_CMYK)
    col->c[3] = 1;
  else if (cs->kind == CS_TINT)
    for (int i = 0; i < 8; i++)
      col->c[i] = 1;
  col->pattern = pdf_null();
  pdf_color_update(col);
}

/* optional content */

static int oc_off(struct rctx *rc, struct pobj ocg)
{
  struct pdf *d = rc->d;
  if (ocg.t != PO_REF)
    return 0;
  struct pobj cfg = pdf_get(d, pdf_get(d, pdf_get(d, d->trailer, "Root"), "OCProperties"), "D");
  struct pobj off = pdf_get(d, cfg, "OFF"), on = pdf_get(d, cfg, "ON");
  for (int i = 0; i < off.n; i++)
    if (off.u.a[i].t == PO_REF && off.u.a[i].n == ocg.n)
      return 1;
  if (pdf_name_is(pdf_get(d, cfg, "BaseState"), "OFF")) {
    for (int i = 0; i < on.n; i++)
      if (on.u.a[i].t == PO_REF && on.u.a[i].n == ocg.n)
        return 0;
    return 1;
  }
  return 0;
}

/* state helpers */

static struct c2d_state *cst(struct rctx *rc) { return &rc->c->st[rc->c->depth]; }

static void gs_init(struct gstate *g, struct c2d_mat base)
{
  memset(g, 0, sizeof *g);
  g->ctm = base;
  color_reset(&g->fill, &cs_gray);
  color_reset(&g->stroke, &cs_gray);
  g->lw = 1;
  g->miter = 10;
  g->fa = g->sa = 1;
  g->th = 1;
}

static void push_state(struct rctx *rc)
{
  if (rc->sp >= 31)
    return;
  rc->stack[rc->sp++] = rc->gs;
  c2d_save(rc->c);
}

static void pop_state(struct rctx *rc)
{
  if (rc->sp <= 0)
    return;
  rc->gs = rc->stack[--rc->sp];
  c2d_restore(rc->c);
}

static void end_path(struct rctx *rc)
{
  if (rc->clip_pending) {
    struct c2d_path *p = &rc->path;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < p->npt; i++) {
      float x = p->pt[i * 2], y = p->pt[i * 2 + 1];
      minx = x < minx ? x : minx;
      maxx = x > maxx ? x : maxx;
      miny = y < miny ? y : miny;
      maxy = y > maxy ? y : maxy;
    }
    int rect = (p->npt == 4 || p->npt == 5) && minx <= 0 && miny <= 0 && maxx >= rc->c->w && maxy >= rc->c->h;
    for (int i = 0; rect && i < p->npt; i++)
      rect = (p->pt[i * 2] == minx || p->pt[i * 2] == maxx) && (p->pt[i * 2 + 1] == miny || p->pt[i * 2 + 1] == maxy);
    if (!rect)
      c2d_clip(rc->c, p, rc->clip_pending == 2);
    rc->clip_pending = 0;
  }
  c2d_path_clear(&rc->path);
}

static void paint_fill(struct rctx *rc, int evenodd)
{
  struct gstate *g = &rc->gs;
  struct c2d_state *st = cst(rc);
  if (g->fill.cs->kind == CS_PATTERN) {
    struct pobj pat = g->fill.pattern;
    if (pat.t != PO_NULL && (int)pdf_num(pdf_get(rc->d, pat, "PatternType"), 0) == 2) {
      c2d_save(rc->c);
      c2d_clip(rc->c, &rc->path, evenodd);
      struct c2d_mat m = mat_mul(rc->base, mat_from_arr(rc->d, pdf_get(rc->d, pat, "Matrix")));
      pdf_paint_shading(rc, pdf_get(rc->d, pat, "Shading"), &m);
      c2d_restore(rc->c);
    }
    return;
  }
  c2d_set_paint_color(&st->fill, g->fill.rgb[0], g->fill.rgb[1], g->fill.rgb[2], (unsigned char)(g->fa * 255 + 0.5));
  c2d_fill(rc->c, &rc->path, evenodd);
}

static void paint_stroke(struct rctx *rc)
{
  struct gstate *g = &rc->gs;
  struct c2d_state *st = cst(rc);
  float s = (float)sqrt(fabs((double)g->ctm.a * g->ctm.d - (double)g->ctm.b * g->ctm.c));
  if (s <= 0)
    return;
  float dash[8];
  int nd = g->ndash;
  double sum = 0;
  for (int i = 0; i < nd; i++) {
    dash[i] = (float)g->dash[i];
    sum += g->dash[i];
  }
  st->m = mat_of(s, 0, 0, s, 0, 0);
  st->line_width = (float)(g->lw * s < 1 ? 1 / s : g->lw);
  st->cap = g->cap;
  st->join = g->join;
  st->miter_limit = (float)g->miter;
  c2d_set_dash(rc->c, dash, sum > 0 ? nd : 0);
  st->dash_off = (float)g->dphase;
  c2d_set_paint_color(&st->stroke, g->stroke.rgb[0], g->stroke.rgb[1], g->stroke.rgb[2],
                      (unsigned char)(g->sa * 255 + 0.5));
  c2d_stroke(rc->c, &rc->path);
  st->m = C2D_IDENTITY;
}

void pdf_paint_shading(struct rctx *rc, struct pobj sh, const struct c2d_mat *m)
{
  struct pdf *d = rc->d;
  int type = (int)pdf_num(pdf_get(d, sh, "ShadingType"), 0);
  if (type != 2 && type != 3)
    return;
  struct pobj co = pdf_get(d, sh, "Coords"), dom = pdf_get(d, sh, "Domain"), fn = pdf_get(d, sh, "Function");
  if (co.t != PO_ARR || co.n < (type == 2 ? 4 : 6))
    return;
  struct cspace *cs = pdf_cs_parse(rc, pdf_get(d, sh, "ColorSpace"), 0);
  double t0 = pdf_num(pdf_at(d, dom, 0), 0), t1 = pdf_num(pdf_at(d, dom, 1), 1);
  struct pfunc *fns[8];
  int nfn = 0;
  if (fn.t == PO_ARR) {
    for (; nfn < fn.n && nfn < 8; nfn++)
      if (!(fns[nfn] = pdf_func_load(d, &rc->ar, fn.u.a[nfn])))
        return;
  } else if ((fns[0] = pdf_func_load(d, &rc->ar, fn)))
    nfn = 1;
  if (!nfn)
    return;
  double v[6];
  for (int i = 0; i < 6; i++)
    v[i] = i < co.n ? pdf_num(pdf_at(d, co, i), 0) : 0;
  struct c2d_gradient *gr =
      type == 2
          ? c2d_gradient_new(C2D_LINEAR, (float)v[0], (float)v[1], 0, (float)v[2], (float)v[3], 0)
          : c2d_gradient_new(C2D_RADIAL, (float)v[0], (float)v[1], (float)v[2], (float)v[3], (float)v[4], (float)v[5]);
  if (!gr)
    return;
  for (int i = 0; i <= 32; i++) {
    double t = t0 + (t1 - t0) * i / 32, comp[8] = {0}, out[8];
    if (nfn == 1) {
      if (pdf_func_eval(fns[0], &t, 1, comp, 8) < 1)
        comp[0] = 0;
    } else
      for (int k = 0; k < nfn; k++)
        comp[k] = pdf_func_eval(fns[k], &t, 1, out, 1) ? out[0] : 0;
    unsigned char rgb[3];
    pdf_cs_rgb(cs, comp, rgb);
    c2d_gradient_add_stop(gr, (float)i / 32, rgb[0], rgb[1], rgb[2], (unsigned char)(rc->gs.fa * 255 + 0.5));
  }
  struct c2d_mat inv;
  struct c2d_state *st = cst(rc);
  if (c2d_mat_invert(m, &inv)) {
    float xs[4], ys[4], w = (float)rc->c->w, h = (float)rc->c->h;
    c2d_mat_apply(&inv, 0, 0, &xs[0], &ys[0]);
    c2d_mat_apply(&inv, w, 0, &xs[1], &ys[1]);
    c2d_mat_apply(&inv, 0, h, &xs[2], &ys[2]);
    c2d_mat_apply(&inv, w, h, &xs[3], &ys[3]);
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < 4; i++) {
      x0 = xs[i] < x0 ? xs[i] : x0;
      x1 = xs[i] > x1 ? xs[i] : x1;
      y0 = ys[i] < y0 ? ys[i] : y0;
      y1 = ys[i] > y1 ? ys[i] : y1;
    }
    st->m = *m;
    c2d_set_paint_gradient(&st->fill, gr);
    c2d_fill_rect(rc->c, x0, y0, x1 - x0, y1 - y0);
    st->m = C2D_IDENTITY;
  }
  c2d_gradient_unref(gr);
}

/* resources and XObjects */

static struct pobj res_get(struct rctx *rc, const char *cat, const char *name, int raw)
{
  struct pobj dict = pdf_get(rc->d, rc->res, cat);
  return raw ? pdf_getraw(rc->d, dict, name) : pdf_get(rc->d, dict, name);
}

static void do_form(struct rctx *rc, struct pobj form);

static void do_xobject(struct rctx *rc, const char *name, int hidden)
{
  struct pobj x = res_get(rc, "XObject", name, 0);
  if (x.t != PO_STREAM || hidden || rc->depth > MAX_FORM_DEPTH)
    return;
  struct pobj oc = pdf_getraw(rc->d, x, "OC");
  if (oc_off(rc, oc))
    return;
  struct pobj st = pdf_get(rc->d, x, "Subtype");
  if (pdf_name_is(st, "Form"))
    do_form(rc, x);
  else if (pdf_name_is(st, "Image")) {
    int len = 0;
    const char *left = 0;
    struct pobj lp = pdf_null();
    unsigned char *data = pdf_stream_data(rc->d, x, &len, &left, &lp);
    if (data)
      pdf_draw_image(rc, x, data, len, left, lp);
    free(data);
  }
}

static void do_form(struct rctx *rc, struct pobj form)
{
  struct pdf *d = rc->d;
  int len = 0;
  unsigned char *data = pdf_stream_data(d, form, &len, 0, 0);
  if (!data)
    return;
  struct pobj saved_res = rc->res, r = pdf_get(d, form, "Resources");
  double tm[6], tlm[6];
  memcpy(tm, rc->tm, sizeof tm);
  memcpy(tlm, rc->tlm, sizeof tlm);
  push_state(rc);
  rc->gs.ctm = mat_mul(rc->gs.ctm, mat_from_arr(d, pdf_get(d, form, "Matrix")));
  struct pobj bb = pdf_get(d, form, "BBox");
  if (bb.t == PO_ARR && bb.n >= 4) {
    c2d_path_clear(&rc->path);
    double x0 = pdf_num(pdf_at(d, bb, 0), 0), y0 = pdf_num(pdf_at(d, bb, 1), 0);
    double x1 = pdf_num(pdf_at(d, bb, 2), 0), y1 = pdf_num(pdf_at(d, bb, 3), 0);
    c2d_path_rect(&rc->path, &rc->gs.ctm, (float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0));
    rc->clip_pending = 1;
    end_path(rc);
  }
  if (r.t == PO_DICT)
    rc->res = r;
  rc->depth++;
  pdf_run_content(rc, data, len);
  rc->depth--;
  rc->res = saved_res;
  pop_state(rc);
  memcpy(rc->tm, tm, sizeof tm);
  memcpy(rc->tlm, tlm, sizeof tlm);
  free(data);
}

/* the interpreter */

struct run {
  struct rctx *rc;
  struct lex l;
  struct arena tmp;
  int hidden_stack[32], nhidden, hidden;
};

#define K1(a) ((unsigned)(a))
#define K2(a, b) ((unsigned)(a) | (unsigned)(b) << 8)
#define K3(a, b, c) ((unsigned)(a) | (unsigned)(b) << 8 | (unsigned)(c) << 16)

static void set_color_op(struct rctx *rc, struct color *col, struct pobj *ops, int n, int pattern_ok)
{
  if (n < 1)
    return;
  struct pobj last = ops[n - 1];
  if (last.t == PO_NAME && col->cs->kind == CS_PATTERN && pattern_ok) {
    col->pattern = res_get(rc, "Pattern", last.u.s, 0);
    n--;
    if (col->cs->alt && n > 0) {
      for (int i = 0; i < n && i < 8; i++)
        col->c[i] = pdf_num(ops[i], 0);
      pdf_cs_rgb(col->cs->alt, col->c, col->rgb);
    }
    return;
  }
  for (int i = 0; i < n && i < 8; i++)
    col->c[i] = pdf_num(ops[i], 0);
  pdf_color_update(col);
}

static void set_device(struct color *col, struct cspace *cs, struct pobj *ops, int n)
{
  col->cs = cs;
  for (int i = 0; i < cs->n && i < 8; i++)
    col->c[i] = i < n ? pdf_num(ops[i], 0) : 0;
  pdf_color_update(col);
}

static void apply_extgstate(struct rctx *rc, struct pobj g)
{
  struct pdf *d = rc->d;
  struct gstate *gs = &rc->gs;
  for (int i = 0; i < g.n; i++) {
    const char *k = g.u.kv[i].k;
    struct pobj v = pdf_deref(d, g.u.kv[i].v);
    if (!strcmp(k, "LW"))
      gs->lw = pdf_num(v, 1);
    else if (!strcmp(k, "LC"))
      gs->cap = (int)pdf_num(v, 0);
    else if (!strcmp(k, "LJ"))
      gs->join = (int)pdf_num(v, 0) == 1 ? C2D_ROUND_JOIN : (int)pdf_num(v, 0) == 2 ? C2D_BEVEL : C2D_MITER;
    else if (!strcmp(k, "ML"))
      gs->miter = pdf_num(v, 10);
    else if (!strcmp(k, "CA"))
      gs->sa = clamp01(pdf_num(v, 1));
    else if (!strcmp(k, "ca"))
      gs->fa = clamp01(pdf_num(v, 1));
    else if (!strcmp(k, "D") && v.t == PO_ARR) {
      struct pobj arr = pdf_at(d, v, 0);
      gs->ndash = arr.t == PO_ARR && arr.n <= 8 ? arr.n : 0;
      for (int j = 0; j < gs->ndash; j++)
        gs->dash[j] = pdf_num(pdf_at(d, arr, j), 0);
      gs->dphase = pdf_num(pdf_at(d, v, 1), 0);
    } else if (!strcmp(k, "Font") && v.t == PO_ARR) {
      gs->font = pdf_font_load(d, v.u.a[0]);
      gs->fs = pdf_num(pdf_at(d, v, 1), 0);
    }
  }
}

static void inline_image(struct run *r)
{
  struct rctx *rc = r->rc;
  struct pdf *d = rc->d;
  struct lex *l = &r->l;
  struct pobj k, v;
  struct pkv kv[24];
  int n = 0;
  while (pdf_lex(l, &k)) {
    if (k.t == PO_KW)
      break;
    if (k.t != PO_NAME || !pdf_lex(l, &v) || v.t == PO_KW)
      continue;
    if (n < 24) {
      kv[n].k = k.u.s;
      kv[n++].v = v;
    }
  }
  if (k.t != PO_KW || strcmp(k.u.s, "ID"))
    return;
  struct pobj dict = pdf_null();
  dict.t = PO_DICT;
  dict.n = n;
  dict.u.kv = arena_alloc(&r->tmp, (unsigned)n * sizeof *kv + 8);
  if (!dict.u.kv)
    return;
  memcpy(dict.u.kv, kv, (unsigned)n * sizeof *kv);
  int start = l->pos < l->len ? l->pos + 1 : l->pos;
  int end = -1;
  for (int i = start; i + 1 < l->len; i++)
    if (l->b[i] == 'E' && l->b[i + 1] == 'I' && (i + 2 >= l->len || l->b[i + 2] <= 32 || l->b[i + 2] == 'Q') &&
        (i == start || l->b[i - 1] <= 32)) {
      end = i;
      break;
    }
  if (end < 0) {
    l->pos = l->len;
    return;
  }
  l->pos = end + 2;
  int dl = end - start;
  const char *left = 0;
  struct pobj lp = pdf_null();
  struct pobj fl = pdf_get(d, dict, "F"), dp = pdf_get(d, dict, "DP");
  if (fl.t == PO_NULL)
    fl = pdf_get(d, dict, "Filter");
  if (dp.t == PO_NULL)
    dp = pdf_get(d, dict, "DecodeParms");
  int out = 0;
  unsigned char *data = pdf_decode(d, l->b + start, dl, fl, dp, &out, &left, &lp);
  if (data && !r->hidden)
    pdf_draw_image(rc, dict, data, out, left, lp);
  free(data);
}

static void exec_op(struct run *r, const char *op, struct pobj *ops, int n)
{
  struct rctx *rc = r->rc;
  struct pdf *d = rc->d;
  struct gstate *g = &rc->gs;
  unsigned key =
      op[0] ? (op[1] ? (op[2] ? (op[3] ? 0xffffffu : K3(op[0], op[1], op[2])) : K2(op[0], op[1])) : K1(op[0])) : 0;
#define N(i) ((i) < n ? pdf_num(ops[i], 0) : 0.0)
  switch (key) {
  case K1('q'):
    push_state(rc);
    break;
  case K1('Q'):
    pop_state(rc);
    break;
  case K2('c', 'm'):
    g->ctm = mat_mul(g->ctm, mat_of(N(0), N(1), N(2), N(3), N(4), N(5)));
    break;
  case K1('w'):
    g->lw = N(0);
    break;
  case K1('J'):
    g->cap = (int)N(0) == 1 ? C2D_ROUND : (int)N(0) == 2 ? C2D_SQUARE : C2D_BUTT;
    break;
  case K1('j'):
    g->join = (int)N(0) == 1 ? C2D_ROUND_JOIN : (int)N(0) == 2 ? C2D_BEVEL : C2D_MITER;
    break;
  case K1('M'):
    g->miter = N(0);
    break;
  case K1('d'):
    if (n >= 2 && ops[0].t == PO_ARR) {
      g->ndash = ops[0].n <= 8 ? ops[0].n : 0;
      for (int i = 0; i < g->ndash; i++)
        g->dash[i] = pdf_num(ops[0].u.a[i], 0);
      g->dphase = N(1);
    }
    break;
  case K2('g', 's'):
    if (n >= 1 && ops[0].t == PO_NAME)
      apply_extgstate(rc, res_get(rc, "ExtGState", ops[0].u.s, 0));
    break;
  case K1('m'):
    c2d_path_move(&rc->path, &g->ctm, (float)N(0), (float)N(1));
    break;
  case K1('l'):
    c2d_path_line(&rc->path, &g->ctm, (float)N(0), (float)N(1));
    break;
  case K1('c'):
    c2d_path_bezier(&rc->path, &g->ctm, (float)N(0), (float)N(1), (float)N(2), (float)N(3), (float)N(4), (float)N(5));
    break;
  case K1('v'):
    c2d_path_bezier(&rc->path, &g->ctm, rc->path.ux, rc->path.uy, (float)N(0), (float)N(1), (float)N(2), (float)N(3));
    break;
  case K1('y'):
    c2d_path_bezier(&rc->path, &g->ctm, (float)N(0), (float)N(1), (float)N(2), (float)N(3), (float)N(2), (float)N(3));
    break;
  case K1('h'):
    c2d_path_close(&rc->path);
    break;
  case K2('r', 'e'):
    c2d_path_rect(&rc->path, &g->ctm, (float)N(0), (float)N(1), (float)N(2), (float)N(3));
    break;
  case K1('S'):
  case K1('s'):
    if (key == K1('s'))
      c2d_path_close(&rc->path);
    if (!r->hidden)
      paint_stroke(rc);
    end_path(rc);
    break;
  case K1('f'):
  case K1('F'):
  case K2('f', '*'):
    if (!r->hidden)
      paint_fill(rc, key == K2('f', '*'));
    end_path(rc);
    break;
  case K1('B'):
  case K2('B', '*'):
  case K1('b'):
  case K2('b', '*'):
    if (op[0] == 'b')
      c2d_path_close(&rc->path);
    if (!r->hidden) {
      paint_fill(rc, op[1] == '*');
      paint_stroke(rc);
    }
    end_path(rc);
    break;
  case K1('n'):
    end_path(rc);
    break;
  case K1('W'):
    rc->clip_pending = 1;
    break;
  case K2('W', '*'):
    rc->clip_pending = 2;
    break;
  case K2('B', 'T'):
    memcpy(rc->tm, (double[6]){1, 0, 0, 1, 0, 0}, sizeof rc->tm);
    memcpy(rc->tlm, rc->tm, sizeof rc->tlm);
    break;
  case K2('T', 'c'):
    g->tc = N(0);
    break;
  case K2('T', 'w'):
    g->tw = N(0);
    break;
  case K2('T', 'z'):
    g->th = N(0) / 100;
    break;
  case K2('T', 'L'):
    g->tl = N(0);
    break;
  case K2('T', 'r'):
    g->tr = (int)N(0);
    break;
  case K2('T', 's'):
    g->rise = N(0);
    break;
  case K2('T', 'f'):
    if (n >= 2 && ops[0].t == PO_NAME) {
      g->font = pdf_font_load(d, pdf_getraw(d, pdf_get(d, rc->res, "Font"), ops[0].u.s));
      g->fs = N(1);
    }
    break;
  case K2('T', 'd'):
  case K2('T', 'D'):
    if (key == K2('T', 'D'))
      g->tl = -N(1);
    rc->tlm[4] += N(0) * rc->tlm[0] + N(1) * rc->tlm[2];
    rc->tlm[5] += N(0) * rc->tlm[1] + N(1) * rc->tlm[3];
    memcpy(rc->tm, rc->tlm, sizeof rc->tm);
    break;
  case K2('T', 'm'):
    rc->tlm[0] = N(0);
    rc->tlm[1] = N(1);
    rc->tlm[2] = N(2);
    rc->tlm[3] = N(3);
    rc->tlm[4] = N(4);
    rc->tlm[5] = N(5);
    memcpy(rc->tm, rc->tlm, sizeof rc->tm);
    break;
  case K2('T', '*'):
    rc->tlm[4] += -g->tl * rc->tlm[2];
    rc->tlm[5] += -g->tl * rc->tlm[3];
    memcpy(rc->tm, rc->tlm, sizeof rc->tm);
    break;
  case K2('T', 'j'):
    if (n >= 1 && ops[0].t == PO_STR && !r->hidden)
      pdf_show_text(rc, (const unsigned char *)ops[0].u.s, ops[0].n);
    break;
  case K1('\''):
  case K1('"'):
    if (key == K1('"')) {
      g->tw = N(0);
      g->tc = N(1);
    }
    rc->tlm[4] += -g->tl * rc->tlm[2];
    rc->tlm[5] += -g->tl * rc->tlm[3];
    memcpy(rc->tm, rc->tlm, sizeof rc->tm);
    if (n >= 1 && ops[n - 1].t == PO_STR && !r->hidden)
      pdf_show_text(rc, (const unsigned char *)ops[n - 1].u.s, ops[n - 1].n);
    break;
  case K2('T', 'J'):
    if (n >= 1 && ops[0].t == PO_ARR)
      for (int i = 0; i < ops[0].n; i++) {
        struct pobj e = ops[0].u.a[i];
        if (e.t == PO_STR) {
          if (!r->hidden)
            pdf_show_text(rc, (const unsigned char *)e.u.s, e.n);
        } else {
          double adj = -pdf_num(e, 0) / 1000 * g->fs * g->th;
          rc->tm[4] += adj * rc->tm[0];
          rc->tm[5] += adj * rc->tm[1];
        }
      }
    break;
  case K1('g'):
    set_device(&g->fill, &cs_gray, ops, n);
    break;
  case K1('G'):
    set_device(&g->stroke, &cs_gray, ops, n);
    break;
  case K2('r', 'g'):
    set_device(&g->fill, &cs_rgb, ops, n);
    break;
  case K2('R', 'G'):
    set_device(&g->stroke, &cs_rgb, ops, n);
    break;
  case K1('k'):
    set_device(&g->fill, &cs_cmyk, ops, n);
    break;
  case K1('K'):
    set_device(&g->stroke, &cs_cmyk, ops, n);
    break;
  case K2('c', 's'):
    if (n >= 1)
      color_reset(&g->fill, pdf_cs_parse(rc, ops[0], 0));
    break;
  case K2('C', 'S'):
    if (n >= 1)
      color_reset(&g->stroke, pdf_cs_parse(rc, ops[0], 0));
    break;
  case K2('s', 'c'):
  case K3('s', 'c', 'n'):
    set_color_op(rc, &g->fill, ops, n, key == K3('s', 'c', 'n'));
    break;
  case K2('S', 'C'):
  case K3('S', 'C', 'N'):
    set_color_op(rc, &g->stroke, ops, n, key == K3('S', 'C', 'N'));
    break;
  case K2('s', 'h'):
    if (n >= 1 && ops[0].t == PO_NAME && !r->hidden)
      pdf_paint_shading(rc, res_get(rc, "Shading", ops[0].u.s, 0), &g->ctm);
    break;
  case K2('D', 'o'):
    if (n >= 1 && ops[0].t == PO_NAME)
      do_xobject(rc, ops[0].u.s, r->hidden);
    break;
  case K2('B', 'I'):
    inline_image(r);
    break;
  case K3('B', 'M', 'C'):
  case K3('B', 'D', 'C'): {
    int hide = 0;
    if (key == K3('B', 'D', 'C') && n >= 2 && ops[0].t == PO_NAME && !strcmp(ops[0].u.s, "OC") && ops[1].t == PO_NAME)
      hide = oc_off(rc, res_get(rc, "Properties", ops[1].u.s, 1));
    if (r->nhidden < 32)
      r->hidden_stack[r->nhidden++] = hide;
    r->hidden += hide;
    break;
  }
  case K3('E', 'M', 'C'):
    if (r->nhidden > 0)
      r->hidden -= r->hidden_stack[--r->nhidden];
    break;
  default:
    break;
  }
#undef N
}

void pdf_run_content(struct rctx *rc, const unsigned char *data, int len)
{
  struct run r;
  memset(&r, 0, sizeof r);
  r.rc = rc;
  r.l = (struct lex){data, len, 0, &r.tmp, 0};
  struct pobj t, ops[MAX_OPERANDS];
  int n = 0;
  while (pdf_lex(&r.l, &t)) {
    if (t.t != PO_KW) {
      if (n < MAX_OPERANDS)
        ops[n++] = t;
      continue;
    }
    if (++rc->ops > MAX_OPS)
      break;
    exec_op(&r, t.u.s, ops, n);
    n = 0;
    arena_reset(&r.tmp);
  }
  arena_free(&r.tmp);
}

/* pages */

static int page_box(struct pdf *d, const struct ppage *pg, double b[4])
{
  struct pobj media = pg->media, crop = pg->crop;
  double m[4] = {0, 0, 612, 792};
  int have = 0;
  for (int pass = 0; pass < 2; pass++) {
    struct pobj a = pass == 0 ? media : crop;
    if (a.t != PO_ARR || a.n < 4)
      continue;
    double v[4];
    for (int i = 0; i < 4; i++)
      v[i] = pdf_num(pdf_at(d, a, i), 0);
    double x0 = v[0] < v[2] ? v[0] : v[2], x1 = v[0] < v[2] ? v[2] : v[0];
    double y0 = v[1] < v[3] ? v[1] : v[3], y1 = v[1] < v[3] ? v[3] : v[1];
    if (pass == 1 && have) {
      x0 = x0 < m[0] ? m[0] : x0;
      y0 = y0 < m[1] ? m[1] : y0;
      x1 = x1 > m[2] ? m[2] : x1;
      y1 = y1 > m[3] ? m[3] : y1;
    }
    if (x1 - x0 < 1 || y1 - y0 < 1)
      continue;
    m[0] = x0;
    m[1] = y0;
    m[2] = x1;
    m[3] = y1;
    have = 1;
  }
  memcpy(b, m, sizeof m);
  return ((pg->rot % 360) + 360) % 360;
}

int pdf_page_size(struct pdf *d, int page, float *w, float *h)
{
  if (page < 0 || page >= d->npages)
    return -1;
  double b[4];
  int rot = page_box(d, &d->pages[page], b) / 90 * 90;
  double bw = b[2] - b[0], bh = b[3] - b[1];
  *w = (float)(rot == 90 || rot == 270 ? bh : bw);
  *h = (float)(rot == 90 || rot == 270 ? bw : bh);
  return 0;
}

static void draw_annots(struct rctx *rc, struct pobj page, struct c2d_mat base)
{
  struct pdf *d = rc->d;
  struct pobj an = pdf_get(d, page, "Annots");
  for (int i = 0; i < an.n && i < 2000; i++) {
    struct pobj a = pdf_at(d, an, i);
    if (!pdf_is_dict(a) || ((int)pdf_num(pdf_get(d, a, "F"), 0) & 0x22) ||
        pdf_name_is(pdf_get(d, a, "Subtype"), "Popup"))
      continue;
    struct pobj n = pdf_get(d, pdf_get(d, a, "AP"), "N");
    if (n.t == PO_DICT) {
      struct pobj as = pdf_get(d, a, "AS");
      n = as.t == PO_NAME ? pdf_get(d, n, as.u.s) : pdf_null();
    }
    struct pobj rect = pdf_get(d, a, "Rect"), bb = pdf_get(d, n, "BBox");
    if (n.t != PO_STREAM || rect.t != PO_ARR || rect.n < 4 || bb.t != PO_ARR || bb.n < 4)
      continue;
    struct c2d_mat fm = mat_from_arr(rc->d, pdf_get(d, n, "Matrix"));
    float xs[4], ys[4];
    double bx0 = pdf_num(pdf_at(d, bb, 0), 0), by0 = pdf_num(pdf_at(d, bb, 1), 0);
    double bx1 = pdf_num(pdf_at(d, bb, 2), 0), by1 = pdf_num(pdf_at(d, bb, 3), 0);
    c2d_mat_apply(&fm, (float)bx0, (float)by0, &xs[0], &ys[0]);
    c2d_mat_apply(&fm, (float)bx1, (float)by0, &xs[1], &ys[1]);
    c2d_mat_apply(&fm, (float)bx0, (float)by1, &xs[2], &ys[2]);
    c2d_mat_apply(&fm, (float)bx1, (float)by1, &xs[3], &ys[3]);
    float tx0 = xs[0], tx1 = xs[0], ty0 = ys[0], ty1 = ys[0];
    for (int k = 1; k < 4; k++) {
      tx0 = xs[k] < tx0 ? xs[k] : tx0;
      tx1 = xs[k] > tx1 ? xs[k] : tx1;
      ty0 = ys[k] < ty0 ? ys[k] : ty0;
      ty1 = ys[k] > ty1 ? ys[k] : ty1;
    }
    double r0 = pdf_num(pdf_at(d, rect, 0), 0), r1 = pdf_num(pdf_at(d, rect, 1), 0);
    double r2 = pdf_num(pdf_at(d, rect, 2), 0), r3 = pdf_num(pdf_at(d, rect, 3), 0);
    double rx = r0 < r2 ? r0 : r2, ry = r1 < r3 ? r1 : r3;
    double rw = fabs(r2 - r0), rh = fabs(r3 - r1);
    double sx = tx1 > tx0 ? rw / (tx1 - tx0) : 1, sy = ty1 > ty0 ? rh / (ty1 - ty0) : 1;
    gs_init(&rc->gs, mat_mul(base, mat_of(sx, 0, 0, sy, rx - tx0 * sx, ry - ty0 * sy)));
    rc->sp = 0;
    c2d_save(rc->c);
    do_form(rc, n);
    c2d_restore(rc->c);
  }
}

int pdf_render_page(struct pdf *d, int page, struct c2d *c, float scale, int ox, int oy)
{
  if (page < 0 || page >= d->npages || scale <= 0)
    return -1;
  const struct ppage *pg = &d->pages[page];
  double b[4], s = scale;
  int rot = page_box(d, pg, b) / 90 * 90;
  struct c2d_mat base;
  switch (rot) {
  case 90:
    base = mat_of(0, s, s, 0, -b[1] * s, -b[0] * s);
    break;
  case 180:
    base = mat_of(-s, 0, 0, s, b[2] * s, -b[1] * s);
    break;
  case 270:
    base = mat_of(0, -s, -s, 0, b[3] * s, b[2] * s);
    break;
  default:
    base = mat_of(s, 0, 0, -s, -b[0] * s, b[3] * s);
  }
  base.e -= (float)ox;
  base.f -= (float)oy;
  struct rctx *rc = calloc(1, sizeof *rc);
  if (!rc)
    return -1;
  rc->d = d;
  rc->c = c;
  rc->base = base;
  c2d_reset_state(c);
  c2d_set_paint_color(&c->st[0].fill, 255, 255, 255, 255);
  c2d_fill_rect(c, 0, 0, (float)c->w, (float)c->h);
  gs_init(&rc->gs, base);
  rc->res = pg->res;
  struct pobj contents = pdf_get(d, pg->dict, "Contents");
  int nparts = contents.t == PO_ARR ? contents.n : 1;
  unsigned char *all = 0;
  int alen = 0;
  for (int i = 0; i < nparts && i < 4096; i++) {
    struct pobj st = contents.t == PO_ARR ? pdf_at(d, contents, i) : contents;
    if (st.t != PO_STREAM)
      continue;
    int len = 0;
    unsigned char *data = pdf_stream_data(d, st, &len, 0, 0);
    unsigned char *na = data ? realloc(all, (unsigned)(alen + len + 1)) : 0;
    if (na) {
      all = na;
      memcpy(all + alen, data, (unsigned)len);
      alen += len;
      all[alen++] = '\n';
    }
    free(data);
  }
  if (all)
    pdf_run_content(rc, all, alen);
  free(all);
  while (rc->sp > 0)
    pop_state(rc);
  rc->res = pg->res;
  c2d_reset_state(c);
  draw_annots(rc, pg->dict, base);
  c2d_reset_state(c);
  pdf_glyphs_free(rc);
  c2d_path_free(&rc->path);
  arena_free(&rc->ar);
  free(rc);
  return 0;
}
