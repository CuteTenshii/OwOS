/* layout.c — build + own the CSS box tree (see layout.h).
 *
 * Stage 1 builds the tree from the HTML walk and copies each element's box
 * style; the renderer derives each box's pixel rectangle from the wrapped line
 * table and paints backgrounds behind the text. Later stages make the box
 * geometry authoritative (it drives text wrapping, then scrolling). */
#include "layout.h"
#include "browser.h" /* BG_NONE, blk_lock/blk_unlock */
#include "owos.h"

static struct lbox *g_pool;
static int g_n, g_cap;
static int g_cur = -1; /* index of the currently open box, -1 = none */

void layout_reset(void)
{
  g_n = 0;
  g_cur = -1;
}

void layout_free(void)
{
  blk_lock();
  free(g_pool);
  g_pool = 0;
  g_n = g_cap = 0;
  g_cur = -1;
  blk_unlock();
}

struct lbox *layout_pool(int *n)
{
  *n = g_n;
  return g_pool;
}

/* Grow the arena to hold one more box. The realloc may MOVE the arena, so guard
 * it against a concurrent render read with blk_lock (links are indices, so a
 * move is otherwise harmless). Returns the new slot, or -1 on OOM (box
 * dropped). */
static int box_alloc(void)
{
  if (g_n >= g_cap) {
    int nc = g_cap ? g_cap * 2 : 256;
    blk_lock();
    struct lbox *p = realloc(g_pool, (unsigned)nc * sizeof *p);
    if (p) {
      g_pool = p;
      g_cap = nc;
    }
    blk_unlock();
    if (!p)
      return -1;
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
  if (i < 0)
    return;
  struct lbox *b = &g_pool[i];
  b->node = el;
  b->kind = (cs->grid == 1) ? LB_GRID : (cs->flex == 1) ? LB_FLEX : LB_BLOCK;
  b->grid_flow = (signed char)(cs->grid_flow == 1 ? 1 : 0);
  b->n_cols = cs->n_cols;
  b->n_rows = cs->n_rows;
  b->col_span = (signed char)(cs->col_span > 0 ? cs->col_span : 1);
  b->row_span = (signed char)(cs->row_span > 0 ? cs->row_span : 1);
  b->justify_items = cs->justify_items;
  if (cs->n_cols > 0)
    for (int i = 0; i < cs->n_cols && i < CSS_GRID_MAX; i++)
      b->cols_t[i] = cs->grid_cols[i];
  if (cs->n_rows > 0)
    for (int i = 0; i < cs->n_rows && i < CSS_GRID_MAX; i++)
      b->rows_t[i] = cs->grid_rows[i];
  b->flex_dir = (unsigned char)(cs->flex_dir == 1 ? 1 : 0);
  b->flex_wrap = (unsigned char)(cs->flex_wrap == 1 ? 1 : 0);
  b->justify = cs->justify;
  b->align_items = cs->align_items;
  b->gap_row = (short)(cs->gap_row > 0 ? cs->gap_row : 0);
  b->gap_col = (short)(cs->gap_col > 0 ? cs->gap_col : 0);
  b->pos = cs->pos;
  b->overflow = cs->overflow;
  b->margin = edge4(cs->margin);                     /* auto (-2) folds to 0; the auto flags drive centring */
  b->ml_auto = (unsigned char)(cs->margin[3] == -2); /* margin[] is T,R,B,L */
  b->mr_auto = (unsigned char)(cs->margin[1] == -2);
  b->border = edge4(cs->border_w);
  b->padding = edge4(cs->padding);
  b->border_color = cs->border_color >= 0 ? (unsigned)cs->border_color : BG_NONE;
  b->bg = cs->bg >= 0 ? (unsigned)cs->bg : BG_NONE;
  b->bgi = cs->bgi;
  b->border_box = (unsigned char)(cs->box_sizing == 1);
  b->radius = cs->radius > 0 ? cs->radius : 0;
  b->sh_color = cs->sh_color >= 0 ? (unsigned)cs->sh_color : BG_NONE;
  b->sh_dx = cs->sh_dx;
  b->sh_dy = cs->sh_dy;
  b->sh_blur = cs->sh_blur;
  b->sh_spread = cs->sh_spread;
  b->spec_w = cs->width;
  b->pct_w = cs->pct_w;
  b->spec_h = cs->height;
  b->pct_h = cs->pct_h;
  b->max_w = cs->max_w;
  b->max_h = cs->max_h;
  b->min_w = cs->min_w;
  b->min_h = cs->min_h;
  b->pct_maxw = cs->pct_maxw;
  b->pct_maxh = cs->pct_maxh;
  b->pct_minw = cs->pct_minw;
  b->pct_minh = cs->pct_minh;
  b->flex_grow = cs->flex_grow;
  b->flex_shrink = cs->flex_shrink;
  b->flex_basis = cs->flex_basis;
  b->pct_basis = cs->pct_basis;
  b->off_t = cs->top;
  b->off_r = cs->right;
  b->off_b = cs->bottom;
  b->off_l = cs->left;
  b->opacity = cs->opacity;
  b->outline_w = cs->outline_w > 0 ? cs->outline_w : 0;
  b->outline_color = cs->outline_color >= 0 ? (unsigned)cs->outline_color : BG_NONE;
  b->z_index = cs->z_index;
  b->t0 = t0;
  b->t1 = t0;
  b->img_idx = -1;
  b->parent = g_cur;
  b->end = i + 1;
  b->x = b->y = b->w = b->h = b->cx = b->cy = b->cw = b->ch = 0;
  g_cur = i;
}

void layout_box_close(int t1)
{
  if (g_cur < 0)
    return;
  g_pool[g_cur].t1 = t1;
  g_pool[g_cur].end = g_n; /* every box created since open is a descendant */
  g_cur = g_pool[g_cur].parent;
}
