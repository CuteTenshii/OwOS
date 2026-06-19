/* css.c — CSS parser + selector matcher + cascade. See css.h.
 *
 * Scope: selectors are tag / .class / #id / * / compound (div.a#b) / descendant
 * (space) / child (>) / grouping (a, b); structural/state pseudo-classes
 * (:first-child, :nth-child(), :not(), :checked, :root, ... — dynamic ones like
 * :hover never match in a static render) and @supports feature queries are
 * honoured. Cascade is
 * by specificity then source order, with !important winning. Properties acted on:
 * color, background[-color], font-weight, text-decoration, display, width,
 * height (others parse-and-ignore, like any browser). Inline <style> and style=""
 * only — external <link rel=stylesheet> is not fetched. Descendant matching is
 * greedy (no backtracking), enough for ordinary selectors. */
#include "owos.h"
#include "css.h"

#define MAX_CLS    6
#define MAX_STEPS  6
#define MAX_PC     3          /* pseudo-classes per compound (input:checked:focus) */
#define MAX_NOTCLS 3          /* classes inside a :not() simple selector            */

/* The argument of :not() — a single simple selector (tag / #id / .class, no
 * combinators or nested pseudos): enough for :not(.x), :not(div), :not(div.x). */
struct simple_sel {
	char tag[16];
	char cls[MAX_NOTCLS][24];
	int  ncls;
	char id[24];
};

/* Pseudo-classes we model. The structural/state ones are matched against the
 * DOM; the dynamic group lands on PC_NEVER (no live interaction state in a static
 * render, so it never matches), as does any pseudo-class we don't recognise (its
 * rule then can't apply, which is what an invalid selector should do). */
enum {
	PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD,
	PC_FIRST_OF_TYPE, PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE,
	PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE,
	PC_ROOT, PC_EMPTY, PC_NOT,
	PC_CHECKED, PC_DISABLED, PC_ENABLED, PC_REQUIRED, PC_OPTIONAL, PC_LINK,
	PC_NEVER,
};

struct pseudo {
	int kind;
	int a, b;                 /* an+b for the nth-* kinds */
	struct simple_sel arg;    /* :not() argument (kind == PC_NOT) */
};

struct sel_step {
	char tag[16];
	char cls[MAX_CLS][24];
	int  ncls;
	char id[24];
	struct pseudo pc[MAX_PC];
	int  npc;
	int  child;            /* combinator to the step on the right is '>' */
};

struct selector { struct sel_step step[MAX_STEPS]; int n; };

/* A parsed @media query the rule lives under (n == 0 → no query, always applies).
 * A query is an OR of comma-separated conditions; each condition is a media type
 * plus ANDed features, optionally `not`-negated. See media_matches(). */
#define MAX_FEAT  4
#define MAX_COND  4
enum { MT_ALL, MT_SCREEN, MT_PRINT, MT_OTHER };
enum { MF_UNKNOWN, MF_MIN_W, MF_MAX_W, MF_W, MF_MIN_H, MF_MAX_H, MF_H,
       MF_ORIENT_PORTRAIT, MF_ORIENT_LANDSCAPE, MF_SCHEME_LIGHT, MF_SCHEME_DARK };

struct media_feature { int kind; int val; };   /* val = px (width/height only) */
struct media_cond {
	int negate;                              /* a leading `not` */
	int mtype;
	struct media_feature feat[MAX_FEAT];
	int nfeat;
};
struct media_query { struct media_cond cond[MAX_COND]; int n; };

struct css_rule {
	struct selector    sel;
	struct css_style   decl;
	struct media_query media;
	int                spec, order, important;
	struct css_rule   *next;
};

/* An @font-face: a declared family name + the raw TTF/OTF src url. */
struct css_face { char family[CSS_FAM_MAX]; char *url; struct css_face *next; };

struct css_sheet { struct css_rule *head, *tail; int order; struct css_face *faces; };

/* Viewport the @media features test against; the browser refreshes it per cascade. */
static int g_vw = 800, g_vh = 600;
void css_set_viewport(int width, int height)
{
	g_vw = width  > 0 ? width  : 0;
	g_vh = height > 0 ? height : 0;
}

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
	s->center = -1;
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
	else if (span_is(p, pe, "text-align"))
		out->center = has_ci(val, "center") ? 1 : (has_ci(val, "left") || has_ci(val, "right") || has_ci(val, "justify") ? 0 : out->center);
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

/* ------------------------------------------------ @media ------------------- */
static const char *skip_sp(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')) p++;
	return p;
}

/* Match lowercase keyword `lit` at *pp (token-bounded); advance past it on a hit. */
static int kw(const char **pp, const char *e, const char *lit)
{
	const char *q = *pp;
	while (q < e && *lit && lower(*q) == *lit) { q++; lit++; }
	if (*lit) return 0;
	if (q < e && is_sel_name(*q)) return 0;
	*pp = q;
	return 1;
}

/* Parse a pixel length at *pp (digits then an optional unit); -1 if no number. */
static int parse_px(const char **pp, const char *e)
{
	const char *p = skip_sp(*pp, e);
	int n = 0, any = 0;
	while (p < e && *p >= '0' && *p <= '9') { n = n * 10 + (*p++ - '0'); any = 1; }
	while (p < e && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) p++;
	*pp = p;
	return any ? n : -1;
}

/* True if [a,e) trimmed equals lowercase literal `lit`. */
static int span_eq_trim(const char *a, const char *e, const char *lit)
{
	a = skip_sp(a, e);
	while (e > a && (e[-1] == ' ' || e[-1] == '\t')) e--;
	return span_is(a, e, lit);
}

/* Parse one `(feature: value)` starting at *pp (which points at '('); always
 * advances past the matching ')'. A feature we don't support stays MF_UNKNOWN,
 * which evaluates false — per CSS, an unrecognised feature makes its query false
 * (so a block gated only on one we can't test won't apply). */
static void parse_feature(const char **pp, const char *e, struct media_cond *c)
{
	const char *p = *pp;
	if (p >= e || *p != '(') { *pp = e; return; }
	p++;
	const char *ns = skip_sp(p, e), *ne = ns;
	while (ne < e && *ne != ':' && *ne != ')') ne++;
	const char *nend = ne;
	while (nend > ns && (nend[-1] == ' ' || nend[-1] == '\t')) nend--;
	const char *vs = ne, *ve = ne;
	if (ne < e && *ne == ':') { vs = ne + 1; ve = vs; while (ve < e && *ve != ')') ve++; }
	const char *after = (ne < e && *ne == ':') ? ve : ne;
	while (after < e && *after != ')') after++;
	if (after < e) after++;
	*pp = after;

	if (c->nfeat >= MAX_FEAT) return;
	struct media_feature f = { MF_UNKNOWN, 0 };
	const char *vp = vs;
	if      (span_is(ns, nend, "min-width"))  { f.kind = MF_MIN_W; f.val = parse_px(&vp, ve); }
	else if (span_is(ns, nend, "max-width"))  { f.kind = MF_MAX_W; f.val = parse_px(&vp, ve); }
	else if (span_is(ns, nend, "width"))      { f.kind = MF_W;     f.val = parse_px(&vp, ve); }
	else if (span_is(ns, nend, "min-height")) { f.kind = MF_MIN_H; f.val = parse_px(&vp, ve); }
	else if (span_is(ns, nend, "max-height")) { f.kind = MF_MAX_H; f.val = parse_px(&vp, ve); }
	else if (span_is(ns, nend, "height"))     { f.kind = MF_H;     f.val = parse_px(&vp, ve); }
	else if (span_is(ns, nend, "orientation")) {
		if (span_eq_trim(vs, ve, "portrait"))  f.kind = MF_ORIENT_PORTRAIT;
		else if (span_eq_trim(vs, ve, "landscape")) f.kind = MF_ORIENT_LANDSCAPE;
	} else if (span_is(ns, nend, "prefers-color-scheme")) {
		if (span_eq_trim(vs, ve, "dark"))       f.kind = MF_SCHEME_DARK;
		else if (span_eq_trim(vs, ve, "light")) f.kind = MF_SCHEME_LIGHT;
	}
	if ((f.kind == MF_MIN_W || f.kind == MF_MAX_W || f.kind == MF_W ||
	     f.kind == MF_MIN_H || f.kind == MF_MAX_H || f.kind == MF_H) && f.val < 0)
		f.kind = MF_UNKNOWN;         /* width/height feature with no usable number */
	c->feat[c->nfeat++] = f;
}

/* Parse one comma-free media query (`[not|only] type? (and feature)*`) into *c. */
static void parse_cond(const char *s, const char *e, struct media_cond *c)
{
	c->negate = 0; c->mtype = MT_ALL; c->nfeat = 0;
	const char *p = skip_sp(s, e);
	if (kw(&p, e, "not")) c->negate = 1;
	p = skip_sp(p, e);
	kw(&p, e, "only");                       /* `only` is a no-op for us */
	p = skip_sp(p, e);
	if (p < e && *p != '(') {                /* an explicit media type */
		if      (kw(&p, e, "screen")) c->mtype = MT_SCREEN;
		else if (kw(&p, e, "all"))    c->mtype = MT_ALL;
		else if (kw(&p, e, "print"))  c->mtype = MT_PRINT;
		else { while (p < e && is_sel_name(*p)) p++; c->mtype = MT_OTHER; }
	}
	for (;;) {
		p = skip_sp(p, e);
		if (p >= e) break;
		if (kw(&p, e, "and")) p = skip_sp(p, e);
		if (p < e && *p == '(') parse_feature(&p, e, c);
		else break;
	}
}

/* Parse a full @media header [s,e) (comma-separated condition list) into *mq. */
static void parse_media_query(const char *s, const char *e, struct media_query *mq)
{
	mq->n = 0;
	const char *p = s;
	while (p < e && mq->n < MAX_COND) {
		const char *comma = p;
		while (comma < e && *comma != ',') comma++;
		parse_cond(p, comma, &mq->cond[mq->n++]);
		p = (comma < e) ? comma + 1 : e;
	}
}

static int feature_matches(const struct media_feature *f)
{
	switch (f->kind) {
	case MF_MIN_W: return g_vw >= f->val;
	case MF_MAX_W: return g_vw <= f->val;
	case MF_W:     return g_vw == f->val;
	case MF_MIN_H: return g_vh >= f->val;
	case MF_MAX_H: return g_vh <= f->val;
	case MF_H:     return g_vh == f->val;
	case MF_ORIENT_PORTRAIT:  return g_vh >= g_vw;
	case MF_ORIENT_LANDSCAPE: return g_vw >  g_vh;
	case MF_SCHEME_LIGHT: return 1;          /* the browser renders a light theme */
	case MF_SCHEME_DARK:  return 0;
	default:              return 0;          /* MF_UNKNOWN / unsupported feature */
	}
}

static int cond_matches(const struct media_cond *c)
{
	int ok = (c->mtype == MT_ALL || c->mtype == MT_SCREEN);
	for (int i = 0; ok && i < c->nfeat; i++)
		if (!feature_matches(&c->feat[i])) ok = 0;
	return c->negate ? !ok : ok;
}

static int media_matches(const struct media_query *mq)
{
	if (mq->n == 0) return 1;
	for (int i = 0; i < mq->n; i++)
		if (cond_matches(&mq->cond[i])) return 1;
	return 0;
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

/* Pick the first url(...) in a src value naming a font we can decode: a raw SFNT
 * (.ttf/.otf) or a WOFF/WOFF2 container (font_load decodes both via the woff.c
 * linked into the browser). Writes it to url[cap] (empty if none qualifies). */
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
			if (ends_ci(base, ".ttf") || ends_ci(base, ".otf") ||
			    ends_ci(base, ".woff2") || ends_ci(base, ".woff")) {
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
                       const struct css_style *decl, int important,
                       const struct media_query *media)
{
	struct css_rule *r = malloc(sizeof *r);
	if (!r) return;
	r->sel = *sel;
	r->decl = *decl;
	r->media = *media;
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

/* Parse the rules in [p,end) and push each under media query *mq. Recurses for a
 * nested @media block; nested queries don't intersect — the inner one replaces an
 * outer unconditional context, otherwise the outer (active) query is kept, since
 * @media-inside-@media is vanishingly rare. */
static void add_rules(css_sheet *sh, const char *p, const char *end,
                      const struct media_query *mq)
{
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
			if (starts_ci(p, "@media")) {
				const char *hdr = p + 6, *b = hdr;
				while (b < end && *b != '{' && *b != '}' && *b != ';') b++;
				if (b < end && *b == '{') {
					struct media_query nested;
					parse_media_query(hdr, b, &nested);
					const char *db = b + 1, *de = db; int depth = 1;
					while (de < end && depth) { if (*de == '{') depth++; else if (*de == '}') depth--; if (depth) de++; }
					add_rules(sh, db, de, mq->n ? mq : &nested);
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
			if (parse_selector(q, comma, &sel)) sheet_push(sh, &sel, &decl, important, mq);
			q = (comma < sel_e) ? comma + 1 : sel_e;
		}
	}
}

void css_add(css_sheet *sh, const char *src, int len)
{
	if (!sh) return;
	struct media_query none = { .n = 0 };
	add_rules(sh, src, src + len, &none);
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
	if (d->center >= 0) out->center = d->center;
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
		if (media_matches(&r->media) && selector_match(&r->sel, el)) match[nm++] = r;

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

/* ------------------------------------------------ devtools: matched rules --- */
/* Tiny bounded string appenders for reconstructing selector/media text. */
static void cm_ap(char *o, int *k, int cap, const char *s)
{ while (*s && *k < cap - 1) o[(*k)++] = *s++; o[*k] = '\0'; }
static void cm_apn(char *o, int *k, int cap, int v)
{
	char b[12]; int n = 0;
	if (v < 0) { cm_ap(o, k, cap, "-"); v = -v; }
	if (v == 0) { cm_ap(o, k, cap, "0"); return; }
	while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
	while (n && *k < cap - 1) o[(*k)++] = b[--n];
	o[*k] = '\0';
}

/* Reconstruct a readable selector ("div.a#b > span") from the parsed steps. */
static void sel_text(const struct selector *sel, char *o, int cap)
{
	int k = 0; if (cap) o[0] = '\0';
	for (int i = 0; i < sel->n; i++) {
		if (i) cm_ap(o, &k, cap, sel->step[i - 1].child ? " > " : " ");
		const struct sel_step *st = &sel->step[i];
		int start = k;
		if (st->tag[0]) cm_ap(o, &k, cap, st->tag);
		if (st->id[0]) { cm_ap(o, &k, cap, "#"); cm_ap(o, &k, cap, st->id); }
		for (int c = 0; c < st->ncls; c++) { cm_ap(o, &k, cap, "."); cm_ap(o, &k, cap, st->cls[c]); }
		if (k == start) cm_ap(o, &k, cap, "*");
	}
}

/* Reconstruct a readable @media prelude ("" when the rule is unconditional). */
static void media_text(const struct media_query *mq, char *o, int cap)
{
	int k = 0; if (cap) o[0] = '\0';
	if (mq->n == 0) return;
	cm_ap(o, &k, cap, "@media ");
	for (int c = 0; c < mq->n; c++) {
		const struct media_cond *cd = &mq->cond[c];
		if (c) cm_ap(o, &k, cap, ", ");
		if (cd->negate) cm_ap(o, &k, cap, "not ");
		int need_and = 0;
		const char *mt = cd->mtype == MT_SCREEN ? "screen" : cd->mtype == MT_PRINT ? "print" : 0;
		if (mt) { cm_ap(o, &k, cap, mt); need_and = 1; }
		for (int f = 0; f < cd->nfeat; f++) {
			if (need_and) cm_ap(o, &k, cap, " and ");
			need_and = 1;
			cm_ap(o, &k, cap, "(");
			const struct media_feature *ft = &cd->feat[f];
			switch (ft->kind) {
			case MF_MIN_W: cm_ap(o, &k, cap, "min-width: "); cm_apn(o, &k, cap, ft->val); cm_ap(o, &k, cap, "px"); break;
			case MF_MAX_W: cm_ap(o, &k, cap, "max-width: "); cm_apn(o, &k, cap, ft->val); cm_ap(o, &k, cap, "px"); break;
			case MF_W:     cm_ap(o, &k, cap, "width: ");     cm_apn(o, &k, cap, ft->val); cm_ap(o, &k, cap, "px"); break;
			case MF_MIN_H: cm_ap(o, &k, cap, "min-height: "); cm_apn(o, &k, cap, ft->val); cm_ap(o, &k, cap, "px"); break;
			case MF_MAX_H: cm_ap(o, &k, cap, "max-height: "); cm_apn(o, &k, cap, ft->val); cm_ap(o, &k, cap, "px"); break;
			case MF_H:     cm_ap(o, &k, cap, "height: ");    cm_apn(o, &k, cap, ft->val); cm_ap(o, &k, cap, "px"); break;
			case MF_ORIENT_PORTRAIT:  cm_ap(o, &k, cap, "orientation: portrait"); break;
			case MF_ORIENT_LANDSCAPE: cm_ap(o, &k, cap, "orientation: landscape"); break;
			case MF_SCHEME_DARK:  cm_ap(o, &k, cap, "prefers-color-scheme: dark"); break;
			case MF_SCHEME_LIGHT: cm_ap(o, &k, cap, "prefers-color-scheme: light"); break;
			default: cm_ap(o, &k, cap, "...");
			}
			cm_ap(o, &k, cap, ")");
		}
	}
}

int css_matches(css_sheet *sh, const struct dom_node *el, struct css_match *out, int cap)
{
	if (!sh || !el || el->type != DOM_ELEMENT || cap <= 0) return 0;
	struct css_rule *match[256];
	int nm = 0;
	for (struct css_rule *r = sh->head; r && nm < 256; r = r->next)
		if (selector_match(&r->sel, el)) match[nm++] = r;

	/* Most-specific / last-declared first (winner on top, like a real devtools). */
	for (int i = 1; i < nm; i++) {
		struct css_rule *k = match[i];
		int j = i - 1;
		while (j >= 0) {
			struct css_rule *m = match[j];
			int klo = (m->important != k->important) ? (k->important > m->important)
			        : (m->spec != k->spec) ? (k->spec > m->spec)
			        : (k->order > m->order);
			if (!klo) break;
			match[j + 1] = m; j--;
		}
		match[j + 1] = k;
	}

	int n = 0;
	for (int i = 0; i < nm && n < cap; i++) {
		struct css_rule *r = match[i];
		sel_text(&r->sel, out[n].selector, (int)sizeof out[n].selector);
		media_text(&r->media, out[n].media, (int)sizeof out[n].media);
		out[n].active = media_matches(&r->media);
		out[n].decl = r->decl;
		n++;
	}
	return n;
}
