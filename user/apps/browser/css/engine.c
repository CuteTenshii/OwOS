/* CSS parser, selector matcher and cascade. See css.h.
   :hover/:active/:focus read the targets set by css_set_state; descendant matching is greedy. */
#include "engine_int.h"

/* custom-property tables: the primary (page) sheet's, plus the applying shadow sheet's own */
static struct var_table g_tab, *g_local;
css_sheet *g_vars_sheet;
int g_vars_gen = -1;
static unsigned g_vars_serial; /* bumped whenever a table is rebuilt or dropped */
static void resolve_vars(const char *in, int inlen, char *out, int *on, int cap, int depth);

/* Viewport the @media features test against. */
int g_vw = 800, g_vh = 600;
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

/* :hover/:active/:focus targets; the generation moves only when a sheet has a rule that reads them. */
const struct dom_node *g_state_hover, *g_state_active, *g_state_focus;
bool g_state_used;
static unsigned g_state_gen;
void css_set_state(const struct dom_node *hover, const struct dom_node *active, const struct dom_node *focus)
{
  if (g_state_used && (hover != g_state_hover || active != g_state_active || focus != g_state_focus))
    g_state_gen++;
  g_state_hover = hover;
  g_state_active = active;
  g_state_focus = focus;
}
unsigned css_state_generation(void) { return g_state_gen; }
bool css_state_in_use(void) { return g_state_used; }

/* cascade */
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
    if (d->border_style[i] != CSS_BST_UNSET)
      out->border_style[i] = d->border_style[i];
    if (d->border_side_color[i] >= 0)
      out->border_side_color[i] = d->border_side_color[i];
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
  if (d->col_start >= 0)
    out->col_start = d->col_start;
  if (d->row_start >= 0)
    out->row_start = d->row_start;
  if (d->n_areas >= 0) {
    out->n_areas = d->n_areas;
    for (int i = 0; i < d->n_areas && i < CSS_AREA_MAX; i++)
      out->areas[i] = d->areas[i];
  }
  if (d->area[0])
    memcpy(out->area, d->area, sizeof out->area);
  if (d->align_self >= 0)
    out->align_self = d->align_self;
  if (d->aspect != -1)
    out->aspect = d->aspect;
  if (d->visibility >= 0)
    out->visibility = d->visibility;
  if (d->text_ovf >= 0)
    out->text_ovf = d->text_ovf;
  if (d->obj_fit >= 0)
    out->obj_fit = d->obj_fit;
  if (d->pointer_events >= 0)
    out->pointer_events = d->pointer_events;
  if (d->user_select >= 0)
    out->user_select = d->user_select;
  if (d->letter_sp != CSS_LS_UNSET)
    out->letter_sp = d->letter_sp;
  if (d->va_kind >= 0) {
    out->va_kind = d->va_kind;
    out->va_len = d->va_len;
  }
  if (d->bg_clip >= 0)
    out->bg_clip = d->bg_clip;
  if (d->sb_w != -1)
    out->sb_w = d->sb_w;
  if (d->sb_thumb != -1)
    out->sb_thumb = d->sb_thumb;
  if (d->sb_track != -1)
    out->sb_track = d->sb_track;
  if (d->tab_size >= 0)
    out->tab_size = d->tab_size;
  if (d->cursor >= 0)
    out->cursor = d->cursor;
  if (d->has_tr >= 0) {
    out->has_tr = d->has_tr;
    out->tr_x = d->tr_x;
    out->tr_y = d->tr_y;
    out->tr_xp = d->tr_xp;
    out->tr_yp = d->tr_yp;
    out->tr_mat = d->tr_mat;
    memcpy(out->tr_m, d->tr_m, sizeof out->tr_m);
  }
  if (d->tr_org >= 0) {
    out->tr_org = d->tr_org;
    out->tr_ox = d->tr_ox;
    out->tr_oy = d->tr_oy;
    out->tr_oxp = d->tr_oxp;
    out->tr_oyp = d->tr_oyp;
  }
  if (d->flt_bright >= 0)
    out->flt_bright = d->flt_bright;
  if (d->flt_gray >= 0)
    out->flt_gray = d->flt_gray;
  if (d->flt_invert >= 0)
    out->flt_invert = d->flt_invert;
  if (d->flt_opacity >= 0)
    out->flt_opacity = d->flt_opacity;
  if (d->clip_kind >= 0) {
    out->clip_kind = d->clip_kind;
    out->clip_n = d->clip_n;
    memcpy(out->clip_v, d->clip_v, sizeof out->clip_v);
    memcpy(out->clip_unit, d->clip_unit, sizeof out->clip_unit);
  }
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
  if (d->svg_decl[0]) {
    int ol = (int)strlen(out->svg_decl), dl = (int)strlen(d->svg_decl);
    if (ol + dl < CSS_SVG_DECL)
      memcpy(out->svg_decl + ol, d->svg_decl, (unsigned)dl + 1);
  }
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
  /* image identity folds as a unit; repeat/size/position are independent longhands */
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

/* Custom properties: global, last declaration wins, honours @media. */
static bool var_name_is(const struct css_var *v, const char *name, int len)
{
  return (int)strlen(v->name) == len && memcmp(v->name, name, (unsigned)len) == 0;
}
static const char *table_lookup(const struct var_table *t, const char *name, int len)
{
  for (int i = 0; i < t->n; i++)
    if (var_name_is(&t->v[i], name, len))
      return t->v[i].val;
  return 0;
}
static const char *var_lookup(const char *name, int len)
{
  const char *v = g_local ? table_lookup(g_local, name, len) : 0;
  return v ? v : table_lookup(&g_tab, name, len);
}
static char *dup_span(const char *s, int n)
{
  char *d = malloc((unsigned)n + 1);
  if (!d)
    return 0;
  memcpy(d, s, (unsigned)n);
  d[n] = '\0';
  return d;
}
static void table_clear(struct var_table *t)
{
  for (int i = 0; i < t->n; i++) {
    free(t->v[i].name);
    free(t->v[i].val);
  }
  t->n = 0;
}
void vars_clear(void)
{
  table_clear(&g_tab);
  g_vars_serial++;
}
void sheet_vars_free(css_sheet *sh)
{
  table_clear(&sh->lvars);
  free(sh->lvars.v);
  sh->lvars.v = 0;
  sh->lvars.cap = 0;
  if (g_local == &sh->lvars)
    g_local = 0;
}
static void var_store(struct var_table *t, const char *name, int nlen, const char *val, int vlen)
{
  if (nlen <= 0)
    return;
  char *nv = dup_span(val, vlen);
  if (!nv)
    return;
  for (int i = 0; i < t->n; i++)
    if (var_name_is(&t->v[i], name, nlen)) {
      free(t->v[i].val);
      t->v[i].val = nv;
      return;
    }
  if (t->n >= t->cap) {
    int nc = t->cap ? t->cap * 2 : 64;
    struct css_var *nvs = realloc(t->v, (unsigned)nc * sizeof *nvs);
    if (!nvs) {
      free(nv);
      return;
    }
    t->v = nvs;
    t->cap = nc;
  }
  char *nn = dup_span(name, nlen);
  if (!nn) {
    free(nv);
    return;
  }
  t->v[t->n].name = nn;
  t->v[t->n].val = nv;
  t->n++;
}

/* Pull every `--name: value` declaration out of `raw` into the table. */
static void vars_from_raw(struct var_table *t, const char *raw)
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
      var_store(t, seg, (int)(colon - seg), vs, (int)(ve - vs));
    }
    if (*p == ';')
      p++;
  }
}
static css_sheet *g_vars_primary;

void css_set_primary(css_sheet *sh) { g_vars_primary = sh; }
void vars_forget_sheet(css_sheet *sh)
{
  if (sh == g_vars_primary)
    g_vars_primary = 0;
  sheet_vars_free(sh);
}

static void vars_load(struct var_table *t, css_sheet *sh)
{
  for (struct css_rule *r = sh->head; r; r = r->next)
    if (media_matches(r->media))
      vars_from_raw(t, r->raw); /* active rules only */
}

/* The global table holds the primary sheet's variables; any other sheet resolves against its own first */
static void ensure_vars(css_sheet *sh)
{
  css_sheet *prim = g_vars_primary ? g_vars_primary : sh;
  if (!(prim == g_vars_sheet && prim->gen == g_vars_gen)) {
    vars_clear();
    vars_load(&g_tab, prim);
    g_vars_sheet = prim;
    g_vars_gen = prim->gen;
  }
  if (sh == prim) {
    g_local = 0;
    return;
  }
  if (sh->lvars_gen != sh->gen) {
    table_clear(&sh->lvars);
    vars_load(&sh->lvars, sh);
    sh->lvars_gen = sh->gen;
    g_vars_serial++;
  }
  g_local = &sh->lvars;
}

bool str_uses_var(const char *s)
{
  for (; *s; s++)
    if ((*s == 'v' || *s == 'V') && starts_ci(s, "var("))
      return true;
  return false;
}
/* Substitute var(--name, fallback) from the table; unknown with no fallback is empty. */
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

/* resolve_vars into a fresh heap string sized to fit (NULL on OOM). */
char *resolve_vars_dup(const char *in, int inlen)
{
  int cap = inlen * 2 + 64;
  for (;;) {
    char *out = malloc((unsigned)cap);
    if (!out)
      return 0;
    int on = 0;
    resolve_vars(in, inlen, out, &on, cap, 0);
    if (on < cap - 1)
      return out;
    free(out);
    cap *= 2;
  }
}

/* Declarations with var() resolved, memoised until the variable table is rebuilt. */
static const struct css_style *rule_decl(struct css_rule *r)
{
  static struct css_style scratch;
  if (!r->has_var)
    return &r->decl;
  if (r->vc && r->vc_serial == g_vars_serial)
    return r->vc;
  struct css_style *dst = r->vc ? r->vc : malloc(sizeof *dst);
  if (!dst)
    dst = &scratch;
  css_style_reset(dst);
  char *buf = resolve_vars_dup(r->raw, (int)strlen(r->raw));
  if (buf) {
    bool imp = false;
    parse_decls(buf, buf + strlen(buf), dst, &imp);
    free(buf);
  }
  if (dst != &scratch) {
    r->vc = dst;
    r->vc_serial = g_vars_serial;
  }
  return dst;
}

/* Rule index: rules bucketed by rightmost key (tag/class/id), rebuilt lazily. */
static unsigned css_hash_n(const char *s, int n)
{
  unsigned h = 5381;
  for (int i = 0; i < n; i++)
    h = h * 33u + (unsigned char)s[i];
  return h % CSS_NBUCK;
}
static unsigned css_hash(const char *s) { return css_hash_n(s, (int)strlen(s)); }
/* Append index keys for compound `st`; returns 0 if it has no usable key (file universally). */
struct idx_key {
  int kind;
  const char *str;
};
struct idx_keys {
  struct idx_key *v;
  int n, cap;
};
static bool idx_keys_push(struct idx_keys *k, int kind, const char *str)
{
  if (k->n == k->cap) {
    int nc = k->cap ? k->cap * 2 : 8;
    struct idx_key *nv = realloc(k->v, (unsigned)nc * sizeof *nv);
    if (!nv)
      return false;
    k->v = nv;
    k->cap = nc;
  }
  k->v[k->n].kind = kind;
  k->v[k->n].str = str;
  k->n++;
  return true;
}
static int compound_keys(const struct sel_step *st, struct idx_keys *out)
{
  if (st->id)
    return idx_keys_push(out, KK_ID, st->id);
  if (st->ncls)
    return idx_keys_push(out, KK_CLS, st->cls[0]);
  if (st->tag[0])
    return idx_keys_push(out, KK_TAG, st->tag);
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
    if (!compound_keys(&sub->step[sub->n - 1], out))
      return 0; /* a branch matches anything */
  }
  return 1;
}

void index_clear(css_sheet *sh)
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

static bool index_put(struct css_idx **head, struct css_rule *r, const char *key)
{
  struct css_idx *e = malloc(sizeof *e);
  if (!e)
    return false;
  e->rule = r;
  e->key = key;
  e->next = *head;
  *head = e;
  return true;
}

static void ensure_index(css_sheet *sh)
{
  if (sh->idx_gen == sh->gen)
    return;
  index_clear(sh);
  sh->idx_failed = false;
  struct idx_keys keys = {0};
  for (struct css_rule *r = sh->head; r; r = r->next) {
    keys.n = 0;
    if (!compound_keys(&r->sel.step[r->sel.n - 1], &keys) || keys.n == 0) {
      if (!index_put(&sh->uni, r, 0)) {
        sh->idx_failed = true;
        break;
      }
      continue;
    }
    for (int i = 0; i < keys.n; i++) {
      struct css_idx **tab = keys.v[i].kind == KK_ID ? sh->id_b : keys.v[i].kind == KK_CLS ? sh->cls_b : sh->tag_b;
      if (!index_put(&tab[css_hash(keys.v[i].str)], r, keys.v[i].str)) {
        sh->idx_failed = true;
        break;
      }
    }
    if (sh->idx_failed)
      break;
  }
  free(keys.v);
  if (sh->idx_failed)
    index_clear(sh);
  sh->idx_gen = sh->gen;
}

static bool rule_ptr_room(struct css_rule ***buf, int *cap, int need)
{
  if (need <= *cap)
    return true;
  int nc = *cap ? *cap * 2 : 64;
  while (nc < need)
    nc *= 2;
  struct css_rule **p = realloc(*buf, (unsigned)nc * sizeof *p);
  if (!p)
    return false;
  *buf = p;
  *cap = nc;
  return true;
}

/* Candidate rules for `el`; a per-rule visit stamp de-dups multi-key rules. */
static unsigned g_visit;
static int collect_candidates(css_sheet *sh, const struct dom_node *el)
{
  ensure_index(sh);
  unsigned stamp = ++g_visit;
  int n = 0;
  bool failed = false;
#define ADD(rp)                                                                                                        \
  do {                                                                                                                 \
    struct css_rule *rr = (rp);                                                                                        \
    if (rr->visit != stamp) {                                                                                          \
      rr->visit = stamp;                                                                                               \
      if (rule_ptr_room(&sh->cand, &sh->cand_cap, n + 1))                                                              \
        sh->cand[n++] = rr;                                                                                            \
      else                                                                                                             \
        failed = true;                                                                                                 \
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
  if (id && id[0])
    for (struct css_idx *e = sh->id_b[css_hash(id)]; e; e = e->next)
      if (streq_ci(e->key, id))
        ADD(e->rule);
  const char *cl = dom_get_attr(el, "class");
  if (cl)
    for (const char *p = cl; *p;) {
      while (*p == ' ' || *p == '\t')
        p++;
      const char *q = p;
      while (*q && *q != ' ' && *q != '\t')
        q++;
      int k = (int)(q - p);
      if (k)
        for (struct css_idx *e = sh->cls_b[css_hash_n(p, k)]; e; e = e->next)
          if ((int)strlen(e->key) == k && memcmp(e->key, p, (unsigned)k) == 0)
            ADD(e->rule);
      p = q;
    }
#undef ADD
  return failed ? -1 : n;
}

void css_set_scope(const struct dom_node *host, int mode)
{
  g_css_scope_host = host;
  g_css_mode = mode;
}

/* Rules matching an element, lowest cascade priority first; `pseudo` selects ::before/::after. */
static int collect_matches(css_sheet *sh, const struct dom_node *el, int pseudo)
{
  int nc, nm = 0;
  if (g_css_mode >= CSS_SCOPE_SLOTTED) { /* ::slotted / ::part rules aren't keyed by `el`: scan them all */
    nc = 0;
    for (struct css_rule *r = sh->head; r; r = r->next)
      if (r->sel.step[r->sel.n - 1].pseudo_el == (g_css_mode == CSS_SCOPE_SLOTTED ? PE_SLOTTED : PE_PART)) {
        if (!rule_ptr_room(&sh->cand, &sh->cand_cap, nc + 1))
          return -1;
        sh->cand[nc++] = r;
      }
  } else {
    nc = collect_candidates(sh, el);
  }
  if (nc < 0)
    return -1;
  for (int i = 0; i < nc; i++) {
    struct css_rule *r = sh->cand[i];
    bool ok;
    if (!media_matches(r->media))
      continue;
    if (g_css_mode == CSS_SCOPE_SLOTTED)
      ok = selector_match_slotted(&r->sel, el);
    else if (g_css_mode == CSS_SCOPE_PART)
      ok = selector_match_part(&r->sel, el);
    else if (g_css_mode == CSS_SCOPE_HOST_ONLY)
      ok = selector_subject_is_host(&r->sel) && selector_match(&r->sel, el);
    else
      ok = pseudo ? selector_match_pseudo(&r->sel, el, pseudo) : selector_match(&r->sel, el);
    if (!ok)
      continue;
    if (!rule_ptr_room(&sh->match, &sh->match_cap, nm + 1))
      return -1;
    sh->match[nm++] = r;
  }
  for (int i = 1; i < nm; i++) {
    struct css_rule *key = sh->match[i];
    int j = i - 1;
    while (j >= 0) {
      struct css_rule *r = sh->match[j];
      int after = (r->important != key->important) ? (r->important > key->important)
                  : (r->spec != key->spec)         ? (r->spec > key->spec)
                                                   : (r->order > key->order);
      if (!after)
        break;
      sh->match[j + 1] = r;
      j--;
    }
    sh->match[j + 1] = key;
  }
  return nm;
}

void css_apply_sheet(css_sheet *sh, const struct dom_node *el, struct css_style *out)
{
  if (!sh || !el || el->type != DOM_ELEMENT)
    return;
  ensure_vars(sh);

  /* Fold in ascending priority so the winner is applied last. */
  int nm = collect_matches(sh, el, 0);
  if (nm < 0)
    return;
  for (int i = 0; i < nm; i++)
    fold(out, rule_decl(sh->match[i]));
}

int css_pseudo_content(css_sheet *sh, const struct dom_node *el, int which, struct css_style *out)
{
  css_style_reset(out);
  if (!sh || !el || el->type != DOM_ELEMENT)
    return 0;
  ensure_vars(sh);
  int nm = collect_matches(sh, el, which);
  if (nm < 0)
    return 0;
  for (int i = 0; i < nm; i++)
    fold(out, rule_decl(sh->match[i]));
  return out->content[0] != '\0';
}

int css_sheet_has_pseudo(const css_sheet *sh) { return sh && sh->has_pseudo_el; }

int css_sheet_has_selection(const css_sheet *sh) { return sh && sh->has_selection; }

int css_selection_color(css_sheet *sh, const struct dom_node *el, unsigned *rgb)
{
  struct css_style st;
  css_style_reset(&st);
  if (!sh || !el || el->type != DOM_ELEMENT)
    return 0;
  ensure_vars(sh);
  int nm = collect_matches(sh, el, PE_SELECTION);
  for (int i = 0; i < nm; i++)
    fold(&st, rule_decl(sh->match[i]));
  if (st.bg < 0)
    return 0;
  *rgb = (unsigned)st.bg;
  return 1;
}

unsigned css_sheet_generation(const css_sheet *sh) { return sh ? (unsigned)sh->gen : 0; }

void css_apply_attrs(const struct dom_node *el, struct css_style *out)
{
  if (!el || el->type != DOM_ELEMENT)
    return;
  bool dialog = streq(el->tag, "dialog");
  if (dialog || dom_get_attr(el, "popover")) { /* UA box for a dialog / popover */
    out->bg = 0xffffff;
    out->color = 0;
    out->margin[1] = out->margin[3] = -2;
    for (int i = 0; i < 4; i++) {
      out->padding[i] = dialog ? 16 : 4;
      out->border_w[i] = dialog ? 2 : 1;
      out->border_style[i] = CSS_BST_SOLID;
    }
    out->border_color = 0;
    if ((dialog && dom_get_attr(el, "open") && dom_get_attr(el, "dialog-modal")) || dom_get_attr(el, "popover-open")) {
      out->pos = CSS_POS_FIXED; /* top layer: centred in the viewport */
      out->margin[0] = out->margin[2] = -2;
    }
  }
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

void css_apply_fullscreen(const struct dom_node *el, struct css_style *out)
{
  if (!el || el->type != DOM_ELEMENT || !dom_get_attr(el, "fs-top"))
    return;
  out->pos = CSS_POS_FIXED;
  out->top = out->right = out->bottom = out->left = 0;
  out->width = out->height = out->max_w = out->max_h = out->min_w = out->min_h = -1;
  out->pct_w = out->pct_h = out->pct_maxw = out->pct_maxh = out->pct_minw = out->pct_minh = -1;
  out->disp = CSS_DISP_BLOCK;
  for (int i = 0; i < 4; i++)
    out->margin[i] = 0;
  out->has_tr = 0;
  out->tr_mat = 0;
}

/* css_bg_argb: cascade-winning background as 0xAARRGGBB; caller composites over the canvas. */

/* Last background[-color] value in a block, var() resolved, into out[cap]; 1 if present. */
static char *extract_bg(const char *decls)
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
  return resolve_vars_dup(bv, (int)(bve - bv)); /* verbatim copy when no var() */
}

static bool bg_decl_color(const char *decls, unsigned *out)
{
  char *val = extract_bg(decls);
  if (!val)
    return false;
  int a;
  int c = bg_value_color(val, &a);
  free(val);
  if (c < 0)
    return false;
  *out = ((unsigned)a << 24) | (unsigned)c;
  return true;
}

int css_bg_argb(css_sheet *sh, const struct dom_node *el, unsigned *out)
{
  if (!sh || !el || el->type != DOM_ELEMENT)
    return 0;
  ensure_vars(sh);

  /* priority: inline style, sheet rules, then bgcolor */
  const char *inl = dom_get_attr(el, "style");
  if (inl && bg_decl_color(inl, out))
    return 1;

  int nm = collect_matches(sh, el, 0);
  if (nm < 0)
    return 0;
  for (int i = nm - 1; i >= 0; i--)
    if (bg_decl_color(sh->match[i]->raw, out))
      return 1;

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

/* Fold the rules of one scrollbar part for `el` into a style. */
static void sb_fold(css_sheet *sh, const struct dom_node *el, int part, struct css_style *st)
{
  css_style_reset(st);
  int nm = collect_matches(sh, el, part);
  for (int i = 0; i < nm; i++)
    fold(st, rule_decl(sh->match[i]));
}

int css_scrollbar_parts(css_sheet *sh, const struct dom_node *el, struct css_sbparts *out)
{
  out->width = out->track = out->thumb = out->radius = -1;
  out->hidden = false;
  if (!sh || !sh->has_scrollbar || !el || el->type != DOM_ELEMENT)
    return 0;
  ensure_vars(sh);
  struct css_style bar, thumb, track;
  sb_fold(sh, el, PE_SCROLLBAR, &bar);
  sb_fold(sh, el, PE_SB_THUMB, &thumb);
  sb_fold(sh, el, PE_SB_TRACK, &track);
  out->hidden = bar.disp == CSS_DISP_NONE;
  out->width = bar.width;
  out->track = track.bg >= 0 ? track.bg : bar.bg;
  out->thumb = thumb.bg;
  out->radius = thumb.radius;
  return out->hidden || out->width >= 0 || out->track >= 0 || out->thumb >= 0;
}

int css_marker_hidden(css_sheet *sh, const struct dom_node *el)
{
  if (!sh || !sh->has_marker || !el || el->type != DOM_ELEMENT)
    return 0;
  ensure_vars(sh);
  struct css_style st;
  css_style_reset(&st);
  int nm = collect_matches(sh, el, PE_MARKER);
  for (int i = 0; i < nm; i++)
    fold(&st, rule_decl(sh->match[i]));
  return st.disp == CSS_DISP_NONE;
}

int css_backdrop_argb(css_sheet *sh, const struct dom_node *el, unsigned *out)
{
  if (!sh || !el || el->type != DOM_ELEMENT)
    return 0;
  ensure_vars(sh);
  int nm = collect_matches(sh, el, PE_BACKDROP);
  for (int i = nm - 1; i >= 0; i--)
    if (bg_decl_color(sh->match[i]->raw, out))
      return 1;
  return 0;
}

/* devtools: matched rules */
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
    if (st->id) {
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
  if (!mq || mq->n == 0)
    return;
  cm_ap(o, &k, cap, "@media ");
  for (int c = 0; c < mq->n; c++) {
    const struct media_cond *cd = &mq->cond[c];
    if (c)
      cm_ap(o, &k, cap, ", ");
    if (cd->negate)
      cm_ap(o, &k, cap, "not ");
    bool need_and = false;
    const char *mt = cd->mtype == MT_SCREEN ? "screen" : cd->mtype == MT_PRINT ? "print" : 0;
    if (mt) {
      cm_ap(o, &k, cap, mt);
      need_and = true;
    }
    for (int f = 0; f < cd->nfeat; f++) {
      if (need_and)
        cm_ap(o, &k, cap, " and ");
      need_and = true;
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

  /* Most specific / last declared first. */
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
    media_text(r->media, out[n].media, (int)sizeof out[n].media);
    out[n].active = media_matches(r->media);
    out[n].decl = r->decl;
    out[n].raw = r->raw;
    n++;
  }
  return n;
}
