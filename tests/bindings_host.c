/* Host stand-ins for the OS services user/apps/browser/js/bindings.c calls. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#define usable_size(p) malloc_size(p)
#else
#include <malloc.h>
#define usable_size(p) malloc_usable_size(p)
#endif
#include "cast.h"
#include "clipboard.h"
#include "features/notification/notification.h"
#include "jsint.h"
#include "notify.h"
#include "websocket.h"

struct browser *B;

void *poison_realloc(void *ptr, unsigned long size)
{
  size_t old = ptr ? usable_size(ptr) : 0;
  void *np = realloc(ptr, size);
  if (np && size > old)
    memset((char *)np + old, 0xA5, size - old);
  return np;
}
int BW = 1024, BH = 768;

bool futex_wait(unsigned *addr, unsigned expected, unsigned ms)
{
  (void)addr;
  (void)expected;
  (void)ms;
  return true;
}
int futex_wake(unsigned *addr, int n)
{
  (void)addr;
  (void)n;
  return 0;
}
long long fake_now_ms;

void *browser_js_stack_floor(void) { return 0; }
unsigned char *browser_cors_fetch(const char *page, int cred, const struct http_req *rq, struct http_resp *rs)
{
  (void)page;
  (void)cred;
  (void)rq;
  (void)rs;
  return 0;
}
/* Image fixtures: a data: URL carries its bytes verbatim after the comma; "RAW" w h then w*h*4 RGBA decodes to itself
 */
unsigned char *decode_data_url(const char *url, int *outlen, char *ctype, int cap)
{
  const char *comma = strchr(url, ',');
  if (!comma)
    return 0;
  int n = (int)strlen(comma + 1);
  unsigned char *out = malloc((size_t)n + 1);
  memcpy(out, comma + 1, (size_t)n + 1);
  *outlen = n;
  snprintf(ctype, (size_t)cap, "image/x-raw");
  return out;
}
unsigned char *image_decode_rgba(const unsigned char *data, int len, unsigned current_color, int max_edge, unsigned *w,
                                 unsigned *h)
{
  (void)current_color;
  (void)max_edge;
  if (len < 5 || memcmp(data, "RAW", 3) != 0 || len < 5 + data[3] * data[4] * 4)
    return 0;
  *w = data[3];
  *h = data[4];
  unsigned char *out = malloc((size_t)*w * *h * 4);
  memcpy(out, data + 5, (size_t)*w * *h * 4);
  return out;
}
int woff_is_woff(const unsigned char *d, int n)
{
  (void)d;
  (void)n;
  return 0;
}
unsigned char *woff_to_sfnt(const unsigned char *d, int n, int *out)
{
  (void)d;
  (void)n;
  (void)out;
  return 0;
}
unsigned char *browser_fetch(const struct http_req *rq, struct http_resp *rs)
{
  (void)rq;
  (void)rs;
  return 0;
}
int browser_range_box(int s, int e, int *x, int *y, int *w, int *h)
{
  (void)s;
  (void)e;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  return 0;
}
int browser_scroll_px(void) { return 0; }
const char *browser_ua_version(void) { return "5.0 (OwOS)"; }
const char *browser_ua(void) { return "Mozilla/5.0 (OwOS)"; }
void browser_viewport_px(int *w, int *h)
{
  *w = BW;
  *h = BH;
}
int browser_form_submit(struct dom_node *n, int fire)
{
  (void)n;
  (void)fire;
  return 0;
}
int clip_get(int idx, char *mime, int mm, void **out, int *len)
{
  (void)idx;
  (void)mime;
  (void)mm;
  (void)out;
  (void)len;
  return -1;
}
int clip_put_text(const char *t, int len)
{
  (void)t;
  (void)len;
  return 0;
}
int clip_put(const char *m, const void *d, int len)
{
  (void)m;
  (void)d;
  (void)len;
  return 0;
}
void devtools_console_append(int level, const char *s, int len)
{
  (void)level;
  (void)s;
  (void)len;
}
int html_node_span(struct dom_node *n, int *s, int *e)
{
  (void)n;
  (void)s;
  (void)e;
  return 0;
}
void js_cast_page_end(void) {}
void js_idb_setup(js_ctx *c, js_object *w)
{
  (void)c;
  (void)w;
}
void js_idb_teardown(void) {}
unsigned msec(void)
{
  static unsigned clock = 1000;
  return clock += 5;
}
void msleep(unsigned ms) { (void)ms; }
long long now_ms(void) { return 1720000000000LL + fake_now_ms; }
unsigned long rtc_epoch(void) { return 1720000000UL; }
void rng_bytes(void *b, unsigned long n)
{
  unsigned char *p = b;
  while (n--)
    *p++ = (unsigned char)rand();
}
int notify_send(const struct notification *n)
{
  (void)n;
  return -1;
}
void setup_media_proto(js_ctx *c) { (void)c; }
void setup_remote_proto(js_ctx *c) { (void)c; }
js_value image_ctor(js_ctx *c, js_value t, js_value *a, int n)
{
  (void)c;
  (void)t;
  (void)a;
  (void)n;
  return js_undef();
}
void sw_install(js_ctx *c, js_object *w, const char *u)
{
  (void)c;
  (void)w;
  (void)u;
}
void sw_teardown(void) {}
int sw_intercept_fetch(js_ctx *c, const char *u, const char *m, js_value *o)
{
  (void)c;
  (void)u;
  (void)m;
  (void)o;
  return 0;
}
void ws_close(ws_conn *c, int code, const char *r)
{
  (void)c;
  (void)code;
  (void)r;
}
void ws_free(ws_conn *c) { (void)c; }
ws_conn *ws_open(const char *u, const char *p)
{
  (void)u;
  (void)p;
  return 0;
}
int ws_poll(ws_conn *c, const uint8_t **o, int *l, int *t)
{
  (void)c;
  (void)o;
  (void)l;
  (void)t;
  return -1;
}
const char *ws_protocol(const ws_conn *c)
{
  (void)c;
  return "";
}
int ws_send(ws_conn *c, const void *d, int l, int t)
{
  (void)c;
  (void)d;
  (void)l;
  (void)t;
  return -1;
}
int ws_state(const ws_conn *c)
{
  (void)c;
  return 3;
}

void mediasession_install(js_ctx *ctx, js_object *navigator, const char *page_url)
{
  (void)ctx;
  (void)navigator;
  (void)page_url;
}

void mediasession_teardown(void) {}
