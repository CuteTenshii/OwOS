#include "engine_int.h"
#include "math.h"

/* Declarations for grid placement and areas, alignment, aspect-ratio, text, transform, filter and clip-path. */

static const char *skip_ws(const char *s)
{
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
    s++;
  return s;
}

/* Next whitespace/comma separated top-level token of [s,e) into buf; returns the position after it, or 0 at the end. */
static const char *next_arg(const char *s, const char *e, char *buf, int cap)
{
  while (s < e && (*s == ' ' || *s == '\t' || *s == ',' || *s == '/'))
    s++;
  if (s >= e)
    return 0;
  int depth = 0, k = 0;
  while (s < e && (depth || (*s != ' ' && *s != '\t' && *s != ',' && *s != '/'))) {
    if (*s == '(')
      depth++;
    else if (*s == ')')
      depth--;
    if (k < cap - 1)
      buf[k++] = *s;
    s++;
  }
  buf[k] = '\0';
  return s;
}

/* Lower-case word followed by '(' at s: returns the length of the word, else 0. */
static int fn_name(const char *s, char *name, int cap)
{
  int k = 0;
  while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-') {
    if (k < cap - 1)
      name[k] = (char)lower(*s);
    k++;
    s++;
  }
  if (*s != '(' || k >= cap)
    return 0;
  name[k] = '\0';
  return k;
}

/* Argument span of the function call at s (just past the '('): sets *ae to the closing ')'. */
static const char *fn_args_end(const char *s)
{
  int depth = 1;
  while (*s) {
    if (*s == '(')
      depth++;
    else if (*s == ')' && --depth == 0)
      return s;
    s++;
  }
  return s;
}

/* An <angle> in degrees; false if not an angle. */
static bool parse_angle(const char *v, int *deg)
{
  v = skip_ws(v);
  double n;
  if (!calc_number(&v, &n))
    return false;
  double d;
  if (starts_ci(v, "deg") || !*v)
    d = n;
  else if (starts_ci(v, "turn"))
    d = n * 360.0;
  else if (starts_ci(v, "grad"))
    d = n * 0.9;
  else if (starts_ci(v, "rad"))
    d = n * 57.29577951;
  else
    return false;
  *deg = (int)(d + (d < 0 ? -0.5 : 0.5));
  return true;
}

/* A <number> or <percentage>: *x1000 gets the value times 1000 (percent counts as value/100). */
static bool parse_num_pct(const char *v, int *x1000)
{
  v = skip_ws(v);
  double n;
  if (!calc_number(&v, &n))
    return false;
  if (*v == '%')
    n /= 100.0;
  else if (*v)
    return false;
  n *= 1000.0;
  *x1000 = (int)(n + (n < 0 ? -0.5 : 0.5));
  return true;
}

static short clamp16(int v) { return (short)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v)); }

/* ---- transform ---- */

/* An affine map under construction: linear part [a c; b d] and a translation whose x and y are each px plus percent of
 * the border box. */
struct xf {
  double a, b, c, d;
  double tx, ty, txp, typ;
};

/* m = m * n (n is applied to a point first). */
static void xf_mul(struct xf *m, const struct xf *n)
{
  struct xf r;
  r.a = m->a * n->a + m->c * n->b;
  r.b = m->b * n->a + m->d * n->b;
  r.c = m->a * n->c + m->c * n->d;
  r.d = m->b * n->c + m->d * n->d;
  r.tx = m->a * n->tx + m->c * n->ty + m->tx;
  r.ty = m->b * n->tx + m->d * n->ty + m->ty;
  r.txp = m->a * n->txp + m->c * n->typ + m->txp;
  r.typ = m->b * n->txp + m->d * n->typ + m->typ;
  *m = r;
}

static void xf_identity(struct xf *x)
{
  x->a = x->d = 1;
  x->b = x->c = x->tx = x->ty = x->txp = x->typ = 0;
}

static int q12(double v) { return (int)(v * 4096.0 + (v < 0 ? -0.5 : 0.5)); }

/* Store `x` in the style: translation always, the linear part when it is not the identity. */
static void xf_store(struct css_style *o, const struct xf *x)
{
  int m[4] = {q12(x->a), q12(x->b), q12(x->c), q12(x->d)};
  memcpy(o->tr_m, m, sizeof m);
  o->tr_mat = m[0] != 4096 || m[1] || m[2] || m[3] != 4096;
  o->tr_x = clamp16((int)(x->tx + (x->tx < 0 ? -0.5 : 0.5)));
  o->tr_y = clamp16((int)(x->ty + (x->ty < 0 ? -0.5 : 0.5)));
  o->tr_xp = clamp16((int)(x->txp + (x->txp < 0 ? -0.5 : 0.5)));
  o->tr_yp = clamp16((int)(x->typ + (x->typ < 0 ? -0.5 : 0.5)));
  o->has_tr = o->tr_mat || o->tr_x || o->tr_y || o->tr_xp || o->tr_yp;
}

/* Load the style's current transform into `x`. */
static void xf_load(const struct css_style *o, struct xf *x)
{
  x->a = o->tr_m[0] / 4096.0;
  x->b = o->tr_m[1] / 4096.0;
  x->c = o->tr_m[2] / 4096.0;
  x->d = o->tr_m[3] / 4096.0;
  x->tx = o->tr_x;
  x->ty = o->tr_y;
  x->txp = o->tr_xp;
  x->typ = o->tr_yp;
}

/* One transform function by name with its arguments, as an affine map; false for functions this does not support. */
static bool xf_func(const char *name, char arg[][40], int n, struct xf *x)
{
  int px, pct, deg;
  xf_identity(x);
  if (!strcmp(name, "translate") || !strcmp(name, "translate3d") || !strcmp(name, "translatex") ||
      !strcmp(name, "translatey")) {
    int axis = !strcmp(name, "translatey") ? 1 : 0;
    for (int i = 0; i < n && i < 2; i++) {
      if (!parse_slen(arg[i], &px, &pct))
        return false;
      if (axis + i == 0) {
        x->tx = px;
        x->txp = pct;
      } else {
        x->ty = px;
        x->typ = pct;
      }
    }
    return n > 0;
  }
  if (starts_ci(name, "scale")) {
    int kx = 1000, ky = 1000;
    if (n > 0 && parse_num_pct(arg[0], &kx))
      ky = kx;
    if (n > 1)
      parse_num_pct(arg[1], &ky);
    if (!strcmp(name, "scalex"))
      ky = 1000;
    else if (!strcmp(name, "scaley")) {
      ky = kx;
      kx = 1000;
    }
    x->a = kx / 1000.0;
    x->d = ky / 1000.0;
    return n > 0;
  }
  if ((!strcmp(name, "rotate") || !strcmp(name, "rotatez")) && n > 0 && parse_angle(arg[0], &deg)) {
    double r = deg * 0.017453292519943295;
    x->a = x->d = cos(r);
    x->b = sin(r);
    x->c = -sin(r);
    return true;
  }
  if (starts_ci(name, "skew")) {
    int ax = 0, ay = 0;
    if (n > 0)
      parse_angle(arg[0], strcmp(name, "skewy") ? &ax : &ay);
    if (n > 1)
      parse_angle(arg[1], &ay);
    x->c = tan(ax * 0.017453292519943295);
    x->b = tan(ay * 0.017453292519943295);
    return true;
  }
  if (!strcmp(name, "matrix") && n >= 6) {
    int m[6] = {0};
    for (int i = 0; i < 6; i++) {
      const char *p = arg[i];
      double v;
      if (!calc_number(&p, &v))
        return false;
      m[i] = (int)(v * 4096.0 + (v < 0 ? -0.5 : 0.5));
    }
    x->a = m[0] / 4096.0;
    x->b = m[1] / 4096.0;
    x->c = m[2] / 4096.0;
    x->d = m[3] / 4096.0;
    x->tx = m[4] / 4096.0;
    x->ty = m[5] / 4096.0;
    return true;
  }
  return false;
}

/* `transform` list (or one of translate/rotate/scale, which compose onto what is already set when `reset` is false). */
static void parse_transform(const char *v, struct css_style *o, bool reset)
{
  struct xf m;
  if (reset)
    xf_identity(&m);
  else
    xf_load(o, &m);
  if (!starts_ci(skip_ws(v), "none"))
    for (const char *s = v; *s;) {
      s = skip_ws(s);
      char name[16];
      int nl = fn_name(s, name, sizeof name);
      if (!nl)
        break;
      const char *as = s + nl + 1, *ae = fn_args_end(as);
      char arg[6][40];
      int n = 0;
      for (const char *q = as; n < 6 && (q = next_arg(q, ae, arg[n], sizeof arg[n])); n++)
        ;
      struct xf f;
      if (xf_func(name, arg, n, &f))
        xf_mul(&m, &f);
      s = *ae ? ae + 1 : ae;
    }
  xf_store(o, &m);
}

/* transform-origin: `x [y]`, each a keyword, length or percentage. */
static void parse_origin(const char *v, struct css_style *o)
{
  char tok[2][40];
  int n = 0;
  for (const char *q = v; n < 2 && (q = next_arg(q, q + strlen(q), tok[n], sizeof tok[n])); n++)
    ;
  if (n == 0)
    return;
  short px[2] = {0, 0}, pct[2] = {50, 50};
  bool xy_known[2] = {false, false};
  for (int i = 0; i < n; i++) {
    int slot = i; /* the first value is the horizontal one unless it names the vertical axis */
    bool vert_kw = starts_ci(tok[i], "top") || starts_ci(tok[i], "bottom");
    bool horz_kw = starts_ci(tok[i], "left") || starts_ci(tok[i], "right");
    if (vert_kw)
      slot = 1;
    else if (horz_kw)
      slot = 0;
    else if (i == 1 && xy_known[0] == false)
      slot = 0;
    short p = 0, c = 50;
    int a, b;
    if (starts_ci(tok[i], "left") || starts_ci(tok[i], "top"))
      c = 0;
    else if (starts_ci(tok[i], "right") || starts_ci(tok[i], "bottom"))
      c = 100;
    else if (starts_ci(tok[i], "center"))
      c = 50;
    else if (parse_slen(tok[i], &a, &b)) {
      p = clamp16(a);
      c = clamp16(b);
    }
    px[slot] = p;
    pct[slot] = c;
    xy_known[slot] = true;
  }
  o->tr_ox = px[0];
  o->tr_oy = px[1];
  o->tr_oxp = pct[0];
  o->tr_oyp = pct[1];
  o->tr_org = 1;
}

/* ---- filter ---- */

static void parse_filter(const char *v, struct css_style *o)
{
  o->flt_bright = 1000;
  o->flt_gray = o->flt_invert = 0;
  o->flt_opacity = 100;
  for (const char *s = v; *s;) {
    s = skip_ws(s);
    char name[16];
    int nl = fn_name(s, name, sizeof name);
    if (!nl)
      break;
    const char *as = s + nl + 1, *ae = fn_args_end(as);
    char arg[40];
    next_arg(as, ae, arg, sizeof arg);
    int k;
    if (parse_num_pct(*arg ? arg : "1", &k)) {
      if (!strcmp(name, "brightness"))
        o->flt_bright = clamp16(k < 0 ? 0 : k);
      else if (!strcmp(name, "grayscale"))
        o->flt_gray = (signed char)(k > 1000 ? 100 : (k < 0 ? 0 : k / 10));
      else if (!strcmp(name, "invert"))
        o->flt_invert = (signed char)(k > 1000 ? 100 : (k < 0 ? 0 : k / 10));
      else if (!strcmp(name, "opacity"))
        o->flt_opacity = (signed char)(k > 1000 ? 100 : (k < 0 ? 0 : k / 10));
    }
    s = *ae ? ae + 1 : ae;
  }
}

/* backdrop-filter: the blur() radius in px (0 for none); other functions aren't painted. */
static void parse_backdrop_filter(const char *v, struct css_style *o)
{
  o->bdf_blur = 0;
  for (const char *s = v; *s;) {
    s = skip_ws(s);
    char name[16];
    int nl = fn_name(s, name, sizeof name);
    if (!nl)
      break;
    const char *as = s + nl + 1, *ae = fn_args_end(as);
    char arg[40];
    next_arg(as, ae, arg, sizeof arg);
    if (!strcmp(name, "blur")) {
      int px = *arg ? one_len(arg, 0) : 0;
      o->bdf_blur = (short)(px < 0 ? 0 : px > 64 ? 64 : px);
    }
    s = *ae ? ae + 1 : ae;
  }
}

/* ---- clip-path ---- */

static void clip_push(struct css_style *o, const char *arg)
{
  if (o->clip_n >= CSS_CLIP_PTS * 2)
    return;
  int px = 0, pct = 0;
  int i = o->clip_n++;
  o->clip_unit[i] = 0;
  o->clip_v[i] = 0;
  if (starts_ci(arg, "closest-side"))
    o->clip_unit[i] = 2;
  else if (starts_ci(arg, "farthest-side"))
    o->clip_unit[i] = 3;
  else if (starts_ci(arg, "left") || starts_ci(arg, "top")) {
    o->clip_unit[i] = 1;
  } else if (starts_ci(arg, "center")) {
    o->clip_unit[i] = 1;
    o->clip_v[i] = 500;
  } else if (starts_ci(arg, "right") || starts_ci(arg, "bottom")) {
    o->clip_unit[i] = 1;
    o->clip_v[i] = 1000;
  } else if (parse_slen(arg, &px, &pct)) {
    if (pct) {
      o->clip_unit[i] = 1;
      o->clip_v[i] = clamp16(pct * 10);
    } else
      o->clip_v[i] = clamp16(px);
  }
}

/* Reset the clip and fill it from `shape(args)`; leaves clip_kind unset (none) when the shape is unusable. */
static void parse_clip_path(const char *v, struct css_style *o)
{
  o->clip_kind = CSS_CLIP_NONE;
  o->clip_n = 0;
  char name[16];
  const char *s = skip_ws(v);
  int nl = fn_name(s, name, sizeof name);
  if (!nl)
    return;
  const char *as = s + nl + 1, *ae = fn_args_end(as);
  char arg[40];
  if (!strcmp(name, "inset")) {
    o->clip_kind = CSS_CLIP_INSET;
    char vals[4][40];
    int n = 0;
    const char *q = as;
    bool round = false;
    while (n < 4 && (q = next_arg(q, ae, arg, sizeof arg))) {
      if (starts_ci(arg, "round")) {
        round = true;
        break;
      }
      memcpy(vals[n++], arg, sizeof arg);
    }
    if (!n) {
      o->clip_kind = CSS_CLIP_NONE;
      return;
    }
    static const signed char pick[5][4] = {{0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    for (int i = 0; i < 4; i++) /* top right bottom left */
      clip_push(o, vals[pick[n][i]]);
    if (round && (q = next_arg(q, ae, arg, sizeof arg)))
      clip_push(o, arg);
    else
      clip_push(o, "0");
    return;
  }
  if (!strcmp(name, "circle") || !strcmp(name, "ellipse")) {
    bool circle = name[0] == 'c';
    o->clip_kind = circle ? CSS_CLIP_CIRCLE : CSS_CLIP_ELLIPSE;
    int nr = circle ? 1 : 2, n = 0;
    const char *q = as;
    bool at = false;
    char radii[2][40] = {"closest-side", "closest-side"};
    char pos[2][40] = {"50%", "50%"};
    int np = 0;
    while ((q = next_arg(q, ae, arg, sizeof arg))) {
      if (starts_ci(arg, "at")) {
        at = true;
        continue;
      }
      if (at) {
        if (np < 2)
          memcpy(pos[np++], arg, sizeof arg);
      } else if (n < nr)
        memcpy(radii[n++], arg, sizeof arg);
    }
    for (int i = 0; i < nr; i++)
      clip_push(o, radii[i]);
    clip_push(o, pos[0]);
    clip_push(o, np > 1 ? pos[1] : "50%");
    return;
  }
  if (!strcmp(name, "polygon")) {
    o->clip_kind = CSS_CLIP_POLYGON;
    const char *q = as;
    while ((q = next_arg(q, ae, arg, sizeof arg))) {
      if (starts_ci(arg, "nonzero") || starts_ci(arg, "evenodd"))
        continue;
      clip_push(o, arg);
    }
    if (o->clip_n < 6 || (o->clip_n & 1)) { /* needs at least a triangle of whole points */
      o->clip_kind = CSS_CLIP_NONE;
      o->clip_n = 0;
    }
  }
}

/* ---- grid ---- */

/* grid-template-areas strings into o->areas; a run of dots is one empty cell and also ends a name (for minified
 * values), returns the number of rows. */
static int parse_area_strings(const char *s, struct css_style *o)
{
  o->n_areas = 0;
  int row = 0;
  while (*s) {
    while (*s && *s != '"' && *s != '\'')
      s++;
    if (!*s)
      break;
    char q = *s++;
    int col = 0;
    while (*s && *s != q) {
      if (*s == ' ' || *s == '\t') {
        s++;
        continue;
      }
      if (*s == '.') {
        while (*s == '.')
          s++;
        col++;
        continue;
      }
      char name[CSS_AREA_NAME];
      int k = 0;
      while (*s && *s != q && *s != ' ' && *s != '\t' && *s != '.') {
        if (k < CSS_AREA_NAME - 1)
          name[k++] = *s;
        s++;
      }
      name[k] = '\0';
      int i = 0;
      while (i < o->n_areas && strcmp(o->areas[i].name, name))
        i++;
      if (i == o->n_areas && o->n_areas < CSS_AREA_MAX) {
        memcpy(o->areas[i].name, name, sizeof name);
        o->areas[i].c0 = (signed char)col;
        o->areas[i].r0 = (signed char)row;
        o->areas[i].c1 = (signed char)col;
        o->areas[i].r1 = (signed char)row;
        o->n_areas++;
      }
      if (i < o->n_areas) {
        if (col + 1 > o->areas[i].c1)
          o->areas[i].c1 = (signed char)(col + 1);
        if (row + 1 > o->areas[i].r1)
          o->areas[i].r1 = (signed char)(row + 1);
      }
      col++;
    }
    if (*s == q)
      s++;
    row++;
  }
  return row;
}

/* One placement line value: `N`, `span N`, `auto`. Sets *line (1-based, 0 auto, -1 = last line) and *span (0 none). */
static bool parse_line(const char *v, int *line, int *span)
{
  v = skip_ws(v);
  *line = 0;
  *span = 0;
  if (starts_ci(v, "auto"))
    return true;
  if (starts_ci(v, "span")) {
    v = skip_ws(v + 4);
    double n;
    if (!calc_number(&v, &n) || n < 1)
      return false;
    *span = (int)n;
    return true;
  }
  double n;
  if (!calc_number(&v, &n) || n == 0)
    return false;
  *line = (int)n;
  return true;
}

/* grid-column / grid-row value `start [/ end]` into a 1-based start (0 auto) and span. */
static bool parse_axis(const char *v, int *start, int *span)
{
  char sbuf[48], ebuf[48];
  const char *sl = v;
  while (*sl && *sl != '/')
    sl++;
  int k = (int)(sl - v) < (int)sizeof sbuf - 1 ? (int)(sl - v) : (int)sizeof sbuf - 1;
  memcpy(sbuf, v, (unsigned)k);
  sbuf[k] = '\0';
  ebuf[0] = '\0';
  if (*sl == '/') {
    int e = 0;
    for (const char *t = sl + 1; *t && e < (int)sizeof ebuf - 1;)
      ebuf[e++] = *t++;
    ebuf[e] = '\0';
  }
  int s0, s1, e0 = 0, e1 = 0;
  if (!parse_line(sbuf, &s0, &s1))
    return false;
  bool has_end = *sl == '/';
  if (has_end && !parse_line(ebuf, &e0, &e1))
    return false;
  *start = s0 > 0 ? s0 : 0;
  *span = 1;
  if (s1)
    *span = s1;
  if (has_end) {
    if (e1)
      *span = e1;
    else if (e0 < 0)
      *span = CSS_GRID_MAX; /* up to the last line */
    else if (e0 > 0 && s0 > 0 && e0 > s0)
      *span = e0 - s0;
  }
  return true;
}

static void set_axis(const char *val, short *start, signed char *span)
{
  int st, sp;
  if (!parse_axis(val, &st, &sp))
    return;
  *start = (short)st;
  *span = (signed char)(sp > CSS_GRID_MAX ? CSS_GRID_MAX : sp);
}

/* `grid-area`: a name, or `row-start / col-start / row-end / col-end`. */
static void parse_grid_area(const char *v, struct css_style *o)
{
  char part[4][40];
  int n = 0;
  const char *s = v;
  while (n < 4) {
    const char *sl = s;
    while (*sl && *sl != '/')
      sl++;
    int k = (int)(sl - s) < 39 ? (int)(sl - s) : 39;
    memcpy(part[n], s, (unsigned)k);
    part[n][k] = '\0';
    n++;
    if (*sl != '/')
      break;
    s = sl + 1;
  }
  const char *p0 = skip_ws(part[0]);
  if (n == 1 && !(*p0 >= '0' && *p0 <= '9') && *p0 != '-' && *p0 != '+' && !starts_ci(p0, "span") &&
      !starts_ci(p0, "auto")) {
    int k = 0;
    while (*p0 && *p0 != ' ' && *p0 != '\t' && k < CSS_AREA_NAME - 1)
      o->area[k++] = *p0++;
    o->area[k] = '\0';
    return;
  }
  char row[96], col[96];
  snprintf(row, sizeof row, "%s", part[0]);
  col[0] = '\0';
  if (n >= 2)
    snprintf(col, sizeof col, "%s", part[1]);
  if (n >= 3) {
    snprintf(row, sizeof row, "%s/%s", part[0], part[2]);
    if (n >= 4)
      snprintf(col, sizeof col, "%s/%s", part[1], part[3]);
  }
  set_axis(row, &o->row_start, &o->row_span);
  if (col[0])
    set_axis(col, &o->col_start, &o->col_span);
}

/* `grid-template: [<areas and row sizes>|<rows>] / <columns>`. */
static void parse_grid_template(const char *v, struct css_style *o)
{
  const char *slash = 0;
  char q = 0;
  for (const char *s = v; *s; s++) {
    if (q) {
      if (*s == q)
        q = 0;
    } else if (*s == '"' || *s == '\'')
      q = *s;
    else if (*s == '/') {
      slash = s;
      break;
    }
  }
  char left[192];
  unsigned ll = slash ? (unsigned)(slash - v) : strlen(v);
  if (ll >= sizeof left)
    ll = sizeof left - 1;
  memcpy(left, v, ll);
  left[ll] = '\0';
  if (slash)
    o->n_cols = (signed char)parse_tracks(slash + 1, o->grid_cols, CSS_GRID_MAX);
  if (!strchr(left, '"') && !strchr(left, '\'')) {
    o->n_rows = (signed char)parse_tracks(left, o->grid_rows, CSS_GRID_MAX);
    return;
  }
  int nrows = parse_area_strings(left, o);
  /* a track size may follow each string; rows without one are auto */
  int r = 0;
  for (const char *s = left; *s && r < CSS_GRID_MAX;) {
    if (*s == '"' || *s == '\'') {
      char c = *s++;
      while (*s && *s != c)
        s++;
      if (*s)
        s++;
      char tok[40];
      const char *t = skip_ws(s);
      const char *e = t;
      while (*e && *e != '"' && *e != '\'')
        e++;
      int k = (int)(e - t) < 39 ? (int)(e - t) : 39;
      memcpy(tok, t, (unsigned)k);
      tok[k] = '\0';
      struct css_track trk[CSS_GRID_MAX];
      o->grid_rows[r].kind = CSS_TRK_AUTO;
      o->grid_rows[r].v = 0;
      if (parse_tracks(tok, trk, 1) == 1)
        o->grid_rows[r] = trk[0];
      r++;
      s = e;
    } else
      s++;
  }
  o->n_rows = (signed char)(nrows < CSS_GRID_MAX ? nrows : CSS_GRID_MAX);
}

/* ---- dispatch ---- */

static bool parse_aspect(const char *v, int *out)
{
  v = skip_ws(v);
  bool autoo = starts_ci(v, "auto");
  if (autoo)
    v = skip_ws(v + 4);
  if (!*v) {
    *out = 0;
    return autoo;
  }
  double a, b = 1;
  if (!calc_number(&v, &a) || a <= 0)
    return false;
  v = skip_ws(v);
  if (*v == '/') {
    v = skip_ws(v + 1);
    if (!calc_number(&v, &b) || b <= 0)
      return false;
  }
  *out = (int)(a * 1000.0 / b + 0.5);
  return true;
}

/* First recognised keyword of a cursor value (a url() list falls through to its keyword fallback). */
static signed char parse_cursor(const char *v)
{
  static const struct {
    const char *kw;
    signed char cur;
  } map[] = {{"pointer", CSS_CUR_POINTER},     {"grab", CSS_CUR_POINTER},        {"grabbing", CSS_CUR_MOVE},
             {"text", CSS_CUR_TEXT},           {"vertical-text", CSS_CUR_TEXT},  {"wait", CSS_CUR_WAIT},
             {"progress", CSS_CUR_WAIT},       {"nwse-resize", CSS_CUR_NWSE},    {"nw-resize", CSS_CUR_NWSE},
             {"se-resize", CSS_CUR_NWSE},      {"nesw-resize", CSS_CUR_NESW},    {"ne-resize", CSS_CUR_NESW},
             {"sw-resize", CSS_CUR_NESW},      {"ns-resize", CSS_CUR_NS},        {"n-resize", CSS_CUR_NS},
             {"s-resize", CSS_CUR_NS},         {"row-resize", CSS_CUR_NS},       {"ew-resize", CSS_CUR_EW},
             {"e-resize", CSS_CUR_EW},         {"w-resize", CSS_CUR_EW},         {"col-resize", CSS_CUR_EW},
             {"move", CSS_CUR_MOVE},           {"all-scroll", CSS_CUR_MOVE},     {"not-allowed", CSS_CUR_NOT_ALLOWED},
             {"no-drop", CSS_CUR_NOT_ALLOWED}, {"crosshair", CSS_CUR_CROSSHAIR}, {"cell", CSS_CUR_CROSSHAIR},
             {"default", CSS_CUR_DEFAULT},     {"help", CSS_CUR_DEFAULT},        {"context-menu", CSS_CUR_DEFAULT},
             {"alias", CSS_CUR_POINTER},       {"copy", CSS_CUR_POINTER},        {"auto", CSS_CUR_AUTO},
             {"none", CSS_CUR_DEFAULT}};
  while (*v) {
    v = skip_ws(v);
    const char *e = v;
    int depth = 0;
    while (*e && (depth || (*e != ',' && *e != ' ' && *e != '\t'))) {
      depth += *e == '(';
      depth -= *e == ')';
      e++;
    }
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
      unsigned n = 0;
      while (map[i].kw[n])
        n++;
      if ((unsigned)(e - v) == n && span_is(v, e, map[i].kw))
        return map[i].cur;
    }
    v = *e ? e + 1 : e;
  }
  return -1;
}

bool parse_ext_decl(const char *p, const char *pe, const char *val, struct css_style *out)
{
  int n;
  if (span_is(p, pe, "align-self") || span_is(p, pe, "place-self")) {
    out->align_self = has_ci(val, "auto") ? CSS_ALIGN_AUTO : parse_align(val);
  } else if (span_is(p, pe, "aspect-ratio")) {
    if (parse_aspect(val, &n))
      out->aspect = n;
  } else if (span_is(p, pe, "visibility")) {
    if (has_ci(val, "hidden") || has_ci(val, "collapse"))
      out->visibility = 0;
    else if (has_ci(val, "visible"))
      out->visibility = 1;
  } else if (span_is(p, pe, "text-overflow")) {
    out->text_ovf = has_ci(val, "ellipsis") ? 1 : has_ci(val, "clip") ? 0 : out->text_ovf;
  } else if (span_is(p, pe, "object-fit")) {
    out->obj_fit = has_ci(val, "scale-down") ? CSS_OF_SCALE_DOWN
                   : has_ci(val, "contain")  ? CSS_OF_CONTAIN
                   : has_ci(val, "cover")    ? CSS_OF_COVER
                   : has_ci(val, "none")     ? CSS_OF_NONE
                   : has_ci(val, "fill")     ? CSS_OF_FILL
                                             : out->obj_fit;
  } else if (span_is(p, pe, "object-position")) {
    parse_bg_position(val, &out->obj_pos_x, &out->obj_pos_y);
  } else if (span_is(p, pe, "pointer-events")) {
    out->pointer_events = has_ci(val, "none") ? 0 : 1;
  } else if (span_is(p, pe, "user-select") || span_is(p, pe, "-webkit-user-select") ||
             span_is(p, pe, "-moz-user-select")) {
    out->user_select = has_ci(val, "none") ? 0 : 1;
  } else if (span_is(p, pe, "letter-spacing")) {
    int px, pct;
    if (has_ci(val, "normal"))
      out->letter_sp = 0;
    else if (parse_slen(val, &px, &pct) && !pct)
      out->letter_sp = clamp16(px);
  } else if (span_is(p, pe, "vertical-align")) {
    int px, pct;
    out->va_kind = has_ci(val, "sub")        ? CSS_VA_SUB
                   : has_ci(val, "super")    ? CSS_VA_SUPER
                   : has_ci(val, "middle")   ? CSS_VA_MIDDLE
                   : has_ci(val, "baseline") ? CSS_VA_BASELINE
                                             : out->va_kind;
    if (parse_slen(val, &px, &pct) && !pct) {
      out->va_kind = CSS_VA_LEN;
      out->va_len = clamp16(px);
    }
  } else if (span_is(p, pe, "scrollbar-width")) {
    out->sb_w = has_ci(val, "none") ? 0 : has_ci(val, "thin") ? 6 : -2;
  } else if (span_is(p, pe, "scrollbar-color")) {
    char tok[2][40];
    int n = 0;
    for (const char *q = val; n < 2 && (q = next_arg(q, q + strlen(q), tok[n], sizeof tok[n])); n++)
      ;
    int thumb = n > 0 ? parse_color(tok[0]) : -1, track = n > 1 ? parse_color(tok[1]) : -1;
    if (n == 2 && thumb >= 0 && track >= 0) {
      out->sb_thumb = thumb;
      out->sb_track = track;
    } else if (has_ci(val, "auto")) {
      out->sb_thumb = out->sb_track = -2;
    }
  } else if (span_is(p, pe, "tab-size") || span_is(p, pe, "-moz-tab-size")) {
    const char *f = val;
    double n;
    if (calc_number(&f, &n) && n >= 0 && n <= 16)
      out->tab_size = (signed char)n;
  } else if (span_is(p, pe, "cursor")) {
    out->cursor = parse_cursor(val);
  } else if (span_is(p, pe, "grid-template-areas")) {
    if (has_ci(val, "none"))
      out->n_areas = 0;
    else
      parse_area_strings(val, out);
  } else if (span_is(p, pe, "grid-template")) {
    parse_grid_template(val, out);
  } else if (span_is(p, pe, "grid-area")) {
    parse_grid_area(val, out);
  } else if (span_is(p, pe, "grid-column") || span_is(p, pe, "grid-column-start") ||
             span_is(p, pe, "grid-column-end")) {
    if (span_is(p, pe, "grid-column"))
      set_axis(val, &out->col_start, &out->col_span);
    else {
      bool end = span_is(p, pe, "grid-column-end");
      int line, span;
      if (parse_line(val, &line, &span)) {
        if (end && line > out->col_start && out->col_start > 0)
          out->col_span = (signed char)(line - out->col_start);
        else if (end && line < 0)
          out->col_span = CSS_GRID_MAX;
        else if (!end && line > 0)
          out->col_start = (short)line;
        if (span)
          out->col_span = (signed char)(span > CSS_GRID_MAX ? CSS_GRID_MAX : span);
      }
    }
  } else if (span_is(p, pe, "grid-row") || span_is(p, pe, "grid-row-start") || span_is(p, pe, "grid-row-end")) {
    if (span_is(p, pe, "grid-row"))
      set_axis(val, &out->row_start, &out->row_span);
    else {
      bool end = span_is(p, pe, "grid-row-end");
      int line, span;
      if (parse_line(val, &line, &span)) {
        if (end && line > out->row_start && out->row_start > 0)
          out->row_span = (signed char)(line - out->row_start);
        else if (end && line < 0)
          out->row_span = CSS_GRID_MAX;
        else if (!end && line > 0)
          out->row_start = (short)line;
        if (span)
          out->row_span = (signed char)(span > CSS_GRID_MAX ? CSS_GRID_MAX : span);
      }
    }
  } else if (span_is(p, pe, "transform") || span_is(p, pe, "-webkit-transform")) {
    parse_transform(val, out, true);
  } else if (span_is(p, pe, "transform-origin")) {
    parse_origin(val, out);
  } else if (span_is(p, pe, "translate")) {
    char buf[96];
    snprintf(buf, sizeof buf, "translate(%s)", val);
    for (char *c = buf + 10; *c; c++)
      if (*c == ' ')
        *c = ',';
    parse_transform(buf, out, false);
  } else if (span_is(p, pe, "scale")) {
    char buf[96];
    snprintf(buf, sizeof buf, "scale(%s)", val);
    for (char *c = buf + 6; *c; c++)
      if (*c == ' ')
        *c = ',';
    parse_transform(buf, out, false);
  } else if (span_is(p, pe, "rotate")) {
    char buf[96];
    snprintf(buf, sizeof buf, "rotate(%s)", val);
    parse_transform(buf, out, false);
  } else if (span_is(p, pe, "filter") || span_is(p, pe, "-webkit-filter")) {
    parse_filter(val, out);
  } else if (span_is(p, pe, "backdrop-filter") || span_is(p, pe, "-webkit-backdrop-filter")) {
    parse_backdrop_filter(val, out);
  } else if (span_is(p, pe, "clip-path") || span_is(p, pe, "-webkit-clip-path")) {
    parse_clip_path(val, out);
  } else
    return false;
  return true;
}
