#include "engine_int.h"

/* value parsing */
/* Colour parsing lives in color.c now (shared with svg.c). */
int parse_color(const char *v) { return color_parse(v); }

/* calc()/min()/max()/clamp(): values are linear a*px + b*%, % kept separate (viewport-relative in min/max/clamp). */
struct calcval {
  double px, pct, num;
  bool dimless;
};
struct calcctx {
  const char *p, *e;
  bool ok;
  int depth;
};

static void calc_skip(struct calcctx *c)
{
  while (c->p < c->e && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
    c->p++;
}

/* Parse a signed decimal at *pp into *out; 1 if a digit was consumed. */
bool calc_number(const char **pp, double *out)
{
  const char *p = *pp;
  bool neg = false;
  if (*p == '-') {
    neg = true;
    p++;
  } else if (*p == '+')
    p++;
  double n = 0;
  bool any = false;
  while (*p >= '0' && *p <= '9') {
    n = n * 10 + (*p++ - '0');
    any = true;
  }
  if (*p == '.') {
    p++;
    double f = 0.1;
    while (*p >= '0' && *p <= '9') {
      n += (*p++ - '0') * f;
      f *= 0.1;
      any = true;
    }
  }
  if (!any)
    return false;
  *out = neg ? -n : n;
  *pp = p;
  return true;
}

/* A number + unit -> calcval; unknown units become a dimensionless number. */
static void unit_value(double n, const char *u, struct calcval *o)
{
  o->px = o->pct = o->num = 0;
  o->dimless = false;
  if (u[0] == '%')
    o->pct = n;
  else if (!u[0]) {
    o->dimless = true;
    o->num = n;
  } else if (starts_ci(u, "px"))
    o->px = n;
  else if (starts_ci(u, "rem"))
    o->px = n * CSS_ROOT_FONT_PX;
  else if (starts_ci(u, "em"))
    o->px = n * CSS_ROOT_FONT_PX;
  else if (starts_ci(u, "ex"))
    o->px = n * (CSS_ROOT_FONT_PX / 2.0);
  else if (starts_ci(u, "ch"))
    o->px = n * (CSS_ROOT_FONT_PX / 2.0);
  else if (starts_ci(u, "vmin"))
    o->px = n * (g_vw < g_vh ? g_vw : g_vh) / 100.0;
  else if (starts_ci(u, "vmax"))
    o->px = n * (g_vw > g_vh ? g_vw : g_vh) / 100.0;
  else if (starts_ci(u, "vw"))
    o->px = n * g_vw / 100.0;
  else if (starts_ci(u, "vh"))
    o->px = n * g_vh / 100.0;
  else if (starts_ci(u, "cm"))
    o->px = n * 96.0 / 2.54;
  else if (starts_ci(u, "mm"))
    o->px = n * 96.0 / 25.4;
  else if (starts_ci(u, "in"))
    o->px = n * 96.0;
  else if (starts_ci(u, "pt"))
    o->px = n * 96.0 / 72.0;
  else if (starts_ci(u, "pc"))
    o->px = n * 16.0;
  else {
    o->dimless = true;
    o->num = n;
  }
}

/* Length magnitude for min()/max()/clamp() comparisons; % is measured against viewport width. */
static double calc_mag(const struct calcval *v)
{
  if (v->dimless)
    return v->num;
  double ref = g_vw > 0 ? g_vw : 800;
  return v->px + v->pct * 0.01 * ref;
}

static struct calcval calc_expr(struct calcctx *c); /* additive, the entry point */

/* Read comma-separated arguments up to and past the closing ')'. */
static int calc_args(struct calcctx *c, struct calcval *out, int maxn)
{
  int n = 0;
  for (;;) {
    struct calcval v = calc_expr(c);
    if (n < maxn)
      out[n++] = v;
    calc_skip(c);
    if (c->p < c->e && *c->p == ',') {
      c->p++;
      continue;
    }
    break;
  }
  calc_skip(c);
  if (c->p < c->e && *c->p == ')')
    c->p++;
  else
    c->ok = false;
  return n;
}

static struct calcval calc_factor(struct calcctx *c)
{
  struct calcval v = {0, 0, 0, 0};
  calc_skip(c);
  if (c->p >= c->e || c->depth > 24) {
    c->ok = false;
    return v;
  }
  if (*c->p == '(') {
    c->p++;
    c->depth++;
    v = calc_expr(c);
    c->depth--;
    calc_skip(c);
    if (c->p < c->e && *c->p == ')')
      c->p++;
    else
      c->ok = false;
    return v;
  }
  int fn = 0; /* 1 calc, 2 min, 3 max, 4 clamp */
  if (starts_ci(c->p, "calc(")) {
    fn = 1;
    c->p += 5;
  } else if (starts_ci(c->p, "min(")) {
    fn = 2;
    c->p += 4;
  } else if (starts_ci(c->p, "max(")) {
    fn = 3;
    c->p += 4;
  } else if (starts_ci(c->p, "clamp(")) {
    fn = 4;
    c->p += 6;
  }
  if (fn) {
    struct calcval args[8];
    c->depth++;
    int n = calc_args(c, args, 8);
    c->depth--;
    if (fn == 1)
      return n ? args[0] : v;
    if (fn == 4) { /* clamp(MIN, VAL, MAX) */
      if (n < 3)
        return n ? args[0] : v;
      double lo = calc_mag(&args[0]), mid = calc_mag(&args[1]), hi = calc_mag(&args[2]);
      return mid < lo ? args[0] : (mid > hi ? args[2] : args[1]);
    }
    if (!n)
      return v; /* min / max: pick the winning argument */
    int best = 0;
    for (int i = 1; i < n; i++) {
      double m = calc_mag(&args[i]), bm = calc_mag(&args[best]);
      if ((fn == 2 && m < bm) || (fn == 3 && m > bm))
        best = i;
    }
    return args[best];
  }
  double num;
  if (!calc_number(&c->p, &num)) {
    c->ok = false;
    return v;
  }
  const char *us = c->p;
  while (c->p < c->e && (((*c->p | 32) >= 'a' && (*c->p | 32) <= 'z') || *c->p == '%'))
    c->p++;
  char unit[8];
  int k = 0;
  for (const char *q = us; q < c->p && k < 7; q++)
    unit[k++] = *q;
  unit[k] = '\0';
  unit_value(num, unit, &v);
  return v;
}

static struct calcval calc_term(struct calcctx *c)
{
  struct calcval a = calc_factor(c);
  for (;;) {
    calc_skip(c);
    if (c->p >= c->e || (*c->p != '*' && *c->p != '/'))
      break;
    char op = *c->p++;
    struct calcval b = calc_factor(c);
    if (op == '*') {
      if (a.dimless) {
        double k = a.num;
        a = b;
        a.px *= k;
        a.pct *= k;
        a.num *= k;
      } else if (b.dimless) {
        double k = b.num;
        a.px *= k;
        a.pct *= k;
        a.num *= k;
      } else
        c->ok = false; /* length * length is invalid */
    } else {
      if (b.dimless && b.num != 0) {
        double k = 1.0 / b.num;
        a.px *= k;
        a.pct *= k;
        a.num *= k;
      } else
        c->ok = false;
    }
  }
  return a;
}

static struct calcval calc_expr(struct calcctx *c)
{
  struct calcval a = calc_term(c);
  for (;;) {
    calc_skip(c);
    if (c->p >= c->e || (*c->p != '+' && *c->p != '-'))
      break;
    char op = *c->p++;
    struct calcval b = calc_term(c);
    if (a.dimless != b.dimless)
      c->ok = false;
    if (op == '+') {
      a.px += b.px;
      a.pct += b.pct;
      a.num += b.num;
    } else {
      a.px -= b.px;
      a.pct -= b.pct;
      a.num -= b.num;
    }
  }
  return a;
}

/* Map a calcval to parse_len2's form: px>=0 via return, else percent via *pct (mixed keeps only %). */
static int calc_result(const struct calcval *r, bool ok, int *pct)
{
  if (!ok)
    return -1;
  if (r->dimless) {
    int n = (int)(r->num + (r->num < 0 ? -0.5 : 0.5));
    return n >= 0 ? n : -1;
  }
  if (r->pct != 0) {
    int p = (int)(r->pct + 0.5);
    if (p >= 0)
      *pct = p;
    return -1;
  }
  int n = (int)(r->px + 0.5);
  return n >= 0 ? n : -1;
}

/* Parse a CSS length: px>=0 (units and calc() resolved), or -1 with *pct = N for %, else *pct = -1. */
int parse_len2(const char *v, int *pct)
{
  *pct = -1;
  while (*v == ' ' || *v == '\t')
    v++;
  struct calcctx c = {v, v + strlen(v), 1, 0};
  struct calcval r = calc_factor(&c); /* one length token, or a calc/min/max/clamp */
  return calc_result(&r, c.ok, pct);
}

/* Parse a CSS length: integer px (or unitless), else -1 (%, em, auto, calc...). */
int parse_len(const char *v)
{
  int pct;
  return parse_len2(v, &pct);
}

/* One box-side length: px>=0, -2 for auto (margins only), else -1. */
short one_len(const char *v, int allow_auto)
{
  while (*v == ' ' || *v == '\t')
    v++;
  if (allow_auto && starts_ci(v, "auto"))
    return -2;
  int pct, px = parse_len2(v, &pct);
  return (short)(px >= 0 ? px : -1);
}

/* Expand a 1-4 value box shorthand ("8px 16px") into out[4] = {T,R,B,L}. */
void parse_box4(const char *v, short out[4], int allow_auto)
{
  short vals[4];
  int n = 0;
  while (*v && n < 4) {
    while (*v == ' ' || *v == '\t')
      v++;
    if (!*v)
      break;
    vals[n++] = one_len(v, allow_auto);
    while (*v && *v != ' ' && *v != '\t')
      v++;
  }
  if (n == 0)
    return;
  short t, r, b, l;
  if (n == 1) {
    t = r = b = l = vals[0];
  } else if (n == 2) {
    t = b = vals[0];
    r = l = vals[1];
  } else if (n == 3) {
    t = vals[0];
    r = l = vals[1];
    b = vals[2];
  } else {
    t = vals[0];
    r = vals[1];
    b = vals[2];
    l = vals[3];
  }
  out[0] = t;
  out[1] = r;
  out[2] = b;
  out[3] = l;
}

/* `border` shorthand: width (length, thin/medium/thick, none/hidden = 0) and colour; -1 if absent. */
void parse_border_val(const char *v, int *w, int *col)
{
  *w = -1;
  *col = -1;
  while (*v) {
    while (*v == ' ' || *v == '\t')
      v++;
    if (!*v)
      break;
    char tok[64];
    int k = 0;
    while (v[k] && v[k] != ' ' && v[k] != '\t' && k < (int)sizeof tok - 1) {
      tok[k] = v[k];
      k++;
    }
    tok[k] = '\0';
    if (starts_ci(tok, "thin"))
      *w = 1;
    else if (starts_ci(tok, "medium"))
      *w = 3;
    else if (starts_ci(tok, "thick"))
      *w = 5;
    else if (starts_ci(tok, "none") || starts_ci(tok, "hidden"))
      *w = 0;
    else {
      int pct, px = parse_len2(tok, &pct);
      if (px >= 0)
        *w = px;
      else {
        int c = parse_color(tok);
        if (c >= 0)
          *col = c;
      }
    }
    v += k;
  }
}

/* SVG paint: -2 none, -3 currentColor, 0xRRGGBB, or -1 if unrecognised (e.g. url(#gradient)). */
int parse_paint_val(const char *v)
{
  while (*v == ' ' || *v == '\t')
    v++;
  if (starts_ci(v, "none") || starts_ci(v, "transparent"))
    return -2;
  if (starts_ci(v, "currentcolor"))
    return -3;
  return parse_color(v);
}

int parse_weight(const char *v)
{
  while (*v == ' ')
    v++;
  if (starts_ci(v, "bold"))
    return 1; /* bold, bolder */
  if (starts_ci(v, "normal") || starts_ci(v, "light"))
    return 0;
  int n = 0;
  bool any = false;
  while (*v >= '0' && *v <= '9') {
    n = n * 10 + (*v++ - '0');
    any = true;
  }
  if (any)
    return n >= 600 ? 1 : 0;
  return -1;
}

int parse_border_style(const char *v)
{
  while (*v) {
    while (*v == ' ' || *v == '\t')
      v++;
    const char *e = v;
    while (*e && *e != ' ' && *e != '\t')
      e++;
    int n = (int)(e - v);
    if ((n == 4 && starts_ci(v, "none")) || (n == 6 && starts_ci(v, "hidden")))
      return CSS_BST_NONE;
    if (n == 6 && starts_ci(v, "dashed"))
      return CSS_BST_DASHED;
    if (n == 6 && starts_ci(v, "dotted"))
      return CSS_BST_DOTTED;
    if (n == 5 && starts_ci(v, "solid"))
      return CSS_BST_SOLID;
    v = e;
  }
  return CSS_BST_UNSET;
}

void parse_border_styles(const char *v, signed char out[4])
{
  signed char vals[4];
  int n = 0;
  while (*v && n < 4) {
    while (*v == ' ' || *v == '\t')
      v++;
    if (!*v)
      break;
    const char *e = v;
    while (*e && *e != ' ' && *e != '\t')
      e++;
    char tok[16];
    int k = (int)(e - v);
    if (k > (int)sizeof tok - 1)
      k = sizeof tok - 1;
    memcpy(tok, v, (unsigned)k);
    tok[k] = '\0';
    vals[n++] = (signed char)parse_border_style(tok);
    v = e;
  }
  if (n == 1)
    out[0] = out[1] = out[2] = out[3] = vals[0];
  else if (n == 2) {
    out[0] = out[2] = vals[0];
    out[1] = out[3] = vals[1];
  } else if (n == 3) {
    out[0] = vals[0];
    out[1] = out[3] = vals[1];
    out[2] = vals[2];
  } else if (n == 4) {
    for (int i = 0; i < 4; i++)
      out[i] = vals[i];
  }
}

/* case-insensitive substring, for text-decoration in a multi-value list */
bool has_ci(const char *hay, const char *needle)
{
  for (; *hay; hay++)
    if (starts_ci(hay, needle))
      return true;
  return false;
}

/* Decode a `content` value into out[cap]: quoted string (\HHHH and \" \\ escapes) -> UTF-8, else "". */
void parse_content(const char *v, char *out, int cap)
{
  out[0] = '\0';
  while (*v == ' ' || *v == '\t')
    v++;
  if (*v != '"' && *v != '\'')
    return; /* only literal strings */
  char q = *v++;
  int k = 0;
  while (*v && *v != q && k < cap - 1) {
    if (*v == '\\') {
      v++;
      int h = 0, n = 0;
      while (n < 6 && ((*v >= '0' && *v <= '9') || (*v >= 'a' && *v <= 'f') || (*v >= 'A' && *v <= 'F'))) {
        int d = (*v <= '9') ? *v - '0' : ((*v | 32) - 'a' + 10);
        h = h * 16 + d;
        v++;
        n++;
      }
      if (n) { /* \HHHH unicode escape -> UTF-8 */
        unsigned cp = (unsigned)h;
        if (*v == ' ')
          v++; /* one trailing space terminates the escape */
        if (cp < 0x80) {
          if (k < cap - 1)
            out[k++] = (char)cp;
        } else if (cp < 0x800) {
          if (k + 2 < cap) {
            out[k++] = 0xC0 | (cp >> 6);
            out[k++] = 0x80 | (cp & 0x3F);
          }
        } else if (cp < 0x10000) {
          if (k + 3 < cap) {
            out[k++] = 0xE0 | (cp >> 12);
            out[k++] = 0x80 | ((cp >> 6) & 0x3F);
            out[k++] = 0x80 | (cp & 0x3F);
          }
        } else {
          if (k + 4 < cap) {
            out[k++] = 0xF0 | (cp >> 18);
            out[k++] = 0x80 | ((cp >> 12) & 0x3F);
            out[k++] = 0x80 | ((cp >> 6) & 0x3F);
            out[k++] = 0x80 | (cp & 0x3F);
          }
        }
      } else if (*v) { /* \" or \\ etc. */
        out[k++] = *v++;
      }
    } else {
      out[k++] = *v++;
    }
  }
  out[k] = '\0';
}

int parse_disp(const char *v)
{
  while (*v == ' ')
    v++;
  if (starts_ci(v, "none"))
    return CSS_DISP_NONE;
  if (starts_ci(v, "inline"))
    return CSS_DISP_INLINE;
  return CSS_DISP_BLOCK; /* block/flex/grid/table */
}

/* backgrounds */
void css_bgi_reset(struct css_bg_image *b)
{
  b->kind = -1;
  b->repeat = -1;
  b->size_kind = -1;
  b->rad_circle = 0;
  b->angle = 180;
  b->size_w = b->size_h = -1;
  b->pos_x = b->pos_y = -1;
  b->nstops = 0;
  b->img_idx = -1;
  b->url[0] = '\0';
}

/* True if `p` begins a CSS gradient function (any flavour we model). */
static bool is_grad_prefix(const char *p)
{
  return starts_ci(p, "linear-gradient(") || starts_ci(p, "radial-gradient(") ||
         starts_ci(p, "repeating-linear-gradient(") || starts_ci(p, "repeating-radial-gradient(");
}

/* background-repeat: a value (one or two keywords); -1 if none recognised. */
int parse_bg_repeat(const char *v)
{
  if (has_ci(v, "repeat-x"))
    return CSS_BGR_REPEAT_X;
  if (has_ci(v, "repeat-y"))
    return CSS_BGR_REPEAT_Y;
  if (has_ci(v, "no-repeat"))
    return CSS_BGR_NO_REPEAT;
  if (has_ci(v, "repeat") || has_ci(v, "round") || has_ci(v, "space"))
    return CSS_BGR_REPEAT;
  return -1;
}

/* background-size: cover/contain or 1-2 lengths (auto). */
void parse_bg_size(const char *v, signed char *kind, short *sw, short *sh)
{
  *kind = -1;
  *sw = *sh = -1;
  while (*v == ' ' || *v == '\t')
    v++;
  if (starts_ci(v, "cover")) {
    *kind = CSS_BGS_COVER;
    return;
  }
  if (starts_ci(v, "contain")) {
    *kind = CSS_BGS_CONTAIN;
    return;
  }
  short vals[2];
  int nv = 0;
  while (*v && nv < 2) {
    while (*v == ' ' || *v == '\t')
      v++;
    if (!*v)
      break;
    if (starts_ci(v, "auto"))
      vals[nv++] = -1;
    else {
      int pct, px = parse_len2(v, &pct);
      vals[nv++] = (short)(px >= 0 ? px : -1);
    }
    while (*v && *v != ' ' && *v != '\t')
      v++;
  }
  if (nv == 0)
    return;
  *kind = CSS_BGS_LEN;
  *sw = vals[0];
  *sh = (nv >= 2) ? vals[1] : -1;
}

/* background-position: 1-2 keyword/percent tokens -> percent per axis; lengths degrade to 0%. */
void parse_bg_position(const char *v, short *px, short *py)
{
  *px = *py = -1;
  char toks[2][32];
  int nt = 0;
  while (*v && nt < 2) {
    while (*v == ' ' || *v == '\t')
      v++;
    if (!*v)
      break;
    int k = 0;
    while (*v && *v != ' ' && *v != '\t' && k < 31)
      toks[nt][k++] = *v++;
    toks[nt][k] = '\0';
    nt++;
  }
  bool xset = false, yset = false;
  for (int i = 0; i < nt; i++) {
    const char *t = toks[i];
    if (starts_ci(t, "left")) {
      *px = 0;
      xset = true;
    } else if (starts_ci(t, "right")) {
      *px = 100;
      xset = true;
    } else if (starts_ci(t, "top")) {
      *py = 0;
      yset = true;
    } else if (starts_ci(t, "bottom")) {
      *py = 100;
      yset = true;
    } else if (starts_ci(t, "center")) { /* assigned to a free axis below */
    } else {
      int pct;
      parse_len2(t, &pct); /* pixel positions aren't supported (no box size here) -> 0% */
      short p = (short)(pct >= 0 ? (pct > 100 ? 100 : pct) : 0);
      if (i == 0 && !xset) {
        *px = p;
        xset = true;
      } else if (!yset) {
        *py = p;
        yset = true;
      }
    }
  }
  for (int i = 0; i < nt; i++)
    if (starts_ci(toks[i], "center")) {
      if (!xset) {
        *px = 50;
        xset = true;
      } else if (!yset) {
        *py = 50;
        yset = true;
      }
    }
  if (xset && !yset)
    *py = 50;
  if (yset && !xset)
    *px = 50;
}

/* One position token -> percent along its axis (0 horizontal, 1 vertical), or -1. */
int bg_pos_single(const char *t, int axis)
{
  while (*t == ' ' || *t == '\t')
    t++;
  if (starts_ci(t, "center"))
    return 50;
  if (axis == 0) {
    if (starts_ci(t, "left"))
      return 0;
    if (starts_ci(t, "right"))
      return 100;
  } else {
    if (starts_ci(t, "top"))
      return 0;
    if (starts_ci(t, "bottom"))
      return 100;
  }
  int pct;
  parse_len2(t, &pct);
  return pct >= 0 ? (pct > 100 ? 100 : pct) : -1;
}

/* A linear-gradient direction token ("to bottom right", "45deg", "1turn") -> a
 * CSS angle in *angle (0 = to top, 90 = to right). Returns 1 if consumed. */
static bool parse_lin_dir(const char *a, short *angle)
{
  while (*a == ' ')
    a++;
  if (starts_ci(a, "to ")) {
    a += 3;
    int top = has_ci(a, "top"), bot = has_ci(a, "bottom"), left = has_ci(a, "left"), right = has_ci(a, "right");
    int ang;
    if (top && right)
      ang = 45;
    else if (bot && right)
      ang = 135;
    else if (bot && left)
      ang = 225;
    else if (top && left)
      ang = 315;
    else if (top)
      ang = 0;
    else if (bot)
      ang = 180;
    else if (right)
      ang = 90;
    else if (left)
      ang = 270;
    else
      return false;
    *angle = (short)ang;
    return true;
  }
  const char *p = a;
  bool neg = false;
  if (*p == '-') {
    neg = true;
    p++;
  } else if (*p == '+')
    p++;
  if (!(*p >= '0' && *p <= '9'))
    return false;
  int n = 0;
  while (*p >= '0' && *p <= '9')
    n = n * 10 + (*p++ - '0');
  if (*p == '.') {
    p++;
    while (*p >= '0' && *p <= '9')
      p++;
  } /* drop fraction */
  int deg;
  if (starts_ci(p, "deg"))
    deg = n;
  else if (starts_ci(p, "turn"))
    deg = n * 360;
  else if (starts_ci(p, "grad"))
    deg = n * 9 / 10;
  else if (starts_ci(p, "rad"))
    deg = n * 57; /* ~180/pi, integer-approx */
  else
    return false;
  if (neg)
    deg = -deg;
  deg %= 360;
  if (deg < 0)
    deg += 360;
  *angle = (short)deg;
  return true;
}

/* A radial-gradient leading token; only the shape is honoured, the fill is centred. */
static bool parse_rad_dir(const char *a, struct css_bg_image *b)
{
  if (has_ci(a, "circle")) {
    b->rad_circle = 1;
    return true;
  }
  if (has_ci(a, "ellipse")) {
    b->rad_circle = 0;
    return true;
  }
  if (has_ci(a, "at ") || has_ci(a, "closest") || has_ci(a, "farthest"))
    return true;
  return false;
}

/* Append one "color [position]" gradient stop to b. */
static void add_gradient_stop(const char *arg, struct css_bg_image *b)
{
  if (b->nstops >= CSS_BG_STOPS)
    return;
  while (*arg == ' ' || *arg == '\t')
    arg++;
  /* A trailing number/percentage at paren depth 0 is the stop position; the rest is the colour. */
  const char *lastsp = 0;
  int depth = 0;
  for (const char *q = arg; *q; q++) {
    if (*q == '(')
      depth++;
    else if (*q == ')')
      depth--;
    else if (*q == ' ' && depth == 0)
      lastsp = q;
  }
  const char *pos = 0;
  if (lastsp) {
    const char *t = lastsp + 1;
    while (*t == ' ')
      t++;
    if ((*t >= '0' && *t <= '9') || *t == '.' || *t == '-' || *t == '+')
      pos = t;
  }
  char cbuf[80];
  int cl = pos ? (int)(lastsp - arg) : (int)strlen(arg);
  if (cl > (int)sizeof cbuf - 1)
    cl = (int)sizeof cbuf - 1;
  for (int i = 0; i < cl; i++)
    cbuf[i] = arg[i];
  cbuf[cl] = '\0';
  while (cl > 0 && (cbuf[cl - 1] == ' ' || cbuf[cl - 1] == '\t'))
    cbuf[--cl] = '\0';
  int a;
  int c = color_parse_a(cbuf, &a);
  if (c < 0)
    return; /* skip an unparseable stop */
  short pp = -1;
  if (pos) {
    int pct;
    parse_len2(pos, &pct);
    if (pct >= 0)
      pp = (short)(pct > 100 ? 100 : pct);
  }
  b->stops[b->nstops].color = (int)(((unsigned)a << 24) | (unsigned)c);
  b->stops[b->nstops].pos = pp;
  b->nstops++;
}

/* Parse a linear-/radial-gradient() value into b. Returns 1 on success. */
static bool parse_gradient(const char *v, struct css_bg_image *b)
{
  while (*v == ' ')
    v++;
  const char *p = v;
  if (starts_ci(p, "repeating-"))
    p += 10; /* treated as non-repeating */
  bool linear;
  if (starts_ci(p, "linear-gradient(")) {
    linear = true;
    p += 16;
  } else if (starts_ci(p, "radial-gradient(")) {
    linear = false;
    p += 16;
  } else
    return false;
  const char *e = p;
  int depth = 1;
  while (*e && depth) {
    if (*e == '(')
      depth++;
    else if (*e == ')') {
      depth--;
      if (!depth)
        break;
    }
    e++;
  }

  b->kind = linear ? CSS_BGK_LINEAR : CSS_BGK_RADIAL;
  b->angle = linear ? 180 : 0;
  b->rad_circle = 0;
  b->nstops = 0;
  const char *a = p;
  bool first = true;
  while (a < e) {
    const char *c = a;
    int d2 = 0; /* split on top-level commas */
    while (c < e && !(*c == ',' && d2 == 0)) {
      if (*c == '(')
        d2++;
      else if (*c == ')')
        d2--;
      c++;
    }
    const char *as = a, *ae = c;
    while (as < ae && (*as == ' ' || *as == '\t'))
      as++;
    while (ae > as && (ae[-1] == ' ' || ae[-1] == '\t'))
      ae--;
    char arg[96];
    int al = (int)(ae - as);
    if (al > (int)sizeof arg - 1)
      al = (int)sizeof arg - 1;
    for (int i = 0; i < al; i++)
      arg[i] = as[i];
    arg[al] = '\0';
    bool consumed = false;
    if (first)
      consumed = linear ? parse_lin_dir(arg, &b->angle) : parse_rad_dir(arg, b);
    if (!consumed)
      add_gradient_stop(arg, b);
    first = false;
    a = (c < e) ? c + 1 : e;
  }
  return b->nstops > 0;
}

/* background-image: none / url(...) / a gradient; takes the first comma layer. */
void parse_bg_image(const char *v, struct css_bg_image *b)
{
  while (*v == ' ' || *v == '\t')
    v++;
  if (starts_ci(v, "none")) {
    b->kind = CSS_BGK_NONE;
    return;
  }
  if (starts_ci(v, "url(")) {
    const char *u = v + 4;
    while (*u == ' ' || *u == '"' || *u == '\'')
      u++;
    int k = 0;
    while (*u && *u != ')' && *u != '"' && *u != '\'' && k < CSS_BG_URL - 1)
      b->url[k++] = *u++;
    b->url[k] = '\0';
    b->kind = CSS_BGK_URL;
    return;
  }
  if (is_grad_prefix(v))
    parse_gradient(v, b);
}

/* Skip past a url()/gradient() function starting at *pp (which begins it). */
static const char *skip_bg_func(const char *p)
{
  while (*p && *p != '(')
    p++;
  if (!*p)
    return p;
  int depth = 0;
  for (; *p; p++) {
    if (*p == '(')
      depth++;
    else if (*p == ')') {
      depth--;
      if (depth == 0) {
        p++;
        break;
      }
    }
  }
  return p;
}

/* First colour token in a background value (skipping url()/gradient()); 0xRRGGBB, alpha via *alpha, or -1. */
int bg_value_color(const char *v, int *alpha)
{
  const char *p = v;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == '/')
      p++;
    if (!*p)
      break;
    if (starts_ci(p, "url(") || is_grad_prefix(p)) {
      p = skip_bg_func(p);
      continue;
    }
    char tok[64];
    int k = 0, depth = 0; /* a token keeps its own parens */
    while (*p && (depth > 0 || (*p != ' ' && *p != '\t' && *p != ',' && *p != '/'))) {
      if (*p == '(')
        depth++;
      else if (*p == ')')
        depth--;
      if (k < (int)sizeof tok - 1)
        tok[k++] = *p;
      p++;
    }
    tok[k] = '\0';
    int a;
    int c = color_parse_a(tok, &a);
    if (c >= 0) {
      *alpha = a;
      return c;
    }
  }
  *alpha = 255;
  return -1;
}

/* Collect position tokens (before any '/') from a background shorthand into b->pos_x/pos_y. */
static void bg_shorthand_position(const char *rest, struct css_bg_image *b)
{
  char pos[48];
  int pl = 0;
  for (const char *p = rest; *p;) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;
    char tok[32];
    int k = 0;
    while (*p && *p != ' ' && *p != '\t') {
      if (k < 31)
        tok[k++] = *p;
      p++;
    }
    tok[k] = '\0';
    bool is_pos = starts_ci(tok, "left") || starts_ci(tok, "right") || starts_ci(tok, "top") ||
                  starts_ci(tok, "bottom") || starts_ci(tok, "center");
    if (!is_pos) {
      const char *q = tok;
      if (*q == '-' || *q == '+')
        q++;
      if (*q >= '0' && *q <= '9')
        is_pos = true;
    }
    if (is_pos && pl + k + 1 < (int)sizeof pos) {
      if (pl)
        pos[pl++] = ' ';
      for (int i = 0; i < k; i++)
        pos[pl++] = tok[i];
    }
  }
  pos[pl] = '\0';
  if (pl) {
    short px, py;
    parse_bg_position(pos, &px, &py);
    if (px >= 0)
      b->pos_x = px;
    if (py >= 0)
      b->pos_y = py;
  }
}

/* The `background` shorthand: image + colour + repeat + position [/ size]. */
void parse_bg_shorthand(const char *v, struct css_style *out)
{
  struct css_bg_image *b = &out->bgi;
  const char *im = 0, *iend = 0;
  for (const char *p = v; *p; p++)
    if (starts_ci(p, "url(") || is_grad_prefix(p)) {
      im = p;
      iend = skip_bg_func(p);
      break;
    }
  if (im)
    parse_bg_image(im, b);
  else
    b->kind = CSS_BGK_NONE; /* shorthand with no image resets it */

  int a;
  int c = bg_value_color(v, &a);
  if (c >= 0)
    out->bg = c; /* the cascade keeps colour as 0xRRGGBB */

  char rest[160];
  int rl = 0; /* the value minus the image function */
  for (const char *p = v; *p && rl < (int)sizeof rest - 1;) {
    if (p == im) {
      p = iend;
      continue;
    }
    rest[rl++] = *p++;
  }
  rest[rl] = '\0';

  int rep = parse_bg_repeat(rest);
  if (rep >= 0)
    b->repeat = rep;

  for (int i = 0; i < rl; i++) /* size sits after a '/' */
    if (rest[i] == '/') {
      signed char sk;
      short sw, sh;
      parse_bg_size(rest + i + 1, &sk, &sw, &sh);
      if (sk >= 0) {
        b->size_kind = sk;
        b->size_w = sw;
        b->size_h = sh;
      }
      rest[i] = '\0';
      rl = i;
      break;
    }
  bg_shorthand_position(rest, b);
}
