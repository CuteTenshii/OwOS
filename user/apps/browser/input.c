#include "main_int.h"

/* Run item `item` of leaf menu `m` */
static void menu_action(struct menu *m, int item)
{
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  if (m == &pagemenu) {
    switch (item) {
    case 0:
      do_reload();
      break; /* Reload current page     */
    case 1:
      navigate("example.com");
      break; /* Home                    */
    case 2:
      do_stop();
      break; /* Stop the in-flight load */
    }
  } else if (m == &editmenu) {
    if (item == 0)
      copy_selection(); /* Copy                    */
    else if (item == 1)
      select_all(); /* Select All              */
  } else if (m == &viewmenu) {
    if (item == 0) { /* Show Images checkbox    */
      view_flags[0] ^= MENU_CHECKED;
      show_images = (view_flags[0] & MENU_CHECKED) != 0;
      B->seq++;
    } /* item 1 = Scroll submenu */
  } else if (m == &scrollmenu) {
    switch (item) {
    case 0:
      scroll_set(0);
      break; /* Top      */
    case 1:
      scroll_set(scroll_max());
      break; /* Bottom   */
    case 2:
      scroll_set(B->pg->scroll - phh);
      break; /* Page Up  */
    case 3:
      scroll_set(B->pg->scroll + phh);
      break; /* Page Down */
    }
  } else if (m == &bmarkmenu) {
    switch (item) {
    case 0:
      toggle_current_bookmark();
      break; /* Add/Remove Bookmark */
    case 1:
      bm_panel_open = !bm_panel_open;
      B->seq++;
      break; /* Show/Hide panel     */
    case 2:
      begin_new_folder(bookmarks_root());
      break; /* New top-level folder */
    }
  }
}

/* Hover link-scan cache */
static struct page *hov_pg;
static unsigned hov_seq;
static int hov_row = -1, hov_link = -1, hov_lo, hov_hi;

/* Line-relative x where display line `row`'s glyphs begin */
static int line_content_x(const struct ltab *t, int row)
{
  int len = t->len[row], base = t->off[row];
  set_block(&g_ui, base);
  int x = t->x[row] + g_ui.bindent * INDENT_PX;
  if (g_ui.bcenter == 1 || g_ui.bcenter == 2) {
    int lw = 0;
    unsigned cp;
    for (int p = base; p < base + len;) {
      int gp = p;
      p += utf8_decode(&B->pg->text[p], &B->pg->text[base + len], &cp);
      lw += glyph_adv(&g_ui, cp, gp);
    }
    int room = (t->w[row] - g_ui.bindent * INDENT_PX) - lw;
    if (room > 0)
      x += g_ui.bcenter == 1 ? room / 2 : room;
  }
  return x;
}

/* Byte column of the glyph under `mx` in the run [base,base+len) starting at pixel `x`, or -1 */
static int glyph_col(int x, int base, int len, int mx)
{
  for (int k = 0; mx >= x && k < len;) {
    unsigned cp;
    int nb = utf8_decode(&B->pg->text[base + k], &B->pg->text[base + len], &cp);
    int cw = glyph_adv(&g_ui, cp, base + k);
    if (mx < x + cw)
      return k;
    x += cw;
    k += nb;
  }
  return -1;
}

/* A hit on a position:fixed / top-layer box: sets *off to the glyph under (mx,my) or -1, true if
 * the overlay takes the click (text, a painted box, or a modal's backdrop). */
static bool overlay_hit(int mx, int my, int py, int phh, int *off)
{
  const struct layout_out *o = g_fr;
  int n, order[OVL_MAX];
  const struct lbox *bx = layout_pool(&n);
  bool modal = false;
  if (!o->n_ov || n != o->n_geo)
    return false;
  overlay_order(o, order);
  for (int q = o->n_ov - 1; q >= 0; q--) {
    const struct lovl *ov = &o->ov[order[q]];
    const struct lbox *b = &bx[ov->box];
    const struct lgeom *g = &o->geo[ov->box];
    int h, top = overlay_place(ov, phh, &h);
    int shift = py + top - g->y;
    const struct ltab *t = &o->ovlt;
    for (int i = ov->row0; i < ov->row1; i++) {
      int sy = shift + t->y[i];
      if (t->len[i] == 0 || my < sy || my >= sy + t->h[i])
        continue;
      int col = glyph_col(line_content_x(t, i), t->off[i], t->len[i], mx);
      if (col >= 0) {
        *off = t->off[i] + col;
        return true;
      }
    }
    for (int s = 0; s < o->n_ovfs; s++) {
      const struct fslice *f = &o->ovfs[s];
      if (f->row < ov->row0 || f->row >= ov->row1 || my < shift + t->y[f->row] || my >= shift + t->y[f->row] + f->h)
        continue;
      set_block(&g_ui, f->off);
      int col = glyph_col(f->x + g_ui.bindent * INDENT_PX, f->off, f->len, mx);
      if (col >= 0) {
        *off = f->off + col;
        return true;
      }
    }
    bool painted = b->bg != BG_NONE || (b->border_color != BG_NONE && b->border.t);
    if (painted && mx >= g->x && mx < g->x + g->w && my >= py + top && my < py + top + h) {
      *off = -1;
      return true;
    }
    modal |= ov->modal;
  }
  if (modal)
    *off = -1;
  return modal;
}

/* CSS cursor (CSS_CUR_AUTO = unset) and whether user-select:none covers the last pane_hit */
static int hit_cursor = CSS_CUR_AUTO;
static bool hit_noselect;

/* Innermost explicit cursor, user-select and pointer-events at text offset `off`; call under browser_buf_lock. */
static void regions_at(int off, int *cursor, int *usel, int *pev)
{
  *cursor = CSS_CUR_AUTO;
  *usel = *pev = -1;
  bool c = false, u = false, p = false;
  for (int i = 0; i < B->pg->nregs && !(c && u && p); i++) {
    const struct pregion *r = &B->pg->regs[i];
    if (off < r->start || off >= r->end)
      continue;
    if (!c && r->cursor >= 0) {
      *cursor = r->cursor;
      c = true;
    }
    if (!u && r->usel >= 0) {
      *usel = r->usel;
      u = true;
    }
    if (!p && r->pev >= 0) {
      *pev = r->pev;
      p = true;
    }
  }
}

/* Resolve a content-pane hit at client coords (mx,my) */
static void pane_hit(int mx, int my, int *off, char *url, int cap, int *newtab)
{
  *off = -1;
  hit_cursor = CSS_CUR_AUTO;
  hit_noselect = false;
  if (cap)
    url[0] = '\0';
  if (newtab)
    *newtab = 0;
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  int right = devtools_visible() ? devtools_left() : BW;
  if (my < py || my >= py + phh || mx < 0 || mx >= right - PAGE_SCROLL_W)
    return; /* outside the content pane: skip the lock */
  browser_buf_lock();
  bool ov_hit = overlay_hit(mx, my, py, phh, off);
  int row = lt_line_at(&g_fr->lt, B->pg->scroll + (my - py));
  if (!ov_hit && row >= 0 && row < g_fr->lt.n) {
    /* proportional */
    int col = glyph_col(line_content_x(&g_fr->lt, row), g_fr->lt.off[row], g_fr->lt.len[row], mx);
    if (col >= 0)
      *off = g_fr->lt.off[row] + col;
  }
  /* An inline image */
  if (*off < 0 && !ov_hit) {
    for (int i = 0; i < B->pg->n_imgs; i++) {
      struct inl_img *im = &B->pg->imgs[i];
      if (im->start_row < 0)
        continue;
      int r1 = im->start_row + (im->rows > 0 ? im->rows : 1);
      if (row >= im->start_row && row < r1) {
        *off = im->text_pos;
        break;
      }
    }
  }
  if (*off >= 0) {
    int usel, pev;
    regions_at(*off, &hit_cursor, &usel, &pev);
    hit_noselect = usel == 0;
    if (pev == 0) { /* pointer-events:none: the pointer sees nothing here */
      *off = -1;
      hit_cursor = CSS_CUR_AUTO;
    }
  }
  if (*off >= 0) {
    bool cache_ok = (hov_pg == B->pg && hov_seq == B->seq && row == hov_row);
    if (cache_ok && *off >= hov_lo && *off < hov_hi) {
      /* Same row + same link/gap span as the last move: reuse it. */
      if (hov_link >= 0 && hov_link < B->pg->nlinks) {
        resolve_url(B->pg->cur_url, B->pg->links[hov_link].href, url, cap);
        if (newtab)
          *newtab = B->pg->links[hov_link].newtab;
      }
    } else {
      /* Scan once, and cache both the covering link and, on a miss, the link-free gap around *off */
      int found = -1, lo = 0, hi = B->pg->text_len;
      for (int i = 0; i < B->pg->nlinks; i++) {
        int s = B->pg->links[i].start, e = B->pg->links[i].end;
        if (*off >= s && *off < e) {
          found = i;
          lo = s;
          hi = e;
          break;
        }
        if (e <= *off && e > lo)
          lo = e;
        if (s > *off && s < hi)
          hi = s;
      }
      if (found >= 0) {
        resolve_url(B->pg->cur_url, B->pg->links[found].href, url, cap);
        if (newtab)
          *newtab = B->pg->links[found].newtab;
      }
      hov_pg = B->pg;
      hov_seq = B->seq;
      hov_row = row;
      hov_link = found;
      hov_lo = lo;
      hov_hi = hi;
    }
  }
  browser_buf_unlock();
}

/* Map a pane pixel to (display line, byte offset within that line), clamped so a drag past */
static void pane_linecol(int mx, int my, int *line, int *col)
{
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  *line = 0;
  *col = 0;
  /* The worker rebuilds the line table and B->pg->text under browser_buf_lock */
  browser_buf_lock();
  if (B->pg->text && g_fr->lt.n > 0) {
    int row = lt_line_at(&g_fr->lt, B->pg->scroll + (my - py));
    if (row < 0)
      row = 0;
    if (row >= g_fr->lt.n)
      row = g_fr->lt.n - 1;
    *line = row;
    int len = g_fr->lt.len[row], base = g_fr->lt.off[row];
    int x = line_content_x(&g_fr->lt, row); /* match draw_styled_line */
    int c = len;                            /* default: end of line */
    for (int k = 0; k < len;) {
      unsigned cp;
      int nb = utf8_decode(&B->pg->text[base + k], &B->pg->text[base + len], &cp);
      int cw = glyph_adv(&g_ui, cp, base + k);
      if (mx < x + cw / 2) {
        c = k;
        break;
      } /* nearest char edge */
      x += cw;
      k += nb;
    }
    *col = c;
  }
  browser_buf_unlock();
}

/* Alpha-blend a translucent selection highlight over the frame buffer rect, clipped to the rows */
/* Selection tint at text offset `off`: the page's ::selection background there, else the default blue. */
unsigned sel_color_at(int off)
{
  for (int i = 0; i < B->pg->nregs; i++) {
    const struct pregion *r = &B->pg->regs[i];
    if (r->sel >= 0 && off >= r->start && off < r->end)
      return (unsigned)r->sel;
  }
  return 0x3366cc;
}

void sel_hl_rect(int x, int y, int w, int h, int ct, int cb, unsigned rgb)
{
  unsigned *fb = browser_surface();
  const unsigned sr = (rgb >> 16) & 0xff, sg = (rgb >> 8) & 0xff, sb = rgb & 0xff, a = 110, ia = 255 - a;
  for (int yy = y; yy < y + h; yy++) {
    if (yy < ct || yy >= cb || yy < 0 || yy >= BH)
      continue;
    unsigned *row = fb + (unsigned long)yy * BW;
    for (int xx = x; xx < x + w; xx++) {
      if (xx < 0 || xx >= BW)
        continue;
      unsigned d = row[xx];
      unsigned r = (sr * a + ((d >> 16) & 0xff) * ia) / 255;
      unsigned g = (sg * a + ((d >> 8) & 0xff) * ia) / 255;
      unsigned b = (sb * a + (d & 0xff) * ia) / 255;
      row[xx] = (r << 16) | (g << 8) | b;
    }
  }
}

static bool in_rect(int mx, int my, const struct rect *r)
{
  return mx >= r->x && mx < r->x + r->w && my >= r->y && my < r->y + r->h;
}

static struct textfield *drag_tf;
static int drag_x, drag_w;

static const char *const field_ctx_items[] = {"Cut", "Copy", "Paste", "Select All"};
static const char *const field_ctx_hints[] = {"Ctrl+X", "Ctrl+C", "Ctrl+V", "Ctrl+A"};
static unsigned char field_ctx_flags[4];
struct menu field_ctx = {"", field_ctx_items, 4, 0, field_ctx_hints, 0, field_ctx_flags, 0};
static struct textfield *ctx_tf;
static int ctx_x, ctx_y;

static void field_copy(struct textfield *tf)
{
  int a, b;
  if (textfield_selection(tf, &a, &b))
    clip_put_text(tf->buf + a, b - a);
}

static void field_paste(struct textfield *tf)
{
  char mime[64];
  void *data;
  int len;
  if (clip_get(0, mime, sizeof mime, &data, &len) != 0)
    return;
  if (!memcmp(mime, "text/", 5))
    textfield_insert(tf, data, len);
  free(data);
}

int field_key(struct textfield *tf, char c, int mods)
{
  if ((unsigned char)c == KEY_INTR) {
    field_copy(tf);
    return 1;
  }
  if (mods & KMOD_CTRL) {
    int a, b;
    if (c == 'x' || c == 'X') {
      if (!textfield_selection(tf, &a, &b))
        return 1;
      field_copy(tf);
      return textfield_key(tf, '\b', 0);
    }
    if (c == 'v' || c == 'V') {
      field_paste(tf);
      return 1;
    }
  }
  return textfield_key(tf, c, mods);
}

void field_menu_draw(void)
{
  if (field_ctx.open)
    menu_popup_draw(&field_ctx, ctx_x, ctx_y);
}

static bool field_menu_click(int mx, int my)
{
  int item = menu_popup_click(&field_ctx, mx, my, ctx_x, ctx_y);
  field_ctx.open = 0;
  B->seq++;
  switch (item) {
  case 0:
    field_key(ctx_tf, 'x', KMOD_CTRL);
    break;
  case 1:
    field_copy(ctx_tf);
    break;
  case 2:
    field_paste(ctx_tf);
    break;
  case 3:
    textfield_select_all(ctx_tf);
    break;
  default:
    return mx >= ctx_x && mx < ctx_x + GUI_MENU_W && my >= ctx_y && my < ctx_y + field_ctx.n * GUI_ROW_H;
  }
  addr_dirty = true;
  return true;
}

void handle_rclick(int mx, int my)
{
  drag_tf = 0;
  field_ctx.open = 0;
  menubar_close(&menubar);
  B->seq++;
  struct textfield *tf = 0;
  int fx = 0, fw = 0, fy = 0, fh = 0;
  struct rect rb, rf, rrl, rtf, rst;
  addr_layout(&rb, &rf, &rrl, &rtf, &rst);
  if (in_rect(mx, my, &rtf)) {
    tf = &B->addr_tf;
    fx = rtf.x, fw = rtf.w, fy = rtf.y, fh = rtf.h;
  } else if (devtools_field_at(mx, my, &tf, &fx, &fw)) {
    fy = my;
    fh = 1;
  }
  if (!tf)
    return;
  int a, b;
  if (!textfield_selection(tf, &a, &b))
    textfield_click(tf, mx, my, fx, fy, fw, fh);
  char mime[8];
  void *data;
  int len, has_sel = textfield_selection(tf, &a, &b);
  int can_paste = clip_get(0, mime, sizeof mime, &data, &len) == 0;
  if (can_paste)
    free(data);
  field_ctx_flags[0] = field_ctx_flags[1] = has_sel ? 0 : MENU_DISABLED;
  field_ctx_flags[2] = can_paste ? 0 : MENU_DISABLED;
  ctx_tf = tf;
  ctx_x = mx + GUI_MENU_W > BW ? BW - GUI_MENU_W : mx;
  ctx_y = my + field_ctx.n * GUI_ROW_H > BH ? BH - field_ctx.n * GUI_ROW_H : my;
  field_ctx.open = 1;
}

void drag_field(struct textfield *tf, int mx, int my, int x, int y, int w, int h)
{
  if (textfield_click(tf, mx, my, x, y, w, h)) {
    drag_tf = tf;
    drag_x = x;
    drag_w = w;
  }
}

/* Report the pointer to the worker when the page has :hover/:active rules and the target changed */
static void post_pointer(int off, int down)
{
  static int last_off = -2, last_down;
  if (!B->ptr_live || (off == last_off && down == last_down))
    return;
  last_off = off;
  last_down = down;
  B->ptr_off = off;
  B->ptr_down = down;
  B->ptr_pending = 1;
}

void handle_click(int mx, int my)
{
  drag_tf = 0;
  if (field_ctx.open && field_menu_click(mx, my))
    return;
  if (my < MENU_H || menu_any_open()) { /* menu bar / dropdown */
    bookmarks_menu_sync();
    struct menu *leaf;
    int item;
    if (menubar_click(&menubar, mx, my, &leaf, &item)) {
      if (item >= 0)
        menu_action(leaf, item);
      B->seq++;
      return;
    }
  }
  /* Developer tools dock (bottom of the window) */
  if (devtools_handle_click(mx, my))
    return;
  if (my >= MENU_H && my < MENU_H + TAB_H) { /* tab strip */
    blur_content_control();
    int idx = 0;
    switch (tabbar_hit(0, MENU_H, BW, n_tabs, mx, my, &idx)) {
    case TABBAR_SELECT:
      select_tab(idx);
      break;
    case TABBAR_CLOSE:
      close_tab(idx);
      break;
    case TABBAR_NEW:
      new_tab();
      break;
    }
    B->seq++;
    return;
  }
  if (my >= MENU_H + TAB_H && my < MENU_H + TAB_H + ADDR_H) { /* address bar */
    blur_content_control();
    struct rect rb, rf, rrl, rtf, rst;
    addr_layout(&rb, &rf, &rrl, &rtf, &rst);
    struct tab *at = &tabs[active_tab];
    if (hist_can_back(at) && button_hit(rb.x, rb.y, rb.w, rb.h, mx, my)) {
      go_back();
    } else if (hist_can_fwd(at) && button_hit(rf.x, rf.y, rf.w, rf.h, mx, my)) {
      go_fwd();
    } else if (button_hit(rrl.x, rrl.y, rrl.w, rrl.h, mx, my)) {
      if (is_loading())
        do_stop();
      else
        do_reload();
    } else if (B->pg->cur_url[0] && button_hit(rst.x, rst.y, rst.w, rst.h, mx, my)) {
      toggle_current_bookmark();
    } else {
      drag_field(&B->addr_tf, mx, my, rtf.x, rtf.y, rtf.w, rtf.h);
      gfx_sel_clear(&g_sel);
    }
    B->seq++;
    return;
  }
  if (bm_panel_open && bm_panel_click(mx, my))
    return;
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  int right = devtools_visible() ? devtools_left() : BW;
  int sbpart = scrollbar_hit_ex(right - PAGE_SCROLL_W, py, PAGE_SCROLL_W, phh, mx, my, &B->pg->sb_st);
  if (sbpart != SCROLLBAR_NONE) { /* scrollbar */
    int target;
    if (sbpart == SCROLLBAR_UP)
      target = B->pg->scroll - 3 * LINE_H;
    else if (sbpart == SCROLLBAR_DOWN)
      target = B->pg->scroll + 3 * LINE_H;
    else
      target = scrollbar_pos_ex(right - PAGE_SCROLL_W, py, PAGE_SCROLL_W, phh, mx, my, B->pg->scroll, g_fr->doc_h, phh,
                                &B->pg->sb_st);
    scroll_set(target);
    return;
  }
  if (my >= py && my < py + phh) { /* content pane click */
    /* Crashed tab: the only thing in the pane is the Reload button. */
    if (tabs[active_tab].crashed) {
      if (button_hit(crash_reload_rect.x, crash_reload_rect.y, crash_reload_rect.w, crash_reload_rect.h, mx, my))
        do_reload();
      return;
    }
    /* Inspect-element picker armed */
    if (devtools_picking()) {
      char res[URL_MAX];
      int off;
      pane_hit(mx, my, &off, res, sizeof res, 0);
      devtools_pick_commit(off);
      return;
    }
    /* A click on an <audio>/<video> controls widget is consumed by it (play/pause, seek, mute) */
    {
      int sc = B->pg->scroll;
      int right = devtools_visible() ? devtools_left() : BW;
      int paw = (right - PAGE_SCROLL_W) - 8;
      if (paw < 8)
        paw = 8;
      for (struct media_el *me = media_doc_first(); me; me = media_doc_next(me)) {
        int sr = media_el_start_row(me);
        if (sr < 0)
          continue;
        int y0 = py + g_fr->lt.y[sr] - sc, h = media_el_height(me, paw);
        if (mx >= 4 && mx < 4 + paw && my >= y0 && my < y0 + h) {
          media_el_click(me, mx, my, 4, y0, paw);
          return;
        }
      }
    }
    char res[URL_MAX];
    int off, nt = 0;
    pane_hit(mx, my, &off, res, sizeof res, &nt);
    post_pointer(off, 1);
    /* A press clears any previous selection. */
    if (gfx_sel_has(&g_sel)) {
      gfx_sel_clear(&g_sel);
      B->seq++;
    }
    g_selecting = false;
    /* If the page registered JS listeners, hand the click to the worker to dispatch through the live */
    if (off >= 0 && B->events_live) {
      /* clientX/clientY are viewport-relative (pane origin), matching getBoundingClientRect */
      B->evt_off = off;
      B->evt_x = mx;
      B->evt_y = my - py;
      {
        int k = 0;
        sapp(B->evt_type, &k, sizeof B->evt_type, "click");
      }
      {
        int k = 0;
        sapp(B->evt_text, &k, sizeof B->evt_text, res);
      }
      B->evt_pending = 1;
      B->seq++;
      return;
    }
    if (res[0]) {
      if (nt)
        new_tab(); /* target="_blank": open + switch to a new tab */
      navigate(res);
      return;
    }
    /* Plain text (no link): begin a selection; a following drag extends it. */
    if (hit_noselect)
      return;
    int line, col;
    pane_linecol(mx, my, &line, &col);
    gfx_sel_start(&g_sel, line, col);
    g_selecting = true;
  }
}

/* Pick the cursor for the element under (mx,my) */
static int cursor_for(int mx, int my, const char *link)
{
  if (mx < 0)
    return WIN_CURSOR_ARROW;
  if (my >= MENU_H && my < MENU_H + TAB_H)
    return WIN_CURSOR_HAND;                                   /* tab strip */
  if (my >= MENU_H + TAB_H && my < MENU_H + TAB_H + ADDR_H) { /* address bar */
    struct rect rb, rf, rrl, rtf, rst;
    addr_layout(&rb, &rf, &rrl, &rtf, &rst);
    if (in_rect(mx, my, &rtf))
      return WIN_CURSOR_TEXT;
    if (in_rect(mx, my, &rb) || in_rect(mx, my, &rf) || in_rect(mx, my, &rrl) || in_rect(mx, my, &rst))
      return WIN_CURSOR_HAND;
    return WIN_CURSOR_ARROW;
  }
  if (bm_panel_open && mx < BM_PANEL_W && my >= MENU_H + TAB_H + ADDR_H)
    return WIN_CURSOR_HAND;
  /* Content pane */
  if (mx >= (devtools_visible() ? devtools_left() : BW) - PAGE_SCROLL_W)
    return WIN_CURSOR_ARROW;
  /* Crashed tab */
  if (tabs[active_tab].crashed)
    return in_rect(mx, my, &crash_reload_rect) ? WIN_CURSOR_HAND : WIN_CURSOR_ARROW;
  if (hit_cursor != CSS_CUR_AUTO)
    return hit_cursor; /* the CSS_CUR_* order is WIN_CURSOR_* */
  return link[0] ? WIN_CURSOR_HAND : WIN_CURSOR_TEXT;
}

/* Pointer moved over the content pane; `down` = the button is held (a drag the page didn't consume) */
static void pointer_moved(int mx, int my, int down)
{
  char url[URL_MAX];
  int off;
  last_my = mx < 0 ? -1 : my; /* track for wheel routing (devtools vs page) */
  last_mx = mx;
  if (mx < 0) {
    url[0] = '\0';
    off = -1;
  } /* pointer left the window: clear */
  else
    pane_hit(mx, my, &off, url, sizeof url, 0);

  post_pointer(off, down);

  /* Feed the inspect-element picker the page offset under the cursor */
  if (devtools_picking())
    devtools_pick_hover(mx >= 0 && mx < devtools_left() ? off : -1);

  /* Per-element cursor; the loading spinner (event loop) outranks it. */
  if (!B->loading)
    gfx_cursor(B->win_id, cursor_for(mx, my, url));

  if (streq(url, hover_url))
    return;
  int k = 0;
  sapp(hover_url, &k, sizeof hover_url, url);
  int y0, y1;
  hover_rows(&y0, &y1);
  damage_rows(y0, y1);
}

/* Pointer moved over the content pane with no button held */
void handle_move(int mx, int my) { pointer_moved(mx, my, 0); }

/* Left-button drag in the content pane (WEV_DRAG) */
void handle_drag(int mx, int my)
{
  if (drag_tf) {
    textfield_drag(drag_tf, mx, drag_x, drag_w);
    B->seq++;
    return;
  }
  if (g_selecting) {
    int line, col;
    pane_linecol(mx, my, &line, &col);
    struct ru_selr before = {0}, after = {0};
    before.has = gfx_sel_range(&g_sel, &before.l0, &before.c0, &before.l1, &before.c1);
    gfx_sel_extend(&g_sel, line, col);
    after.has = gfx_sel_range(&g_sel, &after.l0, &after.c0, &after.l1, &after.c1);
    int lo, hi;
    if (ru_sel_damage(&before, &after, &lo, &hi))
      damage_lines(lo, hi);
    return;
  }
  pointer_moved(mx, my, 1);
}

/* Copy the current text selection to the clipboard */
void copy_selection(void)
{
  int l0, c0, l1, c1;
  if (!gfx_sel_range(&g_sel, &l0, &c0, &l1, &c1))
    return;
  browser_buf_lock();
  char *buf = 0;
  int n = 0;
  if (l0 < g_fr->lt.n && l1 < g_fr->lt.n) {
    int a = g_fr->lt.off[l0] + c0, b = g_fr->lt.off[l1] + c1;
    if (a < 0)
      a = 0;
    if (b > B->pg->text_len)
      b = B->pg->text_len;
    n = b - a;
    if (n > 0 && (buf = malloc((unsigned)n + 1))) {
      memcpy(buf, B->pg->text + a, (unsigned)n);
      buf[n] = '\0';
    }
  }
  browser_buf_unlock();
  if (buf) {
    clip_put_text(buf, n);
    free(buf);
  }
}

/* Select the whole rendered page (Edit > Select All). */
void select_all(void)
{
  browser_buf_lock();
  int last = g_fr->lt.n - 1, last_len = last >= 0 ? g_fr->lt.len[last] : 0;
  browser_buf_unlock();
  if (last < 0)
    return;
  gfx_sel_start(&g_sel, 0, 0);
  gfx_sel_extend(&g_sel, last, last_len);
  g_selecting = false;
  B->seq++;
}

void handle_key(char c, int mods)
{
  if (field_ctx.open) {
    field_ctx.open = 0;
    B->seq++;
    if ((unsigned char)c == 27)
      return;
  }
  /* Naming a new bookmark folder */
  if (bm_naming) {
    unsigned char uc = (unsigned char)c;
    if (uc == '\n' || uc == '\r') {
      folder_add(bm_name_parent, bm_name_buf);
      bm_naming = 0;
      B->seq++;
    } else if (uc == 27) {
      bm_naming = 0;
      B->seq++;
    } else {
      field_key(&bm_name_tf, c, mods);
      B->seq++;
    }
    return;
  }
  /* Ctrl+B toggles the bookmarks panel. */
  if ((mods & KMOD_CTRL) && (c == 'b' || c == 'B')) {
    bm_panel_open = !bm_panel_open;
    B->seq++;
    return;
  }
  /* Developer tools */
  if ((unsigned char)c == KEY_F12 || ((mods & KMOD_CTRL) && (mods & KMOD_SHIFT) && (c == 'i' || c == 'I'))) {
    devtools_toggle();
    return;
  }
  /* Ctrl+Shift+C arms the inspect-element picker (like the ↖ toolbar button) */
  if ((mods & KMOD_CTRL) && (mods & KMOD_SHIFT) && (c == 'c' || c == 'C')) {
    devtools_pick_toggle();
    return;
  }
  /* The Console REPL input takes keys when it is focused (returns 1) */
  if (devtools_handle_key(c, mods))
    return;
  /* Esc cancels an active pick before it would close the panel. */
  if ((unsigned char)c == 27 && devtools_picking()) {
    devtools_pick_toggle();
    return;
  }
  if ((unsigned char)c == 27 && devtools_visible()) {
    devtools_toggle();
    return;
  }

  /* Alt+Left / Alt+Right are history navigation */
  if (mods & KMOD_ALT) {
    if ((unsigned char)c == KEY_LEFT) {
      go_back();
      return;
    }
    if ((unsigned char)c == KEY_RIGHT) {
      go_fwd();
      return;
    }
  }

  /* Ctrl+R reloads (the keyboard delivers the unshifted letter + KMOD_CTRL) */
  if ((mods & KMOD_CTRL) && (c == 'r' || c == 'R')) {
    do_reload();
    return;
  }
  if ((mods & KMOD_CTRL) && (c == 't' || c == 'T')) {
    new_tab();
    return;
  }
  if ((mods & KMOD_CTRL) && (c == 'w' || c == 'W')) {
    close_tab(active_tab);
    return;
  }
  if ((mods & KMOD_CTRL) && c >= '1' && c <= '8') {
    int tab = c - '1';
    if (tab < n_tabs)
      select_tab(tab);
    return;
  }

  /* Ctrl+C (delivered as KEY_INTR) copies the text selection when there is one */
  if ((unsigned char)c == KEY_INTR && gfx_sel_has(&g_sel)) {
    copy_selection();
    return;
  }

  /* Menu accelerator (F5 = Reload): fire the item like a click. */
  struct menu *leaf;
  int sc = menubar_shortcut(&menubar, (unsigned char)c, &leaf);
  if (sc >= 0) {
    menubar_close(&menubar);
    menu_action(leaf, sc);
    return;
  }

  /* A focused content text field captures editing keys */
  if (B->focus_kind != 0) {
    unsigned char uc = (unsigned char)c;
    if (uc == 27) {
      {
        int k = 0;
        sapp(B->evt_type, &k, sizeof B->evt_type, "blur");
      }
      B->evt_pending = 1;
      B->seq++;
      return;
    }
    int ctrl_a = (mods & KMOD_CTRL) && (uc == 'a' || uc == 'A');
    if (ctrl_a || uc == KEY_INTR || uc == KEY_DELETE || uc == '\n' || uc == '\r' || uc == 8 || uc == 127 ||
        ((uc >= 0x20 && uc < 0x7f) && !(mods & KMOD_CTRL))) {
      B->evt_x = ctrl_a ? EDIT_SELECT_ALL : uc;
      {
        int k = 0;
        sapp(B->evt_type, &k, sizeof B->evt_type, "keyinput");
      }
      B->evt_pending = 1;
      B->seq++;
      return;
    }
  }

  if ((unsigned char)c == 27 && B->events_live && !menu_any_open()) {
    int k = 0;
    sapp(B->evt_type, &k, sizeof B->evt_type, "escape");
    B->evt_pending = 1;
    B->seq++;
    return;
  }

  if ((unsigned char)c == 27) {
    menubar_close(&menubar);
    B->seq++;
    return;
  }

  /* The address-bar widget handles editing (printable/Backspace/Delete/caret) */
  if (field_key(&B->addr_tf, c, mods)) {
    if (menu_any_open())
      B->seq++;
    else
      addr_dirty = true;
    return;
  }

  switch ((unsigned char)c) {
  case '\n':
  case '\r':
    submit();
    return;
  case KEY_UP:
    scroll_set(B->pg->scroll - 3 * LINE_H);
    return;
  case KEY_DOWN:
    scroll_set(B->pg->scroll + 3 * LINE_H);
    return;
  }
}
