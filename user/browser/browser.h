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

extern int BW, BH;         /* client size (mutable: the window is resizable)    */
#define ADDR_H    26       /* address-bar height                               */
#define STATUS_H  20       /* bottom status-line height                        */
#define GLYPH_W   8
#define GLYPH_H   16
#define SCROLL_W  10       /* right-edge scrollbar width                       */

#define RAW_MAX   (64 * 1024)    /* captured response body (bytes)             */
#define TEXT_MAX  (32 * 1024)    /* rendered plain text                        */
#define MAX_LINES 2048           /* word-wrapped display lines                 */
#define URL_MAX   512
#define MAX_REDIR 5
#define MAX_COOKIES 32
#define MAX_LINKS   128
#define MAX_HSTS    16     /* hosts that sent Strict-Transport-Security        */

#define ST_COLOR   0x0F
#define ST_BOLD    0x10
#define ST_UNDER   0x20

struct cookie { int used; char host[128]; char name[64]; char val[256]; };
struct link   { int start, end; char href[256]; };
struct purl   { int tls; int port; char host[256]; char path[URL_MAX]; };

struct browser {
	char     addr[URL_MAX];
	int      addr_len;
	char     req_url[URL_MAX];
	int      req_pending;
	int      loading;

	char     status[160];
	char     doc_title[128];
	char     cur_url[URL_MAX];
	char     raw[RAW_MAX];
	int      raw_len;
	char     text[TEXT_MAX];
	unsigned char attr[TEXT_MAX];
	int      text_len;

	struct link links[MAX_LINKS];
	int      nlinks;

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
int  parse_url(const char *url, struct purl *u);            /* html.c */
void resolve_url(const char *base, const char *loc, char *out, int cap);
void html_to_text(void);
void set_status(const char *s);                            /* fetch.c */
void do_fetch(void);

#endif /* OWOS_BROWSER_H */
