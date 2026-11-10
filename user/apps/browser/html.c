/* html.c - the renderer */
#include "html_int.h"

css_sheet *g_sheet; /* the page's collected <style> rules, per render */

/* url() background loader/lookup - defined lower, used during the layout walk */

/* The DOM + CSS sheet now persist for the whole page lifetime (the JS context in js.c does too) */
struct dom_node *g_live_dom;
char g_cur_page_url[URL_MAX];

/* Base URL the walk resolves relative links/sources against */
char g_base_url[URL_MAX];
int g_frame_depth;
bool g_link_nav = true;

/* CSP enforcement runs against the top document (B->pg->csp) */
static bool g_csp_report;

void csp_report(const char *what, const char *url, const char *reason)
{
  if (!g_csp_report)
    return;
  char m[URL_MAX + 160];
  int n = 0;
  sapp(m, &n, sizeof m, "[CSP] refused to load ");
  sapp(m, &n, sizeof m, what);
  sapp(m, &n, sizeof m, " ");
  sapp(m, &n, sizeof m, url);
  if (reason && reason[0]) {
    sapp(m, &n, sizeof m, " (");
    sapp(m, &n, sizeof m, reason);
    sapp(m, &n, sizeof m, ")");
  }
  sapp(m, &n, sizeof m, "\n");
  devtools_console_append(DT_ERROR, m, n);
}

/* CSP gate for a sub-resource URL load (img/style/font/frame) */
bool csp_url_ok(int type, const char *abs, const char *what, char *out, int outcap)
{
  if (g_frame_depth > 0 || !B->pg->csp) {
    int k = 0;
    out[0] = '\0';
    while (abs[k] && k < outcap - 1) {
      out[k] = abs[k];
      k++;
    }
    out[k] = '\0';
    return true;
  }
  char reason[64];
  if (csp_check_url(B->pg->csp, type, abs, g_cur_page_url, out, outcap, reason, sizeof reason))
    return true;
  csp_report(what, abs, reason);
  return false;
}

/* style-src gate for an inline style="" attribute (no nonce/hash for attrs). */
static bool csp_style_attr_ok(void)
{
  if (g_frame_depth > 0 || !B->pg->csp)
    return true;
  return csp_check_inline(B->pg->csp, CSP_STYLE, 0, 0, 0, 0, 0);
}

static unsigned g_doc_adopted_key;
static int g_adopted_mark;
unsigned g_shadow_style_gen; /* bumped whenever a shadow tree's sheet is rebuilt */

static void free_sheet_cb(void *sheet) { css_free(sheet); }

/* The shadow root `el` lives in, or NULL for the page's own tree */
static const struct dom_node *tree_root_shadow(const struct dom_node *el)
{
  while (el->parent)
    el = el->parent;
  return el->host ? el : 0;
}

/* The sheet whose rules apply to `el`: a shadow tree sees only its own <style>s, never the page's */
css_sheet *sheet_for(const struct dom_node *el)
{
  const struct dom_node *root = tree_root_shadow(el);
  return root ? root->shadow_sheet : g_sheet;
}

static unsigned hash_bytes(unsigned h, const char *s, int n)
{
  for (int i = 0; i < n; i++)
    h = (h ^ (unsigned char)s[i]) * 16777619u;
  return h;
}

struct txt {
  char *b;
  int n, cap;
};
static void txt_add(struct txt *t, const char *s, int len)
{
  if (t->n + len + 1 > t->cap) {
    int nc = (t->n + len + 1) * 2;
    char *nb = realloc(t->b, (unsigned)nc);
    if (!nb)
      return;
    t->b = nb;
    t->cap = nc;
  }
  memcpy(t->b + t->n, s, (unsigned)len);
  t->n += len;
  t->b[t->n] = '\0';
}

/* One stylesheet source of a shadow tree: inline <style> text, a fetched <link>, or adopted CSS */
#define SHADOW_PIECES_MAX 48
struct piece {
  char *text;
  int len;
  const char *base; /* what relative url()s resolve against */
  bool own;         /* text is heap memory this piece must free */
};

/* Fetched shadow-tree <link rel=stylesheet> bodies, kept for the page's life so a relayout never refetches */
#define LINK_CACHE_MAX 64
static struct {
  char *url, *body;
  int len;
} g_link_cache[LINK_CACHE_MAX];
static int g_n_link_cache;

static void link_cache_reset(void)
{
  for (int i = 0; i < g_n_link_cache; i++) {
    free(g_link_cache[i].url);
    free(g_link_cache[i].body);
  }
  g_n_link_cache = 0;
}

static int link_cache_find(const char *url)
{
  for (int i = 0; i < g_n_link_cache; i++)
    if (streq(g_link_cache[i].url, url))
      return i;
  return -1;
}

/* The absolute URL of a stylesheet <link> in a shadow tree, or false when it is not one (or CSP refuses it) */
static bool shadow_link_url(const struct dom_node *c, char *out, int cap)
{
  const char *rel = dom_get_attr(c, "rel"), *href = dom_get_attr(c, "href");
  char abs[URL_MAX];
  if (!rel || !href || !href[0] || !contains_ci(rel, "stylesheet") || contains_ci(rel, "alternate"))
    return false;
  resolve_url(g_page_url, href, abs, sizeof abs);
  return csp_url_ok(CSP_STYLE, abs, "stylesheet", out, cap);
}

/* Fetch the not-yet-cached <link> sheets under `n` in one batch */
static void fetch_shadow_links(const struct dom_node *n)
{
  char *urls[8];
  int nu = 0;
  for (const struct dom_node *c = n->first_child; c; c = c->next) {
    char use[URL_MAX];
    if (c->type != DOM_ELEMENT)
      continue;
    if (streq(c->tag, "link") && shadow_link_url(c, use, sizeof use) && link_cache_find(use) < 0 && nu < 8 &&
        g_n_link_cache + nu < LINK_CACHE_MAX)
      urls[nu++] = strdup(use);
    else if (!streq(c->tag, "style"))
      fetch_shadow_links(c);
  }
  if (!nu)
    return;
  unsigned char *bodies[8] = {0};
  int lens[8] = {0};
  if (g_page_fetch)
    g_page_fetch(urls, nu, bodies, lens);
  for (int i = 0; i < nu; i++) { /* a failed fetch is cached empty so it is not retried every layout */
    g_link_cache[g_n_link_cache].url = urls[i];
    g_link_cache[g_n_link_cache].body = bodies[i] && lens[i] > 0 ? (char *)bodies[i] : 0;
    g_link_cache[g_n_link_cache].len = bodies[i] && lens[i] > 0 ? lens[i] : 0;
    if (!g_link_cache[g_n_link_cache].body)
      free(bodies[i]);
    g_n_link_cache++;
  }
}

/* Collect the sources under `n` that belong to the same tree (a nested shadow root is its own tree), in order */
static void gather_pieces(const struct dom_node *n, struct piece *pc, int *np)
{
  for (const struct dom_node *c = n->first_child; c && *np < SHADOW_PIECES_MAX; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    if (streq(c->tag, "style")) {
      struct txt one = {0, 0, 0};
      for (const struct dom_node *t = c->first_child; t; t = t->next)
        if (t->type == DOM_TEXT && t->text)
          txt_add(&one, t->text, (int)strlen(t->text));
      char reason[64];
      if (one.b && (!B->pg->csp || csp_check_inline(B->pg->csp, CSP_STYLE, dom_get_attr(c, "nonce"),
                                                    (const unsigned char *)one.b, one.n, reason, sizeof reason))) {
        pc[*np].text = one.b;
        pc[*np].len = one.n;
        pc[*np].own = true;
        pc[(*np)++].base = g_page_url;
      } else {
        free(one.b);
      }
    } else if (streq(c->tag, "link")) {
      char use[URL_MAX];
      int ci;
      if (shadow_link_url(c, use, sizeof use) && (ci = link_cache_find(use)) >= 0 && g_link_cache[ci].body) {
        pc[*np].text = g_link_cache[ci].body;
        pc[*np].len = g_link_cache[ci].len;
        pc[*np].own = false;
        pc[(*np)++].base = g_link_cache[ci].url;
      }
    } else {
      gather_pieces(c, pc, np);
    }
  }
}

/* Rebuild the sheet of every shadow root whose sources (<style>, <link>, adopted sheets) changed */
static void refresh_shadow_sheets(struct dom_node *n)
{
  for (struct dom_node *c = n->first_child; c; c = c->next)
    if (c->type == DOM_ELEMENT)
      refresh_shadow_sheets(c);
  struct dom_node *root = n->shadow;
  if (!root)
    return;
  refresh_shadow_sheets(root);
  struct piece pc[SHADOW_PIECES_MAX + 1];
  int np = 0;
  fetch_shadow_links(root);
  gather_pieces(root, pc, &np);
  char *adopted = shadow_adopted_css(root);
  if (adopted && np <= SHADOW_PIECES_MAX) {
    pc[np].text = adopted;
    pc[np].len = (int)strlen(adopted);
    pc[np].own = false;
    pc[np++].base = g_page_url;
  }
  unsigned key = 2166136261u;
  for (int i = 0; i < np; i++) {
    key = hash_bytes(key, pc[i].text, pc[i].len);
    key = hash_bytes(key, pc[i].base, (int)strlen(pc[i].base));
  }
  key = np ? key | 1u : 0; /* 0 means no sheet */
  if (key != root->shadow_sheet_key) {
    dom_on_free_sheet = free_sheet_cb;
    if (root->shadow_sheet)
      css_free(root->shadow_sheet);
    root->shadow_sheet = key ? css_new() : 0;
    for (int i = 0; root->shadow_sheet && i < np; i++)
      css_add(root->shadow_sheet, pc[i].text, pc[i].len, pc[i].base);
    root->shadow_sheet_key = key;
    g_shadow_style_gen++;
  }
  for (int i = 0; i < np; i++)
    if (pc[i].own)
      free(pc[i].text);
  free(adopted);
}

/* Computed style for one element */
void compute_style(struct dom_node *el, struct css_style *cs)
{
  unsigned seq = style_key();
  css_sheet *sheet = sheet_for(el);
  dom_on_free_style_cache = free_style_cache;
  if (el->computed_style && el->computed_style_sheet == sheet && el->computed_style_seq == seq) {
    *cs = *(struct css_style *)el->computed_style;
    return;
  }
  css_style_reset(cs);
  css_apply_attrs(el, cs);
  css_set_primary(g_sheet);
  /* Shadow-tree rules that reach `el` from inside lose to the tree it lives in: :host and ::slotted come first */
  if (el->shadow && el->shadow->shadow_sheet) {
    css_set_scope(el, CSS_SCOPE_HOST_ONLY);
    css_apply_sheet(el->shadow->shadow_sheet, el, cs);
  }
  if (el->parent && el->parent->shadow && el->parent->shadow->shadow_sheet) {
    css_set_scope(el->parent, CSS_SCOPE_SLOTTED);
    css_apply_sheet(el->parent->shadow->shadow_sheet, el, cs);
  }
  const struct dom_node *sroot = tree_root_shadow(el);
  css_set_scope(sroot ? sroot->host : 0, CSS_SCOPE_NORMAL);
  css_apply_sheet(sheet, el, cs);
  if (sroot && sroot->host) { /* ::part() rules of the tree around the host win over the component's own */
    css_sheet *outer = sheet_for(sroot->host);
    if (outer) {
      css_set_scope(sroot->host, CSS_SCOPE_PART);
      css_apply_sheet(outer, el, cs);
    }
  }
  css_set_scope(0, CSS_SCOPE_NORMAL);
  const char *inl = dom_get_attr(el, "style");
  if (inl && csp_style_attr_ok())
    css_apply_inline(inl, cs);
  css_apply_fullscreen(el, cs);
  if (!el->computed_style)
    el->computed_style = malloc(sizeof *cs);
  if (el->computed_style) {
    *(struct css_style *)el->computed_style = *cs;
    el->computed_style_sheet = sheet;
    el->computed_style_seq = seq;
  }
}

/* devtools hooks: the live DOM root and a public wrapper over compute_style */
struct dom_node *html_dom_root(void) { return g_live_dom; }
void html_compute_style(struct dom_node *el, struct css_style *out) { compute_style(el, out); }
int html_match_styles(struct dom_node *el, struct css_match *out, int cap)
{
  return css_matches(sheet_for(el), el, out, cap);
}
/* 1 if `el` has a ::before/::after (CSS_PSEUDO_*) with content - for devtools */
int html_has_pseudo(struct dom_node *el, int which)
{
  struct css_style cs;
  return css_pseudo_content(sheet_for(el), el, which, &cs);
}

/* True when `s` already carries a URL scheme that resolve_url won't expand, so it must be used */
bool url_absolute(const char *s)
{
  return starts_ci(s, "http://") || starts_ci(s, "https://") || starts_ci(s, "data:") || starts_ci(s, "file:") ||
         starts_ci(s, "blob:") || starts_ci(s, "mailto:");
}

/* Source charset for the document being parsed */
enum { CS_UTF8, CS_LATIN1 };
static int g_charset;
static bool g_charset_from_header;

/* True if `cs` (a charset name, possibly inside a longer string) is a Latin-1 / Windows-1252 */
static bool charset_is_latin1(const char *cs)
{
  return contains_ci(cs, "8859-1") || contains_ci(cs, "latin-1") || contains_ci(cs, "latin1") ||
         contains_ci(cs, "1252") || contains_ci(cs, "8859_1");
}

/* fetch.c calls this with the raw Content-Type header value (may be NULL) */
void html_set_charset(const char *content_type)
{
  g_charset = CS_UTF8;
  g_charset_from_header = false;
  if (!content_type)
    return;
  const char *cs = content_type;
  for (; *cs; cs++)
    if (starts_ci(cs, "charset"))
      break;
  if (!*cs)
    return; /* header gave no charset */
  g_charset_from_header = true;
  if (charset_is_latin1(cs))
    g_charset = CS_LATIN1;
}

/* Scan the document head for <meta charset=...> / <meta http-equiv="Content-Type" */
static void detect_meta_charset(void)
{
  if (g_charset_from_header)
    return;
  const char *p = B->pg->raw, *end = B->pg->raw + B->pg->raw_len;
  for (; p < end; p++) {
    if (*p != '<' || !starts_ci(p, "<meta"))
      continue;
    const char *q = p, *tend = p;
    while (tend < end && *tend != '>')
      tend++; /* this tag's extent */
    for (q = p; q < tend; q++)
      if (starts_ci(q, "charset")) {
        if (charset_is_latin1(q))
          g_charset = CS_LATIN1;
        return; /* first one wins */
      }
    p = tend;
  }
}

/* Read one source character, transcoding per the active charset, advancing `*pp` */
int read_char(const char **pp, const char *end)
{
  const char *p = *pp;
  unsigned char b = (unsigned char)*p;
  if (b < 0x80) {
    *pp = p + 1;
    return b;
  }
  if (g_charset == CS_LATIN1) {
    *pp = p + 1;
    return cp1252_high(b);
  }
  unsigned u;
  *pp = p + utf8_decode(p, end, &u);
  return (int)u;
}

struct sty sty_cur;

unsigned char sty_byte(void)
{
  unsigned char a = 0;
  if (sty_cur.bold)
    a |= ST_BOLD;
  if (sty_cur.under)
    a |= ST_UNDER;
  if (sty_cur.italic)
    a |= ST_ITALIC;
  if (sty_cur.strike)
    a |= ST_STRIKE;
  if (sty_cur.nowrap)
    a |= ST_NOWRAP;
  if (sty_cur.hidden)
    a |= ST_HIDDEN;
  if (sty_cur.clip)
    a |= ST_CLIP | (sty_cur.ellipsis ? ST_ELLIPSIS : 0);
  return a;
}

/* User-agent default style for a tag (the lowest layer */
void apply_ua_style(const char *name)
{
  if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
    sty_cur.bold = 1;
  } else if (streq(name, "b") || streq(name, "strong")) {
    sty_cur.bold = 1;
  } else if (streq(name, "i") || streq(name, "em") || streq(name, "cite") || streq(name, "var") || streq(name, "dfn") ||
             streq(name, "address")) {
    sty_cur.italic = 1;
  } else if (streq(name, "a")) {
    sty_cur.fg = 0x1a4fbf;
    sty_cur.under = 1;
  } else if (streq(name, "u") || streq(name, "ins")) {
    sty_cur.under = 1;
  } else if (streq(name, "del") || streq(name, "s") || streq(name, "strike")) {
    sty_cur.strike = 1;
  } else if (streq(name, "code") || streq(name, "kbd") || streq(name, "samp") || streq(name, "tt")) {
    sty_cur.fg = 0x9a6020;
  } else if (streq(name, "legend")) {
    sty_cur.bold = 1;
  } else if (streq(name, "mark")) {
    sty_cur.fg = 0xc01818;
  }
}

/* Layer a computed CSS style over the UA defaults already in sty_cur. */
void apply_css(const struct css_style *cs)
{
  if (cs->color >= 0)
    sty_cur.fg = (unsigned)cs->color;
  if (cs->bold >= 0)
    sty_cur.bold = cs->bold ? 1 : 0;
  if (cs->under >= 0)
    sty_cur.under = cs->under ? 1 : 0;
  if (cs->strike >= 0)
    sty_cur.strike = cs->strike ? 1 : 0;
  if (cs->italic >= 0)
    sty_cur.italic = cs->italic ? 1 : 0;
  if (cs->small_caps >= 0)
    sty_cur.smallcaps = cs->small_caps ? 1 : 0;
  if (cs->ttx >= 0)
    sty_cur.ttx = (unsigned char)cs->ttx;
  if (cs->visibility >= 0)
    sty_cur.hidden = cs->visibility == 0;
  if (cs->letter_sp != CSS_LS_UNSET)
    sty_cur.ls = (signed char)(cs->letter_sp < -8 ? -8 : cs->letter_sp > 32 ? 32 : cs->letter_sp);
  if (cs->tab_size >= 0)
    sty_cur.tabsize = (unsigned char)cs->tab_size;
}

/* The renderer's line-break set (distinct from dom.c's CSS-block set */
bool is_block_tag(const char *name)
{
  static const char *const blk[] = {
      "p",          "br",      "div",    "li",       "tr",     "ul",      "ol",      "h1",     "h2",         "h3",
      "h4",         "h5",      "h6",     "table",    "hr",     "section", "article", "header", "footer",     "nav",
      "blockquote", "pre",     "head",   "form",     "dd",     "dt",      "dl",      "figure", "figcaption", "main",
      "aside",      "details", "dialog", "fieldset", "legend", "caption", "address", 0};
  for (int i = 0; blk[i]; i++)
    if (streq(name, blk[i]))
      return true;
  return false;
}

/* Reset the render buffers and re-flatten the live DOM into them */
/* Alpha-composite src (0xAARRGGBB) over the opaque dst (0xRRGGBB). */
static unsigned composite_over(unsigned dst, unsigned src)
{
  unsigned a = (src >> 24) & 0xff;
  if (a == 0)
    return dst;
  if (a == 255)
    return src & 0xFFFFFF;
  unsigned sr = (src >> 16) & 0xff, sg = (src >> 8) & 0xff, sb = src & 0xff;
  unsigned dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
  unsigned ia = 255 - a;
  unsigned r = (sr * a + dr * ia) / 255, g = (sg * a + dg * ia) / 255, b = (sb * a + db * ia) / 255;
  return (r << 16) | (g << 8) | b;
}

/* The canvas background and default text colour, from the <html>/<body> cascade */
static void page_scrollbar_default(void)
{
  B->pg->sb_w = SCROLL_W;
  B->pg->sb_st = SCROLLBAR_DEFAULT;
}

/* Fold the root element's scrollbar-width, scrollbar-color and ::-webkit-scrollbar* rules into the page's scrollbar. */
static void page_scrollbar_style(struct dom_node *el, const struct css_style *cs)
{
  struct scrollbar_style *st = &B->pg->sb_st;
  if (cs->sb_w != -1) {
    B->pg->sb_w = cs->sb_w >= 0 ? cs->sb_w : SCROLL_W;
    st->arrows = cs->sb_w < 0 || cs->sb_w >= SCROLL_W;
  }
  if (cs->sb_thumb != -1) {
    st->thumb = cs->sb_thumb;
    st->track = cs->sb_track;
  }
  struct css_sbparts p;
  if (!css_scrollbar_parts(sheet_for(el), el, &p))
    return;
  if (p.hidden)
    B->pg->sb_w = 0;
  else if (p.width >= 0) {
    B->pg->sb_w = p.width > 24 ? 24 : p.width;
    st->arrows = 0; /* a restyled bar has no buttons unless the page draws them */
  }
  if (p.track >= 0)
    st->track = p.track;
  if (p.thumb >= 0)
    st->thumb = p.thumb;
  if (p.radius >= 0)
    st->radius = p.radius;
}

static void compute_page_colors(struct dom_node *root)
{
  unsigned canvas = 0xffffff;
  B->pg->page_fg = 0x202830;
  page_scrollbar_default();
  struct css_style cs;
  const char *who[] = {"html", "body"};
  for (int i = 0; i < 2; i++) {
    struct dom_node *el = find_tag(root, who[i]);
    if (!el)
      continue;
    compute_style(el, &cs);
    if (cs.color >= 0)
      B->pg->page_fg = (unsigned)cs.color;
    page_scrollbar_style(el, &cs);
    unsigned argb;
    if (css_bg_argb(sheet_for(el), el, &argb))
      canvas = composite_over(canvas, argb);
  }
  B->pg->page_bg = canvas;
}

static struct dom_node *g_hover, *g_pressed; /* element under the pointer, and the one held down */

bool html_set_pointer(int off, bool pressed)
{
  struct dom_node *h = off >= 0 ? html_node_at_offset(off) : 0;
  unsigned before = css_state_generation();
  g_hover = h;
  g_pressed = pressed ? h : 0;
  css_set_state(g_hover, g_pressed, g_focus);
  return css_state_generation() != before;
}

static void layout_dom(void)
{
  css_set_viewport(BW, BH); /* so @media re-evaluates at the current size */
  css_set_state(g_hover, g_pressed, g_focus);
  g_walk_now = msec();
  anim_frame_begin(g_walk_now); /* advance the animation clock for this walk */
  B->pg->nlinks = 0;
  B->pg->nregs = 0;
  if (B->img_pool_len > IMG_POOL / 2 || !img_carry_begin(&g_carry, B->pg->imgs, B->pg->n_imgs))
    B->img_pool_len = 0; /* nothing carried: the pool restarts empty */
  B->pg->n_imgs = 0;
  char *doc_adopted = document_adopted_css();
  unsigned adopted_key = doc_adopted ? hash_bytes(2166136261u, doc_adopted, (int)strlen(doc_adopted)) : 0;
  if (adopted_key != g_doc_adopted_key && g_sheet) { /* adopted rules sit after the page's own, so replace them */
    if (g_doc_adopted_key)
      css_truncate(g_sheet, g_adopted_mark);
    g_adopted_mark = css_mark(g_sheet);
    if (doc_adopted)
      css_add(g_sheet, doc_adopted, (int)strlen(doc_adopted), g_page_url);
    g_doc_adopted_key = adopted_key;
  }
  free(doc_adopted);
  refresh_shadow_sheets(g_live_dom);
  compute_page_colors(g_live_dom);
  sty_cur.fg = B->pg->page_fg;
  sty_cur.bold = sty_cur.under = 0;
  sty_cur.italic = sty_cur.nowrap = sty_cur.ttx = sty_cur.smallcaps = 0;
  sty_cur.hidden = sty_cur.clip = sty_cur.ellipsis = 0;
  sty_cur.tabsize = 4;
  sty_cur.ls = sty_cur.va = 0;
  B->pg->has_ls = false;
  W_bg = BG_NONE;
  W_lineheight = 0;
  W_liststyle = CSS_LST_DEFAULT;
  W_listtype = 0;
  W_olnum = 0;
  W_listrev = 1;
  W_listdef = CSS_LST_DEFAULT;
  W_n = 0;
  W_pend_sp = false;
  W_run_nl = 1;
  W_cur_link = -1;
  g_nspans = 0;
  g_nbg = 0;      /* url() backgrounds re-collected during this walk */
  layout_reset(); /* fresh box tree for this walk */
  g_has_controls = false;
  /* Reset the frame walk context to the top document (the cache itself persists across relayouts */
  g_frame_depth = 0;
  g_link_nav = true;
  g_walk_depth = 0;
  {
    int k = 0;
    g_base_url[0] = '\0';
    sapp(g_base_url, &k, sizeof g_base_url, g_cur_page_url);
  }
  title_walk_begin();
  if (g_live_dom)
    walk(g_live_dom);
  title_walk_publish(); /* also clears it when a script removed the <title> */
  img_carry_end(&g_carry);
  B->img_pool_len = img_carry_pool_len(B->pg->imgs, B->pg->n_imgs, B->img_pool_len);
  B->pg->text_len = W_n;
  B->layout_dirty = 1; /* box tree + text rebuilt → main thread must re-wrap */
}

/* Free the current page's live DOM, CSS sheet and JS context */
void html_page_teardown(void)
{
  js_page_end();
  B->fs_want = 0; /* fullscreen ends with the document */
  canvas_page_end(g_live_dom);
  media_page_teardown(); /* stop playback + free this page's media */
  if (g_live_dom) {
    dom_free(g_live_dom);
    g_live_dom = 0;
  }
  if (g_sheet) {
    css_free(g_sheet);
    g_sheet = 0;
  }
  link_cache_reset();
  frames_free(); /* drop this page's iframe sub-documents */
  dom_on_free_style_cache = 0;
  layout_free(); /* drop the box tree */
  anim_reset();  /* drop per-element animation/transition state */
  g_nspans = 0;
  g_focus = 0;
  g_hover = g_pressed = 0;
  css_set_state(0, 0, 0);
  B->focus_kind = 0;
}

/* --- iframe fetch handshake (worker thread */

int html_iframes_pending(void)
{
  for (struct frame_doc *f = g_frames; f; f = f->next)
    if (f->verdict == 0)
      return 1;
  return 0;
}

int html_iframe_next_pending(char *url, int url_cap, char *parent, int parent_cap)
{
  for (struct frame_doc *f = g_frames; f; f = f->next) {
    if (f->verdict != 0)
      continue;
    int k = 0;
    url[0] = '\0';
    sapp(url, &k, url_cap, f->url);
    k = 0;
    parent[0] = '\0';
    sapp(parent, &k, parent_cap, f->parent);
    return 1;
  }
  return 0;
}

void html_iframe_resolve(const char *url, const char *parent, int verdict, const char *reason,
                         const unsigned char *body, int len, const char *ctype)
{
  struct frame_doc *f = frame_find(url, parent);
  if (!f)
    return;

  if (f->doc) {
    dom_free(f->doc);
    f->doc = 0;
  }
  if (f->sheet) {
    css_free(f->sheet);
    f->sheet = 0;
  }
  {
    int k = 0;
    f->reason[0] = '\0';
    if (reason)
      sapp(f->reason, &k, sizeof f->reason, reason);
  }

  if (verdict == FRAME_OK) {
    /* Render an HTML body (or an empty/sloppy Content-Type, the common case) */
    int as_html = !ctype || !ctype[0] || contains_ci(ctype, "html");
    if (as_html && body && len > 0) {
      f->doc = dom_parse((const char *)body, len);
      f->sheet = css_new();
      if (f->doc && f->sheet)
        collect_inline_styles(f->doc, f->sheet, f->url);
      if (!f->doc) {
        f->verdict = FRAME_ERROR;
        int k = 0;
        f->reason[0] = '\0';
        sapp(f->reason, &k, sizeof f->reason, "could not parse");
        return;
      }
    } else if (!as_html) {
      int k = 0;
      f->reason[0] = '\0';
      sapp(f->reason, &k, sizeof f->reason, "non-HTML content");
    }
    f->verdict = FRAME_OK;
  } else {
    f->verdict = (verdict == FRAME_BLOCKED) ? FRAME_BLOCKED : FRAME_ERROR;
  }
}

/* Re-flatten the live DOM into B's buffers after a script mutated it (run by the worker right */
void html_relayout(void)
{
  if (!g_live_dom)
    return;
  layout_dom(); /* sets B->layout_dirty so the main thread re-wraps */
  B->seq++;
}

void html_to_text(const char *page_url, res_batch_fn fetch)
{
  html_page_teardown(); /* reclaim the previous page's live DOM/CSS/JS */

  B->pg->doc_title[0] = '\0';
  B->pg->nlinks = 0;
  B->pg->nregs = 0;
  B->pg->n_imgs = 0;
  B->img_pool_len = 0;
  B->pg->favicon_href[0] = '\0';
  B->pg->favicon_pri = 0;

  {
    int k = 0;
    sapp(g_cur_page_url, &k, sizeof g_cur_page_url, page_url ? page_url : "");
  }
  g_page_url = g_cur_page_url; /* persists with the page (was a caller pointer) */
  g_nsrc = 0;
  g_n_css_url = 0;

  detect_meta_charset(); /* header charset (if any) already set by fetch.c */
  g_csp_report = true;   /* log CSP violations for this initial load only */

  struct dom_node *root = dom_parse(B->pg->raw, B->pg->raw_len);
  g_sheet = css_new();
  g_doc_adopted_key = 0;
  link_cache_reset();
  g_live_dom = root; /* the live tree  -  kept until the next teardown  */
  if (root) {
    dom_attach_declarative_shadows(root);
    scan_meta_csp(root); /* fold <meta http-equiv=CSP> into B->pg->csp before any load */
    scan_resource_hints(root);
    /* Link:-header sheets come before the document's own (then tree order). */
    for (struct css_link *L = B->ext_css; L; L = L->next)
      src_link(L->url);
    collect_styles(root);

    /* Batch-fetch the external sheets, then add every source in order. */
    unsigned char **bodies = g_n_css_url ? calloc((unsigned)g_n_css_url, sizeof *bodies) : 0;
    int *lens = g_n_css_url ? calloc((unsigned)g_n_css_url, sizeof *lens) : 0;
    if (fetch && g_n_css_url > 0 && bodies && lens)
      fetch(g_css_url, g_n_css_url, bodies, lens);

    for (int s = 0; s < g_nsrc; s++) {
      if (g_src[s].text) {
        css_add(g_sheet, g_src[s].text, g_src[s].len, g_page_url);
      } else {
        int ui = g_src[s].url_i;
        if (ui >= 0 && bodies && bodies[ui])
          css_add(g_sheet, (char *)bodies[ui], lens[ui], g_css_url[ui]);
      }
    }
    for (int i = 0; i < g_n_css_url; i++) {
      free(g_css_url[i]);
      if (bodies)
        free(bodies[i]);
    }
    free(bodies);
    free(lens);

    /* @font-face: load declared faces and pick the body font (before walk so the renderer sees */
    load_faces(root, g_page_url, fetch);

    /* Lay out the parsed page now */
    layout_dom();

    /* url() backgrounds */
    load_bg_images(g_page_url, fetch);
    B->seq++;

    /* Defer the page's <script>s */
    g_page_fetch = fetch;
    B->scripts_pending = 1;
  } else {
    B->pg->text_len = 0;
  }
  g_csp_report = false; /* later relayouts re-decide silently (no log spam) */
  /* DOM + sheet + JS context are NOT freed here - they live until the next navigation */
}

/* Run the current page's <script>s (`fetch` pulls external <script src>) */
void html_run_page_scripts(void)
{
  if (!g_live_dom)
    return;
  js_run_document(g_live_dom, g_page_url, g_page_fetch);
}

/* Render a non-HTML text body verbatim into the styled-text buffer */
void plain_to_text(void)
{
  const char *p = B->pg->raw, *pend = B->pg->raw + B->pg->raw_len;
  int n = 0, col = 0;

  B->pg->doc_title[0] = '\0';
  B->pg->nlinks = 0;
  B->pg->nregs = 0;
  B->pg->n_imgs = 0;
  B->pg->page_bg = 0xffffff;
  B->pg->page_fg = 0x202830;
  page_scrollbar_default();
  sty_cur.fg = B->pg->page_fg;
  sty_cur.bold = sty_cur.under = 0;
  W_bg = BG_NONE;
  W_size = W_center = W_mono = W_indent = W_pre = W_fontpx = 0;

  while (p < pend) {
    unsigned char c = (unsigned char)*p;
    if (c == '\r') {
      p++;
      continue;
    }
    if (c == '\n') {
      n = text_put(n, '\n', 0);
      col = 0;
      p++;
      continue;
    }
    if (c == '\t') {
      int sp = 8 - (col & 7);
      while (sp-- > 0) {
        n = text_put(n, ' ', 0);
        col++;
      }
      p++;
      continue;
    }
    if (c < 0x80) { /* ASCII printable     */
      if (c < 0x20) {
        p++;
        continue;
      }
      n = text_put(n, (char)c, 0);
      col++;
      p++;
    } else { /* transcode to UTF-8  */
      unsigned cp = (unsigned)read_char(&p, pend);
      if (cp_drop(cp))
        continue;
      n = utf8_emit(n, cp, 0);
      col++;
    }
  }
  B->pg->text_len = n;
  B->layout_dirty = 1;
}

/* Fill the styled-text buffer with a literal one-off message (e.g */
void doc_message(const char *m)
{
  int n = 0;
  B->pg->page_bg = 0xffffff;
  B->pg->page_fg = 0x202830;
  page_scrollbar_default();
  while (m[n]) {
    if (!browser_ensure_text(n + 1))
      break;
    B->pg->text[n] = m[n];
    B->pg->attr[n] = 0;
    B->pg->bp[n] = 0;
    B->pg->align[n] = 0;
    B->pg->fsz[n] = 0;
    B->pg->lh[n] = 0;
    B->pg->ls[n] = B->pg->va[n] = 0;
    B->pg->fg[n] = B->pg->page_fg;
    B->pg->bg[n] = BG_NONE;
    n++;
  }
  B->pg->text_len = n;
  B->pg->doc_title[0] = '\0';
  B->pg->nlinks = 0;
  B->pg->nregs = 0;
  B->pg->n_imgs = 0;
  B->layout_dirty = 1;
}
