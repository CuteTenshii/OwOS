/* app_term.c — the terminal window: the shell's home on the desktop.
 *
 * Special among the apps in that it has no state of its own — its client area
 * is the 80x25 VGA console shadow grid (vga.c), rendered cell-by-cell with the
 * 8x16 font. The real shell prints into that grid via the usual kernel path, so
 * it runs in a movable window with zero shell changes. dirty() tracks the
 * console's change counter so the compositor repaints only when text changes.
 */
#include <stdint.h>
#include "wm.h"
#include "apps.h"
#include "fb.h"
#include "vga.h"

/* The 16 VGA text colors as RGB, so the grid renders in its palette. */
static const uint32_t vga_pal[16] = {
	0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
	0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
	0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
	0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static void term_draw(struct window *w, int cx, int cy, int cw, int ch,
                      int focused)
{
	(void)w; (void)cw; (void)ch;
	const uint16_t *grid = vga_shadow();
	int cols = vga_cols(), rows = vga_rows();

	for (int row = 0; row < rows; row++) {
		for (int col = 0; col < cols; col++) {
			uint16_t cell = grid[row * cols + col];
			unsigned char chr = cell & 0xFF;
			uint8_t attr = cell >> 8;
			uint32_t fg = vga_pal[attr & 0x0F];
			uint32_t bg = vga_pal[(attr >> 4) & 0x0F];
			fb_glyph(cx + col * 8, cy + row * 16, chr, fg, bg);
		}
	}

	/* A solid block cursor at the console caret (non-blinking, so an idle shell
	 * needs no per-second repaint of the terminal). */
	if (focused) {
		uint16_t pos = vga_get_pos();
		int row = pos / cols, col = pos % cols;
		fb_fill_rect(cx + col * 8, cy + row * 16 + 13, 8, 2, vga_pal[15]);
	}
}

static uint32_t term_dirty(struct window *w) { (void)w; return vga_dirty(); }

void app_term_init(void)
{
	/* The client is exactly 80x25 cells (640x400 px). */
	int term_w = vga_cols() * 8 + 2 * WM_BORDER;
	int term_h = vga_rows() * 16 + WM_TITLE_H + WM_BORDER;
	struct window *w = wm_create_window(40, 40, term_w, term_h, "terminal", 0);
	if (!w)
		return;
	w->draw  = term_draw;
	w->dirty = term_dirty;
	w->resizable = 0;     /* the console grid is a fixed 80x25 */
	w->cursor = WM_CURSOR_TEXT;   /* I-beam over the console text */
}
