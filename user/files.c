/* files.c — a navigable file explorer for owofs, as a ring-3 app.
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
#define SIDEBAR_W 96
#define LIST_TOP (MENU_H + HEADER_H)   /* y of the first places/list row */

struct fent { char name[20]; int is_dir; unsigned size; };

static char        path[256] = "/";
static int         scroll, sel = -1;
static unsigned    seq;
static struct fent fent_buf[FMAX];
static int         fent_count, cached_seq = -1;

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

static void refresh(void)
{
	if (cached_seq == (int)seq)
		return;
	fent_count = collect();
	cached_seq = (int)seq;
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
static struct volinfo vols[4];
static int            nvols;
static void scan_vols(void)
{
	nvols = 0;
	while (nvols < 4 && volumes(nvols, &vols[nvols]) == 1)
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

static int sidebar_w(void)
{
	int sbw = SIDEBAR_W;
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

static void render(void)
{
	refresh();
	int sbw = sidebar_w();

	/* Sidebar. */
	gfx_fill_rect(0, 0, sbw, H, 0x0e1218);
	gfx_fill_rect(0, MENU_H, sbw, HEADER_H, 0x161d28);
	gfx_text(6, MENU_H + 1, "Places", 0x8aa0b8, 0x161d28);
	for (int i = 0; i < NPLACES; i++) {
		int ry = LIST_TOP + i * ROW_H;
		int here = feq(path, places[i].path);
		unsigned rbg = here ? 0x24405c : 0x0e1218;
		if (here) gfx_fill_rect(0, ry, sbw, ROW_H, rbg);
		gfx_text(6, ry, places[i].label, here ? 0xffffff : 0xc8d0dc, rbg);
	}

	/* Devices: mounted removable volumes, each with an eject button. */
	scan_vols();
	if (nvols > 0) {
		gfx_text(6, dev_header_y(), "Devices", 0x8aa0b8, 0x0e1218);
		for (int i = 0; i < nvols; i++) {
			int ry = dev_row_y(i);
			int here = under_path(path, vols[i].path);
			unsigned rbg = here ? 0x24405c : 0x0e1218;
			if (here) gfx_fill_rect(0, ry, sbw, ROW_H, rbg);
			gfx_text(6, ry, vols[i].label, here ? 0xffffff : 0xc8d0dc, rbg);
			draw_eject(sbw - EJECT_W + 2, ry + 3, 0x8aa0b8);
		}
	}
	gfx_fill_rect(sbw, 0, 1, H, 0x05070a);

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
	gfx_text(lx + 4, MENU_H + 1, pbuf, 0x9ad0ff, 0x1d2430);
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
		gfx_text(lx + 4, ry, fe->name, col, rbg);
		if (fe->is_dir && !feq(fe->name, ".."))
			gfx_text(lx + 4 + flen(fe->name) * 8, ry, "/", col, rbg);
		if (!fe->is_dir) {
			char sb[12];
			u32str(fe->size, sb);
			gfx_text(lx + lw - 4 - flen(sb) * 8, ry, sb, 0x8aa0b8, rbg);
		}
	}

	/* Top menu bar over the sidebar/list, then the dropdown drawn last so it
	 * overlays the listing. */
	gfx_fill_rect(0, 0, W, MENU_H, 0x232a36);
	menu_draw(&gomenu, 2, 0);
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
static void open_file(const char *name)
{
	char fp[256];
	full_path(name, fp, sizeof fp);
	int pid = fork();
	if (pid == 0) {
		execp1(app_for(name), fp);
		exit(1);                     /* exec failed: don't fall back into UI */
	}
	if (pid > 0)
		waitpid(pid);
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
	case 2: cached_seq = -1; seq++;                              break;  /* Refresh */
	case 3: activate(id);                                        break;  /* Open    */
	}
}

static void click(int id, int mx, int my)
{
	if (my < MENU_H || gomenu.open) {     /* menu bar or open dropdown */
		int it = menu_click(&gomenu, mx, my, 2, 0);
		if (it >= 0)
			menu_action(it, id);
		seq++;
		return;
	}
	int sbw = sidebar_w();
	refresh();

	if (mx < sbw) {                       /* sidebar shortcut */
		if (my < LIST_TOP) return;
		int i = (my - LIST_TOP) / ROW_H;
		if (i >= 0 && i < NPLACES) { goto_path(places[i].path); set_title(id); seq++; return; }
		/* Devices region: navigate to the volume, or eject if its button was hit. */
		for (int v = 0; v < nvols; v++) {
			int ry = dev_row_y(v);
			if (my < ry || my >= ry + ROW_H) continue;
			if (mx >= sbw - EJECT_W) {        /* eject button */
				if (eject(vols[v].path) == 0) {
					if (under_path(path, vols[v].path)) goto_path("/");
					set_title(id); seq++;
				}
			} else {                          /* open the device */
				goto_path(vols[v].path); set_title(id); seq++;
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
	struct fent *fe = &fent_buf[e];
	if (fe->is_dir) {                     /* a dir still opens on a single click */
		if (feq(fe->name, "..")) go_up();
		else enter(fe->name);
		scroll = 0; sel = -1;
		set_title(id);
	} else {
		sel = e;                          /* single click selects/highlights */
	}
	seq++;
}

/* Double-click (delivered by the WM after the WEV_MOUSE that selected the row):
 * open the file in the editor. Directories already navigate on a single click. */
static void dblclick(int mx, int my)
{
	refresh();
	int e = list_hit(mx, my);
	if (e < 0) return;
	struct fent *fe = &fent_buf[e];
	if (!fe->is_dir) {
		open_file(fe->name);
		seq++;
	}
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
	if (k == 27) { gomenu.open = 0; seq++; return; }

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
	set_title(id);
	win_cursor(id, WIN_CURSOR_HAND);   /* hand over the clickable file list */

	int last = -1;
	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev)) {
			if (ev.type == WEV_CLOSE) { win_close(id); return 0; }
			else if (ev.type == WEV_MOUSE) click(id, ev.a, ev.b);
			else if (ev.type == WEV_DBLCLICK) dblclick(ev.a, ev.b);
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
