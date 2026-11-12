#include "features/clienthints/clienthints.h"
#include "fontstore.h"
#include "hints.h"
#include "html_int.h"

/* Stylesheets are collected as an ORDERED list of sources first */
struct css_src *g_src;
int g_nsrc;
static int g_src_cap;
char **g_css_url;
int g_n_css_url;
static int g_css_url_cap;
const char *g_page_url;
res_batch_fn g_page_fetch; /* batch fetcher stashed for the deferred script pass */

static bool css_src_room(void) /* ensure one free slot in g_src */
{
  if (g_nsrc < g_src_cap)
    return true;
  int nc = g_src_cap ? g_src_cap * 2 : 16;
  struct css_src *n = realloc(g_src, (unsigned)nc * sizeof *n);
  if (!n)
    return false;
  g_src = n;
  g_src_cap = nc;
  return true;
}
static bool css_url_room(void) /* ensure one free slot in g_css_url */
{
  if (g_n_css_url < g_css_url_cap)
    return true;
  int nc = g_css_url_cap ? g_css_url_cap * 2 : 8;
  char **n = realloc(g_css_url, (unsigned)nc * sizeof *n);
  if (!n)
    return false;
  g_css_url = n;
  g_css_url_cap = nc;
  return true;
}

static void src_inline(const char *text, int len)
{
  if (!css_src_room())
    return;
  g_src[g_nsrc].text = text;
  g_src[g_nsrc].len = len;
  g_src[g_nsrc].url_i = -1;
  g_nsrc++;
}

void src_link(const char *absurl)
{
  if (!css_src_room() || !css_url_room())
    return;
  int L = strlen(absurl);
  char *u = malloc((unsigned)L + 1);
  if (!u)
    return;
  for (int i = 0; i <= L; i++)
    u[i] = absurl[i];
  g_src[g_nsrc].text = 0;
  g_src[g_nsrc].len = 0;
  g_src[g_nsrc].url_i = g_n_css_url;
  g_nsrc++;
  g_css_url[g_n_css_url++] = u;
}

/* Record stylesheet sources in document order */
void collect_styles(struct dom_node *node)
{
  for (struct dom_node *c = node->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    if (streq(c->tag, "style")) {
      /* style-src for an inline <style> */
      int total = 0;
      for (struct dom_node *t = c->first_child; t; t = t->next)
        if (t->type == DOM_TEXT && t->text)
          total += (int)strlen(t->text);
      char *buf = total ? malloc((unsigned)total + 1) : 0;
      int bl = 0;
      if (buf) {
        for (struct dom_node *t = c->first_child; t; t = t->next)
          if (t->type == DOM_TEXT && t->text)
            for (const char *s = t->text; *s; s++)
              buf[bl++] = *s;
        buf[bl] = '\0';
      }
      char reason[64];
      int ok = (!B->pg->csp) ? 1
                             : csp_check_inline(B->pg->csp, CSP_STYLE, dom_get_attr(c, "nonce"),
                                                (const unsigned char *)buf, bl, reason, sizeof reason);
      if (ok) {
        for (struct dom_node *t = c->first_child; t; t = t->next)
          if (t->type == DOM_TEXT && t->text)
            src_inline(t->text, strlen(t->text));
      } else {
        csp_report("inline stylesheet", g_cur_page_url, reason);
      }
      free(buf);
    } else if (streq(c->tag, "link")) {
      const char *rel = dom_get_attr(c, "rel");
      const char *href = dom_get_attr(c, "href");
      if (rel && href && href[0] && contains_ci(rel, "stylesheet") && !contains_ci(rel, "alternate")) {
        char abs[URL_MAX], use[URL_MAX];
        resolve_url(g_page_url, href, abs, sizeof abs);
        if (csp_url_ok(CSP_STYLE, abs, "stylesheet", use, sizeof use))
          src_link(use);
      }
    } else {
      collect_styles(c);
    }
  }
}

/* Start the <link rel=preconnect|preload> hints in <head>; a preload with a media query is skipped. */
void scan_resource_hints(struct dom_node *root)
{
  struct dom_node *head = find_tag(root, "head");
  if (!head)
    return;
  for (struct dom_node *c = head->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT || !streq(c->tag, "link"))
      continue;
    const char *rel = dom_get_attr(c, "rel"), *href = dom_get_attr(c, "href");
    if (!rel || !href || !href[0])
      continue;
    char abs[URL_MAX];
    resolve_url(g_page_url, href, abs, sizeof abs);
    if (hint_rel_has(rel, rel + strlen(rel), "preconnect"))
      hint_preconnect(abs);
    const char *as = dom_get_attr(c, "as");
    if (as && hint_rel_has(rel, rel + strlen(rel), "preload") && !dom_get_attr(c, "media"))
      hint_preload(abs, as, g_page_url);
  }
}

/* First element with this tag name anywhere in the subtree (pre-order). */
struct dom_node *find_tag(struct dom_node *node, const char *tag)
{
  for (struct dom_node *c = node->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    if (streq(c->tag, tag))
      return c;
    struct dom_node *r = find_tag(c, tag);
    if (r)
      return r;
  }
  return 0;
}

/* Fold <meta http-equiv="Content-Security-Policy"> into the page's policy set; <meta http-equiv="Accept-CH"> opts the
 * origin in */
void scan_meta_csp(struct dom_node *node)
{
  for (struct dom_node *c = node->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    if (streq(c->tag, "meta")) {
      const char *he = dom_get_attr(c, "http-equiv");
      const char *ct = dom_get_attr(c, "content");
      if (he && ct && ct[0] && streq_ci(he, "content-security-policy")) {
        /* The DOM keeps attribute values raw, so a meta CSP's quote- keywords arrive entity-encoded */
        int cl = (int)strlen(ct);
        char *dec = malloc((unsigned)cl + 1);
        if (dec) {
          for (int i = 0; i <= cl; i++)
            dec[i] = ct[i];
          decode_entities(dec);
          if (!B->pg->csp)
            B->pg->csp = csp_new();
          csp_add(B->pg->csp, dec);
          free(dec);
        }
      } else if (he && ct && streq_ci(he, "accept-ch")) {
        struct purl pu;
        if (g_page_url && parse_url(g_page_url, &pu) && pu.tls)
          ch_accept(pu.host, ct);
      }
    } else {
      scan_meta_csp(c);
    }
  }
}

/* Codepoints the document's text uses: a BMP bitmap, plus whether any lie above it. */
struct cp_cover {
  unsigned bmp[0x10000 / 32];
  bool astral;
};

static void cover_text(const struct dom_node *root, struct cp_cover *c)
{
  const struct dom_node *n = root;
  while (n) {
    bool skip =
        n->type == DOM_ELEMENT && (streq(n->tag, "script") || streq(n->tag, "style") || streq(n->tag, "template"));
    if (n->type == DOM_TEXT && n->text) {
      const char *p = n->text, *end = p + strlen(p);
      unsigned cp;
      while (p < end) {
        p += utf8_decode(p, end, &cp);
        if (cp < 0x10000)
          c->bmp[cp >> 5] |= 1u << (cp & 31);
        else
          c->astral = true;
      }
    }
    if (n->first_child && !skip) {
      n = n->first_child;
      continue;
    }
    while (n && n != root && !n->next)
      n = n->parent;
    if (!n || n == root)
      break;
    n = n->next;
  }
}

/* True if the text uses any codepoint in [lo,hi]. */
static bool cover_any(const struct cp_cover *c, unsigned lo, unsigned hi)
{
  if (hi > 0xffff) {
    if (c->astral)
      return true;
    hi = 0xffff;
  }
  for (unsigned cp = lo; cp <= hi && cp <= 0xffff;) {
    unsigned w = c->bmp[cp >> 5];
    if (!(cp & 31) && cp + 31 <= hi) {
      if (w)
        return true;
      cp += 32;
      continue;
    }
    if (w & (1u << (cp & 31)))
      return true;
    cp++;
  }
  return false;
}

/* Text the UI font can't draw, which the emoji font then may. */
static bool cover_needs_fallback(const struct cp_cover *c)
{
  if (c->astral)
    return true;
  struct font *ui = gfx_ui_font();
  for (unsigned i = 0x2000 / 32; i < 0x10000 / 32; i++)
    for (unsigned w = c->bmp[i]; w; w &= w - 1) {
      unsigned cp = i * 32 + (unsigned)__builtin_ctz(w);
      if (!ui || font_glyph_index(ui, (int)cp) <= 0)
        return true;
    }
  return false;
}

static unsigned g_font_gen; /* source of page font generations; never reused */

/* Load the @font-face faces the page's text can use: those whose unicode-range covers some of it. Each
 * element then picks among them (and the installed fonts) by its own font-family, weight and style. */
void load_faces(struct dom_node *root, const char *page_url, res_batch_fn fetch)
{
  struct cp_cover *cov = calloc(1, sizeof *cov);
  if (cov && root)
    cover_text(root, cov);
  if (cov && cover_needs_fallback(cov))
    fonts_preload_emoji();

  int nfaces = css_fontface_count(g_sheet);
  char **urls = nfaces ? calloc((unsigned)nfaces, sizeof *urls) : 0;
  struct loaded_face *nf = nfaces ? calloc((unsigned)nfaces, sizeof *nf) : 0;
  int nu = 0;
  if (nfaces && (!urls || !nf))
    nfaces = 0;

  for (int i = 0; i < nfaces; i++) {
    struct css_fontface ff;
    if (!css_fontface_get(g_sheet, i, &ff) || !ff.family[0] || !ff.url[0])
      continue;
    bool used = !ff.nranges || !cov;
    for (int r = 0; r < ff.nranges && !used; r++)
      used = cover_any(cov, ff.ranges[2 * r], ff.ranges[2 * r + 1]);
    if (!used)
      continue; /* a subset for script the page doesn't contain */

    /* A relative src resolves against the sheet that declared it */
    char abs[URL_MAX], use[URL_MAX];
    resolve_url(ff.base && ff.base[0] ? ff.base : page_url, ff.url, abs, sizeof abs);
    if (!csp_url_ok(CSP_FONT, abs, "font", use, sizeof use))
      continue; /* font-src */
    int L = strlen(use);
    char *u = malloc((unsigned)L + 1);
    unsigned *rg = ff.nranges ? malloc((unsigned)ff.nranges * 2 * sizeof *rg) : 0;
    if (!u || (ff.nranges && !rg)) {
      free(u);
      free(rg);
      continue;
    }
    memcpy(u, use, (unsigned)L + 1);
    if (rg)
      memcpy(rg, ff.ranges, (unsigned)ff.nranges * 2 * sizeof *rg);
    struct loaded_face *lf = &nf[nu];
    int j = 0;
    for (; ff.family[j] && j < (int)sizeof lf->family - 1; j++)
      lf->family[j] = ff.family[j];
    lf->family[j] = '\0';
    lf->wmin = ff.wmin;
    lf->wmax = ff.wmax;
    lf->italic = ff.italic;
    lf->ranges = rg;
    lf->nranges = ff.nranges;
    urls[nu++] = u;
  }
  free(cov);

  /* Batch-fetch the font files (multiplexed over h2 same-origin), then parse */
  int nn = 0;
  if (nu > 0) {
    unsigned char **bodies = calloc((unsigned)nu, sizeof *bodies);
    int *lens = calloc((unsigned)nu, sizeof *lens);
    if (bodies && lens && fetch)
      fetch(urls, nu, bodies, lens);
    for (int i = 0; i < nu; i++) {
      unsigned char *body = bodies ? bodies[i] : 0;
      /* font_load transparently decodes WOFF/WOFF2 containers to SFNT. */
      struct font *f = body && lens[i] > 0 ? font_load(body, lens[i]) : 0;
      if (!f) {
        free(body);
        free(nf[i].ranges);
        continue;
      }
      nf[nn] = nf[i];
      nf[nn].font = f;
      nf[nn].data = body;
      nn++;
    }
    free(bodies);
    free(lens);
  }
  for (int i = 0; i < nu; i++)
    free(urls[i]);
  free(urls);

  /* Publish: drop the previous page's faces, install the new set. */
  browser_buf_lock();
  for (int i = 0; i < B->pg->n_faces; i++) {
    if (B->pg->faces[i].font)
      font_free(B->pg->faces[i].font);
    free(B->pg->faces[i].data);
    free(B->pg->faces[i].ranges);
  }
  free(B->pg->faces);
  B->pg->faces = nn ? nf : 0;
  B->pg->n_faces = nn;
  B->pg->faces_cap = nn;
  B->pg->font_gen = ++g_font_gen;
  browser_buf_unlock();
  if (!nn)
    free(nf);
  fonts_page_reset();
}

/* ---------------------------------------------------- url() backgrounds ---- * Each url() */
struct bg_note {
  char *abs; /* table key: resolved abs URL */
  char *use; /* fetch URL (post-CSP, maybe upgraded) */
};
static struct bg_note *g_bgn;
int g_nbg;
static int g_bgn_cap; /* urls collected by the walk this pass */

/* Note one block box's url() background for the batched fetch */
void bg_note_url(const char *raw_url)
{
  if (!raw_url || !raw_url[0])
    return;
  char abs[URL_MAX], use[URL_MAX];
  resolve_url(g_base_url, raw_url, abs, sizeof abs);
  if (!csp_url_ok(CSP_IMG, abs, "image", use, sizeof use))
    return;
  for (int i = 0; i < g_nbg; i++)
    if (streq(g_bgn[i].abs, abs))
      return;
  if (g_nbg == g_bgn_cap) {
    int nc = g_bgn_cap ? g_bgn_cap * 2 : 16;
    struct bg_note *nn = realloc(g_bgn, (unsigned)nc * sizeof *nn);
    if (!nn)
      return;
    memset(nn + g_bgn_cap, 0, (unsigned)(nc - g_bgn_cap) * sizeof *nn);
    g_bgn = nn;
    g_bgn_cap = nc;
  }
  char *na = strdup(abs), *nu = strdup(use);
  if (!na || !nu) {
    free(na);
    free(nu);
    return;
  }
  free(g_bgn[g_nbg].abs); /* left over from an earlier walk */
  free(g_bgn[g_nbg].use);
  g_bgn[g_nbg].abs = na;
  g_bgn[g_nbg].use = nu;
  g_nbg++;
}

/* Fetch + decode the url() backgrounds collected during the layout walk (g_bgabs/ g_bguse) */
void load_bg_images(const char *page_url, res_batch_fn fetch)
{
  for (int i = 0; i < B->pg->n_bgimgs; i++)
    free(B->pg->bgimgs[i].rgba); /* drop the old page's */
  free(B->pg->bgimgs);
  B->pg->bgimgs = 0;
  B->pg->n_bgimgs = B->pg->bgimgs_cap = 0;

  int nu = g_nbg;
  if (nu == 0)
    return;

  struct bg_img *tab = malloc((unsigned)nu * sizeof *tab);
  char **furls = malloc((unsigned)nu * sizeof *furls);
  if (!tab || !furls) {
    free(tab);
    free(furls);
    return;
  }
  for (int i = 0; i < nu; i++)
    furls[i] = g_bgn[i].use;

  unsigned char **bodies = calloc((unsigned)nu, sizeof *bodies);
  int *lens = calloc((unsigned)nu, sizeof *lens);
  int n = 0;
  if (bodies && lens) {
    if (fetch)
      fetch(furls, nu, bodies, lens);
    for (int i = 0; i < nu; i++) {
      strlcpy(tab[n].url, g_bgn[i].abs, (int)sizeof tab[n].url);
      tab[n].iw = tab[n].ih = 0;
      tab[n].rgba = 0;
      if (bodies[i] && lens[i] > 0) {
        unsigned w, h;
        unsigned char *rg = image_decode_rgba(bodies[i], lens[i], 0, 512, &w, &h);
        if (rg) {
          tab[n].rgba = rg;
          tab[n].iw = w;
          tab[n].ih = h;
        }
      }
      free(bodies[i]);
      n++; /* keep even a failed/undecodable entry so the index still maps */
    }
  }
  free(bodies);
  free(lens);
  free(furls);

  browser_buf_lock();
  B->pg->bgimgs = tab;
  B->pg->n_bgimgs = n;
  B->pg->bgimgs_cap = nu;
  /* Re-resolve each box's decoded-image index now the table exists */
  int nboxes = 0;
  struct lbox *boxes = layout_pool(&nboxes);
  for (int i = 0; i < nboxes; i++)
    if (boxes[i].bgi.kind == CSS_BGK_URL && boxes[i].bgi.url[0])
      boxes[i].bgi.img_idx = bg_image_index(boxes[i].bgi.url, page_url);
  browser_buf_unlock();
}

/* The decoded-background index for a box's url() (resolve + match the table key) */
int bg_image_index(const char *raw_url, const char *page_url)
{
  if (!raw_url || !raw_url[0] || B->pg->n_bgimgs == 0)
    return -1;
  char abs[URL_MAX];
  resolve_url(page_url, raw_url, abs, sizeof abs);
  for (int i = 0; i < B->pg->n_bgimgs; i++)
    if (streq(B->pg->bgimgs[i].url, abs))
      return i;
  return -1;
}
