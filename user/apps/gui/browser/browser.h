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
#define RAW_MAX    (8 * 1024 * 1024)
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

#define ST_COLOR   0x0F
#define ST_BOLD    0x10
#define ST_UNDER   0x20

/* How the fetched body is rendered, picked from the response Content-Type. */
enum render_mode { MODE_HTML, MODE_TEXT, MODE_IMAGE };

/* A stored cookie. host/name/val are heap-allocated and sized to fit — there is
 * no fixed field width and no cap on how many cookies the jar holds; the jar is
 * a singly linked list (B->cookies) grown with malloc. */
struct cookie { char *host; char *name; char *val; struct cookie *next; };
/* A host pinned to HTTPS by a Strict-Transport-Security header. Heap-allocated,
 * linked, and uncapped — every site that sends HSTS gets remembered. */
struct hsts_host { char *host; struct hsts_host *next; };
struct link   { int start, end; char href[URL_MAX]; };

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

struct browser {
	char     addr[URL_MAX];      /* address-bar buffer (owned by addr_tf)       */
	struct textfield addr_tf;    /* editable address-bar field (caret + scroll) */
	char     req_url[URL_MAX];
	int      req_pending;
	int      loading;

	char     doc_title[128];
	char     ctype[80];              /* lowercased response Content-Type      */
	bool     nosniff;               /* X-Content-Type-Options: nosniff       */
	char     dl_name[128];           /* filename from Content-Disposition     */
	int      cdisp_attach;           /* 1 = Content-Disposition: attachment   */
	int      dl_saved;               /* set when http_txn streamed a download */
	int      mode;                   /* enum render_mode for the current body */
	char     cur_url[URL_MAX];

	/* Growable, heap-backed (ensure_raw/ensure_text in main.c). raw is the
	 * fetched body or one inline image; text/attr are the rendered styled
	 * glyphs and their per-glyph attribute byte (kept at the same capacity so
	 * draw_styled_line can index both). _cap is the current allocation. */
	char         *raw;       int raw_len,      raw_cap;
	char         *text;
	unsigned char *attr;     int text_len,     text_cap;

	struct link *links;      int nlinks,       links_cap;

	struct inl_img *imgs;    int n_imgs,       imgs_cap;   /* inline <img> list */
	int      img_uid_next;   /* monotonic id source for inl_img.uid (never reused) */
	/* Fixed slab (see IMG_POOL above): the parallel image fetchers need a base
	 * that never moves, so this one is not growable. */
	unsigned char img_pool[IMG_POOL];
	int      img_pool_len;

	/* Favicon for the current page. favicon_href is discovered from a
	 * <link rel=icon> while parsing the HTML (empty -> the /favicon.ico
	 * default); favicon_pri ranks the captured link so a real icon beats an
	 * apple-touch one. After fetch_favicon decodes it, favicon[] holds the
	 * 16x16 ARGB bitmap the tab strip draws and favicon_ok flags it valid. */
	char     favicon_href[URL_MAX];
	int      favicon_pri;            /* 0 none, 1 apple-touch, 2 standard icon */
	unsigned favicon[FAVICON_W * FAVICON_H];
	int      favicon_ok;

	int     *line_off;       /* growable word-wrap line table (ensure_lines)   */
	int     *line_len;
	int      nlines, line_cap;
	int      wrap_cols;
	unsigned wrap_seq;

	int      scroll;
	int      sb_drag;
	unsigned seq;

	int      win_id;                 /* the window handle (for title updates) */

	struct cookie *cookies;          /* head of the heap cookie jar (no cap)   */
	struct hsts_host *hsts;          /* head of the HSTS host list (no cap)    */
	struct css_link *ext_css;        /* stylesheet URLs from the Link: header   */

	/* @font-face faces loaded for this page (each: family name, the parsed font,
	 * and the owned font-file bytes the parser points into). body_font is the
	 * face the page's body text uses, or NULL to fall back to the baked UI font.
	 * Reset each navigation. struct font is from user/lib/font. */
	struct loaded_face { char family[48]; struct font *font; unsigned char *data; } faces[4];
	int          n_faces;
	struct font *body_font;

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

void do_fetch(void);
int  fetch_lazy_visible(void);   /* worker: load one in-view loading=lazy <img> */

#endif /* OWOS_BROWSER_H */
