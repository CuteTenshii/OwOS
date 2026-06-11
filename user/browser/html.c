/* html.c — URL parsing/resolution and the HTML -> styled-text reducer.
 * Ported from the former kernel app_browser.c; pure computation over B's
 * buffers, no I/O. */
#include "browser.h"

/* ----------------------------------------------------------- URL parsing */

int parse_url(const char *url, struct purl *u)
{
	const char *p = url;
	u->tls = 0; u->port = 0;
	if      (starts_ci(p, "http://"))  p += 7;
	else if (starts_ci(p, "https://")) { p += 8; u->tls = 1; }

	int i = 0;
	while (*p && *p != '/' && *p != ':' && i < (int)sizeof(u->host) - 1)
		u->host[i++] = *p++;
	u->host[i] = '\0';
	if (i == 0) return 0;

	if (*p == ':') {
		p++;
		int port = 0;
		while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');
		u->port = port;
	}
	if (u->port == 0) u->port = u->tls ? 443 : 80;

	int j = 0;
	if (*p != '/') u->path[j++] = '/';
	while (*p && j < (int)sizeof(u->path) - 1) u->path[j++] = *p++;
	u->path[j] = '\0';
	if (u->path[0] == '\0') { u->path[0] = '/'; u->path[1] = '\0'; }
	return 1;
}

void resolve_url(const char *base, const char *loc, char *out, int cap)
{
	int n = 0;
	if (starts_ci(loc, "http://") || starts_ci(loc, "https://")) {
		sapp(out, &n, cap, loc);
		return;
	}
	struct purl b;
	if (!parse_url(base, &b)) { sapp(out, &n, cap, loc); return; }
	const char *scheme = b.tls ? "https://" : "http://";

	if (loc[0] == '/' && loc[1] == '/') {
		sapp(out, &n, cap, scheme);
		sapp(out, &n, cap, loc + 2);
	} else if (loc[0] == '/') {
		sapp(out, &n, cap, scheme);
		sapp(out, &n, cap, b.host);
		sapp(out, &n, cap, loc);
	} else {
		sapp(out, &n, cap, scheme);
		sapp(out, &n, cap, b.host);
		int slash = 0;
		for (int i = 0; b.path[i]; i++) if (b.path[i] == '/') slash = i;
		char dir[URL_MAX]; int d = 0;
		for (int i = 0; i <= slash && d < (int)sizeof(dir) - 1; i++)
			dir[d++] = b.path[i];
		dir[d] = '\0';
		sapp(out, &n, cap, dir);
		sapp(out, &n, cap, loc);
	}
}

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
		if (v == 160) return -1;
		if (v >= 0x20 && v < 0x7f) return (int)v;
		return -2;
	}
	struct { const char *name; int ch; } ent[] = {
		{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
		{ "apos", '\'' }, { "nbsp", -1 }, { "mdash", '-' }, { "ndash", '-' },
		{ "hellip", '.' }, { "copy", 'c' }, { "reg", 'r' }, { "trade", 't' },
		{ "rsquo", '\'' }, { "lsquo", '\'' }, { "rdquo", '"' }, { "ldquo", '"' },
		{ "raquo", '>' }, { "laquo", '<' }, { "middot", '-' }, { 0, 0 }
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
					char href[256];
					if (tag_attr(tagbody, tagend, "href", href, sizeof href) &&
					    href[0] && href[0] != '#' &&
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
