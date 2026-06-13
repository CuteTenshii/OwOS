/* html.c — the HTML -> styled-text reducer. Pure computation over B's buffers,
 * no I/O. (URL parsing/resolution now lives in the shared user/lib/url.c.) */
#include "browser.h"

/* -------------------------------------------------- styling: colours & CSS */

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

static int hexnib(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int css_color_index(const char *v)
{
	while (*v == ' ' || *v == '"' || *v == '\'') v++;
	if (*v == '#') {
		v++;
		int d[6], n = 0;
		while (n < 6 && hexnib(v[n]) >= 0) { d[n] = hexnib(v[n]); n++; }
		unsigned rgb;
		if (n >= 6)      rgb = (d[0]*16+d[1]) << 16 | (d[2]*16+d[3]) << 8 | (d[4]*16+d[5]);
		else if (n >= 3) rgb = (d[0]*17) << 16 | (d[1]*17) << 8 | (d[2]*17);
		else return -1;
		return nearest_palette(rgb);
	}
	static const struct { const char *name; unsigned rgb; } named[] = {
		{ "red", 0xff0000 }, { "green", 0x008000 }, { "lime", 0x00ff00 },
		{ "blue", 0x0000ff }, { "navy", 0x000080 }, { "black", 0x000000 },
		{ "white", 0xffffff }, { "gray", 0x808080 }, { "grey", 0x808080 },
		{ "silver", 0xc0c0c0 }, { "orange", 0xffa500 }, { "purple", 0x800080 },
		{ "magenta", 0xff00ff }, { "fuchsia", 0xff00ff }, { "yellow", 0xffff00 },
		{ "olive", 0x808000 }, { "teal", 0x008080 }, { "cyan", 0x00ffff },
		{ "aqua", 0x00ffff }, { "maroon", 0x800000 }, { "brown", 0xa52a2a },
		{ "pink", 0xffc0cb }, { "gold", 0xffd700 }, { "darkblue", 0x00008b },
		{ "darkred", 0x8b0000 }, { "darkgreen", 0x006400 }, { 0, 0 }
	};
	for (int i = 0; named[i].name; i++)
		if (starts_ci(v, named[i].name)) return nearest_palette(named[i].rgb);
	return -1;
}

static int tag_color_index(const char *tag, const char *end)
{
	for (const char *p = tag; p + 5 < end; p++) {
		if (lower(p[0]) == 'c' && starts_ci(p, "color") &&
		    (p == tag || p[-1] != '-')) {
			const char *q = p + 5;
			while (q < end && (*q == ' ')) q++;
			if (q < end && (*q == ':' || *q == '=')) {
				q++;
				while (q < end && (*q == ' ' || *q == '"' || *q == '\'')) q++;
				return css_color_index(q);
			}
		}
	}
	return -1;
}

static int tag_attr(const char *tag, const char *end, const char *attr,
                    char *out, int cap)
{
	int al = slen(attr);
	for (const char *p = tag; p + al < end; p++) {
		if (!starts_ci(p, attr)) continue;
		if (p != tag && (p[-1] == '-' ||
		    (p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z')))
			continue;
		const char *q = p + al;
		while (q < end && *q == ' ') q++;
		if (q >= end || *q != '=') continue;
		q++;
		while (q < end && *q == ' ') q++;
		char quote = 0;
		if (q < end && (*q == '"' || *q == '\'')) quote = *q++;
		int o = 0;
		while (q < end && o < cap - 1) {
			if (quote) { if (*q == quote) break; }
			else if (*q == ' ' || *q == '>') break;
			out[o++] = *q++;
		}
		out[o] = '\0';
		return 1;
	}
	return 0;
}

/* --- per-parse style stack --- */
struct sty { unsigned char fg, bold, under; };
static struct sty sty_cur, sty_stk[48];
static char       sty_name[48][12];
static int        sty_sp;

static void sty_reset(void) { sty_cur.fg = sty_cur.bold = sty_cur.under = 0; sty_sp = 0; }

static void sty_push(const char *nm)
{
	if (sty_sp >= 48) return;
	sty_stk[sty_sp] = sty_cur;
	int i = 0;
	for (; nm[i] && i < 11; i++) sty_name[sty_sp][i] = nm[i];
	sty_name[sty_sp][i] = '\0';
	sty_sp++;
}

static void sty_pop(const char *nm)
{
	for (int i = sty_sp - 1; i >= 0; i--)
		if (streq(sty_name[i], nm)) { sty_cur = sty_stk[i]; sty_sp = i; return; }
}

static unsigned char sty_byte(void)
{
	unsigned char a = sty_cur.fg & ST_COLOR;
	if (sty_cur.bold)  a |= ST_BOLD;
	if (sty_cur.under) a |= ST_UNDER;
	return a;
}

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

static int is_void_tag(const char *name)
{
	static const char *const v[] = {
		"br", "hr", "img", "input", "meta", "link", "source", "area",
		"base", "col", "embed", "param", "track", "wbr", 0
	};
	for (int i = 0; v[i]; i++) if (streq(name, v[i])) return 1;
	return 0;
}

static void open_style(const char *name, const char *tag, const char *tagend)
{
	int color = tag_color_index(tag, tagend);
	int styled = 0;
	if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
		sty_push(name); sty_cur.fg = 8; sty_cur.bold = 1; styled = 1;
	} else if (streq(name, "b") || streq(name, "strong")) {
		sty_push(name); sty_cur.bold = 1; styled = 1;
	} else if (streq(name, "i") || streq(name, "em")) {
		sty_push(name); sty_cur.fg = 7; styled = 1;
	} else if (streq(name, "a")) {
		sty_push(name); sty_cur.fg = 1; sty_cur.under = 1; styled = 1;
	} else if (streq(name, "u") || streq(name, "ins")) {
		sty_push(name); sty_cur.under = 1; styled = 1;
	} else if (streq(name, "code") || streq(name, "kbd") ||
	           streq(name, "samp") || streq(name, "tt")) {
		sty_push(name); sty_cur.fg = 5; styled = 1;
	} else if (streq(name, "mark")) {
		sty_push(name); sty_cur.fg = 2; styled = 1;
	}
	if (color >= 0 && !is_void_tag(name)) {
		if (!styled) sty_push(name);
		sty_cur.fg = (unsigned char)color;
	}
}

/* Fold a Unicode codepoint to a printable ASCII byte for our 7-bit renderer:
 * the char itself if ASCII, a close ASCII stand-in for common Latin-1/punctuation
 * codepoints, -1 for a non-breaking space, or -2 to drop it. Used for numeric
 * entities (&#233;, &#8217;) and the accented named entities below. */
static int uni_to_ascii(unsigned v)
{
	if (v >= 0x20 && v < 0x7f) return (int)v;
	switch (v) {
	case 0xA0: return -1;                                  /* nbsp            */
	case 0xA9: return 'c'; case 0xAE: return 'r'; case 0x2122: return 't';
	case 0xB7: case 0x2022: return '-';                   /* middot, bullet  */
	case 0x2018: case 0x2019: case 0x2032: case 0x2039: return '\'';
	case 0x201C: case 0x201D: case 0x2033: return '"';
	case 0x2013: case 0x2014: case 0x2212: return '-';    /* dashes, minus   */
	case 0x2026: return '.';                              /* ellipsis        */
	case 0xAB: case 0x203A: return '>'; case 0xBB: return '>';
	case 0xB0: return 'o';                                /* degree          */
	case 0xD7: return 'x'; case 0xF7: return '/';         /* times, divide   */
	case 0xB1: return '+';                                /* plus-minus      */
	case 0xA3: return 'L'; case 0x20AC: return 'E'; case 0xA2: return 'c';
	case 0xA7: return 'S'; case 0xB6: return 'P';         /* sect, para      */
	case 0x2020: case 0x2021: return '+';                 /* daggers         */
	}
	switch (v) {                                          /* accented -> base */
	case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 'A';
	case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: return 'a';
	case 0xC7: return 'C'; case 0xE7: return 'c';
	case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
	case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
	case 0xCC: case 0xCD: case 0xCE: case 0xCF: return 'I';
	case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'i';
	case 0xD1: return 'N'; case 0xF1: return 'n';
	case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return 'O';
	case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return 'o';
	case 0xD9: case 0xDA: case 0xDB: case 0xDC: return 'U';
	case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
	case 0xDD: case 0xFD: case 0xFF: return 'y';
	case 0xDF: return 's';                                /* sharp s         */
	}
	return -2;
}

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
		return uni_to_ascii(v);
	}
	static const struct { const char *name; int ch; } ent[] = {
		{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
		{ "apos", '\'' }, { "nbsp", -1 }, { "mdash", '-' }, { "ndash", '-' },
		{ "hellip", '.' }, { "copy", 'c' }, { "reg", 'r' }, { "trade", 't' },
		{ "rsquo", '\'' }, { "lsquo", '\'' }, { "rdquo", '"' }, { "ldquo", '"' },
		{ "sbquo", '\'' }, { "bdquo", '"' }, { "raquo", '>' }, { "laquo", '<' },
		{ "middot", '-' }, { "bull", '-' }, { "deg", 'o' }, { "times", 'x' },
		{ "divide", '/' }, { "plusmn", '+' }, { "euro", 'E' }, { "pound", 'L' },
		{ "cent", 'c' }, { "sect", 'S' }, { "para", 'P' }, { "dagger", '+' },
		{ "agrave", 'a' }, { "aacute", 'a' }, { "acirc", 'a' }, { "atilde", 'a' },
		{ "auml", 'a' }, { "aring", 'a' }, { "aelig", 'a' }, { "ccedil", 'c' },
		{ "egrave", 'e' }, { "eacute", 'e' }, { "ecirc", 'e' }, { "euml", 'e' },
		{ "igrave", 'i' }, { "iacute", 'i' }, { "icirc", 'i' }, { "iuml", 'i' },
		{ "ntilde", 'n' }, { "ograve", 'o' }, { "oacute", 'o' }, { "ocirc", 'o' },
		{ "otilde", 'o' }, { "ouml", 'o' }, { "oslash", 'o' }, { "ugrave", 'u' },
		{ "uacute", 'u' }, { "ucirc", 'u' }, { "uuml", 'u' }, { "yacute", 'y' },
		{ "yuml", 'y' }, { "szlig", 's' }, { "Agrave", 'A' }, { "Aacute", 'A' },
		{ "Auml", 'A' }, { "Ccedil", 'C' }, { "Eacute", 'E' }, { "Egrave", 'E' },
		{ "Ntilde", 'N' }, { "Ouml", 'O' }, { "Uuml", 'U' }, { 0, 0 }
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
 * navigate or fetch them. */
static void decode_entities(char *s)
{
	char *o = s;
	const char *p = s, *end = s + slen(s);
	while (p < end) {
		if (*p == '&') {
			int adv, c = decode_entity(p, end, &adv);
			p += adv;
			if (c == -2) continue;          /* drop                            */
			if (c == -1) { *o++ = ' '; continue; }
			*o++ = (char)c;
		} else {
			*o++ = *p++;
		}
	}
	*o = '\0';
}

void html_to_text(void)
{
	const char *p   = B->raw;
	const char *end = B->raw + B->raw_len;
	char    *o    = B->text;
	unsigned char *oa = B->attr;
	char    *oend = B->text + TEXT_MAX - 1;

	int skip = 0, cap_title = 0, pend_sp = 0, run_nl = 1;
	int tl = 0, t_pend = 0, cur_link = -1;

	sty_reset();
	B->doc_title[0] = '\0';
	B->nlinks = 0;
	B->n_imgs = 0;
	B->img_pool_len = 0;

	while (p < end) {
		if (*p == '<') {
			const char *t = p + 1;
			int closing = (t < end && *t == '/');
			if (closing) t++;
			char name[16]; int nl = 0;
			while (t < end && nl < 15 &&
			       ((*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') ||
			        (*t >= '0' && *t <= '9')))
				name[nl++] = lower(*t++);
			name[nl] = '\0';

			const char *e = p;
			while (e < end && *e != '>') e++;
			const char *tagbody = p + 1;
			const char *tagend  = e;
			p = (e < end) ? e + 1 : end;

			if (streq(name, "script") || streq(name, "style")) {
				skip = closing ? 0 : 1;
				continue;
			}
			if (streq(name, "title")) {
				cap_title = !closing;
				if (cap_title) { tl = 0; t_pend = 0; }
				continue;
			}
			if (skip) continue;

			if (streq(name, "a")) {
				if (closing) {
					if (cur_link >= 0) {
						B->links[cur_link].end = (int)(o - B->text);
						cur_link = -1;
					}
				} else if (B->nlinks < MAX_LINKS) {
					char href[256]; href[0] = '\0';
					if (tag_attr(tagbody, tagend, "href", href, sizeof href))
						decode_entities(href);
					if (href[0] && href[0] != '#' &&
					    !starts_ci(href, "javascript:") &&
					    !starts_ci(href, "mailto:")) {
						struct link *L = &B->links[B->nlinks];
						L->start = (int)(o - B->text);
						int k = 0;
						for (; href[k] && k < (int)sizeof L->href - 1; k++)
							L->href[k] = href[k];
						L->href[k] = '\0';
						cur_link = B->nlinks++;
					}
				}
			}

			if (streq(name, "img") && !closing && B->n_imgs < MAX_INL_IMG) {
				char src[256]; src[0] = '\0';
				if (tag_attr(tagbody, tagend, "src", src, sizeof src))
					decode_entities(src);
				if (src[0] && !starts_ci(src, "data:")) {
					/* put the image on its own line: break before, anchor at
					 * the empty line, break after. */
					if (run_nl < 1 && o > B->text) { *oa++ = 0; *o++ = '\n'; }
					struct inl_img *im = &B->imgs[B->n_imgs];
					int k = 0;
					for (; src[k] && k < (int)sizeof im->href - 1; k++)
						im->href[k] = src[k];
					im->href[k] = '\0';
					im->alt[0] = '\0';
					char alt[48];
					if (tag_attr(tagbody, tagend, "alt", alt, sizeof alt)) {
						int j = 0;
						for (; alt[j] && j < (int)sizeof im->alt - 1; j++)
							im->alt[j] = alt[j];
						im->alt[j] = '\0';
					}
					im->text_pos = (int)(o - B->text);
					im->ok = 0; im->rows = 0; im->start_row = -1;
					im->iw = im->ih = 0; im->coff = im->clen = 0;
					B->n_imgs++;
					if (o < oend) { *oa++ = 0; *o++ = '\n'; }
					run_nl = 1;
				}
			}

			if (closing) sty_pop(name);
			else         open_style(name, tagbody, tagend);

			if (is_block_tag(name)) {
				pend_sp = 0;
				if (run_nl < 2 && o > B->text) { *oa++ = 0; *o++ = '\n'; run_nl++; }
			}
			continue;
		}

		if (skip) { p++; continue; }

		int c;
		if (*p == '&') {
			int adv;
			c = decode_entity(p, end, &adv);
			p += adv;
			if (c == -2) continue;
			if (c == -1) { if (cap_title) t_pend = 1; else pend_sp = 1; continue; }
		} else {
			c = (unsigned char)*p++;
		}

		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			if (cap_title) t_pend = 1; else pend_sp = 1;
			continue;
		}
		if (c < 0x20 || c >= 0x7f) continue;

		if (cap_title) {
			if (t_pend && tl > 0 && tl < (int)sizeof B->doc_title - 1)
				B->doc_title[tl++] = ' ';
			t_pend = 0;
			if (tl < (int)sizeof B->doc_title - 1) B->doc_title[tl++] = (char)c;
			B->doc_title[tl] = '\0';
			continue;
		}

		if (pend_sp) {
			if (o > B->text && run_nl == 0 && o < oend) { *oa++ = 0; *o++ = ' '; }
			pend_sp = 0;
		}
		if (o < oend) { *oa++ = sty_byte(); *o++ = (char)c; run_nl = 0; }
		else break;
	}
	B->text_len = (int)(o - B->text);
}

/* Render a non-HTML text body verbatim into the styled-text buffer: newlines
 * are kept (the word-wrapper honours them), tabs expand to the next 8-column
 * stop, and other control bytes are dropped. This is what fixes plain `.txt`
 * pages, whose line breaks the HTML reducer would otherwise collapse. */
void plain_to_text(void)
{
	const char *p = B->raw, *pend = B->raw + B->raw_len;
	char *o = B->text; unsigned char *oa = B->attr;
	char *oend = B->text + TEXT_MAX - 1;
	int col = 0;

	B->doc_title[0] = '\0';
	B->nlinks = 0;
	B->n_imgs = 0;

	while (p < pend && o < oend) {
		unsigned char c = (unsigned char)*p++;
		if (c == '\r') continue;
		if (c == '\n') { *oa++ = 0; *o++ = '\n'; col = 0; continue; }
		if (c == '\t') {
			int sp = 8 - (col & 7);
			while (sp-- > 0 && o < oend) { *oa++ = 0; *o++ = ' '; col++; }
			continue;
		}
		if (c < 0x20 || c >= 0x7f) continue;
		*oa++ = 0; *o++ = (char)c; col++;
	}
	B->text_len = (int)(o - B->text);
}

/* Fill the styled-text buffer with a literal one-off message (e.g. "unsupported
 * content type"), so render() shows it through the normal text path. */
void doc_message(const char *m)
{
	int i = 0;
	for (; m[i] && i < TEXT_MAX - 1; i++) { B->text[i] = m[i]; B->attr[i] = 0; }
	B->text_len = i;
	B->doc_title[0] = '\0';
	B->nlinks = 0;
	B->n_imgs = 0;
}
