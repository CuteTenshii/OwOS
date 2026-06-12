/* main.c — the browser's windowed UI and event loop (ring 3).
 *
 * Owns the one `struct browser` instance and the colour palette. The address
 * bar, content pane, scrollbar and status line are drawn here; a worker thread
 * (thread_create) runs the blocking fetch so typing/scrolling stay live while a
 * page loads. The worker only touches B's buffers + net; window syscalls (title,
 * present) are made by this, the owning, thread.
 */
#include "browser.h"

static struct browser binst;
struct browser *B = &binst;

int BW = 600, BH = 368;        /* client size; grows/shrinks on resize */

const unsigned palette[16] = {
	0x202830, 0x1a4fbf, 0xc01818, 0x108010, 0x9020a0, 0x9a6020, 0x107c90,
	0x586070, 0x16387a, 0x000000, 0x707000, 0xd07000, 0x808890, 0x304060,
	0x2050d0, 0xa0a0a0,
};

/* The app menu bar: two dropdowns ("Page" and "View") laid out on one bar via
 * the gfx menubar helper, which keeps only one open at a time. Page covers
 * loading/navigation (Reload carries the F5 accelerator); View covers scrolling
 * within the current page. */
static const char *const page_items[] = { "Reload", "Home", "Stop" };
static const char *const page_hints[] = { "F5", "", "" };
static const unsigned char page_keys[] = { KEY_F5, 0, 0 };
static struct menu pagemenu = { "Page", page_items, 3, 0, page_hints, page_keys };

static const char *const view_items[] = { "Top", "Bottom", "Page Up", "Page Down" };
static struct menu viewmenu = { "View", view_items, 4, 0, 0, 0 };

static struct menu *const bar_menus[] = { &pagemenu, &viewmenu };
static struct menubar menubar = { bar_menus, 2, 2, 0 };

/* --- worker thread: drains req_pending, fetches, bumps seq for the UI ----- */
/* 32 KiB: http_txn alone holds hdr[4096] + req[2048] + a cookie buffer on the
 * stack, so an 8 KiB stack overflows into B's .bss and corrupts it. */
static char         worker_stack[32768];
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
		}
		msleep(30);
	}
}

/* --------------------------------------------------------------- word wrap */

static void add_line(int off, int len)
{
	if (B->nlines < MAX_LINES) {
		B->line_off[B->nlines] = off;
		B->line_len[B->nlines] = len;
		B->nlines++;
	}
}

/* How many display rows an inline image reserves at the current pane width. */
static int img_rows(const struct inl_img *im, int cols)
{
	if (!im->ok || im->iw == 0) return 1;             /* alt box: one row       */
	int paw = cols * GLYPH_W; if (paw < GLYPH_W) paw = GLYPH_W;
	int dw = (int)im->iw; if (dw > paw) dw = paw;
	int dh = (int)((unsigned long)im->ih * (unsigned)dw / im->iw);
	int r = (dh + GLYPH_H - 1) / GLYPH_H;
	return r < 1 ? 1 : r;
}

static void rewrap(int cols)
{
	if (cols < 1) cols = 1;
	if (B->wrap_seq == B->seq && B->wrap_cols == cols)
		return;
	for (int q = 0; q < B->n_imgs; q++) B->imgs[q].start_row = -1;
	B->nlines = 0;
	int i = 0;
	while (i < B->text_len && B->nlines < MAX_LINES) {
		int j = i;
		while (j < B->text_len && B->text[j] != '\n') j++;
		if (j == i) {
			int img = -1;
			for (int q = 0; q < B->n_imgs; q++)
				if (B->imgs[q].text_pos == i) { img = q; break; }
			if (img >= 0) {                           /* reserve the image rows  */
				struct inl_img *im = &B->imgs[img];
				im->rows = img_rows(im, cols);
				im->start_row = B->nlines;
				for (int r = 0; r < im->rows && B->nlines < MAX_LINES; r++)
					add_line(i, 0);
			} else {
				add_line(i, 0);
			}
		} else {
			int ls = i;
			while (ls < j) {
				int remain = j - ls;
				if (remain <= cols) { add_line(ls, remain); break; }
				int brk = ls + cols, b = brk;
				while (b > ls && B->text[b] != ' ') b--;
				if (b == ls) b = brk;
				add_line(ls, b - ls);
				ls = b;
				while (ls < j && B->text[ls] == ' ') ls++;
			}
		}
		i = j + 1;
	}
	B->wrap_seq = B->seq;
	B->wrap_cols = cols;
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
		gfx_text(px + 8, py + 8,
		         "Cannot decode image (unreadable or oversized interlaced PNG).",
		         0xb04040, bg);
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

static void draw_inline_image(struct inl_img *im, int py, int phh, int cols)
{
	int paw = cols * GLYPH_W; if (paw < GLYPH_W) paw = GLYPH_W;
	int dw = (int)im->iw; if (dw > paw) dw = paw;
	int dh = (int)((unsigned long)im->ih * (unsigned)dw / im->iw);
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;

	struct inlblit v;
	v.x0 = 4;
	v.y0 = py + (im->start_row - B->scroll) * GLYPH_H;
	v.dw = dw; v.dh = dh; v.clip_top = py; v.clip_bot = py + phh;
	v.iw = im->iw; v.ih = im->ih; v.next_r = 0;

	struct poolrd r = { 0, im->coff, im->clen };
	upng_decode(poolrd_get, &r, inl_row, &v, 0, 0);
}

/* A placeholder box for an image that couldn't be fetched/decoded. */
static void draw_alt_box(struct inl_img *im, int py, int phh)
{
	int y0 = py + (im->start_row - B->scroll) * GLYPH_H;
	if (y0 < py || y0 + GLYPH_H > py + phh) return;        /* keep it in-pane    */
	const char *t = im->alt[0] ? im->alt : "[image]";
	int w = (slen(t) + 2) * GLYPH_W;
	gfx_fill_rect(4, y0, w, GLYPH_H, 0xe6e9ef);
	gfx_rect(4, y0, w, GLYPH_H, 0xb4bac4);
	gfx_text(8, y0, t, 0x7a828c, GFX_TRANSPARENT);
}

/* --------------------------------------------------------------- drawing */

static void draw_styled_line(int x, int y, int idx, unsigned bg)
{
	int off = B->line_off[idx], len = B->line_len[idx];
	int i = 0;
	while (i < len) {
		unsigned char a = B->attr[off + i];
		char run[260]; int rl = 0;
		while (i < len && B->attr[off + i] == a && rl < (int)sizeof run - 1)
			run[rl++] = B->text[off + i++];
		run[rl] = '\0';
		unsigned col = palette[a & ST_COLOR];
		gfx_text(x, y, run, col, bg);
		if (a & ST_BOLD)  gfx_text(x + 1, y, run, col, GFX_TRANSPARENT);
		if (a & ST_UNDER) gfx_fill_rect(x, y + GLYPH_H - 2, rl * GLYPH_W, 1, col);
		x += rl * GLYPH_W;
	}
}

static void render(void)
{
	unsigned bg = 0xf4f5f7;

	/* address bar (below the menu bar) */
	gfx_fill_rect(0, MENU_H, BW, ADDR_H, 0x222831);
	int fx = 6, fy = MENU_H + 4, fw = BW - 12, fh = ADDR_H - 8;
	gfx_fill_rect(fx, fy, fw, fh, 0xffffff);
	gfx_rect(fx, fy, fw, fh, 0x000000);
	int maxc = (fw - 8) / GLYPH_W; if (maxc < 1) maxc = 1;
	char shown[300];
	int al = B->addr_len, off = 0;
	if (al > maxc - 1) off = al - (maxc - 1);
	int sc = 0;
	for (int i = off; i < al && sc < (int)sizeof shown - 2; i++) shown[sc++] = B->addr[i];
	shown[sc++] = '_';
	shown[sc] = '\0';
	gfx_text(fx + 4, fy + (fh - GLYPH_H) / 2, shown, 0x101418, 0xffffff);

	/* content pane */
	int py = MENU_H + ADDR_H, pw = BW - SCROLL_W;
	int phh = BH - MENU_H - ADDR_H - STATUS_H; if (phh < 0) phh = 0;
	gfx_fill_rect(0, py, pw, phh, bg);
	int rows = phh / GLYPH_H;

	if (B->mode == MODE_IMAGE) {
		if (B->loading)
			gfx_text(8, py + 8, "Loading ...", 0x4060a0, bg);
		else
			draw_image(0, py, pw, phh, bg);
		gfx_fill_rect(pw, py, SCROLL_W, phh, 0xc8ccd2);  /* inert scrollbar */
	} else {
		int cols = (pw - 8) / GLYPH_W;
		rewrap(cols);
		if (B->loading && B->nlines == 0) {
			gfx_text(8, py + 8, "Loading ...", 0x4060a0, bg);
		} else if (B->nlines == 0) {
			gfx_text(8, py + 8, "Type a URL above and press Enter.", 0x707880, bg);
			gfx_text(8, py + 8 + GLYPH_H, "e.g. example.com  or  https://example.com",
			         0x9098a0, bg);
		}
		for (int r = 0; r < rows; r++) {
			int idx = B->scroll + r;
			if (idx >= B->nlines) break;
			draw_styled_line(4, py + r * GLYPH_H, idx, bg);
		}

		/* inline images, over their reserved (blank) rows */
		for (int q = 0; q < B->n_imgs; q++) {
			struct inl_img *im = &B->imgs[q];
			if (im->start_row < 0) continue;
			int r0 = im->start_row;
			int r1 = r0 + (im->rows > 0 ? im->rows : 1);
			if (r1 <= B->scroll || r0 >= B->scroll + rows) continue;  /* off-screen */
			if (im->ok) draw_inline_image(im, py, phh, cols);
			else        draw_alt_box(im, py, phh);
		}

		/* scrollbar */
		gfx_fill_rect(pw, py, SCROLL_W, phh, 0xc8ccd2);
		if (B->nlines > rows && rows > 0) {
			int th = phh * rows / B->nlines; if (th < 12) th = 12;
			int maxs = B->nlines - rows;
			int ty = (maxs > 0) ? (phh - th) * B->scroll / maxs : 0;
			gfx_fill_rect(pw + 1, py + ty, SCROLL_W - 2, th, 0x6b7686);
		}
	}

	/* status line */
	int sy = BH - STATUS_H;
	gfx_fill_rect(0, sy, BW, STATUS_H, 0x222831);
	int smaxc = (BW - 12) / GLYPH_W;
	char st[200];
	int sl = slen(B->status), so = 0;
	if (sl > smaxc) so = sl - smaxc;
	int k = 0;
	for (int i = so; B->status[i] && k < (int)sizeof st - 1; i++) st[k++] = B->status[i];
	st[k] = '\0';
	gfx_text(6, sy + (STATUS_H - GLYPH_H) / 2, st, 0xc8d0dc, 0x222831);

	/* App menu bar over the top, then the dropdowns last so they overlay the page. */
	gfx_fill_rect(0, 0, BW, MENU_H, 0x232a36);
	menubar_draw(&menubar);
}

/* --------------------------------------------------------------- input */

static void pane_metrics(int *py, int *phh, int *rows)
{
	*py = MENU_H + ADDR_H;
	*phh = BH - MENU_H - ADDR_H - STATUS_H; if (*phh < 0) *phh = 0;
	*rows = (*phh > 0) ? *phh / GLYPH_H : 1;
}

static void scroll_to_y(int my, int py, int phh, int rows)
{
	if (B->nlines <= rows) return;
	int maxs = B->nlines - rows;
	int th = phh * rows / B->nlines; if (th < 12) th = 12;
	int span = phh - th;
	int rel = my - py - th / 2;
	int s = (span > 0) ? rel * maxs / span : 0;
	if (s < 0) s = 0;
	if (s > maxs) s = maxs;
	B->scroll = s; B->seq++;
}

static void navigate(const char *url)
{
	int i = 0;
	for (; url[i] && i < URL_MAX - 1; i++) { B->addr[i] = url[i]; B->req_url[i] = url[i]; }
	B->addr[i] = '\0'; B->req_url[i] = '\0';
	B->addr_len = i;
	B->req_pending = 1; B->seq++;
}

static void submit(void)
{
	if (B->addr_len == 0) return;
	int i = 0;
	for (; B->addr[i] && i < URL_MAX - 1; i++) B->req_url[i] = B->addr[i];
	B->req_url[i] = '\0';
	B->req_pending = 1; B->seq++;
}

/* Run menu item `item` of bar menu `mi` (0 = Page, 1 = View). Reload is also
 * reached via the F5 accelerator. */
static void menu_action(int mi, int item)
{
	int py, phh, rows;
	pane_metrics(&py, &phh, &rows);
	if (mi == 0) {                                   /* Page                    */
		switch (item) {
		case 0:                                  /* Reload current page     */
			if (B->cur_url[0]) navigate(B->cur_url);
			break;
		case 1: navigate("example.com"); break;  /* Home                    */
		case 2:                                  /* Stop a queued fetch     */
			B->req_pending = 0; set_status("Stopped."); B->seq++;
			break;
		}
	} else {                                         /* View (scrolling)        */
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
		int mi, item;
		if (menubar_click(&menubar, mx, my, &mi, &item)) {
			if (item >= 0)
				menu_action(mi, item);
			B->seq++;
			return;
		}
	}
	int py, phh, rows;
	pane_metrics(&py, &phh, &rows);
	if (mx >= BW - SCROLL_W && my >= py && my < py + phh) {  /* scrollbar */
		scroll_to_y(my, py, phh, rows);
		return;
	}
	if (my >= py && my < py + phh) {                        /* a link? */
		int row = B->scroll + (my - py) / GLYPH_H;
		int col = (mx - 4) / GLYPH_W;
		if (row >= 0 && row < B->nlines && col >= 0 && col < B->line_len[row]) {
			int off = B->line_off[row] + col;
			for (int i = 0; i < B->nlinks; i++)
				if (off >= B->links[i].start && off < B->links[i].end) {
					char res[URL_MAX];
					resolve_url(B->cur_url, B->links[i].href, res, sizeof res);
					navigate(res);
					return;
				}
		}
	}
}

static void handle_key(char c)
{
	/* Menu accelerator (F5 = Reload): fire the item like a click. */
	int mi, sc = menubar_shortcut(&menubar, (unsigned char)c, &mi);
	if (sc >= 0) { menubar_close(&menubar); menu_action(mi, sc); return; }
	if ((unsigned char)c == 27) { menubar_close(&menubar); B->seq++; return; }

	switch ((unsigned char)c) {
	case '\n': case '\r': submit(); return;
	case '\b':
		if (B->addr_len > 0) { B->addr_len--; B->addr[B->addr_len] = '\0'; }
		B->seq++; return;
	case KEY_UP:
		B->scroll -= 3; if (B->scroll < 0) B->scroll = 0; B->seq++; return;
	case KEY_DOWN:
		B->scroll += 3;
		if (B->scroll > B->nlines - 1) B->scroll = B->nlines - 1;
		if (B->scroll < 0) B->scroll = 0;
		B->seq++; return;
	default:
		if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f && B->addr_len < URL_MAX - 1) {
			B->addr[B->addr_len++] = c;
			B->addr[B->addr_len] = '\0';
			B->seq++;
		}
		return;
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

	const char *start = "example.com";
	for (int i = 0; start[i]; i++) B->addr[B->addr_len++] = start[i];
	B->addr[B->addr_len] = '\0';
	set_status("Ready. Type a URL and press Enter.");
	B->wrap_seq = 0xFFFFFFFF;

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
			update_title(id);
		}
		prev_loading = B->loading;

		if (B->seq != last) { render(); win_present(id); last = B->seq; }
		win_wait(id);
	}
}
