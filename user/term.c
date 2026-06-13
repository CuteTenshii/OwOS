/* term.c — a ring-3 terminal: a resizable, scrollable window hosting a shell.
 *
 * A text grid drawn with the 8x16 font (gfx.c) over a scrollback ring buffer.
 * Keys arrive as WEV_KEY events; the shell line-edits in the grid. Commands are
 * served three ways:
 *   - filesystem/string builtins run in ring 3 over syscalls (they use the
 *     shell's own cwd);
 *   - a /bin program is launched detached (GUI apps open their own window);
 *   - anything else is handed to the kernel shell's builtin via shell_cmd()
 *     (journal, neofetch, lspci, cpuinfo, date, ...) and its text is rendered
 *     here — so the user-space terminal gets every kernel command for free.
 *
 * The window resizes (WEV_RESIZE recomputes the grid) and scrolls (mouse wheel
 * -> WEV_SCROLL moves a view offset over the scrollback).
 */

#include "owos.h"
#include "gfx.h"

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
	gfx_fill_rect(0, 0, cols * FONT_W, rows * FONT_H, BG);
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
	tputs("ring-3 builtins: help clear echo pwd cd ls cat write mkdir rmdir rm cp mv stat exit\n");
	tputs("kernel commands (journal neofetch lspci cpuinfo date meminfo tasks ...) run via the\n");
	tputs("kernel shell; /bin programs (editor files clock browser ...) open their own window.\n");
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

static void bi_write(int argc, char **argv)
{
	if (argc < 3) { tputs("usage: write <file> <text...>\n"); return; }
	int len = 0;
	for (int i = 2; i < argc; i++) {
		for (const char *a = argv[i]; *a && len < (int)sizeof iobuf - 1; a++) iobuf[len++] = *a;
		if (i < argc - 1 && len < (int)sizeof iobuf - 1) iobuf[len++] = ' ';
	}
	iobuf[len++] = '\n';
	char p[256]; abspath(argv[1], p);
	if (writefile(p, iobuf, len) < 0) err("write: failed", 0);
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

/* Run a command. The kernel bridge handles builtins AND ring-3 CLI tools
 * (neofetch, curl): it spawns the program, captures its output, and returns the
 * text to render here — so a CLI program shows up inline in the terminal. It
 * returns -1 for a GUI /bin app, which we then launch detached so it opens its
 * own window. */
static void run_other(const char *orig, int argc, char **argv)
{
	if (cmd_begin(orig) == 0) {                          /* builtin or CLI tool   */
		/* Stream the output: render each chunk as it is produced and present,
		 * so a long or voluminous command (curl, ping, journal) shows up live
		 * instead of freezing the window until it finishes. Stay responsive to
		 * window events meanwhile -- a close aborts the command (cmd_abort kills
		 * a CLI child) and we drain to EOF so the runner resets before we exit. */
		int closing = 0;
		for (;;) {
			struct win_event ev;
			while (win_poll(win, &ev))          /* stay closable mid-command   */
				if (ev.type == WEV_CLOSE && !closing) { closing = 1; cmd_abort(); }
			int n = cmd_read(iobuf, sizeof iobuf);
			if (n > 0) {
				if (!closing) { for (int i = 0; i < n; i++) tputc(iobuf[i]); redraw(); }
			} else if (n < 0) {
				break;                              /* end of output          */
			} else {
				if (dirty && !closing) redraw();    /* running; nothing yet    */
				msleep(8);
			}
		}
		if (closing) { win_close(win); exit(0); }
		return;
	}

	char path[256];
	if (argv[0][0] == '/') strcpy(path, argv[0]);
	else { strcpy(path, "/bin/"); strcat(path, argv[0]); }
	struct statinfo st;
	if (stat(path, &st) == 0 && !st.is_dir) {           /* a detached GUI app    */
		if (spawn(path, argc > 1 ? argv[1] : 0) < 0) err("failed to launch", 0);
		return;
	}
	err(argv[0], ": command not found");
}

static void run(const char *orig)
{
	char buf[256]; strcpy(buf, orig);
	char *argv[16];
	int argc = 0, i = 0;
	while (buf[i] && argc < 16) {
		while (buf[i] == ' ') buf[i++] = '\0';
		if (!buf[i]) break;
		argv[argc++] = &buf[i];
		while (buf[i] && buf[i] != ' ') i++;
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
	else if (!strcmp(c, "write")) bi_write(argc, argv);
	else if (!strcmp(c, "cp"))    bi_cp(argc, argv);
	else if (!strcmp(c, "stat"))  bi_stat(argc, argv);
	else if (!strcmp(c, "mkdir")) { for (int k = 1; k < argc; k++) { char p[256]; abspath(argv[k], p); if (mkdir(p) < 0) err("mkdir: failed: ", argv[k]); } }
	else if (!strcmp(c, "rmdir")) { for (int k = 1; k < argc; k++) { char p[256]; abspath(argv[k], p); if (rmdir(p) < 0) err("rmdir: failed: ", argv[k]); } }
	else if (!strcmp(c, "rm"))    { for (int k = 1; k < argc; k++) { char p[256]; abspath(argv[k], p); if (unlink(p) < 0) err("rm: failed: ", argv[k]); } }
	else if (!strcmp(c, "mv"))    { if (argc < 3) tputs("usage: mv <src> <dst>\n"); else { char a[256], b[256]; abspath(argv[1], a); abspath(argv[2], b); if (rename(a, b) < 0) err("mv: failed", 0); } }
	else if (!strcmp(c, "exit"))  { win_close(win); exit(0); }
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
	case '\b':
		if (lpos > 0) { for (int i = lpos - 1; i < llen - 1; i++) line[i] = line[i + 1]; llen--; lpos--; line_redraw(); }
		break;
	case KEY_LEFT:  if (lpos > 0)    { lpos--; line_redraw(); } break;
	case KEY_RIGHT: if (lpos < llen) { lpos++; line_redraw(); } break;
	case KEY_HOME:  lpos = 0;    line_redraw(); break;
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

static void on_resize(int w, int h)
{
	gfx_init(win_fb(), w, h);
	cols = w / FONT_W; if (cols > MAXCOLS) cols = MAXCOLS; if (cols < 1) cols = 1;
	rows = h / FONT_H; if (rows < 1) rows = 1;
	dirty = 1;
}

int main(void)
{
	win = win_create(720, 432, "terminal");
	if (win < 0) { puts("term: no window\n"); return 1; }
	gfx_init(win_fb(), 720, 432);
	win_resizable(win, 1);
	cols = 720 / FONT_W; rows = 432 / FONT_H;

	grid_reset();
	tputs_c("OwOS terminal (ring 3) -- type 'help'.\n", FG);
	prompt();

	for (;;) {
		struct win_event ev;
		while (win_poll(win, &ev)) {
			switch (ev.type) {
			case WEV_CLOSE:  win_close(win); return 0;
			case WEV_KEY:    on_key((char)ev.a); break;
			case WEV_SCROLL: on_scroll(ev.a); break;
			case WEV_RESIZE: on_resize(ev.a, ev.b); break;
			}
		}
		if (dirty) redraw();
		win_wait(win);
	}
}
