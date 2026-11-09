/* Host test for h2 sessions (user/lib/curl/h2.c) against a scripted peer. h2.c is built with its netconn calls
 * renamed to the fakes below, so no sockets are involved. */
#define OWOS_SUITE_TEST
#include "h2.h"
#include "netconn.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct netconn {
  int id;
};

static struct netconn g_conn;

/* ---- the scripted peer */
static unsigned char g_out[65536]; /* bytes the client will read */
static int g_out_len, g_out_pos;
static unsigned char g_in[65536]; /* bytes the client wrote */
static int g_in_len, g_in_parsed;
static int g_prefaces, g_closed, g_sent_tag, g_dead, g_big;
static unsigned g_sids[32];
static int g_nsids;

static void out_frame(int type, int flags, unsigned sid, const void *pay, int len)
{
  unsigned char *p = g_out + g_out_len;
  p[0] = (unsigned char)(len >> 16);
  p[1] = (unsigned char)(len >> 8);
  p[2] = (unsigned char)len;
  p[3] = (unsigned char)type;
  p[4] = (unsigned char)flags;
  p[5] = (unsigned char)(sid >> 24);
  p[6] = (unsigned char)(sid >> 16);
  p[7] = (unsigned char)(sid >> 8);
  p[8] = (unsigned char)sid;
  memcpy(p + 9, pay, (size_t)len);
  g_out_len += 9 + len;
}

/* Answer one finished request: 200 + x-tag (inserted into the HPACK table the first time, then referenced by index)
 * and a body naming the stream. */
static void respond(unsigned sid)
{
  unsigned char hb[64];
  int n = 0;
  hb[n++] = 0x88; /* :status 200 */
  if (!g_sent_tag) {
    hb[n++] = 0x40; /* literal, incremental indexing, new name */
    hb[n++] = 5;
    memcpy(hb + n, "x-tag", 5);
    n += 5;
    hb[n++] = 3;
    memcpy(hb + n, "aaa", 3);
    n += 3;
    g_sent_tag = 1;
  } else {
    hb[n++] = 0xbe; /* dynamic table entry 62 */
  }
  out_frame(1, 0x04, sid, hb, n);
  if (g_big) { /* two DATA frames, enough to flush the engine's body buffer before the stream ends */
    static char big[1500];
    memset(big, 'x', sizeof big);
    out_frame(0, 0, sid, big, sizeof big);
    out_frame(0, 0x01, sid, big, sizeof big);
    return;
  }
  char body[16];
  int bl = snprintf(body, sizeof body, "body%u", sid);
  out_frame(0, 0x01, sid, body, bl);
}

static void srv_reset(void)
{
  g_out_len = g_out_pos = g_in_len = g_in_parsed = 0;
  g_prefaces = g_closed = g_sent_tag = g_nsids = g_dead = g_big = 0;
}

static void srv_parse(void)
{
  if (g_in_parsed == 0 && g_in_len >= 24 && memcmp(g_in, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) == 0) {
    g_prefaces++;
    g_in_parsed = 24;
    unsigned char none[1];
    out_frame(4, 0, 0, none, 0); /* server SETTINGS */
  }
  while (g_in_len - g_in_parsed >= 9) {
    unsigned char *h = g_in + g_in_parsed;
    int len = (h[0] << 16) | (h[1] << 8) | h[2];
    if (g_in_len - g_in_parsed < 9 + len)
      return;
    int type = h[3], flags = h[4];
    unsigned sid = ((unsigned)h[5] << 24) | (h[6] << 16) | (h[7] << 8) | h[8];
    g_in_parsed += 9 + len;
    if (type == 4 && !(flags & 1))
      out_frame(4, 1, 0, h, 0); /* ACK the client SETTINGS */
    if (type == 1) {
      g_sids[g_nsids++] = sid;
      if (flags & 1)
        respond(sid);
    }
  }
}

int h2t_write(netconn *c, const void *buf, int len)
{
  (void)c;
  memcpy(g_in + g_in_len, buf, (size_t)len);
  g_in_len += len;
  if (!g_dead)
    srv_parse();
  return len;
}

int h2t_read(netconn *c, void *buf, int max)
{
  (void)c;
  int avail = g_out_len - g_out_pos;
  if (avail <= 0)
    return 0; /* peer closed */
  int n = avail < max ? avail : max;
  if (n > 5)
    n = 5; /* dribble, so frames straddle reads */
  memcpy(buf, g_out + g_out_pos, (size_t)n);
  g_out_pos += n;
  return n;
}

void h2t_close(netconn *c)
{
  (void)c;
  g_closed++;
}

/* ---- the idle pool, one slot */
static void *g_idle_obj;
static void (*g_idle_destroy)(void *);

static int (*g_idle_closed)(void *);
static int g_peer_closed, g_peer_allow_data;

int h2t_peer_closed(netconn *c, int allow_data)
{
  (void)c;
  g_peer_allow_data = allow_data;
  return g_peer_closed;
}

void h2t_idle_put(const char *host, int port, int tls, int kind, void *obj, void (*destroy)(void *),
                  int (*closed)(void *))
{
  (void)host, (void)port, (void)tls, (void)kind;
  g_idle_obj = obj;
  g_idle_destroy = destroy;
  g_idle_closed = closed;
}

void *h2t_idle_take(const char *host, int port, int tls, int kind)
{
  (void)host, (void)port, (void)tls, (void)kind;
  void *o = g_idle_obj;
  g_idle_obj = 0;
  return o;
}

/* ---- tests */
struct sink {
  char buf[4096];
  int len, abort_after;
};

static int sink_cb(void *ctx, const unsigned char *p, int n)
{
  struct sink *s = ctx;
  memcpy(s->buf + s->len, p, (size_t)n);
  s->len += n;
  s->buf[s->len] = '\0';
  return s->abort_after > 0 && s->len >= s->abort_after;
}

static int get(h2_session *s, const char *path, struct sink *k, char *hdr, int hdrcap)
{
  memset(k, 0, sizeof *k);
  int hl = 0;
  return h2_session_run(s, 1, "example.com", "GET", path, 0, 0, 0, hdr, hdrcap, &hl, sink_cb, k);
}

static void reuse_keeps_connection_state(void)
{
  srv_reset();
  h2_session *s = h2_session_wrap(&g_conn);
  struct sink k;
  char hdr[256];

  expect_eq(get(s, "/a", &k, hdr, sizeof hdr), 200);
  expect_string(k.buf, "body1");
  expect(strstr(hdr, "x-tag: aaa") != 0);
  h2_session_release(s, "example.com", 443);
  expect(g_idle_obj == s);
  expect_eq(g_closed, 0);

  h2_session *again = h2_session_acquire("example.com", 443);
  expect(again == s);
  expect(h2_session_used(again));
  expect_eq(get(again, "/b", &k, hdr, sizeof hdr), 200);
  expect_string(k.buf, "body3");
  expect(strstr(hdr, "x-tag: aaa") != 0); /* resolved through the HPACK table built by the first response */
  h2_session_release(again, "example.com", 443);

  expect_eq(g_prefaces, 1);
  expect_eq(g_nsids, 2);
  expect_eq(g_sids[0], 1);
  expect_eq(g_sids[1], 3);
  h2_session_close(h2_session_acquire("example.com", 443));
}

static void parked_session_reports_a_hung_up_peer(void)
{
  srv_reset();
  g_idle_obj = 0;
  h2_session *s = h2_session_wrap(&g_conn);
  struct sink k;
  char hdr[256];
  expect_eq(get(s, "/a", &k, hdr, sizeof hdr), 200);
  h2_session_release(s, "example.com", 443);
  expect(g_idle_closed != 0);
  g_peer_closed = 0;
  expect_eq(g_idle_closed(s), 0);
  expect_eq(g_peer_allow_data, 1); /* the peer may legitimately send h2 frames while idle */
  g_peer_closed = 1;
  expect_eq(g_idle_closed(s), 1);
  g_peer_closed = 0;
  h2_session_close(h2_session_acquire("example.com", 443));
}

static void aborted_response_is_not_reused(void)
{
  srv_reset();
  h2_session *s = h2_session_wrap(&g_conn);
  struct sink k;
  char hdr[256];
  g_big = 1;
  memset(&k, 0, sizeof k);
  k.abort_after = 1;
  int hl = 0;
  expect_eq(h2_session_run(s, 1, "example.com", "GET", "/a", 0, 0, 0, hdr, sizeof hdr, &hl, sink_cb, &k), HTTP_EABORT);
  expect_eq(g_closed, 0);
  h2_session_release(s, "example.com", 443);
  expect(g_idle_obj == 0);
  expect_eq(g_closed, 1);
}

static void dead_connection_fails_and_is_dropped(void)
{
  srv_reset();
  g_idle_obj = 0;
  h2_session *s = h2_session_wrap(&g_conn);
  struct sink k;
  char hdr[256];
  expect_eq(get(s, "/a", &k, hdr, sizeof hdr), 200);
  h2_session_release(s, "example.com", 443);
  h2_session *again = h2_session_acquire("example.com", 443);
  g_dead = 1; /* the peer went away while idle */
  int before = g_closed;
  expect(get(again, "/b", &k, hdr, sizeof hdr) < 0);
  h2_session_release(again, "example.com", 443);
  expect(g_idle_obj == 0);
  expect_eq(g_closed, before + 1);
}

static void batches_continue_stream_ids(void)
{
  srv_reset();
  g_idle_obj = 0;
  h2_session *s = h2_session_wrap(&g_conn);
  struct h2_fetch r[2] = {{.path = "/x"}, {.path = "/y"}};
  expect_eq(h2_session_fetch_many(s, 1, "example.com", 0, r, 2, 0, 0), 0);
  expect(!r[0].failed && !r[1].failed);
  expect_eq(r[1].blen, 5);
  free(r[0].body);
  free(r[1].body);
  struct h2_fetch q[1] = {{.path = "/z"}};
  expect_eq(h2_session_fetch_many(s, 1, "example.com", 0, q, 1, 0, 0), 0);
  expect(!q[0].failed);
  expect(memcmp(q[0].body, "body5", 5) == 0);
  free(q[0].body);
  expect_eq(g_prefaces, 1);
  expect_eq(g_sids[2], 5);
  h2_session_close(s);
}

int main(void)
{
  test("reuse_keeps_connection_state", reuse_keeps_connection_state);
  test("parked_session_reports_a_hung_up_peer", parked_session_reports_a_hung_up_peer);
  test("aborted_response_is_not_reused", aborted_response_is_not_reused);
  test("dead_connection_fails_and_is_dropped", dead_connection_fails_and_is_dropped);
  test("batches_continue_stream_ids", batches_continue_stream_ids);
  return 0;
}
