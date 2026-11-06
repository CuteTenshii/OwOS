/* html.c — the renderer. Walks the DOM (parsed by dom.c) into styled text in
 * B->pg->text/B->pg->attr, collecting links, inline images, the favicon and
 * title. Element styling comes from the css.c engine (sheet + inline + attrs);
 * this file maps the computed style onto the text renderer and does the
 * HTML-entity work. */
#include "html_int.h"

css_sheet *g_sheet; /* the page's collected <style> rules, per render */

/* url() background loader/lookup — defined lower, used during the layout walk.
 */

/* The DOM + CSS sheet now persist for the whole page lifetime (the JS context
 * in js.c does too), so event listeners can fire and mutate the tree after
 * load. html_page_teardown() frees all three on the next navigation;
 * html_relayout() re-flattens the live tree into B's buffers after a handler
 * changes it. */
struct dom_node *g_live_dom;
char g_cur_page_url[URL_MAX];

/* Base URL the walk resolves relative links/sources against. Normally the page
 * URL; swapped to a frame's own src while its sub-document is being flattened,
 * so a relative <a>/<img> inside an iframe resolves against the frame, not the
 * top page. g_frame_depth is the iframe nesting depth (0 = top document);
 * g_link_nav is 0 while inside a sandboxed frame that may not navigate, so its
 * links render as plain (non-clickable) text. */
char g_base_url[URL_MAX];
int g_frame_depth;
bool g_link_nav = true;

/* CSP enforcement runs against the top document (B->pg->csp); frame
 * sub-documents have their own policy we don't track, so checks are skipped
 * inside frames (g_frame_depth > 0). g_csp_report gates console logging to the
 * initial load so the per-relayout walk doesn't spam duplicate violations. */
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

/* CSP gate for a sub-resource URL load (img/style/font/frame). `abs` is already
 * absolute; on success `out` holds the URL to use (http->https upgraded when
 * the policy asks). Returns 1 = allowed. No-op (allow) inside frames or with no
 * CSP. */
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

/* Computed style for one element: presentational attrs (lowest), sheet rules,
 * then inline style="" (highest). */
void compute_style(struct dom_node *el, struct css_style *cs)
{
  unsigned seq = style_key();
  dom_on_free_style_cache = free_style_cache;
  if (el->computed_style && el->computed_style_sheet == g_sheet && el->computed_style_seq == seq) {
    *cs = *(struct css_style *)el->computed_style;
    return;
  }
  css_style_reset(cs);
  css_apply_attrs(el, cs);
  css_apply_sheet(g_sheet, el, cs);
  const char *inl = dom_get_attr(el, "style");
  if (inl && csp_style_attr_ok())
    css_apply_inline(inl, cs);
  if (!el->computed_style)
    el->computed_style = malloc(sizeof *cs);
  if (el->computed_style) {
    *(struct css_style *)el->computed_style = *cs;
    el->computed_style_sheet = g_sheet;
    el->computed_style_seq = seq;
  }
}

/* devtools hooks: the live DOM root and a public wrapper over compute_style, so
 * the developer-tools panel can walk the tree and read computed styles without
 * reaching into html.c's statics. Worker-thread callers only (the DOM's owner).
 */
struct dom_node *html_dom_root(void) { return g_live_dom; }
void html_compute_style(struct dom_node *el, struct css_style *out) { compute_style(el, out); }
int html_match_styles(struct dom_node *el, struct css_match *out, int cap)
{
  return css_matches(g_sheet, el, out, cap);
}
/* 1 if `el` has a ::before/::after (CSS_PSEUDO_*) with content — for devtools.
 */
int html_has_pseudo(struct dom_node *el, int which)
{
  struct css_style cs;
  return css_pseudo_content(g_sheet, el, which, &cs);
}

/* True when `s` already carries a URL scheme that resolve_url won't expand, so
 * it must be used verbatim rather than joined onto a base (resolve_url only
 * treats http(s):// as absolute, and would mangle e.g. a data:/file:
 * reference). */
bool url_absolute(const char *s)
{
  return starts_ci(s, "http://") || starts_ci(s, "https://") || starts_ci(s, "data:") || starts_ci(s, "file:") ||
         starts_ci(s, "blob:") || starts_ci(s, "mailto:");
}

/* Source charset for the document being parsed. We render Unicode, so non-UTF-8
 * input is transcoded as it is read. Default UTF-8; the HTTP Content-Type
 * charset (set by fetch.c) wins, else a <meta charset> in the document, else
 * the default. A fair number of pages (some Google locales among them) still
 * declare ISO-8859- 1, which in practice is Windows-1252 — so the Latin-1 path
 * uses the cp1252 map for 0x80..0x9F and is the identity for 0xA0..0xFF (==
 * Unicode U+00A0..U+00FF). */
enum { CS_UTF8, CS_LATIN1 };
static int g_charset;
static bool g_charset_from_header;

/* True if `cs` (a charset name, possibly inside a longer string) is a Latin-1 /
 * Windows-1252 label; returns 0 for utf-8 / unknown (treated as UTF-8). */
static bool charset_is_latin1(const char *cs)
{
  return contains_ci(cs, "8859-1") || contains_ci(cs, "latin-1") || contains_ci(cs, "latin1") ||
         contains_ci(cs, "1252") || contains_ci(cs, "8859_1");
}

/* fetch.c calls this with the raw Content-Type header value (may be NULL). A
 * charset there is authoritative (locks out the <meta> guess). */
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

/* Scan the document head for <meta charset=...> / <meta
 * http-equiv="Content-Type" content="...charset=...">. Only consulted when the
 * header did not specify one. */
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

/* Read one source character, transcoding per the active charset, advancing
 * `*pp`. Returns the Unicode codepoint. */
int cp1252_high(unsigned char b)
{
  if (b >= 0xA0)
    return b; /* Latin-1 == Unicode */
  static const unsigned short m[32] = {0x20AC, 0x81,   0x201A, 0x192,  0x201E, 0x2026, 0x2020, 0x2021,
                                       0x02C6, 0x2030, 0x160,  0x2039, 0x152,  0x8D,   0x17D,  0x8F,
                                       0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                       0x02DC, 0x2122, 0x161,  0x203A, 0x153,  0x9D,   0x17E,  0x178};
  return m[b - 0x80];
}

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
  return a;
}

/* User-agent default style for a tag (the lowest layer; author CSS overrides).
 * The colour substitutes (italic grey, link blue, code brown, mark red) stand
 * in for renderings the engine can't otherwise show; headings inherit their
 * colour (so they stay readable on a dark page) and carry weight via bold. */
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
}

/* The renderer's line-break set (distinct from dom.c's CSS-block set: this one
 * includes br/li/tr/dd/dt, which force a new line but aren't <p>-closers). */
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

/* Reset the render buffers and re-flatten the live DOM into them. Shared by the
 * initial html_to_text and by html_relayout (after an event mutates the tree).
 */
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

/* The canvas background and default text colour, from the <html>/<body>
 * cascade. The canvas starts white; each of <html> then <body>'s background
 * composites over it (so a translucent tint blends instead of overriding),
 * matching how a browser propagates the root/body background to the canvas.
 * Defaults: white page, near- black text. The main thread reads
 * B->pg->page_bg/fg. */
static void compute_page_colors(struct dom_node *root)
{
  unsigned canvas = 0xffffff;
  B->pg->page_fg = 0x202830;
  struct css_style cs;
  const char *who[] = {"html", "body"};
  for (int i = 0; i < 2; i++) {
    struct dom_node *el = find_tag(root, who[i]);
    if (!el)
      continue;
    compute_style(el, &cs);
    if (cs.color >= 0)
      B->pg->page_fg = (unsigned)cs.color;
    unsigned argb;
    if (css_bg_argb(g_sheet, el, &argb))
      canvas = composite_over(canvas, argb);
  }
  B->pg->page_bg = canvas;
}

static void layout_dom(void)
{
  css_set_viewport(BW, BH); /* so @media re-evaluates at the current size */
  g_walk_now = msec();
  anim_frame_begin(g_walk_now); /* advance the animation clock for this walk */
  B->pg->nlinks = 0;
  if (B->img_pool_len > IMG_POOL / 2 || !img_carry_begin(&g_carry, B->pg->imgs, B->pg->n_imgs))
    B->img_pool_len = 0; /* nothing carried: the pool restarts empty */
  B->pg->n_imgs = 0;
  compute_page_colors(g_live_dom);
  sty_cur.fg = B->pg->page_fg;
  sty_cur.bold = sty_cur.under = 0;
  sty_cur.italic = sty_cur.nowrap = sty_cur.ttx = sty_cur.smallcaps = 0;
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
  /* Reset the frame walk context to the top document (the cache itself persists
   * across relayouts; only the per-walk base/depth/nav reset here). */
  g_frame_depth = 0;
  g_link_nav = true;
  g_walk_depth = 0;
  {
    int k = 0;
    g_base_url[0] = '\0';
    sapp(g_base_url, &k, sizeof g_base_url, g_cur_page_url);
  }
  if (g_live_dom)
    walk(g_live_dom);
  img_carry_end(&g_carry);
  B->img_pool_len = img_carry_pool_len(B->pg->imgs, B->pg->n_imgs, B->img_pool_len);
  B->pg->text_len = W_n;
  B->layout_dirty = 1; /* box tree + text rebuilt → main thread must re-wrap */
}

/* Free the current page's live DOM, CSS sheet and JS context. Called on every
 * navigation/page replacement before a new page is built (and is a no-op when
 * no live page is held). The JS context goes FIRST: its node wrappers reference
 * the DOM nodes, so the DOM must outlive the wrapper teardown. */
void html_page_teardown(void)
{
  js_page_end();
  media_page_teardown(); /* stop playback + free this page's media */
  if (g_live_dom) {
    dom_free(g_live_dom);
    g_live_dom = 0;
  }
  if (g_sheet) {
    css_free(g_sheet);
    g_sheet = 0;
  }
  frames_free(); /* drop this page's iframe sub-documents */
  dom_on_free_style_cache = 0;
  layout_free(); /* drop the box tree */
  anim_reset();  /* drop per-element animation/transition state */
  g_nspans = 0;
  g_focus = 0;
  B->focus_kind = 0;
}

/* --- iframe fetch handshake (worker thread; resource.c does the network) ----
 */

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
    /* Render an HTML body (or an empty/sloppy Content-Type, the common case);
     * anything else is left as an empty frame with a note. */
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

/* Re-flatten the live DOM into B's buffers after a script mutated it (run by
 * the worker right after event dispatch), forcing a re-wrap + repaint. Landed
 * inline images keep their pixels; the caller fetches any new ones. */
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
  g_live_dom = root; /* the live tree — kept until the next teardown  */
  if (root) {
    scan_meta_csp(root); /* fold <meta http-equiv=CSP> into B->pg->csp before any load */
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

    /* @font-face: load declared faces and pick the body font (before walk so
     * the renderer sees B->pg->body_font for this page's text). */
    load_faces(root, g_page_url, fetch);

    /* Lay out the parsed page now; the worker publishes + paints it before any
     * script runs (see below), so a slow or long-running script never keeps the
     * page blank — the HTML shows as soon as it is parsed, as Chrome does. */
    layout_dom();

    /* url() backgrounds: fetch+decode the urls the walk just collected, then
     * point each box at its decoded entry (no second cascade over the DOM). */
    load_bg_images(g_page_url, fetch);
    B->seq++;

    /* Defer the page's <script>s. Running them here would keep `loading` set
     * and the line table unbuilt until every script finished, so the whole page
     * would sit on "Loading ..." for the duration of the JS. Instead the worker
     * paints this laid-out page first, then calls html_run_page_scripts() on
     * its next pass (see worker_fn). */
    g_page_fetch = fetch;
    B->scripts_pending = 1;
  } else {
    B->pg->text_len = 0;
  }
  g_csp_report = false; /* later relayouts re-decide silently (no log spam) */
  /* DOM + sheet + JS context are NOT freed here — they live until the next
   * navigation (html_page_teardown), so events keep working after load. */
}

/* Run the current page's <script>s (`fetch` pulls external <script src>). Split
 * out of html_to_text and invoked by the worker only AFTER the parsed page has
 * been laid out and painted, so JS never blocks the first paint. The JS context
 * stays alive past this call so listeners/timers fire later; a script that
 * mutates the DOM is re-laid-out by the caller (worker_fn). */
void html_run_page_scripts(void)
{
  if (!g_live_dom)
    return;
  js_run_document(g_live_dom, g_page_url, g_page_fetch);
}

/* Render a non-HTML text body verbatim into the styled-text buffer: newlines
 * are kept (the word-wrapper honours them), tabs expand to the next 8-column
 * stop, and other control bytes are dropped. This is what fixes plain `.txt`
 * pages, whose line breaks the HTML reducer would otherwise collapse. */
void plain_to_text(void)
{
  const char *p = B->pg->raw, *pend = B->pg->raw + B->pg->raw_len;
  int n = 0, col = 0;

  B->pg->doc_title[0] = '\0';
  B->pg->nlinks = 0;
  B->pg->n_imgs = 0;
  B->pg->page_bg = 0xffffff;
  B->pg->page_fg = 0x202830;
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

/* Fill the styled-text buffer with a literal one-off message (e.g. "unsupported
 * content type"), so render() shows it through the normal text path. */
void doc_message(const char *m)
{
  int n = 0;
  B->pg->page_bg = 0xffffff;
  B->pg->page_fg = 0x202830;
  while (m[n]) {
    if (!browser_ensure_text(n + 1))
      break;
    B->pg->text[n] = m[n];
    B->pg->attr[n] = 0;
    B->pg->bp[n] = 0;
    B->pg->align[n] = 0;
    B->pg->fsz[n] = 0;
    B->pg->lh[n] = 0;
    B->pg->fg[n] = B->pg->page_fg;
    B->pg->bg[n] = BG_NONE;
    n++;
  }
  B->pg->text_len = n;
  B->pg->doc_title[0] = '\0';
  B->pg->nlinks = 0;
  B->pg->n_imgs = 0;
  B->layout_dirty = 1;
}
