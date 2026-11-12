#include "engine_int.h"

/* declarations */
void css_style_reset(struct css_style *s)
{
  s->color = s->bg = -1;
  s->bold = s->under = s->strike = -1;
  s->weight = -1;
  s->font_rel_w[0] = s->font_rel_w[1] = s->font_rel_w[2] = 0;
  s->disp = CSS_DISP_UNSET;
  s->center = -1;
  s->width = s->height = -1;
  s->max_w = s->max_h = -1;
  s->min_w = s->min_h = -1;
  s->pct_w = s->pct_h = s->pct_maxw = s->pct_maxh = s->pct_minw = s->pct_minh = -1;
  s->box_sizing = -1;
  s->font_px = -1;
  s->font_pct = -1;
  s->radius = -1;
  s->sh_color = -1;
  s->sh_dx = s->sh_dy = s->sh_blur = s->sh_spread = 0;
  s->margin[0] = s->margin[1] = s->margin[2] = s->margin[3] = -1;
  s->padding[0] = s->padding[1] = s->padding[2] = s->padding[3] = -1;
  s->border_w[0] = s->border_w[1] = s->border_w[2] = s->border_w[3] = -1;
  s->border_style[0] = s->border_style[1] = s->border_style[2] = s->border_style[3] = CSS_BST_UNSET;
  s->border_side_color[0] = s->border_side_color[1] = s->border_side_color[2] = s->border_side_color[3] = -1;
  s->border_color = -1;
  s->pos = -1;
  s->top = s->right = s->bottom = s->left = -1;
  s->flex = s->flex_dir = s->flex_wrap = -1;
  s->flex_grow = s->flex_shrink = -1;
  s->flex_basis = -1;
  s->pct_basis = -1;
  s->justify = s->align_items = -1;
  s->gap_row = s->gap_col = -1;
  s->grid = s->grid_flow = -1;
  s->n_cols = s->n_rows = -1;
  s->col_span = s->row_span = -1;
  s->justify_items = -1;
  s->col_start = s->row_start = -1;
  s->n_areas = -1;
  s->area[0] = '\0';
  s->align_self = -1;
  s->aspect = -1;
  s->visibility = s->text_ovf = -1;
  s->obj_pos_x = s->obj_pos_y = -1;
  s->obj_fit = s->pointer_events = s->user_select = s->cursor = s->tab_size = s->bg_clip = -1;
  s->sb_w = -1;
  s->sb_thumb = s->sb_track = -1;
  s->letter_sp = CSS_LS_UNSET;
  s->va_kind = -1;
  s->va_len = 0;
  s->has_tr = -1;
  s->tr_x = s->tr_y = s->tr_xp = s->tr_yp = 0;
  s->tr_m[0] = s->tr_m[3] = 4096;
  s->tr_m[1] = s->tr_m[2] = 0;
  s->tr_mat = 0;
  s->tr_ox = s->tr_oy = 0;
  s->tr_oxp = s->tr_oyp = 50;
  s->tr_org = -1;
  s->flt_bright = -1;
  s->flt_gray = s->flt_invert = s->flt_opacity = -1;
  s->clip_kind = -1;
  s->clip_n = 0;
  s->overflow = -1;
  s->italic = -1;
  s->small_caps = -1;
  s->ttx = -1;
  s->white_space = -1;
  s->list_style = CSS_LST_DEFAULT;
  s->list_pos = CSS_LSP_DEFAULT;
  s->opacity = -1;
  s->line_height = -1;
  s->lh_pct = -1;
  s->outline_w = -1;
  s->outline_color = -1;
  s->z_index = CSS_Z_AUTO;
  s->fill = s->stroke = -1;
  s->svg_decl[0] = '\0';
  s->anim_name[0] = '\0';
  s->anim_dur = -1;
  s->anim_delay = -1;
  s->anim_iter = -2;
  s->anim_dir = -1;
  s->anim_fill = -1;
  s->anim_timing = -1;
  s->trans_mask = 0;
  s->trans_dur = -1;
  s->trans_delay = -1;
  s->trans_timing = -1;
  css_bgi_reset(&s->bgi);
  s->content[0] = '\0';
  s->svg_decl[0] = '\0';
  s->backdrop = 0;
  s->backdrop_set = 0;
  s->fam[0] = '\0';
}

/* First font-family name of a value, dequoted and lowercased, into out[cap]. */
void first_family(const char *v, char *out, int cap)
{
  while (*v == ' ' || *v == '\t')
    v++;
  char q = 0;
  if (*v == '"' || *v == '\'')
    q = *v++;
  int k = 0;
  while (*v && k < cap - 1) {
    if (q) {
      if (*v == q)
        break;
    } else if (*v == ',' || *v == ';')
      break;
    out[k++] = lower(*v);
    v++;
  }
  while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '\t'))
    k--;
  out[k] = '\0';
}

/* Whole font-family stack into out[cap]: names dequoted, lowercased, trimmed, comma-joined. */
void copy_family_stack(const char *v, char *out, int cap)
{
  int k = 0;
  while (*v && k < cap - 1) {
    while (*v == ' ' || *v == '\t' || *v == ',')
      v++;
    if (!*v || *v == ';')
      break;
    char q = 0;
    if (*v == '"' || *v == '\'')
      q = *v++;
    int start = k;
    while (*v && k < cap - 1) {
      if (q) {
        if (*v == q) {
          v++;
          break;
        }
      } else if (*v == ',' || *v == ';')
        break;
      out[k++] = lower(*v);
      v++;
    }
    while (k > start && (out[k - 1] == ' ' || out[k - 1] == '\t'))
      k--;
    while (*v == ' ' || *v == '\t')
      v++;
    if (*v == ',' && k > start && k < cap - 1)
      out[k++] = ',';
  }
  if (k > 0 && out[k - 1] == ',')
    k--;
  out[k] = '\0';
}

/* Case-insensitive suffix test. */
bool ends_ci(const char *s, const char *suf)
{
  int sl = strlen(s), fl = strlen(suf);
  return sl >= fl && streq_ci(s + sl - fl, suf);
}

/* Lowercased-compare a span [a,ae) against a NUL-terminated lowercase literal. */
bool span_is(const char *a, const char *ae, const char *lit)
{
  while (a < ae && *lit && lower(*a) == *lit) {
    a++;
    lit++;
  }
  return a == ae && !*lit;
}

/* justify-content keyword -> enum css_justify. */
signed char parse_justify(const char *v)
{
  if (has_ci(v, "space-between"))
    return CSS_JUST_BETWEEN;
  if (has_ci(v, "space-around"))
    return CSS_JUST_AROUND;
  if (has_ci(v, "space-evenly"))
    return CSS_JUST_EVENLY;
  if (has_ci(v, "center"))
    return CSS_JUST_CENTER;
  if (has_ci(v, "flex-end") || has_ci(v, "right") || has_ci(v, "end"))
    return CSS_JUST_END;
  return CSS_JUST_START; /* flex-start / start / left / normal */
}

/* align-items keyword -> enum css_align (baseline approximated as start/top). */
signed char parse_align(const char *v)
{
  if (has_ci(v, "center"))
    return CSS_ALIGN_CENTER;
  if (has_ci(v, "flex-end") || has_ci(v, "end"))
    return CSS_ALIGN_END;
  if (has_ci(v, "flex-start") || has_ci(v, "baseline") || has_ci(v, "start"))
    return CSS_ALIGN_START;
  return CSS_ALIGN_STRETCH; /* stretch / normal */
}

/* `gap: <row> [<col>]`; row-gap/column-gap longhands are handled by the caller. */
/* Copy a token span [a,b) into a bounded, NUL-terminated stack buffer. */
static void tok_copy(const char *a, const char *b, char *buf, int cap)
{
  int k = 0;
  while (a < b && k < cap - 1)
    buf[k++] = *a++;
  buf[k] = '\0';
}

/* Classify one grid track token into *t; unrecognised tokens become an auto track. */
static void parse_track_token(const char *a, const char *b, struct css_track *t)
{
  char buf[32];
  tok_copy(a, b, buf, sizeof buf);
  t->kind = CSS_TRK_AUTO;
  t->v = 0;
  if (starts_ci(buf, "auto") || starts_ci(buf, "min-content") || starts_ci(buf, "max-content") ||
      starts_ci(buf, "fit-content"))
    return;
  int n = (int)strlen(buf);
  if (n >= 2 && (buf[n - 1] == 'r' || buf[n - 1] == 'R') && (buf[n - 2] == 'f' || buf[n - 2] == 'F')) { /* <n>fr */
    int fr = 0, scale = 100;
    const char *s = buf;
    while (*s >= '0' && *s <= '9' && fr < 30000)
      fr = fr * 10 + (*s++ - '0');
    fr *= 100;
    if (*s == '.')
      for (s++; *s >= '0' && *s <= '9'; s++)
        if (scale > 1)
          fr += (*s - '0') * (scale /= 10);
    t->kind = CSS_TRK_FR;
    t->v = (short)(fr > 0 && fr <= 32767 ? fr : fr > 0 ? 32767 : 100);
    return;
  }
  int pct;
  int px = parse_len2(buf, &pct);
  if (pct >= 0) {
    t->kind = CSS_TRK_PCT;
    t->v = (short)pct;
  } else if (px >= 0) {
    t->kind = CSS_TRK_PX;
    t->v = (short)px;
  }
}

/* Parse a grid track list into out[max], expanding repeat(); returns the track count. */
int parse_tracks(const char *v, struct css_track *out, int max)
{
  int n = 0;
  const char *s = v;
  while (*s && n < max) {
    while (*s == ' ' || *s == '\t' || *s == ',')
      s++;
    if (!*s)
      break;
    if (starts_ci(s, "repeat(")) {
      s += 7;
      while (*s == ' ')
        s++;
      int cnt = 0;
      while (*s >= '0' && *s <= '9')
        cnt = cnt * 10 + (*s++ - '0');
      while (*s && *s != ',' && *s != ')')
        s++;
      if (*s == ',')
        s++;
      const char *inner = s;
      int depth = 0;
      while (*s && !(*s == ')' && depth == 0)) {
        if (*s == '(')
          depth++;
        else if (*s == ')')
          depth--;
        s++;
      }
      char ibuf[96];
      tok_copy(inner, s, ibuf, sizeof ibuf);
      if (*s == ')')
        s++;
      if (cnt < 1)
        cnt = 1;
      struct css_track tmpl[CSS_GRID_MAX];
      int tn = parse_tracks(ibuf, tmpl, CSS_GRID_MAX);
      for (int r = 0; r < cnt && n < max; r++)
        for (int t = 0; t < tn && n < max; t++)
          out[n++] = tmpl[t];
      continue;
    }
    if (*s == '[') {
      while (*s && *s != ']')
        s++;
      if (*s == ']')
        s++;
      continue;
    } /* skip line names */
    const char *tok = s;
    while (*s && *s != ' ' && *s != '\t' && *s != ',' && *s != '[')
      s++;
    if (s > tok)
      parse_track_token(tok, s, &out[n++]);
  }
  return n;
}

void parse_gap(const char *v, struct css_style *o)
{
  const char *s = v;
  while (*s == ' ' || *s == '\t')
    s++;
  int g1 = parse_len(s);
  while (*s && *s != ' ' && *s != '\t')
    s++;
  while (*s == ' ' || *s == '\t')
    s++;
  int g2 = *s ? parse_len(s) : -1;
  if (g1 >= 0) {
    o->gap_row = (short)g1;
    o->gap_col = (short)(g2 >= 0 ? g2 : g1);
  }
}

/* `flex: none | auto | <grow> [<shrink>] [<basis>]`; a unit/percent/auto token is the basis. */
void parse_flex_shorthand(const char *v, struct css_style *o)
{
  const char *f = v;
  while (*f == ' ' || *f == '\t')
    f++;
  if (starts_ci(f, "none")) {
    o->flex_grow = 0;
    o->flex_shrink = 0;
    o->flex_basis = -1;
    o->pct_basis = -1;
    return;
  }
  if (starts_ci(f, "initial")) {
    o->flex_grow = 0;
    o->flex_shrink = 1;
    o->flex_basis = -1;
    o->pct_basis = -1;
    return;
  }
  if (starts_ci(f, "auto")) {
    o->flex_grow = 1;
    o->flex_shrink = 1;
    o->flex_basis = -1;
    o->pct_basis = -1;
    return;
  }

  bool seen_grow = false, seen_shrink = false;
  int pct;
  while (*f) {
    while (*f == ' ' || *f == '\t')
      f++;
    if (!*f)
      break;
    bool has_unit = false;
    for (const char *q = f; *q && *q != ' ' && *q != '\t'; q++)
      if (*q == '%' || *q == 'p' || *q == 'e' || *q == 'r' || *q == 'v' || *q == 'c') {
        has_unit = true;
        break;
      }
    bool isnum = (*f >= '0' && *f <= '9') || *f == '.';
    if (isnum && !has_unit) { /* unitless: grow, then shrink */
      int n = parse_len(f);
      if (!seen_grow) {
        o->flex_grow = (short)(n < 0 ? 0 : n);
        seen_grow = true;
      } else {
        o->flex_shrink = (short)(n < 0 ? 0 : n);
        seen_shrink = true;
      }
    } else if (starts_ci(f, "auto") || starts_ci(f, "content")) {
      o->flex_basis = -1;
      o->pct_basis = -1;
    } else {
      o->flex_basis = parse_len2(f, &pct);
      o->pct_basis = (short)pct;
    }
    while (*f && *f != ' ' && *f != '\t')
      f++;
  }
  if (!seen_grow)
    o->flex_grow = 1; /* `flex: <basis>` == 1 1 <basis> */
  if (!seen_shrink)
    o->flex_shrink = 1;
}

/* box-shadow: first layer only; inset shadows skipped, missing colour is soft black, px only. */
static bool sh_is_len(const char *t) { return (*t >= '0' && *t <= '9') || *t == '-' || *t == '+' || *t == '.'; }
static int sh_len(const char *t)
{
  bool neg = false;
  if (*t == '-') {
    neg = true;
    t++;
  } else if (*t == '+')
    t++;
  int n = 0;
  while (*t >= '0' && *t <= '9')
    n = n * 10 + (*t++ - '0');
  return neg ? -n : n;
}
void parse_box_shadow(const char *v, struct css_style *o)
{
  while (*v == ' ' || *v == '\t')
    v++;
  if (starts_ci(v, "none"))
    return;
  const char *e = v;
  int depth = 0; /* first top-level comma layer */
  while (*e && !(*e == ',' && depth == 0)) {
    if (*e == '(')
      depth++;
    else if (*e == ')')
      depth--;
    e++;
  }
  int lens[4], nl = 0, col = -1, alpha = 255;
  const char *p = v;
  while (p < e) {
    while (p < e && (*p == ' ' || *p == '\t'))
      p++;
    if (p >= e)
      break;
    char tok[64];
    int k = 0, dep = 0;
    while (p < e && (dep > 0 || (*p != ' ' && *p != '\t'))) {
      if (*p == '(')
        dep++;
      else if (*p == ')')
        dep--;
      if (k < (int)sizeof tok - 1)
        tok[k++] = *p;
      p++;
    }
    tok[k] = '\0';
    if (starts_ci(tok, "inset"))
      return; /* inner shadow: not drawn */
    if (sh_is_len(tok)) {
      if (nl < 4)
        lens[nl++] = sh_len(tok);
    } else {
      int a;
      int c = color_parse_a(tok, &a);
      if (c >= 0) {
        col = c;
        alpha = a;
      }
    }
  }
  if (nl >= 2) {
    o->sh_dx = (short)lens[0];
    o->sh_dy = (short)lens[1];
    o->sh_blur = (short)(nl >= 3 ? lens[2] : 0);
    o->sh_spread = (short)(nl >= 4 ? lens[3] : 0);
    o->sh_color = col >= 0 ? (int)(((unsigned)alpha << 24) | (unsigned)col) : (int)0x40000000;
  }
}

/* list-style[-type] marker style; CSS_LST_DEFAULT when the value names no type. */
signed char parse_list_style(const char *v)
{
  if (has_ci(v, "decimal-leading-zero"))
    return CSS_LST_DECIMAL_ZERO;
  if (has_ci(v, "lower-alpha") || has_ci(v, "lower-latin"))
    return CSS_LST_LOWER_ALPHA;
  if (has_ci(v, "upper-alpha") || has_ci(v, "upper-latin"))
    return CSS_LST_UPPER_ALPHA;
  if (has_ci(v, "lower-roman"))
    return CSS_LST_LOWER_ROMAN;
  if (has_ci(v, "upper-roman"))
    return CSS_LST_UPPER_ROMAN;
  if (has_ci(v, "decimal"))
    return CSS_LST_DECIMAL;
  if (has_ci(v, "circle"))
    return CSS_LST_CIRCLE;
  if (has_ci(v, "square"))
    return CSS_LST_SQUARE;
  if (has_ci(v, "disc"))
    return CSS_LST_DISC;
  if (has_ci(v, "none"))
    return CSS_LST_NONE;
  return CSS_LST_DEFAULT;
}

/* list-style[-position]; CSS_LSP_DEFAULT when the value names no position. */
signed char parse_list_position(const char *v)
{
  if (has_ci(v, "inside"))
    return CSS_LSP_INSIDE;
  if (has_ci(v, "outside"))
    return CSS_LSP_OUTSIDE;
  return CSS_LSP_DEFAULT;
}

/* line-height: normal -> unset; unitless -> lh_pct = N*100; length -> px; percent -> lh_pct. */
void parse_line_height(const char *v, struct css_style *o)
{
  while (*v == ' ' || *v == '\t')
    v++;
  if (starts_ci(v, "normal") || starts_ci(v, "inherit") || starts_ci(v, "initial")) {
    o->line_height = -1;
    o->lh_pct = -1;
    return;
  }
  bool has_unit = false;
  for (const char *q = v; *q && *q != ' ' && *q != '\t'; q++)
    if (*q == '%' || ((*q | 32) >= 'a' && (*q | 32) <= 'z')) {
      has_unit = true;
      break;
    }
  if (!has_unit && ((*v >= '0' && *v <= '9') || *v == '.')) { /* unitless multiplier */
    const char *p = v;
    double n;
    if (calc_number(&p, &n) && n >= 0) {
      o->lh_pct = (short)(n * 100 + 0.5);
      o->line_height = -1;
    }
    return;
  }
  int pct, px = parse_len2(v, &pct);
  if (px >= 0) {
    o->line_height = (short)px;
    o->lh_pct = -1;
  } else if (pct >= 0) {
    o->lh_pct = (short)pct;
    o->line_height = -1;
  }
}

/* font-size: keyword scale, else length/percent; em approximated as rem; *o untouched on failure. */
/* A bare `<number>em` font-size as a percentage of the parent's (0.55em -> 55); -1 for anything else. */
static int font_em_pct(const char *v)
{
  while (*v == ' ' || *v == '\t')
    v++;
  long n = 0, frac = 0, scale = 1;
  bool any = false;
  while (*v >= '0' && *v <= '9' && n < 100000) {
    n = n * 10 + (*v++ - '0');
    any = true;
  }
  if (*v == '.')
    for (v++; *v >= '0' && *v <= '9'; v++, any = true)
      if (scale < 10000) {
        frac = frac * 10 + (*v - '0');
        scale *= 10;
      }
  if (!any || (v[0] | 32) != 'e' || (v[1] | 32) != 'm')
    return -1;
  v += 2;
  while (*v == ' ' || *v == '\t')
    v++;
  if (*v && *v != '!' && *v != ';')
    return -1;
  long pct = n * 100 + frac * 100 / scale;
  return pct > 32767 ? 32767 : (int)pct;
}

void parse_font_size(const char *v, struct css_style *o)
{
  if (has_ci(v, "xx-small"))
    o->font_px = 9;
  else if (has_ci(v, "x-small"))
    o->font_px = 10;
  else if (has_ci(v, "smaller"))
    o->font_pct = 80;
  else if (has_ci(v, "small"))
    o->font_px = 13;
  else if (has_ci(v, "xx-large"))
    o->font_px = 32;
  else if (has_ci(v, "x-large"))
    o->font_px = 24;
  else if (has_ci(v, "larger"))
    o->font_pct = 120;
  else if (has_ci(v, "large"))
    o->font_px = 18;
  else if (has_ci(v, "medium"))
    o->font_px = 16;
  else if (font_em_pct(v) >= 0) /* em is the parent's font-size, so it scales like a percentage */
    o->font_pct = (short)font_em_pct(v);
  else {
    int q;
    int px = parse_len2(v, &q);
    if (px >= 0)
      o->font_px = px;
    else if (q >= 0)
      o->font_pct = (short)q;
  }
}

/* Does the token at p read as the `font` shorthand's size rather than a numeric weight like `400`? */
static bool font_tok_is_size(const char *p)
{
  int digits = 0;
  bool dot = false;
  const char *q = p;
  while ((*q >= '0' && *q <= '9') || *q == '.') {
    if (*q == '.')
      dot = true;
    else
      digits++;
    q++;
  }
  if (!digits && !dot)
    return false; /* not numeric */
  if (dot || *q == '/' || *q == '%')
    return true; /* decimal / lh / percent */
  if (((*q | 32) >= 'a' && (*q | 32) <= 'z'))
    return true; /* carries a unit */
  return false;  /* bare integer: a weight */
}

/* The `font` shorthand; resets omitted longhands only once a valid size and family parse. */
void parse_font_shorthand(const char *v, struct css_style *out)
{
  while (*v == ' ' || *v == '\t')
    v++;
  if (!*v)
    return;

  /* System fonts collapse to the UI default face (size + sans-serif). */
  if (starts_ci(v, "caption") || starts_ci(v, "icon") || starts_ci(v, "menu") || starts_ci(v, "message-box") ||
      starts_ci(v, "small-caption") || starts_ci(v, "status-bar")) {
    out->font_px = starts_ci(v, "small-caption") ? 13 : 15;
    out->italic = 0;
    out->small_caps = 0;
    out->bold = 0;
    out->weight = 400;
    copy_family_stack("sans-serif", out->fam, CSS_FAM_STACK);
    return;
  }

  signed char italic = -1, scaps = -1, bold = -1;
  short weight = -1;
  const char *size_tok = 0;
  const char *p = v;
  for (;;) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;
    if (starts_ci(p, "small-caps"))
      scaps = 1;
    else if (starts_ci(p, "italic") || starts_ci(p, "oblique"))
      italic = 1;
    else if (starts_ci(p, "bold") || starts_ci(p, "lighter")) {
      weight = (short)parse_weight_num(p);
      bold = (signed char)parse_weight(p);
    } else if (font_tok_is_size(p) || starts_ci(p, "xx-small") || starts_ci(p, "x-small") || starts_ci(p, "smaller") ||
               starts_ci(p, "small") || starts_ci(p, "xx-large") || starts_ci(p, "x-large") || starts_ci(p, "larger") ||
               starts_ci(p, "large") || starts_ci(p, "medium")) {
      size_tok = p;
      break;
    } else if (*p >= '0' && *p <= '9') {
      int w = parse_weight(p);
      if (w >= 0) {
        bold = (signed char)w;
        weight = (short)parse_weight_num(p);
      }
    }
    /* normal / stretch (condensed/expanded/…) / unknown: neutral, skip */
    while (*p && *p != ' ' && *p != '\t')
      p++;
  }
  if (!size_tok)
    return; /* invalid: no font-size */

  /* Isolate the size token; split off an optional `/line-height`. */
  const char *se = size_tok;
  while (*se && *se != ' ' && *se != '\t')
    se++;
  char sizebuf[48];
  int sn = 0;
  const char *slash = 0;
  for (const char *q = size_tok; q < se && sn < (int)sizeof sizebuf - 1; q++) {
    if (*q == '/') {
      slash = q + 1;
      break;
    }
    sizebuf[sn++] = *q;
  }
  sizebuf[sn] = '\0';
  parse_font_size(sizebuf, out);
  if (slash) {
    char lhbuf[48];
    int ln = 0;
    for (const char *q = slash; q < se && ln < (int)sizeof lhbuf - 1; q++)
      lhbuf[ln++] = *q;
    lhbuf[ln] = '\0';
    parse_line_height(lhbuf, out);
  }

  const char *fam = se;
  while (*fam == ' ' || *fam == '\t')
    fam++;
  if (*fam)
    copy_family_stack(fam, out->fam, CSS_FAM_STACK);

  out->italic = (italic >= 0) ? italic : 0;
  out->small_caps = (scaps >= 0) ? scaps : 0;
  out->bold = (bold >= 0) ? bold : 0;
  out->weight = weight != -1 ? weight : 400;
}

/* opacity: 0..1 number or N% -> 0..100, -1 if unparseable. */
signed char parse_opacity(const char *v)
{
  while (*v == ' ' || *v == '\t')
    v++;
  const char *p = v;
  double n;
  if (!calc_number(&p, &n))
    return -1;
  while (*p == ' ')
    p++;
  if (*p == '%')
    n /= 100.0;
  if (n < 0)
    n = 0;
  if (n > 1)
    n = 1;
  return (signed char)(n * 100 + 0.5);
}
