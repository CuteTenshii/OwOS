/* css.h — a small but real CSS engine: parse <style> blocks / style="" into
 * rules, match selectors against DOM nodes, cascade by specificity, and produce
 * a computed style. The renderer (html.c) maps the result onto its capabilities
 * (16-colour text, bold/underline, block/inline, image box size). */
#ifndef OWOS_CSS_H
#define OWOS_CSS_H

#include "dom.h"

enum { CSS_DISP_UNSET = 0, CSS_DISP_INLINE, CSS_DISP_BLOCK, CSS_DISP_NONE };
enum css_pos { CSS_POS_STATIC = 0, CSS_POS_REL, CSS_POS_ABS, CSS_POS_FIXED, CSS_POS_STICKY };
/* flex main-axis distribution (justify-content) and cross-axis (align-items). */
enum css_justify { CSS_JUST_START = 0, CSS_JUST_CENTER, CSS_JUST_END,
                   CSS_JUST_BETWEEN, CSS_JUST_AROUND, CSS_JUST_EVENLY };
enum css_align { CSS_ALIGN_STRETCH = 0, CSS_ALIGN_START, CSS_ALIGN_CENTER, CSS_ALIGN_END };
/* text-transform. */
enum css_ttx { CSS_TT_NONE = 0, CSS_TT_UPPER, CSS_TT_LOWER, CSS_TT_CAPITALIZE };
/* white-space: how spaces/newlines collapse and whether lines wrap. */
enum css_ws { CSS_WS_NORMAL = 0, CSS_WS_NOWRAP, CSS_WS_PRE, CSS_WS_PRE_WRAP, CSS_WS_PRE_LINE };
/* list-style-type: the <li> marker shape/number style (CSS_LST_DEFAULT = use the
 * list's own ul/ol default). */
enum css_list { CSS_LST_DEFAULT = 0, CSS_LST_NONE, CSS_LST_DISC, CSS_LST_CIRCLE,
                CSS_LST_SQUARE, CSS_LST_DECIMAL, CSS_LST_DECIMAL_ZERO,
                CSS_LST_LOWER_ALPHA, CSS_LST_UPPER_ALPHA,
                CSS_LST_LOWER_ROMAN, CSS_LST_UPPER_ROMAN };

/* A computed-style delta. Each field is either set or "unset" so deltas layer:
 * colours are 0xRRGGBB or -1; bold/under are 0/1 or -1; width/height are px or
 * -1; disp is a CSS_DISP_* value (UNSET = 0); fam is the full font-family stack
 * (each name lowercased + dequoted, comma-joined), "" = unset. The box-model
 * fields (margin/padding/border/position/flex) back the layout engine (layout.c):
 * lengths are px>=0, -1 unset; margin sides may be -2 (auto); a percent width/
 * height keeps the px field -1 and stores the numerator in pct_*. */
#define CSS_FAM_MAX   48     /* one family name (e.g. an @font-face's font-family) */
#define CSS_FAM_STACK 160    /* a whole font-family stack: "A", "B", sans-serif    */

/* background-image layer. The renderer paints ONE image layer (the topmost in a
 * comma list) over the background colour — a url() bitmap or a CSS gradient.
 * Sub-properties carry their own unset sentinel so longhands cascade independently
 * of the image identity (which `kind`, the gradient and url all come together). */
enum css_bg_kind { CSS_BGK_NONE = 0, CSS_BGK_URL, CSS_BGK_LINEAR, CSS_BGK_RADIAL };
enum { CSS_BGR_REPEAT = 0, CSS_BGR_NO_REPEAT, CSS_BGR_REPEAT_X, CSS_BGR_REPEAT_Y };
enum { CSS_BGS_AUTO = 0, CSS_BGS_COVER, CSS_BGS_CONTAIN, CSS_BGS_LEN };
#define CSS_BG_STOPS 8
#define CSS_BG_URL   128
struct css_gstop { int color; short pos; };  /* color 0xAARRGGBB; pos = percent 0..100, -1 auto */
struct css_bg_image {
	signed char   kind;        /* enum css_bg_kind, -1 unset */
	signed char   repeat;      /* CSS_BGR_*, -1 unset */
	signed char   size_kind;   /* CSS_BGS_*, -1 unset */
	signed char   rad_circle;  /* radial: 1 circle, 0 ellipse */
	short         angle;       /* linear-gradient angle, degrees: 0 = to top, 90 = to right */
	short         size_w, size_h;  /* CSS_BGS_LEN: px>=0, -1 auto component */
	short         pos_x, pos_y;    /* background-position, percent 0..100; -1 unset */
	unsigned char nstops;
	short         img_idx;     /* resolved decoded-image index (renderer sets), -1 none */
	struct css_gstop stops[CSS_BG_STOPS];
	char          url[CSS_BG_URL];  /* url() target, unresolved ("" = none) */
};

struct css_style {
	int          color;
	int          bg;
	signed char  bold;
	signed char  under;
	signed char  disp;
	signed char  center;   /* text-align: 1 = center, 0 = left/other, -1 unset */
	int          width, height;
	int          max_w, max_h;        /* max-width/height: px or -1 */
	int          min_w, min_h;        /* min-width/height: px or -1 */
	short        pct_w, pct_h;         /* width/height as a percent, else -1 */
	short        pct_maxw, pct_maxh;   /* max-width/height as a percent, else -1 */
	short        pct_minw, pct_minh;   /* min-width/height as a percent, else -1 */
	signed char  box_sizing;          /* 1 border-box, 0 content-box, -1 unset */
	int          font_px;             /* font-size in px (incl rem/keywords), -1 unset */
	short        font_pct;            /* font-size as a percent of the parent, else -1 */
	int          radius;              /* border-radius px (uniform), -1 unset */
	short        sh_dx, sh_dy, sh_blur, sh_spread;  /* box-shadow geometry, px */
	int          sh_color;            /* box-shadow 0xAARRGGBB, -1 = no shadow */
	short        margin[4];            /* T,R,B,L: px>=0, -2 auto, -1 unset */
	short        padding[4];           /* T,R,B,L: px>=0, -1 unset */
	short        border_w[4];          /* T,R,B,L: px>=0, -1 unset */
	int          border_color;         /* 0xRRGGBB or -1 */
	signed char  pos;                  /* enum css_pos, or -1 unset */
	int          top, right, bottom, left;   /* px or -1 */
	signed char  flex;                 /* 1 = display:flex/inline-flex, 0 not, -1 unset */
	signed char  flex_dir;             /* 0 row, 1 column, -1 unset */
	signed char  flex_wrap;            /* 0 nowrap, 1 wrap, -1 unset */
	short        flex_grow;            /* >=0, -1 unset */
	short        flex_shrink;          /* >=0, -1 unset (default 1) */
	int          flex_basis;           /* main-size px>=0, -1 auto/unset */
	short        pct_basis;            /* flex-basis as a percent, else -1 */
	signed char  justify;              /* enum css_justify, -1 unset */
	signed char  align_items;          /* enum css_align, -1 unset */
	short        gap_row, gap_col;     /* row/column gap px>=0, -1 unset */
	signed char  overflow;             /* 0 visible, 1 clipped, -1 unset */
	signed char  italic;               /* font-style: 1 italic/oblique, 0 normal, -1 unset */
	signed char  ttx;                  /* text-transform: enum css_ttx, -1 unset */
	signed char  white_space;          /* enum css_ws, -1 unset */
	signed char  list_style;           /* enum css_list (DEFAULT=0 means unset/inherit) */
	signed char  opacity;              /* 0..100 percent, -1 unset */
	short        line_height;          /* line-height px, -1 unset */
	short        lh_pct;               /* line-height as a percent of font-size, -1 unset
	                                    * (a unitless multiplier N is stored as N*100) */
	short        outline_w;            /* outline width px, -1 unset */
	int          outline_color;        /* outline 0xRRGGBB, -1 unset */
	short        z_index;              /* stacking order; CSS_Z_AUTO = unset/auto */
	/* SVG paint (for inline <svg>): -1 unset, -2 none/transparent, -3 currentColor,
	 * else 0xRRGGBB. The browser bakes these onto serialized SVG elements. */
	int          fill, stroke;
	struct css_bg_image bgi;           /* background image/gradient layer (kind<0 unset) */
	/* ::before / ::after generated content (decoded, "" = none). */
	char         content[48];
	char         fam[CSS_FAM_STACK];
};

#define CSS_Z_AUTO  (-32768)   /* z_index sentinel: auto (no explicit stacking level) */

typedef struct css_sheet css_sheet;

void css_style_reset(struct css_style *s);
void css_bgi_reset(struct css_bg_image *b);   /* reset a background layer to unset */

css_sheet *css_new(void);
/* Add a sheet source. `base` is the URL the sheet was loaded from (an external
 * sheet's own URL, or the page URL for an inline <style>); a relative @font-face
 * src resolves against it. */
void       css_add(css_sheet *sheet, const char *src, int len, const char *base);
void       css_free(css_sheet *sheet);

/* The current viewport, what @media (min/max-width|height), orientation and
 * prefers-color-scheme test against. The browser sets it before each cascade
 * (and on resize, so media queries re-evaluate). Defaults to a sane size. */
void       css_set_viewport(int width, int height);

/* @font-face faces declared in the sheet: each a family name + a src URL (the
 * first raw TTF/OTF url; woff/woff2 entries are skipped — we can't decode them).
 * The browser fetches the url and loads the face, keyed by family. */
int  css_fontface_count(const css_sheet *sheet);
/* `base` (may be 0) receives the URL of the sheet that declared the face, for
 * resolving a relative src. */
void css_fontface_at(const css_sheet *sheet, int i, const char **family,
                     const char **url, const char **base);

/* Fold the sheet's matching rules for `el` into *out (specificity/order order). */
void css_apply_sheet(css_sheet *sheet, const struct dom_node *el, struct css_style *out);
/* Fold a style="" declaration block into *out (highest priority). */
void css_apply_inline(const char *decls, struct css_style *out);
/* Fold HTML presentational attrs (color/bgcolor/width/height) into *out (lowest). */
void css_apply_attrs(const struct dom_node *el, struct css_style *out);

/* The cascade-winning background-color for `el`, written to *out as 0xAARRGGBB;
 * returns 1 if `el` sets one, 0 if not. The alpha lets the caller composite a
 * translucent background over what's behind it (per CSS), e.g. to build the page
 * canvas colour from <html> then <body>. (An out-param, not the return value, so
 * an opaque 0xFF****** isn't mistaken for a negative/"none".) */
int css_bg_argb(css_sheet *sheet, const struct dom_node *el, unsigned *out);

/* ::before / ::after generated content. Folds the matching pseudo-element rules
 * for `el` into *out (its `content` holds the decoded text, plus colour/weight/
 * decoration); returns 1 if there is non-empty content to render, else 0. `which`
 * is CSS_PSEUDO_BEFORE or CSS_PSEUDO_AFTER. */
enum { CSS_PSEUDO_BEFORE = 1, CSS_PSEUDO_AFTER = 2 };
int css_pseudo_content(css_sheet *sheet, const struct dom_node *el, int which,
                       struct css_style *out);

/* Devtools Styles pane: the rules whose selector matches `el`, winner first.
 * Each carries the reconstructed selector text, the @media prelude ("" if none)
 * plus whether that media currently applies, and the rule's declarations (the
 * subset css.c models). Returns the number written (<= cap). */
struct css_match {
	char             selector[160];
	char             media[128];
	int              active;        /* 1 = the @media currently matches */
	struct css_style decl;
	const char      *raw;           /* authored declaration text (for devtools), or "" */
};
int css_matches(css_sheet *sheet, const struct dom_node *el,
                struct css_match *out, int cap);

#endif /* OWOS_CSS_H */
