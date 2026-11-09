/* svg_style.c: style sheets, property lookup and style resolution. */
#include "features/svg/svg_int.h"

/* Last `prop: value` in a declaration block; ';' inside parentheses does not end a declaration. */
static bool decl_prop(const char *s, int len, const char *prop, char *out, int cap)
{
  bool found = false;
  int i = 0, pl = (int)strlen(prop);
  while (i < len) {
    while (i < len && (svg_is_ws(s[i]) || s[i] == ';'))
      i++;
    int ns = i;
    while (i < len && s[i] != ':' && s[i] != ';')
      i++;
    int ne = i;
    while (ne > ns && svg_is_ws(s[ne - 1]))
      ne--;
    if (i >= len || s[i] != ':')
      continue;
    i++;
    while (i < len && s[i] == ' ')
      i++;
    int vs = i, depth = 0;
    while (i < len && !(s[i] == ';' && depth == 0)) {
      if (s[i] == '(')
        depth++;
      else if (s[i] == ')' && depth > 0)
        depth--;
      i++;
    }
    int ve = i;
    while (ve > vs && svg_is_ws(s[ve - 1]))
      ve--;
    if (ne - ns != pl || !eq_ci_n(s + ns, prop, pl))
      continue;
    for (int k = vs; k + 10 <= ve; k++)
      if (s[k] == '!' && eq_ci_n(s + k, "!important", 10)) {
        ve = k;
        while (ve > vs && svg_is_ws(s[ve - 1]))
          ve--;
        break;
      }
    int k = ve - vs < cap - 1 ? ve - vs : cap - 1;
    memcpy(out, s + vs, (unsigned)k);
    out[k] = '\0';
    found = true;
  }
  return found;
}

static bool has_class(const struct dom_node *n, const char *cls)
{
  const char *v = dom_get_attr(n, "class");
  int cl = (int)strlen(cls);
  while (v && *v) {
    while (*v == ' ' || *v == '\t' || *v == '\n')
      v++;
    const char *t = v;
    while (*v && *v != ' ' && *v != '\t' && *v != '\n')
      v++;
    if ((int)(v - t) == cl && eq_ci_n(t, cls, cl))
      return true;
  }
  return false;
}

static bool rule_matches(const struct rule *r, const struct dom_node *n)
{
  if (r->tag[0] && !streq_ci(r->tag, n->tag))
    return false;
  if (r->id[0]) {
    const char *id = dom_get_attr(n, "id");
    if (!id || !streq(id, r->id))
      return false;
  }
  return !r->cls[0] || has_class(n, r->cls);
}

/* Property value from style="", then <style> rules by specificity, then the presentation attribute. */
bool svg_get_prop(const struct ctx *cx, const struct dom_node *n, const char *name, char *out, int cap)
{
  const char *st = dom_get_attr(n, "style");
  if (st && decl_prop(st, (int)strlen(st), name, out, cap))
    return true;
  int best = -1;
  char tmp[256];
  for (int i = 0; i < cx->nrules; i++) {
    const struct rule *r = &cx->rules[i];
    if (r->spec >= best && rule_matches(r, n) && decl_prop(r->decl, r->dlen, name, tmp, sizeof tmp)) {
      best = r->spec;
      strlcpy(out, tmp, cap);
    }
  }
  if (best >= 0)
    return true;
  const char *v = dom_get_attr(n, name);
  if (!v)
    return false;
  strlcpy(out, v, cap);
  return true;
}

static bool is_ident_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/* One simple selector (`tag`, `.cls`, `#id`, `tag.cls`, `*`); anything with combinators or pseudos is dropped. */
static void add_rule(struct ctx *cx, const char *s, const char *e, const char *decl, int dlen)
{
  while (s < e && svg_is_ws(*s))
    s++;
  while (e > s && svg_is_ws(e[-1]))
    e--;
  if (s >= e || cx->nrules >= MAX_RULES)
    return;
  struct rule r;
  memset(&r, 0, sizeof r);
  const char *p = s;
  if (*p == '*')
    p++;
  else {
    int k = 0;
    while (p < e && is_ident_char(*p) && k < (int)sizeof r.tag - 1)
      r.tag[k++] = *p++;
  }
  while (p < e) {
    char kind = *p++;
    char *dst = kind == '.' ? r.cls : kind == '#' ? r.id : 0;
    if (!dst || dst[0])
      return;
    int k = 0;
    while (p < e && is_ident_char(*p) && k < 63)
      dst[k++] = *p++;
    if (!k)
      return;
  }
  if (p != e)
    return;
  r.spec = (r.id[0] ? 100 : 0) + (r.cls[0] ? 10 : 0) + (r.tag[0] ? 1 : 0);
  r.decl = decl;
  r.dlen = dlen;
  if (!cx->rules)
    cx->rules = malloc(MAX_RULES * sizeof *cx->rules);
  if (cx->rules)
    cx->rules[cx->nrules++] = r;
}

static void parse_sheet(struct ctx *cx, const char *p)
{
  while (*p) {
    if (svg_is_ws(*p) || *p == '}') {
      p++;
    } else if (p[0] == '/' && p[1] == '*') {
      p += 2;
      while (*p && !(p[0] == '*' && p[1] == '/'))
        p++;
      p += *p ? 2 : 0;
    } else if (starts_ci(p, "<!--") || starts_ci(p, "-->")) {
      p += starts_ci(p, "<!--") ? 4 : 3;
    } else if (starts_ci(p, "<![CDATA[")) {
      p += 9;
    } else if (starts_ci(p, "]]>")) {
      p += 3;
    } else if (*p == '@') {
      while (*p && *p != ';' && *p != '{')
        p++;
      int depth = 0;
      while (*p == '{' || depth > 0) {
        if (*p == '{')
          depth++;
        else if (*p == '}')
          depth--;
        if (!*p)
          break;
        p++;
      }
      if (*p == ';')
        p++;
    } else {
      const char *sel = p;
      while (*p && *p != '{')
        p++;
      if (!*p)
        return;
      const char *selend = p, *body = ++p;
      while (*p && *p != '}')
        p++;
      int dlen = (int)(p - body);
      for (const char *a = sel; a <= selend;) {
        const char *b = a;
        while (b < selend && *b != ',')
          b++;
        add_rule(cx, a, b, body, dlen);
        a = b + 1;
      }
    }
  }
}

void svg_collect(struct ctx *cx, const struct dom_node *n, int depth)
{
  if (depth > 512)
    return;
  for (const struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    const char *id = dom_get_attr(c, "id");
    if (id && *id) {
      if (cx->nids >= cx->idcap) {
        int nc = cx->idcap ? cx->idcap * 2 : 64;
        struct idref *ni = realloc(cx->ids, (unsigned)nc * sizeof *ni);
        if (!ni)
          return;
        cx->ids = ni;
        cx->idcap = nc;
      }
      cx->ids[cx->nids].id = id;
      cx->ids[cx->nids++].n = c;
    }
    if (streq(c->tag, "style")) {
      const char *type = dom_get_attr(c, "type");
      if (!type || !*type || contains_ci(type, "css"))
        for (const struct dom_node *t = c->first_child; t; t = t->next)
          if (t->type == DOM_TEXT && t->text)
            parse_sheet(cx, t->text);
    }
    svg_collect(cx, c, depth + 1);
  }
}

static const struct dom_node *find_id(const struct ctx *cx, const char *id, int len)
{
  for (int i = 0; i < cx->nids; i++)
    if ((int)strlen(cx->ids[i].id) == len && !memcmp(cx->ids[i].id, id, (unsigned)len))
      return cx->ids[i].n;
  return 0;
}

/* Element a `url(#id)` or `#id` reference points at. */
const struct dom_node *svg_ref_target(const struct ctx *cx, const char *v)
{
  const char *h = strchr(v, '#');
  if (!h)
    return 0;
  h++;
  int len = 0;
  while (h[len] && h[len] != ')' && h[len] != '"' && h[len] != '\'' && h[len] != '&' && !svg_is_ws(h[len]))
    len++;
  return find_id(cx, h, len);
}

const struct dom_node *svg_href_target(const struct ctx *cx, const struct dom_node *n)
{
  const char *h = dom_get_attr(n, "href");
  if (!h)
    h = dom_get_attr(n, "xlink:href");
  return h && *h == '#' ? svg_ref_target(cx, h) : 0;
}

/* style resolution */
static bool parse_paint(const struct ctx *cx, const char *v, struct paint *p)
{
  while (*v == ' ' || *v == '\t')
    v++;
  memset(p, 0, sizeof *p);
  if (starts_ci(v, "none") || starts_ci(v, "transparent")) {
    p->kind = PAINT_NONE;
    return true;
  }
  if (starts_ci(v, "currentcolor")) {
    p->kind = PAINT_CURRENT;
    return true;
  }
  if (starts_ci(v, "url(")) {
    const struct dom_node *g = svg_ref_target(cx, v);
    if (g && (streq(g->tag, "lineargradient") || streq(g->tag, "radialgradient") || streq(g->tag, "pattern"))) {
      p->kind = PAINT_URL;
      p->ref = g;
      return true;
    }
    const char *close = strchr(v, ')');
    if (!close)
      return true;
    struct paint fb;
    if (parse_paint(cx, close + 1, &fb)) {
      *p = fb;
      if (p->kind == PAINT_URL)
        p->kind = PAINT_NONE;
    }
    return true;
  }
  int a = 255;
  int c = color_parse_a(v, &a);
  if (c < 0)
    return false;
  p->kind = PAINT_COLOR;
  p->rgb = c;
  p->alpha = a;
  return true;
}

static void parse_dash(const char *v, float vp, struct style *s)
{
  s->ndash = 0;
  if (starts_ci(v, "none"))
    return;
  float d[MAX_DASH], sum = 0;
  int n = 0;
  const char *e = v + strlen(v);
  while (n < MAX_DASH) {
    const char *q = v;
    float x;
    if (!svg_parse_num(&v, e, &x))
      break;
    v = q;
    char buf[32];
    while (*v == ' ' || *v == ',' || *v == '\t')
      v++;
    int k = 0;
    while (*v && *v != ' ' && *v != ',' && *v != '\t' && k < 31)
      buf[k++] = *v++;
    buf[k] = '\0';
    if (!svg_parse_len(buf, vp, &x) || x < 0)
      return;
    d[n++] = x;
    sum += x;
  }
  if (!n || sum <= 0)
    return;
  int reps = (n & 1) ? 2 : 1;
  if (n * reps > MAX_DASH)
    reps = 1;
  for (int r = 0; r < reps; r++)
    for (int i = 0; i < n; i++)
      s->dash[s->ndash++] = d[i];
}

void svg_resolve_style(const struct ctx *cx, const struct dom_node *n, struct style *s)
{
  char b[256];
  float vd = (float)sqrt((double)((cx->vw * cx->vw + cx->vh * cx->vh) * 0.5f));
  if (svg_get_prop(cx, n, "color", b, sizeof b)) {
    int a, c = color_parse_a(b, &a);
    if (c >= 0)
      s->color = c;
  }
  if (svg_get_prop(cx, n, "fill", b, sizeof b))
    parse_paint(cx, b, &s->fill);
  if (svg_get_prop(cx, n, "stroke", b, sizeof b))
    parse_paint(cx, b, &s->stroke);
  if (svg_get_prop(cx, n, "fill-opacity", b, sizeof b))
    s->fill_op = svg_parse_unit(b, s->fill_op);
  if (svg_get_prop(cx, n, "stroke-opacity", b, sizeof b))
    s->stroke_op = svg_parse_unit(b, s->stroke_op);
  float f;
  if (svg_get_prop(cx, n, "stroke-width", b, sizeof b) && svg_parse_len(b, vd, &f) && f >= 0)
    s->sw = f;
  if (svg_get_prop(cx, n, "stroke-miterlimit", b, sizeof b)) {
    const char *q = b;
    if (svg_parse_num(&q, b + strlen(b), &f) && f >= 1)
      s->miter = f;
  }
  if (svg_get_prop(cx, n, "stroke-linecap", b, sizeof b))
    s->cap = starts_ci(b, "round") ? CAP_ROUND : starts_ci(b, "square") ? CAP_SQUARE : CAP_BUTT;
  if (svg_get_prop(cx, n, "stroke-linejoin", b, sizeof b))
    s->join = starts_ci(b, "round") ? JOIN_ROUND : starts_ci(b, "bevel") ? JOIN_BEVEL : JOIN_MITER;
  if (svg_get_prop(cx, n, "stroke-dasharray", b, sizeof b))
    parse_dash(b, vd, s);
  if (svg_get_prop(cx, n, "stroke-dashoffset", b, sizeof b) && svg_parse_len(b, vd, &f))
    s->dash_off = f;
  if (svg_get_prop(cx, n, "fill-rule", b, sizeof b))
    s->evenodd = starts_ci(b, "evenodd");
  if (svg_get_prop(cx, n, "visibility", b, sizeof b))
    s->hidden = starts_ci(b, "hidden") || starts_ci(b, "collapse");
  if (svg_get_prop(cx, n, "font-size", b, sizeof b) && svg_parse_len(b, s->font_size, &f)) {
    const char *u = b;
    while (*u && ((*u >= '0' && *u <= '9') || *u == '.' || *u == '-' || *u == '+'))
      u++;
    if (starts_ci(u, "em"))
      f = f / 16.0f * s->font_size;
    if (f > 0)
      s->font_size = f;
  }
  if (svg_get_prop(cx, n, "font-weight", b, sizeof b)) {
    const char *q = b;
    s->bold = starts_ci(b, "bold") || (svg_parse_num(&q, b + strlen(b), &f) && f >= 600);
  }
  if (svg_get_prop(cx, n, "font-family", b, sizeof b))
    s->mono = contains_ci(b, "mono") || contains_ci(b, "courier") || contains_ci(b, "consolas");
  if (svg_get_prop(cx, n, "text-anchor", b, sizeof b))
    s->anchor = starts_ci(b, "middle") ? 1 : starts_ci(b, "end") ? 2 : 0;
  if (svg_get_prop(cx, n, "letter-spacing", b, sizeof b) && svg_parse_len(b, s->font_size, &f))
    s->letter_spacing = f;
  if (svg_get_prop(cx, n, "marker", b, sizeof b))
    s->marker[0] = s->marker[1] = s->marker[2] = starts_ci(b, "url(") ? svg_ref_target(cx, b) : 0;
  static const char *const mk[3] = {"marker-start", "marker-mid", "marker-end"};
  for (int i = 0; i < 3; i++)
    if (svg_get_prop(cx, n, mk[i], b, sizeof b))
      s->marker[i] = starts_ci(b, "url(") ? svg_ref_target(cx, b) : 0;
}

void svg_style_base(struct style *s, int color)
{
  memset(s, 0, sizeof *s);
  s->fill = (struct paint){PAINT_COLOR, 0, 255, 0};
  s->fill_op = s->stroke_op = 1.0f;
  s->sw = 1.0f;
  s->miter = 4.0f;
  s->font_size = 16.0f;
  s->color = color;
}

float svg_elem_opacity(const struct ctx *cx, const struct dom_node *n)
{
  char b[32];
  return svg_get_prop(cx, n, "opacity", b, sizeof b) ? svg_parse_unit(b, 1.0f) : 1.0f;
}

bool svg_display_none(const struct ctx *cx, const struct dom_node *n)
{
  char b[32];
  return svg_get_prop(cx, n, "display", b, sizeof b) && starts_ci(b, "none");
}
