#include "devtools_int.h"

/* --- DOM snapshot (worker thread) ----------------------------------------- */

/* Append ` name="value"` per attribute, values truncated. */
static void dom_emit_attrs(struct textblk *t, struct dom_node *el)
{
  for (struct dom_attr *a = el->attrs; a; a = a->next) {
    dt_tb_putc(t, ' ');
    dt_tb_puts(t, a->name);
    if (a->value && a->value[0]) {
      dt_tb_puts(t, "=\"");
      int n = 0;
      for (const char *v = a->value; *v && n < 40; v++, n++) {
        char c = *v;
        dt_tb_putc(t, (c == '\n' || c == '\t') ? ' ' : c);
      }
      if (a->value[n])
        dt_tb_puts(t, "...");
      dt_tb_putc(t, '"');
    }
  }
}

/* Record the next line's owning node index in dt_dom_node; call before the line's first character. */
static void dom_record(int my)
{
  if (dt_dom.nlines >= dt_dom_node_cap) {
    int nc = dt_dom_node_cap ? dt_dom_node_cap * 2 : 64;
    int *p = realloc(dt_dom_node, (unsigned)nc * sizeof *p);
    if (p) {
      dt_dom_node = p;
      dt_dom_node_cap = nc;
    }
  }
  if (dt_dom.nlines < dt_dom_node_cap)
    dt_dom_node[dt_dom.nlines] = my;
}

static void dom_indent(struct textblk *t, int depth)
{
  for (int d = 0; d < depth && d < 16; d++)
    dt_tb_puts(t, "  ");
}

/* Collapsed text content (newlines/tabs to single spaces, truncated). */
static void dom_emit_text(struct textblk *t, const char *s, int maxc)
{
  int cnt = 0;
  for (; s && *s && cnt < maxc; s++) {
    char c = *s;
    if (c == '\n' || c == '\t' || c == '\r') {
      if (cnt && t->buf[t->len - 1] == ' ')
        continue; /* collapse runs */
      c = ' ';
    }
    dt_tb_putc(t, c);
    cnt++;
  }
  if (s && *s)
    dt_tb_puts(t, "...");
}

/* Void/self-closing elements that never get a closing tag in the tree. */
static bool dom_is_void(const char *tag)
{
  static const char *const v[] = {"area", "base", "br",    "col",    "embed", "hr",  "img", "input",
                                  "link", "meta", "param", "source", "track", "wbr", 0};
  for (int i = 0; v[i]; i++)
    if (streq(tag, v[i]))
      return true;
  return false;
}

/* True if `n`'s only child is one non-whitespace text node (rendered inline). */
static struct dom_node *dom_only_text(struct dom_node *n)
{
  struct dom_node *c = n->first_child;
  if (!c || c->next || c->type != DOM_TEXT)
    return 0;
  const char *s = c->text;
  while (s && *s && (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r'))
    s++;
  return (s && *s) ? c : 0;
}

/* Emit the DOM subtree at `n`, recording each line's pre-order index in dt_dom_node; `*idx` counts every node. */
/* Walk caps: the builder caps depth at 256, but a malformed tree is possible; the index still advances. */
#define DT_DOM_MAX_DEPTH 300
#define DT_DOM_MAX_NODES 50000

/* `suppress` walks a collapsed subtree advancing *idx without emitting, keeping index space identical. */
void dt_dom_emit(struct textblk *t, struct dom_node *n, int depth, int *idx, int suppress)
{
  int my = (*idx)++;
  if (!n || depth > DT_DOM_MAX_DEPTH || my > DT_DOM_MAX_NODES)
    return;

  if (n->type == DOM_TEXT) {
    const char *s = n->text;
    while (s && *s && (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r'))
      s++;
    if (s && *s && !suppress) {
      dom_record(my);
      dom_indent(t, depth);
      dt_tb_putc(t, '"');
      dom_emit_text(t, n->text, 80);
      dt_tb_putc(t, '"');
      dt_tb_newline(t);
    }
    return;
  }

  const char *tag = n->tag[0] ? n->tag : "?";
  bool isvoid = dom_is_void(tag);
  /* ::before / ::after show as pseudo-element rows (no pre-order index). */
  bool hb = !isvoid && html_has_pseudo(n, CSS_PSEUDO_BEFORE);
  bool ha = !isvoid && html_has_pseudo(n, CSS_PSEUDO_AFTER);
  struct dom_node *only = (!isvoid && !hb && !ha) ? dom_only_text(n) : 0;
  bool empty = !n->first_child && !hb && !ha;
  bool general = !isvoid && !empty && !only; /* multi-line => collapsible */
  bool collapsed = general && dt_is_collapsed(my);

  if (!suppress) { /* opening tag (+ a ▸/▾ on collapsible nodes) */
    dom_record(my);
    dom_indent(t, depth);
    if (general)
      dt_tb_puts(t, collapsed ? "\xe2\x96\xba " : "\xe2\x96\xbc "); /* ► / ▼ (CP437 0x10/0x1F) */
    dt_tb_putc(t, '<');
    dt_tb_puts(t, tag);
    dom_emit_attrs(t, n);
    dt_tb_putc(t, '>');
  }

  if (isvoid) { /* <br>, <img>: no close */
    if (!suppress)
      dt_tb_newline(t);
    for (struct dom_node *c = n->first_child; c; c = c->next) /* normally none */
      dt_dom_emit(t, c, depth + 1, idx, suppress);
    return;
  }
  if (empty) { /* <tag></tag> on one line */
    if (!suppress) {
      dt_tb_puts(t, "</");
      dt_tb_puts(t, tag);
      dt_tb_putc(t, '>');
      dt_tb_newline(t);
    }
    return;
  }
  if (only) { /* <tag>text</tag> inline */
    if (!suppress) {
      dom_emit_text(t, only->text, 80);
      dt_tb_puts(t, "</");
      dt_tb_puts(t, tag);
      dt_tb_putc(t, '>');
      dt_tb_newline(t);
    }
    (*idx)++; /* the inlined text node  */
    return;
  }
  if (collapsed) { /* "<tag…>…</tag>" + hidden subtree */
    if (!suppress) {
      dt_tb_puts(t, "...");
      dt_tb_puts(t, "</");
      dt_tb_puts(t, tag);
      dt_tb_putc(t, '>');
      dt_tb_newline(t);
    }
    for (struct dom_node *c = n->first_child; c; c = c->next)
      dt_dom_emit(t, c, depth + 1, idx, 1); /* advance the index only */
    return;
  }

  if (!suppress)
    dt_tb_newline(t); /* open tag on its own line */
  if (hb && !suppress) {
    dom_record(my);
    dom_indent(t, depth + 1);
    dt_tb_puts(t, "::before");
    dt_tb_newline(t);
  }
  for (struct dom_node *c = n->first_child; c; c = c->next)
    dt_dom_emit(t, c, depth + 1, idx, suppress);
  if (ha && !suppress) {
    dom_record(my);
    dom_indent(t, depth + 1);
    dt_tb_puts(t, "::after");
    dt_tb_newline(t);
  }
  if (!suppress) { /* closing line = same node */
    dom_record(my);
    dom_indent(t, depth);
    dt_tb_puts(t, "</");
    dt_tb_puts(t, tag);
    dt_tb_putc(t, '>');
    dt_tb_newline(t);
  }
}

/* The node at pre-order index `target` under `n`; *idx is the running counter. */
struct dom_node *dt_dom_node_at(struct dom_node *n, int target, int *idx)
{
  if ((*idx)++ == target)
    return n;
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    struct dom_node *r = dt_dom_node_at(c, target, idx);
    if (r)
      return r;
  }
  return 0;
}

/* Inverse of dt_dom_node_at: pre-order index of `target` under `n`, or -1 (same index space). */
int dt_dom_index_of(struct dom_node *n, struct dom_node *target, int *idx)
{
  int my = (*idx)++;
  if (n == target)
    return my;
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    int r = dt_dom_index_of(c, target, idx);
    if (r >= 0)
      return r;
  }
  return -1;
}

/* Laid-out box for `node`, or NULL. The worker is the only writer, so this read is lock-free. */
static struct lbox *lbox_for(struct dom_node *node)
{
  int n = 0;
  struct lbox *p = layout_pool(&n);
  for (int i = 0; i < n; i++)
    if (p[i].node == node)
      return &p[i];
  return 0;
}

/* Append "  label: T R B L\n" for a resolved margin/border/padding edge. */
static void sty_edge(struct textblk *t, const char *label, struct ledge e)
{
  char b[48];
  int k = 0;
  sapp(b, &k, sizeof b, "  ");
  sapp(b, &k, sizeof b, label);
  sapp(b, &k, sizeof b, ": ");
  uapp(b, &k, sizeof b, (unsigned)(e.t < 0 ? 0 : e.t));
  sapp(b, &k, sizeof b, " ");
  uapp(b, &k, sizeof b, (unsigned)(e.r < 0 ? 0 : e.r));
  sapp(b, &k, sizeof b, " ");
  uapp(b, &k, sizeof b, (unsigned)(e.b < 0 ? 0 : e.b));
  sapp(b, &k, sizeof b, " ");
  uapp(b, &k, sizeof b, (unsigned)(e.l < 0 ? 0 : e.l));
  dt_tb_puts(t, b);
  dt_tb_newline(t);
}

/* Append "  label: #rrggbb\n", or nothing when the colour is unset (BG_NONE). */
static void sty_color(struct textblk *t, const char *label, unsigned c)
{
  if (c == BG_NONE)
    return;
  static const char hx[] = "0123456789abcdef";
  char b[40];
  int k = 0;
  sapp(b, &k, sizeof b, "  ");
  sapp(b, &k, sizeof b, label);
  sapp(b, &k, sizeof b, ": #");
  for (int sh = 20; sh >= 0; sh -= 4)
    b[k++] = hx[(c >> sh) & 15];
  b[k] = '\0';
  dt_tb_puts(t, b);
  dt_tb_newline(t);
}

/* Computed box model and a few resolved values for `n`, from its laid-out box. */
static void style_emit_box(struct dom_node *n)
{
  struct lbox *lb = lbox_for(n);
  dt_tb_newline(&dt_sty);
  dt_tb_puts(&dt_sty, "Computed box");
  dt_tb_newline(&dt_sty);
  if (!lb) {
    dt_tb_puts(&dt_sty, "  (inline/anon element - no box geometry)");
    dt_tb_newline(&dt_sty);
    return;
  }

  char b[64];
  int k = 0;
  sapp(b, &k, sizeof b, "  content: ");
  uapp(b, &k, sizeof b, (unsigned)(lb->cw < 0 ? 0 : lb->cw));
  sapp(b, &k, sizeof b, " x ");
  uapp(b, &k, sizeof b, (unsigned)(lb->ch < 0 ? 0 : lb->ch));
  dt_tb_puts(&dt_sty, b);
  dt_tb_newline(&dt_sty);
  sty_edge(&dt_sty, "padding", lb->padding);
  sty_edge(&dt_sty, "border", lb->border);
  sty_edge(&dt_sty, "margin", lb->margin);
  sty_color(&dt_sty, "background", lb->bg);
  if (lb->border.t || lb->border.r || lb->border.b || lb->border.l)
    sty_color(&dt_sty, "border-color", lb->border_color);
}

/* Append "  label: value\n" for a Computed-pane entry (px or free-text). */
static void sty_kv_px(struct textblk *t, const char *label, int v)
{
  if (v < 0)
    return;
  char b[64];
  int k = 0;
  sapp(b, &k, sizeof b, "  ");
  sapp(b, &k, sizeof b, label);
  sapp(b, &k, sizeof b, ": ");
  uapp(b, &k, sizeof b, (unsigned)v);
  sapp(b, &k, sizeof b, "px");
  dt_tb_puts(t, b);
  dt_tb_newline(t);
}
static void sty_kv_str(struct textblk *t, const char *label, const char *val)
{
  char b[96];
  int k = 0;
  sapp(b, &k, sizeof b, "  ");
  sapp(b, &k, sizeof b, label);
  sapp(b, &k, sizeof b, ": ");
  sapp(b, &k, sizeof b, val);
  dt_tb_puts(t, b);
  dt_tb_newline(t);
}

/* Append "  label: <track> <track> ...\n" for a grid track template. */
static void sty_tracks(struct textblk *t, const char *label, const struct css_track *tr, int n)
{
  if (n <= 0)
    return;
  char b[112];
  int k = 0;
  sapp(b, &k, sizeof b, "  ");
  sapp(b, &k, sizeof b, label);
  sapp(b, &k, sizeof b, ": ");
  for (int i = 0; i < n && i < CSS_GRID_MAX; i++) {
    if (i)
      sapp(b, &k, sizeof b, " ");
    if (tr[i].kind == CSS_TRK_PX) {
      uapp(b, &k, sizeof b, (unsigned)tr[i].v);
      sapp(b, &k, sizeof b, "px");
    } else if (tr[i].kind == CSS_TRK_PCT) {
      uapp(b, &k, sizeof b, (unsigned)tr[i].v);
      sapp(b, &k, sizeof b, "%");
    } else if (tr[i].kind == CSS_TRK_FR) {
      uapp(b, &k, sizeof b, (unsigned)tr[i].v);
      sapp(b, &k, sizeof b, "fr");
    } else
      sapp(b, &k, sizeof b, "auto");
  }
  dt_tb_puts(t, b);
  dt_tb_newline(t);
}

/* Flat "Computed" list of the values css.c models for `n`; only set properties (most use -1 for unset). */
static void style_emit_computed(struct dom_node *n)
{
  struct css_style s;
  html_compute_style(n, &s);
  dt_tb_newline(&dt_sty);
  dt_tb_puts(&dt_sty, "Computed");
  dt_tb_newline(&dt_sty);

  if (s.fam[0])
    sty_kv_str(&dt_sty, "font-family", s.fam);
  sty_kv_px(&dt_sty, "font-size", s.font_px);
  if (s.color >= 0)
    sty_color(&dt_sty, "color", (unsigned)s.color);
  if (s.bg >= 0 && (unsigned)s.bg != BG_NONE)
    sty_color(&dt_sty, "background", (unsigned)s.bg);
  if (s.bold == 1)
    sty_kv_str(&dt_sty, "font-weight", "bold");
  if (s.under == 1)
    sty_kv_str(&dt_sty, "text-decoration", "underline");
  if (s.center == 1)
    sty_kv_str(&dt_sty, "text-align", "center");
  if (s.grid == 1) {
    sty_kv_str(&dt_sty, "display", "grid");
    sty_tracks(&dt_sty, "grid-template-columns", s.grid_cols, s.n_cols);
    sty_tracks(&dt_sty, "grid-template-rows", s.grid_rows, s.n_rows);
    if (s.grid_flow == 1)
      sty_kv_str(&dt_sty, "grid-auto-flow", "column");
  } else if (s.flex == 1) {
    sty_kv_str(&dt_sty, "display", "flex");
    if (s.flex_dir >= 0)
      sty_kv_str(&dt_sty, "flex-direction", s.flex_dir == 1 ? "column" : "row");
  } else if (s.disp == CSS_DISP_INLINE)
    sty_kv_str(&dt_sty, "display", "inline");
  else if (s.disp == CSS_DISP_NONE)
    sty_kv_str(&dt_sty, "display", "none");
  else if (s.disp == CSS_DISP_CONTENTS)
    sty_kv_str(&dt_sty, "display", "contents");
  sty_kv_px(&dt_sty, "width", s.width);
  sty_kv_px(&dt_sty, "height", s.height);
  if (s.box_sizing >= 0)
    sty_kv_str(&dt_sty, "box-sizing", s.box_sizing == 1 ? "border-box" : "content-box");
  sty_kv_px(&dt_sty, "border-radius", s.radius);
  if (s.sh_color >= 0)
    sty_kv_str(&dt_sty, "box-shadow", "set");
  if (s.overflow >= 0)
    sty_kv_str(&dt_sty, "overflow", s.overflow == 1 ? "clipped" : "visible");
  if (s.gap_row >= 0)
    sty_kv_px(&dt_sty, "row-gap", s.gap_row);
  if (s.gap_col >= 0)
    sty_kv_px(&dt_sty, "column-gap", s.gap_col);
}

/* Application tab snapshot: cookies and HSTS hosts from B->cookies/B->hsts, built in the worker tick. */

/* FNV-1a signature of the cookie/HSTS jars (no mutation counter), gating Application rebuilds. */
static unsigned app_fnv(unsigned h, const char *s)
{
  if (s)
    for (; *s; s++) {
      h ^= (unsigned char)*s;
      h *= 16777619u;
    }
  return h;
}
unsigned dt_app_sig(void)
{
  unsigned h = 2166136261u;
  for (struct cookie *c = B->cookies; c; c = c->next) {
    h = app_fnv(h, c->name);
    h = app_fnv(h, c->val);
    h = app_fnv(h, c->host);
    h ^= 0x9e3779b9u;
  }
  for (struct hsts_host *hh = B->hsts; hh; hh = hh->next) {
    h = app_fnv(h, hh->host);
    h ^= 0x85ebca6bu;
  }
  return h;
}

void dt_app_emit(void)
{
  dt_tb_reset(&dt_app);
  char b[16];
  int nc = 0;
  for (struct cookie *c = B->cookies; c; c = c->next)
    nc++;
  dt_tb_puts(&dt_app, "Cookies (");
  {
    int k = 0;
    uapp(b, &k, sizeof b, (unsigned)nc);
    dt_tb_puts(&dt_app, b);
  }
  dt_tb_puts(&dt_app, ")");
  dt_tb_newline(&dt_app);
  if (!nc) {
    dt_tb_puts(&dt_app, "  (none)");
    dt_tb_newline(&dt_app);
  }
  for (struct cookie *c = B->cookies; c; c = c->next) {
    dt_tb_puts(&dt_app, "  ");
    dt_tb_puts(&dt_app, c->name ? c->name : "(unnamed)");
    dt_tb_putc(&dt_app, '=');
    dom_emit_text(&dt_app, c->val ? c->val : "", 64);
    dt_tb_puts(&dt_app, "   @ ");
    dt_tb_puts(&dt_app, c->host ? c->host : "");
    dt_tb_newline(&dt_app);
  }
  dt_tb_newline(&dt_app);
  int nh = 0;
  for (struct hsts_host *h = B->hsts; h; h = h->next)
    nh++;
  dt_tb_puts(&dt_app, "HSTS (");
  {
    int k = 0;
    uapp(b, &k, sizeof b, (unsigned)nh);
    dt_tb_puts(&dt_app, b);
  }
  dt_tb_puts(&dt_app, ")");
  dt_tb_newline(&dt_app);
  if (!nh) {
    dt_tb_puts(&dt_app, "  (none)");
    dt_tb_newline(&dt_app);
  }
  for (struct hsts_host *h = B->hsts; h; h = h->next) {
    dt_tb_puts(&dt_app, "  ");
    dt_tb_puts(&dt_app, h->host ? h->host : "");
    dt_tb_newline(&dt_app);
  }
}

/* Build the Styles view: inline style, matching rules (winner first, with selector and @media), then box model. */
void dt_style_emit(struct dom_node *root)
{
  dt_tb_reset(&dt_sty);
  if (!root) {
    dt_tb_puts(&dt_sty, "(no document loaded)");
    dt_tb_newline(&dt_sty);
    return;
  }
  if (dt_sel < 0) {
    dt_tb_puts(&dt_sty, "Select an element in the Elements tab.");
    dt_tb_newline(&dt_sty);
    return;
  }
  int idx = 0;
  struct dom_node *n = dt_dom_node_at(root, dt_sel, &idx);
  if (!n) {
    dt_tb_puts(&dt_sty, "(selection no longer in the tree)");
    dt_tb_newline(&dt_sty);
    return;
  }
  if (n->type == DOM_TEXT) {
    dt_tb_puts(&dt_sty, "(text node - no styles)");
    dt_tb_newline(&dt_sty);
    return;
  }

  dt_tb_putc(&dt_sty, '<');
  dt_tb_puts(&dt_sty, n->tag);
  const char *id = dom_get_attr(n, "id");
  if (id && id[0]) {
    dt_tb_putc(&dt_sty, '#');
    dt_tb_puts(&dt_sty, id);
  }
  const char *cls = dom_get_attr(n, "class");
  if (cls && cls[0]) {
    for (const char *p = cls; *p;) {
      while (*p == ' ')
        p++;
      if (!*p)
        break;
      dt_tb_putc(&dt_sty, '.');
      while (*p && *p != ' ')
        dt_tb_putc(&dt_sty, *p++);
    }
  }
  dt_tb_putc(&dt_sty, '>');
  dt_tb_newline(&dt_sty);
  dt_tb_newline(&dt_sty);

  /* element.style {}: the inline style="" attribute, verbatim. */
  const char *inl = dom_get_attr(n, "style");
  if (inl && inl[0]) {
    dt_tb_puts(&dt_sty, "element.style {");
    dt_tb_newline(&dt_sty);
    dt_tb_puts(&dt_sty, "  ");
    for (const char *p = inl; *p; p++) { /* one declaration per line */
      dt_tb_putc(&dt_sty, *p);
      if (*p == ';' && p[1]) {
        dt_tb_newline(&dt_sty);
        dt_tb_puts(&dt_sty, "  ");
      }
    }
    dt_tb_newline(&dt_sty);
    dt_tb_puts(&dt_sty, "}");
    dt_tb_newline(&dt_sty);
    dt_tb_newline(&dt_sty);
  }

  /* Matching rules, winner first, grouped under their @media when any. */
  struct css_match m[64];
  int nm = html_match_styles(n, m, 64);
  for (int i = 0; i < nm; i++) {
    const char *base = "";
    if (m[i].media[0]) {
      dt_tb_puts(&dt_sty, m[i].media);
      if (!m[i].active)
        dt_tb_puts(&dt_sty, "   /* not currently matched */");
      dt_tb_puts(&dt_sty, " {");
      dt_tb_newline(&dt_sty);
      base = "  ";
    }
    dt_tb_puts(&dt_sty, base);
    dt_tb_puts(&dt_sty, m[i].selector);
    dt_tb_puts(&dt_sty, " {");
    dt_tb_newline(&dt_sty);

    char ind[8];
    int k = 0;
    while (base[k]) {
      ind[k] = base[k];
      k++;
    }
    ind[k++] = ' ';
    ind[k++] = ' ';
    ind[k] = '\0';
    /* Authored declarations verbatim, including properties css.c does not model. */
    if (m[i].raw && m[i].raw[0]) {
      const char *seg = m[i].raw;
      while (*seg) {
        const char *e = seg, *s2;
        while (*e && *e != ';')
          e++;
        s2 = seg;
        while (s2 < e && *s2 == ' ')
          s2++;
        if (e > s2) {
          dt_tb_puts(&dt_sty, ind);
          while (s2 < e)
            dt_tb_putc(&dt_sty, *s2++);
          dt_tb_putc(&dt_sty, ';');
          dt_tb_newline(&dt_sty);
        }
        seg = *e ? e + 1 : e;
      }
    } else {
      dt_tb_puts(&dt_sty, ind);
      dt_tb_puts(&dt_sty, "/* (empty rule) */");
      dt_tb_newline(&dt_sty);
    }
    dt_tb_puts(&dt_sty, base);
    dt_tb_puts(&dt_sty, "}");
    dt_tb_newline(&dt_sty);
    if (m[i].media[0]) {
      dt_tb_puts(&dt_sty, "}");
      dt_tb_newline(&dt_sty);
    }
  }
  if (nm == 0 && !(inl && inl[0])) {
    dt_tb_puts(&dt_sty, "No matching CSS rules.");
    dt_tb_newline(&dt_sty);
  }

  style_emit_box(n);
  style_emit_computed(n);
}
