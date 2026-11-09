/* layout.h , the CSS box tree the browser lays out and paints. The inline content of a block */
#ifndef OWOS_LAYOUT_H
#define OWOS_LAYOUT_H

#include "css.h"
#include "dom.h"
#include <stddef.h>

enum lbox_kind { LB_BLOCK, LB_FLEX, LB_GRID, LB_TEXT, LB_REPLACED };

struct ledge {
  short t, r, b, l;
}; /* margin / border / padding, px (resolved) */

/* Rarely-set properties, allocated per box that uses any (NULL otherwise). */
struct lext {
  unsigned backdrop; /* modal ::backdrop 0xAARRGGBB when backdrop_set, else the default dimming */
  unsigned char backdrop_set;
  signed char n_areas;
  struct css_area areas[CSS_AREA_MAX];           /* grid container's named areas */
  char area[CSS_AREA_NAME];                      /* grid item's grid-area name */
  short tr_x, tr_y, tr_xp, tr_yp;                /* translate: px, and percent of the border box */
  short flt_bright;                              /* filter brightness, permille (1000 identity) */
  signed char flt_gray, flt_invert, flt_opacity; /* filter percent */
  signed char clip_kind, clip_n;
  short clip_v[CSS_CLIP_PTS * 2];
  signed char clip_unit[CSS_CLIP_PTS * 2];
  /* a transform with a linear part (rotate, scale, skew): painted after the fact, so layout never sees it */
  unsigned char mat;
  int m[4];               /* Q12 a b c d */
  short ox, oy, oxp, oyp; /* transform-origin: px and percent of the border box */
};

struct lbox {
  struct dom_node *node; /* source element (NULL = anon), for spans/devtools */
  unsigned char kind;
  unsigned char flex_dir;  /* 0 row / 1 column, when kind==LB_FLEX */
  unsigned char flex_wrap; /* 0 nowrap / 1 wrap, when kind==LB_FLEX */
  signed char justify;     /* enum css_justify (main axis), or -1   */
  signed char align_items; /* enum css_align (cross axis), or -1    */
  short gap_row, gap_col;  /* flex/grid gaps, px (0 if unset)  */
  /* CSS grid (kind==LB_GRID container / any item) */
  signed char grid_flow;
  signed char n_cols, n_rows;
  signed char col_span, row_span;
  signed char justify_items;
  struct css_track cols_t[CSS_GRID_MAX], rows_t[CSS_GRID_MAX];
  short col_start, row_start; /* grid item start line, 1-based, 0 auto */
  signed char align_self;     /* enum css_align, -1/AUTO = the container's align-items */
  int aspect;                 /* aspect-ratio * 1000, 0 none */
  unsigned char hidden;       /* visibility:hidden (inherited): paint no background or border */
  unsigned char bg_clip;      /* background-clip: 0 border box, 1 padding box, 2 content box */
  struct lext *ext;
  signed char pos;      /* enum css_pos, or -1 (static/unset)   */
  signed char overflow; /* 0 visible / 1 hidden / 2 scroll / -1 unset */

  struct ledge margin, border, padding;
  signed char border_style[4];
  unsigned border_side_color[4];
  unsigned char ml_auto, mr_auto; /* left/right margin was `auto` (centring) */
  unsigned char vcenter;   /* position:fixed with auto top/bottom margins and no top/bottom: centred vertically */
  unsigned border_color;   /* 0xRRGGBB, or BG_NONE = none */
  unsigned bg;             /* 0xRRGGBB, or BG_NONE = none */
  struct css_bg_image bgi; /* background image/gradient layer (kind<=0 = none) */
  unsigned char border_box;
  int radius;                             /* border-radius px (0 = square corners) */
  short sh_dx, sh_dy, sh_blur, sh_spread; /* box-shadow geometry, px */
  unsigned sh_color;                      /* box-shadow 0xAARRGGBB, BG_NONE = no shadow */
  int spec_w, spec_h;                     /* >=0 px, -1 auto/unset */
  short pct_w, pct_h;                     /* width/height as a percent, else -1 */
  int max_w, max_h;                       /* px, or -1 */
  int min_w, min_h;                       /* px, or -1 */
  short pct_maxw, pct_maxh;               /* max-width/height as a percent, else -1 */
  short pct_minw, pct_minh;               /* min-width/height as a percent, else -1 */
  short flex_grow;                        /* >=0, -1 unset */
  short flex_shrink;                      /* >=0, -1 unset (default 1 at layout) */
  int flex_basis;                         /* main-size px, -1 auto/unset */
  short pct_basis;                        /* flex-basis as a percent, else -1 */
  int off_t, off_r, off_b, off_l;         /* position offsets, px or -1 unset */
  signed char opacity;                    /* 0..100 percent, -1 unset (= fully opaque) */
  short outline_w;                        /* outline width px (0 = none) */
  unsigned outline_color;                 /* outline 0xRRGGBB, or BG_NONE */
  short z_index;                          /* stacking level, CSS_Z_AUTO = auto */

  int t0, t1;  /* glyph-stream slice (t0==t1 => no own text) */
  int img_idx; /* LB_REPLACED -> B->pg->imgs index, else -1 */

  int parent, end; /* parent index (-1 = top), index past last descendant */

  /* Resolved geometry (filled by the layout pass in later stages), document px */
  int x, y, w, h;     /* margin box */
  int cx, cy, cw, ch; /* content box (inside border + padding) */
};

void layout_reset(void);                 /* start a fresh tree for a new walk */
int layout_rot_boxes(int *out, int max); /* outermost boxes with a rotate/scale/skew transform, in tree order */
void layout_free(void);                  /* drop the tree (page teardown) */
/* HTML-walk hooks */
void layout_box_open(struct dom_node *el, const struct css_style *cs, int t0);
void layout_box_close(int t1);

struct lbox *layout_pool(int *n); /* the box array (pre-order) + count */

static inline unsigned layout_mix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 16777619u;
}

/* Fold every field of `b` that decides line wrapping or box geometry into hash `h` */
static inline unsigned layout_box_sig(const struct lbox *b, unsigned h)
{
  h = layout_mix(h, (unsigned)b->kind | ((unsigned)b->flex_dir << 8) | ((unsigned)b->flex_wrap << 16) |
                        ((unsigned)(unsigned char)b->grid_flow << 24));
  h = layout_mix(h, (unsigned)(unsigned char)b->justify | ((unsigned)(unsigned char)b->align_items << 8) |
                        ((unsigned)(unsigned char)b->pos << 16) | ((unsigned)(unsigned char)b->justify_items << 24));
  h = layout_mix(h, (unsigned)(unsigned short)b->gap_row | ((unsigned)(unsigned short)b->gap_col << 16));
  h = layout_mix(h, (unsigned)(unsigned char)b->n_cols | ((unsigned)(unsigned char)b->n_rows << 8) |
                        ((unsigned)(unsigned char)b->col_span << 16) | ((unsigned)(unsigned char)b->row_span << 24));
  for (int i = 0; i < CSS_GRID_MAX; i++) {
    h = layout_mix(h, (unsigned)(unsigned char)b->cols_t[i].kind | ((unsigned)(unsigned short)b->cols_t[i].v << 8));
    h = layout_mix(h, (unsigned)(unsigned char)b->rows_t[i].kind | ((unsigned)(unsigned short)b->rows_t[i].v << 8));
  }
  const struct ledge *e[3] = {&b->margin, &b->border, &b->padding};
  for (int i = 0; i < 3; i++) {
    h = layout_mix(h, (unsigned)(unsigned short)e[i]->t | ((unsigned)(unsigned short)e[i]->r << 16));
    h = layout_mix(h, (unsigned)(unsigned short)e[i]->b | ((unsigned)(unsigned short)e[i]->l << 16));
  }
  h = layout_mix(h, (unsigned)b->ml_auto | ((unsigned)b->mr_auto << 8) | ((unsigned)b->border_box << 16));
  h = layout_mix(h, (unsigned)(unsigned short)b->col_start | ((unsigned)(unsigned short)b->row_start << 16));
  h = layout_mix(h, (unsigned)(unsigned char)b->align_self);
  h = layout_mix(h, (unsigned)b->aspect);
  if (b->ext) {
    const unsigned char *raw = (const unsigned char *)b->ext;
    for (unsigned i = 0; i < offsetof(struct lext, flt_bright); i++)
      h = layout_mix(h, raw[i]);
  }
  h = layout_mix(h, (unsigned)b->spec_w);
  h = layout_mix(h, (unsigned)b->spec_h);
  h = layout_mix(h, (unsigned)(unsigned short)b->pct_w | ((unsigned)(unsigned short)b->pct_h << 16));
  h = layout_mix(h, (unsigned)b->max_w);
  h = layout_mix(h, (unsigned)b->max_h);
  h = layout_mix(h, (unsigned)b->min_w);
  h = layout_mix(h, (unsigned)b->min_h);
  h = layout_mix(h, (unsigned)(unsigned short)b->pct_maxw | ((unsigned)(unsigned short)b->pct_maxh << 16));
  h = layout_mix(h, (unsigned)(unsigned short)b->pct_minw | ((unsigned)(unsigned short)b->pct_minh << 16));
  h = layout_mix(h, (unsigned)(unsigned short)b->flex_grow | ((unsigned)(unsigned short)b->flex_shrink << 16));
  h = layout_mix(h, (unsigned)b->flex_basis);
  h = layout_mix(h, (unsigned)(unsigned short)b->pct_basis);
  h = layout_mix(h, (unsigned)b->off_t);
  h = layout_mix(h, (unsigned)b->off_r);
  h = layout_mix(h, (unsigned)b->off_b);
  h = layout_mix(h, (unsigned)b->off_l);
  h = layout_mix(h, (unsigned)b->t0);
  h = layout_mix(h, (unsigned)b->t1);
  h = layout_mix(h, (unsigned)b->parent);
  return layout_mix(h, (unsigned)b->end);
}

#endif /* OWOS_LAYOUT_H */
