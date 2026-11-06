#include "main_int.h"

/* Address-bar layout: nav buttons left, URL field middle, star button right. */
#define NAV_BTN_W 28

void addr_layout(struct rect *back, struct rect *fwd, struct rect *rel, struct rect *tf, struct rect *star)
{
  int y0 = MENU_H + TAB_H;
  back->x = 4;
  back->y = y0 + 3;
  back->w = NAV_BTN_W;
  back->h = ADDR_H - 6;
  fwd->x = back->x + NAV_BTN_W + 2;
  fwd->y = back->y;
  fwd->w = NAV_BTN_W;
  fwd->h = back->h;
  rel->x = fwd->x + NAV_BTN_W + 2;
  rel->y = back->y;
  rel->w = NAV_BTN_W;
  rel->h = back->h;
  star->x = BW - NAV_BTN_W - 4;
  star->y = back->y;
  star->w = NAV_BTN_W;
  star->h = back->h;
  int tx = rel->x + NAV_BTN_W + 6;
  tf->x = tx;
  tf->y = y0 + 4;
  tf->w = star->x - tx - 6;
  tf->h = ADDR_H - 8;
  if (tf->w < 32)
    tf->w = 32; /* keep the field usable on a narrow window */
}

/* True while the worker is fetching or a navigation is queued. */
bool is_loading(void) { return B->loading || B->req_pending; }

/* Hand-rasterized reload arrow (LiberationSans lacks U+21BB), filling a d x d box at (x0,y0). */
static void icon_reload(int x0, int y0, int d, unsigned col)
{
  int cx = x0 + d / 2, cy = y0 + d / 2, r = d / 2;
  for (int py = -r; py <= r; py++)
    for (int px = -r; px <= r; px++) {
      int dd = px * px + py * py;
      if (dd > r * r || dd < (r - 2) * (r - 2))
        continue;
      if (py < 0 && px >= 0 && px <= 2)
        continue; /* open the ring at the top */
      gfx_pixel(cx + px, cy + py, col);
    }
  /* arrowhead at the gap's right end */
  for (int k = 0; k <= 3; k++) {
    gfx_pixel(cx + k, cy - r, col);     /* along the top    */
    gfx_pixel(cx + 3, cy - r + k, col); /* turning downward */
  }
}

static void icon_stop(int x0, int y0, int d, unsigned col)
{
  for (int i = 0; i < d; i++) {
    gfx_pixel(x0 + i, y0 + i, col);
    gfx_pixel(x0 + i + 1, y0 + i, col); /* 2px-thick strokes */
    gfx_pixel(x0 + d - 1 - i, y0 + i, col);
    gfx_pixel(x0 + d - 2 - i, y0 + i, col);
  }
}

/* Reload arrow while idle, red stop cross while loading. */
void reload_btn_draw(const struct rect *r)
{
  int d = 12;
  int x0 = r->x + (r->w - d) / 2, y0 = r->y + (r->h - d) / 2;
  button_draw(r->x, r->y, r->w, r->h, "", BTN_NORMAL);
  if (is_loading())
    icon_stop(x0, y0, d, 0xe06a5a);
  else
    icon_reload(x0, y0, d, 0xe6ebf2);
}

/* Bookmark ribbon (V-notch tail) in a d x d box at (x0,y0), filled or outline. */
static void icon_bookmark(int x0, int y0, int d, unsigned col, int filled)
{
  int half = d / 2, notch = d / 3;
  for (int y = 0; y < d; y++)
    for (int x = 0; x < d; x++) {
      int ax = x - half;
      if (ax < 0)
        ax = -ax;
      int bottom = (d - 1) - notch + (half ? notch * ax / half : 0);
      if (y > bottom)
        continue; /* below the ribbon / in the tail cut */
      int edge = (x == 0 || x == d - 1 || y == 0 || y == bottom);
      if (filled || edge)
        gfx_pixel(x0 + x, y0 + y, col);
    }
}

/* Gold ribbon when bookmarked, hollow grey otherwise; disabled on a blank tab. */
void star_btn_draw(const struct rect *r)
{
  bool has_url = B->pg->cur_url[0] != '\0';
  bool marked = has_url && bookmark_find(B->pg->cur_url) != 0;
  int d = 13, x0 = r->x + (r->w - d) / 2, y0 = r->y + (r->h - d) / 2;
  button_draw(r->x, r->y, r->w, r->h, "", has_url ? BTN_NORMAL : BTN_DISABLED);
  if (marked)
    icon_bookmark(x0, y0, d, 0xf2c14e, 1); /* filled gold  */
  else
    icon_bookmark(x0, y0, d, 0x9098a0, 0); /* hollow grey  */
}

/* The page's <title>, else the host, else the URL. */
static void current_page_title(char *out, int cap)
{
  if (B->pg->doc_title[0]) {
    strlcpy(out, B->pg->doc_title, cap);
    return;
  }
  struct purl u;
  if (B->pg->cur_url[0] && parse_url(B->pg->cur_url, &u))
    strlcpy(out, u.host, cap);
  else
    strlcpy(out, B->pg->cur_url[0] ? B->pg->cur_url : "Untitled", cap);
}

/* Bookmarks panel state: open, scroll, selection, and the new-folder name field. */
#define BM_ROW_H 20
bool bm_panel_open;
int bm_panel_scroll;                /* first visible row index */
static struct bm_node *bm_selected; /* highlighted node, or NULL */
struct textfield bm_name_tf;        /* inline new-folder name entry */
char bm_name_buf[64];
int bm_naming;                  /* 1 = the name field is active */
struct bm_node *bm_name_parent; /* folder the new folder joins */

/* The selected folder when the panel is open, else the root. */
static struct bm_node *bm_target_folder(void)
{
  if (bm_panel_open && bm_selected && bm_selected->is_folder)
    return bm_selected;
  return bookmarks_root();
}

/* Toggle the current page's bookmark; no-op on a blank tab. */
void toggle_current_bookmark(void)
{
  const char *url = B->pg->cur_url;
  if (!url[0])
    return;
  if (bookmark_find(url)) {
    bookmark_remove(url);
  } else {
    char title[128];
    current_page_title(title, sizeof title);
    bookmark_add(bm_target_folder(), url, title, B->pg->favicon_ok ? B->pg->favicon : 0);
  }
  B->seq++; /* repaint: the star + panel reflect the change */
}

/* Open the panel with an empty name field for a new folder in `parent` (root if NULL). */
void begin_new_folder(struct bm_node *parent)
{
  bm_panel_open = true;
  bm_name_parent = parent ? parent : bookmarks_root();
  bm_naming = 1;
  textfield_set(&bm_name_tf, "");
  B->seq++;
}

/* Refresh the Bookmarks menu labels and item-0 state before it is drawn or clicked. */
void bookmarks_menu_sync(void)
{
  bool marked = B->pg->cur_url[0] && bookmark_find(B->pg->cur_url) != 0;
  strlcpy(bm_toggle_label, marked ? "Remove Bookmark" : "Add Bookmark", sizeof bm_toggle_label);
  strlcpy(bm_show_label, bm_panel_open ? "Hide Bookmarks" : "Show Bookmarks", sizeof bm_show_label);
  bm_items[0] = bm_toggle_label;
  bm_items[1] = bm_show_label;
  bm_items[2] = "New Folder";
  bm_flags[0] = B->pg->cur_url[0] ? 0 : MENU_DISABLED;
  bm_flags[1] = 0;
  bm_flags[2] = 0;
}

/* URL of the link under the cursor, shown in the status box; empty = none. Main thread only. */
char hover_url[URL_MAX];

/* Last pointer position, so the wheel scrolls the surface under it. */
int last_my = -1;
int last_mx = -1;
int dt_resizing = 0; /* dragging the devtools panel's resize edge */

/* Set when only the address bar changed, so the main loop repaints just that strip. */
bool addr_dirty;

/* Copy `src` into `dst`, truncated with "..." to fit `maxpx` pixels in the UI font. */
void fit_ui(char *dst, int cap, const char *src, int maxpx)
{
  int n = 0;
  if (gfx_ui_width(src) <= maxpx) { /* whole string fits */
    while (src[n] && n < cap - 1) {
      dst[n] = src[n];
      n++;
    }
    dst[n] = '\0';
    return;
  }
  const char *p = src, *end = src + strlen(src);
  int ell = gfx_ui_width("..."), used = 0;
  while (*p) {
    unsigned cp;
    int nb = utf8_decode(p, end, &cp);
    if (used + gfx_ui_cw_cp(cp) + ell > maxpx)
      break;
    for (int i = 0; i < nb && n < cap - 1; i++)
      dst[n++] = p[i];
    used += gfx_ui_cw_cp(cp);
    p += nb;
  }
  dst[n] = '\0';
  sapp(dst, &n, cap, "...");
}

/* Bookmarks panel geometry: panel rect, header buttons, name field, and the first tree row y. */
static void bm_layout(struct rect *nf, struct rect *del, struct rect *name, struct rect *panel, int *list_y)
{
  int top = MENU_H + TAB_H + ADDR_H;
  panel->x = 0;
  panel->y = top;
  panel->w = BM_PANEL_W;
  panel->h = BH - top;
  if (panel->h < 0)
    panel->h = 0;
  int by = top + 24; /* below the header title */
  nf->x = 6;
  nf->y = by;
  nf->w = 96;
  nf->h = 20;
  del->x = 108;
  del->y = by;
  del->w = 60;
  del->h = 20;
  int ny = by + 26;
  name->x = 6;
  name->y = ny;
  name->w = BM_PANEL_W - 12;
  name->h = 20;
  *list_y = ny + (bm_naming ? 26 : 0);
}

/* Visible tree rows (children only when expanded), shared by render and hit-testing. */
struct bm_row {
  struct bm_node *node;
  int depth;
};
static struct bm_row *bm_rows;
static int bm_nrows, bm_rows_cap;
static void bm_flatten_into(struct bm_node *folder, int depth)
{
  for (struct bm_node *n = folder->child; n; n = n->next) {
    struct bm_row *p = grow_arr(bm_rows, &bm_rows_cap, bm_nrows + 1, sizeof *p);
    if (p) {
      bm_rows = p;
      bm_rows[bm_nrows].node = n;
      bm_rows[bm_nrows].depth = depth;
      bm_nrows++;
    }
    if (n->is_folder && n->expanded)
      bm_flatten_into(n, depth + 1);
  }
}
static void bm_flatten(void)
{
  bm_nrows = 0;
  bm_flatten_into(bookmarks_root(), 0);
}

/* Disclosure triangle, right (collapsed) or down (open), centred on `cy`. */
static void bm_tri(int x, int cy, int open, unsigned col)
{
  for (int i = 0; i <= 3; i++)
    for (int j = -(3 - i); j <= (3 - i); j++)
      gfx_pixel(open ? x + j : x + i - 1, open ? cy - 1 + i : cy + j, col);
}

/* A tiny folder glyph anchored at (x,y). */
static void bm_folder_glyph(int x, int y, unsigned col)
{
  gfx_fill_rect(x, y + 1, 5, 2, col);  /* the tab  */
  gfx_fill_rect(x, y + 3, 13, 9, col); /* the body */
}

void bookmarks_panel_render(void)
{
  struct rect nf, del, name, panel;
  int list_y;
  bm_layout(&nf, &del, &name, &panel, &list_y);
  int top = panel.y, bottom = panel.y + panel.h;

  gfx_fill_rect(panel.x, panel.y, panel.w, panel.h, 0x1b2129);
  gfx_fill_rect(0, top, BM_PANEL_W, 24, 0x232c38); /* header */
  gfx_ui_text(8, top + 3, "Bookmarks", 0xe8eaee);
  button_draw(nf.x, nf.y, nf.w, nf.h, "New Folder", BTN_NORMAL);
  button_draw(del.x, del.y, del.w, del.h, "Delete", bm_selected ? BTN_NORMAL : BTN_DISABLED);
  if (bm_naming)
    textfield_draw(&bm_name_tf, name.x, name.y, name.w, name.h, 1);

  bm_flatten();
  int cap = (bottom - list_y) / BM_ROW_H;
  if (cap < 0)
    cap = 0;
  int maxscroll = bm_nrows - cap;
  if (maxscroll < 0)
    maxscroll = 0;
  if (bm_panel_scroll > maxscroll)
    bm_panel_scroll = maxscroll;
  if (bm_panel_scroll < 0)
    bm_panel_scroll = 0;

  int y = list_y;
  for (int i = bm_panel_scroll; i < bm_nrows && y + BM_ROW_H <= bottom; i++, y += BM_ROW_H) {
    struct bm_node *n = bm_rows[i].node;
    int rx = 6 + bm_rows[i].depth * 14;
    if (n == bm_selected)
      gfx_fill_rect(0, y, BM_PANEL_W, BM_ROW_H, 0x2c4a6e);
    char label[128];
    int tx;
    if (n->is_folder) {
      bm_tri(rx + 2, y + BM_ROW_H / 2, n->expanded, 0xb8c0cc);
      bm_folder_glyph(rx + 10, y + 3, 0xd9a441);
      tx = rx + 28;
    } else {
      if (n->icon)
        gfx_blit_argb(rx + 10, y + 2, FAVICON_W, FAVICON_H, n->icon);
      else {
        gfx_fill_rect(rx + 12, y + 4, 12, 12, 0x4a5568); /* globe placeholder */
        gfx_rect(rx + 12, y + 4, 12, 12, 0x6b7688);
      }
      tx = rx + 30;
    }
    fit_ui(label, sizeof label, n->title ? n->title : "", BM_PANEL_W - tx - 6);
    gfx_ui_text(tx, y + 2, label, n->is_folder ? 0xe8eaee : 0xd2d7dd);
  }
  gfx_fill_rect(BM_PANEL_W - 1, top, 1, panel.h, 0x0f1319); /* right border */
}

/* Handle a click in the panel; false lets it fall through to the page. */
bool bm_panel_click(int mx, int my)
{
  struct rect nf, del, name, panel;
  int list_y;
  bm_layout(&nf, &del, &name, &panel, &list_y);
  if (mx < panel.x || mx >= panel.x + panel.w || my < panel.y || my >= panel.y + panel.h)
    return false;
  if (bm_naming)
    return true; /* naming: keys, not clicks */
  if (button_hit(nf.x, nf.y, nf.w, nf.h, mx, my)) {
    begin_new_folder(bm_target_folder());
    return true;
  }
  if (bm_selected && button_hit(del.x, del.y, del.w, del.h, mx, my)) {
    bm_delete(bm_selected);
    bm_selected = 0;
    B->seq++;
    return true;
  }
  if (my >= list_y) {
    bm_flatten();
    int idx = bm_panel_scroll + (my - list_y) / BM_ROW_H;
    if (idx >= 0 && idx < bm_nrows) {
      struct bm_node *n = bm_rows[idx].node;
      int rx = 6 + bm_rows[idx].depth * 14;
      if (n->is_folder && mx >= rx && mx < rx + 10) {
        folder_toggle(n);
        B->seq++;
        return true;
      }
      bm_selected = n;
      if (!n->is_folder && n->url)
        navigate(n->url);
      else
        B->seq++;
    } else {
      bm_selected = 0;
      B->seq++;
    }
  }
  return true;
}
