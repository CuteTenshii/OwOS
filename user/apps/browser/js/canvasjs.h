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

extern js_object *g_path2d_proto, *g_imagedata_proto;

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
void canvas_img_setup(js_ctx *ctx, js_object *win);
void canvas_img_teardown(void);

/* Run `fn` once from the timer queue with `data` readable as ctx->native_data on entry. */
void queue_native(js_ctx *ctx, js_native fn, js_value data);

js_value js_illegal_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc);

void canvas_js_setup(js_ctx *ctx, js_object *win);
void canvas_js_teardown(void);

#endif /* BROWSER_JS_CANVASJS_H */
