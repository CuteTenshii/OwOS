/* files.c — a navigable file explorer for owofs, as a ring-3 app.
 *
 * Ports the former kernel app_files.c: a left "Places" sidebar plus a path
 * header and scrollable listing. Directory contents come from the readdir
 * syscall; the window's pixel buffer is drawn with the gfx lib. files_seq
 * bumps on every navigation/scroll/select to trigger a repaint.
 */
#include "owos.h"
#include "gfx.h"

static int W = 320, H = 380;    /* client size; changes on resize */
#define HEADER_H 18
#define ROW_H    16
#define FMAX     64
#define SIDEBAR_W 96

struct fent { char name[20]; int is_dir; unsigned size; };

static char        path[256] = "/";
static int         scroll, sel = -1;
static unsigned    seq;
static struct fent fent_buf[FMAX];
static int         fent_count, cached_seq = -1;

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
	gfx_fill_rect(0, 0, sbw, HEADER_H, 0x161d28);
	gfx_text(6, 1, "Places", 0x8aa0b8, 0x161d28);
	for (int i = 0; i < NPLACES; i++) {
		int ry = HEADER_H + i * ROW_H;
		int here = feq(path, places[i].path);
		unsigned rbg = here ? 0x24405c : 0x0e1218;
		if (here) gfx_fill_rect(0, ry, sbw, ROW_H, rbg);
		gfx_text(6, ry, places[i].label, here ? 0xffffff : 0xc8d0dc, rbg);
	}
	gfx_fill_rect(sbw, 0, 1, H, 0x05070a);

	/* File list. */
	int lx = sbw + 1, lw = W - sbw - 1;
	int vis = (H - HEADER_H) / ROW_H;
	if (vis < 1) vis = 1;
	if (scroll > fent_count - vis) scroll = fent_count - vis;
	if (scroll < 0) scroll = 0;

	gfx_fill_rect(lx, 0, lw, H, 0x12161e);
	gfx_fill_rect(lx, 0, lw, HEADER_H, 0x1d2430);
	int avail = (lw - 40) / 8;
	if (avail < 4) avail = 4;
	if (avail > 90) avail = 90;
	char pbuf[96];
	int pl = flen(path);
	if (pl <= avail) fcpy(pbuf, path);
	else { pbuf[0] = pbuf[1] = pbuf[2] = '.'; fcpy(pbuf + 3, path + (pl - (avail - 3))); }
	gfx_text(lx + 4, 1, pbuf, 0x9ad0ff, 0x1d2430);
	gfx_glyph(lx + lw - 32, 1, 0x1E, 0xffffff, 0x1d2430);
	gfx_glyph(lx + lw - 16, 1, 0x1F, 0xffffff, 0x1d2430);

	for (int r = 0; r < vis; r++) {
		int e = scroll + r;
		if (e >= fent_count) break;
		struct fent *fe = &fent_buf[e];
		int ry = HEADER_H + r * ROW_H;
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
}

static void click(int id, int mx, int my)
{
	int sbw = sidebar_w();
	refresh();

	if (mx < sbw) {                       /* sidebar shortcut */
		if (my < HEADER_H) return;
		int i = (my - HEADER_H) / ROW_H;
		if (i >= 0 && i < NPLACES) { goto_path(places[i].path); set_title(id); seq++; }
		return;
	}
	int lx = sbw + 1, lw = W - sbw - 1;
	if (my < HEADER_H) {                  /* scroll arrows */
		if (mx >= lx + lw - 36 && mx < lx + lw - 20) scroll -= 3;
		else if (mx >= lx + lw - 20 && mx < lx + lw - 4) scroll += 3;
		if (scroll < 0) scroll = 0;
		seq++;
		return;
	}
	int e = scroll + (my - HEADER_H) / ROW_H;
	if (e < 0 || e >= fent_count) return;
	struct fent *fe = &fent_buf[e];
	if (fe->is_dir) {
		if (feq(fe->name, "..")) go_up();
		else enter(fe->name);
		scroll = 0; sel = -1;
		set_title(id);
	} else {
		sel = e;
	}
	seq++;
}

int main(void)
{
	int id = win_create(W, H, "files");
	if (id < 0) { puts("files: no window\n"); return 1; }
	gfx_init(win_fb(), W, H);
	win_resizable(id, 1);            /* the file list can be resized */
	set_title(id);

	int last = -1;
	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev)) {
			if (ev.type == WEV_CLOSE) { win_close(id); return 0; }
			else if (ev.type == WEV_MOUSE) click(id, ev.a, ev.b);
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
