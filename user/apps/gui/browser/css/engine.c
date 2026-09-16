/* css.c — CSS parser + selector matcher + cascade. See css.h.
 *
 * Scope: selectors are tag / .class / #id / * / compound (div.a#b) / descendant
 * (space) / child (>) / grouping (a, b); structural/state pseudo-classes
 * (:first-child, :nth-child(), :not(), :checked, :root, ... — dynamic ones like
 * :hover never match in a static render) and @supports feature queries are
 * honoured. Cascade is
 * by specificity then source order, with !important winning. Properties acted
 * on: color, background[-color], font-weight, text-decoration, display, width,
 * height (others parse-and-ignore, like any browser). Inline <style> and
 * style="" only — external <link rel=stylesheet> is not fetched. Descendant
 * matching is greedy (no backtracking), enough for ordinary selectors. */
#include "../css.h"
#include "sibling.h"
#include "../color.h" /* shared colour parser */
#include "owos.h"

#define MAX_CLS 6
#define MAX_STEPS 6
#define MAX_PC 3     /* pseudo-classes per compound (input:checked:focus) */
#define MAX_NOTCLS 3 /* classes inside a :not() simple selector            */

/* The argument of :not() — a single simple selector (tag / #id / .class, no
 * combinators or nested pseudos): enough for :not(.x), :not(div), :not(div.x).
 */
struct simple_sel {
  char tag[16];
  char cls[MAX_NOTCLS][24];
  int ncls;
  char id[24];
};

/* Pseudo-classes we model. The structural/state ones are matched against the
 * DOM; the dynamic group lands on PC_NEVER (no live interaction state in a
 * static render, so it never matches), as does any pseudo-class we don't
 * recognise (its rule then can't apply, which is what an invalid selector
 * should do). */
enum {
  PC_FIRST_CHILD,
  PC_LAST_CHILD,
  PC_ONLY_CHILD,
  PC_FIRST_OF_TYPE,
  PC_LAST_OF_TYPE,
  PC_ONLY_OF_TYPE,
  PC_NTH_CHILD,
  PC_NTH_LAST_CHILD,
  PC_NTH_OF_TYPE,
  PC_NTH_LAST_OF_TYPE,
  PC_ROOT,
  PC_EMPTY,
  PC_NOT,
  PC_CHECKED,
  PC_DISABLED,
  PC_ENABLED,
  PC_REQUIRED,
  PC_OPTIONAL,
  PC_LINK,
  PC_MATCHES, /* :is()/:where(): matches if any sub-selector does  */
  PC_NEVER,
};

/* :is()/:where() argument: a heap list of full sub-selectors (each may carry
 * its own combinators and nested :is()). Held by pointer so struct pseudo —
 * embedded in every rule's struct selector — stays fixed-size and can nest
 * arbitrarily. */
struct sel_list;

struct pseudo {
  int kind;
  int a, b;              /* an+b for the nth-* kinds; a=1 marks :where (spec 0) */
  struct simple_sel arg; /* :not() argument (kind == PC_NOT) */
  struct sel_list *list; /* :is()/:where() sub-selectors (kind == PC_MATCHES), else 0 */
};

struct sel_step {
  char tag[16];
  char cls[MAX_CLS][24];
  int ncls;
  char id[24];
  struct pseudo pc[MAX_PC];
  int npc;
  int pseudo_el; /* PE_* — a ::pseudo-element; never matches a normal box */
  int child;     /* combinator to the step on the right is '>' */
};
enum { PE_NONE = 0, PE_BEFORE = CSS_PSEUDO_BEFORE, PE_AFTER = CSS_PSEUDO_AFTER, PE_OTHER = 3 };

struct selector {
  struct sel_step step[MAX_STEPS];
  int n;
};

/* The concrete :is()/:where() argument list (forward-declared above). */
struct sel_list {
  int n;
  struct selector *sels;
};

/* A parsed @media query the rule lives under (n == 0 → no query, always
 * applies). A query is an OR of comma-separated conditions; each condition is a
 * media type plus ANDed features, optionally `not`-negated. See
 * media_matches(). */
#define MAX_FEAT 4
#define MAX_COND 4
enum { MT_ALL, MT_SCREEN, MT_PRINT, MT_OTHER };
enum {
  MF_UNKNOWN,
  MF_MIN_W,
  MF_MAX_W,
  MF_W,
  MF_MIN_H,
  MF_MAX_H,
  MF_H,
  MF_ORIENT_PORTRAIT,
  MF_ORIENT_LANDSCAPE,
  MF_SCHEME_LIGHT,
  MF_SCHEME_DARK
};

struct media_feature {
  int kind;
  int val;
}; /* val = px (width/height only) */
struct media_cond {
  int negate; /* a leading `not` */
  int mtype;
  struct media_feature feat[MAX_FEAT];
  int nfeat;
};
struct media_query {
  struct media_cond cond[MAX_COND];
  int n;
};

#define CSS_RAW_MAX 256
/* Rule index key: a rule is bucketed by its rightmost compound's most specific
 * simple selector (id, else a class, else tag, else universal), so an element
 * only ever tests rules that can match it — without this, Tailwind's thousands
 * of utility rules made the cascade O(elements × all-rules) and pegged the CPU.
 *
 * A rule is filed under one or more keys (see rule_keys): a plain compound
 * gives a single id/class/tag key; a subject-position :is()/:where() with no
 * key of its own gives one key per branch (e.g. :is(a.x, b.y) -> classes x and
 * y), so it leaves the universal bucket and is tested only by elements those
 * branches can match. */
enum { KK_UNIV = 0, KK_ID, KK_CLS, KK_TAG };
struct css_rule {
  struct selector sel;
  struct css_style decl;
  struct media_query media;
  int spec, order, important;
  unsigned char has_var; /* a declaration uses var() -> resolve at fold */
  char raw[CSS_RAW_MAX]; /* authored declaration text (for devtools) */
  unsigned visit;        /* collect_candidates de-dup stamp (multi-key) */
  struct css_rule *next; /* document order (build + fallback iteration)  */
};

/* One index entry: a rule filed in a bucket under a specific key. `key` points
 * into the rule's own selector tree (stable for the rule's lifetime), so a rule
 * keyed under several selectors needs no string copies. */
struct css_idx {
  struct css_rule *rule;
  const char *key; /* the id/class/tag string, or 0 for universal */
  struct css_idx *next;
};

/* An @font-face: a declared family name, the raw TTF/OTF src url, and the base
 * URL of the stylesheet that declared it (a relative src resolves against this,
 * not the page — an external sheet's fonts live next to the sheet). */
struct css_face {
  char family[CSS_FAM_MAX];
  char *url;
  char *base;
  struct css_face *next;
};

/* A parsed @keyframes block: a name plus a set of stops, each an offset
 * (permille, 0..1000) and the declaration delta that applies at it, kept sorted
 * by offset. The animation engine (anim.c) finds the two stops bracketing a
 * progress and interpolates their animatable properties. */
#define CSS_KF_STOPS 16
struct css_keyframes {
  char name[CSS_ANIM_NAME];
  int nstops;
  short off[CSS_KF_STOPS]; /* permille 0..1000, ascending */
  struct css_style stop[CSS_KF_STOPS];
  struct css_keyframes *next;
};

#define CSS_NBUCK 256
struct css_sheet {
  struct css_rule *head, *tail;
  int order;
  int gen;
  struct css_face *faces;
  struct css_keyframes *keyframes; /* @keyframes blocks (name -> stops) */
  int has_pseudo_el;               /* set once any ::before/::after rule is pushed */
  const char *cur_base;            /* base URL for faces added by the in-progress css_add */
  /* Rule index (rebuilt lazily when `gen` changes; see ensure_index). Buckets
   * hold css_idx entries so one rule can be filed under several keys; every
   * entry lives in exactly one bucket chain, so teardown just walks them. */
  struct css_idx *id_b[CSS_NBUCK], *cls_b[CSS_NBUCK], *tag_b[CSS_NBUCK], *uni;
  int idx_gen;
  int idx_failed;
  struct css_rule **cand;
  int cand_cap;
  struct css_rule **match;
  int match_cap;
};

/* CSS custom-property table (built per active sheet; see "custom properties").
 */
#define CSS_VARS_MAX 256
#define CSS_VARNAME 64
#define CSS_VARVAL 220
static struct {
  char name[CSS_VARNAME], val[CSS_VARVAL];
} g_vars[CSS_VARS_MAX];
static int g_nvars;
static css_sheet *g_vars_sheet;
static int g_vars_gen = -1;
static int str_uses_var(const char *s);
static void resolve_vars(const char *in, int inlen, char *out, int *on, int cap, int depth);

/* Viewport the @media features test against; the browser refreshes it per
 * cascade. */
static int g_vw = 800, g_vh = 600;
static unsigned g_viewport_gen;
void css_set_viewport(int width, int height)
{
  int vw = width > 0 ? width : 0;
  int vh = height > 0 ? height : 0;
  if (vw != g_vw || vh != g_vh)
    g_viewport_gen++;
  g_vw = vw;
  g_vh = vh;
}
unsigned css_viewport_generation(void) { return g_viewport_gen; }

/* ------------------------------------------------ value parsing ------------
 */
/* Colour parsing lives in color.c now (shared with svg.c). */
static int parse_color(const char *v) { return color_parse(v); }

/* Root font size: the px basis for font-relative units (rem; em too — we don't
 * track an element-local font-size, so em uses the same basis). Keep in sync
 * with the renderer's default text size. */
#define CSS_ROOT_FONT_PX 16

/* calc()/min()/max()/clamp() evaluation. CSS forbids multiplying two lengths,
 * so any sub-expression stays a linear length a*px + b*% (plus a
 * dimensionless-number case for the scalar side of * and /). Percentages can't
 * be resolved to px at parse time (no containing block is known), so a result
 * keeps px and % separate and the caller maps it onto its box; for
 * min/max/clamp comparisons a % is measured against the viewport width as a
 * heuristic. */
struct calcval {
  double px, pct, num;
  int dimless;
};
struct calcctx {
  const char *p, *e;
  int ok, depth;
};

static void calc_skip(struct calcctx *c)
{
  while (c->p < c->e && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
    c->p++;
}

/* Parse a signed decimal at *pp into *out; 1 if at least one digit was
 * consumed. */
static int calc_number(const char **pp, double *out)
{
  const char *p = *pp;
  int neg = 0;
  if (*p == '-') {
    neg = 1;
    p++;
  } else if (*p == '+')
    p++;
  double n = 0;
  int any = 0;
  while (*p >= '0' && *p <= '9') {
    n = n * 10 + (*p++ - '0');
    any = 1;
  }
  if (*p == '.') {
    p++;
    double f = 0.1;
    while (*p >= '0' && *p <= '9') {
      n += (*p++ - '0') * f;
      f *= 0.1;
      any = 1;
    }
  }
  if (!any)
    return 0;
  *out = neg ? -n : n;
  *pp = p;
  return 1;
}

/* A number + unit -> a calcval. Unknown units degrade to a dimensionless
 * number. */
static void unit_value(double n, const char *u, struct calcval *o)
{
  o->px = o->pct = o->num = 0;
  o->dimless = 0;
  if (u[0] == '%')
    o->pct = n;
  else if (!u[0]) {
    o->dimless = 1;
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
    o->dimless = 1;
    o->num = n;
  }
}

/* A length magnitude for comparing min()/max()/clamp() arguments: a % is
 * measured against the viewport width (a heuristic — the real containing block
 * is unknown). */
static double calc_mag(const struct calcval *v)
{
  if (v->dimless)
    return v->num;
  double ref = g_vw > 0 ? g_vw : 800;
  return v->px + v->pct * 0.01 * ref;
}

static struct calcval calc_expr(struct calcctx *c); /* additive, the entry point */

/* Read comma-separated expression arguments up to (and past) the closing ')'.
 */
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
    c->ok = 0;
  return n;
}

static struct calcval calc_factor(struct calcctx *c)
{
  struct calcval v = {0, 0, 0, 0};
  calc_skip(c);
  if (c->p >= c->e || c->depth > 24) {
    c->ok = 0;
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
      c->ok = 0;
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
    c->ok = 0;
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
        c->ok = 0; /* length * length is invalid */
    } else {
      if (b.dimless && b.num != 0) {
        double k = 1.0 / b.num;
        a.px *= k;
        a.pct *= k;
        a.num *= k;
      } else
        c->ok = 0;
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
      c->ok = 0;
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

/* Map a calcval onto parse_len2's representation: px>=0 via return, else a
 * percent via *pct. A mixed px+% result (e.g. calc(100% - 1rem)) keeps only the
 * percent — we can't store an offset — which is the dominant term in practice.
 */
static int calc_result(const struct calcval *r, int ok, int *pct)
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

/* Parse a CSS length. Returns px>=0 (resolving px/rem/em/vw/vh/physical units
 * and calc()/min()/max()/clamp()); for "N%" or a percent-valued result, returns
 * -1 and sets *pct = N. For auto and anything unresolvable, returns -1 with
 * *pct = -1. */
static int parse_len2(const char *v, int *pct)
{
  *pct = -1;
  while (*v == ' ' || *v == '\t')
    v++;
  struct calcctx c = {v, v + strlen(v), 1, 0};
  struct calcval r = calc_factor(&c); /* one length token, or a calc/min/max/clamp */
  return calc_result(&r, c.ok, pct);
}

/* Parse a CSS length: integer px (or unitless), else -1 (%, em, auto, calc...).
 */
static int parse_len(const char *v)
{
  int pct;
  return parse_len2(v, &pct);
}

/* One length for a single box side: px>=0, -2 for "auto" (margins only), else
 * -1. */
static short one_len(const char *v, int allow_auto)
{
  while (*v == ' ' || *v == '\t')
    v++;
  if (allow_auto && starts_ci(v, "auto"))
    return -2;
  int pct, px = parse_len2(v, &pct);
  return (short)(px >= 0 ? px : -1);
}

/* Expand a 1-4 value box shorthand ("8px 16px") into out[4] = {T,R,B,L}. */
static void parse_box4(const char *v, short out[4], int allow_auto)
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

/* A `border` / `border-<side>` shorthand: pull a width (length or thin/medium/
 * thick; none/hidden = 0) and a colour out of the value; *w / *col stay -1 if
 * absent. Border style keywords (solid/dashed/...) are ignored. */
static void parse_border_val(const char *v, int *w, int *col)
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

/* SVG paint value: -2 none/transparent, -3 currentColor, 0xRRGGBB, or -1 if not
 * a recognised colour (e.g. url(#gradient) — left unset so it doesn't
 * override). */
static int parse_paint_val(const char *v)
{
  while (*v == ' ' || *v == '\t')
    v++;
  if (starts_ci(v, "none") || starts_ci(v, "transparent"))
    return -2;
  if (starts_ci(v, "currentcolor"))
    return -3;
  return parse_color(v);
}

static int parse_weight(const char *v)
{
  while (*v == ' ')
    v++;
  if (starts_ci(v, "bold"))
    return 1; /* bold, bolder */
  if (starts_ci(v, "normal") || starts_ci(v, "light"))
    return 0;
  int n = 0, any = 0;
  while (*v >= '0' && *v <= '9') {
    n = n * 10 + (*v++ - '0');
    any = 1;
  }
  if (any)
    return n >= 600 ? 1 : 0;
  return -1;
}

/* substring (ci) — for text-decoration:"underline" inside a multi-value list.
 */
static int has_ci(const char *hay, const char *needle)
{
  for (; *hay; hay++)
    if (starts_ci(hay, needle))
      return 1;
  return 0;
}

/* Decode a `content` value into out[cap]: a quoted string (with \HHHH unicode
 * and
 * \" / \\ escapes) -> UTF-8; none/normal/empty or a non-string
 * (attr()/counter()/ unresolved var) -> "" (rendered as nothing). */
static void parse_content(const char *v, char *out, int cap)
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

static int parse_disp(const char *v)
{
  while (*v == ' ')
    v++;
  if (starts_ci(v, "none"))
    return CSS_DISP_NONE;
  if (starts_ci(v, "inline"))
    return CSS_DISP_INLINE; /* incl inline-block */
  return CSS_DISP_BLOCK;    /* block/flex/grid/table */
}

/* ------------------------------------------------ backgrounds --------------
 */
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
static int is_grad_prefix(const char *p)
{
  return starts_ci(p, "linear-gradient(") || starts_ci(p, "radial-gradient(") ||
         starts_ci(p, "repeating-linear-gradient(") || starts_ci(p, "repeating-radial-gradient(");
}

/* background-repeat: a value (one or two keywords); -1 if none recognised. */
static int parse_bg_repeat(const char *v)
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
static void parse_bg_size(const char *v, signed char *kind, short *sw, short *sh)
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

/* background-position: 1-2 keyword/percentage tokens -> percent on each axis. A
 * pixel/length token degrades to 0% (we lack the box size at parse time). */
static void parse_bg_position(const char *v, short *px, short *py)
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
  int xset = 0, yset = 0;
  for (int i = 0; i < nt; i++) {
    const char *t = toks[i];
    if (starts_ci(t, "left")) {
      *px = 0;
      xset = 1;
    } else if (starts_ci(t, "right")) {
      *px = 100;
      xset = 1;
    } else if (starts_ci(t, "top")) {
      *py = 0;
      yset = 1;
    } else if (starts_ci(t, "bottom")) {
      *py = 100;
      yset = 1;
    } else if (starts_ci(t, "center")) { /* assigned to a free axis below */
    } else {
      int pct;
      parse_len2(t, &pct); /* pixel positions aren't supported (no box size here) -> 0% */
      short p = (short)(pct >= 0 ? (pct > 100 ? 100 : pct) : 0);
      if (i == 0 && !xset) {
        *px = p;
        xset = 1;
      } else if (!yset) {
        *py = p;
        yset = 1;
      }
    }
  }
  for (int i = 0; i < nt; i++)
    if (starts_ci(toks[i], "center")) {
      if (!xset) {
        *px = 50;
        xset = 1;
      } else if (!yset) {
        *py = 50;
        yset = 1;
      }
    }
  if (xset && !yset)
    *py = 50;
  if (yset && !xset)
    *px = 50;
}

/* One position token -> percent along its axis (0 horizontal, 1 vertical), or
 * -1. */
static int bg_pos_single(const char *t, int axis)
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
static int parse_lin_dir(const char *a, short *angle)
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
      return 0;
    *angle = (short)ang;
    return 1;
  }
  const char *p = a;
  int neg = 0;
  if (*p == '-') {
    neg = 1;
    p++;
  } else if (*p == '+')
    p++;
  if (!(*p >= '0' && *p <= '9'))
    return 0;
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
    return 0;
  if (neg)
    deg = -deg;
  deg %= 360;
  if (deg < 0)
    deg += 360;
  *angle = (short)deg;
  return 1;
}

/* A radial-gradient leading token (shape/size/position), e.g. "circle at
 * center". We only honour the shape; size/position degrade to a centred fill.
 */
static int parse_rad_dir(const char *a, struct css_bg_image *b)
{
  if (has_ci(a, "circle")) {
    b->rad_circle = 1;
    return 1;
  }
  if (has_ci(a, "ellipse")) {
    b->rad_circle = 0;
    return 1;
  }
  if (has_ci(a, "at ") || has_ci(a, "closest") || has_ci(a, "farthest"))
    return 1;
  return 0;
}

/* Append one "color [position]" gradient stop to b. */
static void add_gradient_stop(const char *arg, struct css_bg_image *b)
{
  if (b->nstops >= CSS_BG_STOPS)
    return;
  while (*arg == ' ' || *arg == '\t')
    arg++;
  /* A trailing space-separated number/percentage (at paren depth 0) is the
   * stop's position; everything before it is the colour (which may itself hold
   * spaces). */
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
static int parse_gradient(const char *v, struct css_bg_image *b)
{
  while (*v == ' ')
    v++;
  const char *p = v;
  if (starts_ci(p, "repeating-"))
    p += 10; /* treated as non-repeating */
  int linear;
  if (starts_ci(p, "linear-gradient(")) {
    linear = 1;
    p += 16;
  } else if (starts_ci(p, "radial-gradient(")) {
    linear = 0;
    p += 16;
  } else
    return 0;
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
  int first = 1;
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
    int consumed = 0;
    if (first)
      consumed = linear ? parse_lin_dir(arg, &b->angle) : parse_rad_dir(arg, b);
    if (!consumed)
      add_gradient_stop(arg, b);
    first = 0;
    a = (c < e) ? c + 1 : e;
  }
  return b->nstops > 0;
}

/* background-image: none / url(...) / a gradient. Takes the FIRST comma layer.
 */
static void parse_bg_image(const char *v, struct css_bg_image *b)
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

/* Find the first colour token in a background value, skipping url()/gradient()
 * functions and non-colour keywords. Returns 0xRRGGBB (+ alpha via *alpha) or
 * -1. */
static int bg_value_color(const char *v, int *alpha)
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

/* Collect the position-relevant tokens (keywords + percentages, before any '/')
 * out of a background shorthand and resolve them into b->pos_x/pos_y. */
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
    int is_pos = starts_ci(tok, "left") || starts_ci(tok, "right") || starts_ci(tok, "top") ||
                 starts_ci(tok, "bottom") || starts_ci(tok, "center");
    if (!is_pos) {
      const char *q = tok;
      if (*q == '-' || *q == '+')
        q++;
      if (*q >= '0' && *q <= '9')
        is_pos = 1;
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
static void parse_bg_shorthand(const char *v, struct css_style *out)
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

/* ------------------------------------------------ declarations -------------
 */
void css_style_reset(struct css_style *s)
{
  s->color = s->bg = -1;
  s->bold = s->under = -1;
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
  s->fam[0] = '\0';
}

/* Copy the FIRST font-family name from a value (up to a comma) into out[cap],
 * dequoted and lowercased. Used for an @font-face's own font-family (one name).
 */
static void first_family(const char *v, char *out, int cap)
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

/* Copy a whole font-family VALUE — the comma-separated fallback stack — into
 * out[cap]: each name dequoted + lowercased + space-trimmed, names joined by a
 * single comma (e.g. `"Helvetica Neue",Arial,sans-serif` -> `helvetica neue,
 * arial,sans-serif`). The browser walks this list and uses the first available
 * face, resolving generic keywords (sans-serif/system-ui/monospace/...). */
static void copy_family_stack(const char *v, char *out, int cap)
{
  int k = 0;
  while (*v && k < cap - 1) {
    while (*v == ' ' || *v == '\t' || *v == ',')
      v++; /* skip separators */
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
      k--; /* trim */
    while (*v == ' ' || *v == '\t')
      v++;
    if (*v == ',' && k > start && k < cap - 1)
      out[k++] = ','; /* join */
  }
  if (k > 0 && out[k - 1] == ',')
    k--; /* no trailing comma */
  out[k] = '\0';
}

/* Case-insensitive suffix test. */
static int ends_ci(const char *s, const char *suf)
{
  int sl = strlen(s), fl = strlen(suf);
  return sl >= fl && streq_ci(s + sl - fl, suf);
}

/* Lowercased-compare a span [a,ae) against a NUL-terminated lowercase literal.
 */
static int span_is(const char *a, const char *ae, const char *lit)
{
  while (a < ae && *lit && lower(*a) == *lit) {
    a++;
    lit++;
  }
  return a == ae && !*lit;
}

/* justify-content keyword -> enum css_justify (longest/most-specific first so
 * "flex-start" isn't mistaken for the "start" suffix of nothing, etc.). */
static signed char parse_justify(const char *v)
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

/* align-items keyword -> enum css_align (baseline approximated as start/top).
 */
static signed char parse_align(const char *v)
{
  if (has_ci(v, "center"))
    return CSS_ALIGN_CENTER;
  if (has_ci(v, "flex-end") || has_ci(v, "end"))
    return CSS_ALIGN_END;
  if (has_ci(v, "flex-start") || has_ci(v, "baseline") || has_ci(v, "start"))
    return CSS_ALIGN_START;
  return CSS_ALIGN_STRETCH; /* stretch / normal */
}

/* `gap: <row> [<col>]` (also row-gap/column-gap longhands handled by the
 * caller). */
/* Copy a token span [a,b) into a bounded, NUL-terminated stack buffer. */
static void tok_copy(const char *a, const char *b, char *buf, int cap)
{
  int k = 0;
  while (a < b && k < cap - 1)
    buf[k++] = *a++;
  buf[k] = '\0';
}

/* Classify one grid track token ("100px" / "1fr" / "20%" / "auto" / min-/max-
 * content) into *t. Unrecognised tokens fall back to an auto (content) track.
 */
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
    int fr = 0;
    const char *s = buf;
    while (*s >= '0' && *s <= '9')
      fr = fr * 10 + (*s++ - '0');
    t->kind = CSS_TRK_FR;
    t->v = (short)(fr > 0 ? fr : 1);
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
  /* else: leave as auto */
}

/* Parse a grid track list ("100px 1fr auto repeat(3, 1fr) 20%") into out[max],
 * expanding repeat(<count>, <tracks>). Returns the number of tracks emitted. */
static int parse_tracks(const char *v, struct css_track *out, int max)
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

/* grid-column / grid-row -> a span count (>=1). Handles `span <n>`, `<a> / <b>`
 * (span = b-a), `<a> / span <n>`, and bare/auto (span 1). */
static signed char parse_grid_span(const char *v)
{
  while (*v == ' ')
    v++;
  if (starts_ci(v, "span")) {
    v += 4;
    while (*v == ' ')
      v++;
    int n = 0;
    while (*v >= '0' && *v <= '9')
      n = n * 10 + (*v++ - '0');
    return (signed char)(n > 0 ? n : 1);
  }
  int a = 0, hasA = 0;
  while (*v >= '0' && *v <= '9') {
    a = a * 10 + (*v++ - '0');
    hasA = 1;
  }
  while (*v == ' ')
    v++;
  if (*v == '/') {
    v++;
    while (*v == ' ')
      v++;
    if (starts_ci(v, "span")) {
      v += 4;
      while (*v == ' ')
        v++;
      int n = 0;
      while (*v >= '0' && *v <= '9')
        n = n * 10 + (*v++ - '0');
      return (signed char)(n > 0 ? n : 1);
    }
    int b = 0, hasB = 0;
    while (*v >= '0' && *v <= '9') {
      b = b * 10 + (*v++ - '0');
      hasB = 1;
    }
    if (hasA && hasB && b > a)
      return (signed char)(b - a);
  }
  return 1;
}

static void parse_gap(const char *v, struct css_style *o)
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

/* `flex: none | auto | <grow> [<shrink>] [<basis>]`. A unitless token is grow
 * then shrink; a token carrying a unit/percent (or auto) is the basis. */
static void parse_flex_shorthand(const char *v, struct css_style *o)
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

  int seen_grow = 0, seen_shrink = 0, pct;
  while (*f) {
    while (*f == ' ' || *f == '\t')
      f++;
    if (!*f)
      break;
    int has_unit = 0;
    for (const char *q = f; *q && *q != ' ' && *q != '\t'; q++)
      if (*q == '%' || *q == 'p' || *q == 'e' || *q == 'r' || *q == 'v' || *q == 'c') {
        has_unit = 1;
        break;
      }
    int isnum = (*f >= '0' && *f <= '9') || *f == '.';
    if (isnum && !has_unit) { /* unitless: grow, then shrink */
      int n = parse_len(f);
      if (!seen_grow) {
        o->flex_grow = (short)(n < 0 ? 0 : n);
        seen_grow = 1;
      } else {
        o->flex_shrink = (short)(n < 0 ? 0 : n);
        seen_shrink = 1;
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

/* box-shadow: parse the FIRST layer's "[inset] dx dy [blur] [spread] color"
 * into o. inset (inner) shadows aren't drawn — skipped. A missing colour
 * defaults to a soft black, the usual look. Lengths are signed px (units beyond
 * px are ignored). */
static int sh_is_len(const char *t) { return (*t >= '0' && *t <= '9') || *t == '-' || *t == '+' || *t == '.'; }
static int sh_len(const char *t)
{
  int neg = 0;
  if (*t == '-') {
    neg = 1;
    t++;
  } else if (*t == '+')
    t++;
  int n = 0;
  while (*t >= '0' && *t <= '9')
    n = n * 10 + (*t++ - '0');
  return neg ? -n : n;
}
static void parse_box_shadow(const char *v, struct css_style *o)
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

/* list-style[-type]: the marker shape/number style. Returns CSS_LST_DEFAULT
 * when the value names no type keyword (e.g. a bare `list-style: inside`). */
static signed char parse_list_style(const char *v)
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

/* list-style[-position]: inside/outside. Returns CSS_LSP_DEFAULT when the value
 * names no position keyword (e.g. a bare `list-style: square`). */
static signed char parse_list_position(const char *v)
{
  if (has_ci(v, "inside"))
    return CSS_LSP_INSIDE;
  if (has_ci(v, "outside"))
    return CSS_LSP_OUTSIDE;
  return CSS_LSP_DEFAULT;
}

/* line-height: normal -> unset; a unitless multiplier -> lh_pct = N*100; a
 * length
 * -> px; a percent -> lh_pct. The unset case leaves the renderer's font-derived
 * height. */
static void parse_line_height(const char *v, struct css_style *o)
{
  while (*v == ' ' || *v == '\t')
    v++;
  if (starts_ci(v, "normal") || starts_ci(v, "inherit") || starts_ci(v, "initial")) {
    o->line_height = -1;
    o->lh_pct = -1;
    return;
  }
  int has_unit = 0;
  for (const char *q = v; *q && *q != ' ' && *q != '\t'; q++)
    if (*q == '%' || ((*q | 32) >= 'a' && (*q | 32) <= 'z')) {
      has_unit = 1;
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

/* font-size: the CSS absolute/relative keyword scale (most-specific first),
 * else a length/percent. em/rem resolve via parse_len2 (rem against the 16px
 * root; em approximated likewise, since no element-local size is tracked).
 * smaller/larger map to a percent of the inherited size. Leaves *o untouched if
 * unparseable. */
static void parse_font_size(const char *v, struct css_style *o)
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
  else {
    int q;
    int px = parse_len2(v, &q);
    if (px >= 0)
      o->font_px = px;
    else if (q >= 0)
      o->font_pct = (short)q;
  }
}

/* Does the whitespace-delimited token at p read as the `font` shorthand's
 * font-size, as opposed to a preceding numeric font-weight like `400`? A
 * font-size carries a unit, a decimal point, a percent, or a `/line-height`; a
 * bare 100..900 integer is a weight. Size keywords are matched by the caller.
 */
static int font_tok_is_size(const char *p)
{
  int digits = 0, dot = 0;
  const char *q = p;
  while ((*q >= '0' && *q <= '9') || *q == '.') {
    if (*q == '.')
      dot = 1;
    else
      digits++;
    q++;
  }
  if (!digits && !dot)
    return 0; /* not numeric */
  if (dot || *q == '/' || *q == '%')
    return 1; /* decimal / lh / percent */
  if (((*q | 32) >= 'a' && (*q | 32) <= 'z'))
    return 1; /* carries a unit */
  return 0;   /* bare integer: a weight */
}

/* The `font` shorthand: `[ style || variant || weight || stretch ]? size[/lh]
 * family`, plus the system-font keywords (caption/menu/…). Per spec the
 * shorthand resets its longhands, so an omitted style/variant/weight becomes
 * normal (not inherited) — but only once a valid font-size + family have been
 * recognised; a value we can't parse leaves the style untouched. */
static void parse_font_shorthand(const char *v, struct css_style *out)
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
    copy_family_stack("sans-serif", out->fam, CSS_FAM_STACK);
    return;
  }

  signed char italic = -1, scaps = -1, bold = -1;
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
    else if (starts_ci(p, "bold"))
      bold = 1; /* bold, bolder */
    else if (starts_ci(p, "lighter"))
      bold = 0;
    else if (font_tok_is_size(p) || starts_ci(p, "xx-small") || starts_ci(p, "x-small") || starts_ci(p, "smaller") ||
             starts_ci(p, "small") || starts_ci(p, "xx-large") || starts_ci(p, "x-large") || starts_ci(p, "larger") ||
             starts_ci(p, "large") || starts_ci(p, "medium")) {
      size_tok = p;
      break;
    } else if (*p >= '0' && *p <= '9') {
      int w = parse_weight(p);
      if (w >= 0)
        bold = w;
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
}

/* opacity: a 0..1 number (or an N% form) -> 0..100 percent, -1 if unparseable.
 */
static signed char parse_opacity(const char *v)
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

/* ------------------------------------------------ animation/transition -----
 */
/* A CSS <time> in ms (`1.5s`, `300ms`); -1 if `s` isn't a time (unitless etc.).
 */
static int parse_time_ms(const char *s, const char *e)
{
  while (s < e && (*s == ' ' || *s == '\t'))
    s++;
  const char *p = s;
  double n = 0;
  int any = 0, neg = 0;
  if (p < e && (*p == '-' || *p == '+')) {
    if (*p == '-')
      neg = 1;
    p++;
  }
  while (p < e && *p >= '0' && *p <= '9') {
    n = n * 10 + (*p++ - '0');
    any = 1;
  }
  if (p < e && *p == '.') {
    p++;
    double f = 0.1;
    while (p < e && *p >= '0' && *p <= '9') {
      n += (*p++ - '0') * f;
      f *= 0.1;
      any = 1;
    }
  }
  if (!any)
    return -1;
  if (p + 1 < e && (p[0] == 'm' || p[0] == 'M') && (p[1] == 's' || p[1] == 'S')) { /* ms */
  } else if (p < e && (*p == 's' || *p == 'S'))
    n *= 1000.0;
  else
    return -1; /* no time unit -> not a <time> */
  if (neg)
    return -1;
  return (int)(n + 0.5);
}

/* A CSS <easing-function> keyword span -> enum css_easing, or -1 if
 * unrecognised. cubic-bezier()/steps() degrade to the nearest preset (we don't
 * carry params on the style; anim.c re-derives bezier control points from the
 * preset). */
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

/* A transition-property name span -> a mask of (1<<ANP_*) bits (a shorthand
 * like `margin` expands to all four sides). `all` -> TRANS_ALL, `none`/unknown
 * -> 0. */
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

/* Split a value on top-level whitespace (never inside parentheses, so
 * `cubic-bezier(0, 0, 1, 1)` stays one token), calling `tok` per token. */
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
  /* a bare number is the iteration count */
  {
    const char *p = s;
    int any = 0;
    double n = 0;
    while (p < e && *p >= '0' && *p <= '9') {
      n = n * 10 + (*p++ - '0');
      any = 1;
    }
    if (p < e && *p == '.') {
      p++;
      double f = 0.1;
      while (p < e && *p >= '0' && *p <= '9') {
        n += (*p++ - '0') * f;
        f *= 0.1;
        any = 1;
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
  /* otherwise the animation-name */
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

static void apply_decl(const char *p, const char *pe, const char *v, const char *ve, struct css_style *out,
                       int *important)
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
        *important = 1;
      val[i] = '\0';
      while (i > 0 && (val[i - 1] == ' ' || val[i - 1] == '\t'))
        val[--i] = '\0';
      break;
    }
  }

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
  /* background-attachment / -origin / -clip parse-and-ignore (no box-model
     effect here) */
  else if (span_is(p, pe, "background-attachment") || span_is(p, pe, "background-origin") ||
           span_is(p, pe, "background-clip")) {
  } else if (span_is(p, pe, "font-weight"))
    out->bold = parse_weight(val);
  else if (span_is(p, pe, "text-decoration") || span_is(p, pe, "text-decoration-line"))
    out->under = has_ci(val, "underline") ? 1 : (has_ci(val, "none") ? 0 : out->under);
  else if (span_is(p, pe, "display")) {
    out->disp = parse_disp(val);
    out->flex = has_ci(val, "flex") ? 1 : 0;
    out->grid = has_ci(val, "grid") ? 1 : 0;
  } else if (span_is(p, pe, "text-align"))
    out->center = has_ci(val, "center")
                      ? 1
                      : (has_ci(val, "left") || has_ci(val, "right") || has_ci(val, "justify") ? 0 : out->center);
  else if (span_is(p, pe, "width")) {
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
    /* only the caps axis is renderable (as an uppercase approximation); the
     * ligature/numeric/east-asian/etc. sub-values are inert here. */
    if (has_ci(val, "small-caps") || has_ci(val, "all-small-caps") || has_ci(val, "petite-caps") ||
        has_ci(val, "all-petite-caps") || has_ci(val, "unicase") || has_ci(val, "titling-caps"))
      out->small_caps = 1;
    else if (has_ci(val, "normal") || has_ci(val, "none"))
      out->small_caps = 0;
  }
  /* Font longhands with no effect in this line-based renderer:
   * parsed-and-ignored so they neither error nor fall through to an unrelated
   * branch. */
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
    /* shorthand: a type keyword and/or a position keyword, any order. The
     * image component (none/url()) is captured by the type parse — a bare
     * url() leaves the type at DEFAULT so the ul/ol default marker shows as
     * the fallback, which is all this line-based renderer can draw. */
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
      int neg = 0;
      if (*f == '-') {
        neg = 1;
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
      int any = 0;
      double n = 0;
      while (*f >= '0' && *f <= '9') {
        n = n * 10 + (*f++ - '0');
        any = 1;
      }
      if (*f == '.') {
        f++;
        double fr = 0.1;
        while (*f >= '0' && *f <= '9') {
          n += (*f++ - '0') * fr;
          fr *= 0.1;
          any = 1;
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
  /* animation-play-state / -timeline parse-and-ignore (no paused state
     modelled) */
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
static void parse_decls(const char *s, const char *end, struct css_style *out, int *important)
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
  int imp = 0;
  if (str_uses_var(decls)) { /* resolve var() against the sheet table */
    char buf[512];
    int bn = 0;
    resolve_vars(decls, (int)strlen(decls), buf, &bn, (int)sizeof buf, 0);
    parse_decls(buf, buf + bn, out, &imp);
    return;
  }
  parse_decls(decls, decls + strlen(decls), out, &imp);
}

/* ------------------------------------------------ selectors ----------------
 */
static int parse_selector(const char *s, const char *e, struct selector *sel);
static struct sel_list *parse_sel_list(const char *s, const char *e);
static int selector_match(const struct selector *sel, const struct dom_node *el);
static void free_selector_lists(struct selector *sel);

/* True if byte c is in the NUL-terminated set. */
static int chr_in(const char *set, char c)
{
  for (; *set; set++)
    if (*set == c)
      return 1;
  return 0;
}

/* Advance from s to the first byte in `stop` that sits at bracket-nesting depth
 * 0. (…) and […] are skipped as a unit, so a comma / whitespace / '>' inside an
 * :is()/:not() argument list or an [attr="…"] value never splits a selector.
 * The one scanner shared by the selector-list, compound and :is()-list
 * splitters. */
static const char *scan_top(const char *s, const char *e, const char *stop)
{
  int depth = 0;
  for (; s < e; s++) {
    char c = *s;
    if (c == '(' || c == '[')
      depth++;
    else if (c == ')' || c == ']') {
      if (depth)
        depth--;
    } else if (depth == 0 && chr_in(stop, c))
      break;
  }
  return s;
}

static int is_sel_name(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/* ------------------------------------------------ pseudo-classes ----------- *
 * Structural/state pseudo-classes are matched against the DOM; dynamic ones
 * (:hover/:focus/…) and anything we can't evaluate become PC_NEVER so their
 * rule can't apply to a static render; :is()/:where() parse their argument into
 * a list of sub-selectors matched recursively (PC_MATCHES), matching nothing —
 * never the universal fallback — when the argument is empty or unparsable. */

/* an+b microsyntax (odd|even|<int>|an+b|-n+b) for the :nth-* pseudo-classes. */
static void parse_anb(const char *s, const char *e, int *a, int *b)
{
  *a = 0;
  *b = 0;
  while (s < e && (*s == ' ' || *s == '\t'))
    s++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
    e--;
  if (span_is(s, e, "odd")) {
    *a = 2;
    *b = 1;
    return;
  }
  if (span_is(s, e, "even")) {
    *a = 2;
    *b = 0;
    return;
  }
  int sign = 1, has1 = 0, num1 = 0;
  if (s < e && (*s == '+' || *s == '-')) {
    if (*s == '-')
      sign = -1;
    s++;
  }
  while (s < e && *s >= '0' && *s <= '9') {
    num1 = num1 * 10 + (*s++ - '0');
    has1 = 1;
  }
  if (s < e && (*s == 'n' || *s == 'N')) {
    s++;
    *a = sign * (has1 ? num1 : 1);
    while (s < e && (*s == ' ' || *s == '\t'))
      s++;
    int bsign = 1, has2 = 0, num2 = 0;
    if (s < e && (*s == '+' || *s == '-')) {
      if (*s == '-')
        bsign = -1;
      s++;
    }
    while (s < e && (*s == ' ' || *s == '\t'))
      s++;
    while (s < e && *s >= '0' && *s <= '9') {
      num2 = num2 * 10 + (*s++ - '0');
      has2 = 1;
    }
    *b = has2 ? bsign * num2 : 0;
  } else {
    *b = sign * num1;
  }
}

/* Parse a single simple selector (tag / #id / .class…, no combinators) for
 * :not(). */
static void parse_simple_sel(const char *s, const char *e, struct simple_sel *ss)
{
  ss->tag[0] = '\0';
  ss->id[0] = '\0';
  ss->ncls = 0;
  while (s < e) {
    if (*s == '.') {
      s++;
      char *d = ss->ncls < MAX_NOTCLS ? ss->cls[ss->ncls] : 0;
      int k = 0;
      while (s < e && is_sel_name(*s)) {
        if (d && k < 23)
          d[k++] = *s;
        s++;
      }
      if (d) {
        d[k] = '\0';
        if (k)
          ss->ncls++;
      }
    } else if (*s == '#') {
      s++;
      int k = 0;
      while (s < e && is_sel_name(*s)) {
        if (k < 23)
          ss->id[k++] = *s;
        s++;
      }
      ss->id[k] = '\0';
    } else if (*s == '*') {
      s++;
    } else if (is_sel_name(*s)) {
      int k = 0;
      while (s < e && is_sel_name(*s)) {
        if (k < 15)
          ss->tag[k++] = lower(*s);
        s++;
      }
      ss->tag[k] = '\0';
    } else
      s++;
  }
}

/* Classify a pseudo-class name span [ns,ne) (+ optional arg [as,ae)) into *pc.
 * Returns 1 to record *pc, 0 to ignore the pseudo entirely (:is()/:where()). */
static int classify_pseudo(const char *ns, const char *ne, const char *as, const char *ae, struct pseudo *pc)
{
  pc->kind = PC_NEVER;
  pc->a = 0;
  pc->b = 0;
  pc->list = 0;
  pc->arg.tag[0] = pc->arg.id[0] = '\0';
  pc->arg.ncls = 0;

  if (span_is(ns, ne, "not")) {
    pc->kind = PC_NOT;
    if (as)
      parse_simple_sel(as, ae, &pc->arg);
    return 1;
  }
  if (span_is(ns, ne, "first-child")) {
    pc->kind = PC_FIRST_CHILD;
    return 1;
  }
  if (span_is(ns, ne, "last-child")) {
    pc->kind = PC_LAST_CHILD;
    return 1;
  }
  if (span_is(ns, ne, "only-child")) {
    pc->kind = PC_ONLY_CHILD;
    return 1;
  }
  if (span_is(ns, ne, "first-of-type")) {
    pc->kind = PC_FIRST_OF_TYPE;
    return 1;
  }
  if (span_is(ns, ne, "last-of-type")) {
    pc->kind = PC_LAST_OF_TYPE;
    return 1;
  }
  if (span_is(ns, ne, "only-of-type")) {
    pc->kind = PC_ONLY_OF_TYPE;
    return 1;
  }
  if (span_is(ns, ne, "nth-child")) {
    pc->kind = PC_NTH_CHILD;
    if (as)
      parse_anb(as, ae, &pc->a, &pc->b);
    return 1;
  }
  if (span_is(ns, ne, "nth-last-child")) {
    pc->kind = PC_NTH_LAST_CHILD;
    if (as)
      parse_anb(as, ae, &pc->a, &pc->b);
    return 1;
  }
  if (span_is(ns, ne, "nth-of-type")) {
    pc->kind = PC_NTH_OF_TYPE;
    if (as)
      parse_anb(as, ae, &pc->a, &pc->b);
    return 1;
  }
  if (span_is(ns, ne, "nth-last-of-type")) {
    pc->kind = PC_NTH_LAST_OF_TYPE;
    if (as)
      parse_anb(as, ae, &pc->a, &pc->b);
    return 1;
  }
  if (span_is(ns, ne, "root")) {
    pc->kind = PC_ROOT;
    return 1;
  }
  if (span_is(ns, ne, "empty")) {
    pc->kind = PC_EMPTY;
    return 1;
  }
  if (span_is(ns, ne, "checked")) {
    pc->kind = PC_CHECKED;
    return 1;
  }
  if (span_is(ns, ne, "disabled")) {
    pc->kind = PC_DISABLED;
    return 1;
  }
  if (span_is(ns, ne, "enabled")) {
    pc->kind = PC_ENABLED;
    return 1;
  }
  if (span_is(ns, ne, "required")) {
    pc->kind = PC_REQUIRED;
    return 1;
  }
  if (span_is(ns, ne, "optional")) {
    pc->kind = PC_OPTIONAL;
    return 1;
  }
  if (span_is(ns, ne, "link") || span_is(ns, ne, "any-link")) {
    pc->kind = PC_LINK;
    return 1;
  }
  if (span_is(ns, ne, "is") || span_is(ns, ne, "where")) {
    /* :is()/:where() match if any sub-selector matches the element as its
     * subject. An empty or wholly-unparsable argument matches NOTHING (its rule
     * simply can't apply) — never the universal fallback a pass-through gave.
     */
    struct sel_list *l = (as && as < ae) ? parse_sel_list(as, ae) : 0;
    if (!l)
      return 1; /* PC_NEVER (matches nothing) */
    pc->kind = PC_MATCHES;
    pc->list = l;
    pc->a = span_is(ns, ne, "where"); /* :where() carries zero specificity */
    return 1;
  }
  return 1; /* dynamic state / :has() / unsupported -> PC_NEVER */
}

static void add_pseudo_class(struct sel_step *st, const char *ns, const char *ne, const char *as, const char *ae)
{
  if (st->npc >= MAX_PC)
    return;
  struct pseudo pc;
  if (classify_pseudo(ns, ne, as, ae, &pc))
    st->pc[st->npc++] = pc;
}

/* Parse one compound selector token (between combinators) into *st. */
static void parse_compound(const char *s, const char *e, struct sel_step *st)
{
  st->tag[0] = '\0';
  st->id[0] = '\0';
  st->ncls = 0;
  st->npc = 0;
  st->pseudo_el = 0;
  while (s < e) {
    if (*s == '.') {
      s++;
      char *d = (st->ncls < MAX_CLS) ? st->cls[st->ncls] : 0;
      int k = 0;
      while (s < e && is_sel_name(*s)) {
        if (d && k < 23)
          d[k++] = *s;
        s++;
      }
      if (d) {
        d[k] = '\0';
        if (k)
          st->ncls++;
      }
    } else if (*s == '#') {
      s++;
      int k = 0;
      while (s < e && is_sel_name(*s)) {
        if (k < 23)
          st->id[k++] = *s;
        s++;
      }
      st->id[k] = '\0';
    } else if (*s == ':') {
      s++;
      int dbl = 0;
      if (s < e && *s == ':') {
        s++;
        dbl = 1;
      }
      const char *ns = s;
      while (s < e && is_sel_name(*s))
        s++;
      const char *ne = s;
      const char *as = 0, *ae = 0; /* an optional ( ... ) argument */
      if (s < e && *s == '(') {
        int depth = 1;
        s++;
        as = s;
        while (s < e && depth) {
          if (*s == '(')
            depth++;
          else if (*s == ')') {
            depth--;
            if (!depth)
              break;
          }
          s++;
        }
        ae = s;
        if (s < e)
          s++;
      }
      /* A pseudo-ELEMENT styles generated content / a part, not the element's
       * own box: ::before/::after render content (PE_BEFORE/PE_AFTER); the rest
       * (::selection/::first-line/…) we don't render, so the rule must not
       * match a real element. A single-colon pseudo-CLASS is matched against
       * the DOM (structural/state) or flagged PC_NEVER (dynamic/unsupported).
       */
      if (span_is(ns, ne, "before"))
        st->pseudo_el = PE_BEFORE;
      else if (span_is(ns, ne, "after"))
        st->pseudo_el = PE_AFTER;
      else if (dbl || span_is(ns, ne, "first-line") || span_is(ns, ne, "first-letter"))
        st->pseudo_el = PE_OTHER;
      else
        add_pseudo_class(st, ns, ne, as, ae);
    } else if (*s == '*') {
      s++; /* universal: no constraint */
    } else if (is_sel_name(*s)) {
      int k = 0;
      while (s < e && is_sel_name(*s)) {
        if (k < 15)
          st->tag[k++] = lower(*s);
        s++;
      }
      st->tag[k] = '\0';
    } else {
      s++;
    }
  }
}

/* Parse one full selector (no commas) into *sel; returns 0 if empty/too deep.
 */
static int parse_selector(const char *s, const char *e, struct selector *sel)
{
  sel->n = 0;
  int pending_child = 0;
  while (s < e) {
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
      s++;
    if (s >= e)
      break;
    if (*s == '>') {
      pending_child = 1;
      s++;
      continue;
    }
    /* A compound runs to the next top-level combinator (space/'>'); one inside
     * (…)/[…] belongs to a pseudo argument or [attr] value — scan_top skips it,
     * so :is(.a > .b) and [x="a b"] stay a single compound. */
    const char *tok = s;
    s = scan_top(s, e, " \t\n\r>");
    if (sel->n >= MAX_STEPS)
      return sel->n > 0; /* cap depth, keep what we have (token dropped, unparsed) */
    struct sel_step *st = &sel->step[sel->n];
    parse_compound(tok, s, st);
    st->child = 0;
    if (sel->n > 0 && pending_child)
      sel->step[sel->n - 1].child = 1;
    pending_child = 0;
    sel->n++;
  }
  return sel->n > 0;
}

/* Recursively free the heap :is()/:where() sub-selector lists a selector owns.
 */
static void free_sel_list(struct sel_list *l);
static void free_selector_lists(struct selector *sel)
{
  for (int i = 0; i < sel->n; i++) {
    struct sel_step *st = &sel->step[i];
    for (int j = 0; j < st->npc; j++)
      if (st->pc[j].kind == PC_MATCHES && st->pc[j].list) {
        free_sel_list(st->pc[j].list);
        st->pc[j].list = 0;
      }
  }
}
static void free_sel_list(struct sel_list *l)
{
  if (!l)
    return;
  for (int i = 0; i < l->n; i++)
    free_selector_lists(&l->sels[i]);
  free(l->sels);
  free(l);
}

/* Split [s,e) on top-level commas (respecting ()/[]) and parse each piece as a
 * full selector, collecting the ones that parse into a heap list. Returns 0 if
 * none parse — so an :is()/:where() with no usable argument matches nothing,
 * not everything. A depth guard bounds nested :is(:is(:is(…))) against a
 * hostile sheet. */
static int g_is_depth;
static struct sel_list *parse_sel_list(const char *s, const char *e)
{
  if (g_is_depth >= 8)
    return 0;
  int cap = 1; /* pieces = top-level commas + 1 */
  for (const char *p = s; p < e;) {
    const char *c = scan_top(p, e, ",");
    if (c < e)
      cap++;
    p = (c < e) ? c + 1 : e;
  }
  struct selector *sels = malloc((unsigned)cap * sizeof *sels);
  if (!sels)
    return 0;
  int n = 0;
  g_is_depth++;
  for (const char *q = s; q < e && n < cap;) {
    const char *comma = scan_top(q, e, ",");
    if (parse_selector(q, comma, &sels[n]))
      n++;
    q = (comma < e) ? comma + 1 : e;
  }
  g_is_depth--;
  if (n == 0) {
    free(sels);
    return 0;
  }
  struct sel_list *l = malloc(sizeof *l);
  if (!l) {
    for (int i = 0; i < n; i++)
      free_selector_lists(&sels[i]);
    free(sels);
    return 0;
  }
  l->n = n;
  l->sels = sels;
  return l;
}

static int selector_spec(const struct selector *sel)
{
  int a = 0, b = 0, c = 0;
  for (int i = 0; i < sel->n; i++) {
    const struct sel_step *st = &sel->step[i];
    if (st->id[0])
      a++;
    b += st->ncls;
    if (st->tag[0])
      c++;
    for (int j = 0; j < st->npc; j++) { /* pseudo-classes add specificity */
      const struct pseudo *pc = &st->pc[j];
      if (pc->kind == PC_NOT) { /* :not() contributes its argument's */
        if (pc->arg.id[0])
          a++;
        b += pc->arg.ncls;
        if (pc->arg.tag[0])
          c++;
      } else if (pc->kind == PC_MATCHES) {
        /* :is() takes the specificity of its most-specific argument; :where()
         * (pc->a) contributes nothing at all. */
        if (!pc->a && pc->list) {
          int best = 0;
          for (int k = 0; k < pc->list->n; k++) {
            int s = selector_spec(&pc->list->sels[k]);
            if (s > best)
              best = s;
          }
          a += best / 10000;
          b += (best / 100) % 100;
          c += best % 100;
        }
      } else {
        b++; /* a pseudo-class counts like a class */
      }
    }
  }
  return a * 10000 + b * 100 + c;
}

/* ------------------------------------------------ matching -----------------
 */
static int node_has_class(const struct dom_node *n, const char *cls)
{
  const char *list = dom_get_attr(n, "class");
  if (!list)
    return 0;
  int cl = strlen(cls);
  for (const char *p = list; *p;) {
    while (*p == ' ' || *p == '\t')
      p++;
    const char *q = p;
    while (*q && *q != ' ' && *q != '\t')
      q++;
    if (q - p == cl) {
      int i = 0;
      while (i < cl && p[i] == cls[i])
        i++;
      if (i == cl)
        return 1;
    }
    p = q;
  }
  return 0;
}

/* ------------------------------------------------ pseudo-class matching ----
 */
/* idx satisfies an+b for some integer k >= 0. */
static int anb_match(int a, int b, int idx)
{
  int d = idx - b;
  if (a == 0)
    return d == 0;
  if ((a > 0 && d < 0) || (a < 0 && d > 0))
    return 0;
  return d % a == 0;
}
static int is_form_control(const char *tag)
{
  return streq(tag, "input") || streq(tag, "button") || streq(tag, "select") || streq(tag, "textarea") ||
         streq(tag, "option") || streq(tag, "optgroup") || streq(tag, "fieldset");
}
static int simple_match(const struct simple_sel *ss, const struct dom_node *n)
{
  if (!n || n->type != DOM_ELEMENT)
    return 0;
  if (ss->tag[0] && !streq(ss->tag, n->tag))
    return 0;
  if (ss->id[0]) {
    const char *id = dom_get_attr(n, "id");
    if (!id || !streq_ci(id, ss->id))
      return 0;
  }
  for (int i = 0; i < ss->ncls; i++)
    if (!node_has_class(n, ss->cls[i]))
      return 0;
  return 1;
}
static int pseudo_match(const struct pseudo *pc, const struct dom_node *n)
{
  switch (pc->kind) {
  case PC_FIRST_CHILD:
    return css_elem_index(n) == 1;
  case PC_LAST_CHILD:
    return css_elem_index(n) == css_elem_count(n);
  case PC_ONLY_CHILD:
    return css_elem_count(n) == 1;
  case PC_FIRST_OF_TYPE:
    return css_type_index(n) == 1;
  case PC_LAST_OF_TYPE:
    return css_type_index(n) == css_type_count(n);
  case PC_ONLY_OF_TYPE:
    return css_type_count(n) == 1;
  case PC_NTH_CHILD:
    return anb_match(pc->a, pc->b, css_elem_index(n));
  case PC_NTH_LAST_CHILD:
    return anb_match(pc->a, pc->b, css_elem_count(n) - css_elem_index(n) + 1);
  case PC_NTH_OF_TYPE:
    return anb_match(pc->a, pc->b, css_type_index(n));
  case PC_NTH_LAST_OF_TYPE:
    return anb_match(pc->a, pc->b, css_type_count(n) - css_type_index(n) + 1);
  case PC_ROOT:
    return n->parent && !n->parent->parent;
  case PC_EMPTY:
    for (const struct dom_node *c = n->first_child; c; c = c->next) {
      if (c->type == DOM_ELEMENT)
        return 0;
      if (c->type == DOM_TEXT && c->text)
        for (const char *t = c->text; *t; t++)
          if (*t != ' ' && *t != '\t' && *t != '\n' && *t != '\r' && *t != '\f')
            return 0;
    }
    return 1;
  case PC_CHECKED:
    return dom_get_attr(n, "checked") != 0 || dom_get_attr(n, "selected") != 0;
  case PC_DISABLED:
    return dom_get_attr(n, "disabled") != 0;
  case PC_ENABLED:
    return dom_get_attr(n, "disabled") == 0 && is_form_control(n->tag);
  case PC_REQUIRED:
    return dom_get_attr(n, "required") != 0;
  case PC_OPTIONAL:
    return dom_get_attr(n, "required") == 0 && is_form_control(n->tag);
  case PC_LINK:
    return (streq(n->tag, "a") || streq(n->tag, "area") || streq(n->tag, "link")) && dom_get_attr(n, "href") != 0;
  case PC_NOT:
    return !simple_match(&pc->arg, n);
  case PC_MATCHES:
    if (!pc->list)
      return 0;
    for (int i = 0; i < pc->list->n; i++)
      if (selector_match(&pc->list->sels[i], n))
        return 1;
    return 0;
  default:
    return 0; /* PC_NEVER */
  }
}

/* Match a step's tag/id/class/pseudo-class against `n` (ignoring any
 * pseudo-element). */
static int step_box_match(const struct sel_step *st, const struct dom_node *n)
{
  if (!n || n->type != DOM_ELEMENT)
    return 0;
  if (st->tag[0] && !streq(st->tag, n->tag))
    return 0;
  if (st->id[0]) {
    const char *id = dom_get_attr(n, "id");
    if (!id || !streq_ci(id, st->id))
      return 0;
  }
  for (int i = 0; i < st->ncls; i++)
    if (!node_has_class(n, st->cls[i]))
      return 0;
  for (int i = 0; i < st->npc; i++)
    if (!pseudo_match(&st->pc[i], n))
      return 0;
  return 1;
}

static int step_match(const struct sel_step *st, const struct dom_node *n)
{
  if (st->pseudo_el)
    return 0; /* ::before/::selection/… don't style a box */
  return step_box_match(st, n);
}

/* Match steps [0, top] of `sel` against the ancestors of `subject` (right to
 * left): a '>' step must match the immediate parent, a descendant step the
 * nearest matching ancestor. Greedy, no backtracking — enough for ordinary
 * selectors. Shared by the normal and ::before/::after matchers. */
static int match_ancestors(const struct selector *sel, int top, const struct dom_node *subject)
{
  const struct dom_node *cur = subject;
  for (int i = top; i >= 0; i--) {
    cur = cur ? cur->parent : 0;
    if (sel->step[i].child) {
      if (!step_match(&sel->step[i], cur))
        return 0;
    } else {
      while (cur && !step_match(&sel->step[i], cur))
        cur = cur->parent;
      if (!cur)
        return 0;
    }
  }
  return 1;
}

static int selector_match(const struct selector *sel, const struct dom_node *el)
{
  int i = sel->n - 1;
  if (!step_match(&sel->step[i], el))
    return 0;
  return match_ancestors(sel, i - 1, el);
}

/* Match a ::before / ::after rule against its originating element `el`: the
 * rightmost compound must carry that pseudo-element, its box part must match
 * el, and the ancestor chain matches as usual. */
static int selector_match_pseudo(const struct selector *sel, const struct dom_node *el, int which)
{
  int i = sel->n - 1;
  if (sel->step[i].pseudo_el != which)
    return 0;
  if (!step_box_match(&sel->step[i], el))
    return 0;
  return match_ancestors(sel, i - 1, el);
}

/* ------------------------------------------------ @media -------------------
 */
static const char *skip_sp(const char *p, const char *e)
{
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f'))
    p++;
  return p;
}

/* Match lowercase keyword `lit` at *pp (token-bounded); advance past it on a
 * hit. */
static int kw(const char **pp, const char *e, const char *lit)
{
  const char *q = *pp;
  while (q < e && *lit && lower(*q) == *lit) {
    q++;
    lit++;
  }
  if (*lit)
    return 0;
  if (q < e && is_sel_name(*q))
    return 0;
  *pp = q;
  return 1;
}

/* Parse a media length operand in [s,e) -> px (rounded to int); -1 if no
 * number. Width/height media features carry an integer px, so font- and
 * physical-unit lengths (em/rem, cm/mm/in/pt — common in framework breakpoints)
 * resolve against the same bases the cascade uses, and decimals (37.5em)
 * survive. */
static int parse_len_px(const char *s, const char *e)
{
  s = skip_sp(s, e);
  int neg = 0;
  if (s < e && (*s == '+' || *s == '-')) {
    neg = (*s == '-');
    s++;
  }
  double n = 0;
  int any = 0;
  while (s < e && *s >= '0' && *s <= '9') {
    n = n * 10 + (*s++ - '0');
    any = 1;
  }
  if (s < e && *s == '.') {
    s++;
    double f = 0.1;
    while (s < e && *s >= '0' && *s <= '9') {
      n += (*s++ - '0') * f;
      f *= 0.1;
      any = 1;
    }
  }
  if (!any)
    return -1;
  if (neg)
    n = -n;
  const char *u = skip_sp(s, e);
  double px;
  if (u >= e)
    px = n; /* unitless -> px */
  else if (starts_ci(u, "px"))
    px = n;
  else if (starts_ci(u, "rem"))
    px = n * CSS_ROOT_FONT_PX;
  else if (starts_ci(u, "em"))
    px = n * CSS_ROOT_FONT_PX;
  else if (starts_ci(u, "cm"))
    px = n * 96.0 / 2.54;
  else if (starts_ci(u, "mm"))
    px = n * 96.0 / 25.4;
  else if (starts_ci(u, "in"))
    px = n * 96.0;
  else if (starts_ci(u, "pt"))
    px = n * 96.0 / 72.0;
  else if (starts_ci(u, "pc"))
    px = n * 16.0;
  else
    px = n; /* unknown unit -> px */
  return (int)(px < 0 ? px - 0.5 : px + 0.5);
}

/* True if [a,e) trimmed equals lowercase literal `lit`. */
static int span_eq_trim(const char *a, const char *e, const char *lit)
{
  a = skip_sp(a, e);
  while (e > a && (e[-1] == ' ' || e[-1] == '\t'))
    e--;
  return span_is(a, e, lit);
}

/* ------- modern range syntax: (width >= 600px), (400px <= width <= 900px) ---
 */
enum { OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ };

/* width/height keyword in [s,e) -> 'w' / 'h' (0 = neither). */
static int range_dim(const char *s, const char *e)
{
  s = skip_sp(s, e);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
    e--;
  if (span_is(s, e, "width"))
    return 'w';
  if (span_is(s, e, "height"))
    return 'h';
  return 0;
}

static int op_flip(int op)
{
  switch (op) {
  case OP_LT:
    return OP_GT;
  case OP_LE:
    return OP_GE;
  case OP_GT:
    return OP_LT;
  case OP_GE:
    return OP_LE;
  default:
    return OP_EQ;
  }
}

/* Read a comparison operator at *pp (`<` `<=` `>` `>=` `=` `==`); -1 if none.
 */
static int read_op(const char **pp, const char *e)
{
  const char *p = *pp;
  int op = -1;
  if (p < e && *p == '<') {
    p++;
    if (p < e && *p == '=') {
      p++;
      op = OP_LE;
    } else
      op = OP_LT;
  } else if (p < e && *p == '>') {
    p++;
    if (p < e && *p == '=') {
      p++;
      op = OP_GE;
    } else
      op = OP_GT;
  } else if (p < e && *p == '=') {
    p++;
    if (p < e && *p == '=')
      p++;
    op = OP_EQ;
  }
  *pp = p;
  return op;
}

/* Add one bound `<feature> op <len>` (op as written with the feature on the
 * LEFT) to *c as a min/max/exact feature. Strict </> nudge the integer px
 * boundary by 1 so a viewport exactly on the edge compares correctly. */
static void range_bound(struct media_cond *c, int dim, int op, int len)
{
  if (len < 0 || c->nfeat >= MAX_FEAT)
    return;
  struct media_feature f = {MF_UNKNOWN, len};
  switch (op) {
  case OP_GE:
    f.kind = (dim == 'w') ? MF_MIN_W : MF_MIN_H;
    break;
  case OP_GT:
    f.kind = (dim == 'w') ? MF_MIN_W : MF_MIN_H;
    f.val = len + 1;
    break;
  case OP_LE:
    f.kind = (dim == 'w') ? MF_MAX_W : MF_MAX_H;
    break;
  case OP_LT:
    f.kind = (dim == 'w') ? MF_MAX_W : MF_MAX_H;
    f.val = len - 1;
    break;
  case OP_EQ:
    f.kind = (dim == 'w') ? MF_W : MF_H;
    break;
  }
  if (f.kind != MF_UNKNOWN)
    c->feat[c->nfeat++] = f;
}

/* Parse range syntax inside the parens [s,e): one comparison (feature on either
 * side) or a two-sided interval, emitting one or two min/max/exact features. */
static void parse_range(const char *s, const char *e, struct media_cond *c)
{
  const char *part_s[3], *part_e[3];
  int np = 0;
  int ops[2], no = 0;
  const char *p = skip_sp(s, e), *ts = p;
  while (p < e) {
    if (*p == '<' || *p == '>' || *p == '=') {
      if (np < 3) {
        part_s[np] = ts;
        part_e[np] = p;
        np++;
      }
      int op = read_op(&p, e);
      if (no < 2 && op >= 0)
        ops[no++] = op;
      ts = skip_sp(p, e);
      continue;
    }
    p++;
  }
  if (np < 3) {
    part_s[np] = ts;
    part_e[np] = e;
    np++;
  }

  if (np == 2 && no == 1) { /* A op B — feature on one side */
    int d0 = range_dim(part_s[0], part_e[0]);
    if (d0)
      range_bound(c, d0, ops[0], parse_len_px(part_s[1], part_e[1]));
    else {
      int d1 = range_dim(part_s[1], part_e[1]);
      if (d1)
        range_bound(c, d1, op_flip(ops[0]), parse_len_px(part_s[0], part_e[0]));
    }
  } else if (np == 3 && no == 2) { /* A op0 B op1 C — B the feature */
    int d = range_dim(part_s[1], part_e[1]);
    if (d) {
      range_bound(c, d, op_flip(ops[0]), parse_len_px(part_s[0], part_e[0]));
      range_bound(c, d, ops[1], parse_len_px(part_s[2], part_e[2]));
    }
  }
}

/* Parse one parenthesised media feature starting at *pp (which points at '(');
 * always advances past the matching ')'. Handles classic `(feature: value)` and
 * modern range syntax. A feature we don't support stays MF_UNKNOWN, which
 * evaluates false — per CSS, an unrecognised feature makes its query false (so
 * a block gated only on one we can't test won't apply). */
static void parse_feature(const char **pp, const char *e, struct media_cond *c)
{
  const char *p = *pp;
  if (p >= e || *p != '(') {
    *pp = e;
    return;
  }
  p++;
  const char *is = p;
  while (p < e && *p != ')')
    p++;
  const char *ie = p;
  if (p < e)
    p++;
  *pp = p;

  /* Distinguish classic (a ':') from range (a comparison operator). */
  const char *q = is;
  while (q < ie && *q != ':' && *q != '<' && *q != '>' && *q != '=')
    q++;
  if (q < ie && *q != ':') {
    parse_range(is, ie, c);
    return;
  }

  if (c->nfeat >= MAX_FEAT)
    return;
  const char *ns = skip_sp(is, ie), *ne = (q < ie) ? q : ie;
  const char *nend = ne;
  while (nend > ns && (nend[-1] == ' ' || nend[-1] == '\t'))
    nend--;
  const char *vs = (q < ie) ? q + 1 : ie, *ve = ie;

  struct media_feature f = {MF_UNKNOWN, 0};
  if (span_is(ns, nend, "min-width")) {
    f.kind = MF_MIN_W;
    f.val = parse_len_px(vs, ve);
  } else if (span_is(ns, nend, "max-width")) {
    f.kind = MF_MAX_W;
    f.val = parse_len_px(vs, ve);
  } else if (span_is(ns, nend, "width")) {
    f.kind = MF_W;
    f.val = parse_len_px(vs, ve);
  } else if (span_is(ns, nend, "min-height")) {
    f.kind = MF_MIN_H;
    f.val = parse_len_px(vs, ve);
  } else if (span_is(ns, nend, "max-height")) {
    f.kind = MF_MAX_H;
    f.val = parse_len_px(vs, ve);
  } else if (span_is(ns, nend, "height")) {
    f.kind = MF_H;
    f.val = parse_len_px(vs, ve);
  } else if (span_is(ns, nend, "orientation")) {
    if (span_eq_trim(vs, ve, "portrait"))
      f.kind = MF_ORIENT_PORTRAIT;
    else if (span_eq_trim(vs, ve, "landscape"))
      f.kind = MF_ORIENT_LANDSCAPE;
  } else if (span_is(ns, nend, "prefers-color-scheme")) {
    if (span_eq_trim(vs, ve, "dark"))
      f.kind = MF_SCHEME_DARK;
    else if (span_eq_trim(vs, ve, "light"))
      f.kind = MF_SCHEME_LIGHT;
  }
  if ((f.kind == MF_MIN_W || f.kind == MF_MAX_W || f.kind == MF_W || f.kind == MF_MIN_H || f.kind == MF_MAX_H ||
       f.kind == MF_H) &&
      f.val < 0)
    f.kind = MF_UNKNOWN; /* width/height feature with no usable number */
  c->feat[c->nfeat++] = f;
}

/* Parse one comma-free media query (`[not|only] type? (and feature)*`) into *c.
 */
static void parse_cond(const char *s, const char *e, struct media_cond *c)
{
  c->negate = 0;
  c->mtype = MT_ALL;
  c->nfeat = 0;
  const char *p = skip_sp(s, e);
  if (kw(&p, e, "not"))
    c->negate = 1;
  p = skip_sp(p, e);
  kw(&p, e, "only"); /* `only` is a no-op for us */
  p = skip_sp(p, e);
  if (p < e && *p != '(') { /* an explicit media type */
    if (kw(&p, e, "screen"))
      c->mtype = MT_SCREEN;
    else if (kw(&p, e, "all"))
      c->mtype = MT_ALL;
    else if (kw(&p, e, "print"))
      c->mtype = MT_PRINT;
    else {
      while (p < e && is_sel_name(*p))
        p++;
      c->mtype = MT_OTHER;
    }
  }
  for (;;) {
    p = skip_sp(p, e);
    if (p >= e)
      break;
    if (kw(&p, e, "and"))
      p = skip_sp(p, e);
    if (p < e && *p == '(')
      parse_feature(&p, e, c);
    else
      break;
  }
}

/* Parse a full @media header [s,e) (comma-separated condition list) into *mq.
 */
static void parse_media_query(const char *s, const char *e, struct media_query *mq)
{
  mq->n = 0;
  const char *p = s;
  while (p < e && mq->n < MAX_COND) {
    const char *comma = p;
    while (comma < e && *comma != ',')
      comma++;
    parse_cond(p, comma, &mq->cond[mq->n++]);
    p = (comma < e) ? comma + 1 : e;
  }
}

static int feature_matches(const struct media_feature *f)
{
  switch (f->kind) {
  case MF_MIN_W:
    return g_vw >= f->val;
  case MF_MAX_W:
    return g_vw <= f->val;
  case MF_W:
    return g_vw == f->val;
  case MF_MIN_H:
    return g_vh >= f->val;
  case MF_MAX_H:
    return g_vh <= f->val;
  case MF_H:
    return g_vh == f->val;
  case MF_ORIENT_PORTRAIT:
    return g_vh >= g_vw;
  case MF_ORIENT_LANDSCAPE:
    return g_vw > g_vh;
  case MF_SCHEME_LIGHT:
    return 1; /* the browser renders a light theme */
  case MF_SCHEME_DARK:
    return 0;
  default:
    return 0; /* MF_UNKNOWN / unsupported feature */
  }
}

static int cond_matches(const struct media_cond *c)
{
  int ok = (c->mtype == MT_ALL || c->mtype == MT_SCREEN);
  for (int i = 0; ok && i < c->nfeat; i++)
    if (!feature_matches(&c->feat[i]))
      ok = 0;
  return c->negate ? !ok : ok;
}

static int media_matches(const struct media_query *mq)
{
  if (mq->n == 0)
    return 1;
  for (int i = 0; i < mq->n; i++)
    if (cond_matches(&mq->cond[i]))
      return 1;
  return 0;
}

/* ------------------------------------------------ sheet build --------------
 */
css_sheet *css_new(void)
{
  css_sheet *s = malloc(sizeof *s);
  if (!s)
    return 0;
  s->head = s->tail = 0;
  s->order = 0;
  s->gen = 0;
  s->faces = 0;
  s->keyframes = 0;
  s->has_pseudo_el = 0;
  s->cur_base = 0;
  for (int i = 0; i < CSS_NBUCK; i++) {
    s->id_b[i] = s->cls_b[i] = s->tag_b[i] = 0;
  }
  s->uni = 0;
  s->idx_gen = -1; /* index is stale until first ensure_index */
  return s;
}

/* Pick the first url(...) in a src value naming a font we can decode: a raw
 * SFNT
 * (.ttf/.otf) or a WOFF/WOFF2 container (font_load decodes both via the woff.c
 * linked into the browser). Writes it to url[cap] (empty if none qualifies). */
static void parse_src(const char *v, char *url, int cap)
{
  url[0] = '\0';
  for (const char *p = v; *p;) {
    if (starts_ci(p, "url(")) {
      const char *u = p + 4;
      while (*u == ' ' || *u == '"' || *u == '\'')
        u++;
      char buf[512];
      int k = 0;
      while (*u && *u != ')' && *u != '"' && *u != '\'' && k < (int)sizeof buf - 1)
        buf[k++] = *u++;
      buf[k] = '\0';
      char base[512];
      int bk = 0; /* strip ?query / #frag */
      for (const char *q = buf; *q && *q != '?' && *q != '#' && bk < (int)sizeof base - 1; q++)
        base[bk++] = *q;
      base[bk] = '\0';
      if (ends_ci(base, ".ttf") || ends_ci(base, ".otf") || ends_ci(base, ".woff2") || ends_ci(base, ".woff")) {
        int j = 0;
        for (; buf[j] && j < cap - 1; j++)
          url[j] = buf[j];
        url[j] = '\0';
        return;
      }
      p = u;
    } else {
      p++;
    }
  }
}

/* Parse an @font-face block [s,end): pull out font-family + a usable src url
 * and, if both are present, register the face on the sheet. */
static void parse_fontface(css_sheet *sh, const char *s, const char *end)
{
  char family[CSS_FAM_MAX];
  family[0] = '\0';
  char url[512];
  url[0] = '\0';
  while (s < end) {
    const char *semi = s;
    while (semi < end && *semi != ';')
      semi++;
    const char *colon = s;
    while (colon < semi && *colon != ':')
      colon++;
    if (colon < semi) {
      const char *p = s, *pe = colon;
      while (p < pe && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
      while (pe > p && (pe[-1] == ' ' || pe[-1] == '\t'))
        pe--;
      const char *v = colon + 1, *ve = semi;
      while (v < ve && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r'))
        v++;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n' || ve[-1] == '\r'))
        ve--;
      char val[512];
      int vl = (int)(ve - v);
      if (vl > (int)sizeof val - 1)
        vl = (int)sizeof val - 1;
      for (int i = 0; i < vl; i++)
        val[i] = v[i];
      val[vl] = '\0';
      if (span_is(p, pe, "font-family"))
        first_family(val, family, CSS_FAM_MAX);
      else if (span_is(p, pe, "src"))
        parse_src(val, url, sizeof url);
    }
    s = (semi < end) ? semi + 1 : end;
  }
  if (!family[0] || !url[0])
    return;
  struct css_face *fc = malloc(sizeof *fc);
  if (!fc)
    return;
  int i = 0;
  for (; family[i]; i++)
    fc->family[i] = family[i];
  fc->family[i] = '\0';
  int ul = strlen(url);
  fc->url = malloc((unsigned)ul + 1);
  if (!fc->url) {
    free(fc);
    return;
  }
  for (i = 0; i <= ul; i++)
    fc->url[i] = url[i];
  fc->base = 0;
  if (sh->cur_base && sh->cur_base[0]) {
    int bl = strlen(sh->cur_base);
    fc->base = malloc((unsigned)bl + 1);
    if (fc->base)
      for (i = 0; i <= bl; i++)
        fc->base[i] = sh->cur_base[i];
  }
  fc->next = sh->faces;
  sh->faces = fc;
}

static const char *skip_ws_com(const char *p, const char *end); /* defined below */

/* A @keyframes stop selector: `from` -> 0, `to` -> 1000, `N%` -> N*10 permille;
 * -1 if it isn't a valid keyframe offset. */
static int keyframe_offset(const char *s, const char *e)
{
  while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
    s++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
    e--;
  if (span_is(s, e, "from"))
    return 0;
  if (span_is(s, e, "to"))
    return 1000;
  double n = 0;
  int any = 0;
  const char *p = s;
  while (p < e && *p >= '0' && *p <= '9') {
    n = n * 10 + (*p++ - '0');
    any = 1;
  }
  if (p < e && *p == '.') {
    p++;
    double f = 0.1;
    while (p < e && *p >= '0' && *p <= '9') {
      n += (*p++ - '0') * f;
      f *= 0.1;
      any = 1;
    }
  }
  if (!any || p >= e || *p != '%')
    return -1;
  int pm = (int)(n * 10 + 0.5);
  if (pm < 0)
    pm = 0;
  if (pm > 1000)
    pm = 1000;
  return pm;
}

/* Insert one stop (offset + declaration delta) into *kf, keeping off[]/stop[]
 * ascending by offset; a duplicate offset is overwritten (last wins, like CSS).
 */
static void keyframes_add_stop(struct css_keyframes *kf, int pm, const struct css_style *decl)
{
  for (int i = 0; i < kf->nstops; i++) {
    if (kf->off[i] == pm) {
      kf->stop[i] = *decl;
      return;
    }
  }
  if (kf->nstops >= CSS_KF_STOPS)
    return;
  int i = kf->nstops;
  while (i > 0 && kf->off[i - 1] > pm) {
    kf->off[i] = kf->off[i - 1];
    kf->stop[i] = kf->stop[i - 1];
    i--;
  }
  kf->off[i] = (short)pm;
  kf->stop[i] = *decl;
  kf->nstops++;
}

/* Parse a @keyframes block: `name` is [ns,ne); the brace body is [bs,be). Each
 * inner rule is a comma-separated list of offset selectors + a declaration
 * block, folded via the ordinary declaration parser. Registers the block on the
 * sheet if it yields at least one stop. */
static void parse_keyframes(css_sheet *sh, const char *ns, const char *ne, const char *bs, const char *be)
{
  while (ns < ne && (*ns == ' ' || *ns == '\t' || *ns == '\n' || *ns == '\r'))
    ns++;
  while (ne > ns && (ne[-1] == ' ' || ne[-1] == '\t' || ne[-1] == '\n' || ne[-1] == '\r'))
    ne--;
  /* an animation-name may be quoted ("spin"); dequote it */
  if (ne - ns >= 2 && (*ns == '"' || *ns == '\'') && ne[-1] == ns[0]) {
    ns++;
    ne--;
  }
  if (ns >= ne || (int)(ne - ns) >= CSS_ANIM_NAME)
    return;

  struct css_keyframes *kf = malloc(sizeof *kf);
  if (!kf)
    return;
  int ni = 0;
  for (; ns + ni < ne; ni++)
    kf->name[ni] = ns[ni];
  kf->name[ni] = '\0';
  kf->nstops = 0;
  kf->next = 0;

  const char *p = bs;
  while (p < be) {
    p = skip_ws_com(p, be);
    if (p >= be)
      break;
    const char *sel_s = p;
    while (p < be && *p != '{' && *p != '}')
      p++;
    if (p >= be || *p != '{')
      break;
    const char *sel_e = p++;
    const char *dec_s = p;
    int depth = 1;
    while (p < be && depth) {
      if (*p == '{')
        depth++;
      else if (*p == '}')
        depth--;
      if (depth)
        p++;
    }
    const char *dec_e = p;
    if (p < be)
      p++;

    struct css_style decl;
    css_style_reset(&decl);
    int imp = 0;
    parse_decls(dec_s, dec_e, &decl, &imp);

    const char *q = sel_s;
    while (q < sel_e) {
      const char *comma = q;
      while (comma < sel_e && *comma != ',')
        comma++;
      int pm = keyframe_offset(q, comma);
      if (pm >= 0)
        keyframes_add_stop(kf, pm, &decl);
      q = (comma < sel_e) ? comma + 1 : sel_e;
    }
  }

  if (kf->nstops == 0) {
    free(kf);
    return;
  }
  kf->next = sh->keyframes;
  sh->keyframes = kf;
}

/* ---- @keyframes public accessors (the animation engine, anim.c) ---- */
const struct css_keyframes *css_keyframes_find(css_sheet *sh, const char *name)
{
  if (!sh || !name || !name[0])
    return 0;
  for (struct css_keyframes *kf = sh->keyframes; kf; kf = kf->next)
    if (streq(kf->name, name))
      return kf;
  return 0;
}
int css_keyframes_count(const struct css_keyframes *kf) { return kf ? kf->nstops : 0; }
int css_keyframes_stop(const struct css_keyframes *kf, int i, int *permille, const struct css_style **decl)
{
  if (!kf || i < 0 || i >= kf->nstops)
    return 0;
  if (permille)
    *permille = kf->off[i];
  if (decl)
    *decl = &kf->stop[i];
  return 1;
}

static void sheet_push(css_sheet *sh, const struct selector *sel, const struct css_style *decl, int important,
                       const struct media_query *media, const char *dec_s, const char *dec_e)
{
  struct css_rule *r = malloc(sizeof *r);
  if (!r)
    return;
  r->sel = *sel;
  r->decl = *decl;
  r->media = *media;
  r->spec = selector_spec(sel);
  r->order = sh->order++;
  r->important = important;
  if (sel->step[sel->n - 1].pseudo_el == PE_BEFORE || sel->step[sel->n - 1].pseudo_el == PE_AFTER)
    sh->has_pseudo_el = 1; /* a ::before/::after rule exists in this sheet */
  r->visit = 0;            /* the index (ensure_index) files it by key */
  {                        /* copy the authored declarations, collapsing runs of whitespace */
    int k = 0, sp = 0;
    const char *q = dec_s;
    while (q < dec_e && k < CSS_RAW_MAX - 1) {
      char c = *q++;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
        sp = 1;
        continue;
      }
      if (sp && k > 0)
        r->raw[k++] = ' ';
      sp = 0;
      if (k < CSS_RAW_MAX - 1)
        r->raw[k++] = c;
    }
    r->raw[k] = '\0';
    {
      const char *q = r->raw;
      r->has_var = 0;
      for (; *q; q++)
        if ((*q == 'v' || *q == 'V') && starts_ci(q, "var(")) {
          r->has_var = 1;
          break;
        }
    }
  }
  r->next = 0;
  if (sh->tail)
    sh->tail->next = r;
  else
    sh->head = r;
  sh->tail = r;
}

static const char *skip_ws_com(const char *p, const char *end)
{
  for (;;) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f'))
      p++;
    if (p + 1 < end && p[0] == '/' && p[1] == '*') {
      p += 2;
      while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
        p++;
      p = (p + 1 < end) ? p + 2 : end;
      continue;
    }
    return p;
  }
}

static const char *skip_atrule(const char *p, const char *end)
{
  while (p < end && *p != ';' && *p != '{')
    p++;
  if (p < end && *p == ';')
    return p + 1;
  if (p < end && *p == '{') {
    int depth = 0;
    while (p < end) {
      if (*p == '{')
        depth++;
      else if (*p == '}') {
        depth--;
        if (depth == 0) {
          p++;
          break;
        }
      }
      p++;
    }
  }
  return p;
}

/* Parse the rules in [p,end) and push each under media query *mq. Recurses for
 * a nested @media block; nested queries don't intersect — the inner one
 * replaces an outer unconditional context, otherwise the outer (active) query
 * is kept, since
 * @media-inside-@media is vanishingly rare. */
static void add_rules(css_sheet *sh, const char *p, const char *end, const struct media_query *mq)
{
  while (p < end) {
    p = skip_ws_com(p, end);
    if (p >= end)
      break;
    if (*p == '@') {
      if (starts_ci(p, "@font-face")) { /* parse it, don't skip */
        const char *b = p;
        while (b < end && *b != '{' && *b != ';')
          b++;
        if (b < end && *b == '{') {
          const char *db = b + 1, *de = db;
          int depth = 1;
          while (de < end && depth) {
            if (*de == '{')
              depth++;
            else if (*de == '}')
              depth--;
            if (depth)
              de++;
          }
          parse_fontface(sh, db, de);
          p = (de < end) ? de + 1 : end;
          continue;
        }
      }
      if (starts_ci(p, "@keyframes") || starts_ci(p, "@-webkit-keyframes")) {
        const char *ns = p + (starts_ci(p, "@-webkit-keyframes") ? 18 : 10);
        const char *b = ns;
        while (b < end && *b != '{' && *b != ';')
          b++;
        if (b < end && *b == '{') {
          const char *db = b + 1, *de = db;
          int depth = 1;
          while (de < end && depth) {
            if (*de == '{')
              depth++;
            else if (*de == '}')
              depth--;
            if (depth)
              de++;
          }
          parse_keyframes(sh, ns, b, db, de);
          p = (de < end) ? de + 1 : end;
          continue;
        }
      }
      if (starts_ci(p, "@media")) {
        const char *hdr = p + 6, *b = hdr;
        while (b < end && *b != '{' && *b != '}' && *b != ';')
          b++;
        if (b < end && *b == '{') {
          struct media_query nested;
          parse_media_query(hdr, b, &nested);
          const char *db = b + 1, *de = db;
          int depth = 1;
          while (de < end && depth) {
            if (*de == '{')
              depth++;
            else if (*de == '}')
              depth--;
            if (depth)
              de++;
          }
          add_rules(sh, db, de, mq->n ? mq : &nested);
          p = (de < end) ? de + 1 : end;
          continue;
        }
      }
      /* @layer / @supports: flatten the inner rules into the current context.
       * @layer's cascade-priority ordering is approximated by source order
       * (good enough for Tailwind, which emits layers in priority order); a
       * @supports condition is treated as satisfied (its rules are the modern
       * branch, and any value we can't parse is ignored anyway). The bare
       * `@layer a, b;` order-declaration form has no block and falls through to
       * skip_atrule. Without this, Tailwind v4 — which wraps base resets, the
       * :root variables and every utility in @layer — renders almost nothing.
       */
      if (starts_ci(p, "@layer") || starts_ci(p, "@supports")) {
        const char *b = p;
        while (b < end && *b != '{' && *b != ';')
          b++;
        if (b < end && *b == '{') {
          const char *db = b + 1, *de = db;
          int depth = 1;
          while (de < end && depth) {
            if (*de == '{')
              depth++;
            else if (*de == '}')
              depth--;
            if (depth)
              de++;
          }
          add_rules(sh, db, de, mq);
          p = (de < end) ? de + 1 : end;
          continue;
        }
      }
      p = skip_atrule(p, end);
      continue;
    }

    const char *sel_s = p;
    while (p < end && *p != '{' && *p != '}')
      p++;
    if (p >= end || *p != '{') {
      if (p < end)
        p++;
      continue;
    }
    const char *sel_e = p++;
    const char *dec_s = p;
    while (p < end && *p != '}')
      p++;
    const char *dec_e = p;
    if (p < end)
      p++;

    struct css_style decl;
    css_style_reset(&decl);
    int important = 0;
    parse_decls(dec_s, dec_e, &decl, &important);

    /* Split the selector list on top-level commas only (scan_top skips a comma
     * inside an :is()/:not() list or an [attr] value). */
    for (const char *q = sel_s; q < sel_e;) {
      const char *comma = scan_top(q, sel_e, ",");
      struct selector sel;
      if (parse_selector(q, comma, &sel))
        sheet_push(sh, &sel, &decl, important, mq, dec_s, dec_e);
      else
        free_selector_lists(&sel); /* discard: reclaim any parsed :is() lists */
      q = (comma < sel_e) ? comma + 1 : sel_e;
    }
  }
}

void css_add(css_sheet *sh, const char *src, int len, const char *base)
{
  if (!sh)
    return;
  sh->cur_base = base; /* @font-face src resolves against this */
  struct media_query none = {.n = 0};
  add_rules(sh, src, src + len, &none);
  sh->cur_base = 0;
  sh->gen++; /* custom-property table is now stale */
}

static void index_clear(css_sheet *sh); /* defined with the rule index */

void css_free(css_sheet *sh)
{
  if (!sh)
    return;
  if (sh == g_vars_sheet) {
    g_vars_sheet = 0;
    g_vars_gen = -1;
    g_nvars = 0;
  } /* drop stale var cache */
  free(sh->cand);
  free(sh->match);
  index_clear(sh); /* free the css_idx entries before their rules */
  struct css_rule *r = sh->head;
  while (r) {
    struct css_rule *n = r->next;
    free_selector_lists(&r->sel);
    free(r);
    r = n;
  }
  struct css_face *fc = sh->faces;
  while (fc) {
    struct css_face *n = fc->next;
    free(fc->url);
    free(fc->base);
    free(fc);
    fc = n;
  }
  struct css_keyframes *kf = sh->keyframes;
  while (kf) {
    struct css_keyframes *n = kf->next;
    free(kf);
    kf = n;
  }
  free(sh);
}

int css_fontface_count(const css_sheet *sh)
{
  int n = 0;
  if (sh)
    for (struct css_face *fc = sh->faces; fc; fc = fc->next)
      n++;
  return n;
}

void css_fontface_at(const css_sheet *sh, int i, const char **family, const char **url, const char **base)
{
  *family = 0;
  *url = 0;
  if (base)
    *base = 0;
  if (!sh)
    return;
  for (struct css_face *fc = sh->faces; fc; fc = fc->next, i--)
    if (i == 0) {
      *family = fc->family;
      *url = fc->url;
      if (base)
        *base = fc->base;
      return;
    }
}

/* ------------------------------------------------ cascade ------------------
 */
static void fold(struct css_style *out, const struct css_style *d)
{
  if (d->color >= 0)
    out->color = d->color;
  if (d->bg >= 0)
    out->bg = d->bg;
  if (d->bold >= 0)
    out->bold = d->bold;
  if (d->under >= 0)
    out->under = d->under;
  if (d->disp)
    out->disp = d->disp;
  if (d->center >= 0)
    out->center = d->center;
  if (d->width >= 0) {
    out->width = d->width;
    out->pct_w = -1;
  } else if (d->pct_w >= 0) {
    out->pct_w = d->pct_w;
    out->width = -1;
  }
  if (d->height >= 0) {
    out->height = d->height;
    out->pct_h = -1;
  } else if (d->pct_h >= 0) {
    out->pct_h = d->pct_h;
    out->height = -1;
  }
  if (d->max_w >= 0) {
    out->max_w = d->max_w;
    out->pct_maxw = -1;
  } else if (d->pct_maxw >= 0) {
    out->pct_maxw = d->pct_maxw;
    out->max_w = -1;
  }
  if (d->max_h >= 0) {
    out->max_h = d->max_h;
    out->pct_maxh = -1;
  } else if (d->pct_maxh >= 0) {
    out->pct_maxh = d->pct_maxh;
    out->max_h = -1;
  }
  if (d->min_w >= 0) {
    out->min_w = d->min_w;
    out->pct_minw = -1;
  } else if (d->pct_minw >= 0) {
    out->pct_minw = d->pct_minw;
    out->min_w = -1;
  }
  if (d->min_h >= 0) {
    out->min_h = d->min_h;
    out->pct_minh = -1;
  } else if (d->pct_minh >= 0) {
    out->pct_minh = d->pct_minh;
    out->min_h = -1;
  }
  if (d->box_sizing >= 0)
    out->box_sizing = d->box_sizing;
  if (d->font_px >= 0) {
    out->font_px = d->font_px;
    out->font_pct = -1;
  } else if (d->font_pct >= 0) {
    out->font_pct = d->font_pct;
    out->font_px = -1;
  }
  if (d->radius >= 0)
    out->radius = d->radius;
  if (d->sh_color >= 0) {
    out->sh_color = d->sh_color;
    out->sh_dx = d->sh_dx;
    out->sh_dy = d->sh_dy;
    out->sh_blur = d->sh_blur;
    out->sh_spread = d->sh_spread;
  }
  for (int i = 0; i < 4; i++) {
    if (d->margin[i] != -1)
      out->margin[i] = d->margin[i];
    if (d->padding[i] != -1)
      out->padding[i] = d->padding[i];
    if (d->border_w[i] != -1)
      out->border_w[i] = d->border_w[i];
  }
  if (d->border_color >= 0)
    out->border_color = d->border_color;
  if (d->pos >= 0)
    out->pos = d->pos;
  if (d->top != -1)
    out->top = d->top;
  if (d->right != -1)
    out->right = d->right;
  if (d->bottom != -1)
    out->bottom = d->bottom;
  if (d->left != -1)
    out->left = d->left;
  if (d->flex >= 0)
    out->flex = d->flex;
  if (d->flex_dir >= 0)
    out->flex_dir = d->flex_dir;
  if (d->flex_wrap >= 0)
    out->flex_wrap = d->flex_wrap;
  if (d->flex_grow >= 0)
    out->flex_grow = d->flex_grow;
  if (d->flex_shrink >= 0)
    out->flex_shrink = d->flex_shrink;
  if (d->flex_basis >= 0) {
    out->flex_basis = d->flex_basis;
    out->pct_basis = -1;
  } else if (d->pct_basis >= 0) {
    out->pct_basis = d->pct_basis;
    out->flex_basis = -1;
  }
  if (d->justify >= 0)
    out->justify = d->justify;
  if (d->align_items >= 0)
    out->align_items = d->align_items;
  if (d->gap_row >= 0)
    out->gap_row = d->gap_row;
  if (d->gap_col >= 0)
    out->gap_col = d->gap_col;
  if (d->grid >= 0)
    out->grid = d->grid;
  if (d->grid_flow >= 0)
    out->grid_flow = d->grid_flow;
  if (d->n_cols >= 0) {
    out->n_cols = d->n_cols;
    for (int i = 0; i < d->n_cols && i < CSS_GRID_MAX; i++)
      out->grid_cols[i] = d->grid_cols[i];
  }
  if (d->n_rows >= 0) {
    out->n_rows = d->n_rows;
    for (int i = 0; i < d->n_rows && i < CSS_GRID_MAX; i++)
      out->grid_rows[i] = d->grid_rows[i];
  }
  if (d->col_span >= 0)
    out->col_span = d->col_span;
  if (d->row_span >= 0)
    out->row_span = d->row_span;
  if (d->justify_items >= 0)
    out->justify_items = d->justify_items;
  if (d->overflow >= 0)
    out->overflow = d->overflow;
  if (d->italic >= 0)
    out->italic = d->italic;
  if (d->small_caps >= 0)
    out->small_caps = d->small_caps;
  if (d->ttx >= 0)
    out->ttx = d->ttx;
  if (d->white_space >= 0)
    out->white_space = d->white_space;
  if (d->list_style != CSS_LST_DEFAULT)
    out->list_style = d->list_style;
  if (d->list_pos != CSS_LSP_DEFAULT)
    out->list_pos = d->list_pos;
  if (d->opacity >= 0)
    out->opacity = d->opacity;
  if (d->line_height >= 0) {
    out->line_height = d->line_height;
    out->lh_pct = -1;
  } else if (d->lh_pct >= 0) {
    out->lh_pct = d->lh_pct;
    out->line_height = -1;
  }
  if (d->outline_w >= 0)
    out->outline_w = d->outline_w;
  if (d->outline_color >= 0)
    out->outline_color = d->outline_color;
  if (d->z_index != CSS_Z_AUTO)
    out->z_index = d->z_index;
  if (d->fill != -1)
    out->fill = d->fill;
  if (d->stroke != -1)
    out->stroke = d->stroke;
  if (d->anim_name[0]) {
    int i = 0;
    for (; d->anim_name[i] && i < CSS_ANIM_NAME - 1; i++)
      out->anim_name[i] = d->anim_name[i];
    out->anim_name[i] = '\0';
  }
  if (d->anim_dur >= 0)
    out->anim_dur = d->anim_dur;
  if (d->anim_delay >= 0)
    out->anim_delay = d->anim_delay;
  if (d->anim_iter != -2)
    out->anim_iter = d->anim_iter;
  if (d->anim_dir >= 0)
    out->anim_dir = d->anim_dir;
  if (d->anim_fill >= 0)
    out->anim_fill = d->anim_fill;
  if (d->anim_timing >= 0)
    out->anim_timing = d->anim_timing;
  if (d->trans_mask)
    out->trans_mask = d->trans_mask;
  if (d->trans_dur >= 0)
    out->trans_dur = d->trans_dur;
  if (d->trans_delay >= 0)
    out->trans_delay = d->trans_delay;
  if (d->trans_timing >= 0)
    out->trans_timing = d->trans_timing;
  /* background image layer: the image identity (kind + gradient/url) folds as a
   * unit; repeat/size/position are independent longhands with their own
   * sentinels. */
  if (d->bgi.kind >= 0) {
    out->bgi.kind = d->bgi.kind;
    out->bgi.angle = d->bgi.angle;
    out->bgi.rad_circle = d->bgi.rad_circle;
    out->bgi.nstops = d->bgi.nstops;
    for (int i = 0; i < d->bgi.nstops; i++)
      out->bgi.stops[i] = d->bgi.stops[i];
    int i = 0;
    for (; d->bgi.url[i] && i < CSS_BG_URL - 1; i++)
      out->bgi.url[i] = d->bgi.url[i];
    out->bgi.url[i] = '\0';
  }
  if (d->bgi.repeat >= 0)
    out->bgi.repeat = d->bgi.repeat;
  if (d->bgi.size_kind >= 0) {
    out->bgi.size_kind = d->bgi.size_kind;
    out->bgi.size_w = d->bgi.size_w;
    out->bgi.size_h = d->bgi.size_h;
  }
  if (d->bgi.pos_x >= 0)
    out->bgi.pos_x = d->bgi.pos_x;
  if (d->bgi.pos_y >= 0)
    out->bgi.pos_y = d->bgi.pos_y;
  if (d->content[0]) {
    int i = 0;
    for (; d->content[i] && i < (int)sizeof out->content - 1; i++)
      out->content[i] = d->content[i];
    out->content[i] = '\0';
  }
  if (d->fam[0]) {
    int i = 0;
    for (; d->fam[i] && i < CSS_FAM_STACK - 1; i++)
      out->fam[i] = d->fam[i];
    out->fam[i] = '\0';
  }
}

/* ------------------------------------------------ custom properties -------- *
 * CSS variables. Authored `--name: value` declarations are collected sheet-wide
 * into a table (a pragmatic model: variables are nearly always defined on
 * :root, and we treat them as global, last-declaration-wins, honouring @media).
 * At fold time, a declaration that uses var(--name, fallback) is resolved
 * against this table (recursively) before it's interpreted. */
static const char *var_lookup(const char *name, int len)
{
  for (int i = 0; i < g_nvars; i++) {
    int k = 0;
    const char *nm = g_vars[i].name;
    while (k < len && nm[k] && nm[k] == name[k])
      k++;
    if (k == len && !nm[k])
      return g_vars[i].val;
  }
  return 0;
}
static void var_store(const char *name, int nlen, const char *val, int vlen)
{
  int slot = -1;
  if (nlen <= 0 || nlen >= CSS_VARNAME)
    return;
  for (int i = 0; i < g_nvars; i++) { /* later wins: overwrite */
    int k = 0;
    const char *nm = g_vars[i].name;
    while (k < nlen && nm[k] == name[k])
      k++;
    if (k == nlen && !nm[k]) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    if (g_nvars >= CSS_VARS_MAX)
      return;
    slot = g_nvars++;
  }
  {
    int i = 0;
    for (; i < nlen; i++)
      g_vars[slot].name[i] = name[i];
    g_vars[slot].name[i] = '\0';
  }
  if (vlen >= CSS_VARVAL)
    vlen = CSS_VARVAL - 1;
  {
    int i = 0;
    for (; i < vlen; i++)
      g_vars[slot].val[i] = val[i];
    g_vars[slot].val[i] = '\0';
  }
}

/* Pull every `--name: value` declaration out of `raw` into the table. */
static void vars_from_raw(const char *raw)
{
  const char *p = raw;
  while (*p) {
    const char *seg = p, *colon, *e;
    while (*p && *p != ';')
      p++;
    e = p;
    colon = seg;
    while (colon < e && *colon != ':')
      colon++;
    while (seg < colon && *seg == ' ')
      seg++;
    if (colon < e && seg + 1 < colon && seg[0] == '-' && seg[1] == '-') {
      const char *vs = colon + 1, *ve = e;
      while (vs < ve && *vs == ' ')
        vs++;
      while (ve > vs && ve[-1] == ' ')
        ve--;
      var_store(seg, (int)(colon - seg), vs, (int)(ve - vs));
    }
    if (*p == ';')
      p++;
  }
}
static void ensure_vars(css_sheet *sh)
{
  if (sh == g_vars_sheet && sh->gen == g_vars_gen)
    return;
  g_nvars = 0;
  for (struct css_rule *r = sh->head; r; r = r->next)
    if (media_matches(&r->media))
      vars_from_raw(r->raw); /* active rules only */
  g_vars_sheet = sh;
  g_vars_gen = sh->gen;
}

static int str_uses_var(const char *s)
{
  for (; *s; s++)
    if ((*s == 'v' || *s == 'V') && starts_ci(s, "var("))
      return 1;
  return 0;
}
/* Copy `in`, substituting var(--name, fallback) from the table (recursive,
 * depth- bounded). Unknown var with no fallback expands to empty, as in real
 * CSS. */
static void resolve_vars(const char *in, int inlen, char *out, int *on, int cap, int depth)
{
  int i = 0;
  while (i < inlen && *on < cap - 1) {
    if ((in[i] == 'v' || in[i] == 'V') && i + 4 <= inlen && starts_ci(in + i, "var(")) {
      int ns, ne, fbs = 0, fbe = 0, paren = 1;
      const char *val;
      i += 4;
      ns = i;
      while (i < inlen && in[i] != ',' && in[i] != ')')
        i++;
      ne = i;
      if (i < inlen && in[i] == ',') { /* fallback (balanced parens) */
        i++;
        fbs = i;
        while (i < inlen && paren) {
          if (in[i] == '(')
            paren++;
          else if (in[i] == ')') {
            paren--;
            if (!paren)
              break;
          }
          i++;
        }
        fbe = i;
      } else {
        while (i < inlen && in[i] != ')')
          i++;
      }
      if (i < inlen && in[i] == ')')
        i++;
      while (ns < ne && in[ns] == ' ')
        ns++;
      while (ne > ns && in[ne - 1] == ' ')
        ne--;
      val = var_lookup(in + ns, ne - ns);
      if (val && depth < 8)
        resolve_vars(val, (int)strlen(val), out, on, cap, depth + 1);
      else if (val) {
        for (int k = 0; val[k] && *on < cap - 1; k++)
          out[(*on)++] = val[k];
      } else if (fbe > fbs) {
        while (fbs < fbe && in[fbs] == ' ')
          fbs++;
        resolve_vars(in + fbs, fbe - fbs, out, on, cap, depth + 1);
      }
    } else {
      out[(*on)++] = in[i++];
    }
  }
  out[*on] = '\0';
}

/* ------------------------------------------------ rule index -------------- *
 * Bucket rules by their rightmost key so an element tests only the rules that
 * can possibly match it (its tag / each class / its id) plus the universal ones
 * — instead of every rule in the sheet. Rebuilt lazily when the sheet changes.
 */
static unsigned css_hash(const char *s)
{
  unsigned h = 5381;
  for (; *s; s++)
    h = h * 33u + (unsigned char)*s;
  return h % CSS_NBUCK;
}
/* Append the index key(s) under which the rightmost compound `st` must be
 * filed, each pointing into the selector tree (stable for the rule's life).
 * Returns 1 with
 * >=1 keys appended, or 0 if the compound can match an element with no usable
 * key (explicit *, an [attr]-only or bare dynamic-pseudo subject) — the caller
 * then files the whole rule universally. A subject :is()/:where() with no key
 * of its own contributes one key per branch, so :is(a.x, b.y) is filed under
 * classes x and y; if any branch is itself universal the whole rule falls back
 * to universal. */
struct idx_key {
  int kind;
  const char *str;
};
static int compound_keys(const struct sel_step *st, struct idx_key *out, int max, int *n)
{
  if (st->id[0]) {
    if (*n < max) {
      out[*n].kind = KK_ID;
      out[*n].str = st->id;
      (*n)++;
    }
    return 1;
  }
  if (st->ncls) {
    if (*n < max) {
      out[*n].kind = KK_CLS;
      out[*n].str = st->cls[0];
      (*n)++;
    }
    return 1;
  }
  if (st->tag[0]) {
    if (*n < max) {
      out[*n].kind = KK_TAG;
      out[*n].str = st->tag;
      (*n)++;
    }
    return 1;
  }
  const struct pseudo *pm = 0;
  for (int i = 0; i < st->npc; i++)
    if (st->pc[i].kind == PC_MATCHES && st->pc[i].list) {
      pm = &st->pc[i];
      break;
    }
  if (!pm)
    return 0; /* :hover-only / [attr] / * subject -> universal */
  for (int i = 0; i < pm->list->n; i++) {
    const struct selector *sub = &pm->list->sels[i];
    if (!compound_keys(&sub->step[sub->n - 1], out, max, n))
      return 0; /* a branch matches anything */
  }
  return 1;
}

static void index_clear(css_sheet *sh)
{
  struct css_idx **tabs[3] = {sh->id_b, sh->cls_b, sh->tag_b};
  for (int t = 0; t < 3; t++)
    for (int i = 0; i < CSS_NBUCK; i++) {
      for (struct css_idx *e = tabs[t][i]; e;) {
        struct css_idx *nx = e->next;
        free(e);
        e = nx;
      }
      tabs[t][i] = 0;
    }
  for (struct css_idx *e = sh->uni; e;) {
    struct css_idx *nx = e->next;
    free(e);
    e = nx;
  }
  sh->uni = 0;
}

static int index_put(struct css_idx **head, struct css_rule *r, const char *key)
{
  struct css_idx *e = malloc(sizeof *e);
  if (!e)
    return 0;
  e->rule = r;
  e->key = key;
  e->next = *head;
  *head = e;
  return 1;
}

static void ensure_index(css_sheet *sh)
{
  if (sh->idx_gen == sh->gen)
    return;
  index_clear(sh);
  sh->idx_failed = 0;
  for (struct css_rule *r = sh->head; r; r = r->next) {
    struct idx_key keys[32];
    int nk = 0;
    if (!compound_keys(&r->sel.step[r->sel.n - 1], keys, 32, &nk) || nk == 0) {
      if (!index_put(&sh->uni, r, 0)) {
        sh->idx_failed = 1;
        break;
      }
      continue;
    }
    for (int i = 0; i < nk; i++) {
      struct css_idx **tab = keys[i].kind == KK_ID ? sh->id_b : keys[i].kind == KK_CLS ? sh->cls_b : sh->tag_b;
      if (!index_put(&tab[css_hash(keys[i].str)], r, keys[i].str)) {
        sh->idx_failed = 1;
        break;
      }
    }
    if (sh->idx_failed)
      break;
  }
  if (sh->idx_failed)
    index_clear(sh);
  sh->idx_gen = sh->gen;
}

/* Copy `s` truncated to 23 chars (the max stored key width) into out[24] so
 * element keys hash to the same bucket as the rule keys, which are capped the
 * same at parse. */
static void key23(const char *s, char *out)
{
  int k = 0;
  for (; s[k] && k < 23; k++)
    out[k] = s[k];
  out[k] = '\0';
}

static int rule_ptr_room(struct css_rule ***buf, int *cap, int need)
{
  if (need <= *cap)
    return 1;
  int nc = *cap ? *cap * 2 : 64;
  while (nc < need)
    nc *= 2;
  struct css_rule **p = realloc(*buf, (unsigned)nc * sizeof *p);
  if (!p)
    return 0;
  *buf = p;
  *cap = nc;
  return 1;
}

/* Candidate rules for `el`: the universal bucket + its tag/id/class buckets. A
 * rule may be filed under several keys (an :is()/:where() subject), so a
 * per-rule visit stamp de-dups; selector_match still confirms each (the buckets
 * are a superset). */
static unsigned g_visit;
static int collect_candidates(css_sheet *sh, const struct dom_node *el)
{
  ensure_index(sh);
  unsigned stamp = ++g_visit;
  int n = 0, failed = 0;
#define ADD(rp)                                                                                                        \
  do {                                                                                                                 \
    struct css_rule *rr = (rp);                                                                                        \
    if (rr->visit != stamp) {                                                                                          \
      rr->visit = stamp;                                                                                               \
      if (rule_ptr_room(&sh->cand, &sh->cand_cap, n + 1))                                                              \
        sh->cand[n++] = rr;                                                                                             \
      else                                                                                                             \
        failed = 1;                                                                                                    \
    }                                                                                                                  \
  } while (0)
  if (sh->idx_failed)
    for (struct css_rule *r = sh->head; r; r = r->next)
      ADD(r);
  for (struct css_idx *e = sh->uni; e; e = e->next)
    ADD(e->rule);
  if (el->tag[0])
    for (struct css_idx *e = sh->tag_b[css_hash(el->tag)]; e; e = e->next)
      if (streq(e->key, el->tag))
        ADD(e->rule);
  const char *id = dom_get_attr(el, "id");
  if (id && id[0]) {
    char b[24];
    key23(id, b);
    for (struct css_idx *e = sh->id_b[css_hash(b)]; e; e = e->next)
      if (streq_ci(e->key, b))
        ADD(e->rule);
  }
  const char *cl = dom_get_attr(el, "class");
  if (cl)
    for (const char *p = cl; *p;) {
      while (*p == ' ' || *p == '\t')
        p++;
      char b[24];
      int k = 0;
      while (*p && *p != ' ' && *p != '\t') {
        if (k < 23)
          b[k++] = *p;
        p++;
      }
      b[k] = '\0';
      if (k)
        for (struct css_idx *e = sh->cls_b[css_hash(b)]; e; e = e->next)
          if (streq(e->key, b))
            ADD(e->rule);
    }
#undef ADD
  return failed ? -1 : n;
}

void css_apply_sheet(css_sheet *sh, const struct dom_node *el, struct css_style *out)
{
  if (!sh || !el || el->type != DOM_ELEMENT)
    return;
  ensure_vars(sh);

  /* Collect matches, then fold in ascending priority (important, spec, order)
   * so the winner is applied last. */
  int nc = collect_candidates(sh, el), nm = 0;
  if (nc < 0)
    return;
  for (int i = 0; i < nc; i++)
    if (media_matches(&sh->cand[i]->media) && selector_match(&sh->cand[i]->sel, el)) {
      if (!rule_ptr_room(&sh->match, &sh->match_cap, nm + 1))
        return;
      sh->match[nm++] = sh->cand[i];
    }

  for (int i = 1; i < nm; i++) { /* insertion sort, stable enough */
    struct css_rule *k = sh->match[i];
    int j = i - 1;
    while (j >= 0) {
      struct css_rule *m = sh->match[j];
      int hi = (m->important != k->important) ? (m->important > k->important)
               : (m->spec != k->spec)         ? (m->spec > k->spec)
                                              : (m->order > k->order);
      if (!hi)
        break;
      sh->match[j + 1] = m;
      j--;
    }
    sh->match[j + 1] = k;
  }
  for (int i = 0; i < nm; i++) {
    struct css_rule *r = sh->match[i];
    if (r->has_var) { /* re-parse with var() substituted */
      char buf[512];
      int bn = 0;
      struct css_style tmp;
      int imp = 0;
      resolve_vars(r->raw, (int)strlen(r->raw), buf, &bn, (int)sizeof buf, 0);
      css_style_reset(&tmp);
      parse_decls(buf, buf + bn, &tmp, &imp);
      fold(out, &tmp);
    } else
      fold(out, &r->decl);
  }
}

int css_pseudo_content(css_sheet *sh, const struct dom_node *el, int which, struct css_style *out)
{
  css_style_reset(out);
  if (!sh || !el || el->type != DOM_ELEMENT)
    return 0;
  ensure_vars(sh);
  int nc = collect_candidates(sh, el), nm = 0;
  if (nc < 0)
    return 0;
  for (int i = 0; i < nc; i++)
    if (media_matches(&sh->cand[i]->media) && selector_match_pseudo(&sh->cand[i]->sel, el, which)) {
      if (!rule_ptr_room(&sh->match, &sh->match_cap, nm + 1))
        return 0;
      sh->match[nm++] = sh->cand[i];
    }
  for (int i = 1; i < nm; i++) { /* ascending priority: winner folds last */
    struct css_rule *k = sh->match[i];
    int j = i - 1;
    while (j >= 0) {
      struct css_rule *m = sh->match[j];
      int hi = (m->important != k->important) ? (m->important > k->important)
               : (m->spec != k->spec)         ? (m->spec > k->spec)
                                              : (m->order > k->order);
      if (!hi)
        break;
      sh->match[j + 1] = m;
      j--;
    }
    sh->match[j + 1] = k;
  }
  for (int i = 0; i < nm; i++) {
    struct css_rule *r = sh->match[i];
    if (r->has_var) {
      char buf[512];
      int bn = 0;
      struct css_style tmp;
      int imp = 0;
      resolve_vars(r->raw, (int)strlen(r->raw), buf, &bn, (int)sizeof buf, 0);
      css_style_reset(&tmp);
      parse_decls(buf, buf + bn, &tmp, &imp);
      fold(out, &tmp);
    } else
      fold(out, &r->decl);
  }
  return out->content[0] != '\0';
}

int css_sheet_has_pseudo(const css_sheet *sh) { return sh && sh->has_pseudo_el; }

void css_apply_attrs(const struct dom_node *el, struct css_style *out)
{
  if (!el || el->type != DOM_ELEMENT)
    return;
  const char *c = dom_get_attr(el, "color");
  if (c) {
    int rgb = parse_color(c);
    if (rgb >= 0)
      out->color = rgb;
  }
  const char *bg = dom_get_attr(el, "bgcolor");
  if (bg) {
    int rgb = parse_color(bg);
    if (rgb >= 0)
      out->bg = rgb;
  }
  const char *w = dom_get_attr(el, "width");
  if (w) {
    int px = parse_len(w);
    if (px >= 0)
      out->width = px;
  }
  const char *h = dom_get_attr(el, "height");
  if (h) {
    int px = parse_len(h);
    if (px >= 0)
      out->height = px;
  }
}

/* ------------------------------------------------ page background (alpha) -- *
 * A background-color may be translucent; per CSS it composites over what's
 * behind it (an element's box over its ancestors; the root's over the canvas).
 * css_bg_argb returns an element's cascade-winning background as 0xAARRGGBB via
 * *out, and the caller composites <html> then <body> over a white canvas — so a
 * Tailwind /var(--tw-bg-opacity) tint blends rather than being treated as
 * opaque. The colour + its alpha both come from color_parse_a (color.c). */

/* Pull the (last) background[-color] value out of a declaration block,
 * resolving var() against the sheet table, into out[cap]. Returns 1 if one was
 * present. */
static int extract_bg(const char *decls, char *out, int cap)
{
  const char *bv = 0, *bve = 0, *s = decls;
  while (*s) {
    const char *semi = s;
    while (*semi && *semi != ';')
      semi++;
    const char *colon = s;
    while (colon < semi && *colon != ':')
      colon++;
    if (colon < semi) {
      const char *p = s, *pe = colon;
      while (p < pe && (*p == ' ' || *p == '\t'))
        p++;
      while (pe > p && (pe[-1] == ' ' || pe[-1] == '\t'))
        pe--;
      if (span_is(p, pe, "background-color") || span_is(p, pe, "background")) {
        bv = colon + 1;
        bve = semi;
      }
    }
    s = *semi ? semi + 1 : semi;
  }
  if (!bv)
    return 0;
  while (bv < bve && (*bv == ' ' || *bv == '\t'))
    bv++;
  while (bve > bv && (bve[-1] == ' ' || bve[-1] == '\t'))
    bve--;
  int on = 0;
  resolve_vars(bv, (int)(bve - bv), out, &on, cap, 0); /* verbatim copy when no var() */
  return 1;
}

int css_bg_argb(css_sheet *sh, const struct dom_node *el, unsigned *out)
{
  if (!sh || !el || el->type != DOM_ELEMENT)
    return 0;
  ensure_vars(sh);
  char val[160];

  /* highest priority first: inline style="", then sheet rules, then bgcolor=""
   */
  const char *inl = dom_get_attr(el, "style");
  if (inl && extract_bg(inl, val, sizeof val)) {
    int a;
    int c = bg_value_color(val, &a);
    if (c >= 0) {
      *out = ((unsigned)a << 24) | (unsigned)c;
      return 1;
    }
  }

  int nc = collect_candidates(sh, el), nm = 0;
  if (nc < 0)
    return 0;
  for (int i = 0; i < nc; i++)
    if (media_matches(&sh->cand[i]->media) && selector_match(&sh->cand[i]->sel, el)) {
      if (!rule_ptr_room(&sh->match, &sh->match_cap, nm + 1))
        return 0;
      sh->match[nm++] = sh->cand[i];
    }
  for (int i = 1; i < nm; i++) { /* sort highest priority first */
    struct css_rule *k = sh->match[i];
    int j = i - 1;
    while (j >= 0) {
      struct css_rule *m = sh->match[j];
      int klo = (m->important != k->important) ? (k->important > m->important)
                : (m->spec != k->spec)         ? (k->spec > m->spec)
                                               : (k->order > m->order);
      if (!klo)
        break;
      sh->match[j + 1] = m;
      j--;
    }
    sh->match[j + 1] = k;
  }
  for (int i = 0; i < nm; i++)
    if (extract_bg(sh->match[i]->raw, val, sizeof val)) {
      int a;
      int c = bg_value_color(val, &a);
      if (c >= 0) {
        *out = ((unsigned)a << 24) | (unsigned)c;
        return 1;
      }
    }

  const char *bgc = dom_get_attr(el, "bgcolor");
  if (bgc) {
    int c = parse_color(bgc);
    if (c >= 0) {
      *out = 0xff000000u | (unsigned)c;
      return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------ devtools: matched rules ---
 */
/* Tiny bounded string appenders for reconstructing selector/media text. */
static void cm_ap(char *o, int *k, int cap, const char *s)
{
  while (*s && *k < cap - 1)
    o[(*k)++] = *s++;
  o[*k] = '\0';
}
static void cm_apn(char *o, int *k, int cap, int v)
{
  char b[12];
  int n = 0;
  if (v < 0) {
    cm_ap(o, k, cap, "-");
    v = -v;
  }
  if (v == 0) {
    cm_ap(o, k, cap, "0");
    return;
  }
  while (v) {
    b[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n && *k < cap - 1)
    o[(*k)++] = b[--n];
  o[*k] = '\0';
}

/* Reconstruct a readable selector ("div.a#b > span") from the parsed steps. */
static void sel_text(const struct selector *sel, char *o, int cap)
{
  int k = 0;
  if (cap)
    o[0] = '\0';
  for (int i = 0; i < sel->n; i++) {
    if (i)
      cm_ap(o, &k, cap, sel->step[i - 1].child ? " > " : " ");
    const struct sel_step *st = &sel->step[i];
    int start = k;
    if (st->tag[0])
      cm_ap(o, &k, cap, st->tag);
    if (st->id[0]) {
      cm_ap(o, &k, cap, "#");
      cm_ap(o, &k, cap, st->id);
    }
    for (int c = 0; c < st->ncls; c++) {
      cm_ap(o, &k, cap, ".");
      cm_ap(o, &k, cap, st->cls[c]);
    }
    for (int j = 0; j < st->npc; j++) /* keep :is()/:where() visible, not a bare * */
      if (st->pc[j].kind == PC_MATCHES && st->pc[j].list) {
        cm_ap(o, &k, cap, st->pc[j].a ? ":where(" : ":is(");
        for (int m = 0; m < st->pc[j].list->n; m++) {
          if (m)
            cm_ap(o, &k, cap, ", ");
          char sub[128];
          sel_text(&st->pc[j].list->sels[m], sub, (int)sizeof sub);
          cm_ap(o, &k, cap, sub);
        }
        cm_ap(o, &k, cap, ")");
      }
    if (k == start)
      cm_ap(o, &k, cap, "*");
  }
}

/* Reconstruct a readable @media prelude ("" when the rule is unconditional). */
static void media_text(const struct media_query *mq, char *o, int cap)
{
  int k = 0;
  if (cap)
    o[0] = '\0';
  if (mq->n == 0)
    return;
  cm_ap(o, &k, cap, "@media ");
  for (int c = 0; c < mq->n; c++) {
    const struct media_cond *cd = &mq->cond[c];
    if (c)
      cm_ap(o, &k, cap, ", ");
    if (cd->negate)
      cm_ap(o, &k, cap, "not ");
    int need_and = 0;
    const char *mt = cd->mtype == MT_SCREEN ? "screen" : cd->mtype == MT_PRINT ? "print" : 0;
    if (mt) {
      cm_ap(o, &k, cap, mt);
      need_and = 1;
    }
    for (int f = 0; f < cd->nfeat; f++) {
      if (need_and)
        cm_ap(o, &k, cap, " and ");
      need_and = 1;
      cm_ap(o, &k, cap, "(");
      const struct media_feature *ft = &cd->feat[f];
      switch (ft->kind) {
      case MF_MIN_W:
        cm_ap(o, &k, cap, "min-width: ");
        cm_apn(o, &k, cap, ft->val);
        cm_ap(o, &k, cap, "px");
        break;
      case MF_MAX_W:
        cm_ap(o, &k, cap, "max-width: ");
        cm_apn(o, &k, cap, ft->val);
        cm_ap(o, &k, cap, "px");
        break;
      case MF_W:
        cm_ap(o, &k, cap, "width: ");
        cm_apn(o, &k, cap, ft->val);
        cm_ap(o, &k, cap, "px");
        break;
      case MF_MIN_H:
        cm_ap(o, &k, cap, "min-height: ");
        cm_apn(o, &k, cap, ft->val);
        cm_ap(o, &k, cap, "px");
        break;
      case MF_MAX_H:
        cm_ap(o, &k, cap, "max-height: ");
        cm_apn(o, &k, cap, ft->val);
        cm_ap(o, &k, cap, "px");
        break;
      case MF_H:
        cm_ap(o, &k, cap, "height: ");
        cm_apn(o, &k, cap, ft->val);
        cm_ap(o, &k, cap, "px");
        break;
      case MF_ORIENT_PORTRAIT:
        cm_ap(o, &k, cap, "orientation: portrait");
        break;
      case MF_ORIENT_LANDSCAPE:
        cm_ap(o, &k, cap, "orientation: landscape");
        break;
      case MF_SCHEME_DARK:
        cm_ap(o, &k, cap, "prefers-color-scheme: dark");
        break;
      case MF_SCHEME_LIGHT:
        cm_ap(o, &k, cap, "prefers-color-scheme: light");
        break;
      default:
        cm_ap(o, &k, cap, "...");
      }
      cm_ap(o, &k, cap, ")");
    }
  }
}

int css_matches(css_sheet *sh, const struct dom_node *el, struct css_match *out, int cap)
{
  if (!sh || !el || el->type != DOM_ELEMENT || cap <= 0)
    return 0;
  int nc = collect_candidates(sh, el), nm = 0;
  if (nc < 0)
    return 0;
  for (int i = 0; i < nc; i++)
    if (selector_match(&sh->cand[i]->sel, el)) {
      if (!rule_ptr_room(&sh->match, &sh->match_cap, nm + 1))
        return 0;
      sh->match[nm++] = sh->cand[i];
    }

  /* Most-specific / last-declared first (winner on top, like a real devtools).
   */
  for (int i = 1; i < nm; i++) {
    struct css_rule *k = sh->match[i];
    int j = i - 1;
    while (j >= 0) {
      struct css_rule *m = sh->match[j];
      int klo = (m->important != k->important) ? (k->important > m->important)
                : (m->spec != k->spec)         ? (k->spec > m->spec)
                                               : (k->order > m->order);
      if (!klo)
        break;
      sh->match[j + 1] = m;
      j--;
    }
    sh->match[j + 1] = k;
  }

  int n = 0;
  for (int i = 0; i < nm && n < cap; i++) {
    struct css_rule *r = sh->match[i];
    sel_text(&r->sel, out[n].selector, (int)sizeof out[n].selector);
    media_text(&r->media, out[n].media, (int)sizeof out[n].media);
    out[n].active = media_matches(&r->media);
    out[n].decl = r->decl;
    out[n].raw = r->raw;
    n++;
  }
  return n;
}
