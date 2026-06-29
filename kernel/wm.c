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
#include <stddef.h>
#include "wm.h"
#include "apps.h"
#include "heap.h"
#include "fb.h"
#include "console.h"
#include "mouse.h"
#include "keyboard.h"
#include "timer.h"
#include "irqflags.h"
#include "rtc.h"
#include "sched.h"
#include "edid.h"
#include "crash.h"
#include "uwin.h"
#include "battery.h"
#include "sb16.h"
#include "hda.h"
#include "power.h"
#include "nic.h"
#include "net.h"

/* The window count is UNCAPPED. wins[]/zlist[] and the per-frame snapshot grow
 * on the kernel heap as windows open, so the only ceiling is memory (each window
 * is kmalloc'd, with its own client buffer) — there is no fixed compositor slot
 * table. WM_LIST_MAX only bounds how many windows the taskbar / Alt-Tab overlay
 * lay out at once (a display detail, not an open limit). */
#define WM_LIST_MAX 64
static struct window **wins;   /* grown array of kmalloc'd windows (stable addrs) */
static int            *zlist;  /* z-order: indices into wins[], bottom..top       */
static int             wins_cap;
static int             nwin;

/* Per-frame compositor diff snapshot, one entry parallel to wins[], grown with
 * it. (Was MAXWIN-sized arrays on the wm_thread stack.) */
struct pfsnap { int x, y, w, h, vis, z, nr; uint32_t dirty, cur; };
static struct pfsnap *pf;

/* Grow wins[]/zlist[]/pf[] to hold at least `need` slots (~2x). Returns 0 if the
 * heap is exhausted — wm_create_window then fails gracefully (NULL). Runs under
 * the int 0x80 gate (IF=0) on the create path, atomic vs the WM thread that reads
 * these; cli guards the rare WM-thread caller (crash_pump). */
static int wm_reserve(int need)
{
	if (need <= wins_cap)
		return 1;
	int nc = wins_cap ? wins_cap * 2 : 16;
	while (nc < need)
		nc *= 2;
	struct window **nw = kmalloc((size_t)nc * sizeof *nw);
	int           *nz = kmalloc((size_t)nc * sizeof *nz);
	struct pfsnap *np = kmalloc((size_t)nc * sizeof *np);
	if (!nw || !nz || !np) { if (nw) kfree(nw); if (nz) kfree(nz); if (np) kfree(np); return 0; }
	for (int i = 0; i < wins_cap; i++) { nw[i] = wins[i]; nz[i] = zlist[i]; np[i] = pf[i]; }
	for (int i = wins_cap; i < nc; i++) {
		nw[i] = 0; nz[i] = 0;
		np[i].x = np[i].y = np[i].w = np[i].h = np[i].vis = np[i].z = np[i].nr = 0;
		np[i].dirty = 0xFFFFFFFF; np[i].cur = 0;
	}
	uintptr_t fl = irq_save();
	struct window **ow = wins; int *oz = zlist; struct pfsnap *op = pf;
	wins = nw; zlist = nz; pf = np; wins_cap = nc;
	if (fl & 0x200) __asm__ volatile ("sti");
	if (ow) kfree(ow);
	if (oz) kfree(oz);
	if (op) kfree(op);
	return 1;
}

static int screen_w, screen_h;

int wm_screen_w(void) { return screen_w; }
int wm_screen_h(void) { return screen_h; }

/* Taskbar Start button + launcher popup. */
#define START_W   70         /* width of the "OwOS" Start button              */
#define SM_ITEM_H 20         /* start-menu item height                        */
#define SM_W      150        /* start-menu popup width                        */
#define SM_SEP_H  9          /* separator strip above the power actions       */
static int start_open;

/* Taskbar window-button context menu: right-clicking a button opens a small
 * popup just above it with "Close" (ask the app to close) and "Force Kill"
 * (terminate it outright — for a window whose app has hung). tb_menu_win is the
 * targeted wins[] index; the popup is anchored at (tb_menu_x, tb_menu_y). */
#define TBM_W      120       /* context-menu width                            */
#define TBM_ITEM_H 22        /* context-menu item height                      */
#define TBM_N      2         /* number of items (Close, Force Kill)           */
static int tb_menu_open;
static int tb_menu_win = -1;
static int tb_menu_x, tb_menu_y;
static int tbm_h(void) { return TBM_N * TBM_ITEM_H; }

/* Power actions live at the bottom of the Start menu, below the app list and a
 * separator. They aren't /bin programs (apps.c) — they call straight into
 * power.c. "Sleep" (ACPI S3) is only listed when the firmware actually supports
 * it (power_can_sleep, cached at wm_init): VMware and other hosts that don't
 * expose S3 would otherwise show a dead button. */
enum { PA_SLEEP, PA_RESTART, PA_SHUTDOWN };
static int sleep_ok;                 /* ACPI S3 available? (probed once)        */

static int sm_power_n(void) { return sleep_ok ? 3 : 2; }
/* Map a power-section row to a PA_* action: with no Sleep, row 0 is Restart. */
static int sm_power_action(int row) { return sleep_ok ? row : row + 1; }
static const char *sm_power_label(int row)
{
	switch (sm_power_action(row)) {
	case PA_SLEEP:   return "Sleep";
	case PA_RESTART: return "Restart";
	default:         return "Shut Down";
	}
}

static int sm_apps_h(void) { return apps_menu_count() * SM_ITEM_H; }
static int sm_h(void)  { return sm_apps_h() + SM_SEP_H + sm_power_n() * SM_ITEM_H; }
static int sm_top(void){ return screen_h - WM_BAR_H - sm_h(); }

/* Set after an S3 resume so the next compositor frame repaints the whole screen
 * (the panel may have been powered down and redrawn from scratch). */
static int force_full;

/* Title-bar buttons, laid out right-to-left: slot 0 = close, 1 = maximize,
 * 2 = minimize. Each is a TB_BTN_W square; the close box keeps its original
 * w->w-18 position so slot N sits one button + gap further left. */
#define TB_BTN_W   14
#define TB_BTN_GAP 3

/* Taskbar window-button band, between the Start button and the right-side
 * clock/battery/volume indicators. draw_taskbar publishes the indicators' left
 * edge here each frame so the input hit-test (which runs before compose) can
 * lay the buttons out the same way. */
#define TB_WIN_GAP 4
static int tb_band_right;
static int tb_band_left(void) { return START_W + 6; }

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

/* The title shown in chrome (title bar + taskbar button): the app's own title,
 * with " (Not Responding)" appended while the WM judges the window hung. The
 * app's w->title is never mutated; callers pass a buffer of WM_TITLE_DISP. */
#define WM_TITLE_DISP (WM_TITLE_MAX + 18)   /* room for the suffix + NUL */
static void disp_title(struct window *w, char *buf)
{
	int n = 0;
	for (; w->title[n] && n < WM_TITLE_MAX - 1; n++)
		buf[n] = w->title[n];
	buf[n] = '\0';
	if (uwin_not_responding(w)) {
		static const char sfx[] = " (Not Responding)";
		for (int j = 0; sfx[j]; j++)
			buf[n++] = sfx[j];
		buf[n] = '\0';
	}
}

/* ----------------------------------------------------------- z-order helpers */

/* Topmost *visible* window — the one that holds focus (keyboard + highlight). */
static int topmost_visible(void)
{
	for (int i = nwin - 1; i >= 0; i--)
		if (wins[zlist[i]]->visible)
			return zlist[i];
	return -1;
}

/* Alt+Tab switcher session. While the user holds Alt and taps Tab, the WM shows
 * a centered overlay of switchable windows in most-recently-used order and moves
 * a highlight through them; releasing Alt raises the highlighted one. The z-order
 * is left untouched until commit, so repeated taps cycle every window instead of
 * just toggling the top two. */
static int at_active;             /* switcher overlay is showing               */
static int at_order[WM_LIST_MAX];      /* switch targets, most-recent first          */
static int at_count;
static int at_sel;                /* highlighted row in at_order                */

/* Snapshot the switchable windows (live, and either visible or minimized) into
 * at_order, topmost first so index 0 is the current window and 1 the previous.
 * Returns the count. */
static int alttab_build(void)
{
	int n = 0;
	for (int i = nwin - 1; i >= 0 && n < WM_LIST_MAX; i--) {  /* bottom..top -> reverse */
		struct window *w = wins[zlist[i]];
		if (!w->dead && (w->visible || w->minimized))
			at_order[n++] = zlist[i];
	}
	return n;
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
		struct window *w = wins[zlist[i]];
		if (!w->visible)
			continue;
		if (px >= w->x && px < w->x + w->w &&
		    py >= w->y && py < w->y + w->h)
			return zlist[i];
	}
	return -1;
}

/* --------------------------------------------------- minimize / maximize */

/* Live-resize hand-off state (shared by the grip drag in handle_input and the
 * programmatic maximize/restore below). The frame size trails a target; the app
 * is re-handed it only once it has acked the previous size by presenting, so
 * each buffer realloc/remap in on_resize lands while the app is parked in
 * win_wait, never mid-paint. The tracker outlives the gesture until the app's
 * content catches up to the final size. */
static int      rsz_win = -1;
static int      rsz_edge;       /* edges being dragged (RZ_* bits); 0 = the grip */
static int      rsz_target_w, rsz_target_h;
static int      rsz_sent_w, rsz_sent_h;
static uint32_t rsz_ack;

/* Move/resize a window's frame now, and (if it has an on_resize) start the
 * settle hand-off so its client buffer follows. Used by maximize and restore. */
static void resize_to(int idx, int nx, int ny, int nw, int nh)
{
	struct window *w = wins[idx];
	w->x = nx; w->y = ny; w->w = nw; w->h = nh;
	if (w->on_resize) {
		rsz_win = idx;
		rsz_target_w = nw; rsz_target_h = nh;
		rsz_sent_w = rsz_sent_h = -1;   /* force the first hand-off */
		rsz_ack = 0;
	}
}

static void wm_minimize(int idx)
{
	wins[idx]->visible = 0;
	wins[idx]->minimized = 1;
}

/* Bring a minimized (or merely lowered) window back up and focus it. */
static void wm_unminimize(int idx)
{
	wins[idx]->minimized = 0;
	wins[idx]->visible = 1;
	raise_window(idx);
}

static void wm_toggle_maximize(int idx)
{
	struct window *w = wins[idx];
	if (w->maximized) {
		w->maximized = 0;
		resize_to(idx, w->saved_x, w->saved_y, w->saved_w, w->saved_h);
	} else {
		w->saved_x = w->x; w->saved_y = w->y;
		w->saved_w = w->w; w->saved_h = w->h;
		w->maximized = 1;
		/* Fill the desktop work area, NOT the whole screen: the title bar and
		 * the taskbar both stay visible (maximize, not fullscreen). */
		resize_to(idx, 0, 0, screen_w, screen_h - WM_BAR_H);
	}
}

/* Terminate the app behind a window without its cooperation: kill the owning
 * process (its exit tears the window down via uwin_task_exiting -> uwin_free,
 * and the WM frees the buffer in the next uwin_reap), or, for a kernel-side
 * window with no process, fall back to its close action / hide. */
static void wm_force_kill(int idx)
{
	struct window *w = wins[idx];
	uint32_t pid = uwin_owner_pid(w);
	if (pid)
		sched_kill((int)pid);
	else if (w->on_close)
		w->on_close(w);
	else
		w->visible = 0;
}

/* Title-bar buttons by kind: 0=close, 1=maximize, 2=minimize. Close follows
 * w->closable; maximize follows w->resizable; minimize is always offered. */
static int btn_present(struct window *w, int kind)
{
	if (kind == 0) return w->closable;
	if (kind == 1) return w->resizable;
	return 1;                       /* minimize */
}

/* Packed slot index from the right edge (0 = rightmost), or -1 if this button
 * isn't shown. The kinds are ordered close > maximize > minimize from the right,
 * and absent ones are skipped so the rest close up — no gap when, e.g., a
 * non-resizable window omits maximize. */
static int btn_slot(struct window *w, int kind)
{
	if (!btn_present(w, kind))
		return -1;
	int slot = 0;
	if (kind > 0 && btn_present(w, 0)) slot++;   /* close sits right of these */
	if (kind > 1 && btn_present(w, 1)) slot++;   /* maximize right of minimize */
	return slot;
}

/* Fill *bx,*by with button `kind`'s top-left; return 1 if it's shown, else 0. */
static int titlebtn_box(struct window *w, int kind, int *bx, int *by)
{
	int slot = btn_slot(w, kind);
	if (slot < 0)
		return 0;
	*bx = w->x + w->w - 4 - (slot + 1) * TB_BTN_W - slot * TB_BTN_GAP;
	*by = w->y + 4;
	return 1;
}

/* Which title-bar button is under (mx,my): kind 0/1/2, or -1 for none. */
static int titlebtn_hit(struct window *w, int mx, int my)
{
	for (int k = 0; k < 3; k++) {
		int bx, by;
		if (!titlebtn_box(w, k, &bx, &by))
			continue;
		if (mx >= bx && mx < bx + TB_BTN_W && my >= by && my < by + TB_BTN_W)
			return k;
	}
	return -1;
}

/* Lay the taskbar window buttons across the band [left,right): one per live
 * (non-dead) window. Fills idxs/bxs/bws and returns the count. Both the painter
 * and the hit-test call this so they always agree. */
static int taskbar_layout(int left, int right, int *idxs, int *bxs, int *bws,
                          int max)
{
	int n = 0;
	for (int i = 0; i < nwin && n < max; i++)
		if (!wins[i]->dead)
			idxs[n++] = i;
	if (n == 0)
		return 0;
	int avail = right - left;
	if (avail < n)            /* nothing sensible fits */
		return 0;
	int bw = avail / n;
	if (bw > 168)
		bw = 168;
	for (int k = 0; k < n; k++) {
		bxs[k] = left + k * bw;
		bws[k] = bw - TB_WIN_GAP;
		if (bws[k] < 1)
			bws[k] = 1;
	}
	return n;
}

/* The live window whose taskbar button is under (mx,my), or -1. */
static int taskbar_hit(int mx, int my)
{
	if (my < screen_h - WM_BAR_H)
		return -1;
	int idxs[WM_LIST_MAX], bxs[WM_LIST_MAX], bws[WM_LIST_MAX];
	int n = taskbar_layout(tb_band_left(), tb_band_right, idxs, bxs, bws, WM_LIST_MAX);
	for (int k = 0; k < n; k++)
		if (mx >= bxs[k] && mx < bxs[k] + bws[k])
			return idxs[k];
	return -1;
}

/* --------------------------------------------------------------- rendering */

static void draw_background(void)
{
	/* A smooth vertical gradient: deep indigo at top to slate at the bottom.
	 * The gradient is purely a function of y, so confine the scan to the clip
	 * band's rows — at 1080p an unclipped full-height repaint ran every frame a
	 * tiny rect (cursor, clock) was damaged, scaling the idle cost with the
	 * panel height for no visible benefit. */
	int bx, by, bw, bh;
	fb_get_clip(&bx, &by, &bw, &bh);
	for (int y = by; y < by + bh; y++) {
		int t = y * 255 / screen_h;
		uint8_t r = (uint8_t)(24 + t * 40 / 255);
		uint8_t g = (uint8_t)(26 + t * 56 / 255);
		uint8_t b = (uint8_t)(54 + t * 70 / 255);
		fb_fill_rect(bx, y, bw, 1, fb_rgb(r, g, b));
	}
}

static void ui_text_clip(int x, int y, const char *s, uint32_t col, int maxw);

static void draw_window(struct window *w, int focused)
{
	uint32_t title_bg = focused ? 0x2f6fb0 : 0x55606e;
	uint32_t frame    = 0x10141a;

	/* Outer frame and title bar. */
	fb_fill_rect(w->x, w->y, w->w, w->h, frame);
	fb_fill_rect(w->x, w->y, w->w, WM_TITLE_H, title_bg);

	/* Clip the title to stop before the leftmost title-bar button so a long
	 * page title gets "text.." instead of bleeding under the buttons. */
	int title_x = w->x + 8;
	int title_right = w->x + w->w - 4;
	for (int k = 0; k < 3; k++) {
		int bbx, bby;
		if (titlebtn_box(w, k, &bbx, &bby) && bbx < title_right)
			title_right = bbx;
	}
	char dt[WM_TITLE_DISP];
	disp_title(w, dt);
	ui_text_clip(title_x, w->y + (WM_TITLE_H - UIFONT_H) / 2, dt,
	             0xffffff, title_right - title_x - 6);

	/* Title-bar buttons, right to left: close, maximize/restore, minimize. */
	int bx, by;
	if (titlebtn_box(w, 2, &bx, &by)) {            /* minimize: a low bar  */
		fb_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0x3a4658);
		fb_fill_rect(bx + 3, by + 9, 8, 2, 0xffffff);
	}
	if (titlebtn_box(w, 1, &bx, &by)) {            /* maximize / restore   */
		fb_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0x3a4658);
		if (w->maximized) {                        /* restore: two squares */
			fb_rect(bx + 4, by + 2, 6, 6, 0xffffff);
			fb_fill_rect(bx + 2, by + 5, 6, 6, 0x3a4658);
			fb_rect(bx + 2, by + 5, 6, 6, 0xffffff);
		} else {                                   /* maximize: one square */
			fb_rect(bx + 3, by + 3, 8, 8, 0xffffff);
		}
	}
	if (titlebtn_box(w, 0, &bx, &by)) {            /* close: an X glyph    */
		fb_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0xc04030);
		for (int d = 0; d < 7; d++) {              /* two 1px diagonals     */
			fb_pixel(bx + 4 + d, by + 4 + d, 0xffffff);
			fb_pixel(bx + 10 - d, by + 4 + d, 0xffffff);
		}
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
static void draw_resize_nwse(int mx, int my)
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

/* ↗↙ diagonal double-arrow (the other corner). */
static void draw_resize_nesw(int mx, int my)
{
	for (int halo = 1; halo >= 0; halo--) {
		for (int t = -5; t <= 5; t++)
			cur_stroke(mx + t, my - t, halo);          /* anti-diagonal shaft */
		for (int k = 0; k <= 4; k++) {
			cur_stroke(mx + 5 - k, my - 5,     halo);  /* NE head            */
			cur_stroke(mx + 5,     my - 5 + k, halo);
			cur_stroke(mx - 5 + k, my + 5,     halo);  /* SW head            */
			cur_stroke(mx - 5,     my + 5 - k, halo);
		}
	}
}

/* ↕ vertical double-arrow (top/bottom edge). */
static void draw_resize_ns(int mx, int my)
{
	for (int halo = 1; halo >= 0; halo--) {
		for (int t = -6; t <= 6; t++) cur_stroke(mx, my + t, halo);   /* shaft */
		for (int k = 0; k <= 4; k++) {
			cur_stroke(mx - k, my - 6 + k, halo);      /* top head    */
			cur_stroke(mx + k, my - 6 + k, halo);
			cur_stroke(mx - k, my + 6 - k, halo);      /* bottom head */
			cur_stroke(mx + k, my + 6 - k, halo);
		}
	}
}

/* ↔ horizontal double-arrow (left/right edge). */
static void draw_resize_ew(int mx, int my)
{
	for (int halo = 1; halo >= 0; halo--) {
		for (int t = -6; t <= 6; t++) cur_stroke(mx + t, my, halo);   /* shaft */
		for (int k = 0; k <= 4; k++) {
			cur_stroke(mx - 6 + k, my - k, halo);      /* left head  */
			cur_stroke(mx - 6 + k, my + k, halo);
			cur_stroke(mx + 6 - k, my - k, halo);      /* right head */
			cur_stroke(mx + 6 - k, my + k, halo);
		}
	}
}

static void draw_cursor(int kind, int mx, int my)
{
	switch (kind) {
	case WM_CURSOR_BUSY:        draw_busy(mx, my);   return;
	case WM_CURSOR_RESIZE_NWSE: draw_resize_nwse(mx, my); return;
	case WM_CURSOR_RESIZE_NESW: draw_resize_nesw(mx, my); return;
	case WM_CURSOR_RESIZE_NS:   draw_resize_ns(mx, my); return;
	case WM_CURSOR_RESIZE_EW:   draw_resize_ew(mx, my); return;
	default:
		if (kind < 0 || kind >= (int)(sizeof cur_shapes / sizeof cur_shapes[0]) ||
		    !cur_shapes[kind].rows)
			kind = WM_CURSOR_ARROW;
		const struct cur_shape *s = &cur_shapes[kind];
		draw_glyph(s, mx - s->hotx, my - s->hoty);
		return;
	}
}

/* Battery indicator, drawn just left of the clock. `clock_x` is the clock
 * text's left edge; we lay the icon + "NN%" out ending a little before it.
 * The reading is cached and only refreshed every ~30 s, since evaluating the
 * ACPI _BST AML on every taskbar repaint (once a second) is wasteful. We only
 * draw when a real percentage (0..100) comes back: some hypervisors (VMware)
 * advertise a phantom battery whose _BST can't be evaluated, and that simply
 * fails the read here and shows nothing rather than a bogus gauge. */
static int draw_battery(int clock_x, int y)
{
	static int last_poll = -1000;
	static int cached_pct = -1;
	static int cached_charging;

	int now = (int)timer_seconds();
	if (now - last_poll >= 30 || last_poll == -1000) {
		last_poll = now;
		if (battery_present())
			cached_pct = battery_percent(&cached_charging, 0, 0);
		else
			cached_pct = -1;
	}
	if (cached_pct < 0 || cached_pct > 100)
		return clock_x;   /* no real battery, or the reading couldn't be taken */

	char s[5];   /* "100%" + NUL */
	int n = 0;
	if (cached_pct >= 100) { s[n++] = '1'; s[n++] = '0'; s[n++] = '0'; }
	else if (cached_pct >= 10) { s[n++] = '0' + cached_pct / 10; s[n++] = '0' + cached_pct % 10; }
	else s[n++] = '0' + cached_pct;
	s[n++] = '%';
	s[n] = 0;

	const uint32_t bg = 0x0c0e14;
	int icon_w = 22, icon_h = 11, nub_w = 2;
	int txt_w = fb_ui_width(s);
	int total = icon_w + nub_w + 6 + txt_w;
	int x = clock_x - 12 - total;
	int iy = y + (WM_BAR_H - icon_h) / 2;

	/* fill colour: red when low, amber while charging, green otherwise */
	uint32_t fill = cached_pct <= 15 ? 0xd05040
	              : cached_charging ? 0xe0b040 : 0x60c060;

	fb_rect(x, iy, icon_w, icon_h, 0xc8d0dc);                 /* outline   */
	fb_fill_rect(x + icon_w, iy + 3, nub_w, icon_h - 6, 0xc8d0dc); /* nub  */
	int inner_w = icon_w - 4;
	int lvl = inner_w * cached_pct / 100;
	if (lvl < 1 && cached_pct > 0) lvl = 1;
	fb_fill_rect(x + 2, iy + 2, inner_w, icon_h - 4, bg);     /* clear gauge */
	if (lvl > 0)
		fb_fill_rect(x + 2, iy + 2, lvl, icon_h - 4, fill);

	fb_ui_text(x + icon_w + nub_w + 6, y + (WM_BAR_H - UIFONT_H) / 2, s, 0xc8d0dc);
	return x;
}

/* Volume indicator: a speaker glyph + "<device> NN%", drawn just left of the
 * battery (or the clock when there's no battery). `anchor_x` is that neighbour's
 * left edge. Shows whichever card owns output — HDA if present, else SB16 — so
 * the active device and level are both visible. Reflects a key press on the next
 * taskbar repaint (≤ 1 s). Draws nothing when there's no sound card. */
static int draw_volume(int anchor_x, int y)
{
	int vol;
	const char *dev;
	if (hda_present())       { vol = hda_volume();  dev = "HDA";  }
	else if (sb16_present()) { vol = sb16_volume(); dev = "SB16"; }
	else                     return anchor_x;        /* no audio hardware */

	int pct = vol * 100 / 8;                          /* 0..8 scale -> percent */
	char s[12];                                       /* "SB16 100%" + NUL     */
	int n = 0;
	for (const char *d = dev; *d; d++) s[n++] = *d;
	s[n++] = ' ';
	if (pct >= 100)     { s[n++] = '1'; s[n++] = '0'; s[n++] = '0'; }
	else if (pct >= 10) { s[n++] = '0' + pct / 10; s[n++] = '0' + pct % 10; }
	else                  s[n++] = '0' + pct;
	s[n++] = '%';
	s[n] = 0;

	int icon_w = 14, icon_h = 11;
	int txt_w = fb_ui_width(s);
	int total = icon_w + 6 + txt_w;
	int x = anchor_x - 12 - total;
	int iy = y + (WM_BAR_H - icon_h) / 2;

	uint32_t col = (vol == 0) ? 0x808890 : 0xc8d0dc;  /* dim when muted / zero */

	/* Speaker: a small square body, then a staircase "cone" that grows toward
	 * the listener (no line/triangle primitive, so approximate with rects). */
	int body = 5, by = iy + (icon_h - body) / 2;
	fb_fill_rect(x, by, body, body, col);
	for (int i = 0; i < 4; i++) {
		int ch = 3 + i * 2;
		fb_fill_rect(x + body + i, iy + (icon_h - ch) / 2, 1, ch, col);
	}
	/* Sound waves: one or two ticks to the right, scaled to how loud it is. */
	if (vol > 0) {
		int wx = x + body + 5;
		fb_fill_rect(wx, iy + 3, 1, icon_h - 6, col);
		if (vol >= 4) fb_fill_rect(wx + 2, iy + 1, 1, icon_h - 2, col);
	}

	fb_ui_text(x + icon_w + 6, y + (WM_BAR_H - UIFONT_H) / 2, s, col);
	return x;
}

/* Network indicator: a four-bar signal glyph + the IPv4 address, drawn just left
 * of the volume indicator. `anchor_x` is that neighbour's left edge. Bars + text
 * are green and show the leased IP when the link is up; dimmed to grey with
 * "no link" when a NIC is present but the cable/carrier is down. Draws nothing
 * when there's no NIC at all (matching the battery/volume "absent hardware shows
 * nothing" rule). The state comes from the active driver via nic_present/
 * nic_link_up, so it tracks whichever card (igb/e1000/rtl8139) the kernel kept. */
static int draw_network(int anchor_x, int y)
{
	if (!nic_present())
		return anchor_x;

	/* "link up" is not "has an address": a virtual NIC reports the carrier up
	 * instantly, but until DHCP leases an address the machine holds no IP. Show
	 * the address only once net_have_ip() is true; while the link is up but
	 * unleased, say so rather than displaying a placeholder we don't really own. */
	int up  = nic_link_up();
	int got = net_have_ip();
	char s[20];                          /* "255.255.255.255" / "no link" + NUL */
	int n = 0;
	if (got) {
		const uint8_t *ip = net_my_ip();
		for (int i = 0; i < 4; i++) {
			int v = ip[i];
			if (v >= 100) s[n++] = '0' + v / 100;
			if (v >= 10)  s[n++] = '0' + (v / 10) % 10;
			s[n++] = '0' + v % 10;
			if (i < 3) s[n++] = '.';
		}
	} else {
		for (const char *d = up ? "no ip" : "no link"; *d; d++) s[n++] = *d;
	}
	s[n] = 0;

	uint32_t col = got ? 0x60c060 : 0x808890;

	int icon_w = 16, icon_h = 11;
	int txt_w = fb_ui_width(s);
	int total = icon_w + 6 + txt_w;
	int x = anchor_x - 12 - total;
	int iy = y + (WM_BAR_H - icon_h) / 2;

	/* Four rising bars (signal-strength glyph). When the link is down, only the
	 * shortest bar is lit so it still reads as "network, but not connected". */
	for (int i = 0; i < 4; i++) {
		int bh = 3 + i * 2;
		uint32_t bc = (up || i == 0) ? col : 0x3a4150;
		fb_fill_rect(x + i * 4, iy + (icon_h - bh), 3, bh, bc);
	}

	fb_ui_text(x + icon_w + 6, y + (WM_BAR_H - UIFONT_H) / 2, s, col);
	return x;
}

/* Draw UI text at (x,y) in colour `col`, truncated with ".." to fit `maxw`
 * pixels — the taskbar buttons are narrow and titles are arbitrary. */
static void ui_text_clip(int x, int y, const char *s, uint32_t col, int maxw)
{
	char buf[WM_TITLE_MAX + 2];
	int n = 0;
	for (; s[n] && n < WM_TITLE_MAX; n++)
		buf[n] = s[n];
	buf[n] = '\0';
	if (maxw <= 0)
		return;
	if (fb_ui_width(buf) <= maxw) {
		fb_ui_text(x, y, buf, col);
		return;
	}
	while (n > 0) {                       /* shrink until "text.." fits */
		buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '\0';
		if (fb_ui_width(buf) <= maxw)
			break;
		buf[n] = '\0';
		n--;
	}
	if (n > 0)
		fb_ui_text(x, y, buf, col);
}

/* A button per live window, between the Start button and the right indicators.
 * The focused window's button is highlighted; a minimized one is dimmed. */
static void draw_taskbar_windows(int y, int uy, int right)
{
	tb_band_right = right;
	int idxs[WM_LIST_MAX], bxs[WM_LIST_MAX], bws[WM_LIST_MAX];
	int n = taskbar_layout(tb_band_left(), right, idxs, bxs, bws, WM_LIST_MAX);
	int focus = topmost_visible();
	int by = y + 3, bh = WM_BAR_H - 6;
	for (int k = 0; k < n; k++) {
		struct window *w = wins[idxs[k]];
		int active = (idxs[k] == focus);
		uint32_t bg = active ? 0x2f6fb0 : w->minimized ? 0x12161e : 0x1d2533;
		fb_fill_rect(bxs[k], by, bws[k], bh, bg);
		fb_fill_rect(bxs[k], by, bws[k], 1, active ? 0x4f8fd0 : 0x2a3550);
		uint32_t fg = w->minimized ? 0x8b94a4 : 0xc8d0dc;
		char dt[WM_TITLE_DISP];
		disp_title(w, dt);
		ui_text_clip(bxs[k] + 8, uy, dt, fg, bws[k] - 12);
	}
}

static void draw_taskbar(void)
{
	int y = screen_h - WM_BAR_H;
	fb_fill_rect(0, y, screen_w, WM_BAR_H, 0x0c0e14);
	fb_fill_rect(0, y, screen_w, 1, 0x2a3550);
	uint32_t btn = start_open ? 0x2f6fb0 : 0x0c0e14;   /* Start button */
	int uy = y + (WM_BAR_H - UIFONT_H) / 2;             /* UI text baseline row */
	fb_fill_rect(0, y + 1, START_W, WM_BAR_H - 1, btn);
	fb_ui_text(10, uy, "OwOS", 0x9ad0ff);

	struct rtc_time t;
	rtc_now(&t);
	char s[9] = "00:00:00";
	put2(s + 0, t.hour);
	put2(s + 3, t.min);
	put2(s + 6, t.sec);
	int clock_x = screen_w - fb_ui_width(s) - 12;
	fb_ui_text(clock_x, uy, s, 0xc8d0dc);

	/* Right-side indicators, each laid out leftward from the previous one's
	 * edge: clock | battery | volume | network. */
	int ind_left = clock_x;
	ind_left = draw_battery(ind_left, y);
	ind_left = draw_volume(ind_left, y);
	ind_left = draw_network(ind_left, y);
	draw_taskbar_windows(y, uy, ind_left - 10);
}

/* The Start menu popup, listing the launchable apps above the taskbar. */
static void draw_start_menu(void)
{
	int n = apps_menu_count();
	int top = sm_top();
	fb_fill_rect(0, top, SM_W, sm_h(), 0x141a24);
	for (int i = 0; i < n; i++) {
		int iy = top + i * SM_ITEM_H;
		fb_ui_text(10, iy + (SM_ITEM_H - UIFONT_H) / 2, apps_menu_name(i), 0xc8d0dc);
	}
	/* Separator, then the power actions below it. */
	int sep_y = top + sm_apps_h() + SM_SEP_H / 2;
	fb_fill_rect(8, sep_y, SM_W - 16, 1, 0x05070a);
	int py = top + sm_apps_h() + SM_SEP_H;
	for (int i = 0; i < sm_power_n(); i++) {
		int iy = py + i * SM_ITEM_H;
		fb_ui_text(10, iy + (SM_ITEM_H - UIFONT_H) / 2, sm_power_label(i), 0xa8b4c4);
	}
	fb_rect(0, top, SM_W, sm_h(), 0x05070a);
}

/* The taskbar window-button context menu (Close / Force Kill), anchored above
 * the right-clicked button. Force Kill is tinted warm-red to flag it as the
 * destructive action. */
static void draw_tb_menu(void)
{
	static const char *const items[TBM_N] = { "Close", "Force Kill" };
	int x = tb_menu_x, y = tb_menu_y;
	fb_fill_rect(x, y, TBM_W, tbm_h(), 0x141a24);
	for (int i = 0; i < TBM_N; i++) {
		int iy = y + i * TBM_ITEM_H;
		uint32_t fg = (i == 1) ? 0xe08070 : 0xc8d0dc;
		fb_ui_text(x + 12, iy + (TBM_ITEM_H - UIFONT_H) / 2, items[i], fg);
	}
	fb_rect(x, y, TBM_W, tbm_h(), 0x05070a);
}

/* Hints from handle_input for cursor selection: a grip drag is live, or a window
 * is still catching up to a finished resize (so we show the spinner briefly). */
static int hover_resizing;
static int hover_settling;

/* Edge/corner of a window frame the pointer is over (a resize grab zone). */
enum { RZ_L = 1, RZ_R = 2, RZ_T = 4, RZ_B = 8 };

/* Which frame edges (RZ_* bitmask) the pointer at (mx,my) is within WM_EDGE of,
 * for a resizable, non-maximized window. 0 = not on an edge. The pointer is
 * assumed already inside the window box (window_at matched it). */
static int resize_edge(struct window *w, int mx, int my)
{
	if (!w->resizable || w->maximized) return 0;
	int e = 0;
	if (mx < w->x + WM_EDGE)              e |= RZ_L;
	else if (mx >= w->x + w->w - WM_EDGE) e |= RZ_R;
	if (my < w->y + WM_EDGE)              e |= RZ_T;
	else if (my >= w->y + w->h - WM_EDGE) e |= RZ_B;
	return e;
}

/* The directional cursor for an edge mask. */
static int edge_cursor(int e)
{
	switch (e) {
	case RZ_L: case RZ_R:                 return WM_CURSOR_RESIZE_EW;
	case RZ_T: case RZ_B:                 return WM_CURSOR_RESIZE_NS;
	case RZ_L | RZ_T: case RZ_R | RZ_B:   return WM_CURSOR_RESIZE_NWSE;
	case RZ_R | RZ_T: case RZ_L | RZ_B:   return WM_CURSOR_RESIZE_NESW;
	}
	return WM_CURSOR_ARROW;
}

/* Decide which cursor the pointer should be wearing this frame. Resize and
 * "busy" hints from an in-progress interaction win; then clickable chrome (the
 * Start button, an open menu item, a close box) gets the hand; then the grip
 * gets the resize arrow; otherwise a window may dress its own client area. */
static int pick_cursor(int mx, int my)
{
	if (hover_resizing) return edge_cursor(rsz_edge ? rsz_edge : (RZ_R | RZ_B));
	if (hover_settling) return WM_CURSOR_BUSY;

	if (tb_menu_open && mx >= tb_menu_x && mx < tb_menu_x + TBM_W &&
	    my >= tb_menu_y && my < tb_menu_y + tbm_h())
		return WM_CURSOR_HAND;

	int bar_y = screen_h - WM_BAR_H;
	if (my >= bar_y) {
		if (mx >= 0 && mx < START_W)
			return WM_CURSOR_HAND;
		return taskbar_hit(mx, my) >= 0 ? WM_CURSOR_HAND : WM_CURSOR_ARROW;
	}
	if (start_open && mx >= 0 && mx < SM_W && my >= sm_top())
		return WM_CURSOR_HAND;

	int idx = window_at(mx, my);
	if (idx >= 0) {
		struct window *w = wins[idx];
		if (titlebtn_hit(w, mx, my) >= 0)
			return WM_CURSOR_HAND;
		if (w->resizable &&
		    mx >= w->x + w->w - WM_GRIP && mx < w->x + w->w &&
		    my >= w->y + w->h - WM_GRIP && my < w->y + w->h)
			return WM_CURSOR_RESIZE_NWSE;          /* the larger BR grip zone   */
		int e = resize_edge(w, mx, my);
		if (e) return edge_cursor(e);
		if (w->cursor > 0 &&
		    my >= w->y + WM_TITLE_H && my < w->y + w->h - WM_BORDER &&
		    mx >= w->x + WM_BORDER && mx < w->x + w->w - WM_BORDER)
			return w->cursor;
	}
	return WM_CURSOR_ARROW;
}

/* Alt+Tab switcher overlay: a centered panel listing the switchable windows,
 * one per row, with the current selection highlighted. Drawn above everything
 * but the cursor while a switch gesture is in progress. */
#define AT_ROW_H  24
#define AT_PAD    12
#define AT_PANEL_W 340

static void switcher_box(int *px, int *py, int *pw, int *ph)
{
	int h = AT_PAD * 2 + at_count * AT_ROW_H;
	int maxh = screen_h - WM_BAR_H - 20;
	if (h > maxh) h = maxh;                 /* clamp if a great many windows */
	*pw = AT_PANEL_W;
	*ph = h;
	*px = (screen_w - AT_PANEL_W) / 2;
	*py = (screen_h - h) / 2;
}

static void draw_switcher(void)
{
	int x, y, pw, ph;
	switcher_box(&x, &y, &pw, &ph);
	fb_fill_rect(x, y, pw, ph, 0x141a24);
	fb_rect(x, y, pw, ph, 0x05070a);
	fb_rect(x + 1, y + 1, pw - 2, ph - 2, 0x2a3550);

	int rows = (ph - 2 * AT_PAD) / AT_ROW_H;   /* how many fit after clamping */
	if (rows > at_count) rows = at_count;
	for (int i = 0; i < rows; i++) {
		int ry = y + AT_PAD + i * AT_ROW_H;
		if (i == at_sel)
			fb_fill_rect(x + 4, ry, pw - 8, AT_ROW_H, 0x2f6fb0);
		struct window *w = wins[at_order[i]];
		uint32_t fg = (i == at_sel) ? 0xffffff : 0xc8d0dc;
		ui_text_clip(x + 14, ry + (AT_ROW_H - UIFONT_H) / 2, w->title, fg,
		             pw - 28);
	}
}

static void compose(int mx, int my)
{
	draw_background();
	int top = topmost_visible();
	for (int i = 0; i < nwin; i++) {
		int idx = zlist[i];
		if (wins[idx]->visible)
			draw_window(wins[idx], idx == top);
	}
	draw_taskbar();
	if (start_open)
		draw_start_menu();
	if (tb_menu_open)
		draw_tb_menu();
	if (at_active)
		draw_switcher();
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
	static int  rsz_ox, rsz_oy, rsz_ow, rsz_oh;  /* frame box when the grab began */
	static int  rsz_mx0, rsz_my0;                /* cursor when the grab began    */
	/* The live-resize hand-off state (rsz_win/rsz_target/rsz_sent/rsz_ack) is at
	 * file scope so maximize/restore can drive it too; see resize_to(). */

	static int  prev_rdown;
	int down = buttons & 1;        /* left button                              */
	int rdown = buttons & 2;       /* right button                             */

	/* Wheel: route to whatever visible window is under the cursor. */
	if (dz) {
		int idx = window_at(mx, my);
		if (idx >= 0 && wins[idx]->scroll)
			wins[idx]->scroll(wins[idx], mx, my, dz);
	}

	/* Right-press: raise the window under the cursor and, if the press landed in
	 * its client area (not the chrome), hand it to the app's rclick — typically a
	 * context menu. Tracked on its own edge so it never disturbs a left drag. */
	if (rdown && !prev_rdown) {
		int bar_y = screen_h - WM_BAR_H;
		int tbw = (my >= bar_y) ? taskbar_hit(mx, my) : -1;
		if (tbw >= 0) {                 /* right-click a taskbar button: its menu */
			tb_menu_open = 1;
			tb_menu_win = tbw;
			start_open = 0;
			tb_menu_x = mx;
			if (tb_menu_x + TBM_W > screen_w) tb_menu_x = screen_w - TBM_W;
			if (tb_menu_x < 0) tb_menu_x = 0;
			tb_menu_y = bar_y - tbm_h();
		} else {
			tb_menu_open = 0;       /* a right-click elsewhere dismisses it */
			int idx = window_at(mx, my);
			if (idx >= 0) {
				raise_window(idx);
				struct window *w = wins[idx];
				if (w->rclick && my >= w->y + WM_TITLE_H)
					w->rclick(w, mx, my);
			}
		}
	}
	prev_rdown = rdown;

	if (down && !prev_down) {      /* press                                    */
		/* An open taskbar context menu intercepts the next click: act on the
		 * item under the cursor, otherwise just dismiss it (then fall through so
		 * the click still does whatever it landed on). */
		if (tb_menu_open) {
			int inside = mx >= tb_menu_x && mx < tb_menu_x + TBM_W &&
			             my >= tb_menu_y && my < tb_menu_y + tbm_h();
			if (inside) {
				int item = (my - tb_menu_y) / TBM_ITEM_H;
				int idx = tb_menu_win;
				tb_menu_open = 0;
				if (idx >= 0 && idx < nwin && !wins[idx]->dead) {
					struct window *w = wins[idx];
					if (item == 0) {           /* Close: ask the app */
						if (w->on_close) w->on_close(w);
						else w->visible = 0;
					} else {                   /* Force Kill */
						wm_force_kill(idx);
					}
				}
				prev_down = down;
				return;
			}
			tb_menu_open = 0;
		}
		/* Start button / launcher popup take priority over windows. */
		int bar_y = screen_h - WM_BAR_H;
		if (my >= bar_y && mx >= 0 && mx < START_W) {   /* the Start button   */
			start_open = !start_open;
			prev_down = down;
			return;
		}
		/* A taskbar window button: restore a minimized window, minimize the
		 * focused one, or raise any other. */
		int tbw = (my >= bar_y) ? taskbar_hit(mx, my) : -1;
		if (tbw >= 0) {
			start_open = 0;
			if (wins[tbw]->minimized)
				wm_unminimize(tbw);
			else if (tbw == topmost_visible())
				wm_minimize(tbw);
			else
				raise_window(tbw);
			prev_down = down;
			return;
		}
		if (start_open) {
			int top = sm_top();
			if (mx >= 0 && mx < SM_W && my >= top && my < bar_y) {
				int rel = my - top;
				if (rel < sm_apps_h()) {
					apps_menu_launch(rel / SM_ITEM_H);   /* launch + close */
					start_open = 0;
				} else {
					int prel = rel - sm_apps_h() - SM_SEP_H;
					/* The SM_SEP_H separator gap is dead space (prel < 0). */
					if (prel >= 0 && prel / SM_ITEM_H < sm_power_n()) {
						start_open = 0;
						switch (sm_power_action(prel / SM_ITEM_H)) {
						case PA_SLEEP:                   /* ACPI S3 */
							power_sleep();
							force_full = 1;   /* repaint after resume */
							break;
						case PA_RESTART:  power_reset(); break;
						case PA_SHUTDOWN: power_off();   break;
						}
					}
				}
				prev_down = down;
				return;
			}
			start_open = 0;        /* a click elsewhere closes it, then falls through */
		}
		int idx = window_at(mx, my);
		if (idx >= 0) {
			raise_window(idx);
			struct window *w = wins[idx];
			/* Title-bar buttons: close (app's on_close, else hide), maximize/
			 * restore, or minimize to the taskbar. */
			int tb = titlebtn_hit(w, mx, my);
			if (tb == 0) {                        /* close                   */
				/* Windows-style two stage: a responsive window is asked to
				 * close (on_close), but one already flagged Not Responding is
				 * force-killed on this click instead of asking again. */
				if (uwin_not_responding(w))
					wm_force_kill(idx);
				else if (w->on_close)
					w->on_close(w);
				else
					w->visible = 0;
			} else if (tb == 1) {                 /* maximize / restore      */
				wm_toggle_maximize(idx);
			} else if (tb == 2) {                 /* minimize                */
				wm_minimize(idx);
			} else if (w->resizable && !w->maximized &&
			           ((mx >= w->x + w->w - WM_GRIP && my >= w->y + w->h - WM_GRIP)
			            || resize_edge(w, mx, my))) {  /* grip or any frame edge */
				/* The BR grip is a larger hot-zone for the classic corner;
				 * resize_edge covers every edge/corner within WM_EDGE. */
				int e = (mx >= w->x + w->w - WM_GRIP &&
				         my >= w->y + w->h - WM_GRIP)
				        ? (RZ_R | RZ_B) : resize_edge(w, mx, my);
				resizing = idx;
				rsz_edge = e;
				rsz_ox = w->x; rsz_oy = w->y; rsz_ow = w->w; rsz_oh = w->h;
				rsz_mx0 = mx; rsz_my0 = my;
				rsz_win = idx;
				rsz_target_w = w->w; rsz_target_h = w->h;
				rsz_sent_w = rsz_sent_h = -1;  /* force the first hand-off  */
				rsz_ack = 0;
			} else if (my < w->y + WM_TITLE_H) {  /* grab the title bar      */
				/* Dragging a maximized window restores its saved size and
				 * follows the cursor (standard unmaximize-on-drag). */
				if (w->maximized) {
					int nw = w->saved_w, nh = w->saved_h;
					int nx = mx - nw / 2;
					if (nx < 0) nx = 0;
					if (nx > screen_w - nw) nx = screen_w - nw;
					w->maximized = 0;
					resize_to(idx, nx, 0, nw, nh);
				}
				dragging = idx;
				drag_dx = mx - w->x;
				drag_dy = my - w->y;
			} else {                               /* press in the client area */
				if (w->click) w->click(w, mx, my);
				if (w->drag)  app_dragging = idx;  /* follow motion until release */
			}
		}
	} else if (down && app_dragging >= 0) {     /* app-captured drag (scrollbar) */
		struct window *w = wins[app_dragging];
		if (w->drag) w->drag(w, mx, my);
	} else if (down && dragging >= 0) {         /* drag                        */
		struct window *w = wins[dragging];
		int nx = mx - drag_dx, ny = my - drag_dy;
		if (nx < 0) nx = 0;
		if (ny < 0) ny = 0;
		if (nx > screen_w - w->w) nx = screen_w - w->w;
		if (ny > screen_h - WM_BAR_H - WM_TITLE_H)
			ny = screen_h - WM_BAR_H - WM_TITLE_H;
		w->x = nx;
		w->y = ny;
	} else if (down && resizing >= 0) {         /* resize from an edge/corner  */
		struct window *w = wins[resizing];
		int dx = mx - rsz_mx0, dy = my - rsz_my0;
		int nx = rsz_ox, ny = rsz_oy, nw = rsz_ow, nh = rsz_oh;
		if (rsz_edge & RZ_R) nw = rsz_ow + dx;
		if (rsz_edge & RZ_B) nh = rsz_oh + dy;
		if (rsz_edge & RZ_L) { nw = rsz_ow - dx; nx = rsz_ox + dx; }
		if (rsz_edge & RZ_T) { nh = rsz_oh - dy; ny = rsz_oy + dy; }
		/* Min size, keeping the anchored (non-dragged) edge fixed. */
		if (nw < WM_MIN_W) { if (rsz_edge & RZ_L) nx -= WM_MIN_W - nw; nw = WM_MIN_W; }
		if (nh < WM_MIN_H) { if (rsz_edge & RZ_T) ny -= WM_MIN_H - nh; nh = WM_MIN_H; }
		/* Clamp to the screen (above the taskbar). */
		if (nx < 0) { nw += nx; nx = 0; }
		if (ny < 0) { nh += ny; ny = 0; }
		if (nx + nw > screen_w) nw = screen_w - nx;
		if (ny + nh > screen_h - WM_BAR_H) nh = screen_h - WM_BAR_H - ny;
		w->x = nx; w->y = ny;       /* frame tracks the cursor every frame      */
		w->w = nw; w->h = nh;
		rsz_target_w = nw;          /* commit step hands this to the app        */
		rsz_target_h = nh;
	} else if (!down) {                          /* release                     */
		dragging = -1;
		resizing = -1;              /* grip let go; rsz_win lives on to settle  */
		app_dragging = -1;
	}

	/* Pointer motion with no button held: deliver a hover event to the window
	 * under the cursor (apps use it for link-target status bars, tooltips).
	 * Fires only on actual movement and only inside the client area; when the
	 * pointer leaves the window we last hovered, that window gets one final
	 * hover with out-of-bounds (mx<0) coords so it can clear its hover state. */
	static int hover_slot = -1, hover_px = -1, hover_py = -1;
	if (!down && !rdown && dragging < 0 && resizing < 0 && app_dragging < 0 &&
	    (mx != hover_px || my != hover_py)) {
		hover_px = mx; hover_py = my;
		int idx = window_at(mx, my);
		struct window *w = idx >= 0 ? wins[idx] : 0;
		int in_client = w && my >= w->y + WM_TITLE_H && my < w->y + w->h - WM_BORDER &&
		                mx >= w->x + WM_BORDER && mx < w->x + w->w - WM_BORDER;
		if (hover_slot >= 0 && (idx != hover_slot || !in_client)) {
			struct window *p = wins[hover_slot];
			if (!p->dead && p->hover) p->hover(p, -1, -1);   /* left: clear  */
			hover_slot = -1;
		}
		if (in_client && w->hover) { w->hover(w, mx, my); hover_slot = idx; }
	}

	/* Drive the live-resize hand-off. Hand the app each new frame size once it
	 * has presented the previous one (so the realloc/remap never collides with
	 * an in-flight paint), and keep going after release until its content has
	 * reached the final size. */
	if (rsz_win >= 0) {
		struct window *w = wins[rsz_win];
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
		if (wins[i]->dead) { idx = i; break; }
	if (idx < 0) {
		if (!wm_reserve(nwin + 1))   /* grow the table; only the heap caps us */
			return 0;
		idx = nwin++;
		wins[idx] = kmalloc(sizeof(struct window));
		if (!wins[idx]) { nwin--; return 0; }
		zlist[idx] = idx;
	}
	struct window *win = wins[idx];
	*win = (struct window){ 0 };
	win->x = x; win->y = y; win->w = w; win->h = h;
	title_copy(win->title, title);
	win->closable = closable;
	win->resizable = 1;            /* opt out with w->resizable = 0 after this  */
	win->visible = 1;
	pf[idx].dirty = 0xFFFFFFFF;    /* force its first content paint            */
	raise_window(idx);             /* newest is topmost / focused              */
	return win;
}

void wm_destroy_window(struct window *w)
{
	if (!w)
		return;
	w->visible = 0;
	w->dead = 1;
	w->draw = 0; w->click = 0; w->rclick = 0; w->dirty = 0; w->key = 0;
	w->on_close = 0;
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

int wm_feed_key(char c, unsigned mods)
{
	/* The Super/Windows key toggles the Start menu, whatever holds focus. */
	if (c == KEY_SUPER) {
		start_open = !start_open;
		return 1;
	}

	/* Alt+Tab switcher: first Tab opens the overlay on the previous window
	 * (index 1), further Tabs step forward, Shift+Tab back. Z-order is untouched
	 * until Alt is released (KEY_ALT_UP). */
	if (c == KEY_TAB && (mods & KMOD_ALT)) {
		if (!at_active) {
			at_count = alttab_build();
			if (at_count <= 1) {     /* nothing to switch to */
				at_count = 0;
				return 1;
			}
			at_active = 1;
			at_sel = 0;
		}
		if (mods & KMOD_SHIFT)
			at_sel = (at_sel - 1 + at_count) % at_count;
		else
			at_sel = (at_sel + 1) % at_count;
		return 1;
	}
	if (c == KEY_ALT_UP) {           /* Alt released: commit the selection */
		if (at_active) {
			int idx = at_order[at_sel];
			at_active = 0;
			if (!wins[idx]->dead) {   /* the app may have exited mid-switch */
				if (wins[idx]->minimized)
					wm_unminimize(idx);
				else {
					wins[idx]->visible = 1;
					raise_window(idx);
				}
			}
		}
		return 1;
	}

	int t = topmost_visible();
	if (t < 0)
		return 0;
	struct window *w = wins[t];
	if (w->key) {
		w->key(w, c, mods);
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
	return t >= 0 && wins[t]->key != 0;
}

/* Free space in the focused window's key-event sink, for paste flow control.
 * A huge number when nothing focused, or the focused window doesn't queue keys
 * (e.g. the shell's own line editor) — those paths absorb keys without a bounded
 * ring, so a paste need not be throttled for them. */
int wm_focus_key_space(void)
{
	int t = topmost_visible();
	if (t < 0 || !wins[t]->key)
		return 1 << 30;
	return uwin_key_space(wins[t]);
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

/* Damage the bounding box of two rects as a SINGLE region. A moved window's old
 * and new rects must clear-and-redraw in one compose+present, or the per-rect
 * present loop updates the visible buffer in two steps and a drag tears: the old
 * position shows cleared before the new one is drawn. Coalescing is right here
 * (one window, old≈new each frame) even though unrelated rects stay separate. */
static void damage_add_union(int x0, int y0, int w0, int h0,
                             int x1, int y1, int w1, int h1)
{
	int ax0 = x0 < x1 ? x0 : x1;
	int ay0 = y0 < y1 ? y0 : y1;
	int ax1 = (x0 + w0) > (x1 + w1) ? (x0 + w0) : (x1 + w1);
	int ay1 = (y0 + h0) > (y1 + h1) ? (y0 + h0) : (y1 + h1);
	damage_add(ax0, ay0, ax1 - ax0, ay1 - ay0);
}

extern const char *g_boot_stage;   /* breadcrumb shown on the panic screen     */

/* Super+V clipboard history: the keyboard IRQ raises `cliphist_req`; the WM
 * thread launches /bin/cliphist next frame and tracks its pid so it can paste
 * the chosen clip into the now-focused window when the viewer exits. */
static volatile int cliphist_req;
static int          cliphist_pid;

void wm_request_cliphist(void) { cliphist_req = 1; }

/* A pending resolution change, posted by the sysfs writer (a ring-3 task in the
 * int 0x80 gate) and picked up by the compositor thread, which owns the
 * framebuffer geometry. 0 width = nothing pending. */
static volatile int mode_req_w, mode_req_h;

void wm_request_mode(int w, int h) { mode_req_w = w; mode_req_h = h; }

/* Apply a queued resolution change on the compositor thread: reprogram the
 * adapter, then re-derive everything keyed off the screen size — cursor bounds,
 * a maximized window's fill, and any window now hanging off the smaller screen.
 * force_full repaints the whole desktop at the new size. */
static void wm_apply_mode(void)
{
	int w = mode_req_w, h = mode_req_h;
	mode_req_w = mode_req_h = 0;
	if (w <= 0 || fb_set_mode((uint32_t)w, (uint32_t)h) != 0)
		return;

	screen_w = (int)fb_width();
	screen_h = (int)fb_height();
	mouse_set_bounds(screen_w - 1, screen_h - 1);

	for (int i = 0; i < nwin; i++) {
		struct window *win = wins[i];
		if (win->maximized) {
			resize_to(i, 0, 0, screen_w, screen_h - WM_BAR_H);
			continue;
		}
		if (win->w > screen_w)                 win->w = screen_w;
		if (win->h > screen_h - WM_BAR_H)      win->h = screen_h - WM_BAR_H;
		if (win->x + win->w > screen_w)        win->x = screen_w - win->w;
		if (win->y + win->h > screen_h - WM_BAR_H)
			win->y = screen_h - WM_BAR_H - win->h;
		if (win->x < 0) win->x = 0;
		if (win->y < 0) win->y = 0;
	}
	force_full = 1;
}

static void wm_thread(void)
{
	g_boot_stage = "wm_thread:enter";
	/* Previous-frame snapshots live in pf[] (grown with wins[]); see struct
	 * pfsnap. New slots start with pf.dirty = 0xFFFFFFFF so a window's first
	 * content always differs and paints. */
	int prev_mx = -9999, prev_my = -9999;
	int      prev_cur = -1;       /* cursor shape shown last frame              */
	int      prev_wheel = mouse_wheel();
	int      prev_start = 0;
	int      prev_tb_menu = 0;     /* taskbar context menu shown last frame    */
	int      last_sec = -1;
	int      prev_focus = -1;     /* topmost-visible window shown last frame     */
	int      prev_at_active = 0;  /* Alt+Tab overlay shown last frame            */
	int      prev_at_sel = -1;    /* highlighted switcher row last frame         */
	int      inited = 0;

	/* Pace the compositor to the panel's native refresh. edid_refresh_hz()
	 * reads it over GMBUS on real Intel hardware and returns 0 under QEMU /
	 * non-Intel / eDP, where we fall back to 60. Clamp to one tick so a high
	 * refresh can't spin the loop faster than the timer can wake it. */
	g_boot_stage = "wm_thread:edid";
	int      hz = edid_refresh_hz();
	g_boot_stage = "wm_thread:edid-done";
	int      frame_ms = 1000 / (hz ? hz : 60);
	if (frame_ms < 1000 / TIMER_HZ)
		frame_ms = 1000 / TIMER_HZ;

	g_boot_stage = "wm_thread:loop";
	for (;;) {
		if (mode_req_w)
			wm_apply_mode();           /* a queued resolution change            */

		/* Super+V: launch the clipboard-history viewer, and when it exits after
		 * picking a clip (exit 0), inject a paste into whatever window now has
		 * focus — the same path Ctrl+V uses. Reaped here, before the generic
		 * reap below, so we see its exit status. */
		if (cliphist_req) {
			cliphist_req = 0;
			if (!cliphist_pid)
				cliphist_pid = apps_open_cliphist();
		}
		if (cliphist_pid) {
			int cst;
			if (sched_try_reap(cliphist_pid, &cst)) {
				cliphist_pid = 0;
				if (cst == 0)
					keyboard_request_paste();
			}
		}

		/* Reap any GUI apps we launched from the Start menu that have exited. */
		int rst;
		while (sched_try_reap(-1, &rst))
			;
		crash_pump();              /* surface any crash as a window            */
		uwin_reap();               /* free buffers of windows torn down off-thread,
		                              now that we (the WM) are not mid-blit       */

		int mx = mouse_x(), my = mouse_y(), btn = mouse_buttons();
		int wheel = mouse_wheel();
		int dz = wheel - prev_wheel;
		prev_wheel = wheel;
		handle_input(mx, my, btn, dz);

		ndmg = 0;
		int full = !inited || force_full;  /* first frame, or a wake repaint    */
		force_full = 0;

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
		for (int i = 0; i < nwin; i++)
			pf[i].cur = wins[i]->dirty ? wins[i]->dirty(wins[i]) : 0;

		if (!full) {
			/* A z-order / visibility / focus change is recomposed in full. Two
			 * windows can't be damaged-and-repainted reliably here: it would need
			 * every window's old AND new rect (2 each), which overruns MAXDMG once
			 * a few windows are open (damage_add silently drops the overflow). And
			 * a focus change flips a title-bar highlight without moving the window
			 * at all, so there's no per-window rect to key off. The classic symptom
			 * is two windows both showing the focused (blue) title bar: the one that
			 * lost focus never got repainted. A focus change is detected directly
			 * off topmost_visible() rather than inferred from the zlist diff, which
			 * is unreliable when nwin grows (a freshly created window's prev_z slot
			 * holds stale data that can coincidentally match). These changes are
			 * rare and user-driven and the damage spans most of the screen anyway,
			 * so the full repaint costs little and never drops a window. */
			int focus = topmost_visible();
			int zvis = (focus != prev_focus);
			for (int i = 0; i < nwin; i++)
				if (zlist[i] != pf[i].z || wins[i]->visible != pf[i].vis)
					zvis = 1;
			if (zvis)
				full = 1;
			prev_focus = focus;

			/* The Alt+Tab overlay opening/closing or moving its highlight
			 * repaints in full: the panel is centered and overlaps an arbitrary
			 * stack of windows, so a sub-rect diff isn't worth the bookkeeping
			 * for a momentary, user-driven popup. */
			if (at_active != prev_at_active || at_sel != prev_at_sel)
				full = 1;
			prev_at_active = at_active;
			prev_at_sel = at_sel;

			/* The taskbar context menu opening/closing overlaps an arbitrary
			 * window stack — repaint in full, like the Start menu and switcher. */
			if (tb_menu_open != prev_tb_menu)
				full = 1;
			prev_tb_menu = tb_menu_open;

			/* Windows that moved or resized: damage old∪new as one region so the
			 * drag updates atomically (a two-rect present tears, see above). */
			for (int i = 0; i < nwin; i++)
				if (wins[i]->x != pf[i].x || wins[i]->y != pf[i].y ||
				    wins[i]->w != pf[i].w || wins[i]->h != pf[i].h)
					damage_add_union(pf[i].x, pf[i].y, pf[i].w, pf[i].h,
					                 wins[i]->x, wins[i]->y, wins[i]->w, wins[i]->h);
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
				if (wins[i]->visible && wins[i]->dirty &&
				    pf[i].cur != pf[i].dirty)
					damage_add(wins[i]->x, wins[i]->y, wins[i]->w, wins[i]->h);
			/* A window flipping to/from "Not Responding" rewrites its title with
			 * no present behind it — damage its title bar so the suffix shows or
			 * clears. (Its taskbar button repaints on the clock tick below.) */
			for (int i = 0; i < nwin; i++)
				if (wins[i]->visible &&
				    uwin_not_responding(wins[i]) != pf[i].nr)
					damage_add(wins[i]->x, wins[i]->y, wins[i]->w, WM_TITLE_H);
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
			/* Compose and present each damaged rectangle on its own. Coalescing
			 * them into a single bounding box (as this once did) is pathological
			 * when the rects are far apart: a terminal printing in one corner
			 * while the cursor sits in another would union to nearly the whole
			 * screen, so every such frame recomposed and copied a full
			 * 1920x1080 — the dominant cause of high-resolution stutter. Per-rect,
			 * the compose and the back->front copy both touch only genuinely
			 * damaged pixels; the dead gap between rects is never paid for.
			 * Overlapping rects (a slow drag's old/new frame) just recompose the
			 * shared pixels twice, which is idempotent and far cheaper than the
			 * gap. fb_present's own sfence drains the WC buffer per rect (<=16). */
			for (int k = 0; k < ndmg; k++) {
				fb_set_clip(dmg[k].x, dmg[k].y, dmg[k].w, dmg[k].h);
				compose(mx, my);
				fb_present();
			}
		}

		/* Snapshot for next frame's diff. */
		for (int i = 0; i < nwin; i++) {
			pf[i].x = wins[i]->x;   pf[i].y = wins[i]->y;
			pf[i].w = wins[i]->w;   pf[i].h = wins[i]->h;
			pf[i].vis = wins[i]->visible; pf[i].z = zlist[i];
			pf[i].nr = uwin_not_responding(wins[i]);
			pf[i].dirty = pf[i].cur;   /* sampled before compose(), above */
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

	/* Probe ACPI S3 once; the Start menu shows "Sleep" only if it's available. */
	sleep_ok = power_can_sleep();

	/* Apps register their windows here; the terminal is created last so it
	 * starts focused / on top (see apps.c). */
	mouse_init(screen_w - 1, screen_h - 1);
	apps_start();

	/* Route the console into the terminal window, then start compositing. */
	con_use_framebuffer(1);
	sched_create_thread("wm", wm_thread);
}
