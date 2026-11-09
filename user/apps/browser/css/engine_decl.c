#include "engine_int.h"

/* animation/transition */
/* A CSS <time> in ms; -1 if not a time. */
static int parse_time_ms(const char *s, const char *e)
{
  while (s < e && (*s == ' ' || *s == '\t'))
    s++;
  const char *p = s;
  double n = 0;
  bool any = false, neg = false;
  if (p < e && (*p == '-' || *p == '+')) {
    if (*p == '-')
      neg = true;
    p++;
  }
  while (p < e && *p >= '0' && *p <= '9') {
    n = n * 10 + (*p++ - '0');
    any = true;
  }
  if (p < e && *p == '.') {
    p++;
    double f = 0.1;
    while (p < e && *p >= '0' && *p <= '9') {
      n += (*p++ - '0') * f;
      f *= 0.1;
      any = true;
    }
  }
  if (!any)
    return -1;
  if (p + 1 < e && (p[0] == 'm' || p[0] == 'M') && (p[1] == 's' || p[1] == 'S')) {
  } else if (p < e && (*p == 's' || *p == 'S'))
    n *= 1000.0;
  else
    return -1; /* no time unit -> not a <time> */
  if (neg)
    return -1;
  return (int)(n + 0.5);
}

/* <easing-function> keyword -> enum css_easing, -1 if unknown; functions map to nearest preset. */
static int parse_easing(const char *s, const char *e)
{
  while (s < e && (*s == ' ' || *s == '\t'))
    s++;
  if (span_is(s, e, "linear"))
    return CSS_EASE_LINEAR;
  if (span_is(s, e, "ease-in-out"))
    return CSS_EASE_IN_OUT;
  if (span_is(s, e, "ease-in"))
    return CSS_EASE_IN;
  if (span_is(s, e, "ease-out"))
    return CSS_EASE_OUT;
  if (span_is(s, e, "ease"))
    return CSS_EASE;
  if (span_is(s, e, "step-start"))
    return CSS_EASE_STEP_START;
  if (span_is(s, e, "step-end"))
    return CSS_EASE_STEP_END;
  /* function forms carry parenthesised args; match by prefix and approximate */
  if (s < e && starts_ci(s, "steps"))
    return CSS_EASE_STEP_END;
  if (s < e && starts_ci(s, "cubic-bezier"))
    return CSS_EASE;
  return -1;
}

/* transition-property name -> (1<<ANP_*) mask; `all` -> TRANS_ALL, none/unknown -> 0. */
static unsigned trans_prop_bit(const char *s, const char *e)
{
  while (s < e && (*s == ' ' || *s == '\t'))
    s++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
    e--;
  if (span_is(s, e, "all"))
    return TRANS_ALL;
  if (span_is(s, e, "none") || s >= e)
    return 0;
  if (span_is(s, e, "color"))
    return 1u << ANP_COLOR;
  if (span_is(s, e, "background-color") || span_is(s, e, "background"))
    return 1u << ANP_BG;
  if (span_is(s, e, "border-color"))
    return 1u << ANP_BORDER_COLOR;
  if (span_is(s, e, "outline-color"))
    return 1u << ANP_OUTLINE_COLOR;
  if (span_is(s, e, "outline-width"))
    return 1u << ANP_OUTLINE_W;
  if (span_is(s, e, "opacity"))
    return 1u << ANP_OPACITY;
  if (span_is(s, e, "width"))
    return 1u << ANP_WIDTH;
  if (span_is(s, e, "height"))
    return 1u << ANP_HEIGHT;
  if (span_is(s, e, "min-width"))
    return 1u << ANP_MIN_W;
  if (span_is(s, e, "min-height"))
    return 1u << ANP_MIN_H;
  if (span_is(s, e, "max-width"))
    return 1u << ANP_MAX_W;
  if (span_is(s, e, "max-height"))
    return 1u << ANP_MAX_H;
  if (span_is(s, e, "margin"))
    return (1u << ANP_MARGIN_T) | (1u << ANP_MARGIN_R) | (1u << ANP_MARGIN_B) | (1u << ANP_MARGIN_L);
  if (span_is(s, e, "margin-top"))
    return 1u << ANP_MARGIN_T;
  if (span_is(s, e, "margin-right"))
    return 1u << ANP_MARGIN_R;
  if (span_is(s, e, "margin-bottom"))
    return 1u << ANP_MARGIN_B;
  if (span_is(s, e, "margin-left"))
    return 1u << ANP_MARGIN_L;
  if (span_is(s, e, "padding"))
    return (1u << ANP_PADDING_T) | (1u << ANP_PADDING_R) | (1u << ANP_PADDING_B) | (1u << ANP_PADDING_L);
  if (span_is(s, e, "padding-top"))
    return 1u << ANP_PADDING_T;
  if (span_is(s, e, "padding-right"))
    return 1u << ANP_PADDING_R;
  if (span_is(s, e, "padding-bottom"))
    return 1u << ANP_PADDING_B;
  if (span_is(s, e, "padding-left"))
    return 1u << ANP_PADDING_L;
  if (span_is(s, e, "top"))
    return 1u << ANP_TOP;
  if (span_is(s, e, "right"))
    return 1u << ANP_RIGHT;
  if (span_is(s, e, "bottom"))
    return 1u << ANP_BOTTOM;
  if (span_is(s, e, "left"))
    return 1u << ANP_LEFT;
  if (span_is(s, e, "border-radius"))
    return 1u << ANP_RADIUS;
  if (span_is(s, e, "font-size"))
    return 1u << ANP_FONT_SIZE;
  return 0; /* transform/other unsupported: ignore */
}

/* animation-direction / -fill-mode / -iteration-count keyword classifiers. */
static int parse_anim_dir(const char *s, const char *e)
{
  if (span_is(s, e, "reverse"))
    return CSS_ADIR_REVERSE;
  if (span_is(s, e, "alternate-reverse"))
    return CSS_ADIR_ALTERNATE_REVERSE;
  if (span_is(s, e, "alternate"))
    return CSS_ADIR_ALTERNATE;
  if (span_is(s, e, "normal"))
    return CSS_ADIR_NORMAL;
  return -1;
}
static int parse_anim_fill(const char *s, const char *e)
{
  if (span_is(s, e, "forwards"))
    return CSS_AFILL_FORWARDS;
  if (span_is(s, e, "backwards"))
    return CSS_AFILL_BACKWARDS;
  if (span_is(s, e, "both"))
    return CSS_AFILL_BOTH;
  if (span_is(s, e, "none"))
    return CSS_AFILL_NONE;
  return -1;
}

/* Split on top-level whitespace (not inside parens), calling `tok` per token. */
static void for_each_token(const char *s, const char *e, void (*tok)(const char *, const char *, void *), void *ud)
{
  while (s < e) {
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
      s++;
    if (s >= e)
      break;
    const char *ts = s;
    int par = 0;
    while (s < e && (par > 0 || (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r'))) {
      if (*s == '(')
        par++;
      else if (*s == ')' && par > 0)
        par--;
      s++;
    }
    tok(ts, s, ud);
  }
}

struct anim_sh {
  struct css_style *out;
  int ntime;
};
static void anim_token(const char *s, const char *e, void *ud)
{
  struct anim_sh *a = ud;
  int t = parse_time_ms(s, e);
  if (t >= 0) {
    if (a->ntime == 0)
      a->out->anim_dur = t;
    else if (a->ntime == 1)
      a->out->anim_delay = t;
    a->ntime++;
    return;
  }
  int ez = parse_easing(s, e);
  if (ez >= 0) {
    a->out->anim_timing = (signed char)ez;
    return;
  }
  int dir = parse_anim_dir(s, e);
  if (dir >= 0) {
    a->out->anim_dir = (signed char)dir;
    return;
  }
  int fl = parse_anim_fill(s, e);
  if (fl >= 0) {
    a->out->anim_fill = (signed char)fl;
    return;
  }
  if (span_is(s, e, "infinite")) {
    a->out->anim_iter = -1;
    return;
  }
  {
    const char *p = s;
    bool any = false;
    double n = 0;
    while (p < e && *p >= '0' && *p <= '9') {
      n = n * 10 + (*p++ - '0');
      any = true;
    }
    if (p < e && *p == '.') {
      p++;
      double f = 0.1;
      while (p < e && *p >= '0' && *p <= '9') {
        n += (*p++ - '0') * f;
        f *= 0.1;
        any = true;
      }
    }
    if (any && p >= e) {
      a->out->anim_iter = (int)(n + 0.5);
      return;
    }
  }
  if (span_is(s, e, "none")) {
    a->out->anim_name[0] = '\0';
    return;
  }
  if (!span_is(s, e, "running") && !span_is(s, e, "paused")) {
    int n = 0;
    const char *p = s;
    if (e - s >= 2 && (*s == '"' || *s == '\'') && e[-1] == s[0]) {
      p = s + 1;
      e--;
    }
    for (; p < e && n < CSS_ANIM_NAME - 1; p++)
      a->out->anim_name[n++] = *p;
    a->out->anim_name[n] = '\0';
  }
}

struct trans_sh {
  struct css_style *out;
  unsigned mask;
  int ntime;
};
static void trans_token(const char *s, const char *e, void *ud)
{
  struct trans_sh *t = ud;
  int ms = parse_time_ms(s, e);
  if (ms >= 0) {
    if (t->ntime == 0)
      t->out->trans_dur = ms;
    else if (t->ntime == 1)
      t->out->trans_delay = ms;
    t->ntime++;
    return;
  }
  int ez = parse_easing(s, e);
  if (ez >= 0) {
    t->out->trans_timing = (signed char)ez;
    return;
  }
  t->mask |= trans_prop_bit(s, e);
}

/* Parse one comma-separated `transition` segment [s,e). */
static void parse_transition_segment(const char *s, const char *e, struct css_style *out)
{
  struct trans_sh t = {out, 0, 0};
  for_each_token(s, e, trans_token, &t);
  if (t.mask == 0)
    t.mask = TRANS_ALL; /* only a duration given -> `all` */
  out->trans_mask |= t.mask;
}

/* Properties an inline <svg> needs verbatim: the page CSS cascades onto its elements and is serialized with them. */
static const char *const svg_props[] = {
    "fill",
    "stroke",
    "stroke-width",
    "stroke-linecap",
    "stroke-linejoin",
    "stroke-miterlimit",
    "stroke-dasharray",
    "stroke-dashoffset",
    "stroke-opacity",
    "fill-opacity",
    "fill-rule",
    "clip-rule",
    "clip-path",
    "mask",
    "filter",
    "opacity",
    "color",
    "display",
    "visibility",
    "transform",
    "stop-color",
    "stop-opacity",
    "flood-color",
    "flood-opacity",
    "marker-start",
    "marker-mid",
    "marker-end",
    "marker",
    "text-anchor",
    "dominant-baseline",
    "paint-order",
    "font-size",
    "font-weight",
    "font-style",
    "letter-spacing",
};

static void svg_decl_add(const char *p, const char *pe, const char *val, struct css_style *out)
{
  int ol = (int)strlen(out->svg_decl), pl = (int)(pe - p), vl = (int)strlen(val);
  if (ol + pl + vl + 2 >= CSS_SVG_DECL)
    return;
  for (unsigned i = 0; i < sizeof svg_props / sizeof *svg_props; i++) {
    if ((int)strlen(svg_props[i]) != pl || !eq_ci_n(p, svg_props[i], pl))
      continue;
    char *d = out->svg_decl + ol;
    memcpy(d, p, (unsigned)pl);
    d[pl] = ':';
    memcpy(d + pl + 1, val, (unsigned)vl);
    d[pl + 1 + vl] = ';';
    d[pl + 2 + vl] = '\0';
    return;
  }
}

static void apply_decl(const char *p, const char *pe, const char *v, const char *ve, struct css_style *out,
                       bool *important)
{
  while (p < pe && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    p++;
  while (pe > p && (pe[-1] == ' ' || pe[-1] == '\t'))
    pe--;
  while (v < ve && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r'))
    v++;
  while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n' || ve[-1] == '\r'))
    ve--;
  if (p >= pe || v >= ve)
    return;

  /* copy value to a small NUL-terminated buffer + strip !important */
  char val[128];
  int pct; /* scratch for parse_len2's percent out-param */
  int vl = (int)(ve - v);
  if (vl > (int)sizeof val - 1)
    vl = (int)sizeof val - 1;
  for (int i = 0; i < vl; i++)
    val[i] = v[i];
  val[vl] = '\0';
  for (int i = 0; i + 9 < vl + 1; i++) {
    if (val[i] == '!' && starts_ci(val + i + 1, "important")) {
      if (important)
        *important = true;
      val[i] = '\0';
      while (i > 0 && (val[i - 1] == ' ' || val[i - 1] == '\t'))
        val[--i] = '\0';
      break;
    }
  }

  svg_decl_add(p, pe, val, out);
  if (span_is(p, pe, "color"))
    out->color = parse_color(val);
  else if (span_is(p, pe, "background-color"))
    out->bg = parse_color(val);
  else if (span_is(p, pe, "background"))
    parse_bg_shorthand(val, out);
  else if (span_is(p, pe, "background-image"))
    parse_bg_image(val, &out->bgi);
  else if (span_is(p, pe, "background-repeat")) {
    int r = parse_bg_repeat(val);
    if (r >= 0)
      out->bgi.repeat = (signed char)r;
  } else if (span_is(p, pe, "background-position")) {
    short bx, by;
    parse_bg_position(val, &bx, &by);
    if (bx >= 0)
      out->bgi.pos_x = bx;
    if (by >= 0)
      out->bgi.pos_y = by;
  } else if (span_is(p, pe, "background-position-x")) {
    int q = bg_pos_single(val, 0);
    if (q >= 0)
      out->bgi.pos_x = (short)q;
  } else if (span_is(p, pe, "background-position-y")) {
    int q = bg_pos_single(val, 1);
    if (q >= 0)
      out->bgi.pos_y = (short)q;
  } else if (span_is(p, pe, "background-size")) {
    signed char sk;
    short sw, sh;
    parse_bg_size(val, &sk, &sw, &sh);
    if (sk >= 0) {
      out->bgi.size_kind = sk;
      out->bgi.size_w = sw;
      out->bgi.size_h = sh;
    }
  }
  /* background-attachment / -origin / -clip: parsed and ignored */
  else if (span_is(p, pe, "background-attachment") || span_is(p, pe, "background-origin") ||
           span_is(p, pe, "background-clip")) {
  } else if (span_is(p, pe, "font-weight"))
    out->bold = parse_weight(val);
  else if (span_is(p, pe, "text-decoration") || span_is(p, pe, "text-decoration-line")) {
    if (has_ci(val, "underline"))
      out->under = 1;
    else if (has_ci(val, "none"))
      out->under = 0;
    if (has_ci(val, "line-through"))
      out->strike = 1;
    else if (has_ci(val, "none"))
      out->strike = 0;
  } else if (span_is(p, pe, "display")) {
    out->disp = parse_disp(val);
    out->flex = has_ci(val, "flex") ? 1 : 0;
    out->grid = has_ci(val, "grid") ? 1 : 0;
  } else if (span_is(p, pe, "text-align")) {
    if (has_ci(val, "center"))
      out->center = 1;
    else if (has_ci(val, "right"))
      out->center = 2;
    else if (has_ci(val, "justify"))
      out->center = 3;
    else if (has_ci(val, "left") || has_ci(val, "start"))
      out->center = 0;
  } else if (span_is(p, pe, "width")) {
    out->width = parse_len2(val, &pct);
    out->pct_w = (short)pct;
  } else if (span_is(p, pe, "height")) {
    out->height = parse_len2(val, &pct);
    out->pct_h = (short)pct;
  } else if (span_is(p, pe, "max-width")) {
    out->max_w = parse_len2(val, &pct);
    out->pct_maxw = (short)pct;
  } else if (span_is(p, pe, "max-height")) {
    out->max_h = parse_len2(val, &pct);
    out->pct_maxh = (short)pct;
  } else if (span_is(p, pe, "min-width")) {
    out->min_w = parse_len2(val, &pct);
    out->pct_minw = (short)pct;
  } else if (span_is(p, pe, "min-height")) {
    out->min_h = parse_len2(val, &pct);
    out->pct_minh = (short)pct;
  } else if (span_is(p, pe, "box-sizing"))
    out->box_sizing = has_ci(val, "border-box") ? 1 : has_ci(val, "content-box") ? 0 : out->box_sizing;
  else if (span_is(p, pe, "margin"))
    parse_box4(val, out->margin, 1);
  else if (span_is(p, pe, "margin-top"))
    out->margin[0] = one_len(val, 1);
  else if (span_is(p, pe, "margin-right"))
    out->margin[1] = one_len(val, 1);
  else if (span_is(p, pe, "margin-bottom"))
    out->margin[2] = one_len(val, 1);
  else if (span_is(p, pe, "margin-left"))
    out->margin[3] = one_len(val, 1);
  else if (span_is(p, pe, "padding"))
    parse_box4(val, out->padding, 0);
  else if (span_is(p, pe, "padding-top"))
    out->padding[0] = one_len(val, 0);
  else if (span_is(p, pe, "padding-right"))
    out->padding[1] = one_len(val, 0);
  else if (span_is(p, pe, "padding-bottom"))
    out->padding[2] = one_len(val, 0);
  else if (span_is(p, pe, "padding-left"))
    out->padding[3] = one_len(val, 0);
  else if (span_is(p, pe, "border-width"))
    parse_box4(val, out->border_w, 0);
  else if (span_is(p, pe, "border-color")) {
    int c = parse_color(val);
    if (c >= 0)
      out->border_color = c;
  } else if (span_is(p, pe, "border-top-color") || span_is(p, pe, "border-right-color") ||
             span_is(p, pe, "border-bottom-color") || span_is(p, pe, "border-left-color")) {
    int c = parse_color(val);
    if (c >= 0) {
      int side = span_is(p, pe, "border-top-color")      ? 0
                 : span_is(p, pe, "border-right-color")  ? 1
                 : span_is(p, pe, "border-bottom-color") ? 2
                                                         : 3;
      out->border_side_color[side] = c;
    }
  } else if (span_is(p, pe, "border-style") || span_is(p, pe, "border-top-style") ||
             span_is(p, pe, "border-right-style") || span_is(p, pe, "border-bottom-style") ||
             span_is(p, pe, "border-left-style")) {
    int side = span_is(p, pe, "border-top-style")      ? 0
               : span_is(p, pe, "border-right-style")  ? 1
               : span_is(p, pe, "border-bottom-style") ? 2
               : span_is(p, pe, "border-left-style")   ? 3
                                                       : -1;
    if (side < 0)
      parse_border_styles(val, out->border_style);
    else {
      int bs = parse_border_style(val);
      if (bs >= 0)
        out->border_style[side] = (signed char)bs;
    }
  } else if (span_is(p, pe, "border") || span_is(p, pe, "border-top") || span_is(p, pe, "border-right") ||
             span_is(p, pe, "border-bottom") || span_is(p, pe, "border-left")) {
    int w, c;
    parse_border_val(val, &w, &c);
    int s = span_is(p, pe, "border-top")      ? 0
            : span_is(p, pe, "border-right")  ? 1
            : span_is(p, pe, "border-bottom") ? 2
            : span_is(p, pe, "border-left")   ? 3
                                              : -1;
    if (w >= 0) {
      if (s < 0)
        out->border_w[0] = out->border_w[1] = out->border_w[2] = out->border_w[3] = (short)w;
      else
        out->border_w[s] = (short)w;
    }
    if (c >= 0)
      out->border_color = c;
    int bs = parse_border_style(val);
    if (bs >= 0) {
      if (s < 0)
        for (int i = 0; i < 4; i++)
          out->border_style[i] = (signed char)bs;
      else
        out->border_style[s] = (signed char)bs;
    }
  } else if (span_is(p, pe, "border-top-width"))
    out->border_w[0] = one_len(val, 0);
  else if (span_is(p, pe, "border-right-width"))
    out->border_w[1] = one_len(val, 0);
  else if (span_is(p, pe, "border-bottom-width"))
    out->border_w[2] = one_len(val, 0);
  else if (span_is(p, pe, "border-left-width"))
    out->border_w[3] = one_len(val, 0);
  else if (span_is(p, pe, "flex-direction"))
    out->flex_dir = has_ci(val, "column") ? 1 : 0;
  else if (span_is(p, pe, "flex-wrap"))
    out->flex_wrap = has_ci(val, "nowrap") ? 0 : (has_ci(val, "wrap") ? 1 : out->flex_wrap);
  else if (span_is(p, pe, "flex-flow")) {
    if (has_ci(val, "column"))
      out->flex_dir = 1;
    else if (has_ci(val, "row"))
      out->flex_dir = 0;
    if (has_ci(val, "nowrap"))
      out->flex_wrap = 0;
    else if (has_ci(val, "wrap"))
      out->flex_wrap = 1;
  } else if (span_is(p, pe, "flex-grow")) {
    int g = parse_len(val);
    if (g >= 0)
      out->flex_grow = (short)g;
  } else if (span_is(p, pe, "flex-shrink")) {
    int g = parse_len(val);
    if (g >= 0)
      out->flex_shrink = (short)g;
  } else if (span_is(p, pe, "flex-basis")) {
    if (has_ci(val, "auto") || has_ci(val, "content")) {
      out->flex_basis = -1;
      out->pct_basis = -1;
    } else {
      int q;
      out->flex_basis = parse_len2(val, &q);
      out->pct_basis = (short)q;
    }
  } else if (span_is(p, pe, "flex"))
    parse_flex_shorthand(val, out);
  else if (span_is(p, pe, "justify-content"))
    out->justify = parse_justify(val);
  else if (span_is(p, pe, "align-items"))
    out->align_items = parse_align(val);
  else if (span_is(p, pe, "gap") || span_is(p, pe, "grid-gap"))
    parse_gap(val, out);
  else if (span_is(p, pe, "row-gap") || span_is(p, pe, "grid-row-gap")) {
    int g = parse_len(val);
    if (g >= 0)
      out->gap_row = (short)g;
  } else if (span_is(p, pe, "column-gap") || span_is(p, pe, "grid-column-gap")) {
    int g = parse_len(val);
    if (g >= 0)
      out->gap_col = (short)g;
  } else if (span_is(p, pe, "grid-template-columns"))
    out->n_cols = (signed char)parse_tracks(val, out->grid_cols, CSS_GRID_MAX);
  else if (span_is(p, pe, "grid-template-rows"))
    out->n_rows = (signed char)parse_tracks(val, out->grid_rows, CSS_GRID_MAX);
  else if (span_is(p, pe, "grid-auto-flow"))
    out->grid_flow = has_ci(val, "column") ? 1 : 0;
  else if (span_is(p, pe, "grid-column") || span_is(p, pe, "grid-column-start") || span_is(p, pe, "grid-column-end"))
    out->col_span = parse_grid_span(val);
  else if (span_is(p, pe, "grid-row") || span_is(p, pe, "grid-row-start") || span_is(p, pe, "grid-row-end"))
    out->row_span = parse_grid_span(val);
  else if (span_is(p, pe, "justify-items"))
    out->justify_items = parse_align(val);
  else if (span_is(p, pe, "place-items")) { /* align-items [justify-items] */
    out->align_items = parse_align(val);
    const char *q = val;
    while (*q && *q != ' ')
      q++;
    while (*q == ' ')
      q++;
    out->justify_items = parse_align(*q ? q : val);
  } else if (span_is(p, pe, "position")) {
    const char *f = val;
    while (*f == ' ')
      f++;
    out->pos = starts_ci(f, "relative")   ? CSS_POS_REL
               : starts_ci(f, "absolute") ? CSS_POS_ABS
               : starts_ci(f, "fixed")    ? CSS_POS_FIXED
               : starts_ci(f, "sticky")   ? CSS_POS_STICKY
                                          : CSS_POS_STATIC;
  } else if (span_is(p, pe, "top"))
    out->top = parse_len(val);
  else if (span_is(p, pe, "right"))
    out->right = parse_len(val);
  else if (span_is(p, pe, "bottom"))
    out->bottom = parse_len(val);
  else if (span_is(p, pe, "left"))
    out->left = parse_len(val);
  else if (span_is(p, pe, "overflow") || span_is(p, pe, "overflow-x") || span_is(p, pe, "overflow-y"))
    out->overflow =
        (has_ci(val, "hidden") || has_ci(val, "auto") || has_ci(val, "scroll") || has_ci(val, "clip")) ? 1 : 0;
  else if (span_is(p, pe, "fill"))
    out->fill = parse_paint_val(val);
  else if (span_is(p, pe, "stroke"))
    out->stroke = parse_paint_val(val);
  else if (span_is(p, pe, "content"))
    parse_content(val, out->content, sizeof out->content);
  else if (span_is(p, pe, "font-family"))
    copy_family_stack(val, out->fam, CSS_FAM_STACK);
  else if (span_is(p, pe, "font-size"))
    parse_font_size(val, out);
  else if (span_is(p, pe, "border-radius") || span_is(p, pe, "border-top-left-radius") ||
           span_is(p, pe, "border-top-right-radius") || span_is(p, pe, "border-bottom-left-radius") ||
           span_is(p, pe, "border-bottom-right-radius")) {
    int q;
    int px = parse_len2(val, &q); /* first value, uniform */
    if (px >= 0)
      out->radius = px;
    else if (q >= 0)
      out->radius = 9999; /* % -> pill (clamped to half) */
  } else if (span_is(p, pe, "box-shadow") || span_is(p, pe, "-webkit-box-shadow"))
    parse_box_shadow(val, out);
  else if (span_is(p, pe, "font-style"))
    out->italic = (has_ci(val, "italic") || has_ci(val, "oblique")) ? 1 : has_ci(val, "normal") ? 0 : out->italic;
  else if (span_is(p, pe, "font"))
    parse_font_shorthand(val, out);
  else if (span_is(p, pe, "font-variant") || span_is(p, pe, "font-variant-caps")) {
    /* only the caps axis renders (as uppercase); other sub-values are inert */
    if (has_ci(val, "small-caps") || has_ci(val, "all-small-caps") || has_ci(val, "petite-caps") ||
        has_ci(val, "all-petite-caps") || has_ci(val, "unicase") || has_ci(val, "titling-caps"))
      out->small_caps = 1;
    else if (has_ci(val, "normal") || has_ci(val, "none"))
      out->small_caps = 0;
  }
  /* Font longhands with no effect here: parsed and ignored. */
  else if (span_is(p, pe, "font-stretch") || span_is(p, pe, "font-kerning") ||
           span_is(p, pe, "font-feature-settings") || span_is(p, pe, "font-variation-settings") ||
           span_is(p, pe, "font-optical-sizing") || span_is(p, pe, "font-size-adjust") ||
           span_is(p, pe, "font-language-override") || span_is(p, pe, "font-synthesis") ||
           span_is(p, pe, "font-smooth") || span_is(p, pe, "-webkit-font-smoothing") ||
           span_is(p, pe, "-moz-osx-font-smoothing") || span_is(p, pe, "font-variant-ligatures") ||
           span_is(p, pe, "font-variant-numeric") || span_is(p, pe, "font-variant-east-asian") ||
           span_is(p, pe, "font-variant-alternates") || span_is(p, pe, "font-variant-position")) {
  } else if (span_is(p, pe, "text-transform")) {
    if (has_ci(val, "uppercase"))
      out->ttx = CSS_TT_UPPER;
    else if (has_ci(val, "lowercase"))
      out->ttx = CSS_TT_LOWER;
    else if (has_ci(val, "capitalize"))
      out->ttx = CSS_TT_CAPITALIZE;
    else if (has_ci(val, "none"))
      out->ttx = CSS_TT_NONE;
  } else if (span_is(p, pe, "white-space")) {
    if (has_ci(val, "pre-wrap"))
      out->white_space = CSS_WS_PRE_WRAP;
    else if (has_ci(val, "pre-line"))
      out->white_space = CSS_WS_PRE_LINE;
    else if (has_ci(val, "nowrap"))
      out->white_space = CSS_WS_NOWRAP;
    else if (has_ci(val, "pre"))
      out->white_space = CSS_WS_PRE;
    else if (has_ci(val, "normal"))
      out->white_space = CSS_WS_NORMAL;
  } else if (span_is(p, pe, "list-style")) {
    /* shorthand: type and/or position keyword, any order; bare url() leaves type DEFAULT. */
    out->list_style = parse_list_style(val);
    out->list_pos = parse_list_position(val);
  } else if (span_is(p, pe, "list-style-type"))
    out->list_style = parse_list_style(val);
  else if (span_is(p, pe, "list-style-position"))
    out->list_pos = parse_list_position(val);
  else if (span_is(p, pe, "line-height"))
    parse_line_height(val, out);
  else if (span_is(p, pe, "opacity"))
    out->opacity = parse_opacity(val);
  else if (span_is(p, pe, "outline") || span_is(p, pe, "outline-width") || span_is(p, pe, "outline-color") ||
           span_is(p, pe, "outline-style")) {
    int w, c;
    parse_border_val(val, &w, &c);
    if (w >= 0)
      out->outline_w = (short)w;
    if (c >= 0)
      out->outline_color = c;
  } else if (span_is(p, pe, "z-index")) {
    const char *f = val;
    while (*f == ' ' || *f == '\t')
      f++;
    if (starts_ci(f, "auto"))
      out->z_index = CSS_Z_AUTO;
    else {
      bool neg = false;
      if (*f == '-') {
        neg = true;
        f++;
      } else if (*f == '+')
        f++;
      if (*f >= '0' && *f <= '9') {
        int n = 0;
        while (*f >= '0' && *f <= '9')
          n = n * 10 + (*f++ - '0');
        out->z_index = (short)(neg ? -n : n);
      }
    }
  } else if (span_is(p, pe, "animation") || span_is(p, pe, "-webkit-animation")) {
    struct anim_sh a = {out, 0};
    for_each_token(val, val + strlen(val), anim_token, &a);
  } else if (span_is(p, pe, "animation-name") || span_is(p, pe, "-webkit-animation-name")) {
    const char *f = val;
    while (*f == ' ' || *f == '\t')
      f++;
    const char *e = f;
    while (*e)
      e++;
    while (e > f && (e[-1] == ' ' || e[-1] == '\t'))
      e--;
    if (span_is(f, e, "none"))
      out->anim_name[0] = '\0';
    else {
      if (e - f >= 2 && (*f == '"' || *f == '\'') && e[-1] == f[0]) {
        f++;
        e--;
      }
      int n = 0;
      for (; f < e && n < CSS_ANIM_NAME - 1; f++)
        out->anim_name[n++] = *f;
      out->anim_name[n] = '\0';
    }
  } else if (span_is(p, pe, "animation-duration") || span_is(p, pe, "-webkit-animation-duration")) {
    int t = parse_time_ms(val, val + strlen(val));
    if (t >= 0)
      out->anim_dur = t;
  } else if (span_is(p, pe, "animation-delay") || span_is(p, pe, "-webkit-animation-delay")) {
    int t = parse_time_ms(val, val + strlen(val));
    if (t >= 0)
      out->anim_delay = t;
  } else if (span_is(p, pe, "animation-timing-function") || span_is(p, pe, "-webkit-animation-timing-function")) {
    int ez = parse_easing(val, val + strlen(val));
    if (ez >= 0)
      out->anim_timing = (signed char)ez;
  } else if (span_is(p, pe, "animation-iteration-count") || span_is(p, pe, "-webkit-animation-iteration-count")) {
    const char *f = val;
    while (*f == ' ')
      f++;
    if (starts_ci(f, "infinite"))
      out->anim_iter = -1;
    else {
      bool any = false;
      double n = 0;
      while (*f >= '0' && *f <= '9') {
        n = n * 10 + (*f++ - '0');
        any = true;
      }
      if (*f == '.') {
        f++;
        double fr = 0.1;
        while (*f >= '0' && *f <= '9') {
          n += (*f++ - '0') * fr;
          fr *= 0.1;
          any = true;
        }
      }
      if (any)
        out->anim_iter = (int)(n + 0.5);
    }
  } else if (span_is(p, pe, "animation-direction") || span_is(p, pe, "-webkit-animation-direction")) {
    int d = parse_anim_dir(val, val + strlen(val));
    if (d >= 0)
      out->anim_dir = (signed char)d;
  } else if (span_is(p, pe, "animation-fill-mode") || span_is(p, pe, "-webkit-animation-fill-mode")) {
    int fm = parse_anim_fill(val, val + strlen(val));
    if (fm >= 0)
      out->anim_fill = (signed char)fm;
  }
  /* animation-play-state / -timeline: parsed and ignored */
  else if (span_is(p, pe, "animation-play-state") || span_is(p, pe, "animation-timeline") ||
           span_is(p, pe, "-webkit-animation-play-state")) {
  } else if (span_is(p, pe, "transition") || span_is(p, pe, "-webkit-transition")) {
    const char *seg = val, *end = val + strlen(val);
    out->trans_mask = 0; /* the shorthand resets the property set */
    while (seg < end) {
      const char *comma = seg;
      while (comma < end && *comma != ',')
        comma++;
      parse_transition_segment(seg, comma, out);
      seg = (comma < end) ? comma + 1 : end;
    }
  } else if (span_is(p, pe, "transition-property") || span_is(p, pe, "-webkit-transition-property")) {
    const char *seg = val, *end = val + strlen(val);
    out->trans_mask = 0;
    while (seg < end) {
      const char *comma = seg;
      while (comma < end && *comma != ',')
        comma++;
      out->trans_mask |= trans_prop_bit(seg, comma);
      seg = (comma < end) ? comma + 1 : end;
    }
  } else if (span_is(p, pe, "transition-duration") || span_is(p, pe, "-webkit-transition-duration")) {
    int t = parse_time_ms(val, val + strlen(val));
    if (t >= 0)
      out->trans_dur = t;
  } else if (span_is(p, pe, "transition-delay") || span_is(p, pe, "-webkit-transition-delay")) {
    int t = parse_time_ms(val, val + strlen(val));
    if (t >= 0)
      out->trans_delay = t;
  } else if (span_is(p, pe, "transition-timing-function") || span_is(p, pe, "-webkit-transition-timing-function")) {
    int ez = parse_easing(val, val + strlen(val));
    if (ez >= 0)
      out->trans_timing = (signed char)ez;
  }
}

/* Parse a `prop: value; ...` block [s,end) into *out. */
void parse_decls(const char *s, const char *end, struct css_style *out, bool *important)
{
  while (s < end) {
    const char *semi = s;
    while (semi < end && *semi != ';')
      semi++;
    const char *colon = s;
    while (colon < semi && *colon != ':')
      colon++;
    if (colon < semi)
      apply_decl(s, colon, colon + 1, semi, out, important);
    s = (semi < end) ? semi + 1 : end;
  }
}

void css_apply_inline(const char *decls, struct css_style *out)
{
  bool imp = false;
  if (str_uses_var(decls)) { /* resolve var() against the sheet table */
    char *buf = resolve_vars_dup(decls, (int)strlen(decls));
    if (buf) {
      parse_decls(buf, buf + strlen(buf), out, &imp);
      free(buf);
    }
    return;
  }
  parse_decls(decls, decls + strlen(decls), out, &imp);
}
