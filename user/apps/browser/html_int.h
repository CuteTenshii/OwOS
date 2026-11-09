/* Private declarations shared by the html*.c renderer modules. */
#ifndef HTML_INT_H
#define HTML_INT_H

#include <stdbool.h>

#include "browser.h"
#include "css.h"
#include "features/anim/anim.h" /* CSS @keyframes / transition engine */
#include "features/canvas/canvas.h"
#include "features/media/bmedia.h" /* <audio>/<video> element registration */
#include "features/svg/svg.h"
#include "font.h"
#include "imgcarry.h"
#include "imgdec.h" /* shared image decoder for url() background images */
#include "layout.h"

#define HTML_BASE_FONT 16

/* Current text style */
struct sty {
  unsigned fg;
  unsigned char bold, under, italic, strike, nowrap, ttx, smallcaps;
};

struct css_src {
  const char *text;
  int len;
  int url_i;
}; /* text -> inline; else url_i */

struct frame_doc {
  char url[URL_MAX];    /* absolute src - the cache key (with parent)   */
  char parent[URL_MAX]; /* the document that embeds this frame          */
  int verdict;          /* 0 = pending, else FRAME_OK/BLOCKED/ERROR     */
  char reason[96];      /* shown when blocked/failed                    */
  struct dom_node *doc; /* parsed sub-document (NULL until OK)          */
  css_sheet *sheet;     /* the sub-document's inline <style> rules      */
  struct frame_doc *next;
};

extern css_sheet *g_sheet;
extern struct dom_node *g_live_dom;
extern char g_cur_page_url[URL_MAX];
extern char g_base_url[URL_MAX];
extern int g_frame_depth;
extern bool g_link_nav;
extern struct sty sty_cur;

extern unsigned char W_size, W_center, W_mono, W_indent, W_pre;
extern unsigned char W_fontpx;
extern unsigned W_bg;
extern char W_listtype;
extern int W_olnum;
extern signed char W_listrev;
extern signed char W_listdef;
extern unsigned char W_lineheight;
extern signed char W_liststyle;
extern const unsigned char ua_h_px[6];
extern int W_n, W_run_nl;
extern bool W_pend_sp;

extern struct frame_doc *g_frames;
extern struct img_carry g_carry;
extern struct dom_node *g_focus;
extern bool g_has_controls;
extern int g_walk_depth;
extern unsigned g_walk_now;
extern int g_nbg;
extern int W_cur_link;
extern int g_nspans;

extern struct css_src *g_src;
extern int g_nsrc;
extern char **g_css_url;
extern int g_n_css_url;
extern const char *g_page_url;
extern res_batch_fn g_page_fetch;

static inline void free_style_cache(void *cache) { free(cache); }
extern unsigned g_shadow_style_gen;
css_sheet *sheet_for(const struct dom_node *el);
static inline unsigned style_key(void)
{
  return css_viewport_generation() + css_sheet_generation(g_sheet) + g_shadow_style_gen;
}
static inline unsigned char clamp_fontpx(int px) { return (unsigned char)(px < 8 ? 8 : (px > 120 ? 120 : px)); }

void csp_report(const char *what, const char *url, const char *reason);
bool csp_url_ok(int type, const char *abs, const char *what, char *out, int outcap);
void compute_style(struct dom_node *el, struct css_style *cs);
bool url_absolute(const char *s);
int cp1252_high(unsigned char b);
int read_char(const char **pp, const char *end);
unsigned char sty_byte(void);
void apply_ua_style(const char *name);
void apply_css(const struct css_style *cs);
bool is_block_tag(const char *name);
int decode_entity(const char *p, const char *end, int *adv);
void decode_entities(char *s);
int text_put(int n, char c, unsigned char a);
int span_begin(struct dom_node *el);
void span_end(int i);
int utf8_emit(int n, unsigned cp, unsigned char a);
bool cp_drop(unsigned cp);
void emit_text(const char *s);
void block_break(void);
void capture_title(struct dom_node *el);
int open_link(struct dom_node *el);
void close_link(void);
void handle_svg(struct dom_node *el, const struct css_style *cs);
void handle_img(struct dom_node *el, const struct css_style *cs);
void handle_canvas(struct dom_node *el, const struct css_style *cs);
void handle_media(struct dom_node *el);
void handle_favicon(struct dom_node *el);
void emit_lit(const char *s, unsigned char a);
struct frame_doc *frame_find(const char *url, const char *parent);
void frames_free(void);
void collect_inline_styles(struct dom_node *node, css_sheet *sheet, const char *base);
void handle_iframe(struct dom_node *el);
void emit_input(struct dom_node *el);
void emit_textarea(struct dom_node *el);
void emit_select(struct dom_node *el);
void emit_progress(struct dom_node *el);
void emit_meter(struct dom_node *el);
void walk(struct dom_node *node);
void src_link(const char *absurl);
void collect_styles(struct dom_node *node);
struct dom_node *find_tag(struct dom_node *node, const char *tag);
void scan_meta_csp(struct dom_node *node);
void scan_resource_hints(struct dom_node *root);
void load_faces(struct dom_node *root, const char *page_url, res_batch_fn fetch);
void bg_note_url(const char *raw_url);
void load_bg_images(const char *page_url, res_batch_fn fetch);
int bg_image_index(const char *raw_url, const char *page_url);

#endif
