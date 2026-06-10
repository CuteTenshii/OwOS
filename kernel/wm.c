/* wm.c — a tiny compositing window manager.
 *
 * Once the framebuffer is up, this owns the screen: a desktop background, a set
 * of windows with draggable title bars, a hardware-ish mouse cursor, and a
 * taskbar. It runs as one kernel thread ("wm") that, whenever the mouse moves,
 * a window is dragged, the clock ticks, or the terminal's text changes,
 * recomposes the whole scene into the off-screen back buffer and flips it to
 * the screen in one shot (so there's never a half-drawn frame).
 *
 * The terminal window is special: its client area is the 80x25 console shadow
 * grid (see vga.c), so the real shell renders into a movable window with no
 * changes to the shell itself. Keyboard input still flows through the kernel's
 * main loop into the shell; the mouse only drives the window manager.
 */

#include <stdint.h>
#include "wm.h"
#include "fb.h"
#include "vga.h"
#include "mouse.h"
#include "timer.h"
#include "rtc.h"
#include "sched.h"

/* The 16 VGA text colors as RGB, so the terminal grid renders in its palette. */
static const uint32_t vga_pal[16] = {
	0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
	0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
	0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
	0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

#define TITLE_H   22         /* title-bar height                              */
#define BORDER    2
#define BAR_H     28         /* taskbar height                                */

enum win_kind { WK_TERMINAL, WK_CLOCK, WK_ABOUT };

struct window {
	int  x, y, w, h;
	const char *title;
	enum win_kind kind;
	int  closable;
	int  visible;
};

#define MAXWIN 8
static struct window wins[MAXWIN];
static int            nwin;
static int            zlist[MAXWIN];   /* window indices, bottom .. top        */

static int screen_w, screen_h;

/* 12x19 arrow cursor: 'X' outline, '.' fill, ' ' transparent. */
static const char *const cursor_glyph[] = {
	"X           ",
	"XX          ",
	"X.X         ",
	"X..X        ",
	"X...X       ",
	"X....X      ",
	"X.....X     ",
	"X......X    ",
	"X.......X   ",
	"X........X  ",
	"X.....XXXXX ",
	"X..X..X     ",
	"X.X X..X    ",
	"XX  X..X    ",
	"X    X..X   ",
	"     X..X   ",
	"      X..X  ",
	"      X..X  ",
	"       XX   ",
};
#define CURSOR_W 12
#define CURSOR_H 19

/* ----------------------------------------------------------- z-order helpers */

static int top_window(void) { return nwin ? zlist[nwin - 1] : -1; }

static void raise_window(int idx)
{
	int at = -1;
	for (int i = 0; i < nwin; i++)
		if (zlist[i] == idx) { at = i; break; }
	if (at < 0)
		return;
	for (int i = at; i < nwin - 1; i++)
		zlist[i] = zlist[i + 1];
	zlist[nwin - 1] = idx;
}

/* Topmost visible window containing (px,py), or -1. */
static int window_at(int px, int py)
{
	for (int i = nwin - 1; i >= 0; i--) {
		struct window *w = &wins[zlist[i]];
		if (!w->visible)
			continue;
		if (px >= w->x && px < w->x + w->w &&
		    py >= w->y && py < w->y + w->h)
			return zlist[i];
	}
	return -1;
}

/* ------------------------------------------------------------- text helpers */

static void put2(char *p, int v) { p[0] = '0' + v / 10; p[1] = '0' + v % 10; }

/* --------------------------------------------------------------- rendering */

static void draw_background(void)
{
	/* A smooth vertical gradient: deep indigo at top to slate at the bottom. */
	for (int y = 0; y < screen_h; y++) {
		int t = y * 255 / screen_h;
		uint8_t r = (uint8_t)(24 + t * 40 / 255);
		uint8_t g = (uint8_t)(26 + t * 56 / 255);
		uint8_t b = (uint8_t)(54 + t * 70 / 255);
		fb_fill_rect(0, y, screen_w, 1, fb_rgb(r, g, b));
	}
}

/* Render the 80x25 console shadow grid into a terminal window's client area. */
static void draw_terminal_client(int cx, int cy, int focused)
{
	const uint16_t *grid = vga_shadow();
	int cols = vga_cols(), rows = vga_rows();

	for (int row = 0; row < rows; row++) {
		for (int col = 0; col < cols; col++) {
			uint16_t cell = grid[row * cols + col];
			unsigned char ch = cell & 0xFF;
			uint8_t attr = cell >> 8;
			uint32_t fg = vga_pal[attr & 0x0F];
			uint32_t bg = vga_pal[(attr >> 4) & 0x0F];
			fb_glyph(cx + col * 8, cy + row * 16, ch, fg, bg);
		}
	}

	/* A solid block cursor at the console caret (kept non-blinking so an idle
	 * shell needs no per-second repaint of the terminal). */
	if (focused) {
		uint16_t pos = vga_get_pos();
		int row = pos / cols, col = pos % cols;
		fb_fill_rect(cx + col * 8, cy + row * 16 + 13, 8, 2, vga_pal[15]);
	}
}

static void draw_clock_client(int cx, int cy, int w, int h)
{
	struct rtc_time t;
	rtc_now(&t);
	char s[9] = "00:00:00";
	put2(s + 0, t.hour);
	put2(s + 3, t.min);
	put2(s + 6, t.sec);

	fb_fill_rect(cx, cy, w, h, 0x101820);
	/* Each glyph is 8px; center the 8-char string roughly in the client. */
	int tx = cx + (w - 8 * 8) / 2;
	int ty = cy + (h - 16) / 2;
	fb_text(tx, ty, s, 0x66ddff, 0x101820);
}

static void draw_about_client(int cx, int cy, int w, int h)
{
	fb_fill_rect(cx, cy, w, h, 0x181c24);
	const char *lines[] = {
		"OwOS",
		"a from-scratch x86 kernel",
		"",
		"now with a window manager,",
		"a mouse, and real pixels.",
		"",
		"drag me by the title bar.",
	};
	for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		uint32_t c = (i == 0) ? 0x9ad0ff : 0xc8d0dc;
		fb_text(cx + 12, cy + 10 + (int)i * 18, lines[i], c, 0x181c24);
	}
}

static void draw_window(int idx, int focused)
{
	struct window *w = &wins[idx];
	uint32_t title_bg = focused ? 0x2f6fb0 : 0x55606e;
	uint32_t frame    = 0x10141a;

	/* Outer frame and title bar. */
	fb_fill_rect(w->x, w->y, w->w, w->h, frame);
	fb_fill_rect(w->x, w->y, w->w, TITLE_H, title_bg);
	fb_text(w->x + 8, w->y + 3, w->title, 0xffffff, title_bg);

	if (w->closable) {
		int bx = w->x + w->w - 18, by = w->y + 4;
		fb_fill_rect(bx, by, 14, 14, 0xc04030);
		fb_text(bx + 3, by - 1, "x", 0xffffff, 0xc04030);
	}

	/* Client area inset by the border and below the title bar. */
	int cx = w->x + BORDER;
	int cy = w->y + TITLE_H;
	int cw = w->w - 2 * BORDER;
	int ch = w->h - TITLE_H - BORDER;

	switch (w->kind) {
	case WK_TERMINAL: draw_terminal_client(cx, cy, focused);   break;
	case WK_CLOCK:    draw_clock_client(cx, cy, cw, ch);       break;
	case WK_ABOUT:    draw_about_client(cx, cy, cw, ch);       break;
	}
	fb_rect(w->x, w->y, w->w, w->h, 0x05070a);
}

static void draw_cursor(int mx, int my)
{
	for (int row = 0; row < CURSOR_H; row++) {
		const char *line = cursor_glyph[row];
		for (int col = 0; line[col]; col++) {
			char p = line[col];
			if (p == 'X')
				fb_pixel(mx + col, my + row, 0x000000);
			else if (p == '.')
				fb_pixel(mx + col, my + row, 0xffffff);
		}
	}
}

static void draw_taskbar(void)
{
	int y = screen_h - BAR_H;
	fb_fill_rect(0, y, screen_w, BAR_H, 0x0c0e14);
	fb_fill_rect(0, y, screen_w, 1, 0x2a3550);
	fb_text(10, y + 6, "OwOS", 0x9ad0ff, 0x0c0e14);

	struct rtc_time t;
	rtc_now(&t);
	char s[9] = "00:00:00";
	put2(s + 0, t.hour);
	put2(s + 3, t.min);
	put2(s + 6, t.sec);
	fb_text(screen_w - 8 * 8 - 12, y + 6, s, 0xc8d0dc, 0x0c0e14);
}

static void compose(int mx, int my)
{
	draw_background();
	int top = top_window();
	for (int i = 0; i < nwin; i++) {
		int idx = zlist[i];
		if (wins[idx].visible)
			draw_window(idx, idx == top);
	}
	draw_taskbar();
	draw_cursor(mx, my);
	fb_present();
}

/* ----------------------------------------------------------------- input */

static void handle_input(int mx, int my, int buttons)
{
	static int  prev_down;
	static int  dragging = -1;
	static int  drag_dx, drag_dy;

	int down = buttons & 1;        /* left button                              */

	if (down && !prev_down) {      /* press                                    */
		int idx = window_at(mx, my);
		if (idx >= 0) {
			raise_window(idx);
			struct window *w = &wins[idx];
			/* Close box? */
			if (w->closable &&
			    mx >= w->x + w->w - 18 && mx < w->x + w->w - 4 &&
			    my >= w->y + 4 && my < w->y + 18) {
				w->visible = 0;
			} else if (my < w->y + TITLE_H) {   /* grab the title bar          */
				dragging = idx;
				drag_dx = mx - w->x;
				drag_dy = my - w->y;
			}
		}
	} else if (down && dragging >= 0) {         /* drag                        */
		struct window *w = &wins[dragging];
		int nx = mx - drag_dx, ny = my - drag_dy;
		if (nx < 0) nx = 0;
		if (ny < 0) ny = 0;
		if (nx > screen_w - w->w) nx = screen_w - w->w;
		if (ny > screen_h - BAR_H - TITLE_H) ny = screen_h - BAR_H - TITLE_H;
		w->x = nx;
		w->y = ny;
	} else if (!down) {                          /* release                     */
		dragging = -1;
	}

	prev_down = down;
}

/* ---------------------------------------------------------------- the thread */

static void add_window(int x, int y, int w, int h, const char *title,
                       enum win_kind kind, int closable)
{
	if (nwin >= MAXWIN)
		return;
	int idx = nwin;
	wins[idx] = (struct window){ x, y, w, h, title, kind, closable, 1 };
	zlist[idx] = idx;
	nwin++;
}

/* Damage list: the rectangles that changed this frame. The compositor redraws
 * the scene clipped to each one and presents only those pixels, instead of
 * pushing the whole screen to VRAM every frame. */
#define MAXDMG 16
struct rect { int x, y, w, h; };
static struct rect dmg[MAXDMG];
static int ndmg;

static void damage_add(int x, int y, int w, int h)
{
	if (w > 0 && h > 0 && ndmg < MAXDMG) {
		dmg[ndmg].x = x; dmg[ndmg].y = y;
		dmg[ndmg].w = w; dmg[ndmg].h = h;
		ndmg++;
	}
}

static void wm_thread(void)
{
	/* Previous-frame snapshots, to diff against and damage what moved. */
	int prev_x[MAXWIN], prev_y[MAXWIN], prev_vis[MAXWIN], prev_z[MAXWIN];
	int prev_mx = -9999, prev_my = -9999;
	uint32_t last_dirty = 0xFFFFFFFF;
	int      last_sec = -1;
	int      inited = 0;

	for (;;) {
		int mx = mouse_x(), my = mouse_y(), btn = mouse_buttons();
		handle_input(mx, my, btn);

		ndmg = 0;
		int full = !inited;       /* only the very first frame paints all      */

		if (!full) {
			/* A z-order/focus/visibility change only ever affects pixels inside
			 * window rectangles (never the bare desktop between them), so damage
			 * every window's old and new rect rather than the whole screen. */
			int zvis = 0;
			for (int i = 0; i < nwin; i++)
				if (zlist[i] != prev_z[i] || wins[i].visible != prev_vis[i])
					zvis = 1;
			if (zvis)
				for (int i = 0; i < nwin; i++) {
					damage_add(prev_x[i], prev_y[i], wins[i].w, wins[i].h);
					damage_add(wins[i].x, wins[i].y, wins[i].w, wins[i].h);
				}

			/* Windows that moved: damage both their old and new rectangles. */
			for (int i = 0; i < nwin; i++)
				if (wins[i].x != prev_x[i] || wins[i].y != prev_y[i]) {
					damage_add(prev_x[i], prev_y[i], wins[i].w, wins[i].h);
					damage_add(wins[i].x, wins[i].y, wins[i].w, wins[i].h);
				}
			/* The cursor moved: damage where it was and where it is. */
			if (mx != prev_mx || my != prev_my) {
				damage_add(prev_mx, prev_my, CURSOR_W, CURSOR_H);
				damage_add(mx, my, CURSOR_W, CURSOR_H);
			}
			/* The terminal printed something: damage that window. */
			if (vga_dirty() != last_dirty)
				for (int i = 0; i < nwin; i++)
					if (wins[i].kind == WK_TERMINAL && wins[i].visible)
						damage_add(wins[i].x, wins[i].y, wins[i].w, wins[i].h);
			/* The clock ticked: damage the clock window and the taskbar. */
			if ((int)timer_seconds() != last_sec) {
				for (int i = 0; i < nwin; i++)
					if (wins[i].kind == WK_CLOCK && wins[i].visible)
						damage_add(wins[i].x, wins[i].y, wins[i].w, wins[i].h);
				damage_add(0, screen_h - BAR_H, screen_w, BAR_H);
			}
		}

		if (full) {
			fb_clip_full();
			compose(mx, my);
			fb_present();
		} else {
			for (int k = 0; k < ndmg; k++) {
				fb_set_clip(dmg[k].x, dmg[k].y, dmg[k].w, dmg[k].h);
				compose(mx, my);
				fb_present();
			}
		}

		/* Snapshot for next frame's diff. */
		for (int i = 0; i < nwin; i++) {
			prev_x[i] = wins[i].x;   prev_y[i] = wins[i].y;
			prev_vis[i] = wins[i].visible; prev_z[i] = zlist[i];
		}
		prev_mx = mx; prev_my = my;
		last_dirty = vga_dirty();
		last_sec = (int)timer_seconds();
		inited = 1;

		timer_sleep(15);           /* ~50 fps cap; idle when nothing changes   */
	}
}

void wm_init(void)
{
	if (!fb_active())
		return;

	screen_w = (int)fb_width();
	screen_h = (int)fb_height();

	/* The terminal: client is exactly 80x25 cells (640x400 px). */
	int term_w = vga_cols() * 8 + 2 * BORDER;
	int term_h = vga_rows() * 16 + TITLE_H + BORDER;
	add_window(40, 40, term_w, term_h, "terminal", WK_TERMINAL, 0);
	add_window(screen_w - 220, 60, 180, 110, "clock", WK_CLOCK, 1);
	add_window(screen_w - 300, 220, 260, 170, "about", WK_ABOUT, 1);
	raise_window(0);               /* the terminal starts focused / on top     */

	/* Route the console into the terminal window, then start compositing. */
	mouse_init(screen_w - 1, screen_h - 1);
	vga_use_framebuffer(1);
	sched_create_thread("wm", wm_thread);
}
