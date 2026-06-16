/* svg.c — a small SVG renderer. dom_parse() gives the element tree (SVG is XML);
 * this walks it, accumulating affine transforms and paint, and rasterizes the
 * shapes into an ARGB bitmap with a scanline polygon filler.
 *
 * Supported: rect, circle, ellipse, line, polyline, polygon, path (M/L/H/V/C/S/
 * Q/T/Z, abs+rel; A and complex grammar omitted); fill (nonzero) + stroke;
 * fill/stroke/style colours (#hex, rgb(), named, none, currentColor); transforms
 * (translate/scale/rotate/matrix); viewBox + width/height with aspect preserved.
 * Not supported: gradients, patterns, filters, text, clipping, arcs. Edges are
 * antialiased by rendering at SS× and box-downsampling. */
#include "owos.h"
#include "dom.h"
#include "svg.h"

#define SS_MAX 3            /* supersample factor cap (AA quality vs memory) */

/* ------------------------------------------------ tiny float helpers ------- */
/* cos/sin via range reduction + minimax polynomial (no libm, only used by
 * rotate()). Accurate enough for rasterizing small icons. */
static float f_cos(float x)
{
	const float PI = 3.14159265f, TWO_PI = 6.28318531f;
	x = x - TWO_PI * (float)(int)(x / TWO_PI);
	if (x < 0) x += TWO_PI;
	if (x > PI) x -= TWO_PI;
	float x2 = x * x;
	return 1.0f - x2/2.0f + x2*x2/24.0f - x2*x2*x2/720.0f + x2*x2*x2*x2/40320.0f;
}
static float f_sin(float x) { return f_cos(x - 1.57079633f); }

/* ------------------------------------------------ affine matrix ------------ */
struct mat { float a, b, c, d, e, f; };   /* x' = a x + c y + e,  y' = b x + d y + f */

static struct mat mat_id(void) { struct mat m = { 1, 0, 0, 1, 0, 0 }; return m; }

static struct mat mat_mul(struct mat A, struct mat B)   /* apply B, then A */
{
	struct mat r;
	r.a = A.a*B.a + A.c*B.b;
	r.b = A.b*B.a + A.d*B.b;
	r.c = A.a*B.c + A.c*B.d;
	r.d = A.b*B.c + A.d*B.d;
	r.e = A.a*B.e + A.c*B.f + A.e;
	r.f = A.b*B.e + A.d*B.f + A.f;
	return r;
}

static void mat_apply(struct mat m, float x, float y, float *ox, float *oy)
{
	*ox = m.a*x + m.c*y + m.e;
	*oy = m.b*x + m.d*y + m.f;
}

/* Average device scale of a matrix (for converting stroke-width to pixels). */
static float mat_scale(struct mat m)
{
	float sx = m.a*m.a + m.b*m.b, sy = m.c*m.c + m.d*m.d;
	float s = (sx + sy) * 0.5f;
	/* sqrt via Newton */
	float g = s > 1 ? s : 1;
	for (int i = 0; i < 8; i++) g = 0.5f * (g + s / g);
	return g;
}

/* ------------------------------------------------ number / token parse ----- */
static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c==','; }

static int parse_num(const char **pp, const char *end, float *out)
{
	const char *p = *pp;
	while (p < end && is_ws(*p)) p++;
	if (p >= end) return 0;
	int sign = 1;
	if (*p == '+') p++; else if (*p == '-') { sign = -1; p++; }
	float v = 0; int any = 0;
	while (p < end && *p >= '0' && *p <= '9') { v = v*10 + (*p++ - '0'); any = 1; }
	if (p < end && *p == '.') {
		p++; float frac = 0.1f;
		while (p < end && *p >= '0' && *p <= '9') { v += (*p++ - '0') * frac; frac *= 0.1f; any = 1; }
	}
	if (!any) return 0;
	if (p < end && (*p == 'e' || *p == 'E')) {
		p++; int es = 1, ev = 0;
		if (p < end && *p == '+') p++; else if (p < end && *p == '-') { es = -1; p++; }
		while (p < end && *p >= '0' && *p <= '9') ev = ev*10 + (*p++ - '0');
		float m = 1; for (int i = 0; i < ev; i++) m *= 10;
		if (es > 0) v *= m; else v /= m;
	}
	*out = sign * v;
	*pp = p;
	return 1;
}

/* Length attribute -> user units (drop a trailing px/unit; % unsupported -> 0). */
static float attr_len(const struct dom_node *n, const char *name, float def)
{
	const char *v = dom_get_attr(n, name);
	if (!v) return def;
	const char *p = v, *e = v + strlen(v);
	float out;
	if (!parse_num(&p, e, &out)) return def;
	while (p < e && *p == ' ') p++;
	if (p < e && *p == '%') return def;
	return out;
}

/* ------------------------------------------------ colour ------------------- */
static int hexnib(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Returns 0xRRGGBB, -1 for "none", -2 for "not a colour / inherit". */
static int parse_paint(const char *v)
{
	while (*v == ' ' || *v == '\t') v++;
	if (starts_ci(v, "none"))        return -1;
	if (starts_ci(v, "transparent")) return -1;
	if (starts_ci(v, "currentColor"))return 0x000000;
	if (*v == '#') {
		v++;
		int d[6], n = 0;
		while (n < 6 && hexnib(v[n]) >= 0) { d[n] = hexnib(v[n]); n++; }
		if (n >= 6) return (d[0]*16+d[1])<<16 | (d[2]*16+d[3])<<8 | (d[4]*16+d[5]);
		if (n >= 3) return (d[0]*17)<<16 | (d[1]*17)<<8 | (d[2]*17);
		return -2;
	}
	if (starts_ci(v, "rgb")) {
		const char *p = v + 3;
		if (*p == 'a' || *p == 'A') p++;
		while (*p == ' ' || *p == '(') p++;
		int c[3] = { 0, 0, 0 };
		for (int i = 0; i < 3; i++) {
			while (*p == ' ') p++;
			int val = 0, got = 0;
			while (*p >= '0' && *p <= '9') { val = val*10 + (*p++ - '0'); got = 1; }
			if (!got) return -2;
			c[i] = val > 255 ? 255 : val;
			while (*p == ' ' || *p == ',') p++;
		}
		return (c[0]<<16)|(c[1]<<8)|c[2];
	}
	static const struct { const char *n; int rgb; } named[] = {
		{ "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 },
		{ "green", 0x008000 }, { "blue", 0x0000ff }, { "yellow", 0xffff00 },
		{ "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xc0c0c0 },
		{ "orange", 0xffa500 }, { "purple", 0x800080 }, { "navy", 0x000080 },
		{ "lime", 0x00ff00 }, { "teal", 0x008080 }, { "aqua", 0x00ffff },
		{ "cyan", 0x00ffff }, { "magenta", 0xff00ff }, { "maroon", 0x800000 },
		{ "olive", 0x808000 }, { "pink", 0xffc0cb }, { "gold", 0xffd700 }, { 0, 0 }
	};
	for (int i = 0; named[i].n; i++) if (streq_ci(v, named[i].n)) return named[i].rgb;
	return -2;
}

/* Pull a property out of a style="" string (e.g. "fill:#fff;stroke:none"). */
static int style_prop(const char *style, const char *prop, char *out, int cap)
{
	int pl = strlen(prop);
	for (const char *p = style; *p; p++) {
		if (!starts_ci(p, prop)) continue;
		char prev = (p != style) ? p[-1] : ';';
		if (prev != ';' && prev != ' ' && prev != '{') continue;
		const char *q = p + pl;
		while (*q == ' ') q++;
		if (*q != ':') continue;
		q++;
		while (*q == ' ') q++;
		int k = 0;
		while (*q && *q != ';' && k < cap - 1) out[k++] = *q++;
		out[k] = '\0';
		return 1;
	}
	return 0;
}

/* ------------------------------------------------ paint state -------------- */
struct paint { int fill; int stroke; float stroke_w; };

/* Resolve an element's paint over the inherited one. */
static struct paint resolve_paint(const struct dom_node *n, struct paint in)
{
	struct paint p = in;
	char buf[64];
	const char *v;

	if ((v = dom_get_attr(n, "fill")))   { int c = parse_paint(v); if (c != -2) p.fill = c; }
	if ((v = dom_get_attr(n, "stroke"))) { int c = parse_paint(v); if (c != -2) p.stroke = c; }
	if ((v = dom_get_attr(n, "stroke-width"))) {
		const char *q = v, *e = v + strlen(v); float w;
		if (parse_num(&q, e, &w)) p.stroke_w = w;
	}
	const char *st = dom_get_attr(n, "style");
	if (st) {
		if (style_prop(st, "fill", buf, sizeof buf))   { int c = parse_paint(buf); if (c != -2) p.fill = c; }
		if (style_prop(st, "stroke", buf, sizeof buf)) { int c = parse_paint(buf); if (c != -2) p.stroke = c; }
		if (style_prop(st, "stroke-width", buf, sizeof buf)) {
			const char *q = buf, *e = buf + strlen(buf); float w;
			if (parse_num(&q, e, &w)) p.stroke_w = w;
		}
	}
	return p;
}

/* ------------------------------------------------ transform parse ---------- */
static struct mat parse_transform(const char *s)
{
	struct mat m = mat_id();
	if (!s) return m;
	const char *p = s, *end = s + strlen(s);
	while (p < end) {
		while (p < end && is_ws(*p)) p++;
		const char *name = p;
		while (p < end && ((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z'))) p++;
		int nl = (int)(p - name);
		while (p < end && *p != '(') p++;
		if (p >= end) break;
		p++;
		float arg[6]; int na = 0;
		while (na < 6 && parse_num(&p, end, &arg[na])) na++;
		while (p < end && *p != ')') p++;
		if (p < end) p++;

		struct mat t = mat_id();
		if (nl == 9 && starts_ci(name, "translate")) {
			t.e = arg[0]; t.f = (na > 1) ? arg[1] : 0;
		} else if (nl == 5 && starts_ci(name, "scale")) {
			t.a = arg[0]; t.d = (na > 1) ? arg[1] : arg[0];
		} else if (nl == 6 && starts_ci(name, "rotate")) {
			float r = arg[0] * 3.14159265f / 180.0f;
			float cs = f_cos(r), sn = f_sin(r);
			struct mat rot = { cs, sn, -sn, cs, 0, 0 };
			if (na >= 3) {
				struct mat pre = mat_id();  pre.e = arg[1];  pre.f = arg[2];
				struct mat post = mat_id(); post.e = -arg[1]; post.f = -arg[2];
				t = mat_mul(mat_mul(pre, rot), post);
			} else t = rot;
		} else if (nl == 6 && starts_ci(name, "matrix") && na >= 6) {
			t.a = arg[0]; t.b = arg[1]; t.c = arg[2]; t.d = arg[3]; t.e = arg[4]; t.f = arg[5];
		}
		m = mat_mul(m, t);
	}
	return m;
}

/* ------------------------------------------------ rasterizer --------------- */
struct ras {
	unsigned *buf; int w, h;     /* device buffer (SS resolution) */
	float *px, *py; int np, cap; /* current path's points */
	int   *sub;    int ns, csub; /* subpath start indices */
};

static int ras_pt(struct ras *r, float x, float y)
{
	if (r->np >= r->cap) {
		int nc = r->cap ? r->cap * 2 : 64;
		float *nx = realloc(r->px, (unsigned)nc * sizeof(float));
		float *ny = realloc(r->py, (unsigned)nc * sizeof(float));
		if (!nx || !ny) { if (nx) r->px = nx; if (ny) r->py = ny; return 0; }
		r->px = nx; r->py = ny; r->cap = nc;
	}
	r->px[r->np] = x; r->py[r->np] = y; r->np++;
	return 1;
}

static void ras_begin_sub(struct ras *r)
{
	if (r->ns >= r->csub) {
		int nc = r->csub ? r->csub * 2 : 16;
		int *ns = realloc(r->sub, (unsigned)nc * sizeof(int));
		if (!ns) return;
		r->sub = ns; r->csub = nc;
	}
	r->sub[r->ns++] = r->np;
}

static void ras_reset(struct ras *r) { r->np = 0; r->ns = 0; }

static unsigned blend(unsigned dst, int rgb)
{
	(void)dst;
	return 0xFF000000u | (unsigned)rgb;   /* opaque write; AA comes from SS downsample */
}

/* Scanline nonzero fill of the current subpaths in colour `rgb`. */
static void fill_path(struct ras *r, int rgb)
{
	if (r->ns == 0 || rgb < 0) return;
	float ymin = 1e9f, ymax = -1e9f;
	for (int i = 0; i < r->np; i++) {
		if (r->py[i] < ymin) ymin = r->py[i];
		if (r->py[i] > ymax) ymax = r->py[i];
	}
	int y0 = (int)ymin; if (y0 < 0) y0 = 0;
	int y1 = (int)ymax + 1; if (y1 > r->h) y1 = r->h;

	/* x-crossings buffer */
	static float xs[1024];
	static int   wd[1024];

	for (int y = y0; y < y1; y++) {
		float yc = y + 0.5f;
		int nx = 0;
		for (int s = 0; s < r->ns && nx < 1024; s++) {
			int a = r->sub[s];
			int b = (s + 1 < r->ns) ? r->sub[s + 1] : r->np;
			int cnt = b - a;
			if (cnt < 2) continue;
			for (int i = 0; i < cnt; i++) {
				int i0 = a + i, i1 = a + (i + 1) % cnt;
				float ya = r->py[i0], yb = r->py[i1];
				if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) {
					float t = (yc - ya) / (yb - ya);
					if (nx < 1024) {
						xs[nx] = r->px[i0] + t * (r->px[i1] - r->px[i0]);
						wd[nx] = (yb > ya) ? 1 : -1;
						nx++;
					}
				}
			}
		}
		for (int i = 1; i < nx; i++) {       /* sort by x (with winding) */
			float kx = xs[i]; int kw = wd[i]; int j = i - 1;
			while (j >= 0 && xs[j] > kx) { xs[j+1] = xs[j]; wd[j+1] = wd[j]; j--; }
			xs[j+1] = kx; wd[j+1] = kw;
		}
		int wind = 0;
		for (int i = 0; i + 1 < nx; i++) {
			wind += wd[i];
			if (wind != 0) {
				int xa = (int)(xs[i] + 0.5f), xb = (int)(xs[i+1] + 0.5f);
				if (xa < 0) xa = 0;
				if (xb > r->w) xb = r->w;
				unsigned *row = r->buf + (unsigned)y * r->w;
				for (int x = xa; x < xb; x++) row[x] = blend(row[x], rgb);
			}
		}
	}
}

/* Stroke the current subpaths: each segment becomes a filled quad of width w. */
static void stroke_path(struct ras *r, int rgb, float w, int closed)
{
	if (rgb < 0 || r->ns == 0) return;
	if (w < 1) w = 1;
	float hw = w * 0.5f;
	/* snapshot points (fill_path reuses r) */
	int np = r->np;
	float *px = malloc((unsigned)np * sizeof(float));
	float *py = malloc((unsigned)np * sizeof(float));
	int   ns = r->ns;
	int  *sb = malloc((unsigned)ns * sizeof(int));
	if (!px || !py || !sb) { free(px); free(py); free(sb); return; }
	for (int i = 0; i < np; i++) { px[i] = r->px[i]; py[i] = r->py[i]; }
	for (int i = 0; i < ns; i++) sb[i] = r->sub[i];

	for (int s = 0; s < ns; s++) {
		int a = sb[s];
		int b = (s + 1 < ns) ? sb[s + 1] : np;
		int cnt = b - a;
		int segs = closed ? cnt : cnt - 1;
		for (int i = 0; i < segs; i++) {
			float x0 = px[a + i], y0 = py[a + i];
			float x1 = px[a + (i + 1) % cnt], y1 = py[a + (i + 1) % cnt];
			float dx = x1 - x0, dy = y1 - y0;
			float len = dx*dx + dy*dy;
			if (len <= 0) continue;
			float g = len; for (int k = 0; k < 8; k++) g = 0.5f*(g + len/g);
			float nx = -dy / g * hw, ny = dx / g * hw;
			ras_reset(r);
			ras_begin_sub(r);
			ras_pt(r, x0 + nx, y0 + ny); ras_pt(r, x1 + nx, y1 + ny);
			ras_pt(r, x1 - nx, y1 - ny); ras_pt(r, x0 - nx, y0 - ny);
			fill_path(r, rgb);
		}
	}
	free(px); free(py); free(sb);
}

/* ------------------------------------------------ shape emitters ----------- */
static void emit_ellipse(struct ras *r, struct mat m, float cx, float cy, float rx, float ry)
{
	ras_reset(r);
	ras_begin_sub(r);
	const int N = 48;
	for (int i = 0; i < N; i++) {
		float a = (float)i / N * 6.28318531f;
		float ux = cx + rx * f_cos(a), uy = cy + ry * f_sin(a);
		float dx, dy; mat_apply(m, ux, uy, &dx, &dy);
		ras_pt(r, dx, dy);
	}
}

static void emit_points(struct ras *r, struct mat m, const char *pts)
{
	ras_reset(r);
	ras_begin_sub(r);
	const char *p = pts, *e = pts + strlen(pts);
	float x, y;
	while (parse_num(&p, e, &x) && parse_num(&p, e, &y)) {
		float dx, dy; mat_apply(m, x, y, &dx, &dy);
		ras_pt(r, dx, dy);
	}
}

/* Add a flattened cubic bezier (P0 current, to P3) to the path. */
static void bez3(struct ras *r, struct mat m, float x0, float y0, float x1, float y1,
                 float x2, float y2, float x3, float y3)
{
	const int N = 16;
	for (int i = 1; i <= N; i++) {
		float t = (float)i / N, u = 1 - t;
		float bx = u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3;
		float by = u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3;
		float dx, dy; mat_apply(m, bx, by, &dx, &dy);
		ras_pt(r, dx, dy);
	}
}
static void bez2(struct ras *r, struct mat m, float x0, float y0, float x1, float y1,
                 float x2, float y2)
{
	const int N = 12;
	for (int i = 1; i <= N; i++) {
		float t = (float)i / N, u = 1 - t;
		float bx = u*u*x0 + 2*u*t*x1 + t*t*x2;
		float by = u*u*y0 + 2*u*t*y1 + t*t*y2;
		float dx, dy; mat_apply(m, bx, by, &dx, &dy);
		ras_pt(r, dx, dy);
	}
}

/* Parse a path "d" into device-space subpaths in r. */
static void emit_path(struct ras *r, struct mat m, const char *d)
{
	ras_reset(r);
	const char *p = d, *e = d + strlen(d);
	float cx = 0, cy = 0, sx = 0, sy = 0;     /* current + subpath start (user space) */
	float pcx = 0, pcy = 0;                    /* last control point (for S/T) */
	char prev = 0;
	int have_sub = 0;
	while (p < e) {
		while (p < e && is_ws(*p)) p++;
		if (p >= e) break;
		char cmd = *p;
		if ((cmd>='A'&&cmd<='Z')||(cmd>='a'&&cmd<='z')) p++;
		else cmd = prev;                       /* implicit repeat of last command */
		int rel = (cmd >= 'a');
		char c = rel ? cmd - 32 : cmd;
		float a[6];

		if (c == 'M') {
			parse_num(&p, e, &a[0]); parse_num(&p, e, &a[1]);
			cx = rel ? cx + a[0] : a[0]; cy = rel ? cy + a[1] : a[1];
			sx = cx; sy = cy;
			ras_begin_sub(r); have_sub = 1;
			float dx, dy; mat_apply(m, cx, cy, &dx, &dy); ras_pt(r, dx, dy);
			prev = rel ? 'l' : 'L';
		} else if (c == 'L') {
			parse_num(&p, e, &a[0]); parse_num(&p, e, &a[1]);
			cx = rel ? cx + a[0] : a[0]; cy = rel ? cy + a[1] : a[1];
			if (!have_sub) { ras_begin_sub(r); have_sub = 1; }
			float dx, dy; mat_apply(m, cx, cy, &dx, &dy); ras_pt(r, dx, dy);
			prev = cmd;
		} else if (c == 'H') {
			parse_num(&p, e, &a[0]); cx = rel ? cx + a[0] : a[0];
			float dx, dy; mat_apply(m, cx, cy, &dx, &dy); ras_pt(r, dx, dy);
			prev = cmd;
		} else if (c == 'V') {
			parse_num(&p, e, &a[0]); cy = rel ? cy + a[0] : a[0];
			float dx, dy; mat_apply(m, cx, cy, &dx, &dy); ras_pt(r, dx, dy);
			prev = cmd;
		} else if (c == 'C' || c == 'S') {
			float x1, y1, x2, y2, x3, y3;
			if (c == 'S') {
				x1 = 2*cx - pcx; y1 = 2*cy - pcy;
				parse_num(&p,e,&x2); parse_num(&p,e,&y2); parse_num(&p,e,&x3); parse_num(&p,e,&y3);
			} else {
				parse_num(&p,e,&x1); parse_num(&p,e,&y1);
				parse_num(&p,e,&x2); parse_num(&p,e,&y2);
				parse_num(&p,e,&x3); parse_num(&p,e,&y3);
			}
			if (rel) { x1+=cx; y1+=cy; x2+=cx; y2+=cy; x3+=cx; y3+=cy; }
			if (!have_sub) { ras_begin_sub(r); have_sub = 1; }
			bez3(r, m, cx, cy, x1, y1, x2, y2, x3, y3);
			pcx = x2; pcy = y2; cx = x3; cy = y3;
			prev = cmd;
		} else if (c == 'Q' || c == 'T') {
			float x1, y1, x2, y2;
			if (c == 'T') {
				x1 = 2*cx - pcx; y1 = 2*cy - pcy;
				parse_num(&p,e,&x2); parse_num(&p,e,&y2);
			} else {
				parse_num(&p,e,&x1); parse_num(&p,e,&y1);
				parse_num(&p,e,&x2); parse_num(&p,e,&y2);
			}
			if (rel) { x1+=cx; y1+=cy; x2+=cx; y2+=cy; }
			if (!have_sub) { ras_begin_sub(r); have_sub = 1; }
			bez2(r, m, cx, cy, x1, y1, x2, y2);
			pcx = x1; pcy = y1; cx = x2; cy = y2;
			prev = cmd;
		} else if (c == 'Z') {
			cx = sx; cy = sy;
			prev = cmd;
		} else if (c == 'A') {                 /* arcs unsupported: skip 7 params */
			for (int i = 0; i < 7; i++) parse_num(&p, e, &a[0]);
			prev = cmd;
		} else {
			p++;                               /* unknown: advance to avoid stall */
		}
	}
}

/* ------------------------------------------------ tree walk ---------------- */
static void render_node(struct dom_node *n, struct ras *r, struct mat ctm, struct paint pin)
{
	for (struct dom_node *c = n->first_child; c; c = c->next) {
		if (c->type != DOM_ELEMENT) continue;
		const char *tag = c->tag;

		struct mat m = mat_mul(ctm, parse_transform(dom_get_attr(c, "transform")));
		struct paint pp = resolve_paint(c, pin);

		if (streq(tag, "g") || streq(tag, "svg") || streq(tag, "a")) {
			render_node(c, r, m, pp);
		} else if (streq(tag, "rect")) {
			float x = attr_len(c,"x",0), y = attr_len(c,"y",0);
			float w = attr_len(c,"width",0), h = attr_len(c,"height",0);
			if (w > 0 && h > 0) {
				ras_reset(r); ras_begin_sub(r);
				float dx,dy;
				mat_apply(m,x,  y,  &dx,&dy); ras_pt(r,dx,dy);
				mat_apply(m,x+w,y,  &dx,&dy); ras_pt(r,dx,dy);
				mat_apply(m,x+w,y+h,&dx,&dy); ras_pt(r,dx,dy);
				mat_apply(m,x,  y+h,&dx,&dy); ras_pt(r,dx,dy);
				fill_path(r, pp.fill);
				stroke_path(r, pp.stroke, pp.stroke_w * mat_scale(m), 1);
			}
		} else if (streq(tag, "circle")) {
			float cx = attr_len(c,"cx",0), cy = attr_len(c,"cy",0), rr = attr_len(c,"r",0);
			if (rr > 0) {
				emit_ellipse(r, m, cx, cy, rr, rr);
				fill_path(r, pp.fill);
				stroke_path(r, pp.stroke, pp.stroke_w * mat_scale(m), 1);
			}
		} else if (streq(tag, "ellipse")) {
			float cx = attr_len(c,"cx",0), cy = attr_len(c,"cy",0);
			float rx = attr_len(c,"rx",0), ry = attr_len(c,"ry",0);
			if (rx > 0 && ry > 0) {
				emit_ellipse(r, m, cx, cy, rx, ry);
				fill_path(r, pp.fill);
				stroke_path(r, pp.stroke, pp.stroke_w * mat_scale(m), 1);
			}
		} else if (streq(tag, "line")) {
			float x1 = attr_len(c,"x1",0), y1 = attr_len(c,"y1",0);
			float x2 = attr_len(c,"x2",0), y2 = attr_len(c,"y2",0);
			ras_reset(r); ras_begin_sub(r);
			float dx,dy; mat_apply(m,x1,y1,&dx,&dy); ras_pt(r,dx,dy);
			mat_apply(m,x2,y2,&dx,&dy); ras_pt(r,dx,dy);
			int sc = (pp.stroke >= 0) ? pp.stroke : 0x000000;
			stroke_path(r, sc, (pp.stroke_w > 0 ? pp.stroke_w : 1) * mat_scale(m), 0);
		} else if (streq(tag, "polyline") || streq(tag, "polygon")) {
			const char *pts = dom_get_attr(c, "points");
			if (pts) {
				int poly = streq(tag, "polygon");
				emit_points(r, m, pts);
				if (poly) fill_path(r, pp.fill);
				stroke_path(r, pp.stroke, pp.stroke_w * mat_scale(m), poly);
			}
		} else if (streq(tag, "path")) {
			const char *d = dom_get_attr(c, "d");
			if (d) {
				emit_path(r, m, d);
				fill_path(r, pp.fill);
				stroke_path(r, pp.stroke, pp.stroke_w * mat_scale(m), 0);
			}
		}
	}
}

/* ------------------------------------------------ entry -------------------- */
static struct dom_node *find_svg(struct dom_node *n)
{
	for (struct dom_node *c = n->first_child; c; c = c->next) {
		if (c->type != DOM_ELEMENT) continue;
		if (streq(c->tag, "svg")) return c;
		struct dom_node *r = find_svg(c);
		if (r) return r;
	}
	return 0;
}

int svg_probe(const char *data, int len)
{
	const char *p = data, *e = data + (len < 2048 ? len : 2048);
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\xef' ||
	                 *p == '\xbb' || *p == '\xbf')) p++;   /* skip ws + UTF-8 BOM */
	for (; p < e; p++) {
		if (p + 4 < e && p[0] == '<' && starts_ci(p + 1, "svg")) return 1;
		if (p + 5 < e && p[0] == '<' && p[1] == '?' && starts_ci(p + 2, "xml")) continue;
		if (p[0] == '<' && p[1] != '?' && p[1] != '!') break;   /* a non-svg root tag */
	}
	return 0;
}

int svg_size(const char *data, int len, int *w, int *h)
{
	struct dom_node *root = dom_parse(data, len);
	if (!root) return 0;
	struct dom_node *svg = find_svg(root);
	int ok = 0;
	if (svg) {
		float vw = 0, vh = 0;
		const char *vb = dom_get_attr(svg, "viewBox");
		if (vb) {
			const char *p = vb, *e = vb + strlen(vb);
			float a; parse_num(&p,e,&a); parse_num(&p,e,&a);
			parse_num(&p,e,&vw); parse_num(&p,e,&vh);
		}
		if (vw <= 0 || vh <= 0) { vw = attr_len(svg, "width", 0); vh = attr_len(svg, "height", 0); }
		if (vw > 0 && vh > 0) { *w = (int)(vw + 0.5f); *h = (int)(vh + 0.5f); ok = 1; }
	}
	dom_free(root);
	return ok;
}

int svg_render(const char *data, int len, int w, int h, unsigned *out)
{
	if (w <= 0 || h <= 0 || !out) return 0;

	struct dom_node *root = dom_parse(data, len);
	if (!root) return 0;
	struct dom_node *svg = find_svg(root);
	if (!svg) { dom_free(root); return 0; }

	/* user coordinate system: viewBox, else width/height attrs, else device */
	float vbx = 0, vby = 0, vbw = 0, vbh = 0;
	const char *vb = dom_get_attr(svg, "viewBox");
	if (vb) {
		const char *p = vb, *e = vb + strlen(vb);
		parse_num(&p,e,&vbx); parse_num(&p,e,&vby); parse_num(&p,e,&vbw); parse_num(&p,e,&vbh);
	}
	if (vbw <= 0 || vbh <= 0) {
		vbw = attr_len(svg, "width", 0);
		vbh = attr_len(svg, "height", 0);
		vbx = vby = 0;
	}
	if (vbw <= 0 || vbh <= 0) { vbw = (float)w; vbh = (float)h; }

	int ss = SS_MAX;
	while (ss > 1 && (long)w * ss * h * ss > 4 * 1024 * 1024) ss--;   /* cap buffer */
	int dw = w * ss, dh = h * ss;

	struct ras r;
	r.buf = malloc((unsigned)dw * dh * sizeof(unsigned));
	r.px = r.py = 0; r.np = r.cap = 0;
	r.sub = 0; r.ns = r.csub = 0;
	r.w = dw; r.h = dh;
	if (!r.buf) { dom_free(root); return 0; }
	for (int i = 0; i < dw * dh; i++) r.buf[i] = 0;

	/* fit user space into the SS device buffer, aspect-preserved + centered */
	float sxr = dw / vbw, syr = dh / vbh;
	float s = sxr < syr ? sxr : syr;
	float ox = (dw - vbw * s) * 0.5f, oy = (dh - vbh * s) * 0.5f;
	struct mat ctm = mat_id();
	ctm.a = s; ctm.d = s;
	ctm.e = ox - vbx * s; ctm.f = oy - vby * s;

	struct paint base = { 0x000000, -1, 1.0f };   /* default fill black, no stroke */
	render_node(svg, &r, ctm, base);

	/* box-downsample SS -> 1, averaging colour and alpha */
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int ar = 0, rr = 0, gg = 0, bb = 0;
			for (int sy = 0; sy < ss; sy++)
				for (int sx = 0; sx < ss; sx++) {
					unsigned px = r.buf[(unsigned)(y*ss+sy) * dw + (x*ss+sx)];
					int al = (px >> 24) & 0xff;
					ar += al;
					rr += ((px >> 16) & 0xff) * al / 255;
					gg += ((px >> 8) & 0xff) * al / 255;
					bb += (px & 0xff) * al / 255;
				}
			int n = ss * ss;
			int a = ar / n;
			int R = a ? (rr * 255 / ar) : 0;
			int G = a ? (gg * 255 / ar) : 0;
			int B = a ? (bb * 255 / ar) : 0;
			out[y * w + x] = ((unsigned)a << 24) | ((unsigned)R << 16) |
			                 ((unsigned)G << 8) | (unsigned)B;
		}
	}

	free(r.buf); free(r.px); free(r.py); free(r.sub);
	dom_free(root);
	return 1;
}
