/* devices.c, Mouse, Keyboard, Sound, Display and Fonts pages: state in sysfs/procfs and its setters */
#include "fontstore.h"
#include "settings.h"

/* Live state mirrored from /proc, re-read when a category is entered */
static int mouse_level = 3;       /* pointer-speed level 1..5 */
static char kbd_layout[8] = "us"; /* "us" / "fr" */

/* Pointer-speed levels 1..5, by their dropdown labels */
static const char *const mouse_opts[5] = {
    "Slowest", "Slow", "Normal", "Fast", "Fastest",
};
static struct select_box mouse_sel;

/* Keyboard layouts: sysfs name + the label shown in the dropdown */
static const struct {
  const char *name, *label;
} kbd_opts[] = {
    {"us", "US English"},
    {"fr", "French"},
};
#define NKBD ((int)(sizeof kbd_opts / sizeof kbd_opts[0]))
static const char *kbd_opt_lbl[NKBD]; /* select_box wants a char* array */
static struct select_box kbd_sel;

#define MAX_OUTS 6
static char sound_name[MAX_OUTS][20];    /* audio output device names */
static const char *sound_opts[MAX_OUTS]; /* select_box array (-> sound_name) */
static int sound_n;                      /* how many outputs the codec exposes */
static int sound_cur;                    /* index of the active output */
static struct select_box sound_sel;

#define MAX_MODES 16
static char disp_label[MAX_MODES][16];   /* "1280 x 720" per offered mode */
static const char *disp_opts[MAX_MODES]; /* select_box option array (-> labels) */
static int disp_w[MAX_MODES], disp_h[MAX_MODES];
static int disp_n;      /* how many modes are offered */
static int disp_cur;    /* index of the active mode, -1 if none */
static bool disp_fixed; /* true = resolution can't be changed */
static struct select_box disp_sel;

/* Refresh rate: a second Display control over /sys/class/graphics/fb0/rate */
#define MAX_RATES 8
static char rate_label[MAX_RATES][12]; /* "60 Hz" per offered rate */
static const char *rate_opts[MAX_RATES];
static int rate_val[MAX_RATES]; /* the numeric Hz behind each option */
static int rate_n;
static int rate_cur; /* index of the active rate, -1 if none */
static struct select_box rate_sel;

/* Interface scale: Auto (EDID DPI) or 1x..4x over /sys/class/graphics/fb0/scale */
static const char *const scale_opts[] = {"Auto", "100%", "200%", "300%", "400%"};
static int scale_cur;
static struct select_box scale_sel;

#define MAX_FONTS 16
static char font_name[MAX_FONTS][64];    /* installed font files */
static const char *font_opts[MAX_FONTS]; /* select_box array (-> font_name) */
static int font_n;
static struct select_box sans_sel, mono_sel;

static void load_mouse(void)
{
  char t[16];
  if (read_proc("/sys/class/input/mouse0/speed", t, sizeof t) > 0) {
    int v = 0;
    for (const char *p = t; *p >= '0' && *p <= '9'; p++)
      v = v * 10 + (*p - '0');
    if (v >= 1 && v <= 5)
      mouse_level = v;
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
    if (streq(kbd_layout, kbd_opts[i].name))
      cur = i;
  }
  select_init(&kbd_sel, kbd_opt_lbl, NKBD, cur);
}

static void load_sound(void)
{
  char buf[256];
  sound_n = 0;
  sound_cur = 0;

  int len = readfile("/sys/class/sound/card0/outputs", buf, sizeof buf - 1);
  if (len < 0)
    len = 0;
  buf[len] = 0;
  for (const char *p = buf; *p && sound_n < MAX_OUTS;) {
    int i = 0;
    while (*p && *p != '\n' && i < (int)sizeof sound_name[0] - 1)
      sound_name[sound_n][i++] = *p++;
    sound_name[sound_n][i] = 0;
    while (*p && *p != '\n')
      p++; /* skip an over-long name's tail */
    if (*p == '\n')
      p++;
    if (i > 0) {
      sound_opts[sound_n] = sound_name[sound_n];
      sound_n++;
    }
  }

  char cur[24];
  if (read_proc("/sys/class/sound/card0/output", cur, sizeof cur) > 0)
    for (int i = 0; i < sound_n; i++)
      if (streq(sound_name[i], cur)) {
        sound_cur = i;
        break;
      }

  select_init(&sound_sel, sound_opts, sound_n, sound_cur);
}

/* Build "WxH" (compact, for the sysfs write) and "W x H" (the menu label) */
static void mode_str(int w, int h, char *out, int spaced)
{
  char t[12];
  int i = 0;
  uitoa((unsigned)w, t);
  for (int j = 0; t[j]; j++)
    out[i++] = t[j];
  if (spaced) {
    out[i++] = ' ';
    out[i++] = 'x';
    out[i++] = ' ';
  } else
    out[i++] = 'x';
  uitoa((unsigned)h, t);
  for (int j = 0; t[j]; j++)
    out[i++] = t[j];
  out[i] = 0;
}

/* Parse a leading "WxH" out of `s` into *w,*h (0 if absent) */
static void parse_wh(const char *s, int *w, int *h)
{
  int a = 0, b = 0;
  while (*s >= '0' && *s <= '9')
    a = a * 10 + (*s++ - '0');
  if (*s == 'x' || *s == 'X')
    s++;
  while (*s >= '0' && *s <= '9')
    b = b * 10 + (*s++ - '0');
  *w = a;
  *h = b;
}

static void load_display(void)
{
  char buf[256];
  disp_n = 0;
  disp_cur = -1;

  int len = readfile("/sys/class/graphics/fb0/modes", buf, sizeof buf - 1);
  if (len < 0)
    len = 0;
  buf[len] = 0;
  for (const char *p = buf; *p && disp_n < MAX_MODES;) {
    int w, h;
    parse_wh(p, &w, &h);
    while (*p && *p != '\n')
      p++;
    if (*p == '\n')
      p++;
    if (w > 0 && h > 0) {
      disp_w[disp_n] = w;
      disp_h[disp_n] = h;
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
    if (disp_w[i] == cw && disp_h[i] == ch) {
      disp_cur = i;
      break;
    }

  /* A single offered mode means the kernel can't reprogram it, it just reports the current one */
  disp_fixed = (disp_n <= 1);
  select_init(&disp_sel, disp_opts, disp_n, disp_cur < 0 ? 0 : disp_cur);
}

/* Append hz to the offered-rate list as "N Hz" if there's room and it's new */
static void add_rate(int hz)
{
  if (hz <= 0 || rate_n >= MAX_RATES)
    return;
  for (int i = 0; i < rate_n; i++)
    if (rate_val[i] == hz)
      return;
  char *o = rate_label[rate_n];
  uitoa((unsigned)hz, o);
  int k = strlen(o);
  o[k++] = ' ';
  o[k++] = 'H';
  o[k++] = 'z';
  o[k] = 0;
  rate_val[rate_n] = hz;
  rate_opts[rate_n] = rate_label[rate_n];
  rate_n++;
}

static void load_rate(void)
{
  char t[24];
  rate_n = 0;
  rate_cur = -1;

  /* Cap the list at the panel's real refresh; without EDID fall back to the safe 30/60 pair */
  int cap = 0;
  if (read_proc("/sys/class/graphics/fb0/refresh", t, sizeof t) > 0)
    cap = patoi(t);
  if (cap <= 0)
    cap = 60;

  static const int cand[] = {30, 60, 90, 120, 144, 165, 240};
  for (int i = 0; i < (int)(sizeof cand / sizeof cand[0]); i++)
    if (cand[i] <= cap)
      add_rate(cand[i]);
  add_rate(cap); /* include an odd panel rate (e.g. 75) not among the candidates */

  int cur = 0;
  if (read_proc("/sys/class/graphics/fb0/rate", t, sizeof t) > 0)
    cur = patoi(t);
  add_rate(cur); /* the live value may be outside the candidate set, show it */
  for (int i = 0; i < rate_n; i++)
    if (rate_val[i] == cur) {
      rate_cur = i;
      break;
    }

  select_init(&rate_sel, rate_opts, rate_n, rate_cur < 0 ? 0 : rate_cur);
}

static void load_scale(void)
{
  char t[12];
  scale_cur = 0;
  if (read_proc("/sys/class/graphics/fb0/scale", t, sizeof t) > 0 && t[0] >= '1' && t[0] <= '4')
    scale_cur = t[0] - '0';
  select_init(&scale_sel, scale_opts, 5, scale_cur);
}

static void load_fonts(void)
{
  char cur[2][64];
  font_n = 0;
  while (font_n < MAX_FONTS && fontstore_list(font_n, font_name[font_n]) == 0) {
    font_opts[font_n] = font_name[font_n];
    font_n++;
  }
  struct select_box *box[2] = {&sans_sel, &mono_sel};
  for (int r = 0; r < 2; r++) {
    int at = 0;
    fontstore_current(r, cur[r]);
    for (int i = 0; i < font_n; i++)
      if (streq(font_name[i], cur[r]))
        at = i;
    select_init(box[r], font_opts, font_n, at);
  }
}

/* apply changes */

static void set_mouse_level(int level)
{
  char t[4];
  if (level < 1)
    level = 1;
  if (level > 5)
    level = 5;
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
  if (i < 0 || i >= sound_n)
    return;
  uitoa((unsigned)i, t);
  if (writefile("/sys/class/sound/card0/output", t, strlen(t)) == 0)
    sound_cur = i;
}

static void set_display(int i)
{
  char t[16];
  if (i < 0 || i >= disp_n)
    return;
  mode_str(disp_w[i], disp_h[i], t, 0);
  /* The kernel queues the switch for the compositor thread */
  if (writefile("/sys/class/graphics/fb0/mode", t, strlen(t)) == 0)
    disp_cur = i;
}

static void set_rate(int i)
{
  char t[8];
  if (i < 0 || i >= rate_n)
    return;
  uitoa((unsigned)rate_val[i], t);
  /* The kernel clamps and stores it; the compositor polls the node and re-paces */
  if (writefile("/sys/class/graphics/fb0/rate", t, strlen(t)) == 0)
    rate_cur = i;
}

static void set_font(int role, int i)
{
  if (i >= 0 && i < font_n)
    fontstore_choose(role, font_name[i]);
}

static void set_scale(int i)
{
  char t[8] = "auto";
  if (i > 0) {
    t[0] = '0' + i;
    t[1] = 0;
  }
  if (writefile("/sys/class/graphics/fb0/scale", t, strlen(t)) == 0)
    scale_cur = i;
}

static void ap_mouse(int i) { set_mouse_level(i + 1); }
static void ap_kbd(int i) { set_kbd_layout(kbd_opts[i].name); }
static void ap_sound(int i) { set_sound_output(i); }
static void ap_display(int i) { set_display(i); }
static void ap_rate(int i) { set_rate(i); }
static void ap_scale(int i) { set_scale(i); }
static void ap_sans(int i) { set_font(FONT_SANS, i); }
static void ap_mono(int i) { set_font(FONT_MONO, i); }

/* Re-read the state the category displays so it reflects the live kernel value */
void dev_load(int cat)
{
  if (cat == CAT_MOUSE)
    load_mouse();
  else if (cat == CAT_KEYBOARD)
    load_kbd();
  else if (cat == CAT_SOUND)
    load_sound();
  else if (cat == CAT_DISPLAY) {
    load_display();
    load_rate();
    load_scale();
  } else if (cat == CAT_FONTS)
    load_fonts();
}

/* Register the category's dropdowns from its freshly loaded state */
void dev_controls(int cat)
{
  switch (cat) {
  case CAT_MOUSE:
    controls_add("Pointer speed", "Higher moves the pointer farther per motion.", &mouse_sel, ap_mouse);
    break;
  case CAT_KEYBOARD:
    controls_add("Layout", "Affects the whole system.", &kbd_sel, ap_kbd);
    break;
  case CAT_SOUND:
    if (sound_n)
      controls_add("Output device", "Routes playback to the selected output.", &sound_sel, ap_sound);
    break;
  case CAT_FONTS:
    if (font_n) {
      controls_add("Interface font", "Used by apps opened from now on. Install fonts from Files.", &sans_sel, ap_sans);
      controls_add("Monospace font", "Used for terminals, code and CSS monospace text.", &mono_sel, ap_mono);
    }
    break;
  case CAT_DISPLAY:
    if (disp_n) {
      controls_add("Resolution",
                   disp_fixed ? "Resolution is fixed by the bootloader." : "Resolution applies immediately.", &disp_sel,
                   ap_display);
      controls_add("Refresh rate", "Frame rate the desktop renders at.", &rate_sel, ap_rate);
      controls_add("Interface scale", "Auto follows the monitor's DPI. Applies immediately.", &scale_sel, ap_scale);
    }
    break;
  }
}
