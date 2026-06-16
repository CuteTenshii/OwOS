/* cliphist.c — the Super+V clipboard-history viewer.
 *
 * A plain user-space window over the clipboard lib (clipd): it lists the clip
 * history newest-first, and clicking an item promotes it back to the front so
 * the next paste uses it. The clipboard needs no kernel access — this is pure
 * presentation; the WM (which already routes input) launches it on Super+V and
 * delivers the paste to whatever window had focus.
 */
#include "owos.h"
#include "gfx.h"
#include "clipboard.h"
#include "mime.h"

#define W        360
#define H        320
#define BG       0x12161eu
#define HEADER_H 28
#define ROW_H    28
#define PAD      10
#define FG       0xc8d0dcu
#define DIM      0x8aa0b8u
#define ACCENT   0x66ddffu

#define MAX_ROWS 32
static char rows[MAX_ROWS][80];     /* one display line per history item */
static int  nrows;

/* Build the visible rows from the clipboard history. */
static void rebuild(void)
{
	int n = clip_count();
	if (n > MAX_ROWS) n = MAX_ROWS;
	nrows = n;
	for (int i = 0; i < n; i++) {
		char mime[64]; int full = 0; char *preview = 0;
		if (clip_list(i, mime, sizeof mime, &full, &preview) != 0) {
			rows[i][0] = '\0';
			continue;
		}
		int j = 0;
		if (mime_is_text(mime) && preview && preview[0]) {
			for (const char *p = preview; *p && j < (int)sizeof rows[i] - 1; p++)
				rows[i][j++] = (*p == '\n' || *p == '\t' || *p == '\r') ? ' ' : *p;
		} else {
			/* "[image/png] 4096 bytes" for a non-text clip */
			rows[i][j++] = '[';
			for (const char *p = mime; *p && j < (int)sizeof rows[i] - 12; p++) rows[i][j++] = *p;
			rows[i][j++] = ']';
			rows[i][j++] = ' ';
			char num[12]; int k = 0; int v = full;
			if (v == 0) num[k++] = '0';
			while (v) { num[k++] = (char)('0' + v % 10); v /= 10; }
			while (k && j < (int)sizeof rows[i] - 7) rows[i][j++] = num[--k];
			const char *suf = " bytes";
			for (const char *p = suf; *p && j < (int)sizeof rows[i] - 1; p++) rows[i][j++] = *p;
		}
		rows[i][j] = '\0';
		free(preview);
	}
}

static void render(int id)
{
	gfx_clear(BG);
	gfx_ui_text(PAD, (HEADER_H - UIFONT_H) / 2, "Clipboard history", ACCENT);
	if (nrows == 0) {
		gfx_ui_text(PAD, H / 2 - UIFONT_H / 2, "Nothing has been copied yet.", DIM);
	} else {
		int y = HEADER_H;
		for (int i = 0; i < nrows; i++) {
			gfx_ui_text(PAD, y + (ROW_H - UIFONT_H) / 2, rows[i], FG);
			y += ROW_H;
		}
	}
	win_present(id);
}

/* Re-add history item `idx` as the newest clip (get its bytes, put them back). */
static void promote(int idx)
{
	char mime[64]; void *data = 0; int len = 0;
	if (clip_get(idx, mime, sizeof mime, &data, &len) == 0) {
		clip_put(mime, data, len);
		free(data);
	}
}

int main(void)
{
	int id = win_create(W, H, "Clipboard");
	if (id < 0) { puts("cliphist: no window\n"); return 1; }
	gfx_init(win_fb(), W, H);
	win_resizable(id, 0);

	rebuild();
	render(id);

	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev)) {
			if (ev.type == WEV_CLOSE) { win_close(id); return 1; }
			if (ev.type == WEV_KEY && ev.a == 0x1B) { win_close(id); return 1; }  /* Esc */
			if (ev.type == WEV_MOUSE && nrows > 0 && ev.b >= HEADER_H) {
				int row = (ev.b - HEADER_H) / ROW_H;
				if (row >= 0 && row < nrows) {
					promote(row);
					win_close(id);
					return 0;          /* 0 = a clip was selected */
				}
			}
		}
		win_wait(id);
	}
}
