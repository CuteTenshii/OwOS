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

static void rewrap(int cols)
{
	if (cols < 1) cols = 1;
	if (B->wrap_seq == B->seq && B->wrap_cols == cols)
		return;
	B->nlines = 0;
	int i = 0;
	while (i < B->text_len && B->nlines < MAX_LINES) {
		int j = i;
		while (j < B->text_len && B->text[j] != '\n') j++;
		if (j == i) {
			add_line(i, 0);
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

	/* address bar */
	gfx_fill_rect(0, 0, BW, ADDR_H, 0x222831);
	int fx = 6, fy = 4, fw = BW - 12, fh = ADDR_H - 8;
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
	int py = ADDR_H, pw = BW - SCROLL_W;
	int phh = BH - ADDR_H - STATUS_H; if (phh < 0) phh = 0;
	gfx_fill_rect(0, py, pw, phh, bg);
	int cols = (pw - 8) / GLYPH_W;
	rewrap(cols);
	int rows = phh / GLYPH_H;
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

	/* scrollbar */
	gfx_fill_rect(pw, py, SCROLL_W, phh, 0xc8ccd2);
	if (B->nlines > rows && rows > 0) {
		int th = phh * rows / B->nlines; if (th < 12) th = 12;
		int maxs = B->nlines - rows;
		int ty = (maxs > 0) ? (phh - th) * B->scroll / maxs : 0;
		gfx_fill_rect(pw + 1, py + ty, SCROLL_W - 2, th, 0x6b7686);
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
}

/* --------------------------------------------------------------- input */

static void pane_metrics(int *py, int *phh, int *rows)
{
	*py = ADDR_H;
	*phh = BH - ADDR_H - STATUS_H; if (*phh < 0) *phh = 0;
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

static void handle_click(int mx, int my)
{
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
	win_resizable(id, 1);            /* the browser can be resized */

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
		if (prev_loading && !B->loading)             /* fetch finished */
			update_title(id);
		prev_loading = B->loading;

		if (B->seq != last) { render(); win_present(id); last = B->seq; }
		win_wait(id);
	}
}
