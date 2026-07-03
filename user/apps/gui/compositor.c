/* compositor.c — the user-space window manager.
 *
 * Runs the desktop on the root-only windowing primitives (owos.h): SYS_FB_* for
 * the shared cached-RAM back buffer, SYS_INPUT_POLL for the pointer + keyboard,
 * and SYS_WIN_* to enumerate / map / move / focus / feed the windows the kernel
 * brokers. It composites the desktop (background, framed windows with title bars
 * + borders + close/max/min boxes, taskbar with clock/battery/volume/network,
 * Start menu, Alt+Tab overlay, "Not Responding" suffix, crash windows, and the
 * cursor) into the back buffer using damage-tracking, presenting only the damaged
 * rectangles.
 *
 * This is the live desktop: /etc/inittab launches it as the first init entry, and
 * the first SYS_FB_MAP marks it as the compositor so the kernel stops draining the
 * physical keyboard into the recovery console. The in-kernel WM has been removed.
 *
 * The metrics/colours/layout mirror the old kernel wm.c; where that drew with
 * kernel fb_* into a clipped surface, we draw with gfx_* into the mapped back
 * buffer, using gfx_set_clip() as the per-rect clip.
 */
#include "owos.h"
#include "gfx.h"
#include "clipboard.h"

/* ---- WM metrics (mirror kernel/wm.h) ---- */
#define WM_TITLE_H   22
#define WM_BORDER    2
#define WM_BAR_H     28
#define WM_TITLE_MAX 64
#define WM_GRIP      14
#define WM_EDGE      5
#define WM_MIN_W     140
#define WM_MIN_H     120

/* Cursor shapes (mirror enum wm_cursor / WIN_CURSOR_*). */
enum { CUR_ARROW = 0, CUR_HAND, CUR_TEXT, CUR_BUSY,
       CUR_RESIZE_NWSE, CUR_RESIZE_NESW, CUR_RESIZE_NS, CUR_RESIZE_EW };

/* Global keys the kernel routes through wm_feed_key (mirror keyboard.h). */
#define KEY_TAB     '\t'
#define KEY_ALT_UP  0x1B   /* internal: "Alt released" marker (kernel wm KEY_ALT_UP) */

#define WM_LIST_MAX 64

/* ---- screen + back buffer ---- */
static unsigned *back;         /* mapped back buffer (COMPOSITOR_FB_ADDR)         */
static int screen_w, screen_h; /* visible dimensions                              */
static int fb_pitch;           /* back-buffer stride in pixels (== gfx surface w) */

/* Mirror of the active gfx clip rect, so blit_client (which writes `back`
 * directly) intersects against the same region gfx_set_clip installed. */
static int clip_x0, clip_y0, clip_x1, clip_y1;
static void set_clip(int x, int y, int w, int h)
{
	int x1 = x + w, y1 = y + h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > screen_w) x1 = screen_w;
	if (y1 > screen_h) y1 = screen_h;
	clip_x0 = x; clip_y0 = y; clip_x1 = x1 > x ? x1 : x; clip_y1 = y1 > y ? y1 : y;
	gfx_set_clip(x, y, w, h);
}
static void clip_full(void)
{
	clip_x0 = 0; clip_y0 = 0; clip_x1 = screen_w; clip_y1 = screen_h;
	gfx_clip_none();
}

/* ------------------------------------------------------------- window model */
/* Each frame we SYS_WIN_ENUM into cur[]; entries are in ascending z (bottom
 * first), so the array index doubles as the z-order slot. The kernel owns z/
 * focus/geometry; we drive them through win_set_geom/win_set_focus. */
static struct winrec cur[WM_LIST_MAX];
static int nwin;

static int win_visible(const struct winrec *w)        { return (w->flags & UWIN_F_VISIBLE) != 0; }
static int win_minimized(const struct winrec *w)      { return (w->flags & UWIN_F_MINIMIZED) != 0; }

/* Compositor-tracked maximize state (see the maxrec table below): -1 if not. */
static int is_maxed(int id);
static void maxed_forget_gone(void);

/* wins index by z position i (bottom..top); cur[] is already in that order. */
static int slot_of(int id)
{
	for (int i = 0; i < nwin; i++)
		if (cur[i].id == id) return i;
	return -1;
}

/* ------------------------------------------------------------- "Not Responding"
 * The kernel exposes this only implicitly (the WM decided it). We don't have that
 * signal here yet, so treat every window as responsive for the suffix logic; the
 * hook stays so the draw/damage paths match wm.c. Phase 3 can wire a real signal
 * (e.g. a winrec flag). */
static int not_responding(const struct winrec *w) { (void)w; return 0; }

/* ------------------------------------------------------------- title helpers */
static void put2(char *p, int v) { p[0] = (char)('0' + v / 10); p[1] = (char)('0' + v % 10); }

#define WM_TITLE_DISP (WM_TITLE_MAX + 18)
static void disp_title(const struct winrec *w, char *buf)
{
	int n = 0;
	for (; w->title[n] && n < WM_TITLE_MAX - 1; n++) buf[n] = w->title[n];
	buf[n] = '\0';
	if (not_responding(w)) {
		static const char sfx[] = " (Not Responding)";
		for (int j = 0; sfx[j]; j++) buf[n++] = sfx[j];
		buf[n] = '\0';
	}
}

/* ------------------------------------------------------------- taskbar / menus */
#define START_W   70
#define SM_ITEM_H 20
#define SM_W      150
#define SM_SEP_H  9
static int start_open;

#define TBM_W      120
#define TBM_ITEM_H 22
#define TBM_N      2
static int tb_menu_open;
static int tb_menu_win = -1;    /* targeted cur[] id */
static int tb_menu_x, tb_menu_y;
static int tbm_h(void) { return TBM_N * TBM_ITEM_H; }

#define TB_BTN_W   14
#define TB_BTN_GAP 3
#define TB_WIN_GAP 4
static int tb_band_right;
static int tb_band_left(void) { return START_W + 6; }

/* ------------------------------------------------------------- Start menu apps
 * Ported app_menu[] from kernel/apps.c: each row spawns a /bin program. */
static const struct { const char *name; const char *bin; } app_menu[] = {
	{ "Terminal",      "term"     },
	{ "Files",         "files"    },
	{ "Editor",        "editor"   },
	{ "Browser",       "browser"  },
	{ "Media Player",  "player"   },
	{ "Clock",         "clock"    },
	{ "Calculator",    "calc"     },
	{ "Notifications", "notifyd"  },
	{ "Settings",      "settings" },
	{ "About",         "about"    },
};
static int apps_menu_count(void) { return (int)(sizeof app_menu / sizeof app_menu[0]); }

/* Power actions. We can't probe ACPI S3 from user space, so always offer the two
 * that the power() syscall supports (shut down / reboot); no "Sleep". */
enum { PA_RESTART, PA_SHUTDOWN };
static int sm_power_n(void) { return 2; }
static const char *sm_power_label(int row) { return row == 0 ? "Restart" : "Shut Down"; }

static int sm_apps_h(void) { return apps_menu_count() * SM_ITEM_H; }
static int sm_h(void)  { return sm_apps_h() + SM_SEP_H + sm_power_n() * SM_ITEM_H; }
static int sm_top(void){ return screen_h - WM_BAR_H - sm_h(); }

static int force_full;

/* ------------------------------------------------------------- child reaping
 * We spawn Start-menu apps detached; reap their zombies non-blocking so they
 * don't accumulate (mirrors wm.c's sched_try_reap loop). spawn() returns a pid;
 * a subsequent waitpid on any of our children would block, so we track pids and
 * only reap those. The kernel has no non-blocking waitpid wrapper for arbitrary
 * children, so we keep a small pid ring and try each. */
#define MAXKIDS 64
static int kids[MAXKIDS];
static int nkids;

static void kid_track(int pid) { if (pid > 0 && nkids < MAXKIDS) kids[nkids++] = pid; }

/* Reap exited children non-blocking so zombies don't accumulate. wm.c reaped as
 * the kernel-side parent; we're the user-space parent, so we use waitpid_nb (a
 * blocking waitpid would freeze the UI). */
static void reap_kids(void)
{
	int st;
	for (int i = 0; i < nkids; ) {
		if (waitpid_nb(kids[i], &st)) kids[i] = kids[--nkids];
		else i++;
	}
}

static void launch_app(int i)
{
	if (i < 0 || i >= apps_menu_count()) return;
	char path[64];
	int j = 0;
	const char *pfx = "/bin/";
	for (int k = 0; pfx[k]; k++) path[j++] = pfx[k];
	for (int k = 0; app_menu[i].bin[k] && j < (int)sizeof path - 1; k++)
		path[j++] = app_menu[i].bin[k];
	path[j] = '\0';
	kid_track(spawn(path, 0));
}

/* ------------------------------------------------------------- cursors */
static const char *const arrow_glyph[] = {
	"X           ","XX          ","X.X         ","X..X        ","X...X       ",
	"X....X      ","X.....X     ","X......X    ","X.......X   ","X........X  ",
	"X.....XXXXX ","X..X..X     ","X.X X..X    ","XX  X..X    ","X    X..X   ",
	"     X..X   ","      X..X  ","      X..X  ","       XX   ",
};
static const char *const hand_glyph[] = {
	"  XX     ","  X.X    ","  X.X    ","  X.X    ","  X.XXXX ","  X.X..X ",
	"XXX...X.X","X.......X","X.......X","X.......X"," X......X"," X......X",
	"  X.....X","  XXXXXXX",
};
static const char *const text_glyph[] = {
	"X...X","XX.XX"," X.X "," X.X "," X.X "," X.X "," X.X ",
	" X.X "," X.X "," X.X "," X.X "," X.X ","XX.XX","X...X",
};
struct cur_shape { const char *const *rows; int nrows, hotx, hoty; };
static const struct cur_shape cur_shapes[] = {
	[CUR_ARROW] = { arrow_glyph, 19, 0, 0 },
	[CUR_HAND]  = { hand_glyph,  14, 2, 0 },
	[CUR_TEXT]  = { text_glyph,  14, 2, 7 },
};
#define CUR_DMG_PAD 9
#define CUR_DMG_W   24
#define CUR_DMG_H   30
static unsigned cursor_frame;

/* ------------------------------------------------------------- z / hit-testing */

/* Topmost visible window's cur[] slot (holds focus), or -1. */
static int topmost_visible(void)
{
	for (int i = nwin - 1; i >= 0; i--)
		if (win_visible(&cur[i])) return i;
	return -1;
}

/* Topmost visible window containing (px,py), or -1. */
static int window_at(int px, int py)
{
	for (int i = nwin - 1; i >= 0; i--) {
		struct winrec *w = &cur[i];
		if (!win_visible(w)) continue;
		if (px >= w->x && px < w->x + w->w && py >= w->y && py < w->y + w->h)
			return i;
	}
	return -1;
}

/* Raise a window: focus it (the kernel makes the focused window topmost). */
static void raise_window(int slot)
{
	if (slot >= 0 && slot < nwin) win_set_focus(cur[slot].id);
}

/* ------------------------------------------------------------- Alt+Tab switcher */
static int at_active;
static int at_order[WM_LIST_MAX];   /* cur[] slots, most-recent first */
static int at_count;
static int at_sel;

static int alttab_build(void)
{
	int n = 0;
	for (int i = nwin - 1; i >= 0 && n < WM_LIST_MAX; i--) {
		struct winrec *w = &cur[i];
		if (win_visible(w) || win_minimized(w)) at_order[n++] = i;
	}
	return n;
}

/* ------------------------------------------------------------- title buttons */
static int btn_present(const struct winrec *w, int kind)
{
	if (kind == 0) return 1;                 /* close (all app windows closable) */
	if (kind == 1) return 1;                 /* maximize (all resizable)         */
	return 1;                                /* minimize                         */
}
static int btn_slot(const struct winrec *w, int kind)
{
	if (!btn_present(w, kind)) return -1;
	int slot = 0;
	if (kind > 0 && btn_present(w, 0)) slot++;
	if (kind > 1 && btn_present(w, 1)) slot++;
	return slot;
}
static int titlebtn_box(const struct winrec *w, int kind, int *bx, int *by)
{
	int slot = btn_slot(w, kind);
	if (slot < 0) return 0;
	*bx = w->x + w->w - 4 - (slot + 1) * TB_BTN_W - slot * TB_BTN_GAP;
	*by = w->y + 4;
	return 1;
}
static int titlebtn_hit(const struct winrec *w, int mx, int my)
{
	for (int k = 0; k < 3; k++) {
		int bx, by;
		if (!titlebtn_box(w, k, &bx, &by)) continue;
		if (mx >= bx && mx < bx + TB_BTN_W && my >= by && my < by + TB_BTN_W)
			return k;
	}
	return -1;
}

/* ------------------------------------------------------------- taskbar layout */
static int taskbar_layout(int left, int right, int *idxs, int *bxs, int *bws, int max)
{
	int n = 0;
	for (int i = 0; i < nwin && n < max; i++) idxs[n++] = i;
	if (n == 0) return 0;
	int avail = right - left;
	if (avail < n) return 0;
	int bw = avail / n;
	if (bw > 168) bw = 168;
	for (int k = 0; k < n; k++) {
		bxs[k] = left + k * bw;
		bws[k] = bw - TB_WIN_GAP;
		if (bws[k] < 1) bws[k] = 1;
	}
	return n;
}
static int taskbar_hit(int mx, int my)
{
	if (my < screen_h - WM_BAR_H) return -1;
	int idxs[WM_LIST_MAX], bxs[WM_LIST_MAX], bws[WM_LIST_MAX];
	int n = taskbar_layout(tb_band_left(), tb_band_right, idxs, bxs, bws, WM_LIST_MAX);
	for (int k = 0; k < n; k++)
		if (mx >= bxs[k] && mx < bxs[k] + bws[k]) return idxs[k];
	return -1;
}

/* ------------------------------------------------------------- rendering */

static void ui_text_clip(int x, int y, const char *s, unsigned col, int maxw);

static void draw_background(void)
{
	/* Vertical gradient (deep indigo -> slate); a function of y only. gfx_fill_rect
	 * bounds each row to the active clip, so an unclipped full-scene redraw during
	 * a small damage rect only touches the damaged rows. */
	for (int y = 0; y < screen_h; y++) {
		int t = y * 255 / screen_h;
		unsigned r = (unsigned)(24 + t * 40 / 255);
		unsigned g = (unsigned)(26 + t * 56 / 255);
		unsigned b = (unsigned)(54 + t * 70 / 255);
		gfx_fill_rect(0, y, screen_w, 1, (r << 16) | (g << 8) | b);
	}
}

/* Blit a window's client buffer into the back buffer at the client rect,
 * honouring the active gfx clip (intersected manually since this writes `back`
 * directly for an opaque XRGB source with a source stride). */
static void blit_client(int dx, int dy, const unsigned *src, int sw, int sh, int sstride)
{
	int cx0 = dx, cy0 = dy, cx1 = dx + sw, cy1 = dy + sh;
	if (cx0 < clip_x0) cx0 = clip_x0;
	if (cy0 < clip_y0) cy0 = clip_y0;
	if (cx1 > clip_x1) cx1 = clip_x1;
	if (cy1 > clip_y1) cy1 = clip_y1;
	for (int y = cy0; y < cy1; y++) {
		const unsigned *s = src + (long)(y - dy) * sstride + (cx0 - dx);
		unsigned *d = back + (long)y * fb_pitch + cx0;
		for (int x = cx0; x < cx1; x++) *d++ = *s++;
	}
}

/* the per-window client buffer, pinned via win_map and kept mapped across frames
 * (the Phase-1 pin), refreshed each frame from the enumerated set. */
struct clientmap { int id, valid; struct winmap m; };
static struct clientmap cmap[WM_LIST_MAX];
static int ncmap;

static const struct winmap *client_for(int id)
{
	for (int i = 0; i < ncmap; i++)
		if (cmap[i].id == id) return cmap[i].valid ? &cmap[i].m : 0;
	return 0;
}

/* Refresh the pinned client maps to match the enumerated set: (re)map every live
 * window (win_map is idempotent and picks up a resized buffer's new size), and
 * unmap any id no longer enumerated so the kernel can reclaim its frames. */
static void refresh_maps(void)
{
	/* drop maps for gone windows */
	for (int i = 0; i < ncmap; ) {
		if (slot_of(cmap[i].id) < 0) {
			win_unmap(cmap[i].id);
			cmap[i] = cmap[--ncmap];
		} else i++;
	}
	/* (re)map every enumerated window */
	for (int s = 0; s < nwin; s++) {
		int id = cur[s].id, found = -1;
		for (int i = 0; i < ncmap; i++) if (cmap[i].id == id) { found = i; break; }
		if (found < 0) {
			if (ncmap >= WM_LIST_MAX) continue;
			found = ncmap++;
			cmap[found].id = id;
		}
		cmap[found].valid = (win_map(id, &cmap[found].m) == 0);
	}
	maxed_forget_gone();   /* clear maximize state for closed windows */
}

static void draw_window(const struct winrec *w, int focused)
{
	unsigned title_bg = focused ? 0x2f6fb0 : 0x55606e;
	unsigned frame    = 0x10141a;

	gfx_fill_rect(w->x, w->y, w->w, w->h, frame);
	gfx_fill_rect(w->x, w->y, w->w, WM_TITLE_H, title_bg);

	int title_x = w->x + 8;
	int title_right = w->x + w->w - 4;
	for (int k = 0; k < 3; k++) {
		int bbx, bby;
		if (titlebtn_box(w, k, &bbx, &bby) && bbx < title_right) title_right = bbx;
	}
	char dt[WM_TITLE_DISP];
	disp_title(w, dt);
	ui_text_clip(title_x, w->y + (WM_TITLE_H - UIFONT_H) / 2, dt, 0xffffff,
	             title_right - title_x - 6);

	int bx, by;
	if (titlebtn_box(w, 2, &bx, &by)) {                 /* minimize */
		gfx_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0x3a4658);
		gfx_fill_rect(bx + 3, by + 9, 8, 2, 0xffffff);
	}
	if (titlebtn_box(w, 1, &bx, &by)) {                 /* maximize / restore */
		gfx_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0x3a4658);
		if (is_maxed(w->id) >= 0) {
			gfx_rect(bx + 4, by + 2, 6, 6, 0xffffff);
			gfx_fill_rect(bx + 2, by + 5, 6, 6, 0x3a4658);
			gfx_rect(bx + 2, by + 5, 6, 6, 0xffffff);
		} else {
			gfx_rect(bx + 3, by + 3, 8, 8, 0xffffff);
		}
	}
	if (titlebtn_box(w, 0, &bx, &by)) {                 /* close */
		gfx_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0xc04030);
		for (int d = 0; d < 7; d++) {
			gfx_pixel(bx + 4 + d, by + 4 + d, 0xffffff);
			gfx_pixel(bx + 10 - d, by + 4 + d, 0xffffff);
		}
	}

	/* Client area: blit the app's pixel buffer, or fill the chrome bg if unmapped. */
	int cx = w->x + WM_BORDER;
	int cy = w->y + WM_TITLE_H;
	int cw = w->w - 2 * WM_BORDER;
	int ch = w->h - WM_TITLE_H - WM_BORDER;
	const struct winmap *cm = client_for(w->id);
	if (cm && cm->vaddr) {
		int bw = cm->w < cw ? cm->w : cw;
		int bh = cm->h < ch ? cm->h : ch;
		blit_client(cx, cy, (const unsigned *)cm->vaddr, bw, bh, cm->stride);
		if (bw < cw) gfx_fill_rect(cx + bw, cy, cw - bw, ch, 0x10141a);
		if (bh < ch) gfx_fill_rect(cx, cy + bh, cw, ch - bh, 0x10141a);
	} else {
		gfx_fill_rect(cx, cy, cw, ch, 0x10141a);
	}

	gfx_rect(w->x, w->y, w->w, w->h, 0x05070a);

	/* Resize grip ticks (all app windows are resizable). */
	int gx = w->x + w->w - WM_GRIP, gy = w->y + w->h - WM_GRIP;
	unsigned gc = focused ? 0x7f93ad : 0x55606e;
	for (int d = 3; d < WM_GRIP; d += 4)
		for (int t = 0; t <= d; t++)
			gfx_pixel(gx + WM_GRIP - 1 - t, gy + WM_GRIP - 1 - (d - t), gc);
}

/* ---- cursor drawing (ported from wm.c) ---- */
static void draw_glyph(const struct cur_shape *s, int x, int y)
{
	for (int row = 0; row < s->nrows; row++) {
		const char *line = s->rows[row];
		for (int col = 0; line[col]; col++) {
			if (line[col] == 'X') gfx_pixel(x + col, y + row, 0x000000);
			else if (line[col] == '.') gfx_pixel(x + col, y + row, 0xffffff);
		}
	}
}
static void cur_dot(int cx, int cy, unsigned inner)
{
	gfx_fill_rect(cx - 2, cy - 2, 4, 4, 0x000000);
	gfx_fill_rect(cx - 1, cy - 1, 2, 2, inner);
}
static void draw_busy(int mx, int my)
{
	static const int dx[8] = {  0,  4,  6,  4,  0, -4, -6, -4 };
	static const int dy[8] = { -6, -4,  0,  4,  6,  4,  0, -4 };
	int head = (int)((cursor_frame / 3) & 7);
	for (int i = 0; i < 8; i++) {
		int dist = (head - i + 8) & 7;
		int b = 40 + (7 - dist) * 30;
		if (b > 255) b = 255;
		cur_dot(mx + dx[i], my + dy[i], (unsigned)((b << 16) | (b << 8) | b));
	}
}
static void cur_stroke(int x, int y, int halo)
{
	if (halo) {
		gfx_pixel(x, y, 0xffffff); gfx_pixel(x + 1, y, 0xffffff);
		gfx_pixel(x - 1, y, 0xffffff); gfx_pixel(x, y + 1, 0xffffff);
		gfx_pixel(x, y - 1, 0xffffff);
	} else gfx_pixel(x, y, 0x000000);
}
static void draw_resize_nwse(int mx, int my)
{
	for (int halo = 1; halo >= 0; halo--) {
		for (int t = -5; t <= 5; t++) cur_stroke(mx + t, my + t, halo);
		for (int k = 0; k <= 4; k++) {
			cur_stroke(mx - 5 + k, my - 5, halo); cur_stroke(mx - 5, my - 5 + k, halo);
			cur_stroke(mx + 5 - k, my + 5, halo); cur_stroke(mx + 5, my + 5 - k, halo);
		}
	}
}
static void draw_resize_nesw(int mx, int my)
{
	for (int halo = 1; halo >= 0; halo--) {
		for (int t = -5; t <= 5; t++) cur_stroke(mx + t, my - t, halo);
		for (int k = 0; k <= 4; k++) {
			cur_stroke(mx + 5 - k, my - 5, halo); cur_stroke(mx + 5, my - 5 + k, halo);
			cur_stroke(mx - 5 + k, my + 5, halo); cur_stroke(mx - 5, my + 5 - k, halo);
		}
	}
}
static void draw_resize_ns(int mx, int my)
{
	for (int halo = 1; halo >= 0; halo--) {
		for (int t = -6; t <= 6; t++) cur_stroke(mx, my + t, halo);
		for (int k = 0; k <= 4; k++) {
			cur_stroke(mx - k, my - 6 + k, halo); cur_stroke(mx + k, my - 6 + k, halo);
			cur_stroke(mx - k, my + 6 - k, halo); cur_stroke(mx + k, my + 6 - k, halo);
		}
	}
}
static void draw_resize_ew(int mx, int my)
{
	for (int halo = 1; halo >= 0; halo--) {
		for (int t = -6; t <= 6; t++) cur_stroke(mx + t, my, halo);
		for (int k = 0; k <= 4; k++) {
			cur_stroke(mx - 6 + k, my - k, halo); cur_stroke(mx - 6 + k, my + k, halo);
			cur_stroke(mx + 6 - k, my - k, halo); cur_stroke(mx + 6 - k, my + k, halo);
		}
	}
}
static void draw_cursor(int kind, int mx, int my)
{
	switch (kind) {
	case CUR_BUSY:        draw_busy(mx, my);        return;
	case CUR_RESIZE_NWSE: draw_resize_nwse(mx, my); return;
	case CUR_RESIZE_NESW: draw_resize_nesw(mx, my); return;
	case CUR_RESIZE_NS:   draw_resize_ns(mx, my);   return;
	case CUR_RESIZE_EW:   draw_resize_ew(mx, my);   return;
	default:
		if (kind < 0 || kind >= (int)(sizeof cur_shapes / sizeof cur_shapes[0]) ||
		    !cur_shapes[kind].rows)
			kind = CUR_ARROW;
		const struct cur_shape *s = &cur_shapes[kind];
		draw_glyph(s, mx - s->hotx, my - s->hoty);
		return;
	}
}

/* ---- taskbar indicators (battery/volume/network are unavailable to a plain
 * user program; the compositor draws only the clock. Phase 3 can expose these
 * via /proc reads if wanted.) ---- */
static void ui_text_clip(int x, int y, const char *s, unsigned col, int maxw)
{
	char buf[WM_TITLE_MAX + 2];
	int n = 0;
	for (; s[n] && n < WM_TITLE_MAX; n++) buf[n] = s[n];
	buf[n] = '\0';
	if (maxw <= 0) return;
	if (gfx_ui_width(buf) <= maxw) { gfx_ui_text(x, y, buf, col); return; }
	while (n > 0) {
		buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '\0';
		if (gfx_ui_width(buf) <= maxw) break;
		buf[n] = '\0';
		n--;
	}
	if (n > 0) gfx_ui_text(x, y, buf, col);
}

static void draw_taskbar_windows(int y, int uy, int right)
{
	tb_band_right = right;
	int idxs[WM_LIST_MAX], bxs[WM_LIST_MAX], bws[WM_LIST_MAX];
	int n = taskbar_layout(tb_band_left(), right, idxs, bxs, bws, WM_LIST_MAX);
	int focus = topmost_visible();
	int by = y + 3, bh = WM_BAR_H - 6;
	for (int k = 0; k < n; k++) {
		struct winrec *w = &cur[idxs[k]];
		int active = (idxs[k] == focus);
		unsigned bg = active ? 0x2f6fb0 : win_minimized(w) ? 0x12161e : 0x1d2533;
		gfx_fill_rect(bxs[k], by, bws[k], bh, bg);
		gfx_fill_rect(bxs[k], by, bws[k], 1, active ? 0x4f8fd0 : 0x2a3550);
		unsigned fg = win_minimized(w) ? 0x8b94a4 : 0xc8d0dc;
		char dt[WM_TITLE_DISP];
		disp_title(w, dt);
		ui_text_clip(bxs[k] + 8, uy, dt, fg, bws[k] - 12);
	}
}

static void draw_taskbar(void)
{
	int y = screen_h - WM_BAR_H;
	gfx_fill_rect(0, y, screen_w, WM_BAR_H, 0x0c0e14);
	gfx_fill_rect(0, y, screen_w, 1, 0x2a3550);
	unsigned btn = start_open ? 0x2f6fb0 : 0x0c0e14;
	int uy = y + (WM_BAR_H - UIFONT_H) / 2;
	gfx_fill_rect(0, y + 1, START_W, WM_BAR_H - 1, btn);
	gfx_ui_text(10, uy, "OwOS", 0x9ad0ff);

	unsigned t = now_hms();
	char s[9] = "00:00:00";
	put2(s + 0, (int)((t >> 16) & 0xFF));
	put2(s + 3, (int)((t >> 8) & 0xFF));
	put2(s + 6, (int)(t & 0xFF));
	int clock_x = screen_w - gfx_ui_width(s) - 12;
	gfx_ui_text(clock_x, uy, s, 0xc8d0dc);

	draw_taskbar_windows(y, uy, clock_x - 10);
}

static void draw_start_menu(void)
{
	int n = apps_menu_count();
	int top = sm_top();
	gfx_fill_rect(0, top, SM_W, sm_h(), 0x141a24);
	for (int i = 0; i < n; i++) {
		int iy = top + i * SM_ITEM_H;
		gfx_ui_text(10, iy + (SM_ITEM_H - UIFONT_H) / 2, app_menu[i].name, 0xc8d0dc);
	}
	int sep_y = top + sm_apps_h() + SM_SEP_H / 2;
	gfx_fill_rect(8, sep_y, SM_W - 16, 1, 0x05070a);
	int py = top + sm_apps_h() + SM_SEP_H;
	for (int i = 0; i < sm_power_n(); i++) {
		int iy = py + i * SM_ITEM_H;
		gfx_ui_text(10, iy + (SM_ITEM_H - UIFONT_H) / 2, sm_power_label(i), 0xa8b4c4);
	}
	gfx_rect(0, top, SM_W, sm_h(), 0x05070a);
}

static void draw_tb_menu(void)
{
	static const char *const items[TBM_N] = { "Close", "Force Kill" };
	int x = tb_menu_x, y = tb_menu_y;
	gfx_fill_rect(x, y, TBM_W, tbm_h(), 0x141a24);
	for (int i = 0; i < TBM_N; i++) {
		int iy = y + i * TBM_ITEM_H;
		unsigned fg = (i == 1) ? 0xe08070 : 0xc8d0dc;
		gfx_ui_text(x + 12, iy + (TBM_ITEM_H - UIFONT_H) / 2, items[i], fg);
	}
	gfx_rect(x, y, TBM_W, tbm_h(), 0x05070a);
}

/* ---- Alt+Tab overlay ---- */
#define AT_ROW_H  24
#define AT_PAD    12
#define AT_PANEL_W 340
static void switcher_box(int *px, int *py, int *pw, int *ph)
{
	int h = AT_PAD * 2 + at_count * AT_ROW_H;
	int maxh = screen_h - WM_BAR_H - 20;
	if (h > maxh) h = maxh;
	*pw = AT_PANEL_W;
	*ph = h;
	*px = (screen_w - AT_PANEL_W) / 2;
	*py = (screen_h - h) / 2;
}
static void draw_switcher(void)
{
	int x, y, pw, ph;
	switcher_box(&x, &y, &pw, &ph);
	gfx_fill_rect(x, y, pw, ph, 0x141a24);
	gfx_rect(x, y, pw, ph, 0x05070a);
	gfx_rect(x + 1, y + 1, pw - 2, ph - 2, 0x2a3550);
	int rows = (ph - 2 * AT_PAD) / AT_ROW_H;
	if (rows > at_count) rows = at_count;
	for (int i = 0; i < rows; i++) {
		int ry = y + AT_PAD + i * AT_ROW_H;
		if (i == at_sel) gfx_fill_rect(x + 4, ry, pw - 8, AT_ROW_H, 0x2f6fb0);
		struct winrec *w = &cur[at_order[i]];
		unsigned fg = (i == at_sel) ? 0xffffff : 0xc8d0dc;
		ui_text_clip(x + 14, ry + (AT_ROW_H - UIFONT_H) / 2, w->title, fg, pw - 28);
	}
}

/* ---- crash windows: rendered directly (no kernel window). Pending reports are
 * drained once via crash_poll(); each becomes a persistent panel the user can
 * dismiss by clicking its close box. ---- */
struct crashwin { int used, x, y, w, h; struct crash_rec rec; };
#define MAXCRASHWIN 4
static struct crashwin crashwins[MAXCRASHWIN];

static void crash_ingest(void)
{
	struct crash_rec recs[MAXCRASHWIN];
	int n = crash_poll(recs, MAXCRASHWIN);
	for (int i = 0; i < n; i++) {
		for (int s = 0; s < MAXCRASHWIN; s++) {
			if (crashwins[s].used) continue;
			crashwins[s].used = 1;
			crashwins[s].x = 140 + s * 22; crashwins[s].y = 110 + s * 22;
			crashwins[s].w = 460; crashwins[s].h = 300;
			crashwins[s].rec = recs[i];
			force_full = 1;
			break;
		}
	}
}

static const char *hex8(unsigned v, char *out)
{
	static const char d[] = "0123456789abcdef";
	for (int i = 0; i < 8; i++) out[i] = d[(v >> ((7 - i) * 4)) & 0xF];
	out[8] = '\0';
	return out;
}
static void draw_crashwin(struct crashwin *c)
{
	unsigned bg = 0x1a1020;
	int x0 = c->x, y0 = c->y;
	gfx_fill_rect(x0, y0, c->w, WM_TITLE_H, 0xc04030);
	gfx_ui_text(x0 + 8, y0 + (WM_TITLE_H - UIFONT_H) / 2, "crash", 0xffffff);
	/* close box (X) top-right */
	int bx = x0 + c->w - 4 - TB_BTN_W, by = y0 + 4;
	gfx_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0x902020);
	for (int d = 0; d < 7; d++) {
		gfx_pixel(bx + 4 + d, by + 4 + d, 0xffffff);
		gfx_pixel(bx + 10 - d, by + 4 + d, 0xffffff);
	}
	int cy = y0 + WM_TITLE_H;
	gfx_fill_rect(x0, cy, c->w, c->h - WM_TITLE_H, bg);

	struct crash_rec *r = &c->rec;
	char hx[12];
	int x = x0 + 12, y = cy + 10, lh = 16;
	char title[48];
	int t = 0;
	for (; r->name[t] && t < 23; t++) title[t] = r->name[t];
	const char *suf = " has crashed.";
	for (int k = 0; suf[k] && t < 47; k++) title[t++] = suf[k];
	title[t] = '\0';
	gfx_ui_text(x, y, title, 0xff8080); y += lh + 8;

	gfx_ui_text(x, y, "fault vector:", 0x9098a0);
	char vs[4]; put2(vs, (int)(r->vector % 100)); vs[2] = 0;
	gfx_ui_text(x + 120, y, vs, 0xe0e6ee); y += lh;
	gfx_ui_text(x, y, "eip=", 0x9098a0); gfx_ui_text(x + 40, y, hex8(r->eip, hx), 0x66ddff);
	if (r->vector == 14) {
		gfx_ui_text(x + 170, y, "cr2=", 0x9098a0);
		gfx_ui_text(x + 210, y, hex8(r->cr2, hx), 0xffe08a);
	}
	y += lh;
	gfx_ui_text(x, y, "err=", 0x9098a0); gfx_ui_text(x + 40, y, hex8(r->err, hx), 0xc8d0dc);
	y += lh + 8;
	gfx_ui_text(x, y, "stack trace:", 0x9098a0); y += lh;
	for (int i = 0; i < r->ntrace && i < CRASH_TRACE_MAX; i++) {
		gfx_ui_text(x + 12, y, hex8(r->trace[i], hx), 0xb0c0d0);
		if (i == 0) gfx_ui_text(x + 12 + 90, y, "<- fault", 0x707880);
		y += lh;
	}
}

/* ------------------------------------------------------------- cursor pick */
enum { RZ_L = 1, RZ_R = 2, RZ_T = 4, RZ_B = 8 };

static int hover_resizing;
static int rsz_edge_hint;

static int resize_edge(const struct winrec *w, int mx, int my)
{
	if (is_maxed(w->id) >= 0) return 0;
	int e = 0;
	if (mx < w->x + WM_EDGE)              e |= RZ_L;
	else if (mx >= w->x + w->w - WM_EDGE) e |= RZ_R;
	if (my < w->y + WM_EDGE)              e |= RZ_T;
	else if (my >= w->y + w->h - WM_EDGE) e |= RZ_B;
	return e;
}
static int edge_cursor(int e)
{
	switch (e) {
	case RZ_L: case RZ_R:               return CUR_RESIZE_EW;
	case RZ_T: case RZ_B:               return CUR_RESIZE_NS;
	case RZ_L | RZ_T: case RZ_R | RZ_B: return CUR_RESIZE_NWSE;
	case RZ_R | RZ_T: case RZ_L | RZ_B: return CUR_RESIZE_NESW;
	}
	return CUR_ARROW;
}
static int pick_cursor(int mx, int my)
{
	if (hover_resizing) return edge_cursor(rsz_edge_hint ? rsz_edge_hint : (RZ_R | RZ_B));

	if (tb_menu_open && mx >= tb_menu_x && mx < tb_menu_x + TBM_W &&
	    my >= tb_menu_y && my < tb_menu_y + tbm_h())
		return CUR_HAND;
	int bar_y = screen_h - WM_BAR_H;
	if (my >= bar_y) {
		if (mx >= 0 && mx < START_W) return CUR_HAND;
		return taskbar_hit(mx, my) >= 0 ? CUR_HAND : CUR_ARROW;
	}
	if (start_open && mx >= 0 && mx < SM_W && my >= sm_top()) return CUR_HAND;
	int idx = window_at(mx, my);
	if (idx >= 0) {
		struct winrec *w = &cur[idx];
		if (titlebtn_hit(w, mx, my) >= 0) return CUR_HAND;
		if (mx >= w->x + w->w - WM_GRIP && mx < w->x + w->w &&
		    my >= w->y + w->h - WM_GRIP && my < w->y + w->h)
			return CUR_RESIZE_NWSE;
		int e = resize_edge(w, mx, my);
		if (e) return edge_cursor(e);
	}
	return CUR_ARROW;
}

/* ------------------------------------------------------------- compose */
static void compose(int mx, int my)
{
	draw_background();
	int top = topmost_visible();
	for (int i = 0; i < nwin; i++)
		if (win_visible(&cur[i])) draw_window(&cur[i], i == top);
	draw_taskbar();
	if (start_open) draw_start_menu();
	if (tb_menu_open) draw_tb_menu();
	for (int i = 0; i < MAXCRASHWIN; i++)
		if (crashwins[i].used) draw_crashwin(&crashwins[i]);
	if (at_active) draw_switcher();
	draw_cursor(pick_cursor(mx, my), mx, my);
}

/* ------------------------------------------------------------- input */
static void deliver_client(int slot, unsigned type, int mx, int my)
{
	struct winrec *w = &cur[slot];
	struct winevent ev = { type, mx - (w->x + WM_BORDER), my - (w->y + WM_TITLE_H) };
	win_deliver(w->id, &ev);
}

/* Live-drag / resize state (tracked by stable window id, since cur[] slots move
 * as z-order changes between frames). */
static int drag_id = -1, resize_id = -1;
static int drag_dx, drag_dy;
static int rsz_edge;
static int rsz_ox, rsz_oy, rsz_ow, rsz_oh, rsz_mx0, rsz_my0;
static int app_drag_id = -1;

/* Maximize/restore tracked in the compositor (there is no cross-process maximize
 * primitive): a window id is "maximized" while it fills the work area; the entry
 * remembers its pre-maximize frame so a second click restores it. */
struct maxrec { int id, used, sx, sy, sw, sh; };
static struct maxrec maxed[WM_LIST_MAX];
static int is_maxed(int id)
{
	for (int i = 0; i < WM_LIST_MAX; i++) if (maxed[i].used && maxed[i].id == id) return i;
	return -1;
}
static void maxed_forget_gone(void)
{
	for (int i = 0; i < WM_LIST_MAX; i++)
		if (maxed[i].used && slot_of(maxed[i].id) < 0) maxed[i].used = 0;
}
static void toggle_maximize(int slot)
{
	struct winrec *w = &cur[slot];
	int m = is_maxed(w->id);
	if (m >= 0) {                          /* restore */
		win_set_geom(w->id, maxed[m].sx, maxed[m].sy, maxed[m].sw, maxed[m].sh);
		maxed[m].used = 0;
	} else {                               /* maximize: fill the work area */
		for (int i = 0; i < WM_LIST_MAX; i++) if (!maxed[i].used) {
			maxed[i].used = 1; maxed[i].id = w->id;
			maxed[i].sx = w->x; maxed[i].sy = w->y;
			maxed[i].sw = w->w; maxed[i].sh = w->h;
			break;
		}
		win_set_geom(w->id, 0, 0, screen_w, screen_h - WM_BAR_H);
	}
}

static void handle_input(int mx, int my, int buttons, int dz)
{
	static int prev_down, prev_rdown;
	int down = buttons & 1;
	int rdown = buttons & 2;

	/* Wheel -> focused window under cursor. */
	if (dz) {
		int idx = window_at(mx, my);
		if (idx >= 0) { struct winevent ev = { WEV_SCROLL, dz, 0 }; win_deliver(cur[idx].id, &ev); }
	}

	/* Right-press. */
	if (rdown && !prev_rdown) {
		int bar_y = screen_h - WM_BAR_H;
		int tbw = (my >= bar_y) ? taskbar_hit(mx, my) : -1;
		if (tbw >= 0) {
			tb_menu_open = 1;
			tb_menu_win = cur[tbw].id;
			start_open = 0;
			tb_menu_x = mx;
			if (tb_menu_x + TBM_W > screen_w) tb_menu_x = screen_w - TBM_W;
			if (tb_menu_x < 0) tb_menu_x = 0;
			tb_menu_y = bar_y - tbm_h();
		} else {
			tb_menu_open = 0;
			int idx = window_at(mx, my);
			if (idx >= 0) {
				raise_window(idx);
				if (my >= cur[idx].y + WM_TITLE_H)
					deliver_client(idx, WEV_RCLICK, mx, my);
			}
		}
	}
	prev_rdown = rdown;

	if (down && !prev_down) {                       /* left press */
		/* crash windows sit above everything but the menus: close box / dismiss. */
		for (int i = MAXCRASHWIN - 1; i >= 0; i--) {
			struct crashwin *c = &crashwins[i];
			if (!c->used) continue;
			if (mx >= c->x && mx < c->x + c->w && my >= c->y && my < c->y + c->h) {
				int bx = c->x + c->w - 4 - TB_BTN_W, by = c->y + 4;
				if (mx >= bx && mx < bx + TB_BTN_W && my >= by && my < by + TB_BTN_W) {
					c->used = 0;
					force_full = 1;
				}
				prev_down = down;
				return;                         /* clicks over a crash panel stop here */
			}
		}
		if (tb_menu_open) {
			int inside = mx >= tb_menu_x && mx < tb_menu_x + TBM_W &&
			             my >= tb_menu_y && my < tb_menu_y + tbm_h();
			if (inside) {
				int item = (my - tb_menu_y) / TBM_ITEM_H;
				int id = tb_menu_win;
				tb_menu_open = 0;
				int s = slot_of(id);
				if (s >= 0) {
					if (item == 0) {            /* Close: ask the app */
						struct winevent ev = { WEV_CLOSE, 0, 0 };
						win_deliver(id, &ev);
					} else {                    /* Force Kill */
						if (cur[s].owner_pid > 0) kill(cur[s].owner_pid, SIGKILL);
					}
				}
				prev_down = down;
				return;
			}
			tb_menu_open = 0;
		}
		int bar_y = screen_h - WM_BAR_H;
		if (my >= bar_y && mx >= 0 && mx < START_W) {   /* Start button */
			start_open = !start_open;
			prev_down = down;
			return;
		}
		int tbw = (my >= bar_y) ? taskbar_hit(mx, my) : -1;
		if (tbw >= 0) {
			start_open = 0;
			/* restore/minimize/raise via focus + geom (kernel owns min/max state,
			 * but a plain raise is all we can express cross-process here). */
			raise_window(tbw);
			prev_down = down;
			return;
		}
		if (start_open) {
			int top = sm_top();
			if (mx >= 0 && mx < SM_W && my >= top && my < bar_y) {
				int rel = my - top;
				if (rel < sm_apps_h()) {
					launch_app(rel / SM_ITEM_H);
					start_open = 0;
				} else {
					int prel = rel - sm_apps_h() - SM_SEP_H;
					if (prel >= 0 && prel / SM_ITEM_H < sm_power_n()) {
						start_open = 0;
						if (sm_power_label(prel / SM_ITEM_H)[0] == 'R') power(1); /* Restart */
						else power(0);                                            /* Shut Down */
					}
				}
				prev_down = down;
				return;
			}
			start_open = 0;
		}
		int idx = window_at(mx, my);
		if (idx >= 0) {
			raise_window(idx);
			struct winrec *w = &cur[idx];
			int tb = titlebtn_hit(w, mx, my);
			if (tb == 0) {                          /* close */
				struct winevent ev = { WEV_CLOSE, 0, 0 };
				win_deliver(w->id, &ev);
			} else if (tb == 1) {                   /* maximize / restore */
				toggle_maximize(idx);
			} else if (tb == 2) {                   /* minimize: no cross-process
				* minimize primitive yet (the kernel owns min state) — best effort
				* is to drop focus so another window comes forward. */
				win_set_focus(0);
			} else if (is_maxed(w->id) < 0 &&
			           ((mx >= w->x + w->w - WM_GRIP && my >= w->y + w->h - WM_GRIP)
			            || resize_edge(w, mx, my))) {
				int e = (mx >= w->x + w->w - WM_GRIP && my >= w->y + w->h - WM_GRIP)
				        ? (RZ_R | RZ_B) : resize_edge(w, mx, my);
				resize_id = w->id; rsz_edge = e;
				rsz_ox = w->x; rsz_oy = w->y; rsz_ow = w->w; rsz_oh = w->h;
				rsz_mx0 = mx; rsz_my0 = my;
			} else if (my < w->y + WM_TITLE_H) {    /* title-bar drag */
				drag_id = w->id;
				drag_dx = mx - w->x; drag_dy = my - w->y;
			} else {                                /* client press */
				deliver_client(idx, WEV_MOUSE, mx, my);
				app_drag_id = w->id;
			}
		}
	} else if (down && app_drag_id >= 0) {          /* app-captured drag */
		int s = slot_of(app_drag_id);
		if (s >= 0) deliver_client(s, WEV_DRAG, mx, my);
	} else if (down && drag_id >= 0) {              /* title drag */
		int s = slot_of(drag_id);
		if (s >= 0) {
			struct winrec *w = &cur[s];
			int nx = mx - drag_dx, ny = my - drag_dy;
			if (nx < 0) nx = 0;
			if (ny < 0) ny = 0;
			if (nx > screen_w - w->w) nx = screen_w - w->w;
			if (ny > screen_h - WM_BAR_H - WM_TITLE_H) ny = screen_h - WM_BAR_H - WM_TITLE_H;
			win_set_geom(w->id, nx, ny, w->w, w->h);
		}
	} else if (down && resize_id >= 0) {            /* resize */
		int s = slot_of(resize_id);
		if (s >= 0) {
			int dx = mx - rsz_mx0, dy = my - rsz_my0;
			int nx = rsz_ox, ny = rsz_oy, nw = rsz_ow, nh = rsz_oh;
			if (rsz_edge & RZ_R) nw = rsz_ow + dx;
			if (rsz_edge & RZ_B) nh = rsz_oh + dy;
			if (rsz_edge & RZ_L) { nw = rsz_ow - dx; nx = rsz_ox + dx; }
			if (rsz_edge & RZ_T) { nh = rsz_oh - dy; ny = rsz_oy + dy; }
			if (nw < WM_MIN_W) { if (rsz_edge & RZ_L) nx -= WM_MIN_W - nw; nw = WM_MIN_W; }
			if (nh < WM_MIN_H) { if (rsz_edge & RZ_T) ny -= WM_MIN_H - nh; nh = WM_MIN_H; }
			if (nx < 0) { nw += nx; nx = 0; }
			if (ny < 0) { nh += ny; ny = 0; }
			if (nx + nw > screen_w) nw = screen_w - nx;
			if (ny + nh > screen_h - WM_BAR_H) nh = screen_h - WM_BAR_H - ny;
			win_set_geom(cur[s].id, nx, ny, nw, nh);
		}
	} else if (!down) {                             /* release */
		drag_id = resize_id = app_drag_id = -1;
	}

	/* Hover: no button held -> WEV_MOVE to the window under the cursor. */
	static int hover_id = -1, hover_px = -1, hover_py = -1;
	if (!down && !rdown && drag_id < 0 && resize_id < 0 && app_drag_id < 0 &&
	    (mx != hover_px || my != hover_py)) {
		hover_px = mx; hover_py = my;
		int idx = window_at(mx, my);
		struct winrec *w = idx >= 0 ? &cur[idx] : 0;
		int in_client = w && my >= w->y + WM_TITLE_H && my < w->y + w->h - WM_BORDER &&
		                mx >= w->x + WM_BORDER && mx < w->x + w->w - WM_BORDER;
		if (hover_id >= 0 && (idx < 0 || cur[idx].id != hover_id || !in_client)) {
			struct winevent ev = { WEV_MOVE, -1, -1 };
			win_deliver(hover_id, &ev);
			hover_id = -1;
		}
		if (in_client) { deliver_client(idx, WEV_MOVE, mx, my); hover_id = w->id; }
	}

	hover_resizing = (resize_id >= 0);
	rsz_edge_hint = rsz_edge;
	prev_down = down;
}

/* ---- global key routing (Super -> Start menu; Alt+Tab -> switcher; else the
 * focused window). Input keys arrive as {c, mods}. ---- */

/* Clipboard-history viewer (Super+V): launch /bin/cliphist, then on its exit-0
 * (a clip was picked & promoted to the front) paste it into whatever window now
 * holds focus — the same path Ctrl+V uses (paste_begin, below). */
static void paste_begin(void);
static int cliphist_pid;
static void open_cliphist(void)
{
	if (cliphist_pid > 0) return;          /* one at a time */
	cliphist_pid = spawn("/bin/cliphist", 0);
	/* Reaped exclusively by cliphist_reap (which needs its exit status), so it is
	 * deliberately NOT added to kids[]. */
}
static void cliphist_reap(void)
{
	if (cliphist_pid <= 0) return;
	int st;
	if (waitpid_nb(cliphist_pid, &st)) {
		int picked = (st == 0);
		cliphist_pid = 0;
		if (picked) paste_begin();          /* paste the promoted clip into focus */
	}
}

/* Paste back-pressure: keys that couldn't fit the focused window's event ring
 * this frame wait here and are re-offered next frame before fresh input is
 * pulled, so a large paste never overruns a slow app's evq (win_key_space). */
#define PENDING_MAX 256
static struct { int c, mods; } pending[PENDING_MAX];
static int pend_head, pend_tail;
static int pend_empty(void) { return pend_head == pend_tail; }
static void pend_push(int c, int mods)
{
	int next = (pend_head + 1) % PENDING_MAX;
	if (next == pend_tail) return;         /* full: drop (paste larger than 255 backlog) */
	pending[pend_head].c = c; pending[pend_head].mods = mods; pend_head = next;
}

static void feed_key(int c, int mods)
{
	if (c == KEY_CLIPHIST) { open_cliphist(); return; }
	if (c == 'v' && (mods & KMOD_CTRL)) { paste_begin(); return; }
	if (c == KEY_SUPER) { start_open = !start_open; return; }
	if (c == KEY_TAB && (mods & KMOD_ALT)) {
		if (!at_active) {
			at_count = alttab_build();
			if (at_count <= 1) { at_count = 0; return; }
			at_active = 1; at_sel = 0;
		}
		if (mods & KMOD_SHIFT) at_sel = (at_sel - 1 + at_count) % at_count;
		else                   at_sel = (at_sel + 1) % at_count;
		return;
	}
	/* We don't get an "Alt released" event from input_poll; commit the switch when
	 * a non-Tab key arrives while the overlay is up, or on any key without Alt. */
	if (at_active && !(mods & KMOD_ALT)) {
		int slot = at_order[at_sel];
		at_active = 0;
		if (slot >= 0 && slot < nwin) raise_window(slot);
		return;
	}
	int t = topmost_visible();
	if (t < 0) return;
	/* Flow control: if the focused window's event ring is full, park the key and
	 * retry next frame rather than letting win_deliver silently drop it. */
	if (win_key_space(cur[t].id) <= 0) { pend_push(c, mods); return; }
	struct winevent ev = { WEV_KEY, c, mods };
	win_deliver(cur[t].id, &ev);
}

/* Drain parked keys before pulling fresh input; stop as soon as the target fills
 * again (a key that can't be delivered is re-parked at the queue front by feed_key). */
static void pending_flush(void)
{
	while (!pend_empty()) {
		int c = pending[pend_tail].c, mods = pending[pend_tail].mods;
		int t = topmost_visible();
		if (t < 0) { pend_tail = pend_head; return; }   /* nothing focused: discard */
		if (win_key_space(cur[t].id) <= 0) return;      /* still full: try next frame */
		pend_tail = (pend_tail + 1) % PENDING_MAX;
		struct winevent ev = { WEV_KEY, c, mods };
		win_deliver(cur[t].id, &ev);
	}
}

/* Clipboard paste (Ctrl+V, or a Super+V history pick). The clipboard lives in
 * /bin/clipd; we pull the newest clip and, if it's text, inject it into the
 * focused window as key events — CRLF/CR folded to LF (so a multi-line paste
 * submits each line), tabs and newlines kept, other control bytes dropped — so an
 * app sees exactly the bytes it would if typed. Held here and drained a few keys
 * per frame (win_key_space back-pressure, like pending[]) so a large paste can't
 * overrun a slow app's event ring. */
#define PASTE_MAX 8192
static char paste_buf[PASTE_MAX];
static int  paste_len, paste_pos;

static void paste_begin(void)
{
	if (paste_pos < paste_len) return;          /* a paste is still draining */
	char mime[32];
	void *data = 0;
	int len = 0;
	if (clip_get(0, mime, sizeof mime, &data, &len) < 0) return;
	int out = 0;
	if (mime[0]=='t' && mime[1]=='e' && mime[2]=='x' && mime[3]=='t' && mime[4]=='/') {
		const char *raw = data;
		for (int i = 0; i < len && out < PASTE_MAX; i++) {
			char c = raw[i];
			if (c == '\r') {                    /* fold CR and CRLF down to one LF */
				if (i + 1 < len && raw[i + 1] == '\n') i++;
				c = '\n';
			}
			if (c != '\n' && c != '\t' && (unsigned char)c < 0x20)
				continue;                       /* drop stray control bytes */
			paste_buf[out++] = c;
		}
	}
	free(data);
	paste_pos = 0;
	paste_len = out;                            /* publish last */
}

/* Drain the pending paste into the focused window; stop when its event ring fills
 * (resumes next frame) or focus is lost (discard the rest). */
static void paste_flush(void)
{
	while (paste_pos < paste_len) {
		int t = topmost_visible();
		if (t < 0) { paste_pos = paste_len; return; }   /* nothing focused: discard */
		if (win_key_space(cur[t].id) <= 0) return;      /* full: try next frame */
		struct winevent ev = { WEV_KEY, (unsigned char)paste_buf[paste_pos++], 0 };
		win_deliver(cur[t].id, &ev);
	}
}
#define MAXDMG 16
struct rect { int x, y, w, h; };
static struct rect dmg[MAXDMG];
static int ndmg;
static void damage_add(int x, int y, int w, int h)
{
	if (w > 0 && h > 0 && ndmg < MAXDMG) {
		dmg[ndmg].x = x; dmg[ndmg].y = y; dmg[ndmg].w = w; dmg[ndmg].h = h;
		ndmg++;
	}
}
static void damage_add_union(int x0, int y0, int w0, int h0,
                             int x1, int y1, int w1, int h1)
{
	int ax0 = x0 < x1 ? x0 : x1, ay0 = y0 < y1 ? y0 : y1;
	int ax1 = (x0 + w0) > (x1 + w1) ? (x0 + w0) : (x1 + w1);
	int ay1 = (y0 + h0) > (y1 + h1) ? (y0 + h0) : (y1 + h1);
	damage_add(ax0, ay0, ax1 - ax0, ay1 - ay0);
}

static void present_rect(int x, int y, int w, int h)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > screen_w) w = screen_w - x;
	if (y + h > screen_h) h = screen_h - y;
	if (w > 0 && h > 0) fb_present_rect(x, y, w, h);
}

/* ------------------------------------------------------------- prev-frame snapshot */
struct pfsnap { int id, x, y, w, h, vis, min; unsigned seq; };
static struct pfsnap pf[WM_LIST_MAX];
static int npf;

static struct pfsnap *pf_find(int id)
{
	for (int i = 0; i < npf; i++) if (pf[i].id == id) return &pf[i];
	return 0;
}

/* (Re)bind the compositor to the framebuffer's current geometry: remap the back
 * buffer (idempotent; same vaddr, picks up any realloc), re-init the gfx surface
 * with the new stride, update the cursor bounds, and force a full repaint. The
 * kernel's fb_set_mode keeps w*h <= the capacity the buffer was allocated at, so
 * the mapped region always covers the new mode. */
static int bind_mode(void)
{
	struct fbinfo fi;
	if (fb_info(&fi) < 0) return -1;
	back = fb_map();
	if (!back) return -1;
	screen_w = fi.w; screen_h = fi.h; fb_pitch = fi.pitch;
	gfx_init(back, fb_pitch, screen_h);        /* surface stride = pitch */
	input_set_bounds(screen_w - 1, screen_h - 1);
	clip_full();
	force_full = 1;
	return 0;
}

/* A Settings-app resolution change reprograms fb_set_mode in the kernel; detect
 * it by re-querying fb_info each frame. On change, rebind the surface and pull any
 * window that now hangs off the smaller screen back into view (a maximized window
 * is re-filled to the new work area). Cheap: one syscall/frame, geometry work only
 * on an actual change. */
static void check_resolution(void)
{
	struct fbinfo fi;
	if (fb_info(&fi) < 0) return;
	if (fi.w == screen_w && fi.h == screen_h && fi.pitch == fb_pitch) return;

	bind_mode();
	for (int i = 0; i < nwin; i++) {
		struct winrec *w = &cur[i];
		int m = is_maxed(w->id);
		if (m >= 0) { win_set_geom(w->id, 0, 0, screen_w, screen_h - WM_BAR_H); continue; }
		int nx = w->x, ny = w->y, nw = w->w, nh = w->h;
		if (nw > screen_w) nw = screen_w;
		if (nh > screen_h - WM_BAR_H) nh = screen_h - WM_BAR_H;
		if (nx + nw > screen_w) nx = screen_w - nw;
		if (ny + nh > screen_h - WM_BAR_H) ny = screen_h - WM_BAR_H - nh;
		if (nx < 0) nx = 0;
		if (ny < 0) ny = 0;
		if (nx != w->x || ny != w->y || nw != w->w || nh != w->h)
			win_set_geom(w->id, nx, ny, nw, nh);
	}
}

int main(void)
{
	if (getuid() != 0) { puts("compositor: must run as root\n"); return 1; }

	if (bind_mode() < 0) { puts("compositor: no framebuffer\n"); return 1; }

	/* Desktop session autostart (was the kernel WM's apps_start): the clipboard
	 * server (+ its VMware host bridge), the notification daemon, then the terminal
	 * — so a fresh desktop comes up with a terminal open and copy/paste +
	 * notifications working. */
	kid_track(spawn("/bin/clipd", 0));
	kid_track(spawn("/bin/vmclipd", 0));    /* VMware host clipboard bridge; self-exits off VMware */
	kid_track(spawn("/bin/notifyd", 0));
	kid_track(spawn("/bin/term", 0));

	int prev_mx = -9999, prev_my = -9999, prev_cur = -1;
	int prev_start = 0, prev_tb_menu = 0, prev_at_active = 0, prev_at_sel = -1;
	int prev_focus = -1, last_sec = -1, inited = 0;

	for (;;) {
		struct inputstate in;
		if (input_poll(&in) < 0) { msleep(16); continue; }

		cliphist_reap();                /* Super+V viewer exited -> maybe paste */
		reap_kids();
		crash_ingest();

		/* Refresh the window set, then (re)pin client buffers to match it. */
		nwin = win_enum(cur, WM_LIST_MAX);
		if (nwin < 0) nwin = 0;
		refresh_maps();                 /* maps must be current before compose */

		check_resolution();             /* Settings may have changed the fb mode */

		/* Drain any keys parked by paste back-pressure before pulling new input. */
		pending_flush();
		paste_flush();                  /* inject any in-progress clipboard paste */

		/* Keys first (may open menus / switch focus). */
		for (int i = 0; i < in.nkeys; i++)
			feed_key(in.keys[i].c, in.keys[i].mods);

		static int prev_wheel = 0;
		int dz = in.wheel - prev_wheel;   /* wheel is a running total; delta = this frame */
		prev_wheel = in.wheel;

		handle_input(in.mx, in.my, in.buttons, dz);

		int mx = in.mx, my = in.my;
		ndmg = 0;
		int full = !inited || force_full;
		force_full = 0;

		/* Focus / z-order / visibility / add / remove change -> full repaint. */
		if (!full) {
			int focus = topmost_visible();
			int focus_id = focus >= 0 ? cur[focus].id : -1;
			int zvis = (focus_id != prev_focus);
			if (nwin != npf) zvis = 1;              /* window added or removed */
			for (int i = 0; i < nwin && !zvis; i++) {
				struct pfsnap *p = pf_find(cur[i].id);
				if (!p) { zvis = 1; break; }         /* a new window            */
				if (pf[i].id != cur[i].id) zvis = 1; /* z-order reordered       */
				if (p->vis != win_visible(&cur[i]) ||
				    p->min != win_minimized(&cur[i]))
					zvis = 1;                        /* shown/hidden/minimized  */
			}
			if (zvis) full = 1;
			prev_focus = focus_id;

			if (at_active != prev_at_active || at_sel != prev_at_sel) full = 1;
			prev_at_active = at_active; prev_at_sel = at_sel;
			if (tb_menu_open != prev_tb_menu) full = 1;
			prev_tb_menu = tb_menu_open;

			/* moved/resized windows: old ∪ new as one region */
			for (int i = 0; i < nwin; i++) {
				struct pfsnap *p = pf_find(cur[i].id);
				if (p && (p->x != cur[i].x || p->y != cur[i].y ||
				          p->w != cur[i].w || p->h != cur[i].h))
					damage_add_union(p->x, p->y, p->w, p->h,
					                 cur[i].x, cur[i].y, cur[i].w, cur[i].h);
			}
			/* cursor moved/changed/animating */
			int cur_kind = pick_cursor(mx, my);
			if (mx != prev_mx || my != prev_my || cur_kind != prev_cur ||
			    cur_kind == CUR_BUSY) {
				damage_add(prev_mx - CUR_DMG_PAD, prev_my - CUR_DMG_PAD, CUR_DMG_W, CUR_DMG_H);
				damage_add(mx - CUR_DMG_PAD, my - CUR_DMG_PAD, CUR_DMG_W, CUR_DMG_H);
			}
			prev_cur = cur_kind;
			/* content changed (present_seq) */
			for (int i = 0; i < nwin; i++) {
				struct pfsnap *p = pf_find(cur[i].id);
				if (win_visible(&cur[i]) && p && p->seq != cur[i].present_seq)
					damage_add(cur[i].x, cur[i].y, cur[i].w, cur[i].h);
			}
			/* clock tick */
			int sec = (int)(now_hms() & 0xFF);
			if (sec != last_sec) damage_add(0, screen_h - WM_BAR_H, screen_w, WM_BAR_H);
			/* Start menu toggle */
			if (start_open != prev_start) damage_add(0, sm_top(), SM_W, sm_h() + WM_BAR_H);
			/* any crash window forces full (rare) */
			for (int i = 0; i < MAXCRASHWIN; i++) if (crashwins[i].used) full = 1;
		}

		if (full) {
			clip_full();
			compose(mx, my);
			present_rect(0, 0, screen_w, screen_h);
		} else if (ndmg > 0) {
			for (int k = 0; k < ndmg; k++) {
				set_clip(dmg[k].x, dmg[k].y, dmg[k].w, dmg[k].h);
				compose(mx, my);
				present_rect(dmg[k].x, dmg[k].y, dmg[k].w, dmg[k].h);
			}
			clip_full();
		}

		/* snapshot for next frame */
		npf = 0;
		for (int i = 0; i < nwin && npf < WM_LIST_MAX; i++) {
			pf[npf].id = cur[i].id;
			pf[npf].x = cur[i].x; pf[npf].y = cur[i].y;
			pf[npf].w = cur[i].w; pf[npf].h = cur[i].h;
			pf[npf].vis = win_visible(&cur[i]); pf[npf].min = win_minimized(&cur[i]);
			pf[npf].seq = cur[i].present_seq;
			npf++;
		}
		prev_mx = mx; prev_my = my;
		prev_start = start_open;
		last_sec = (int)(now_hms() & 0xFF);
		inited = 1;
		cursor_frame++;

		msleep(16);   /* ~60 fps pacing; idle when static */
	}
}
