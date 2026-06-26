/* layout.h — the CSS box tree the browser lays out and paints.
 *
 * The inline content of a block stays the existing glyph stream (browser.h
 * B->pg->text): a box owns a [t0,t1) slice of it, word-wrapped + painted by the
 * flow renderer (main.c) within the box's content rectangle. Only block/flex
 * OUTER geometry lives here — margin/border/padding/background/size/flex/position.
 *
 * The tree is built by hooks the HTML walk calls (layout_box_open/close), so no
 * second CSS cascade is needed: the walk already computed each element's style.
 * Nodes live in one growable arena; links are ARRAY INDICES, not pointers, so a
 * realloc that moves the arena can't dangle them. Descendants are contiguous
 * (the walk is pre-order), so a box records `parent` and `end` (one past its last
 * descendant) and children are recovered by scanning [self+1, end). Worker builds
 * it under blk_lock; the render thread reads it under blk_lock; dropped on
 * page teardown — same discipline as the glyph stream and span table. */
#ifndef OWOS_LAYOUT_H
#define OWOS_LAYOUT_H

#include "dom.h"
#include "css.h"

enum lbox_kind { LB_BLOCK, LB_FLEX, LB_TEXT, LB_REPLACED };

struct ledge { short t, r, b, l; };   /* margin / border / padding, px (resolved) */

struct lbox {
	struct dom_node *node;        /* source element (NULL = anon), for spans/devtools */
	unsigned char    kind;
	unsigned char    flex_dir;    /* 0 row / 1 column, when kind==LB_FLEX */
	signed char      pos;         /* enum css_pos, or -1 (static/unset)   */
	signed char      overflow;    /* 0 visible / 1 clipped / -1 unset     */

	struct ledge margin, border, padding;
	unsigned     border_color;    /* 0xRRGGBB, or BG_NONE = none */
	unsigned     bg;              /* 0xRRGGBB, or BG_NONE = none */
	int          spec_w, spec_h;  /* >=0 px, -1 auto/unset */
	short        pct_w, pct_h;    /* width/height as a percent, else -1 */
	int          max_w, max_h;    /* px, or -1 */
	short        flex_grow;       /* >=0, -1 unset */

	int          t0, t1;          /* glyph-stream slice (t0==t1 => no own text) */
	int          img_idx;         /* LB_REPLACED -> B->pg->imgs index, else -1 */

	int          parent, end;     /* parent index (-1 = top), index past last descendant */

	/* Resolved geometry (filled by the layout pass in later stages), document px. */
	int          x, y, w, h;      /* margin box */
	int          cx, cy, cw, ch;  /* content box (inside border + padding) */
};

void  layout_reset(void);   /* start a fresh tree for a new walk */
void  layout_free(void);    /* drop the tree (page teardown) */
/* HTML-walk hooks: open a block/flex box for `el` (style copied from `cs`), and
 * close the current one. `t0`/`t1` are the current glyph-stream offset (W_n). */
void  layout_box_open(struct dom_node *el, const struct css_style *cs, int t0);
void  layout_box_close(int t1);

struct lbox *layout_pool(int *n);   /* the box array (pre-order) + count */

#endif /* OWOS_LAYOUT_H */
