/* css.c — CSS parser + selector matcher + cascade. See css.h.
 *
 * Scope: selectors are tag / .class / #id / * / compound (div.a#b) / descendant
 * (space) / child (>) / grouping (a, b); pseudo-classes are ignored. Cascade is
 * by specificity then source order, with !important winning. Properties acted on:
 * color, background[-color], font-weight, text-decoration, display, width,
 * height (others parse-and-ignore, like any browser). Inline <style> and style=""
 * only — external <link rel=stylesheet> is not fetched. Descendant matching is
 * greedy (no backtracking), enough for ordinary selectors. */
#include "owos.h"
#include "css.h"

#define MAX_CLS    6
#define MAX_STEPS  6

struct sel_step {
	char tag[16];
	char cls[MAX_CLS][24];
	int  ncls;
	char id[24];
	int  child;            /* combinator to the step on the right is '>' */
};

struct selector { struct sel_step step[MAX_STEPS]; int n; };

struct css_rule {
	struct selector  sel;
	struct css_style decl;
	int              spec, order, important;
	struct css_rule *next;
};

/* An @font-face: a declared family name + the raw TTF/OTF src url. */
struct css_face { char family[CSS_FAM_MAX]; char *url; struct css_face *next; };

struct css_sheet { struct css_rule *head, *tail; int order; struct css_face *faces; };

/* ------------------------------------------------ value parsing ------------ */
static int hexnib(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int named_rgb(const char *v)
{
	static const struct { const char *name; int rgb; } t[] = {
		{ "red", 0xff0000 }, { "green", 0x008000 }, { "lime", 0x00ff00 },
		{ "blue", 0x0000ff }, { "navy", 0x000080 }, { "black", 0x000000 },
		{ "white", 0xffffff }, { "gray", 0x808080 }, { "grey", 0x808080 },
		{ "silver", 0xc0c0c0 }, { "orange", 0xffa500 }, { "purple", 0x800080 },
		{ "magenta", 0xff00ff }, { "fuchsia", 0xff00ff }, { "yellow", 0xffff00 },
		{ "olive", 0x808000 }, { "teal", 0x008080 }, { "cyan", 0x00ffff },
		{ "aqua", 0x00ffff }, { "maroon", 0x800000 }, { "brown", 0xa52a2a },
		{ "pink", 0xffc0cb }, { "gold", 0xffd700 }, { "darkblue", 0x00008b },
		{ "darkred", 0x8b0000 }, { "darkgreen", 0x006400 }, { "darkgray", 0xa9a9a9 },
		{ "darkgrey", 0xa9a9a9 }, { "lightgray", 0xd3d3d3 }, { "lightgrey", 0xd3d3d3 },
		{ "transparent", -1 }, { 0, 0 }
	};
	for (int i = 0; t[i].name; i++) if (streq_ci(v, t[i].name)) return t[i].rgb;
	return -1;
}

/* Parse a CSS colour value into 0xRRGGBB, or -1 if it isn't a colour. */
static int parse_color(const char *v)
{
	while (*v == ' ' || *v == '\t') v++;
	if (*v == '#') {
		v++;
		int d[6], n = 0;
		while (n < 6 && hexnib(v[n]) >= 0) { d[n] = hexnib(v[n]); n++; }
		if (n >= 6) return (d[0]*16+d[1]) << 16 | (d[2]*16+d[3]) << 8 | (d[4]*16+d[5]);
		if (n >= 3) return (d[0]*17) << 16 | (d[1]*17) << 8 | (d[2]*17);
		return -1;
	}
	if (starts_ci(v, "rgb")) {
		const char *p = v + 3;
		if (*p == 'a' || *p == 'A') p++;
		while (*p == ' ' || *p == '(') p++;
		int c[3] = { 0, 0, 0 };
		for (int i = 0; i < 3; i++) {
			while (*p == ' ') p++;
			int got = 0, val = 0;
			while (*p >= '0' && *p <= '9') { val = val * 10 + (*p++ - '0'); got = 1; }
			if (!got) return -1;
			c[i] = val > 255 ? 255 : val;
			while (*p == ' ' || *p == ',') p++;
		}
		return (c[0] << 16) | (c[1] << 8) | c[2];
	}
	return named_rgb(v);
}

/* Parse a CSS length: integer px (or unitless), else -1 (%, em, auto, calc...). */
static int parse_len(const char *v)
{
	while (*v == ' ' || *v == '\t') v++;
	int n = 0, any = 0;
	while (*v >= '0' && *v <= '9') { n = n * 10 + (*v++ - '0'); any = 1; }
	if (!any) return -1;
	while (*v == ' ') v++;
	if (*v == '%' || *v == 'e' || *v == 'r' || *v == 'v' || *v == 'c')   /* %, em/ex, rem, vw, cm/ch */
		return -1;
	return n;
}

static int parse_weight(const char *v)
{
	while (*v == ' ') v++;
	if (starts_ci(v, "bold")) return 1;        /* bold, bolder */
	if (starts_ci(v, "normal") || starts_ci(v, "light")) return 0;
	int n = 0, any = 0;
	while (*v >= '0' && *v <= '9') { n = n * 10 + (*v++ - '0'); any = 1; }
	if (any) return n >= 600 ? 1 : 0;
	return -1;
}

/* substring (ci) — for text-decoration:"underline" inside a multi-value list. */
static int has_ci(const char *hay, const char *needle)
{
	for (; *hay; hay++) if (starts_ci(hay, needle)) return 1;
	return 0;
}

static int parse_disp(const char *v)
{
	while (*v == ' ') v++;
	if (starts_ci(v, "none"))   return CSS_DISP_NONE;
	if (starts_ci(v, "inline")) return CSS_DISP_INLINE;   /* incl inline-block */
	return CSS_DISP_BLOCK;                                /* block/flex/grid/table */
}

/* ------------------------------------------------ declarations ------------- */
void css_style_reset(struct css_style *s)
{
	s->color = s->bg = -1;
	s->bold = s->under = -1;
	s->disp = CSS_DISP_UNSET;
	s->width = s->height = -1;
	s->fam[0] = '\0';
}

/* Copy the FIRST font-family name from a value (up to a comma) into out[cap],
 * dequoted and lowercased. Used for an @font-face's own font-family (one name). */
static void first_family(const char *v, char *out, int cap)
{
	while (*v == ' ' || *v == '\t') v++;
	char q = 0;
	if (*v == '"' || *v == '\'') q = *v++;
	int k = 0;
	while (*v && k < cap - 1) {
		if (q) { if (*v == q) break; }
		else if (*v == ',' || *v == ';') break;
		out[k++] = lower(*v); v++;
	}
	while (k > 0 && (out[k-1] == ' ' || out[k-1] == '\t')) k--;
	out[k] = '\0';
}

/* Copy a whole font-family VALUE — the comma-separated fallback stack — into
 * out[cap]: each name dequoted + lowercased + space-trimmed, names joined by a
 * single comma (e.g. `"Helvetica Neue",Arial,sans-serif` -> `helvetica neue,
 * arial,sans-serif`). The browser walks this list and uses the first available
 * face, resolving generic keywords (sans-serif/system-ui/monospace/...). */
static void copy_family_stack(const char *v, char *out, int cap)
{
	int k = 0;
	while (*v && k < cap - 1) {
		while (*v == ' ' || *v == '\t' || *v == ',') v++;     /* skip separators */
		if (!*v || *v == ';') break;
		char q = 0;
		if (*v == '"' || *v == '\'') q = *v++;
		int start = k;
		while (*v && k < cap - 1) {
			if (q) { if (*v == q) { v++; break; } }
			else if (*v == ',' || *v == ';') break;
			out[k++] = lower(*v); v++;
		}
		while (k > start && (out[k-1] == ' ' || out[k-1] == '\t')) k--;  /* trim */
		while (*v == ' ' || *v == '\t') v++;
		if (*v == ',' && k > start && k < cap - 1) out[k++] = ',';       /* join */
	}
	if (k > 0 && out[k-1] == ',') k--;                        /* no trailing comma */
	out[k] = '\0';
}

/* Case-insensitive suffix test. */
static int ends_ci(const char *s, const char *suf)
{
	int sl = strlen(s), fl = strlen(suf);
	return sl >= fl && streq_ci(s + sl - fl, suf);
}

/* Lowercased-compare a span [a,ae) against a NUL-terminated lowercase literal. */
static int span_is(const char *a, const char *ae, const char *lit)
{
	while (a < ae && *lit && lower(*a) == *lit) { a++; lit++; }
	return a == ae && !*lit;
}

static void apply_decl(const char *p, const char *pe, const char *v, const char *ve,
                       struct css_style *out, int *important)
{
	while (p < pe && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
	while (pe > p && (pe[-1] == ' ' || pe[-1] == '\t')) pe--;
	while (v < ve && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
	while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n' || ve[-1] == '\r')) ve--;
	if (p >= pe || v >= ve) return;

	/* copy value to a small NUL-terminated buffer + strip !important */
	char val[128];
	int vl = (int)(ve - v);
	if (vl > (int)sizeof val - 1) vl = (int)sizeof val - 1;
	for (int i = 0; i < vl; i++) val[i] = v[i];
	val[vl] = '\0';
	for (int i = 0; i + 9 < vl + 1; i++) {
		if (val[i] == '!' && starts_ci(val + i + 1, "important")) {
			if (important) *important = 1;
			val[i] = '\0';
			while (i > 0 && (val[i-1] == ' ' || val[i-1] == '\t')) val[--i] = '\0';
			break;
		}
	}

	if (span_is(p, pe, "color"))                out->color  = parse_color(val);
	else if (span_is(p, pe, "background-color") ||
	         span_is(p, pe, "background"))       out->bg     = parse_color(val);
	else if (span_is(p, pe, "font-weight"))      out->bold   = parse_weight(val);
	else if (span_is(p, pe, "text-decoration") ||
	         span_is(p, pe, "text-decoration-line"))
		out->under = has_ci(val, "underline") ? 1 : (has_ci(val, "none") ? 0 : out->under);
	else if (span_is(p, pe, "display"))          out->disp   = parse_disp(val);
	else if (span_is(p, pe, "width"))            out->width  = parse_len(val);
	else if (span_is(p, pe, "height"))           out->height = parse_len(val);
	else if (span_is(p, pe, "font-family"))      copy_family_stack(val, out->fam, CSS_FAM_STACK);
}

/* Parse a `prop: value; ...` block [s,end) into *out. */
static void parse_decls(const char *s, const char *end, struct css_style *out, int *important)
{
	while (s < end) {
		const char *semi = s;
		while (semi < end && *semi != ';') semi++;
		const char *colon = s;
		while (colon < semi && *colon != ':') colon++;
		if (colon < semi) apply_decl(s, colon, colon + 1, semi, out, important);
		s = (semi < end) ? semi + 1 : end;
	}
}

void css_apply_inline(const char *decls, struct css_style *out)
{
	int imp = 0;
	parse_decls(decls, decls + strlen(decls), out, &imp);
}

/* ------------------------------------------------ selectors ---------------- */
static int is_sel_name(char c)
{ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'; }

/* Parse one compound selector token (between combinators) into *st. */
static void parse_compound(const char *s, const char *e, struct sel_step *st)
{
	st->tag[0] = '\0'; st->id[0] = '\0'; st->ncls = 0;
	while (s < e) {
		if (*s == '.') {
			s++;
			char *d = (st->ncls < MAX_CLS) ? st->cls[st->ncls] : 0;
			int k = 0;
			while (s < e && is_sel_name(*s)) { if (d && k < 23) d[k++] = *s; s++; }
			if (d) { d[k] = '\0'; if (k) st->ncls++; }
		} else if (*s == '#') {
			s++;
			int k = 0;
			while (s < e && is_sel_name(*s)) { if (k < 23) st->id[k++] = *s; s++; }
			st->id[k] = '\0';
		} else if (*s == ':') {                 /* pseudo: skip the whole token */
			s++;
			if (s < e && *s == ':') s++;
			while (s < e && (is_sel_name(*s) || *s == '(')) {
				if (*s == '(') { while (s < e && *s != ')') s++; if (s < e) s++; break; }
				s++;
			}
		} else if (*s == '*') {
			s++;                                 /* universal: no constraint */
		} else if (is_sel_name(*s)) {
			int k = 0;
			while (s < e && is_sel_name(*s)) { if (k < 15) st->tag[k++] = lower(*s); s++; }
			st->tag[k] = '\0';
		} else {
			s++;
		}
	}
}

/* Parse one full selector (no commas) into *sel; returns 0 if empty/too deep. */
static int parse_selector(const char *s, const char *e, struct selector *sel)
{
	sel->n = 0;
	int pending_child = 0;
	while (s < e) {
		while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
		if (s >= e) break;
		if (*s == '>') { pending_child = 1; s++; continue; }
		const char *tok = s;
		while (s < e && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != '>') s++;
		if (sel->n >= MAX_STEPS) return sel->n > 0;   /* cap depth, keep what we have */
		struct sel_step *st = &sel->step[sel->n];
		parse_compound(tok, s, st);
		st->child = 0;
		if (sel->n > 0 && pending_child) sel->step[sel->n - 1].child = 1;
		pending_child = 0;
		sel->n++;
	}
	return sel->n > 0;
}

static int selector_spec(const struct selector *sel)
{
	int a = 0, b = 0, c = 0;
	for (int i = 0; i < sel->n; i++) {
		if (sel->step[i].id[0]) a++;
		b += sel->step[i].ncls;
		if (sel->step[i].tag[0]) c++;
	}
	return a * 10000 + b * 100 + c;
}

/* ------------------------------------------------ matching ----------------- */
static int node_has_class(const struct dom_node *n, const char *cls)
{
	const char *list = dom_get_attr(n, "class");
	if (!list) return 0;
	int cl = strlen(cls);
	for (const char *p = list; *p; ) {
		while (*p == ' ' || *p == '\t') p++;
		const char *q = p;
		while (*q && *q != ' ' && *q != '\t') q++;
		if (q - p == cl) {
			int i = 0;
			while (i < cl && p[i] == cls[i]) i++;
			if (i == cl) return 1;
		}
		p = q;
	}
	return 0;
}

static int step_match(const struct sel_step *st, const struct dom_node *n)
{
	if (!n || n->type != DOM_ELEMENT) return 0;
	if (st->tag[0] && !streq(st->tag, n->tag)) return 0;
	if (st->id[0]) {
		const char *id = dom_get_attr(n, "id");
		if (!id || !streq_ci(id, st->id)) return 0;
	}
	for (int i = 0; i < st->ncls; i++)
		if (!node_has_class(n, st->cls[i])) return 0;
	return 1;
}

static int selector_match(const struct selector *sel, const struct dom_node *el)
{
	int i = sel->n - 1;
	if (!step_match(&sel->step[i], el)) return 0;
	const struct dom_node *cur = el;
	for (i = sel->n - 2; i >= 0; i--) {
		if (sel->step[i].child) {
			cur = cur->parent;
			if (!step_match(&sel->step[i], cur)) return 0;
		} else {
			cur = cur ? cur->parent : 0;
			while (cur && !step_match(&sel->step[i], cur)) cur = cur->parent;
			if (!cur) return 0;
		}
	}
	return 1;
}

/* ------------------------------------------------ sheet build -------------- */
css_sheet *css_new(void)
{
	css_sheet *s = malloc(sizeof *s);
	if (!s) return 0;
	s->head = s->tail = 0;
	s->order = 0;
	s->faces = 0;
	return s;
}

/* Pick the first url(...) in a src value that names a raw TTF/OTF (we can't decode
 * woff/woff2). Writes it to url[cap] (empty if none qualifies). */
static void parse_src(const char *v, char *url, int cap)
{
	url[0] = '\0';
	for (const char *p = v; *p; ) {
		if (starts_ci(p, "url(")) {
			const char *u = p + 4;
			while (*u == ' ' || *u == '"' || *u == '\'') u++;
			char buf[512]; int k = 0;
			while (*u && *u != ')' && *u != '"' && *u != '\'' && k < (int)sizeof buf - 1)
				buf[k++] = *u++;
			buf[k] = '\0';
			char base[512]; int bk = 0;               /* strip ?query / #frag */
			for (const char *q = buf; *q && *q != '?' && *q != '#' && bk < (int)sizeof base - 1; q++)
				base[bk++] = *q;
			base[bk] = '\0';
			if (ends_ci(base, ".ttf") || ends_ci(base, ".otf")) {
				int j = 0; for (; buf[j] && j < cap - 1; j++) url[j] = buf[j]; url[j] = '\0';
				return;
			}
			p = u;
		} else {
			p++;
		}
	}
}

/* Parse an @font-face block [s,end): pull out font-family + a usable src url and,
 * if both are present, register the face on the sheet. */
static void parse_fontface(css_sheet *sh, const char *s, const char *end)
{
	char family[CSS_FAM_MAX]; family[0] = '\0';
	char url[512]; url[0] = '\0';
	while (s < end) {
		const char *semi = s; while (semi < end && *semi != ';') semi++;
		const char *colon = s; while (colon < semi && *colon != ':') colon++;
		if (colon < semi) {
			const char *p = s, *pe = colon;
			while (p < pe && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
			while (pe > p && (pe[-1] == ' ' || pe[-1] == '\t')) pe--;
			const char *v = colon + 1, *ve = semi;
			while (v < ve && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
			while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n' || ve[-1] == '\r')) ve--;
			char val[512]; int vl = (int)(ve - v);
			if (vl > (int)sizeof val - 1) vl = (int)sizeof val - 1;
			for (int i = 0; i < vl; i++) val[i] = v[i];
			val[vl] = '\0';
			if (span_is(p, pe, "font-family")) first_family(val, family, CSS_FAM_MAX);
			else if (span_is(p, pe, "src"))    parse_src(val, url, sizeof url);
		}
		s = (semi < end) ? semi + 1 : end;
	}
	if (!family[0] || !url[0]) return;
	struct css_face *fc = malloc(sizeof *fc);
	if (!fc) return;
	int i = 0; for (; family[i]; i++) fc->family[i] = family[i]; fc->family[i] = '\0';
	int ul = strlen(url);
	fc->url = malloc((unsigned)ul + 1);
	if (!fc->url) { free(fc); return; }
	for (i = 0; i <= ul; i++) fc->url[i] = url[i];
	fc->next = sh->faces;
	sh->faces = fc;
}

static void sheet_push(css_sheet *sh, const struct selector *sel,
                       const struct css_style *decl, int important)
{
	struct css_rule *r = malloc(sizeof *r);
	if (!r) return;
	r->sel = *sel;
	r->decl = *decl;
	r->spec = selector_spec(sel);
	r->order = sh->order++;
	r->important = important;
	r->next = 0;
	if (sh->tail) sh->tail->next = r;
	else sh->head = r;
	sh->tail = r;
}

static const char *skip_ws_com(const char *p, const char *end)
{
	for (;;) {
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')) p++;
		if (p + 1 < end && p[0] == '/' && p[1] == '*') {
			p += 2;
			while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
			p = (p + 1 < end) ? p + 2 : end;
			continue;
		}
		return p;
	}
}

static const char *skip_atrule(const char *p, const char *end)
{
	while (p < end && *p != ';' && *p != '{') p++;
	if (p < end && *p == ';') return p + 1;
	if (p < end && *p == '{') {
		int depth = 0;
		while (p < end) {
			if (*p == '{') depth++;
			else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
			p++;
		}
	}
	return p;
}

void css_add(css_sheet *sh, const char *src, int len)
{
	if (!sh) return;
	const char *p = src, *end = src + len;
	while (p < end) {
		p = skip_ws_com(p, end);
		if (p >= end) break;
		if (*p == '@') {
			if (starts_ci(p, "@font-face")) {        /* parse it, don't skip */
				const char *b = p;
				while (b < end && *b != '{' && *b != ';') b++;
				if (b < end && *b == '{') {
					const char *db = b + 1, *de = db; int depth = 1;
					while (de < end && depth) { if (*de == '{') depth++; else if (*de == '}') depth--; if (depth) de++; }
					parse_fontface(sh, db, de);
					p = (de < end) ? de + 1 : end;
					continue;
				}
			}
			p = skip_atrule(p, end);
			continue;
		}

		const char *sel_s = p;
		while (p < end && *p != '{' && *p != '}') p++;
		if (p >= end || *p != '{') { if (p < end) p++; continue; }
		const char *sel_e = p++;
		const char *dec_s = p;
		while (p < end && *p != '}') p++;
		const char *dec_e = p;
		if (p < end) p++;

		struct css_style decl;
		css_style_reset(&decl);
		int important = 0;
		parse_decls(dec_s, dec_e, &decl, &important);

		const char *q = sel_s;                  /* split the selector list on ',' */
		while (q < sel_e) {
			const char *comma = q;
			while (comma < sel_e && *comma != ',') comma++;
			struct selector sel;
			if (parse_selector(q, comma, &sel)) sheet_push(sh, &sel, &decl, important);
			q = (comma < sel_e) ? comma + 1 : sel_e;
		}
	}
}

void css_free(css_sheet *sh)
{
	if (!sh) return;
	struct css_rule *r = sh->head;
	while (r) { struct css_rule *n = r->next; free(r); r = n; }
	struct css_face *fc = sh->faces;
	while (fc) { struct css_face *n = fc->next; free(fc->url); free(fc); fc = n; }
	free(sh);
}

int css_fontface_count(const css_sheet *sh)
{
	int n = 0;
	if (sh) for (struct css_face *fc = sh->faces; fc; fc = fc->next) n++;
	return n;
}

void css_fontface_at(const css_sheet *sh, int i, const char **family, const char **url)
{
	*family = 0; *url = 0;
	if (!sh) return;
	for (struct css_face *fc = sh->faces; fc; fc = fc->next, i--)
		if (i == 0) { *family = fc->family; *url = fc->url; return; }
}

/* ------------------------------------------------ cascade ------------------ */
static void fold(struct css_style *out, const struct css_style *d)
{
	if (d->color >= 0)  out->color  = d->color;
	if (d->bg >= 0)     out->bg     = d->bg;
	if (d->bold >= 0)   out->bold   = d->bold;
	if (d->under >= 0)  out->under  = d->under;
	if (d->disp)        out->disp   = d->disp;
	if (d->width >= 0)  out->width  = d->width;
	if (d->height >= 0) out->height = d->height;
	if (d->fam[0]) { int i = 0; for (; d->fam[i] && i < CSS_FAM_STACK - 1; i++) out->fam[i] = d->fam[i]; out->fam[i] = '\0'; }
}

void css_apply_sheet(css_sheet *sh, const struct dom_node *el, struct css_style *out)
{
	if (!sh || !el || el->type != DOM_ELEMENT) return;

	/* Collect matches, then fold in ascending priority (important, spec, order)
	 * so the winner is applied last. */
	struct css_rule *match[256];
	int nm = 0;
	for (struct css_rule *r = sh->head; r && nm < 256; r = r->next)
		if (selector_match(&r->sel, el)) match[nm++] = r;

	for (int i = 1; i < nm; i++) {           /* insertion sort, stable enough */
		struct css_rule *k = match[i];
		int j = i - 1;
		while (j >= 0) {
			struct css_rule *m = match[j];
			int hi = (m->important != k->important) ? (m->important > k->important)
			       : (m->spec != k->spec) ? (m->spec > k->spec)
			       : (m->order > k->order);
			if (!hi) break;
			match[j + 1] = m; j--;
		}
		match[j + 1] = k;
	}
	for (int i = 0; i < nm; i++) fold(out, &match[i]->decl);
}

void css_apply_attrs(const struct dom_node *el, struct css_style *out)
{
	if (!el || el->type != DOM_ELEMENT) return;
	const char *c = dom_get_attr(el, "color");
	if (c) { int rgb = parse_color(c); if (rgb >= 0) out->color = rgb; }
	const char *bg = dom_get_attr(el, "bgcolor");
	if (bg) { int rgb = parse_color(bg); if (rgb >= 0) out->bg = rgb; }
	const char *w = dom_get_attr(el, "width");
	if (w) { int px = parse_len(w); if (px >= 0) out->width = px; }
	const char *h = dom_get_attr(el, "height");
	if (h) { int px = parse_len(h); if (px >= 0) out->height = px; }
}
