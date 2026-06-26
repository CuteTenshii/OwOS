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
 * browser`. See ensure_*() in main.c for the grow helpers and blk_lock() for the
 * realloc/render interlock.
 */
#ifndef OWOS_BROWSER_H
#define OWOS_BROWSER_H

#include "owos.h"
#include "gfx.h"
#include "inflate.h"
#include "upng.h"
#include "url.h"     /* struct purl, parse_url, resolve_url (shared with curl) */
#include "h2.h"      /* HTTP/2 engine: h2_run + struct h2_opts (TLS+ALPN path) */
#include "dom.h"     /* DOM tree + the shared lower/starts_ci/streq_ci helpers  */
#include "csp.h"     /* Content-Security-Policy engine (per-page enforcement)   */

extern int BW, BH;         /* client size (mutable: the window is resizable)    */
#define MENU_H    18       /* top app-menu bar height (above the address bar)  */
#define TAB_H     24       /* tab strip height (between menu and address bar)  */
#define ADDR_H    26       /* address-bar height                               */
#define GLYPH_W   8
#define GLYPH_H   16
#define SCROLL_W  10       /* right-edge scrollbar width                       */

/* Initial heap reservations for the growable buffers. Each one starts at the
 * size below and grows (~1.5x, never shrinking) whenever an append would
 * overflow it, so a page body, the rendered text, the inline-image store, and
 * the link/image lists are bounded only by available memory — not a fixed cap.
 * The numbers are just "fits the common case without a realloc". */
#define RAW_INIT     (256 * 1024)     /* captured response body / one image     */
#define TEXT_INIT    (128 * 1024)     /* rendered styled text (+ equal attr)    */
#define LINES_INIT   4096             /* word-wrapped display lines             */
#define LINKS_INIT   128              /* extracted <a href> links               */
#define IMGS_INIT    16               /* inline <img> elements                  */
/* img_pool stays a single fixed slab, not a growable buffer: the inline-image
 * fetchers run on several threads at once (fetch.c), each reserving a disjoint
 * region under a short lock and then copying into it WITHOUT the lock — which is
 * only safe while the base address never moves. So this one keeps a hard cap. */
#define IMG_POOL   (16 * 1024 * 1024) /* shared store for inline images' bytes  */
/* Per-fetch sanity cap on ONE inline image's body (fetch.c's image fetchers
 * malloc a doubling buffer up to this). The page body itself is unbounded (it
 * grows B->raw); this only stops a single runaway sub-resource. */
#define RAW_MAX    (32 * 1024 * 1024)   /* a big page decompresses to several MB */
/* The address-bar/URL buffer size. Unlike the cookie and HSTS jars (now uncapped
 * heap lists), URLs flow through two SHARED, buffer-based APIs — the gfx
 * textfield widget (the address bar, edited in place) and resolve_url(base, rel,
 * out, cap) in user/lib/curl (also used by curl) — plus several on-stack scratch
 * URLs in the redirect follower. So this stays a constant, raised from a stingy
 * 512 to 2 KiB (the classic safe-URL length, past any real URL). Going fully
 * unbounded means heap-growing the gfx textfield + resolve_url's output (shared
 * with curl) and the html.c link store; see worker_stack below for the matching
 * stack headroom. */
#define URL_MAX   2048
#define MAX_REDIR 5
#define FAVICON_W  16      /* favicon decoded to a fixed 16x16 ARGB bitmap and */
#define FAVICON_H  16      /* shown in the page's tab (0xAARRGGBB per pixel)   */

#define ST_COLOR   0x0F            /* legacy; color is now full 0xRRGGBB (B->fg) */
#define ST_BOLD    0x10
#define ST_UNDER   0x20

/* Per-glyph background sentinel: B->bg[i] == BG_NONE means "no background"
 * (a real CSS colour is 0x000000..0xFFFFFF, so the high byte is free). */
#define BG_NONE    0xFF000000u

/* Per-glyph block-layout byte (B->bp): paragraph size/kind, alignment, monospace
 * and indent. Shared by the html walk (sets it) and the renderer (applies it). */
#define BP_SIZE      0x07   /* 0 = body, 1..6 = h1..h6, 7 = horizontal rule */
#define BP_CENTER    0x08
#define BP_MONO      0x10
#define BP_INDENT    0xE0
#define BP_INDENT_SH 5

/* How the fetched body is rendered, picked from the response Content-Type. */
enum render_mode { MODE_HTML, MODE_TEXT, MODE_IMAGE };

/* A stored cookie. host/name/val are heap-allocated and sized to fit — there is
 * no fixed field width and no cap on how many cookies the jar holds; the jar is
 * a singly linked list (B->cookies) grown with malloc. */
struct cookie { char *host; char *name; char *val; struct cookie *next; };
/* A host pinned to HTTPS by a Strict-Transport-Security header. Heap-allocated,
 * linked, and uncapped — every site that sends HSTS gets remembered. */
struct hsts_host { char *host; struct hsts_host *next; };
struct link   { int start, end; char href[URL_MAX]; signed char newtab; };

/* An external stylesheet URL declared in the response's Link: header (resolved
 * absolute). The document's own <link rel=stylesheet> elements are found while
 * walking the DOM; these come from the HTTP header instead. Heap list, uncapped.*/
struct css_link { char *url; struct css_link *next; };

/* Batch-fetch n absolute URLs into bodies[i] (malloc'd, caller frees) + lens[i].
 * Injected into the renderer so html.c can pull external stylesheets — same-
 * origin ones multiplexed over h2 — without doing network I/O itself. */
typedef void (*res_batch_fn)(char *const *urls, int n, unsigned char **bodies, int *lens);

/* An inline <img>: anchored at a text offset, fetched into img_pool, laid out
 * into reserved blank rows, and blitted decode-on-scroll when in view. */
struct inl_img {
	char     href[URL_MAX];  /* src, resolved against the page URL            */
	char     alt[48];        /* alt text, shown when the image can't be drawn */
	int      text_pos;       /* offset into B->text where it is anchored      */
	int      start_row;      /* first display row (set by rewrap)             */
	int      rows;           /* reserved display rows (set by rewrap)         */
	int      ok;             /* 1 = img_pool holds a decodable PNG            */
	unsigned iw, ih;         /* natural pixel dimensions                      */
	int      coff, clen;     /* its PNG bytes within img_pool                 */
	int      lazy;           /* 1 = loading="lazy": fetch only when in view   */
	int      want;           /* UI flag: a lazy image is now scrolled in      */
	int      fetched;        /* 1 = a fetch has been attempted (ok = result)  */
	int      cw, ch;         /* width/height from the attr or CSS, px (0=auto) */
	int      is_argb;        /* 1 = img_pool holds raw RGBA (SVG), not PNG    */
	int      uid;            /* session-unique id once landed (0 = none);     *
	                          * keys the decoded-pixel cache so a PNG inflates *
	                          * once, not on every scroll repaint (see main.c) */
};

/* A single tab's page: everything that *is* one rendered document. Each tab owns
 * one (it lives inline in struct tab); B->pg points at the active tab's, so a tab
 * switch is just a pointer swap — no serialize/restore, and no field can leak
 * across tabs. The big image bytes are NOT here (see img_pool in struct browser):
 * the active load shares one 16 MiB scratch slab, and a backgrounded tab keeps a
 * right-sized copy in img_save so switching back doesn't refetch. */
struct page {
	char     doc_title[128];
	char     ctype[80];              /* lowercased response Content-Type      */
	int      mode;                   /* enum render_mode for the current body */
	char     cur_url[URL_MAX];

	/* Growable, heap-backed (ensure_raw/ensure_text in main.c). raw is the
	 * fetched body or one inline image; text/attr/bp/fg/bg are the rendered
	 * styled glyphs kept at one shared capacity so draw_styled_line indexes them
	 * together. bp packs block layout (BP_* in html.c); fg/bg are 0xRRGGBB per
	 * glyph (bg == BG_NONE = none). _cap is the current allocation. */
	char         *raw;       int raw_len,  raw_cap;
	char         *text;
	unsigned char *attr;     int text_len, text_cap;
	unsigned char *bp;
	unsigned      *fg;
	unsigned      *bg;
	/* Page canvas colours from the <html>/<body> cascade (worker sets, render
	 * reads): the pane clears to page_bg and untyped text defaults to page_fg. */
	unsigned       page_bg, page_fg;

	struct link *links;      int nlinks,  links_cap;
	struct inl_img *imgs;    int n_imgs,  imgs_cap;   /* inline <img> list */
	int      img_uid_next;   /* monotonic id source for inl_img.uid (never reused) */
	unsigned char *img_save; int img_save_len;        /* backgrounded image bytes */

	/* Favicon: favicon_href is discovered from <link rel=icon> (empty -> the
	 * /favicon.ico default); favicon_pri ranks the link; favicon[] is the decoded
	 * 16x16 ARGB the tab strip draws when favicon_ok. */
	char     favicon_href[URL_MAX];
	int      favicon_pri;
	unsigned favicon[FAVICON_W * FAVICON_H];
	int      favicon_ok;

	int      scroll;                 /* vertical scroll position (display lines) */

	struct csp *csp;                 /* the page's Content-Security-Policy set    */

	/* @font-face faces loaded for this page (family + parsed font + owned bytes);
	 * body_font is the face the body text uses, or NULL for the baked UI font. */
	struct loaded_face { char family[48]; struct font *font; unsigned char *data; } *faces;
	int          n_faces, faces_cap;
	struct font *body_font;
};

struct browser {
	struct page *pg;             /* the active tab's page (tabs[active_tab].page) */

	char     addr[URL_MAX];      /* address-bar buffer (owned by addr_tf)       */
	struct textfield addr_tf;    /* editable address-bar field (caret + scroll) */
	char     req_url[URL_MAX];
	int      req_pending;
	int      loading;
	volatile int abort_load;     /* main -> worker: cancel the in-flight fetch
	                              * (Stop button / Page>Stop / closing the tab). The
	                              * body readers poll it and bail; cleared when the
	                              * worker picks up the next navigation.            */

	bool     nosniff;               /* X-Content-Type-Options: nosniff       */
	char     dl_name[128];           /* filename from Content-Disposition     */
	int      cdisp_attach;           /* 1 = Content-Disposition: attachment   */
	int      dl_saved;               /* set when http_txn streamed a download */

	/* Shared inline-image scratch for the ACTIVE load: the parallel image
	 * fetchers reserve disjoint regions under a short lock and copy in WITHOUT
	 * the lock, so the base must never move (hence a fixed slab, not growable).
	 * Only the active tab loads; a backgrounded tab keeps its bytes in
	 * page->img_save and they are copied back here on reveal. */
	unsigned char img_pool[IMG_POOL];
	int      img_pool_len;

	/* Word-wrap line table — render scratch for the active page (rebuilt by
	 * rewrap() from B->pg->text at the live width on every active repaint). With
	 * box layout on, line_x/line_w are the per-line content origin + width the box
	 * tree placed the line at; in flat mode they are the fixed 4 / pane width. */
	int     *line_off;
	int     *line_len;
	int     *line_x;
	int     *line_w;
	int      nlines, line_cap;
	int      wrap_cols;
	unsigned wrap_seq;

	int      sb_drag;
	unsigned seq;

	/* A content-pane interaction the main thread hands to the worker to dispatch
	 * through the live JS DOM (the engine is single-threaded, worker-owned). The
	 * main thread fills these + sets evt_pending; the worker resolves the node,
	 * runs listeners, relays out, and navigates if the default wasn't prevented. */
	volatile int evt_pending;        /* 1 = an event is queued for the worker      */
	int      evt_off;                /* text offset the click resolved to          */
	int      evt_x, evt_y;           /* viewport-relative coords (event.clientX/Y)  */
	char     evt_type[16];           /* "click", "input", "change", ...            */
	char     evt_text[URL_MAX];      /* payload: the link href for click default,
	                                  * or the new field value for input/change    */
	volatile int drop_dom;           /* main asks the worker to free the live page  */
	volatile int recascade;          /* main asks the worker to re-run the cascade
	                                  * (window resized → @media may change)        */
	volatile int events_live;        /* worker -> main: page has listeners, so the
	                                  * main thread should route clicks as events   */
	volatile int focus_kind;         /* worker -> main: 0 none, 1 text input,
	                                  * 2 textarea — so the main thread routes keys
	                                  * to the focused control instead of the addr bar */

	/* the History API, wired to the real per-tab back/forward history. JS runs on
	 * the worker; these let it drive the main thread's history (which owns the
	 * tabs[] hist list + the nav buttons). */
	volatile int hist_go;            /* worker -> main: -1 back, +1 forward, 0 none */
	volatile int hist_nav;           /* worker -> main: a pushState/replaceState URL */
	int      hist_replace;           /* the queued hist_nav replaces, not pushes     */
	char     hist_url[URL_MAX];      /* its URL                                      */
	int      hist_len;               /* main -> worker: history.length               */
	volatile int loc_nav;            /* worker -> main: location.assign/replace/reload */
	char     loc_url[URL_MAX];       /* its target URL                               */
	char     referrer[URL_MAX];      /* the page navigated away from (document.referrer) */

	int      win_id;                 /* the window handle (for title updates) */

	struct cookie *cookies;          /* head of the heap cookie jar (no cap)   */
	struct hsts_host *hsts;          /* head of the HSTS host list (no cap)    */
	struct css_link *ext_css;        /* stylesheet URLs from the Link: header   */

	int           sock;
	unsigned char rbuf[2048];
	int           rpos, rlen, reof;
};


extern struct browser *B;
extern const unsigned palette[16];

/* --- shared tiny helpers (header-inlined so each module gets its own copy) -
 * strlen/streq come from owos.h; lower/starts_ci/streq_ci come from dom.h (shared
 * with the parser); these are the remaining browser-specific ones. */
static inline void sapp(char *d, int *n, int cap, const char *s)
{ while (*s && *n < cap - 1) d[(*n)++] = *s++; d[*n] = '\0'; }
static inline void uapp(char *d, int *n, int cap, unsigned v)
{
	char t[12]; int k = 0;
	if (v == 0) { if (*n < cap - 1) d[(*n)++] = '0'; d[*n] = '\0'; return; }
	while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
	while (k && *n < cap - 1) d[(*n)++] = t[--k];
	d[*n] = '\0';
}

/* --- growable buffers + the realloc/render interlock (main.c) -------------- *
 * Each ensure_*(need) grows the matching B buffer so it holds at least `need`
 * elements (bytes, or entries for lines/links/imgs), reallocating ~1.5x and
 * never shrinking. They return 1 on success, 0 if the heap is exhausted — a
 * caller that gets 0 stops appending, so an out-of-memory page is truncated
 * gracefully instead of overrunning. text/attr grow together to a shared cap.
 *
 * The worker thread fills these buffers while the main thread renders from them.
 * Appends into existing capacity are pointer-stable and safe to race (the reader
 * may see a half-written glyph, never a bad pointer); only a realloc that MOVES
 * a buffer is dangerous. So ensure_raw/text/img_pool/links/imgs take blk_lock
 * around the realloc, and render() (plus the click hit-test) hold blk_lock while
 * they read — serialising the move against any in-flight read. ensure_lines is
 * unlocked: the line table is written and read only by the main thread. */
int  ensure_raw(int need);
int  ensure_text(int need);
int  ensure_links(int need);
int  ensure_imgs(int need);
int  ensure_lines(int need);
void blk_lock(void);
void blk_unlock(void);

/* --- cross-module API ----------------------------------------------------- */
/* parse_url / resolve_url live in user/lib/url.h (shared with curl). */
/* Render B->raw as HTML into the styled-text buffers. `page_url` resolves
 * relative stylesheet hrefs; `fetch` (may be NULL) batch-pulls external <link
 * rel=stylesheet> + Link-header sheets so the CSS is complete before layout. */
void html_to_text(const char *page_url, res_batch_fn fetch);
void html_set_charset(const char *content_type);          /* charset from HTTP CT */

/* js.c: run every inline <script> in `root` (document order) against a fresh JS
 * engine context, with minimal document/window DOM bindings. Called by
 * html_to_text after the DOM is parsed and before it is walked, so any DOM the
 * scripts mutate is reflected in the rendered page. */
void js_run_document(struct dom_node *root, const char *page_url, res_batch_fn fetch);
/* js.c lifetime + DOM-event API (the JS context now lives for the whole page). */
void js_page_end(void);                                    /* reclaim the page ctx  */
int  js_dispatch_dom_event(struct dom_node *target, const char *type, int x, int y);
int  js_has_listeners(void);                               /* any listener registered */
int  js_timers_pump(void);     /* fire due setTimeout/Interval/rAF; -> # callbacks fired */
int  js_websockets_pump(void); /* drain live WebSockets -> open/message/close/error events */

/* html.c: tear down the live DOM + CSS sheet + JS context of the current page
 * (called on every page replacement), re-flatten the live DOM into B's buffers
 * after a script mutates it, and map a text offset back to its deepest DOM node. */
void html_page_teardown(void);
void html_relayout(void);
struct dom_node *html_node_at_offset(int off);
/* Inverse: the [start,end) text range a node covers, 0 if it was never laid out.
 * Backs the DOM geometry APIs (js.c) via browser_range_box. Worker-thread-only. */
int  html_node_span(struct dom_node *node, int *start, int *end);

/* Approximate element box geometry for the DOM geometry APIs (js.c). The line-flow
 * renderer has no per-element layout, so a box is reconstructed from the wrapped
 * line table; browser_range_box returns a document-relative px box (0 if the range
 * renders to nothing). Takes blk_lock internally, so it is worker-thread-safe. */
int  browser_range_box(int start, int end, int *x, int *y, int *w, int *h);
int  browser_scroll_px(void);                /* page vertical scroll, px (window.scrollY) */
void browser_viewport_px(int *w, int *h);    /* content viewport size (innerWidth/Height) */

/* Form controls. html_control_kind classifies a node; the worker walks up from a
 * clicked node to the nearest control and performs its default action. The focused
 * text control (set by the worker) is where the renderer draws a caret. */
enum ctrl_kind { CTRL_NONE, CTRL_TEXT, CTRL_TEXTAREA, CTRL_CHECKBOX, CTRL_RADIO,
                 CTRL_SELECT, CTRL_SUBMIT, CTRL_BUTTON };
int  html_control_kind(struct dom_node *node);
void html_set_focus(struct dom_node *node);     /* NULL clears focus */
struct dom_node *html_focus(void);
int  html_has_controls(void);                   /* page has form controls to click */
void html_toggle_checked(struct dom_node *c);
void html_radio_select(struct dom_node *c);
void html_select_cycle(struct dom_node *sel);
void html_seed_textarea(struct dom_node *ta);
struct dom_node *html_focus_edit(int ch);       /* edit focused field; returns it */
int  html_form_submit_url(struct dom_node *control, char *out, int cap);

void plain_to_text(void);                                  /* html.c */
void doc_message(const char *m);
void show_error(const char *m);                            /* fetch.c */

/* --- jar.c: persistent cookie + HSTS store, backed by a real SQLite3 file
 * (user/lib/sqlite). The cookie jar (a `cookies` table) and the HSTS host list
 * (an `hsts` table) live in one db; both in-memory lists are uncapped heap
 * lists (B->cookies, B->hsts). The HTTP transport (fetch.c) only touches the
 * jar through these calls — no SQL leaks into fetch.c. ------------------------ */
void jar_load(void);                       /* fill B->cookies + B->hsts at boot */
void jar_save(void);                       /* serialise the whole db now        */
void jar_flush(void);                      /* save once, only if it changed     */
void cookie_store(const char *host, const char *setcookie_value);
int  cookie_header(const char *host, char *out, int cap);  /* "a=b; c=d" -> len */
void hsts_add(const char *host);
int  hsts_has(const char *host);

/* --- devtools.c: an in-window developer-tools panel (F12 / Ctrl+Shift+I) ----
 * A bottom-docked overlay with three views: an Elements (DOM tree) view, a
 * Styles (computed CSS for the selected node) view, and a Console capturing the
 * page's console.* output and JS errors. The DOM/CSS snapshots are built on the
 * worker thread (which owns the live DOM's lifetime) by devtools_worker_tick();
 * the console buffer is appended from the worker (js.c) and read by the main
 * thread's render — a small spinlock guards both against the render thread. */
enum { DT_LOG, DT_WARN, DT_ERROR };
void devtools_toggle(void);
int  devtools_visible(void);
int  devtools_top(void);          /* panel's top y (main thread)                */
int  devtools_left(void);         /* panel's left x; BW if hidden (right-dock)  */
void devtools_render(void);       /* main thread: draw the panel                */
int  devtools_handle_click(int mx, int my);  /* 1 = consumed                    */
int  devtools_handle_scroll(int delta);      /* 1 = consumed                    */
int  devtools_on_splitter(int mx, int my);   /* 1 = pointer is on the resize edge */
void devtools_set_width(int w);              /* drag-resize the docked panel      */
void devtools_console_append(int level, const char *s, int len);  /* worker     */
void devtools_worker_tick(void);  /* worker: rebuild DOM/Styles snapshot if open */
/* Network tab: each HTTP response the page made (logged from the fetch paths),
 * reset on each top-level navigation. Worker/image-thread callers. */
void devtools_net_add(const char *method, const char *url, int status,
                      const char *ctype, int size);
void devtools_net_reset(void);

/* Per-tab devtools view state: which panel is open and where each is scrolled, so
 * every browser tab keeps its own devtools. `cap` is an opaque per-tab capture
 * handle (the Elements/Console/Network content), owned by devtools.c — frozen
 * when the tab is backgrounded and restored on reveal, so switching tabs no longer
 * wipes the panel. devtools_view_free releases it when a tab closes. */
#define DT_PANELS 4
struct dt_view { int open; int tab; int scroll[DT_PANELS]; void *cap; };
void devtools_view_save(struct dt_view *v);        /* active view+capture -> v       */
void devtools_view_load(const struct dt_view *v);  /* v -> active, restoring capture */
void devtools_view_free(struct dt_view *v);        /* free a closed tab's capture    */
void devtools_reset_live(void);                    /* drop the live (statics) capture */

/* html.c hooks the devtools snapshot uses (worker-thread callers only). */
struct css_style;
struct css_match;
struct dom_node *html_dom_root(void);
void html_compute_style(struct dom_node *el, struct css_style *out);
int  html_match_styles(struct dom_node *el, struct css_match *out, int cap);
int  html_has_pseudo(struct dom_node *el, int which);   /* ::before/::after present? */

void do_fetch(void);
/* The JS fetch() transport (resource.c): one HTTP request, returns the body
 * (caller frees) for any status with *status + *out_ctype filled, NULL on a
 * connection error. Worker-thread only. */
unsigned char *browser_fetch_url(const char *url, const char *method,
                                 const unsigned char *reqbody, int reqlen, const char *req_ctype,
                                 const char *accept, int *status, char *out_ctype, int oc_cap, int *outlen,
                                 const char *extra_hdrs);
void fetch_inline_images(const char *page_url);  /* re-pull inline <img> after relayout */
int  fetch_lazy_visible(void);   /* worker: load one in-view loading=lazy <img> */

/* --- iframes -------------------------------------------------------------- *
 * A nested browsing context is rendered as an indented, bordered sub-flow of the
 * parent document. html.c finds <iframe> elements during the walk and keeps a
 * per-page cache of each frame's fetched, parsed sub-document keyed by its src
 * URL; resource.c does the actual fetch and decides — from the framed response's
 * X-Frame-Options / Content-Security-Policy `frame-ancestors` — whether the
 * document may be embedded at all. A frame that isn't yet fetched shows a
 * placeholder; the worker thread drains the pending set (fetch_iframes) and
 * re-flattens the page so the allowed ones appear. The iframe's `sandbox`
 * attribute gates whether links inside the frame may navigate; `allow`
 * (Permissions Policy) is parsed and surfaced in the frame chrome. */
enum { FRAME_OK = 1, FRAME_BLOCKED = 2, FRAME_ERROR = 3 };  /* embedding verdict */
int  html_iframes_pending(void);     /* worker: any frame still needs fetching?  */
/* worker: copy the next unfetched frame's absolute src + its embedder URL out;
 * returns 1 when one was found, 0 when none remain. */
int  html_iframe_next_pending(char *url, int url_cap, char *parent, int parent_cap);
/* worker: hand a fetched frame's result back to the cache. `verdict` is one of
 * FRAME_*; `reason` is shown when the frame is blocked/failed; an FRAME_OK HTML
 * body is parsed into the frame's live sub-document. */
void html_iframe_resolve(const char *url, const char *parent, int verdict,
                         const char *reason, const unsigned char *body, int len,
                         const char *ctype);
void fetch_iframes(void);            /* resource.c: drain + fetch the pending set */
/* Commit in-memory image bytes (inline <svg> serialized by html.c) into image
 * slot idx with no fetch; SVG is rasterized to RGBA like a fetched .svg, with
 * `current_color` (0xRRGGBB) as the value of `currentColor`. */
void resource_commit_inline(int idx, const unsigned char *body, int len, unsigned current_color);

#endif /* OWOS_BROWSER_H */
