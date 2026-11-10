/* Host stand-ins for the OS services user/apps/browser/js/bindings.c calls. */
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

/* A futex over one condition variable: every wake is a broadcast, which futex callers already tolerate */
static pthread_mutex_t fx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fxc = PTHREAD_COND_INITIALIZER;

bool futex_wait(unsigned *addr, unsigned expected, unsigned ms)
{
  bool timed_out = false;
  pthread_mutex_lock(&fx);
  if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) == expected) {
    if (ms) {
      struct timespec t;
      clock_gettime(CLOCK_REALTIME, &t);
      t.tv_sec += ms / 1000;
      t.tv_nsec += (long)(ms % 1000) * 1000000;
      if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
      }
      timed_out = pthread_cond_timedwait(&fxc, &fx, &t) != 0;
    } else {
      pthread_cond_wait(&fxc, &fx);
    }
  }
  pthread_mutex_unlock(&fx);
  return timed_out;
}
int futex_wake(unsigned *addr, int n)
{
  (void)addr;
  (void)n;
  pthread_mutex_lock(&fx);
  pthread_cond_broadcast(&fxc);
  pthread_mutex_unlock(&fx);
  return 0;
}

/* Threads run on malloc'd stacks so the engine's stack limit means what it does in the guest */
#define HOST_THREADS 64
static struct {
  pthread_t th;
  char *base;
  void *top;
  void (*fn)(void *);
  void *arg;
  int used, done;
} hthreads[HOST_THREADS];

void *thread_stack_alloc(int bytes)
{
  int i;
  for (i = 0; i < HOST_THREADS; i++)
    if (!hthreads[i].base) {
      hthreads[i].base = malloc((size_t)bytes);
      hthreads[i].top = hthreads[i].base ? hthreads[i].base + bytes : 0;
      return hthreads[i].top;
    }
  return 0;
}

void thread_stack_free(void *top)
{
  int i;
  for (i = 0; i < HOST_THREADS; i++)
    if (hthreads[i].top == top) {
      free(hthreads[i].base);
      memset(&hthreads[i], 0, sizeof hthreads[i]);
    }
}

static void host_thread_done(void *p) { __atomic_store_n(&((typeof(hthreads[0]) *)p)->done, 1, __ATOMIC_RELEASE); }

static void *host_thread_main(void *p)
{
  pthread_cleanup_push(host_thread_done, p);
  ((typeof(hthreads[0]) *)p)->fn(((typeof(hthreads[0]) *)p)->arg);
  pthread_cleanup_pop(1);
  return 0;
}

int thread_create_arg(void (*fn)(void *), void *arg, void *stack_top)
{
  int i;
  for (i = 0; i < HOST_THREADS; i++)
    if (hthreads[i].top == stack_top && !hthreads[i].used) {
      pthread_attr_t attr;
      size_t size = (size_t)((char *)hthreads[i].top - hthreads[i].base);
      hthreads[i].fn = fn;
      hthreads[i].arg = arg;
      hthreads[i].used = 1;
      pthread_attr_init(&attr);
      pthread_attr_setstack(&attr, hthreads[i].base, size);
      if (pthread_create(&hthreads[i].th, &attr, host_thread_main, &hthreads[i]) != 0)
        return -1;
      return i + 1;
    }
  return -1;
}

int waitpid_nb(int pid, int *status)
{
  if (pid < 1 || pid > HOST_THREADS || !hthreads[pid - 1].used ||
      !__atomic_load_n(&hthreads[pid - 1].done, __ATOMIC_ACQUIRE))
    return 0;
  pthread_join(hthreads[pid - 1].th, 0);
  hthreads[pid - 1].used = 0;
  *status = 0;
  return 1;
}
long long fake_now_ms;

void *browser_js_stack_floor(void) { return 0; }
/* A fixture server: /echo answers with the method, URL, content type and body it was sent; /missing is a 404 */
unsigned char *browser_cors_fetch(const char *page, int cred, const struct http_req *rq, struct http_resp *rs)
{
  char text[1024];
  int n;
  unsigned char *out;
  (void)page;
  if (!strstr(rq->url, "/echo") && !strstr(rq->url, "/missing"))
    return 0;
  n = strstr(rq->url, "/missing")
          ? snprintf(text, sizeof text, "nope")
          : snprintf(text, sizeof text, "%s %s %s cred=%d [%.*s] %s", rq->method, rq->url, rq->ctype ? rq->ctype : "-",
                     cred, rq->body_len, rq->body ? (const char *)rq->body : "", rq->extra_hdrs ? rq->extra_hdrs : "");
  rs->status = strstr(rq->url, "/missing") ? 404 : 200;
  rs->len = n;
  snprintf(rs->ctype, sizeof rs->ctype, "text/plain");
  if (rs->final_url)
    snprintf(rs->final_url, (size_t)rs->final_cap, "%s", rq->url);
  if (rs->hdr)
    snprintf(rs->hdr, (size_t)rs->hdr_cap,
             "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nX-Fixture: yes\r\nSet-Cookie: a=b\r\n\r\n", rs->status,
             rs->status == 404 ? "Not Found" : "OK");
  out = malloc((size_t)n + 1);
  memcpy(out, text, (size_t)n + 1);
  return out;
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
int image_sniff(const unsigned char *data, int len) { return len >= 3 && memcmp(data, "RAW", 3) == 0 ? 3 : 0; }
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
/* Like the real transport, serves blob: object URLs from the blob store and nothing else */
unsigned char *browser_fetch(const struct http_req *rq, struct http_resp *rs)
{
  unsigned char *body = starts_ci(rq->url, "blob:") ? blobstore_get(rq->url, &rs->len, rs->ctype, sizeof rs->ctype) : 0;
  rs->status = body ? 200 : 0;
  return body;
}
/* Box every node reports; tests set it to fake a laid-out page (0 = no layout) */
int test_box_x, test_box_y, test_box_w, test_box_h, test_scroll, test_doc_h;
int browser_range_box(int s, int e, int *x, int *y, int *w, int *h)
{
  (void)s;
  (void)e;
  *x = test_box_x;
  *y = test_box_y;
  *w = test_box_w;
  *h = test_box_h;
  return test_box_w > 0;
}
int browser_scroll_px(void) { return test_scroll; }
int browser_doc_height_px(void) { return test_doc_h; }
const char *browser_ua_version(void) { return "5.0 (OwOS)"; }
const char *browser_ua(void) { return "Mozilla/5.0 (OwOS)"; }
void browser_viewport_px(int *w, int *h)
{
  *w = BW;
  *h = BH;
}
int html_control_kind(struct dom_node *n)
{
  return n && streq(n->tag, "input") && dom_get_attr(n, "type") && streq(dom_get_attr(n, "type"), "file") ? CTRL_FILE
                                                                                                          : CTRL_NONE;
}
void browser_file_input_activate(struct dom_node *n) { (void)n; }
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
  *s = *e = 0;
  return test_box_w > 0;
}
void js_cast_page_end(void) {}
unsigned msec(void)
{
  static unsigned clock = 1000;
  return __atomic_add_fetch(&clock, 5, __ATOMIC_RELAXED);
}
void msleep(unsigned ms)
{
  struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
  nanosleep(&t, 0);
}
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
/* An echo server: ws://.../echo answers every message with itself, close ends the connection */
struct ws_conn {
  unsigned char *msg[16];
  int len[16], text[16], n, state, closing;
  unsigned char *current;
};
void ws_close(ws_conn *c, int code, const char *r)
{
  (void)code;
  (void)r;
  c->state = 2;
  c->closing = 1;
}
void ws_free(ws_conn *c)
{
  int i;
  if (!c)
    return;
  for (i = 0; i < c->n; i++)
    free(c->msg[i]);
  free(c->current);
  free(c);
}
ws_conn *ws_open(const char *u, const char *p)
{
  ws_conn *c;
  (void)p;
  if (!strstr(u, "/echo"))
    return 0;
  c = calloc(1, sizeof *c);
  c->state = 1;
  return c;
}
int ws_poll(ws_conn *c, const uint8_t **o, int *l, int *t)
{
  int i;
  if (c->n) {
    free(c->current);
    c->current = c->msg[0];
    *o = c->current;
    *l = c->len[0];
    *t = c->text[0];
    for (i = 1; i < c->n; i++) {
      c->msg[i - 1] = c->msg[i];
      c->len[i - 1] = c->len[i];
      c->text[i - 1] = c->text[i];
    }
    c->n--;
    return 1;
  }
  if (c->closing) {
    c->state = 3;
    return 2;
  }
  return 0;
}
const char *ws_protocol(const ws_conn *c)
{
  (void)c;
  return "echo.v1";
}
int ws_send(ws_conn *c, const void *d, int l, int t)
{
  if (c->n == 16)
    return -1;
  c->msg[c->n] = malloc((size_t)l + 1);
  memcpy(c->msg[c->n], d, (size_t)l);
  c->len[c->n] = l;
  c->text[c->n++] = t;
  return 0;
}
int ws_state(const ws_conn *c) { return c->state; }

void mediasession_install(js_ctx *ctx, js_object *navigator, const char *page_url)
{
  (void)ctx;
  (void)navigator;
  (void)page_url;
}

void mediasession_teardown(void) {}

void gamepad_install(js_ctx *ctx, js_object *navigator)
{
  (void)ctx;
  (void)navigator;
}

void gamepad_teardown(void) {}
void bluetooth_install(js_ctx *ctx, js_object *navigator, const char *page_url)
{
  (void)ctx;
  (void)navigator;
  (void)page_url;
}
void bluetooth_teardown(void) {}

/* PCM sink for the audio worklet: counts the frames it was given and keeps the loudest sample (the worklet runs on
 * its own thread, so everything the tests read is atomic) */
int host_audio_available = 1;
int host_audio_opens;
long host_audio_frames;
int host_audio_peak;
static int audio_is_open;

int audio_open(void)
{
  if (!__atomic_load_n(&host_audio_available, __ATOMIC_RELAXED) || audio_is_open)
    return -1;
  audio_is_open = 1;
  __atomic_add_fetch(&host_audio_opens, 1, __ATOMIC_RELAXED);
  return 0;
}

int audio_write(const void *pcm, int len)
{
  const short *s = pcm;
  int i, peak = __atomic_load_n(&host_audio_peak, __ATOMIC_RELAXED);
  if (!audio_is_open)
    return -1;
  for (i = 0; i < len / 2; i++) {
    int v = s[i] < 0 ? -s[i] : s[i];
    if (v > peak)
      peak = v;
  }
  __atomic_store_n(&host_audio_peak, peak, __ATOMIC_RELAXED);
  __atomic_add_fetch(&host_audio_frames, len / 4, __ATOMIC_RELAXED);
  msleep((unsigned)(len / 4 * 1000 / 48000));
  return len;
}

int audio_close(void)
{
  audio_is_open = 0;
  return 0;
}
