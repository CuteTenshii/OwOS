/* <img> loading for scripts (naturalWidth, load events, decode()), ImageBitmap and queued native callbacks. */

#include "bindings.h"
#include "canvasjs.h"
#include "fetchint.h"
#include "imgdec.h"

#define CACHE_SLOTS 24
#define MAX_IMAGE_SIDE 8192

static struct cached_image {
  char url[URL_MAX];
  unsigned char *rgba;
  int w, h;
  unsigned age;
} g_cache[CACHE_SLOTS];
static unsigned g_age;

static js_object *g_bitmap_proto;

void queue_native(js_ctx *ctx, js_native fn, js_value data)
{
  js_object *f = js_new_native(ctx, fn, "", 0);
  gc_push_scope();
  gc_keep(f);
  f->fn->data = data;
  js_value args[2] = {js_obj_value(f), js_number(0)};
  js_call(ctx, js_get(ctx, js_obj_value(ctx->global), "setTimeout"), js_undef(), args, 2);
  gc_pop_scope();
}

/* ------------------------------------------------------------ decoded images */

static struct cached_image *cache_find(const char *url)
{
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (g_cache[i].rgba && streq(g_cache[i].url, url)) {
      g_cache[i].age = ++g_age;
      return &g_cache[i];
    }
  return 0;
}

static struct cached_image *cache_slot(void)
{
  struct cached_image *best = &g_cache[0];
  for (int i = 0; i < CACHE_SLOTS; i++) {
    if (!g_cache[i].rgba)
      return &g_cache[i];
    if (g_cache[i].age < best->age)
      best = &g_cache[i];
  }
  free(best->rgba);
  best->rgba = 0;
  return best;
}

static unsigned char *fetch_image_bytes(const char *url, int *len)
{
  *len = 0;
  if (starts_ci(url, "data:")) {
    char ctype[64];
    return decode_data_url(url, len, ctype, sizeof ctype);
  }
  if (!starts_ci(url, "http://") && !starts_ci(url, "https://") && !starts_ci(url, "blob:"))
    return 0;
  struct http_req rq = {0};
  struct http_resp rs = {0};
  rq.url = url;
  rq.accept = "image/png,image/webp,image/gif,image/svg+xml,*/*";
  unsigned char *body = browser_fetch(&rq, &rs);
  if (body && rs.status != 200) {
    free(body);
    return 0;
  }
  *len = rs.len;
  return body;
}

const unsigned char *js_image_pixels(struct dom_node *img, int *w, int *h)
{
  const char *src = dom_get_attr(img, "src");
  char url[URL_MAX];
  if (!src || !*src)
    return 0;
  if (starts_ci(src, "data:") || starts_ci(src, "blob:") || starts_ci(src, "http://") || starts_ci(src, "https://"))
    strlcpy(url, src, sizeof url);
  else
    resolve_url(g_page_url_buf, src, url, sizeof url);
  if (B->pg->csp) {
    char use[URL_MAX], reason[64];
    if (!csp_check_url(B->pg->csp, CSP_IMG, url, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
      js_csp_log("image load of", url, reason);
      return 0;
    }
    strlcpy(url, use, sizeof url);
  }
  struct cached_image *c = cache_find(url);
  if (!c) {
    int len;
    unsigned char *body = fetch_image_bytes(url, &len);
    if (!body)
      return 0;
    unsigned dw = 0, dh = 0;
    unsigned char *px = image_decode_rgba(body, len, 0, 256, &dw, &dh);
    free(body);
    if (!px)
      return 0;
    if (dw > MAX_IMAGE_SIDE || dh > MAX_IMAGE_SIDE) {
      free(px);
      return 0;
    }
    c = cache_slot();
    strlcpy(c->url, url, sizeof c->url);
    c->rgba = px;
    c->w = (int)dw;
    c->h = (int)dh;
    c->age = ++g_age;
  }
  *w = c->w;
  *h = c->h;
  return c->rgba;
}

/* ---------------------------------------------------------- HTMLImageElement */

static struct dom_node *img_node(js_ctx *ctx, js_value t)
{
  struct dom_node *n = node_of(t);
  if (!n || !streq(n->tag, "img"))
    js_throw_type(ctx, "Illegal invocation");
  return n && streq(n->tag, "img") ? n : 0;
}

static js_value fire_image_event(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value pair = ctx->native_data;
  js_value target = js_get_index(ctx, pair, 0);
  (void)t, (void)args, (void)argc;
  if (target.type == JV_OBJ)
    emit_event(target.u.obj, js_to_boolean(js_get_index(ctx, pair, 1)) ? "load" : "error", js_undef());
  return js_undef();
}

static js_value img_get_src(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = img_node(ctx, t);
  const char *v = n ? dom_get_attr(n, "src") : 0;
  char url[URL_MAX];
  (void)args, (void)argc;
  if (!v || !*v)
    return js_mk_cstring(ctx, "");
  resolve_url(g_page_url_buf, v, url, sizeof url);
  return js_mk_cstring(ctx, url);
}

/* Setting src loads synchronously; the load or error event follows from the timer queue */
static js_value img_set_src(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = img_node(ctx, t);
  if (!n)
    return js_undef();
  dom_set_attr(n, "src", arg_str(ctx, A(0)));
  int w = 0, h = 0;
  bool ok = js_image_pixels(n, &w, &h) != 0;
  gc_push_scope();
  js_object *pair = gc_keep(js_new_array(ctx, 0));
  js_array_push(ctx, pair, t);
  js_array_push(ctx, pair, js_bool(ok));
  queue_native(ctx, fire_image_event, js_obj_value(pair));
  gc_pop_scope();
  return js_undef();
}

static js_value natural_size(js_ctx *ctx, js_value t, bool height)
{
  struct dom_node *n = img_node(ctx, t);
  int w = 0, h = 0;
  if (n && js_image_pixels(n, &w, &h))
    return js_number(height ? h : w);
  return js_number(0);
}

static js_value img_natural_width(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  return natural_size(ctx, t, false);
}

static js_value img_natural_height(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  return natural_size(ctx, t, true);
}

static js_value img_complete(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx, (void)t, (void)args, (void)argc;
  return js_bool(1); /* loads finish inside the src setter */
}

static js_value dim_get(js_ctx *ctx, js_value t, bool height)
{
  struct dom_node *n = img_node(ctx, t);
  const char *v = n ? dom_get_attr(n, height ? "height" : "width") : 0;
  if (v && *v)
    return js_number(js_to_number(ctx, js_mk_cstring(ctx, v)));
  return natural_size(ctx, t, height);
}

static js_value dim_set(js_ctx *ctx, js_value t, js_value v, bool height)
{
  struct dom_node *n = img_node(ctx, t);
  if (n)
    dom_set_attr(n, height ? "height" : "width", arg_str(ctx, v));
  return js_undef();
}

static js_value img_get_width(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  return dim_get(ctx, t, false);
}
static js_value img_get_height(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  return dim_get(ctx, t, true);
}
static js_value img_set_width(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return dim_set(ctx, t, A(0), false);
}
static js_value img_set_height(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return dim_set(ctx, t, A(0), true);
}

static js_value img_decode(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = img_node(ctx, t);
  int w, h;
  (void)args, (void)argc;
  if (n && js_image_pixels(n, &w, &h))
    return js_promise_resolved(ctx, js_undef());
  return js_promise_rejected(
      ctx, js_make_error(ctx, ctx->error_proto, "EncodingError", "The source image cannot be decoded"));
}

/* --------------------------------------------------------------- ImageBitmap */

static js_value bitmap_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct chost *b = host_of(t, HK_BITMAP);
  (void)ctx, (void)args, (void)argc;
  if (b) {
    free(b->p);
    b->p = 0;
    b->w = b->h = 0;
  }
  return js_undef();
}

static js_value create_image_bitmap(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct pixsrc src;
  (void)t;
  int rc = source_pixels(ctx, A(0), &src);
  if (rc == SRC_BAD_TYPE)
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError",
                           "The provided value is not of type '(HTMLImageElement or HTMLCanvasElement or ImageData)'"));
  if (rc == SRC_NOT_READY)
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->error_proto, "InvalidStateError", "The source image is not ready"));
  int sx = 0, sy = 0, sw = src.w, sh = src.h;
  if (argc >= 5) {
    sx = js_to_int_sat(ctx, args[1]);
    sy = js_to_int_sat(ctx, args[2]);
    sw = js_to_int_sat(ctx, args[3]);
    sh = js_to_int_sat(ctx, args[4]);
  }
  if (sw <= 0 || sh <= 0 || (long long)sw * sh > 16 * 1024 * 1024)
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->error_proto, "RangeError", "The crop rectangle is empty or too large"));
  unsigned char *px = calloc((unsigned)sw * (unsigned)sh, 4);
  if (!px)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "RangeError", "out of memory"));
  for (int y = 0; y < sh; y++) {
    int yy = sy + y;
    if (yy < 0 || yy >= src.h)
      continue;
    int x0 = sx < 0 ? -sx : 0, x1 = sx + sw > src.w ? src.w - sx : sw;
    if (x1 > x0)
      memcpy(px + ((unsigned long)y * sw + x0) * 4, src.rgba + ((unsigned long)yy * src.w + sx + x0) * 4,
             (unsigned long)(x1 - x0) * 4);
  }
  gc_push_scope();
  js_value bm = make_host(ctx, g_bitmap_proto, HK_BITMAP, px, sw, sh);
  if (bm.type != JV_OBJ) {
    free(px);
    gc_pop_scope();
    return js_promise_rejected(ctx, bm);
  }
  gc_keep(bm.u.obj);
  js_def(bm.u.obj, "width", js_number(sw), P_ENUMERABLE);
  js_def(bm.u.obj, "height", js_number(sh), P_ENUMERABLE);
  js_value p = js_promise_resolved(ctx, bm);
  gc_pop_scope();
  return p;
}

void canvas_img_setup(js_ctx *ctx, js_object *win)
{
  g_bitmap_proto = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)&g_bitmap_proto);
  def_method(g_bitmap_proto, "close", bitmap_close, 0);
  def_method(win, "createImageBitmap", create_image_bitmap, 1);
  js_object *ic = js_new_native(ctx, js_illegal_ctor, "ImageBitmap", 0);
  gc_push_scope();
  gc_keep(ic);
  js_def(ic, "prototype", js_obj_value(g_bitmap_proto), 0);
  js_def(g_bitmap_proto, "constructor", js_obj_value(ic), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "ImageBitmap", js_obj_value(ic), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();

  for (unsigned i = 0; i < g_nhtml_ifaces; i++) {
    if (!streq(g_html_ifaces[i].name, "HTMLImageElement"))
      continue;
    js_object *ip = g_html_ifaces[i].proto;
    def_acc(ip, "src", img_get_src, img_set_src);
    def_acc(ip, "currentSrc", img_get_src, 0);
    def_acc(ip, "naturalWidth", img_natural_width, 0);
    def_acc(ip, "naturalHeight", img_natural_height, 0);
    def_acc(ip, "complete", img_complete, 0);
    def_acc(ip, "width", img_get_width, img_set_width);
    def_acc(ip, "height", img_get_height, img_set_height);
    def_method(ip, "decode", img_decode, 0);
    break;
  }
}

void canvas_img_teardown(void)
{
  gc_remove_root((void **)&g_bitmap_proto);
  g_bitmap_proto = 0;
  for (int i = 0; i < CACHE_SLOTS; i++) {
    free(g_cache[i].rgba);
    memset(&g_cache[i], 0, sizeof g_cache[i]);
  }
}
