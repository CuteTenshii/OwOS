/* browser.h — shared state and helpers for the web browser.
 *
 * The app is split across:
 *   html.c   URL parsing/resolution + HTML -> styled text + link extraction
 *   fetch.c  the HTTP/HTTPS transport: buffered reader, cookies, redirects
 *   main.c   the windowed UI: draw, word-wrap, input, and the event loop
 *
 * It runs entirely in user space over the windowing + net syscalls. The
 * content-volume buffers (the response body, the rendered text/attributes, the
 * word-wrap line table, the inline-image store, and the link/image lists) live
 * on the user-space heap and grow on demand — there is no fixed page/image size
 * cap, only available memory. The small fixed-shape state (the address bar, the
 * cookie jar, the favicon bitmap, the socket reader) stays inline in `struct
 * browser`. See browser_ensure_*() in buffers.c for the grow helpers and browser_buf_lock() for
 * the realloc/render interlock.
 */
#ifndef OWOS_BROWSER_H
#define OWOS_BROWSER_H

#include <stdbool.h>

#include "dom.h"                              /* DOM tree + the shared lower/starts_ci/streq_ci helpers  */
#include "features/csp/csp.h"                 /* Content-Security-Policy engine (per-page enforcement)   */
#include "features/permissions/permissions.h" /* per-origin permission store + Permissions-Policy    */
#include "gfx.h"
#include "h2.h" /* HTTP/2 engine: h2_run + struct h2_opts (TLS+ALPN path) */
#include "inflate.h"
#include "netconn.h" /* TCP-or-TLS transport wrapper (h2/h1.1 over user-space TLS) */
#include "owos.h"
#include "upng.h"
#include "url.h" /* struct purl, parse_url, resolve_url (shared with curl) */

/* One canonical User-Agent, shared by the HTTP transport and the JS navigator
 * (userAgent/appVersion), so the string a site sees over the wire and the one
 * it reads from script never disagree. It keeps OwOS's own identity but carries
 * a well-formed WebKit/Chrome/Safari token chain so UA parsers that gate on a
 * known engine (Cloudflare et al.) don't reject it as malformed. The machine
 * arch comes from uname(2) and the version from OWOS_VERSION — neither is
 * hardcoded; both are resolved once at first use. browser_ua() is the full
 * "Mozilla/..." string; browser_ua_version() is it minus "Mozilla/"
 * (navigator.appVersion). */
const char *browser_ua(void);
const char *browser_ua_version(void);

extern int BW, BH; /* client size (mutable: the window is resizable)    */
#define MENU_H 18  /* top app-menu bar height (above the address bar)  */
#define TAB_H 24   /* tab strip height (between menu and address bar)  */
#define ADDR_H 26  /* address-bar height                               */
#define GLYPH_W 8
#define GLYPH_H 16
#define SCROLL_W 10 /* right-edge scrollbar width                       */

/* Initial heap reservations for the growable buffers. Each one starts at the
 * size below and grows (~1.5x, never shrinking) whenever an append would
 * overflow it, so a page body, the rendered text, the inline-image store, and
 * the link/image lists are bounded only by available memory — not a fixed cap.
 * The numbers are just "fits the common case without a realloc". */
#define RAW_INIT (256 * 1024)  /* captured response body / one image     */
#define TEXT_INIT (128 * 1024) /* rendered styled text (+ equal attr)    */
#define LINES_INIT 4096        /* word-wrapped display lines             */
#define LINKS_INIT 128         /* extracted <a href> links               */
#define IMGS_INIT 16           /* inline <img> elements                  */
/* img_pool stays a single fixed slab, not a growable buffer: the inline-image
 * fetchers run on several threads at once (fetch.c), each reserving a disjoint
 * region under a short lock and then copying into it WITHOUT the lock — which
 * is only safe while the base address never moves. So this one keeps a hard
 * cap. */
#define IMG_POOL                                                                                                       \
  (16 * 1024 * 1024) /* shared store for inline images' bytes                                                          \
                      */
/* Per-fetch sanity cap on ONE inline image's body (fetch.c's image fetchers
 * malloc a doubling buffer up to this). The page body itself is unbounded (it
 * grows B->raw); this only stops a single runaway sub-resource. */
#define RAW_MAX (32 * 1024 * 1024) /* a big page decompresses to several MB */
/* Buffer for a response's full header block (the synthesized "HTTP/x NNN\r\n…"
 * text both transports hand to parse_response). Must hold every header, not
 * just the first few KiB: a large CSP or several Set-Cookie can push
 * content-encoding past a stingy cap, and a lost content-encoding renders the
 * compressed body raw (e.g. github.com's CSP shoved "content-encoding: gzip"
 * past the old 4 KiB). */
#define RESP_HDR_MAX 16384
/* The address-bar/URL buffer size. Unlike the cookie and HSTS jars (now
 * uncapped heap lists), URLs flow through two SHARED, buffer-based APIs — the
 * gfx textfield widget (the address bar, edited in place) and resolve_url(base,
 * rel, out, cap) in user/lib/curl (also used by curl) — plus several on-stack
 * scratch URLs in the redirect follower. So this stays a constant, raised from
 * a stingy 512 to 2 KiB (the classic safe-URL length, past any real URL). Going
 * fully unbounded means heap-growing the gfx textfield + resolve_url's output
 * (shared with curl) and the html.c link store; see worker_stack below for the
 * matching stack headroom. */
#define URL_MAX 2048
#define POST_BODY_MAX 8192 /* cap on a form/POST request body (see struct form_submission) */
#define MAX_REDIR 5
#define FAVICON_W 16 /* favicon decoded to a fixed 16x16 ARGB bitmap and */
#define FAVICON_H 16 /* shown in the page's tab (0xAARRGGBB per pixel)   */

#define ST_COLOR 0x0F  /* legacy; color is now full 0xRRGGBB (B->fg) */
#define ST_NOWRAP 0x01 /* white-space:nowrap — this glyph's line must not wrap */
#define ST_BOLD 0x10
#define ST_UNDER 0x20
#define ST_ITALIC                                                                                                      \
  0x40                 /* font-style:italic — drawn with a faux-italic shear                                         \
                        */
#define ST_STRIKE 0x80 /* line-through (del/s/strike, text-decoration) */

/* Per-glyph background sentinel: B->bg[i] == BG_NONE means "no background"
 * (a real CSS colour is 0x000000..0xFFFFFF, so the high byte is free). */
#define BG_NONE 0xFF000000u

/* Per-glyph block-layout byte (B->bp): paragraph size/kind, alignment,
 * monospace and indent. Shared by the html walk (sets it) and the renderer
 * (applies it). */
#define BP_SIZE 0x07 /* 0 = body, 1..6 = h1..h6, 7 = horizontal rule */
#define BP_CENTER 0x08
#define BP_MONO 0x10
#define BP_INDENT 0xE0
#define BP_INDENT_SH 5

/* How the fetched body is rendered, picked from the response Content-Type. */
enum render_mode { MODE_HTML, MODE_TEXT, MODE_IMAGE };

/* A stored cookie. host/name/val are heap-allocated and sized to fit — there is
 * no fixed field width and no cap on how many cookies the jar holds; the jar is
 * a singly linked list (B->cookies) grown with malloc. */
struct cookie {
  char *host;
  char *name;
  char *val;
  struct cookie *next;
};
/* A host pinned to HTTPS by a Strict-Transport-Security header. Heap-allocated,
 * linked, and uncapped — every site that sends HSTS gets remembered. */
struct hsts_host {
  char *host;
  struct hsts_host *next;
};
struct link {
  int start, end;
  char href[URL_MAX];
  signed char newtab;
};

/* An external stylesheet URL declared in the response's Link: header (resolved
 * absolute). The document's own <link rel=stylesheet> elements are found while
 * walking the DOM; these come from the HTTP header instead. Heap list,
 * uncapped.*/
struct css_link {
  char *url;
  struct css_link *next;
};

/* Batch-fetch n absolute URLs into bodies[i] (malloc'd, caller frees) +
 * lens[i]. Injected into the renderer so html.c can pull external stylesheets —
 * same- origin ones multiplexed over h2 — without doing network I/O itself. */
typedef void (*res_batch_fn)(char *const *urls, int n, unsigned char **bodies, int *lens);

/* An inline <img>: anchored at a text offset, fetched into img_pool, laid out
 * into reserved blank rows, and blitted decode-on-scroll when in view. */
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
  int uid;            /* session-unique id once landed (0 = none);     *
                       * keys the decoded-pixel cache so a PNG inflates *
                       * once, not on every scroll repaint (see main.c) */
};

/* A single tab's page: everything that *is* one rendered document. Each tab
 * owns one (it lives inline in struct tab); B->pg points at the active tab's,
 * so a tab switch is just a pointer swap — no serialize/restore, and no field
 * can leak across tabs. The big image bytes are NOT here (see img_pool in
 * struct browser): the active load shares one 16 MiB scratch slab, and a
 * backgrounded tab keeps a right-sized copy in img_save so switching back
 * doesn't refetch. */
struct page {
  char doc_title[128];
  char ctype[80]; /* lowercased response Content-Type      */
  int mode;       /* enum render_mode for the current body */
  char cur_url[URL_MAX];

  /* Growable, heap-backed (browser_ensure_raw/browser_ensure_text in buffers.c). raw is the
   * fetched body or one inline image; text/attr/bp/fg/bg are the rendered
   * styled glyphs kept at one shared capacity so draw_styled_line indexes them
   * together. bp packs block layout (BP_* in html.c); fg/bg are 0xRRGGBB per
   * glyph (bg == BG_NONE = none). _cap is the current allocation. */
  char *raw;
  int raw_len, raw_cap;
  char *text;
  unsigned char *attr;
  int text_len, text_cap;
  unsigned char *bp;
  unsigned char *align; /* per-glyph text alignment: 0 left, 1 center, 2 right, 3 justify */
  unsigned char *fsz;   /* per-glyph CSS font-size px (0 = default body font) */
  unsigned char *lh;    /* per-glyph CSS line-height px (0 = font-derived default) */
  unsigned *fg;
  unsigned *bg;
  /* Page canvas colours from the <html>/<body> cascade (worker sets, render
   * reads): the pane clears to page_bg and untyped text defaults to page_fg. */
  unsigned page_bg, page_fg;

  struct link *links;
  int nlinks, links_cap;
  struct inl_img *imgs;
  int n_imgs, imgs_cap; /* inline <img> list */

  /* Decoded url() background images for this page: built by load_bg_images
   * during load, blitted by paint_boxes (a box's css_bg_image.img_idx indexes
   * here), freed on teardown. rgba is iw*ih straight RGBA, or NULL if the
   * fetch/decode failed. */
  struct bg_img {
    char url[URL_MAX];
    unsigned iw, ih;
    unsigned char *rgba;
  } *bgimgs;
  int n_bgimgs, bgimgs_cap;
  int img_uid_next; /* monotonic id source for inl_img.uid (never reused) */
  unsigned char *img_save;
  int img_save_len; /* backgrounded image bytes */

  /* Favicon: favicon_href is discovered from <link rel=icon> (empty -> the
   * /favicon.ico default); favicon_pri ranks the link; favicon[] is the decoded
   * 16x16 ARGB the tab strip draws when favicon_ok. */
  char favicon_href[URL_MAX];
  int favicon_pri;
  unsigned favicon[FAVICON_W * FAVICON_H];
  int favicon_ok;

  int scroll; /* vertical scroll position, document pixels */

  struct csp *csp;      /* the page's Content-Security-Policy set    */
  unsigned perm_policy; /* Permissions-Policy: bitmap of blocked features
                           (permissions.h) */

  /* @font-face faces loaded for this page (family + parsed font + owned bytes);
   * body_font is the face the body text uses, or NULL for the baked UI font. */
  struct loaded_face {
    char family[48];
    struct font *font;
    unsigned char *data;
  } *faces;
  int n_faces, faces_cap;
  struct font *body_font;
};

/* One wrapped text run of a flex-row item. Side-by-side items can't share a
 * main line-table row (that table is one slice per row), so a row flex
 * container reserves a blank band in the line table and its items' wrapped
 * lines live here instead, each tagged with the absolute band row it paints on
 * and its own column x/width. */
struct fslice {
  int row, off, len, x, w, h, maxpx;
};

struct browser {
  struct page *pg; /* the active tab's page (tabs[active_tab].page) */

  char addr[URL_MAX];       /* address-bar buffer (owned by addr_tf)       */
  struct textfield addr_tf; /* editable address-bar field (caret + scroll) */
  char req_url[URL_MAX];
  /* The queued navigation's HTTP method + body. Empty req_method means GET (the
   * common case); a form POST fills these alongside req_url. Set and consumed
   * on the worker thread (do_fetch clears req_method after capturing it), so no
   * lock is needed; the GET navigation setters clear req_method so a prior POST
   * never leaks into the next GET. */
  char req_method[8];
  char req_body[POST_BODY_MAX];
  int req_body_len;
  char req_ctype[80];
  int req_pending;
  int loading;
  volatile int abort_load;      /* main -> worker: cancel the in-flight fetch
                                 * (Stop button / Page>Stop / closing the tab). The
                                 * body readers poll it and bail; cleared when the
                                 * worker picks up the next navigation.            */
  volatile int scripts_pending; /* worker: the page is parsed, laid out and painted
                                 * with all its assets fetched; run its <script>s on
                                 * the NEXT worker pass. `loading` clears there (the
                                 * load is complete once assets are in), before the
                                 * scripts execute — so JS never blocks first paint
                                 * nor the "loaded" state.                           */
  volatile int in_scripts;      /* worker is executing the deferred script pass with
                                 * `loading` already clear; load_quiesce waits on this
                                 * too so a tab switch can't swap the page mid-script.*/

  bool nosniff;      /* X-Content-Type-Options: nosniff       */
  char dl_name[128]; /* filename from Content-Disposition     */
  int cdisp_attach;  /* 1 = Content-Disposition: attachment   */
  int dl_saved;      /* set when http_txn streamed a download */

  /* Shared inline-image scratch for the ACTIVE load: the parallel image
   * fetchers reserve disjoint regions under a short lock and copy in WITHOUT
   * the lock, so the base must never move (hence a fixed slab, not growable).
   * Only the active tab loads; a backgrounded tab keeps its bytes in
   * page->img_save and they are copied back here on reveal. */
  unsigned char img_pool[IMG_POOL];
  int img_pool_len;

  /* Word-wrap line table — render scratch for the active page (rebuilt by
   * rewrap() from B->pg->text at the live width on every active repaint). With
   * box layout on, line_x/line_w are the per-line content origin + width the
   * box tree placed the line at; in flat mode they are the fixed 4 / pane
   * width. */
  int *line_off;
  int *line_len;
  int *line_x;
  int *line_w;
  int *line_y;  /* per-line document-pixel top (running sum of heights) */
  int *line_h;  /* per-line pixel height                                */
  int *line_px; /* largest effective glyph size per line                */
  int nlines, line_cap;
  int wrap_cols;
  int layout_dirty; /* 1 = box tree/text changed, re-wrap needed (NOT bumped by
                       scroll) */

  /* Flex-row item text (see struct fslice), rebuilt with the line table and
   * drawn over the reserved band; never freed (grows like the line table). */
  struct fslice *fslices;
  int n_fslices, fslice_cap;

  int sb_drag;
  unsigned seq;
  unsigned layout_seq; /* reflow generation: bumped whenever rewrap re-flows the
                        * line table (resize, late image, @font-face). Lets the
                        * devtools snapshot track geometry without rebuilding on
                        * every unrelated repaint. */

  /* A content-pane interaction the main thread hands to the worker to dispatch
   * through the live JS DOM (the engine is single-threaded, worker-owned). The
   * main thread fills these + sets evt_pending; the worker resolves the node,
   * runs listeners, relays out, and navigates if the default wasn't prevented.
   */
  volatile int evt_pending;  /* 1 = an event is queued for the worker      */
  int evt_off;               /* text offset the click resolved to          */
  int evt_x, evt_y;          /* viewport-relative coords (event.clientX/Y)  */
  char evt_type[16];         /* "click", "input", "change", ...            */
  char evt_text[URL_MAX];    /* payload: the link href for click default,
                              * or the new field value for input/change    */
  volatile int drop_dom;     /* main asks the worker to free the live page  */
  volatile int drop_dom_tab; /* the tab that owned the DOM being torn down, so
                              * a teardown fault is blamed on it and not on
                              * whatever tab B->pg was swapped to meanwhile   */
  volatile int worker_pause; /* main -> worker: park at the top of the loop so
                              * the page set (B->pg / active_tab / tabs[]) can
                              * be mutated with the worker guaranteed idle */
  volatile int worker_idle;  /* worker -> main: parked, safe to mutate the set */
  volatile int recascade;    /* main asks the worker to re-run the cascade
                              * (window resized → @media may change)        */
  volatile int want_paw;     /* main -> worker: current content-pane pixel
                              * width. The worker owns the line-table flow
                              * (rewrap); the main thread only paints, so it
                              * never blocks the event loop on a big reflow.  */
  volatile int events_live;  /* worker -> main: page has listeners, so the
                              * main thread should route clicks as events   */
  volatile int focus_kind;   /* worker -> main: 0 none, 1 text input,
                              * 2 textarea — so the main thread routes keys
                              * to the focused control instead of the addr bar */

  /* the History API, wired to the real per-tab back/forward history. JS runs on
   * the worker; these let it drive the main thread's history (which owns the
   * tabs[] hist list + the nav buttons). */
  volatile int hist_go;   /* worker -> main: -1 back, +1 forward, 0 none */
  volatile int hist_nav;  /* worker -> main: a pushState/replaceState URL */
  int hist_replace;       /* the queued hist_nav replaces, not pushes     */
  char hist_url[URL_MAX]; /* its URL                                      */
  int hist_len;           /* main -> worker: history.length               */
  volatile int loc_nav;   /* worker -> main: location.assign/replace/reload */
  char loc_url[URL_MAX];  /* its target URL                               */
  char referrer[URL_MAX]; /* the page navigated away from (document.referrer) */

  /* window.alert/confirm/prompt. JS runs on the worker, but the modal dialog
   * must run on the MAIN thread — the gfx surface is main-thread-only and the
   * dialog is parented to the browser window the main task owns. The worker
   * fills these, posts dlg_req, and busy-waits on dlg_done; the main loop runs
   * the dialog (dialog lib) and returns dlg_ok + (for prompt) the entered text
   * back in dlg_text. */
  volatile int dlg_req;  /* worker -> main: 0 none, 1 alert, 2 confirm, 3 prompt,
                            4 permission */
  volatile int dlg_done; /* main -> worker: the dialog closed */
  int dlg_ok;            /* main -> worker: confirm/prompt accepted (OK/Yes); permission:
                            chosen button index (0=Refuse,1=Once,2=Always) */
  char dlg_title[64];
  char dlg_msg[512];
  char dlg_text[512]; /* prompt: default text in, entered text out            */

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

/* --- shared tiny helpers (header-inlined so each module gets its own copy) -
 * strlen/streq come from owos.h; lower/starts_ci/streq_ci come from dom.h
 * (shared with the parser); these are the remaining browser-specific ones. */
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

/* --- growable buffers + the realloc/render interlock (buffers.c) ----------- *
 * Each browser_ensure_*(need) grows the matching B buffer so it holds at least `need`
 * elements (bytes, or entries for lines/links/imgs), reallocating ~1.5x and
 * never shrinking. They return 1 on success, 0 if the heap is exhausted — a
 * caller that gets 0 stops appending, so an out-of-memory page is truncated
 * gracefully instead of overrunning. text/attr grow together to a shared cap.
 *
 * The worker thread fills these buffers while the main thread renders from
 * them. Appends into existing capacity are pointer-stable and safe to race (the
 * reader may see a half-written glyph, never a bad pointer); only a realloc
 * that MOVES a buffer is dangerous. So browser_ensure_raw/text/img_pool/links/imgs take
 * browser_buf_lock around the realloc, and render() (plus the click hit-test) hold
 * browser_buf_lock while they read — serialising the move against any in-flight read.
 * browser_ensure_lines is unlocked: the line table is written and read only by the main
 * thread. */
int browser_ensure_raw(int need);
int browser_grow_cap(int cap, int init, int need);
int browser_ensure_text(int need);
int browser_ensure_links(int need);
int browser_ensure_imgs(int need);
int browser_ensure_lines(int need);
void browser_ov_add(int row, int off, int len, int x, int w, int h, int maxpx);
unsigned *browser_surface(void);
int browser_surface_ready(void);
void browser_present(int id);
void browser_blit_rect(int x, int y, int w, int h);
void browser_buf_lock(void);
void browser_buf_unlock(void);

/* Lowest safe native stack address for JS recursion on the worker thread,
 * passed to js_set_stack_limit so a deeply recursive script throws instead of
 * smashing the worker stack into .bss. Defined in main.c (it owns the worker
 * stack). */
void *browser_js_stack_floor(void);

/* --- cross-module API ----------------------------------------------------- */
/* parse_url / resolve_url live in user/lib/url.h (shared with curl). */
/* Render B->raw as HTML into the styled-text buffers. `page_url` resolves
 * relative stylesheet hrefs; `fetch` (may be NULL) batch-pulls external <link
 * rel=stylesheet> + Link-header sheets so the CSS is complete before layout. */
void html_to_text(const char *page_url, res_batch_fn fetch);
void html_run_page_scripts(void);                /* deferred <script> pass; run after first paint */
void html_set_charset(const char *content_type); /* charset from HTTP CT */

/* js.c: run every inline <script> in `root` (document order) against a fresh JS
 * engine context, with minimal document/window DOM bindings. Called by
 * html_to_text after the DOM is parsed and before it is walked, so any DOM the
 * scripts mutate is reflected in the rendered page. */
void js_run_document(struct dom_node *root, const char *page_url, res_batch_fn fetch);
/* js.c lifetime + DOM-event API (the JS context now lives for the whole page).
 */
void js_page_end(void); /* reclaim the page ctx  */
int js_dispatch_dom_event(struct dom_node *target, const char *type, int x, int y);
int js_has_listeners(void);      /* any listener registered */
int js_timers_pump(void);        /* fire due setTimeout/Interval/rAF; -> # callbacks fired */
int js_websockets_pump(void);    /* drain live WebSockets -> open/message/close/error events */
int js_notifications_pump(void); /* deliver deferred Notification show/close events */
int js_cast_pump(void);          /* deliver Remote Playback availability + connect/disconnect */
int js_idb_pump(void);           /* deliver IndexedDB request events + commit/abort transactions */
/* Run one devtools-console REPL command against the live page context, echoing
 * the input + result/exception to the console. Worker-thread only (owns g_ctx).
 */
void js_repl_run(const char *src);

/* html.c: tear down the live DOM + CSS sheet + JS context of the current page
 * (called on every page replacement), re-flatten the live DOM into B's buffers
 * after a script mutates it, and map a text offset back to its deepest DOM
 * node. */
void html_page_teardown(void);
void html_relayout(void);
struct dom_node *html_node_at_offset(int off);
/* Inverse: the [start,end) text range a node covers, 0 if it was never laid
 * out. Backs the DOM geometry APIs (js.c) via browser_range_box.
 * Worker-thread-only. */
int html_node_span(struct dom_node *node, int *start, int *end);

/* Approximate element box geometry for the DOM geometry APIs (js.c). The
 * line-flow renderer has no per-element layout, so a box is reconstructed from
 * the wrapped line table; browser_range_box returns a document-relative px box
 * (0 if the range renders to nothing). Takes browser_buf_lock internally, so it is
 * worker-thread-safe. */
int browser_range_box(int start, int end, int *x, int *y, int *w, int *h);
int browser_scroll_px(void);              /* page vertical scroll, px (window.scrollY) */
void browser_viewport_px(int *w, int *h); /* content viewport size (innerWidth/Height) */

/* Form controls. html_control_kind classifies a node; the worker walks up from
 * a clicked node to the nearest control and performs its default action. The
 * focused text control (set by the worker) is where the renderer draws a caret.
 */
enum ctrl_kind {
  CTRL_NONE,
  CTRL_TEXT,
  CTRL_TEXTAREA,
  CTRL_CHECKBOX,
  CTRL_RADIO,
  CTRL_SELECT,
  CTRL_SUBMIT,
  CTRL_BUTTON
};
int html_control_kind(struct dom_node *node);
void html_set_focus(struct dom_node *node); /* NULL clears focus */
struct dom_node *html_focus(void);
int html_has_controls(void); /* page has form controls to click */
void html_toggle_checked(struct dom_node *c);
void html_radio_select(struct dom_node *c);
void html_select_cycle(struct dom_node *sel);
void html_seed_textarea(struct dom_node *ta);
struct dom_node *html_focus_edit(int ch); /* edit focused field; returns it */

/* A resolved form submission: the target URL, the HTTP method, and (for POST)
 * the request body + its Content-Type. html_form_build walks the form enclosing
 * a control, applies action/method/enctype (with the submit control's
 * formaction/ formmethod/formenctype overrides), enforces CSP form-action, and
 * fills this. A GET folds the field set into url's query string and leaves body
 * empty. */
struct form_submission {
  char url[URL_MAX];
  char method[8];           /* "GET" or "POST" */
  char body[POST_BODY_MAX]; /* request body for POST (empty for GET) */
  int body_len;
  char ctype[80]; /* Content-Type for the POST body */
};
int html_form_build(struct dom_node *control, struct form_submission *fs);
/* Submit the form enclosing `node` (a control or the <form> itself): optionally
 * fire the 'submit' event first (requestSubmit / interactive submit do, the
 * bare form.submit() does not), then navigate to the built target unless a
 * handler called preventDefault. Returns 1 if a form was found. Worker-thread
 * only. */
int browser_form_submit(struct dom_node *node, int fire_event);

void plain_to_text(void); /* html.c */
void doc_message(const char *m);
void show_error(const char *m); /* fetch.c */

/* --- jar.c: persistent cookie + HSTS store, backed by a real SQLite3 file
 * (user/lib/sqlite). The cookie jar (a `cookies` table) and the HSTS host list
 * (an `hsts` table) live in one db; both in-memory lists are uncapped heap
 * lists (B->cookies, B->hsts). The HTTP transport (fetch.c) only touches the
 * jar through these calls — no SQL leaks into fetch.c. ------------------------
 */
void jar_load(void);  /* fill B->cookies + B->hsts at boot */
void jar_save(void);  /* serialise the whole db now        */
void jar_flush(void); /* save once, only if it changed     */
void cookie_store(const char *host, const char *setcookie_value);
int cookie_header(const char *host, char *out, int cap); /* "a=b; c=d" -> len */
void hsts_add(const char *host);
int hsts_has(const char *host);

/* --- devtools.c: an in-window developer-tools panel (F12 / Ctrl+Shift+I) ----
 * A bottom-docked overlay with three views: an Elements (DOM tree) view, a
 * Styles (computed CSS for the selected node) view, and a Console capturing the
 * page's console.* output and JS errors. The DOM/CSS snapshots are built on the
 * worker thread (which owns the live DOM's lifetime) by devtools_worker_tick();
 * the console buffer is appended from the worker (js.c) and read by the main
 * thread's render — a small spinlock guards both against the render thread. */
enum { DT_LOG, DT_WARN, DT_ERROR };
void devtools_toggle(void);
int devtools_visible(void);
/* Inspect-element picker (page→tree): toggle arms it; the main thread feeds the
 * pane text offset under the pointer (hover) and at a click (commit), the
 * worker resolves each to the enclosing element and highlights / selects it. */
void devtools_pick_toggle(void);
int devtools_picking(void);
void devtools_pick_hover(int off);         /* pointer at pane offset (-1 = left the pane) */
void devtools_pick_commit(int off);        /* click at pane offset: select + end picking  */
int devtools_top(void);                    /* panel's top y (main thread)                */
int devtools_left(void);                   /* panel's left x; BW if hidden (right-dock)  */
void devtools_render(void);                /* main thread: draw the panel                */
int devtools_take_paint(void);             /* main: 1 = repaint only the dock (log line)  */
int devtools_handle_click(int mx, int my); /* 1 = consumed                    */
int devtools_handle_scroll(int delta);     /* 1 = consumed                    */
int devtools_handle_key(char c, int mods); /* Console REPL input; 1 = consumed */
/* The selected element's page-overlay box (DOCUMENT px) + label, 0 if none. The
 * caller translates y by the page scroll and clips to the content pane. */
int devtools_highlight(int *x, int *y, int *w, int *h, const char **label);
int devtools_on_splitter(int mx, int my);                        /* 1 = pointer is on the resize edge */
void devtools_set_width(int w);                                  /* drag-resize the docked panel      */
void devtools_console_append(int level, const char *s, int len); /* worker */
void devtools_worker_tick(void);                                 /* worker: rebuild DOM/Styles snapshot if open */
/* Network tab: each HTTP response the page made (logged from the fetch paths),
 * reset on each top-level navigation. Worker/image-thread callers. The strings
 * are borrowed (devtools copies what it keeps); `resp_hdr` is the raw response
 * header block (NULL when the transport didn't surface it) and `ms` the request
 * duration in milliseconds (0 = unknown). */
/* net_log.state — a request's lifecycle in the Network panel. NET_DONE MUST be
 * 0 so the completed-response callers (positional net_log initializers, which
 * omit the trailing fields) default to a finished request. */
enum { NET_DONE, NET_PENDING, NET_FAILED, NET_CANCELLED };
struct net_log {
  const char *method;
  const char *url;
  int status;
  const char *ctype;
  int size;
  int ms;
  const char *resp_hdr;
  int state;          /* NET_* — 0 (NET_DONE) for the completed-response callers */
  const char *reason; /* NET_FAILED failure text (e.g. "Connection failed");
                         else NULL */
};
/* A request shows the moment it starts, not only when it finishes:
 * devtools_net_begin logs an in-flight (NET_PENDING) row and returns an id;
 * devtools_net_update resolves that row to its outcome (done / failed /
 * cancelled). devtools_net_add is the one-shot form for requests already
 * complete when logged (cache hits, file:/data:). */
int devtools_net_begin(const char *method, const char *url);
void devtools_net_update(int id, const struct net_log *e);
void devtools_net_add(const struct net_log *e);
void devtools_net_reset(void);

/* Per-tab devtools view state: which panel is open and where each is scrolled,
 * so every browser tab keeps its own devtools. `cap` is an opaque per-tab
 * capture handle (the Elements/Console/Network content), owned by devtools.c —
 * frozen when the tab is backgrounded and restored on reveal, so switching tabs
 * no longer wipes the panel. devtools_view_free releases it when a tab closes.
 */
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

/* html.c hooks the devtools snapshot uses (worker-thread callers only). */
struct css_style;
struct css_match;
struct dom_node *html_dom_root(void);
void html_compute_style(struct dom_node *el, struct css_style *out);
int html_match_styles(struct dom_node *el, struct css_match *out, int cap);
int html_has_pseudo(struct dom_node *el, int which); /* ::before/::after present? */

void do_fetch(void);

/* One HTTP request the browser transport performs. Only `url` is required; the
 * rest default (method -> GET, accept -> all types, no body/headers, cookies
 * sent). `extra_hdrs` is a "Name: value\r\n" blob; `no_cookies` omits the
 * Cookie header (the CORS uncredentialed case). */
struct http_req {
  const char *url;
  const char *method;
  const unsigned char *body;
  int body_len;
  const char *ctype; /* request Content-Type */
  const char *accept;
  const char *extra_hdrs;
  int no_cookies;
};

/* What a request reports back. The optional out buffers (`final_url` after
 * redirects, the raw `hdr` block) are filled only when the caller supplies the
 * pointer + cap; leave them NULL / 0 to skip. */
struct http_resp {
  int status;      /* out: HTTP status, 0 on transport failure */
  int len;         /* out: body length                         */
  char ctype[160]; /* out: response Content-Type               */
  char *final_url;
  int final_cap; /* out (opt): final URL after redirects     */
  char *hdr;
  int hdr_cap; /* out (opt): raw response header block      */
};

/* The browser's single-request transport (sub-resources, the JS fetch()): one
 * request following redirects, returning the body (caller frees) for ANY final
 * status with `rs` filled, NULL on a transport/OOM error. Worker-thread only.
 */
unsigned char *browser_fetch(const struct http_req *rq, struct http_resp *rs);

/* Stream a media resource (<audio>/<video> src) to the temp file `file`,
 * issuing an open-ended Range request so a partial-content server answers 206
 * and the body is written straight to disk — a clip of any size loads without
 * ever holding the whole thing in memory (unlike browser_fetch, which buffers
 * it and caps at RAW_MAX). A server that ignores Range answers 200 with the
 * full body; both stream identically. Follows redirects, sends cookies,
 * HSTS-upgrades. Returns the byte count written on a 2xx (with *status set to
 * it), or -1 on a transport, non-2xx, or disk error. Worker/loader-thread only.
 */
long browser_fetch_media(const char *url, const char *accept, const char *file, int *status);

/* The CORS-aware transport for fetch()/XMLHttpRequest: on a cross-origin
 * request it adds the Origin header, runs a preflight OPTIONS for a non-simple
 * one, and only returns the body if the response's Access-Control-Allow-*
 * headers admit `page_url`'s origin. Cookies ride only when `with_credentials`
 * (fetch credentials:"include" / XHR withCredentials). NULL on a transport
 * error OR a CORS denial (the JS layer reports both as a network error). The
 * page-set request headers travel in `rq->extra_hdrs`. Worker-thread only. */
unsigned char *browser_cors_fetch(const char *page_url, int with_credentials, const struct http_req *rq,
                                  struct http_resp *rs);

/* Attribute subsequent sub-resource loads to `page_url`'s origin for CORP. */
void resource_set_page(const char *page_url);
void fetch_inline_images(const char *page_url); /* re-pull inline <img> after relayout */
int fetch_lazy_visible(void);                   /* worker: load one in-view loading=lazy <img> */

/* --- iframes -------------------------------------------------------------- *
 * A nested browsing context is rendered as an indented, bordered sub-flow of
 * the parent document. html.c finds <iframe> elements during the walk and keeps
 * a per-page cache of each frame's fetched, parsed sub-document keyed by its
 * src URL; resource.c does the actual fetch and decides — from the framed
 * response's X-Frame-Options / Content-Security-Policy `frame-ancestors` —
 * whether the document may be embedded at all. A frame that isn't yet fetched
 * shows a placeholder; the worker thread drains the pending set (fetch_iframes)
 * and re-flattens the page so the allowed ones appear. The iframe's `sandbox`
 * attribute gates whether links inside the frame may navigate; `allow`
 * (Permissions Policy) is parsed and surfaced in the frame chrome. */
enum { FRAME_OK = 1, FRAME_BLOCKED = 2, FRAME_ERROR = 3 }; /* embedding verdict */
int html_iframes_pending(void);                            /* worker: any frame still needs fetching?  */
/* worker: copy the next unfetched frame's absolute src + its embedder URL out;
 * returns 1 when one was found, 0 when none remain. */
int html_iframe_next_pending(char *url, int url_cap, char *parent, int parent_cap);
/* worker: hand a fetched frame's result back to the cache. `verdict` is one of
 * FRAME_*; `reason` is shown when the frame is blocked/failed; an FRAME_OK HTML
 * body is parsed into the frame's live sub-document. */
void html_iframe_resolve(const char *url, const char *parent, int verdict, const char *reason,
                         const unsigned char *body, int len, const char *ctype);
void fetch_iframes(void); /* resource.c: drain + fetch the pending set */
/* Commit in-memory image bytes (inline <svg> serialized by html.c) into image
 * slot idx with no fetch; SVG is rasterized to RGBA like a fetched .svg, with
 * `current_color` (0xRRGGBB) as the value of `currentColor`. */
void resource_commit_inline(int idx, const unsigned char *body, int len, unsigned current_color);

#endif /* OWOS_BROWSER_H */
