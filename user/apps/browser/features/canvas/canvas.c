/* canvas.c: registry of canvas surfaces, shared fonts and sizing. */
#include "canvas.h"
#include "browser.h"
#include "dom.h"
#include "font.h"
#include "fontstore.h"

#define MAX_SIDE 16384
#define MAX_RECS 256

static struct canvas_rec *g_recs;
static mutex_t g_list_lock = MUTEX_INIT;
static int g_nrecs, g_next_id;
static struct font *g_sans, *g_mono;
static bool g_fonts_tried;

static struct font *load_font(const char *path)
{
  struct statinfo st;
  if (stat(path, &st) < 0 || st.is_dir || st.size == 0)
    return 0;
  unsigned char *buf = malloc(st.size);
  if (!buf)
    return 0;
  struct font *f = readfile_at(path, 0, buf, st.size) == (int)st.size ? font_load(buf, (int)st.size) : 0;
  if (!f)
    free(buf); /* the font keeps pointing into buf, so it stays allocated for the process lifetime */
  return f;
}

static void attach_fonts(struct c2d *c)
{
  if (!g_fonts_tried) {
    g_fonts_tried = true;
    char path[96];
    fontstore_path(FONT_SANS, path, sizeof path);
    g_sans = load_font(path);
    fontstore_path(FONT_MONO, path, sizeof path);
    g_mono = load_font(path);
  }
  c->sans = g_sans;
  c->mono = g_mono;
}

static int attr_dim(const struct dom_node *el, const char *name, int def)
{
  const char *v = dom_get_attr(el, name);
  long n = 0;
  if (!v || *v < '0' || *v > '9')
    return def;
  for (; *v >= '0' && *v <= '9' && n <= MAX_SIDE; v++)
    n = n * 10 + (*v - '0');
  return n > MAX_SIDE ? MAX_SIDE : (int)n;
}

void canvas_attr_size(const struct dom_node *el, int *w, int *h)
{
  *w = attr_dim(el, "width", CANVAS_DEFAULT_W);
  *h = attr_dim(el, "height", CANVAS_DEFAULT_H);
}

struct canvas_rec *canvas_find(const struct dom_node *el)
{
  struct canvas_rec *found = 0;
  mutex_lock(&g_list_lock);
  for (struct canvas_rec *r = g_recs; r && !found; r = r->next)
    if (r->el == el)
      found = r;
  mutex_unlock(&g_list_lock);
  return found;
}

static struct canvas_rec *add_rec(struct dom_node *el, int w, int h, void *owner)
{
  struct canvas_rec *r;
  mutex_lock(&g_list_lock);
  if (g_nrecs >= MAX_RECS) {
    mutex_unlock(&g_list_lock);
    return 0;
  }
  mutex_unlock(&g_list_lock);
  r = calloc(1, sizeof *r);
  if (!r)
    return 0;
  r->c = c2d_new(w, h);
  if (!r->c) {
    free(r);
    return 0;
  }
  attach_fonts(r->c);
  r->owner = owner;
  r->el = el;
  mutex_lock(&g_list_lock);
  r->id = ++g_next_id;
  r->next = g_recs;
  g_recs = r;
  g_nrecs++;
  mutex_unlock(&g_list_lock);
  return r;
}

struct canvas_rec *canvas_open(struct dom_node *el, void *owner)
{
  struct canvas_rec *r = canvas_find(el);
  int w, h;
  if (r)
    return r;
  canvas_attr_size(el, &w, &h);
  return add_rec(el, w, h, owner);
}

struct canvas_rec *canvas_open_offscreen(int w, int h, void *owner) { return add_rec(0, w, h, owner); }

struct canvas_rec *canvas_find_id(int id)
{
  struct canvas_rec *found = 0;
  mutex_lock(&g_list_lock);
  for (struct canvas_rec *r = g_recs; r && !found; r = r->next)
    if (r->id == id)
      found = r;
  mutex_unlock(&g_list_lock);
  return found;
}

bool canvas_resize(struct canvas_rec *r, int w, int h)
{
  mutex_lock(&r->lock);
  bool ok = c2d_resize(r->c, w, h);
  free(r->front);
  r->front = 0;
  mutex_unlock(&r->lock);
  if (ok)
    r->pending = 1;
  return ok;
}

void canvas_page_end(void *owner)
{
  mutex_lock(&g_list_lock);
  for (struct canvas_rec **pp = &g_recs; *pp;) {
    struct canvas_rec *r = *pp;
    if (r->owner != owner) {
      pp = &r->next;
      continue;
    }
    *pp = r->next;
    g_nrecs--;
    mutex_lock(&r->lock);
    c2d_free(r->c);
    free(r->front);
    mutex_unlock(&r->lock);
    free(r);
  }
  mutex_unlock(&g_list_lock);
}

static void flush_one(struct canvas_rec *r)
{
  if (!r->pending)
    return;
  r->pending = 0;
  unsigned long bytes = (unsigned long)r->c->w * r->c->h * 4;
  mutex_lock(&r->lock);
  if (!r->front)
    r->front = malloc(bytes);
  if (r->front) {
    memcpy(r->front, r->c->px, bytes);
    r->dirty = 1;
  }
  mutex_unlock(&r->lock);
}

void canvas_flush(void) { canvas_each(flush_one); }

void canvas_each(void (*fn)(struct canvas_rec *r))
{
  mutex_lock(&g_list_lock);
  for (struct canvas_rec *r = g_recs; r; r = r->next)
    fn(r);
  mutex_unlock(&g_list_lock);
}
