/* browser.h: shared state and helpers for the web browser (html.c, fetch.c, main.c). */
#ifndef OWOS_BROWSER_H
#define OWOS_BROWSER_H

#include <stdbool.h>

#include "dom.h"
#include "features/csp/csp.h"
#include "features/permissions/permissions.h"
#include "gfx.h"
#include "h2.h"
#include "inflate.h"
#include "netconn.h"
#include "owos.h"
#include "png.h"
#include "url.h"

/* Canonical User-Agent shared by the HTTP transport and the JS navigator; resolved once. */
const char *browser_ua(void);
const char *browser_ua_version(void); /* browser_ua() minus "Mozilla/" (navigator.appVersion) */

extern int BW, BH; /* client size (mutable: the window is resizable)    */
extern bool g_fullscreen; /* the window covers the screen: no browser chrome  */
#define MENU_H (g_fullscreen ? 0 : 18) /* top app-menu bar height (above the address bar)  */
#define TAB_H (g_fullscreen ? 0 : 24)  /* tab strip height (between menu and address bar)  */
#define ADDR_H (g_fullscreen ? 0 : 26) /* address-bar height                               */
#define GLYPH_W 8
#define GLYPH_H 16
#define SCROLL_W 10                 /* right-edge scrollbar width                       */
#define PAGE_SCROLL_W (B->pg->sb_w) /* the page pane's scrollbar: SCROLL_W unless the page restyles it */

/* Initial heap sizes of the growable buffers (grow ~1.5x, never shrink). */
#define RAW_INIT (256 * 1024)  /* captured response body / one image     */
#define TEXT_INIT (128 * 1024) /* rendered styled text (+ equal attr)    */
#define LINES_INIT 4096        /* word-wrapped display lines             */
#define LINKS_INIT 128         /* extracted <a href> links               */
#define IMGS_INIT 16           /* inline <img> elements                  */
/* img_pool is a fixed slab: image fetch threads copy into it without the lock, so it must not move. */
#define IMG_POOL (16 * 1024 * 1024)
/* Per-fetch cap on ONE inline image body; the page body itself is unbounded. */
#define RAW_MAX (32 * 1024 * 1024)
/* Must hold the whole response header block, or a late content-encoding is lost. */
#define RESP_HDR_MAX 16384
/* Address-bar/URL buffer size; shared fixed-size APIs (gfx textfield, resolve_url) prevent growth. */
#define URL_MAX 2048
#define POST_BODY_MAX 8192 /* cap on a form/POST request body (see struct form_submission) */
#define MAX_REDIR 5
#define FAVICON_W 16 /* favicon decoded to a fixed 16x16 ARGB bitmap and */
#define FAVICON_H 16 /* shown in the page's tab (0xAARRGGBB per pixel)   */

#define ST_COLOR 0x0F    /* legacy; color is now full 0xRRGGBB (B->fg) */
#define ST_NOWRAP 0x01   /* white-space:nowrap: this glyph's line must not wrap */
#define ST_ELLIPSIS 0x04 /* text-overflow: ellipsis on a clipping block */
#define ST_CLIP 0x08     /* overflow:hidden block: text past the column edge is not drawn */
#define ST_HIDDEN 0x02   /* visibility:hidden: takes space, draws nothing */
#define ST_BOLD 0x10
#define ST_UNDER 0x20
#define ST_ITALIC 0x40 /* faux-italic shear */
#define ST_STRIKE 0x80 /* line-through (del/s/strike, text-decoration) */

/* B->bg[i] == BG_NONE means no background (real colours leave the high byte free). */
#define BG_NONE 0xFF000000u

/* Per-glyph block-layout byte (B->bp): set by the html walk, applied by the renderer. */
#define BP_SIZE 0x07 /* 0 = body, 1..6 = h1..h6, 7 = horizontal rule */
#define BP_CENTER 0x08
#define BP_MONO 0x10
#define BP_INDENT 0xE0
#define BP_INDENT_SH 5

/* How the fetched body is rendered, picked from the response Content-Type. */
enum render_mode { MODE_HTML, MODE_TEXT, MODE_IMAGE };

/* A stored cookie; fields are heap-allocated, the jar is a linked list (B->cookies). */
struct cookie {
  char *host;
  char *name;
  char *val;
  struct cookie *next;
};
/* A host pinned to HTTPS by Strict-Transport-Security. */
struct hsts_host {
  char *host;
  struct hsts_host *next;
};
struct link {
  int start, end;
  char href[URL_MAX];
  signed char newtab;
};

/* Explicit cursor, user-select and pointer-events of an element, over its text range; -1 = not set there. */
struct pregion {
  int start, end;
  signed char cursor, usel, pev;
  int sel; /* ::selection background 0xRRGGBB, -1 = not set here */
};

/* An external stylesheet URL from the response's Link: header (absolute). */
struct css_link {
  char *url;
  struct css_link *next;
};

/* Batch-fetch n absolute URLs into bodies[i] (malloc'd, caller frees) + lens[i]. */
typedef void (*res_batch_fn)(char *const *urls, int n, unsigned char **bodies, int *lens);

/* An inline <img>, anchored at a text offset and blitted decode-on-scroll. */
struct inl_img {
  char href[URL_MAX]; /* src, resolved against the page URL            */
  char alt[48];       /* alt text, shown when the image can't be drawn */
  int text_pos;       /* offset into B->text where it is anchored      */
  int start_row;      /* first display row (set by rewrap)             */
  int rows;           /* reserved display rows (set by rewrap)         */
  int ok;             /* 1 = img_pool holds a decodable PNG            */
  unsigned iw, ih;    /* natural pixel dimensions                      */
  int coff, clen;     /* its PNG bytes within img_pool                 */
  int lazy;           /* 1 = loading="lazy": fetch only when in view   */
  int want;           /* UI flag: a lazy image is now scrolled in      */
  int fetched;        /* 1 = a fetch has been attempted (ok = result)  */
  int cw, ch;         /* width/height from the attr or CSS, px (0=auto) */
  int is_argb;        /* 1 = img_pool holds raw RGBA (SVG), not PNG    */
  int uid;            /* session-unique id once landed (0 = none), keys the pixel cache */
  int fit;            /* object-fit: enum css_objfit */
  int hidden;         /* visibility:hidden: reserve the space, draw nothing */
  struct {            /* transform on the element: the map is drawn after the image is painted */
    bool on;
    int m[4];               /* Q12 a b c d */
    short tx, ty, txp, typ; /* translation: px and percent of the image box */
    short ox, oy, oxp, oyp; /* transform-origin: px and percent of the image box */
  } xf;
  short fbright;              /* filter brightness permille, 1000 = none */
  signed char fgray, finvert; /* filter grayscale / invert percent */
};

/* 32 random hex digits, the shape Chrome gives target and frame ids */
void make_unique_id(char out[33]);
struct page;
/* For threads other than the UI: visit every tab's page, or test an id, with the tab table locked */
void tabs_each_page(void (*fn)(const struct page *pg, bool active, void *ctx), void *ctx);
bool tabs_has_target(const char *id);

/* One tab's rendered document; B->pg points at the active tab's. */
struct page {
  char target_id[33]; /* CDP target and main-frame id, fixed for the tab's life */
  char loader_id[33];
  char doc_title[128];
  char ctype[80]; /* lowercased response Content-Type      */
  int mode;       /* enum render_mode for the current body */
  char cur_url[URL_MAX];

  /* Heap-backed; text/attr/bp/fg/bg share one capacity. fg/bg are 0xRRGGBB (bg == BG_NONE = none). */
  char *raw;
  int raw_len, raw_cap;
  char *text;
  unsigned char *attr;
  int text_len, text_cap;
  unsigned char *bp;
  unsigned char *align;         /* per-glyph text alignment: 0 left, 1 center, 2 right, 3 justify */
  unsigned char *fsz;           /* per-glyph CSS font-size px (0 = default body font) */
  unsigned char *lh;            /* per-glyph CSS line-height px (0 = font-derived default) */
  signed char *ls;              /* per-glyph letter-spacing px */
  signed char *va;              /* per-glyph vertical-align shift, px down */
  unsigned sid;                 /* sessionStorage scope: assigned on first use, dies with the tab */
  int sb_w;                     /* page scrollbar width, px (0 = hidden); SCROLL_W unless the page restyles it */
  struct scrollbar_style sb_st; /* its colours, thumb radius and arrows */
  bool has_ls;                  /* some glyph has letter-spacing (the layout signature must see it) */
  unsigned *fg;
  unsigned *bg;
  /* Page canvas colours from the <html>/<body> cascade. */
  unsigned page_bg, page_fg;

  struct link *links;
  int nlinks, links_cap;
  struct pregion *regs; /* children before their parents, so the first containing entry is the innermost */
  int nregs, regs_cap;
  struct inl_img *imgs;
  int n_imgs, imgs_cap; /* inline <img> list */

  /* Decoded url() background images; rgba is iw*ih straight RGBA, or NULL on failure. */
  struct bg_img {
    char url[URL_MAX];
    unsigned iw, ih;
    unsigned char *rgba;
  } *bgimgs;
  int n_bgimgs, bgimgs_cap;
  int img_uid_next; /* monotonic id source for inl_img.uid (never reused) */
  unsigned char *img_save;
  int img_save_len; /* backgrounded image bytes */

  /* favicon_pri ranks the discovered <link rel=icon>; favicon[] is 16x16 ARGB. */
  char favicon_href[URL_MAX];
  int favicon_pri;
  unsigned favicon[FAVICON_W * FAVICON_H];
  int favicon_ok;

  int scroll; /* vertical scroll position, document pixels */

  struct csp *csp;       /* the page's Content-Security-Policy set    */
  unsigned perm_policy;  /* Permissions-Policy: bitmap of blocked features (permissions.h) */
  char *perm_policy_hdr; /* the Permissions-Policy header text, for Client Hints delegation */

  /* @font-face faces; body_font is the face the body text uses, or NULL for the UI font. */
  struct loaded_face {
    char family[48];
    struct font *font;
    unsigned char *data;
  } *faces;
  int n_faces, faces_cap;
  struct font *body_font;
};

/* One wrapped text run of a flex-row item, tagged with its absolute band row and column. */
struct fslice {
  int row, off, len, x, w, h, maxpx;
};

struct browser {
  struct page *pg; /* the active tab's page (tabs[active_tab]->page) */

  char addr[URL_MAX];       /* address-bar buffer (owned by addr_tf)       */
  struct textfield addr_tf; /* editable address-bar field (caret + scroll) */
  char req_url[URL_MAX];
  /* Queued navigation method/body; empty req_method means GET. Worker-owned, no lock. */
  char req_method[8];
  char *req_body; /* malloc'd request body, replaced (and freed) by the next POST */
  int req_body_len;
  char req_ctype[80];
  int req_pending;
  int loading;
  volatile int abort_load;      /* main -> worker: cancel the in-flight fetch */
  volatile int nav_cancel;      /* worker -> main: the user stayed on the page, drop the pending navigation */
  volatile int scripts_pending; /* worker: page painted, run its <script>s on the next pass */
  volatile int in_scripts;      /* worker is running the deferred script pass; load_quiesce waits */

  bool nosniff;      /* X-Content-Type-Options: nosniff       */
  char dl_name[128]; /* filename from Content-Disposition     */
  int cdisp_attach;  /* 1 = Content-Disposition: attachment   */
  int dl_saved;      /* set when http_txn streamed a download */

  /* Inline-image scratch for the active load; fixed slab so the base never moves. */
  unsigned char img_pool[IMG_POOL];
  int img_pool_len;

  /* The worker builds the next layout aside and swaps it in under browser_buf_lock. */
  int wrap_cols;
  int layout_dirty; /* 1 = box tree/text changed, re-wrap needed (NOT bumped by scroll) */

  int sb_drag;
  unsigned seq;
  unsigned layout_seq; /* reflow generation, bumped whenever rewrap re-flows the line table */

  /* Content-pane interaction handed from the main thread to the worker for JS dispatch. */
  volatile int evt_pending;    /* 1 = an event is queued for the worker      */
  int evt_off;                 /* text offset the click resolved to          */
  int evt_x, evt_y;            /* viewport-relative coords (event.clientX/Y)  */
  char evt_type[16];           /* "click", "input", "change", ...            */
  char evt_text[URL_MAX];      /* link href for click default, or new field value for input/change */
  volatile int drop_dom;       /* main asks the worker to free the live page  */
  volatile int drop_dom_tab;   /* tab that owned the DOM being torn down, for fault blame */
  volatile int worker_pause;   /* main -> worker: park so the page set can be mutated */
  volatile int worker_idle;    /* worker -> main: parked, safe to mutate the set */
  volatile int recascade;      /* main asks the worker to re-run the cascade (@media)  */
  volatile unsigned resize_ms; /* main: msec() of the latest resize, so the worker cascades once it settles */
  volatile int want_paw;       /* main -> worker: content-pane pixel width; the worker owns rewrap */
  volatile int events_live;    /* worker -> main: page has listeners, route clicks as events   */
  volatile int ptr_live;       /* worker -> main: a rule reads :hover/:active, so report the pointer */
  volatile int ptr_pending;    /* main -> worker: ptr_off/ptr_down changed                   */
  volatile int ptr_off;        /* text offset under the pointer, -1 none                    */
  volatile int ptr_down;       /* the button is held                                         */
  volatile int focus_kind;     /* worker -> main: 0 none, 1 text input, 2 textarea */

  /* History API: the main thread owns the tabs[] hist list and nav buttons. */
  volatile int fs_want;   /* worker -> main: 1 = fill the screen with the window, 0 = restore */
  struct dom_node *evt_node; /* main -> worker: the element a "fsenter" or "cast" event is about */
  volatile int hist_go;   /* worker -> main: -1 back, +1 forward, 0 none */
  volatile int hist_nav;  /* worker -> main: a pushState/replaceState URL */
  int hist_replace;       /* the queued hist_nav replaces, not pushes     */
  char hist_url[URL_MAX]; /* its URL                                      */
  int hist_key;           /* its state key (history.state lookup on popstate) */
  int hist_len;           /* main -> worker: history.length               */
  volatile int loc_nav;   /* worker -> main: location.assign/replace/reload */
  char loc_url[URL_MAX];  /* its target URL                               */
  char referrer[URL_MAX]; /* the page navigated away from (document.referrer) */

  /* window.alert/confirm/prompt: the worker posts dlg_req and waits on dlg_done; main runs the dialog. */
  volatile int dlg_req;  /* worker -> main: 0 none, 1 alert, 2 confirm, 3 prompt,
                            4 permission, 5 list (dlg_text: one item per line),
                            6 file picker (dlg_pick flags, dlg_msg = accept, answer in dlg_paths) */
  volatile int dlg_done; /* main -> worker: the dialog closed */
  int dlg_ok;            /* main -> worker: confirm/prompt accepted (OK/Yes); permission:
                            chosen button index (0=Refuse,1=Once,2=Always); list: chosen row, -1 if cancelled */
  char dlg_title[64];
  char dlg_msg[512];
  char dlg_text[1024];   /* prompt: default text in, entered text out; list: the items */
  int dlg_pick;          /* file picker: PICK_MULTIPLE | PICK_DIRECTORY */
  char dlg_paths[16384]; /* file picker answer: NUL-terminated paths, ended by an empty string; dlg_ok = count */

  int win_id; /* the window handle (for title updates) */

  struct cookie *cookies;   /* head of the heap cookie jar (no cap)   */
  struct hsts_host *hsts;   /* head of the HSTS host list (no cap)    */
  struct css_link *ext_css; /* stylesheet URLs from the Link: header   */

  netconn *sock; /* the page fetch transport (TCP or user-space TLS) */
  unsigned char rbuf[2048];
  int rpos, rlen, reof;
};

extern struct browser *B;
extern const unsigned palette[16];

static inline void sapp(char *d, int *n, int cap, const char *s)
{
  while (*s && *n < cap - 1)
    d[(*n)++] = *s++;
  d[*n] = '\0';
}
static inline void uapp(char *d, int *n, int cap, unsigned v)
{
  char t[12];
  int k = 0;
  if (v == 0) {
    if (*n < cap - 1)
      d[(*n)++] = '0';
    d[*n] = '\0';
    return;
  }
  while (v) {
    t[k++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (k && *n < cap - 1)
    d[(*n)++] = t[--k];
  d[*n] = '\0';
}

/* buffers.c: browser_ensure_*(need) grow a B buffer to hold `need` elements; 0 = heap exhausted.
 * Only a realloc that moves a buffer is unsafe, so growth and readers (render, hit-test) take
 * browser_buf_lock. The line table is double-buffered in main.c instead. */
int browser_ensure_raw(int need);
int browser_grow_cap(int cap, int init, int need);
int browser_ensure_text(int need);
int browser_ensure_links(int need);
int browser_ensure_regs(int need);
int browser_ensure_imgs(int need);
unsigned *browser_surface(void);
void browser_surface_redirect(
    unsigned *to); /* paint into `to` (BW x BH) instead of the window surface; NULL undoes it */
int browser_surface_ready(void);
void browser_present(int id);
void browser_blit_rect(int x, int y, int w, int h);
void browser_buf_lock(void);
void browser_buf_unlock(void);

/* Lowest safe native stack address for JS recursion on the worker (defined in main.c). */
void *browser_js_stack_floor(void);

/* Render B->raw as HTML into the styled-text buffers; `fetch` (may be NULL) batch-pulls stylesheets. */
void html_to_text(const char *page_url, res_batch_fn fetch);
char *document_adopted_css(void); /* same, for document.adoptedStyleSheets */
char *
shadow_adopted_css(const struct dom_node *root); /* malloc'd text of a shadow root's adoptedStyleSheets, or NULL */
void html_run_page_scripts(void);                /* deferred <script> pass; run after first paint */
void html_set_charset(const char *content_type); /* charset from HTTP CT */

/* js.c: run every inline <script> in `root` against a fresh JS context. */
void js_run_document(struct dom_node *root, const char *page_url, res_batch_fn fetch);
void js_page_end(void);       /* reclaim the page ctx  */
void js_page_interrupt(void); /* any thread: unwind the running page script (cleared when the next ctx starts) */
int js_dispatch_dom_event(struct dom_node *target, const char *type, int x, int y);
int js_has_listeners(void); /* any listener registered */
int js_before_unload(void); /* worker: fire beforeunload; 0 if the user chose to stay */
void js_history_pop(int key, const char *url); /* worker: same-document traversal; fire popstate */
void js_page_hide(void);    /* worker: page is going away; fire visibilitychange (hidden) */
bool js_escape_key(void);   /* Escape: leave fullscreen, else close the topmost modal dialog or auto popover */
bool js_fullscreen_enter(struct dom_node *n); /* worker: make `n` the fullscreen element; false if not in the document */
bool js_dialog_form_submit(struct dom_node *form, struct dom_node *submitter); /* method=dialog: close its dialog */
int js_resize_pump(void);        /* report ResizeObserver size changes; -> # callbacks fired */
int js_intersection_pump(void);  /* report IntersectionObserver changes; -> # callbacks fired */
int js_timers_pump(void);        /* fire due setTimeout/Interval/rAF; -> # callbacks fired */
int js_workers_pump(void);       /* deliver queued Worker postMessage traffic */
int js_websockets_pump(void);    /* drain live WebSockets -> open/message/close/error events */
int js_notifications_pump(void); /* deliver deferred Notification show/close events */
int js_cast_pump(void);          /* deliver Remote Playback availability + connect/disconnect */
int js_idb_pump(void);           /* deliver IndexedDB request events + commit/abort transactions */
/* Run one devtools-console REPL command against the live page. Worker-thread only. */
void js_repl_run(const char *src);
/* Evaluate src in the live page context, without echoing to the console: the result as a malloc'd string,
 * or the exception's "Name: message" with *threw set. NULL when no page script context exists. */
char *js_page_eval(const char *src, int *threw);

/* html.c: tear down the live page, re-flatten the live DOM, map a text offset to a DOM node. */
void html_page_teardown(void);
void html_relayout(void);
bool html_set_pointer(int off, bool pressed); /* true when :hover/:active restyles the page */
struct dom_node *html_node_at_offset(int off);
/* Inverse: the [start,end) text range a node covers, 0 if never laid out. Worker-thread only. */
int html_node_span(struct dom_node *node, int *start, int *end);

/* Approximate document-relative px box of a text range (0 if empty); takes browser_buf_lock. */
int browser_range_box(int start, int end, int *x, int *y, int *w, int *h);
int browser_scroll_px(void);              /* page vertical scroll, px (window.scrollY) */
int browser_doc_height_px(void);          /* laid-out document height, px */
int js_resize_pump(void);                 /* report ResizeObserver size changes; -> # callbacks fired */
void browser_viewport_px(int *w, int *h); /* content viewport size (innerWidth/Height) */

/* Form controls; the worker performs a clicked control's default action. */
enum ctrl_kind {
  CTRL_NONE,
  CTRL_TEXT,
  CTRL_TEXTAREA,
  CTRL_CHECKBOX,
  CTRL_RADIO,
  CTRL_SELECT,
  CTRL_SUBMIT,
  CTRL_BUTTON,
  CTRL_FILE
};
#define PICK_MULTIPLE 1
#define PICK_DIRECTORY 2
/* Ask the main thread for files (blocks like alert); out gets NUL-terminated paths ended by an empty string.
 * Returns the count, 0 if cancelled. */
int worker_pick(int flags, const char *accept, char *out, int outsz);
int html_control_kind(struct dom_node *node);
void html_set_focus(struct dom_node *node); /* NULL clears focus */
struct dom_node *html_focus(void);
int html_has_controls(void); /* page has form controls to click */
void html_toggle_checked(struct dom_node *c);
void html_radio_select(struct dom_node *c);
void html_select_cycle(struct dom_node *sel);
void html_seed_textarea(struct dom_node *ta);
struct dom_node *html_focus_edit(int ch); /* edit focused field; returns it */
#define EDIT_SELECT_ALL 0x100             /* html_focus_command: select the whole value */
int html_focus_command(int cmd);          /* EDIT_SELECT_ALL or KEY_INTR (copy); 1 = relayout */

/* A resolved form submission; a GET folds the fields into url's query and leaves body empty. */
struct form_submission {
  char url[URL_MAX];
  char method[8]; /* "GET" or "POST" */
  char *body;     /* malloc'd request body for POST (NULL for GET); the caller frees or hands it on */
  int body_len;
  char ctype[80]; /* Content-Type for the POST body */
};
int html_form_build(struct dom_node *control, struct form_submission *fs);
/* Pick files for an <input type=file> (honouring multiple/webkitdirectory/accept) and store the selection;
 * 1 if the selection changed. Worker only. */
int html_file_choose(struct dom_node *input);
void js_files_changed(struct dom_node *input); /* drop the cached input.files */
/* Pick files for the input, then fire input and change on it. Worker only. */
void browser_file_input_activate(struct dom_node *input);
/* Submit the form enclosing `node`, optionally firing 'submit' first. Returns 1 if found. Worker only. */
int browser_form_submit(struct dom_node *node, int fire_event);

void plain_to_text(void); /* html.c */
void doc_message(const char *m);
void show_error(const char *m); /* fetch.c */

/* jar.c: persistent cookie + HSTS store backed by SQLite (user/lib/sqlite). */
void jar_load(void);  /* fill B->cookies + B->hsts at boot */
void jar_save(void);  /* serialise the whole db now        */
void jar_flush(void); /* save once, only if it changed     */
void cookie_store(const char *host, const char *setcookie_value);
int cookie_header(const char *host, char *out, int cap); /* "a=b; c=d" -> len */
void hsts_add(const char *host);
int hsts_has(const char *host);

/* devtools.c: bottom-docked panel (Elements, Styles, Console). Snapshots are built on the worker. */
enum { DT_LOG, DT_WARN, DT_ERROR };
void devtools_toggle(void);
int devtools_visible(void);
/* Inspect-element picker: main feeds hover/click pane offsets, the worker resolves the element. */
void devtools_pick_toggle(void);
int devtools_picking(void);
void devtools_pick_hover(int off);         /* pointer at pane offset (-1 = left the pane) */
void devtools_pick_commit(int off);        /* click at pane offset: select + end picking  */
int devtools_top(void);                    /* panel's top y (main thread)                */
int devtools_left(void);                   /* panel's left x; BW if hidden (right-dock)  */
void devtools_render(void);                /* main thread: draw the panel                */
int devtools_take_paint(void);             /* main: 1 = repaint only the dock (log line)  */
int devtools_handle_click(int mx, int my); /* 1 = consumed                    */
/* textfield_click that also arms drag-selection in that field for the held button */
void drag_field(struct textfield *tf, int mx, int my, int x, int y, int w, int h);
int devtools_handle_scroll(int delta);                                        /* 1 = consumed                    */
int field_key(struct textfield *tf, char c, int mods);                        /* textfield_key plus Ctrl+X/C/V */
int devtools_field_at(int mx, int my, struct textfield **tf, int *x, int *w); /* focuses it; 1 = over a field */
int devtools_handle_key(char c, int mods);                                    /* Console REPL input; 1 = consumed */
/* Selected element's overlay box (DOCUMENT px) + label, 0 if none. */
int devtools_highlight(int *x, int *y, int *w, int *h, const char **label);
int devtools_on_splitter(int mx, int my);                        /* 1 = pointer is on the resize edge */
void devtools_set_width(int w);                                  /* drag-resize the docked panel      */
void devtools_console_append(int level, const char *s, int len); /* worker */
void devtools_worker_tick(void);                                 /* worker: rebuild DOM/Styles snapshot if open */
/* Network tab entry. Strings are borrowed; `ms` is the duration (0 = unknown). */
/* net_log.state; NET_DONE must be 0 so positional initializers default to a finished request. */
enum { NET_DONE, NET_PENDING, NET_FAILED, NET_CANCELLED };
struct net_log {
  const char *method;
  const char *url;
  int status;
  const char *ctype;
  int size;
  int ms;
  const char *resp_hdr;
  int state;          /* NET_* */
  const char *reason; /* NET_FAILED failure text, else NULL */
  /* Optional, for remote DevTools; initialisers that stop before these leave them unset */
  const char *req_hdr; /* the request line + header block as sent, or NULL */
  const void *body;    /* the decoded response body, or NULL */
  int body_len;
  int from_cache; /* served from the HTTP cache, so no request was sent */
};
/* _begin logs an in-flight row and returns an id, _update resolves it, _add is the one-shot form. */
int devtools_net_begin(const char *method, const char *url);
void devtools_net_update(int id, const struct net_log *e);
void devtools_net_add(const struct net_log *e);
void devtools_nav_reset(void); /* new top-level navigation: clear the net log and the console */

/* Per-tab devtools view state; `cap` is an opaque capture handle owned by devtools.c. */
#define DT_PANELS 5
struct dt_view {
  int open;
  int tab;
  int scroll[DT_PANELS];
  void *cap;
};
void devtools_view_save(struct dt_view *v);       /* active view+capture -> v       */
void devtools_view_load(const struct dt_view *v); /* v -> active, restoring capture */
void devtools_view_free(struct dt_view *v);       /* free a closed tab's capture    */
void devtools_reset_live(void);                   /* drop the live (statics) capture */

/* html.c hooks for the devtools snapshot (worker-thread only). */
struct css_style;
struct css_match;
struct dom_node *html_dom_root(void);
void html_compute_style(struct dom_node *el, struct css_style *out);
int html_match_styles(struct dom_node *el, struct css_match *out, int cap);
int html_has_pseudo(struct dom_node *el, int which); /* ::before/::after present? */

void do_fetch(void);

/* fetch() `cache` request modes */
enum http_cache_mode { HC_DEFAULT = 0, HC_NO_STORE, HC_RELOAD, HC_NO_CACHE, HC_FORCE_CACHE, HC_ONLY_IF_CACHED };

/* One HTTP request; only `url` is required. `extra_hdrs` is a "Name: value\r\n" blob. */
struct http_req {
  const char *url;
  const char *method;
  const unsigned char *body;
  int body_len;
  const char *ctype; /* request Content-Type */
  const char *accept;
  const char *extra_hdrs;
  int no_cookies;
  int cache_mode; /* enum http_cache_mode */
};

/* Optional out buffers (`final_url`, `hdr`) are filled only when pointer + cap are given. */
struct http_resp {
  int status;      /* out: HTTP status, 0 on transport failure */
  int len;         /* out: body length                         */
  char ctype[160]; /* out: response Content-Type               */
  char *final_url;
  int final_cap; /* out (opt): final URL after redirects     */
  char *hdr;
  int hdr_cap;    /* out (opt): raw response header block      */
  int from_cache; /* out: served from the HTTP cache, no request sent */
};

/* Single request following redirects; returns the body (caller frees) for any status, NULL on error. */
unsigned char *browser_fetch(const struct http_req *rq, struct http_resp *rs);

/* Stream a media resource to `file` via an open-ended Range request. Returns bytes written
 * on 2xx (*status set), else -1. Worker/loader-thread only. */
long browser_fetch_media(const char *url, const char *accept, const char *file, int *status);

/* CORS-aware fetch()/XHR transport (Origin, preflight, Allow-* check). NULL on error or denial. */
unsigned char *browser_cors_fetch(const char *page_url, int with_credentials, const struct http_req *rq,
                                  struct http_resp *rs);

/* blob: object URLs (blobstore.c): thread-safe byte copies; get returns a malloc'd body or NULL. */
bool blobstore_put(const char *url, const unsigned char *data, int len, const char *type);
void blobstore_del(const char *url);
void blobstore_clear(void);
unsigned char *blobstore_get(const char *url, int *len, char *ctype, int cap);

/* Attribute subsequent sub-resource loads to `page_url`'s origin for CORP. */
void resource_set_page(const char *page_url);
/* Client Hints headers for a request to *u: a sub-resource of the current page, or a top-level navigation. */
int ch_headers_for(const struct purl *u, bool subresource, char *out, int cap);
void fetch_inline_images(const char *page_url); /* re-pull inline <img> after relayout */
int fetch_lazy_visible(void);                   /* worker: load one in-view loading=lazy <img> */

/* iframes: html.c caches each frame's sub-document by src; resource.c fetches it and checks
 * X-Frame-Options / CSP frame-ancestors. */
enum { FRAME_OK = 1, FRAME_BLOCKED = 2, FRAME_ERROR = 3 }; /* embedding verdict */
int html_iframes_pending(void);                            /* worker: any frame still needs fetching?  */
/* worker: copy the next unfetched frame's src + embedder URL out; 1 if found. */
int html_iframe_next_pending(char *url, int url_cap, char *parent, int parent_cap);
/* worker: hand a fetched frame's result back; `verdict` is FRAME_*, `reason` shown if blocked/failed. */
void html_iframe_resolve(const char *url, const char *parent, int verdict, const char *reason,
                         const unsigned char *body, int len, const char *ctype);
void fetch_iframes(void); /* resource.c: drain + fetch the pending set */
/* Commit in-memory image bytes into slot idx; SVG is rasterized with `current_color` (0xRRGGBB). */
void resource_commit_inline(int idx, const unsigned char *body, int len, unsigned current_color);
int resource_commit_rgba(int idx, const unsigned char *rgba, unsigned w, unsigned h);

#endif /* OWOS_BROWSER_H */
