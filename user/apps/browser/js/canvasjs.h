/* canvasjs.h: HTMLCanvasElement, CanvasRenderingContext2D and friends (js/canvas*.c). */
#ifndef BROWSER_JS_CANVASJS_H
#define BROWSER_JS_CANVASJS_H

#include "features/canvas/canvas.h"
#include "jsint.h"

/* Native objects (OBJ_HOST): `tag` says what `p` is and how the collector releases it. */
enum { HK_GRADIENT = 1, HK_PATTERN, HK_PATH, HK_BITMAP, HK_CTX, HK_OFFSCREEN };
struct chost {
  int tag;
  void *p;
  int w, h; /* HK_BITMAP: p is w*h*4 RGBA bytes */
};

/* The prototypes are per thread: the page and each worker that draws set up their own */
extern __thread js_object *g_path2d_proto, *g_imagedata_proto;

int js_nums(js_ctx *ctx, js_value *args, int argc, int n, float *out, const char *iface, const char *what);

js_value make_host(js_ctx *ctx, js_object *proto, int tag, void *p, int w, int h);
struct chost *host_of(js_value v, int tag);

/* Borrowed RGBA pixels behind a drawImage/createPattern/createImageBitmap source. */
struct pixsrc {
  const unsigned char *rgba;
  int w, h;
};
enum { SRC_OK, SRC_BAD_TYPE, SRC_NOT_READY };
int source_pixels(js_ctx *ctx, js_value v, struct pixsrc *out);

/* Path2D and the SVG path-data parser behind it (canvaspath.c). */
bool path_from_svg(struct c2d_path *p, const char *d);
void canvas_path_setup(js_ctx *ctx, js_object *win);

/* Image element loading and ImageBitmap (canvasimg.c). */
const unsigned char *js_image_pixels(struct dom_node *img, int *w, int *h);
js_value make_bitmap(js_ctx *ctx, unsigned char *px, int w, int h);
void canvas_img_setup(js_ctx *ctx, js_object *win);
void canvas_img_setup_worker(js_ctx *ctx, js_object *win); /* ImageBitmap and createImageBitmap only */
void canvas_img_teardown(void);

/* Run `fn` once from the timer queue with `data` readable as ctx->native_data on entry. */
void queue_native(js_ctx *ctx, js_native fn, js_value data);

js_value js_illegal_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc);

void canvas_js_setup(js_ctx *ctx, js_object *win);
/* The part of the canvas API a worker has: OffscreenCanvas, CanvasRenderingContext2D, ImageData, Path2D, ... */
void canvas_js_setup_worker(js_ctx *ctx, js_object *win);
/* Structured clone of an OffscreenCanvas: it travels as the id of its surface, which both threads then draw on */
int canvas_clone_write(js_ctx *ctx, js_value v, char kind[16], unsigned char **bytes, int *len, char **meta);
js_value canvas_clone_read(js_ctx *ctx, const char *kind, const unsigned char *bytes, int len);
void canvas_js_teardown(void);

#endif /* BROWSER_JS_CANVASJS_H */
