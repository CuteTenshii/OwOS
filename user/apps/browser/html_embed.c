#include "html_int.h"

/* Serialize a DOM subtree back into SVG/XML text */
struct sbuf {
  char *p;
  int n, cap;
  bool oom;
};
static void svg_put(struct sbuf *b, const char *s)
{
  int len = (int)strlen(s);
  if (b->oom)
    return;
  if (b->n + len + 1 > b->cap) {
    int nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->n + len + 1)
      nc *= 2;
    char *np = realloc(b->p, (unsigned)nc);
    if (!np) {
      b->oom = true;
      return;
    }
    b->p = np;
    b->cap = nc;
  }
  memcpy(b->p + b->n, s, (unsigned)len + 1);
  b->n += len;
}
/* Attribute text, with the quote that would end it escaped. */
static void svg_put_attr(struct sbuf *b, const char *v)
{
  for (; *v; v++) {
    if (*v == '"')
      svg_put(b, "&quot;");
    else {
      char c[2] = {*v, 0};
      svg_put(b, c);
    }
  }
}

/* Properties that do not inherit: an HTML ancestor's value must not reach the <svg>. */
static bool svg_noninherited(const char *d, int len)
{
  static const char *const names[] = {"clip-path", "mask",       "filter",       "opacity",     "display",
                                      "transform", "stop-color", "stop-opacity", "flood-color", "flood-opacity"};
  for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
    if ((int)strlen(names[i]) == len && eq_ci_n(d, names[i], len))
      return true;
  return false;
}

/* Append the declarations of `decl` ("a:b;c:d;") that inherit; `fonts` includes the font properties. */
static void svg_put_inherited(struct sbuf *b, const char *decl, bool fonts)
{
  while (*decl) {
    const char *colon = strchr(decl, ':'), *semi = strchr(decl, ';');
    if (!colon || !semi)
      return;
    int nl = (int)(colon - decl);
    bool font = (nl >= 4 && eq_ci_n(decl, "font", 4)) || (nl == 14 && eq_ci_n(decl, "letter-spacing", 14));
    if (!svg_noninherited(decl, nl) && (fonts || !font)) {
      char tmp[CSS_SVG_DECL];
      int n = (int)(semi - decl) + 1;
      memcpy(tmp, decl, (unsigned)n);
      tmp[n] = '\0';
      svg_put_attr(b, tmp);
    }
    decl = semi + 1;
  }
}

/* Bound the recursion like the DOM walk (WALK_DEPTH_MAX) */
#define SVG_MAX_DEPTH 256

/* Serialize a DOM subtree back into SVG/XML text. The page CSS that reaches each element goes out as a style
 * attribute, so the markup renders on its own exactly as it would loaded as an image. `top` is the <svg> being
 * embedded: it also receives the properties inherited from its HTML ancestors, and loses the box properties
 * (opacity, transform, ...) the page applies to it as a whole. */
static void svg_serialize(struct dom_node *el, struct sbuf *b, int depth, bool top)
{
  if (depth >= SVG_MAX_DEPTH)
    return;
  if (el->type == DOM_TEXT) {
    if (el->text)
      svg_put(b, el->text);
    return;
  }
  svg_put(b, "<");
  svg_put(b, el->tag);
  svg_put(b, " style=\"");
  if (top) {
    struct dom_node *anc[32];
    int na = 0;
    for (struct dom_node *a = el->parent; a && a->type == DOM_ELEMENT && na < 32; a = a->parent)
      anc[na++] = a;
    for (int i = na - 1; i >= 0; i--) {
      struct css_style as;
      compute_style(anc[i], &as);
      svg_put_inherited(b, as.svg_decl, false);
    }
  }
  {
    struct css_style ecs;
    compute_style(el, &ecs);
    if (top)
      svg_put_inherited(b, ecs.svg_decl, true);
    else
      svg_put_attr(b, ecs.svg_decl);
  }
  const char *own = dom_get_attr(el, "style");
  if (own)
    svg_put_attr(b, own);
  svg_put(b, "\"");
  for (struct dom_attr *a = el->attrs; a; a = a->next) {
    if (streq(a->name, "style"))
      continue;
    svg_put(b, " ");
    svg_put(b, a->name);
    svg_put(b, "=\"");
    svg_put_attr(b, a->value ? a->value : "");
    svg_put(b, "\"");
  }
  if (!el->first_child) {
    svg_put(b, "/>");
    return;
  }
  svg_put(b, ">");
  for (struct dom_node *c = el->first_child; c; c = c->next)
    svg_serialize(c, b, depth + 1, false);
  svg_put(b, "</");
  svg_put(b, el->tag);
  svg_put(b, ">");
}

static struct dom_node *svg_find_id(struct dom_node *n, const char *id, int len, int depth)
{
  if (depth > SVG_MAX_DEPTH)
    return 0;
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    const char *v = dom_get_attr(c, "id");
    if (v && (int)strlen(v) == len && !memcmp(v, id, (unsigned)len))
      return c;
    struct dom_node *r = svg_find_id(c, id, len, depth + 1);
    if (r)
      return r;
  }
  return 0;
}

static bool svg_defines(const struct sbuf *b, const char *id, int len)
{
  for (int i = 0; i + len + 5 <= b->n; i++)
    if (!memcmp(b->p + i, " id=\"", 5) && !memcmp(b->p + i + 5, id, (unsigned)len) && b->p[i + 5 + len] == '"')
      return true;
  return false;
}

/* Pull in the elements the markup points at (use, gradients, clip paths, markers, ...) that live elsewhere on the
 * page, as a trailing <defs>, so the serialized document is self-contained. */
static void svg_add_references(struct sbuf *b)
{
  struct dom_node *doc = html_dom_root();
  static const char tail[] = "</svg>";
  if (!doc || b->n < (int)sizeof tail - 1 || memcmp(b->p + b->n - (sizeof tail - 1), tail, sizeof tail - 1))
    return;
  b->n -= (int)sizeof tail - 1;
  b->p[b->n] = '\0';
  svg_put(b, "<defs>");
  int scan = 0;
  for (int round = 0; round < 64 && scan < b->n && !b->oom && b->n < (1 << 20); round++) {
    int end = b->n;
    for (int i = scan; i + 2 < end; i++) {
      int s;
      if (!memcmp(b->p + i, "url(#", 5))
        s = i + 5;
      else if (!memcmp(b->p + i, "href=\"#", 7))
        s = i + 7;
      else
        continue;
      int e = s;
      while (e < end && b->p[e] != ')' && b->p[e] != '"' && b->p[e] != '\'' && b->p[e] != ' ' && b->p[e] != ';')
        e++;
      if (e == s || svg_defines(b, b->p + s, e - s))
        continue;
      struct dom_node *t = svg_find_id(doc, b->p + s, e - s, 0);
      if (t)
        svg_serialize(t, b, 0, false);
    }
    scan = end;
  }
  svg_put(b, "</defs></svg>");
}

struct img_carry g_carry;

/* Size, object-fit and visibility of a replaced element from its computed style. */
static void img_box_style(struct inl_img *im, const struct css_style *cs)
{
  im->cw = (cs->width >= 0) ? cs->width : 0;
  im->ch = (cs->height >= 0) ? cs->height : 0;
  if (cs->aspect > 0 && im->cw && !im->ch)
    im->ch = (int)((long)im->cw * 1000 / cs->aspect);
  im->fit = cs->obj_fit > 0 ? cs->obj_fit : 0;
  im->bw = (short)(cs->border_w[0] > 0 ? (cs->border_w[0] > 64 ? 64 : cs->border_w[0]) : 0);
  im->bcol = cs->border_color >= 0 ? (unsigned)cs->border_color : sty_cur.fg;
  im->radius = (short)(cs->radius > 0 ? (cs->radius > 9999 ? 9999 : cs->radius) : 0);
  if (!im->radius && cs->pct_w == 100 && (cs->pct_h == 100 || cs->height < 0))
    im->radius = (short)layout_clip_radius(); /* filling a rounded box that clips its content */
  if (cs->box_sizing == 1) {                  /* the CSS size includes the border; the picture gets what is inside */
    if (im->cw)
      im->cw = im->cw > 2 * im->bw ? im->cw - 2 * im->bw : 1;
    if (im->ch)
      im->ch = im->ch > 2 * im->bw ? im->ch - 2 * im->bw : 1;
  }
  im->hidden = sty_cur.hidden;
  im->xf.on = cs->has_tr == 1;
  if (im->xf.on) {
    memcpy(im->xf.m, cs->tr_m, sizeof im->xf.m);
    im->xf.tx = cs->tr_x;
    im->xf.ty = cs->tr_y;
    im->xf.txp = cs->tr_xp;
    im->xf.typ = cs->tr_yp;
    im->xf.ox = cs->tr_ox;
    im->xf.oy = cs->tr_oy;
    im->xf.oxp = cs->tr_oxp;
    im->xf.oyp = cs->tr_oyp;
  }
  im->fbright = cs->flt_bright >= 0 ? cs->flt_bright : 1000;
  im->fgray = cs->flt_gray >= 0 ? cs->flt_gray : 0;
  im->finvert = cs->flt_invert >= 0 ? cs->flt_invert : 0;
  im->inl = 0; /* the slot is reused across walks: only handle_img makes an image inline */
  im->halign = 0;
  im->va = 0;
  im->ml = im->mr = 0;
  im->fitw = 0;
}

/* Inline <svg>…</svg> */
void handle_svg(struct dom_node *el, const struct css_style *cs)
{
  if (!browser_ensure_imgs(B->pg->n_imgs + 1))
    return;
  struct sbuf sb = {0};
  svg_serialize(el, &sb, 0, true);
  svg_add_references(&sb);
  if (sb.oom || sb.n < 5) {
    free(sb.p);
    return;
  }
  unsigned color = cs->color >= 0 ? (unsigned)cs->color : sty_cur.fg;
  unsigned h1 = 2166136261u, h2 = 5381u;
  for (int i = 0; i < sb.n; i++) {
    h1 = (h1 ^ (unsigned char)sb.p[i]) * 16777619u;
    h2 = h2 * 33u + (unsigned char)sb.p[i];
  }
  char key[64];
  {
    int k = 0;
    sapp(key, &k, sizeof key, "svg:");
    uapp(key, &k, sizeof key, h1);
    sapp(key, &k, sizeof key, ":");
    uapp(key, &k, sizeof key, h2);
    sapp(key, &k, sizeof key, ":");
    uapp(key, &k, sizeof key, (unsigned)sb.n);
    sapp(key, &k, sizeof key, ":");
    uapp(key, &k, sizeof key, color);
  }

  int idx = B->pg->n_imgs;
  struct inl_img *im = &B->pg->imgs[idx];
  bool carried = img_carry_take(&g_carry, im, key, idx, true, B->pg->imgs);
  if (!carried && !svg_probe(sb.p, sb.n)) {
    free(sb.p);
    return;
  }

  if (W_run_nl < 1 && W_n > 0)
    W_n = text_put(W_n, '\n', 0);

  strlcpy(im->href, key, (int)sizeof im->href);
  im->alt[0] = '\0';
  im->lazy = 0;
  img_box_style(im, cs);
  im->text_pos = W_n;
  im->rows = 0;
  im->start_row = -1;
  im->lx = -1;
  if (!carried) {
    img_clear(im);
    /* currentColor resolves to the SVG's own CSS color if it sets one, else the inherited text */
    resource_commit_inline(idx, (const unsigned char *)sb.p, sb.n, color);
  }
  im->want = 0;
  im->fetched = 1; /* nothing to fetch */
  B->pg->n_imgs++; /* expose once committed */
  W_n = text_put(W_n, '\n', 0);
  W_run_nl = 1;
  free(sb.p);
}

/* Append an inline <img> to B->pg->imgs on its own line (box from the computed CSS width/height) */
void handle_img(struct dom_node *el, const struct css_style *cs, int place)
{
  if (!browser_ensure_imgs(B->pg->n_imgs + 1))
    return;
  const char *src = dom_get_attr(el, "src");
  if (!src || !src[0])
    return;
  char srcbuf[URL_MAX];
  int k = 0;
  for (; src[k] && k < (int)sizeof srcbuf - 1; k++)
    srcbuf[k] = src[k];
  srcbuf[k] = '\0';
  decode_entities(srcbuf);
  if (!srcbuf[0] || starts_ci(srcbuf, "data:"))
    return;

  /* Resolve to an absolute URL (a frame's relative src needs the frame's base), then apply CSP */
  char imgabs[URL_MAX], imgurl[URL_MAX];
  if (url_absolute(srcbuf)) {
    int a = 0;
    imgabs[0] = '\0';
    sapp(imgabs, &a, sizeof imgabs, srcbuf);
  } else
    resolve_url(g_base_url, srcbuf, imgabs, sizeof imgabs);
  if (!csp_url_ok(CSP_IMG, imgabs, "image", imgurl, sizeof imgurl))
    return;

  /* An absolutely positioned image filling its container is that container's backdrop: paint it as one. */
  bool fills =
      (cs->pct_w == 100 && cs->pct_h == 100) || (cs->top == 0 && cs->right == 0 && cs->bottom == 0 && cs->left == 0);
  if (cs->pos == CSS_POS_ABS && fills && g_frame_depth == 0 &&
      layout_cover_image(srcbuf, bg_image_index(srcbuf, g_base_url), cs->obj_pos_x, cs->obj_pos_y)) {
    bg_note_url(srcbuf);
    return;
  }

  int idx = B->pg->n_imgs;
  bool inl = place < 0 && idx < (int)INLINE_IMG_MAX;
  if (!inl && W_run_nl < 1 && W_n > 0)
    W_n = text_put(W_n, '\n', 0);

  struct inl_img *im = &B->pg->imgs[idx];
  strlcpy(im->href, imgurl, (int)sizeof im->href);
  if (!img_carry_take(&g_carry, im, im->href, idx, false, B->pg->imgs))
    img_clear(im);

  im->alt[0] = '\0';
  const char *alt = dom_get_attr(el, "alt");
  if (alt) {
    int a = 0;
    for (; alt[a] && a < (int)sizeof im->alt - 1; a++)
      im->alt[a] = alt[a];
    im->alt[a] = '\0';
  }

  const char *ld = dom_get_attr(el, "loading"); /* lazy -> fetch on scroll-in */
  im->lazy = (ld && starts_ci(ld, "lazy")) ? 1 : 0;

  img_box_style(im, cs);
  im->inl = inl;
  im->halign = (signed char)(place > 0 ? place : 0);
  if (inl) {
    im->ml = (short)(cs->margin[3] > 0 ? (cs->margin[3] > 999 ? 999 : cs->margin[3]) : 0);
    im->mr = (short)(cs->margin[1] > 0 ? (cs->margin[1] > 999 ? 999 : cs->margin[1]) : 0);
  }
  int fp = W_fontpx ? W_fontpx : HTML_BASE_FONT;
  im->va = (short)(cs->va_kind == CSS_VA_MIDDLE  ? INLINE_VA_MIDDLE
                   : cs->va_kind == CSS_VA_SUB   ? fp / 5
                   : cs->va_kind == CSS_VA_SUPER ? -fp / 3
                   : cs->va_kind == CSS_VA_LEN   ? -cs->va_len
                                                 : 0);

  if (inl && W_pend_sp) { /* the collapsed space before it, as emit_text would write before a glyph */
    if (W_n > 0 && W_run_nl == 0)
      W_n = text_put(W_n, ' ', 0);
    W_pend_sp = false;
  }
  im->text_pos = W_n;
  im->rows = 0;
  im->start_row = -1;
  im->lx = -1;
  B->pg->n_imgs++;
  if (inl) {
    W_n = utf8_emit(W_n, INLINE_IMG_CP0 + (unsigned)idx, sty_byte());
    W_run_nl = 0;
  } else {
    W_n = text_put(W_n, '\n', 0);
    W_run_nl = 1;
  }
}

/* <canvas>: its bitmap is committed into the image pool like an inline <img>; canvas_tick keeps it current. */
void handle_canvas(struct dom_node *el, const struct css_style *cs)
{
  if (!browser_ensure_imgs(B->pg->n_imgs + 1))
    return;
  struct canvas_rec *r = canvas_open(el, g_live_dom);
  if (!r)
    return;
  int aw, ah;
  canvas_attr_size(el, &aw, &ah);
  if ((aw != r->c->w || ah != r->c->h) && canvas_resize(r, aw, ah))
    r->uid = 0;
  char key[48];
  int k = 0;
  sapp(key, &k, sizeof key, "canvas:");
  uapp(key, &k, sizeof key, (unsigned)r->id);
  sapp(key, &k, sizeof key, ":");
  uapp(key, &k, sizeof key, (unsigned)r->c->w);
  sapp(key, &k, sizeof key, "x");
  uapp(key, &k, sizeof key, (unsigned)r->c->h);

  if (W_run_nl < 1 && W_n > 0)
    W_n = text_put(W_n, '\n', 0);
  int idx = B->pg->n_imgs;
  struct inl_img *im = &B->pg->imgs[idx];
  if (!img_carry_take(&g_carry, im, key, idx, true, B->pg->imgs)) {
    img_clear(im);
    mutex_lock(&r->lock);
    r->uid = resource_commit_rgba(idx, r->c->px, (unsigned)r->c->w, (unsigned)r->c->h);
    mutex_unlock(&r->lock);
  } else {
    r->uid = im->uid;
  }
  strlcpy(im->href, key, (int)sizeof im->href);
  strlcpy(im->alt, "[canvas]", (int)sizeof im->alt);
  im->lazy = 0;
  img_box_style(im, cs);
  im->text_pos = W_n;
  im->rows = 0;
  im->start_row = -1;
  im->lx = -1;
  im->want = 0;
  im->fetched = 1; /* nothing to fetch */
  B->pg->n_imgs++;
  W_n = text_put(W_n, '\n', 0);
  W_run_nl = 1;
}

/* <audio>/<video>: register the media element and, when it has a visible widget (any <video> */
void handle_media(struct dom_node *el)
{
  bool is_video = streq(el->tag, "video");
  bool controls = dom_get_attr(el, "controls") != 0;
  bool widget = is_video || controls;
  int text_pos = -1;
  if (widget) {
    if (W_run_nl < 1 && W_n > 0)
      W_n = text_put(W_n, '\n', 0);
    text_pos = W_n;
  }
  media_register_element(el, text_pos, g_base_url);
  if (widget) {
    W_n = text_put(W_n, '\n', 0);
    W_run_nl = 1;
  }
}

/* <link rel="icon"> */
void handle_favicon(struct dom_node *el)
{
  const char *rel = dom_get_attr(el, "rel");
  if (!rel || !contains_ci(rel, "icon") || contains_ci(rel, "mask-icon"))
    return;
  int pri = contains_ci(rel, "apple-touch") ? 1 : 2;
  if (pri < B->pg->favicon_pri)
    return;
  const char *href = dom_get_attr(el, "href");
  if (!href || !href[0] || starts_ci(href, "data:"))
    return;
  char buf[URL_MAX];
  int k = 0;
  for (; href[k] && k < (int)sizeof buf - 1; k++)
    buf[k] = href[k];
  buf[k] = '\0';
  decode_entities(buf);
  int j = 0;
  for (; buf[j] && j < (int)sizeof B->pg->favicon_href - 1; j++)
    B->pg->favicon_href[j] = buf[j];
  B->pg->favicon_href[j] = '\0';
  B->pg->favicon_pri = pri;
}

/* Emit a literal ASCII/UTF-8 run with attr `a`, no whitespace collapsing - used for the chrome */
void emit_lit(const char *s, unsigned char a)
{
  if (W_pend_sp) {
    if (W_n > 0 && W_run_nl == 0)
      W_n = text_put(W_n, ' ', 0);
    W_pend_sp = false;
  }
  for (const char *p = s; *p; p++) {
    W_n = text_put(W_n, *p, a);
    W_run_nl = 0;
  }
}

/* --- iframes --- */
#define FRAME_MAX_DEPTH 4

/* sandbox flags we act on (a present `sandbox` attribute always sets SBX_ON) */
enum { SBX_ON = 1, SBX_SCRIPTS = 2, SBX_SAME_ORIGIN = 4, SBX_FORMS = 8, SBX_TOP_NAV = 16, SBX_POPUPS = 32 };

struct frame_doc *g_frames;

struct frame_doc *frame_find(const char *url, const char *parent)
{
  for (struct frame_doc *f = g_frames; f; f = f->next)
    if (streq(f->url, url) && streq(f->parent, parent))
      return f;
  return 0;
}

static struct frame_doc *frame_get(const char *url, const char *parent)
{
  struct frame_doc *f = frame_find(url, parent);
  if (f)
    return f;
  f = malloc(sizeof *f);
  if (!f)
    return 0;
  int k = 0;
  f->url[0] = '\0';
  sapp(f->url, &k, sizeof f->url, url);
  k = 0;
  f->parent[0] = '\0';
  sapp(f->parent, &k, sizeof f->parent, parent);
  f->verdict = 0;
  f->reason[0] = '\0';
  f->doc = 0;
  f->sheet = 0;
  f->next = g_frames;
  g_frames = f;
  return f;
}

void frames_free(void)
{
  struct frame_doc *f = g_frames;
  while (f) {
    struct frame_doc *n = f->next;
    if (f->doc)
      dom_free(f->doc);
    if (f->sheet)
      css_free(f->sheet);
    free(f);
    f = n;
  }
  g_frames = 0;
}

/* Collect a sub-document's inline <style> blocks into `sheet` (frames don't pull external */
void collect_inline_styles(struct dom_node *node, css_sheet *sheet, const char *base)
{
  for (struct dom_node *c = node->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    if (streq(c->tag, "style")) {
      for (struct dom_node *t = c->first_child; t; t = t->next)
        if (t->type == DOM_TEXT && t->text)
          css_add(sheet, t->text, strlen(t->text), base);
    } else {
      collect_inline_styles(c, sheet, base);
    }
  }
}

static int parse_sandbox(struct dom_node *el)
{
  const char *s = dom_get_attr(el, "sandbox");
  if (!s)
    return 0; /* no attribute = no sandbox */
  int f = SBX_ON;
  if (contains_ci(s, "allow-scripts"))
    f |= SBX_SCRIPTS;
  if (contains_ci(s, "allow-same-origin"))
    f |= SBX_SAME_ORIGIN;
  if (contains_ci(s, "allow-forms"))
    f |= SBX_FORMS;
  if (contains_ci(s, "allow-top-navigation"))
    f |= SBX_TOP_NAV;
  if (contains_ci(s, "allow-popups"))
    f |= SBX_POPUPS;
  return f;
}

/* Emit the frame's chrome line */
static void frame_chrome(const char *host, const char *note, int sbx, const char *allow)
{
  block_break();
  char line[256];
  int n = 0;
  sapp(line, &n, sizeof line, "\xe2\x96\xa3 frame"); /* ▣ */
  if (host && host[0]) {
    sapp(line, &n, sizeof line, " ");
    sapp(line, &n, sizeof line, host);
  }
  if (sbx & SBX_ON)
    sapp(line, &n, sizeof line, " \xc2\xb7 sandbox"); /* · */
  if (allow && allow[0]) {
    sapp(line, &n, sizeof line, " \xc2\xb7 allow=");
    sapp(line, &n, sizeof line, allow);
  }
  if (note && note[0]) {
    sapp(line, &n, sizeof line, " \xe2\x80\x94 ");
    sapp(line, &n, sizeof line, note);
  } /*  -  */
  emit_lit(line, ST_BOLD | 6);
  block_break();
}

/* Flatten an allowed frame's sub-document into the parent flow */
static void frame_render(struct frame_doc *f, int sbx)
{
  if (!f->doc) {
    emit_lit("(empty frame)", 6);
    return;
  }

  struct sty sv_sty = sty_cur;
  unsigned char sv_indent = W_indent;
  css_sheet *sv_sheet = g_sheet;
  bool sv_nav = g_link_nav;
  char sv_base[URL_MAX];
  {
    int k = 0;
    sv_base[0] = '\0';
    sapp(sv_base, &k, sizeof sv_base, g_base_url);
  }

  /* A sandboxed frame may navigate only with allow-top-navigation/-popups */
  bool nav = !(sbx & SBX_ON) || (sbx & (SBX_TOP_NAV | SBX_POPUPS));
  g_link_nav = sv_nav && nav;
  g_sheet = f->sheet ? f->sheet : sv_sheet;
  W_indent = (W_indent < 7) ? (unsigned char)(W_indent + 1) : 7;
  {
    int k = 0;
    g_base_url[0] = '\0';
    sapp(g_base_url, &k, sizeof g_base_url, f->url);
  }

  g_frame_depth++;
  walk(f->doc);
  g_frame_depth--;

  g_link_nav = sv_nav;
  g_sheet = sv_sheet;
  W_indent = sv_indent;
  sty_cur = sv_sty;
  {
    int k = 0;
    g_base_url[0] = '\0';
    sapp(g_base_url, &k, sizeof g_base_url, sv_base);
  }
}

void handle_iframe(struct dom_node *el)
{
  const char *src = dom_get_attr(el, "src");
  char srcbuf[URL_MAX];
  int k = 0;
  if (src)
    for (; src[k] && k < (int)sizeof srcbuf - 1; k++)
      srcbuf[k] = src[k];
  srcbuf[k] = '\0';
  decode_entities(srcbuf);

  int sbx = parse_sandbox(el);
  const char *allow = dom_get_attr(el, "allow");

  if (!srcbuf[0]) {
    frame_chrome("", "no src", sbx, allow);
    return;
  }
  if (starts_ci(srcbuf, "javascript:") || starts_ci(srcbuf, "about:"))
    return;
  if (g_frame_depth >= FRAME_MAX_DEPTH) {
    frame_chrome("", "nested too deep", sbx, allow);
    return;
  }

  char abs[URL_MAX];
  if (url_absolute(srcbuf)) {
    int k2 = 0;
    abs[0] = '\0';
    sapp(abs, &k2, sizeof abs, srcbuf);
  } else
    resolve_url(g_base_url, srcbuf, abs, sizeof abs);

  char host[URL_HOST_MAX];
  struct purl u;
  if (parse_url(abs, &u)) {
    int j = 0;
    host[0] = '\0';
    sapp(host, &j, sizeof host, u.host);
  } else {
    host[0] = '\0';
  }

  /* CSP frame-src (top document only */
  if (g_frame_depth == 0 && B->pg->csp) {
    char fu[URL_MAX], reason[64];
    if (!csp_check_url(B->pg->csp, CSP_FRAME, abs, g_cur_page_url, fu, sizeof fu, reason, sizeof reason)) {
      csp_report("frame", abs, reason);
      frame_chrome(host, "blocked by CSP frame-src", sbx, allow);
      return;
    }
    {
      int j = 0;
      abs[0] = '\0';
      sapp(abs, &j, sizeof abs, fu);
    } /* use upgraded URL */
  }

  struct frame_doc *f = frame_get(abs, g_base_url);
  if (!f) {
    frame_chrome(host, "out of memory", sbx, allow);
    return;
  }

  switch (f->verdict) {
  case FRAME_OK:
    frame_chrome(host, 0, sbx, allow);
    frame_render(f, sbx);
    block_break();
    break;
  case FRAME_BLOCKED:
    frame_chrome(host, f->reason[0] ? f->reason : "blocked from embedding", sbx, allow);
    break;
  case FRAME_ERROR:
    frame_chrome(host, f->reason[0] ? f->reason : "could not load", sbx, allow);
    break;
  default:                                                 /* still pending a fetch */
    frame_chrome(host, "loading\xe2\x80\xa6", sbx, allow); /* … */
    break;
  }
}
