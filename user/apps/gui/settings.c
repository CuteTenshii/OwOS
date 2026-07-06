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
 *   Display           resolution     -> /sys/class/graphics/fb0/{mode,modes}
 *                     refresh rate   -> /sys/class/graphics/fb0/{rate,refresh}
 *   About             system info    -> /sys/class/dmi/id, /proc/cpuinfo, ...
 *
 * A category owns a list of `control`s (label + dropdown + apply callback), laid
 * out top to bottom, so a pane can carry several settings (Display has two). The
 * sidebar owns its width as a resizable property (drag the divider). A `seq`
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

enum { CAT_MOUSE = 0, CAT_KEYBOARD, CAT_SOUND, CAT_DISPLAY, CAT_ABOUT, NCATS };
static const char *const cat_name[NCATS] = {
	"Mouse / Touchpad", "Keyboard", "Sound", "Display", "About",
};

static int      sel = CAT_MOUSE;    /* selected category */
static unsigned seq = 1;            /* bump to request a repaint */

/* Live state mirrored from /proc, re-read when a category is entered. Each
 * category presents its choices through one select (dropdown) widget. */
static int  mouse_level = 3;        /* pointer-speed level 1..5 */
static char kbd_layout[8] = "us";   /* "us" / "fr"              */

/* Pointer-speed levels 1..5, by their dropdown labels. */
static const char *const mouse_opts[5] = {
	"Slowest", "Slow", "Normal", "Fast", "Fastest",
};
static struct select_box mouse_sel;

/* Keyboard layouts: sysfs name + the label shown in the dropdown. */
static const struct { const char *name, *label; } kbd_opts[] = {
	{ "us", "US English" }, { "fr", "French" },
};
#define NKBD ((int)(sizeof kbd_opts / sizeof kbd_opts[0]))
static const char *kbd_opt_lbl[NKBD];   /* select_box wants a char* array */
static struct select_box kbd_sel;

#define MAX_OUTS 6
static char sound_name[MAX_OUTS][20]; /* audio output device names            */
static const char *sound_opts[MAX_OUTS]; /* select_box array (-> sound_name)   */
static int  sound_n;                  /* how many outputs the codec exposes   */
static int  sound_cur;                /* index of the active output           */
static struct select_box sound_sel;

#define MAX_MODES 16
static char disp_label[MAX_MODES][16]; /* "1280 x 720" per offered mode        */
static const char *disp_opts[MAX_MODES]; /* select_box option array (-> labels) */
static int  disp_w[MAX_MODES], disp_h[MAX_MODES];
static int  disp_n;                    /* how many modes are offered           */
static int  disp_cur;                  /* index of the active mode, -1 if none */
static int  disp_fixed;                /* 1 = resolution can't be changed      */
static struct select_box disp_sel;

/* Refresh rate: a second Display control over /sys/class/graphics/fb0/rate (the
 * compositor's frame-pacing target). The list is capped at the panel's real
 * refresh (fb0/refresh, from EDID) — above it there's no vblank to sync to. */
#define MAX_RATES 8
static char rate_label[MAX_RATES][12]; /* "60 Hz" per offered rate             */
static const char *rate_opts[MAX_RATES];
static int  rate_val[MAX_RATES];       /* the numeric Hz behind each option    */
static int  rate_n;
static int  rate_cur;                  /* index of the active rate, -1 if none */
static struct select_box rate_sel;

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

static int patoi(const char *s)
{
	int v = 0;
	while (*s == ' ') s++;
	while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	return v;
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
	select_init(&mouse_sel, mouse_opts, 5, mouse_level - 1);
}

static void load_kbd(void)
{
	char t[16];
	if (read_proc("/sys/class/input/kbd0/layout", t, sizeof t) > 0) {
		t[sizeof kbd_layout - 1] = 0;
		strcpy(kbd_layout, t);
	}
	int cur = 0;
	for (int i = 0; i < NKBD; i++) {
		kbd_opt_lbl[i] = kbd_opts[i].label;
		if (streq(kbd_layout, kbd_opts[i].name)) cur = i;
	}
	select_init(&kbd_sel, kbd_opt_lbl, NKBD, cur);
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
		if (i > 0) { sound_opts[sound_n] = sound_name[sound_n]; sound_n++; }
	}

	char cur[24];
	if (read_proc("/sys/class/sound/card0/output", cur, sizeof cur) > 0)
		for (int i = 0; i < sound_n; i++)
			if (streq(sound_name[i], cur)) { sound_cur = i; break; }

	select_init(&sound_sel, sound_opts, sound_n, sound_cur);
}

/* Build "WxH" (compact, for the sysfs write) and "W x H" (the menu label). */
static void mode_str(int w, int h, char *out, int spaced)
{
	char t[12];
	int i = 0;
	uitoa((unsigned)w, t);
	for (int j = 0; t[j]; j++) out[i++] = t[j];
	if (spaced) { out[i++] = ' '; out[i++] = 'x'; out[i++] = ' '; }
	else        out[i++] = 'x';
	uitoa((unsigned)h, t);
	for (int j = 0; t[j]; j++) out[i++] = t[j];
	out[i] = 0;
}

/* Parse a leading "WxH" out of `s` into *w,*h (0 if absent). */
static void parse_wh(const char *s, int *w, int *h)
{
	int a = 0, b = 0;
	while (*s >= '0' && *s <= '9') a = a * 10 + (*s++ - '0');
	if (*s == 'x' || *s == 'X') s++;
	while (*s >= '0' && *s <= '9') b = b * 10 + (*s++ - '0');
	*w = a; *h = b;
}

static void load_display(void)
{
	char buf[256];
	disp_n = 0; disp_cur = -1;

	int len = readfile("/sys/class/graphics/fb0/modes", buf, sizeof buf - 1);
	if (len < 0) len = 0;
	buf[len] = 0;
	for (const char *p = buf; *p && disp_n < MAX_MODES; ) {
		int w, h;
		parse_wh(p, &w, &h);
		while (*p && *p != '\n') p++;
		if (*p == '\n') p++;
		if (w > 0 && h > 0) {
			disp_w[disp_n] = w; disp_h[disp_n] = h;
			mode_str(w, h, disp_label[disp_n], 1);
			disp_opts[disp_n] = disp_label[disp_n];
			disp_n++;
		}
	}

	char cur[24];
	int cw = 0, ch = 0;
	if (read_proc("/sys/class/graphics/fb0/mode", cur, sizeof cur) > 0)
		parse_wh(cur, &cw, &ch);
	for (int i = 0; i < disp_n; i++)
		if (disp_w[i] == cw && disp_h[i] == ch) { disp_cur = i; break; }

	/* A single offered mode means the kernel can't reprogram it (an adopted
	 * GOP/bootloader framebuffer) — it just reports the current one. */
	disp_fixed = (disp_n <= 1);
	select_init(&disp_sel, disp_opts, disp_n, disp_cur < 0 ? 0 : disp_cur);
}

/* Append hz to the offered-rate list as "N Hz" if there's room and it's new. */
static void add_rate(int hz)
{
	if (hz <= 0 || rate_n >= MAX_RATES) return;
	for (int i = 0; i < rate_n; i++)
		if (rate_val[i] == hz) return;
	char *o = rate_label[rate_n];
	uitoa((unsigned)hz, o);
	int k = strlen(o);
	o[k++] = ' '; o[k++] = 'H'; o[k++] = 'z'; o[k] = 0;
	rate_val[rate_n]  = hz;
	rate_opts[rate_n] = rate_label[rate_n];
	rate_n++;
}

static void load_rate(void)
{
	char t[24];
	rate_n = 0; rate_cur = -1;

	/* Cap the list at the panel's real refresh (EDID); without EDID (QEMU) fall
	 * back to the safe 30/60 pair. Above the panel rate there's no vblank to sync
	 * to, so a higher pacing target would only tear. */
	int cap = 0;
	if (read_proc("/sys/class/graphics/fb0/refresh", t, sizeof t) > 0)
		cap = patoi(t);
	if (cap <= 0) cap = 60;

	static const int cand[] = { 30, 60, 90, 120, 144, 165, 240 };
	for (int i = 0; i < (int)(sizeof cand / sizeof cand[0]); i++)
		if (cand[i] <= cap) add_rate(cand[i]);
	add_rate(cap);   /* include an odd panel rate (e.g. 75) not among the candidates */

	int cur = 0;
	if (read_proc("/sys/class/graphics/fb0/rate", t, sizeof t) > 0)
		cur = patoi(t);
	add_rate(cur);   /* the live value may be outside the candidate set — show it */
	for (int i = 0; i < rate_n; i++)
		if (rate_val[i] == cur) { rate_cur = i; break; }

	select_init(&rate_sel, rate_opts, rate_n, rate_cur < 0 ? 0 : rate_cur);
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
static void build_controls(int cat);

static void load_category(int cat)
{
	if      (cat == CAT_MOUSE)    load_mouse();
	else if (cat == CAT_KEYBOARD) load_kbd();
	else if (cat == CAT_SOUND)    load_sound();
	else if (cat == CAT_DISPLAY)  { load_display(); load_rate(); }
	else if (cat == CAT_ABOUT)    load_about();
	build_controls(cat);
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

static void set_display(int i)
{
	char t[16];
	if (i < 0 || i >= disp_n) return;
	mode_str(disp_w[i], disp_h[i], t, 0);
	/* The kernel queues the switch for the compositor thread; the write succeeds
	 * once it's accepted. Mirror the selection so the UI tracks it immediately. */
	if (writefile("/sys/class/graphics/fb0/mode", t, strlen(t)) == 0)
		disp_cur = i;
}

static void set_rate(int i)
{
	char t[8];
	if (i < 0 || i >= rate_n) return;
	uitoa((unsigned)rate_val[i], t);
	/* The kernel clamps and stores it; the compositor polls the node and re-paces,
	 * then persists the choice to /etc/display.conf. */
	if (writefile("/sys/class/graphics/fb0/rate", t, strlen(t)) == 0)
		rate_cur = i;
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

static int ctrl_box_w(int cw)
{
	int w = 220;
	if (w > cw - 32) w = cw - 32;
	if (w < 80) w = 80;
	return w;
}

/* A detail-pane control: a labelled dropdown plus the callback that pushes its
 * value to the kernel. Each category owns a list of these, laid out top to
 * bottom — the generalisation (over one hardcoded select per category) is what
 * lets Display carry both Resolution and Refresh rate. */
struct control {
	const char *label, *hint;
	struct select_box *sel;
	void (*apply)(int idx);
};
static struct control ctrls[3];
static int nctrl;

/* Per-control vertical geometry: label, then the box, then a dim hint under it. */
#define CONTROLS_Y0   44
#define CONTROL_PITCH 84
static int ctrl_label_y(int i) { return CONTROLS_Y0 + i * CONTROL_PITCH; }
static int ctrl_box_y(int i)   { return ctrl_label_y(i) + 24; }
static int ctrl_hint_y(int i)  { return ctrl_box_y(i) + GUI_SELECT_H + 8; }

/* Bridge each control's (int idx) selection to its typed setter. */
static void ap_mouse(int i)   { set_mouse_level(i + 1); }
static void ap_kbd(int i)     { set_kbd_layout(kbd_opts[i].name); }
static void ap_sound(int i)   { set_sound_output(i); }
static void ap_display(int i) { set_display(i); }
static void ap_rate(int i)    { set_rate(i); }

static void addc(const char *label, const char *hint,
                 struct select_box *sel, void (*apply)(int))
{
	if (nctrl >= (int)(sizeof ctrls / sizeof ctrls[0])) return;
	ctrls[nctrl].label = label;
	ctrls[nctrl].hint  = hint;
	ctrls[nctrl].sel   = sel;
	ctrls[nctrl].apply = apply;
	nctrl++;
}

/* Populate ctrls[] for the active category from its freshly loaded state. */
static void build_controls(int cat)
{
	nctrl = 0;
	switch (cat) {
	case CAT_MOUSE:
		addc("Pointer speed", "Higher moves the pointer farther per motion.",
		     &mouse_sel, ap_mouse);
		break;
	case CAT_KEYBOARD:
		addc("Layout", "Affects the whole system.", &kbd_sel, ap_kbd);
		break;
	case CAT_SOUND:
		if (sound_n)
			addc("Output device", "Routes playback to the selected output.",
			     &sound_sel, ap_sound);
		break;
	case CAT_DISPLAY:
		if (disp_n) {
			addc("Resolution",
			     disp_fixed ? "Resolution is fixed by the bootloader."
			                : "Resolution applies immediately.",
			     &disp_sel, ap_display);
			addc("Refresh rate", "Frame rate the desktop renders at.",
			     &rate_sel, ap_rate);
		}
		break;
	}
}

/* Draw the active category's controls: labels and hints first, then the boxes —
 * the open one last so its expanded list overlays the controls below it. */
static void draw_controls(int cx, int cw)
{
	if (nctrl == 0) {
		const char *msg = sel == CAT_SOUND   ? "No audio output device." :
		                  sel == CAT_DISPLAY ? "No framebuffer." :
		                                       "No options available.";
		gfx_ui_text(cx + 16, CONTROLS_Y0, msg, FG_DIM);
		return;
	}
	int bw = ctrl_box_w(cw), open = -1;
	for (int i = 0; i < nctrl; i++) {
		gfx_ui_text(cx + 16, ctrl_label_y(i), ctrls[i].label, FG_TEXT);
		if (ctrls[i].hint)
			gfx_ui_text(cx + 16, ctrl_hint_y(i), ctrls[i].hint, FG_DIM);
	}
	for (int i = 0; i < nctrl; i++) {
		if (ctrls[i].sel->open) { open = i; continue; }
		select_draw(ctrls[i].sel, cx + 16, ctrl_box_y(i), bw, GUI_SELECT_H);
	}
	if (open >= 0)
		select_draw(ctrls[open].sel, cx + 16, ctrl_box_y(open), bw, GUI_SELECT_H);
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

	/* Detail pane: About renders its own text block; every other category shows
	 * the category title over its generic control stack. */
	int cx = sbw + 1, cw = W - sbw - 1;
	gfx_fill_rect(cx, 0, cw, H, BG_PANEL);
	if (sel == CAT_ABOUT) {
		draw_about(cx, cw);
	} else {
		gfx_ui_text(cx + 16, 16, cat_name[sel], FG_WHITE);
		draw_controls(cx, cw);
	}
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

	/* Detail-pane controls: route to the open dropdown first (its list overlays
	 * the boxes below it), then the rest. Acting on one closes the others. */
	int cx = sbw + 1, cw = W - sbw - 1;
	int bw = ctrl_box_w(cw), opened = -1;
	for (int i = 0; i < nctrl; i++) if (ctrls[i].sel->open) opened = i;

	int order[3], no = 0;
	if (opened >= 0) order[no++] = opened;
	for (int i = 0; i < nctrl; i++) if (i != opened) order[no++] = i;

	for (int k = 0; k < no; k++) {
		int i = order[k];
		int r = select_click(ctrls[i].sel, cx + 16, ctrl_box_y(i), bw, GUI_SELECT_H, mx, my);
		if (r == 0) continue;
		if (r == 1) ctrls[i].apply(ctrls[i].sel->selected);
		for (int j = 0; j < nctrl; j++) if (j != i) ctrls[j].sel->open = 0;
		seq++;
		return;
	}
}

static void drag(int mx)
{
	if (sidebar_drag(&sb, 0, mx))
		seq++;
}

/* Up/Down move the selected category; an open dropdown captures keys instead. */
static void key(int k)
{
	for (int i = 0; i < nctrl; i++) {
		if (!ctrls[i].sel->open) continue;
		int r = select_key(ctrls[i].sel, (char)k);
		if (r == 1)       { ctrls[i].apply(ctrls[i].sel->selected); seq++; return; }
		else if (r == -1) { seq++; return; }
		break;                        /* open but unhandled: fall through to nav */
	}
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
	load_display();
	load_rate();
	load_about();
	build_controls(sel);

	int last = -1;
	for (;;) {
		struct win_event ev;
		while (win_poll(id, &ev)) {
			if      (ev.type == WEV_CLOSE)  { win_close(id); return 0; }
			else if (ev.type == WEV_MOUSE)  click(id, ev.a, ev.b);
			else if (ev.type == WEV_DRAG)   drag(ev.a);
			else if (ev.type == WEV_MOVE)   {
				/* Track hover for whichever dropdown is open. */
				int sbw = sidebar_w(), cx = sbw + 1, cw = W - sbw - 1, bw = ctrl_box_w(cw);
				for (int i = 0; i < nctrl; i++) {
					if (!ctrls[i].sel->open) continue;
					if (select_motion(ctrls[i].sel, cx + 16, ctrl_box_y(i),
					                  bw, GUI_SELECT_H, ev.a, ev.b))
						seq++;
					break;
				}
			}
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
