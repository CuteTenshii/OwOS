/* layout.c — build + own the CSS box tree (see layout.h).
 *
 * Stage 1 builds the tree from the HTML walk and copies each element's box style;
 * the renderer derives each box's pixel rectangle from the wrapped line table and
 * paints backgrounds behind the text. Later stages make the box geometry
 * authoritative (it drives text wrapping, then scrolling). */
#include "owos.h"
#include "layout.h"
#include "browser.h"   /* BG_NONE, blk_lock/blk_unlock */

static struct lbox *g_pool;
static int          g_n, g_cap;
static int          g_cur = -1;   /* index of the currently open box, -1 = none */

void layout_reset(void)
{
	g_n = 0;
	g_cur = -1;
}

void layout_free(void)
{
	blk_lock();
	free(g_pool);
	g_pool = 0; g_n = g_cap = 0; g_cur = -1;
	blk_unlock();
}

struct lbox *layout_pool(int *n) { *n = g_n; return g_pool; }

/* Grow the arena to hold one more box. The realloc may MOVE the arena, so guard
 * it against a concurrent render read with blk_lock (links are indices, so a move
 * is otherwise harmless). Returns the new slot, or -1 on OOM (box dropped). */
static int box_alloc(void)
{
	if (g_n >= g_cap) {
		int nc = g_cap ? g_cap * 2 : 256;
		blk_lock();
		struct lbox *p = realloc(g_pool, (unsigned)nc * sizeof *p);
		if (p) { g_pool = p; g_cap = nc; }
		blk_unlock();
		if (!p) return -1;
	}
	return g_n++;
}

static struct ledge edge4(const short v[4])
{
	struct ledge e;
	e.t = v[0] > 0 ? v[0] : 0;
	e.r = v[1] > 0 ? v[1] : 0;
	e.b = v[2] > 0 ? v[2] : 0;
	e.l = v[3] > 0 ? v[3] : 0;
	return e;
}

void layout_box_open(struct dom_node *el, const struct css_style *cs, int t0)
{
	int i = box_alloc();
	if (i < 0) return;
	struct lbox *b = &g_pool[i];
	b->node     = el;
	b->kind     = (cs->flex == 1) ? LB_FLEX : LB_BLOCK;
	b->flex_dir = (unsigned char)(cs->flex_dir == 1 ? 1 : 0);
	b->pos      = cs->pos;
	b->overflow = cs->overflow;
	b->margin   = edge4(cs->margin);   /* -2 auto folded to 0 here; resolved in stage 3 */
	b->border   = edge4(cs->border_w);
	b->padding  = edge4(cs->padding);
	b->border_color = cs->border_color >= 0 ? (unsigned)cs->border_color : BG_NONE;
	b->bg       = cs->bg >= 0 ? (unsigned)cs->bg : BG_NONE;
	b->spec_w   = cs->width;   b->pct_w = cs->pct_w;
	b->spec_h   = cs->height;  b->pct_h = cs->pct_h;
	b->max_w    = cs->max_w;   b->max_h = cs->max_h;
	b->flex_grow = cs->flex_grow;
	b->t0 = t0; b->t1 = t0;
	b->img_idx = -1;
	b->parent = g_cur;
	b->end = i + 1;
	b->x = b->y = b->w = b->h = b->cx = b->cy = b->cw = b->ch = 0;
	g_cur = i;
}

void layout_box_close(int t1)
{
	if (g_cur < 0) return;
	g_pool[g_cur].t1  = t1;
	g_pool[g_cur].end = g_n;       /* every box created since open is a descendant */
	g_cur = g_pool[g_cur].parent;
}
