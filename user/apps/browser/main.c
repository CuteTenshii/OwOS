/* main.c , the browser's windowed UI and event loop. Owns the one `struct browser` instance */
#include "main_int.h"

static struct browser binst;
struct browser *B = &binst;

static struct layout_out g_lo[2];
struct layout_out *g_fr = &g_lo[0]; /* what render() paints */
struct layout_out *g_bk = &g_lo[1]; /* what rewrap() builds */

/* Content-pane text selection (drag to select, Ctrl+C to copy) */
struct gfx_sel g_sel;
bool g_selecting;

int BW = 600, BH = 368; /* client size; grows/shrinks on resize */

const unsigned palette[16] = {
    0x202830, 0x1a4fbf, 0xc01818, 0x108010, 0x9020a0, 0x9a6020, 0x107c90, 0x586070,
    0x16387a, 0x000000, 0x707000, 0xd07000, 0x808890, 0x304060, 0x2050d0, 0xa0a0a0,
};

/* The app menu bar */
bool show_images = true; /* "View > Show Images" checkbox state */

static const char *const page_items[] = {"Reload", "Home", "Stop"};
static const char *const page_hints[] = {"F5", "", ""};
static const unsigned char page_keys[] = {KEY_F5, 0, 0};
unsigned char page_flags[] = {0, 0, MENU_DISABLED}; /* Stop: idle */
struct menu pagemenu = {"Page", page_items, 3, 0, page_hints, page_keys, page_flags, 0};

/* "View > Scroll" submenu (opened by the View "Scroll" item). */
static const char *const scroll_items[] = {"Top", "Bottom", "Page Up", "Page Down"};
struct menu scrollmenu = {"Scroll", scroll_items, 4, 0, 0, 0, 0, 0};

static const char *const view_items[] = {"Show Images", "Scroll"};
unsigned char view_flags[] = {MENU_CHECKED, 0}; /* images on */
static struct menu *const view_subs[] = {0, &scrollmenu};
struct menu viewmenu = {"View", view_items, 2, 0, 0, 0, view_flags, view_subs};

/* "Edit" menu */
static const char *const edit_items[] = {"Copy", "Select All"};
unsigned char edit_flags[] = {MENU_DISABLED, 0};
struct menu editmenu = {"Edit", edit_items, 2, 0, 0, 0, edit_flags, 0};

/* "Bookmarks" menu */
const char *bm_items[3];
unsigned char bm_flags[3];
char bm_toggle_label[24]; /* "Add"/"Remove Bookmark"  */
char bm_show_label[20];   /* "Show"/"Hide Bookmarks"  */
struct menu bmarkmenu = {"Bookmarks", bm_items, 3, 0, 0, 0, bm_flags, 0};

static struct menu *const bar_menus[] = {&pagemenu, &editmenu, &viewmenu, &bmarkmenu};
struct menubar menubar = {bar_menus, 4, 2, 0};

/* The window's owner sets the title once a fetch finishes (worker can't). */
void update_title(int id)
{
  if (B->pg->doc_title[0]) {
    win_settitle(id, B->pg->doc_title);
    return;
  }
  struct purl u;
  if (B->pg->cur_url[0] && parse_url(B->pg->cur_url, &u)) {
    char t[80];
    int ti = 0;
    sapp(t, &ti, sizeof t, "browser - ");
    sapp(t, &ti, sizeof t, u.host);
    win_settitle(id, t);
    return;
  }
  win_settitle(id, "New Tab"); /* blank tab: match the tab-strip label */
}

int main(int argc, char **argv)
{
  const char *start = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : 0;
  B->pg = &tabs[active_tab].page; /* point at the first tab's page */
  int id = win_create(BW, BH, "browser");
  if (id < 0) {
    puts("browser: no window\n");
    return 1;
  }
  B->win_id = id;
  gfx_init(browser_surface(), BW, BH);
  B->pg->page_bg = 0xffffff;
  B->pg->page_fg = 0x202830; /* until a page sets them */

  textfield_init(&B->addr_tf, B->addr, URL_MAX);
  textfield_init(&bm_name_tf, bm_name_buf, sizeof bm_name_buf);
  if (start)
    textfield_set(&B->addr_tf, start); /* else a blank New Tab (empty bar) */
  B->layout_dirty = 1;

  jar_load();       /* restore cookies + HSTS from the persistent SQLite db */
  perm_load();      /* restore "Allow always" permission grants */
  cache_load();     /* restore the persistent HTTP response cache from disk */
  bookmarks_load(); /* restore the bookmarks tree from disk */

  int wtid = thread_create(worker_fn, &worker_stack[sizeof worker_stack]);

  /* Launched with a URL */
  if (start)
    navigate(start);
  else
    update_title(id); /* blank launch tab: title matches "New Tab" */

  unsigned last = B->seq - 1;
  int prev_loading = 0;
  /* Repaint coalescing while a page loads */
  unsigned frame = 0, last_paint_frame = 0;
  const unsigned LOAD_REDRAW_FRAMES = 6; /* ~6*15ms ≈ 90ms → ~10 fps while loading */
  for (;;) {
    struct win_event ev;
    while (win_poll(id, &ev)) {
      if (ev.type == WEV_CLOSE) {
        worker_quit = true;
        B->dlg_done = 1; /* release the worker if it's blocked in a dialog */
        if (wtid >= 0)
          thread_join(wtid); /* let the worker exit first */
        win_close(id);
        return 0;
      } else if (ev.type == WEV_KEY) {
        handle_key((char)ev.a, ev.b);
      } else if (ev.type == WEV_MOUSE) {
        last_mx = ev.a;
        last_my = ev.b;
        if (devtools_on_splitter(ev.a, ev.b)) {
          dt_resizing = 1;
        } /* start a panel resize */
        else {
          dt_resizing = 0;
          handle_click(ev.a, ev.b);
        }
      } else if (ev.type == WEV_DRAG) {
        if (dt_resizing)
          devtools_set_width(BW - ev.a); /* docked right: width = BW - x */
        else
          handle_drag(ev.a, ev.b);
      } else if (ev.type == WEV_MOVE) {
        handle_move(ev.a, ev.b);
      } else if (ev.type == WEV_SCROLL) {
        /* The wheel scrolls whichever surface the pointer is over */
        if (bm_panel_open && last_mx >= 0 && last_mx < BM_PANEL_W && last_my >= MENU_H + TAB_H + ADDR_H) {
          bm_panel_scroll -= ev.a;
          if (bm_panel_scroll < 0)
            bm_panel_scroll = 0;
          B->seq++;
        } else if (devtools_visible() && last_mx >= devtools_left() && last_my >= devtools_top()) {
          devtools_handle_scroll(ev.a);
        } else {
          scroll_set(B->pg->scroll - ev.a * 3 * LINE_H); /* wheel up scrolls back */
        }
      } else if (ev.type == WEV_RESIZE) {
        int prev_w = BW, prev_h = BH;
        BW = ev.a;
        BH = ev.b;
        /* Grow the back buffer BEFORE committing the new size */
        browser_surface();
        if (!browser_surface_ready()) {
          BW = prev_w;
          BH = prev_h;
        }
        gfx_init(browser_surface(), BW, BH);
        B->resize_ms = msec();
        B->recascade = 1; /* @media may change → worker re-cascades  */
        B->seq++;         /* re-wrap + repaint at the new width */
      }
    }
    /* History API requests posted by JS on the worker (see browser/js/bindings.c) */
    if (B->hist_nav) { /* pushState / replaceState */
      struct tab *t = &tabs[active_tab];
      B->hist_nav = 0;
      if (B->hist_replace && t->hist_n > 0) {
        int n = (int)strlen(B->hist_url) + 1;
        char *dup = malloc((unsigned)n);
        if (dup) {
          memcpy(dup, B->hist_url, (unsigned)n);
          free(t->hist[t->hist_i]);
          t->hist[t->hist_i] = dup;
        }
      } else {
        hist_push(t, B->hist_url);
      }
      textfield_set(&B->addr_tf, B->hist_url);
      B->seq++;
    }
    if (B->hist_go) { /* history.back/forward/go */
      int d = B->hist_go;
      B->hist_go = 0;
      if (d < 0)
        go_back();
      else
        go_fwd();
    }
    if (B->loc_nav) { /* location.assign/replace/reload */
      B->loc_nav = 0;
      navigate(B->loc_url);
    }
    /* window.alert/confirm/prompt */
    if (B->dlg_req && !B->dlg_done) {
      int req = B->dlg_req, ok = 0;
      if (req == 1) { /* alert */
        dialog_msg(id, B->dlg_title, B->dlg_msg, DLG_ICON_INFO | DLG_OK);
        ok = 1;
      } else if (req == 2) { /* confirm */
        ok = dialog_msg(id, B->dlg_title, B->dlg_msg, DLG_ICON_QUESTION | DLG_OKCANCEL) == DLG_R_OK;
      } else if (req == 4) { /* permission: Refuse/Once/Always */
        static const char *const perm_btns[3] = {"Refuse", "Allow once", "Allow always"};
        /* A locked modal (no close/min/max, not draggable) */
        int r = dialog_choose(id, B->dlg_title, B->dlg_msg, perm_btns, 3, DLG_ICON_QUESTION);
        ok = r; /* -1 = dismissed via Esc (worker: ask again later) */
      } else {  /* prompt */
        ok = dialog_prompt(id, B->dlg_title, B->dlg_msg, B->dlg_text, B->dlg_text, sizeof B->dlg_text);
      }
      gfx_init(browser_surface(), BW, BH); /* dialog borrowed the surface */
      B->dlg_ok = ok;
      B->dlg_done = 1; /* release the worker (publish last) */
      B->seq++;        /* repaint the browser window */
    }
    B->hist_len = tabs[active_tab].hist_n;

    if (B->loading && !prev_loading)   /* fetch started */
      gfx_cursor(id, WIN_CURSOR_BUSY); /* spinner while loading */
    if (prev_loading && !B->loading) { /* fetch finished */
      gfx_cursor(id, WIN_CURSOR_ARROW);
      imgc_clear();      /* drop the old page's decoded images */
      tab_sync_active(); /* refresh the tab label */
      update_title(id);
      if (B->pg->cur_url[0])                        /* show the resolved URL */
        textfield_set(&B->addr_tf, B->pg->cur_url); /* (incl. the scheme) */
      /* Record the page we landed on, unless this load was itself a Back/Next move */
      if (!nav_from_history)
        hist_push(&tabs[active_tab], B->pg->cur_url);
      nav_from_history = false;
      if (B->pg->favicon_ok && B->pg->cur_url[0]) /* update a saved bookmark's icon */
        bookmark_set_icon(B->pg->cur_url, B->pg->favicon);
    }
    prev_loading = B->loading;

    /* While audio plays, nudge a repaint a few times a second so the seek bar and time advance */
    if (media_active() && (frame - last_paint_frame) >= LOAD_REDRAW_FRAMES)
      damage_media();

    /* Publish the content-pane pixel width for the worker's line-table flow (must match render */
    {
      int right = devtools_visible() ? devtools_left() : BW;
      int wpaw = right - SCROLL_W - 8;
      if (wpaw < 8)
        wpaw = 8;
      B->want_paw = wpaw;
    }

    int due = !B->loading || (frame - last_paint_frame) >= LOAD_REDRAW_FRAMES;
    int dt_paint = devtools_take_paint(); /* devtools-local change (log line) */
    unsigned seq = B->seq;
    if (seq != last && due) {
      if (!render_incremental(seq - last, id)) {
        render();
        browser_present(id);
      }
      g_dmg.bumps = 0;
      g_dmg.band = false;
      last = seq;
      last_paint_frame = frame;
      addr_dirty = false;
    } else if (addr_dirty || dt_paint) { /* partial repaints: addr bar / devtools dock */
      int addr_paint = addr_dirty;
      if (addr_dirty) {
        render_addr();
        addr_dirty = false;
      }
      if (dt_paint) {
        browser_buf_lock();
        devtools_render(); /* opaque right-side split: dock only */
        browser_buf_unlock();
      }
      if (addr_paint)
        browser_blit_rect(0, MENU_H + TAB_H, BW, ADDR_H);
      if (dt_paint) {
        int left = devtools_left();
        browser_blit_rect(left, devtools_top(), BW - left, BH - devtools_top());
      }
      win_present(id);
    }
    frame++;
    win_wait(id);
  }
}
