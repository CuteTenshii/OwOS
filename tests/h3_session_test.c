/* Host test for HTTP/3 sessions (user/lib/curl/h3.c) over a scripted QUIC peer. h3.c is built with its quic_* and
 * idle-pool calls renamed to the fakes below, so no sockets or crypto are involved. */
#define OWOS_SUITE_TEST
#include "h3.h"
#include "netconn.h"
#include "quic.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct quic {
  int id;
};
static struct quic g_q;

/* ---- the scripted peer */
struct fstream {
  int used;
  uint64_t sid;
  uint8_t req[8192];
  int req_len;
  uint8_t resp[16384];
  int resp_len, vis, rd, fin;
};
static struct fstream g_str[32];

enum { MODE_PLAIN, MODE_INTERIM, MODE_LONG, MODE_PUSH };
static int g_mode, g_connects, g_closes, g_dead, g_send_rc, g_peer_closed, g_streams;
static uint64_t g_next;
static int g_done_calls, g_pushes;
static char g_push_conn[64], g_push_auth[64], g_push_path[64], g_push_body[64];

static struct fstream *fs_get(uint64_t sid)
{
  for (int i = 0; i < 32; i++)
    if (g_str[i].used && g_str[i].sid == sid)
      return &g_str[i];
  for (int i = 0; i < 32; i++)
    if (!g_str[i].used) {
      memset(&g_str[i], 0, sizeof g_str[i]);
      g_str[i].used = 1;
      g_str[i].sid = sid;
      return &g_str[i];
    }
  return 0;
}

static void reset(void)
{
  memset(g_str, 0, sizeof g_str);
  g_pushes = 0;
  h3_set_push_handler(0, 0);
  g_mode = g_connects = g_closes = g_dead = g_send_rc = g_peer_closed = g_done_calls = 0;
  g_streams = 100;
  g_next = 0;
}

static int put_varint(uint8_t *p, uint32_t v)
{
  if (v < 0x40) {
    p[0] = (uint8_t)v;
    return 1;
  }
  p[0] = 0x40 | (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
  return 2;
}

static void frame(struct fstream *fs, int type, const uint8_t *pay, int len)
{
  uint8_t *p = fs->resp + fs->resp_len;
  *p++ = (uint8_t)type;
  p += put_varint(p, (uint32_t)len);
  memcpy(p, pay, (size_t)len);
  fs->resp_len = (int)(p - fs->resp) + len;
}

/* QPACK: a literal field with a literal name and a short value. */
static int lit(uint8_t *p, const char *name, const char *val, int vlen)
{
  int n = 0, nl = (int)strlen(name);
  p[n++] = (uint8_t)(0x20 | nl);
  memcpy(p + n, name, (size_t)nl);
  n += nl;
  if (vlen < 127)
    p[n++] = (uint8_t)vlen;
  else {
    p[n++] = 0x7f;
    int rest = vlen - 127;
    while (rest >= 128) {
      p[n++] = (uint8_t)(0x80 | (rest & 0x7f));
      rest >>= 7;
    }
    p[n++] = (uint8_t)rest;
  }
  memcpy(p + n, val, (size_t)vlen);
  return n + vlen;
}

static void respond(struct fstream *fs)
{
  uint8_t hb[4096];
  int n;
  if (g_mode == MODE_PUSH) { /* PUSH_PROMISE 0 for GET https://example.com/app.css, ahead of the page */
    static const uint8_t pp[] = {0x00, 0x00, 0x00, 0xd1, 0xd7, 0x50, 11,  'e', 'x', 'a', 'm', 'p', 'l', 'e',
                                 '.',  'c',  'o',  'm',  0x51, 8,    '/', 'a', 'p', 'p', '.', 'c', 's', 's'};
    frame(fs, 5, pp, sizeof pp);
  }
  if (g_mode == MODE_INTERIM) {
    n = 0;
    hb[n++] = 0, hb[n++] = 0;
    hb[n++] = 0xc0 | 24; /* :status 103 */
    n += lit(hb + n, "link", "</x.css>; rel=preload", 21);
    frame(fs, 1, hb, n);
  }
  n = 0;
  hb[n++] = 0, hb[n++] = 0;
  hb[n++] = 0xc0 | 25; /* :status 200 */
  if (g_mode == MODE_LONG) {
    static char big[3000];
    memset(big, 'a', sizeof big);
    n += lit(hb + n, "x-long", big, sizeof big);
    hb[n++] = 0xc0 | 42; /* content-encoding: br, after the long value */
  } else {
    n += lit(hb + n, "x-k", "v", 1);
  }
  static uint8_t blk[4096];
  memcpy(blk, hb, (size_t)n);
  frame(fs, 1, blk, n);
  char body[16];
  int bl = snprintf(body, sizeof body, "body%llu", (unsigned long long)fs->sid);
  frame(fs, 0, (const uint8_t *)body, bl);
  fs->fin = 1;
}

/* Bytes the server writes on a stream it opened. */
static void uni_write(uint64_t sid, const void *data, int len, int fin)
{
  struct fstream *fs = fs_get(sid);
  memcpy(fs->resp + fs->resp_len, data, (size_t)len);
  fs->resp_len += len;
  fs->fin |= fin;
}

int h3t_connect(quic **out, const char *host, uint16_t port, const char *alpn)
{
  (void)host, (void)port, (void)alpn;
  g_connects++;
  *out = &g_q;
  static const uint8_t control[3] = {0x00, 0x04, 0x00}; /* the server's control stream: type, empty SETTINGS */
  uni_write(3, control, 3, 0);
  return QUIC_OK;
}

int h3t_open(quic *q, uint64_t *sid)
{
  (void)q;
  if (g_streams-- <= 0)
    return -1;
  *sid = g_next;
  g_next += 4;
  return 0;
}

int h3t_send(quic *q, uint64_t sid, const void *data, uint32_t len, int fin)
{
  (void)q;
  if (g_send_rc)
    return g_send_rc;
  struct fstream *fs = fs_get(sid);
  memcpy(fs->req + fs->req_len, data, len);
  fs->req_len += (int)len;
  if (fin && (sid & 3) == 0 && !g_dead)
    respond(fs);
  return 0;
}

int h3t_poll(quic *q)
{
  (void)q;
  if (g_dead)
    return QUIC_ECLOSED;
  for (int i = 0; i < 32; i++) {
    struct fstream *fs = &g_str[i];
    if (fs->used && (fs->sid & 3) == 3 && fs->vis < fs->resp_len) {
      fs->vis = fs->resp_len; /* the server's own streams arrive whole */
      return 1;
    }
  }
  for (int i = 0; i < 32; i++) {
    struct fstream *fs = &g_str[i];
    if (fs->used && fs->vis < fs->resp_len) {
      fs->vis += fs->resp_len - fs->vis < 7 ? fs->resp_len - fs->vis : 7; /* dribble: frames straddle reads */
      return 1;
    }
  }
  return 0;
}

int h3t_read(quic *q, uint64_t sid, void *buf, uint32_t max, int *fin)
{
  (void)q;
  struct fstream *fs = fs_get(sid);
  int n = fs->vis - fs->rd;
  if (n > (int)max)
    n = (int)max;
  memcpy(buf, fs->resp + fs->rd, (size_t)n);
  fs->rd += n;
  if (fin)
    *fin = fs->fin && fs->rd == fs->resp_len;
  return n;
}

int h3t_next_uni(quic *q, uint64_t *sid)
{
  (void)q;
  for (int i = 0; i < 32; i++) {
    struct fstream *fs = &g_str[i];
    if (fs->used && (fs->sid & 3) == 3 && fs->vis > fs->rd) {
      *sid = fs->sid;
      return 1;
    }
    if (fs->used && (fs->sid & 3) == 3 && fs->fin && fs->vis == fs->resp_len && fs->rd == fs->resp_len &&
        fs->resp_len > 0) {
      *sid = fs->sid;
      return 1;
    }
  }
  return 0;
}

void h3t_done(quic *q, uint64_t sid)
{
  (void)q;
  g_done_calls++;
  if ((sid & 3) == 3) /* a finished server stream's slot is gone */
    fs_get(sid)->used = 0;
}

int h3t_peer_closed(quic *q) { return (void)q, g_peer_closed; }
int h3t_left(const quic *q) { return (void)q, g_streams; }
unsigned h3t_idle(const quic *q) { return (void)q, 0; }
void h3t_close(quic *q) { (void)q, g_closes++; }

/* ---- the idle pool, one slot */
static void *g_idle_obj;
static int (*g_idle_closed)(void *);

void h3t_idle_put(const char *host, int port, int tls, int kind, void *obj, void (*destroy)(void *),
                  int (*closed)(void *))
{
  (void)host, (void)port, (void)tls, (void)kind, (void)destroy;
  g_idle_obj = obj;
  g_idle_closed = closed;
}

void *h3t_idle_take(const char *host, int port, int tls, int kind)
{
  (void)host, (void)port, (void)tls, (void)kind;
  void *o = g_idle_obj;
  g_idle_obj = 0;
  return o;
}

/* ---- tests */
struct sink {
  char buf[4096];
  int len;
};

static int sink_cb(void *ctx, const unsigned char *p, int n)
{
  struct sink *s = ctx;
  memcpy(s->buf + s->len, p, (size_t)n);
  s->len += n;
  s->buf[s->len] = '\0';
  return 0;
}

static int get(h3_session *s, const char *path, struct sink *k, char *hdr, int hdrcap)
{
  memset(k, 0, sizeof *k);
  int hl = 0;
  return h3_session_run(s, "example.com", "GET", path, 0, 0, 0, 0, hdr, hdrcap, &hl, sink_cb, k);
}

static int contains(const uint8_t *hay, int hl, const char *needle)
{
  int nl = (int)strlen(needle);
  for (int i = 0; i + nl <= hl; i++)
    if (memcmp(hay + i, needle, (size_t)nl) == 0)
      return 1;
  return 0;
}

static void reuse_keeps_the_connection(void)
{
  reset();
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  expect_eq(g_connects, 1);
  expect(g_str[0].used);
  struct fstream *ctrl = fs_get(2), *enc = fs_get(6), *dec = fs_get(10);
  expect_eq(ctrl->req_len, 3); /* control stream: type 0, empty SETTINGS */
  expect_eq(ctrl->req[1], 4);
  expect_eq(enc->req[0], 2);
  expect_eq(dec->req[0], 3);

  struct sink k;
  char hdr[512];
  expect_eq(get(s, "/a", &k, hdr, sizeof hdr), 200);
  expect_string(k.buf, "body0");
  expect(strstr(hdr, "HTTP/3 200\r\n") == hdr);
  expect(strstr(hdr, "x-k: v") != 0);
  h3_session_release(s, "example.com", 443);
  expect(g_idle_obj == s);

  h3_session *again = h3_session_acquire("example.com", 443);
  expect(again == s);
  expect(h3_session_used(again));
  expect_eq(get(again, "/b", &k, hdr, sizeof hdr), 200);
  expect_string(k.buf, "body4"); /* the next client bidi stream */
  expect_eq(g_connects, 1);
  expect_eq(g_done_calls, 2);
  h3_session_close(again);
  expect_eq(g_closes, 1);
}

struct blocks {
  char b[4][512];
  int n;
};

static int collect(void *ctx, const char *block, int len)
{
  struct blocks *bl = ctx;
  if (bl->n < 4) {
    memcpy(bl->b[bl->n], block, (size_t)len);
    bl->b[bl->n++][len] = '\0';
  }
  return 0;
}

static void early_hints_arrive_separately(void)
{
  reset();
  g_mode = MODE_INTERIM;
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  struct blocks bl = {0};
  struct h3_opts opts = {collect, &bl};
  struct sink k = {0};
  char hdr[512];
  int hl = 0;
  expect_eq(h3_session_run(s, "example.com", "GET", "/", 0, 0, 0, &opts, hdr, sizeof hdr, &hl, sink_cb, &k), 200);
  expect_eq(bl.n, 2);
  expect(strncmp(bl.b[0], "HTTP/3 103", 10) == 0);
  expect(strstr(bl.b[0], "link: </x.css>; rel=preload") != 0);
  expect(strncmp(bl.b[1], "HTTP/3 200", 10) == 0);
  expect(strstr(hdr, "link:") == 0); /* the final block replaced the interim one */
  expect_string(k.buf, "body0");
  h3_session_close(s);
}

static void headers_after_a_long_value_survive(void)
{
  reset();
  g_mode = MODE_LONG;
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  struct sink k;
  char hdr[8192];
  expect_eq(get(s, "/", &k, hdr, sizeof hdr), 200);
  expect(strstr(hdr, "x-long: aaaa") != 0);
  expect(strstr(hdr, "content-encoding: br") != 0);
  h3_session_close(s);
}

static void post_sends_a_data_frame(void)
{
  reset();
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  struct sink k = {0};
  char hdr[256];
  int hl = 0;
  expect_eq(h3_session_run(s, "example.com", "POST", "/form", "content-type: text/plain\r\n",
                           (const unsigned char *)"hello=world", 11, 0, hdr, sizeof hdr, &hl, sink_cb, &k),
            200);
  struct fstream *fs = fs_get(0);
  expect(contains(fs->req, fs->req_len, "content-length"));
  expect(contains(fs->req, fs->req_len, "11"));
  expect(contains(fs->req, fs->req_len, "hello=world"));
  expect_eq(fs->req[fs->req_len - 13], 0x00); /* the DATA frame carrying the body, last on the stream */
  expect_eq(fs->req[fs->req_len - 12], 11);
  expect(memcmp(fs->req + fs->req_len - 11, "hello=world", 11) == 0);
  h3_session_close(s);
}

struct done_log {
  int order[4], n;
};

static void on_done(void *ctx, int i, const unsigned char *body, int blen)
{
  struct done_log *l = ctx;
  (void)body, (void)blen;
  l->order[l->n++] = i;
}

static void batches_use_one_stream_each(void)
{
  reset();
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  struct h2_fetch r[3] = {{.path = "/x"}, {.path = "/y"}, {.path = "/z"}};
  struct done_log log = {{0}, 0};
  expect_eq(h3_session_fetch_many(s, "example.com", 0, r, 3, on_done, &log), 0);
  for (int i = 0; i < 3; i++) {
    expect_eq(r[i].status, 200);
    expect(!r[i].failed);
  }
  expect_eq(log.n, 3);
  expect(fs_get(0)->req_len > 0 && fs_get(4)->req_len > 0 && fs_get(8)->req_len > 0);

  struct h2_fetch q[1] = {{.path = "/w"}};
  expect_eq(h3_session_fetch_many(s, "example.com", 0, q, 1, 0, 0), 0);
  expect(!q[0].failed);
  expect(q[0].blen == 6 && memcmp(q[0].body, "body12", 6) == 0); /* streams 0, 4, 8 used; the next is 12 */
  free(q[0].body);
  h3_session_close(s);
}

static void a_dead_connection_is_not_reused(void)
{
  reset();
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  struct sink k;
  char hdr[256];
  g_dead = 1;
  expect(get(s, "/", &k, hdr, sizeof hdr) < 0);
  g_idle_obj = 0;
  h3_session_release(s, "example.com", 443);
  expect(g_idle_obj == 0);
  expect_eq(g_closes, 1);
}

static void stream_limit_retires_the_session(void)
{
  reset();
  g_streams = 1;
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  struct sink k;
  char hdr[256];
  expect_eq(get(s, "/a", &k, hdr, sizeof hdr), 200);
  g_idle_obj = 0;
  h3_session_release(s, "example.com", 443); /* no streams left: not worth parking */
  expect(g_idle_obj == 0);
  expect_eq(g_closes, 1);
}

static void a_body_over_the_server_limit_fails_to_send(void)
{
  reset();
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  g_send_rc = -2;
  struct sink k = {0};
  char hdr[256];
  int hl = 0;
  expect_eq(h3_session_run(s, "example.com", "POST", "/", 0, (const unsigned char *)"x", 1, 0, hdr, sizeof hdr, &hl,
                           sink_cb, &k),
            HTTP_ESEND);
  g_send_rc = 0;
  g_idle_obj = 0;
  h3_session_release(s, "example.com", 443);
  expect(g_idle_obj == 0); /* a half-sent request leaves the connection unusable */
}

static void parked_session_reports_a_hung_up_peer(void)
{
  reset();
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  h3_session_release(s, "example.com", 443);
  expect(g_idle_closed != 0);
  g_peer_closed = 0;
  expect_eq(g_idle_closed(s), 0);
  g_peer_closed = 1;
  expect_eq(g_idle_closed(s), 1);
  g_peer_closed = 0;
  h3_session_close(s);
}

static int put_frame(uint8_t *p, int type, const uint8_t *pay, int len)
{
  p[0] = (uint8_t)type;
  p[1] = (uint8_t)len;
  memcpy(p + 2, pay, (size_t)len);
  return 2 + len;
}

static void goaway_stops_the_session_and_refuses_unprocessed_requests(void)
{
  reset();
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  struct sink k;
  char hdr[256];
  expect_eq(get(s, "/a", &k, hdr, sizeof hdr), 200); /* stream 0, before any GOAWAY */
  const uint8_t goaway4[3] = {0x07, 0x01, 0x04};     /* streams from 4 up are not processed */
  uni_write(3, goaway4, 3, 0);
  g_idle_obj = 0;
  h3_session_release(s, "example.com", 443); /* this one still served its request, but is not worth keeping */
  expect_eq(g_closes, 0);                    /* the GOAWAY has not been read yet: parked */
  expect(g_idle_obj == s);

  s = h3_session_acquire("example.com", 443);
  expect_eq(get(s, "/b", &k, hdr, sizeof hdr), HTTP_ESEND); /* stream 4: the GOAWAY arrives, it is refused */
  g_idle_obj = 0;
  h3_session_release(s, "example.com", 443);
  expect(g_idle_obj == 0); /* closed, not parked */
  expect_eq(g_closes, 1);
}

static void a_goaway_with_room_lets_earlier_requests_finish(void)
{
  reset();
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  const uint8_t goaway8[3] = {0x07, 0x01, 0x08};
  uni_write(3, goaway8, 3, 0);
  struct sink k;
  char hdr[256];
  expect_eq(get(s, "/a", &k, hdr, sizeof hdr), 200); /* stream 0 < 8: served */
  expect_string(k.buf, "body0");
  g_idle_obj = 0;
  h3_session_release(s, "example.com", 443);
  expect(g_idle_obj == 0); /* but the session takes no more */
  expect_eq(g_closes, 1);
}

static void on_push(void *ctx, const char *conn, const char *scheme, const char *authority, const char *path,
                    const char *hdr, const unsigned char *body, int blen)
{
  (void)ctx, (void)scheme, (void)hdr;
  g_pushes++;
  snprintf(g_push_conn, sizeof g_push_conn, "%s", conn);
  snprintf(g_push_auth, sizeof g_push_auth, "%s", authority);
  snprintf(g_push_path, sizeof g_push_path, "%s", path);
  snprintf(g_push_body, sizeof g_push_body, "%.*s", blen, (const char *)body);
}

/* PUSH_PROMISE payload for push id `id`: GET https://example.com<path>. */
static int promise(uint8_t *out, uint8_t id, const char *path)
{
  uint8_t pay[128];
  int n = 0;
  pay[n++] = id;
  pay[n++] = 0, pay[n++] = 0; /* QPACK prefix */
  pay[n++] = 0xc0 | 17;       /* :method GET */
  pay[n++] = 0xc0 | 23;       /* :scheme https */
  pay[n++] = 0x50;            /* :authority, literal value */
  pay[n++] = 11;
  memcpy(pay + n, "example.com", 11);
  n += 11;
  pay[n++] = 0x51; /* :path */
  pay[n++] = (uint8_t)strlen(path);
  memcpy(pay + n, path, strlen(path));
  n += (int)strlen(path);
  return put_frame(out, 0x05, pay, n);
}

/* A push stream: type 1, push id, a 200 response with `body`. */
static int push_stream(uint8_t *out, uint8_t id, const char *body)
{
  uint8_t hb[16], n = 0;
  hb[n++] = 0, hb[n++] = 0, hb[n++] = 0xc0 | 25;
  int o = 0;
  out[o++] = 0x01;
  out[o++] = id;
  o += put_frame(out + o, 0x01, hb, n);
  o += put_frame(out + o, 0x00, (const uint8_t *)body, (int)strlen(body));
  return o;
}

static void promised_pushes_reach_the_handler(void)
{
  reset();
  h3_set_push_handler(on_push, 0);
  g_mode = MODE_PUSH;
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  struct fstream *ctrl = fs_get(2);
  expect_eq(ctrl->req_len, 6); /* MAX_PUSH_ID follows SETTINGS */
  expect_eq(ctrl->req[3], 0x0d);
  expect_eq(ctrl->req[5], 7);

  uint8_t buf[256];
  int n = push_stream(buf, 0, "pushed-css");
  uni_write(7, buf, n, 1);
  n = push_stream(buf, 1, "never promised"); /* push 1 has no promise */
  uni_write(11, buf, n, 1);
  struct sink k;
  char hdr[256];
  expect_eq(get(s, "/", &k, hdr, sizeof hdr), 200);
  expect_string(k.buf, "body0");
  expect_eq(g_pushes, 1);
  expect_string(g_push_conn, "example.com");
  expect_string(g_push_auth, "example.com");
  expect_string(g_push_path, "/app.css");
  expect_string(g_push_body, "pushed-css");
  h3_session_close(s);
}

static void pushes_are_not_invited_without_a_handler(void)
{
  reset();
  g_mode = MODE_PUSH;
  h3_session *s;
  expect_eq(h3_session_open(&s, "example.com", 443), 0);
  expect_eq(fs_get(2)->req_len, 3);
  uint8_t buf[256];
  int n = push_stream(buf, 0, "unasked");
  uni_write(7, buf, n, 1);
  struct sink k;
  char hdr[256];
  expect_eq(get(s, "/", &k, hdr, sizeof hdr), 200);
  expect_eq(g_pushes, 0);
  h3_session_close(s);
}

int main(void)
{
  test("reuse_keeps_the_connection", reuse_keeps_the_connection);
  test("early_hints_arrive_separately", early_hints_arrive_separately);
  test("headers_after_a_long_value_survive", headers_after_a_long_value_survive);
  test("post_sends_a_data_frame", post_sends_a_data_frame);
  test("batches_use_one_stream_each", batches_use_one_stream_each);
  test("a_dead_connection_is_not_reused", a_dead_connection_is_not_reused);
  test("stream_limit_retires_the_session", stream_limit_retires_the_session);
  test("a_body_over_the_server_limit_fails_to_send", a_body_over_the_server_limit_fails_to_send);
  test("parked_session_reports_a_hung_up_peer", parked_session_reports_a_hung_up_peer);
  test("goaway_stops_the_session_and_refuses_unprocessed_requests",
       goaway_stops_the_session_and_refuses_unprocessed_requests);
  test("a_goaway_with_room_lets_earlier_requests_finish", a_goaway_with_room_lets_earlier_requests_finish);
  test("promised_pushes_reach_the_handler", promised_pushes_reach_the_handler);
  test("pushes_are_not_invited_without_a_handler", pushes_are_not_invited_without_a_handler);
  return 0;
}
