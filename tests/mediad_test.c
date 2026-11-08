/* Host test for the mediad wire codec and session policy (user/lib/mediactl/session.c). */
#define OWOS_SUITE_TEST
#include "session.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct media_buf b;
static struct media_req rq;
static struct media_info in, pa;
static struct media_hub h;
static int a, c;
static unsigned arg;

static struct media_info info_of(const char *app, const char *title, int state)
{
  struct media_info i;
  memset(&i, 0, sizeof i);
  snprintf(i.app, sizeof i.app, "%s", app);
  snprintf(i.title, sizeof i.title, "%s", title);
  i.state = state;
  return i;
}

static bool decode(const struct media_buf *b, struct media_req *rq)
{
  int type;
  size_t len;
  return media_hdr_parse(b->p, &type, &len) && len == b->len - MEDIA_HDR &&
         media_dec_request(type, b->p + MEDIA_HDR, len, rq);
}

static bool alive_all(unsigned pid, void *ctx)
{
  (void)pid, (void)ctx;
  return true;
}

static bool alive_odd(unsigned pid, void *ctx)
{
  int *calls = ctx;
  (*calls)++;
  return pid & 1;
}

static int next(struct media_hub *h, unsigned id, unsigned *arg) { return media_hub_poll(h, id, arg); }

static void test_request_frames_round_trip(void)
{
  expect(media_enc_open(&b, 42, "player") && decode(&b, &rq));
  expect(rq.op == MEDIA_OP_OPEN && rq.pid == 42);
  expect_string(rq.info.app, "player");

  in = info_of("player", "Song \xc3\xa9", MEDIA_PAUSED);
  strcpy(in.artist, "Artist");
  strcpy(in.album, "Album");
  strcpy(in.artwork, "/home/root/a.png");
  in.pos_ms = 1234;
  in.dur_ms = 99999;
  in.caps = 0xdeadbeefu;
  expect(media_enc_update(&b, 7, &in) && decode(&b, &rq));
  expect(rq.op == MEDIA_OP_UPDATE && rq.id == 7);
  expect(memcmp(&rq.info, &in, sizeof in) == 0);

  expect(media_enc_id(&b, MEDIA_OP_POLL, 9) && decode(&b, &rq) && rq.op == MEDIA_OP_POLL && rq.id == 9);
  expect(media_enc_id(&b, MEDIA_OP_CLOSE, 3) && decode(&b, &rq) && rq.op == MEDIA_OP_CLOSE && rq.id == 3);
  expect(media_enc_status(&b) && decode(&b, &rq) && rq.op == MEDIA_OP_STATUS);
  expect(media_enc_send(&b, MEDIA_CMD_SEEK_TO, 0xfffffff0u) && decode(&b, &rq));
  expect(rq.op == MEDIA_OP_SEND && rq.cmd == MEDIA_CMD_SEEK_TO && rq.arg == 0xfffffff0u);
}

static void test_long_fields_are_truncated(void)
{
  struct media_info big;
  memset(&big, 'x', sizeof big);
  big.state = MEDIA_PLAYING;
  expect(media_enc_update(&b, 1, &big) && decode(&b, &rq));
  expect(strlen(rq.info.title) == MEDIA_STR_TEXT - 1 && strlen(rq.info.artwork) == MEDIA_STR_ART - 1);
}

static void test_replies_round_trip(void)
{
  unsigned id, cmd;
  bool have;
  struct media_info out;
  expect(media_enc_ack_id(&b, 5) && b.p[0] == MEDIA_ACK && media_dec_id(b.p + MEDIA_HDR, b.len - MEDIA_HDR, &id) &&
         id == 5);
  expect(media_enc_ack_cmd(&b, MEDIA_CMD_NEXT, 8) && media_dec_cmd(b.p + MEDIA_HDR, b.len - MEDIA_HDR, &cmd, &arg));
  expect(cmd == MEDIA_CMD_NEXT && arg == 8);
  expect(media_enc_ack_status(&b, &in) && media_dec_status(b.p + MEDIA_HDR, b.len - MEDIA_HDR, &have, &out));
  expect(have && memcmp(&out, &in, sizeof in) == 0);
  expect(media_enc_ack_status(&b, NULL) && media_dec_status(b.p + MEDIA_HDR, b.len - MEDIA_HDR, &have, &out) && !have);
  expect(media_enc_ack(&b, false) && b.p[0] == MEDIA_NAK && b.len == MEDIA_HDR);
}

static void test_truncated_oversized_and_malformed(void)
{
  media_enc_update(&b, 7, &in);
  size_t plen = b.len - MEDIA_HDR;
  for (size_t n = 0; n < plen; n++)
    if (media_dec_request(MEDIA_OP_UPDATE, b.p + MEDIA_HDR, n, &rq)) {
      expect(0 && "prefix accepted");
      break;
    }
  unsigned char *pl = malloc(plen + 1);
  memcpy(pl, b.p + MEDIA_HDR, plen);
  pl[plen] = 0;
  expect(!media_dec_request(MEDIA_OP_UPDATE, pl, plen + 1, &rq));
  expect(media_dec_request(MEDIA_OP_UPDATE, pl, plen, &rq));
  pl[4] = MEDIA_STR_APP;
  pl[5] = 0;
  expect(!media_dec_request(MEDIA_OP_UPDATE, pl, plen, &rq));
  memcpy(pl, b.p + MEDIA_HDR, plen);
  pl[6] = '\0';
  expect(!media_dec_request(MEDIA_OP_UPDATE, pl, plen, &rq));
  memcpy(pl, b.p + MEDIA_HDR, plen);
  pl[plen - 16] = 9;
  expect(!media_dec_request(MEDIA_OP_UPDATE, pl, plen, &rq));
  free(pl);
  expect(!media_dec_request('?', NULL, 0, &rq));
  expect(!media_dec_request(MEDIA_OP_STATUS, (const unsigned char *)"x", 1, &rq));

  unsigned char hdr[MEDIA_HDR] = {'O', 0, 0x11, 0, 0};
  int type;
  size_t len;
  expect(!media_hdr_parse(hdr, &type, &len));
  hdr[2] = 0x0f;
  expect(media_hdr_parse(hdr, &type, &len) && type == 'O' && len == 0x0f00);
  hdr[1] = 0xff, hdr[2] = 0xff, hdr[3] = 0xff, hdr[4] = 0xff;
  expect(!media_hdr_parse(hdr, &type, &len));
}

static void test_active_session_most_recently(void)
{
  expect(media_hub_active(&h) == NULL);
  a = media_hub_open(&h, 10, "a");
  c = media_hub_open(&h, 11, "b");
  expect(a > 0 && c > 0 && a != c);
  expect(media_hub_active(&h)->id == (unsigned)c);
  pa = info_of("a", "A", MEDIA_PLAYING);
  struct media_info pc = info_of("b", "B", MEDIA_PLAYING);
  media_hub_update(&h, (unsigned)a, &pa);
  expect(media_hub_active(&h)->id == (unsigned)a);
  media_hub_update(&h, (unsigned)c, &pc);
  expect(media_hub_active(&h)->id == (unsigned)c);
  media_hub_update(&h, (unsigned)a, &pa);
  expect(media_hub_active(&h)->id == (unsigned)c);
  struct media_info paused = info_of("b", "B", MEDIA_PAUSED);
  media_hub_update(&h, (unsigned)c, &paused);
  expect(media_hub_active(&h)->id == (unsigned)a);
  media_hub_update(&h, (unsigned)a, &(struct media_info){.state = MEDIA_PAUSED});
  expect(media_hub_active(&h)->id == (unsigned)a);
  media_hub_update(&h, (unsigned)c, &paused);
  expect(media_hub_active(&h)->id == (unsigned)c);
  expect(media_hub_update(&h, 999, &pa) == -1);
  struct media_info bad = info_of("a", "A", 7);
  expect(media_hub_update(&h, (unsigned)a, &bad) == -1);
}

static void test_commands_route_to_the(void)
{
  media_hub_update(&h, (unsigned)a, &pa);
  expect(media_hub_send(&h, MEDIA_CMD_NEXT, 0) == 0);
  expect(next(&h, (unsigned)c, &arg) == MEDIA_CMD_NONE);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_NEXT);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_NONE);
  expect(next(&h, 999, &arg) == -1);
  expect(media_hub_send(&h, MEDIA_CMD_NONE, 0) == -1);
  expect(media_hub_send(&h, MEDIA_CMD_SEEK_TO + 1, 0) == -1);
}

static void test_queue_keeps_order_and(void)
{
  media_hub_send(&h, MEDIA_CMD_NEXT, 0);
  media_hub_send(&h, MEDIA_CMD_NEXT, 0);
  media_hub_send(&h, MEDIA_CMD_PREV, 0);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_NEXT);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_NEXT);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_PREV);

  media_hub_send(&h, MEDIA_CMD_PAUSE, 0);
  media_hub_send(&h, MEDIA_CMD_PAUSE, 0);
  media_hub_send(&h, MEDIA_CMD_PLAY, 0);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_PAUSE);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_PLAY);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_NONE);

  media_hub_send(&h, MEDIA_CMD_TOGGLE, 0);
  media_hub_send(&h, MEDIA_CMD_TOGGLE, 0);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_NONE);
  media_hub_send(&h, MEDIA_CMD_TOGGLE, 0);
  media_hub_send(&h, MEDIA_CMD_TOGGLE, 0);
  media_hub_send(&h, MEDIA_CMD_TOGGLE, 0);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_TOGGLE);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_NONE);

  media_hub_send(&h, MEDIA_CMD_SEEK_FWD, 5000);
  media_hub_send(&h, MEDIA_CMD_SEEK_FWD, 3000);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_SEEK_FWD && arg == 8000);
  media_hub_send(&h, MEDIA_CMD_SEEK_FWD, ~0u - 1);
  media_hub_send(&h, MEDIA_CMD_SEEK_FWD, 10);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_SEEK_FWD && arg == ~0u);
  media_hub_send(&h, MEDIA_CMD_SEEK_FWD, 0);
  media_hub_send(&h, MEDIA_CMD_SEEK_FWD, 0);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_SEEK_FWD);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_SEEK_FWD);
  media_hub_send(&h, MEDIA_CMD_SEEK_FWD, 1000);
  media_hub_send(&h, MEDIA_CMD_SEEK_BACK, 1000);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_SEEK_FWD);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_SEEK_BACK);

  media_hub_send(&h, MEDIA_CMD_NEXT, 0);
  media_hub_send(&h, MEDIA_CMD_SEEK_FWD, 1000);
  media_hub_send(&h, MEDIA_CMD_SEEK_BACK, 500);
  media_hub_send(&h, MEDIA_CMD_SEEK_TO, 1000);
  media_hub_send(&h, MEDIA_CMD_SEEK_TO, 2000);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_NEXT);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_SEEK_TO && arg == 2000);
  expect(next(&h, (unsigned)a, &arg) == MEDIA_CMD_NONE);
}

static void test_queue_grows_without_a(void)
{
  for (unsigned i = 0; i < 1000; i++)
    media_hub_send(&h, MEDIA_CMD_NEXT, i);
  unsigned n = 0;
  while (next(&h, (unsigned)a, &arg) == MEDIA_CMD_NEXT)
    expect(arg == n++ || n > 1000);
  expect(n == 1000);
}

static void test_closing_a_session_re(void)
{
  media_hub_send(&h, MEDIA_CMD_STOP, 0);
  expect(media_hub_close(&h, (unsigned)a) == 0 && media_hub_close(&h, (unsigned)a) == -1);
  expect(media_hub_active(&h)->id == (unsigned)c);
  expect(next(&h, (unsigned)a, &arg) == -1);
  expect(next(&h, (unsigned)c, &arg) == MEDIA_CMD_NONE);
}

static void test_dead_sessions_are_reaped(void)
{
  media_hub_free(&h);
  int s1 = media_hub_open(&h, 1, "one"), s2 = media_hub_open(&h, 2, "two"), s3 = media_hub_open(&h, 3, "three");
  media_hub_update(&h, (unsigned)s2, &pa);
  expect(media_hub_send(&h, MEDIA_CMD_NEXT, 0) == 0);
  expect(media_hub_reap(&h, alive_all, NULL) == 0 && h.n == 3);
  int calls = 0;
  expect(media_hub_reap(&h, alive_odd, &calls) == 1 && calls == 3 && h.n == 2);
  expect(next(&h, (unsigned)s2, &arg) == -1);
  expect(next(&h, (unsigned)s1, &arg) == MEDIA_CMD_NONE && next(&h, (unsigned)s3, &arg) == MEDIA_CMD_NONE);
  expect(media_hub_active(&h)->id == (unsigned)s3);
  expect(media_hub_send(&h, MEDIA_CMD_PLAY, 0) == 0 && next(&h, (unsigned)s3, &arg) == MEDIA_CMD_PLAY);
  media_hub_free(&h);
  expect(media_hub_send(&h, MEDIA_CMD_PLAY, 0) == -1);

  media_buf_free(&b);
}

int main(void)
{
  test("request frames round-trip", test_request_frames_round_trip);
  test("long fields are truncated by the encoder to fit", test_long_fields_are_truncated);
  test("replies round-trip", test_replies_round_trip);
  test("truncated, oversized and malformed payloads are rejected", test_truncated_oversized_and_malformed);
  test("active session: most recently playing, else most recently updated", test_active_session_most_recently);
  test("commands route to the active session only", test_commands_route_to_the);
  test("queue keeps order and coalesces", test_queue_keeps_order_and);
  test("queue grows without a fixed cap", test_queue_grows_without_a);
  test("closing a session re-selects and drops its queue", test_closing_a_session_re);
  test("dead sessions are reaped through the liveness callback", test_dead_sessions_are_reaped);

  return 0;
}
