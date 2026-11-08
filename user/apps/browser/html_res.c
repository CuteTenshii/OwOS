#include "features/clienthints/clienthints.h"
#include "fontstore.h"
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

/* Generic font-family keywords that resolve to the default proportional UI font (we have no */
static bool fam_generic_default(const char *n)
{
  return streq(n, "sans-serif") || streq(n, "serif") || streq(n, "system-ui") || streq(n, "ui-sans-serif") ||
         streq(n, "ui-serif") || streq(n, "ui-rounded") || streq(n, "cursive") || streq(n, "fantasy") ||
         streq(n, "-apple-system") || streq(n, "blinkmacsystemfont");
}
static bool fam_monospace(const char *n) { return streq(n, "monospace") || streq(n, "ui-monospace"); }

/* Load a TTF from a local file into `slot` (so it's freed with the other faces) */
static struct font *load_face_file(const char *path, struct loaded_face *slot)
{
  struct statinfo st;
  if (stat(path, &st) < 0 || st.is_dir || st.size == 0)
    return false;
  unsigned char *buf = malloc(st.size);
  if (!buf)
    return false;
  if (readfile_at(path, 0, buf, st.size) != (int)st.size) {
    free(buf);
    return false;
  }
  struct font *f = font_load(buf, (int)st.size);
  if (!f) {
    free(buf);
    return false;
  }
  slot->data = buf;
  slot->font = f;
  slot->family[0] = '\0';
  return f;
}

/* Load the sheet's @font-face faces */
/* Is `family` one of the comma-separated names in the font-family `stack`? Uses the same naive */
static bool family_in_stack(const char *stack, const char *family)
{
  const char *p = stack;
  while (*p) {
    char name[CSS_FAM_MAX];
    int n = 0;
    while (*p && *p != ',' && n < (int)sizeof name - 1)
      name[n++] = *p++;
    name[n] = '\0';
    while (*p && *p != ',')
      p++;
    if (*p == ',')
      p++;
    if (name[0] && streq(name, family))
      return true;
  }
  return false;
}

/* Ensure B->pg->faces holds `need` entries (grows on demand) */
static bool faces_room(int need)
{
  if (need <= B->pg->faces_cap)
    return true;
  int nc = B->pg->faces_cap ? B->pg->faces_cap : 4;
  while (nc < need)
    nc *= 2;
  struct loaded_face *n = realloc(B->pg->faces, (unsigned)nc * sizeof *n);
  if (!n)
    return false;
  B->pg->faces = n;
  B->pg->faces_cap = nc;
  return true;
}

void load_faces(struct dom_node *root, const char *page_url, res_batch_fn fetch)
{
  /* The page renders with a single body font, so the ONLY @font-face faces that can ever be used */
  struct css_style cs;
  struct dom_node *body_el = find_tag(root, "body");
  const char *stack = "";
  if (body_el) {
    compute_style(body_el, &cs);
    stack = cs.fam;
  }

  int nfaces = css_fontface_count(g_sheet);
  char **urls = nfaces ? malloc((unsigned)nfaces * sizeof *urls) : 0;
  char (*fams)[CSS_FAM_MAX] = nfaces ? malloc((unsigned)nfaces * sizeof *fams) : 0;
  int nu = 0;
  if (nfaces && (!urls || !fams)) {
    free(urls);
    free(fams);
    urls = 0;
    fams = 0;
    nfaces = 0;
  }

  for (int i = 0; i < nfaces; i++) {
    const char *family = 0, *url = 0, *base = 0;
    css_fontface_at(g_sheet, i, &family, &url, &base);
    if (!family || !family[0] || !url || !url[0])
      continue;
    if (!family_in_stack(stack, family))
      continue; /* body can't use it */

    /* A relative src resolves against the sheet that declared it (so an external sheet's */
    char abs[URL_MAX], use[URL_MAX];
    resolve_url(base && base[0] ? base : page_url, url, abs, sizeof abs);
    if (!csp_url_ok(CSP_FONT, abs, "font", use, sizeof use))
      continue; /* font-src */
    int L = strlen(use);
    char *u = malloc((unsigned)L + 1);
    if (!u)
      continue;
    for (int k = 0; k <= L; k++)
      u[k] = use[k];

    int j = 0;
    for (; family[j] && j < CSS_FAM_MAX - 1; j++)
      fams[nu][j] = family[j];
    fams[nu][j] = '\0';
    urls[nu++] = u;
  }

  /* Batch-fetch the font files (multiplexed over h2 same-origin), then parse */
  struct loaded_face *nf = malloc((unsigned)(nu + 1) * sizeof *nf);
  int nn = 0;
  if (nf && nu > 0) {
    unsigned char **bodies = calloc((unsigned)nu, sizeof *bodies);
    int *lens = calloc((unsigned)nu, sizeof *lens);
    if (bodies && lens) {
      if (fetch)
        fetch(urls, nu, bodies, lens);
      for (int i = 0; i < nu; i++) {
        unsigned char *body = bodies[i];
        if (!body || lens[i] <= 0) {
          free(body);
          continue;
        }
        /* font_load transparently decodes WOFF/WOFF2 containers to SFNT. */
        struct font *f = font_load(body, lens[i]);
        if (!f) {
          free(body);
          continue;
        } /* unsupported (e.g. pure CFF) */
        int j = 0;
        for (; fams[i][j] && j < (int)sizeof nf[nn].family - 1; j++)
          nf[nn].family[j] = fams[i][j];
        nf[nn].family[j] = '\0';
        nf[nn].font = f;
        nf[nn].data = body;
        nn++;
      }
    }
    free(bodies);
    free(lens);
  }
  for (int i = 0; i < nu; i++)
    free(urls[i]);
  free(urls);
  free(fams);

  /* Walk the <body> font-family stack and pick the first available family. */
  struct font *chosen = 0;
  if (body_el && nf) {
    const char *p = stack;
    while (*p) {
      char name[CSS_FAM_MAX];
      int n = 0;
      while (*p && *p != ',' && n < (int)sizeof name - 1)
        name[n++] = *p++;
      name[n] = '\0';
      while (*p && *p != ',')
        p++;
      if (*p == ',')
        p++;
      if (!name[0])
        continue;

      int hit = 0; /* a loaded @font-face? */
      for (int i = 0; i < nn; i++)
        if (streq(nf[i].family, name)) {
          chosen = nf[i].font;
          hit = 1;
          break;
        }
      if (hit)
        break;
      if (fam_generic_default(name)) {
        chosen = 0;
        break;
      } /* default UI sans */
      char fpath[96];
      if (fam_monospace(name))
        fontstore_path(FONT_MONO, fpath, sizeof fpath);
      if (fam_monospace(name) || fontstore_find(name, fpath, sizeof fpath)) {
        struct font *m = load_face_file(fpath, &nf[nn]);
        if (m) {
          chosen = m;
          nn++;
          break;
        }
      }
      /* a named family we don't have -> fall through to the next entry */
    }
  }

  /* Publish: drop the previous page's faces, install the new set. */
  browser_buf_lock();
  for (int i = 0; i < B->pg->n_faces; i++) {
    if (B->pg->faces[i].font)
      font_free(B->pg->faces[i].font);
    free(B->pg->faces[i].data);
  }
  if (nn > 0 && faces_room(nn)) {
    for (int i = 0; i < nn; i++)
      B->pg->faces[i] = nf[i];
    B->pg->n_faces = nn;
    B->pg->body_font = chosen;
  } else {
    B->pg->n_faces = 0;
    B->pg->body_font = 0; /* faces_room OOM: free below, fall back */
  }
  browser_buf_unlock();

  if (nf) {
    if (B->pg->n_faces == 0) /* publish failed: don't leak parsed faces */
      for (int i = 0; i < nn; i++) {
        if (nf[i].font)
          font_free(nf[i].font);
        free(nf[i].data);
      }
    free(nf);
  }
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
