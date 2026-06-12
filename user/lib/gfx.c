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
		for (int i = 0; i < m->n; i++) {
			int ry = y + GUI_BAR_H + i * GUI_ROW_H;
			gfx_text(x + 6, ry, m->items[i], 0xe0e6ee, 0x2a3342);
			/* Accelerator hint ("Ctrl+S"), right-aligned and dimmed. */
			if (m->hints && m->hints[i] && m->hints[i][0]) {
				int hw = wstrlen(m->hints[i]) * 8;
				gfx_text(x + GUI_MENU_W - 6 - hw, ry, m->hints[i],
				         0x8a93a0, 0x2a3342);
			}
		}
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

int menu_shortcut(const struct menu *m, int key)
{
	if (!m->keys || !key)
		return -1;
	for (int i = 0; i < m->n; i++)
		if (m->keys[i] == key)
			return i;
	return -1;
}

/* ------------------------------------------------------------- menu bar */

#define MENUBAR_GAP 4   /* px between adjacent titles on the bar */

/* x of menu i's title: the bar origin plus the widths of the titles before it. */
static int menubar_title_x(const struct menubar *mb, int i)
{
	int x = mb->x;
	for (int k = 0; k < i; k++)
		x += wstrlen(mb->menus[k]->title) * 8 + 6 + MENUBAR_GAP;
	return x;
}

void menubar_close(struct menubar *mb)
{
	for (int i = 0; i < mb->n; i++)
		mb->menus[i]->open = 0;
}

void menubar_draw(const struct menubar *mb)
{
	/* Closed titles first; the open one (if any) last so its dropdown overlays
	 * a neighbouring title. */
	int openi = -1;
	for (int i = 0; i < mb->n; i++) {
		if (mb->menus[i]->open) { openi = i; continue; }
		menu_draw(mb->menus[i], menubar_title_x(mb, i), mb->y);
	}
	if (openi >= 0)
		menu_draw(mb->menus[openi], menubar_title_x(mb, openi), mb->y);
}

int menubar_click(struct menubar *mb, int mx, int my, int *menu, int *item)
{
	*menu = -1;
	*item = -1;

	/* On the title row: toggle the clicked title and close the others, so only
	 * one dropdown is ever open. */
	if (my >= mb->y && my < mb->y + GUI_BAR_H) {
		for (int i = 0; i < mb->n; i++) {
			int x = menubar_title_x(mb, i);
			int tw = wstrlen(mb->menus[i]->title) * 8 + 6;
			if (mx >= x && mx < x + tw) {
				int wasopen = mb->menus[i]->open;
				menubar_close(mb);
				mb->menus[i]->open = !wasopen;
				*menu = i;
				return 1;
			}
		}
		menubar_close(mb);        /* empty bar area: dismiss any open menu */
		return 1;
	}

	/* Below the bar: hand the click to whichever menu is open (menu_click closes
	 * it and returns the chosen item, or -1 if the click missed the dropdown). */
	for (int i = 0; i < mb->n; i++) {
		if (!mb->menus[i]->open)
			continue;
		int it = menu_click(mb->menus[i], mx, my, menubar_title_x(mb, i), mb->y);
		if (it >= 0) { *menu = i; *item = it; }
		return 1;
	}
	return 0;
}

int menubar_shortcut(const struct menubar *mb, int key, int *menu)
{
	for (int i = 0; i < mb->n; i++) {
		int it = menu_shortcut(mb->menus[i], key);
		if (it >= 0) { if (menu) *menu = i; return it; }
	}
	if (menu)
		*menu = -1;
	return -1;
}
