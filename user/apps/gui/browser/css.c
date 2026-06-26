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
#include "color.h"  /* shared colour parser */

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
	int  pseudo_el;        /* PE_* — a ::pseudo-element; never matches a normal box */
	int  child;            /* combinator to the step on the right is '>' */
};
enum { PE_NONE = 0, PE_BEFORE = CSS_PSEUDO_BEFORE, PE_AFTER = CSS_PSEUDO_AFTER, PE_OTHER = 3 };

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

#define CSS_RAW_MAX 256
/* Rule index key: a rule is bucketed by its rightmost compound's most specific
 * simple selector (id, else a class, else tag, else universal), so an element
 * only ever tests rules that can match it — without this, Tailwind's thousands of
 * utility rules made the cascade O(elements × all-rules) and pegged the CPU. */
enum { KK_UNIV = 0, KK_ID, KK_CLS, KK_TAG };
struct css_rule {
	struct selector    sel;
	struct css_style   decl;
	struct media_query media;
	int                spec, order, important;
	unsigned char      has_var;            /* a declaration uses var() -> resolve at fold */
	char               raw[CSS_RAW_MAX];   /* authored declaration text (for devtools) */
	int                keykind;            /* KK_* — which bucket table this rule lives in */
	char               keystr[24];         /* the id/class/tag string for that bucket      */
	struct css_rule   *next;               /* document order (build + fallback iteration)  */
	struct css_rule   *hnext;              /* chain within its index bucket                */
};

/* An @font-face: a declared family name + the raw TTF/OTF src url. */
struct css_face { char family[CSS_FAM_MAX]; char *url; struct css_face *next; };

#define CSS_NBUCK 256
struct css_sheet {
	struct css_rule *head, *tail; int order; int gen; struct css_face *faces;
	/* Rule index (rebuilt lazily when `gen` changes; see ensure_index). */
	struct css_rule *id_b[CSS_NBUCK], *cls_b[CSS_NBUCK], *tag_b[CSS_NBUCK], *uni;
	int idx_gen;
};

/* CSS custom-property table (built per active sheet; see "custom properties"). */
#define CSS_VARS_MAX  256
#define CSS_VARNAME   64
#define CSS_VARVAL    220
static struct { char name[CSS_VARNAME], val[CSS_VARVAL]; } g_vars[CSS_VARS_MAX];
static int        g_nvars;
static css_sheet *g_vars_sheet;
static int        g_vars_gen = -1;
static int  str_uses_var(const char *s);
static void resolve_vars(const char *in, int inlen, char *out, int *on, int cap, int depth);

/* Viewport the @media features test against; the browser refreshes it per cascade. */
static int g_vw = 800, g_vh = 600;
void css_set_viewport(int width, int height)
{
	g_vw = width  > 0 ? width  : 0;
	g_vh = height > 0 ? height : 0;
}

/* ------------------------------------------------ value parsing ------------ */
/* Colour parsing lives in color.c now (shared with svg.c). */
static int parse_color(const char *v) { return color_parse(v); }


/* Parse a CSS length. Returns px>=0 (also for unitless). For "N%", returns -1 and
 * sets *pct = N. For auto/em/rem/vw/calc and anything unsupported, returns -1 with
 * *pct = -1. (Fractions are floored — we render integer px.) */
static int parse_len2(const char *v, int *pct)
{
	*pct = -1;
	while (*v == ' ' || *v == '\t') v++;
	int neg = (*v == '-'); if (*v == '-' || *v == '+') v++;
	int n = 0, any = 0;
	while (*v >= '0' && *v <= '9') { n = n * 10 + (*v++ - '0'); any = 1; }
	if (*v == '.') { v++; while (*v >= '0' && *v <= '9') v++; }   /* drop fraction */
	if (!any) return -1;
	while (*v == ' ') v++;
	if (*v == '%') { if (!neg) *pct = n; return -1; }
	if (*v == 'e' || *v == 'r' || *v == 'v' || *v == 'c')   /* em/ex, rem, vw, cm/ch */
		return -1;
	return neg ? -1 : n;   /* negative px (rare, e.g. margins) treated as unset */
}

/* Parse a CSS length: integer px (or unitless), else -1 (%, em, auto, calc...). */
static int parse_len(const char *v) { int pct; return parse_len2(v, &pct); }

/* One length for a single box side: px>=0, -2 for "auto" (margins only), else -1. */
static short one_len(const char *v, int allow_auto)
{
	while (*v == ' ' || *v == '\t') v++;
	if (allow_auto && starts_ci(v, "auto")) return -2;
	int pct, px = parse_len2(v, &pct);
	return (short)(px >= 0 ? px : -1);
}

/* Expand a 1-4 value box shorthand ("8px 16px") into out[4] = {T,R,B,L}. */
static void parse_box4(const char *v, short out[4], int allow_auto)
{
	short vals[4]; int n = 0;
	while (*v && n < 4) {
		while (*v == ' ' || *v == '\t') v++;
		if (!*v) break;
		vals[n++] = one_len(v, allow_auto);
		while (*v && *v != ' ' && *v != '\t') v++;
	}
	if (n == 0) return;
	short t, r, b, l;
	if (n == 1)      { t = r = b = l = vals[0]; }
	else if (n == 2) { t = b = vals[0]; r = l = vals[1]; }
	else if (n == 3) { t = vals[0]; r = l = vals[1]; b = vals[2]; }
	else             { t = vals[0]; r = vals[1]; b = vals[2]; l = vals[3]; }
	out[0] = t; out[1] = r; out[2] = b; out[3] = l;
}

/* A `border` / `border-<side>` shorthand: pull a width (length or thin/medium/
 * thick; none/hidden = 0) and a colour out of the value; *w / *col stay -1 if
 * absent. Border style keywords (solid/dashed/...) are ignored. */
static void parse_border_val(const char *v, int *w, int *col)
{
	*w = -1; *col = -1;
	while (*v) {
		while (*v == ' ' || *v == '\t') v++;
		if (!*v) break;
		char tok[64]; int k = 0;
		while (v[k] && v[k] != ' ' && v[k] != '\t' && k < (int)sizeof tok - 1) { tok[k] = v[k]; k++; }
		tok[k] = '\0';
		if (starts_ci(tok, "thin"))        *w = 1;
		else if (starts_ci(tok, "medium")) *w = 3;
		else if (starts_ci(tok, "thick"))  *w = 5;
		else if (starts_ci(tok, "none") || starts_ci(tok, "hidden")) *w = 0;
		else {
			int pct, px = parse_len2(tok, &pct);
			if (px >= 0) *w = px;
			else { int c = parse_color(tok); if (c >= 0) *col = c; }
		}
		v += k;
	}
}

/* SVG paint value: -2 none/transparent, -3 currentColor, 0xRRGGBB, or -1 if not a
 * recognised colour (e.g. url(#gradient) — left unset so it doesn't override). */
static int parse_paint_val(const char *v)
{
	while (*v == ' ' || *v == '\t') v++;
	if (starts_ci(v, "none") || starts_ci(v, "transparent")) return -2;
	if (starts_ci(v, "currentcolor")) return -3;
	return parse_color(v);
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

/* Decode a `content` value into out[cap]: a quoted string (with \HHHH unicode and
 * \" / \\ escapes) -> UTF-8; none/normal/empty or a non-string (attr()/counter()/
 * unresolved var) -> "" (rendered as nothing). */
static void parse_content(const char *v, char *out, int cap)
{
	out[0] = '\0';
	while (*v == ' ' || *v == '\t') v++;
	if (*v != '"' && *v != '\'') return;        /* only literal strings */
	char q = *v++;
	int k = 0;
	while (*v && *v != q && k < cap - 1) {
		if (*v == '\\') {
			v++;
			int h = 0, n = 0;
			while (n < 6 && ((*v>='0'&&*v<='9')||(*v>='a'&&*v<='f')||(*v>='A'&&*v<='F'))) {
				int d = (*v<='9')?*v-'0':((*v|32)-'a'+10); h = h*16 + d; v++; n++;
			}
			if (n) {                            /* \HHHH unicode escape -> UTF-8 */
				unsigned cp = (unsigned)h;
				if (*v == ' ') v++;             /* one trailing space terminates the escape */
				if (cp < 0x80) { if (k < cap-1) out[k++] = (char)cp; }
				else if (cp < 0x800) { if (k+2 < cap) { out[k++]=0xC0|(cp>>6); out[k++]=0x80|(cp&0x3F); } }
				else if (cp < 0x10000) { if (k+3 < cap) { out[k++]=0xE0|(cp>>12); out[k++]=0x80|((cp>>6)&0x3F); out[k++]=0x80|(cp&0x3F); } }
				else { if (k+4 < cap) { out[k++]=0xF0|(cp>>18); out[k++]=0x80|((cp>>12)&0x3F); out[k++]=0x80|((cp>>6)&0x3F); out[k++]=0x80|(cp&0x3F); } }
			} else if (*v) {                    /* \" or \\ etc. */
				out[k++] = *v++;
			}
		} else {
			out[k++] = *v++;
		}
	}
	out[k] = '\0';
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
	s->max_w = s->max_h = -1;
	s->pct_w = s->pct_h = s->pct_maxw = s->pct_maxh = -1;
	s->margin[0] = s->margin[1] = s->margin[2] = s->margin[3] = -1;
	s->padding[0] = s->padding[1] = s->padding[2] = s->padding[3] = -1;
	s->border_w[0] = s->border_w[1] = s->border_w[2] = s->border_w[3] = -1;
	s->border_color = -1;
	s->pos = -1;
	s->top = s->right = s->bottom = s->left = -1;
	s->flex = s->flex_dir = -1;
	s->flex_grow = -1;
	s->overflow = -1;
	s->fill = s->stroke = -1;
	s->content[0] = '\0';
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
	int pct;            /* scratch for parse_len2's percent out-param */
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
	else if (span_is(p, pe, "display"))          { out->disp = parse_disp(val); out->flex = has_ci(val, "flex") ? 1 : 0; }
	else if (span_is(p, pe, "text-align"))
		out->center = has_ci(val, "center") ? 1 : (has_ci(val, "left") || has_ci(val, "right") || has_ci(val, "justify") ? 0 : out->center);
	else if (span_is(p, pe, "width"))            { out->width  = parse_len2(val, &pct); out->pct_w = (short)pct; }
	else if (span_is(p, pe, "height"))           { out->height = parse_len2(val, &pct); out->pct_h = (short)pct; }
	else if (span_is(p, pe, "max-width"))        { out->max_w  = parse_len2(val, &pct); out->pct_maxw = (short)pct; }
	else if (span_is(p, pe, "max-height"))       { out->max_h  = parse_len2(val, &pct); out->pct_maxh = (short)pct; }
	else if (span_is(p, pe, "margin"))           parse_box4(val, out->margin, 1);
	else if (span_is(p, pe, "margin-top"))       out->margin[0]  = one_len(val, 1);
	else if (span_is(p, pe, "margin-right"))     out->margin[1]  = one_len(val, 1);
	else if (span_is(p, pe, "margin-bottom"))    out->margin[2]  = one_len(val, 1);
	else if (span_is(p, pe, "margin-left"))      out->margin[3]  = one_len(val, 1);
	else if (span_is(p, pe, "padding"))          parse_box4(val, out->padding, 0);
	else if (span_is(p, pe, "padding-top"))      out->padding[0] = one_len(val, 0);
	else if (span_is(p, pe, "padding-right"))    out->padding[1] = one_len(val, 0);
	else if (span_is(p, pe, "padding-bottom"))   out->padding[2] = one_len(val, 0);
	else if (span_is(p, pe, "padding-left"))     out->padding[3] = one_len(val, 0);
	else if (span_is(p, pe, "border-width"))     parse_box4(val, out->border_w, 0);
	else if (span_is(p, pe, "border-color"))     { int c = parse_color(val); if (c >= 0) out->border_color = c; }
	else if (span_is(p, pe, "border") || span_is(p, pe, "border-top") || span_is(p, pe, "border-right") ||
	         span_is(p, pe, "border-bottom") || span_is(p, pe, "border-left")) {
		int w, c; parse_border_val(val, &w, &c);
		int s = span_is(p, pe, "border-top") ? 0 : span_is(p, pe, "border-right") ? 1 :
		        span_is(p, pe, "border-bottom") ? 2 : span_is(p, pe, "border-left") ? 3 : -1;
		if (w >= 0) { if (s < 0) out->border_w[0] = out->border_w[1] = out->border_w[2] = out->border_w[3] = (short)w; else out->border_w[s] = (short)w; }
		if (c >= 0) out->border_color = c;
	}
	else if (span_is(p, pe, "border-top-width"))    out->border_w[0] = one_len(val, 0);
	else if (span_is(p, pe, "border-right-width"))  out->border_w[1] = one_len(val, 0);
	else if (span_is(p, pe, "border-bottom-width")) out->border_w[2] = one_len(val, 0);
	else if (span_is(p, pe, "border-left-width"))   out->border_w[3] = one_len(val, 0);
	else if (span_is(p, pe, "flex-direction"))   out->flex_dir = has_ci(val, "column") ? 1 : 0;
	else if (span_is(p, pe, "flex-grow"))        { int g = parse_len(val); if (g >= 0) out->flex_grow = (short)g; }
	else if (span_is(p, pe, "flex")) {
		const char *f = val; while (*f == ' ') f++;
		if (starts_ci(f, "none")) out->flex_grow = 0;
		else { int g = parse_len(f); out->flex_grow = (short)(g >= 0 ? g : 1); }   /* flex:1 / auto -> grow 1 */
	}
	else if (span_is(p, pe, "position")) {
		const char *f = val; while (*f == ' ') f++;
		out->pos = starts_ci(f, "relative") ? CSS_POS_REL : starts_ci(f, "absolute") ? CSS_POS_ABS :
		           starts_ci(f, "fixed") ? CSS_POS_FIXED : starts_ci(f, "sticky") ? CSS_POS_STICKY : CSS_POS_STATIC;
	}
	else if (span_is(p, pe, "top"))              out->top    = parse_len(val);
	else if (span_is(p, pe, "right"))            out->right  = parse_len(val);
	else if (span_is(p, pe, "bottom"))           out->bottom = parse_len(val);
	else if (span_is(p, pe, "left"))             out->left   = parse_len(val);
	else if (span_is(p, pe, "overflow") || span_is(p, pe, "overflow-x") || span_is(p, pe, "overflow-y"))
		out->overflow = (has_ci(val, "hidden") || has_ci(val, "auto") || has_ci(val, "scroll") || has_ci(val, "clip")) ? 1 : 0;
	else if (span_is(p, pe, "fill"))             out->fill   = parse_paint_val(val);
	else if (span_is(p, pe, "stroke"))           out->stroke = parse_paint_val(val);
	else if (span_is(p, pe, "content"))          parse_content(val, out->content, sizeof out->content);
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
	if (str_uses_var(decls)) {                  /* resolve var() against the sheet table */
		char buf[512]; int bn = 0;
		resolve_vars(decls, (int)strlen(decls), buf, &bn, (int)sizeof buf, 0);
		parse_decls(buf, buf + bn, out, &imp);
		return;
	}
	parse_decls(decls, decls + strlen(decls), out, &imp);
}

/* ------------------------------------------------ selectors ---------------- */
static int is_sel_name(char c)
{ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'; }

/* Parse one compound selector token (between combinators) into *st. */
static void parse_compound(const char *s, const char *e, struct sel_step *st)
{
	st->tag[0] = '\0'; st->id[0] = '\0'; st->ncls = 0; st->pseudo_el = 0;
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
		} else if (*s == ':') {                 /* pseudo: skip, but flag elements */
			s++;
			int dbl = 0;
			if (s < e && *s == ':') { s++; dbl = 1; }
			const char *ns = s;
			while (s < e && is_sel_name(*s)) s++;
			/* A pseudo-ELEMENT styles generated content / selection / a part, not
			 * the element's own box. ::before/::after generate renderable content
			 * (PE_BEFORE/PE_AFTER); the rest (::selection, ::placeholder, ::first-
			 * line, …) we don't render, so the rule must not match a real element
			 * (e.g. `html::selection{background:…}` is NOT the page background). */
			if (span_is(ns, s, "before"))           st->pseudo_el = PE_BEFORE;
			else if (span_is(ns, s, "after"))       st->pseudo_el = PE_AFTER;
			else if (dbl || span_is(ns, s, "first-line") || span_is(ns, s, "first-letter"))
				st->pseudo_el = PE_OTHER;
			if (s < e && *s == '(') { while (s < e && *s != ')') s++; if (s < e) s++; }
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

/* Match a step's tag/id/class against `n` (ignoring any pseudo-element). */
static int step_box_match(const struct sel_step *st, const struct dom_node *n)
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

static int step_match(const struct sel_step *st, const struct dom_node *n)
{
	if (st->pseudo_el) return 0;            /* ::before/::selection/… don't style a box */
	return step_box_match(st, n);
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

/* Match a ::before / ::after rule against its originating element `el`: the
 * rightmost compound must carry that pseudo-element, its box part must match el,
 * and the ancestor chain matches as usual. */
static int selector_match_pseudo(const struct selector *sel, const struct dom_node *el, int which)
{
	int i = sel->n - 1;
	if (sel->step[i].pseudo_el != which) return 0;
	if (!step_box_match(&sel->step[i], el)) return 0;
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
	s->gen = 0;
	s->faces = 0;
	for (int i = 0; i < CSS_NBUCK; i++) { s->id_b[i] = s->cls_b[i] = s->tag_b[i] = 0; }
	s->uni = 0;
	s->idx_gen = -1;             /* index is stale until first ensure_index */
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
                       const struct media_query *media,
                       const char *dec_s, const char *dec_e)
{
	struct css_rule *r = malloc(sizeof *r);
	if (!r) return;
	r->sel = *sel;
	r->decl = *decl;
	r->media = *media;
	r->spec = selector_spec(sel);
	r->order = sh->order++;
	r->important = important;
	{   /* index key = the rightmost compound's most specific simple selector */
		const struct sel_step *st = &sel->step[sel->n - 1];
		const char *ks = 0;
		if (st->id[0])        { r->keykind = KK_ID;  ks = st->id; }
		else if (st->ncls)    { r->keykind = KK_CLS; ks = st->cls[0]; }
		else if (st->tag[0])  { r->keykind = KK_TAG; ks = st->tag; }
		else                  { r->keykind = KK_UNIV; }
		int k = 0;
		if (ks) for (; ks[k] && k < (int)sizeof r->keystr - 1; k++) r->keystr[k] = ks[k];
		r->keystr[k] = '\0';
	}
	{   /* copy the authored declarations, collapsing runs of whitespace */
		int k = 0, sp = 0;
		const char *q = dec_s;
		while (q < dec_e && k < CSS_RAW_MAX - 1) {
			char c = *q++;
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') { sp = 1; continue; }
			if (sp && k > 0) r->raw[k++] = ' ';
			sp = 0;
			if (k < CSS_RAW_MAX - 1) r->raw[k++] = c;
		}
		r->raw[k] = '\0';
		{ const char *q = r->raw; r->has_var = 0; for (; *q; q++) if ((*q == 'v' || *q == 'V') && starts_ci(q, "var(")) { r->has_var = 1; break; } }
	}
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
			/* @layer / @supports: flatten the inner rules into the current context.
			 * @layer's cascade-priority ordering is approximated by source order
			 * (good enough for Tailwind, which emits layers in priority order); a
			 * @supports condition is treated as satisfied (its rules are the modern
			 * branch, and any value we can't parse is ignored anyway). The bare
			 * `@layer a, b;` order-declaration form has no block and falls through to
			 * skip_atrule. Without this, Tailwind v4 — which wraps base resets, the
			 * :root variables and every utility in @layer — renders almost nothing. */
			if (starts_ci(p, "@layer") || starts_ci(p, "@supports")) {
				const char *b = p;
				while (b < end && *b != '{' && *b != ';') b++;
				if (b < end && *b == '{') {
					const char *db = b + 1, *de = db; int depth = 1;
					while (de < end && depth) { if (*de == '{') depth++; else if (*de == '}') depth--; if (depth) de++; }
					add_rules(sh, db, de, mq);
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
			if (parse_selector(q, comma, &sel)) sheet_push(sh, &sel, &decl, important, mq, dec_s, dec_e);
			q = (comma < sel_e) ? comma + 1 : sel_e;
		}
	}
}

void css_add(css_sheet *sh, const char *src, int len)
{
	if (!sh) return;
	struct media_query none = { .n = 0 };
	add_rules(sh, src, src + len, &none);
	sh->gen++;                            /* custom-property table is now stale */
}

void css_free(css_sheet *sh)
{
	if (!sh) return;
	if (sh == g_vars_sheet) { g_vars_sheet = 0; g_vars_gen = -1; g_nvars = 0; }   /* drop stale var cache */
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
	if (d->width >= 0)       { out->width = d->width; out->pct_w = -1; }
	else if (d->pct_w >= 0)  { out->pct_w = d->pct_w; out->width = -1; }
	if (d->height >= 0)      { out->height = d->height; out->pct_h = -1; }
	else if (d->pct_h >= 0)  { out->pct_h = d->pct_h; out->height = -1; }
	if (d->max_w >= 0)       { out->max_w = d->max_w; out->pct_maxw = -1; }
	else if (d->pct_maxw >= 0) { out->pct_maxw = d->pct_maxw; out->max_w = -1; }
	if (d->max_h >= 0)       { out->max_h = d->max_h; out->pct_maxh = -1; }
	else if (d->pct_maxh >= 0) { out->pct_maxh = d->pct_maxh; out->max_h = -1; }
	for (int i = 0; i < 4; i++) {
		if (d->margin[i]   != -1) out->margin[i]   = d->margin[i];
		if (d->padding[i]  != -1) out->padding[i]  = d->padding[i];
		if (d->border_w[i] != -1) out->border_w[i] = d->border_w[i];
	}
	if (d->border_color >= 0) out->border_color = d->border_color;
	if (d->pos >= 0)        out->pos      = d->pos;
	if (d->top != -1)       out->top      = d->top;
	if (d->right != -1)     out->right    = d->right;
	if (d->bottom != -1)    out->bottom   = d->bottom;
	if (d->left != -1)      out->left     = d->left;
	if (d->flex >= 0)       out->flex     = d->flex;
	if (d->flex_dir >= 0)   out->flex_dir = d->flex_dir;
	if (d->flex_grow >= 0)  out->flex_grow = d->flex_grow;
	if (d->overflow >= 0)   out->overflow = d->overflow;
	if (d->fill != -1)   out->fill   = d->fill;
	if (d->stroke != -1) out->stroke = d->stroke;
	if (d->content[0]) { int i = 0; for (; d->content[i] && i < (int)sizeof out->content - 1; i++) out->content[i] = d->content[i]; out->content[i] = '\0'; }
	if (d->fam[0]) { int i = 0; for (; d->fam[i] && i < CSS_FAM_STACK - 1; i++) out->fam[i] = d->fam[i]; out->fam[i] = '\0'; }
}

/* ------------------------------------------------ custom properties -------- *
 * CSS variables. Authored `--name: value` declarations are collected sheet-wide
 * into a table (a pragmatic model: variables are nearly always defined on :root,
 * and we treat them as global, last-declaration-wins, honouring @media). At fold
 * time, a declaration that uses var(--name, fallback) is resolved against this
 * table (recursively) before it's interpreted. */
static const char *var_lookup(const char *name, int len)
{
	for (int i = 0; i < g_nvars; i++) {
		int k = 0; const char *nm = g_vars[i].name;
		while (k < len && nm[k] && nm[k] == name[k]) k++;
		if (k == len && !nm[k]) return g_vars[i].val;
	}
	return 0;
}
static void var_store(const char *name, int nlen, const char *val, int vlen)
{
	int slot = -1;
	if (nlen <= 0 || nlen >= CSS_VARNAME) return;
	for (int i = 0; i < g_nvars; i++) {                 /* later wins: overwrite */
		int k = 0; const char *nm = g_vars[i].name;
		while (k < nlen && nm[k] == name[k]) k++;
		if (k == nlen && !nm[k]) { slot = i; break; }
	}
	if (slot < 0) { if (g_nvars >= CSS_VARS_MAX) return; slot = g_nvars++; }
	{ int i = 0; for (; i < nlen; i++) g_vars[slot].name[i] = name[i]; g_vars[slot].name[i] = '\0'; }
	if (vlen >= CSS_VARVAL) vlen = CSS_VARVAL - 1;
	{ int i = 0; for (; i < vlen; i++) g_vars[slot].val[i] = val[i]; g_vars[slot].val[i] = '\0'; }
}

/* Pull every `--name: value` declaration out of `raw` into the table. */
static void vars_from_raw(const char *raw)
{
	const char *p = raw;
	while (*p) {
		const char *seg = p, *colon, *e;
		while (*p && *p != ';') p++;
		e = p;
		colon = seg; while (colon < e && *colon != ':') colon++;
		while (seg < colon && *seg == ' ') seg++;
		if (colon < e && seg + 1 < colon && seg[0] == '-' && seg[1] == '-') {
			const char *vs = colon + 1, *ve = e;
			while (vs < ve && *vs == ' ') vs++;
			while (ve > vs && ve[-1] == ' ') ve--;
			var_store(seg, (int)(colon - seg), vs, (int)(ve - vs));
		}
		if (*p == ';') p++;
	}
}
static void ensure_vars(css_sheet *sh)
{
	if (sh == g_vars_sheet && sh->gen == g_vars_gen) return;
	g_nvars = 0;
	for (struct css_rule *r = sh->head; r; r = r->next)
		if (media_matches(&r->media)) vars_from_raw(r->raw);   /* active rules only */
	g_vars_sheet = sh; g_vars_gen = sh->gen;
}

static int str_uses_var(const char *s)
{
	for (; *s; s++) if ((*s == 'v' || *s == 'V') && starts_ci(s, "var(")) return 1;
	return 0;
}
/* Copy `in`, substituting var(--name, fallback) from the table (recursive, depth-
 * bounded). Unknown var with no fallback expands to empty, as in real CSS. */
static void resolve_vars(const char *in, int inlen, char *out, int *on, int cap, int depth)
{
	int i = 0;
	while (i < inlen && *on < cap - 1) {
		if ((in[i] == 'v' || in[i] == 'V') && i + 4 <= inlen && starts_ci(in + i, "var(")) {
			int ns, ne, fbs = 0, fbe = 0, paren = 1;
			const char *val;
			i += 4;
			ns = i;
			while (i < inlen && in[i] != ',' && in[i] != ')') i++;
			ne = i;
			if (i < inlen && in[i] == ',') {            /* fallback (balanced parens) */
				i++; fbs = i;
				while (i < inlen && paren) { if (in[i] == '(') paren++; else if (in[i] == ')') { paren--; if (!paren) break; } i++; }
				fbe = i;
			} else { while (i < inlen && in[i] != ')') i++; }
			if (i < inlen && in[i] == ')') i++;
			while (ns < ne && in[ns] == ' ') ns++;
			while (ne > ns && in[ne - 1] == ' ') ne--;
			val = var_lookup(in + ns, ne - ns);
			if (val && depth < 8) resolve_vars(val, (int)strlen(val), out, on, cap, depth + 1);
			else if (val) { for (int k = 0; val[k] && *on < cap - 1; k++) out[(*on)++] = val[k]; }
			else if (fbe > fbs) { while (fbs < fbe && in[fbs] == ' ') fbs++; resolve_vars(in + fbs, fbe - fbs, out, on, cap, depth + 1); }
		} else {
			out[(*on)++] = in[i++];
		}
	}
	out[*on] = '\0';
}

/* ------------------------------------------------ rule index -------------- *
 * Bucket rules by their rightmost key so an element tests only the rules that can
 * possibly match it (its tag / each class / its id) plus the universal ones —
 * instead of every rule in the sheet. Rebuilt lazily when the sheet changes. */
static unsigned css_hash(const char *s)
{
	unsigned h = 5381;
	for (; *s; s++) h = h * 33u + (unsigned char)*s;
	return h % CSS_NBUCK;
}
static void ensure_index(css_sheet *sh)
{
	if (sh->idx_gen == sh->gen) return;
	for (int i = 0; i < CSS_NBUCK; i++) { sh->id_b[i] = sh->cls_b[i] = sh->tag_b[i] = 0; }
	sh->uni = 0;
	for (struct css_rule *r = sh->head; r; r = r->next) {
		struct css_rule **head =
			r->keykind == KK_ID  ? &sh->id_b[css_hash(r->keystr)] :
			r->keykind == KK_CLS ? &sh->cls_b[css_hash(r->keystr)] :
			r->keykind == KK_TAG ? &sh->tag_b[css_hash(r->keystr)] : &sh->uni;
		r->hnext = *head; *head = r;
	}
	sh->idx_gen = sh->gen;
}

/* Copy `s` truncated to 23 chars (the rule keystr width) into out[24] so element
 * keys hash to the same bucket as the rules that were truncated at parse time. */
static void key23(const char *s, char *out)
{
	int k = 0;
	for (; s[k] && k < 23; k++) out[k] = s[k];
	out[k] = '\0';
}

/* Candidate rules for `el`: the universal bucket + its tag/id/class buckets. A
 * rule lives in exactly one bucket, so there are no duplicates; selector_match
 * still confirms each (the buckets are a superset filter). */
static int collect_candidates(css_sheet *sh, const struct dom_node *el,
                              struct css_rule **out, int cap)
{
	ensure_index(sh);
	int n = 0;
	for (struct css_rule *r = sh->uni; r && n < cap; r = r->hnext) out[n++] = r;
	if (el->tag[0])
		for (struct css_rule *r = sh->tag_b[css_hash(el->tag)]; r && n < cap; r = r->hnext)
			if (streq(r->keystr, el->tag)) out[n++] = r;
	const char *id = dom_get_attr(el, "id");
	if (id && id[0]) {
		char b[24]; key23(id, b);
		for (struct css_rule *r = sh->id_b[css_hash(b)]; r && n < cap; r = r->hnext)
			if (streq_ci(r->keystr, b)) out[n++] = r;
	}
	const char *cl = dom_get_attr(el, "class");
	if (cl) for (const char *p = cl; *p; ) {
		while (*p == ' ' || *p == '\t') p++;
		char b[24]; int k = 0;
		while (*p && *p != ' ' && *p != '\t') { if (k < 23) b[k++] = *p; p++; }
		b[k] = '\0';
		if (k) for (struct css_rule *r = sh->cls_b[css_hash(b)]; r && n < cap; r = r->hnext)
			if (streq(r->keystr, b)) out[n++] = r;
	}
	return n;
}

void css_apply_sheet(css_sheet *sh, const struct dom_node *el, struct css_style *out)
{
	if (!sh || !el || el->type != DOM_ELEMENT) return;
	ensure_vars(sh);

	/* Collect matches, then fold in ascending priority (important, spec, order)
	 * so the winner is applied last. */
	struct css_rule *cand[512]; struct css_rule *match[256];
	int nc = collect_candidates(sh, el, cand, 512), nm = 0;
	for (int i = 0; i < nc && nm < 256; i++)
		if (media_matches(&cand[i]->media) && selector_match(&cand[i]->sel, el)) match[nm++] = cand[i];

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
	for (int i = 0; i < nm; i++) {
		struct css_rule *r = match[i];
		if (r->has_var) {                       /* re-parse with var() substituted */
			char buf[512]; int bn = 0;
			struct css_style tmp; int imp = 0;
			resolve_vars(r->raw, (int)strlen(r->raw), buf, &bn, (int)sizeof buf, 0);
			css_style_reset(&tmp);
			parse_decls(buf, buf + bn, &tmp, &imp);
			fold(out, &tmp);
		} else fold(out, &r->decl);
	}
}

int css_pseudo_content(css_sheet *sh, const struct dom_node *el, int which,
                       struct css_style *out)
{
	css_style_reset(out);
	if (!sh || !el || el->type != DOM_ELEMENT) return 0;
	ensure_vars(sh);
	struct css_rule *cand[512]; struct css_rule *match[256];
	int nc = collect_candidates(sh, el, cand, 512), nm = 0;
	for (int i = 0; i < nc && nm < 256; i++)
		if (media_matches(&cand[i]->media) && selector_match_pseudo(&cand[i]->sel, el, which))
			match[nm++] = cand[i];
	for (int i = 1; i < nm; i++) {           /* ascending priority: winner folds last */
		struct css_rule *k = match[i];
		int j = i - 1;
		while (j >= 0) {
			struct css_rule *m = match[j];
			int hi = (m->important != k->important) ? (m->important > k->important)
			       : (m->spec != k->spec) ? (m->spec > k->spec) : (m->order > k->order);
			if (!hi) break;
			match[j + 1] = m; j--;
		}
		match[j + 1] = k;
	}
	for (int i = 0; i < nm; i++) {
		struct css_rule *r = match[i];
		if (r->has_var) {
			char buf[512]; int bn = 0; struct css_style tmp; int imp = 0;
			resolve_vars(r->raw, (int)strlen(r->raw), buf, &bn, (int)sizeof buf, 0);
			css_style_reset(&tmp);
			parse_decls(buf, buf + bn, &tmp, &imp);
			fold(out, &tmp);
		} else fold(out, &r->decl);
	}
	return out->content[0] != '\0';
}

void css_apply_attrs(const struct dom_node *el, struct css_style *out)
{	if (!el || el->type != DOM_ELEMENT) return;
	const char *c = dom_get_attr(el, "color");
	if (c) { int rgb = parse_color(c); if (rgb >= 0) out->color = rgb; }
	const char *bg = dom_get_attr(el, "bgcolor");
	if (bg) { int rgb = parse_color(bg); if (rgb >= 0) out->bg = rgb; }
	const char *w = dom_get_attr(el, "width");
	if (w) { int px = parse_len(w); if (px >= 0) out->width = px; }
	const char *h = dom_get_attr(el, "height");
	if (h) { int px = parse_len(h); if (px >= 0) out->height = px; }
}

/* ------------------------------------------------ page background (alpha) -- *
 * A background-color may be translucent; per CSS it composites over what's behind
 * it (an element's box over its ancestors; the root's over the canvas). css_bg_argb
 * returns an element's cascade-winning background as 0xAARRGGBB via *out, and the
 * caller composites <html> then <body> over a white canvas — so a Tailwind
 * /var(--tw-bg-opacity) tint blends rather than being treated as opaque. The
 * colour + its alpha both come from color_parse_a (color.c). */

/* Pull the (last) background[-color] value out of a declaration block, resolving
 * var() against the sheet table, into out[cap]. Returns 1 if one was present. */
static int extract_bg(const char *decls, char *out, int cap)
{
	const char *bv = 0, *bve = 0, *s = decls;
	while (*s) {
		const char *semi = s; while (*semi && *semi != ';') semi++;
		const char *colon = s; while (colon < semi && *colon != ':') colon++;
		if (colon < semi) {
			const char *p = s, *pe = colon;
			while (p < pe && (*p == ' ' || *p == '\t')) p++;
			while (pe > p && (pe[-1] == ' ' || pe[-1] == '\t')) pe--;
			if (span_is(p, pe, "background-color") || span_is(p, pe, "background")) { bv = colon + 1; bve = semi; }
		}
		s = *semi ? semi + 1 : semi;
	}
	if (!bv) return 0;
	while (bv < bve && (*bv == ' ' || *bv == '\t')) bv++;
	while (bve > bv && (bve[-1] == ' ' || bve[-1] == '\t')) bve--;
	int on = 0;
	resolve_vars(bv, (int)(bve - bv), out, &on, cap, 0);   /* verbatim copy when no var() */
	return 1;
}

int css_bg_argb(css_sheet *sh, const struct dom_node *el, unsigned *out)
{
	if (!sh || !el || el->type != DOM_ELEMENT) return 0;
	ensure_vars(sh);
	char val[160];

	/* highest priority first: inline style="", then sheet rules, then bgcolor="" */
	const char *inl = dom_get_attr(el, "style");
	if (inl && extract_bg(inl, val, sizeof val)) {
		int a; int c = color_parse_a(val, &a); if (c >= 0) { *out = ((unsigned)a << 24) | (unsigned)c; return 1; }
	}

	struct css_rule *cand[512]; struct css_rule *match[256];
	int nc = collect_candidates(sh, el, cand, 512), nm = 0;
	for (int i = 0; i < nc && nm < 256; i++)
		if (media_matches(&cand[i]->media) && selector_match(&cand[i]->sel, el)) match[nm++] = cand[i];
	for (int i = 1; i < nm; i++) {           /* sort highest priority first */
		struct css_rule *k = match[i]; int j = i - 1;
		while (j >= 0) {
			struct css_rule *m = match[j];
			int klo = (m->important != k->important) ? (k->important > m->important)
			        : (m->spec != k->spec) ? (k->spec > m->spec) : (k->order > m->order);
			if (!klo) break;
			match[j + 1] = m; j--;
		}
		match[j + 1] = k;
	}
	for (int i = 0; i < nm; i++)
		if (extract_bg(match[i]->raw, val, sizeof val)) {
			int a; int c = color_parse_a(val, &a); if (c >= 0) { *out = ((unsigned)a << 24) | (unsigned)c; return 1; }
		}

	const char *bgc = dom_get_attr(el, "bgcolor");
	if (bgc) { int c = parse_color(bgc); if (c >= 0) { *out = 0xff000000u | (unsigned)c; return 1; } }
	return 0;
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
	struct css_rule *cand[512]; struct css_rule *match[256];
	int nc = collect_candidates(sh, el, cand, 512), nm = 0;
	for (int i = 0; i < nc && nm < 256; i++)
		if (selector_match(&cand[i]->sel, el)) match[nm++] = cand[i];

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
		out[n].raw = r->raw;
		n++;
	}
	return n;
}
