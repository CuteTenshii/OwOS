/* files.c — a navigable file explorer for owofs.
 *
 * Ports the former kernel app_files.c: a top "Go" menu bar (mirroring the
 * editor's File menu), a left "Places" sidebar, plus a path header and
 * scrollable listing. Directory contents come from the readdir syscall; the
 * window's pixel buffer is drawn with the gfx lib. files_seq bumps on every
 * navigation/scroll/select/menu action to trigger a repaint.
 */
#include "owos.h"
#include "gfx.h"

static int W = 320, H = 380;    /* client size; changes on resize */
#define MENU_H   18             /* top menu bar (above the sidebar + list) */
#define HEADER_H 18
#define ROW_H    16
#define FMAX     64
#define SIDEBAR_W   96   /* default sidebar width (a customizable property)   */
#define SIDEBAR_MIN 64   /* narrowest the divider drag may make it            */
#define SIDEBAR_MAX 220  /* widest                                            */
#define LIST_TOP (MENU_H + HEADER_H)   /* y of the first places/list row */

/* The left navigation pane is a gfx sidebar widget: it owns its width as a
 * resizable property (drag the divider, clamped to [MIN,MAX]); we draw our own
 * Places/Devices rows into it. */
static struct sidebar sb;

struct fent { char name[20]; int is_dir; unsigned size; };

static char        path[256] = "/";
static int         scroll, sel = -1;
static unsigned    seq;
static struct fent fent_buf[FMAX];
static int         fent_count;
static char        list_path[256];   /* path the cached listing reflects */
static int         force_relist;     /* set by F5/Refresh to re-read in place */

/* The single drop-down menu ("Go"), mirroring the editor's File menu: a top
 * bar with mouse-driven items, two of them carrying keyboard accelerators
 * (F5 refresh, Ctrl+O open) wired through menu_shortcut. */
static const char *const go_items[] = { "Up", "Home", "Refresh", "Open" };
static const char *const go_hints[] = { "", "", "F5", "Ctrl+O" };
static const unsigned char go_keys[] = { 0, 0, KEY_F5, KEY_OPEN };
static struct menu gomenu = { "Go", go_items, 4, 0, go_hints, go_keys };

static int  flen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void fcpy(char *d, const char *s) { int i = 0; while ((d[i] = s[i])) i++; }
static int  feq(const char *a, const char *b)
{ int i = 0; while (a[i] && a[i] == b[i]) i++; return a[i] == b[i]; }

static void u32str(unsigned v, char *out)
{
	char t[12];
	int n = 0;
	if (v == 0) { out[0] = '0'; out[1] = 0; return; }
	while (v) { t[n++] = '0' + v % 10; v /= 10; }
	int i = 0;
	while (n) out[i++] = t[--n];
	out[i] = 0;
}

/* Format a byte count as "N B", or "N.N KiB/MiB/GiB/TiB" scaled to the largest
 * unit that keeps the whole part under 1024. Integer-only (no 64-bit / float):
 * the fractional digit comes from the leftover low bits below the chosen unit. */
static void size_str(unsigned v, char *out)
{
	static const char *const unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	int u = 0;
	unsigned whole = v;
	while (whole >= 1024 && u < 4) { whole >>= 10; u++; }

	char t[12];
	u32str(whole, t);
	int i = 0;
	for (int j = 0; t[j]; j++) out[i++] = t[j];

	if (u > 0) {                       /* one fractional digit for scaled units */
		unsigned rem = v & ((1u << (10 * u)) - 1);
		out[i++] = '.';
		out[i++] = '0' + (((rem >> (10 * (u - 1))) * 10) >> 10);
	}
	out[i++] = ' ';
	for (const char *p = unit[u]; *p; p++) out[i++] = *p;
	out[i] = 0;
}

static int collect(void)
{
	int n = 0;
	struct dirent_u d;
	if (!feq(path, "/")) {
		fcpy(fent_buf[n].name, "..");
		fent_buf[n].is_dir = 1; fent_buf[n].size = 0; n++;
	}
	for (int i = 0; n < FMAX && readdir(path, i, &d) == 1; i++)
		if (d.is_dir) {
			fcpy(fent_buf[n].name, d.name);
			fent_buf[n].is_dir = 1; fent_buf[n].size = 0; n++;
		}
	for (int i = 0; n < FMAX && readdir(path, i, &d) == 1; i++)
		if (!d.is_dir) {
			fcpy(fent_buf[n].name, d.name);
			fent_buf[n].is_dir = 0; fent_buf[n].size = d.size; n++;
		}
	return n;
}

/* Re-read the directory only when the path changed or an explicit refresh was
 * asked for — NOT on every repaint. A resize bumps `seq` each frame, and on a
 * removable device re-listing per frame is real block I/O (visible lag). */
static void refresh(void)
{
	if (!force_relist && feq(list_path, path))
		return;
	fent_count = collect();
	fcpy(list_path, path);
	force_relist = 0;
}

static void go_up(void)
{
	int n = flen(path);
	if (n <= 1) return;
	int i = n - 1;
	while (i > 0 && path[i] != '/') i--;
	if (i == 0) { path[0] = '/'; path[1] = 0; }
	else        { path[i] = 0; }
}

static void enter(const char *name)
{
	int n = flen(path);
	int base = (n == 1) ? 1 : (path[n] = '/', n + 1);
	if (base + flen(name) >= (int)sizeof(path) - 1) return;
	int i = 0;
	while (name[i]) { path[base + i] = name[i]; i++; }
	path[base + i] = 0;
}

static const struct { const char *label, *path; } places[] = {
	{ "Root", "/" }, { "bin", "/bin" }, { "home", "/home" }, { "root", "/home/root" },
};
#define NPLACES (int)(sizeof(places) / sizeof(places[0]))

/* Removable volumes (USB sticks, etc.), refreshed from the kernel each paint so
 * the list reflects plug/eject in real time. */
static struct volinfo vols[6];
static int            nvols;
static void scan_vols(void)
{
	nvols = 0;
	while (nvols < 6 && volumes(nvols, &vols[nvols]) == 1)
		nvols++;
}

/* 1 if `p` is `base` or sits inside it (so a device stays highlighted while you
 * browse into it). */
static int under_path(const char *p, const char *base)
{
	int i = 0;
	while (base[i] && base[i] == p[i]) i++;
	if (base[i]) return 0;
	return p[i] == '\0' || p[i] == '/';
}

/* Sidebar geometry for the Devices section (below Places, after a small gap). */
static int dev_header_y(void) { return LIST_TOP + NPLACES * ROW_H + 6; }
static int dev_row_y(int i)   { return dev_header_y() + ROW_H + i * ROW_H; }
#define EJECT_W 14   /* right-edge hit zone (and icon) for the eject button */

/* A tiny eject glyph: a triangle over a tray bar, drawn in a ~7x9 box. */
static void draw_eject(int x, int y, unsigned c)
{
	gfx_fill_rect(x + 3, y + 3, 1, 1, c);
	gfx_fill_rect(x + 2, y + 4, 3, 1, c);
	gfx_fill_rect(x + 1, y + 5, 5, 1, c);
	gfx_fill_rect(x + 1, y + 7, 5, 1, c);
}

/* A "connect" glyph: a down-arrow into a tray bar (insert), the mirror of eject.
 * Marks a present-but-unmounted device; clicking it mounts the device. */
static void draw_connect(int x, int y, unsigned c)
{
	gfx_fill_rect(x + 1, y + 3, 5, 1, c);
	gfx_fill_rect(x + 2, y + 4, 3, 1, c);
	gfx_fill_rect(x + 3, y + 5, 1, 1, c);
	gfx_fill_rect(x + 1, y + 7, 5, 1, c);
}

/* The width actually rendered this frame: the widget's property width, capped so
 * the file list always keeps ~80px (a narrow window can't be all sidebar). The
 * property itself (sb.w) is unchanged — only the on-screen pane is clamped. */
static int sidebar_w(void)
{
	int sbw = sb.w;
	if (sbw > W - 80) sbw = W - 80;
	if (sbw < 0) sbw = 0;
	return sbw;
}

static void set_title(int id)
{
	char t[64];
	const char *pre = "files  ";
	int i = 0;
	while (pre[i]) { t[i] = pre[i]; i++; }
	for (int j = 0; path[j] && i < 63; j++) t[i++] = path[j];
	t[i] = 0;
	win_settitle(id, t);
}

static void goto_path(const char *p)
{
	int i = 0;
	while (p[i] && i < (int)sizeof(path) - 1) { path[i] = p[i]; i++; }
	path[i] = 0;
	scroll = 0; sel = -1;
}

static void ctx_draw(void);   /* right-click popup, painted last (see below) */

static void render(void)
{
	refresh();
	int sbw = sidebar_w();

	/* Sidebar: the widget paints the panel base, the right divider and the
	 * resize grip; we overdraw the header strip and the Places/Devices rows. */
	sidebar_draw(&sb, 0, 0, sbw, H, 0x0e1218, 0x05070a);
	gfx_fill_rect(0, MENU_H, sbw, HEADER_H, 0x161d28);
	gfx_ui_text(6, MENU_H + 1, "Places", 0x8aa0b8);
	for (int i = 0; i < NPLACES; i++) {
		int ry = LIST_TOP + i * ROW_H;
		int here = feq(path, places[i].path);
		unsigned rbg = here ? 0x24405c : 0x0e1218;
		if (here) gfx_fill_rect(0, ry, sbw, ROW_H, rbg);
		gfx_ui_text(6, ry, places[i].label, here ? 0xffffff : 0xc8d0dc);
	}

	/* Devices: every present removable device. Mounted ones are navigable and
	 * carry an eject button; unmounted ones read dimmer and carry a connect
	 * (mount) button. */
	scan_vols();
	if (nvols > 0) {
		gfx_ui_text(6, dev_header_y(), "Devices", 0x8aa0b8);
		for (int i = 0; i < nvols; i++) {
			int ry = dev_row_y(i);
			int mounted = vols[i].mounted;
			int here = mounted && under_path(path, vols[i].point);
			unsigned rbg = here ? 0x24405c : 0x0e1218;
			if (here) gfx_fill_rect(0, ry, sbw, ROW_H, rbg);
			unsigned fg = here ? 0xffffff : (mounted ? 0xc8d0dc : 0x8a96a4);
			gfx_ui_text(6, ry, vols[i].label, fg);
			if (mounted) draw_eject(sbw - EJECT_W + 2, ry + 3, 0x8aa0b8);
			else         draw_connect(sbw - EJECT_W + 2, ry + 3, 0x8aa0b8);
		}
	}

	/* File list. */
	int lx = sbw + 1, lw = W - sbw - 1;
	int vis = (H - LIST_TOP) / ROW_H;
	if (vis < 1) vis = 1;
	if (scroll > fent_count - vis) scroll = fent_count - vis;
	if (scroll < 0) scroll = 0;

	gfx_fill_rect(lx, 0, lw, H, 0x12161e);
	gfx_fill_rect(lx, MENU_H, lw, HEADER_H, 0x1d2430);
	int avail = (lw - 40) / 8;
	if (avail < 4) avail = 4;
	if (avail > 90) avail = 90;
	char pbuf[96];
	int pl = flen(path);
	if (pl <= avail) fcpy(pbuf, path);
	else { pbuf[0] = pbuf[1] = pbuf[2] = '.'; fcpy(pbuf + 3, path + (pl - (avail - 3))); }
	gfx_ui_text(lx + 4, MENU_H + 1, pbuf, 0x9ad0ff);
	gfx_glyph(lx + lw - 32, MENU_H + 1, 0x1E, 0xffffff, 0x1d2430);
	gfx_glyph(lx + lw - 16, MENU_H + 1, 0x1F, 0xffffff, 0x1d2430);

	for (int r = 0; r < vis; r++) {
		int e = scroll + r;
		if (e >= fent_count) break;
		struct fent *fe = &fent_buf[e];
		int ry = LIST_TOP + r * ROW_H;
		unsigned rbg = (e == sel) ? 0x2f6fb0 : 0x12161e;
		if (e == sel) gfx_fill_rect(lx, ry, lw, ROW_H, rbg);
		unsigned col = fe->is_dir ? 0xffd479 : 0xc8d0dc;
		gfx_ui_text(lx + 4, ry, fe->name, col);
		if (fe->is_dir && !feq(fe->name, ".."))
			gfx_ui_text(lx + 4 + gfx_ui_width(fe->name), ry, "/", col);
		if (!fe->is_dir) {
			char sb[16];
			size_str(fe->size, sb);
			gfx_ui_text(lx + lw - 4 - gfx_ui_width(sb), ry, sb, 0x8aa0b8);
		}
	}

	/* Top menu bar over the sidebar/list, then the dropdown drawn last so it
	 * overlays the listing. */
	gfx_fill_rect(0, 0, W, MENU_H, 0x232a36);
	menu_draw(&gomenu, 2, 0);

	ctx_draw();   /* right-click popup overlays everything */
}

/* Build the absolute path of `name` under the current directory into out. */
static void full_path(const char *name, char *out, int max)
{
	int i = 0;
	while (i < max - 1 && path[i]) { out[i] = path[i]; i++; }
	if (flen(path) != 1 && i < max - 1) out[i++] = '/';   /* "/" needs no sep */
	int j = 0;
	while (i < max - 1 && name[j]) out[i++] = name[j++];
	out[i] = 0;
}

/* File-type associations: a filename extension picks the /bin program that
 * opens it; anything unlisted falls back to the editor. Add a row to extend it
 * (e.g. another image type -> "imgview"). */
static const struct { const char *ext; const char *prog; } assoc[] = {
	{ "png", "imgview" },
};

/* Case-insensitive compare of `a` against the lowercase literal `b`. */
static int ext_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char c = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		if (c != *b)
			return 0;
	}
	return *a == '\0' && *b == '\0';
}

/* The program to open `name` with: by its extension, else the editor. */
static const char *app_for(const char *name)
{
	const char *dot = 0;
	for (const char *p = name; *p; p++)
		if (*p == '.')
			dot = p;
	if (dot)
		for (unsigned i = 0; i < sizeof assoc / sizeof assoc[0]; i++)
			if (ext_eq(dot + 1, assoc[i].ext))
				return assoc[i].prog;
	return "editor";
}

/* Open a file by launching its associated program on it. Fork so this explorer
 * survives; the child replaces its image with the program, passing the path as
 * argv[1]. waitpid keeps the child off the zombie list (the shell's outer wait
 * loop still pumps keystrokes to whichever window holds focus, so the opened app
 * stays interactive while we block). */
static void open_with(const char *name, const char *prog)
{
	char fp[256];
	full_path(name, fp, sizeof fp);
	int pid = fork();
	if (pid == 0) {
		execp1(prog, fp);
		exit(1);                     /* exec failed: don't fall back into UI */
	}
	if (pid > 0)
		waitpid(pid);
}

static void open_file(const char *name) { open_with(name, app_for(name)); }

static int list_hit(int mx, int my);   /* list row under (mx,my), or -1 */

/* ----------------------------------------------------------- context menu */
/*
 * A right-click popup whose items depend on what sits under the cursor: a file,
 * a directory, a Places shortcut, a removable device, or empty space. It's a
 * self-contained popup (not a `struct menu`, which is anchored to a bar title):
 * ctx_open() captures the target and clamps the popup inside the window,
 * ctx_draw() paints it last so it overlays the listing, and ctx_click() routes a
 * left-press to an item or dismisses the menu.
 */
enum ctx_kind { CTX_BG, CTX_FILE, CTX_DIR, CTX_PLACE, CTX_DEVICE };
enum ctx_act  { A_OPEN, A_EDIT, A_EJECT, A_CONNECT, A_REFRESH, A_UP, A_HOME };

struct ctx_item { const char *label; int act; };
static const struct ctx_item ci_file[]   = { {"Open", A_OPEN}, {"Edit", A_EDIT} };
static const struct ctx_item ci_dir[]     = { {"Open", A_OPEN} };
static const struct ctx_item ci_place[]   = { {"Open", A_OPEN} };
static const struct ctx_item ci_device[]  = { {"Open", A_OPEN}, {"Unmount", A_EJECT} };
static const struct ctx_item ci_device_off[] = { {"Connect", A_CONNECT} };
static const struct ctx_item ci_bg[]      = { {"Refresh", A_REFRESH},
                                              {"Go Up", A_UP}, {"Home", A_HOME} };

static struct {
	int  open;
	int  x, y;                 /* popup top-left (clamped into the window) */
	enum ctx_kind kind;
	int  target;               /* place / device index                    */
	char name[20];             /* file / dir entry name                   */
} ctx;

#define CTX_W   96
#define CTX_ROW 16

/* The item table + count for the menu's current kind. */
static const struct ctx_item *ctx_items(int *n)
{
	switch (ctx.kind) {
	case CTX_FILE:   *n = 2; return ci_file;
	case CTX_DIR:    *n = 1; return ci_dir;
	case CTX_PLACE:  *n = 1; return ci_place;
	case CTX_DEVICE:
		if (ctx.target >= 0 && ctx.target < nvols && !vols[ctx.target].mounted) {
			*n = 1; return ci_device_off;
		}
		*n = 2; return ci_device;
	default:         *n = 3; return ci_bg;
	}
}

/* Open the popup at (mx,my) for the given target, clamped to stay on-screen. */
static void ctx_open(enum ctx_kind kind, int target, const char *name,
                     int mx, int my)
{
	int n;
	ctx.kind = kind;
	ctx.target = target;
	ctx.name[0] = 0;
	if (name) fcpy(ctx.name, name);
	ctx_items(&n);
	int h = n * CTX_ROW + 2;
	int x = mx, y = my;
	if (x + CTX_W > W) x = W - CTX_W;
	if (y + h > H)     y = H - h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	ctx.x = x; ctx.y = y;
	ctx.open = 1;
}

static void ctx_draw(void)
{
	if (!ctx.open) return;
	int n;
	const struct ctx_item *it = ctx_items(&n);
	int h = n * CTX_ROW + 2;
	gfx_fill_rect(ctx.x, ctx.y, CTX_W, h, 0x232a36);
	gfx_rect(ctx.x, ctx.y, CTX_W, h, 0x05070a);
	for (int i = 0; i < n; i++)
		gfx_ui_text(ctx.x + 6, ctx.y + 1 + i * CTX_ROW, it[i].label, 0xc8d0dc);
}

/* Run the chosen context-menu action against the captured target. */
static void ctx_run(int act, int id)
{
	switch (act) {
	case A_OPEN:
		if (ctx.kind == CTX_FILE) { open_file(ctx.name); }
		else if (ctx.kind == CTX_DIR) {
			if (feq(ctx.name, "..")) go_up(); else enter(ctx.name);
			scroll = 0; sel = -1; set_title(id);
		} else if (ctx.kind == CTX_PLACE) {
			goto_path(places[ctx.target].path); set_title(id);
		} else if (ctx.kind == CTX_DEVICE) {
			goto_path(vols[ctx.target].point); set_title(id);
		}
		break;
	case A_EDIT:   open_with(ctx.name, "editor"); break;
	case A_CONNECT:
		if (ctx.target >= 0 && ctx.target < nvols) {
			char pt[40]; strcpy(pt, "/media/"); strcat(pt, vols[ctx.target].dev);
			mount(vols[ctx.target].dev, pt); set_title(id);
		}
		break;
	case A_EJECT:
		if (umount(vols[ctx.target].point) == 0 &&
		    under_path(path, vols[ctx.target].point)) {
			goto_path("/"); set_title(id);
		}
		break;
	case A_REFRESH: force_relist = 1; break;
	case A_UP:      go_up(); scroll = 0; sel = -1; set_title(id); break;
	case A_HOME:    goto_path("/"); set_title(id); break;
	}
}

/* Route a left-press while the popup is open: fire an item, or dismiss. Always
 * closes the menu and bumps seq so the popup repaints away. */
static void ctx_click(int id, int mx, int my)
{
	int n;
	const struct ctx_item *it = ctx_items(&n);
	if (mx >= ctx.x && mx < ctx.x + CTX_W &&
	    my >= ctx.y + 1 && my < ctx.y + 1 + n * CTX_ROW) {
		int i = (my - (ctx.y + 1)) / CTX_ROW;
		if (i >= 0 && i < n) ctx_run(it[i].act, id);
	}
	ctx.open = 0;
	seq++;
}

/* A right-press at (mx,my): figure out what's under the cursor and pop up the
 * matching context menu. Mirrors click()'s hit regions. */
static void rclick(int id, int mx, int my)
{
	int sbw = sidebar_w();
	refresh();
	scan_vols();

	if (my < MENU_H) { ctx_open(CTX_BG, 0, 0, mx, my); seq++; return; }

	if (mx < sbw) {                       /* sidebar */
		if (my >= LIST_TOP) {
			int i = (my - LIST_TOP) / ROW_H;
			if (i >= 0 && i < NPLACES) {
				ctx_open(CTX_PLACE, i, 0, mx, my); seq++; return;
			}
			for (int v = 0; v < nvols; v++) {
				int ry = dev_row_y(v);
				if (my >= ry && my < ry + ROW_H) {
					ctx_open(CTX_DEVICE, v, 0, mx, my); seq++; return;
				}
			}
		}
		ctx_open(CTX_BG, 0, 0, mx, my); seq++; return;
	}

	int e = list_hit(mx, my);             /* file list */
	if (e >= 0) {
		struct fent *fe = &fent_buf[e];
		sel = e;                          /* highlight the right-clicked row */
		ctx_open(fe->is_dir ? CTX_DIR : CTX_FILE, e, fe->name, mx, my);
	} else {
		ctx_open(CTX_BG, 0, 0, mx, my);
	}
	seq++;
}

/* Map a click in the list area to an entry index, or -1 (sidebar/header/empty).
 * Used by both the single- and double-click handlers. */
static int list_hit(int mx, int my)
{
	int sbw = sidebar_w();
	if (mx < sbw || my < LIST_TOP)
		return -1;
	int e = scroll + (my - LIST_TOP) / ROW_H;
	return (e >= 0 && e < fent_count) ? e : -1;
}

/* Activate the current selection: descend into a directory or open a file with
 * its associated program. Shared by the Enter key and the menu's Open item. */
static void activate(int id)
{
	refresh();
	if (sel < 0 || sel >= fent_count) return;
	struct fent *fe = &fent_buf[sel];
	if (fe->is_dir) {
		if (feq(fe->name, "..")) go_up();
		else enter(fe->name);
		scroll = 0; sel = -1;
		set_title(id);
	} else {
		open_file(fe->name);
	}
	seq++;
}

/* Run a "Go" menu item (also reached via the F5/Ctrl+O accelerators). */
static void menu_action(int item, int id)
{
	switch (item) {
	case 0: go_up(); scroll = 0; sel = -1; set_title(id); seq++; break;  /* Up      */
	case 1: goto_path("/"); set_title(id); seq++;                break;  /* Home    */
	case 2: force_relist = 1; seq++;                            break;  /* Refresh */
	case 3: activate(id);                                        break;  /* Open    */
	}
}

static void click(int id, int mx, int my)
{
	if (ctx.open) { ctx_click(id, mx, my); return; }  /* popup eats the click */
	if (my < MENU_H || gomenu.open) {     /* menu bar or open dropdown */
		int it = menu_click(&gomenu, mx, my, 2, 0);
		if (it >= 0)
			menu_action(it, id);
		seq++;
		return;
	}
	int sbw = sidebar_w();
	refresh();

	/* Divider grip: grab it to resize the sidebar. Consumes the press (and the
	 * following WEV_DRAG motion) so it isn't also taken as a row click. */
	if (sidebar_press(&sb, 0, 0, sbw, H, mx, my)) { seq++; return; }

	if (mx < sbw) {                       /* sidebar shortcut */
		if (my < LIST_TOP) return;
		int i = (my - LIST_TOP) / ROW_H;
		if (i >= 0 && i < NPLACES) { goto_path(places[i].path); set_title(id); seq++; return; }
		/* Devices region: an unmounted device connects (mounts) on any click; a
		 * mounted one navigates, or ejects if its right-edge button was hit. */
		for (int v = 0; v < nvols; v++) {
			int ry = dev_row_y(v);
			if (my < ry || my >= ry + ROW_H) continue;
			if (!vols[v].mounted) {           /* connect (mount) the device */
				char pt[40]; strcpy(pt, "/media/"); strcat(pt, vols[v].dev);
				mount(vols[v].dev, pt); set_title(id); seq++;
			} else if (mx >= sbw - EJECT_W) { /* eject button -> unmount */
				if (umount(vols[v].point) == 0) {
					if (under_path(path, vols[v].point)) goto_path("/");
					set_title(id); seq++;
				}
			} else {                          /* open the device */
				goto_path(vols[v].point); set_title(id); seq++;
			}
			return;
		}
		return;
	}
	int lx = sbw + 1, lw = W - sbw - 1;
	if (my < LIST_TOP) {                  /* scroll arrows */
		if (mx >= lx + lw - 36 && mx < lx + lw - 20) scroll -= 3;
		else if (mx >= lx + lw - 20 && mx < lx + lw - 4) scroll += 3;
		if (scroll < 0) scroll = 0;
		seq++;
		return;
	}
	int e = list_hit(mx, my);
	if (e < 0) return;
	sel = e;                              /* single click selects/highlights */
	seq++;
}

/* Double-click (delivered by the WM after the WEV_MOUSE that selected the row):
 * activate the entry — descend into a directory or open a file in its program. */
static void dblclick(int id, int mx, int my)
{
	refresh();
	int e = list_hit(mx, my);
	if (e < 0) return;
	struct fent *fe = &fent_buf[e];
	if (fe->is_dir) {
		if (feq(fe->name, "..")) go_up();
		else enter(fe->name);
		scroll = 0; sel = -1;
		set_title(id);
	} else {
		open_file(fe->name);
	}
	seq++;
}

/* Left-button motion (WEV_DRAG): drive a live sidebar resize if the press
 * grabbed the divider grip. The widget clamps the width to [MIN,MAX]; we just
 * repaint when it moved. */
static void drag(int id, int mx, int my)
{
	(void)id; (void)my;
	if (sidebar_drag(&sb, 0, mx))
		seq++;
}

/* Keyboard navigation: arrow up/down move the highlighted entry (directories
 * included, unlike a single click which only selects files), keeping it in view
 * by scrolling. Enter activates the selection — descend into a directory or
 * open a file with its associated program, mirroring click/dblclick. */
static void key(int id, int k)
{
	/* Menu accelerators (F5 refresh, Ctrl+O open): fire the item like a click. */
	int sc = menu_shortcut(&gomenu, k);
	if (sc >= 0) { gomenu.open = 0; menu_action(sc, id); return; }
	if (k == 27) { gomenu.open = 0; ctx.open = 0; seq++; return; }

	refresh();
	int vis = (H - LIST_TOP) / ROW_H;
	if (vis < 1) vis = 1;

	if (k == KEY_UP || k == KEY_DOWN) {
		if (fent_count == 0) return;
		if (sel < 0)
			sel = (k == KEY_DOWN) ? 0 : fent_count - 1;
		else {
			sel += (k == KEY_DOWN) ? 1 : -1;
			if (sel < 0) sel = 0;
			if (sel >= fent_count) sel = fent_count - 1;
		}
		if (sel < scroll) scroll = sel;                  /* keep in view */
		else if (sel >= scroll + vis) scroll = sel - vis + 1;
		seq++;
	} else if (k == '\n') {
		activate(id);
	}
}

int main(void)
{
	int id = win_create(W, H, "files");
	if (id < 0) { puts("files: no window\n"); return 1; }
	gfx_init(win_fb(), W, H);
	sidebar_init(&sb, SIDEBAR_W, SIDEBAR_MIN, SIDEBAR_MAX);
	set_title(id);

	int last = -1;
	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev)) {
			if (ev.type == WEV_CLOSE) { win_close(id); return 0; }
			else if (ev.type == WEV_MOUSE) click(id, ev.a, ev.b);
			else if (ev.type == WEV_DRAG) drag(id, ev.a, ev.b);
			else if (ev.type == WEV_RCLICK) rclick(id, ev.a, ev.b);
			else if (ev.type == WEV_DBLCLICK) dblclick(id, ev.a, ev.b);
			else if (ev.type == WEV_KEY) key(id, ev.a);
			else if (ev.type == WEV_RESIZE) {
				W = ev.a; H = ev.b;
				gfx_init(win_fb(), W, H);
				seq++;               /* force a repaint at the new size */
			}
		}
		if ((int)seq != last) { render(); win_present(id); last = seq; }
		win_wait(id);
	}
}
