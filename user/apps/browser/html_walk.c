#include "html_int.h"

static void walk_element_inner(struct dom_node *el);

/* Record this element's covered text range as a span (for click hit-testing) */
/* Emit a ::before / ::after pseudo-element's generated content as inline text */
static void emit_pseudo(struct dom_node *el, int which)
{
  if (!css_sheet_has_pseudo(g_sheet))
    return; /* no ::before/::after rules -> skip the extra cascade */
  unsigned seq = style_key();
  if (el->pseudo_seq != seq || el->pseudo_sheet != g_sheet) {
    dom_on_free_style_cache = free_style_cache;
    for (int w = 0; w < 2; w++) {
      struct css_style tmp;
      free(el->pseudo_style[w]);
      el->pseudo_style[w] = 0;
      if (css_pseudo_content(g_sheet, el, w + 1, &tmp) && (el->pseudo_style[w] = malloc(sizeof tmp)))
        *(struct css_style *)el->pseudo_style[w] = tmp;
    }
    el->pseudo_sheet = g_sheet;
    el->pseudo_seq = seq;
  }
  const struct css_style *pcs = el->pseudo_style[which - 1];
  if (!pcs)
    return;
  struct sty sv = sty_cur;
  apply_css(pcs);
  emit_text(pcs->content);
  sty_cur = sv;
}

/* The render walk recurses one stack frame per DOM level, each carrying a ~552-byte `struct */
#define WALK_DEPTH_MAX 256
int g_walk_depth;
unsigned g_walk_now; /* msec() sampled once per layout_dom walk */

/* Decimal/alpha/roman numeral writers for ordered <li> markers (1-based). */
static int li_decimal(int n, char *b, int pad)
{
  char t[12];
  int k = 0, v = n < 0 ? 0 : n;
  if (v == 0)
    t[k++] = '0';
  while (v) {
    t[k++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (k < pad)
    t[k++] = '0';
  int j = 0;
  while (k)
    b[j++] = t[--k];
  return j;
}
static int li_alpha(int n, char *b, int upper)
{
  char t[12];
  int k = 0;
  if (n < 1)
    n = 1;
  while (n > 0) {
    n--;
    t[k++] = (char)((upper ? 'A' : 'a') + n % 26);
    n /= 26;
  }
  int j = 0;
  while (k)
    b[j++] = t[--k];
  return j;
}
static int li_roman(int n, char *b, int upper)
{
  static const int v[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
  static const char *const su[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
  static const char *const sl[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
  if (n < 1 || n > 3999)
    return li_decimal(n, b, 0); /* out of range: fall back */
  int j = 0;
  for (int i = 0; i < 13; i++)
    while (n >= v[i]) {
      const char *s = upper ? su[i] : sl[i];
      while (*s)
        b[j++] = *s++;
      n -= v[i];
    }
  return j;
}

/* Build the <li> marker text for `ls` (resolved, not DEFAULT) and item number `n` */
static int li_marker(signed char ls, int n, char *out, int cap)
{
  if (ls == CSS_LST_NONE || cap < 8)
    return 0;
  int n0 = 0;
  if (ls == CSS_LST_DISC) {
    out[0] = '\0';
    sapp(out, &n0, cap, "\xe2\x80\xa2 ");
    return 1;
  } /* • */
  if (ls == CSS_LST_CIRCLE) {
    out[0] = '\0';
    sapp(out, &n0, cap, "\xe2\x97\xa6 ");
    return 1;
  } /* ◦ */
  if (ls == CSS_LST_SQUARE) {
    out[0] = '\0';
    sapp(out, &n0, cap, "\xe2\x96\xaa ");
    return 1;
  } /* ▪ */
  int k = 0;
  switch (ls) {
  case CSS_LST_DECIMAL_ZERO:
    k = li_decimal(n, out, 2);
    break;
  case CSS_LST_LOWER_ALPHA:
    k = li_alpha(n, out, 0);
    break;
  case CSS_LST_UPPER_ALPHA:
    k = li_alpha(n, out, 1);
    break;
  case CSS_LST_LOWER_ROMAN:
    k = li_roman(n, out, 0);
    break;
  case CSS_LST_UPPER_ROMAN:
    k = li_roman(n, out, 1);
    break;
  default:
    k = li_decimal(n, out, 0);
    break; /* CSS_LST_DECIMAL */
  }
  out[k++] = '.';
  out[k++] = ' ';
  out[k] = '\0';
  return 1;
}

/* Map an HTML presentational marker attribute - `type` on ul/ol/li, e.g */
static signed char list_type_from_attr(const char *v)
{
  if (!v || !v[0])
    return CSS_LST_DEFAULT;
  if (!v[1])
    switch (v[0]) { /* single-letter ordered forms */
    case '1':
      return CSS_LST_DECIMAL;
    case 'a':
      return CSS_LST_LOWER_ALPHA;
    case 'A':
      return CSS_LST_UPPER_ALPHA;
    case 'i':
      return CSS_LST_LOWER_ROMAN;
    case 'I':
      return CSS_LST_UPPER_ROMAN;
    }
  if (streq_ci(v, "disc"))
    return CSS_LST_DISC;
  if (streq_ci(v, "circle"))
    return CSS_LST_CIRCLE;
  if (streq_ci(v, "square"))
    return CSS_LST_SQUARE;
  if (streq_ci(v, "none"))
    return CSS_LST_NONE;
  return CSS_LST_DEFAULT;
}

/* Parse a signed integer attribute (`<ol start>` / `<li value>`) */
static int attr_int(struct dom_node *el, const char *name, int dflt)
{
  const char *v = dom_get_attr(el, name);
  if (!v)
    return dflt;
  while (*v == ' ' || *v == '\t')
    v++;
  int neg = 0;
  if (*v == '-') {
    neg = 1;
    v++;
  } else if (*v == '+')
    v++;
  if (*v < '0' || *v > '9')
    return dflt;
  int n = 0;
  for (; *v >= '0' && *v <= '9'; v++)
    n = n * 10 + (*v - '0');
  return neg ? -n : n;
}

/* Count direct <li> element children of a list */
static int count_list_items(struct dom_node *list)
{
  int n = 0;
  for (struct dom_node *c = list->first_child; c; c = c->next)
    if (c->type == DOM_ELEMENT && streq(c->tag, "li"))
      n++;
  return n;
}

static void walk_element(struct dom_node *el)
{
  if (g_walk_depth >= WALK_DEPTH_MAX)
    return;
  g_walk_depth++;
  int si = span_begin(el);
  walk_element_inner(el);
  span_end(si);
  g_walk_depth--;
}

static void walk_element_inner(struct dom_node *el)
{
  const char *name = el->tag;

  if (streq(name, "script") || streq(name, "style"))
    return; /* not rendered */
  if (streq(name, "noscript"))
    return;                   /* scripting is on, so its fallback is hidden */
  if (streq(name, "title")) { /* the document title... */
    struct dom_node *a;
    for (a = el->parent; a; a = a->parent)
      if (streq(a->tag, "svg"))
        return; /* ...not an SVG <title> tooltip */
    if (g_frame_depth == 0)
      capture_title(el); /* a frame's <title> isn't the page's */
    return;
  }

  struct css_style cs;
  compute_style(el, &cs);
  if (cs.disp == CSS_DISP_NONE)
    return;                                 /* display:none */
  anim_apply(g_sheet, el, &cs, g_walk_now); /* overlay @keyframes / transition */
  if (streq(name, "dialog") || dom_get_attr(el, "popover"))
    g_has_controls = true; /* route clicks and Escape to the page */
  if (streq(name, "dialog") && !dom_get_attr(el, "open"))
    return; /* closed modal is hidden */
  if (dom_get_attr(el, "popover") && !dom_get_attr(el, "popover-open"))
    return; /* closed popover is hidden */
  if (streq(name, "colgroup") || streq(name, "col"))
    return; /* table column metadata, no box */

  if (streq(name, "img")) {
    handle_img(el, &cs);
    return;
  }
  if (streq(name, "audio") || streq(name, "video")) {
    handle_media(el);
    return;
  }
  if (streq(name, "svg")) {
    handle_svg(el, &cs);
    return;
  }
  if (streq(name, "iframe")) {
    handle_iframe(el);
    return;
  }
  if (streq(name, "link")) {
    if (g_frame_depth == 0)
      handle_favicon(el);
    return;
  }
  if (streq(name, "br")) {
    block_break();
    return;
  }
  if (streq(name, "hr")) { /* a rule line  */
    block_break();
    unsigned char s = W_size;
    W_size = 7;
    W_n = text_put(W_n, ' ', 0);
    W_run_nl = 0;
    W_size = s;
    block_break();
    return;
  }
  if (streq(name, "input")) {
    g_has_controls = true;
    emit_input(el);
    return;
  }
  if (streq(name, "select")) {
    g_has_controls = true;
    emit_select(el);
    return;
  }
  if (streq(name, "button")) {
    struct sty sv = sty_cur;
    sty_cur.bold = 1;
    g_has_controls = true;
    emit_lit("[ ", ST_BOLD);
    walk(el);
    emit_lit(" ]", ST_BOLD);
    sty_cur = sv;
    return;
  }
  if (streq(name, "textarea")) {
    g_has_controls = true;
    emit_textarea(el);
    return;
  }
  if (streq(name, "progress")) {
    g_has_controls = true;
    emit_progress(el);
    return;
  }
  if (streq(name, "meter")) {
    g_has_controls = true;
    emit_meter(el);
    return;
  }
  if (streq(name, "rp"))
    return;                               /* ruby fallback parens, hidden */
  if (streq(name, "q")) {                 /* auto quotation marks */
    emit_lit("\xe2\x80\x9c", sty_byte()); /* “ */
    walk(el);
    emit_lit("\xe2\x80\x9d", sty_byte()); /* ” */
    return;
  }
  if (streq(name, "rt")) { /* ruby annotation, in parens */
    unsigned char sv_px = W_fontpx;
    W_fontpx = clamp_fontpx((W_fontpx ? W_fontpx : HTML_BASE_FONT) * 3 / 4);
    emit_lit("(", sty_byte());
    walk(el);
    emit_lit(")", sty_byte());
    W_fontpx = sv_px;
    return;
  }
  if (streq(name, "summary")) { /* disclosure heading */
    block_break();
    struct sty sv = sty_cur;
    sty_cur.bold = 1;
    emit_lit("\xe2\x96\xbe ", ST_BOLD); /* ▾ marker */
    walk(el);
    sty_cur = sv;
    block_break();
    return;
  }

  struct sty saved = sty_cur;
  unsigned char sv_size = W_size, sv_center = W_center, sv_mono = W_mono, sv_indent = W_indent, sv_pre = W_pre,
                sv_fontpx = W_fontpx;
  unsigned sv_bg = W_bg;
  char sv_lt = W_listtype;
  int sv_ol = W_olnum;
  signed char sv_lrev = W_listrev, sv_ldef = W_listdef;
  unsigned char sv_lh = W_lineheight;
  signed char sv_ls = W_liststyle;
  int linked = streq(name, "a") ? open_link(el) : 0;

  apply_ua_style(name);
  apply_css(&cs);
  if (cs.bg >= 0)
    W_bg = (unsigned)cs.bg;

  /* Block-level browser defaults. */
  if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2])
    W_size = (unsigned char)(name[1] - '0');

  /* font-size: an explicit CSS value wins; a percent scales the inherited size; else a heading's */
  if (cs.font_px >= 0)
    W_fontpx = clamp_fontpx(cs.font_px);
  else if (cs.font_pct >= 0)
    W_fontpx = clamp_fontpx((W_fontpx ? W_fontpx : HTML_BASE_FONT) * cs.font_pct / 100);
  else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2])
    W_fontpx = ua_h_px[name[1] - '1'];
  else if (streq(name, "small"))
    W_fontpx = clamp_fontpx((W_fontpx ? W_fontpx : HTML_BASE_FONT) * 5 / 6);
  else if (streq(name, "sub") || streq(name, "sup"))
    W_fontpx = clamp_fontpx((W_fontpx ? W_fontpx : HTML_BASE_FONT) * 3 / 4);
  if (streq(name, "center") || streq(name, "caption"))
    W_center = 1;
  else if (cs.center >= 0)
    W_center = (unsigned char)cs.center;
  if (streq(name, "pre")) {
    W_mono = 1;
    W_pre = 1;
  }
  if (streq(name, "kbd")) {
    W_mono = 1;
    if (cs.bg < 0)
      W_bg = 0xe8eaee; /* keycap tint */
  }
  if (streq(name, "blockquote"))
    W_indent = (unsigned char)(W_indent < 7 ? W_indent + 1 : 7);
  if (streq(name, "ul") || streq(name, "ol")) {
    int ordered = name[0] == 'o';
    W_indent = (unsigned char)(W_indent < 7 ? W_indent + 1 : 7);
    W_listtype = ordered ? 'o' : 'u';
    W_listdef = list_type_from_attr(dom_get_attr(el, "type")); /* presentational default */
    W_listrev = (ordered && dom_get_attr(el, "reversed")) ? -1 : 1;
    /* Seed the running number so the first item (no `value`) lands on `start` */
    int start = ordered ? attr_int(el, "start", W_listrev < 0 ? count_list_items(el) : 1) : 1;
    W_olnum = start - W_listrev;
  }

  /* white-space: nowrap suppresses line breaks; pre/pre-wrap/pre-line preserve spaces+newlines */
  if (cs.white_space == CSS_WS_NOWRAP)
    sty_cur.nowrap = 1;
  else if (cs.white_space == CSS_WS_PRE) {
    W_pre = 1;
    sty_cur.nowrap = 1;
  } else if (cs.white_space == CSS_WS_PRE_WRAP || cs.white_space == CSS_WS_PRE_LINE)
    W_pre = 1;
  else if (cs.white_space == CSS_WS_NORMAL) {
    W_pre = 0;
    sty_cur.nowrap = 0;
  }

  /* line-height: a px value, else a percent/unitless multiple of the resolved font-size */
  if (cs.line_height >= 0)
    W_lineheight = (unsigned char)(cs.line_height > 255 ? 255 : cs.line_height);
  else if (cs.lh_pct >= 0) {
    int base = W_fontpx ? W_fontpx : HTML_BASE_FONT;
    int lh = base * cs.lh_pct / 100;
    W_lineheight = (unsigned char)(lh > 255 ? 255 : (lh < 0 ? 0 : lh));
  }

  if (cs.list_style != CSS_LST_DEFAULT)
    W_liststyle = cs.list_style;

  int blk = (cs.disp == CSS_DISP_BLOCK) ? 1 : (cs.disp == CSS_DISP_INLINE) ? 0 : is_block_tag(name);
  if (cs.pos == CSS_POS_FIXED)
    blk = 1; /* a fixed box is blockified so it gets a layout box */

  if (blk)
    block_break();
  if (streq(name, "li")) { /* list marker  */
    unsigned char a = sty_byte();
    /* Marker type precedence */
    signed char ls = W_liststyle;
    if (ls == CSS_LST_DEFAULT)
      ls = list_type_from_attr(dom_get_attr(el, "type"));
    if (ls == CSS_LST_DEFAULT)
      ls = W_listdef;
    if (ls == CSS_LST_DEFAULT)
      ls = (W_listtype == 'o') ? CSS_LST_DECIMAL : CSS_LST_DISC;
    int ordered = (ls >= CSS_LST_DECIMAL); /* numbered kinds advance a counter */
    int num = 0;
    if (ordered) {
      num = attr_int(el, "value", W_olnum + W_listrev); /* `value` reseeds the sequence */
      W_olnum = num;
    }
    char mark[24];
    if (li_marker(ls, num, mark, sizeof mark))
      emit_lit(mark, a);
  }
  if (blk) {
    if (cs.bgi.kind == CSS_BGK_URL) { /* note the url() for the batched fetch, resolve its index */
      if (g_frame_depth == 0)
        bg_note_url(cs.bgi.url); /* frames don't pull background images */
      cs.bgi.img_idx = bg_image_index(cs.bgi.url, g_base_url);
    }
    layout_box_open(el, &cs, W_n); /* a block/flex box for the layout tree */
  }
  emit_pseudo(el, CSS_PSEUDO_BEFORE);
  walk(el);
  emit_pseudo(el, CSS_PSEUDO_AFTER);
  if (blk)
    layout_box_close(W_n);
  if (blk)
    block_break();

  if (linked)
    close_link();
  sty_cur = saved;
  W_size = sv_size;
  W_center = sv_center;
  W_mono = sv_mono;
  W_indent = sv_indent;
  W_pre = sv_pre;
  W_bg = sv_bg;
  W_fontpx = sv_fontpx;
  W_listtype = sv_lt;
  W_listrev = sv_lrev;
  W_listdef = sv_ldef;
  W_lineheight = sv_lh;
  W_liststyle = sv_ls;
  /* The ordered-list counter is list-scoped, not element-scoped */
  if (streq(name, "ul") || streq(name, "ol"))
    W_olnum = sv_ol;
}

void walk(struct dom_node *node)
{
  for (struct dom_node *c = node->first_child; c; c = c->next) {
    if (c->type == DOM_TEXT) {
      if (c->text)
        emit_text(c->text);
    } else
      walk_element(c);
  }
}
