/* Private declarations shared by the browser UI modules split out of main.c. */
#ifndef BROWSER_MAIN_INT_H
#define BROWSER_MAIN_INT_H

// clang-format off
#include <stdbool.h>
// clang-format
#include "animimg.h" /* GIF / animated WebP frame playback */
#include "browser.h"
#include "clipboard.h" /* clip_put_text for Copy */
#include "dialog.h"
#include "features/anim/anim.h"
#include "features/bookmarks/bookmarks.h" /* persistent (url,title) bookmarks store */
#include "features/cache/cache.h"         /* persistent HTTP response cache: load at startup */
#include "features/media/bmedia.h"        /* <audio>/<video> controls widget + playback state */
#include "font.h"                         /* custom @font-face faces (user/lib/font) */
#include "layout.h"                       /* the CSS box tree (backgrounds/borders behind the text) */
#include "render_util.h"

/* The word-wrap line table */
struct ltab {
  int *off, *len, *x, *w, *y, *h, *px;
  int n, cap;
};

struct lgeom {
  int x, y, w, h, cx, cy, cw, ch;
};

struct lmedia {
  struct media_el *me;
  int row, rows;
};

/* A position:fixed / top-layer box laid out out of flow: its lines live in layout_out.ovlt */
struct lovl {
  int box;       /* index into the box pool */
  int row0, row1; /* its line range in ovlt */
  int y0;        /* ovlt pixel top of the range */
  int h;         /* border-box height */
  bool modal;    /* a modal dialog: dims and blocks the page */
};

#define OVL_MAX 32

/* One complete layout result */
struct layout_out {
  struct ltab lt;
  struct ltab ovlt;    /* the lines of every overlay, stacked */
  struct fslice *ovfs; /* flex/grid item text of the overlays */
  int n_ovfs, cap_ovfs;
  struct lovl *ov;
  int n_ov, cap_ov;
  struct fslice *fs; /* flex/grid item text over reserved bands */
  int n_fs, cap_fs;
  struct lgeom *geo; /* per-box geometry, indexed like the box pool */
  int n_geo, cap_geo;
  int *img_pos, *img_row, *img_rows;
  int n_img, cap_img;
  struct lmedia *med;
  int n_med, cap_med;
  struct ru_bands bands;
  int doc_h;
  int over; /* furthest a glyph can spill below its line box, px */
};
/* --- tabs ------------------------------------------------------------------ */
/* Each tab keeps its URL, label and scroll offset, plus */
#define MAX_TABS 8
#define HIST_MAX 32 /* per-tab back/forward history depth */
struct tab {
  char url[URL_MAX]; /* address-bar / navigation target               */
  char label[40];    /* tab-strip label (page title or host)          */
  struct page page;

  /* Per-tab navigation history */
  char *hist[HIST_MAX];
  int hist_n; /* number of entries in use                    */
  int hist_i; /* index of the current page (0..hist_n-1)     */

  struct dt_view dt; /* this tab's devtools view (panel/scroll/open) */

  /* Set when this tab's page faulted (JS-runtime/GC OOM, a use-after-free, deep recursion overrun) */
  bool crashed;
  int crash_sig;
};
#define LINE_H UIFONT_H
#define BODY_PX 15.0f
struct bctx {
  int bpx;     /* heading px, 0 = default body font   */
  bool bmono;  /* <pre>: render in the monospace cell */
  int bcenter; /* text-align:center / <center>        */
  int bindent; /* indent level 0..7                   */
  bool bhr;    /* this line is a horizontal rule      */
  int blh;     /* CSS line-height px at the line start, 0 = auto */
  struct ru_gtab at;
  struct ru_gtab gt;
  struct font *gf; /* the face the caches currently hold */
  bool worker;
  bool held; /* the caller already holds browser_buf_lock */
};
#define INDENT_PX 24
struct rect {
  int x, y, w, h;
};
#define BM_PANEL_W 250

struct dmg_state {
  unsigned bumps;
  bool band;
  int y0, y1;
};

/* tabs.c */
extern struct tab tabs[MAX_TABS];
extern int n_tabs;
extern int active_tab;
extern bool nav_from_history;
void hist_push(struct tab *t, const char *url);
bool hist_can_back(const struct tab *t);
bool hist_can_fwd(const struct tab *t);
void pane_metrics(int *py, int *phh, int *rows);
void scroll_set(int target);
void navigate(const char *url);
void blur_content_control(void);
void submit(void);
void do_reload(void);
void do_stop(void);
void go_back(void);
void go_fwd(void);
void tab_sync_active(void);
void select_tab(int i);
void new_tab(void);
void close_tab(int i);

/* worker.c */
extern unsigned g_lay_sig;
extern char worker_stack[262144];
extern volatile bool worker_quit;
void worker_fn(void);

/* text.c */
void *grow_arr(void *p, int *cap, int need, unsigned esz);
int lt_bottom(const struct ltab *t, int i);
int lt_doc_px(const struct ltab *t);
int lt_line_at(const struct ltab *t, int y);
extern struct bctx g_ui;
extern struct bctx g_wk;
void set_block(struct bctx *c, int off);
int glyph_px(int off);
int body_cp_adv(struct bctx *c, unsigned cp, int px);
int body_draw_run(int x, int y, const char *s, int len, int width, unsigned fg, int ct, int cb, int px, int baseline,
/* Advance of the glyph at text offset `off`: its font size plus its letter-spacing. */
static inline int glyph_adv(struct bctx *c, unsigned cp, int off) { return body_cp_adv(c, cp, glyph_px(off)) + B->pg->ls[off]; }
                  int italic);
int run_ascent(int px);
int line_height(int maxpx, int lhmax);
extern bool g_ov_active;
extern int g_ov_base;
extern int g_ov_n;
void add_line(int off, int len, int x, int w, int h, int maxpx);
void img_disp_size(const struct inl_img *im, int paw, int *dw, int *dh);
extern struct ru_imgidx g_imgix;
extern bool g_imgix_ok;
extern bool g_has_media;
int img_at_pos(int pos);
int scroll_max(void);

/* flow.c */
extern bool g_box_layout;
unsigned layout_sig(int paw);
void layout_keep(void);
void box_extent(const struct lbox *b, const struct lgeom *g, int *top, int *bot);
void rewrap(int paw);

/* image.c */
const unsigned *xmap_get(unsigned dw, unsigned iw);
void draw_image(int px, int py, int pw, int ph, unsigned bg);
void imgc_clear(void);
void image_anim_tick(void); /* advance a playing full-page GIF / animated WebP */
void draw_inline_image(struct inl_img *im, int py, int ct, int cb, int paw);
void draw_alt_box(struct inl_img *im, int py, int ct, int cb);

/* paint.c */
int draw_slice_core(int x, int y, int off, int len, int paw, int ct, int cb, int line_h, int maxpx);
void draw_styled_line(int x, int y, int idx, int paw, int ct, int cb);
void paint_boxes(int py, int pw, int ct, int cb);
int overlay_order(const struct layout_out *o, int *order); /* overlay indices, bottom to top */
int overlay_place(const struct lovl *ov, int phh, int *h); /* pane-relative border-box top and drawn height */
void paint_overlay_boxes(const struct lovl *ov, int py, int pw, int ct, int cb, int top, int h);
void draw_dt_highlight(int x, int y, int w, int h, int clip_top, int clip_bot, int clip_right, const char *label);
extern struct rect crash_reload_rect;
void draw_crash_page(int py, int pw, int phh);

/* chrome.c */
void addr_layout(struct rect *back, struct rect *fwd, struct rect *rel, struct rect *tf, struct rect *star);
bool is_loading(void);
void reload_btn_draw(const struct rect *r);
void star_btn_draw(const struct rect *r);
extern bool bm_panel_open;
extern int bm_panel_scroll;
extern struct textfield bm_name_tf;
extern char bm_name_buf[64];
extern int bm_naming;
extern struct bm_node *bm_name_parent;
void toggle_current_bookmark(void);
void begin_new_folder(struct bm_node *parent);
void bookmarks_menu_sync(void);
extern char hover_url[URL_MAX];
extern int last_my;
extern int last_mx;
extern int dt_resizing;
extern bool addr_dirty;
void fit_ui(char *dst, int cap, const char *src, int maxpx);
void bookmarks_panel_render(void);
bool bm_panel_click(int mx, int my);

/* render.c */
void hover_rows(int *y0, int *y1);
void render(void);
void damage_rows(int y0, int y1);
void damage_lines(int lo, int hi);
void damage_media(void);
bool render_incremental(unsigned seq_delta, int id);
bool menu_any_open(void);
void render_addr(void);

/* input.c */
void sel_hl_rect(int x, int y, int w, int h, int ct, int cb, unsigned rgb);
unsigned sel_color_at(int off);
void handle_click(int mx, int my);
void handle_move(int mx, int my);
void handle_drag(int mx, int my);
void handle_rclick(int mx, int my);
void field_menu_draw(void);                            /* right-click menu for text fields, drawn last */
extern struct menu field_ctx;
void copy_selection(void);
void select_all(void);
void handle_key(char c, int mods);

/* main.c */
extern struct layout_out *g_fr;
extern struct layout_out *g_bk;
extern struct gfx_sel g_sel;
extern bool g_selecting;
extern bool show_images;
extern unsigned char page_flags[];
extern struct menu pagemenu;
extern struct menu scrollmenu;
extern unsigned char view_flags[];
extern struct menu viewmenu;
extern unsigned char edit_flags[];
extern struct menu editmenu;
extern const char *bm_items[3];
extern unsigned char bm_flags[3];
extern char bm_toggle_label[24];
extern char bm_show_label[20];
extern struct menu bmarkmenu;
extern struct menubar menubar;
void update_title(int id);
extern struct dmg_state g_dmg;

#endif
