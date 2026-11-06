#include "engine_int.h"

/* @media */
static const char *skip_sp(const char *p, const char *e)
{
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f'))
    p++;
  return p;
}

/* Match lowercase keyword `lit` at *pp (token-bounded); advance past it on a hit. */
static bool kw(const char **pp, const char *e, const char *lit)
{
  const char *q = *pp;
  while (q < e && *lit && lower(*q) == *lit) {
    q++;
    lit++;
  }
  if (*lit)
    return false;
  if (q < e && is_sel_name(*q))
    return false;
  *pp = q;
  return true;
}

/* Media length operand in [s,e) -> integer px (em/rem and physical units resolve like the cascade); -1 if none. */
static int parse_len_px(const char *s, const char *e)
{
  s = skip_sp(s, e);
  bool neg = false;
  if (s < e && (*s == '+' || *s == '-')) {
    neg = (*s == '-');
    s++;
  }
  double n = 0;
  bool any = false;
  while (s < e && *s >= '0' && *s <= '9') {
    n = n * 10 + (*s++ - '0');
    any = true;
  }
  if (s < e && *s == '.') {
    s++;
    double f = 0.1;
    while (s < e && *s >= '0' && *s <= '9') {
      n += (*s++ - '0') * f;
      f *= 0.1;
      any = true;
    }
  }
  if (!any)
    return -1;
  if (neg)
    n = -n;
  const char *u = skip_sp(s, e);
  double px;
  if (u >= e)
    px = n;
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
static bool span_eq_trim(const char *a, const char *e, const char *lit)
{
  a = skip_sp(a, e);
  while (e > a && (e[-1] == ' ' || e[-1] == '\t'))
    e--;
  return span_is(a, e, lit);
}

/* range syntax: (width >= 600px), (400px <= width <= 900px) */
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

/* Read a comparison operator at *pp (`<` `<=` `>` `>=` `=` `==`); -1 if none. */
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

/* Add bound `<feature> op <len>` (feature on the left) to *c; strict </> nudge the px edge by 1. */
static bool cond_add_feat(struct media_cond *c, struct media_feature f)
{
  struct media_feature *nf = realloc(c->feat, (unsigned)(c->nfeat + 1) * sizeof *nf);
  if (!nf)
    return false;
  c->feat = nf;
  c->feat[c->nfeat++] = f;
  return true;
}

static void range_bound(struct media_cond *c, int dim, int op, int len)
{
  if (len < 0)
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
    cond_add_feat(c, f);
}

/* Parse range syntax in parens [s,e): one comparison or a two-sided interval. */
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

  if (np == 2 && no == 1) { /* A op B */
    int d0 = range_dim(part_s[0], part_e[0]);
    if (d0)
      range_bound(c, d0, ops[0], parse_len_px(part_s[1], part_e[1]));
    else {
      int d1 = range_dim(part_s[1], part_e[1]);
      if (d1)
        range_bound(c, d1, op_flip(ops[0]), parse_len_px(part_s[0], part_e[0]));
    }
  } else if (np == 3 && no == 2) { /* A op0 B op1 C */
    int d = range_dim(part_s[1], part_e[1]);
    if (d) {
      range_bound(c, d, op_flip(ops[0]), parse_len_px(part_s[0], part_e[0]));
      range_bound(c, d, ops[1], parse_len_px(part_s[2], part_e[2]));
    }
  }
}

/* Parse one parenthesised media feature at *pp, advancing past ')'; unsupported ones are MF_UNKNOWN (false). */
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

  /* classic (a ':') vs range (a comparison operator) */
  const char *q = is;
  while (q < ie && *q != ':' && *q != '<' && *q != '>' && *q != '=')
    q++;
  if (q < ie && *q != ':') {
    parse_range(is, ie, c);
    return;
  }

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
  cond_add_feat(c, f);
}

/* Parse one comma-free media query (`[not|only] type? (and feature)*`) into *c. */
static void parse_cond(const char *s, const char *e, struct media_cond *c)
{
  c->negate = false;
  c->mtype = MT_ALL;
  c->feat = 0;
  c->nfeat = 0;
  const char *p = skip_sp(s, e);
  if (kw(&p, e, "not"))
    c->negate = true;
  p = skip_sp(p, e);
  kw(&p, e, "only");
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

/* Parse a full @media header [s,e) (comma-separated conditions) into *mq. */
static void parse_media_query(const char *s, const char *e, struct media_query *mq)
{
  mq->n = 0;
  mq->cond = 0;
  const char *p = s;
  while (p < e) {
    const char *comma = p;
    while (comma < e && *comma != ',')
      comma++;
    struct media_cond *nc = realloc(mq->cond, (unsigned)(mq->n + 1) * sizeof *nc);
    if (!nc)
      break;
    mq->cond = nc;
    parse_cond(p, comma, &mq->cond[mq->n++]);
    p = (comma < e) ? comma + 1 : e;
  }
}

static void media_free(struct media_query *mq)
{
  for (int i = 0; i < mq->n; i++)
    free(mq->cond[i].feat);
  free(mq->cond);
  free(mq);
}

static bool feature_matches(const struct media_feature *f)
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
    return true; /* the browser renders a light theme */
  case MF_SCHEME_DARK:
    return false;
  default:
    return false; /* MF_UNKNOWN / unsupported feature */
  }
}

static bool cond_matches(const struct media_cond *c)
{
  bool ok = (c->mtype == MT_ALL || c->mtype == MT_SCREEN);
  for (int i = 0; ok && i < c->nfeat; i++)
    if (!feature_matches(&c->feat[i]))
      ok = false;
  return c->negate ? !ok : ok;
}

bool media_matches(const struct media_query *mq)
{
  if (!mq || mq->n == 0)
    return true;
  for (int i = 0; i < mq->n; i++)
    if (cond_matches(&mq->cond[i]))
      return true;
  return false;
}

/* sheet build */
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
  s->medias = 0;
  s->has_pseudo_el = false;
  s->cur_base = 0;
  for (int i = 0; i < CSS_NBUCK; i++) {
    s->id_b[i] = s->cls_b[i] = s->tag_b[i] = 0;
  }
  s->uni = 0;
  s->idx_gen = -1; /* index is stale until first ensure_index */
  s->idx_failed = false;
  s->cand = 0;
  s->cand_cap = 0;
  s->match = 0;
  s->match_cap = 0;
  return s;
}

/* First url(...) in a src value naming a decodable font (SFNT or WOFF/WOFF2), into url[cap]. */
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

/* Parse an @font-face block [s,end); registers the face if family and a usable src exist. */
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

/* @keyframes stop selector: `from` -> 0, `to` -> 1000, `N%` -> N*10; -1 if invalid. */
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
  bool any = false;
  const char *p = s;
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
  if (!any || p >= e || *p != '%')
    return -1;
  int pm = (int)(n * 10 + 0.5);
  if (pm < 0)
    pm = 0;
  if (pm > 1000)
    pm = 1000;
  return pm;
}

/* Insert a stop into *kf keeping offsets ascending; a duplicate offset is overwritten. */
static void keyframes_add_stop(struct css_keyframes *kf, int pm, const struct css_style *decl)
{
  for (int i = 0; i < kf->nstops; i++) {
    if (kf->off[i] == pm) {
      kf->stop[i] = *decl;
      return;
    }
  }
  if (kf->nstops >= kf->cap) {
    int nc = kf->cap ? kf->cap * 2 : 4;
    short *no = realloc(kf->off, (unsigned)nc * sizeof *no);
    if (no)
      kf->off = no;
    struct css_style *ns = realloc(kf->stop, (unsigned)nc * sizeof *ns);
    if (ns)
      kf->stop = ns;
    if (!no || !ns)
      return;
    kf->cap = nc;
  }
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

/* Parse a @keyframes block (name [ns,ne), body [bs,be)); registers it if it yields a stop. */
static void parse_keyframes(css_sheet *sh, const char *ns, const char *ne, const char *bs, const char *be)
{
  while (ns < ne && (*ns == ' ' || *ns == '\t' || *ns == '\n' || *ns == '\r'))
    ns++;
  while (ne > ns && (ne[-1] == ' ' || ne[-1] == '\t' || ne[-1] == '\n' || ne[-1] == '\r'))
    ne--;
  /* dequote a quoted animation-name */
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
  kf->nstops = kf->cap = 0;
  kf->off = 0;
  kf->stop = 0;
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
    bool imp = false;
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
    free(kf->off);
    free(kf->stop);
    free(kf);
    return;
  }
  kf->next = sh->keyframes;
  sh->keyframes = kf;
}

/* ---- @keyframes accessors (anim.c) ---- */
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

static char no_raw[1];

/* Copy [s,e) with whitespace runs collapsed; a shared empty string on OOM. */
static char *copy_collapsed(const char *s, const char *e)
{
  char *out = malloc((unsigned)(e - s) + 1);
  if (!out)
    return no_raw;
  int k = 0;
  bool sp = false;
  for (; s < e; s++) {
    char c = *s;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      sp = true;
      continue;
    }
    if (sp && k > 0)
      out[k++] = ' ';
    sp = false;
    out[k++] = c;
  }
  out[k] = '\0';
  return out;
}

/* Takes ownership of *sel (freed here when the rule can't be stored). */
static void sheet_push(css_sheet *sh, struct selector *sel, const struct css_style *decl, bool important,
                       const struct media_query *media, const char *dec_s, const char *dec_e)
{
  struct css_rule *r = malloc(sizeof *r);
  if (!r) {
    selector_free(sel);
    return;
  }
  r->sel = *sel;
  r->decl = *decl;
  r->media = media;
  r->spec = selector_spec(sel);
  r->order = sh->order++;
  r->important = important;
  if (sel->step[sel->n - 1].pseudo_el == PE_BEFORE || sel->step[sel->n - 1].pseudo_el == PE_AFTER)
    sh->has_pseudo_el = true; /* a ::before/::after rule exists in this sheet */
  r->visit = 0;               /* the index (ensure_index) files it by key */
  r->vc = 0;
  r->vc_serial = 0;
  r->raw = copy_collapsed(dec_s, dec_e);
  r->has_var = r->raw[0] && str_uses_var(r->raw);
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

/* Parse rules in [p,end) under media query *mq; a nested @media replaces only an unconditional outer one. */
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
          struct media_query *nested = calloc(1, sizeof *nested);
          if (nested) {
            parse_media_query(hdr, b, nested);
            nested->next = sh->medias;
            sh->medias = nested;
          }
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
          add_rules(sh, db, de, (mq && mq->n) || !nested ? mq : nested);
          p = (de < end) ? de + 1 : end;
          continue;
        }
      }
      /* @layer / @supports: flatten inner rules (layer order approximated by source; @supports assumed true). */
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
    bool important = false;
    parse_decls(dec_s, dec_e, &decl, &important);

    /* Split on top-level commas only (scan_top skips :is()/:not() lists and [attr] values). */
    for (const char *q = sel_s; q < sel_e;) {
      const char *comma = scan_top(q, sel_e, ",");
      struct selector sel;
      if (parse_selector(q, comma, &sel))
        sheet_push(sh, &sel, &decl, important, mq, dec_s, dec_e);
      else
        selector_free(&sel); /* discard: reclaim any parsed :is() lists */
      q = (comma < sel_e) ? comma + 1 : sel_e;
    }
  }
}

void css_add(css_sheet *sh, const char *src, int len, const char *base)
{
  if (!sh)
    return;
  sh->cur_base = base; /* @font-face src resolves against this */
  add_rules(sh, src, src + len, 0);
  sh->cur_base = 0;
  sh->gen++; /* custom-property table is now stale */
}

void css_free(css_sheet *sh)
{
  if (!sh)
    return;
  if (sh == g_vars_sheet) {
    g_vars_sheet = 0;
    g_vars_gen = -1;
    vars_clear();
  } /* drop stale var cache */
  free(sh->cand);
  free(sh->match);
  while (sh->medias) {
    struct media_query *n = sh->medias->next;
    media_free(sh->medias);
    sh->medias = n;
  }
  index_clear(sh); /* free the css_idx entries before their rules */
  struct css_rule *r = sh->head;
  while (r) {
    struct css_rule *n = r->next;
    selector_free(&r->sel);
    if (r->raw != no_raw)
      free(r->raw);
    free(r->vc);
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
    free(kf->off);
    free(kf->stop);
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
