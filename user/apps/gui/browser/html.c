/* html.c — the renderer. Walks the DOM (parsed by dom.c) into styled text in
 * B->text/B->attr, collecting links, inline images, the favicon and title.
 * Element styling comes from the css.c engine (sheet + inline + attrs); this file
 * maps the computed style onto the text renderer and does the HTML-entity work. */
#include "browser.h"
#include "css.h"
#include "font.h"

static css_sheet *g_sheet;     /* the page's collected <style> rules, per render */

/* Nearest of the 16-colour palette to an 0xRRGGBB value. */
static int nearest_palette(unsigned rgb)
{
	int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	int best = 0; long bestd = 1L << 30;
	for (int i = 0; i < 16; i++) {
		int pr = (palette[i] >> 16) & 0xff, pg = (palette[i] >> 8) & 0xff,
		    pb = palette[i] & 0xff;
		long d = (long)(r - pr) * (r - pr) + (long)(g - pg) * (g - pg) +
		         (long)(b - pb) * (b - pb);
		if (d < bestd) { bestd = d; best = i; }
	}
	return best;
}

/* Computed style for one element: presentational attrs (lowest), sheet rules,
 * then inline style="" (highest). */
static void compute_style(struct dom_node *el, struct css_style *cs)
{
	css_style_reset(cs);
	css_apply_attrs(el, cs);
	css_apply_sheet(g_sheet, el, cs);
	const char *inl = dom_get_attr(el, "style");
	if (inl) css_apply_inline(inl, cs);
}

/* Case-insensitive substring test (for matching tokens inside a rel= list). */
static int contains_ci(const char *hay, const char *needle)
{
	for (; *hay; hay++)
		if (starts_ci(hay, needle)) return 1;
	return 0;
}

/* Source charset for the document being parsed. We render Unicode, so non-UTF-8
 * input is transcoded as it is read. Default UTF-8; the HTTP Content-Type charset
 * (set by fetch.c) wins, else a <meta charset> in the document, else the default.
 * A fair number of pages (some Google locales among them) still declare ISO-8859-
 * 1, which in practice is Windows-1252 — so the Latin-1 path uses the cp1252 map
 * for 0x80..0x9F and is the identity for 0xA0..0xFF (== Unicode U+00A0..U+00FF). */
enum { CS_UTF8, CS_LATIN1 };
static int g_charset;
static int g_charset_from_header;

/* True if `cs` (a charset name, possibly inside a longer string) is a Latin-1 /
 * Windows-1252 label; returns 0 for utf-8 / unknown (treated as UTF-8). */
static int charset_is_latin1(const char *cs)
{
	return contains_ci(cs, "8859-1") || contains_ci(cs, "latin-1") ||
	       contains_ci(cs, "latin1")  || contains_ci(cs, "1252")    ||
	       contains_ci(cs, "8859_1");
}

/* fetch.c calls this with the raw Content-Type header value (may be NULL). A
 * charset there is authoritative (locks out the <meta> guess). */
void html_set_charset(const char *content_type)
{
	g_charset = CS_UTF8;
	g_charset_from_header = 0;
	if (!content_type) return;
	const char *cs = content_type;
	for (; *cs; cs++) if (starts_ci(cs, "charset")) break;
	if (!*cs) return;                                  /* header gave no charset */
	g_charset_from_header = 1;
	if (charset_is_latin1(cs)) g_charset = CS_LATIN1;
}

/* Scan the document head for <meta charset=...> / <meta http-equiv="Content-Type"
 * content="...charset=...">. Only consulted when the header did not specify one. */
static void detect_meta_charset(void)
{
	if (g_charset_from_header) return;
	const char *p = B->raw, *end = B->raw + B->raw_len;
	for (; p < end; p++) {
		if (*p != '<' || !starts_ci(p, "<meta")) continue;
		const char *q = p, *tend = p;
		while (tend < end && *tend != '>') tend++;       /* this tag's extent */
		for (q = p; q < tend; q++)
			if (starts_ci(q, "charset")) {
				if (charset_is_latin1(q)) g_charset = CS_LATIN1;
				return;                                  /* first one wins */
			}
		if (starts_ci(p + 5, " charset") || tend >= end) { /* keep scanning <head> */ }
		p = tend;
	}
}

/* Read one source character, transcoding per the active charset, advancing `*pp`.
 * Returns the Unicode codepoint. */
static int cp1252_high(unsigned char b)
{
	if (b >= 0xA0) return b;                              /* Latin-1 == Unicode */
	static const unsigned short m[32] = {
		0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,
		0x02C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,
		0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
		0x02DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178
	};
	return m[b - 0x80];
}

static int read_char(const char **pp, const char *end)
{
	const char *p = *pp;
	unsigned char b = (unsigned char)*p;
	if (b < 0x80) { *pp = p + 1; return b; }
	if (g_charset == CS_LATIN1) { *pp = p + 1; return cp1252_high(b); }
	unsigned u;
	*pp = p + utf8_decode(p, end, &u);
	return (int)u;
}

/* Current text style. The walk saves/restores it around each element (via the C
 * call stack), so style nesting needs no explicit stack. */
struct sty { unsigned char fg, bold, under; };
static struct sty sty_cur;

static unsigned char sty_byte(void)
{
	unsigned char a = sty_cur.fg & ST_COLOR;
	if (sty_cur.bold)  a |= ST_BOLD;
	if (sty_cur.under) a |= ST_UNDER;
	return a;
}

/* User-agent default style for a tag (the lowest layer; author CSS overrides). */
static void apply_ua_style(const char *name)
{
	if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
		sty_cur.fg = 8; sty_cur.bold = 1;
	} else if (streq(name, "b") || streq(name, "strong")) {
		sty_cur.bold = 1;
	} else if (streq(name, "i") || streq(name, "em")) {
		sty_cur.fg = 7;
	} else if (streq(name, "a")) {
		sty_cur.fg = 1; sty_cur.under = 1;
	} else if (streq(name, "u") || streq(name, "ins")) {
		sty_cur.under = 1;
	} else if (streq(name, "code") || streq(name, "kbd") ||
	           streq(name, "samp") || streq(name, "tt")) {
		sty_cur.fg = 5;
	} else if (streq(name, "mark")) {
		sty_cur.fg = 2;
	}
}

/* Layer a computed CSS style over the UA defaults already in sty_cur. */
static void apply_css(const struct css_style *cs)
{
	if (cs->color >= 0) sty_cur.fg = (unsigned char)nearest_palette((unsigned)cs->color);
	if (cs->bold >= 0)  sty_cur.bold = cs->bold ? 1 : 0;
	if (cs->under >= 0) sty_cur.under = cs->under ? 1 : 0;
}

/* The renderer's line-break set (distinct from dom.c's CSS-block set: this one
 * includes br/li/tr/dd/dt, which force a new line but aren't <p>-closers). */
static int is_block_tag(const char *name)
{
	static const char *const blk[] = {
		"p", "br", "div", "li", "tr", "ul", "ol", "h1", "h2", "h3",
		"h4", "h5", "h6", "table", "hr", "section", "article", "header",
		"footer", "nav", "blockquote", "pre", "head", "form",
		"dd", "dt", "dl", "figure", "main", "aside", 0
	};
	for (int i = 0; blk[i]; i++) if (streq(name, blk[i])) return 1;
	return 0;
}

/* decode_entity returns a real Unicode codepoint now (the renderer is UTF-8), or
 * the sentinels -1 (non-breaking space -> collapsible space) and -2 (drop). */
static int decode_entity(const char *p, const char *end, int *adv)
{
	int n = 1;
	while (p + n < end && n < 12 && p[n] != ';' && p[n] != '&' &&
	       p[n] != ' ' && p[n] != '<') n++;
	if (p + n >= end || p[n] != ';') { *adv = 1; return '&'; }
	const char *body = p + 1;
	int blen = n - 1;
	if (blen >= 2 && body[0] == '#') {
		unsigned v = 0;
		if (body[1] == 'x' || body[1] == 'X') {
			for (int i = 2; i < blen; i++) {
				char c = body[i];
				if      (c >= '0' && c <= '9') v = v * 16 + (c - '0');
				else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
				else { v = 0; break; }
			}
		} else {
			for (int i = 1; i < blen; i++) {
				char c = body[i];
				if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
				else { v = 0; break; }
			}
		}
		*adv = n + 1;
		if (v == 0xA0) return -1;            /* nbsp -> collapsible space */
		if (v == 0) return -2;               /* malformed -> drop         */
		return (int)v;
	}
	/* Named entities -> their real Unicode codepoints (the renderer is UTF-8). */
	static const struct { const char *name; int ch; } ent[] = {
		{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
		{ "apos", '\'' }, { "nbsp", -1 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
		{ "hellip", 0x2026 }, { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 },
		{ "rsquo", 0x2019 }, { "lsquo", 0x2018 }, { "rdquo", 0x201D }, { "ldquo", 0x201C },
		{ "sbquo", 0x201A }, { "bdquo", 0x201E }, { "raquo", 0xBB }, { "laquo", 0xAB },
		{ "middot", 0xB7 }, { "bull", 0x2022 }, { "deg", 0xB0 }, { "times", 0xD7 },
		{ "divide", 0xF7 }, { "plusmn", 0xB1 }, { "euro", 0x20AC }, { "pound", 0xA3 },
		{ "cent", 0xA2 }, { "sect", 0xA7 }, { "para", 0xB6 }, { "dagger", 0x2020 },
		{ "Dagger", 0x2021 }, { "prime", 0x2032 }, { "Prime", 0x2033 },
		{ "agrave", 0xE0 }, { "aacute", 0xE1 }, { "acirc", 0xE2 }, { "atilde", 0xE3 },
		{ "auml", 0xE4 }, { "aring", 0xE5 }, { "aelig", 0xE6 }, { "ccedil", 0xE7 },
		{ "egrave", 0xE8 }, { "eacute", 0xE9 }, { "ecirc", 0xEA }, { "euml", 0xEB },
		{ "igrave", 0xEC }, { "iacute", 0xED }, { "icirc", 0xEE }, { "iuml", 0xEF },
		{ "ntilde", 0xF1 }, { "ograve", 0xF2 }, { "oacute", 0xF3 }, { "ocirc", 0xF4 },
		{ "otilde", 0xF5 }, { "ouml", 0xF6 }, { "oslash", 0xF8 }, { "ugrave", 0xF9 },
		{ "uacute", 0xFA }, { "ucirc", 0xFB }, { "uuml", 0xFC }, { "yacute", 0xFD },
		{ "yuml", 0xFF }, { "szlig", 0xDF }, { "Agrave", 0xC0 }, { "Aacute", 0xC1 },
		{ "Acirc", 0xC2 }, { "Atilde", 0xC3 }, { "Auml", 0xC4 }, { "Aring", 0xC5 },
		{ "AElig", 0xC6 }, { "Ccedil", 0xC7 }, { "Egrave", 0xC8 }, { "Eacute", 0xC9 },
		{ "Ecirc", 0xCA }, { "Euml", 0xCB }, { "Igrave", 0xCC }, { "Iacute", 0xCD },
		{ "Ntilde", 0xD1 }, { "Ograve", 0xD2 }, { "Oacute", 0xD3 }, { "Ouml", 0xD6 },
		{ "Oslash", 0xD8 }, { "Ugrave", 0xD9 }, { "Uuml", 0xDC }, { 0, 0 }
	};
	for (int i = 0; ent[i].name; i++) {
		const char *nm = ent[i].name;
		int k = 0;
		while (nm[k] && k < blen && nm[k] == body[k]) k++;
		if (!nm[k] && k == blen) { *adv = n + 1; return ent[i].ch; }
	}
	*adv = 1;
	return '&';
}

/* Decode HTML entities in a string in place — mainly so href/src attribute
 * values (e.g. "a?x=1&amp;y=2") become real URLs ("a?x=1&y=2") before we
 * navigate or fetch them. A codepoint is written back as UTF-8; since an entity
 * is always at least as long as the bytes it decodes to (and raw bytes copy 1:1),
 * the output never overruns the input, so the in-place rewrite is safe. */
static void decode_entities(char *s)
{
	char *o = s;
	const char *p = s, *end = s + strlen(s);
	while (p < end) {
		if (*p == '&') {
			int adv, c = decode_entity(p, end, &adv);
			p += adv;
			if (c == -2) continue;          /* drop                            */
			if (c == -1) { *o++ = ' '; continue; }
			o += utf8_encode((unsigned)c, o);
		} else {
			*o++ = *p++;
		}
	}
	*o = '\0';
}

/* Append one styled glyph at offset `n`, growing text/attr as needed, and return
 * the new length. If the heap is exhausted the glyph is silently dropped (the
 * page just stops growing) — the same graceful truncation the old fixed buffer
 * did when full, only now the ceiling is available memory, not a fixed cap. */
/* Current block context, saved/restored around each element like sty_cur
 * (BP_* layout in browser.h). */
static unsigned char W_size, W_center, W_mono, W_indent, W_pre;
static unsigned char W_bg = 0xFF;   /* background palette index, 0xFF = none    */
static char W_listtype;             /* 'u' / 'o' / 0 — the enclosing list kind  */
static int  W_olnum;                /* <ol> item counter                        */

static unsigned char block_byte(void)
{
	return (unsigned char)((W_size & 7) | (W_center ? BP_CENTER : 0) |
	        (W_mono ? BP_MONO : 0) | ((W_indent & 7) << BP_INDENT_SH));
}

static int text_put(int n, char c, unsigned char a)
{
	if (!ensure_text(n + 1)) return n;
	B->text[n] = c;
	B->attr[n] = a;
	B->bp[n]  = block_byte();
	B->bgi[n] = W_bg;
	return n + 1;
}

/* Walk state (single-threaded: only the worker calls in). W_n = rendered length;
 * pend_sp/run_nl drive whitespace collapsing; W_cur_link = open <a>, or -1. */
static int W_n, W_pend_sp, W_run_nl, W_cur_link;

static void walk(struct dom_node *node);

/* Append a codepoint to the styled-text buffer as UTF-8 (each byte carries the
 * same attr, so the renderer's same-attr runs keep a glyph's bytes together). */
static int utf8_emit(int n, unsigned cp, unsigned char a)
{
	char b[4];
	int k = utf8_encode(cp, b);
	for (int i = 0; i < k; i++) n = text_put(n, b[i], a);
	return n;
}

/* Codepoints that must never reach layout: C0/C1 controls, DEL, soft hyphen, the
 * zero-width set, and the BOM. (Real spaces/newlines are handled before this.) */
static int cp_drop(unsigned cp)
{
	if (cp < 0x20 || cp == 0x7f) return 1;
	if (cp >= 0x80 && cp <= 0x9f) return 1;
	if (cp == 0xAD) return 1;                              /* soft hyphen     */
	if (cp >= 0x200B && cp <= 0x200F) return 1;            /* zero-width / dir */
	if (cp == 0xFEFF) return 1;                            /* BOM / ZWNBSP    */
	return 0;
}

/* Emit text-node data: decode entities AND raw UTF-8, collapse whitespace, drop
 * control/zero-width codepoints, and store the rest as UTF-8. */
static void emit_text(const char *s)
{
	const char *p = s, *end = s + strlen(s);
	while (p < end) {
		int c;
		if (*p == '&') {
			int adv;
			c = decode_entity(p, end, &adv);
			p += adv;
			if (c == -2) continue;
			if (c == -1) { W_pend_sp = 1; continue; }      /* nbsp -> space   */
		} else {
			c = read_char(&p, end);
		}
		if (W_pre) {                                        /* <pre>: keep spacing */
			if (c == '\n') { W_n = text_put(W_n, '\n', 0); W_run_nl = 1; continue; }
			if (c == '\r') continue;
			if (c == '\t') { for (int t = 0; t < 4; t++) W_n = text_put(W_n, ' ', sty_byte()); W_run_nl = 0; continue; }
			if (cp_drop((unsigned)c)) continue;
			W_n = utf8_emit(W_n, (unsigned)c, sty_byte());
			W_run_nl = 0;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { W_pend_sp = 1; continue; }
		if (cp_drop((unsigned)c)) continue;
		if (W_pend_sp) {
			if (W_n > 0 && W_run_nl == 0) W_n = text_put(W_n, ' ', 0);
			W_pend_sp = 0;
		}
		W_n = utf8_emit(W_n, (unsigned)c, sty_byte());
		W_run_nl = 0;
	}
}

/* End the current line: at most one blank line, never leading, clears pending sp. */
static void block_break(void)
{
	W_pend_sp = 0;
	if (W_run_nl < 2 && W_n > 0) { W_n = text_put(W_n, '\n', 0); W_run_nl++; }
}

static void capture_title(struct dom_node *el)
{
	int tl = 0, t_pend = 0;
	B->doc_title[0] = '\0';
	for (struct dom_node *c = el->first_child; c; c = c->next) {
		if (c->type != DOM_TEXT || !c->text) continue;
		const char *p = c->text, *end = p + strlen(c->text);
		while (p < end) {
			int ch;
			if (*p == '&') {
				int adv;
				ch = decode_entity(p, end, &adv);
				p += adv;
				if (ch == -2) continue;
				if (ch == -1) { t_pend = 1; continue; }
			} else {
				ch = (unsigned char)*p++;
			}
			if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { t_pend = 1; continue; }
			if (ch < 0x20 || ch >= 0x7f) continue;
			if (t_pend && tl > 0 && tl < (int)sizeof B->doc_title - 1)
				B->doc_title[tl++] = ' ';
			t_pend = 0;
			if (tl < (int)sizeof B->doc_title - 1) B->doc_title[tl++] = (char)ch;
			B->doc_title[tl] = '\0';
		}
	}
}

/* Start recording an <a>: returns 1 if the href is navigable (caller closes it),
 * 0 for empty / fragment / javascript: / mailto:. */
static int open_link(struct dom_node *el)
{
	if (!ensure_links(B->nlinks + 1)) return 0;
	const char *href = dom_get_attr(el, "href");
	if (!href || !href[0]) return 0;
	char buf[URL_MAX];
	int k = 0;
	for (; href[k] && k < (int)sizeof buf - 1; k++) buf[k] = href[k];
	buf[k] = '\0';
	decode_entities(buf);
	if (!buf[0] || buf[0] == '#' || starts_ci(buf, "javascript:") ||
	    starts_ci(buf, "mailto:"))
		return 0;
	struct link *L = &B->links[B->nlinks];
	L->start = W_n;
	int j = 0;
	for (; buf[j] && j < (int)sizeof L->href - 1; j++) L->href[j] = buf[j];
	L->href[j] = '\0';
	W_cur_link = B->nlinks++;
	return 1;
}

static void close_link(void)
{
	if (W_cur_link >= 0) {
		B->links[W_cur_link].end = W_n;
		W_cur_link = -1;
	}
}

/* Append an inline <img> to B->imgs on its own line (box from the computed CSS
 * width/height). data: URIs are skipped. */
static void handle_img(struct dom_node *el, const struct css_style *cs)
{
	if (!ensure_imgs(B->n_imgs + 1)) return;
	const char *src = dom_get_attr(el, "src");
	if (!src || !src[0]) return;
	char srcbuf[URL_MAX];
	int k = 0;
	for (; src[k] && k < (int)sizeof srcbuf - 1; k++) srcbuf[k] = src[k];
	srcbuf[k] = '\0';
	decode_entities(srcbuf);
	if (!srcbuf[0] || starts_ci(srcbuf, "data:")) return;

	if (W_run_nl < 1 && W_n > 0) W_n = text_put(W_n, '\n', 0);

	struct inl_img *im = &B->imgs[B->n_imgs];
	int j = 0;
	for (; srcbuf[j] && j < (int)sizeof im->href - 1; j++) im->href[j] = srcbuf[j];
	im->href[j] = '\0';

	im->alt[0] = '\0';
	const char *alt = dom_get_attr(el, "alt");
	if (alt) {
		int a = 0;
		for (; alt[a] && a < (int)sizeof im->alt - 1; a++) im->alt[a] = alt[a];
		im->alt[a] = '\0';
	}

	const char *ld = dom_get_attr(el, "loading");      /* lazy -> fetch on scroll-in */
	im->lazy = (ld && starts_ci(ld, "lazy")) ? 1 : 0;
	im->want = 0; im->fetched = 0;

	im->cw = (cs->width  >= 0) ? cs->width  : 0;
	im->ch = (cs->height >= 0) ? cs->height : 0;

	im->text_pos = W_n;
	im->ok = 0; im->rows = 0; im->start_row = -1;
	im->iw = im->ih = 0; im->coff = im->clen = 0;
	im->is_argb = 0; im->uid = 0;
	B->n_imgs++;
	W_n = text_put(W_n, '\n', 0);
	W_run_nl = 1;
}

/* <link rel="icon">: standard icon outranks apple-touch; mask-icon (SVG) skipped. */
static void handle_favicon(struct dom_node *el)
{
	const char *rel = dom_get_attr(el, "rel");
	if (!rel || !contains_ci(rel, "icon") || contains_ci(rel, "mask-icon")) return;
	int pri = contains_ci(rel, "apple-touch") ? 1 : 2;
	if (pri < B->favicon_pri) return;
	const char *href = dom_get_attr(el, "href");
	if (!href || !href[0] || starts_ci(href, "data:")) return;
	char buf[URL_MAX];
	int k = 0;
	for (; href[k] && k < (int)sizeof buf - 1; k++) buf[k] = href[k];
	buf[k] = '\0';
	decode_entities(buf);
	int j = 0;
	for (; buf[j] && j < (int)sizeof B->favicon_href - 1; j++) B->favicon_href[j] = buf[j];
	B->favicon_href[j] = '\0';
	B->favicon_pri = pri;
}

/* Emit a literal ASCII/UTF-8 run with attr `a`, no whitespace collapsing — used
 * for the chrome around form controls and list markers. */
static void emit_lit(const char *s, unsigned char a)
{
	if (W_pend_sp) { if (W_n > 0 && W_run_nl == 0) W_n = text_put(W_n, ' ', 0); W_pend_sp = 0; }
	for (const char *p = s; *p; p++) { W_n = text_put(W_n, *p, a); W_run_nl = 0; }
}

/* Render <input> as a visible boxed widget (no interaction yet). */
static void emit_input(struct dom_node *el)
{
	const char *type = dom_get_attr(el, "type");
	const char *val  = dom_get_attr(el, "value");
	const char *ph   = dom_get_attr(el, "placeholder");
	if (type && streq(type, "hidden")) return;
	if (type && (streq(type, "submit") || streq(type, "button") || streq(type, "reset"))) {
		emit_lit("[ ", ST_BOLD);
		emit_lit(val ? val : (streq(type, "reset") ? "Reset" : "Submit"), ST_BOLD);
		emit_lit(" ]", ST_BOLD);
		return;
	}
	if (type && (streq(type, "checkbox") || streq(type, "radio"))) {
		int on = dom_get_attr(el, "checked") != 0;
		if (streq(type, "radio")) emit_lit(on ? "(o)" : "( )", ST_BOLD);
		else                      emit_lit(on ? "[x]" : "[ ]", ST_BOLD);
		return;
	}
	emit_lit("[ ", ST_BOLD);
	emit_lit(val ? val : (ph ? ph : "         "), val || ph ? (unsigned char)0 : ST_BOLD);
	emit_lit(" ]", ST_BOLD);
}

/* Render <select> as a boxed control showing the selected (or first) option. */
static void emit_select(struct dom_node *el)
{
	struct dom_node *pick = 0;
	for (struct dom_node *o = el->first_child; o; o = o->next) {
		if (o->type == DOM_TEXT || !o->tag || !streq(o->tag, "option")) continue;
		if (!pick) pick = o;
		if (dom_get_attr(o, "selected")) { pick = o; break; }
	}
	emit_lit("[ ", ST_BOLD);
	if (pick)
		for (struct dom_node *t = pick->first_child; t; t = t->next)
			if (t->type == DOM_TEXT && t->text) emit_text(t->text);
	emit_lit(" \xe2\x96\xbe ]", ST_BOLD);   /* small down triangle */
}

static void walk(struct dom_node *node);

static void walk_element(struct dom_node *el)
{
	const char *name = el->tag;

	if (streq(name, "script") || streq(name, "style")) return;   /* not rendered */
	if (streq(name, "title")) { capture_title(el); return; }

	struct css_style cs;
	compute_style(el, &cs);
	if (cs.disp == CSS_DISP_NONE) return;                        /* display:none */

	if (streq(name, "img"))  { handle_img(el, &cs); return; }
	if (streq(name, "link")) { handle_favicon(el); return; }
	if (streq(name, "br")) { block_break(); return; }
	if (streq(name, "hr")) {                                     /* a rule line  */
		block_break();
		unsigned char s = W_size; W_size = 7;
		W_n = text_put(W_n, ' ', 0); W_run_nl = 0;
		W_size = s; block_break();
		return;
	}
	if (streq(name, "input"))  { emit_input(el);  return; }
	if (streq(name, "select")) { emit_select(el); return; }
	if (streq(name, "button")) {
		struct sty sv = sty_cur; sty_cur.bold = 1;
		emit_lit("[ ", ST_BOLD); walk(el); emit_lit(" ]", ST_BOLD);
		sty_cur = sv; return;
	}
	if (streq(name, "textarea")) { emit_lit("[ ", ST_BOLD); walk(el); emit_lit(" ]", ST_BOLD); return; }

	struct sty saved = sty_cur;
	unsigned char sv_size = W_size, sv_center = W_center, sv_mono = W_mono,
	              sv_indent = W_indent, sv_pre = W_pre, sv_bg = W_bg;
	char sv_lt = W_listtype; int sv_ol = W_olnum;
	int linked = streq(name, "a") ? open_link(el) : 0;

	apply_ua_style(name);
	apply_css(&cs);
	if (cs.bg >= 0) W_bg = (unsigned char)nearest_palette((unsigned)cs.bg);

	/* Block-level browser defaults. */
	if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) W_size = (unsigned char)(name[1] - '0');
	if (streq(name, "center") || cs.center > 0) W_center = 1;
	if (streq(name, "pre")) { W_mono = 1; W_pre = 1; }
	if (streq(name, "blockquote")) W_indent = (unsigned char)(W_indent < 7 ? W_indent + 1 : 7);
	if (streq(name, "ul")) { W_indent = (unsigned char)(W_indent < 7 ? W_indent + 1 : 7); W_listtype = 'u'; }
	if (streq(name, "ol")) { W_indent = (unsigned char)(W_indent < 7 ? W_indent + 1 : 7); W_listtype = 'o'; W_olnum = 0; }

	int blk = (cs.disp == CSS_DISP_BLOCK) ? 1 :
	          (cs.disp == CSS_DISP_INLINE) ? 0 : is_block_tag(name);

	if (blk) block_break();
	if (streq(name, "li")) {                                     /* list marker  */
		unsigned char a = sty_byte();
		if (W_listtype == 'o') {
			char num[16]; int k = 0, v = ++W_olnum;
			char d[8]; int z = 0; if (v == 0) d[z++] = '0'; while (v) { d[z++] = (char)('0' + v % 10); v /= 10; }
			while (z) num[k++] = d[--z]; num[k++] = '.'; num[k++] = ' '; num[k] = '\0';
			emit_lit(num, a);
		} else {
			emit_lit("\xe2\x80\xa2 ", a);                       /* bullet "• "  */
		}
	}
	walk(el);
	if (blk) block_break();

	if (linked) close_link();
	sty_cur = saved;
	W_size = sv_size; W_center = sv_center; W_mono = sv_mono;
	W_indent = sv_indent; W_pre = sv_pre; W_bg = sv_bg;
	W_listtype = sv_lt; W_olnum = sv_ol;
}

static void walk(struct dom_node *node)
{
	for (struct dom_node *c = node->first_child; c; c = c->next) {
		if (c->type == DOM_TEXT) { if (c->text) emit_text(c->text); }
		else                     walk_element(c);
	}
}

/* Stylesheets are collected as an ORDERED list of sources first, then external
 * ones are batch-fetched (same-origin multiplexed over h2), then all are added in
 * document order — so the cascade order is preserved AND the network round-trips
 * are batched rather than one blocking fetch per <link>. CSS_FETCH_MAX bounds the
 * external fetches; CSS_SRC_MAX the total <style>/<link> sources. */
#define CSS_FETCH_MAX 24
#define CSS_SRC_MAX   64
struct css_src { const char *text; int len; int url_i; };  /* text -> inline; else url_i */
static struct css_src g_src[CSS_SRC_MAX];
static int   g_nsrc;
static char *g_css_url[CSS_FETCH_MAX];
static int   g_n_css_url;
static const char *g_page_url;

static void src_inline(const char *text, int len)
{
	if (g_nsrc < CSS_SRC_MAX) {
		g_src[g_nsrc].text = text; g_src[g_nsrc].len = len; g_src[g_nsrc].url_i = -1;
		g_nsrc++;
	}
}

static void src_link(const char *absurl)
{
	if (g_nsrc >= CSS_SRC_MAX || g_n_css_url >= CSS_FETCH_MAX) return;
	int L = strlen(absurl);
	char *u = malloc((unsigned)L + 1);
	if (!u) return;
	for (int i = 0; i <= L; i++) u[i] = absurl[i];
	g_src[g_nsrc].text = 0; g_src[g_nsrc].len = 0; g_src[g_nsrc].url_i = g_n_css_url;
	g_nsrc++;
	g_css_url[g_n_css_url++] = u;
}

/* Record stylesheet sources in document order: inline <style> text, and external
 * <link rel=stylesheet> URLs (resolved against the page URL). */
static void collect_styles(struct dom_node *node)
{
	for (struct dom_node *c = node->first_child; c; c = c->next) {
		if (c->type != DOM_ELEMENT) continue;
		if (streq(c->tag, "style")) {
			for (struct dom_node *t = c->first_child; t; t = t->next)
				if (t->type == DOM_TEXT && t->text)
					src_inline(t->text, strlen(t->text));
		} else if (streq(c->tag, "link")) {
			const char *rel = dom_get_attr(c, "rel");
			const char *href = dom_get_attr(c, "href");
			if (rel && href && href[0] && contains_ci(rel, "stylesheet") &&
			    !contains_ci(rel, "alternate")) {
				char abs[URL_MAX];
				resolve_url(g_page_url, href, abs, sizeof abs);
				src_link(abs);
			}
		} else {
			collect_styles(c);
		}
	}
}

/* First element with this tag name anywhere in the subtree (pre-order). */
static struct dom_node *find_tag(struct dom_node *node, const char *tag)
{
	for (struct dom_node *c = node->first_child; c; c = c->next) {
		if (c->type != DOM_ELEMENT) continue;
		if (streq(c->tag, tag)) return c;
		struct dom_node *r = find_tag(c, tag);
		if (r) return r;
	}
	return 0;
}

/* Generic font-family keywords that resolve to the default proportional UI font
 * (we have no serif/cursive face, so they all map to the system sans). */
static int fam_generic_default(const char *n)
{
	return streq(n, "sans-serif") || streq(n, "serif") || streq(n, "system-ui") ||
	       streq(n, "ui-sans-serif") || streq(n, "ui-serif") || streq(n, "ui-rounded") ||
	       streq(n, "cursive") || streq(n, "fantasy") || streq(n, "-apple-system") ||
	       streq(n, "blinkmacsystemfont");
}
static int fam_monospace(const char *n)
{
	return streq(n, "monospace") || streq(n, "ui-monospace");
}

/* Load a TTF from a local file into `slot` (so it's freed with the other faces).
 * Used to back the `monospace` generic with the shipped Liberation Mono. */
static struct font *load_face_file(const char *path, struct loaded_face *slot)
{
	struct statinfo st;
	if (stat(path, &st) < 0 || st.is_dir || st.size == 0) return 0;
	unsigned char *buf = malloc(st.size);
	if (!buf) return 0;
	if (readfile_at(path, 0, buf, st.size) != (int)st.size) { free(buf); return 0; }
	struct font *f = font_load(buf, (int)st.size);
	if (!f) { free(buf); return 0; }
	slot->data = buf; slot->font = f; slot->family[0] = '\0';
	return f;
}

/* Load the sheet's @font-face faces, then pick the one the <body> text uses by
 * walking its font-family stack (first available wins): an @font-face match, then
 * a generic (sans-serif/system-ui/... -> default UI font; monospace -> Liberation
 * Mono), else the next fallback. The faces' src URLs are resolved against the
 * page, batch-fetched (multiplexed like the stylesheets), and parsed by
 * user/lib/font; the kept face owns its file bytes (the parser points into them),
 * so unloaded bodies are freed. The result is swapped into B under blk_lock
 * because render() reads B->body_font and B->faces on the main thread while this
 * runs on the fetch worker. */
#define FONT_FETCH_MAX 4
static void load_faces(struct dom_node *root, const char *page_url, res_batch_fn fetch)
{
	/* Resolve up to FONT_FETCH_MAX face URLs, remembering each face's family. */
	char  *urls[FONT_FETCH_MAX];
	char   fams[FONT_FETCH_MAX][CSS_FAM_MAX];
	int    nu = 0;

	int nfaces = css_fontface_count(g_sheet);
	for (int i = 0; i < nfaces && nu < FONT_FETCH_MAX; i++) {
		const char *family = 0, *url = 0;
		css_fontface_at(g_sheet, i, &family, &url);
		if (!family || !family[0] || !url || !url[0]) continue;

		char abs[URL_MAX];
		resolve_url(page_url, url, abs, sizeof abs);
		int L = strlen(abs);
		char *u = malloc((unsigned)L + 1);
		if (!u) continue;
		for (int k = 0; k <= L; k++) u[k] = abs[k];

		int j = 0;
		for (; family[j] && j < CSS_FAM_MAX - 1; j++) fams[nu][j] = family[j];
		fams[nu][j] = '\0';
		urls[nu++] = u;
	}

	/* Batch-fetch the font files (multiplexed over h2 same-origin), then parse. */
	struct loaded_face nf[FONT_FETCH_MAX];
	int nn = 0;
	if (nu > 0) {
		unsigned char *bodies[FONT_FETCH_MAX]; int lens[FONT_FETCH_MAX];
		if (fetch) fetch(urls, nu, bodies, lens);
		else for (int i = 0; i < nu; i++) { bodies[i] = 0; lens[i] = 0; }

		for (int i = 0; i < nu; i++) {
			unsigned char *body = bodies[i];
			if (!body || lens[i] <= 0) { free(body); continue; }
			/* font_load transparently decodes WOFF/WOFF2 containers to SFNT. */
			struct font *f = font_load(body, lens[i]);
			if (!f) { free(body); continue; }   /* unsupported (e.g. pure CFF) */
			int j = 0;
			for (; fams[i][j] && j < (int)sizeof nf[nn].family - 1; j++)
				nf[nn].family[j] = fams[i][j];
			nf[nn].family[j] = '\0';
			nf[nn].font = f;
			nf[nn].data = body;
			nn++;
		}
	}
	for (int i = 0; i < nu; i++) free(urls[i]);

	/* Walk the <body> font-family stack and pick the first available family. */
	struct font *chosen = 0;
	struct dom_node *body_el = find_tag(root, "body");
	if (body_el) {
		struct css_style cs;
		compute_style(body_el, &cs);
		const char *p = cs.fam;
		while (*p) {
			char name[CSS_FAM_MAX]; int n = 0;
			while (*p && *p != ',' && n < (int)sizeof name - 1) name[n++] = *p++;
			name[n] = '\0';
			while (*p && *p != ',') p++;
			if (*p == ',') p++;
			if (!name[0]) continue;

			int hit = 0;                                  /* a loaded @font-face? */
			for (int i = 0; i < nn; i++)
				if (streq(nf[i].family, name)) { chosen = nf[i].font; hit = 1; break; }
			if (hit) break;
			if (fam_generic_default(name)) { chosen = 0; break; }   /* default UI sans */
			if (fam_monospace(name) && nn < FONT_FETCH_MAX) {
				struct font *m = load_face_file("/etc/fonts/LiberationMono-Regular.ttf", &nf[nn]);
				if (m) { chosen = m; nn++; break; }
			}
			/* a named family we don't have -> fall through to the next entry */
		}
	}

	/* Publish: drop the previous page's faces, install the new set. */
	blk_lock();
	for (int i = 0; i < B->n_faces; i++) {
		if (B->faces[i].font) font_free(B->faces[i].font);
		free(B->faces[i].data);
	}
	for (int i = 0; i < nn; i++) B->faces[i] = nf[i];
	B->n_faces = nn;
	B->body_font = chosen;
	blk_unlock();
}

void html_to_text(const char *page_url, res_batch_fn fetch)
{
	B->doc_title[0] = '\0';
	B->nlinks = 0;
	B->n_imgs = 0;
	B->img_pool_len = 0;
	B->favicon_href[0] = '\0';
	B->favicon_pri = 0;

	sty_cur.fg = sty_cur.bold = sty_cur.under = 0;
	W_n = 0; W_pend_sp = 0; W_run_nl = 1; W_cur_link = -1;

	g_page_url = page_url ? page_url : "";
	g_nsrc = 0;
	g_n_css_url = 0;

	detect_meta_charset();          /* header charset (if any) already set by fetch.c */

	struct dom_node *root = dom_parse(B->raw, B->raw_len);
	g_sheet = css_new();
	if (root) {
		/* Link:-header sheets come before the document's own (then tree order). */
		for (struct css_link *L = B->ext_css; L; L = L->next) src_link(L->url);
		collect_styles(root);

		/* Batch-fetch the external sheets, then add every source in order. */
		unsigned char *bodies[CSS_FETCH_MAX]; int lens[CSS_FETCH_MAX];
		if (fetch && g_n_css_url > 0) fetch(g_css_url, g_n_css_url, bodies, lens);
		else for (int i = 0; i < g_n_css_url; i++) { bodies[i] = 0; lens[i] = 0; }

		for (int s = 0; s < g_nsrc; s++) {
			if (g_src[s].text) {
				css_add(g_sheet, g_src[s].text, g_src[s].len);
			} else {
				int ui = g_src[s].url_i;
				if (ui >= 0 && bodies[ui]) css_add(g_sheet, (char *)bodies[ui], lens[ui]);
			}
		}
		for (int i = 0; i < g_n_css_url; i++) { free(g_css_url[i]); free(bodies[i]); }

		/* @font-face: load declared faces and pick the body font (before walk so
		 * the renderer sees B->body_font for this page's text). */
		load_faces(root, g_page_url, fetch);

		walk(root);
		dom_free(root);
	}
	css_free(g_sheet);
	g_sheet = 0;
	B->text_len = W_n;
}

/* Render a non-HTML text body verbatim into the styled-text buffer: newlines
 * are kept (the word-wrapper honours them), tabs expand to the next 8-column
 * stop, and other control bytes are dropped. This is what fixes plain `.txt`
 * pages, whose line breaks the HTML reducer would otherwise collapse. */
void plain_to_text(void)
{
	const char *p = B->raw, *pend = B->raw + B->raw_len;
	int n = 0, col = 0;

	B->doc_title[0] = '\0';
	B->nlinks = 0;
	B->n_imgs = 0;

	while (p < pend) {
		unsigned char c = (unsigned char)*p;
		if (c == '\r') { p++; continue; }
		if (c == '\n') { n = text_put(n, '\n', 0); col = 0; p++; continue; }
		if (c == '\t') {
			int sp = 8 - (col & 7);
			while (sp-- > 0) { n = text_put(n, ' ', 0); col++; }
			p++;
			continue;
		}
		if (c < 0x80) {                                   /* ASCII printable     */
			if (c < 0x20) { p++; continue; }
			n = text_put(n, (char)c, 0); col++; p++;
		} else {                                          /* transcode to UTF-8  */
			unsigned cp = (unsigned)read_char(&p, pend);
			if (cp_drop(cp)) continue;
			n = utf8_emit(n, cp, 0);
			col++;
		}
	}
	B->text_len = n;
}

/* Fill the styled-text buffer with a literal one-off message (e.g. "unsupported
 * content type"), so render() shows it through the normal text path. */
void doc_message(const char *m)
{
	int n = 0;
	while (m[n]) {
		if (!ensure_text(n + 1)) break;
		B->text[n] = m[n]; B->attr[n] = 0; n++;
	}
	B->text_len = n;
	B->doc_title[0] = '\0';
	B->nlinks = 0;
	B->n_imgs = 0;
}
