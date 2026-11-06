/* Private shared state and helpers for the devtools_*.c modules. */
#ifndef DEVTOOLS_INT_H
#define DEVTOOLS_INT_H

#include "browser.h"
#include "color.h" /* color_parse: %c styling maps CSS colours to console spans */
#include "css.h"
#include "layout.h" /* struct lbox: computed box geometry for the Styles box model */
#include "mutex.h"
#include <stdbool.h>

enum { TAB_ELEMENTS, TAB_STYLES, TAB_CONSOLE, TAB_NETWORK, TAB_APP, N_TABS };
typedef char dt_panel_count_matches[N_TABS == DT_PANELS ? 1 : -1]; /* keep dt_view in sync */
#define DT_HDR 22                                                  /* tab-strip height at the panel top           */
/* A code view is monospace: rows use the fixed cell font via gfx_text_utf8,
 * which decodes UTF-8 (so accents render) while keeping one column per
 * character. */
#define ROW_H GLYPH_H /* monospace cell height (16)                    */
struct textblk {
  char *buf;
  int len, cap;
  int *off;
  int nlines, off_cap; /* off[i] = start of line i in buf */
  bool line_open;      /* a line has been started since the last '\n' */
};
#define DT_CRUMB_MAX 24
struct dt_crumb {
  int node_idx;
  char label[40];
};
#define CON_MAX 400
/* A `%c` style span: from byte `off` in the line's (marker-stripped) text,
 * apply these overrides until the next run. Parsed out of the JS_STYLE_ON/OFF
 * markers the engine embeds (see interp.h) when con_push stores the line. */
struct crun {
  int off;
  unsigned fg, bg;
  unsigned char has_fg, has_bg, bold;
};
struct conline {
  char *text;
  unsigned char level;
  struct crun *runs;
  int nruns;
};
#define DT_INPUT_H 24
#define DT_FBTN_W 46 /* console filter / clear button width */
#define NET_MAX 256
struct netinfo {
  int id, state, status, size, ms;
  char method[8];
  char ctype[80];
  char err[64];
  char *url;
  char *resp_hdr;
};
enum { NF_ALL, NF_DOC, NF_CSS, NF_JS, NF_IMG, NF_FONT, NF_OTHER, NF_N };
/* A conline `level` used only by net rows for the dim NET_PENDING colour —
 * never a console level (those are DT_LOG/WARN/ERROR), so the console filter is
 * unaffected. */
#define DT_NET_PENDING_LVL 3

extern mutex_t dt_lk;
extern bool dt_open;
extern volatile bool dt_dirty;
extern volatile bool dt_paint_only;
extern volatile bool dt_pick;
extern int dt_tab;
extern int dt_scroll[N_TABS];
extern int dt_sel;
extern struct textblk dt_dom;
extern struct textblk dt_sty;
extern struct textblk dt_app;
extern int *dt_dom_node;
extern int dt_dom_node_cap;
extern int *dt_collapsed, dt_collapsed_n, dt_collapsed_cap;
extern char dt_find[128];
extern struct textfield dt_find_tf;
extern bool dt_find_init, dt_find_focused;
extern struct dt_crumb dt_crumbs[DT_CRUMB_MAX];
extern int dt_ncrumbs;
extern int dt_con_filter;
extern char dt_eval_buf[256];
extern volatile bool dt_eval_pending;
extern char dt_input[256];
extern struct textfield dt_input_tf;
extern bool dt_input_focused;
extern int dt_nd_scroll;
extern int dt_net_filter;
extern int dt_net_sel;
extern struct conline dt_con[CON_MAX];
extern int dt_con_start, dt_con_count;
extern char *dt_con_pend;
extern int dt_con_pend_len, dt_con_pend_cap;
extern int dt_con_pend_level;
extern bool dt_con_follow;
extern struct conline dt_net[NET_MAX];
extern int dt_net_start, dt_net_count;
extern struct netinfo dt_netinfo[NET_MAX];
extern const char *const dt_net_filter_names[NF_N];

void dt_con_clear(void);
void dt_con_draw_runs(int x, int y, const char *s, int off, int end, unsigned deffg, const struct crun *runs,
                      int nruns);
int dt_con_visible_count(void);
void dt_con_free_runs(int slot);
void dt_netinfo_clear(struct netinfo *ni);
bool dt_net_pass_slot(int slot);
void dt_net_putsize(char *b, int *n, int cap, int size);
int dt_net_visible_count(void);
int dt_hdr_line(const char *h, int idx, char *out, int max);
int dt_hdr_nlines(const char *h);
void dt_app_emit(void);
unsigned dt_app_sig(void);
void dt_dom_emit(struct textblk *t, struct dom_node *n, int depth, int *idx, int suppress);
int dt_dom_index_of(struct dom_node *n, struct dom_node *target, int *idx);
struct dom_node *dt_dom_node_at(struct dom_node *n, int target, int *idx);
void dt_style_emit(struct dom_node *root);
int dt_content_rows(int top);
bool dt_is_collapsed(int idx);
void dt_toggle_collapsed(int idx);
int dt_tb_line(const struct textblk *t, int i, char *out, int max);
void dt_tb_newline(struct textblk *t);
void dt_tb_putc(struct textblk *t, char c);
void dt_tb_puts(struct textblk *t, const char *s);
void dt_tb_reset(struct textblk *t);
bool dt_con_pass(int level);
void dt_hist_add(const char *s);
void dt_hist_recall(int dir);
void dt_input_ensure(void);

#endif
