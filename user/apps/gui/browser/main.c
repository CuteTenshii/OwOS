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
	unsigned char *p = realloc(B->bp,  (unsigned)nc);
	unsigned char *g = realloc(B->bgi, (unsigned)nc);
	if (t) B->text = t;
	if (a) B->attr = a;
	if (p) B->bp  = p;
	if (g) B->bgi = g;
	if (t && a && p && g) B->text_cap = nc;
	blk_unlock();
	return t && a && p && g;
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
#define HIST_MAX 32      /* per-tab back/forward history depth */
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

	/* Per-tab navigation history: URLs landed on, with hist_i the current entry.
	 * Back/Next move hist_i and re-navigate; a fresh navigation truncates any
	 * forward entries. Entries are malloc'd; the oldest is dropped when full. */
	char    *hist[HIST_MAX];
	int      hist_n;             /* number of entries in use                    */
	int      hist_i;             /* index of the current page (0..hist_n-1)     */

	struct dt_view dt;           /* this tab's devtools view (panel/scroll/open) */
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

/* Set while a fetch was started by Back/Next so its completion doesn't push a new
 * history entry (we're moving within the existing list, not extending it). */
static int nav_from_history;

/* Free a tab's whole history (on close). */
static void hist_clear(struct tab *t)
{
	for (int i = 0; i < t->hist_n; i++) { free(t->hist[i]); t->hist[i] = 0; }
	t->hist_n = 0; t->hist_i = 0;
}

/* Record `url` as the tab's current page. A reload of the same page is ignored;
 * any forward entries are dropped; the oldest is evicted when the list is full. */
static void hist_push(struct tab *t, const char *url)
{
	if (!url || !url[0]) return;
	if (t->hist_n > 0 && streq(t->hist[t->hist_i], url)) return;   /* same page */

	for (int i = t->hist_i + 1; i < t->hist_n; i++) { free(t->hist[i]); t->hist[i] = 0; }
	t->hist_n = t->hist_i + 1;

	int n = (int)strlen(url) + 1;
	char *dup = malloc(n);
	if (!dup) return;                                  /* OOM: just don't record */
	memcpy(dup, url, n);

	if (t->hist_n >= HIST_MAX) {                        /* full: drop the oldest */
		free(t->hist[0]);
		for (int i = 1; i < t->hist_n; i++) t->hist[i - 1] = t->hist[i];
		t->hist_n--;
	}
	t->hist[t->hist_n++] = dup;
	t->hist_i = t->hist_n - 1;
}

static int hist_can_back(const struct tab *t) { return t->hist_i > 0; }
static int hist_can_fwd(const struct tab *t)  { return t->hist_i < t->hist_n - 1; }

/* Snapshot the active tab's current page into its own heap blob (no-op while a
 * fetch is in flight or the page is blank — those just re-fetch on return). */
static void tab_cache_active(int i)
{
	struct tab *t = &tabs[i];
	/* We're leaving this page; its live DOM/JS go away (the worker frees them).
	 * Snapshots are of the flat buffers only, so interactivity doesn't persist. */
	B->drop_dom = 1; B->events_live = 0;
	t->scroll = B->scroll;
	devtools_view_save(&t->dt);              /* remember this tab's devtools view */
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
	B->drop_dom = 1; B->events_live = 0;   /* restored from a snapshot: static page */
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

static void worker_navigate(const char *url)
{
	int k = 0;
	for (; url[k] && k < URL_MAX - 1; k++) B->req_url[k] = url[k];
	B->req_url[k] = '\0';
	B->req_pending = 1; B->seq++;
}

/* Submit the form enclosing `from`: fire 'submit', and unless prevented navigate
 * to the action with the form's field values. Returns 1 if a form was found. */
static int worker_submit(struct dom_node *from)
{
	char url[URL_MAX];
	struct dom_node *form;
	for (form = from; form; form = form->parent) if (streq(form->tag, "form")) break;
	if (!form) return 0;
	if (!js_dispatch_dom_event(form, "submit", 0, 0) &&
	    html_form_submit_url(from, url, sizeof url) && url[0])
		worker_navigate(url);
	return 1;
}

/* Perform a clicked control's default action: toggle/cycle/focus/submit, firing
 * the matching DOM events. Returns 1 if the page should re-flatten afterwards. */
static int activate_control(struct dom_node *c, int kind)
{
	switch (kind) {
	case CTRL_CHECKBOX:
		html_toggle_checked(c);
		js_dispatch_dom_event(c, "change", 0, 0);
		html_set_focus(0);
		return 1;
	case CTRL_RADIO:
		html_radio_select(c);
		js_dispatch_dom_event(c, "change", 0, 0);
		html_set_focus(0);
		return 1;
	case CTRL_SELECT:
		html_select_cycle(c);
		js_dispatch_dom_event(c, "change", 0, 0);
		html_set_focus(0);
		return 1;
	case CTRL_TEXT:
		html_set_focus(c);
		return 1;
	case CTRL_TEXTAREA:
		if (!dom_get_attr(c, "value")) html_seed_textarea(c);
		html_set_focus(c);
		return 1;
	case CTRL_SUBMIT:
		html_set_focus(0);
		worker_submit(c);
		return 1;
	default:
		html_set_focus(0);
		return 1;
	}
}

/* Run a queued content-pane event through the live JS DOM (worker-thread only —
 * the engine is single-threaded). For a click: resolve the target node, dispatch
 * the click, then unless a handler called preventDefault perform the default
 * action (activate a form control, or follow a link). For keyinput: edit the
 * focused text control and fire an 'input' event. Re-flatten if anything changed. */
static void worker_dispatch_event(void)
{
	unsigned mseq = dom_mutation_seq;
	int need_relayout = 0;
	B->evt_pending = 0;

	if (streq(B->evt_type, "blur")) {
		if (html_focus()) { html_set_focus(0); html_relayout(); }
		return;
	}
	if (streq(B->evt_type, "keyinput")) {
		struct dom_node *f = html_focus();                /* evt_x carries the char */
		int ch = B->evt_x;
		if (f && (ch == '\n' || ch == '\r') && html_control_kind(f) == CTRL_TEXT) {
			worker_submit(f);                             /* Enter in a single-line field */
			need_relayout = 1;
		} else if ((f = html_focus_edit(ch)) != 0) {
			js_dispatch_dom_event(f, "input", 0, 0);
			need_relayout = 1;
		}
	} else {                                               /* a click */
		struct dom_node *t = html_node_at_offset(B->evt_off), *ctrl;
		int prevented = js_dispatch_dom_event(t, "click", B->evt_x, B->evt_y);
		if (!prevented) {
			int kind = CTRL_NONE;
			for (ctrl = t; ctrl; ctrl = ctrl->parent)
				if ((kind = html_control_kind(ctrl)) != CTRL_NONE) break;
			if (ctrl) need_relayout |= activate_control(ctrl, kind);
			else {
				if (html_focus()) { html_set_focus(0); need_relayout = 1; }   /* blur */
				if (B->evt_text[0]) worker_navigate(B->evt_text);             /* link */
			}
		}
	}

	if (dom_mutation_seq != mseq || need_relayout) {
		html_relayout();
		if (B->n_imgs > 0) fetch_inline_images(B->cur_url);
	}
	B->events_live = js_has_listeners() || html_has_controls();
}

static void worker_fn(void)
{
	for (;;) {
		if (worker_quit)
			exit(0);
		if (B->drop_dom) {                   /* main switched away: free the page  */
			B->drop_dom = 0;
			B->events_live = 0;
			html_page_teardown();
		}
		if (B->recascade) {                  /* window resized: re-run the cascade  */
			B->recascade = 0;
			html_relayout();                 /* re-evaluates @media at the new size */
			if (B->n_imgs > 0) fetch_inline_images(B->cur_url);
			B->seq++;
		}
		if (B->req_pending) {
			B->req_pending = 0;
			B->abort_load = 0;            /* a fresh navigation clears any pending Stop */
			B->loading = 1; B->seq++;
			do_fetch();
			B->loading = 0; B->abort_load = 0;
			B->events_live = js_has_listeners() || html_has_controls();
			B->seq++;
		} else if (B->evt_pending) {
			worker_dispatch_event();
		} else if (fetch_lazy_visible()) {
			continue;                    /* a deferred image landed: look again */
		}
		/* Drive the page's timers (setTimeout/setInterval/rAF) and live
		 * WebSockets. A fired callback may mutate the DOM — relayout + repaint
		 * when it did. */
		{
			unsigned mseq = dom_mutation_seq;
			if (js_timers_pump() + js_websockets_pump()) {
				if (dom_mutation_seq != mseq) {
					html_relayout();
					if (B->n_imgs > 0) fetch_inline_images(B->cur_url);
				}
				B->seq++;
			}
		}
		/* Load any <iframe> documents the last layout discovered: fetch each,
		 * check it against X-Frame-Options / CSP frame-ancestors, then re-flatten
		 * so the allowed ones render in place of their placeholders. A frame's own
		 * content may itself hold iframes, surfaced on a later pass. */
		if (!B->req_pending && !B->loading && html_iframes_pending()) {
			fetch_iframes();
			html_relayout();
			if (B->n_imgs > 0) fetch_inline_images(B->cur_url);
			B->seq++;
		}

		/* Rebuild the devtools DOM/Styles snapshot from the live tree (this thread
		 * owns it) when the panel is open and the page changed. No-op when closed. */
		devtools_worker_tick();
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

/* Heading pixel sizes (BP_SIZE 1..6 = h1..h6); 0 = ordinary body text, 7 = a
 * horizontal rule. Kept within LINE_H (18) so a taller heading still fits one
 * row — headings also come bold (stamped in html.c), which carries the weight. */
static const int g_size_px[8] = { 0, 18, 17, 16, 15, 14, 13, 0 };

/* Block context for the paragraph currently being measured/drawn, decoded from a
 * glyph's BP_* byte by set_block(): a heading px (0 = default UI font), monospace
 * (<pre>), centring, and an indent level (blockquote/list nesting). */
static int cur_bpx;                   /* heading px, 0 = default body font   */
static int cur_bmono;                 /* <pre>: render in the monospace cell */
static int cur_bcenter;               /* text-align:center / <center>        */
static int cur_bindent;               /* indent level 0..7                   */
static int cur_bhr;                   /* this line is a horizontal rule      */

/* Indent width in pixels for one nesting level. */
#define INDENT_PX 24

static void set_block(int off)
{
	unsigned char bp = (off >= 0 && off < B->text_len) ? B->bp[off] : 0;
	int size = bp & BP_SIZE;
	cur_bhr     = (size == 7);
	cur_bpx     = (size >= 1 && size <= 6) ? g_size_px[size] : 0;
	cur_bmono   = (bp & BP_MONO) ? 1 : 0;
	cur_bcenter = (bp & BP_CENTER) ? 1 : 0;
	cur_bindent = (bp & BP_INDENT) >> BP_INDENT_SH;
}

/* Pen advance of one codepoint in the active body font (baked UI font when no
 * custom face: ASCII/Latin-1 by value, a '?'-width box for anything above it). */
/* Raw, non-HTML content (text/plain, source, etc.) renders in the fixed-width
 * monospace cell font, not the proportional UI font — only HTML gets the latter.
 * A <pre> block (cur_bmono) is monospace too, even inside an HTML page. */
static int body_mono(void) { return B->mode == MODE_TEXT || cur_bmono; }

static int body_cp_adv(unsigned cp)
{
	if (body_mono()) return GLYPH_W;              /* monospace cell             */
	if (!B->body_font) {                          /* the gfx system-ui font     */
		return cur_bpx ? gfx_ui_cw_cp_px(cp, cur_bpx) : gfx_ui_cw_cp(cp);
	}
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
	 * which decodes UTF-8 and renders accents/Greek/Cyrillic from its cmap. A
	 * heading paragraph (cur_bpx) draws the run at that pixel size instead. */
	if (cur_bpx) { gfx_ui_text_px(x, y, s, fg, cur_bpx); return x + gfx_ui_width_px(s, cur_bpx); }
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

static void pane_metrics(int *py, int *phh, int *rows);

/* Highest valid scroll line: the last `rows` lines fill the pane exactly, so the
 * bottom stop can't scroll content up into blank space below it. */
static int scroll_max(void)
{
	int py, phh, rows;
	pane_metrics(&py, &phh, &rows);
	int max = B->nlines - rows;
	return max < 0 ? 0 : max;
}

/* --- approximate element geometry for the DOM geometry APIs (js.c) ---------- *
 * The line-flow renderer has no per-element box model, so an element's pixel box
 * is reconstructed from the wrapped line table + inline-image rows. Returns a
 * document-relative box (x,y,w,h) for the rendered text range [start,end), 0 if
 * the range produced no rows (unrendered/empty). Width is exact for a range that
 * lies on one text line; the content column otherwise. Runs on the worker (JS)
 * thread, so it holds blk_lock — render() rebuilds the line table and uses the
 * glyph cache (set_block/body_cp_adv) under the same lock. */
int browser_range_box(int start, int end, int *ox, int *oy, int *ow, int *oh)
{
	*ox = *oy = *ow = *oh = 0;
	if (end <= start) end = start + 1;            /* zero-width span: one anchor cell */
	int paw = (BW - SCROLL_W) - 8; if (paw < 8) paw = 8;

	blk_lock();
	int first = -1, last = -1, text_rows = 0, img_w = 0;
	for (int r = 0; r < B->nlines; r++) {
		int rs = B->line_off[r], rl = B->line_len[r];
		int hit = rl > 0 ? (rs < end && rs + rl > start)    /* text-line overlap */
		                 : (rs >= start && rs < end);       /* blank/anchor row  */
		if (!hit) continue;
		if (first < 0) first = r;
		last = r;
		if (rl > 0) text_rows++;
	}
	/* Inline images anchored inside the span: the <img> element itself (whose span
	 * ends at its anchor) and any ancestor block enclosing it. */
	for (int q = 0; q < B->n_imgs; q++) {
		struct inl_img *im = &B->imgs[q];
		if (im->start_row < 0 || im->text_pos < start || im->text_pos > end) continue;
		int s0 = im->start_row, s1 = s0 + (im->rows > 0 ? im->rows : 1) - 1;
		if (first < 0 || s0 < first) first = s0;
		if (s1 > last) last = s1;
		int dw = 0, dh = 0; img_disp_size(im, paw, &dw, &dh);
		if (dw > img_w) img_w = dw;
	}
	if (first < 0) { blk_unlock(); return 0; }

	int x = 4, w = paw, y = first * LINE_H, h = (last - first + 1) * LINE_H;
	if (first == last && text_rows == 1) {        /* one text line: exact x + width */
		int rs = B->line_off[first], re = rs + B->line_len[first];
		const char *t = B->text;
		unsigned cp;
		set_block(rs);
		x += cur_bindent * INDENT_PX;
		if (cur_bcenter) {
			int lw = 0;
			for (int p = rs; p < re; ) { p += utf8_decode(&t[p], &t[re], &cp); lw += body_cp_adv(cp); }
			int room = (paw - cur_bindent * INDENT_PX) - lw;
			if (room > 0) x += room / 2;
		}
		int a = start > rs ? start : rs, b = end < re ? end : re;
		for (int p = rs; p < a; ) { p += utf8_decode(&t[p], &t[re], &cp); x += body_cp_adv(cp); }
		w = 0;
		for (int p = a;  p < b; ) { p += utf8_decode(&t[p], &t[re], &cp); w += body_cp_adv(cp); }
		if (w < 1) w = 1;
	} else if (text_rows == 0 && img_w > 0) {     /* image-only box (drawn at x=4) */
		x = 4; w = img_w;
	} else {                                      /* block / multi-line: content col */
		set_block(B->line_off[first]);
		x = 4 + cur_bindent * INDENT_PX;
		w = paw - cur_bindent * INDENT_PX; if (w < 1) w = 1;
	}
	blk_unlock();
	*ox = x; *oy = y; *ow = w; *oh = h;
	return 1;
}

int browser_scroll_px(void) { return B->scroll * LINE_H; }   /* window.scrollY */

void browser_viewport_px(int *w, int *h)
{
	int py, phh, rows;
	pane_metrics(&py, &phh, &rows);
	if (w) { int vw = BW - SCROLL_W; *w = vw > 0 ? vw : 0; }
	if (h) *h = phh;
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
		/* Block context of this paragraph governs font size/monospace and how
		 * much of the pane width its indent leaves for text. */
		set_block(i);
		int avail = paw - cur_bindent * INDENT_PX;
		if (avail < 8) avail = 8;
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
					if (w + cw > avail && e > ls) break;
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
	int max = scroll_max();
	if (B->scroll > max) B->scroll = max;
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
static void draw_styled_line(int x, int y, int idx, int paw, int ct, int cb)
{
	int off = B->line_off[idx], len = B->line_len[idx];
	set_block(off);

	/* A horizontal rule is a whole-line element: a centred grey bar, no text. */
	if (cur_bhr) {
		int m = cur_bindent * INDENT_PX;
		gfx_fill_rect(x + m, y + LINE_H / 2, paw - m, 1, 0xc4c8cf);
		return;
	}

	x += cur_bindent * INDENT_PX;                  /* blockquote / list indent  */
	if (cur_bcenter) {                             /* text-align:center         */
		int lw = 0, p = off, end = off + len;
		unsigned cp;
		while (p < end) { p += utf8_decode(&B->text[p], &B->text[end], &cp); lw += body_cp_adv(cp); }
		int room = (paw - cur_bindent * INDENT_PX) - lw;
		if (room > 0) x += room / 2;
	}

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

/* Address-bar row layout: two nav buttons (Back, Forward) at the left, then the
 * URL text field filling the rest. One source of truth for both render() and the
 * click handler, so the drawn rects and hit rects never drift. */
struct rect { int x, y, w, h; };
#define NAV_BTN_W 28

static void addr_layout(struct rect *back, struct rect *fwd, struct rect *rel, struct rect *tf)
{
	int y0 = MENU_H + TAB_H;
	back->x = 4;                       back->y = y0 + 3; back->w = NAV_BTN_W; back->h = ADDR_H - 6;
	fwd->x  = back->x + NAV_BTN_W + 2;  fwd->y = back->y; fwd->w  = NAV_BTN_W; fwd->h  = back->h;
	rel->x  = fwd->x + NAV_BTN_W + 2;   rel->y = back->y; rel->w  = NAV_BTN_W; rel->h  = back->h;
	int tx  = rel->x + NAV_BTN_W + 6;
	tf->x = tx; tf->y = y0 + 4; tf->w = BW - tx - 6; tf->h = ADDR_H - 8;
	if (tf->w < 32) tf->w = 32;       /* keep the field usable on a narrow window */
}

/* A load is in flight when the worker is fetching or a navigation is queued for
 * it — the reload/stop button shows Stop, and Page>Stop is enabled, only then. */
static int is_loading(void) { return B->loading || B->req_pending; }

/* The reload (circular arrow) and stop (×) glyphs are hand-rasterized:
 * LiberationSans has no dependable U+21BB, and drawing them keeps the button
 * crisp at any size. Both fill a d×d box at (x0,y0). */
static void icon_reload(int x0, int y0, int d, unsigned col)
{
	int cx = x0 + d / 2, cy = y0 + d / 2, r = d / 2;
	for (int py = -r; py <= r; py++)
		for (int px = -r; px <= r; px++) {
			int dd = px * px + py * py;
			if (dd > r * r || dd < (r - 2) * (r - 2)) continue;
			if (py < 0 && px >= 0 && px <= 2) continue;   /* open the ring at the top */
			gfx_pixel(cx + px, cy + py, col);
		}
	/* arrowhead at the gap's right end: a corner stroke that reads as clockwise */
	for (int k = 0; k <= 3; k++) {
		gfx_pixel(cx + k,     cy - r,     col);           /* along the top    */
		gfx_pixel(cx + 3,     cy - r + k, col);           /* turning downward */
	}
}

static void icon_stop(int x0, int y0, int d, unsigned col)
{
	for (int i = 0; i < d; i++) {
		gfx_pixel(x0 + i,         y0 + i, col);
		gfx_pixel(x0 + i + 1,     y0 + i, col);           /* 2px-thick strokes */
		gfx_pixel(x0 + d - 1 - i, y0 + i, col);
		gfx_pixel(x0 + d - 2 - i, y0 + i, col);
	}
}

/* Draw the combined reload/stop button: the reload arrow while idle, a red × to
 * stop while a load is in flight. */
static void reload_btn_draw(const struct rect *r)
{
	int d = 12;
	int x0 = r->x + (r->w - d) / 2, y0 = r->y + (r->h - d) / 2;
	button_draw(r->x, r->y, r->w, r->h, "", BTN_NORMAL);
	if (is_loading()) icon_stop(x0, y0, d, 0xe06a5a);
	else              icon_reload(x0, y0, d, 0xe6ebf2);
}

/* The absolute URL of the link currently under the cursor, shown in a status box
 * pinned to the content pane's bottom-left (like Chrome/Firefox). Empty = none.
 * Main-thread-only UI state, so it lives here rather than in the shared struct. */
static char hover_url[URL_MAX];

/* Last pointer Y (from WEV_MOVE/clicks), so the wheel scrolls whichever surface
 * it's over: the devtools dock when the pointer is inside it, else the page. */
static int last_my = -1;

/* Set when only the address-bar chrome changed (a URL-field keystroke): the main
 * loop then repaints just that strip instead of re-wrapping and re-blitting the
 * whole page. Distinct from B->seq, which forces a full render. */
static int addr_dirty;

/* Copy `src` into `dst` (cap bytes) truncated to fit `maxpx` pixels in the UI
 * font, appending a "..." ellipsis when it had to shorten. Used for the hovered-
 * link status box, where a long URL must not overrun the box. */
static void fit_ui(char *dst, int cap, const char *src, int maxpx)
{
	int n = 0;
	if (gfx_ui_width(src) <= maxpx) {            /* whole string fits */
		while (src[n] && n < cap - 1) { dst[n] = src[n]; n++; }
		dst[n] = '\0';
		return;
	}
	const char *p = src, *end = src + strlen(src);
	int ell = gfx_ui_width("..."), used = 0;
	while (*p) {
		unsigned cp;
		int nb = utf8_decode(p, end, &cp);
		if (used + gfx_ui_cw_cp(cp) + ell > maxpx) break;
		for (int i = 0; i < nb && n < cap - 1; i++) dst[n++] = p[i];
		used += gfx_ui_cw_cp(cp);
		p += nb;
	}
	dst[n] = '\0';
	sapp(dst, &n, cap, "...");
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

	/* address bar (below the tab strip): Back/Forward buttons + the text field.
	 * The buttons dim when their direction has no history (button_hit also refuses
	 * a disabled button, so a click on a dim arrow does nothing). */
	gfx_fill_rect(0, MENU_H + TAB_H, BW, ADDR_H, 0x222831);
	struct rect rb, rf, rrl, rtf;
	addr_layout(&rb, &rf, &rrl, &rtf);
	struct tab *at = &tabs[active_tab];
	button_draw(rb.x, rb.y, rb.w, rb.h, "\xe2\x86\x90",   /* ← */
	            hist_can_back(at) ? BTN_NORMAL : BTN_DISABLED);
	button_draw(rf.x, rf.y, rf.w, rf.h, "\xe2\x86\x92",   /* → */
	            hist_can_fwd(at) ? BTN_NORMAL : BTN_DISABLED);
	reload_btn_draw(&rrl);
	textfield_draw(&B->addr_tf, rtf.x, rtf.y, rtf.w, rtf.h, 1);

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
			draw_styled_line(4, py + r * LINE_H, idx, paw, py, py + phh);
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

	/* Hovered-link status: a small flat box pinned to the content pane's bottom-
	 * left, like Chrome/Firefox — drawn only while a link is under the cursor, on
	 * top of the page but below the menu bar and its dropdowns. */
	if (hover_url[0]) {
		int sy0 = MENU_H + TAB_H + ADDR_H;
		int sh = UIFONT_H + 6;
		int by = BH - sh; if (by < sy0) by = sy0;
		int maxw = BW - 2;
		char shown[URL_MAX];
		fit_ui(shown, sizeof shown, hover_url, maxw - 12);
		int sw = gfx_ui_width(shown) + 12; if (sw > maxw) sw = maxw;
		gfx_fill_rect(0, by, sw, sh, 0x202830);
		gfx_rect(0, by, sw, sh, 0x10141a);
		gfx_ui_text(6, by + 3, shown, 0xe8eaee);
	}

	/* Developer tools overlay (F12 / Ctrl+Shift+I): a bottom dock drawn over the
	 * page but under the menu dropdowns. A no-op when closed. */
	devtools_render();

	/* App menu bar over the top, then the dropdowns last so they overlay the page.
	 * "Stop" is only meaningful while a fetch is in flight, so disable it when
	 * idle — the widget dims it and ignores clicks/accelerators. */
	page_flags[2] = is_loading() ? 0 : MENU_DISABLED;
	gfx_fill_rect(0, 0, BW, MENU_H, 0x232a36);
	menubar_draw(&menubar);

	blk_unlock();
}

/* True while any menu dropdown is expanded. Such a dropdown overlaps the address
 * bar, so the chrome-only fast path must defer to a full render() then. */
static int menu_any_open(void)
{
	for (int i = 0; i < menubar.n; i++)
		if (menubar.menus[i] && menubar.menus[i]->open)
			return 1;
	return 0;
}

/* Repaint only the address-bar strip (its background, the Back/Forward buttons,
 * and the URL field) and leave the page untouched. The page below and the menu
 * bar above don't intersect this row, so a present's frame diff blits just this
 * strip — no rewrap, no line/image redraw. Touches no shared content buffers, so
 * unlike render() it needs no blk_lock. */
static void render_addr(void)
{
	gfx_fill_rect(0, MENU_H + TAB_H, BW, ADDR_H, 0x222831);
	struct rect rb, rf, rrl, rtf;
	addr_layout(&rb, &rf, &rrl, &rtf);
	struct tab *at = &tabs[active_tab];
	button_draw(rb.x, rb.y, rb.w, rb.h, "\xe2\x86\x90",
	            hist_can_back(at) ? BTN_NORMAL : BTN_DISABLED);
	button_draw(rf.x, rf.y, rf.w, rf.h, "\xe2\x86\x92",
	            hist_can_fwd(at) ? BTN_NORMAL : BTN_DISABLED);
	reload_btn_draw(&rrl);
	textfield_draw(&B->addr_tf, rtf.x, rtf.y, rtf.w, rtf.h, 1);
}

/* --------------------------------------------------------------- input */

static void pane_metrics(int *py, int *phh, int *rows)
{
	*py = MENU_H + TAB_H + ADDR_H;
	*phh = BH - MENU_H - TAB_H - ADDR_H; if (*phh < 0) *phh = 0;
	*rows = (*phh > 0) ? *phh / LINE_H : 1;
}

/* Apply a target scroll line, clamped to the page, repainting only when it
 * actually moves — so wheeling or arrowing at an end is a no-op instead of a
 * full content redraw. Callers wanting a tighter bottom stop (e.g. Page Down)
 * pass an already-tighter target; this only re-clamps for safety. */
static void scroll_set(int target)
{
	int max = scroll_max();
	if (target > max) target = max;
	if (target < 0) target = 0;
	if (target != B->scroll) { B->scroll = target; B->seq++; }
}

static void navigate(const char *url)
{
	hover_url[0] = '\0';                      /* drop any stale hovered-link status */
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

/* Reload the current page (reload button, Page>Reload, F5, Ctrl+R). No-op on a
 * blank tab, matching Page>Reload. */
static void do_reload(void)
{
	if (B->cur_url[0]) navigate(B->cur_url);
}

/* Stop the in-flight load: drop the queued request and signal the worker to bail
 * out of the fetch it is running (polled by the body readers in fetch.c). */
static void do_stop(void)
{
	B->req_pending = 0;
	B->abort_load = 1;
	B->seq++;
}

/* Navigate to the active tab's previous/next history entry. The hist index moves
 * here; nav_from_history keeps the load's completion from pushing a new entry. */
static void go_back(void)
{
	struct tab *t = &tabs[active_tab];
	if (!hist_can_back(t)) return;
	t->hist_i--;
	nav_from_history = 1;
	navigate(t->hist[t->hist_i]);
}

static void go_fwd(void)
{
	struct tab *t = &tabs[active_tab];
	if (!hist_can_fwd(t)) return;
	t->hist_i++;
	nav_from_history = 1;
	navigate(t->hist[t->hist_i]);
}

/* Reset B to an empty page (a fresh/blank tab) without touching the net. */
static void show_blank(void)
{
	B->drop_dom = 1; B->events_live = 0;
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
	devtools_view_load(&tabs[i].dt);         /* this tab's devtools view; drops old capture */
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
	tabs[active_tab].hist_n = 0; tabs[active_tab].hist_i = 0;   /* fresh history */
	for (int k = 0; k < HIST_MAX; k++) tabs[active_tab].hist[k] = 0;
	tabs[active_tab].dt.open = 0; tabs[active_tab].dt.tab = 0;  /* devtools closed   */
	for (int k = 0; k < DT_PANELS; k++) tabs[active_tab].dt.scroll[k] = 0;
	devtools_view_load(&tabs[active_tab].dt);
	copy_str(tabs[active_tab].label, "New Tab", sizeof tabs[active_tab].label);
	show_blank();
}

static void close_tab(int i)
{
	if (i < 0 || i >= n_tabs || n_tabs <= 1) return;   /* keep at least one */
	int was_active = (i == active_tab);
	if (was_active) do_stop();    /* only the active tab loads: cancel its fetch */
	tab_drop(&tabs[i]);                           /* free its snapshot first */
	hist_clear(&tabs[i]);                         /* and its history strings  */
	for (int j = i; j < n_tabs - 1; j++) {       /* shift the rest down a slot */
		char *d = (char *)&tabs[j], *s = (char *)&tabs[j + 1];
		for (int k = 0; k < (int)sizeof(struct tab); k++) d[k] = s[k];
	}
	tabs[n_tabs - 1].blob = 0;                    /* vacated slot now aliases the */
	tabs[n_tabs - 1].cached = 0;                  /* moved blob — drop the alias  */
	for (int k = 0; k < HIST_MAX; k++) tabs[n_tabs - 1].hist[k] = 0;  /* + history */
	tabs[n_tabs - 1].hist_n = 0; tabs[n_tabs - 1].hist_i = 0;
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
		case 0: do_reload(); break;              /* Reload current page     */
		case 1: navigate("example.com"); break;  /* Home                    */
		case 2: do_stop(); break;                /* Stop the in-flight load */
		}
	} else if (m == &viewmenu) {
		if (item == 0) {                         /* Show Images checkbox    */
			view_flags[0] ^= MENU_CHECKED;
			show_images = (view_flags[0] & MENU_CHECKED) != 0;
			B->seq++;
		}                                        /* item 1 = Scroll submenu */
	} else if (m == &scrollmenu) {
		switch (item) {
		case 0: scroll_set(0); break;                    /* Top      */
		case 1: scroll_set(B->nlines - rows); break;     /* Bottom   */
		case 2: scroll_set(B->scroll - rows); break;     /* Page Up  */
		case 3: scroll_set(B->scroll + rows); break;     /* Page Down */
		}
	}
}

/* Resolve a content-pane hit at client coords (mx,my): sets *off to the text
 * offset under the cursor (-1 if outside the pane or past the line's glyphs) and
 * copies the absolute href of any <a> covering it into `url` ("" if none). Holds
 * blk_lock across the walk + resolve, since the worker can move the text/line/
 * link tables with a realloc mid-load. Shared by the click and hover handlers. */
static void pane_hit(int mx, int my, int *off, char *url, int cap)
{
	*off = -1;
	if (cap) url[0] = '\0';
	int py, phh, rows;
	pane_metrics(&py, &phh, &rows);
	if (my < py || my >= py + phh) return;
	int row = B->scroll + (my - py) / LINE_H;
	blk_lock();
	if (row >= 0 && row < B->nlines) {
		/* proportional: walk the line's glyphs to find which char the point
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
			*off = base + col;
			for (int i = 0; i < B->nlinks; i++)
				if (*off >= B->links[i].start && *off < B->links[i].end) {
					resolve_url(B->cur_url, B->links[i].href, url, cap);
					break;
				}
		}
	}
	blk_unlock();
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
	/* Developer tools dock (bottom of the window): consumes clicks within it.
	 * Its rect sits below the address bar, so it never shadows the bars above. */
	if (devtools_handle_click(mx, my)) return;
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
		struct rect rb, rf, rrl, rtf;
		addr_layout(&rb, &rf, &rrl, &rtf);
		struct tab *at = &tabs[active_tab];
		if (hist_can_back(at) && button_hit(rb.x, rb.y, rb.w, rb.h, mx, my)) {
			go_back();
		} else if (hist_can_fwd(at) && button_hit(rf.x, rf.y, rf.w, rf.h, mx, my)) {
			go_fwd();
		} else if (button_hit(rrl.x, rrl.y, rrl.w, rrl.h, mx, my)) {
			if (is_loading()) do_stop(); else do_reload();
		} else {
			textfield_click(&B->addr_tf, mx, my, rtf.x, rtf.y, rtf.w, rtf.h);
		}
		B->seq++;
		return;
	}
	int py, phh, rows;
	pane_metrics(&py, &phh, &rows);
	int sbpart = scrollbar_hit(BW - SCROLL_W, py, SCROLL_W, phh, mx, my);
	if (sbpart != SCROLLBAR_NONE) {                          /* scrollbar */
		int target;
		if (sbpart == SCROLLBAR_UP)        target = B->scroll - 3;
		else if (sbpart == SCROLLBAR_DOWN) target = B->scroll + 3;
		else target = scrollbar_pos(BW - SCROLL_W, py, SCROLL_W, phh, mx, my,
		                            B->scroll, B->nlines, rows);
		scroll_set(target);
		return;
	}
	if (my >= py && my < py + phh) {                        /* content pane click */
		char res[URL_MAX];
		int off;
		pane_hit(mx, my, &off, res, sizeof res);
		/* If the page registered JS listeners, hand the click to the worker to
		 * dispatch through the live DOM (it follows the link `res` afterwards
		 * unless a handler calls preventDefault). Otherwise take the fast path and
		 * navigate directly — pages with no scripts behave exactly as before. */
		if (off >= 0 && B->events_live) {
			/* clientX/clientY are viewport-relative (pane origin), matching
			 * getBoundingClientRect; the pane's left edge is window x 0. */
			B->evt_off = off; B->evt_x = mx; B->evt_y = my - py;
			{ int k = 0; sapp(B->evt_type, &k, sizeof B->evt_type, "click"); }
			{ int k = 0; sapp(B->evt_text, &k, sizeof B->evt_text, res); }
			B->evt_pending = 1; B->seq++;
			return;
		}
		if (res[0]) { navigate(res); return; }
	}
}

/* Pointer moved over the content pane with no button held (WEV_MOVE; mx<0 when it
 * left the window): show the hovered link's target in the bottom-left status box,
 * or clear it. Only repaints when the shown URL actually changes. */
static void handle_move(int mx, int my)
{
	char url[URL_MAX];
	int off;
	last_my = mx < 0 ? -1 : my;         /* track for wheel routing (devtools vs page) */
	if (mx < 0) url[0] = '\0';          /* pointer left the window: clear */
	else        pane_hit(mx, my, &off, url, sizeof url);
	if (streq(url, hover_url)) return;
	int k = 0; sapp(hover_url, &k, sizeof hover_url, url);
	B->seq++;
}

static void handle_key(char c, int mods)
{
	/* Developer tools: toggle on F12 or Ctrl+Shift+I; Esc closes an open panel.
	 * Ctrl+Shift+I arrives as a bare 'i' char with the CTRL|SHIFT modifier bits. */
	if ((unsigned char)c == KEY_F12 ||
	    ((mods & KMOD_CTRL) && (mods & KMOD_SHIFT) && (c == 'i' || c == 'I'))) {
		devtools_toggle();
		return;
	}
	if ((unsigned char)c == 27 && devtools_visible()) { devtools_toggle(); return; }

	/* Alt+Left / Alt+Right are history navigation — checked before the address
	 * bar so the modifier turns the arrow into back/forward instead of a caret
	 * move. The keyboard reports the chord via mods (KMOD_*); the app decides. */
	if (mods & KMOD_ALT) {
		if ((unsigned char)c == KEY_LEFT)  { go_back(); return; }
		if ((unsigned char)c == KEY_RIGHT) { go_fwd();  return; }
	}

	/* Ctrl+R reloads (the keyboard delivers the unshifted letter + KMOD_CTRL).
	 * Checked before the address field so it isn't typed into the URL. */
	if ((mods & KMOD_CTRL) && (c == 'r' || c == 'R')) { do_reload(); return; }

	/* Menu accelerator (F5 = Reload): fire the item like a click. */
	struct menu *leaf;
	int sc = menubar_shortcut(&menubar, (unsigned char)c, &leaf);
	if (sc >= 0) { menubar_close(&menubar); menu_action(leaf, sc); return; }

	/* A focused content text field captures editing keys: post them to the worker
	 * (which owns the DOM) as keyinput/blur events instead of editing the address
	 * bar. Esc blurs; other navigation keys fall through to the defaults below. */
	if (B->focus_kind != 0) {
		unsigned char uc = (unsigned char)c;
		if (uc == 27) {
			{ int k = 0; sapp(B->evt_type, &k, sizeof B->evt_type, "blur"); }
			B->evt_pending = 1; B->seq++;
			return;
		}
		if (uc == '\n' || uc == '\r' || uc == 8 || uc == 127 || (uc >= 0x20 && uc < 0x7f)) {
			B->evt_x = uc;
			{ int k = 0; sapp(B->evt_type, &k, sizeof B->evt_type, "keyinput"); }
			B->evt_pending = 1; B->seq++;
			return;
		}
	}

	if ((unsigned char)c == 27) { menubar_close(&menubar); B->seq++; return; }

	/* The address-bar widget handles editing (printable/Backspace/Delete/caret).
	 * That only changes the URL field, so flag a chrome-only repaint rather than
	 * bumping B->seq (a full page render) — unless an open dropdown overlaps the
	 * strip, where only a full render restores it correctly. */
	if (textfield_key(&B->addr_tf, c)) {
		if (menu_any_open()) B->seq++; else addr_dirty = 1;
		return;
	}

	switch ((unsigned char)c) {
	case '\n': case '\r': submit(); return;
	case KEY_UP:   scroll_set(B->scroll - 3); return;
	case KEY_DOWN: scroll_set(B->scroll + 3); return;
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

int main(int argc, char **argv)
{
	const char *start = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "example.com";
	int id = win_create(BW, BH, "browser");
	if (id < 0) { puts("browser: no window\n"); return 1; }
	B->win_id = id;
	gfx_init(win_fb(), BW, BH);

	textfield_init(&B->addr_tf, B->addr, URL_MAX);
	textfield_set(&B->addr_tf, start);
	B->wrap_seq = 0xFFFFFFFF;

	jar_load();   /* restore cookies + HSTS from the persistent SQLite db */

	int wtid = thread_create(worker_fn, &worker_stack[sizeof worker_stack]);

	/* Launched with a URL (e.g. `browser http://host/page`): load it at startup. */
	if (argc > 1 && argv[1] && argv[1][0]) navigate(argv[1]);

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
				handle_key((char)ev.a, ev.b);
			} else if (ev.type == WEV_MOUSE) {
				handle_click(ev.a, ev.b);
			} else if (ev.type == WEV_MOVE) {
				handle_move(ev.a, ev.b);
			} else if (ev.type == WEV_SCROLL) {
				/* The wheel scrolls whichever surface the pointer is over: the
				 * devtools dock when inside it, otherwise the page. */
				if (devtools_visible() && last_my >= devtools_top()) {
					devtools_handle_scroll(ev.a);
				} else {
					scroll_set(B->scroll - ev.a * 3);  /* wheel up scrolls back */
				}
			} else if (ev.type == WEV_RESIZE) {
				BW = ev.a; BH = ev.b;
				gfx_init(win_fb(), BW, BH);
				B->recascade = 1;    /* @media may change → worker re-cascades  */
				B->seq++;            /* re-wrap + repaint at the new width */
			}
		}
		/* History API requests posted by JS on the worker (see browser/js.c).
		 * pushState/replaceState is applied before a back/forward posted in the
		 * same script tick, so the entry it added is in place first. */
		if (B->hist_nav) {                           /* pushState / replaceState */
			struct tab *t = &tabs[active_tab];
			B->hist_nav = 0;
			if (B->hist_replace && t->hist_n > 0) {
				int n = (int)strlen(B->hist_url) + 1;
				char *dup = malloc((unsigned)n);
				if (dup) { memcpy(dup, B->hist_url, (unsigned)n); free(t->hist[t->hist_i]); t->hist[t->hist_i] = dup; }
			} else {
				hist_push(t, B->hist_url);
			}
			textfield_set(&B->addr_tf, B->hist_url);
			B->seq++;
		}
		if (B->hist_go) {                            /* history.back/forward/go */
			int d = B->hist_go; B->hist_go = 0;
			if (d < 0) go_back(); else go_fwd();
		}
		if (B->loc_nav) {                            /* location.assign/replace/reload */
			B->loc_nav = 0;
			navigate(B->loc_url);
		}
		B->hist_len = tabs[active_tab].hist_n;

		if (B->loading && !prev_loading)             /* fetch started */
			win_cursor(id, WIN_CURSOR_BUSY);         /* spinner while loading */
		if (prev_loading && !B->loading) {           /* fetch finished */
			win_cursor(id, WIN_CURSOR_ARROW);
			tab_sync_active();                       /* refresh the tab label */
			update_title(id);
			if (B->cur_url[0])                       /* show the resolved URL */
				textfield_set(&B->addr_tf, B->cur_url);  /* (incl. the scheme) */
			/* Record the page we landed on, unless this load was itself a
			 * Back/Next move (then we only walked the existing history). */
			if (!nav_from_history) hist_push(&tabs[active_tab], B->cur_url);
			nav_from_history = 0;
		}
		prev_loading = B->loading;

		if (B->seq != last) {
			render(); win_present(id); last = B->seq; addr_dirty = 0;
		} else if (addr_dirty) {                     /* address-bar keystroke only */
			render_addr(); win_present(id); addr_dirty = 0;
		}
		win_wait(id);
	}
}
