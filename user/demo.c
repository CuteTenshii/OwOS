/* demo.c — a minimal ring-3 windowed program, proving the windowing layer.
 *
 * Creates a window, draws into its shared buffer, and runs an event loop:
 * left-clicks drop a marker and bump a counter, 'q' or the close box quits.
 */
#include "owos.h"
#include "gfx.h"

#define W 360
#define H 240
#define BG 0x202830u

static void uitoa(unsigned v, char *out)
{
	char t[12];
	int k = 0;
	if (v == 0) { out[0] = '0'; out[1] = 0; return; }
	while (v) { t[k++] = '0' + v % 10; v /= 10; }
	int p = 0;
	while (k)
		out[p++] = t[--k];
	out[p] = 0;
}

int main(void)
{
	int id = win_create(W, H, "demo");
	if (id < 0) {
		puts("demo: win_create failed\n");
		return 1;
	}
	gfx_init(win_fb(), W, H);

	int clicks = 0, mx = W / 2, my = H / 2;
	int need = 1;
	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev)) {
			need = 1;
			if (ev.type == WEV_CLOSE) { win_close(id); return 0; }
			if (ev.type == WEV_KEY && ev.a == 'q') { win_close(id); return 0; }
			if (ev.type == WEV_MOUSE) { clicks++; mx = ev.a; my = ev.b; }
		}
		if (need) {
			gfx_clear(BG);
			gfx_text(12, 10, "hello from ring 3!", 0x9ad0ff, BG);
			gfx_text(12, 30, "click me - press q to quit", 0xc8d0dc, BG);
			gfx_fill_rect(mx - 5, my - 5, 10, 10, 0xff6680);
			char n[12];
			uitoa((unsigned)clicks, n);
			gfx_text(12, H - 24, "clicks:", 0x8aa0b8, BG);
			gfx_text(12 + 8 * 8, H - 24, n, 0xffffff, BG);
			win_present(id);
			need = 0;
		}
		win_wait(id);
	}
}
