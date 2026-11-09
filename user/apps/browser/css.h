/* css.h: CSS engine (parse, selector match, cascade, computed style) used by the renderer. */
#ifndef OWOS_CSS_H
#define OWOS_CSS_H

#include "dom.h"

enum { CSS_DISP_UNSET = 0, CSS_DISP_INLINE, CSS_DISP_BLOCK, CSS_DISP_NONE, CSS_DISP_CONTENTS };
enum css_pos { CSS_POS_STATIC = 0, CSS_POS_REL, CSS_POS_ABS, CSS_POS_FIXED, CSS_POS_STICKY };
/* flex justify-content and align-items. */
enum css_justify {
  CSS_JUST_START = 0,
  CSS_JUST_CENTER,
  CSS_JUST_END,
  CSS_JUST_BETWEEN,
  CSS_JUST_AROUND,
  CSS_JUST_EVENLY
};
enum css_align { CSS_ALIGN_STRETCH = 0, CSS_ALIGN_START, CSS_ALIGN_CENTER, CSS_ALIGN_END };
/* Grid track sizing; AUTO covers auto/min-content/max-content, FR is a flexible fraction. */
enum css_track_kind { CSS_TRK_AUTO = 0, CSS_TRK_PX, CSS_TRK_PCT, CSS_TRK_FR };
#define CSS_GRID_MAX 12 /* max explicit tracks per axis */
struct css_track {
  signed char kind;
  short v;
}; /* v = px | percent | fr weight */
enum css_ttx { CSS_TT_NONE = 0, CSS_TT_UPPER, CSS_TT_LOWER, CSS_TT_CAPITALIZE };
enum css_ws { CSS_WS_NORMAL = 0, CSS_WS_NOWRAP, CSS_WS_PRE, CSS_WS_PRE_WRAP, CSS_WS_PRE_LINE };
/* list-style-type (DEFAULT = the list's own ul/ol default). */
enum css_list {
  CSS_LST_DEFAULT = 0,
  CSS_LST_NONE,
  CSS_LST_DISC,
  CSS_LST_CIRCLE,
  CSS_LST_SQUARE,
  CSS_LST_DECIMAL,
  CSS_LST_DECIMAL_ZERO,
  CSS_LST_LOWER_ALPHA,
  CSS_LST_UPPER_ALPHA,
  CSS_LST_LOWER_ROMAN,
  CSS_LST_UPPER_ROMAN
};
/* list-style-position (DEFAULT = unset/inherit). */
enum css_list_pos { CSS_LSP_DEFAULT = 0, CSS_LSP_OUTSIDE, CSS_LSP_INSIDE };
enum css_border_style { CSS_BST_UNSET = -1, CSS_BST_NONE = 0, CSS_BST_SOLID, CSS_BST_DASHED, CSS_BST_DOTTED };

/* Animation/transition easing; bezier presets are solved in anim.c. */
enum css_easing {
  CSS_EASE_LINEAR = 0,
  CSS_EASE,
  CSS_EASE_IN,
  CSS_EASE_OUT,
  CSS_EASE_IN_OUT,
  CSS_EASE_STEP_START,
  CSS_EASE_STEP_END
};
enum { CSS_ADIR_NORMAL = 0, CSS_ADIR_REVERSE, CSS_ADIR_ALTERNATE, CSS_ADIR_ALTERNATE_REVERSE };
enum { CSS_AFILL_NONE = 0, CSS_AFILL_FORWARDS, CSS_AFILL_BACKWARDS, CSS_AFILL_BOTH };

/* Animatable property index (anim.c walks 0..ANP_COUNT); transitions use a (1<<ANP_*) bitmask. */
enum css_anim_prop {
  ANP_COLOR = 0,
  ANP_BG,
  ANP_BORDER_COLOR,
  ANP_OUTLINE_COLOR,
  ANP_OPACITY,
  ANP_WIDTH,
  ANP_HEIGHT,
  ANP_MIN_W,
  ANP_MIN_H,
  ANP_MAX_W,
  ANP_MAX_H,
  ANP_MARGIN_T,
  ANP_MARGIN_R,
  ANP_MARGIN_B,
  ANP_MARGIN_L,
  ANP_PADDING_T,
  ANP_PADDING_R,
  ANP_PADDING_B,
  ANP_PADDING_L,
  ANP_TOP,
  ANP_RIGHT,
  ANP_BOTTOM,
  ANP_LEFT,
  ANP_RADIUS,
  ANP_OUTLINE_W,
  ANP_FONT_SIZE,
  ANP_COUNT
};
#define TRANS_ALL (1u << 31) /* transition-property: all */
#define CSS_ANIM_NAME 32

/* A computed-style delta; unset fields are -1 (disp UNSET = 0) so deltas layer.
 * Lengths are px>=0, margin may be -2 (auto), a percent size keeps px -1 and sets pct_*. */
#define CSS_FAM_MAX 48    /* one family name */
#define CSS_FAM_STACK 160 /* a whole font-family stack: "A", "B", sans-serif    */

/* background-image layer: only the topmost image of a comma list is painted. */
enum css_bg_kind { CSS_BGK_NONE = 0, CSS_BGK_URL, CSS_BGK_LINEAR, CSS_BGK_RADIAL };
enum { CSS_BGR_REPEAT = 0, CSS_BGR_NO_REPEAT, CSS_BGR_REPEAT_X, CSS_BGR_REPEAT_Y };
enum { CSS_BGS_AUTO = 0, CSS_BGS_COVER, CSS_BGS_CONTAIN, CSS_BGS_LEN };
#define CSS_BG_STOPS 8
#define CSS_BG_URL 128
struct css_gstop {
  int color;
  short pos;
}; /* color 0xAARRGGBB; pos = percent 0..100, -1 auto */
struct css_bg_image {
  signed char kind;       /* enum css_bg_kind, -1 unset */
  signed char repeat;     /* CSS_BGR_*, -1 unset */
  signed char size_kind;  /* CSS_BGS_*, -1 unset */
  signed char rad_circle; /* radial: 1 circle, 0 ellipse */
  short angle;            /* linear-gradient angle, degrees: 0 = to top, 90 = to right */
  short size_w, size_h;   /* CSS_BGS_LEN: px>=0, -1 auto component */
  short pos_x, pos_y;     /* background-position, percent 0..100; -1 unset */
  unsigned char nstops;
  short img_idx; /* resolved decoded-image index (renderer sets), -1 none */
  struct css_gstop stops[CSS_BG_STOPS];
  char url[CSS_BG_URL]; /* url() target, unresolved ("" = none) */
};

#define CSS_SVG_DECL 160
struct css_style {
  int color;
  int bg;
  signed char bold;
  signed char under;
  signed char strike;
  signed char disp;
  signed char center; /* text-align: 1 = center, 0 = left/other, -1 unset */
  int width, height;
  int max_w, max_h;                       /* max-width/height: px or -1 */
  int min_w, min_h;                       /* min-width/height: px or -1 */
  short pct_w, pct_h;                     /* width/height as a percent, else -1 */
  short pct_maxw, pct_maxh;               /* max-width/height as a percent, else -1 */
  short pct_minw, pct_minh;               /* min-width/height as a percent, else -1 */
  signed char box_sizing;                 /* 1 border-box, 0 content-box, -1 unset */
  int font_px;                            /* font-size in px (incl rem/keywords), -1 unset */
  short font_pct;                         /* font-size as a percent of the parent, else -1 */
  int radius;                             /* border-radius px (uniform), -1 unset */
  short sh_dx, sh_dy, sh_blur, sh_spread; /* box-shadow geometry, px */
  int sh_color;                           /* box-shadow 0xAARRGGBB, -1 = no shadow */
  short margin[4];                        /* T,R,B,L: px>=0, -2 auto, -1 unset */
  short padding[4];                       /* T,R,B,L: px>=0, -1 unset */
  short border_w[4];                      /* T,R,B,L: px>=0, -1 unset */
  signed char border_style[4];            /* T,R,B,L: CSS_BST_*, -1 unset */
  int border_color;                       /* 0xRRGGBB or -1 */
  int border_side_color[4];               /* T,R,B,L: colour or -1 unset */
  signed char pos;                        /* enum css_pos, or -1 unset */
  int top, right, bottom, left;           /* px or -1 */
  signed char flex;                       /* 1 = display:flex/inline-flex, 0 not, -1 unset */
  signed char flex_dir;                   /* 0 row, 1 column, -1 unset */
  signed char flex_wrap;                  /* 0 nowrap, 1 wrap, -1 unset */
  short flex_grow;                        /* >=0, -1 unset */
  short flex_shrink;                      /* >=0, -1 unset (default 1) */
  int flex_basis;                         /* main-size px>=0, -1 auto/unset */
  short pct_basis;                        /* flex-basis as a percent, else -1 */
  signed char justify;                    /* enum css_justify, -1 unset */
  signed char align_items;                /* enum css_align, -1 unset */
  short gap_row, gap_col;                 /* row/column gap px>=0, -1 unset */
  /* CSS grid: container track templates + auto-flow, item spans; -1 = unset. */
  signed char grid;               /* 1 = display:grid/inline-grid, 0 not, -1 unset */
  signed char grid_flow;          /* grid-auto-flow: 0 row, 1 column, -1 unset */
  signed char n_cols, n_rows;     /* explicit template track counts, -1 unset */
  signed char col_span, row_span; /* grid item span (>=1), -1 unset */
  signed char justify_items;      /* enum css_align (inline axis), -1 unset */
  struct css_track grid_cols[CSS_GRID_MAX];
  struct css_track grid_rows[CSS_GRID_MAX];
  signed char overflow;    /* 0 visible, 1 clipped, -1 unset */
  signed char italic;      /* font-style: 1 italic/oblique, 0 normal, -1 unset */
  signed char small_caps;  /* font-variant[-caps]: 1 small-caps, 0 normal, -1 unset */
  signed char ttx;         /* text-transform: enum css_ttx, -1 unset */
  signed char white_space; /* enum css_ws, -1 unset */
  signed char list_style;  /* enum css_list (DEFAULT=0 means unset/inherit) */
  signed char list_pos;    /* enum css_list_pos (DEFAULT=0 means unset/inherit) */
  signed char opacity;     /* 0..100 percent, -1 unset */
  short line_height;       /* line-height px, -1 unset */
  short lh_pct;            /* line-height percent of font-size, -1 unset (unitless N stored as N*100) */
  short outline_w;         /* outline width px, -1 unset */
  int outline_color;       /* outline 0xRRGGBB, -1 unset */
  short z_index;           /* stacking order; CSS_Z_AUTO = unset/auto */
  /* SVG paint: -1 unset, -2 none, -3 currentColor, else 0xRRGGBB. */
  int fill, stroke;
  /* SVG presentation declarations ("name:value;" in cascade order) for the inline-SVG serializer. */
  char svg_decl[CSS_SVG_DECL];
  /* Animation and transition parameters read by anim.c; -1 = unset (anim_iter -2 unset, -1 infinite). */
  char anim_name[CSS_ANIM_NAME]; /* animation-name (@keyframes), "" = none */
  int anim_dur;                  /* animation-duration ms, -1 unset */
  int anim_delay;                /* animation-delay ms, -1 unset */
  int anim_iter;                 /* iteration count, -1 infinite, -2 unset */
  signed char anim_dir;          /* enum CSS_ADIR_*, -1 unset */
  signed char anim_fill;         /* enum CSS_AFILL_*, -1 unset */
  signed char anim_timing;       /* enum css_easing, -1 unset */
  unsigned trans_mask;           /* (1<<ANP_*) bits + TRANS_ALL, 0 = none */
  int trans_dur;                 /* transition-duration ms, -1 unset */
  int trans_delay;               /* transition-delay ms, -1 unset */
  signed char trans_timing;      /* enum css_easing, -1 unset */
  struct css_bg_image bgi;       /* background image/gradient layer (kind<0 unset) */
  /* ::before / ::after generated content (decoded, "" = none). */
  char content[48];
  char fam[CSS_FAM_STACK];
};

#define CSS_Z_AUTO (-32768) /* z_index sentinel: auto (no explicit stacking level) */

typedef struct css_sheet css_sheet;

void css_style_reset(struct css_style *s);
void css_bgi_reset(struct css_bg_image *b); /* reset a background layer to unset */

css_sheet *css_new(void);
/* Add a sheet source; `base` is its load URL, used to resolve relative @font-face src. */
void css_add(css_sheet *sheet, const char *src, int len, const char *base);
void css_free(css_sheet *sheet);

/* Viewport that @media queries test against; set before each cascade and on resize. */
void css_set_viewport(int width, int height);
unsigned css_viewport_generation(void);

/* @font-face faces: family + first raw TTF/OTF url (woff/woff2 are skipped, not decodable). */
int css_fontface_count(const css_sheet *sheet);
/* `base` (may be 0) receives the declaring sheet's URL. */
void css_fontface_at(const css_sheet *sheet, int i, const char **family, const char **url, const char **base);

/* A parsed selector list for querySelector/matches; css_sel_parse returns NULL if nothing parses. */
struct sel_list;
struct sel_list *css_sel_parse(const char *s);
bool css_sel_match(const struct sel_list *l, const struct dom_node *el);
void css_sel_free(struct sel_list *l);

/* Fold the sheet's matching rules for `el` into *out (specificity/order). */
void css_apply_sheet(css_sheet *sheet, const struct dom_node *el, struct css_style *out);
/* Shadow scoping for the next css_apply_sheet calls. `host` is what :host matches. mode picks the rules used:
 * NORMAL all but ::slotted/::part, HOST_ONLY just those whose subject is :host (styling the host itself),
 * SLOTTED the ::slotted() rules (styling a light child), PART the ::part() rules (styling a shadow element).
 * css_set_primary names the page sheet whose custom properties every sheet sees. */
enum { CSS_SCOPE_NORMAL, CSS_SCOPE_HOST_ONLY, CSS_SCOPE_SLOTTED, CSS_SCOPE_PART };
void css_set_scope(const struct dom_node *host, int mode);
void css_set_primary(css_sheet *sheet);
int css_mark(const css_sheet *sheet);          /* position after the rules added so far */
void css_truncate(css_sheet *sheet, int mark); /* drop every rule added after css_mark() returned `mark` */
/* Fold a style="" declaration block into *out (highest priority). */
void css_apply_inline(const char *decls, struct css_style *out);
/* Fold HTML presentational attrs into *out (lowest priority). */
void css_apply_attrs(const struct dom_node *el, struct css_style *out);

/* Cascade-winning background-color for `el` as 0xAARRGGBB in *out; returns 1 if set. */
int css_bg_argb(css_sheet *sheet, const struct dom_node *el, unsigned *out);

/* Fold ::before/::after rules for `el` into *out; returns 1 if there is content to render. */
enum { CSS_PSEUDO_BEFORE = 1, CSS_PSEUDO_AFTER = 2 };
int css_pseudo_content(css_sheet *sheet, const struct dom_node *el, int which, struct css_style *out);

/* 1 if the sheet declares any ::before/::after rule. */
int css_sheet_has_pseudo(const css_sheet *sheet);

/* Bumped by every css_add; with css_viewport_generation() it keys cached styles. */
unsigned css_sheet_generation(const css_sheet *sheet);

/* @keyframes lookup for anim.c; stops are an offset (permille) + a declaration delta. */
struct css_keyframes;
const struct css_keyframes *css_keyframes_find(css_sheet *sheet, const char *name);
int css_keyframes_count(const struct css_keyframes *kf);
/* The i-th stop; *decl is borrowed until the sheet is freed. Returns 0 out of range. */
int css_keyframes_stop(const struct css_keyframes *kf, int i, int *permille, const struct css_style **decl);

/* Devtools Styles pane: rules matching `el`, winner first. Returns the number written (<= cap). */
struct css_match {
  char selector[160];
  char media[128];
  int active; /* 1 = the @media currently matches */
  struct css_style decl;
  const char *raw; /* authored declaration text (for devtools), or "" */
};
int css_matches(css_sheet *sheet, const struct dom_node *el, struct css_match *out, int cap);

#endif /* OWOS_CSS_H */
