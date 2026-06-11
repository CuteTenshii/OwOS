/* about.c — the "about" window, as a ring-3 app. */
#include "owos.h"
#include "gfx.h"

#define W 280
#define H 180
#define BG 0x181c24u

int main(void)
{
	int id = win_create(W, H, "about");
	if (id < 0) { puts("about: no window\n"); return 1; }
	gfx_init(win_fb(), W, H);

	gfx_clear(BG);
	const char *lines[] = {
		"OwOS",
		"a from-scratch x86 kernel",
		"",
		"now with ring-3 GUI apps:",
		"windows, a mouse, real pixels.",
		"",
		"launched from the Start menu.",
	};
	for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
		gfx_text(12, 10 + (int)i * 18, lines[i], i == 0 ? 0x9ad0ff : 0xc8d0dc, BG);
	win_present(id);

	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev))
			if (ev.type == WEV_CLOSE) { win_close(id); return 0; }
		win_wait(id);
	}
}
