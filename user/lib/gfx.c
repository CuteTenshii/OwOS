/* gfx.c — user-space drawing into a window's shared pixel buffer. See gfx.h. */
#include "gfx.h"
#include "font8x16.h"

static unsigned *g_fb;
static int       g_w, g_h;

void gfx_init(unsigned *fb, int w, int h) { g_fb = fb; g_w = w; g_h = h; }

void gfx_pixel(int x, int y, unsigned color)
{
	if ((unsigned)x < (unsigned)g_w && (unsigned)y < (unsigned)g_h)
		g_fb[y * g_w + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, unsigned color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > g_w) w = g_w - x;
	if (y + h > g_h) h = g_h - y;
	for (int r = 0; r < h; r++) {
		unsigned *p = g_fb + (long)(y + r) * g_w + x;
		for (int c = 0; c < w; c++)
			p[c] = color;
	}
}

void gfx_clear(unsigned color) { gfx_fill_rect(0, 0, g_w, g_h, color); }

void gfx_rect(int x, int y, int w, int h, unsigned color)
{
	gfx_fill_rect(x, y, w, 1, color);
	gfx_fill_rect(x, y + h - 1, w, 1, color);
	gfx_fill_rect(x, y, 1, h, color);
	gfx_fill_rect(x + w - 1, y, 1, h, color);
}

void gfx_glyph(int x, int y, unsigned char ch, unsigned fg, unsigned bg)
{
	const unsigned char *gl = font8x16[ch];
	int transparent = (bg & GFX_TRANSPARENT) != 0;
	for (int row = 0; row < 16; row++) {
		int sy = y + row;
		if ((unsigned)sy >= (unsigned)g_h)
			continue;
		unsigned char bits = gl[row];
		for (int col = 0; col < 8; col++) {
			int sx = x + col;
			if ((unsigned)sx >= (unsigned)g_w)
				continue;
			if (bits & (0x80 >> col))
				g_fb[sy * g_w + sx] = fg;
			else if (!transparent)
				g_fb[sy * g_w + sx] = bg;
		}
	}
}

void gfx_text(int x, int y, const char *s, unsigned fg, unsigned bg)
{
	for (; *s; s++, x += 8)
		gfx_glyph(x, y, (unsigned char)*s, fg, bg);
}

/* --------------------------------------------------------------- GUI widgets */

static int wstrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

void status_set(struct status_bar *s, const char *msg)
{
	int i = 0;
	for (; msg[i] && i < (int)sizeof(s->msg) - 1; i++)
		s->msg[i] = msg[i];
	s->msg[i] = '\0';
}

void status_draw(const struct status_bar *s, int x, int y, int w,
                 unsigned fg, unsigned bg)
{
	gfx_fill_rect(x, y, w, GUI_BAR_H, bg);
	gfx_text(x + 6, y + 1, s->msg, fg, bg);
}

void menu_draw(const struct menu *m, int x, int y)
{
	unsigned bg = m->open ? 0x3a4a60 : 0x232a36;
	int tw = wstrlen(m->title) * 8 + 6;
	if (m->open)
		gfx_fill_rect(x, y, tw, GUI_BAR_H, bg);
	gfx_text(x + 3, y + 1, m->title, 0xe0e6ee, bg);
	if (m->open) {
		gfx_fill_rect(x, y + GUI_BAR_H, GUI_MENU_W, m->n * GUI_ROW_H, 0x2a3342);
		for (int i = 0; i < m->n; i++)
			gfx_text(x + 6, y + GUI_BAR_H + i * GUI_ROW_H, m->items[i],
			         0xe0e6ee, 0x2a3342);
		gfx_rect(x, y + GUI_BAR_H, GUI_MENU_W, m->n * GUI_ROW_H, 0x05070a);
	}
}

int menu_click(struct menu *m, int mx, int my, int x, int y)
{
	int tw = wstrlen(m->title) * 8 + 6;
	if (my >= y && my < y + GUI_BAR_H) {        /* the bar row */
		m->open = (mx >= x && mx < x + tw) ? !m->open : 0;
		return -1;
	}
	if (m->open) {                              /* a popup item? */
		int i = (my - (y + GUI_BAR_H)) / GUI_ROW_H;
		m->open = 0;
		if (mx >= x && mx < x + GUI_MENU_W && i >= 0 && i < m->n)
			return i;
	}
	return -1;
}
