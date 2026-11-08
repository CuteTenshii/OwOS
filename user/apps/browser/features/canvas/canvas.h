/* canvas.h: the page's <canvas> surfaces. A record couples a DOM element (or a detached/offscreen canvas) to its
 * canvas2d bitmap; layout publishes the pixels into the image pool and canvas_tick keeps them current. */
#ifndef BROWSER_CANVAS_H
#define BROWSER_CANVAS_H

#include "canvas2d.h"
#include "mutex.h"
#include <stdbool.h>

struct dom_node;

#define CANVAS_DEFAULT_W 300
#define CANVAS_DEFAULT_H 150

struct canvas_rec {
  struct canvas_rec *next;
  void *owner;         /* the JS context that created it */
  struct dom_node *el; /* NULL for OffscreenCanvas */
  int id;              /* never reused */
  struct c2d *c;
  int uid;              /* image-pool id once laid out (0 = not on the page) */
  volatile int pending; /* drawn into since the last flush (JS thread) */
  volatile int dirty;   /* front differs from the pool copy (UI thread consumes) */
  unsigned char *front; /* last finished frame, what the UI copies; guarded by lock */
  mutex_t lock;         /* guards c->px against resize while the UI copies it */
};

/* The record for `el`, created at the size of its width/height attributes. NULL when out of memory. */
struct canvas_rec *canvas_open(struct dom_node *el, void *owner);
struct canvas_rec *canvas_find(const struct dom_node *el);
struct canvas_rec *canvas_open_offscreen(int w, int h, void *owner);

/* Width/height attributes of `el`, falling back to 300x150. */
void canvas_attr_size(const struct dom_node *el, int *w, int *h);

/* Resize (clearing the bitmap and resetting state); false on a bad size or no memory. */
bool canvas_resize(struct canvas_rec *r, int w, int h);

/* Drop every record created by `owner`. */
void canvas_page_end(void *owner);

/* Visit every record under the registry lock. */
void canvas_each(void (*fn)(struct canvas_rec *r));

/* JS thread, once its tasks have run: snapshot canvases drawn into so the UI never sees a half-drawn frame. */
void canvas_flush(void);

/* UI thread: copy changed canvases into the image pool and repaint their rows. */
void canvas_tick(void);

#endif /* BROWSER_CANVAS_H */
