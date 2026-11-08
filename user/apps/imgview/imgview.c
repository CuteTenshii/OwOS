/* imgview.c, a PNG image viewer with a File menu and a properties panel. Opens a window with a
 * menu bar (File: Open / Reload / Quit, View: Properties), an image canvas, and a status bar */
#include "gfx.h"
#include "imganim.h"
#include "owos.h"
#include "png.h"
#include "webp.h"
#include <stdbool.h>

/* layout */
#define MENU_H GUI_BAR_H   /* top menu bar */
#define STATUS_H GUI_BAR_H /* bottom status / open-prompt bar */
#define CW_MIN 480         /* canvas size is clamped to this range so */
#define CH_MIN 320         /* there's room for chrome + the panel even */
#define CW_MAX 700         /* for tiny images, and the window stays */
#define CH_MAX 460         /* within the screen-sized window-buffer cap */

static int W, H;                         /* window client size */
static int cw, ch;                       /* image canvas size (= W, H-MENU_H-STATUS_H) */
static unsigned canvas[CW_MAX * CH_MAX]; /* backing store for the image area */

static int wid;           /* window id */
static char curpath[256]; /* path of the image currently shown */
static unsigned iw, ih;   /* its pixel dimensions */
static int load_ok;       /* a valid image is loaded */
enum { FMT_PNG, FMT_WEBP, FMT_GIF };
static int fmt; /* container of the loaded image */

static struct imganim *anim; /* playing GIF / animated WebP, else NULL */
static unsigned char *anim_file;
static unsigned anim_next_ms;

static struct png_meta meta; /* probed metadata for curpath */
static int meta_valid;       /* meta + the props list match curpath */

static int props_open;  /* the properties overlay is showing */
static int prop_scroll; /* first visible properties row */

static int prompt_on; /* the bottom "Open:" prompt is active */
static char prompt_buf[256];
static int prompt_len;

static struct status_bar sbar;

/* tiny helpers */

static void scopy(char *d, const char *s, int max)
{
  int i = 0;
  for (; s[i] && i < max - 1; i++)
    d[i] = s[i];
  d[i] = 0;
}

static const char *basename_of(const char *p)
{
  const char *b = p;
  for (; *p; p++)
    if (*p == '/')
      b = p + 1;
  return b;
}

/* String builders into caller buffers (no libc printf here) */
static char *as(char *p, const char *s)
{
  while (*s)
    *p++ = *s++;
  *p = 0;
  return p;
}
static char *au(char *p, unsigned v)
{
  char t[12];
  int n = 0;
  if (!v)
    t[n++] = '0';
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n)
    *p++ = t[--n];
  *p = 0;
  return p;
}
static char *au2(char *p, unsigned v)
{
  *p++ = (char)('0' + (v / 10) % 10);
  *p++ = (char)('0' + v % 10);
  *p = 0;
  return p;
}

static void set_status(const char *s) { status_set(&sbar, s); }

/* a buffered file reader over readfile_at (streams any-size files) */
#define FR_BUF 65536
static unsigned char fr_buffer[FR_BUF];
struct freader {
  const char *name;
  unsigned off, len, pos;
  bool eof;
};

static void fr_init(struct freader *f, const char *name)
{
  f->name = name;
  f->off = 0;
  f->len = 0;
  f->pos = 0;
  f->eof = false;
}

static int fr_get(void *c)
{
  struct freader *f = c;
  if (f->pos >= f->len) {
    if (f->eof)
      return -1;
    int n = readfile_at(f->name, f->off, fr_buffer, FR_BUF);
    if (n <= 0) {
      f->eof = true;
      return -1;
    }
    f->off += (unsigned)n;
    f->len = (unsigned)n;
    f->pos = 0;
  }
  return fr_buffer[f->pos++];
}

/* Read a whole file into a fresh malloc'd buffer */
static unsigned char *read_whole(const char *path, int *len)
{
  struct statinfo st;
  if (stat(path, &st) != 0 || st.size == 0)
    return 0;
  unsigned char *buf = malloc(st.size);
  if (!buf)
    return 0;
  unsigned off = 0;
  while (off < st.size) {
    int n = readfile_at(path, off, buf + off, st.size - off);
    if (n <= 0)
      break;
    off += (unsigned)n;
  }
  if (off != st.size) {
    free(buf);
    return 0;
  }
  *len = (int)st.size;
  return buf;
}

/* Probe dimensions + format from a file: a RIFF/WEBP header routes to the WebP reader, GIF89a/87a to the GIF one */
static int probe_image(const char *path, unsigned *w, unsigned *h, int *format)
{
  unsigned char hdr[64];
  int n = readfile_at(path, 0, hdr, sizeof hdr);
  if (n >= 12 && hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F' && hdr[8] == 'W' && hdr[9] == 'E' &&
      hdr[10] == 'B' && hdr[11] == 'P') {
    *format = FMT_WEBP;
    return webp_dimensions(hdr, n, w, h) == WEBP_OK ? 0 : -1;
  }
  if (n >= 10 && imganim_sniff(hdr, n) == IMGANIM_GIF) {
    *format = FMT_GIF;
    *w = hdr[6] | (hdr[7] << 8);
    *h = hdr[8] | (hdr[9] << 8);
    return 0;
  }
  *format = FMT_PNG;
  struct freader fr;
  fr_init(&fr, path);
  return (png_dimensions(fr_get, &fr, w, h) == 0 && *w && *h) ? 0 : -1;
}

/* scaled blit into the backing canvas */
struct view {
  unsigned cw, ch;         /* canvas size */
  unsigned iw, ih;         /* image size */
  unsigned dw, dh, ox, oy; /* drawn size + top-left offset (letterboxed) */
  unsigned next_r;         /* next destination row awaiting a source row */
  int id;                  /* window, so we can present rows as they decode */
  unsigned shown;          /* next_r at the last present (throttle presents) */
};

/* Per-destination-column source byte offset, (x*iw/dw)*4 */
static unsigned xmap[CW_MAX];

/* Copy canvas rows [y0,y1) into the window's image area (rows below the menu bar) */
static void blit_band(int y0, int y1)
{
  if (y0 < 0)
    y0 = 0;
  if (y1 > ch)
    y1 = ch;
  if (y1 <= y0)
    return;
  unsigned *fb = win_fb();
  memcpy(fb + (unsigned long)(MENU_H + y0) * W, canvas + (unsigned long)y0 * cw, (unsigned long)(y1 - y0) * cw * 4u);
}

static void blit_canvas(void) { blit_band(0, ch); }

/* Called per source scanline */
static int v_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  (void)w;
  struct view *v = c;
  unsigned *fb = canvas;
  while (v->next_r < v->dh && (v->next_r * v->ih / v->dh) == y) {
    unsigned *drow = fb + (unsigned long)(v->oy + v->next_r) * v->cw + v->ox;
    for (unsigned x = 0; x < v->dw; x++) {
      const unsigned char *p = rgba + xmap[x]; /* no per-pixel divide */
      unsigned a = p[3];
      if (a == 255) {
        drow[x] = ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
      } else if (a) { /* blend over the letterbox bg */
        unsigned d = drow[x], ia = 255 - a;
        unsigned r = (p[0] * a + ((d >> 16) & 0xff) * ia) / 255;
        unsigned g = (p[1] * a + ((d >> 8) & 0xff) * ia) / 255;
        unsigned b = (p[2] * a + (d & 0xff) * ia) / 255;
        drow[x] = (r << 16) | (g << 8) | b;
      }
    }
    v->next_r++;
  }
  /* Decoding a megabyte PNG off PIO disk takes seconds */
  if (v->id >= 0 && v->next_r - v->shown >= 16) {
    blit_band((int)(v->oy + v->shown), (int)(v->oy + v->next_r));
    v->shown = v->next_r;
    win_present(v->id);
  }
  return 0;
}

/* Fit iw x ih into the canvas (1:1 when it fits, else letterboxed) and build the column map */
static void view_setup(struct view *v)
{
  v->cw = cw;
  v->ch = ch;
  v->iw = iw;
  v->ih = ih;
  v->next_r = 0;
  v->id = wid;
  v->shown = 0;
  if (iw <= (unsigned)cw && ih <= (unsigned)ch) { /* fits: show 1:1 */
    v->dw = iw;
    v->dh = ih;
  } else if ((unsigned long)iw * ch > (unsigned long)ih * cw) {
    v->dw = cw;
    v->dh = ih * cw / iw; /* width-bound */
  } else {
    v->dh = ch;
    v->dw = iw * ch / ih; /* height-bound */
  }
  if (v->dw == 0)
    v->dw = 1;
  if (v->dh == 0)
    v->dh = 1;
  v->ox = ((unsigned)cw - v->dw) / 2;
  v->oy = ((unsigned)ch - v->dh) / 2;

  for (unsigned x = 0; x < v->dw; x++) /* precompute the horizontal map */
    xmap[x] = (x * v->iw / v->dw) * 4;
}

static void stop_anim(void)
{
  imganim_close(anim);
  free(anim_file);
  anim = 0;
  anim_file = 0;
}

/* Paint the player's current frame over a fresh letterbox background; no presenting, the main loop does that */
static void draw_anim_frame(void)
{
  struct view v;
  view_setup(&v);
  v.id = -1;
  for (unsigned y = 0; y < v.dh; y++)
    for (unsigned x = 0; x < v.dw; x++)
      canvas[(unsigned long)(v.oy + y) * cw + v.ox + x] = 0x141a24;
  for (unsigned y = 0; y < anim->h; y++)
    v_row(&v, y, anim->rgba + (unsigned long)y * anim->w * 4u, anim->w);
}

/* Decode curpath into the backing canvas */
static void decode_to_canvas(void)
{
  stop_anim();
  for (int i = 0; i < cw * ch; i++)
    canvas[i] = 0x141a24; /* letterbox background */
  if (!load_ok)
    return;

  struct view v;
  view_setup(&v);

  /* Paint the letterbox background once up front, then stream the image in over it */
  blit_canvas();
  win_present(wid);

  if (fmt != FMT_PNG) {
    /* WebP and GIF need the whole file in memory; decode it */
    int flen = 0;
    unsigned char *file = read_whole(curpath, &flen);
    if (!file)
      return;
    anim = imganim_open(file, flen);
    unsigned delay;
    if (anim && anim->w == iw && anim->h == ih && imganim_step(anim, &delay)) {
      anim_file = file; /* the player reads from it until stop_anim */
      anim_next_ms = msec() + delay;
      draw_anim_frame();
      return;
    }
    stop_anim();
    unsigned char *rgba;
    unsigned dw = 0, dh = 0;
    if (fmt == FMT_WEBP && webp_decode(file, flen, &rgba, &dw, &dh) == WEBP_OK) {
      for (unsigned y = 0; y < dh; y++)
        v_row(&v, y, rgba + (unsigned long)y * dw * 4u, dw);
      free(rgba);
    }
    free(file);
  } else {
    struct freader fr;
    fr_init(&fr, curpath);
    png_decode(fr_get, &fr, v_row, &v, 0, 0);
  }
}

/* properties list */
#define PROP_MAX 80
static char prop_key[PROP_MAX][20];
static char prop_val[PROP_MAX][PNG_VAL_MAX];
static int n_props;

static void padd(const char *k, const char *v)
{
  if (n_props >= PROP_MAX)
    return;
  scopy(prop_key[n_props], k, sizeof prop_key[0]);
  scopy(prop_val[n_props], v, sizeof prop_val[0]);
  n_props++;
}

static void exif_cb(void *ctx, const char *key, const char *val)
{
  (void)ctx;
  padd(key, val);
}

static const char *color_name(unsigned color)
{
  switch (color) {
  case 0:
    return "Grayscale";
  case 2:
    return "RGB (truecolor)";
  case 3:
    return "Palette (indexed)";
  case 4:
    return "Grayscale + alpha";
  case 6:
    return "RGBA (truecolor+alpha)";
  default:
    return "Unknown";
  }
}

/* Format curpath's size, human-readable, into b */
static void fmt_size(char *b)
{
  struct statinfo st;
  unsigned sz = (stat(curpath, &st) == 0) ? st.size : 0;
  char *p = b;
  if (sz < 1024) {
    p = au(p, sz);
    as(p, " B");
  } else if (sz < 1024u * 1024u) {
    unsigned t = sz * 10 / 1024;
    p = au(p, t / 10);
    *p++ = '.';
    p = au(p, t % 10);
    as(p, " KB");
  } else {
    unsigned t = sz * 10 / (1024u * 1024u);
    p = au(p, t / 10);
    *p++ = '.';
    p = au(p, t % 10);
    as(p, " MB");
  }
}

/* Properties for a WebP or GIF image (probed from its header) */
static void build_props_container(void)
{
  char b[PNG_VAL_MAX], *p;
  n_props = 0;
  padd("File", basename_of(curpath));
  if (!load_ok) {
    padd("Status", "not a readable image");
    return;
  }
  fmt_size(b);
  padd("Size", b);
  p = b;
  p = au(p, iw);
  p = as(p, " x ");
  p = au(p, ih);
  as(p, " px");
  padd("Dimensions", b);

  unsigned char hdr[64];
  int n = readfile_at(curpath, 0, hdr, sizeof hdr);
  struct webp_info info;
  if (fmt == FMT_GIF) {
    char ver[7] = {0};
    for (int i = 0; i < 6 && i < n; i++)
      ver[i] = (char)hdr[i];
    padd("Format", ver);
  } else if (n > 0 && webp_probe(hdr, n, &info) == WEBP_OK) {
    padd("Format", info.lossless ? "WebP lossless (VP8L)" : "WebP lossy (VP8)");
    padd("Transparency", info.has_alpha ? "Yes (alpha channel)" : "No");
    padd("Container", info.extended ? "Extended (VP8X)" : "Simple");
    if (info.animation)
      padd("Animation", "Yes");
  } else {
    padd("Format", "WebP");
  }
}

/* Rebuild the key/value list from `meta` (assumed valid) + a stat() of curpath */
static void build_props(void)
{
  char b[PNG_VAL_MAX], *p;
  n_props = 0;

  padd("File", basename_of(curpath));

  if (!load_ok) {
    padd("Status", "not a readable PNG");
    return;
  }

  fmt_size(b);
  padd("Size", b);

  p = b;
  p = au(p, meta.w);
  p = as(p, " x ");
  p = au(p, meta.h);
  as(p, " px");
  padd("Dimensions", b);

  padd("Color space", color_name(meta.color));

  if (meta.color == 3 && meta.palette_n) {
    p = au(b, meta.palette_n);
    as(p, " colors");
    padd("Palette", b);
  }

  p = au(b, meta.depth);
  as(p, " bits/sample");
  padd("Bit depth", b);

  p = au(b, meta.channels);
  as(p, "");
  padd("Channels", b);

  p = au(b, meta.depth * meta.channels);
  as(p, " bits/pixel");
  padd("Pixel format", b);

  padd("Transparency", meta.has_alpha ? "Yes (alpha channel)" : meta.has_trns ? "Yes (color key / tRNS)" : "No");

  padd("Interlace", meta.interlaced ? "Adam7" : "None");

  if (meta.have_phys) {
    if (meta.phys_metre) { /* pixels/metre -> DPI */
      unsigned dx = (meta.ppu_x * 254u + 5000u) / 10000u;
      unsigned dy = (meta.ppu_y * 254u + 5000u) / 10000u;
      p = au(b, dx);
      p = as(p, " x ");
      p = au(p, dy);
      as(p, " DPI");
    } else {
      p = au(b, meta.ppu_x);
      p = as(p, " x ");
      p = au(p, meta.ppu_y);
      as(p, " px/unit");
    }
    padd("Resolution", b);
  }

  if (meta.have_gamma) { /* gAMA is gamma * 100000 */
    unsigned g = meta.gamma;
    p = au(b, g / 100000u);
    *p++ = '.';
    unsigned f = g % 100000u; /* zero-pad the 5-digit fraction */
    for (unsigned d = 10000u; d >= 1u; d /= 10u) {
      *p++ = (char)('0' + (f / d) % 10u);
      if (d == 1u)
        break;
    }
    *p = 0;
    padd("Gamma", b);
  }

  if (meta.have_time) {
    p = b;
    p = au(p, meta.year);
    *p++ = '-';
    p = au2(p, meta.mon);
    *p++ = '-';
    p = au2(p, meta.day);
    *p++ = ' ';
    p = au2(p, meta.hour);
    *p++ = ':';
    p = au2(p, meta.min);
    *p++ = ':';
    au2(p, meta.sec);
    padd("Modified", b);
  }

  for (int i = 0; i < meta.n_text; i++)
    padd(meta.text[i].key, meta.text[i].val);

  if (meta.exif_len > 0)
    png_exif_emit(meta.exif, meta.exif_len, exif_cb, 0);
  else if (meta.exif_len < 0)
    padd("EXIF", "(present, too large)");
}

/* Ensure meta + the props list are current for curpath (lazy: only on demand) */
static void ensure_meta(void)
{
  if (meta_valid)
    return;
  if (fmt != FMT_PNG) {
    build_props_container();
    meta_valid = 1;
    return;
  }
  if (load_ok) {
    struct freader fr;
    fr_init(&fr, curpath);
    png_probe(fr_get, &fr, &meta);
  }
  build_props();
  meta_valid = 1;
}

/* file actions */

static void load(const char *path)
{
  unsigned w = 0, h = 0;
  int format = FMT_PNG;
  if (probe_image(path, &w, &h, &format) != 0 || w == 0 || h == 0) {
    load_ok = 0;
    fmt = FMT_PNG;
    meta_valid = 0;
    scopy(curpath, path, sizeof curpath); /* set, so the UI says which file */
    decode_to_canvas();
    set_status("not a readable image");
    win_settitle(wid, "Image Viewer");
    return;
  }
  scopy(curpath, path, sizeof curpath);
  fmt = format;
  iw = w;
  ih = h;
  load_ok = 1;
  meta_valid = 0;
  prop_scroll = 0;

  win_cursor(wid, WIN_CURSOR_BUSY);
  decode_to_canvas();
  win_cursor(wid, WIN_CURSOR_ARROW);

  char title[160], *p;
  p = as(title, basename_of(curpath));
  p = as(p, "  (");
  p = au(p, iw);
  p = as(p, "x");
  p = au(p, ih);
  as(p, ")");
  win_settitle(wid, title);
  set_status("opened");
}

static void toggle_props(void)
{
  props_open = !props_open;
  if (props_open) {
    prop_scroll = 0;
    if (!meta_valid) { /* reading the header off disk: show busy */
      win_cursor(wid, WIN_CURSOR_BUSY);
      ensure_meta();
      win_cursor(wid, WIN_CURSOR_ARROW);
    }
  }
}

static void start_prompt(void)
{
  prompt_on = 1;
  prompt_len = 0;
  prompt_buf[0] = 0;
}

static void confirm_prompt(void)
{
  prompt_buf[prompt_len] = 0;
  if (prompt_len == 0) {
    set_status("canceled");
    return;
  }
  load(prompt_buf);
}

static void file_action(int item)
{
  switch (item) {
  case 0:
    start_prompt();
    break; /* Open */
  case 1:  /* Reload */
    if (load_ok || curpath[0])
      load(curpath);
    else
      set_status("nothing to reload");
    break;
  case 2:
    win_close(wid);
    exit(0); /* Quit */
  }
}

/* menus */

static const char *const file_items[] = {"Open...", "Reload", "Quit"};
static const char *const file_hints[] = {"Ctrl+O", "F5", ""};
static const unsigned char file_keys[] = {KEY_OPEN, KEY_F5, 0};
static struct menu filemenu = {"File", file_items, 3, 0, file_hints, file_keys};

static const char *const view_items[] = {"Properties"};
static const char *const view_hints[] = {"I"};
static const unsigned char view_keys[] = {0};
static unsigned char view_flags[1];
static struct menu viewmenu = {"View", view_items, 1, 0, view_hints, view_keys, view_flags};

static struct menu *const bar_menus[] = {&filemenu, &viewmenu};
static struct menubar mbar = {bar_menus, 2, 2, 0};

static void menu_action(struct menu *leaf, int item)
{
  if (leaf == &viewmenu)
    toggle_props();
  else
    file_action(item);
}

/* properties overlay drawing */

static int props_visible_rows(int ph)
{
  int v = (ph - 34) / 16;
  return v < 1 ? 1 : v;
}

static void clamp_scroll(void)
{
  int ph = ch - 32;
  int vis = props_visible_rows(ph);
  int maxs = n_props - vis;
  if (maxs < 0)
    maxs = 0;
  if (prop_scroll > maxs)
    prop_scroll = maxs;
  if (prop_scroll < 0)
    prop_scroll = 0;
}

static void draw_props(void)
{
  int px = 24, py = MENU_H + 16;
  int pw = cw - 48, ph = ch - 32;
  unsigned bg = 0x1b2230, bord = 0x3a4660, hdr = 0x232c3c;
  unsigned kfg = 0x8fa3bf, vfg = 0xe4ecf7, tfg = 0xeaf0fb;

  gfx_fill_rect(px, py, pw, ph, bg);
  gfx_rect(px, py, pw, ph, bord);
  gfx_fill_rect(px + 1, py + 1, pw - 2, 24, hdr);
  gfx_ui_text(px + 10, py + 4, "Image Properties", tfg);

  int vis = props_visible_rows(ph);
  clamp_scroll();
  int y = py + 30;
  for (int i = 0; i < vis; i++) {
    int idx = prop_scroll + i;
    if (idx >= n_props)
      break;
    gfx_text(px + 12, y, prop_key[idx], kfg, bg);
    gfx_text(px + 12 + 152, y, prop_val[idx], vfg, bg);
    y += 16;
  }

  if (n_props > vis) { /* scroll affordance: a thumb on the right edge */
    int trough_x = px + pw - 8, trough_y = py + 28, trough_h = ph - 34;
    gfx_fill_rect(trough_x, trough_y, 4, trough_h, 0x12161e);
    int th = trough_h * vis / n_props;
    if (th < 8)
      th = 8;
    int ty = trough_y + (trough_h - th) * prop_scroll / (n_props - vis);
    gfx_fill_rect(trough_x, ty, 4, th, 0x55668a);
  }
}

/* rendering */

static void render(void)
{
  blit_canvas(); /* image area */

  if (!load_ok) { /* friendly empty state */
    const char *m = curpath[0] ? "Cannot display this file" : "Open an image:  File > Open";
    int tw = gfx_ui_width(m);
    gfx_ui_text((W - tw) / 2, MENU_H + ch / 2 - UIFONT_H / 2, m, 0x9fb0c4);
  }

  gfx_fill_rect(0, 0, W, MENU_H, 0x232a36); /* menu bar background */

  /* Status / open-prompt bar */
  int sy = H - STATUS_H;
  struct status_bar bar;
  char line[128], *p = line;
  if (prompt_on) {
    p = as(line, "Open: ");
    p = as(p, prompt_buf);
    as(p, "_");
    status_set(&bar, line);
    status_draw(&bar, 0, sy, W, 0xffe08a, 0x161d28);
  } else {
    p = as(line, load_ok ? basename_of(curpath) : "[no image]");
    if (load_ok) {
      p = as(p, "   ");
      p = au(p, iw);
      p = as(p, "x");
      p = au(p, ih);
      p = as(p, "  ");
      p = as(p, fmt == FMT_WEBP ? "WebP" : fmt == FMT_GIF ? "GIF" : color_name(meta_valid ? meta.color : 2));
    }
    status_set(&bar, line);
    status_draw(&bar, 0, sy, W, 0x9fb0c4, 0x161d28);
  }

  if (props_open)
    draw_props();

  /* Menu bar drawn last so an open dropdown overlays the panel */
  view_flags[0] = props_open ? MENU_CHECKED : 0;
  menubar_draw(&mbar);
}

/* input */

static void on_key(char c)
{
  if (prompt_on) {
    if (c == '\n') {
      confirm_prompt();
      prompt_on = 0;
    } else if (c == 27) {
      prompt_on = 0;
      set_status("canceled");
    } else if (c == '\b') {
      if (prompt_len > 0)
        prompt_buf[--prompt_len] = 0;
    } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F && prompt_len < (int)sizeof(prompt_buf) - 1)
      prompt_buf[prompt_len++] = c;
    return;
  }

  struct menu *leaf;
  int sc = menubar_shortcut(&mbar, (unsigned char)c, &leaf);
  if (sc >= 0) {
    menubar_close(&mbar);
    menu_action(leaf, sc);
    return;
  }

  if (c == 27) {
    if (props_open)
      props_open = 0;
    else
      menubar_close(&mbar);
    return;
  }
  if (c == 'i' || c == 'I') {
    toggle_props();
    return;
  }

  if (props_open) {
    if ((unsigned char)c == KEY_UP) {
      prop_scroll--;
      clamp_scroll();
    } else if ((unsigned char)c == KEY_DOWN) {
      prop_scroll++;
      clamp_scroll();
    } else if ((unsigned char)c == KEY_HOME) {
      prop_scroll = 0;
    }
  }
}

static void on_click(int mx, int my)
{
  if (prompt_on)
    return;
  if (my < MENU_H || filemenu.open || viewmenu.open) { /* bar or open dropdown */
    struct menu *leaf;
    int item;
    if (menubar_click(&mbar, mx, my, &leaf, &item) && item >= 0)
      menu_action(leaf, item);
  }
}

static void on_scroll(int delta)
{
  if (!props_open)
    return;
  prop_scroll -= delta; /* wheel up (+) scrolls toward the top */
  clamp_scroll();
}

int main(int argc, char **argv)
{
  /* Size the canvas to the first image (if given) within the clamp range */
  const char *arg = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : 0;
  unsigned w0 = CW_MIN, h0 = CH_MIN;
  if (arg) {
    unsigned w = 0, h = 0;
    int format;
    if (probe_image(arg, &w, &h, &format) == 0 && w && h) {
      w0 = w;
      h0 = h; /* a readable image: fit the window to it */
    }
  }

  cw = (int)w0;
  ch = (int)h0;
  if (cw > CW_MAX || ch > CH_MAX) { /* shrink to fit, keep aspect */
    if ((unsigned long)w0 * CH_MAX > (unsigned long)h0 * CW_MAX) {
      cw = CW_MAX;
      ch = (int)(h0 * CW_MAX / w0);
    } else {
      ch = CH_MAX;
      cw = (int)(w0 * CH_MAX / h0);
    }
  }
  if (cw < CW_MIN)
    cw = CW_MIN; /* grow to leave room for chrome */
  if (ch < CH_MIN)
    ch = CH_MIN;
  if (cw > CW_MAX)
    cw = CW_MAX;
  if (ch > CH_MAX)
    ch = CH_MAX;

  W = cw;
  H = MENU_H + ch + STATUS_H;

  wid = win_create(W, H, "Image Viewer");
  if (wid < 0) {
    puts("imgview: could not open a window\n");
    return 1;
  }
  gfx_init(win_fb(), W, H);
  win_resizable(wid, 0); /* fixed size: the buffer matches W*H */

  if (arg)
    load(arg); /* shows the image, or "not a readable PNG" */
  else {
    decode_to_canvas(); /* paints the empty-state background */
    set_status("no image");
  }

  render();
  win_present(wid);

  bool need = false;
  for (;;) {
    struct win_event ev;
    win_wait(wid);
    while (win_poll(wid, &ev)) {
      need = true;
      if (ev.type == WEV_CLOSE) {
        win_close(wid);
        return 0;
      } else if (ev.type == WEV_KEY)
        on_key((char)ev.a);
      else if (ev.type == WEV_MOUSE)
        on_click(ev.a, ev.b);
      else if (ev.type == WEV_SCROLL)
        on_scroll(ev.a);
    }
    if (anim && (int)(msec() - anim_next_ms) >= 0) {
      unsigned delay;
      if (imganim_step(anim, &delay)) {
        draw_anim_frame();
        anim_next_ms = msec() + delay;
        need = true;
      } else {
        stop_anim();
      }
    }
    if (need) {
      render();
      win_present(wid);
      need = false;
    }
  }
}
