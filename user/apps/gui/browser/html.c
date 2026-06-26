/* html.c — the renderer. Walks the DOM (parsed by dom.c) into styled text in
 * B->pg->text/B->pg->attr, collecting links, inline images, the favicon and title.
 * Element styling comes from the css.c engine (sheet + inline + attrs); this file
 * maps the computed style onto the text renderer and does the HTML-entity work. */
#include "browser.h"
#include "css.h"
#include "font.h"
#include "svg.h"
#include "layout.h"

static css_sheet *g_sheet;     /* the page's collected <style> rules, per render */

/* The DOM + CSS sheet now persist for the whole page lifetime (the JS context in
 * js.c does too), so event listeners can fire and mutate the tree after load.
 * html_page_teardown() frees all three on the next navigation; html_relayout()
 * re-flattens the live tree into B's buffers after a handler changes it. */
static struct dom_node *g_live_dom;
static char g_cur_page_url[URL_MAX];

/* Base URL the walk resolves relative links/sources against. Normally the page
 * URL; swapped to a frame's own src while its sub-document is being flattened, so
 * a relative <a>/<img> inside an iframe resolves against the frame, not the top
 * page. g_frame_depth is the iframe nesting depth (0 = top document); g_link_nav
 * is 0 while inside a sandboxed frame that may not navigate, so its links render
 * as plain (non-clickable) text. */
static char g_base_url[URL_MAX];
static int  g_frame_depth;
static int  g_link_nav = 1;

/* CSP enforcement runs against the top document (B->pg->csp); frame sub-documents
 * have their own policy we don't track, so checks are skipped inside frames
 * (g_frame_depth > 0). g_csp_report gates console logging to the initial load so
 * the per-relayout walk doesn't spam duplicate violations. */
static int g_csp_report;

static void csp_report(const char *what, const char *url, const char *reason)
{
	if (!g_csp_report) return;
	char m[URL_MAX + 160]; int n = 0;
	sapp(m, &n, sizeof m, "[CSP] refused to load ");
	sapp(m, &n, sizeof m, what);
	sapp(m, &n, sizeof m, " ");
	sapp(m, &n, sizeof m, url);
	if (reason && reason[0]) { sapp(m, &n, sizeof m, " ("); sapp(m, &n, sizeof m, reason); sapp(m, &n, sizeof m, ")"); }
	sapp(m, &n, sizeof m, "\n");
	devtools_console_append(DT_ERROR, m, n);
}

/* CSP gate for a sub-resource URL load (img/style/font/frame). `abs` is already
 * absolute; on success `out` holds the URL to use (http->https upgraded when the
 * policy asks). Returns 1 = allowed. No-op (allow) inside frames or with no CSP. */
static int csp_url_ok(int type, const char *abs, const char *what, char *out, int outcap)
{
	if (g_frame_depth > 0 || !B->pg->csp) {
		int k = 0; out[0] = '\0'; while (abs[k] && k < outcap - 1) { out[k] = abs[k]; k++; } out[k] = '\0';
		return 1;
	}
	char reason[64];
	if (csp_check_url(B->pg->csp, type, abs, g_cur_page_url, out, outcap, reason, sizeof reason)) return 1;
	csp_report(what, abs, reason);
	return 0;
}

/* style-src gate for an inline style="" attribute (no nonce/hash for attrs). */
static int csp_style_attr_ok(void)
{
	if (g_frame_depth > 0 || !B->pg->csp) return 1;
	return csp_check_inline(B->pg->csp, CSP_STYLE, 0, 0, 0, 0, 0);
}

/* Computed style for one element: presentational attrs (lowest), sheet rules,
 * then inline style="" (highest). */
static void compute_style(struct dom_node *el, struct css_style *cs)
{
	css_style_reset(cs);
	css_apply_attrs(el, cs);
	css_apply_sheet(g_sheet, el, cs);
	const char *inl = dom_get_attr(el, "style");
	if (inl && csp_style_attr_ok()) css_apply_inline(inl, cs);
}

/* devtools hooks: the live DOM root and a public wrapper over compute_style, so
 * the developer-tools panel can walk the tree and read computed styles without
 * reaching into html.c's statics. Worker-thread callers only (the DOM's owner). */
struct dom_node *html_dom_root(void) { return g_live_dom; }
void html_compute_style(struct dom_node *el, struct css_style *out) { compute_style(el, out); }
int html_match_styles(struct dom_node *el, struct css_match *out, int cap)
{ return css_matches(g_sheet, el, out, cap); }
/* 1 if `el` has a ::before/::after (CSS_PSEUDO_*) with content — for devtools. */
int html_has_pseudo(struct dom_node *el, int which)
{ struct css_style cs; return css_pseudo_content(g_sheet, el, which, &cs); }

/* True when `s` already carries a URL scheme that resolve_url won't expand, so it
 * must be used verbatim rather than joined onto a base (resolve_url only treats
 * http(s):// as absolute, and would mangle e.g. a data:/file: reference). */
static int url_absolute(const char *s)
{
	return starts_ci(s, "http://") || starts_ci(s, "https://") || starts_ci(s, "data:") ||
	       starts_ci(s, "file:")   || starts_ci(s, "blob:")    || starts_ci(s, "mailto:");
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
	const char *p = B->pg->raw, *end = B->pg->raw + B->pg->raw_len;
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
 * call stack), so style nesting needs no explicit stack. fg is a full 0xRRGGBB. */
struct sty { unsigned fg; unsigned char bold, under; };
static struct sty sty_cur;

static unsigned char sty_byte(void)
{
	unsigned char a = 0;
	if (sty_cur.bold)  a |= ST_BOLD;
	if (sty_cur.under) a |= ST_UNDER;
	return a;
}

/* User-agent default style for a tag (the lowest layer; author CSS overrides).
 * The colour substitutes (italic grey, link blue, code brown, mark red) stand in
 * for renderings the engine can't otherwise show; headings inherit their colour
 * (so they stay readable on a dark page) and carry weight via bold. */
static void apply_ua_style(const char *name)
{
	if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
		sty_cur.bold = 1;
	} else if (streq(name, "b") || streq(name, "strong")) {
		sty_cur.bold = 1;
	} else if (streq(name, "i") || streq(name, "em")) {
		sty_cur.fg = 0x586070;
	} else if (streq(name, "a")) {
		sty_cur.fg = 0x1a4fbf; sty_cur.under = 1;
	} else if (streq(name, "u") || streq(name, "ins")) {
		sty_cur.under = 1;
	} else if (streq(name, "code") || streq(name, "kbd") ||
	           streq(name, "samp") || streq(name, "tt")) {
		sty_cur.fg = 0x9a6020;
	} else if (streq(name, "mark")) {
		sty_cur.fg = 0xc01818;
	}
}

/* Layer a computed CSS style over the UA defaults already in sty_cur. */
static void apply_css(const struct css_style *cs)
{
	if (cs->color >= 0) sty_cur.fg = (unsigned)cs->color;
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
	while (p + n < end && n < 32 && p[n] != ';' && p[n] != '&' &&
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
		if (v == 0) return -2;                                  /* NULL -> drop      */
		if (v >= 0x80 && v <= 0x9F)                             /* HTML5: C1 numeric */
			return cp1252_high((unsigned char)v);              /* refs are cp1252   */
		if (v == 0xA0) return -1;                               /* nbsp -> coll. sp. */
		if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
			return 0xFFFD;                                      /* invalid -> U+FFFD */
		return (int)v;
	}
	/* Named entities -> their real Unicode codepoints (the renderer is UTF-8). A
	 * practical subset of the HTML5 named character references: the markup
	 * essentials, the full Latin-1 block, General Punctuation, common math/arrow/
	 * Greek/letterlike symbols, card suits and currency. nbsp maps to the -1
	 * collapsible-space sentinel. (The rare two-codepoint references are omitted —
	 * every entry expands to one codepoint, which keeps the in-place rewrite in
	 * decode_entities safe.) */
	static const struct { const char *name; int ch; } ent[] = {
		/* Markup essentials */
		{ "amp", '&' }, { "AMP", '&' }, { "lt", '<' }, { "LT", '<' },
		{ "gt", '>' }, { "GT", '>' }, { "quot", '"' }, { "QUOT", '"' },
		{ "apos", '\'' },
		/* Spaces and joiners */
		{ "nbsp", -1 }, { "NonBreakingSpace", -1 }, { "ensp", 0x2002 },
		{ "emsp", 0x2003 }, { "emsp13", 0x2004 }, { "emsp14", 0x2005 },
		{ "numsp", 0x2007 }, { "puncsp", 0x2008 }, { "thinsp", 0x2009 },
		{ "ThinSpace", 0x2009 }, { "hairsp", 0x200A }, { "VeryThinSpace", 0x200A },
		{ "zwnj", 0x200C }, { "zwj", 0x200D }, { "lrm", 0x200E }, { "rlm", 0x200F },
		{ "shy", 0xAD },
		/* General punctuation */
		{ "ndash", 0x2013 }, { "mdash", 0x2014 }, { "horbar", 0x2015 },
		{ "hellip", 0x2026 }, { "mldr", 0x2026 }, { "nldr", 0x2025 },
		{ "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "rsquor", 0x2019 },
		{ "lsquor", 0x201A }, { "sbquo", 0x201A }, { "ldquo", 0x201C },
		{ "rdquo", 0x201D }, { "rdquor", 0x201D }, { "ldquor", 0x201E },
		{ "bdquo", 0x201E }, { "dagger", 0x2020 }, { "Dagger", 0x2021 },
		{ "ddagger", 0x2021 }, { "bull", 0x2022 }, { "bullet", 0x2022 },
		{ "tridot", 0x25EC }, { "prime", 0x2032 }, { "Prime", 0x2033 },
		{ "tprime", 0x2034 }, { "bprime", 0x2035 }, { "backprime", 0x2035 },
		{ "lsaquo", 0x2039 }, { "rsaquo", 0x203A }, { "oline", 0x203E },
		{ "OverBar", 0x203E }, { "caret", 0x2041 }, { "hybull", 0x2043 },
		{ "frasl", 0x2044 }, { "permil", 0x2030 }, { "pertenk", 0x2031 },
		{ "lowbar", '_' }, { "verbar", '|' }, { "vert", '|' }, { "Verbar", 0x2016 },
		{ "commat", '@' }, { "num", '#' }, { "percnt", '%' }, { "ast", '*' },
		{ "midast", '*' }, { "plus", '+' }, { "sol", '/' }, { "bsol", '\\' },
		{ "colon", ':' }, { "semi", ';' }, { "quest", '?' }, { "excl", '!' },
		{ "period", '.' }, { "comma", ',' }, { "lpar", '(' }, { "rpar", ')' },
		{ "lbrack", '[' }, { "lsqb", '[' }, { "rbrack", ']' }, { "rsqb", ']' },
		{ "lbrace", '{' }, { "lcub", '{' }, { "rbrace", '}' }, { "rcub", '}' },
		{ "grave", '`' }, { "DiacriticalGrave", '`' }, { "Hat", '^' },
		{ "dollar", '$' }, { "equals", '=' },
		/* Latin-1 symbols */
		{ "iexcl", 0xA1 }, { "cent", 0xA2 }, { "pound", 0xA3 }, { "curren", 0xA4 },
		{ "yen", 0xA5 }, { "brvbar", 0xA6 }, { "sect", 0xA7 }, { "uml", 0xA8 },
		{ "die", 0xA8 }, { "Dot", 0xA8 }, { "copy", 0xA9 }, { "ordf", 0xAA },
		{ "laquo", 0xAB }, { "not", 0xAC }, { "reg", 0xAE }, { "circledR", 0xAE },
		{ "macr", 0xAF }, { "strns", 0xAF }, { "deg", 0xB0 }, { "plusmn", 0xB1 },
		{ "pm", 0xB1 }, { "PlusMinus", 0xB1 }, { "sup2", 0xB2 }, { "sup3", 0xB3 },
		{ "acute", 0xB4 }, { "DiacriticalAcute", 0xB4 }, { "micro", 0xB5 },
		{ "para", 0xB6 }, { "middot", 0xB7 }, { "centerdot", 0xB7 },
		{ "CenterDot", 0xB7 }, { "cedil", 0xB8 }, { "Cedilla", 0xB8 },
		{ "sup1", 0xB9 }, { "ordm", 0xBA }, { "raquo", 0xBB }, { "frac14", 0xBC },
		{ "frac12", 0xBD }, { "half", 0xBD }, { "frac34", 0xBE }, { "iquest", 0xBF },
		{ "times", 0xD7 }, { "divide", 0xF7 }, { "div", 0xF7 },
		/* Latin-1 letters */
		{ "Agrave", 0xC0 }, { "Aacute", 0xC1 }, { "Acirc", 0xC2 }, { "Atilde", 0xC3 },
		{ "Auml", 0xC4 }, { "Aring", 0xC5 }, { "AElig", 0xC6 }, { "Ccedil", 0xC7 },
		{ "Egrave", 0xC8 }, { "Eacute", 0xC9 }, { "Ecirc", 0xCA }, { "Euml", 0xCB },
		{ "Igrave", 0xCC }, { "Iacute", 0xCD }, { "Icirc", 0xCE }, { "Iuml", 0xCF },
		{ "ETH", 0xD0 }, { "Ntilde", 0xD1 }, { "Ograve", 0xD2 }, { "Oacute", 0xD3 },
		{ "Ocirc", 0xD4 }, { "Otilde", 0xD5 }, { "Ouml", 0xD6 }, { "Oslash", 0xD8 },
		{ "Ugrave", 0xD9 }, { "Uacute", 0xDA }, { "Ucirc", 0xDB }, { "Uuml", 0xDC },
		{ "Yacute", 0xDD }, { "THORN", 0xDE }, { "szlig", 0xDF },
		{ "agrave", 0xE0 }, { "aacute", 0xE1 }, { "acirc", 0xE2 }, { "atilde", 0xE3 },
		{ "auml", 0xE4 }, { "aring", 0xE5 }, { "aelig", 0xE6 }, { "ccedil", 0xE7 },
		{ "egrave", 0xE8 }, { "eacute", 0xE9 }, { "ecirc", 0xEA }, { "euml", 0xEB },
		{ "igrave", 0xEC }, { "iacute", 0xED }, { "icirc", 0xEE }, { "iuml", 0xEF },
		{ "eth", 0xF0 }, { "ntilde", 0xF1 }, { "ograve", 0xF2 }, { "oacute", 0xF3 },
		{ "ocirc", 0xF4 }, { "otilde", 0xF5 }, { "ouml", 0xF6 }, { "oslash", 0xF8 },
		{ "ugrave", 0xF9 }, { "uacute", 0xFA }, { "ucirc", 0xFB }, { "uuml", 0xFC },
		{ "yacute", 0xFD }, { "thorn", 0xFE }, { "yuml", 0xFF },
		/* Latin Extended-A: ligatures and accented letters in common use */
		{ "OElig", 0x152 }, { "oelig", 0x153 }, { "Scaron", 0x160 },
		{ "scaron", 0x161 }, { "Yuml", 0x178 }, { "fnof", 0x192 },
		{ "circ", 0x2C6 }, { "tilde", 0x2DC }, { "DiacriticalTilde", 0x2DC },
		/* Letterlike symbols */
		{ "trade", 0x2122 }, { "TRADE", 0x2122 }, { "weierp", 0x2118 },
		{ "wp", 0x2118 }, { "image", 0x2111 }, { "Im", 0x2111 },
		{ "real", 0x211C }, { "Re", 0x211C }, { "alefsym", 0x2135 },
		{ "aleph", 0x2135 }, { "ell", 0x2113 }, { "naturals", 0x2115 },
		{ "Nopf", 0x2115 }, { "Ropf", 0x211D }, { "Zopf", 0x2124 },
		{ "Qopf", 0x211A }, { "numero", 0x2116 }, { "copysr", 0x2117 },
		{ "rx", 0x211E },
		/* Arrows */
		{ "larr", 0x2190 }, { "leftarrow", 0x2190 }, { "uarr", 0x2191 },
		{ "uparrow", 0x2191 }, { "rarr", 0x2192 }, { "rightarrow", 0x2192 },
		{ "darr", 0x2193 }, { "downarrow", 0x2193 }, { "harr", 0x2194 },
		{ "leftrightarrow", 0x2194 }, { "varr", 0x2195 }, { "crarr", 0x21B5 },
		{ "lArr", 0x21D0 }, { "uArr", 0x21D1 }, { "rArr", 0x21D2 },
		{ "Rightarrow", 0x21D2 }, { "dArr", 0x21D3 }, { "hArr", 0x21D4 },
		{ "Leftrightarrow", 0x21D4 },
		/* Mathematical operators */
		{ "forall", 0x2200 }, { "part", 0x2202 }, { "exist", 0x2203 },
		{ "empty", 0x2205 }, { "emptyset", 0x2205 }, { "nabla", 0x2207 },
		{ "isin", 0x2208 }, { "in", 0x2208 }, { "notin", 0x2209 },
		{ "ni", 0x220B }, { "prod", 0x220F }, { "sum", 0x2211 },
		{ "minus", 0x2212 }, { "lowast", 0x2217 }, { "radic", 0x221A },
		{ "Sqrt", 0x221A }, { "prop", 0x221D }, { "propto", 0x221D },
		{ "infin", 0x221E }, { "ang", 0x2220 }, { "angle", 0x2220 },
		{ "and", 0x2227 }, { "wedge", 0x2227 }, { "or", 0x2228 }, { "vee", 0x2228 },
		{ "cap", 0x2229 }, { "cup", 0x222A }, { "int", 0x222B }, { "Integral", 0x222B },
		{ "there4", 0x2234 }, { "therefore", 0x2234 }, { "becaus", 0x2235 },
		{ "sim", 0x223C }, { "cong", 0x2245 }, { "asymp", 0x2248 },
		{ "ne", 0x2260 }, { "equiv", 0x2261 }, { "le", 0x2264 }, { "leq", 0x2264 },
		{ "ge", 0x2265 }, { "geq", 0x2265 }, { "sub", 0x2282 }, { "subset", 0x2282 },
		{ "sup", 0x2283 }, { "supset", 0x2283 }, { "nsub", 0x2284 },
		{ "sube", 0x2286 }, { "subseteq", 0x2286 }, { "supe", 0x2287 },
		{ "supseteq", 0x2287 }, { "oplus", 0x2295 }, { "otimes", 0x2297 },
		{ "perp", 0x22A5 }, { "sdot", 0x22C5 },
		/* Technical / misc symbols */
		{ "lceil", 0x2308 }, { "rceil", 0x2309 }, { "lfloor", 0x230A },
		{ "rfloor", 0x230B }, { "lang", 0x27E8 }, { "langle", 0x27E8 },
		{ "rang", 0x27E9 }, { "rangle", 0x27E9 }, { "loz", 0x25CA },
		{ "lozenge", 0x25CA }, { "spades", 0x2660 }, { "spadesuit", 0x2660 },
		{ "clubs", 0x2663 }, { "clubsuit", 0x2663 }, { "hearts", 0x2665 },
		{ "heartsuit", 0x2665 }, { "diams", 0x2666 }, { "diamondsuit", 0x2666 },
		{ "star", 0x2606 }, { "starf", 0x2605 }, { "bigstar", 0x2605 },
		{ "phone", 0x260E }, { "female", 0x2640 }, { "male", 0x2642 },
		{ "check", 0x2713 }, { "checkmark", 0x2713 }, { "cross", 0x2717 },
		{ "sung", 0x266A }, { "flat", 0x266D }, { "natur", 0x266E }, { "sharp", 0x266F },
		/* Currency */
		{ "euro", 0x20AC },
		/* Greek lowercase */
		{ "alpha", 0x3B1 }, { "beta", 0x3B2 }, { "gamma", 0x3B3 }, { "delta", 0x3B4 },
		{ "epsilon", 0x3B5 }, { "epsiv", 0x3B5 }, { "zeta", 0x3B6 }, { "eta", 0x3B7 },
		{ "theta", 0x3B8 }, { "iota", 0x3B9 }, { "kappa", 0x3BA }, { "lambda", 0x3BB },
		{ "mu", 0x3BC }, { "nu", 0x3BD }, { "xi", 0x3BE }, { "omicron", 0x3BF },
		{ "pi", 0x3C0 }, { "rho", 0x3C1 }, { "sigmaf", 0x3C2 }, { "sigmav", 0x3C2 },
		{ "sigma", 0x3C3 }, { "tau", 0x3C4 }, { "upsilon", 0x3C5 }, { "upsi", 0x3C5 },
		{ "phi", 0x3C6 }, { "chi", 0x3C7 }, { "psi", 0x3C8 }, { "omega", 0x3C9 },
		{ "thetasym", 0x3D1 }, { "thetav", 0x3D1 }, { "upsih", 0x3D2 }, { "piv", 0x3D6 },
		/* Greek uppercase */
		{ "Alpha", 0x391 }, { "Beta", 0x392 }, { "Gamma", 0x393 }, { "Delta", 0x394 },
		{ "Epsilon", 0x395 }, { "Zeta", 0x396 }, { "Eta", 0x397 }, { "Theta", 0x398 },
		{ "Iota", 0x399 }, { "Kappa", 0x39A }, { "Lambda", 0x39B }, { "Mu", 0x39C },
		{ "Nu", 0x39D }, { "Xi", 0x39E }, { "Omicron", 0x39F }, { "Pi", 0x3A0 },
		{ "Rho", 0x3A1 }, { "Sigma", 0x3A3 }, { "Tau", 0x3A4 }, { "Upsilon", 0x3A5 },
		{ "Phi", 0x3A6 }, { "Chi", 0x3A7 }, { "Psi", 0x3A8 }, { "Omega", 0x3A9 },
		{ 0, 0 }
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
static unsigned W_bg = BG_NONE;     /* current background 0xRRGGBB, BG_NONE = none */
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
	B->pg->text[n] = c;
	B->pg->attr[n] = a;
	B->pg->bp[n]   = block_byte();
	B->pg->fg[n]   = sty_cur.fg;
	B->pg->bg[n]   = W_bg;
	return n + 1;
}

/* Walk state (single-threaded: only the worker calls in). W_n = rendered length;
 * pend_sp/run_nl drive whitespace collapsing; W_cur_link = open <a>, or -1. */
static int W_n, W_pend_sp, W_run_nl, W_cur_link;

/* Element span table: every element records the [start,end) text range it covers,
 * so a content-pane click resolves to the deepest enclosing DOM node (the click
 * target for event dispatch). Built during the walk, read by html_node_at_offset.
 * Worker-thread-only — both writer (walk) and reader (dispatch) run there. */
struct domspan { int start, end; struct dom_node *node; };
static struct domspan *g_spans;
static int g_nspans, g_spans_cap;

static int span_begin(struct dom_node *el) {
    if (g_nspans >= g_spans_cap) {
        int nc = g_spans_cap ? g_spans_cap * 2 : 256;
        struct domspan *p = realloc(g_spans, (unsigned)nc * sizeof *p);
        if (!p) return -1;
        g_spans = p; g_spans_cap = nc;
    }
    int i = g_nspans++;
    g_spans[i].start = W_n; g_spans[i].end = W_n; g_spans[i].node = el;
    return i;
}
static void span_end(int i) { if (i >= 0 && i < g_nspans) g_spans[i].end = W_n; }

struct dom_node *html_node_at_offset(int off) {
    struct dom_node *best = 0;
    int bestw = 1 << 30;
    for (int i = 0; i < g_nspans; i++) {
        if (off >= g_spans[i].start && off < g_spans[i].end) {
            int w = g_spans[i].end - g_spans[i].start;
            if (w <= bestw) { bestw = w; best = g_spans[i].node; }  /* deepest = narrowest, last wins */
        }
    }
    return best;
}

/* Inverse of html_node_at_offset: the [start,end) text range a node covers (its
 * one span from the last walk). 0 if the node was never laid out — detached, or a
 * head/script-only node that emits no text. Worker-thread-only (the walk's owner). */
int html_node_span(struct dom_node *node, int *start, int *end) {
    for (int i = 0; i < g_nspans; i++)
        if (g_spans[i].node == node) {
            if (start) *start = g_spans[i].start;
            if (end)   *end   = g_spans[i].end;
            return 1;
        }
    return 0;
}

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
	B->pg->doc_title[0] = '\0';
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
			if (t_pend && tl > 0 && tl < (int)sizeof B->pg->doc_title - 1)
				B->pg->doc_title[tl++] = ' ';
			t_pend = 0;
			if (tl < (int)sizeof B->pg->doc_title - 1) B->pg->doc_title[tl++] = (char)ch;
			B->pg->doc_title[tl] = '\0';
		}
	}
}

/* Start recording an <a>: returns 1 if the href is navigable (caller closes it),
 * 0 for empty / fragment / javascript: / mailto:. */
static int open_link(struct dom_node *el)
{
	if (!g_link_nav) return 0;        /* sandboxed frame: link isn't navigable */
	if (!ensure_links(B->pg->nlinks + 1)) return 0;
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
	struct link *L = &B->pg->links[B->pg->nlinks];
	L->start = W_n;
	/* Inside a frame, store the absolute href (resolved against the frame's own
	 * URL): the click handler resolves L->href against the top page, so a frame's
	 * relative link would otherwise resolve against the wrong base. */
	char abs[URL_MAX];
	const char *put = buf;
	if (g_frame_depth > 0 && !url_absolute(buf)) { resolve_url(g_base_url, buf, abs, sizeof abs); put = abs; }
	int j = 0;
	for (; put[j] && j < (int)sizeof L->href - 1; j++) L->href[j] = put[j];
	L->href[j] = '\0';
	/* target="_blank"/"_new" → open in a new tab (the click handler honors this). */
	const char *tgt = dom_get_attr(el, "target");
	L->newtab = (tgt && (streq_ci(tgt, "_blank") || streq_ci(tgt, "_new"))) ? 1 : 0;
	W_cur_link = B->pg->nlinks++;
	return 1;
}

static void close_link(void)
{
	if (W_cur_link >= 0) {
		B->pg->links[W_cur_link].end = W_n;
		W_cur_link = -1;
	}
}

/* Append an inline <img> to B->pg->imgs on its own line (box from the computed CSS
 * width/height). data: URIs are skipped. */
/* Serialize a DOM subtree back into SVG/XML text: `<tag attr="v" ...>kids</tag>`,
 * or `<tag .../>` when childless. Bounded append; values are emitted raw (the DOM
 * keeps them undecoded), which is what the SVG parser wants. Reuses the existing
 * svg.c rasterizer rather than teaching the layout engine SVG geometry. */
static void svg_put(char *b, int *n, int cap, const char *s)
{
	while (*s && *n < cap - 1) b[(*n)++] = *s++;
}
/* Emit ` name="value"` for a baked SVG paint: -1 skip, -2 none, -3 currentColor,
 * else #rrggbb. Prepended before the element's own attrs so a CSS-resolved paint
 * wins over a presentation attribute (svg.c reads the first matching attr). */
static void svg_put_paint(char *b, int *n, int cap, const char *name, int v)
{
	if (v == -1) return;
	svg_put(b, n, cap, " "); svg_put(b, n, cap, name); svg_put(b, n, cap, "=\"");
	if (v == -2)      svg_put(b, n, cap, "none");
	else if (v == -3) svg_put(b, n, cap, "currentColor");
	else {
		char h[8] = "#"; const char *d = "0123456789abcdef";
		h[1] = d[(v >> 20) & 0xf]; h[2] = d[(v >> 16) & 0xf];
		h[3] = d[(v >> 12) & 0xf]; h[4] = d[(v >> 8) & 0xf];
		h[5] = d[(v >> 4) & 0xf];  h[6] = d[v & 0xf]; h[7] = '\0';
		svg_put(b, n, cap, h);
	}
	svg_put(b, n, cap, "\"");
}
static void svg_serialize(struct dom_node *el, char *b, int *n, int cap)
{
	if (el->type == DOM_TEXT) { if (el->text) svg_put(b, n, cap, el->text); return; }
	svg_put(b, n, cap, "<"); svg_put(b, n, cap, el->tag);
	/* Bake the element's cascaded fill/stroke so CSS (e.g. Tailwind fill-* classes,
	 * `svg{fill:…}`) colours the icon — svg.c only reads attrs/inline-style. */
	{
		struct css_style ecs;
		compute_style(el, &ecs);
		svg_put_paint(b, n, cap, "fill", ecs.fill);
		svg_put_paint(b, n, cap, "stroke", ecs.stroke);
	}
	for (struct dom_attr *a = el->attrs; a; a = a->next) {
		svg_put(b, n, cap, " "); svg_put(b, n, cap, a->name);
		svg_put(b, n, cap, "=\""); svg_put(b, n, cap, a->value ? a->value : "");
		svg_put(b, n, cap, "\"");
	}
	if (!el->first_child) { svg_put(b, n, cap, "/>"); return; }
	svg_put(b, n, cap, ">");
	for (struct dom_node *c = el->first_child; c; c = c->next)
		svg_serialize(c, b, n, cap);
	svg_put(b, n, cap, "</"); svg_put(b, n, cap, el->tag); svg_put(b, n, cap, ">");
}

/* Inline <svg>…</svg>: serialize the subtree and hand it to the SVG rasterizer
 * as a self-contained document, emitted as an inline image (same machinery as
 * <img src=.svg>). The bytes are committed synchronously here on the parse/walk
 * thread — no network fetch — and the slot is exposed only once committed. */
static void handle_svg(struct dom_node *el, const struct css_style *cs)
{
	if (!ensure_imgs(B->pg->n_imgs + 1)) return;
	int cap = 32768;
	char *buf = malloc((unsigned)cap);
	if (!buf) return;
	int len = 0;
	svg_serialize(el, buf, &len, cap);
	buf[len] = '\0';
	if (len < 5 || !svg_probe(buf, len)) { free(buf); return; }

	if (W_run_nl < 1 && W_n > 0) W_n = text_put(W_n, '\n', 0);

	int idx = B->pg->n_imgs;
	struct inl_img *im = &B->pg->imgs[idx];
	im->href[0] = '\0'; im->alt[0] = '\0';
	im->lazy = 0; im->want = 0; im->fetched = 1;   /* nothing to fetch */
	im->cw = (cs->width  >= 0) ? cs->width  : 0;
	im->ch = (cs->height >= 0) ? cs->height : 0;
	im->text_pos = W_n;
	im->ok = 0; im->rows = 0; im->start_row = -1;
	im->iw = im->ih = 0; im->coff = im->clen = 0;
	im->is_argb = 0; im->uid = 0;

	/* currentColor resolves to the SVG's own CSS color if it sets one, else the
	 * inherited text colour (sty_cur.fg) — icons are usually `fill="currentColor"`
	 * coloured by `color` on an ancestor link/element, not on the <svg> itself. */
	resource_commit_inline(idx, (const unsigned char *)buf, len,
	                        cs->color >= 0 ? (unsigned)cs->color : sty_cur.fg);
	B->pg->n_imgs++;                                                   /* expose once committed */
	W_n = text_put(W_n, '\n', 0); W_run_nl = 1;
	free(buf);
}

static void handle_img(struct dom_node *el, const struct css_style *cs)
{
	if (!ensure_imgs(B->pg->n_imgs + 1)) return;
	const char *src = dom_get_attr(el, "src");
	if (!src || !src[0]) return;
	char srcbuf[URL_MAX];
	int k = 0;
	for (; src[k] && k < (int)sizeof srcbuf - 1; k++) srcbuf[k] = src[k];
	srcbuf[k] = '\0';
	decode_entities(srcbuf);
	if (!srcbuf[0] || starts_ci(srcbuf, "data:")) return;

	/* Resolve to an absolute URL (a frame's relative src needs the frame's base),
	 * then apply CSP img-src — a blocked image is dropped, not laid out. The
	 * fetchers resolve im->href against the page, so storing the absolute URL is
	 * equivalent and lets CSP's http->https upgrade stick. */
	char imgabs[URL_MAX], imgurl[URL_MAX];
	if (url_absolute(srcbuf)) { int a = 0; imgabs[0] = '\0'; sapp(imgabs, &a, sizeof imgabs, srcbuf); }
	else                      resolve_url(g_base_url, srcbuf, imgabs, sizeof imgabs);
	if (!csp_url_ok(CSP_IMG, imgabs, "image", imgurl, sizeof imgurl)) return;

	if (W_run_nl < 1 && W_n > 0) W_n = text_put(W_n, '\n', 0);

	struct inl_img *im = &B->pg->imgs[B->pg->n_imgs];
	int j = 0;
	for (; imgurl[j] && j < (int)sizeof im->href - 1; j++) im->href[j] = imgurl[j];
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
	B->pg->n_imgs++;
	W_n = text_put(W_n, '\n', 0);
	W_run_nl = 1;
}

/* <link rel="icon">: standard icon outranks apple-touch; mask-icon (SVG) skipped. */
static void handle_favicon(struct dom_node *el)
{
	const char *rel = dom_get_attr(el, "rel");
	if (!rel || !contains_ci(rel, "icon") || contains_ci(rel, "mask-icon")) return;
	int pri = contains_ci(rel, "apple-touch") ? 1 : 2;
	if (pri < B->pg->favicon_pri) return;
	const char *href = dom_get_attr(el, "href");
	if (!href || !href[0] || starts_ci(href, "data:")) return;
	char buf[URL_MAX];
	int k = 0;
	for (; href[k] && k < (int)sizeof buf - 1; k++) buf[k] = href[k];
	buf[k] = '\0';
	decode_entities(buf);
	int j = 0;
	for (; buf[j] && j < (int)sizeof B->pg->favicon_href - 1; j++) B->pg->favicon_href[j] = buf[j];
	B->pg->favicon_href[j] = '\0';
	B->pg->favicon_pri = pri;
}

/* Emit a literal ASCII/UTF-8 run with attr `a`, no whitespace collapsing — used
 * for the chrome around form controls and list markers. */
static void emit_lit(const char *s, unsigned char a)
{
	if (W_pend_sp) { if (W_n > 0 && W_run_nl == 0) W_n = text_put(W_n, ' ', 0); W_pend_sp = 0; }
	for (const char *p = s; *p; p++) { W_n = text_put(W_n, *p, a); W_run_nl = 0; }
}

/* --- iframes -------------------------------------------------------------- *
 * Each <iframe> the walk meets becomes a cache entry keyed by (absolute src,
 * embedder URL). The entry starts PENDING; the worker fetches the document
 * (resource.c), decides whether it may be embedded, and hands the result back via
 * html_iframe_resolve, which parses an allowed HTML body into a live sub-document.
 * The walk then flattens that sub-document inline, indented and bordered, into the
 * parent's text — the same line-flow the rest of the page uses. The cache lives
 * for the page's lifetime (freed by html_page_teardown) so it survives the many
 * relayouts a page does, rather than re-fetching each time. */
#define FRAME_MAX_DEPTH 4

/* sandbox flags we act on (a present `sandbox` attribute always sets SBX_ON). */
enum { SBX_ON = 1, SBX_SCRIPTS = 2, SBX_SAME_ORIGIN = 4, SBX_FORMS = 8,
       SBX_TOP_NAV = 16, SBX_POPUPS = 32 };

struct frame_doc {
	char  url[URL_MAX];          /* absolute src — the cache key (with parent)   */
	char  parent[URL_MAX];       /* the document that embeds this frame          */
	int   verdict;               /* 0 = pending, else FRAME_OK/BLOCKED/ERROR     */
	char  reason[96];            /* shown when blocked/failed                    */
	struct dom_node *doc;        /* parsed sub-document (NULL until OK)          */
	css_sheet       *sheet;      /* the sub-document's inline <style> rules      */
	struct frame_doc *next;
};
static struct frame_doc *g_frames;

static struct frame_doc *frame_find(const char *url, const char *parent)
{
	for (struct frame_doc *f = g_frames; f; f = f->next)
		if (streq(f->url, url) && streq(f->parent, parent)) return f;
	return 0;
}

static struct frame_doc *frame_get(const char *url, const char *parent)
{
	struct frame_doc *f = frame_find(url, parent);
	if (f) return f;
	f = malloc(sizeof *f);
	if (!f) return 0;
	int k = 0; f->url[0] = '\0';    sapp(f->url, &k, sizeof f->url, url);
	k = 0;     f->parent[0] = '\0'; sapp(f->parent, &k, sizeof f->parent, parent);
	f->verdict = 0; f->reason[0] = '\0'; f->doc = 0; f->sheet = 0;
	f->next = g_frames; g_frames = f;
	return f;
}

static void frames_free(void)
{
	struct frame_doc *f = g_frames;
	while (f) {
		struct frame_doc *n = f->next;
		if (f->doc)   dom_free(f->doc);
		if (f->sheet) css_free(f->sheet);
		free(f);
		f = n;
	}
	g_frames = 0;
}

/* Collect a sub-document's inline <style> blocks into `sheet` (frames don't pull
 * external stylesheets — only the page's own walk does the batched <link> fetch). */
static void collect_inline_styles(struct dom_node *node, css_sheet *sheet)
{
	for (struct dom_node *c = node->first_child; c; c = c->next) {
		if (c->type != DOM_ELEMENT) continue;
		if (streq(c->tag, "style")) {
			for (struct dom_node *t = c->first_child; t; t = t->next)
				if (t->type == DOM_TEXT && t->text)
					css_add(sheet, t->text, strlen(t->text));
		} else {
			collect_inline_styles(c, sheet);
		}
	}
}

static int parse_sandbox(struct dom_node *el)
{
	const char *s = dom_get_attr(el, "sandbox");
	if (!s) return 0;                                  /* no attribute = no sandbox */
	int f = SBX_ON;
	if (contains_ci(s, "allow-scripts"))        f |= SBX_SCRIPTS;
	if (contains_ci(s, "allow-same-origin"))    f |= SBX_SAME_ORIGIN;
	if (contains_ci(s, "allow-forms"))          f |= SBX_FORMS;
	if (contains_ci(s, "allow-top-navigation")) f |= SBX_TOP_NAV;
	if (contains_ci(s, "allow-popups"))         f |= SBX_POPUPS;
	return f;
}

/* Emit the frame's chrome line: a marker, the host, sandbox/allow badges, and an
 * optional status note (loading / blocked / error reason). */
static void frame_chrome(const char *host, const char *note, int sbx, const char *allow)
{
	block_break();
	char line[256]; int n = 0;
	sapp(line, &n, sizeof line, "\xe2\x96\xa3 frame");          /* ▣ */
	if (host && host[0]) { sapp(line, &n, sizeof line, " "); sapp(line, &n, sizeof line, host); }
	if (sbx & SBX_ON)     sapp(line, &n, sizeof line, " \xc2\xb7 sandbox");      /* · */
	if (allow && allow[0]) { sapp(line, &n, sizeof line, " \xc2\xb7 allow="); sapp(line, &n, sizeof line, allow); }
	if (note && note[0])  { sapp(line, &n, sizeof line, " \xe2\x80\x94 "); sapp(line, &n, sizeof line, note); }  /* — */
	emit_lit(line, ST_BOLD | 6);
	block_break();
}

/* Flatten an allowed frame's sub-document into the parent flow: indented one
 * level, under the frame's own CSS sheet and base URL, with link navigation gated
 * by the sandbox. The block context is saved/restored around the recursive walk. */
static void frame_render(struct frame_doc *f, int sbx)
{
	if (!f->doc) { emit_lit("(empty frame)", 6); return; }

	struct sty    sv_sty   = sty_cur;
	unsigned char sv_indent = W_indent;
	css_sheet    *sv_sheet = g_sheet;
	int           sv_nav   = g_link_nav;
	char          sv_base[URL_MAX]; { int k = 0; sv_base[0] = '\0'; sapp(sv_base, &k, sizeof sv_base, g_base_url); }

	/* A sandboxed frame may navigate only with allow-top-navigation/-popups; a
	 * nested frame can never widen what an outer sandbox already forbade. */
	int nav = !(sbx & SBX_ON) || (sbx & (SBX_TOP_NAV | SBX_POPUPS));
	g_link_nav = sv_nav && nav;
	g_sheet    = f->sheet ? f->sheet : sv_sheet;
	W_indent   = (W_indent < 7) ? (unsigned char)(W_indent + 1) : 7;
	{ int k = 0; g_base_url[0] = '\0'; sapp(g_base_url, &k, sizeof g_base_url, f->url); }

	g_frame_depth++;
	walk(f->doc);
	g_frame_depth--;

	g_link_nav = sv_nav;
	g_sheet    = sv_sheet;
	W_indent   = sv_indent;
	sty_cur    = sv_sty;
	{ int k = 0; g_base_url[0] = '\0'; sapp(g_base_url, &k, sizeof g_base_url, sv_base); }
}

static void handle_iframe(struct dom_node *el)
{
	const char *src = dom_get_attr(el, "src");
	char srcbuf[URL_MAX];
	int k = 0;
	if (src) for (; src[k] && k < (int)sizeof srcbuf - 1; k++) srcbuf[k] = src[k];
	srcbuf[k] = '\0';
	decode_entities(srcbuf);

	int sbx = parse_sandbox(el);
	const char *allow = dom_get_attr(el, "allow");

	if (!srcbuf[0]) { frame_chrome("", "no src", sbx, allow); return; }
	if (starts_ci(srcbuf, "javascript:") || starts_ci(srcbuf, "about:")) return;
	if (g_frame_depth >= FRAME_MAX_DEPTH) { frame_chrome("", "nested too deep", sbx, allow); return; }

	char abs[URL_MAX];
	if (url_absolute(srcbuf)) { int k2 = 0; abs[0] = '\0'; sapp(abs, &k2, sizeof abs, srcbuf); }
	else                      resolve_url(g_base_url, srcbuf, abs, sizeof abs);

	char host[URL_HOST_MAX]; struct purl u;
	if (parse_url(abs, &u)) { int j = 0; host[0] = '\0'; sapp(host, &j, sizeof host, u.host); }
	else                    { host[0] = '\0'; }

	/* CSP frame-src (top document only; falls back to child-src then default-src).
	 * upgrade-insecure-requests also rewrites the frame URL here. */
	if (g_frame_depth == 0 && B->pg->csp) {
		char fu[URL_MAX], reason[64];
		if (!csp_check_url(B->pg->csp, CSP_FRAME, abs, g_cur_page_url, fu, sizeof fu, reason, sizeof reason)) {
			csp_report("frame", abs, reason);
			frame_chrome(host, "blocked by CSP frame-src", sbx, allow);
			return;
		}
		{ int j = 0; abs[0] = '\0'; sapp(abs, &j, sizeof abs, fu); }   /* use upgraded URL */
	}

	struct frame_doc *f = frame_get(abs, g_base_url);
	if (!f) { frame_chrome(host, "out of memory", sbx, allow); return; }

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
	default:                                          /* still pending a fetch */
		frame_chrome(host, "loading\xe2\x80\xa6", sbx, allow);   /* … */
		break;
	}
}

/* The focused form control (a text input or textarea), or NULL. The worker sets
 * it on click (html_set_focus); the renderer draws a caret in it. */
static struct dom_node *g_focus;
struct dom_node *html_focus(void) { return g_focus; }

/* Set by the control emitters during the walk; lets the worker route content
 * clicks even on a script-less page so its form controls stay interactive. */
static int g_has_controls;
int html_has_controls(void) { return g_has_controls; }

static int input_is_text(const char *type)
{
	if (!type || !type[0]) return 1;
	return streq(type, "text") || streq(type, "search") || streq(type, "email") ||
	       streq(type, "url")  || streq(type, "tel")    || streq(type, "password") ||
	       streq(type, "number");
}

int html_control_kind(struct dom_node *n)
{
	if (!n || n->type != DOM_ELEMENT) return CTRL_NONE;
	if (streq(n->tag, "textarea")) return CTRL_TEXTAREA;
	if (streq(n->tag, "select"))   return CTRL_SELECT;
	if (streq(n->tag, "button")) {
		const char *t = dom_get_attr(n, "type");
		return (t && streq(t, "submit")) ? CTRL_SUBMIT : CTRL_BUTTON;
	}
	if (streq(n->tag, "input")) {
		const char *t = dom_get_attr(n, "type");
		if (t && streq(t, "checkbox")) return CTRL_CHECKBOX;
		if (t && streq(t, "radio"))    return CTRL_RADIO;
		if (t && (streq(t, "submit") || streq(t, "button") || streq(t, "reset"))) return CTRL_SUBMIT;
		if (t && streq(t, "hidden"))   return CTRL_NONE;
		return CTRL_TEXT;
	}
	return CTRL_NONE;
}

void html_set_focus(struct dom_node *n)
{
	int k = html_control_kind(n);
	g_focus = (k == CTRL_TEXT || k == CTRL_TEXTAREA) ? n : 0;
	B->focus_kind = (k == CTRL_TEXT) ? 1 : (k == CTRL_TEXTAREA) ? 2 : 0;
}

/* --- control default actions (worker-side; called from main.c) ------------- */

void html_toggle_checked(struct dom_node *c)
{
	if (dom_get_attr(c, "checked")) dom_remove_attr(c, "checked");
	else                           dom_set_attr(c, "checked", "");
}

static struct dom_node *form_of(struct dom_node *n)
{
	for (struct dom_node *p = n; p; p = p->parent) if (streq(p->tag, "form")) return p;
	return 0;
}
static void radio_clear(struct dom_node *root, const char *name, struct dom_node *keep)
{
	for (struct dom_node *c = root->first_child; c; c = c->next) {
		if (c->type != DOM_ELEMENT) continue;
		if (streq(c->tag, "input")) {
			const char *t = dom_get_attr(c, "type"), *nm = dom_get_attr(c, "name");
			if (t && streq(t, "radio") && c != keep && nm && name && streq(nm, name))
				dom_remove_attr(c, "checked");
		}
		radio_clear(c, name, keep);
	}
}
void html_radio_select(struct dom_node *c)
{
	const char *name = dom_get_attr(c, "name");
	struct dom_node *scope = form_of(c);
	if (!scope) scope = g_live_dom;
	if (name && scope) radio_clear(scope, name, c);
	dom_set_attr(c, "checked", "");
}

void html_select_cycle(struct dom_node *sel)
{
	struct dom_node *first = 0, *cur = 0, *nextopt = 0;
	int take = 0;
	for (struct dom_node *o = sel->first_child; o; o = o->next) {
		if (o->type != DOM_ELEMENT || !streq(o->tag, "option")) continue;
		if (!first) first = o;
		if (take) { nextopt = o; take = 0; }
		if (dom_get_attr(o, "selected")) { cur = o; take = 1; }
	}
	if (!cur) { if (first) dom_set_attr(first, "selected", ""); return; }
	dom_remove_attr(cur, "selected");
	dom_set_attr(nextopt ? nextopt : first, "selected", "");   /* wrap to first */
}

void html_seed_textarea(struct dom_node *ta)
{
	char buf[1024]; int n = 0;
	for (struct dom_node *c = ta->first_child; c; c = c->next)
		if (c->type == DOM_TEXT && c->text)
			for (const char *p = c->text; *p && n < (int)sizeof buf - 1; p++) buf[n++] = *p;
	buf[n] = '\0';
	dom_set_attr(ta, "value", buf);
}

/* Edit the focused text control's value: append a char, or backspace. Returns the
 * node so the caller can fire an 'input' event on it. */
struct dom_node *html_focus_edit(int ch)
{
	char buf[2048]; int n = 0;
	const char *v;
	if (!g_focus) return 0;
	v = dom_get_attr(g_focus, "value");
	if (v) for (const char *p = v; *p && n < (int)sizeof buf - 1; p++) buf[n++] = *p;
	if (ch == '\b' || ch == 127) { if (n > 0) n--; }
	else if (ch == '\n' || ch == '\r') { if (n < (int)sizeof buf - 1) buf[n++] = '\n'; }  /* textarea newline */
	else if (ch >= 0x20 && n < (int)sizeof buf - 1) buf[n++] = (char)ch;
	buf[n] = '\0';
	dom_set_attr(g_focus, "value", buf);
	return g_focus;
}

/* Build the GET URL a submit would navigate to (action + a query from the form's
 * named controls). Returns 1 with `out` filled, 0 if there's no submittable form. */
static void q_field(char *q, int *qn, int cap, const char *name, const char *val)
{
	if (*qn && *qn < cap - 1) q[(*qn)++] = '&';
	sapp(q, qn, cap, name);
	if (*qn < cap - 1) q[(*qn)++] = '=';
	sapp(q, qn, cap, val ? val : "");
	q[*qn] = '\0';
}
static void form_fields(struct dom_node *n, char *q, int *qn, int cap)
{
	for (struct dom_node *c = n->first_child; c; c = c->next) {
		if (c->type != DOM_ELEMENT) continue;
		const char *tag = c->tag, *name = dom_get_attr(c, "name");
		const char *type = dom_get_attr(c, "type");
		if (name && name[0] && (streq(tag, "input") || streq(tag, "select") || streq(tag, "textarea"))) {
			int skip = type && (streq(type, "submit") || streq(type, "button") || streq(type, "reset"));
			if (!skip) {
				if (type && (streq(type, "checkbox") || streq(type, "radio"))) {
					if (dom_get_attr(c, "checked")) {
						const char *v = dom_get_attr(c, "value");
						q_field(q, qn, cap, name, v ? v : "on");
					}
				} else {
					q_field(q, qn, cap, name, dom_get_attr(c, "value"));
				}
			}
		}
		form_fields(c, q, qn, cap);
	}
}
int html_form_submit_url(struct dom_node *control, char *out, int cap)
{
	struct dom_node *form = form_of(control);
	const char *action, *method;
	char q[URL_MAX]; int qn = 0, on = 0;
	if (!form) return 0;
	action = dom_get_attr(form, "action");
	method = dom_get_attr(form, "method");
	if (action && action[0]) resolve_url(g_cur_page_url, action, out, cap);
	else                     { sapp(out, &on, cap, g_cur_page_url); }

	/* CSP form-action: refuse a submission whose target the policy forbids. Logged
	 * unconditionally (this is a user action, not the spammy per-relayout walk). */
	if (B->pg->csp) {
		char fa[URL_MAX], reason[64];
		if (!csp_check_url(B->pg->csp, CSP_FORM, out, g_cur_page_url, fa, sizeof fa, reason, sizeof reason)) {
			char m[URL_MAX + 96]; int n = 0;
			sapp(m, &n, sizeof m, "[CSP] refused form submission to ");
			sapp(m, &n, sizeof m, out);
			sapp(m, &n, sizeof m, " ("); sapp(m, &n, sizeof m, reason); sapp(m, &n, sizeof m, ")\n");
			devtools_console_append(DT_ERROR, m, n);
			return 0;
		}
		{ int k = 0; out[0] = '\0'; sapp(out, &k, cap, fa); }   /* use upgraded action */
	}

	if (method && streq_ci(method, "post")) return 1;   /* no body support: hit action */
	q[0] = '\0';
	form_fields(form, q, &qn, sizeof q);
	if (qn > 0) {
		on = (int)strlen(out);
		sapp(out, &on, cap, "?");
		sapp(out, &on, cap, q);
	}
	return 1;
}

/* Render <input> as a boxed widget reflecting its live value/checked state; a
 * focused text field shows a caret so typing is visible. */
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
	if (!input_is_text(type)) { emit_lit("[ ", ST_BOLD); emit_lit(val ? val : "", 0); emit_lit(" ]", ST_BOLD); return; }
	{
		int focused = (el == g_focus);
		emit_lit("[ ", ST_BOLD);
		if (val && val[0])           emit_lit(val, 0);
		else if (!focused && ph)     emit_lit(ph, 0);
		else if (!focused)           emit_lit("         ", ST_BOLD);
		if (focused) emit_lit("|", ST_BOLD);          /* caret */
		emit_lit(" ]", ST_BOLD);
	}
}

/* <textarea>: edits land in a synthetic "value" attribute (seeded from the initial
 * text on first focus); show that when present, else the original content. */
static void emit_textarea(struct dom_node *el)
{
	const char *val = dom_get_attr(el, "value");
	int focused = (el == g_focus);
	emit_lit("[ ", ST_BOLD);
	if (val) emit_lit(val, 0);
	else     walk(el);
	if (focused) emit_lit("|", ST_BOLD);
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
static void walk_element_inner(struct dom_node *el);

/* Record this element's covered text range as a span (for click hit-testing),
 * then walk it. Wrapping the whole body means every early return still closes the
 * span, and nested elements naturally produce narrower (deeper) spans. */
/* Emit a ::before / ::after pseudo-element's generated content as inline text,
 * styled by the pseudo rule's colour/weight/decoration. No-op when the element
 * has no such rule (or its content is empty/non-string). */
static void emit_pseudo(struct dom_node *el, int which)
{
	struct css_style pcs;
	if (!css_pseudo_content(g_sheet, el, which, &pcs)) return;
	struct sty sv = sty_cur;
	apply_css(&pcs);
	emit_text(pcs.content);
	sty_cur = sv;
}

static void walk_element(struct dom_node *el)
{
	int si = span_begin(el);
	walk_element_inner(el);
	span_end(si);
}

static void walk_element_inner(struct dom_node *el)
{
	const char *name = el->tag;

	if (streq(name, "script") || streq(name, "style")) return;   /* not rendered */
	if (streq(name, "title")) {                                  /* the document title... */
		struct dom_node *a;
		for (a = el->parent; a; a = a->parent)
			if (streq(a->tag, "svg")) return;            /* ...not an SVG <title> tooltip */
		if (g_frame_depth == 0) capture_title(el);        /* a frame's <title> isn't the page's */
		return;
	}

	struct css_style cs;
	compute_style(el, &cs);
	if (cs.disp == CSS_DISP_NONE) return;                        /* display:none */

	if (streq(name, "img"))    { handle_img(el, &cs); return; }
	if (streq(name, "svg"))    { handle_svg(el, &cs); return; }
	if (streq(name, "iframe")) { handle_iframe(el); return; }
	if (streq(name, "link")) { if (g_frame_depth == 0) handle_favicon(el); return; }
	if (streq(name, "br")) { block_break(); return; }
	if (streq(name, "hr")) {                                     /* a rule line  */
		block_break();
		unsigned char s = W_size; W_size = 7;
		W_n = text_put(W_n, ' ', 0); W_run_nl = 0;
		W_size = s; block_break();
		return;
	}
	if (streq(name, "input"))  { g_has_controls = 1; emit_input(el);  return; }
	if (streq(name, "select")) { g_has_controls = 1; emit_select(el); return; }
	if (streq(name, "button")) {
		struct sty sv = sty_cur; sty_cur.bold = 1;
		g_has_controls = 1;
		emit_lit("[ ", ST_BOLD); walk(el); emit_lit(" ]", ST_BOLD);
		sty_cur = sv; return;
	}
	if (streq(name, "textarea")) { g_has_controls = 1; emit_textarea(el); return; }

	struct sty saved = sty_cur;
	unsigned char sv_size = W_size, sv_center = W_center, sv_mono = W_mono,
	              sv_indent = W_indent, sv_pre = W_pre;
	unsigned sv_bg = W_bg;
	char sv_lt = W_listtype; int sv_ol = W_olnum;
	int linked = streq(name, "a") ? open_link(el) : 0;

	apply_ua_style(name);
	apply_css(&cs);
	if (cs.bg >= 0) W_bg = (unsigned)cs.bg;

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
	if (blk) layout_box_open(el, &cs, W_n);     /* a block/flex box for the layout tree */
	emit_pseudo(el, CSS_PSEUDO_BEFORE);
	walk(el);
	emit_pseudo(el, CSS_PSEUDO_AFTER);
	if (blk) layout_box_close(W_n);
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
 * are batched rather than one blocking fetch per <link>. Both lists grow on demand
 * (a page may carry any number of <style>/<link> sources); the allocated capacity
 * persists across pages as a high-water mark (counts reset to 0 each load). */
struct css_src { const char *text; int len; int url_i; };  /* text -> inline; else url_i */
static struct css_src *g_src;
static int   g_nsrc, g_src_cap;
static char **g_css_url;
static int   g_n_css_url, g_css_url_cap;
static const char *g_page_url;

static int css_src_room(void)        /* ensure one free slot in g_src */
{
	if (g_nsrc < g_src_cap) return 1;
	int nc = g_src_cap ? g_src_cap * 2 : 16;
	struct css_src *n = realloc(g_src, (unsigned)nc * sizeof *n);
	if (!n) return 0;
	g_src = n; g_src_cap = nc; return 1;
}
static int css_url_room(void)        /* ensure one free slot in g_css_url */
{
	if (g_n_css_url < g_css_url_cap) return 1;
	int nc = g_css_url_cap ? g_css_url_cap * 2 : 8;
	char **n = realloc(g_css_url, (unsigned)nc * sizeof *n);
	if (!n) return 0;
	g_css_url = n; g_css_url_cap = nc; return 1;
}

static void src_inline(const char *text, int len)
{
	if (!css_src_room()) return;
	g_src[g_nsrc].text = text; g_src[g_nsrc].len = len; g_src[g_nsrc].url_i = -1;
	g_nsrc++;
}

static void src_link(const char *absurl)
{
	if (!css_src_room() || !css_url_room()) return;
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
			/* style-src for an inline <style>: gate on its nonce / a sha256 of the
			 * full rule text / 'unsafe-inline'. The sheet keeps the text node
			 * pointers, so only the hash uses a temporary concatenation. */
			int total = 0;
			for (struct dom_node *t = c->first_child; t; t = t->next)
				if (t->type == DOM_TEXT && t->text) total += (int)strlen(t->text);
			char *buf = total ? malloc((unsigned)total + 1) : 0;
			int bl = 0;
			if (buf) {
				for (struct dom_node *t = c->first_child; t; t = t->next)
					if (t->type == DOM_TEXT && t->text)
						for (const char *s = t->text; *s; s++) buf[bl++] = *s;
				buf[bl] = '\0';
			}
			char reason[64];
			int ok = (!B->pg->csp) ? 1 :
			         csp_check_inline(B->pg->csp, CSP_STYLE, dom_get_attr(c, "nonce"),
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
			if (rel && href && href[0] && contains_ci(rel, "stylesheet") &&
			    !contains_ci(rel, "alternate")) {
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

/* Fold any <meta http-equiv="Content-Security-Policy" content="..."> into the
 * page's policy set (meta CSP augments header CSP; both are enforced). */
static void scan_meta_csp(struct dom_node *node)
{
	for (struct dom_node *c = node->first_child; c; c = c->next) {
		if (c->type != DOM_ELEMENT) continue;
		if (streq(c->tag, "meta")) {
			const char *he = dom_get_attr(c, "http-equiv");
			const char *ct = dom_get_attr(c, "content");
			if (he && ct && ct[0] && streq_ci(he, "content-security-policy")) {
				if (!B->pg->csp) B->pg->csp = csp_new();
				csp_add(B->pg->csp, ct);
			}
		} else {
			scan_meta_csp(c);
		}
	}
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
 * because render() reads B->pg->body_font and B->pg->faces on the main thread while this
 * runs on the fetch worker. */
/* Is `family` one of the comma-separated names in the font-family `stack`? Uses
 * the same naive split as the picker below, so the faces we fetch are exactly the
 * ones the picker could select. */
static int family_in_stack(const char *stack, const char *family)
{
	const char *p = stack;
	while (*p) {
		char name[CSS_FAM_MAX]; int n = 0;
		while (*p && *p != ',' && n < (int)sizeof name - 1) name[n++] = *p++;
		name[n] = '\0';
		while (*p && *p != ',') p++;
		if (*p == ',') p++;
		if (name[0] && streq(name, family)) return 1;
	}
	return 0;
}

/* Ensure B->pg->faces holds `need` entries (grows on demand). render() reads only
 * B->pg->body_font, never the array, so the realloc is safe under blk_lock. */
static int faces_room(int need)
{
	if (need <= B->pg->faces_cap) return 1;
	int nc = B->pg->faces_cap ? B->pg->faces_cap : 4;
	while (nc < need) nc *= 2;
	struct loaded_face *n = realloc(B->pg->faces, (unsigned)nc * sizeof *n);
	if (!n) return 0;
	B->pg->faces = n; B->pg->faces_cap = nc;
	return 1;
}

static void load_faces(struct dom_node *root, const char *page_url, res_batch_fn fetch)
{
	/* The page renders with a single body font, so the ONLY @font-face faces that
	 * can ever be used are those whose family is named in the <body> font-family
	 * stack. Fetch exactly those — any number of them, no fixed cap — and skip the
	 * rest, so an icon-font-heavy page doesn't download faces it never renders. */
	struct css_style cs;
	struct dom_node *body_el = find_tag(root, "body");
	const char *stack = "";
	if (body_el) { compute_style(body_el, &cs); stack = cs.fam; }

	int nfaces = css_fontface_count(g_sheet);
	char **urls              = nfaces ? malloc((unsigned)nfaces * sizeof *urls) : 0;
	char (*fams)[CSS_FAM_MAX] = nfaces ? malloc((unsigned)nfaces * sizeof *fams) : 0;
	int nu = 0;
	if (nfaces && (!urls || !fams)) { free(urls); free(fams); urls = 0; fams = 0; nfaces = 0; }

	for (int i = 0; i < nfaces; i++) {
		const char *family = 0, *url = 0;
		css_fontface_at(g_sheet, i, &family, &url);
		if (!family || !family[0] || !url || !url[0]) continue;
		if (!family_in_stack(stack, family)) continue;        /* body can't use it */

		char abs[URL_MAX], use[URL_MAX];
		resolve_url(page_url, url, abs, sizeof abs);
		if (!csp_url_ok(CSP_FONT, abs, "font", use, sizeof use)) continue;  /* font-src */
		int L = strlen(use);
		char *u = malloc((unsigned)L + 1);
		if (!u) continue;
		for (int k = 0; k <= L; k++) u[k] = use[k];

		int j = 0;
		for (; family[j] && j < CSS_FAM_MAX - 1; j++) fams[nu][j] = family[j];
		fams[nu][j] = '\0';
		urls[nu++] = u;
	}

	/* Batch-fetch the font files (multiplexed over h2 same-origin), then parse.
	 * nf holds the loaded faces plus one slot for the monospace fallback below. */
	struct loaded_face *nf = malloc((unsigned)(nu + 1) * sizeof *nf);
	int nn = 0;
	if (nf && nu > 0) {
		unsigned char **bodies = calloc((unsigned)nu, sizeof *bodies);
		int           *lens   = calloc((unsigned)nu, sizeof *lens);
		if (bodies && lens) {
			if (fetch) fetch(urls, nu, bodies, lens);
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
		free(bodies); free(lens);
	}
	for (int i = 0; i < nu; i++) free(urls[i]);
	free(urls); free(fams);

	/* Walk the <body> font-family stack and pick the first available family. */
	struct font *chosen = 0;
	if (body_el && nf) {
		const char *p = stack;
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
			if (fam_monospace(name)) {
				struct font *m = load_face_file("/etc/fonts/LiberationMono-Regular.ttf", &nf[nn]);
				if (m) { chosen = m; nn++; break; }
			}
			/* a named family we don't have -> fall through to the next entry */
		}
	}

	/* Publish: drop the previous page's faces, install the new set. */
	blk_lock();
	for (int i = 0; i < B->pg->n_faces; i++) {
		if (B->pg->faces[i].font) font_free(B->pg->faces[i].font);
		free(B->pg->faces[i].data);
	}
	if (nn > 0 && faces_room(nn)) {
		for (int i = 0; i < nn; i++) B->pg->faces[i] = nf[i];
		B->pg->n_faces = nn;
		B->pg->body_font = chosen;
	} else {
		B->pg->n_faces = 0;
		B->pg->body_font = 0;                  /* faces_room OOM: free below, fall back */
	}
	blk_unlock();

	if (nf) {
		if (B->pg->n_faces == 0)               /* publish failed: don't leak parsed faces */
			for (int i = 0; i < nn; i++) { if (nf[i].font) font_free(nf[i].font); free(nf[i].data); }
		free(nf);
	}
}

/* Reset the render buffers and re-flatten the live DOM into them. Shared by the
 * initial html_to_text and by html_relayout (after an event mutates the tree). */
/* Alpha-composite src (0xAARRGGBB) over the opaque dst (0xRRGGBB). */
static unsigned composite_over(unsigned dst, unsigned src)
{
	unsigned a = (src >> 24) & 0xff;
	if (a == 0)   return dst;
	if (a == 255) return src & 0xFFFFFF;
	unsigned sr = (src >> 16) & 0xff, sg = (src >> 8) & 0xff, sb = src & 0xff;
	unsigned dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
	unsigned ia = 255 - a;
	unsigned r = (sr * a + dr * ia) / 255, g = (sg * a + dg * ia) / 255, b = (sb * a + db * ia) / 255;
	return (r << 16) | (g << 8) | b;
}

/* The canvas background and default text colour, from the <html>/<body> cascade.
 * The canvas starts white; each of <html> then <body>'s background composites over
 * it (so a translucent tint blends instead of overriding), matching how a browser
 * propagates the root/body background to the canvas. Defaults: white page, near-
 * black text. The main thread reads B->pg->page_bg/fg. */
static void compute_page_colors(struct dom_node *root)
{
	unsigned canvas = 0xffffff;
	B->pg->page_fg = 0x202830;
	struct css_style cs;
	const char *who[] = { "html", "body" };
	for (int i = 0; i < 2; i++) {
		struct dom_node *el = find_tag(root, who[i]);
		if (!el) continue;
		compute_style(el, &cs);
		if (cs.color >= 0) B->pg->page_fg = (unsigned)cs.color;
		unsigned argb;
		if (css_bg_argb(g_sheet, el, &argb)) canvas = composite_over(canvas, argb);
	}
	B->pg->page_bg = canvas;
}

static void layout_dom(void)
{
	css_set_viewport(BW, BH);        /* so @media re-evaluates at the current size */
	B->pg->nlinks = 0;
	B->pg->n_imgs = 0;
	B->img_pool_len = 0;
	compute_page_colors(g_live_dom);
	sty_cur.fg = B->pg->page_fg; sty_cur.bold = sty_cur.under = 0;
	W_bg = BG_NONE;
	W_n = 0; W_pend_sp = 0; W_run_nl = 1; W_cur_link = -1;
	g_nspans = 0;
	layout_reset();                  /* fresh box tree for this walk */
	g_has_controls = 0;
	/* Reset the frame walk context to the top document (the cache itself persists
	 * across relayouts; only the per-walk base/depth/nav reset here). */
	g_frame_depth = 0; g_link_nav = 1;
	{ int k = 0; g_base_url[0] = '\0'; sapp(g_base_url, &k, sizeof g_base_url, g_cur_page_url); }
	if (g_live_dom) walk(g_live_dom);
	B->pg->text_len = W_n;
}

/* Free the current page's live DOM, CSS sheet and JS context. Called on every
 * navigation/page replacement before a new page is built (and is a no-op when no
 * live page is held). The JS context goes FIRST: its node wrappers reference the
 * DOM nodes, so the DOM must outlive the wrapper teardown. */
void html_page_teardown(void)
{
	js_page_end();
	if (g_live_dom) { dom_free(g_live_dom); g_live_dom = 0; }
	if (g_sheet)    { css_free(g_sheet);    g_sheet = 0; }
	frames_free();                     /* drop this page's iframe sub-documents */
	layout_free();                     /* drop the box tree */
	g_nspans = 0;
	g_focus = 0; B->focus_kind = 0;
}

/* --- iframe fetch handshake (worker thread; resource.c does the network) ---- */

int html_iframes_pending(void)
{
	for (struct frame_doc *f = g_frames; f; f = f->next)
		if (f->verdict == 0) return 1;
	return 0;
}

int html_iframe_next_pending(char *url, int url_cap, char *parent, int parent_cap)
{
	for (struct frame_doc *f = g_frames; f; f = f->next) {
		if (f->verdict != 0) continue;
		int k = 0; url[0] = '\0';    sapp(url, &k, url_cap, f->url);
		k = 0;     parent[0] = '\0'; sapp(parent, &k, parent_cap, f->parent);
		return 1;
	}
	return 0;
}

void html_iframe_resolve(const char *url, const char *parent, int verdict,
                         const char *reason, const unsigned char *body, int len,
                         const char *ctype)
{
	struct frame_doc *f = frame_find(url, parent);
	if (!f) return;

	if (f->doc)   { dom_free(f->doc); f->doc = 0; }
	if (f->sheet) { css_free(f->sheet); f->sheet = 0; }
	{ int k = 0; f->reason[0] = '\0'; if (reason) sapp(f->reason, &k, sizeof f->reason, reason); }

	if (verdict == FRAME_OK) {
		/* Render an HTML body (or an empty/sloppy Content-Type, the common case);
		 * anything else is left as an empty frame with a note. */
		int as_html = !ctype || !ctype[0] || contains_ci(ctype, "html");
		if (as_html && body && len > 0) {
			f->doc = dom_parse((const char *)body, len);
			f->sheet = css_new();
			if (f->doc && f->sheet) collect_inline_styles(f->doc, f->sheet);
			if (!f->doc) {
				f->verdict = FRAME_ERROR;
				int k = 0; f->reason[0] = '\0'; sapp(f->reason, &k, sizeof f->reason, "could not parse");
				return;
			}
		} else if (!as_html) {
			int k = 0; f->reason[0] = '\0'; sapp(f->reason, &k, sizeof f->reason, "non-HTML content");
		}
		f->verdict = FRAME_OK;
	} else {
		f->verdict = (verdict == FRAME_BLOCKED) ? FRAME_BLOCKED : FRAME_ERROR;
	}
}

/* Re-flatten the live DOM into B's buffers after a script mutated it (run by the
 * worker right after event dispatch), forcing a re-wrap + repaint. Inline images
 * are rebuilt fresh, so the caller re-fetches them if any remain. */
void html_relayout(void)
{
	if (!g_live_dom) return;
	layout_dom();
	B->wrap_seq = 0xFFFFFFFFu;
	B->seq++;
}

void html_to_text(const char *page_url, res_batch_fn fetch)
{
	html_page_teardown();           /* reclaim the previous page's live DOM/CSS/JS */

	B->pg->doc_title[0] = '\0';
	B->pg->nlinks = 0;
	B->pg->n_imgs = 0;
	B->img_pool_len = 0;
	B->pg->favicon_href[0] = '\0';
	B->pg->favicon_pri = 0;

	{ int k = 0; sapp(g_cur_page_url, &k, sizeof g_cur_page_url, page_url ? page_url : ""); }
	g_page_url = g_cur_page_url;    /* persists with the page (was a caller pointer) */
	g_nsrc = 0;
	g_n_css_url = 0;

	detect_meta_charset();          /* header charset (if any) already set by fetch.c */
	g_csp_report = 1;               /* log CSP violations for this initial load only */

	struct dom_node *root = dom_parse(B->pg->raw, B->pg->raw_len);
	g_sheet = css_new();
	g_live_dom = root;              /* the live tree — kept until the next teardown  */
	if (root) {
		scan_meta_csp(root);    /* fold <meta http-equiv=CSP> into B->pg->csp before any load */
		/* Link:-header sheets come before the document's own (then tree order). */
		for (struct css_link *L = B->ext_css; L; L = L->next) src_link(L->url);
		collect_styles(root);

		/* Batch-fetch the external sheets, then add every source in order. */
		unsigned char **bodies = g_n_css_url ? calloc((unsigned)g_n_css_url, sizeof *bodies) : 0;
		int            *lens   = g_n_css_url ? calloc((unsigned)g_n_css_url, sizeof *lens)   : 0;
		if (fetch && g_n_css_url > 0 && bodies && lens) fetch(g_css_url, g_n_css_url, bodies, lens);

		for (int s = 0; s < g_nsrc; s++) {
			if (g_src[s].text) {
				css_add(g_sheet, g_src[s].text, g_src[s].len);
			} else {
				int ui = g_src[s].url_i;
				if (ui >= 0 && bodies && bodies[ui]) css_add(g_sheet, (char *)bodies[ui], lens[ui]);
			}
		}
		for (int i = 0; i < g_n_css_url; i++) { free(g_css_url[i]); if (bodies) free(bodies[i]); }
		free(bodies); free(lens);

		/* @font-face: load declared faces and pick the body font (before walk so
		 * the renderer sees B->pg->body_font for this page's text). */
		load_faces(root, g_page_url, fetch);

		/* Lay out and paint the parsed page BEFORE running scripts: a slow or
		 * long-running script (e.g. a setInterval-heavy SPA) must not keep the
		 * page blank. The main thread repaints from B->seq within a frame while
		 * JS keeps running on this worker thread. */
		layout_dom();
		B->seq++;

		/* Then run scripts (`fetch` pulls external <script src>); the JS context
		 * stays alive past this call so listeners/timers fire later. Relayout only
		 * if a script actually mutated the DOM — listener-only scripts need none. */
		unsigned mseq = dom_mutation_seq;
		js_run_document(root, g_page_url, fetch);
		if (dom_mutation_seq != mseq)
			layout_dom();
	} else {
		B->pg->text_len = 0;
	}
	g_csp_report = 0;               /* later relayouts re-decide silently (no log spam) */
	/* DOM + sheet + JS context are NOT freed here — they live until the next
	 * navigation (html_page_teardown), so events keep working after load. */
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
	B->pg->page_bg = 0xffffff; B->pg->page_fg = 0x202830;
	sty_cur.fg = B->pg->page_fg; sty_cur.bold = sty_cur.under = 0;
	W_bg = BG_NONE; W_size = W_center = W_mono = W_indent = W_pre = 0;

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
	B->pg->text_len = n;
}

/* Fill the styled-text buffer with a literal one-off message (e.g. "unsupported
 * content type"), so render() shows it through the normal text path. */
void doc_message(const char *m)
{
	int n = 0;
	B->pg->page_bg = 0xffffff; B->pg->page_fg = 0x202830;
	while (m[n]) {
		if (!ensure_text(n + 1)) break;
		B->pg->text[n] = m[n]; B->pg->attr[n] = 0;
		B->pg->bp[n] = 0; B->pg->fg[n] = B->pg->page_fg; B->pg->bg[n] = BG_NONE;
		n++;
	}
	B->pg->text_len = n;
	B->pg->doc_title[0] = '\0';
	B->pg->nlinks = 0;
	B->pg->n_imgs = 0;
}
