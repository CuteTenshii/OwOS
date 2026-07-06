/* color.c — shared CSS/SVG colour parser (see color.h). Consolidates the hex /
 * rgb() / hsl() / oklch() / named parsing that css.c and svg.c each used to carry
 * their own copy of. Pure arithmetic + libm (pow/cos/sin) for the colour-space
 * conversions; no allocation. */
#include "owos.h"
#include "math.h"
#include "color.h"
#include "dom.h"     /* lower / starts_ci / streq_ci */

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

/* HSL(0..360, 0..100%, 0..100%) -> 0xRRGGBB. Pure arithmetic (no libm). */
static int hsl_to_rgb(int H, int S, int L)
{
	double h = H, s = S / 100.0, l = L / 100.0, c, x, m, r, g, b, t;
	int ri, gi, bi;
	while (h < 0) h += 360;
	while (h >= 360) h -= 360;
	if (s < 0) s = 0;
	if (s > 1) s = 1;
	if (l < 0) l = 0;
	if (l > 1) l = 1;
	c = (1 - (l * 2 - 1 < 0 ? -(l * 2 - 1) : (l * 2 - 1))) * s;
	{
		double hp = h / 60.0;
		double mod2 = hp - 2.0 * (double)((int)(hp / 2.0));
		t = mod2 - 1; if (t < 0) t = -t;
		x = c * (1 - t);
	}
	m = l - c / 2;
	if      (h <  60) { r = c; g = x; b = 0; }
	else if (h < 120) { r = x; g = c; b = 0; }
	else if (h < 180) { r = 0; g = c; b = x; }
	else if (h < 240) { r = 0; g = x; b = c; }
	else if (h < 300) { r = x; g = 0; b = c; }
	else              { r = c; g = 0; b = x; }
	ri = (int)((r + m) * 255 + 0.5);
	gi = (int)((g + m) * 255 + 0.5);
	bi = (int)((b + m) * 255 + 0.5);
	if (ri < 0) ri = 0;
	if (ri > 255) ri = 255;
	if (gi < 0) gi = 0;
	if (gi > 255) gi = 255;
	if (bi < 0) bi = 0;
	if (bi > 255) bi = 255;
	return (ri << 16) | (gi << 8) | bi;
}

/* OKLCH(L C H) -> 0xRRGGBB (L in 0..1 or %, C chroma, H hue degrees). */
static double lin2srgb(double c)
{
	if (c <= 0.0) return 0.0;
	if (c >= 1.0) return 1.0;
	return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
static int oklch_to_rgb(double L, double C, double H)
{
	double hr = H * 3.14159265358979 / 180.0;
	double a = C * cos(hr), b = C * sin(hr);
	double l_ = L + 0.3963377774 * a + 0.2158037573 * b;
	double m_ = L - 0.1055613458 * a - 0.0638541728 * b;
	double s_ = L - 0.0894841775 * a - 1.2914855480 * b;
	double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
	double r = lin2srgb( 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s);
	double g = lin2srgb(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s);
	double bb = lin2srgb(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
	int ri = (int)(r * 255 + 0.5), gi = (int)(g * 255 + 0.5), bi = (int)(bb * 255 + 0.5);
	ri = ri < 0 ? 0 : ri > 255 ? 255 : ri;
	gi = gi < 0 ? 0 : gi > 255 ? 255 : gi;
	bi = bi < 0 ? 0 : bi > 255 ? 255 : bi;
	return (ri << 16) | (gi << 8) | bi;
}

/* Alpha token (number 0..1, or NN%) -> 0..255; opaque (255) if unreadable. */
static int alpha_tok(const char *p)
{
	while (*p == ' ') p++;
	double val = 0; int got = 0;
	while (*p >= '0' && *p <= '9') { val = val * 10 + (*p++ - '0'); got = 1; }
	if (*p == '.') { p++; double f = 0.1; while (*p >= '0' && *p <= '9') { val += (*p++ - '0') * f; f *= 0.1; } got = 1; }
	if (!got) return 255;
	if (*p == '%') return (int)(val * 255 / 100 + 0.5);
	int a = (int)(val * 255 + 0.5);
	return a < 0 ? 0 : a > 255 ? 255 : a;
}

int color_parse_a(const char *v, int *alpha)
{
	if (alpha) *alpha = 255;
	while (*v == ' ' || *v == '\t') v++;

	if (*v == '#') {
		v++;
		int d[8], n = 0;
		while (n < 8 && hexnib(v[n]) >= 0) { d[n] = hexnib(v[n]); n++; }
		/* Only the standard 3/4/6/8-nibble forms; 5 or 7 nibbles is invalid CSS. */
		if (n == 8) { if (alpha) *alpha = d[6] * 16 + d[7];
		              return (d[0]*16+d[1]) << 16 | (d[2]*16+d[3]) << 8 | (d[4]*16+d[5]); }
		if (n == 6) return (d[0]*16+d[1]) << 16 | (d[2]*16+d[3]) << 8 | (d[4]*16+d[5]);
		if (n == 4) { if (alpha) *alpha = d[3] * 17;
		              return (d[0]*17) << 16 | (d[1]*17) << 8 | (d[2]*17); }
		if (n == 3) return (d[0]*17) << 16 | (d[1]*17) << 8 | (d[2]*17);
		return -1;
	}

	if (starts_ci(v, "rgb")) {
		const char *p = v + 3;
		int rgba = (*p == 'a' || *p == 'A');
		if (rgba) p++;
		while (*p == ' ' || *p == '(') p++;
		int c[3] = { 0, 0, 0 };
		for (int i = 0; i < 3; i++) {
			while (*p == ' ') p++;
			int got = 0, val = 0;
			while (*p >= '0' && *p <= '9') { val = val * 10 + (*p++ - '0'); got = 1; }
			if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }   /* drop fraction */
			if (!got) return -1;
			if (*p == '%') { val = val * 255 / 100; p++; }
			c[i] = val > 255 ? 255 : val;
			while (*p == ' ' || *p == ',') p++;
		}
		if (alpha && (*p == '/' || rgba)) { while (*p == '/' || *p == ' ' || *p == ',') p++; if (*p) *alpha = alpha_tok(p); }
		return (c[0] << 16) | (c[1] << 8) | c[2];
	}

	if (starts_ci(v, "hsl")) {
		const char *p = v + 3;
		int hsla = (*p == 'a' || *p == 'A');
		if (hsla) p++;
		while (*p == ' ' || *p == '(') p++;
		int comp[3] = { 0, 0, 0 };
		for (int i = 0; i < 3; i++) {
			int got = 0, val = 0, neg = 0;
			while (*p == ' ') p++;
			if (*p == '-') { neg = 1; p++; }
			while (*p >= '0' && *p <= '9') { val = val * 10 + (*p++ - '0'); got = 1; }
			if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
			if (!got) return -1;
			comp[i] = neg ? -val : val;
			while (*p && *p != ',' && *p != ' ' && *p != ')' && *p != '/') p++;
			while (*p == ' ' || *p == ',') p++;
		}
		if (alpha && *p == '/') { p++; *alpha = alpha_tok(p); }
		return hsl_to_rgb(comp[0], comp[1], comp[2]);
	}

	if (starts_ci(v, "oklch")) {
		const char *p = v + 5;
		while (*p == ' ' || *p == '(') p++;
		double comp[3] = { 0, 0, 0 };
		for (int i = 0; i < 3; i++) {
			while (*p == ' ') p++;
			int got = 0; double val = 0; int neg = 0;
			if (*p == '-') { neg = 1; p++; }
			while (*p >= '0' && *p <= '9') { val = val * 10 + (*p++ - '0'); got = 1; }
			if (*p == '.') { p++; double f = 0.1; while (*p >= '0' && *p <= '9') { val += (*p++ - '0') * f; f *= 0.1; } got = 1; }
			if (!got) return -1;
			if (*p == '%') { val /= 100.0; p++; }
			comp[i] = neg ? -val : val;
			while (*p && *p != ' ' && *p != ',' && *p != ')' && *p != '/') p++;
			while (*p == ' ' || *p == ',') p++;
		}
		if (alpha && *p == '/') { p++; *alpha = alpha_tok(p); }
		return oklch_to_rgb(comp[0], comp[1], comp[2]);
	}

	{ int rgb = named_rgb(v); if (rgb < 0 && streq_ci(v, "transparent") && alpha) *alpha = 0; return rgb; }
}

int color_parse(const char *s) { return color_parse_a(s, 0); }
