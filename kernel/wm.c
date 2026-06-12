/* wm.c — a tiny compositing window manager (app-agnostic core).
 *
 * Once the framebuffer is up, this owns the screen: a desktop background, a set
 * of windows with draggable title bars, a hardware-ish mouse cursor, and a
 * taskbar. It runs as one kernel thread ("wm") that, whenever the mouse moves,
 * a window is dragged, the taskbar clock ticks, or any window's dirty() value
 * changes, recomposes the affected rectangles into the off-screen back buffer
 * and flips them to the screen in one shot (so there's never a half-drawn
 * frame).
 *
 * What a window *contains* is none of this file's business: each window carries
 * draw/click/dirty callbacks (see wm.h) supplied by an app module (app_*.c).
 * The compositor only frames, stacks, drags, and damages them. Apps register
 * themselves from apps_start(); adding one never touches this file.
 *
 * Keyboard input still flows through the kernel's main loop into the shell; the
 * mouse only drives the window manager.
 */

#include <stdint.h>
#include "wm.h"
#include "apps.h"
#include "fb.h"
#include "vga.h"
#include "mouse.h"
#include "timer.h"
#include "rtc.h"
#include "sched.h"
#include "edid.h"
#include "crash.h"

#define MAXWIN 40    /* compositor window slots: app windows + terminal + chrome */
static struct window wins[MAXWIN];
static int            nwin;
static int            zlist[MAXWIN];   /* window indices, bottom .. top        */

static int screen_w, screen_h;

int wm_screen_w(void) { return screen_w; }
int wm_screen_h(void) { return screen_h; }

/* Taskbar Start button + launcher popup. */
#define START_W   70         /* width of the "OwOS" Start button              */
#define SM_ITEM_H 20         /* start-menu item height                        */
#define SM_W      150        /* start-menu popup width                        */
static int start_open;
static int sm_h(void)  { return apps_menu_count() * SM_ITEM_H; }
static int sm_top(void){ return screen_h - WM_BAR_H - sm_h(); }

/* Bitmap cursors: 'X' outline (black), '.' fill (white), ' ' transparent. Each
 * shape declares a hotspot — the pixel that actually points, drawn at the mouse
 * coordinate (the arrow's tip is its top-left; the hand's is its fingertip). */

/* 12x19 arrow — the default pointer. Hotspot at its tip (0,0). */
static const char *const arrow_glyph[] = {
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

/* Pointing hand — over clickable chrome (buttons, menu items). Hotspot =
 * fingertip (2,0). */
static const char *const hand_glyph[] = {
	"  XX     ",
	"  X.X    ",
	"  X.X    ",
	"  X.X    ",
	"  X.XXXX ",
	"  X.X..X ",
	"XXX...X.X",
	"X.......X",
	"X.......X",
	"X.......X",
	" X......X",
	" X......X",
	"  X.....X",
	"  XXXXXXX",
};

/* I-beam — over editable text. Hotspot at its waist (2,7). */
static const char *const text_glyph[] = {
	"X...X",
	"XX.XX",
	" X.X ",
	" X.X ",
	" X.X ",
	" X.X ",
	" X.X ",
	" X.X ",
	" X.X ",
	" X.X ",
	" X.X ",
	" X.X ",
	"XX.XX",
	"X...X",
};

struct cur_shape { const char *const *rows; int nrows, hotx, hoty; };
static const struct cur_shape cur_shapes[] = {
	[WM_CURSOR_ARROW] = { arrow_glyph, 19, 0, 0 },
	[WM_CURSOR_HAND]  = { hand_glyph,  14, 2, 0 },
	[WM_CURSOR_TEXT]  = { text_glyph,  14, 2, 7 },
};

/* A damage box generous enough to cover any shape drawn around the hotspot:
 * the arrow reaches +18 below / +11 right of its tip, the spinner ±8 around its
 * centre. Damaged at the old and new pointer position every time it moves. */
#define CUR_DMG_PAD 9
#define CUR_DMG_W   24
#define CUR_DMG_H   30

/* Bumped once per compositor frame; drives the spinner's animation. */
static unsigned cursor_frame;

static void put2(char *p, int v) { p[0] = '0' + v / 10; p[1] = '0' + v % 10; }

/* Copy `s` into the fixed title buffer, NUL-terminated and truncated to fit. */
static void title_copy(char *dst, const char *s)
{
	int i = 0;
	for (; s[i] && i < WM_TITLE_MAX - 1; i++)
		dst[i] = s[i];
	dst[i] = '\0';
}

/* ----------------------------------------------------------- z-order helpers */

/* Topmost *visible* window — the one that holds focus (keyboard + highlight). */
static int topmost_visible(void)
{
	for (int i = nwin - 1; i >= 0; i--)
		if (wins[zlist[i]].visible)
			return zlist[i];
	return -1;
}

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

static void draw_window(struct window *w, int focused)
{
	uint32_t title_bg = focused ? 0x2f6fb0 : 0x55606e;
	uint32_t frame    = 0x10141a;

	/* Outer frame and title bar. */
	fb_fill_rect(w->x, w->y, w->w, w->h, frame);
	fb_fill_rect(w->x, w->y, w->w, WM_TITLE_H, title_bg);
	fb_text(w->x + 8, w->y + 3, w->title, 0xffffff, title_bg);

	if (w->closable) {
		int bx = w->x + w->w - 18, by = w->y + 4;
		fb_fill_rect(bx, by, 14, 14, 0xc04030);
		fb_text(bx + 3, by - 1, "x", 0xffffff, 0xc04030);
	}

	/* Client area inset by the border and below the title bar. */
	int cx = w->x + WM_BORDER;
	int cy = w->y + WM_TITLE_H;
	int cw = w->w - 2 * WM_BORDER;
	int ch = w->h - WM_TITLE_H - WM_BORDER;
	if (w->draw)
		w->draw(w, cx, cy, cw, ch, focused);

	fb_rect(w->x, w->y, w->w, w->h, 0x05070a);

	/* Resize grip: a few diagonal ticks in the bottom-right corner. */
	if (w->resizable) {
		int gx = w->x + w->w - WM_GRIP, gy = w->y + w->h - WM_GRIP;
		uint32_t gc = focused ? 0x7f93ad : 0x55606e;
		for (int d = 3; d < WM_GRIP; d += 4)
			for (int t = 0; t <= d; t++) {
				fb_pixel(gx + WM_GRIP - 1 - t, gy + WM_GRIP - 1 - (d - t), gc);
			}
	}
}

/* Blit a bitmap shape with its top-left at (x,y). */
static void draw_glyph(const struct cur_shape *s, int x, int y)
{
	for (int row = 0; row < s->nrows; row++) {
		const char *line = s->rows[row];
		for (int col = 0; line[col]; col++) {
			if (line[col] == 'X')
				fb_pixel(x + col, y + row, 0x000000);
			else if (line[col] == '.')
				fb_pixel(x + col, y + row, 0xffffff);
		}
	}
}

/* One outlined dot of the spinner: a black 4x4 square with a coloured 2x2 core,
 * so it reads on any background. */
static void cur_dot(int cx, int cy, uint32_t inner)
{
	fb_fill_rect(cx - 2, cy - 2, 4, 4, 0x000000);
	fb_fill_rect(cx - 1, cy - 1, 2, 2, inner);
}

/* "Busy / loading" — eight dots in a ring with a bright head that rotates,
 * trailing into grey. Centred on the pointer. */
static void draw_busy(int mx, int my)
{
	static const int dx[8] = {  0,  4,  6,  4,  0, -4, -6, -4 };
	static const int dy[8] = { -6, -4,  0,  4,  6,  4,  0, -4 };
	int head = (int)((cursor_frame / 3) & 7);
	for (int i = 0; i < 8; i++) {
		int dist = (head - i + 8) & 7;        /* 0 = the bright head      */
		int b = 40 + (7 - dist) * 30;         /* 40..250 grey ramp        */
		if (b > 255) b = 255;
		cur_dot(mx + dx[i], my + dy[i], fb_rgb((uint8_t)b, (uint8_t)b, (uint8_t)b));
	}
}

/* Plot one cell of the resize cursor: pass 1 lays a white halo (the cell plus
 * its 4-neighbours), pass 0 the black core — together an outlined stroke. */
static void cur_stroke(int x, int y, int halo)
{
	if (halo) {
		fb_pixel(x,     y,     0xffffff);
		fb_pixel(x + 1, y,     0xffffff);
		fb_pixel(x - 1, y,     0xffffff);
		fb_pixel(x,     y + 1, 0xffffff);
		fb_pixel(x,     y - 1, 0xffffff);
	} else {
		fb_pixel(x, y, 0x000000);
	}
}

/* Diagonal double-arrow (↖↘) for the resize grip: a shaft with an arrowhead at
 * each end. Drawn halo-first so the white outline never paints over the core. */
static void draw_resize(int mx, int my)
{
	for (int halo = 1; halo >= 0; halo--) {
		for (int t = -5; t <= 5; t++)
			cur_stroke(mx + t, my + t, halo);          /* shaft           */
		for (int k = 0; k <= 4; k++) {
			cur_stroke(mx - 5 + k, my - 5,     halo);  /* NW head: arms   */
			cur_stroke(mx - 5,     my - 5 + k, halo);
			cur_stroke(mx + 5 - k, my + 5,     halo);  /* SE head: arms   */
			cur_stroke(mx + 5,     my + 5 - k, halo);
		}
	}
}

static void draw_cursor(int kind, int mx, int my)
{
	switch (kind) {
	case WM_CURSOR_BUSY:    draw_busy(mx, my);   return;
	case WM_CURSOR_RESIZE:  draw_resize(mx, my); return;
	default:
		if (kind < 0 || kind >= (int)(sizeof cur_shapes / sizeof cur_shapes[0]) ||
		    !cur_shapes[kind].rows)
			kind = WM_CURSOR_ARROW;
		const struct cur_shape *s = &cur_shapes[kind];
		draw_glyph(s, mx - s->hotx, my - s->hoty);
		return;
	}
}

static void draw_taskbar(void)
{
	int y = screen_h - WM_BAR_H;
	fb_fill_rect(0, y, screen_w, WM_BAR_H, 0x0c0e14);
	fb_fill_rect(0, y, screen_w, 1, 0x2a3550);
	uint32_t btn = start_open ? 0x2f6fb0 : 0x0c0e14;   /* Start button */
	fb_fill_rect(0, y + 1, START_W, WM_BAR_H - 1, btn);
	fb_text(10, y + 6, "OwOS", 0x9ad0ff, btn);

	struct rtc_time t;
	rtc_now(&t);
	char s[9] = "00:00:00";
	put2(s + 0, t.hour);
	put2(s + 3, t.min);
	put2(s + 6, t.sec);
	fb_text(screen_w - 8 * 8 - 12, y + 6, s, 0xc8d0dc, 0x0c0e14);
}

/* The Start menu popup, listing the launchable apps above the taskbar. */
static void draw_start_menu(void)
{
	int n = apps_menu_count();
	int top = sm_top();
	fb_fill_rect(0, top, SM_W, sm_h(), 0x141a24);
	for (int i = 0; i < n; i++) {
		int iy = top + i * SM_ITEM_H;
		fb_text(10, iy + 3, apps_menu_name(i), 0xc8d0dc, 0x141a24);
	}
	fb_rect(0, top, SM_W, sm_h(), 0x05070a);
}

/* Hints from handle_input for cursor selection: a grip drag is live, or a window
 * is still catching up to a finished resize (so we show the spinner briefly). */
static int hover_resizing;
static int hover_settling;

/* Decide which cursor the pointer should be wearing this frame. Resize and
 * "busy" hints from an in-progress interaction win; then clickable chrome (the
 * Start button, an open menu item, a close box) gets the hand; then the grip
 * gets the resize arrow; otherwise a window may dress its own client area. */
static int pick_cursor(int mx, int my)
{
	if (hover_resizing) return WM_CURSOR_RESIZE;
	if (hover_settling) return WM_CURSOR_BUSY;

	int bar_y = screen_h - WM_BAR_H;
	if (my >= bar_y)
		return (mx >= 0 && mx < START_W) ? WM_CURSOR_HAND : WM_CURSOR_ARROW;
	if (start_open && mx >= 0 && mx < SM_W && my >= sm_top())
		return WM_CURSOR_HAND;

	int idx = window_at(mx, my);
	if (idx >= 0) {
		struct window *w = &wins[idx];
		if (w->closable &&
		    mx >= w->x + w->w - 18 && mx < w->x + w->w - 4 &&
		    my >= w->y + 4 && my < w->y + 18)
			return WM_CURSOR_HAND;
		if (w->resizable &&
		    mx >= w->x + w->w - WM_GRIP && mx < w->x + w->w &&
		    my >= w->y + w->h - WM_GRIP && my < w->y + w->h)
			return WM_CURSOR_RESIZE;
		if (w->cursor > 0 &&
		    my >= w->y + WM_TITLE_H && my < w->y + w->h - WM_BORDER &&
		    mx >= w->x + WM_BORDER && mx < w->x + w->w - WM_BORDER)
			return w->cursor;
	}
	return WM_CURSOR_ARROW;
}

static void compose(int mx, int my)
{
	draw_background();
	int top = topmost_visible();
	for (int i = 0; i < nwin; i++) {
		int idx = zlist[i];
		if (wins[idx].visible)
			draw_window(&wins[idx], idx == top);
	}
	draw_taskbar();
	if (start_open)
		draw_start_menu();
	draw_cursor(pick_cursor(mx, my), mx, my);
	/* No fb_present() here: the caller sets the clip rect, composes, and
	 * presents once per frame so multi-rect damage drains the write-combining
	 * buffer a single time instead of once per rectangle. */
}

/* ----------------------------------------------------------------- input */

static void handle_input(int mx, int my, int buttons, int dz)
{
	static int  prev_down;
	static int  dragging = -1;
	static int  resizing = -1;
	static int  app_dragging = -1;  /* window whose app captured the drag        */
	static int  drag_dx, drag_dy;   /* dragging: cursor offset within the title */
	static int  rsz_dx, rsz_dy;     /* resizing: cursor offset from bottom-right */
	/* Live resize. The target frame size trails the cursor every frame; the app
	 * is re-handed it only once it has presented (acked) the previous size, so
	 * each buffer realloc/remap in uw_on_resize lands while the app is parked in
	 * win_wait, never mid-paint. The tracker persists past button release until
	 * the app's content has caught up to the final size. */
	static int      rsz_win = -1;
	static int      rsz_target_w, rsz_target_h;
	static int      rsz_sent_w, rsz_sent_h;
	static uint32_t rsz_ack;

	int down = buttons & 1;        /* left button                              */

	/* Wheel: route to whatever visible window is under the cursor. */
	if (dz) {
		int idx = window_at(mx, my);
		if (idx >= 0 && wins[idx].scroll)
			wins[idx].scroll(&wins[idx], mx, my, dz);
	}

	if (down && !prev_down) {      /* press                                    */
		/* Start button / launcher popup take priority over windows. */
		int bar_y = screen_h - WM_BAR_H;
		if (my >= bar_y && mx >= 0 && mx < START_W) {   /* the Start button   */
			start_open = !start_open;
			prev_down = down;
			return;
		}
		if (start_open) {
			int top = sm_top();
			if (mx >= 0 && mx < SM_W && my >= top && my < bar_y) {
				apps_menu_launch((my - top) / SM_ITEM_H);   /* launch + close */
				start_open = 0;
				prev_down = down;
				return;
			}
			start_open = 0;        /* a click elsewhere closes it, then falls through */
		}
		int idx = window_at(mx, my);
		if (idx >= 0) {
			raise_window(idx);
			struct window *w = &wins[idx];
			/* Close box? An app with on_close is told (so it can free
			 * resources / exit); otherwise the WM just hides the window. */
			if (w->closable &&
			    mx >= w->x + w->w - 18 && mx < w->x + w->w - 4 &&
			    my >= w->y + 4 && my < w->y + 18) {
				if (w->on_close)
					w->on_close(w);
				else
					w->visible = 0;
			} else if (w->resizable &&            /* bottom-right resize grip */
			           mx >= w->x + w->w - WM_GRIP && mx < w->x + w->w &&
			           my >= w->y + w->h - WM_GRIP && my < w->y + w->h) {
				resizing = idx;
				rsz_dx = (w->x + w->w) - mx;
				rsz_dy = (w->y + w->h) - my;
				rsz_win = idx;
				rsz_target_w = w->w; rsz_target_h = w->h;
				rsz_sent_w = rsz_sent_h = -1;  /* force the first hand-off  */
				rsz_ack = 0;
			} else if (my < w->y + WM_TITLE_H) {  /* grab the title bar      */
				dragging = idx;
				drag_dx = mx - w->x;
				drag_dy = my - w->y;
			} else {                               /* press in the client area */
				if (w->click) w->click(w, mx, my);
				if (w->drag)  app_dragging = idx;  /* follow motion until release */
			}
		}
	} else if (down && app_dragging >= 0) {     /* app-captured drag (scrollbar) */
		struct window *w = &wins[app_dragging];
		if (w->drag) w->drag(w, mx, my);
	} else if (down && dragging >= 0) {         /* drag                        */
		struct window *w = &wins[dragging];
		int nx = mx - drag_dx, ny = my - drag_dy;
		if (nx < 0) nx = 0;
		if (ny < 0) ny = 0;
		if (nx > screen_w - w->w) nx = screen_w - w->w;
		if (ny > screen_h - WM_BAR_H - WM_TITLE_H)
			ny = screen_h - WM_BAR_H - WM_TITLE_H;
		w->x = nx;
		w->y = ny;
	} else if (down && resizing >= 0) {         /* resize from the grip        */
		struct window *w = &wins[resizing];
		int nw = mx + rsz_dx - w->x, nh = my + rsz_dy - w->y;
		if (nw < WM_MIN_W) nw = WM_MIN_W;
		if (nh < WM_MIN_H) nh = WM_MIN_H;
		if (w->x + nw > screen_w) nw = screen_w - w->x;
		if (w->y + nh > screen_h - WM_BAR_H) nh = screen_h - WM_BAR_H - w->y;
		w->w = nw;                  /* frame tracks the cursor every frame      */
		w->h = nh;
		rsz_target_w = nw;          /* commit step hands this to the app        */
		rsz_target_h = nh;
	} else if (!down) {                          /* release                     */
		dragging = -1;
		resizing = -1;              /* grip let go; rsz_win lives on to settle  */
		app_dragging = -1;
	}

	/* Drive the live-resize hand-off. Hand the app each new frame size once it
	 * has presented the previous one (so the realloc/remap never collides with
	 * an in-flight paint), and keep going after release until its content has
	 * reached the final size. */
	if (rsz_win >= 0) {
		struct window *w = &wins[rsz_win];
		if (w->dead || !w->on_resize) {
			rsz_win = -1;
		} else {
			if (rsz_target_w != rsz_sent_w || rsz_target_h != rsz_sent_h) {
				uint32_t seq = w->dirty ? w->dirty(w) : 0;
				if (seq != rsz_ack) {          /* app acked the last hand-off */
					w->on_resize(w, rsz_target_w - 2 * WM_BORDER,
					             rsz_target_h - WM_TITLE_H - WM_BORDER);
					rsz_sent_w = rsz_target_w;
					rsz_sent_h = rsz_target_h;
					rsz_ack = w->dirty ? w->dirty(w) : 0;
				}
			} else if (resizing < 0) {
				rsz_win = -1;                  /* released and fully caught up */
			}
		}
	}

	/* Publish cursor hints for pick_cursor: the diagonal arrow while actively
	 * dragging a grip, the spinner while a released resize is still settling. */
	hover_resizing = (resizing >= 0);
	hover_settling = (rsz_win >= 0 && resizing < 0);

	prev_down = down;
}

/* --------------------------------------------------------- app registration */

struct window *wm_create_window(int x, int y, int w, int h,
                                const char *title, int closable)
{
	int idx = -1;
	for (int i = 0; i < nwin; i++)       /* reuse a destroyed slot if any */
		if (wins[i].dead) { idx = i; break; }
	if (idx < 0) {
		if (nwin >= MAXWIN)
			return 0;
		idx = nwin++;
		zlist[idx] = idx;
	}
	wins[idx] = (struct window){ 0 };
	wins[idx].x = x; wins[idx].y = y; wins[idx].w = w; wins[idx].h = h;
	title_copy(wins[idx].title, title);
	wins[idx].closable = closable;
	wins[idx].resizable = 1;       /* opt out with w->resizable = 0 after this  */
	wins[idx].visible = 1;
	raise_window(idx);             /* newest is topmost / focused              */
	return &wins[idx];
}

void wm_destroy_window(struct window *w)
{
	if (!w)
		return;
	w->visible = 0;
	w->dead = 1;
	w->draw = 0; w->click = 0; w->dirty = 0; w->key = 0; w->on_close = 0;
	w->state = 0;
}

void wm_set_title(struct window *w, const char *title)
{
	if (w)
		title_copy(w->title, title);
}

void wm_set_cursor(struct window *w, int cursor)
{
	if (w && cursor >= 0 && cursor < WM_CURSOR_COUNT)
		w->cursor = cursor;
}

int wm_feed_key(char c)
{
	int t = topmost_visible();
	if (t < 0)
		return 0;
	struct window *w = &wins[t];
	if (w->key) {
		w->key(w, c);
		return 1;
	}
	return 0;            /* focused window doesn't take keys (e.g. the terminal) */
}

/* 1 if the focused window is a keyboard-consuming GUI app. The shell uses this
 * while foreground-waiting on a child: if a GUI window holds focus, the shell
 * pumps keystrokes to it (else they belong to a console program's stdin). */
int wm_focus_takes_keys(void)
{
	int t = topmost_visible();
	return t >= 0 && wins[t].key != 0;
}

/* ---------------------------------------------------------------- the thread */

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
	int prev_w[MAXWIN], prev_h[MAXWIN];
	uint32_t prev_dirty[MAXWIN];
	int prev_mx = -9999, prev_my = -9999;
	int      prev_cur = -1;       /* cursor shape shown last frame              */
	int      prev_wheel = mouse_wheel();
	int      prev_start = 0;
	int      last_sec = -1;
	int      inited = 0;

	/* Pace the compositor to the panel's native refresh. edid_refresh_hz()
	 * reads it over GMBUS on real Intel hardware and returns 0 under QEMU /
	 * non-Intel / eDP, where we fall back to 60. Clamp to one tick so a high
	 * refresh can't spin the loop faster than the timer can wake it. */
	int      hz = edid_refresh_hz();
	int      frame_ms = 1000 / (hz ? hz : 60);
	if (frame_ms < 1000 / TIMER_HZ)
		frame_ms = 1000 / TIMER_HZ;

	for (int i = 0; i < MAXWIN; i++)
		prev_dirty[i] = 0xFFFFFFFF;

	for (;;) {
		/* Reap any GUI apps we launched from the Start menu that have exited. */
		int rst;
		while (sched_try_reap(-1, &rst))
			;
		crash_pump();              /* surface any ring-3 crash as a window      */

		int mx = mouse_x(), my = mouse_y(), btn = mouse_buttons();
		int wheel = mouse_wheel();
		int dz = wheel - prev_wheel;
		prev_wheel = wheel;
		handle_input(mx, my, btn, dz);

		ndmg = 0;
		int full = !inited;       /* only the very first frame paints all      */

		/* Sample every window's dirty() value ONCE, before compose(), and reuse
		 * it for both change-detection and next frame's baseline. dirty() is the
		 * app's present_seq, bumped from the app thread / the win_present syscall
		 * concurrently with us. Re-reading it after compose() (as the snapshot
		 * below once did) loses any present that lands between compose and the
		 * snapshot: the WM records that seq as "seen" but never composited its
		 * pixels, so the window stays stuck on a stale frame until an unrelated
		 * sub-rect damage (cursor, clock) paints a sliver of the real content —
		 * a persistent partial paint. Sampling here makes the baseline reflect
		 * exactly what was composed, so a later present always differs next frame
		 * and forces a full-rect redraw. */
		uint32_t cur_dirty[MAXWIN];
		for (int i = 0; i < nwin; i++)
			cur_dirty[i] = wins[i].dirty ? wins[i].dirty(&wins[i]) : 0;

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
					damage_add(prev_x[i], prev_y[i], prev_w[i], prev_h[i]);
					damage_add(wins[i].x, wins[i].y, wins[i].w, wins[i].h);
				}

			/* Windows that moved or resized: damage old and new rectangles. */
			for (int i = 0; i < nwin; i++)
				if (wins[i].x != prev_x[i] || wins[i].y != prev_y[i] ||
				    wins[i].w != prev_w[i] || wins[i].h != prev_h[i]) {
					damage_add(prev_x[i], prev_y[i], prev_w[i], prev_h[i]);
					damage_add(wins[i].x, wins[i].y, wins[i].w, wins[i].h);
				}
			/* The cursor moved, changed shape, or is animating (the spinner):
			 * damage where it was and where it is. */
			int cur_kind = pick_cursor(mx, my);
			if (mx != prev_mx || my != prev_my || cur_kind != prev_cur ||
			    cur_kind == WM_CURSOR_BUSY) {
				damage_add(prev_mx - CUR_DMG_PAD, prev_my - CUR_DMG_PAD,
				           CUR_DMG_W, CUR_DMG_H);
				damage_add(mx - CUR_DMG_PAD, my - CUR_DMG_PAD,
				           CUR_DMG_W, CUR_DMG_H);
			}
			prev_cur = cur_kind;
			/* Any window whose contents changed (per its dirty() callback): a
			 * terminal that printed, a clock that ticked, a list that scrolled. */
			for (int i = 0; i < nwin; i++)
				if (wins[i].visible && wins[i].dirty &&
				    cur_dirty[i] != prev_dirty[i])
					damage_add(wins[i].x, wins[i].y, wins[i].w, wins[i].h);
			/* The taskbar clock ticked. */
			if ((int)timer_seconds() != last_sec)
				damage_add(0, screen_h - WM_BAR_H, screen_w, WM_BAR_H);
			/* The Start menu opened or closed: repaint the popup + taskbar. */
			if (start_open != prev_start)
				damage_add(0, sm_top(), SM_W, sm_h() + WM_BAR_H);
		}

		if (full) {
			fb_clip_full();
			compose(mx, my);
			fb_present();
		} else if (ndmg > 0) {
			/* Coalesce every damaged rectangle into one bounding box, then
			 * compose the scene and present once. The old loop walked the whole
			 * window stack and drained the WC buffer per rect — and a drag
			 * dirties 3-4 rects a frame (old + new window rect + cursor), so
			 * that was 3-4x the work for one logical update. */
			int x0 = dmg[0].x, y0 = dmg[0].y;
			int x1 = dmg[0].x + dmg[0].w, y1 = dmg[0].y + dmg[0].h;
			for (int k = 1; k < ndmg; k++) {
				if (dmg[k].x < x0)             x0 = dmg[k].x;
				if (dmg[k].y < y0)             y0 = dmg[k].y;
				if (dmg[k].x + dmg[k].w > x1)  x1 = dmg[k].x + dmg[k].w;
				if (dmg[k].y + dmg[k].h > y1)  y1 = dmg[k].y + dmg[k].h;
			}
			fb_set_clip(x0, y0, x1 - x0, y1 - y0);
			compose(mx, my);
			fb_present();
		}

		/* Snapshot for next frame's diff. */
		for (int i = 0; i < nwin; i++) {
			prev_x[i] = wins[i].x;   prev_y[i] = wins[i].y;
			prev_w[i] = wins[i].w;   prev_h[i] = wins[i].h;
			prev_vis[i] = wins[i].visible; prev_z[i] = zlist[i];
			prev_dirty[i] = cur_dirty[i];   /* sampled before compose(), above */
		}
		prev_mx = mx; prev_my = my;
		prev_start = start_open;
		last_sec = (int)timer_seconds();
		inited = 1;
		cursor_frame++;            /* advance the spinner's animation phase     */

		timer_sleep(frame_ms);     /* pace to panel refresh; idle when static  */
	}
}

void wm_init(void)
{
	if (!fb_active())
		return;

	screen_w = (int)fb_width();
	screen_h = (int)fb_height();

	/* Apps register their windows here; the terminal is created last so it
	 * starts focused / on top (see apps.c). */
	mouse_init(screen_w - 1, screen_h - 1);
	apps_start();

	/* Route the console into the terminal window, then start compositing. */
	vga_use_framebuffer(1);
	sched_create_thread("wm", wm_thread);
}
