#include "main_int.h"

/* The hovered-link status box rows (screen y), which sit at the pane's bottom-left. */
void hover_rows(int *y0, int *y1)
{
  int sh = UIFONT_H + 6;
  int by = BH - sh;
  if (by < MENU_H + TAB_H + ADDR_H)
    by = MENU_H + TAB_H + ADDR_H;
  *y0 = by;
  *y1 = by + sh < BH ? by + sh : BH;
}

/* Hovered-link status */
static void draw_hover_status(void)
{
  int by, y1;
  hover_rows(&by, &y1);
  int sh = UIFONT_H + 6;
  int maxw = (devtools_visible() ? devtools_left() : BW) - 2;
  char shown[URL_MAX];
  fit_ui(shown, sizeof shown, hover_url, maxw - 12);
  int sw = gfx_ui_width(shown) + 12;
  if (sw > maxw)
    sw = maxw;
  gfx_fill_rect(0, by, sw, sh, 0x202830);
  gfx_rect(0, by, sw, sh, 0x10141a);
  gfx_ui_text(6, by + 3, shown, 0xe8eaee);
}

/* Shade the pane rect behind a modal dialog with `argb` (0xAARRGGBB) */
static void dim_rect(int x, int y, int w, int h, int ct, int cb, unsigned argb)
{
  unsigned *fb = browser_surface();
  if (y < ct) {
    h -= ct - y;
    y = ct;
  }
  if (y + h > cb)
    h = cb - y;
  unsigned a = argb >> 24, ia = 255 - a;
  unsigned sr = ((argb >> 16) & 0xff) * a, sg = ((argb >> 8) & 0xff) * a, sb = (argb & 0xff) * a;
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++) {
      unsigned p = fb[(unsigned long)yy * BW + xx];
      unsigned r = (sr + ((p >> 16) & 0xff) * ia) / 255, g = (sg + ((p >> 8) & 0xff) * ia) / 255,
               b = (sb + (p & 0xff) * ia) / 255;
      fb[(unsigned long)yy * BW + xx] = (r << 16) | (g << 8) | b;
    }
}

/* The page scrollbar (an empty one when total <= visible), unless the page hides it. */
static void draw_page_scrollbar(int pw, int py, int phh, int scroll, int total, int visible)
{
  if (PAGE_SCROLL_W > 0)
    scrollbar_draw_ex(pw, py, PAGE_SCROLL_W, phh, scroll, total, visible, &B->pg->sb_st);
}

/* Paint the position:fixed / top-layer boxes bottom to top: those with a negative z-index under the page's
 * content (`below`), the rest over it */
static void paint_overlays(int py, int pw, int ct, int cb, bool below)
{
  const struct layout_out *o = g_fr;
  int n, order[OVL_MAX], pane_y, phh, rows;
  if (!o->n_ov)
    return;
  const struct lbox *bx = layout_pool(&n);
  if (n != o->n_geo)
    return;
  pane_metrics(&pane_y, &phh, &rows);
  overlay_order(o, order);
  for (int q = 0; q < o->n_ov; q++) {
    const struct lovl *ov = &o->ov[order[q]];
    int z = bx[ov->box].z_index;
    if ((z != CSS_Z_AUTO && z < 0) != below)
      continue;
    int h, top = overlay_place(ov, phh, &h);
    if (ov->modal) {
      const struct lext *x = bx[ov->box].ext;
      dim_rect(0, py, pw, phh, ct, cb, x && x->backdrop_set ? x->backdrop : 0x99000000u);
    }
    paint_overlay_boxes(ov, py, pw, ct, cb, top, h);
    int shift = py + top - o->geo[ov->box].y;
    const struct ltab *t = &o->ovlt;
    for (int i = ov->row0; i < ov->row1; i++) {
      int sy = shift + t->y[i];
      if (sy + t->h[i] + o->over <= ct || sy >= cb || t->off[i] + t->len[i] > B->pg->text_len)
        continue;
      draw_slice_core(t->x[i], sy, t->off[i], t->len[i], t->w[i], ct, cb, t->h[i], t->px[i]);
    }
    for (int s = 0; s < o->n_ovfs; s++) {
      const struct fslice *f = &o->ovfs[s];
      if (f->row < ov->row0 || f->row >= ov->row1 || f->off + f->len > B->pg->text_len)
        continue;
      int sy = shift + t->y[f->row];
      if (sy + f->h + o->over <= ct || sy >= cb)
        continue;
      draw_slice_core(f->x, sy, f->off, f->len, f->w, ct, cb, f->h, f->maxpx);
    }
  }
}

/* The page's box backgrounds, text, images and media for the rows [ct,cb) of the surface, through g_gate; `fill` first
 * paints the canvas colour. */
static void paint_page_core(int py, int pw, int ct, int cb, unsigned bg, bool fill)
{
  const struct ltab *t = &g_fr->lt;
  int paw = pw - 8; /* content width, px */
  int sc = B->pg->scroll, vt = sc + (ct - py), vb = sc + (cb - py), over = g_fr->over;
  gfx_set_clip(0, ct, pw, cb - ct);
  if (fill) {
    gfx_fill_rect(0, ct, pw, cb - ct, bg);
    paint_overlays(py, pw, ct, cb, true);
  }

  paint_boxes(py, pw, ct, cb); /* block backgrounds behind the text */
  if (B->loading && t->n == 0) {
    gfx_ui_text(8, py + 8, "Loading ...", 0x4060a0);
  } else if (t->n == 0 && !B->pg->cur_url[0]) {
    /* No page loaded */
    gfx_ui_text(8, py + 8, "Type a URL above and press Enter.", 0x707880);
    gfx_ui_text(8, py + 8 + UIFONT_H, "e.g. example.com  or  https://example.com", 0x9098a0);
  }
  for (int i = lt_line_at(t, vt - over); i < t->n; i++) {
    int sy = py + t->y[i] - sc;
    if (sy >= cb)
      break; /* below the band: done */
    if (sy + t->h[i] + over <= ct || t->off[i] + t->len[i] > B->pg->text_len || !gate_text(t->off[i]))
      continue;
    draw_styled_line(t->x[i], sy, i, t->w[i], ct, cb);
  }

  /* flex-row item text, drawn over the reserved band at each item's column */
  for (int q = 0; q < g_fr->n_fs; q++) {
    const struct fslice *f = &g_fr->fs[q];
    if (f->row < 0 || f->row >= t->n || f->off + f->len > B->pg->text_len || !gate_text(f->off))
      continue;
    int sy = py + t->y[f->row] - sc;
    if (sy + f->h + over <= ct || sy >= cb)
      continue;
    draw_slice_core(f->x, sy, f->off, f->len, f->w, ct, cb, f->h, f->maxpx);
  }

  /* inline images, over their reserved (blank) rows */
  for (int q = 0; q < B->pg->n_imgs; q++) {
    struct inl_img *im = &B->pg->imgs[q];
    if (im->start_row < 0 || im->start_row >= t->n || !gate_text(im->text_pos))
      continue;
    int r1 = im->start_row + (im->rows > 0 ? im->rows : 1);
    if (r1 > t->n)
      r1 = t->n;
    int itop = t->y[im->start_row], ibot = lt_bottom(t, r1 - 1);
    if (ibot <= vt || itop >= vb)
      continue; /* off-screen */
    /* On-screen now */
    if (im->lazy && !im->fetched && show_images)
      im->want = 1;
    if (im->ok && show_images)
      draw_inline_image(im, py, ct, cb, paw);
    else
      draw_alt_box(im, py, ct, cb);
  }

  /* <audio>/<video> control widgets, over their reserved rows */
  for (struct media_el *me = media_doc_first(); me; me = media_doc_next(me)) {
    int sr = media_el_start_row(me);
    if (sr < 0 || sr >= t->n || g_gate.only >= 0)
      continue;
    int r1 = sr + media_el_rows(me);
    if (r1 > t->n)
      r1 = t->n;
    int itop = t->y[sr], ibot = lt_bottom(t, r1 - 1);
    if (ibot <= vt || itop >= vb)
      continue; /* off-screen */
    media_el_draw(me, 4, py + t->y[sr] - sc, paw, ct, cb);
  }
}

static unsigned *g_rot_buf; /* scratch surface a transformed subtree is painted into */
static long g_rot_px;

/* Paint each transformed box (rotate, scale, skew) apart from the page and draw it through its transform onto the
 * rows [ct,cb) of the page just painted without it. */
static void paint_rotated(int py, int pw, int ct, int cb, unsigned bg)
{
  int n;
  const struct lbox *bx = layout_pool(&n);
  if (!g_gate.n || n != g_fr->n_geo)
    return;
  int pane_y, phh, rows, sc = B->pg->scroll;
  pane_metrics(&pane_y, &phh, &rows);
  if ((long)BW * BH > g_rot_px) {
    unsigned *nb = realloc(g_rot_buf, (unsigned long)BW * BH * 4);
    if (!nb)
      return;
    g_rot_buf = nb;
    g_rot_px = (long)BW * BH;
  }
  unsigned *real = browser_surface();
  int count = g_gate.n;
  for (int k = 0; k < count; k++) {
    const struct lbox *b = &bx[g_gate.r[k].box0];
    const struct lgeom *g = &g_fr->geo[g_gate.r[k].box0];
    int sx0 = g->x, sy0 = py + g->y - sc;
    int ex0 = sx0 < 0 ? 0 : sx0, ex1 = sx0 + g->w > pw ? pw : sx0 + g->w;
    int er0 = sy0 < py ? py : sy0, er1 = sy0 + g->h > py + phh ? py + phh : sy0 + g->h;
    if (ex1 <= ex0 || er1 <= er0)
      continue;
    const struct lext *x = b->ext;
    struct ru_xf xf = {x->m[0] / 4096.0,
                       x->m[1] / 4096.0,
                       x->m[2] / 4096.0,
                       x->m[3] / 4096.0,
                       sx0 + x->ox + x->oxp * g->w / 100.0,
                       sy0 + x->oy + x->oyp * g->h / 100.0,
                       x->tr_x + x->tr_xp * g->w / 100.0,
                       x->tr_y + x->tr_yp * g->h / 100.0};
    int dx0, dy0, dx1, dy1;
    ru_xf_bounds(&xf, ex0, er0, ex1 - ex0, er1 - er0, &dx0, &dy0, &dx1, &dy1);
    if (dx1 <= 0 || dx0 >= pw || dy1 <= ct || dy0 >= cb)
      continue;
    unsigned *back = malloc((unsigned long)(ex1 - ex0) * (unsigned long)(er1 - er0) * 4);
    if (!back)
      continue;
    /* the page without the box, then the box over it: what differs is the box */
    browser_surface_redirect(g_rot_buf);
    gfx_init(g_rot_buf, BW, BH);
    g_gate.only = -1;
    paint_page_core(py, pw, er0, er1, bg, true);
    for (int r = er0; r < er1; r++)
      memcpy(back + (unsigned long)(r - er0) * (ex1 - ex0), g_rot_buf + (unsigned long)r * BW + ex0,
             (unsigned long)(ex1 - ex0) * 4);
    g_gate.only = k;
    paint_page_core(py, pw, er0, er1, bg, false);
    g_gate.only = -1;
    browser_surface_redirect(0);
    gfx_init(real, BW, BH);
    ru_affine_draw(real, BW, 0, ct, pw, cb, g_rot_buf, BW, back, ex0, er0, ex1 - ex0, er1 - er0, &xf);
    free(back);
  }
}

/* Paint the page (box backgrounds, text, images, media) for the content-pane rows [ct,cb) */
static void paint_page_band(int py, int pw, int ct, int cb, unsigned bg)
{
  /* The worker owns the line-table flow (see worker_fn) */
  if (!B->pg->text) {
    g_fr->lt.n = 0;
    B->layout_dirty = 1;
  }
  int rot[GATE_MAX], n, nb = 0;
  const struct lbox *bx = layout_pool(&n);
  if (n == g_fr->n_geo)
    nb = layout_rot_boxes(rot, GATE_MAX);
  g_gate.n = nb;
  g_gate.only = -1;
  for (int k = 0; k < nb; k++) {
    g_gate.r[k].box0 = rot[k];
    g_gate.r[k].box1 = bx[rot[k]].end;
    g_gate.r[k].t0 = bx[rot[k]].t0;
    g_gate.r[k].t1 = bx[rot[k]].t1;
  }
  paint_page_core(py, pw, ct, cb, bg, true);
  paint_rotated(py, pw, ct, cb, bg);
  g_gate.n = 0;
  gfx_set_clip(0, ct, pw, cb - ct);
  paint_overlays(py, pw, ct, cb, false);
  gfx_clip_none();
}

/* What the surface holds from the last paint, so a scroll or a small change can reuse it instead */
static struct {
  bool valid;
  const struct page *pg;
  int bw, bh, scroll;
  unsigned sig;
} g_painted;

/* Hash of everything that can change the pane without a tracked UI action. */
static unsigned render_sig(void)
{
  const struct page *pg = B->pg;
  unsigned h = layout_mix((unsigned)(unsigned long)pg, (unsigned)BW | ((unsigned)BH << 16));
  h = layout_mix(h, B->layout_seq);
  h = layout_mix(h, (unsigned)g_fr->lt.n);
  h = layout_mix(h, (unsigned)pg->text_len);
  h = layout_mix(h, (unsigned)pg->mode | ((unsigned)B->loading << 8) | ((unsigned)B->req_pending << 9) |
                        ((unsigned)tabs[active_tab]->crashed << 10) | ((unsigned)show_images << 11) |
                        ((unsigned)devtools_visible() << 12) | ((unsigned)bm_panel_open << 13) |
                        ((unsigned)menu_any_open() << 14) | ((unsigned)field_ctx.open << 24) |
                        ((unsigned)media_active() << 15) | ((unsigned)active_tab << 16));
  h = layout_mix(h, (unsigned)n_tabs);
  h = layout_mix(h, pg->page_bg);
  h = layout_mix(h, pg->font_gen);
  h = layout_mix(h, (unsigned)pg->n_bgimgs);
  for (int q = 0; q < pg->n_imgs; q++) {
    const struct inl_img *im = &pg->imgs[q];
    h = layout_mix(h, (unsigned)im->ok | ((unsigned)im->start_row << 1));
    h = layout_mix(h, (unsigned)im->uid);
  }
  return h;
}

void render(void)
{
  unsigned bg = B->pg->page_bg;
  /* Hold off any buffer-relocating realloc on the worker thread, and any swap of the line table */
  browser_buf_lock();

  /* tab strip (between the menu bar and the address bar) */
  static const char **labels;
  static const unsigned **icons;
  static int strip_cap;
  if (n_tabs > strip_cap) {
    const char **nl = realloc(labels, (unsigned)n_tabs * sizeof *nl);
    if (nl)
      labels = nl;
    const unsigned **ni = realloc(icons, (unsigned)n_tabs * sizeof *ni);
    if (ni)
      icons = ni;
    if (nl && ni)
      strip_cap = n_tabs;
  }
  int shown = n_tabs < strip_cap ? n_tabs : strip_cap;
  for (int i = 0; i < shown; i++) {
    labels[i] = tabs[i]->label[0] ? tabs[i]->label : "New Tab";
    icons[i] = tabs[i]->page.favicon_ok ? tabs[i]->page.favicon : 0;
  }
  if (!g_fullscreen) {
    tabbar_draw(&tab_strip, 0, MENU_H, BW, labels, icons, shown, active_tab < shown ? active_tab : 0);

    /* address bar (below the tab strip) */
    gfx_fill_rect(0, MENU_H + TAB_H, BW, ADDR_H, 0x222831);
    struct rect rb, rf, rrl, rtf, rst;
    addr_layout(&rb, &rf, &rrl, &rtf, &rst);
    struct tab *at = tabs[active_tab];
    button_draw(rb.x, rb.y, rb.w, rb.h, "\xe2\x86\x90", /* ← */
                hist_can_back(at) ? BTN_NORMAL : BTN_DISABLED);
    button_draw(rf.x, rf.y, rf.w, rf.h, "\xe2\x86\x92", /* → */
                hist_can_fwd(at) ? BTN_NORMAL : BTN_DISABLED);
    reload_btn_draw(&rrl);
    textfield_draw(&B->addr_tf, rtf.x, rtf.y, rtf.w, rtf.h, 1);
    star_btn_draw(&rst);
  }

  /* content pane , shrinks to the left of the devtools dock when it is open */
  int right = devtools_visible() ? devtools_left() : BW;
  int py = MENU_H + TAB_H + ADDR_H, pw = right - PAGE_SCROLL_W;
  int phh = BH - MENU_H - TAB_H - ADDR_H;
  if (phh < 0)
    phh = 0;

  if (tabs[active_tab]->crashed) {
    gfx_fill_rect(0, py, pw, phh, bg);
    draw_crash_page(py, pw, phh);
    draw_page_scrollbar(pw, py, phh, 0, 0, 0); /* inert scrollbar */
  } else if (B->pg->mode == MODE_IMAGE) {
    gfx_fill_rect(0, py, pw, phh, bg);
    if (B->loading)
      gfx_ui_text(8, py + 8, "Loading ...", 0x4060a0);
    else
      draw_image(0, py, pw, phh, bg);
    draw_page_scrollbar(pw, py, phh, 0, 0, 0); /* inert scrollbar */
  } else {
    paint_page_band(py, pw, py, py + phh, bg);
    /* scrollbar (units are document pixels) */
    draw_page_scrollbar(pw, py, phh, B->pg->scroll, g_fr->doc_h, phh);
  }

  struct media_el *fsm = media_fullscreen();
  if (fsm) {
    gfx_fill_rect(0, py, BW, phh, 0x000000);
    media_fs_draw(fsm, 0, py, BW, phh, py, py + phh);
  } else if (hover_url[0])
    draw_hover_status();

  /* Devtools element highlight */
  if (devtools_visible() && B->pg->mode != MODE_IMAGE) {
    int hx, hy, hw, hh;
    const char *hlabel = 0;
    if (devtools_highlight(&hx, &hy, &hw, &hh, &hlabel))
      draw_dt_highlight(hx, py + hy - B->pg->scroll, hw, hh, py, py + phh, pw, hlabel);
  }

  /* Developer tools (F12 / Ctrl+Shift+I) */
  devtools_render();

  /* Bookmarks panel over the content pane (below the menu dropdowns). */
  if (bm_panel_open)
    bookmarks_panel_render();

  /* App menu bar over the top, then the dropdowns last so they overlay the page */
  page_flags[2] = is_loading() ? 0 : MENU_DISABLED;
  edit_flags[0] = gfx_sel_has(&g_sel) ? 0 : MENU_DISABLED; /* Copy: only with a selection */
  bookmarks_menu_sync();
  if (!g_fullscreen) {
    gfx_fill_rect(0, 0, BW, MENU_H, 0x232a36);
    menubar_draw(&menubar);
  }
  field_menu_draw();

  g_painted.valid = true;
  g_painted.pg = B->pg;
  g_painted.bw = BW;
  g_painted.bh = BH;
  g_painted.scroll = B->pg->scroll;
  g_painted.sig = render_sig();
  browser_buf_unlock();
}

/* Pane rows dirtied by tracked UI actions (hover box, selection drag, media ticks) since the last */
struct dmg_state g_dmg;

void damage_rows(int y0, int y1)
{
  if (!g_dmg.band) {
    g_dmg.band = true;
    g_dmg.y0 = y0;
    g_dmg.y1 = y1;
  } else {
    if (y0 < g_dmg.y0)
      g_dmg.y0 = y0;
    if (y1 > g_dmg.y1)
      g_dmg.y1 = y1;
  }
  g_dmg.bumps++;
  B->seq++;
}

/* Mark the display lines [lo,hi] dirty, if any of them is on screen. */
void damage_lines(int lo, int hi)
{
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  browser_buf_lock();
  const struct ltab *t = &g_fr->lt;
  if (lo < 0)
    lo = 0;
  if (hi >= t->n)
    hi = t->n - 1;
  int y0 = 0, y1 = 0;
  if (lo <= hi) {
    y0 = py + t->y[lo] - B->pg->scroll;
    y1 = py + lt_bottom(t, hi) - B->pg->scroll;
  }
  browser_buf_unlock();
  y0 = y0 < py ? py : y0;
  y1 = y1 > py + phh ? py + phh : y1;
  if (y1 > y0)
    damage_rows(y0, y1);
}

/* A playing <audio>/<video> widget ticks: dirty just the visible widget rows. */
void damage_media(void)
{
  if (media_fullscreen()) {
    B->seq++;
    return;
  }
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  int y0 = py + phh, y1 = py;
  browser_buf_lock();
  const struct ltab *t = &g_fr->lt;
  for (struct media_el *me = media_doc_first(); me; me = media_doc_next(me)) {
    int sr = media_el_start_row(me);
    if (sr < 0 || sr >= t->n)
      continue;
    int r1 = sr + media_el_rows(me);
    if (r1 > t->n)
      r1 = t->n;
    int top = py + t->y[sr] - B->pg->scroll, bot = py + lt_bottom(t, r1 - 1) - B->pg->scroll;
    if (top < y0)
      y0 = top;
    if (bot > y1)
      y1 = bot;
  }
  browser_buf_unlock();
  y0 = y0 < py ? py : y0;
  y1 = y1 > py + phh ? py + phh : y1;
  if (y1 > y0)
    damage_rows(y0, y1);
}

/* Paint only what the tracked damage covers, straight onto the surface from the last paint */
bool render_incremental(unsigned seq_delta, int id)
{
  if (seq_delta != g_dmg.bumps || !g_painted.valid || g_painted.pg != B->pg || g_painted.bw != BW ||
      g_painted.bh != BH || g_painted.sig != render_sig())
    return false;
  if (devtools_visible() || bm_panel_open || menu_any_open() || tabs[active_tab]->crashed || media_fullscreen() ||
      B->pg->mode == MODE_IMAGE || is_loading() || !browser_surface_ready())
    return false;
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  int pw = BW - PAGE_SCROLL_W;
  bool scrolled = B->pg->scroll != g_painted.scroll;
  for (int k = 0; scrolled && k < g_fr->n_ov; k++)
    if (!g_fr->ov[k].doc)
      return false; /* a viewport-fixed box stays put while the page moves under it: shifting pixels would drag it */
  struct ru_scroll_plan plan = {0};
  if (scrolled &&
      (g_dmg.band || hover_url[0] || media_active() || !ru_scroll_plan(g_painted.scroll, B->pg->scroll, phh, &plan)))
    return false;

  int ct = 0, cb = 0;
  browser_buf_lock();
  unsigned *fb = browser_surface();
  if (scrolled) {
    memmove(fb + (unsigned long)(py + plan.dst) * BW, fb + (unsigned long)(py + plan.src) * BW,
            (unsigned long)plan.rows * BW * 4);
    ct = py + plan.strip;
    cb = ct + plan.strip_n;
  } else if (g_dmg.band) {
    ct = g_dmg.y0 < py ? py : g_dmg.y0;
    cb = g_dmg.y1 > py + phh ? py + phh : g_dmg.y1;
  }
  if (cb > ct)
    paint_page_band(py, pw, ct, cb, B->pg->page_bg);
  if (scrolled || cb > ct) {
    draw_page_scrollbar(pw, py, phh, B->pg->scroll, g_fr->doc_h, phh);
    if (hover_url[0])
      draw_hover_status();
  }
  g_painted.scroll = B->pg->scroll;
  browser_buf_unlock();

  if (scrolled)
    browser_blit_rect(0, py, BW, phh);
  else if (cb > ct) {
    browser_blit_rect(0, ct, BW, cb - ct);
    browser_blit_rect(pw, py, PAGE_SCROLL_W, phh);
  }
  if (scrolled || cb > ct)
    win_present(id);
  g_dmg.bumps = 0;
  g_dmg.band = false;
  return true;
}

/* True while any menu dropdown is expanded */
bool menu_any_open(void)
{
  for (int i = 0; i < menubar.n; i++)
    if (menubar.menus[i] && menubar.menus[i]->open)
      return true;
  return false;
}

/* Repaint only the address-bar strip */
void render_addr(void)
{
  if (g_fullscreen)
    return;
  browser_buf_lock();
  gfx_fill_rect(0, MENU_H + TAB_H, BW, ADDR_H, 0x222831);
  struct rect rb, rf, rrl, rtf, rst;
  addr_layout(&rb, &rf, &rrl, &rtf, &rst);
  struct tab *at = tabs[active_tab];
  button_draw(rb.x, rb.y, rb.w, rb.h, "\xe2\x86\x90", hist_can_back(at) ? BTN_NORMAL : BTN_DISABLED);
  button_draw(rf.x, rf.y, rf.w, rf.h, "\xe2\x86\x92", hist_can_fwd(at) ? BTN_NORMAL : BTN_DISABLED);
  reload_btn_draw(&rrl);
  textfield_draw(&B->addr_tf, rtf.x, rtf.y, rtf.w, rtf.h, 1);
  star_btn_draw(&rst);
  browser_buf_unlock();
}
