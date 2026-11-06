#include "html_int.h"

/* Append one styled glyph at offset `n`, growing text/attr as needed, and return the new length */
/* Current block context, saved/restored around each element like sty_cur */
unsigned char W_size, W_center, W_mono, W_indent, W_pre;
unsigned char W_fontpx;                    /* current CSS font-size px (0 = default body) */
unsigned W_bg = BG_NONE;                   /* current background 0xRRGGBB, BG_NONE = none */
char W_listtype;                           /* 'u' / 'o' / 0  -  the enclosing list kind  */
int W_olnum;                               /* <ol> running item number (last assigned) */
signed char W_listrev;                     /* <ol reversed>: -1 counts down, +1 up     */
signed char W_listdef = CSS_LST_DEFAULT;   /* list's presentational default (type="") */
unsigned char W_lineheight;                /* current CSS line-height px (0 = font-derived) */
signed char W_liststyle = CSS_LST_DEFAULT; /* inherited list-style-type    */

/* CSS base font (px) that percent/relative sizes resolve against, plus the heading UA sizes */
const unsigned char ua_h_px[6] = {32, 24, 19, 16, 13, 11};

static unsigned char block_byte(void)
{
  return (unsigned char)((W_size & 7) | (W_center ? BP_CENTER : 0) | (W_mono ? BP_MONO : 0) |
                         ((W_indent & 7) << BP_INDENT_SH));
}

int text_put(int n, char c, unsigned char a)
{
  if (!browser_ensure_text(n + 1))
    return n;
  B->pg->text[n] = c;
  B->pg->attr[n] = a;
  B->pg->bp[n] = block_byte();
  B->pg->align[n] = W_center;
  B->pg->fsz[n] = W_fontpx;
  B->pg->lh[n] = W_lineheight;
  B->pg->fg[n] = sty_cur.fg;
  B->pg->bg[n] = W_bg;
  return n + 1;
}

/* Walk state (single-threaded */
int W_n, W_run_nl, W_cur_link;
bool W_pend_sp;

/* Element span table */
struct domspan {
  int start, end;
  struct dom_node *node;
};
static struct domspan *g_spans;
int g_nspans;
static int g_spans_cap;

int span_begin(struct dom_node *el)
{
  if (g_nspans >= g_spans_cap) {
    int nc = g_spans_cap ? g_spans_cap * 2 : 256;
    struct domspan *p = realloc(g_spans, (unsigned)nc * sizeof *p);
    if (!p)
      return -1;
    g_spans = p;
    g_spans_cap = nc;
  }
  int i = g_nspans++;
  g_spans[i].start = W_n;
  g_spans[i].end = W_n;
  g_spans[i].node = el;
  return i;
}
void span_end(int i)
{
  if (i >= 0 && i < g_nspans)
    g_spans[i].end = W_n;
}

struct dom_node *html_node_at_offset(int off)
{
  struct dom_node *best = 0;
  int bestw = 1 << 30;
  for (int i = 0; i < g_nspans; i++) {
    if (off >= g_spans[i].start && off < g_spans[i].end) {
      int w = g_spans[i].end - g_spans[i].start;
      if (w <= bestw) {
        bestw = w;
        best = g_spans[i].node;
      } /* deepest = narrowest, last wins */
    }
  }
  return best;
}

/* Inverse of html_node_at_offset */
int html_node_span(struct dom_node *node, int *start, int *end)
{
  for (int i = 0; i < g_nspans; i++)
    if (g_spans[i].node == node) {
      if (start)
        *start = g_spans[i].start;
      if (end)
        *end = g_spans[i].end;
      return 1;
    }
  return 0;
}

/* Append a codepoint to the styled-text buffer as UTF-8 (each byte carries the same attr */
int utf8_emit(int n, unsigned cp, unsigned char a)
{
  char b[4];
  int k = utf8_encode(cp, b);
  for (int i = 0; i < k; i++)
    n = text_put(n, b[i], a);
  return n;
}

/* Codepoints that must never reach layout */
bool cp_drop(unsigned cp)
{
  if (cp < 0x20 || cp == 0x7f)
    return true;
  if (cp >= 0x80 && cp <= 0x9f)
    return true;
  if (cp == 0xAD)
    return true; /* soft hyphen     */
  if (cp >= 0x200B && cp <= 0x200F)
    return true; /* zero-width / dir */
  if (cp == 0xFEFF)
    return true; /* BOM / ZWNBSP    */
  return false;
}

/* Apply text-transform (and font-variant small-caps) to one codepoint */
static unsigned ttx_apply(unsigned cp, unsigned char ttx, int smallcaps, bool *word)
{
  bool alpha =
      (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7);
  unsigned up = cp, lo = cp;
  if (cp >= 'a' && cp <= 'z')
    up = cp - 32;
  else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)
    up = cp - 0x20;
  if (cp >= 'A' && cp <= 'Z')
    lo = cp + 32;
  else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)
    lo = cp + 0x20;
  unsigned r = cp;
  switch (ttx) {
  case CSS_TT_UPPER:
    r = up;
    break;
  case CSS_TT_LOWER:
    r = lo;
    break;
  case CSS_TT_CAPITALIZE:
    r = (*word && alpha) ? up : cp;
    break;
  }
  if (smallcaps) {
    if (r >= 'a' && r <= 'z')
      r -= 32;
    else if (r >= 0xE0 && r <= 0xFE && r != 0xF7)
      r -= 0x20;
  }
  *word = !alpha;
  return r;
}

/* Emit text-node data */
void emit_text(const char *s)
{
  const char *p = s, *end = s + strlen(s);
  bool ws = true; /* at a word start (for capitalize) */
  while (p < end) {
    int c;
    if (*p == '&') {
      int adv;
      c = decode_entity(p, end, &adv);
      p += adv;
      if (c == -2)
        continue;
      if (c == -1) {
        W_pend_sp = true;
        continue;
      } /* nbsp -> space   */
    } else {
      c = read_char(&p, end);
    }
    if (W_pre) { /* <pre>: keep spacing */
      if (c == '\n') {
        W_n = text_put(W_n, '\n', 0);
        W_run_nl = 1;
        ws = true;
        continue;
      }
      if (c == '\r')
        continue;
      if (c == '\t') {
        for (int t = 0; t < 4; t++)
          W_n = text_put(W_n, ' ', sty_byte());
        W_run_nl = 0;
        ws = true;
        continue;
      }
      if (cp_drop((unsigned)c))
        continue;
      W_n = utf8_emit(W_n, ttx_apply((unsigned)c, sty_cur.ttx, sty_cur.smallcaps, &ws), sty_byte());
      W_run_nl = 0;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      W_pend_sp = true;
      ws = true;
      continue;
    }
    if (cp_drop((unsigned)c))
      continue;
    if (W_pend_sp) {
      if (W_n > 0 && W_run_nl == 0)
        W_n = text_put(W_n, ' ', 0);
      W_pend_sp = false;
    }
    W_n = utf8_emit(W_n, ttx_apply((unsigned)c, sty_cur.ttx, sty_cur.smallcaps, &ws), sty_byte());
    W_run_nl = 0;
  }
}

/* End the current line */
void block_break(void)
{
  W_pend_sp = false;
  if (W_run_nl < 2 && W_n > 0) {
    W_n = text_put(W_n, '\n', 0);
    W_run_nl++;
  }
}

void capture_title(struct dom_node *el)
{
  int tl = 0;
  bool t_pend = false;
  B->pg->doc_title[0] = '\0';
  for (struct dom_node *c = el->first_child; c; c = c->next) {
    if (c->type != DOM_TEXT || !c->text)
      continue;
    const char *p = c->text, *end = p + strlen(c->text);
    while (p < end) {
      int ch;
      if (*p == '&') {
        int adv;
        ch = decode_entity(p, end, &adv);
        p += adv;
        if (ch == -2)
          continue;
        if (ch == -1) {
          t_pend = true;
          continue;
        }
      } else {
        ch = (unsigned char)*p++;
      }
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        t_pend = true;
        continue;
      }
      if (ch < 0x20 || ch >= 0x7f)
        continue;
      if (t_pend && tl > 0 && tl < (int)sizeof B->pg->doc_title - 1)
        B->pg->doc_title[tl++] = ' ';
      t_pend = false;
      if (tl < (int)sizeof B->pg->doc_title - 1)
        B->pg->doc_title[tl++] = (char)ch;
      B->pg->doc_title[tl] = '\0';
    }
  }
}

/* Start recording an <a> */
int open_link(struct dom_node *el)
{
  if (!g_link_nav)
    return 0; /* sandboxed frame: link isn't navigable */
  if (!browser_ensure_links(B->pg->nlinks + 1))
    return 0;
  const char *href = dom_get_attr(el, "href");
  if (!href || !href[0])
    return 0;
  char buf[URL_MAX];
  int k = 0;
  for (; href[k] && k < (int)sizeof buf - 1; k++)
    buf[k] = href[k];
  buf[k] = '\0';
  decode_entities(buf);
  if (!buf[0] || buf[0] == '#' || starts_ci(buf, "javascript:") || starts_ci(buf, "mailto:"))
    return 0;
  struct link *L = &B->pg->links[B->pg->nlinks];
  L->start = W_n;
  /* Inside a frame, store the absolute href (resolved against the frame's own URL) */
  char abs[URL_MAX];
  const char *put = buf;
  if (g_frame_depth > 0 && !url_absolute(buf)) {
    resolve_url(g_base_url, buf, abs, sizeof abs);
    put = abs;
  }
  int j = 0;
  for (; put[j] && j < (int)sizeof L->href - 1; j++)
    L->href[j] = put[j];
  L->href[j] = '\0';
  /* target="_blank"/"_new" → open in a new tab (the click handler honors this) */
  const char *tgt = dom_get_attr(el, "target");
  L->newtab = (tgt && (streq_ci(tgt, "_blank") || streq_ci(tgt, "_new"))) ? 1 : 0;
  W_cur_link = B->pg->nlinks++;
  return 1;
}

void close_link(void)
{
  if (W_cur_link >= 0) {
    B->pg->links[W_cur_link].end = W_n;
    W_cur_link = -1;
  }
}
