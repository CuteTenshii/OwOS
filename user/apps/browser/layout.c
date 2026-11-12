/* layout.c , build + own the CSS box tree (see layout.h). Stage 1 builds the tree from the HTML */
#include "layout.h"
#include "browser.h" /* BG_NONE, browser_buf_lock/browser_buf_unlock */
#include "owos.h"

static struct lbox *g_pool;
static int g_n, g_cap;
static int g_cur = -1; /* index of the currently open box, -1 = none */
static bool g_has_rot; /* some box has a transform with a linear part */

static void exts_free(void)
{
  for (int i = 0; i < g_n; i++) {
    free(g_pool[i].ext);
    g_pool[i].ext = 0;
  }
}

void layout_reset(void)
{
  browser_buf_lock(); /* the painter dereferences box extras under this lock */
  exts_free();
  g_n = 0;
  g_cur = -1;
  g_has_rot = false;
  browser_buf_unlock();
}

int layout_rot_boxes(int *out, int max)
{
  int n = 0, next = 0;
  for (int i = 0; g_has_rot && i < g_n && n < max; i++) {
    if (i < next || !g_pool[i].ext || !g_pool[i].ext->mat)
      continue;
    out[n++] = i;
    next = g_pool[i].end; /* a transformed box inside another is painted with it */
  }
  return n;
}

void layout_free(void)
{
  browser_buf_lock();
  exts_free();
  free(g_pool);
  g_pool = 0;
  g_n = g_cap = 0;
  g_cur = -1;
  browser_buf_unlock();
}

struct lbox *layout_pool(int *n)
{
  *n = g_n;
  return g_pool;
}

/* Grow the arena to hold one more box */
static int box_alloc(void)
{
  if (g_n >= g_cap) {
    int nc = g_cap ? g_cap * 2 : 256;
    browser_buf_lock();
    struct lbox *p = realloc(g_pool, (unsigned)nc * sizeof *p);
    if (p) {
      g_pool = p;
      g_cap = nc;
    }
    browser_buf_unlock();
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

/* The rarely-set properties of `cs`, or NULL when it sets none of them. */
static struct lext *ext_new(const struct css_style *cs)
{
  bool tr = cs->has_tr == 1, mat = cs->tr_mat == 1, flt = cs->flt_bright >= 0, clip = cs->clip_kind > CSS_CLIP_NONE;
  if (cs->n_areas <= 0 && !cs->area[0] && !tr && !flt && !clip && !cs->backdrop_set)
    return 0;
  struct lext *x = calloc(1, sizeof *x);
  if (!x)
    return 0;
  if (cs->n_areas > 0) {
    x->n_areas = cs->n_areas;
    memcpy(x->areas, cs->areas, (unsigned)cs->n_areas * sizeof *x->areas);
  }
  for (int i = 0; cs->area[i] && i < CSS_AREA_NAME - 1; i++)
    x->area[i] = cs->area[i];
  x->backdrop = cs->backdrop;
  x->backdrop_set = (unsigned char)cs->backdrop_set;
  if (mat) {
    x->mat = 1;
    memcpy(x->m, cs->tr_m, sizeof x->m);
    x->tr_x = cs->tr_x;
    x->tr_y = cs->tr_y;
    x->tr_xp = cs->tr_xp;
    x->tr_yp = cs->tr_yp;
    x->ox = cs->tr_ox;
    x->oy = cs->tr_oy;
    x->oxp = cs->tr_oxp;
    x->oyp = cs->tr_oyp;
  } else if (tr) {
    x->tr_x = cs->tr_x;
    x->tr_y = cs->tr_y;
    x->tr_xp = cs->tr_xp;
    x->tr_yp = cs->tr_yp;
  }
  x->flt_bright = 1000;
  x->flt_opacity = 100;
  if (flt) {
    x->flt_bright = cs->flt_bright;
    x->flt_gray = cs->flt_gray;
    x->flt_invert = cs->flt_invert;
    x->flt_opacity = cs->flt_opacity;
  }
  if (clip) {
    x->clip_kind = cs->clip_kind;
    x->clip_n = cs->clip_n;
    memcpy(x->clip_v, cs->clip_v, (unsigned)cs->clip_n * sizeof *x->clip_v);
    memcpy(x->clip_unit, cs->clip_unit, (unsigned)cs->clip_n);
  }
  return x;
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
  b->col_start = (short)(cs->col_start > 0 ? cs->col_start : 0);
  b->row_start = (short)(cs->row_start > 0 ? cs->row_start : 0);
  b->align_self = cs->align_self;
  b->aspect = cs->aspect > 0 ? cs->aspect : 0;
  b->hidden = (unsigned char)(cs->visibility == 0);
  b->bg_clip = (unsigned char)(cs->bg_clip > 0 ? cs->bg_clip : 0);
  b->ext = ext_new(cs);
  g_has_rot |= b->ext && b->ext->mat;
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
  b->vcenter = (unsigned char)(cs->pos == CSS_POS_FIXED && cs->top < 0 && cs->bottom < 0 && cs->margin[0] == -2 &&
                               cs->margin[2] == -2);
  b->border_color = cs->border_color >= 0 ? (unsigned)cs->border_color : BG_NONE;
  short border_w[4];
  for (int i = 0; i < 4; i++) {
    b->border_style[i] = cs->border_style[i];
    border_w[i] = cs->border_style[i] == CSS_BST_NONE ? 0 : cs->border_w[i];
    b->border_side_color[i] = cs->border_side_color[i] >= 0 ? (unsigned)cs->border_side_color[i] : b->border_color;
  }
  b->border = edge4(border_w);
  b->padding = edge4(cs->padding);
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

bool layout_cover_image(const char *url, int img_idx, short pos_x, short pos_y)
{
  for (int p = g_cur; p >= 0; p = g_pool[p].parent) {
    struct css_bg_image *b = &g_pool[p].bgi;
    if (g_pool[p].pos <= CSS_POS_STATIC)
      continue;
    css_bgi_reset(b);
    b->kind = CSS_BGK_URL;
    b->repeat = CSS_BGR_NO_REPEAT;
    b->size_kind = CSS_BGS_COVER;
    b->pos_x = pos_x >= 0 ? pos_x : 50;
    b->pos_y = pos_y >= 0 ? pos_y : 50;
    b->img_idx = (short)img_idx;
    strlcpy(b->url, url, (int)sizeof b->url);
    return true;
  }
  return false;
}

int layout_clip_radius(void)
{
  if (g_cur < 0 || g_pool[g_cur].overflow != 1 || g_pool[g_cur].radius <= 0)
    return 0;
  int r = g_pool[g_cur].radius - g_pool[g_cur].border.t; /* the padding edge's radius */
  return r > 0 ? r : 0;
}

void layout_box_close(int t1)
{
  if (g_cur < 0)
    return;
  g_pool[g_cur].t1 = t1;
  g_pool[g_cur].end = g_n; /* every box created since open is a descendant */
  g_cur = g_pool[g_cur].parent;
}
