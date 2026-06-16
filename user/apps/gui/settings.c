/* settings.c — a desktop Settings app for OwOS.
 *
 * A master/detail window: a gfx `sidebar` widget down the left edge lists the
 * setting categories, the right pane shows the selected category's controls.
 * Each category maps to real kernel state reached through the ordinary file
 * interface (no bespoke syscalls): device tunables live in the sysfs tree,
 * system info in /proc — the Unix split.
 *
 *   Mouse / Touchpad  pointer speed  -> /sys/class/input/mouse0/speed  (1..5)
 *   Keyboard          layout         -> /sys/class/input/kbd0/layout   ("us"/"fr")
 *   About             system info    -> /sys/class/dmi/id, /proc/cpuinfo, ...
 *
 * The sidebar owns its width as a resizable property (drag the divider). A `seq`
 * counter bumps on every change so the loop repaints only when something moved.
 */
#include "owos.h"
#include "gfx.h"

static int W = 480, H = 320;        /* client size; changes on resize */
#define HEADER_H 22                 /* sidebar title strip                  */
#define ROW_H    26                 /* sidebar category row height          */
#define SIDEBAR_W   150
#define SIDEBAR_MIN 120
#define SIDEBAR_MAX 220

/* Theme (matches the Files explorer). */
#define BG_PANEL   0x12161eu
#define BG_SIDE    0x0e1218u
#define BG_HEADER  0x161d28u
#define BG_DIVIDER 0x05070au
#define BG_SEL     0x24405cu
#define FG_TITLE   0x8aa0b8u
#define FG_TEXT    0xc8d0dcu
#define FG_DIM     0x8aa0b8u
#define FG_WHITE   0xffffffu

static struct sidebar sb;

enum { CAT_MOUSE = 0, CAT_KEYBOARD, CAT_SOUND, CAT_ABOUT, NCATS };
static const char *const cat_name[NCATS] = {
	"Mouse / Touchpad", "Keyboard", "Sound", "About",
};

static int      sel = CAT_MOUSE;    /* selected category */
static unsigned seq = 1;            /* bump to request a repaint */

/* Live state mirrored from /proc, re-read when a category is entered. */
static int  mouse_level = 3;        /* pointer-speed level 1..5 */
static char kbd_layout[8] = "us";   /* "us" / "fr"              */

#define MAX_OUTS 6
static char sound_name[MAX_OUTS][20]; /* audio output device names            */
static int  sound_n;                  /* how many outputs the codec exposes   */
static int  sound_cur;                /* index of the active output           */

static char  about_line[10][72];    /* About: one formatted line per row */
static int   about_n;

/* ------------------------------------------------------------- small helpers */

static void uitoa(unsigned v, char *out)
{
	char t[12];
	int n = 0;
	if (v == 0) { out[0] = '0'; out[1] = 0; return; }
	while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
	int i = 0;
	while (n) out[i++] = t[--n];
	out[i] = 0;
}

/* Read a /proc file into `buf` (NUL-terminated, trailing newline trimmed).
 * Returns the length, or 0 if the read failed / file was empty. */
static int read_proc(const char *name, char *buf, int max)
{
	int n = readfile(name, buf, max - 1);
	if (n < 0) n = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
	buf[n] = 0;
	return n;
}

/* Copy the text after "<key> " on its line in `buf` into out (trimmed at EOL).
 * Returns 1 if the key was found at a line start, else 0. */
static int field(const char *buf, const char *key, char *out, int max)
{
	for (const char *p = buf; *p; ) {
		int i = 0;
		while (key[i] && p[i] == key[i]) i++;
		if (!key[i] && p[i] == ' ') {
			const char *v = p + i + 1;
			int n = 0;
			while (v[n] && v[n] != '\n' && n < max - 1) { out[n] = v[n]; n++; }
			out[n] = 0;
			return 1;
		}
		while (*p && *p != '\n') p++;
		if (*p == '\n') p++;
	}
	return 0;
}

/* ------------------------------------------------------------- state loading */

static void load_mouse(void)
{
	char t[16];
	if (read_proc("/sys/class/input/mouse0/speed", t, sizeof t) > 0) {
		int v = 0;
		for (const char *p = t; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
		if (v >= 1 && v <= 5) mouse_level = v;
	}
}

static void load_kbd(void)
{
	char t[16];
	if (read_proc("/sys/class/input/kbd0/layout", t, sizeof t) > 0) {
		t[sizeof kbd_layout - 1] = 0;
		strcpy(kbd_layout, t);
	}
}

static void load_sound(void)
{
	char buf[256];
	sound_n = 0;
	sound_cur = 0;

	int len = readfile("/sys/class/sound/card0/outputs", buf, sizeof buf - 1);
	if (len < 0) len = 0;
	buf[len] = 0;
	for (const char *p = buf; *p && sound_n < MAX_OUTS; ) {
		int i = 0;
		while (*p && *p != '\n' && i < (int)sizeof sound_name[0] - 1)
			sound_name[sound_n][i++] = *p++;
		sound_name[sound_n][i] = 0;
		while (*p && *p != '\n') p++;       /* skip an over-long name's tail */
		if (*p == '\n') p++;
		if (i > 0) sound_n++;
	}

	char cur[24];
	if (read_proc("/sys/class/sound/card0/output", cur, sizeof cur) > 0)
		for (int i = 0; i < sound_n; i++)
			if (streq(sound_name[i], cur)) { sound_cur = i; break; }
}

static void aline(const char *s)
{
	if (about_n >= 10) return;
	int i = 0;
	while (s[i] && i < 71) { about_line[about_n][i] = s[i]; i++; }
	about_line[about_n][i] = 0;
	about_n++;
}

static void load_about(void)
{
	char buf[512], v[96], v2[32], line[72];
	about_n = 0;

	aline("OwOS");
	aline("");

	if (read_proc("/sys/class/dmi/id/product_name", v, sizeof v) > 0) {
		strcpy(line, "Host: "); strcat(line, v); aline(line);
	}

	if (read_proc("/proc/cpuinfo", buf, sizeof buf) > 0) {
		strcpy(line, "CPU: ");
		if (field(buf, "model", v, sizeof v)) strcat(line, v);
		else                                  strcat(line, "unknown");
		if (field(buf, "mhz", v2, sizeof v2)) { strcat(line, " @ "); strcat(line, v2); strcat(line, " MHz"); }
		aline(line);
	}

	if (read_proc("/proc/meminfo", buf, sizeof buf) > 0) {
		unsigned used = 0, total = 0;
		if (field(buf, "used", v, sizeof v))
			for (const char *p = v; *p >= '0' && *p <= '9'; p++) used = used * 10 + (*p - '0');
		if (field(buf, "total", v, sizeof v))
			for (const char *p = v; *p >= '0' && *p <= '9'; p++) total = total * 10 + (*p - '0');
		strcpy(line, "Memory: ");
		uitoa(used / 1024, v); strcat(line, v); strcat(line, " / ");
		uitoa(total / 1024, v); strcat(line, v); strcat(line, " MiB");
		aline(line);
	}

	if (read_proc("/sys/firmware/type", v, sizeof v) > 0) {
		strcpy(line, "Firmware: "); strcat(line, v); aline(line);
	}

	if (read_proc("/proc/uptime", v, sizeof v) > 0) {
		strcpy(line, "Uptime: "); strcat(line, v); strcat(line, " s"); aline(line);
	}
}

/* Re-read the state the given category displays so it reflects the live kernel
 * value (it may have changed via the terminal or hotkeys since last shown). */
static void load_category(int cat)
{
	if      (cat == CAT_MOUSE)    load_mouse();
	else if (cat == CAT_KEYBOARD) load_kbd();
	else if (cat == CAT_SOUND)    load_sound();
	else if (cat == CAT_ABOUT)    load_about();
}

/* ------------------------------------------------------------- apply changes */

static void set_mouse_level(int level)
{
	char t[4];
	if (level < 1) level = 1;
	if (level > 5) level = 5;
	uitoa((unsigned)level, t);
	if (writefile("/sys/class/input/mouse0/speed", t, strlen(t)) == 0)
		mouse_level = level;
}

static void set_kbd_layout(const char *name)
{
	if (writefile("/sys/class/input/kbd0/layout", name, strlen(name)) == 0)
		strcpy(kbd_layout, name);
}

static void set_sound_output(int i)
{
	char t[4];
	if (i < 0 || i >= sound_n) return;
	uitoa((unsigned)i, t);
	if (writefile("/sys/class/sound/card0/output", t, strlen(t)) == 0)
		sound_cur = i;
}

/* ------------------------------------------------------------- layout / paint */

/* Width actually rendered this frame: the widget property, capped so the detail
 * pane always keeps ~200px on a narrow window. */
static int sidebar_w(void)
{
	int sbw = sb.w;
	if (sbw > W - 200) sbw = W - 200;
	if (sbw < 0) sbw = 0;
	return sbw;
}

/* Mouse speed option buttons: 5 cells starting at (ox,oy). The hit-test mirrors
 * this geometry. */
#define SPD_BW  44
#define SPD_GAP 8
static int spd_ox(int cx) { return cx + 16; }
static int spd_oy(void)   { return 80; }
static int spd_bx(int cx, int i) { return spd_ox(cx) + i * (SPD_BW + SPD_GAP); }

/* Keyboard layout option buttons. */
static const struct { const char *name, *label; } kbd_opts[] = {
	{ "us", "US English" }, { "fr", "French" },
};
#define NKBD ((int)(sizeof kbd_opts / sizeof kbd_opts[0]))
#define KBD_BW  120
static int kbd_oy(void)        { return 80; }
static int kbd_by(int i)       { return kbd_oy() + i * (GUI_BTN_H + 8); }

static void draw_mouse(int cx, int cw)
{
	(void)cw;
	gfx_ui_text(cx + 16, 16, "Mouse / Touchpad", FG_WHITE);
	gfx_ui_text(cx + 16, 52, "Pointer speed", FG_TEXT);

	static const char *const lbl[5] = { "1", "2", "3", "4", "5" };
	for (int i = 0; i < 5; i++) {
		int st = (mouse_level == i + 1) ? BTN_PRESSED : BTN_NORMAL;
		button_draw(spd_bx(cx, i), spd_oy(), SPD_BW, GUI_BTN_H, lbl[i], st);
	}
	int oy2 = spd_oy() + GUI_BTN_H + 6;
	gfx_ui_text(spd_ox(cx), oy2, "Slow", FG_DIM);
	const char *fast = "Fast";
	gfx_ui_text(spd_bx(cx, 4) + SPD_BW - gfx_ui_width(fast), oy2, fast, FG_DIM);

	gfx_ui_text(cx + 16, oy2 + 30,
	            "Higher moves the pointer farther per motion.", FG_DIM);
}

static void draw_keyboard(int cx, int cw)
{
	(void)cw;
	gfx_ui_text(cx + 16, 16, "Keyboard", FG_WHITE);
	gfx_ui_text(cx + 16, 52, "Layout", FG_TEXT);

	for (int i = 0; i < NKBD; i++) {
		int st = streq(kbd_layout, kbd_opts[i].name) ? BTN_PRESSED : BTN_NORMAL;
		button_draw(cx + 16, kbd_by(i), KBD_BW, GUI_BTN_H, kbd_opts[i].label, st);
	}
	gfx_ui_text(cx + 16, kbd_by(NKBD) + 12,
	            "Affects the whole system.", FG_DIM);
}

/* Sound output buttons, one per available output device. */
#define SND_BW 160
static int snd_by(int i) { return 80 + i * (GUI_BTN_H + 8); }

static void draw_sound(int cx, int cw)
{
	(void)cw;
	gfx_ui_text(cx + 16, 16, "Sound", FG_WHITE);
	gfx_ui_text(cx + 16, 52, "Output device", FG_TEXT);

	if (sound_n == 0) {
		gfx_ui_text(cx + 16, 84, "No audio output device.", FG_DIM);
		return;
	}
	for (int i = 0; i < sound_n; i++) {
		int st = (i == sound_cur) ? BTN_PRESSED : BTN_NORMAL;
		button_draw(cx + 16, snd_by(i), SND_BW, GUI_BTN_H, sound_name[i], st);
	}
	gfx_ui_text(cx + 16, snd_by(sound_n) + 12,
	            "Routes playback to the selected output.", FG_DIM);
}

/* Draw `s` at (x,y), truncated to fit `maxw` pixels so a long line (a verbose
 * CPU brand string) can't spill past the detail pane's right edge. */
static void draw_fit(int x, int y, const char *s, int maxw, unsigned fg)
{
	if (gfx_ui_width(s) <= maxw) { gfx_ui_text(x, y, s, fg); return; }
	char t[80];
	int i = 0;
	while (s[i] && i < 79) {
		t[i] = s[i]; t[i + 1] = 0;
		if (gfx_ui_width(t) > maxw) { t[i] = 0; break; }
		i++;
	}
	gfx_ui_text(x, y, t, fg);
}

static void draw_about(int cx, int cw)
{
	int maxw = cw - 32;
	for (int i = 0; i < about_n; i++)
		draw_fit(cx + 16, 16 + i * 20, about_line[i], maxw,
		         i == 0 ? FG_WHITE : FG_TEXT);
}

static void render(void)
{
	int sbw = sidebar_w();
	gfx_clear(BG_PANEL);

	/* Sidebar: the widget paints the panel base, divider and resize grip; we
	 * overdraw the title strip and the category rows. */
	sidebar_draw(&sb, 0, 0, sbw, H, BG_SIDE, BG_DIVIDER);
	gfx_fill_rect(0, 0, sbw, HEADER_H, BG_HEADER);
	gfx_ui_text(8, 2, "Settings", FG_TITLE);
	for (int i = 0; i < NCATS; i++) {
		int ry = HEADER_H + i * ROW_H;
		int here = (i == sel);
		if (here) gfx_fill_rect(0, ry, sbw, ROW_H, BG_SEL);
		gfx_ui_text(8, ry + (ROW_H - UIFONT_H) / 2, cat_name[i],
		            here ? FG_WHITE : FG_TEXT);
	}

	/* Detail pane. */
	int cx = sbw + 1, cw = W - sbw - 1;
	gfx_fill_rect(cx, 0, cw, H, BG_PANEL);
	if      (sel == CAT_MOUSE)    draw_mouse(cx, cw);
	else if (sel == CAT_KEYBOARD) draw_keyboard(cx, cw);
	else if (sel == CAT_SOUND)    draw_sound(cx, cw);
	else                          draw_about(cx, cw);
}

/* ------------------------------------------------------------- input */

static void click(int id, int mx, int my)
{
	(void)id;
	int sbw = sidebar_w();

	/* Divider grip: grab it to resize the sidebar (consumes the press). */
	if (sidebar_press(&sb, 0, 0, sbw, H, mx, my)) { seq++; return; }

	if (mx < sbw) {                       /* a category row */
		if (my < HEADER_H) return;
		int i = (my - HEADER_H) / ROW_H;
		if (i >= 0 && i < NCATS && i != sel) {
			sel = i;
			load_category(sel);
			seq++;
		}
		return;
	}

	/* Detail-pane controls. */
	int cx = sbw + 1;
	if (sel == CAT_MOUSE) {
		for (int i = 0; i < 5; i++)
			if (button_hit(spd_bx(cx, i), spd_oy(), SPD_BW, GUI_BTN_H, mx, my)) {
				set_mouse_level(i + 1); seq++; return;
			}
	} else if (sel == CAT_KEYBOARD) {
		for (int i = 0; i < NKBD; i++)
			if (button_hit(cx + 16, kbd_by(i), KBD_BW, GUI_BTN_H, mx, my)) {
				set_kbd_layout(kbd_opts[i].name); seq++; return;
			}
	} else if (sel == CAT_SOUND) {
		for (int i = 0; i < sound_n; i++)
			if (button_hit(cx + 16, snd_by(i), SND_BW, GUI_BTN_H, mx, my)) {
				set_sound_output(i); seq++; return;
			}
	}
}

static void drag(int mx)
{
	if (sidebar_drag(&sb, 0, mx))
		seq++;
}

/* Up/Down move the selected category; the change re-reads its live state. */
static void key(int k)
{
	if (k == KEY_UP || k == KEY_DOWN) {
		sel += (k == KEY_DOWN) ? 1 : -1;
		if (sel < 0) sel = NCATS - 1;
		if (sel >= NCATS) sel = 0;
		load_category(sel);
		seq++;
	}
}

int main(void)
{
	int id = win_create(W, H, "settings");
	if (id < 0) { puts("settings: no window\n"); return 1; }
	gfx_init(win_fb(), W, H);
	sidebar_init(&sb, SIDEBAR_W, SIDEBAR_MIN, SIDEBAR_MAX);
	win_resizable(id, 1);

	/* Prime every category's state once so switching is instant. */
	load_mouse();
	load_kbd();
	load_sound();
	load_about();

	int last = -1;
	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev)) {
			if      (ev.type == WEV_CLOSE)  { win_close(id); return 0; }
			else if (ev.type == WEV_MOUSE)  click(id, ev.a, ev.b);
			else if (ev.type == WEV_DRAG)   drag(ev.a);
			else if (ev.type == WEV_KEY)    key(ev.a);
			else if (ev.type == WEV_RESIZE) {
				W = ev.a; H = ev.b;
				gfx_init(win_fb(), W, H);
				seq++;
			}
		}
		if ((int)seq != last) { render(); win_present(id); last = seq; }
		win_wait(id);
	}
}
