/* editor.c — a windowed plain-text editor, running entirely in ring 3.
 *
 * The user-space successor to the kernel-side app_edit.c: it owns a window via
 * the windowing syscalls, draws into the shared buffer with the gfx lib, reads
 * input through win_poll events, and loads/saves files with readfile/writefile.
 * A File menu (New / Open / Save / Save As) is driven by the mouse, with a
 * bottom minibuffer prompt for typing a filename on Open / Save As.
 */
#include "owos.h"
#include "gfx.h"

static int W = 560, H = 360;    /* client size; grows/shrinks on resize */
#define MAXLINES  200
#define MAXCOLS   120
#define MENU_H    18
#define STATUS_H  18
#define ROW_H     16
#define COL_W     8
#define FNAME_MAX 64

enum { PROMPT_NONE, PROMPT_OPEN, PROMPT_SAVEAS };

static char  lines[MAXLINES][MAXCOLS];
static int   linelen[MAXLINES];
static int   nlines = 1;
static int   curx, cury, top_line;
static char  fname[FNAME_MAX];
static int   has_name, modified;

static int   prompt_mode;
static char  prompt_buf[FNAME_MAX];
static int   prompt_len;

static char  io_buf[MAXLINES * (MAXCOLS + 1)];

/* GUI widgets (shared gfx lib): the File menu and the status message. The hint
 * labels and key bytes are parallel to file_items, giving New/Open/Save the
 * Ctrl+N/O/S accelerators (Save As has none -- shown only in the dropdown). */
static const char *const file_items[] = { "New", "Open", "Save", "Save As" };
static const char *const file_hints[] = { "Ctrl+N", "Ctrl+O", "Ctrl+S", "" };
static const unsigned char file_keys[] = { KEY_NEW, KEY_OPEN, KEY_SAVE, 0 };
static struct menu       filemenu = { "File", file_items, 4, 0,
                                      file_hints, file_keys };
static struct status_bar sbar;

/* ------------------------------------------------------------- tiny helpers */

static void scopy(char *d, const char *s, int max)
{
	int i = 0;
	for (; s[i] && i < max - 1; i++)
		d[i] = s[i];
	d[i] = '\0';
}

static void set_status(const char *s) { status_set(&sbar, s); }

static int has_high;   /* 1 if the buffer holds any byte >= 0x80 (-> UTF-8) */

static void ed_clear(void)
{
	nlines = 1;
	linelen[0] = 0;
	lines[0][0] = 0;
	curx = cury = top_line = 0;
	modified = 0;
	has_high = 0;
}

/* ------------------------------------------------------------- buffer edits */

static void ed_insert(char c)
{
	if (linelen[cury] >= MAXCOLS - 1)
		return;
	for (int j = linelen[cury]; j > curx; j--)
		lines[cury][j] = lines[cury][j - 1];
	lines[cury][curx] = c;
	linelen[cury]++;
	curx++;
	modified = 1;
	if ((unsigned char)c >= 0x80) has_high = 1;
}

static void ed_newline(void)
{
	if (nlines >= MAXLINES)
		return;
	for (int i = nlines; i > cury + 1; i--) {
		linelen[i] = linelen[i - 1];
		for (int j = 0; j < linelen[i]; j++)
			lines[i][j] = lines[i - 1][j];
	}
	int tail = linelen[cury] - curx;
	for (int j = 0; j < tail; j++)
		lines[cury + 1][j] = lines[cury][curx + j];
	linelen[cury + 1] = tail;
	linelen[cury] = curx;
	nlines++;
	cury++;
	curx = 0;
	modified = 1;
}

static void ed_backspace(void)
{
	if (curx > 0) {
		for (int j = curx - 1; j < linelen[cury] - 1; j++)
			lines[cury][j] = lines[cury][j + 1];
		linelen[cury]--;
		curx--;
		modified = 1;
	} else if (cury > 0) {
		int prev = cury - 1, pl = linelen[prev], cl = linelen[cury];
		if (pl + cl >= MAXCOLS)
			return;
		for (int j = 0; j < cl; j++)
			lines[prev][pl + j] = lines[cury][j];
		linelen[prev] = pl + cl;
		for (int i = cury; i < nlines - 1; i++) {
			linelen[i] = linelen[i + 1];
			for (int j = 0; j < linelen[i]; j++)
				lines[i][j] = lines[i + 1][j];
		}
		nlines--;
		cury = prev;
		curx = pl;
		modified = 1;
	}
}

static void ed_delete(void)
{
	if (curx < linelen[cury]) {
		for (int j = curx; j < linelen[cury] - 1; j++)
			lines[cury][j] = lines[cury][j + 1];
		linelen[cury]--;
		modified = 1;
	} else if (cury < nlines - 1) {
		cury++;
		curx = 0;
		ed_backspace();
	}
}

static void clamp_curx(void)
{
	if (curx > linelen[cury]) curx = linelen[cury];
	if (curx < 0) curx = 0;
}

/* -------------------------------------------------------------- file I/O */

static void ed_load(const char *name)
{
	int n = readfile(name, io_buf, (int)sizeof(io_buf));
	if (n < 0) { set_status("open: not found"); return; }
	ed_clear();
	int li = 0, col = 0;
	for (int i = 0; i < n; i++) {
		char ch = io_buf[i];
		if (ch == '\n') {
			linelen[li] = col;
			if (li < MAXLINES - 1) { li++; col = 0; }
			else break;
		} else if (ch != '\r' && col < MAXCOLS - 1) {
			lines[li][col++] = ch;
			if ((unsigned char)ch >= 0x80) has_high = 1;
		}
	}
	linelen[li] = col;
	nlines = li + 1;
	curx = cury = top_line = 0;
	scopy(fname, name, sizeof fname);
	has_name = 1;
	modified = 0;
	set_status("opened");
}

static void ed_save(const char *name)
{
	int p = 0;
	for (int i = 0; i < nlines; i++) {
		for (int j = 0; j < linelen[i] && p < (int)sizeof(io_buf); j++)
			io_buf[p++] = lines[i][j];
		if (i < nlines - 1 && p < (int)sizeof(io_buf))
			io_buf[p++] = '\n';
	}
	if (writefile(name, io_buf, p) == 0) {
		scopy(fname, name, sizeof fname);
		has_name = 1;
		modified = 0;
		set_status("saved");
	} else {
		set_status("save failed");
	}
}

/* --------------------------------------------------------------- menu */

static void start_prompt(int mode)
{
	prompt_mode = mode;
	prompt_len = 0;
	prompt_buf[0] = 0;
}

static void menu_action(int item)
{
	switch (item) {
	case 0: ed_clear(); has_name = 0; fname[0] = 0;
	        set_status("new file"); break;
	case 1: start_prompt(PROMPT_OPEN); break;
	case 2: if (has_name) ed_save(fname);
	        else start_prompt(PROMPT_SAVEAS);
	        break;
	case 3: start_prompt(PROMPT_SAVEAS); break;
	}
}

static void confirm_prompt(void)
{
	prompt_buf[prompt_len] = 0;
	if (prompt_len == 0) { set_status("canceled"); return; }
	if (prompt_mode == PROMPT_OPEN) ed_load(prompt_buf);
	else                            ed_save(prompt_buf);
}

/* --------------------------------------------------------------- input */

static void key(int id, char c)
{
	if (prompt_mode) {
		if (c == '\n') { confirm_prompt(); prompt_mode = PROMPT_NONE; }
		else if (c == 27) { prompt_mode = PROMPT_NONE; set_status("canceled"); }
		else if (c == '\b') { if (prompt_len > 0) prompt_buf[--prompt_len] = 0; }
		else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F &&
		         prompt_len < (int)sizeof(prompt_buf) - 1)
			prompt_buf[prompt_len++] = c;
		return;
	}
	/* Menu accelerators (Ctrl+N/O/S): fire the item directly, like a click. */
	int sc = menu_shortcut(&filemenu, (unsigned char)c);
	if (sc >= 0) { filemenu.open = 0; menu_action(sc); return; }
	if (c == 27) { filemenu.open = 0; return; }

	switch (c) {
	case '\n':       ed_newline();                       break;
	case '\b':       ed_backspace();                     break;
	case KEY_DELETE: ed_delete();                        break;
	case KEY_LEFT:
		if (curx > 0) curx--;
		else if (cury > 0) { cury--; curx = linelen[cury]; }
		break;
	case KEY_RIGHT:
		if (curx < linelen[cury]) curx++;
		else if (cury < nlines - 1) { cury++; curx = 0; }
		break;
	case KEY_UP:   if (cury > 0) { cury--; clamp_curx(); }            break;
	case KEY_DOWN: if (cury < nlines - 1) { cury++; clamp_curx(); }   break;
	case KEY_HOME: curx = 0;                                          break;
	case KEY_END:  curx = linelen[cury];                              break;
	case KEY_F5:   /* reload the file from disk, discarding edits */
		if (has_name) ed_load(fname);
		else          set_status("nothing to reload");
		break;
	default:
		if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F)
			ed_insert(c);
		break;
	}
	(void)id;

	int visrows = (H - MENU_H - STATUS_H) / ROW_H;
	if (cury < top_line) top_line = cury;
	if (cury >= top_line + visrows) top_line = cury - visrows + 1;
	if (top_line < 0) top_line = 0;
}

static void click(int mx, int my)
{
	if (prompt_mode)
		return;
	if (my < MENU_H || filemenu.open) {       /* menu bar or open dropdown */
		int it = menu_click(&filemenu, mx, my, 2, 0);
		if (it >= 0)
			menu_action(it);
		return;
	}
	int ty0 = MENU_H, ty1 = H - STATUS_H;
	if (my >= ty0 && my < ty1) {
		int row = (my - ty0) / ROW_H + top_line;
		if (row >= nlines) row = nlines - 1;
		if (row < 0) row = 0;
		cury = row;
		curx = (mx - 2) / COL_W;
		clamp_curx();
	}
}

/* --------------------------------------------------------------- rendering */

static void apnd(char *buf, int *p, int cap, const char *s)
{
	for (int i = 0; s[i] && *p < cap - 1; i++)
		buf[(*p)++] = s[i];
	buf[*p] = 0;
}

static void apnd_uint(char *buf, int *p, int cap, unsigned v)
{
	char t[12];
	int n = 0;
	if (v == 0) t[n++] = '0';
	while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
	while (n && *p < cap - 1) buf[(*p)++] = t[--n];
	buf[*p] = 0;
}

static void render(void)
{
	unsigned txt_bg = 0x0d1117;

	/* Menu bar background (the File menu itself is drawn last, on top). */
	gfx_fill_rect(0, 0, W, MENU_H, 0x232a36);

	/* Text area. */
	int ty0 = MENU_H, th = H - MENU_H - STATUS_H;
	gfx_fill_rect(0, ty0, W, th, txt_bg);
	int visrows = th / ROW_H;
	int maxcol = (W - 4) / COL_W;
	for (int r = 0; r < visrows; r++) {
		int ln = top_line + r;
		if (ln >= nlines) break;
		int ry = ty0 + r * ROW_H;
		int cols = linelen[ln] < maxcol ? linelen[ln] : maxcol;
		for (int j = 0; j < cols; j++)
			gfx_glyph(2 + j * COL_W, ry, (unsigned char)lines[ln][j],
			          0xc8d0dc, txt_bg);
	}

	/* Caret. */
	if (cury >= top_line && cury < top_line + visrows) {
		int cc = curx < maxcol ? curx : maxcol;
		gfx_fill_rect(2 + cc * COL_W, ty0 + (cury - top_line) * ROW_H, 2, ROW_H,
		              0x66ddff);
	}

	/* Status / prompt bar, rendered with the gfx status_bar widget (it owns the
	 * bar background + the left-aligned message). */
	int sy = H - STATUS_H;
	struct status_bar bar;
	char line[64];
	int p = 0;
	line[0] = 0;
	if (prompt_mode) {
		apnd(line, &p, sizeof line,
		     prompt_mode == PROMPT_OPEN ? "Open: " : "Save as: ");
		apnd(line, &p, sizeof line, prompt_buf);
		apnd(line, &p, sizeof line, "_");
		status_set(&bar, line);
		status_draw(&bar, 0, sy, W, 0xffe08a, 0x161d28);
	} else {
		apnd(line, &p, sizeof line, has_name ? fname : "[no name]");
		if (modified) apnd(line, &p, sizeof line, " *");
		apnd(line, &p, sizeof line, "  -  ");
		apnd(line, &p, sizeof line, sbar.msg);
		status_set(&bar, line);
		status_draw(&bar, 0, sy, W, 0x9fb0c4, 0x161d28);

		/* Cursor position + encoding, right-aligned over the bar the widget
		 * just drew (status_draw is left-only). */
		char rstat[40];
		int rp = 0;
		rstat[0] = 0;
		apnd(rstat, &rp, sizeof rstat, "Ln ");
		apnd_uint(rstat, &rp, sizeof rstat, (unsigned)(cury + 1));
		apnd(rstat, &rp, sizeof rstat, ", Col ");
		apnd_uint(rstat, &rp, sizeof rstat, (unsigned)(curx + 1));
		apnd(rstat, &rp, sizeof rstat, has_high ? "  UTF-8" : "  ASCII");
		(void)rp;
		int rx = W - 6 - gfx_ui_width(rstat);
		if (rx < 6) rx = 6;
		gfx_ui_text(rx, sy + (GUI_BAR_H - UIFONT_H) / 2, rstat, 0x9fb0c4);
	}

	/* The File menu, drawn last so its dropdown overlays the text. */
	menu_draw(&filemenu, 2, 0);
}

int main(int argc, char **argv)
{
	ed_clear();
	set_status("new file");
	int id = win_create(W, H, "edit");
	if (id < 0) {
		puts("editor: win_create failed\n");
		return 1;
	}
	gfx_init(win_fb(), W, H);
	win_cursor(id, WIN_CURSOR_TEXT);   /* I-beam over the text area */

	/* Launched with a path (e.g. double-clicked in the file explorer): open
	 * it straight away instead of starting on a blank buffer. */
	if (argc > 1 && argv[1] && argv[1][0])
		ed_load(argv[1]);

	int need = 1;
	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev)) {
			need = 1;
			if (ev.type == WEV_CLOSE) { win_close(id); return 0; }
			else if (ev.type == WEV_KEY) key(id, (char)ev.a);
			else if (ev.type == WEV_MOUSE) click(ev.a, ev.b);
			else if (ev.type == WEV_RESIZE) {
				W = ev.a; H = ev.b;
				gfx_init(win_fb(), W, H);   /* new buffer + pitch */
			}
		}
		if (need) {
			render();
			win_present(id);
			need = 0;
		}
		win_wait(id);
	}
}
