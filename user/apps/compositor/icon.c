/* icon.c: app icons for the start menu, taskbar, title bars and Alt+Tab */
#include "comp.h"
#include "png.h"

#define ICONS_DIR "/usr/share/icons/"
#define ICON_DEFAULT "application-default"
#define ICON_CACHE_MAX 32
#define ICON_SRC_MAX 256

static struct {
  char name[48];
  bool ok;
  unsigned px[ICON_PX * ICON_PX];
} cache[ICON_CACHE_MAX];
static int ncache;

struct reader {
  const char *path;
  unsigned off, len, pos;
  bool eof;
  unsigned char buf[512];
};

static int rd_get(void *c)
{
  struct reader *r = c;
  if (r->pos >= r->len) {
    if (r->eof)
      return -1;
    int n = readfile_at(r->path, r->off, r->buf, sizeof r->buf);
    if (n <= 0) {
      r->eof = true;
      return -1;
    }
    r->off += (unsigned)n;
    r->len = (unsigned)n;
    r->pos = 0;
  }
  return r->buf[r->pos++];
}

struct scale {
  unsigned *src;
  unsigned w;
};

static int store_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  struct scale *s = c;
  for (unsigned x = 0; x < w; x++) {
    const unsigned char *p = rgba + x * 4;
    s->src[y * s->w + x] = (unsigned)p[3] << 24 | (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
  }
  return 0;
}

/* Box-filter the w x h source to ICON_PX x ICON_PX, averaging colour weighted by alpha */
static void downscale(const unsigned *src, unsigned w, unsigned h, unsigned *out)
{
  for (unsigned dy = 0; dy < ICON_PX; dy++)
    for (unsigned dx = 0; dx < ICON_PX; dx++) {
      unsigned x0 = dx * w / ICON_PX, x1 = (dx + 1) * w / ICON_PX;
      unsigned y0 = dy * h / ICON_PX, y1 = (dy + 1) * h / ICON_PX;
      unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
      for (unsigned y = y0; y < y1; y++)
        for (unsigned x = x0; x < x1; x++) {
          unsigned p = src[y * w + x], pa = p >> 24;
          r += ((p >> 16) & 255) * pa;
          g += ((p >> 8) & 255) * pa;
          b += (p & 255) * pa;
          a += pa;
          n++;
        }
      out[dy * ICON_PX + dx] = a ? (unsigned)(a / n) << 24 | (unsigned)(r / a) << 16 | (unsigned)(g / a) << 8 | (unsigned)(b / a) : 0;
    }
}

/* Decode a PNG at least ICON_PX square and at most ICON_SRC_MAX square into out */
static bool load_png(const char *path, unsigned *out)
{
  struct reader rd = {.path = path};
  unsigned w, h;
  if (png_dimensions(rd_get, &rd, &w, &h) != 0 || w < ICON_PX || h < ICON_PX || w > ICON_SRC_MAX || h > ICON_SRC_MAX)
    return false;
  unsigned *src = malloc(w * h * 4);
  if (!src)
    return false;
  struct scale s = {src, w};
  rd = (struct reader){.path = path};
  bool ok = png_decode(rd_get, &rd, store_row, &s, 0, 0) == PNG_OK;
  if (ok)
    downscale(src, w, h, out);
  free(src);
  return ok;
}

/* The cached ICON_PX x ICON_PX ARGB bitmap for an Icon= value (a name under /usr/share/icons, or an absolute
 * path), or NULL if it has no usable image */
static const unsigned *icon_lookup(const char *name)
{
  for (int i = 0; i < ncache; i++)
    if (streq(cache[i].name, name))
      return cache[i].ok ? cache[i].px : 0;
  if (ncache == ICON_CACHE_MAX || strlen(name) >= sizeof cache[0].name)
    return 0;
  char path[96];
  if (name[0] == '/')
    strcpy(path, name);
  else {
    strcpy(path, ICONS_DIR);
    strcat(path, name);
    strcat(path, ".png");
  }
  int i = ncache++;
  strcpy(cache[i].name, name);
  cache[i].ok = load_png(path, cache[i].px);
  return cache[i].ok ? cache[i].px : 0;
}

const unsigned *app_icon(int app)
{
  const unsigned *px = app >= 0 && app < apps.n && apps.v[app]->icon ? icon_lookup(apps.v[app]->icon) : 0;
  return px ? px : icon_lookup(ICON_DEFAULT);
}

/* Whether `title` starts with `name`, ignoring case */
static bool title_starts(const char *title, const char *name)
{
  for (; *name; title++, name++)
    if (!*title || (*title | 32) != (*name | 32))
      return false;
  return true;
}

const unsigned *window_icon(const struct winrec *w)
{
  for (int i = 0; i < apps.n; i++)
    if (app_pid[i] > 0 && app_pid[i] == w->owner_pid)
      return app_icon(i);
  for (int i = 0; i < apps.n; i++)
    if (title_starts(w->title, apps.v[i]->name))
      return app_icon(i);
  return icon_lookup(ICON_DEFAULT);
}
