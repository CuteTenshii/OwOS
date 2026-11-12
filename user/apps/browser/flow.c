#include "main_int.h"

/* Approximate document-relative element box from the wrapped line table; worker thread, holds browser_buf_lock. */
int browser_range_box(int start, int end, int *ox, int *oy, int *ow, int *oh)
{
  *ox = *oy = *ow = *oh = 0;
  if (end <= start)
    end = start + 1; /* zero-width span: one anchor cell */
  int paw = (BW - PAGE_SCROLL_W) - 8;
  if (paw < 8)
    paw = 8;

  struct bctx *c = &g_wk;
  browser_buf_lock();
  c->held = true;
  int first = -1, last = -1, text_rows = 0, img_w = 0;
  for (int r = 0; r < g_fr->lt.n; r++) {
    int rs = g_fr->lt.off[r], rl = g_fr->lt.len[r];
    int hit = rl > 0 ? (rs < end && rs + rl > start) /* text-line overlap */
                     : (rs >= start && rs < end);    /* blank/anchor row  */
    if (!hit)
      continue;
    if (first < 0)
      first = r;
    last = r;
    if (rl > 0)
      text_rows++;
  }
  /* Inline images anchored in the span, and ancestor blocks enclosing them. */
  for (int q = 0; q < B->pg->n_imgs; q++) {
    struct inl_img *im = &B->pg->imgs[q];
    if (im->start_row < 0 || im->text_pos < start || im->text_pos > end)
      continue;
    int s0 = im->start_row, s1 = s0 + (im->rows > 0 ? im->rows : 1) - 1;
    if (first < 0 || s0 < first)
      first = s0;
    if (s1 > last)
      last = s1;
    int dw = 0, dh = 0;
    img_disp_size(im, paw, &dw, &dh);
    if (dw > img_w)
      img_w = dw;
  }
  if (first < 0) {
    c->held = false;
    browser_buf_unlock();
    return 0;
  }

  int x = g_fr->lt.x[first], w = g_fr->lt.w[first];
  int y = g_fr->lt.y[first], h = lt_bottom(&g_fr->lt, last) - g_fr->lt.y[first];
  if (first == last && text_rows == 1) { /* one text line: exact x + width */
    int rs = g_fr->lt.off[first], re = rs + g_fr->lt.len[first];
    const char *t = B->pg->text;
    unsigned cp;
    set_block(c, rs);
    x += c->bindent * INDENT_PX;
    if (c->bcenter == 1 || c->bcenter == 2) {
      int lw = 0;
      for (int p = rs; p < re;) {
        int gp = p;
        p += utf8_decode(&t[p], &t[re], &cp);
        lw += glyph_adv(c, cp, gp);
      }
      int room = (g_fr->lt.w[first] - c->bindent * INDENT_PX) - lw;
      if (room > 0)
        x += c->bcenter == 1 ? room / 2 : room;
    }
    int a = start > rs ? start : rs, b = end < re ? end : re;
    for (int p = rs; p < a;) {
      int gp = p;
      p += utf8_decode(&t[p], &t[re], &cp);
      x += glyph_adv(c, cp, gp);
    }
    w = 0;
    for (int p = a; p < b;) {
      int gp = p;
      p += utf8_decode(&t[p], &t[re], &cp);
      w += glyph_adv(c, cp, gp);
    }
    if (w < 1)
      w = 1;
  } else if (text_rows == 0 && img_w > 0) { /* image-only box */
    w = img_w;
  } else { /* block / multi-line: content col */
    set_block(c, g_fr->lt.off[first]);
    x = g_fr->lt.x[first] + c->bindent * INDENT_PX;
    w = g_fr->lt.w[first] - c->bindent * INDENT_PX;
    if (w < 1)
      w = 1;
  }
  c->held = false;
  browser_buf_unlock();
  *ox = x;
  *oy = y;
  *ow = w;
  *oh = h;
  return 1;
}

int browser_scroll_px(void) { return B->pg->scroll; } /* window.scrollY (document px) */
int browser_doc_height_px(void) { return g_fr->doc_h; }

void browser_viewport_px(int *w, int *h)
{
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  if (w) {
    int vw = BW - PAGE_SCROLL_W;
    *w = vw > 0 ? vw : 0;
  }
  if (h)
    *h = phh;
}

/* CSS box layer, behind a flag; inline text still flows through the shared wrap. */
bool g_box_layout = true;

static int rows_for(int px) { return px > 0 ? (px + LINE_H - 1) / LINE_H : 0; }
static int max0(int v) { return v > 0 ? v : 0; }
/* Reserve `n` LINE_H-tall blank lines. */
static void add_n_blanks(int n, int off, int x, int w)
{
  while (n-- > 0)
    add_line(off, 0, x, w, LINE_H, (int)BODY_PX);
}
/* Reserve one blank line of exactly `h` px; no-op for h<=0. */
static void add_blank_px(int h, int off, int x, int w)
{
  if (h > 0)
    add_line(off, 0, x, w, h, (int)BODY_PX);
}
/* Line height for the block set by set_block. */
static int block_line_h(void)
{
  if (g_wk.blh > 0)
    return g_wk.blh < 6 ? 6 : g_wk.blh; /* explicit CSS line-height */
  if (g_wk.bpx <= 0)
    return LINE_H;                        /* default body font */
  int lh = g_wk.bpx + (g_wk.bpx + 2) / 4; /* ~1.25 line-height */
  return lh < LINE_H ? LINE_H : lh;
}

/* Wrap glyph slice [t0,t1) at origin `base_x` / width `w` px into display lines. */
static void flow_slice(int t0, int t1, int base_x, int w)
{
  /* Buffers may not exist yet after a mid-flight relayout; never scan NULL. */
  if (!B->pg->text || !B->pg->attr)
    return;
  struct bctx *c = &g_wk;
  struct layout_out *o = g_bk;
  const struct page *pg = B->pg;
  if (t1 > pg->text_len)
    t1 = pg->text_len;
  if (w < 8)
    w = 8;
  int i = t0;
  while (i < t1) {
    int j = i;
    while (j < t1 && pg->text[j] != '\n')
      j++;
    set_block(c, i);
    int avail = w - c->bindent * INDENT_PX;
    if (avail < 8)
      avail = 8;
    if (j == i) { /* blank line / replaced-element anchor */
      int img = img_at_pos(i);
      struct media_el *me;
      if (img >= 0) {
        struct inl_img *im = &pg->imgs[img];
        int dw = 0, dh = 0;
        img_disp_size(im, w, &dw, &dh);
        if (img < o->n_img) {
          o->img_rows[img] = 1; /* one display line, exact px tall */
          o->img_row[img] = o->lt.n;
        }
        add_line(i, 0, base_x, w, dh > 0 ? dh : LINE_H, (int)BODY_PX);
      } else if (g_has_media && (me = media_at_text_pos(i)) != 0) {
        int mr = media_widget_rows(me, w, LINE_H);
        struct lmedia *mp = grow_arr(o->med, &o->cap_med, o->n_med + 1, sizeof *mp);
        if (mp) {
          o->med = mp;
          mp[o->n_med++] = (struct lmedia){me, o->lt.n, mr};
        }
        for (int r = 0; r < mr; r++)
          add_line(i, 0, base_x, w, LINE_H, (int)BODY_PX);
      } else {
        add_line(i, 0, base_x, w, block_line_h(), (int)BODY_PX);
      }
    } else {
      int ls = i;
      while (ls < j) {
        int brk;
        if (pg->attr[ls] & ST_NOWRAP) {
          brk = j; /* white-space:nowrap - one line, may overflow */
        } else {
          /* greedy fit by pixel width; hard-break if one word overflows */
          int wd = 0, e = ls, lastsp = -1;
          while (e < j) {
            unsigned cp;
            int nb = utf8_decode(&pg->text[e], &pg->text[j], &cp);
            int cw = glyph_adv(c, cp, e);
            if (wd + cw > avail && e > ls)
              break;
            if (cp == ' ')
              lastsp = e;
            wd += cw;
            e += nb;
          }
          brk = (e < j && lastsp > ls) ? lastsp : e;
        }
        int maxpx, lhmax;
        ru_line_scan(pg->text, pg->fsz, pg->bp, pg->lh, ls, brk, (int)BODY_PX, &maxpx, &lhmax);
        add_line(ls, brk - ls, base_x, w, line_height(maxpx, lhmax), maxpx);
        ls = brk;
        while (ls < j && pg->text[ls] == ' ')
          ls++;
      }
    }
    i = j + 1;
  }
}

/* Resolve a CSS length to px: px, else percent of `basis`, else -1 (auto/unset). */
static int resolve_len(int px, int pct, int basis)
{
  if (px >= 0)
    return px;
  if (pct >= 0)
    return (int)((long)basis * pct / 100);
  return -1;
}

/* Resolve a box's border-box width within `avail_w`; `frame` is horizontal padding+border. */
static int box_width(const struct lbox *b, int avail_w, int frame, int ml, int mr)
{
  int extra = b->border_box ? 0 : frame; /* added to a content-box length */
  int spec = resolve_len(b->spec_w, b->pct_w, avail_w);
  int box_w = spec >= 0 ? spec + extra : avail_w - ml - mr;

  int maxw = resolve_len(b->max_w, b->pct_maxw, avail_w);
  if (maxw >= 0 && box_w > maxw + extra)
    box_w = maxw + extra;
  int minw = resolve_len(b->min_w, b->pct_minw, avail_w);
  if (minw >= 0 && box_w < minw + extra)
    box_w = minw + extra;

  if (box_w < frame + 1)
    box_w = frame + 1; /* room for >=1px of content */
  if (box_w < 16)
    box_w = 16;
  if (box_w > avail_w)
    box_w = avail_w;
  return box_w;
}

/* Minimum content-box height in px (CSS height is a floor), or -1 if content-sized. box_w is the border-box
 * width, which an aspect-ratio turns into a height when none is given. */
static int box_min_content_h(const struct lbox *b, int vframe, int box_w)
{
  int frame = b->border_box ? vframe : 0;
  int h = b->spec_h >= 0 ? b->spec_h - frame : -1;
  if (h < 0 && b->aspect > 0 && b->pct_h < 0) {
    int hframe = b->border.l + b->border.r + b->padding.l + b->padding.r;
    h = (int)((long)(b->border_box ? box_w : box_w - hframe) * 1000 / b->aspect) - frame;
    if (h < 0)
      h = 0;
  }
  int minh = b->min_h >= 0 ? b->min_h - frame : -1;
  int target = h > minh ? h : minh;
  if (b->max_h >= 0) {
    int maxh = b->max_h - frame;
    if (maxh < 0)
      maxh = 0;
    if (target < 0 || target > maxh)
      target = maxh; /* max-height also caps */
  }
  return target;
}

/* --- flex (row): items share a band reserved in the main table, text drawn via the overlay --- */

/* Max-content width of a glyph slice in px. */
static int slice_maxcontent(int t0, int t1)
{
  int best = 0, i = t0;
  while (i <= t1) {
    int j = i;
    while (j < t1 && B->pg->text[j] != '\n')
      j++;
    if (j > i) {
      set_block(&g_wk, i);
      int w = g_wk.bindent * INDENT_PX;
      unsigned cp;
      for (int p = i; p < j;) {
        int gp = p;
        p += utf8_decode(&B->pg->text[p], &B->pg->text[j], &cp);
        w += glyph_adv(&g_wk, cp, gp);
      }
      if (w > best)
        best = w;
    }
    if (j >= t1)
      break;
    i = j + 1;
  }
  return best;
}

/* A flex item's flex-basis as a border-box main size. */
static int flex_base_main(const struct lbox *it, int cont_w)
{
  int frame = it->border.l + it->border.r + it->padding.l + it->padding.r;
  int spec = -1;
  if (it->flex_basis >= 0 || it->pct_basis >= 0)
    spec = resolve_len(it->flex_basis, it->pct_basis, cont_w);
  else if (it->spec_w >= 0 || it->pct_w >= 0)
    spec = resolve_len(it->spec_w, it->pct_w, cont_w);
  if (spec >= 0)
    return spec + (it->border_box ? 0 : frame);
  return slice_maxcontent(it->t0, it->t1) + frame;
}

/* Clamp a border-box main size to min/max-width and a 1px floor. */
static int flex_clamp_main(const struct lbox *it, int v, int cont_w)
{
  int frame = it->border.l + it->border.r + it->padding.l + it->padding.r;
  int extra = it->border_box ? 0 : frame;
  int maxw = resolve_len(it->max_w, it->pct_maxw, cont_w);
  if (maxw >= 0 && v > maxw + extra)
    v = maxw + extra;
  int minw = resolve_len(it->min_w, it->pct_minw, cont_w);
  if (minw >= 0 && v < minw + extra)
    v = minw + extra;
  if (v < frame + 1)
    v = frame + 1;
  return v;
}

/* position:fixed boxes leave the flow and are laid out into the overlay table */
static bool out_of_flow(const struct lbox *b) { return b->pos == CSS_POS_FIXED; }

/* An item's cross-axis alignment: its align-self, else the container's align-items. */
static signed char item_align(const struct lbox *cont, const struct lbox *it)
{
  if (it->align_self >= 0 && it->align_self != CSS_ALIGN_AUTO)
    return it->align_self;
  return cont->align_items < 0 ? CSS_ALIGN_STRETCH : cont->align_items;
}

/* Number of direct child boxes of box `idx`, and (when items != NULL) their indices. */
static int box_children(const struct lbox *bx, int idx, int *items)
{
  int n = 0;
  for (int j = idx + 1; j < bx[idx].end;) {
    if (bx[j].parent == idx && out_of_flow(&bx[j])) {
      j = bx[j].end;
    } else if (bx[j].parent == idx) {
      if (items)
        items[n] = j;
      n++;
      j = bx[j].end;
    } else
      j++;
  }
  return n;
}

/* Lay out a row flex container's children side by side; false if out of memory. */
static bool flow_flex_row(struct lbox *bx, int idx, int content_x, int content_w)
{
  struct lbox *cont = &bx[idx];
  struct lgeom *geo = g_bk->geo;
  int nit = box_children(bx, idx, 0);
  if (nit == 0)
    return true;
  int *mem = malloc((unsigned long)nit * 7 * sizeof *mem);
  if (!mem)
    return false;
  int *items = mem, *base = mem + nit, *main_sz = mem + 2 * nit, *rows_of = mem + 3 * nit;
  int *ov_start = mem + 4 * nit, *item_x = mem + 5 * nit, *toppad = mem + 6 * nit;
  box_children(bx, idx, items);

  int gap = cont->gap_col;
  int total_gap = gap * (nit - 1);

  long sumbase = 0, sumgrow = 0, sumshrinkw = 0;
  for (int k = 0; k < nit; k++) {
    base[k] = flex_base_main(&bx[items[k]], content_w);
    main_sz[k] = base[k];
    sumbase += base[k];
    int g = bx[items[k]].flex_grow;
    if (g < 0)
      g = 0;
    int s = bx[items[k]].flex_shrink;
    if (s < 0)
      s = 1;
    sumgrow += g;
    sumshrinkw += (long)s * base[k];
  }
  long avail = content_w - total_gap;
  long freepx = avail - sumbase;
  if (freepx > 0 && sumgrow > 0) {
    for (int k = 0; k < nit; k++) {
      int g = bx[items[k]].flex_grow;
      if (g < 0)
        g = 0;
      main_sz[k] = base[k] + (int)(freepx * g / sumgrow);
    }
  } else if (freepx < 0 && sumshrinkw > 0) {
    long need = -freepx;
    for (int k = 0; k < nit; k++) {
      int s = bx[items[k]].flex_shrink;
      if (s < 0)
        s = 1;
      main_sz[k] = base[k] - (int)(need * ((long)s * base[k]) / sumshrinkw);
    }
  }
  long used = 0;
  for (int k = 0; k < nit; k++) {
    main_sz[k] = flex_clamp_main(&bx[items[k]], main_sz[k], content_w);
    used += main_sz[k];
  }

  int leftover = (int)(content_w - total_gap - used);
  if (leftover < 0)
    leftover = 0;
  int start = 0, between = 0;
  signed char jc = cont->justify < 0 ? CSS_JUST_START : cont->justify;
  switch (jc) {
  case CSS_JUST_CENTER:
    start = leftover / 2;
    break;
  case CSS_JUST_END:
    start = leftover;
    break;
  case CSS_JUST_BETWEEN:
    if (nit > 1)
      between = leftover / (nit - 1);
    else
      start = leftover / 2;
    break;
  case CSS_JUST_AROUND:
    between = leftover / nit;
    start = between / 2;
    break;
  case CSS_JUST_EVENLY:
    between = leftover / (nit + 1);
    start = between;
    break;
  default:
    break; /* flex-start */
  }

  /* Phase 1: flow each item's text to learn its cross size; band is the tallest item. */
  int band_top = g_bk->lt.n;
  int band_top_y = lt_doc_px(&g_bk->lt); /* pixel top of the band (its rows are added below) */
  int x = content_x + start, band_h = 0;
  for (int k = 0; k < nit; k++) {
    struct lbox *it = &bx[items[k]];
    int fl = it->border.l + it->padding.l, fr = it->border.r + it->padding.r;
    int cw = main_sz[k] - fl - fr;
    if (cw < 4)
      cw = 4;
    item_x[k] = x;
    ov_start[k] = g_bk->n_fs;
    g_ov_active = true;
    g_ov_base = band_top;
    g_ov_n = 0;
    flow_slice(it->t0, it->t1, x + fl, cw);
    g_ov_active = false;
    int vframe = it->border.t + it->border.b + it->padding.t + it->padding.b;
    int content_px = g_ov_n * LINE_H;
    int minch = box_min_content_h(it, vframe, main_sz[k]);
    if (minch > content_px)
      content_px = minch;
    int orows = rows_for(content_px + vframe);
    if (orows < 1)
      orows = 1;
    rows_of[k] = orows;
    toppad[k] = rows_for(it->border.t + it->padding.t);
    if (orows > band_h)
      band_h = orows;
    x += main_sz[k] + gap + between;
  }
  int cvframe = cont->border.t + cont->border.b + cont->padding.t + cont->padding.b;
  int cminch =
      box_min_content_h(cont, cvframe, content_w + cont->border.l + cont->border.r + cont->padding.l + cont->padding.r);
  if (cminch > 0 && rows_for(cminch) > band_h)
    band_h = rows_for(cminch);
  if (band_h < 1)
    band_h = 1;

  /* Phase 2: cross-axis place each item (align-items) and record geometry. */
  for (int k = 0; k < nit; k++) {
    struct lbox *it = &bx[items[k]];
    signed char ai = item_align(cont, it);
    int h_rows = (ai == CSS_ALIGN_STRETCH) ? band_h : rows_of[k];
    int off_rows = 0;
    if (ai == CSS_ALIGN_CENTER)
      off_rows = (band_h - h_rows) / 2;
    else if (ai == CSS_ALIGN_END)
      off_rows = band_h - h_rows;
    int shift = off_rows + toppad[k];
    if (shift) {
      int end = (k + 1 < nit) ? ov_start[k + 1] : g_bk->n_fs;
      for (int s = ov_start[k]; s < end; s++)
        g_bk->fs[s].row += shift;
    }
    int fl = it->border.l + it->padding.l, fr = it->border.r + it->padding.r;
    int ft = it->border.t + it->padding.t;
    int vframe = ft + it->border.b + it->padding.b;
    struct lgeom *g = &geo[items[k]];
    g->x = item_x[k];
    g->w = main_sz[k];
    g->y = band_top_y + off_rows * LINE_H;
    g->h = h_rows * LINE_H;
    g->cx = item_x[k] + fl;
    g->cw = main_sz[k] - fl - fr;
    if (g->cw < 4)
      g->cw = 4;
    g->cy = g->y + ft;
    g->ch = g->h - vframe;
    if (g->ch < 0)
      g->ch = 0;
  }

  add_n_blanks(band_h, cont->t0, content_x, content_w);
  free(mem);
  return true;
}

/* An item's explicit cell: its grid-area name in the container, else its start lines (-1 auto) and spans. */
static void grid_item_pos(const struct lbox *cont, const struct lbox *it, int *c0, int *r0, int *cs, int *rs)
{
  *c0 = it->col_start > 0 ? it->col_start - 1 : -1;
  *r0 = it->row_start > 0 ? it->row_start - 1 : -1;
  *cs = it->col_span > 0 ? it->col_span : 1;
  *rs = it->row_span > 0 ? it->row_span : 1;
  if (it->ext && it->ext->area[0] && cont->ext)
    for (int i = 0; i < cont->ext->n_areas; i++) {
      const struct css_area *a = &cont->ext->areas[i];
      if (!strcmp(a->name, it->ext->area)) {
        *c0 = a->c0;
        *r0 = a->r0;
        *cs = a->c1 - a->c0;
        *rs = a->r1 - a->r0;
        return;
      }
    }
}

/* --- grid: like flex-row, cells are drawn via the band overlay; no dense packing of row spans --- */
/* False when memory runs out, so the caller flows the container as a plain block. */
static bool flow_grid(struct lbox *bx, int idx, int content_x, int content_w)
{
  struct lbox *cont = &bx[idx];
  struct lgeom *geo = g_bk->geo;
  int ncols = cont->n_cols > 0 ? cont->n_cols : 1;
  if (cont->ext)
    for (int i = 0; i < cont->ext->n_areas; i++)
      if (cont->ext->areas[i].c1 > ncols)
        ncols = cont->ext->areas[i].c1;
  if (ncols > CSS_GRID_MAX)
    ncols = CSS_GRID_MAX;

  int nit = box_children(bx, idx, 0);
  if (nit == 0)
    return true;
  int *mem = malloc((unsigned long)nit * 10 * sizeof *mem);
  if (!mem)
    return false;
  int *items = mem, *it_col = mem + nit, *it_row = mem + 2 * nit, *it_cs = mem + 3 * nit, *it_rs = mem + 4 * nit;
  int *rows_of = mem + 5 * nit, *ov_start = mem + 6 * nit, *toppad = mem + 7 * nit, *it_x = mem + 8 * nit,
      *it_w = mem + 9 * nit;
  box_children(bx, idx, items);

  int gcol = cont->gap_col > 0 ? cont->gap_col : 0;
  int grow_gap = cont->gap_row > 0 ? cont->gap_row : 0;

  /* placement: named areas and start lines first claim their cells, the rest auto-flow by row */
  for (int k = 0; k < nit; k++) {
    grid_item_pos(cont, &bx[items[k]], &it_col[k], &it_row[k], &it_cs[k], &it_rs[k]);
    int span = it_cs[k] < CSS_GRID_MAX ? it_cs[k] : 1;
    if (it_col[k] >= 0 && it_col[k] + span > ncols)
      ncols = it_col[k] + span;
  }
  if (ncols > CSS_GRID_MAX)
    ncols = CSS_GRID_MAX;
  int nrows = ru_grid_place(ncols, nit, it_col, it_row, it_cs, it_rs);
  if (nrows < 0) {
    free(mem);
    return false;
  }
  if (nrows < 1)
    nrows = 1;
  int *rmem = malloc(((unsigned long)nrows * 2 + 1) * sizeof *rmem);
  if (!rmem) {
    free(mem);
    return false;
  }
  int *rowh = rmem, *row_off = rmem + nrows;

  /* column track sizing: fixed, auto (max-content), fr, then overflow scale-down */
  int colw[CSS_GRID_MAX], is_fr[CSS_GRID_MAX], fr_w[CSS_GRID_MAX], is_auto[CSS_GRID_MAX];
  int total_gap = gcol * (ncols - 1);
  int avail = content_w - total_gap;
  if (avail < ncols)
    avail = ncols;
  long fr_sum = 0, fixed = 0;
  int auto_cnt = 0;
  for (int c = 0; c < ncols; c++) {
    struct css_track t = c < cont->n_cols   ? cont->cols_t[c]
                         : cont->n_cols > 0 ? (struct css_track){CSS_TRK_AUTO, 0}
                                            : (struct css_track){CSS_TRK_FR, 1};
    is_fr[c] = is_auto[c] = 0;
    fr_w[c] = 0;
    colw[c] = 0;
    if (t.kind == CSS_TRK_PX) {
      colw[c] = t.v > 0 ? t.v : 0;
      fixed += colw[c];
    } else if (t.kind == CSS_TRK_PCT) {
      colw[c] = (int)((long)content_w * t.v / 100);
      if (colw[c] < 0)
        colw[c] = 0;
      fixed += colw[c];
    } else if (t.kind == CSS_TRK_FR) {
      is_fr[c] = 1;
      fr_w[c] = t.v > 0 ? t.v : 1;
      fr_sum += fr_w[c];
    } else {
      is_auto[c] = 1;
      auto_cnt++;
    }
  }
  for (int c = 0; c < ncols; c++) {
    if (!is_auto[c])
      continue;
    int best = 0;
    for (int k = 0; k < nit; k++) {
      if (it_cs[k] != 1 || it_col[k] != c)
        continue;
      struct lbox *it = &bx[items[k]];
      int frame = it->border.l + it->border.r + it->padding.l + it->padding.r;
      int mc = slice_maxcontent(it->t0, it->t1) + frame;
      if (mc > best)
        best = mc;
    }
    colw[c] = best;
    fixed += best;
  }
  long leftover = avail - fixed;
  if (leftover < 0)
    leftover = 0;
  if (fr_sum > 0) {
    for (int c = 0; c < ncols; c++)
      if (is_fr[c])
        colw[c] = (int)(leftover * fr_w[c] / fr_sum);
  } else if (leftover > 0 && auto_cnt > 0) {
    int add = (int)(leftover / auto_cnt);
    for (int c = 0; c < ncols; c++)
      if (is_auto[c])
        colw[c] += add;
  }
  long tot = 0;
  for (int c = 0; c < ncols; c++) {
    if (colw[c] < 0)
      colw[c] = 0;
    tot += colw[c];
  }
  if (tot > avail && tot > 0)
    for (int c = 0; c < ncols; c++)
      colw[c] = (int)((long)colw[c] * avail / tot);
  for (int c = 0; c < ncols; c++)
    if (colw[c] < 1)
      colw[c] = 1;

  int track_x[CSS_GRID_MAX + 1];
  track_x[0] = content_x;
  for (int c = 0; c < ncols; c++)
    track_x[c + 1] = track_x[c] + colw[c] + gcol;

  /* flow each item into the overlay, measuring rows, column x and border-box width */
  int band_top = g_bk->lt.n;
  int band_top_y = lt_doc_px(&g_bk->lt);
  for (int k = 0; k < nit; k++) {
    struct lbox *it = &bx[items[k]];
    int cs = it_cs[k];
    int iw = 0;
    for (int c = it_col[k]; c < it_col[k] + cs && c < ncols; c++)
      iw += colw[c];
    iw += gcol * (cs - 1);
    it_x[k] = track_x[it_col[k]];
    it_w[k] = iw;
    int fl = it->border.l + it->padding.l, fr = it->border.r + it->padding.r;
    int cw = iw - fl - fr;
    if (cw < 4)
      cw = 4;
    ov_start[k] = g_bk->n_fs;
    g_ov_active = true;
    g_ov_base = band_top;
    g_ov_n = 0;
    flow_slice(it->t0, it->t1, it_x[k] + fl, cw);
    g_ov_active = false;
    int vframe = it->border.t + it->border.b + it->padding.t + it->padding.b;
    int content_px = g_ov_n * LINE_H;
    int minch = box_min_content_h(it, vframe, iw);
    if (minch > content_px)
      content_px = minch;
    int orows = rows_for(content_px + vframe);
    if (orows < 1)
      orows = 1;
    rows_of[k] = orows;
    toppad[k] = rows_for(it->border.t + it->padding.t);
  }

  /* row-track heights: px floor, single-row items raise it, spanning items extend the last row */
  for (int r = 0; r < nrows; r++) {
    rowh[r] = 0;
    if (cont->n_rows > 0 && r < cont->n_rows && cont->rows_t[r].kind == CSS_TRK_PX) {
      int rr = rows_for(cont->rows_t[r].v);
      if (rr > rowh[r])
        rowh[r] = rr;
    }
  }
  for (int k = 0; k < nit; k++)
    if (it_rs[k] == 1 && it_row[k] < nrows && rows_of[k] > rowh[it_row[k]])
      rowh[it_row[k]] = rows_of[k];
  for (int r = 0; r < nrows; r++)
    if (rowh[r] < 1)
      rowh[r] = 1;

  int gap_rows = rows_for(grow_gap);
  row_off[0] = 0;
  for (int r = 0; r < nrows; r++)
    row_off[r + 1] = row_off[r] + rowh[r] + (r + 1 < nrows ? gap_rows : 0);
  for (int k = 0; k < nit; k++) { /* extend last spanned row for tall row-span items */
    if (it_rs[k] <= 1)
      continue;
    int r0 = it_row[k], r1 = r0 + it_rs[k];
    if (r1 > nrows)
      r1 = nrows;
    int have = row_off[r1] - row_off[r0] - (r1 < nrows ? gap_rows : 0);
    if (rows_of[k] > have && r1 - 1 >= 0 && r1 - 1 < nrows) {
      rowh[r1 - 1] += rows_of[k] - have;
      for (int r = r1 - 1; r < nrows; r++)
        row_off[r + 1] = row_off[r] + rowh[r] + (r + 1 < nrows ? gap_rows : 0);
    }
  }
  int band_h = row_off[nrows];
  int cvframe = cont->border.t + cont->border.b + cont->padding.t + cont->padding.b;
  int cminch =
      box_min_content_h(cont, cvframe, content_w + cont->border.l + cont->border.r + cont->padding.l + cont->padding.r);
  if (cminch > 0 && rows_for(cminch) > band_h)
    band_h = rows_for(cminch);
  if (band_h < 1)
    band_h = 1;

  /* place: shift each cell's text to its row (align-items) and record geometry */
  for (int k = 0; k < nit; k++) {
    struct lbox *it = &bx[items[k]];
    signed char ai = item_align(cont, it);
    int r0 = it_row[k], r1 = r0 + it_rs[k];
    if (r1 > nrows)
      r1 = nrows;
    int cell_rows = row_off[r1] - row_off[r0] - (r1 < nrows ? gap_rows : 0);
    if (cell_rows < 1)
      cell_rows = 1;
    int h_rows = (ai == CSS_ALIGN_STRETCH) ? cell_rows : rows_of[k];
    if (h_rows > cell_rows)
      h_rows = cell_rows;
    int off_rows = 0;
    if (ai == CSS_ALIGN_CENTER)
      off_rows = (cell_rows - h_rows) / 2;
    else if (ai == CSS_ALIGN_END)
      off_rows = cell_rows - h_rows;
    int base_row = row_off[r0] + off_rows;
    int shift = base_row + toppad[k];
    if (shift) {
      int end = (k + 1 < nit) ? ov_start[k + 1] : g_bk->n_fs;
      for (int s = ov_start[k]; s < end; s++)
        g_bk->fs[s].row += shift;
    }
    int fl = it->border.l + it->padding.l, fr = it->border.r + it->padding.r;
    int ft = it->border.t + it->padding.t;
    int vframe = ft + it->border.b + it->padding.b;
    struct lgeom *g = &geo[items[k]];
    g->x = it_x[k];
    g->w = it_w[k];
    g->y = band_top_y + base_row * LINE_H;
    g->h = h_rows * LINE_H;
    g->cx = it_x[k] + fl;
    g->cw = it_w[k] - fl - fr;
    if (g->cw < 4)
      g->cw = 4;
    g->cy = g->y + ft;
    g->ch = g->h - vframe;
    if (g->ch < 0)
      g->ch = 0;
  }

  add_n_blanks(band_h, cont->t0, content_x, content_w);
  free(rmem);
  free(mem);
  return true;
}

/* Lay out one box and its children recursively; geometry in document px, `x`/`avail_w` from the parent. */
static void flow_box(struct lbox *bx, int idx, int x, int avail_w)
{
  struct lbox *b = &bx[idx];
  int ml = max0(b->margin.l), mr = max0(b->margin.r);
  int frame = b->border.l + b->border.r + b->padding.l + b->padding.r;
  int box_w = box_width(b, avail_w, frame, ml, mr);

  /* Horizontal placement: auto margins absorb the slack. */
  int slack = avail_w - box_w - ml - mr;
  int box_x;
  if (b->ml_auto && b->mr_auto)
    box_x = x + ml + (slack > 0 ? slack / 2 : 0);
  else if (b->ml_auto)
    box_x = x + ml + (slack > 0 ? slack : 0);
  else
    box_x = x + ml;

  /* position: relative shifts from the flow spot; absolute/fixed use the containing block edges. */
  int dy = 0;
  if (b->pos == CSS_POS_REL || b->pos == CSS_POS_STICKY) {
    if (b->off_l >= 0)
      box_x += b->off_l;
    else if (b->off_r >= 0)
      box_x -= b->off_r;
    if (b->off_t >= 0)
      dy = b->off_t;
    else if (b->off_b >= 0)
      dy = -b->off_b;
  } else if (b->pos == CSS_POS_ABS || b->pos == CSS_POS_FIXED) {
    if (b->off_l >= 0)
      box_x = x + b->off_l;
    else if (b->off_r >= 0)
      box_x = x + avail_w - box_w - b->off_r;
    if (b->off_t >= 0)
      dy = b->off_t;
    else if (b->off_b >= 0)
      dy = -b->off_b;
  }

  if (b->ext && !b->ext->mat) /* translate; the vertical part moves the box at the end, like top: */
    box_x += b->ext->tr_x + b->ext->tr_xp * box_w / 100;

  int content_x = box_x + b->border.l + b->padding.l;
  int content_w = box_w - frame;
  if (content_w < 8)
    content_w = 8;

  add_blank_px(max0(b->margin.t), b->t0, box_x, box_w);
  int box_top_y = lt_doc_px(&g_bk->lt);
  add_blank_px(b->border.t + b->padding.t, b->t0, content_x, content_w);
  int content_top_y = lt_doc_px(&g_bk->lt);

  struct lgeom *g = &g_bk->geo[idx];
  g->x = box_x;
  g->w = box_w;
  g->cx = content_x;
  g->cw = content_w;

  /* grid/row flex: children placed via the band overlay; column flex stacks as blocks */
  bool banded = false;
  if (b->kind == LB_GRID)
    banded = flow_grid(bx, idx, content_x, content_w);
  else if (b->kind == LB_FLEX && b->flex_dir == 0)
    banded = flow_flex_row(bx, idx, content_x, content_w);
  if (!banded) {
    int pos = b->t0;
    for (int j = idx + 1; j < b->end;) {
      if (bx[j].parent != idx) {
        j++;
        continue;
      } /* not a direct child */
      flow_slice(pos, bx[j].t0, content_x, content_w); /* inline text before it */
      if (!out_of_flow(&bx[j]))
        flow_box(bx, j, content_x, content_w);
      pos = bx[j].t1;
      j = bx[j].end; /* skip its descendants */
    }
    flow_slice(pos, b->t1, content_x, content_w);
  }

  /* Pad short content up to the box's minimum content height. */
  int vframe = b->border.t + b->border.b + b->padding.t + b->padding.b;
  int min_ch = box_min_content_h(b, vframe, box_w);
  if (min_ch > 0) {
    int have = lt_doc_px(&g_bk->lt) - content_top_y;
    if (min_ch > have)
      add_blank_px(min_ch - have, b->t1, content_x, content_w);
  }

  int content_bot_y = lt_doc_px(&g_bk->lt);
  add_blank_px(b->padding.b + b->border.b, b->t1, content_x, content_w);
  int box_bot_y = lt_doc_px(&g_bk->lt);
  add_blank_px(max0(b->margin.b), b->t1, box_x, box_w);

  if (b->ext && !b->ext->mat)
    dy += b->ext->tr_y + b->ext->tr_yp * (box_bot_y - box_top_y) / 100;
  g->y = box_top_y + dy;
  g->h = box_bot_y - box_top_y;
  g->cy = content_top_y + dy;
  g->ch = content_bot_y - content_top_y;
}

/* Lay out every position:fixed box into the overlay table, stacked; the page flow never sees them. */
static void layout_overlays(int paw)
{
  struct layout_out *o = g_bk;
  int n;
  struct lbox *bx = layout_pool(&n);
  bool any = false;
  o->n_ov = 0;
  o->ovlt.n = 0;
  o->n_ovfs = 0;
  for (int i = 0; i < n && !any; i++)
    any = out_of_flow(&bx[i]);
  if (!any || o->n_geo != n)
    return;

  struct ltab page_lt = o->lt;
  struct fslice *page_fs = o->fs;
  int page_n_fs = o->n_fs, page_cap_fs = o->cap_fs, page_n_med = o->n_med;
  o->lt = o->ovlt;
  o->fs = o->ovfs;
  o->n_fs = 0;
  o->cap_fs = o->cap_ovfs;

  for (int i = 0; i < n && o->n_ov < OVL_MAX; i++) {
    struct lbox *b = &bx[i];
    if (!out_of_flow(b))
      continue;
    struct lovl rec = {i, o->lt.n, 0, lt_doc_px(&o->lt), 0, false};
    int saved_w = b->spec_w, saved_t = b->off_t, saved_b = b->off_b;
    int frame = b->border.l + b->border.r + b->padding.l + b->padding.r;
    if (b->kind == LB_BLOCK && b->spec_w < 0 && b->pct_w < 0 && !(b->off_l >= 0 && b->off_r >= 0))
      b->spec_w = slice_maxcontent(b->t0, b->t1) + (b->border_box ? frame : 0); /* shrink to fit */
    b->off_t = b->off_b = -1;                                                   /* placed vertically at paint time */
    if (b->node && dom_get_attr(b->node, "fs-top"))
      flow_box(bx, i, 0, paw + 8); /* the fullscreen element covers the pane edge to edge */
    else
      flow_box(bx, i, 4, paw);
    b->spec_w = saved_w;
    b->off_t = saved_t;
    b->off_b = saved_b;
    rec.row1 = o->lt.n;
    rec.h = o->geo[i].h;
    rec.modal = b->node && (dom_get_attr(b->node, "dialog-modal") || dom_get_attr(b->node, "fs-top"));
    for (int q = 0; q < o->n_img; q++) /* overlay images are not painted */
      if (o->img_pos[q] >= b->t0 && o->img_pos[q] < b->t1)
        o->img_row[q] = -1;
    struct lovl *np = grow_arr(o->ov, &o->cap_ov, o->n_ov + 1, sizeof *np);
    if (!np)
      break;
    o->ov = np;
    o->ov[o->n_ov++] = rec;
  }

  o->n_med = page_n_med;
  o->ovlt = o->lt;
  o->ovfs = o->fs;
  o->n_ovfs = o->n_fs;
  o->cap_ovfs = o->cap_fs;
  o->lt = page_lt;
  o->fs = page_fs;
  o->n_fs = page_n_fs;
  o->cap_fs = page_cap_fs;
}

/* Lay out the whole box tree, flowing stray text full width. */
static void layout_flow(int paw)
{
  int n;
  struct lbox *bx = layout_pool(&n);
  int pos = 0;
  for (int j = 0; j < n;) {
    if (bx[j].parent != -1) {
      j++;
      continue;
    }
    flow_slice(pos, bx[j].t0, 4, paw);
    if (!out_of_flow(&bx[j]))
      flow_box(bx, j, 4, paw);
    pos = bx[j].t1;
    j = bx[j].end;
  }
  flow_slice(pos, B->pg->text_len, 4, paw);
  layout_overlays(paw);
}

/* Hash of every layout input, so paint-only animation frames can reuse the layout. */
unsigned layout_sig(int paw)
{
  const struct page *pg = B->pg;
  unsigned h = layout_mix((unsigned)paw, pg->font_gen);
  h = layout_mix(h, (unsigned)pg->mode);
  if (pg->text)
    h = layout_mix(h, ru_text_sig(pg->text, pg->fsz, pg->lh, pg->bp, pg->attr, pg->text_len, ST_NOWRAP));
  if (pg->ff) /* the font each glyph is measured in */
    for (int i = 0; i < pg->text_len; i++)
      h = h * 31u + pg->ff[i];
  if (pg->has_ls)
    for (int i = 0; i < pg->text_len; i++)
      h = layout_mix(h, (unsigned)(unsigned char)pg->ls[i]);
  int n;
  const struct lbox *bx = layout_pool(&n);
  h = layout_mix(h, (unsigned)n);
  for (int i = 0; i < n; i++)
    h = layout_box_sig(&bx[i], h);
  h = layout_mix(h, (unsigned)pg->n_imgs);
  for (int q = 0; q < pg->n_imgs; q++) {
    const struct inl_img *im = &pg->imgs[q];
    h = layout_mix(h, (unsigned)im->text_pos | ((unsigned)im->ok << 31));
    h = layout_mix(h, im->iw);
    h = layout_mix(h, im->ih);
    h = layout_mix(h, (unsigned)im->cw);
    h = layout_mix(h, (unsigned)im->ch);
  }
  for (struct media_el *me = media_doc_first(); me; me = media_doc_next(me)) {
    h = layout_mix(h, (unsigned)(unsigned long)me);
    h = layout_mix(h, (unsigned)media_widget_rows(me, paw, LINE_H));
  }
  return h;
}

/* Publish a finished layout to the painter and DOM geometry APIs; caller holds browser_buf_lock. */
static void layout_apply(const struct layout_out *o)
{
  struct page *pg = B->pg;
  int n = pg->n_imgs < o->n_img ? pg->n_imgs : o->n_img;
  for (int q = 0; q < n; q++) {
    pg->imgs[q].start_row = o->img_row[q];
    if (o->img_rows[q] >= 0)
      pg->imgs[q].rows = o->img_rows[q];
  }
  for (int q = n; q < pg->n_imgs; q++)
    pg->imgs[q].start_row = -1;
  media_reset_layout();
  for (int m = 0; m < o->n_med; m++)
    media_set_layout(o->med[m].me, o->med[m].row, o->med[m].rows);
  int nb;
  struct lbox *bx = layout_pool(&nb);
  if (nb == o->n_geo)
    for (int i = 0; i < nb; i++) {
      bx[i].x = o->geo[i].x;
      bx[i].y = o->geo[i].y;
      bx[i].w = o->geo[i].w;
      bx[i].h = o->geo[i].h;
      bx[i].cx = o->geo[i].cx;
      bx[i].cy = o->geo[i].cy;
      bx[i].cw = o->geo[i].cw;
      bx[i].ch = o->geo[i].ch;
    }
}

/* DOM re-flattened without layout changes: re-attach the layout to the fresh box pool. */
void layout_keep(void)
{
  browser_buf_lock();
  layout_apply(g_fr);
  B->layout_dirty = 0;
  browser_buf_unlock();
}

/* Vertical extent of box `i` including the outline and shadow that spill past it. */
void box_extent(const struct lbox *b, const struct lgeom *g, int *top, int *bot)
{
  *top = g->y;
  *bot = g->y + g->h;
  if (b->outline_w > 0) {
    *top -= b->outline_w;
    *bot += b->outline_w;
  }
  if (b->sh_color != BG_NONE) {
    int bl = b->sh_blur < 0 ? 0 : b->sh_blur;
    int ext = b->sh_spread + bl;
    int st = g->y + b->sh_dy - ext, sb = g->y + g->h + b->sh_dy + ext;
    if (st < *top)
      *top = st;
    if (sb > *bot)
      *bot = sb;
  }
}

/* Per-layout paint order (z bands, vertical cull index) and max glyph spill below a line box. */
static void layout_index(struct layout_out *o, const struct lbox *bx, int nb)
{
  ru_bands_free(&o->bands);
  int *tmp = nb > 0 ? malloc((unsigned long)nb * 3 * sizeof *tmp) : 0;
  if (tmp) {
    int *z = tmp, *top = tmp + nb, *bot = tmp + 2 * nb;
    for (int i = 0; i < nb; i++) {
      z[i] = bx[i].z_index;
      box_extent(&bx[i], &o->geo[i], &top[i], &bot[i]);
    }
    ru_bands_build(&o->bands, z, top, bot, nb, CSS_Z_AUTO);
    free(tmp);
  }
  int over = 0;
  for (int i = 0; i < o->lt.n; i++) {
    int v = o->lt.px[i] + (o->lt.px[i] + 2) / 4 + 2 - o->lt.h[i];
    if (v > over)
      over = v;
  }
  for (int i = 0; i < o->n_fs; i++) {
    int v = o->fs[i].maxpx + (o->fs[i].maxpx + 2) / 4 + 2 - o->fs[i].h;
    if (v > over)
      over = v;
  }
  o->over = over;
}

/* Word-wrap the page text to `paw` px, building in g_bk and swapping in under a short lock. */
void rewrap(int paw)
{
  if (paw < 8)
    paw = 8;
  /* Without a text buffer, drop to an empty page and force a re-wrap. */
  if (!B->pg->text) {
    browser_buf_lock();
    g_fr->lt.n = 0;
    g_fr->doc_h = 0;
    browser_buf_unlock();
    B->layout_dirty = 1;
    return;
  }
  if (!B->layout_dirty && B->wrap_cols == paw)
    return;
  B->layout_dirty = 0;

  struct layout_out *o = g_bk;
  int nb;
  struct lbox *bx = layout_pool(&nb);
  o->lt.n = 0;
  o->n_fs = 0;
  o->n_ov = 0;
  o->n_med = 0;
  o->n_geo = 0;
  o->n_img = 0;
  struct lgeom *geo = nb > 0 ? grow_arr(o->geo, &o->cap_geo, nb, sizeof *geo) : 0;
  if (geo) {
    o->geo = geo;
    memset(geo, 0, (unsigned long)nb * sizeof *geo);
    o->n_geo = nb;
  }
  int ni = B->pg->n_imgs;
  if (ni > o->cap_img) {
    int nc = browser_grow_cap(o->cap_img, 64, ni);
    int *m = realloc(o->img_pos, (unsigned long)nc * 3 * sizeof *m);
    if (m) {
      o->img_pos = m;
      o->cap_img = nc;
    }
  }
  if (ni <= o->cap_img) {
    o->img_row = o->img_pos + o->cap_img;
    o->img_rows = o->img_pos + 2 * o->cap_img;
    for (int q = 0; q < ni; q++) {
      o->img_pos[q] = B->pg->imgs[q].text_pos;
      o->img_row[q] = -1;
      o->img_rows[q] = -1;
    }
    o->n_img = ni;
  }
  g_imgix_ok = o->n_img == ni && ru_imgidx_build(&g_imgix, o->img_pos, ni);
  g_has_media = media_doc_first() != 0;

  if (g_box_layout && nb > 0 && o->n_geo == nb)
    layout_flow(paw); /* box-driven layout */
  else
    flow_slice(0, B->pg->text_len, 4, paw); /* flat flow  */
  free(g_imgix.ord);
  g_imgix.ord = 0;
  o->doc_h = lt_doc_px(&o->lt);
  layout_index(o, bx, o->n_geo);
  g_lay_sig = layout_sig(paw);

  browser_buf_lock();
  layout_apply(o);
  g_bk = g_fr;
  g_fr = o;
  B->wrap_cols = paw;
  B->layout_seq++; /* geometry changed: devtools box model + highlight */
  int max = scroll_max();
  if (B->pg->scroll > max)
    B->pg->scroll = max;
  if (B->pg->scroll < 0)
    B->pg->scroll = 0;
  browser_buf_unlock();
}
