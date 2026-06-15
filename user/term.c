/* term.c — a terminal: a resizable, scrollable window hosting a shell.
 *
 * A text grid drawn with the 8x16 font (gfx.c) over a scrollback ring buffer.
 * Keys arrive as WEV_KEY events; the shell line-edits in the grid. This is a
 * self-contained shell -- there is no kernel-shell bridge. Commands are
 * served three ways:
 *   - builtins run over syscalls: filesystem/string ops, plus system
 *     info/actions that read or write /proc (cpuinfo, journal, clip, poweroff,
 *     dhcp, services, ...);
 *   - CLI tools (neofetch, curl, lspci, ping, ...) are spawned with their stdout
 *     captured through a pipe (spawn_cap) and rendered inline like a pty;
 *   - any other /bin program is launched detached (GUI apps open their window).
 * Ctrl+C arrives as a KEY_INTR event: it cancels the line at the prompt, or
 * kills the captured child mid-command.
 *
 * The window resizes (WEV_RESIZE recomputes the grid) and scrolls (mouse wheel
 * -> WEV_SCROLL moves a view offset over the scrollback, or a drag/click on the
 * gfx scrollbar widget down the right edge).
 */

#include "owos.h"
#include "gfx.h"
#include "lzss.h"
#include "deflate.h"

#define FONT_W 8
#define FONT_H 16
#define MAXCOLS 200
#define SBROWS  400                       /* scrollback lines                   */

#define BG        0x0e1116u
#define FG        0xc8d0dcu
#define FG_PROMPT 0x49d17fu
#define FG_DIR    0x6cb6ffu
#define FG_ERR    0xe06c75u

#define HOME "/home/root"

/* ---- scrollback ring buffer ------------------------------------------------- */

static unsigned char gch[SBROWS][MAXCOLS];
static unsigned      gfg[SBROWS][MAXCOLS];
static int  top;                          /* phys index of logical line 0       */
static int  nlines = 1;                   /* logical lines in use               */
static int  cl, cc;                       /* cursor: logical line, column       */
static int  view;                         /* lines scrolled up from the bottom  */
static int  ls_line, ls_col;              /* where the input line starts        */
static unsigned cur_fg = FG;
static int  cols = 90, rows = 27;         /* active grid (from window size)     */
static int  winw = 720, winh = 432;       /* window client size in pixels       */
static int  win, dirty = 1;

static int phys(int logical) { return (top + logical) % SBROWS; }

static void line_blank(int p) { for (int x = 0; x < MAXCOLS; x++) { gch[p][x] = ' '; gfg[p][x] = FG; } }

static void grid_reset(void)
{
	for (int p = 0; p < SBROWS; p++) line_blank(p);
	top = 0; nlines = 1; cl = cc = 0; view = 0;
	dirty = 1;
}

/* Advance to a fresh next line, scrolling the ring when the cursor falls off the
 * bottom of the scrollback. */
static void tnewline(void)
{
	cc = 0;
	cl++;
	if (cl >= nlines) {
		if (nlines < SBROWS) {
			nlines = cl + 1;
		} else {
			top = (top + 1) % SBROWS;     /* drop the oldest line               */
			cl--; if (ls_line > 0) ls_line--;
			line_blank((top + nlines - 1) % SBROWS);
		}
	}
}

/* 16-colour ANSI palette. The bridge sends kernel-builtin colours as SGR escape
 * sequences (see vga.c capture_emit_color); we parse them so neofetch et al.
 * keep their colours. Index 0-7 normal, 8-15 bright. */
static const unsigned ansi_rgb[16] = {
	0x1b1f26, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xc8d0dc,
	0x5c6370, 0xff7b86, 0xb5e890, 0xffd68a, 0x7ec5ff, 0xe0a0ff, 0x70d8e4, 0xffffff,
};
static int esc;                           /* 0 normal, 1 saw ESC, 2 in CSI       */
static int eparm[8], enp;

static void sgr_apply(void)
{
	for (int i = 0; i <= enp; i++) {
		int p = eparm[i];
		if (p == 0)                  cur_fg = FG;              /* reset           */
		else if (p >= 30 && p <= 37) cur_fg = ansi_rgb[p - 30];
		else if (p >= 90 && p <= 97) cur_fg = ansi_rgb[8 + p - 90];
		/* bg codes (40-47, 100-107) ignored: the grid background is fixed */
	}
}

static void tputc(char c)
{
	/* ANSI SGR (colour) escape parser: ESC [ params m. Other escapes are
	 * swallowed. Only the bridge's output carries escapes; typed input never
	 * does (on_key filters to printable bytes). */
	if (esc == 1) { esc = (c == '[') ? 2 : 0; if (esc == 2) { enp = 0; eparm[0] = 0; } return; }
	if (esc == 2) {
		if (c >= '0' && c <= '9') eparm[enp] = eparm[enp] * 10 + (c - '0');
		else if (c == ';') { if (enp < 7) eparm[++enp] = 0; }
		else { if (c == 'm') sgr_apply(); esc = 0; }
		return;
	}
	if (c == 0x1b) { esc = 1; return; }

	view = 0;                             /* any output snaps to the bottom      */
	if (c == '\n') { tnewline(); }
	else if (c == '\b') { if (cc > 0) { cc--; gch[phys(cl)][cc] = ' '; } }
	else if (c == '\t') { do { tputc(' '); } while (cc % 8); }
	else {
		if (cc >= cols) tnewline();
		int p = phys(cl);
		gch[p][cc] = (unsigned char)c;
		gfg[p][cc] = cur_fg;
		cc++;
	}
	dirty = 1;
}

static void tputs(const char *s)              { while (*s) tputc(*s++); }
static void tputs_c(const char *s, unsigned f){ unsigned o = cur_fg; cur_fg = f; tputs(s); cur_fg = o; }
static void tdec(unsigned v)
{
	char b[12]; int n = 0;
	if (!v) { tputc('0'); return; }
	while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
	while (n--) tputc(b[n]);
}

static void redraw(void)
{
	gfx_fill_rect(0, 0, winw - GUI_SCROLL_W, winh, BG);     /* text area          */
	int bottom = nlines > rows ? nlines - rows : 0;
	int vt = bottom - view;
	if (vt < 0) vt = 0;
	for (int r = 0; r < rows; r++) {
		int L = vt + r;
		if (L >= nlines) break;
		int p = phys(L);
		for (int x = 0; x < cols; x++)
			if (gch[p][x] != ' ')
				gfx_glyph(x * FONT_W, r * FONT_H, gch[p][x], gfg[p][x], BG);
	}
	if (view == 0 && cl >= vt && cl < vt + rows)            /* caret at bottom    */
		gfx_fill_rect(cc * FONT_W, (cl - vt) * FONT_H + FONT_H - 2, FONT_W, 2, FG);

	/* Scrollbar down the right edge: `scroll` is the top visible line (vt), which
	 * runs 0..bottom as `view` runs bottom..0 (view counts lines scrolled back). */
	scrollbar_draw(winw - GUI_SCROLL_W, 0, GUI_SCROLL_W, winh, bottom - view, nlines, rows);

	win_present(win);
	dirty = 0;
}

/* ---- tiny string helpers ---------------------------------------------------- */

static int  has_prefix(const char *s, const char *p) { while (*p) { if (*s++ != *p++) return 0; } return 1; }

/* ---- cwd + path resolution -------------------------------------------------- */

static char cwd[256] = HOME;

/* Resolve `name` against cwd into `out`: ~ expands to HOME, then . and .. are
 * collapsed. Every syscall path goes through here (the process kernel cwd is
 * "/", so the shell must hand syscalls absolute paths). */
static void abspath(const char *name, char *out)
{
	char in[512];
	if (name[0] == '~' && (name[1] == '\0' || name[1] == '/')) {
		strcpy(in, HOME); strcat(in, name + 1);
	} else if (name[0] == '/') {
		strcpy(in, name);
	} else {
		strcpy(in, cwd);
		int l = strlen(in);
		if (l == 0 || in[l - 1] != '/') { in[l] = '/'; in[l + 1] = '\0'; }
		strcat(in, name);
	}
	char seg[64][32];
	int ns = 0, i = 0;
	while (in[i]) {
		if (in[i] == '/') { i++; continue; }
		char s[32]; int j = 0;
		while (in[i] && in[i] != '/' && j < 31) s[j++] = in[i++];
		s[j] = '\0';
		if (strcmp(s, ".") == 0) continue;
		if (strcmp(s, "..") == 0) { if (ns > 0) ns--; continue; }
		if (ns < 64) strcpy(seg[ns++], s);
	}
	if (ns == 0) { strcpy(out, "/"); return; }
	out[0] = '\0';
	for (int k = 0; k < ns; k++) { strcat(out, "/"); strcat(out, seg[k]); }
}

/* ---- builtins --------------------------------------------------------------- */

static char iobuf[64 * 1024];

static void prompt(void)
{
	tputs_c("owos:", FG_PROMPT);
	if (strcmp(cwd, HOME) == 0) tputs_c("~", FG_PROMPT);
	else if (has_prefix(cwd, HOME "/")) { tputs_c("~", FG_PROMPT); tputs_c(cwd + strlen(HOME), FG_PROMPT); }
	else tputs_c(cwd, FG_PROMPT);
	tputs_c("$ ", FG_PROMPT);
	ls_line = cl; ls_col = cc;
}

static void err(const char *a, const char *b)
{
	tputs_c(a, FG_ERR);
	if (b) tputs_c(b, FG_ERR);
	tputc('\n');
}

static void bi_help(void)
{
	tputs_c("OwOS shell.  Ctrl+C cancels the line / aborts a command.\n", FG_DIR);
	tputs("files    ls cat cp mv rm mkdir rmdir stat pwd cd grep\n");
	tputs("archive  zip unzip gzip gunzip\n");
	tputs("system   uptime uname whoami cpuinfo meminfo date ps journal which\n");
	tputs("device   storage lsusb mount umount loadkeys clip\n");
	tputs("net      services (dhcp/ntp run as services: `services start ntp`)\n");
	tputs("power    poweroff restart\n");
	tputs("shell    help clear echo sleep exit\n");
	tputs("CLI tools (neofetch curl lspci ping dig play torrent) run inline; other /bin\n");
	tputs("programs (editor files clock browser ...) open their own window.\n");
}

static void bi_ls(int argc, char **argv)
{
	char p[256];
	abspath(argc > 1 ? argv[1] : ".", p);
	struct dirent_u de;
	int i = 0;
	struct statinfo st;
	if (stat(p, &st) != 0) { err("ls: no such path: ", p); return; }
	while (readdir(p, i++, &de) == 1) {
		tputs_c(de.name, de.is_dir ? FG_DIR : FG);
		if (de.is_dir) tputc('/');
		tputc('\n');
	}
}

static void bi_cat(int argc, char **argv)
{
	if (argc < 2) { tputs("usage: cat <file>\n"); return; }
	char p[256]; abspath(argv[1], p);
	int n = readfile(p, iobuf, sizeof iobuf);
	if (n < 0) { err("cat: cannot read: ", argv[1]); return; }
	for (int i = 0; i < n; i++) tputc(iobuf[i]);
	if (n && iobuf[n - 1] != '\n') tputc('\n');
}

static void bi_cp(int argc, char **argv)
{
	if (argc < 3) { tputs("usage: cp <src> <dst>\n"); return; }
	char a[256], b[256]; abspath(argv[1], a); abspath(argv[2], b);
	int n = readfile(a, iobuf, sizeof iobuf);
	if (n < 0) { err("cp: cannot read source", 0); return; }
	if (writefile(b, iobuf, n) < 0) err("cp: cannot write dest", 0);
}

static void bi_stat(int argc, char **argv)
{
	if (argc < 2) { tputs("usage: stat <path>\n"); return; }
	char p[256]; abspath(argv[1], p);
	struct statinfo st;
	if (stat(p, &st) != 0) { err("stat: no such path", 0); return; }
	tputs(p);
	if (st.is_dir) tputs("  <dir>\n");
	else { tputs("  "); tdec(st.size); tputs(" bytes\n"); }
}

/* ---- info builtins: read /proc and render (no kernel-shell bridge) ---------- */

/* Print /proc/<name> verbatim. 1 on success, 0 if the entry is missing. */
static int cat_proc(const char *name)
{
	char p[64];
	strcpy(p, "/proc/");
	strcat(p, name);
	int n = readfile(p, iobuf, sizeof iobuf);
	if (n < 0) return 0;
	for (int i = 0; i < n; i++) tputc(iobuf[i]);
	if (n && iobuf[n - 1] != '\n') tputc('\n');
	return 1;
}

static unsigned to_uint(const char *s)
{
	unsigned v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
	return v;
}

static void bi_uptime(void)
{
	if (readfile("/proc/uptime", iobuf, sizeof iobuf) < 0) { err("uptime: unavailable", 0); return; }
	unsigned s = to_uint(iobuf), h = s / 3600, m = (s % 3600) / 60;
	tputs("up ");
	if (h)        { tdec(h); tputs("h "); }
	if (h || m)   { tdec(m); tputs("m "); }
	tdec(s % 60); tputs("s\n");
}

static void bi_uname(int argc, char **argv)
{
	int all = argc > 1 && !strcmp(argv[1], "-a");
	if (all) tputs("OwOS owos 0.1 i386\n");
	else     tputs("OwOS\n");
}

static void bi_which(int argc, char **argv)
{
	static const char *const ring3[] = {
		"help", "clear", "echo", "pwd", "cd", "ls", "cat", "cp", "mv",
		"stat", "mkdir", "rmdir", "rm", "exit", "uptime", "uname", "whoami",
		"which", "cpuinfo", "meminfo", "date", "storage", "sleep",
	};
	if (argc < 2) { tputs("usage: which <name>\n"); return; }
	for (unsigned i = 0; i < sizeof ring3 / sizeof ring3[0]; i++)
		if (!strcmp(argv[1], ring3[i])) { tputs(argv[1]); tputs(": shell builtin\n"); return; }
	char path[256];
	strcpy(path, "/bin/"); strcat(path, argv[1]);
	struct statinfo st;
	if (stat(path, &st) == 0 && !st.is_dir) { tputs(path); tputc('\n'); return; }
	err(argv[1], " not found");
}

/* ---- control builtins: act via /proc, no kernel-shell bridge ---------------- */

static void bi_loadkeys(int argc, char **argv)
{
	if (argc < 2) { tputs("usage: loadkeys <us|fr>\n"); return; }
	if (writefile("/proc/keyboard", argv[1], strlen(argv[1])) < 0)
		err("loadkeys: unknown layout (try us or fr)", 0);
	else { tputs("keyboard layout: "); tputs(argv[1]); tputc('\n'); }
}

static void bi_clip(int argc, char **argv)
{
	if (argc < 2) {                              /* no text: print the clipboard  */
		int n = readfile("/proc/clipboard", iobuf, sizeof iobuf);
		for (int i = 0; i < n; i++) tputc(iobuf[i]);
		if (n <= 0 || iobuf[n - 1] != '\n') tputc('\n');
		return;
	}
	int len = 0;                                 /* join argv[1..] with spaces    */
	for (int k = 1; k < argc; k++) {
		for (const char *a = argv[k]; *a && len < (int)sizeof iobuf - 1; a++) iobuf[len++] = *a;
		if (k < argc - 1 && len < (int)sizeof iobuf - 1) iobuf[len++] = ' ';
	}
	if (writefile("/proc/clipboard", iobuf, len) < 0) err("clip: failed", 0);
}

/* Default mount point for a device: /media/<dev>. */
static void media_point(const char *dev, char *out)
{
	strcpy(out, "/media/");
	strcat(out, dev);
}

static void bi_mount(int argc, char **argv)
{
	if (argc > 1) {                              /* mount <dev> [point]           */
		char point[64];
		if (argc > 2) strcpy(point, argv[2]);
		else          media_point(argv[1], point);
		if (mount(argv[1], point) == 0) {
			tputs("mounted "); tputs(argv[1]); tputs(" at "); tputs(point); tputc('\n');
		} else err("mount: cannot mount ", argv[1]);
		return;
	}
	struct volinfo v;                            /* no arg: mount first unmounted */
	for (int i = 0; volumes(i, &v) == 1; i++) {
		if (!v.mounted) {
			char point[64]; media_point(v.dev, point);
			if (mount(v.dev, point) == 0) {
				tputs("mounted "); tputs(v.label); tputs(" at "); tputs(point); tputc('\n');
			} else err("mount: failed to mount ", v.dev);
			return;
		}
	}
	tputs("mount: no unmounted device (see `ls /dev`)\n");
}

static void bi_umount(int argc, char **argv)
{
	if (argc > 1) {                              /* umount <point>                */
		if (umount(argv[1]) == 0) { tputs("unmounted "); tputs(argv[1]); tputc('\n'); }
		else err("umount: nothing mounted at ", argv[1]);
		return;
	}
	struct volinfo v;                            /* no arg: unmount first mounted */
	for (int i = 0; volumes(i, &v) == 1; i++)
		if (v.mounted) {
			if (umount(v.point) == 0) { tputs("unmounted "); tputs(v.point); tputc('\n'); }
			else err("umount: failed", 0);
			return;
		}
	tputs("umount: nothing mounted\n");
}

static void bi_services(int argc, char **argv)
{
	if (argc < 2 || !strcmp(argv[1], "list") || !strcmp(argv[1], "status")) {
		cat_proc("services");
		return;
	}
	char req[128];                               /* "start X" / "stop X" / ...    */
	int len = 0;
	for (int k = 1; k < argc; k++) {
		for (const char *a = argv[k]; *a && len < (int)sizeof req - 1; a++) req[len++] = *a;
		if (k < argc - 1 && len < (int)sizeof req - 1) req[len++] = ' ';
	}
	req[len] = '\0';
	if (writefile("/proc/services", req, len) < 0) err("services: failed", 0);
}

/* ---- file codecs: zip/unzip (LZSS) + gzip/gunzip (DEFLATE) ------------------ */

static unsigned char cbuf[96 * 1024];        /* codec output (input is iobuf)   */

/* zip/gzip: read src, compress into cbuf, write dst (default = src + suffix). */
static void do_compress(int argc, char **argv, const char *suffix, const char *cmd,
                        int (*comp)(const uint8_t *, uint32_t, uint8_t *, uint32_t),
                        uint32_t (*bound)(uint32_t))
{
	if (argc < 2) { tputs("usage: "); tputs(cmd); tputs(" <src> [dst]\n"); return; }
	char sp[256]; abspath(argv[1], sp);
	int n = readfile(sp, iobuf, sizeof iobuf);
	if (n < 0) { err(cmd, ": no such file"); return; }
	if (bound((uint32_t)n) > sizeof cbuf) { err(cmd, ": file too large"); return; }
	int clen = comp((const uint8_t *)iobuf, (uint32_t)n, cbuf, sizeof cbuf);
	if (clen < 0) { err(cmd, ": compression failed"); return; }
	char dp[256];
	if (argc > 2) abspath(argv[2], dp);
	else { abspath(argv[1], dp); strcat(dp, suffix); }
	if (writefile(dp, cbuf, clen) < 0) { err(cmd, ": write failed"); return; }
	tputs(argv[1]); tputs(" -> "); tputs(argc > 2 ? argv[2] : ""); if (argc <= 2) { tputs(argv[1]); tputs(suffix); }
	tputs(" ("); tdec((unsigned)n); tputs(" -> "); tdec((unsigned)clen); tputs(" bytes)\n");
}

/* unzip/gunzip: read src, decompress into cbuf, write dst (default = src minus
 * suffix). `orig` sizes the output; the codec validates the container. */
static void do_decompress(int argc, char **argv, const char *suffix, const char *cmd,
                          int (*dec)(const uint8_t *, uint32_t, uint8_t *, uint32_t),
                          int (*orig)(const uint8_t *, uint32_t))
{
	if (argc < 2) { tputs("usage: "); tputs(cmd); tputs(" <src> [dst]\n"); return; }
	char sp[256]; abspath(argv[1], sp);
	int n = readfile(sp, iobuf, sizeof iobuf);
	if (n < 0) { err(cmd, ": no such file"); return; }
	int o = orig((const uint8_t *)iobuf, (uint32_t)n);
	if (o < 0 || (unsigned)o > sizeof cbuf) { err(cmd, ": not a valid archive"); return; }
	int dlen = dec((const uint8_t *)iobuf, (uint32_t)n, cbuf, (uint32_t)o);
	if (dlen < 0) { err(cmd, ": corrupt archive"); return; }
	char dp[256];
	if (argc > 2) { abspath(argv[2], dp); }
	else {                                   /* strip the suffix from src        */
		int sl = strlen(argv[1]), xl = strlen(suffix);
		if (sl <= xl || strcmp(argv[1] + sl - xl, suffix) != 0) {
			tputs(cmd); tputs(": pass an explicit dst\n"); return;
		}
		char base[256]; strcpy(base, argv[1]); base[sl - xl] = '\0';
		abspath(base, dp);
	}
	if (writefile(dp, cbuf, dlen) < 0) { err(cmd, ": write failed"); return; }
	tputs(argv[1]); tputs(" -> "); tputs(argc > 2 ? argv[2] : "(decompressed)");
	tputs(" ("); tdec((unsigned)dlen); tputs(" bytes)\n");
}

/* grep <pattern> <file>: print lines of the file that contain the pattern. */
static void bi_grep(int argc, char **argv)
{
	if (argc < 3) { tputs("usage: grep <pattern> <file>\n"); return; }
	char p[256]; abspath(argv[2], p);
	int n = readfile(p, iobuf, sizeof iobuf);
	if (n < 0) { err("grep: cannot read ", argv[2]); return; }
	const char *pat = argv[1];
	int i = 0;
	while (i < n) {
		int ls = i;
		while (i < n && iobuf[i] != '\n') i++;
		iobuf[i < n ? i : n - 1] = '\0';         /* terminate the line for strstr  */
		int hit = 0;
		for (const char *s = &iobuf[ls]; *s; s++) {     /* substring search        */
			const char *a = s, *b = pat;
			while (*a && *b && *a == *b) { a++; b++; }
			if (!*b) { hit = 1; break; }
		}
		if (hit) { tputs(&iobuf[ls]); tputc('\n'); }
		i++;
	}
}

/* /bin programs whose stdout we capture and render inline (CLI tools), rather
 * than launching as a detached window. Anything else under /bin is a GUI app. */
static int is_cli(const char *name)
{
	static const char *const cli[] = {
		"neofetch", "curl", "lspci", "play", "ping", "dig", "torrent",
	};
	for (unsigned i = 0; i < sizeof cli / sizeof cli[0]; i++)
		if (!strcmp(name, cli[i])) return 1;
	return 0;
}

/* Spawn a /bin CLI tool with its stdout wired to a pipe (spawn_cap) and render
 * the output as it streams in. The terminal stays responsive to window events;
 * closing the window kills the child. This replaces the kernel-shell bridge:
 * the command is an ordinary process whose output we read like a pty. */
static void render_child(const char *path, char **argv, int argc)
{
	int rfd = -1;
	int pid = spawn_cap(path, argv, argc, &rfd);
	if (pid < 0) { err("failed to launch ", path); return; }
	int closing = 0, intr = 0;
	for (;;) {
		struct win_event ev;
		while (win_poll(win, &ev)) {                 /* stay responsive mid-command */
			if (ev.type == WEV_CLOSE && !closing) { closing = 1; kill(pid); }
			else if (ev.type == WEV_KEY && (unsigned char)ev.a == KEY_INTR && !intr && !closing) {
				intr = 1; kill(pid); tputs("^C\n"); redraw();   /* Ctrl+C: abort */
			}
		}
		int n = read_nb(rfd, iobuf, sizeof iobuf);
		if (n > 0) {
			if (!closing && !intr) { for (int i = 0; i < n; i++) tputc(iobuf[i]); redraw(); }
		} else if (n < 0) {
			break;                                  /* child closed stdout / exited */
		} else {
			if (dirty && !closing) redraw();        /* running; nothing yet     */
			msleep(8);
		}
	}
	close(rfd);
	waitpid(pid);                                   /* reap the zombie           */
	if (closing) { win_close(win); exit(0); }
	/* intr: fall through -- run()'s caller prints a fresh prompt */
}

/* A non-builtin command: a captured CLI tool, a detached GUI app, or (until the
 * last kernel builtins are ported) the kernel-shell bridge. */
static void run_other(const char *orig, int argc, char **argv)
{
	char path[256];
	if (argv[0][0] == '/') strcpy(path, argv[0]);
	else { strcpy(path, "/bin/"); strcat(path, argv[0]); }

	if (is_cli(argv[0])) { render_child(path, argv, argc); return; }

	struct statinfo st;
	if (stat(path, &st) == 0 && !st.is_dir) {           /* a detached GUI app    */
		if (spawn(path, argc > 1 ? argv[1] : 0) < 0) err("failed to launch", 0);
		return;
	}

	/* The shell dispatches entirely in user space -- there is no kernel-shell bridge,
	 * and every command it offers is implemented here. */
	err(argv[0], ": command not found");
}

static void run(const char *orig)
{
	char buf[256]; strcpy(buf, orig);
	char *argv[16];
	int argc = 0, i = 0;
	/* Split on whitespace, with double-quoted runs kept whole and the quotes
	 * stripped (so `curl -H "X-Test: a b"` is one argument). Compacted in place. */
	while (buf[i] && argc < 16) {
		while (buf[i] == ' ') i++;
		if (!buf[i]) break;
		char *w = &buf[i];
		argv[argc++] = w;
		while (buf[i] && buf[i] != ' ') {
			if (buf[i] == '"') {
				i++;
				while (buf[i] && buf[i] != '"') *w++ = buf[i++];
				if (buf[i] == '"') i++;
			} else {
				*w++ = buf[i++];
			}
		}
		if (buf[i]) i++;
		*w = '\0';
	}
	if (argc == 0) return;
	const char *c = argv[0];

	if      (!strcmp(c, "help"))  bi_help();
	else if (!strcmp(c, "clear")) grid_reset();
	else if (!strcmp(c, "echo"))  { for (int k = 1; k < argc; k++) { tputs(argv[k]); if (k < argc - 1) tputc(' '); } tputc('\n'); }
	else if (!strcmp(c, "pwd"))   { tputs(cwd); tputc('\n'); }
	else if (!strcmp(c, "cd")) {
		if (argc < 2) { strcpy(cwd, HOME); }
		else { char p[256]; abspath(argv[1], p); struct statinfo st;
		       if (stat(p, &st) == 0 && st.is_dir) strcpy(cwd, p);
		       else err("cd: not a directory: ", argv[1]); }
	}
	else if (!strcmp(c, "ls"))    bi_ls(argc, argv);
	else if (!strcmp(c, "cat"))   bi_cat(argc, argv);
	else if (!strcmp(c, "cp"))    bi_cp(argc, argv);
	else if (!strcmp(c, "stat"))  bi_stat(argc, argv);
	else if (!strcmp(c, "mkdir")) { for (int k = 1; k < argc; k++) { char p[256]; abspath(argv[k], p); if (mkdir(p) < 0) err("mkdir: failed: ", argv[k]); } }
	else if (!strcmp(c, "rmdir")) { for (int k = 1; k < argc; k++) { char p[256]; abspath(argv[k], p); if (rmdir(p) < 0) err("rmdir: failed: ", argv[k]); } }
	else if (!strcmp(c, "rm"))    { for (int k = 1; k < argc; k++) { char p[256]; abspath(argv[k], p); if (unlink(p) < 0) err("rm: failed: ", argv[k]); } }
	else if (!strcmp(c, "mv"))    { if (argc < 3) tputs("usage: mv <src> <dst>\n"); else { char a[256], b[256]; abspath(argv[1], a); abspath(argv[2], b); if (rename(a, b) < 0) err("mv: failed", 0); } }
	else if (!strcmp(c, "exit"))  { win_close(win); exit(0); }
	else if (!strcmp(c, "uptime"))  bi_uptime();
	else if (!strcmp(c, "uname"))   bi_uname(argc, argv);
	else if (!strcmp(c, "whoami"))  tputs("root\n");
	else if (!strcmp(c, "which"))   bi_which(argc, argv);
	else if (!strcmp(c, "meminfo")) cat_proc("meminfo");
	else if (!strcmp(c, "cpuinfo")) cat_proc("cpuinfo");
	else if (!strcmp(c, "date"))    cat_proc("date");
	else if (!strcmp(c, "storage")) { if (!cat_proc("storage")) tputs("no storage devices\n"); }
	else if (!strcmp(c, "sleep"))   { if (argc > 1) msleep(to_uint(argv[1]) * 1000); }
	else if (!strcmp(c, "journal")) cat_proc("journal");
	else if (!strcmp(c, "poweroff")) power(0);
	else if (!strcmp(c, "restart"))  power(1);
	else if (!strcmp(c, "loadkeys")) bi_loadkeys(argc, argv);
	else if (!strcmp(c, "clip"))    bi_clip(argc, argv);
	else if (!strcmp(c, "mount"))   bi_mount(argc, argv);
	else if (!strcmp(c, "umount"))  bi_umount(argc, argv);
	else if (!strcmp(c, "services")) bi_services(argc, argv);
	else if (!strcmp(c, "lsusb"))   cat_proc("usb");
	else if (!strcmp(c, "ps"))      cat_proc("tasks");
	else if (!strcmp(c, "grep"))    bi_grep(argc, argv);
	else if (!strcmp(c, "zip"))     do_compress(argc, argv, ".z", "zip", zip_compress, zip_bound);
	else if (!strcmp(c, "unzip"))   do_decompress(argc, argv, ".z", "unzip", zip_decompress, zip_orig_size);
	else if (!strcmp(c, "gzip"))    do_compress(argc, argv, ".gz", "gzip", gzip_compress, gzip_bound);
	else if (!strcmp(c, "gunzip"))  do_decompress(argc, argv, ".gz", "gunzip", gzip_decompress, gzip_orig_size);
	else                        run_other(orig, argc, argv);
}

/* ---- line editor ------------------------------------------------------------ */

static char line[256];
static int  llen, lpos, rlen;
static char hist[16][256];
static int  hcount, hpos;

static void caret_to(int off)
{
	int tcol = ls_col + off, tline = ls_line;
	while (tcol >= cols) { tcol -= cols; tline++; }
	cc = tcol; cl = tline;
}

static void line_redraw(void)
{
	cl = ls_line; cc = ls_col;
	for (int i = 0; i < llen; i++) tputc(line[i]);
	for (int i = llen; i < rlen; i++) tputc(' ');
	rlen = llen;
	caret_to(lpos);
	dirty = 1;
}

static void line_set(const char *s) { strcpy(line, s); llen = lpos = strlen(line); line_redraw(); }

static void line_submit(void)
{
	line[llen] = '\0';
	cl = ls_line; cc = ls_col;
	for (int i = 0; i < llen; i++) tputc(line[i]);
	tputc('\n');
	if (llen) {
		for (int k = 15; k > 0; k--) strcpy(hist[k], hist[k - 1]);
		strcpy(hist[0], line);
		if (hcount < 16) hcount++;
	}
	char tmp[256]; strcpy(tmp, line);
	llen = lpos = rlen = 0; hpos = -1;
	redraw();                            /* flush the echoed line + newline now: a
	                                        command that blocks in the kernel (play,
	                                        sleep, curl) stalls this event handler, and
	                                        the present only runs in the main loop after
	                                        we return -- without this the screen freezes
	                                        on the pre-Enter line for the whole command. */
	run(tmp);
	cur_fg = FG; esc = 0;                 /* don't let a command's colour bleed   */
	prompt();
}

static void on_key(char c)
{
	switch ((unsigned char)c) {
	case '\n': case '\r': line_submit(); break;
	case KEY_INTR:                              /* Ctrl+C: abandon the line     */
		cl = ls_line; cc = ls_col;
		for (int i = 0; i < llen; i++) tputc(line[i]);
		tputs("^C\n");
		llen = lpos = rlen = 0; hpos = -1;
		prompt();
		break;
	case '\b':
		if (lpos > 0) { for (int i = lpos - 1; i < llen - 1; i++) line[i] = line[i + 1]; llen--; lpos--; line_redraw(); }
		break;
	case KEY_LEFT:  if (lpos > 0)    { lpos--; line_redraw(); } break;
	case KEY_RIGHT: if (lpos < llen) { lpos++; line_redraw(); } break;
	case KEY_HOME:
	case KEY_SELALL: lpos = 0;    line_redraw(); break;  /* Home / Ctrl+A */
	case KEY_END:   lpos = llen; line_redraw(); break;
	case KEY_DELETE:
		if (lpos < llen) { for (int i = lpos; i < llen - 1; i++) line[i] = line[i + 1]; llen--; line_redraw(); }
		break;
	case KEY_UP:    if (hcount && hpos + 1 < hcount) { hpos++; line_set(hist[hpos]); } break;
	case KEY_DOWN:
		if (hpos > 0) { hpos--; line_set(hist[hpos]); }
		else if (hpos == 0) { hpos = -1; line_set(""); }
		break;
	default:
		if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f && llen < (int)sizeof line - 1) {
			for (int i = llen; i > lpos; i--) line[i] = line[i - 1];
			line[lpos++] = c; llen++; line_redraw();
		}
		break;
	}
}

static void on_scroll(int dz)
{
	int maxoff = nlines > rows ? nlines - rows : 0;
	view -= dz * 3;                       /* wheel up scrolls back to older lines  */
	if (view < 0) view = 0;
	if (view > maxoff) view = maxoff;
	dirty = 1;
}

/* The scrollbar widget works in top-line units (`scroll`, 0..bottom); `view`
 * counts lines scrolled back (bottom..0), so the two are mirror images. */
static int sb_drag;                       /* 1 while the thumb is held           */

static void sb_set(int scroll)
{
	int maxoff = nlines > rows ? nlines - rows : 0;
	view = maxoff - scroll;
	if (view < 0) view = 0;
	if (view > maxoff) view = maxoff;
	dirty = 1;
}

/* Left-click on the bar: arrows step a line, a track grab jumps the thumb to the
 * cursor and starts a drag. A click anywhere else ends any pending drag. */
static void on_click(int mx, int my)
{
	sb_drag = 0;
	int maxoff = nlines > rows ? nlines - rows : 0;
	if (maxoff <= 0) return;                                /* nothing to scroll  */
	int sbx = winw - GUI_SCROLL_W;
	int part = scrollbar_hit(sbx, 0, GUI_SCROLL_W, winh, mx, my);
	int scroll = maxoff - view;
	if (part == SCROLLBAR_UP)        sb_set(scroll - 3);
	else if (part == SCROLLBAR_DOWN) sb_set(scroll + 3);
	else if (part == SCROLLBAR_TRACK) {
		sb_drag = 1;
		sb_set(scrollbar_pos(sbx, 0, GUI_SCROLL_W, winh, mx, my, scroll, nlines, rows));
	}
}

/* Drag while the thumb is held: track the cursor's y. mx is pinned into the bar
 * so a little horizontal drift doesn't drop the grab. */
static void on_drag(int mx, int my)
{
	if (!sb_drag) return;
	int maxoff = nlines > rows ? nlines - rows : 0;
	int sbx = winw - GUI_SCROLL_W;
	sb_set(scrollbar_pos(sbx, 0, GUI_SCROLL_W, winh, sbx + GUI_SCROLL_W / 2, my,
	                     maxoff - view, nlines, rows));
}

static void on_resize(int w, int h)
{
	gfx_init(win_fb(), w, h);
	winw = w; winh = h;
	cols = (w - GUI_SCROLL_W) / FONT_W;   /* reserve the right edge for the bar    */
	if (cols > MAXCOLS) cols = MAXCOLS;
	if (cols < 1) cols = 1;
	rows = h / FONT_H; if (rows < 1) rows = 1;
	dirty = 1;
}

int main(void)
{
	win = win_create(720, 432, "terminal");
	if (win < 0) { puts("term: no window\n"); return 1; }
	gfx_init(win_fb(), 720, 432);
	win_resizable(win, 1);
	winw = 720; winh = 432;
	cols = (720 - GUI_SCROLL_W) / FONT_W; rows = 432 / FONT_H;

	grid_reset();
	tputs_c("OwOS terminal -- type 'help'.\n", FG);
	prompt();

	for (;;) {
		struct win_event ev;
		while (win_poll(win, &ev)) {
			switch (ev.type) {
			case WEV_CLOSE:  win_close(win); return 0;
			case WEV_KEY:    on_key((char)ev.a); break;
			case WEV_SCROLL: on_scroll(ev.a); break;
			case WEV_MOUSE:  on_click(ev.a, ev.b); break;
			case WEV_DRAG:   on_drag(ev.a, ev.b); break;
			case WEV_RESIZE: on_resize(ev.a, ev.b); break;
			}
		}
		if (dirty) redraw();
		win_wait(win);
	}
}
