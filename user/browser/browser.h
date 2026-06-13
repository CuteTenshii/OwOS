/* browser.h — shared state and helpers for the ring-3 web browser.
 *
 * The app is split across:
 *   html.c   URL parsing/resolution + HTML -> styled text + link extraction
 *   fetch.c  the HTTP/HTTPS transport: buffered reader, cookies, redirects
 *   main.c   the windowed UI: draw, word-wrap, input, and the event loop
 *
 * It runs entirely in ring 3 over the windowing + net syscalls. All buffers
 * live in one static `struct browser` (no kmalloc in user space); sizes are
 * trimmed to fit the per-process user window.
 */
#ifndef OWOS_BROWSER_H
#define OWOS_BROWSER_H

#include "owos.h"
#include "gfx.h"
#include "inflate.h"
#include "upng.h"
#include "url.h"     /* struct purl, parse_url, resolve_url (shared with curl) */

extern int BW, BH;         /* client size (mutable: the window is resizable)    */
#define MENU_H    18       /* top app-menu bar height (above the address bar)  */
#define TAB_H     24       /* tab strip height (between menu and address bar)  */
#define ADDR_H    26       /* address-bar height                               */
#define STATUS_H  20       /* bottom status-line height                        */
#define GLYPH_W   8
#define GLYPH_H   16
#define SCROLL_W  10       /* right-edge scrollbar width                       */

/* Buffer sizes — generous now that each process gets a 64 MiB window backed by
 * pmm high memory (see kernel/loader.h). RAW_MAX bounds a page body OR a single
 * inline image; pages and most images now load whole instead of truncating. */
#define RAW_MAX   (2 * 1024 * 1024)   /* captured response body / one image     */
#define TEXT_MAX  (512 * 1024)        /* rendered plain text                    */
#define MAX_LINES 16384               /* word-wrapped display lines             */
#define URL_MAX   512
#define MAX_REDIR 5
#define MAX_COOKIES 32
#define MAX_LINKS   128
#define MAX_HSTS    16     /* hosts that sent Strict-Transport-Security        */
#define MAX_INL_IMG 16     /* inline <img> per page                            */
#define IMG_POOL   (8 * 1024 * 1024)  /* shared store for inline images' bytes */

#define ST_COLOR   0x0F
#define ST_BOLD    0x10
#define ST_UNDER   0x20

/* How the fetched body is rendered, picked from the response Content-Type. */
enum render_mode { MODE_HTML, MODE_TEXT, MODE_IMAGE };

struct cookie { int used; char host[128]; char name[64]; char val[256]; };
struct link   { int start, end; char href[256]; };

/* An inline <img>: anchored at a text offset, fetched into img_pool, laid out
 * into reserved blank rows, and blitted decode-on-scroll when in view. */
struct inl_img {
	char     href[256];      /* src, resolved against the page URL            */
	char     alt[48];        /* alt text, shown when the image can't be drawn */
	int      text_pos;       /* offset into B->text where it is anchored      */
	int      start_row;      /* first display row (set by rewrap)             */
	int      rows;           /* reserved display rows (set by rewrap)         */
	int      ok;             /* 1 = img_pool holds a decodable PNG            */
	unsigned iw, ih;         /* natural pixel dimensions                      */
	int      coff, clen;     /* its PNG bytes within img_pool                 */
};

struct browser {
	char     addr[URL_MAX];
	int      addr_len;
	char     req_url[URL_MAX];
	int      req_pending;
	int      loading;

	char     status[160];
	char     doc_title[128];
	char     ctype[80];              /* lowercased response Content-Type      */
	char     dl_name[128];           /* filename from Content-Disposition     */
	int      cdisp_attach;           /* 1 = Content-Disposition: attachment   */
	int      dl_saved;               /* set when http_txn streamed a download */
	int      mode;                   /* enum render_mode for the current body */
	char     cur_url[URL_MAX];
	char     raw[RAW_MAX];
	int      raw_len;
	char     text[TEXT_MAX];
	unsigned char attr[TEXT_MAX];
	int      text_len;

	struct link links[MAX_LINKS];
	int      nlinks;

	struct inl_img imgs[MAX_INL_IMG];   /* inline images on the current page    */
	int      n_imgs;
	unsigned char img_pool[IMG_POOL];   /* their PNG bytes, packed             */
	int      img_pool_len;

	int      line_off[MAX_LINES];
	int      line_len[MAX_LINES];
	int      nlines;
	int      wrap_cols;
	unsigned wrap_seq;

	int      scroll;
	int      sb_drag;
	unsigned seq;

	int      win_id;                 /* the window handle (for title updates) */

	struct cookie cookies[MAX_COOKIES];
	char     hsts[MAX_HSTS][128];    /* hosts pinned to HTTPS via HSTS         */
	int      n_hsts;

	int           sock;
	unsigned char rbuf[2048];
	int           rpos, rlen, reof;
};

extern struct browser *B;
extern const unsigned palette[16];

/* --- shared tiny helpers (header-inlined so each module gets its own copy) - */
static inline int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static inline int streq(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static inline int starts_ci(const char *s, const char *pre)
{ while (*pre) { if (lower(*s) != lower(*pre)) return 0; s++; pre++; } return 1; }
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

/* --- cross-module API ----------------------------------------------------- */
/* parse_url / resolve_url live in user/lib/url.h (shared with curl). */
void html_to_text(void);
void plain_to_text(void);                                  /* html.c */
void doc_message(const char *m);
void set_status(const char *s);                            /* fetch.c */
void do_fetch(void);

#endif /* OWOS_BROWSER_H */
