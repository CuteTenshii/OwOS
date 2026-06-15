/* clock.c — a digital clock app. Redraws when the second ticks. */
#include "owos.h"
#include "gfx.h"

#define W 180
#define H 110
#define BG 0x101820u

static void put2(char *p, int v) { p[0] = '0' + v / 10; p[1] = '0' + v % 10; }

int main(void)
{
	int id = win_create(W, H, "clock");
	if (id < 0) { puts("clock: no window\n"); return 1; }
	gfx_init(win_fb(), W, H);
	win_resizable(id, 0);            /* fixed-size clock face */

	unsigned last = 0xffffffff;
	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev))
			if (ev.type == WEV_CLOSE) { win_close(id); return 0; }

		unsigned t = now_hms();
		if (t != last) {
			last = t;
			char s[9] = "00:00:00";
			put2(s + 0, (int)((t >> 16) & 0xFF));
			put2(s + 3, (int)((t >> 8) & 0xFF));
			put2(s + 6, (int)(t & 0xFF));
			gfx_clear(BG);
			gfx_ui_text((W - gfx_ui_width(s)) / 2, (H - UIFONT_H) / 2, s, 0x66ddff);
			win_present(id);
		}
		win_wait(id);
	}
}
