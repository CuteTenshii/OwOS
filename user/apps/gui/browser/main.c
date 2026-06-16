/* main.c — the browser's windowed UI and event loop.
 *
 * Owns the one `struct browser` instance and the colour palette. The address
 * bar, content pane and scrollbar are drawn here; a worker thread
 * (thread_create) runs the blocking fetch so typing/scrolling stay live while a
 * page loads. The worker only touches B's buffers + net; window syscalls (title,
 * present) are made by this, the owning, thread.
 */
#include "browser.h"
#include "font.h"   /* custom @font-face faces (user/lib/font) */

static struct browser binst;
struct browser *B = &binst;

int BW = 600, BH = 368;        /* client size; grows/shrinks on resize */

const unsigned palette[16] = {
	0x202830, 0x1a4fbf, 0xc01818, 0x108010, 0x9020a0, 0x9a6020, 0x107c90,
	0x586070, 0x16387a, 0x000000, 0x707000, 0xd07000, 0x808890, 0x304060,
	0x2050d0, 0xa0a0a0,
};

/* --- growable buffers + the realloc/render interlock ----------------------- *
 * The worker thread grows B's content buffers while the main thread renders
 * from them. A plain append never moves a buffer, so it races harmlessly (as it
 * did when these were fixed arrays); only a realloc that relocates a buffer can
 * hand the reader a dangling base pointer. blk_lock serialises those moves
 * against render() — the same xchg spinlock the user malloc uses, yielding to
 * the holder on contention (single CPU; the timer preempts a spinner anyway). */
static volatile int blk_lk;
static inline int blk_xchg(volatile int *p, int v)
{
	__asm__ volatile ("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
	return v;
}
void blk_lock(void)   { while (blk_xchg(&blk_lk, 1)) msleep(0); }
void blk_unlock(void) { __asm__ volatile ("" ::: "memory"); blk_lk = 0; }

/* Next capacity that holds at least `need` elements, growing ~1.5x from `cap`
 * (or `init` on the first allocation). */
static int grow_cap(int cap, int init, int need)
{
	int nc = cap > 0 ? cap : init;
	while (nc < need) nc += nc / 2 + 1;
	return nc;
}

/* raw / img_pool: byte buffers. Lock only around the relocating realloc. */
int ensure_raw(int need)
{
	if (need <= B->raw_cap) return 1;
	int nc = grow_cap(B->raw_cap, RAW_INIT, need);
	blk_lock();
	char *p = realloc(B->raw, (unsigned)nc);
	if (p) { B->raw = p; B->raw_cap = nc; }
	blk_unlock();
	return p != 0;
}

/* text + attr share one capacity so draw_styled_line can index both. Only
 * advance text_cap when BOTH reallocs succeed, so the two stay consistent. */
int ensure_text(int need)
{
	if (need <= B->text_cap) return 1;
	int nc = grow_cap(B->text_cap, TEXT_INIT, need);
	blk_lock();
	char          *t = realloc(B->text, (unsigned)nc);
	unsigned char *a = realloc(B->attr, (unsigned)nc);
	if (t) B->text = t;
	if (a) B->attr = a;
	if (t && a) B->text_cap = nc;
	blk_unlock();
	return t && a;
}

int ensure_links(int need)
{
	if (need <= B->links_cap) return 1;
	int nc = grow_cap(B->links_cap, LINKS_INIT, need);
	blk_lock();
	struct link *p = realloc(B->links, (unsigned)nc * sizeof *p);
	if (p) { B->links = p; B->links_cap = nc; }
	blk_unlock();
	return p != 0;
}

int ensure_imgs(int need)
{
	if (need <= B->imgs_cap) return 1;
	int nc = grow_cap(B->imgs_cap, IMGS_INIT, need);
	blk_lock();
	struct inl_img *p = realloc(B->imgs, (unsigned)nc * sizeof *p);
	if (p) { B->imgs = p; B->imgs_cap = nc; }
	blk_unlock();
	return p != 0;
}

/* The word-wrap line table is written and read only by the main thread, so it
 * grows without the lock (no cross-thread move to guard against). */
int ensure_lines(int need)
{
	if (need <= B->line_cap) return 1;
	int nc = grow_cap(B->line_cap, LINES_INIT, need);
	int *off = realloc(B->line_off, (unsigned)nc * sizeof *off);
	int *len = realloc(B->line_len, (unsigned)nc * sizeof *len);
	if (off) B->line_off = off;
	if (len) B->line_len = len;
	if (off && len) B->line_cap = nc;
	return off && len;
}

/* The app menu bar: "Page" and "View" laid out on one bar via the gfx menubar
 * helper (one open at a time). It also exercises the widget's per-item states:
 * "Stop" is DISABLED while idle, "Show Images" is a CHECKED toggle, and "Scroll"
 * is a submenu holding the scroll commands. */
static int show_images = 1;        /* "View > Show Images" checkbox state */

static const char *const page_items[] = { "Reload", "Home", "Stop" };
static const char *const page_hints[] = { "F5", "", "" };
static const unsigned char page_keys[] = { KEY_F5, 0, 0 };
static unsigned char page_flags[]      = { 0, 0, MENU_DISABLED };   /* Stop: idle */
static struct menu pagemenu = { "Page", page_items, 3, 0,
                                page_hints, page_keys, page_flags, 0 };

/* "View > Scroll" submenu (opened by the View "Scroll" item). */
static const char *const scroll_items[] = { "Top", "Bottom", "Page Up", "Page Down" };
static struct menu scrollmenu = { "Scroll", scroll_items, 4, 0, 0, 0, 0, 0 };

static const char *const view_items[] = { "Show Images", "Scroll" };
static unsigned char       view_flags[] = { MENU_CHECKED, 0 };      /* images on */
static struct menu *const  view_subs[]  = { 0, &scrollmenu };
static struct menu viewmenu = { "View", view_items, 2, 0,
                                0, 0, view_flags, view_subs };

static struct menu *const bar_menus[] = { &pagemenu, &viewmenu };
static struct menubar menubar = { bar_menus, 2, 2, 0 };

/* --- tabs ------------------------------------------------------------------ */
/* Each tab keeps its URL, label and scroll offset, plus — when it isn't the
 * active tab — a *snapshot* of its rendered page so switching back is instant
 * and never re-hits the network. Only the active tab's page lives in B (the
 * growable working buffers); an inactive tab's page is serialized into a single
 * heap blob the tab owns (malloc'd on cache, freed on restore/close). The blob
 * packs every variable-sized part — text, attr, img_pool, raw, the link list,
 * and the image list — back to back; the small fixed metadata (lengths, urls,
 * favicon) lives in the tab struct. A snapshot that can't be allocated falls
 * back to the old behaviour: re-fetch on select. There is no shared arena and no
 * eviction — each tab's blob is independent and demand-paged, so the only bound
 * on a cached page is available memory. The strip is the gfx tabbar widget;
 * this owns the list. */
#define MAX_TABS 8
struct tab {
	char url[URL_MAX];
	char label[40];
	int  scroll;

	int  cached;                 /* 1 = a page snapshot blob is held           */
	unsigned char *blob;         /* malloc'd snapshot, NULL when not cached     */
	int  blob_len;               /* its size in bytes                          */

	/* snapshot of B's per-page metadata (the buffers themselves go in blob) */
	char cur_url[URL_MAX];
	char doc_title[128];
	char ctype[80];
	int  mode, text_len, raw_len, img_pool_len, nlinks, n_imgs;

	/* the tab's favicon (decoded 16x16 ARGB), shown in the strip even while the
	 * tab is inactive — small enough to keep inline rather than in the blob. */
	unsigned favicon[FAVICON_W * FAVICON_H];
	int      favicon_ok;
};
static struct tab tabs[MAX_TABS];
static int n_tabs = 1;
static int active_tab = 0;

static void update_title(int id);   /* defined below; called from tab restore */

static void copy_str(char *d, const char *s, int cap)
{
	int i = 0;
	for (; s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = '\0';
}

/* Release a tab's cached snapshot. */
static void tab_drop(struct tab *t)
{
	if (t->blob) { free(t->blob); t->blob = 0; }
	t->cached = 0; t->blob_len = 0;
}

/* Snapshot the active tab's current page into its own heap blob (no-op while a
 * fetch is in flight or the page is blank — those just re-fetch on return). */
static void tab_cache_active(int i)
{
	struct tab *t = &tabs[i];
	t->scroll = B->scroll;
	if (B->loading || (B->mode == MODE_HTML && B->text_len == 0 && !B->cur_url[0])) {
		tab_drop(t);
		return;
	}

	int tl = B->text_len;
	int link_b = B->nlinks * (int)sizeof(struct link);
	int img_b  = B->n_imgs * (int)sizeof(struct inl_img);
	int need   = tl * 2 + B->img_pool_len + B->raw_len + link_b + img_b;

	tab_drop(t);
	unsigned char *blob = malloc((unsigned)(need > 0 ? need : 1));
	if (!blob) { t->cached = 0; return; }            /* too big: re-fetch later  */

	int o = 0;
	memcpy(blob + o, B->text,     tl);              o += tl;
	memcpy(blob + o, B->attr,     tl);              o += tl;
	memcpy(blob + o, B->img_pool, B->img_pool_len); o += B->img_pool_len;
	memcpy(blob + o, B->raw,      B->raw_len);       o += B->raw_len;
	memcpy(blob + o, B->links,    link_b);           o += link_b;
	memcpy(blob + o, B->imgs,     img_b);            o += img_b;

	t->cached = 1; t->blob = blob; t->blob_len = need;
	t->mode = B->mode; t->text_len = B->text_len; t->raw_len = B->raw_len;
	t->img_pool_len = B->img_pool_len; t->nlinks = B->nlinks; t->n_imgs = B->n_imgs;
	memcpy(t->cur_url, B->cur_url, sizeof t->cur_url);
	memcpy(t->doc_title, B->doc_title, sizeof t->doc_title);
	memcpy(t->ctype, B->ctype, sizeof t->ctype);
	memcpy(t->favicon, B->favicon, sizeof t->favicon);
	t->favicon_ok = B->favicon_ok;
}

/* Restore tab `i`'s snapshot into B (instant, no network), growing B's buffers
 * to fit. Frees the blob — the page is live in B now and re-cached on the next
 * switch away. A failed grow leaves a blank page (it'll re-fetch on demand). */
static void tab_restore(int i)
{
	struct tab *t = &tabs[i];
	int tl = t->text_len;
	int link_b = t->nlinks * (int)sizeof(struct link);
	int img_b  = t->n_imgs * (int)sizeof(struct inl_img);

	if (!ensure_text(tl) || !ensure_raw(t->raw_len) ||
	    !ensure_links(t->nlinks) || !ensure_imgs(t->n_imgs)) {
		tab_drop(t);
		B->mode = MODE_HTML; B->text_len = 0; B->nlines = 0;
		B->n_imgs = 0; B->nlinks = 0; B->wrap_seq = 0xFFFFFFFFu; B->seq++;
		return;
	}

	int o = 0;
	memcpy(B->text,     t->blob + o, tl);              o += tl;
	memcpy(B->attr,     t->blob + o, tl);              o += tl;
	memcpy(B->img_pool, t->blob + o, t->img_pool_len); o += t->img_pool_len;
	memcpy(B->raw,      t->blob + o, t->raw_len);       o += t->raw_len;
	memcpy(B->links,    t->blob + o, link_b);           o += link_b;
	memcpy(B->imgs,     t->blob + o, img_b);            o += img_b;

	B->mode = t->mode; B->text_len = t->text_len; B->raw_len = t->raw_len;
	B->img_pool_len = t->img_pool_len; B->nlinks = t->nlinks; B->n_imgs = t->n_imgs;
	memcpy(B->cur_url, t->cur_url, sizeof B->cur_url);
	memcpy(B->doc_title, t->doc_title, sizeof B->doc_title);
	memcpy(B->ctype, t->ctype, sizeof B->ctype);
	memcpy(B->favicon, t->favicon, sizeof B->favicon);
	B->favicon_ok = t->favicon_ok;

	B->loading = 0; B->req_pending = 0;
	B->scroll = t->scroll;
	B->wrap_seq = 0xFFFFFFFFu;            /* force a re-wrap at the live width */
	B->seq++;

	tab_drop(t);                          /* it's live in B now */

	textfield_set(&B->addr_tf, B->cur_url[0] ? B->cur_url : t->url);
	update_title(B->win_id);
}

/* --- worker thread: drains req_pending, fetches, bumps seq for the UI ----- */
/* 64 KiB: http_txn holds hdr[4096] + req[2048] + a cookie buffer, and the
 * redirect follower stacks several URL_MAX (2 KiB) scratch URLs; a too-small
 * stack overflows into B's .bss and corrupts it. Bumped from 32 KiB when
 * URL_MAX grew from 512 to 2048. */
static char         worker_stack[65536];
static volatile int worker_quit;

static void worker_fn(void)
{
	for (;;) {
		if (worker_quit)
			exit(0);
		if (B->req_pending) {
			B->req_pending = 0;
			B->loading = 1; B->seq++;
			do_fetch();
			B->loading = 0; B->seq++;
		} else if (fetch_lazy_visible()) {
			continue;                    /* a deferred image landed: look again */
		}
		msleep(30);
	}
}

/* --------------------------------------------------------------- word wrap */

/* The page body is drawn with the PROPORTIONAL sans-serif UI font (gfx_ui_*),
 * not the monospace cell grid: a glyph's pen advance is gfx_ui_cw(c), a line is
 * LINE_H tall, and wrapping/hit-testing measure in pixels. (The address bar, tabs
 * and menus already use this font.) */
#define LINE_H UIFONT_H

/* --- body font: the baked proportional UI font, OR a page @font-face -------- *
 * When the page loads a usable @font-face and applies it to the body (B->body_
 * font != NULL), body text is drawn with that custom face via user/lib/font;
 * otherwise it falls back to the baked gfx_ui font. Custom glyphs are rasterized
 * on demand and cached per ASCII codepoint (the body text is folded to 7-bit), at
 * a fixed pixel size, so a repaint only blits. The cache is main-thread-only;
 * B->body_font is swapped by the worker under blk_lock (see html.c). */
/* B->text holds UTF-8; the renderer decodes a codepoint at a time via the shared
 * utf8_decode/utf8_seqlen (user/lib). Per-codepoint glyph cache for the active
 * direct-mapped table (evict on collision, so a CJK page with hundreds of glyphs
 * stays bounded and a repaint only blits). Main-thread-only; B->body_font is
 * swapped by the worker under blk_lock (see html.c). */
#define BODY_PX 15.0f
#define GCACHE_N 1024
struct gcache { unsigned cp; unsigned char *bm; int w, h, ox, oy, adv; int valid; };
static struct gcache g_glyph[GCACHE_N];
static struct font  *g_gf;            /* the face the cache currently holds */
static int           g_baseline;      /* baseline offset (px) within a line */

static void gcache_sync(void)
{
	if (g_gf == B->body_font) return;
	for (int i = 0; i < GCACHE_N; i++) {
		free(g_glyph[i].bm); g_glyph[i].bm = 0; g_glyph[i].valid = 0;
	}
	g_gf = B->body_font;
	if (g_gf) { int asc = 0; font_vmetrics(g_gf, BODY_PX, &asc, 0, 0); g_baseline = asc; }
}

static struct gcache *gcache_get(unsigned cp)
{
	struct gcache *g = &g_glyph[cp & (GCACHE_N - 1)];
	if (g->valid && g->cp == cp) return g;
	if (g->valid) { free(g->bm); g->bm = 0; }
	int gi = font_glyph_index(B->body_font, (int)cp);
	g->bm = font_render_glyph(B->body_font, gi, BODY_PX, &g->w, &g->h, &g->ox, &g->oy, &g->adv);
	g->cp = cp; g->valid = 1;
	return g;
}

/* Pen advance of one codepoint in the active body font (baked UI font when no
 * custom face: ASCII/Latin-1 by value, a '?'-width box for anything above it). */
/* Raw, non-HTML content (text/plain, source, etc.) renders in the fixed-width
 * monospace cell font, not the proportional UI font — only HTML gets the latter. */
static int body_mono(void) { return B->mode == MODE_TEXT; }

static int body_cp_adv(unsigned cp)
{
	if (body_mono()) return GLYPH_W;              /* monospace cell             */
	if (!B->body_font) return gfx_ui_cw_cp(cp);   /* the gfx system-ui font     */
	gcache_sync();
	return gcache_get(cp)->adv;
}

static int body_width(const char *s)
{
	const char *end = s + strlen(s);
	if (!body_mono() && B->body_font) gcache_sync();
	int w = 0; unsigned cp;
	while (s < end) { s += utf8_decode(s, end, &cp); w += body_cp_adv(cp); }
	return w;
}

/* Alpha-blend one cached glyph's coverage in colour fg, clipped to [ct,cb). */
static void blit_glyph(int x, int y, const struct gcache *g, unsigned fg, int ct, int cb)
{
	if (!g->bm) return;
	unsigned *fb = win_fb();
	unsigned fr = (fg >> 16) & 0xff, fgc = (fg >> 8) & 0xff, fbb = fg & 0xff;
	for (int gy = 0; gy < g->h; gy++) {
		int py = y + g->oy + gy;
		if (py < ct || py >= cb || py < 0 || py >= BH) continue;
		const unsigned char *srow = g->bm + gy * g->w;
		unsigned *drow = fb + (unsigned long)py * BW;
		for (int gx = 0; gx < g->w; gx++) {
			int px = x + g->ox + gx;
			if (px < 0 || px >= BW) continue;
			unsigned a = srow[gx];
			if (!a) continue;
			if (a == 255) { drow[px] = fg; continue; }
			unsigned dst = drow[px], ia = 255 - a;
			unsigned r = (fr * a + ((dst >> 16) & 0xff) * ia) / 255;
			unsigned gg = (fgc * a + ((dst >> 8) & 0xff) * ia) / 255;
			unsigned bb = (fbb * a + (dst & 0xff) * ia) / 255;
			drow[px] = (r << 16) | (gg << 8) | bb;
		}
	}
}

/* A "tofu" box for a codepoint the active font (or the ASCII baked font) can't
 * draw, so missing glyphs are visible rather than silently dropped. */
static void draw_tofu(int x, int y, int w, unsigned fg)
{
	if (w > 4) gfx_rect(x + 1, y + 3, w - 3, UIFONT_H - 6, fg);
}

/* Draw a UTF-8 string at line-top (x,y) in colour fg, one codepoint at a time,
 * clipped to [ct,cb). Returns the pen x after the string. With a custom face,
 * glyphs come from font.c; otherwise the baked UI font draws ASCII/Latin-1 and a
 * tofu box stands in for anything it lacks. */
static int body_draw(int x, int y, const char *s, unsigned fg, int ct, int cb)
{
	const char *end = s + strlen(s);
	unsigned cp;
	if (body_mono()) {                            /* raw content: monospace cells */
		(void)ct; (void)cb;
		gfx_text(x, y, s, fg, GFX_TRANSPARENT);
		return x + body_width(s);
	}
	if (B->body_font) {
		gcache_sync();
		while (s < end) {
			s += utf8_decode(s, end, &cp);
			struct gcache *g = gcache_get(cp);
			if (g->bm) blit_glyph(x, y + g_baseline, g, fg, ct, cb);
			else if (cp > 0x20) draw_tofu(x, y, g->adv, fg);   /* .notdef / empty */
			x += g->adv;
		}
		return x;
	}
	/* No @font-face: delegate the whole UTF-8 run to the gfx system-ui font,
	 * which decodes UTF-8 and renders accents/Greek/Cyrillic from its cmap. */
	gfx_ui_text(x, y, s, fg);
	return x + gfx_ui_width(s);
}

static void add_line(int off, int len)
{
	if (!ensure_lines(B->nlines + 1))
		return;                       /* out of memory: stop adding lines */
	B->line_off[B->nlines] = off;
	B->line_len[B->nlines] = len;
	B->nlines++;
}

/* How many display rows an inline image reserves at the current pane width. */
/* Decide an inline image's drawn size (dw x dh px) within a pane `paw` px wide,
 * honouring the CSS/HTML width & height: both given pin the box (a single one
 * scales the other by the natural aspect ratio); with neither, the natural size
 * is used. The result is shrunk to fit the pane width, ratio preserved. dw/dh are
 * 0 when nothing can be sized yet (no explicit size and the image isn't decoded),
 * which the caller renders as a one-row alt box. Explicit sizes work even before
 * the image loads, so a lazy/slow image reserves its space and the page doesn't
 * reflow when it lands. */
static void img_disp_size(const struct inl_img *im, int paw, int *dw, int *dh)
{
	int natural = im->ok && im->iw && im->ih;
	int w = 0, h = 0;
	if (im->cw && im->ch) {
		w = im->cw; h = im->ch;
	} else if (im->cw) {
		w = im->cw;
		h = natural ? (int)((unsigned long)im->cw * im->ih / im->iw) : im->cw;
	} else if (im->ch) {
		h = im->ch;
		w = natural ? (int)((unsigned long)im->ch * im->iw / im->ih) : im->ch;
	} else if (natural) {
		w = (int)im->iw; h = (int)im->ih;
	} else {
		*dw = 0; *dh = 0; return;
	}
	if (w > paw) { h = (int)((long)h * paw / (w ? w : 1)); w = paw; }  /* fit width */
	*dw = w < 1 ? 1 : w;
	*dh = h < 1 ? 1 : h;
}

/* Display rows an inline image reserves, given the pane content width `paw` px. */
static int img_rows(const struct inl_img *im, int paw)
{
	if (paw < 1) paw = 1;
	int dw = 0, dh = 0;
	img_disp_size(im, paw, &dw, &dh);
	if (dh <= 0) return 1;                            /* alt box: one row       */
	int r = (dh + LINE_H - 1) / LINE_H;
	return r < 1 ? 1 : r;
}

/* Word-wrap B->text into display lines that fit `paw` pixels wide (proportional
 * font), reserving rows for inline images. wrap_cols caches the pane width. */
static void rewrap(int paw)
{
	if (paw < 8) paw = 8;
	if (B->wrap_seq == B->seq && B->wrap_cols == paw)
		return;
	for (int q = 0; q < B->n_imgs; q++) B->imgs[q].start_row = -1;
	B->nlines = 0;
	int i = 0;
	while (i < B->text_len) {
		int j = i;
		while (j < B->text_len && B->text[j] != '\n') j++;
		if (j == i) {
			int img = -1;
			for (int q = 0; q < B->n_imgs; q++)
				if (B->imgs[q].text_pos == i) { img = q; break; }
			if (img >= 0) {                           /* reserve the image rows  */
				struct inl_img *im = &B->imgs[img];
				im->rows = img_rows(im, paw);
				im->start_row = B->nlines;
				for (int r = 0; r < im->rows; r++)
					add_line(i, 0);
			} else {
				add_line(i, 0);
			}
		} else {
			int ls = i;
			while (ls < j) {
				/* greedy fit by pixel width; break at the last space if a word
				 * would overflow, else hard-break (one char always fits). */
				int w = 0, e = ls, lastsp = -1;
				while (e < j) {
					unsigned cp;
					int nb = utf8_decode(&B->text[e], &B->text[j], &cp);
					int cw = body_cp_adv(cp);
					if (w + cw > paw && e > ls) break;
					if (cp == ' ') lastsp = e;
					w += cw; e += nb;
				}
				int brk = (e < j && lastsp > ls) ? lastsp : e;
				add_line(ls, brk - ls);
				ls = brk;
				while (ls < j && B->text[ls] == ' ') ls++;
			}
		}
		i = j + 1;
	}
	B->wrap_seq = B->seq;
	B->wrap_cols = paw;
	if (B->scroll > B->nlines - 1) B->scroll = B->nlines - 1;
	if (B->scroll < 0) B->scroll = 0;
}

/* ------------------------------------------------------------- image draw */

/* PNG byte source: pulls straight out of the already-fetched body buffer, so
 * the decoder streams from B->raw without a second copy. */
struct rawrd { int pos; };
static int rawrd_get(void *c)
{
	struct rawrd *r = c;
	if (r->pos >= B->raw_len) return -1;
	return (unsigned char)B->raw[r->pos++];
}

/* Decode state: a fit-to-pane letterbox, blitting each PNG scanline directly
 * into the window buffer inside the content pane (mirrors imgview's v_row). */
struct imgblit {
	int px, py;                  /* pane top-left (window coords)            */
	unsigned iw, ih;             /* source image size                       */
	unsigned dw, dh, ox, oy;     /* drawn size + offset within the pane      */
	unsigned next_r;             /* next dest row awaiting its source row    */
};

/* Composite one RGBA source pixel over an existing framebuffer pixel. */
static unsigned blend_px(const unsigned char *p, unsigned dst)
{
	unsigned a = p[3];
	if (a == 255) return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
	if (a == 0)   return dst;
	unsigned ia = 255 - a;
	unsigned r = (p[0] * a + ((dst >> 16) & 0xff) * ia) / 255;
	unsigned g = (p[1] * a + ((dst >> 8) & 0xff) * ia) / 255;
	unsigned b = (p[2] * a + (dst & 0xff) * ia) / 255;
	return (r << 16) | (g << 8) | b;
}

static int img_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
	(void)w;
	struct imgblit *v = c;
	unsigned *fb = win_fb();
	while (v->next_r < v->dh && (v->next_r * v->ih / v->dh) == y) {
		unsigned *drow = fb + (unsigned long)(v->py + v->oy + v->next_r) * BW
		                 + v->px + v->ox;
		for (unsigned x = 0; x < v->dw; x++) {
			const unsigned char *p = rgba + (x * v->iw / v->dw) * 4;
			drow[x] = blend_px(p, drow[x]);
		}
		v->next_r++;
	}
	return 0;
}

/* Decode B->raw as a PNG into the content pane (px,py,pw,ph), fit-to-pane with
 * aspect preserved. Re-decoded each repaint — cheap enough, and keeps no whole
 * decoded image in RAM. */
static void draw_image(int px, int py, int pw, int ph, unsigned bg)
{
	struct rawrd r = { 0 };
	unsigned iw = 0, ih = 0;
	if (upng_dimensions(rawrd_get, &r, &iw, &ih) != 0 || !iw || !ih) {
		gfx_ui_text(px + 8, py + 8,
		            "Cannot decode image (unreadable or oversized interlaced PNG).",
		            0xb04040);
		return;
	}
	struct imgblit v;
	v.px = px; v.py = py; v.iw = iw; v.ih = ih; v.next_r = 0;
	if (iw <= (unsigned)pw && ih <= (unsigned)ph) {
		v.dw = iw; v.dh = ih;                          /* fits: show 1:1     */
	} else if ((unsigned long)iw * ph > (unsigned long)ih * pw) {
		v.dw = pw; v.dh = ih * pw / iw;                /* width-bound        */
	} else {
		v.dh = ph; v.dw = iw * ph / ih;                /* height-bound       */
	}
	if (!v.dw) v.dw = 1;
	if (!v.dh) v.dh = 1;
	v.ox = ((unsigned)pw - v.dw) / 2;
	v.oy = ((unsigned)ph - v.dh) / 2;

	struct rawrd r2 = { 0 };
	upng_decode(rawrd_get, &r2, img_row, &v, 0, 0);
}

/* ------------------------------------------------------- inline page images */

/* Byte reader over an inline image's PNG bytes in img_pool. */
struct poolrd { int pos, off, len; };
static int poolrd_get(void *c)
{
	struct poolrd *r = c;
	return r->pos < r->len ? B->img_pool[r->off + r->pos++] : -1;
}

/* Blit one inline image left-aligned at (x0,y0), width dw, height dh, clipped
 * vertically to the content pane [clip_top, clip_bot) so it scrolls cleanly. */
struct inlblit {
	int x0, y0, dw, dh, clip_top, clip_bot;
	unsigned iw, ih, next_r;
};

static int inl_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
	(void)w;
	struct inlblit *v = c;
	unsigned *fb = win_fb();
	while (v->next_r < (unsigned)v->dh && (v->next_r * v->ih / (unsigned)v->dh) == y) {
		int dy = v->y0 + (int)v->next_r;
		if (dy >= v->clip_top && dy < v->clip_bot) {
			unsigned *drow = fb + (unsigned long)dy * BW + v->x0;
			for (int x = 0; x < v->dw; x++) {
				const unsigned char *p = rgba + ((unsigned)x * v->iw / (unsigned)v->dw) * 4;
				drow[x] = blend_px(p, drow[x]);
			}
		}
		v->next_r++;
	}
	return 0;
}

static void draw_inline_image(struct inl_img *im, int py, int phh, int paw)
{
	if (paw < 1) paw = 1;
	int dw = 0, dh = 0;
	img_disp_size(im, paw, &dw, &dh);
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;

	struct inlblit v;
	v.x0 = 4;
	v.y0 = py + (im->start_row - B->scroll) * LINE_H;
	v.dw = dw; v.dh = dh; v.clip_top = py; v.clip_bot = py + phh;
	v.iw = im->iw; v.ih = im->ih; v.next_r = 0;

	if (im->is_argb) {                 /* SVG: raw RGBA in img_pool, scaled blit */
		const unsigned char *src = B->img_pool + im->coff;
		unsigned *fb = win_fb();
		for (int ry = 0; ry < dh; ry++) {
			int dy = v.y0 + ry;
			if (dy < v.clip_top || dy >= v.clip_bot) continue;
			unsigned sy = (unsigned)ry * im->ih / (unsigned)dh;
			const unsigned char *srow = src + (unsigned long)sy * im->iw * 4;
			unsigned *drow = fb + (unsigned long)dy * BW + v.x0;
			for (int x = 0; x < dw; x++) {
				const unsigned char *p = srow + ((unsigned)x * im->iw / (unsigned)dw) * 4;
				drow[x] = blend_px(p, drow[x]);
			}
		}
		return;
	}

	struct poolrd r = { 0, im->coff, im->clen };
	upng_decode(poolrd_get, &r, inl_row, &v, 0, 0);
}

/* A placeholder box for an image that couldn't be fetched/decoded. */
static void draw_alt_box(struct inl_img *im, int py, int phh)
{
	int y0 = py + (im->start_row - B->scroll) * LINE_H;
	if (y0 < py || y0 + LINE_H > py + phh) return;         /* keep it in-pane    */
	const char *t = im->alt[0] ? im->alt : "[image]";
	int w = gfx_ui_width(t) + 8;
	gfx_fill_rect(4, y0, w, LINE_H, 0xe6e9ef);
	gfx_rect(4, y0, w, LINE_H, 0xb4bac4);
	gfx_ui_text(8, y0 + 1, t, 0x7a828c);
}

/* --------------------------------------------------------------- drawing */

/* Draw one wrapped display line in the body font (the page @font-face when set,
 * else the baked gfx_ui font), clipped vertically to [ct,cb). The pane bg is
 * already filled, so the text blends over existing pixels. Bold is a 1px-offset
 * overstrike; underline is a rule under the run. */
static void draw_styled_line(int x, int y, int idx, int ct, int cb)
{
	int off = B->line_off[idx], len = B->line_len[idx];
	int i = 0;
	while (i < len) {
		unsigned char a = B->attr[off + i];
		char run[260]; int rl = 0;
		/* Accumulate whole codepoints (a multibyte char's bytes share one attr),
		 * so the run boundary never splits a UTF-8 sequence into garbage. */
		while (i < len && B->attr[off + i] == a) {
			int nb = utf8_seqlen((unsigned char)B->text[off + i]);
			if (i + nb > len) nb = 1;
			if (rl + nb > (int)sizeof run - 1) break;
			for (int b = 0; b < nb; b++) run[rl++] = B->text[off + i++];
		}
		run[rl] = '\0';
		unsigned col = palette[a & ST_COLOR];
		body_draw(x, y, run, col, ct, cb);
		if (a & ST_BOLD) body_draw(x + 1, y, run, col, ct, cb);
		int rw = body_width(run);
		if (a & ST_UNDER) gfx_fill_rect(x, y + LINE_H - 4, rw, 1, col);
		x += rw;
	}
}

static void render(void)
{
	unsigned bg = 0xf4f5f7;

	/* Hold off any buffer-relocating realloc on the worker thread while we read
	 * text/attr/line_off/img_pool/imgs/raw below (see blk_lock in browser.h).
	 * render() does only in-memory gfx work — no syscalls that block — so the
	 * worker is never stalled for long. */
	blk_lock();

	/* tab strip (between the menu bar and the address bar). Each tab passes its
	 * decoded favicon, or NULL so the widget draws the default globe. */
	const char *labels[MAX_TABS];
	const unsigned *icons[MAX_TABS];
	for (int i = 0; i < n_tabs; i++) {
		labels[i] = tabs[i].label[0] ? tabs[i].label : "New Tab";
		icons[i]  = tabs[i].favicon_ok ? tabs[i].favicon : 0;
	}
	tabbar_draw(0, MENU_H, BW, labels, icons, n_tabs, active_tab);

	/* address bar (below the tab strip): a shared text-field widget */
	gfx_fill_rect(0, MENU_H + TAB_H, BW, ADDR_H, 0x222831);
	int fx = 6, fy = MENU_H + TAB_H + 4, fw = BW - 12, fh = ADDR_H - 8;
	textfield_draw(&B->addr_tf, fx, fy, fw, fh, 1);

	/* content pane */
	int py = MENU_H + TAB_H + ADDR_H, pw = BW - SCROLL_W;
	int phh = BH - MENU_H - TAB_H - ADDR_H; if (phh < 0) phh = 0;
	gfx_fill_rect(0, py, pw, phh, bg);
	int rows = phh / LINE_H;

	if (B->mode == MODE_IMAGE) {
		if (B->loading)
			gfx_ui_text(8, py + 8, "Loading ...", 0x4060a0);
		else
			draw_image(0, py, pw, phh, bg);
		scrollbar_draw(pw, py, SCROLL_W, phh, 0, 0, 0);  /* inert scrollbar */
	} else {
		int paw = pw - 8;                          /* content width, px */
		rewrap(paw);
		if (B->loading && B->nlines == 0) {
			gfx_ui_text(8, py + 8, "Loading ...", 0x4060a0);
		} else if (B->nlines == 0) {
			gfx_ui_text(8, py + 8, "Type a URL above and press Enter.", 0x707880);
			gfx_ui_text(8, py + 8 + UIFONT_H, "e.g. example.com  or  https://example.com",
			            0x9098a0);
		}
		for (int r = 0; r < rows; r++) {
			int idx = B->scroll + r;
			if (idx >= B->nlines) break;
			draw_styled_line(4, py + r * LINE_H, idx, py, py + phh);
		}

		/* inline images, over their reserved (blank) rows */
		for (int q = 0; q < B->n_imgs; q++) {
			struct inl_img *im = &B->imgs[q];
			if (im->start_row < 0) continue;
			int r0 = im->start_row;
			int r1 = r0 + (im->rows > 0 ? im->rows : 1);
			if (r1 <= B->scroll || r0 >= B->scroll + rows) continue;  /* off-screen */
			/* On-screen now: a deferred loading="lazy" image gets flagged so the
			 * worker fetches it (no-op once fetched). */
			if (im->lazy && !im->fetched && show_images) im->want = 1;
			if (im->ok && show_images) draw_inline_image(im, py, phh, paw);
			else                       draw_alt_box(im, py, phh);
		}

		/* scrollbar */
		scrollbar_draw(pw, py, SCROLL_W, phh, B->scroll, B->nlines, rows);
	}

	/* App menu bar over the top, then the dropdowns last so they overlay the page.
	 * "Stop" is only meaningful while a fetch is in flight, so disable it when
	 * idle — the widget dims it and ignores clicks/accelerators. */
	page_flags[2] = B->loading ? 0 : MENU_DISABLED;
	gfx_fill_rect(0, 0, BW, MENU_H, 0x232a36);
	menubar_draw(&menubar);

	blk_unlock();
}

/* --------------------------------------------------------------- input */

static void pane_metrics(int *py, int *phh, int *rows)
{
	*py = MENU_H + TAB_H + ADDR_H;
	*phh = BH - MENU_H - TAB_H - ADDR_H; if (*phh < 0) *phh = 0;
	*rows = (*phh > 0) ? *phh / LINE_H : 1;
}

static void navigate(const char *url)
{
	textfield_set(&B->addr_tf, url);          /* writes into B->addr (its buffer) */
	int i = 0;
	for (; url[i] && i < URL_MAX - 1; i++) B->req_url[i] = url[i];
	B->req_url[i] = '\0';
	B->req_pending = 1; B->seq++;
}

static void submit(void)
{
	if (B->addr_tf.len == 0) return;
	int i = 0;
	for (; B->addr[i] && i < URL_MAX - 1; i++) B->req_url[i] = B->addr[i];
	B->req_url[i] = '\0';
	B->req_pending = 1; B->seq++;
}

/* Reset B to an empty page (a fresh/blank tab) without touching the net. */
static void show_blank(void)
{
	textfield_set(&B->addr_tf, "");
	B->cur_url[0] = '\0'; B->doc_title[0] = '\0';
	B->mode = MODE_HTML;
	B->text_len = 0; B->nlines = 0; B->n_imgs = 0; B->nlinks = 0;
	B->wrap_seq = 0xFFFFFFFF;
	B->favicon_ok = 0;                          /* a blank tab shows the globe */
	tabs[active_tab].favicon_ok = 0;
	B->seq++;
}

/* Pull the freshly-loaded page's url + a label back into the active tab (called
 * when a fetch finishes — only the active tab ever loads). */
static void tab_sync_active(void)
{
	struct tab *t = &tabs[active_tab];
	copy_str(t->url, B->cur_url, sizeof t->url);
	if (B->doc_title[0]) {
		copy_str(t->label, B->doc_title, sizeof t->label);
	} else {
		struct purl u;
		if (B->cur_url[0] && parse_url(B->cur_url, &u))
			copy_str(t->label, u.host, sizeof t->label);
		else
			copy_str(t->label, "Untitled", sizeof t->label);
	}
	memcpy(t->favicon, B->favicon, sizeof t->favicon);   /* freshly decoded icon */
	t->favicon_ok = B->favicon_ok;
}

/* Reveal tab `i`: restore its cached page instantly, else (re)load or blank. */
static void reveal_tab(int i)
{
	B->scroll = tabs[i].scroll;
	if (tabs[i].cached)        tab_restore(i);
	else if (tabs[i].url[0])   navigate(tabs[i].url);
	else                       show_blank();
}

/* Make tab `i` the active one. The page we're leaving is snapshotted so coming
 * back is instant; the one we're entering is restored from its snapshot (no
 * network) when present, or loaded as before. */
static void select_tab(int i)
{
	if (i < 0 || i >= n_tabs || i == active_tab) return;
	tab_cache_active(active_tab);            /* remember where we were */
	active_tab = i;
	reveal_tab(i);
}

static void new_tab(void)
{
	if (n_tabs >= MAX_TABS) { B->seq++; return; }
	tab_cache_active(active_tab);
	active_tab = n_tabs++;
	tabs[active_tab].url[0] = '\0';
	tabs[active_tab].scroll = 0;
	tabs[active_tab].cached = 0;
	tabs[active_tab].blob = 0;
	tabs[active_tab].blob_len = 0;
	copy_str(tabs[active_tab].label, "New Tab", sizeof tabs[active_tab].label);
	show_blank();
}

static void close_tab(int i)
{
	if (i < 0 || i >= n_tabs || n_tabs <= 1) return;   /* keep at least one */
	int was_active = (i == active_tab);
	tab_drop(&tabs[i]);                           /* free its snapshot first */
	for (int j = i; j < n_tabs - 1; j++) {       /* shift the rest down a slot */
		char *d = (char *)&tabs[j], *s = (char *)&tabs[j + 1];
		for (int k = 0; k < (int)sizeof(struct tab); k++) d[k] = s[k];
	}
	tabs[n_tabs - 1].blob = 0;                    /* vacated slot now aliases the */
	tabs[n_tabs - 1].cached = 0;                  /* moved blob — drop the alias  */
	n_tabs--;
	if (active_tab > i)          active_tab--;
	else if (active_tab >= n_tabs) active_tab = n_tabs - 1;
	if (was_active)                                   /* reveal the neighbour */
		reveal_tab(active_tab);
	B->seq++;
}

/* Run item `item` of leaf menu `m` (a bar menu or one of its submenus — the
 * menubar helper reports which one, so we dispatch by pointer). Reload is also
 * reached via the F5 accelerator. */
static void menu_action(struct menu *m, int item)
{
	int py, phh, rows;
	pane_metrics(&py, &phh, &rows);
	if (m == &pagemenu) {
		switch (item) {
		case 0:                                  /* Reload current page     */
			if (B->cur_url[0]) navigate(B->cur_url);
			break;
		case 1: navigate("example.com"); break;  /* Home                    */
		case 2:                                  /* Stop a queued fetch     */
			B->req_pending = 0; B->seq++;
			break;
		}
	} else if (m == &viewmenu) {
		if (item == 0) {                         /* Show Images checkbox    */
			view_flags[0] ^= MENU_CHECKED;
			show_images = (view_flags[0] & MENU_CHECKED) != 0;
			B->seq++;
		}                                        /* item 1 = Scroll submenu */
	} else if (m == &scrollmenu) {
		switch (item) {
		case 0: B->scroll = 0; B->seq++; break;          /* Top             */
		case 1:                                          /* Bottom          */
			B->scroll = B->nlines - rows;
			if (B->scroll < 0) B->scroll = 0;
			B->seq++;
			break;
		case 2:                                          /* Page Up         */
			B->scroll -= rows;
			if (B->scroll < 0) B->scroll = 0;
			B->seq++;
			break;
		case 3:                                          /* Page Down       */
			B->scroll += rows;
			if (B->scroll > B->nlines - 1) B->scroll = B->nlines - 1;
			if (B->scroll < 0) B->scroll = 0;
			B->seq++;
			break;
		}
	}
}

static void handle_click(int mx, int my)
{
	if (my < MENU_H || pagemenu.open || viewmenu.open) {   /* menu bar / dropdown */
		struct menu *leaf;
		int item;
		if (menubar_click(&menubar, mx, my, &leaf, &item)) {
			if (item >= 0)
				menu_action(leaf, item);
			B->seq++;
			return;
		}
	}
	if (my >= MENU_H && my < MENU_H + TAB_H) {              /* tab strip */
		int idx = 0;
		switch (tabbar_hit(0, MENU_H, BW, n_tabs, mx, my, &idx)) {
		case TABBAR_SELECT: select_tab(idx); break;
		case TABBAR_CLOSE:  close_tab(idx);  break;
		case TABBAR_NEW:    new_tab();       break;
		}
		B->seq++;
		return;
	}
	if (my >= MENU_H + TAB_H && my < MENU_H + TAB_H + ADDR_H) {   /* address bar */
		textfield_click(&B->addr_tf, mx, my,
		                6, MENU_H + TAB_H + 4, BW - 12, ADDR_H - 8);
		B->seq++;
		return;
	}
	int py, phh, rows;
	pane_metrics(&py, &phh, &rows);
	int sbpart = scrollbar_hit(BW - SCROLL_W, py, SCROLL_W, phh, mx, my);
	if (sbpart != SCROLLBAR_NONE) {                          /* scrollbar */
		if (sbpart == SCROLLBAR_UP)        B->scroll -= 3;
		else if (sbpart == SCROLLBAR_DOWN) B->scroll += 3;
		else B->scroll = scrollbar_pos(BW - SCROLL_W, py, SCROLL_W, phh, mx, my,
		                               B->scroll, B->nlines, rows);
		if (B->scroll > B->nlines - 1) B->scroll = B->nlines - 1;
		if (B->scroll < 0) B->scroll = 0;
		B->seq++;
		return;
	}
	if (my >= py && my < py + phh) {                        /* a link? */
		int row = B->scroll + (my - py) / LINE_H;
		/* The link/line tables can be moved by a worker realloc mid-load; hold
		 * the interlock while we scan them and copy the target href out. */
		char res[URL_MAX]; res[0] = '\0';
		blk_lock();
		if (row >= 0 && row < B->nlines) {
			/* proportional: walk the line's glyphs to find which char the click
			 * (mx, relative to the 4px left margin) falls on. */
			int len = B->line_len[row], px = mx - 4, acc = 0, col = -1;
			int base = B->line_off[row];
			for (int k = 0; k < len; ) {
				unsigned cp;
				int nb = utf8_decode(&B->text[base + k], &B->text[base + len], &cp);
				int cw = body_cp_adv(cp);
				if (px < acc + cw) { col = k; break; }
				acc += cw; k += nb;
			}
			if (col >= 0) {
				int off = B->line_off[row] + col;
				for (int i = 0; i < B->nlinks; i++)
					if (off >= B->links[i].start && off < B->links[i].end) {
						resolve_url(B->cur_url, B->links[i].href, res, sizeof res);
						break;
					}
			}
		}
		blk_unlock();
		if (res[0]) { navigate(res); return; }
	}
}

static void handle_key(char c)
{
	/* Menu accelerator (F5 = Reload): fire the item like a click. */
	struct menu *leaf;
	int sc = menubar_shortcut(&menubar, (unsigned char)c, &leaf);
	if (sc >= 0) { menubar_close(&menubar); menu_action(leaf, sc); return; }
	if ((unsigned char)c == 27) { menubar_close(&menubar); B->seq++; return; }

	/* The address-bar widget handles editing (printable/Backspace/Delete/caret). */
	if (textfield_key(&B->addr_tf, c)) { B->seq++; return; }

	switch ((unsigned char)c) {
	case '\n': case '\r': submit(); return;
	case KEY_UP:
		B->scroll -= 3; if (B->scroll < 0) B->scroll = 0; B->seq++; return;
	case KEY_DOWN:
		B->scroll += 3;
		if (B->scroll > B->nlines - 1) B->scroll = B->nlines - 1;
		if (B->scroll < 0) B->scroll = 0;
		B->seq++; return;
	}
}

/* The window's owner sets the title once a fetch finishes (worker can't). */
static void update_title(int id)
{
	if (B->doc_title[0]) { win_settitle(id, B->doc_title); return; }
	struct purl u;
	if (parse_url(B->cur_url, &u)) {
		char t[80]; int ti = 0;
		sapp(t, &ti, sizeof t, "browser - ");
		sapp(t, &ti, sizeof t, u.host);
		win_settitle(id, t);
	}
}

int main(void)
{
	int id = win_create(BW, BH, "browser");
	if (id < 0) { puts("browser: no window\n"); return 1; }
	B->win_id = id;
	gfx_init(win_fb(), BW, BH);

	textfield_init(&B->addr_tf, B->addr, URL_MAX);
	textfield_set(&B->addr_tf, "example.com");
	B->wrap_seq = 0xFFFFFFFF;

	jar_load();   /* restore cookies + HSTS from the persistent SQLite db */

	int wtid = thread_create(worker_fn, &worker_stack[sizeof worker_stack]);

	unsigned last = B->seq - 1;
	int prev_loading = 0;
	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev)) {
			if (ev.type == WEV_CLOSE) {
				worker_quit = 1;
				if (wtid >= 0) thread_join(wtid);   /* let the worker exit first */
				win_close(id);
				return 0;
			} else if (ev.type == WEV_KEY) {
				handle_key((char)ev.a);
			} else if (ev.type == WEV_MOUSE) {
				handle_click(ev.a, ev.b);
			} else if (ev.type == WEV_SCROLL) {
				B->scroll -= ev.a * 3;       /* wheel up scrolls back */
				if (B->scroll > B->nlines - 1) B->scroll = B->nlines - 1;
				if (B->scroll < 0) B->scroll = 0;
				B->seq++;
			} else if (ev.type == WEV_RESIZE) {
				BW = ev.a; BH = ev.b;
				gfx_init(win_fb(), BW, BH);
				B->seq++;            /* re-wrap + repaint at the new width */
			}
		}
		if (B->loading && !prev_loading)             /* fetch started */
			win_cursor(id, WIN_CURSOR_BUSY);         /* spinner while loading */
		if (prev_loading && !B->loading) {           /* fetch finished */
			win_cursor(id, WIN_CURSOR_ARROW);
			tab_sync_active();                       /* refresh the tab label */
			update_title(id);
			if (B->cur_url[0])                       /* show the resolved URL */
				textfield_set(&B->addr_tf, B->cur_url);  /* (incl. the scheme) */
		}
		prev_loading = B->loading;

		if (B->seq != last) { render(); win_present(id); last = B->seq; }
		win_wait(id);
	}
}
