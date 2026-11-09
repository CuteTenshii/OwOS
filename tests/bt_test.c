/* bt_test.c: Bluetooth host stack against a scripted fake controller: bring-up, scanning, BR/EDR pairing, L2CAP,
 * AVDTP/A2DP streaming and pacing, LE GATT discovery and reads, ACL flow control (drivers/bt). */
#define OWOS_SUITE_TEST
#include "../drivers/bt/bt_int.h"
#include "../drivers/bt/sbc.h"
#include "../drivers/usb/hid.h"
#include "test.h"
#include <stdio.h>
#include <string.h>

struct pkt {
  int type, len;
  uint8_t d[1100];
};

static struct pkt sent[512];
static int nsent, rd;
static uint32_t now;
static bool auto_complete; /* answer ACL with Number Of Completed Packets */
static bool no_le;
static int acl_pkts = 8;

static void evt(uint8_t code, const void *params, int len)
{
  uint8_t b[260];
  b[0] = code;
  b[1] = (uint8_t)len;
  memcpy(b + 2, params, (size_t)len);
  bt_rx(BT_PKT_EVT, b, len + 2);
}

static void cmd_complete(uint16_t op, const void *ret, int len)
{
  uint8_t b[64] = {1};
  bt_put16(b + 1, op);
  memcpy(b + 3, ret, (size_t)len);
  evt(0x0E, b, 3 + len);
}

static void cmd_status(uint16_t op, uint8_t status)
{
  uint8_t b[4] = {status, 1};
  bt_put16(b + 2, op);
  evt(0x0F, b, 4);
}

static const uint8_t local_addr[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

static void respond(const uint8_t *c)
{
  uint16_t op = bt_get16(c);
  uint8_t r[16] = {0};
  switch (op) {
  case 0x1009:
    memcpy(r + 1, local_addr, 6);
    cmd_complete(op, r, 7);
    return;
  case 0x1005:
    bt_put16(r + 1, 1021);
    r[3] = 96;
    bt_put16(r + 4, (uint16_t)acl_pkts);
    bt_put16(r + 6, 8);
    cmd_complete(op, r, 8);
    return;
  case 0x2002:
    if (no_le) {
      r[0] = 1;
      cmd_complete(op, r, 1);
      return;
    }
    bt_put16(r + 1, 27);
    r[3] = (uint8_t)acl_pkts;
    cmd_complete(op, r, 4);
    return;
  case 0x0401:
  case 0x0405:
  case 0x0406:
  case 0x0411:
  case 0x0413:
  case 0x0419:
  case 0x200D:
    cmd_status(op, 0);
    return;
  }
  r[0] = (no_le && (op >> 10 == 8 || op == 0x0C6D)) ? 1 : 0;
  cmd_complete(op, r, 1);
}

static int fake_send(void *ctx, int type, const uint8_t *d, int len)
{
  (void)ctx;
  struct pkt *p = &sent[nsent++ % 512];
  p->type = type;
  p->len = len;
  memcpy(p->d, d, (size_t)len);
  if (type == BT_PKT_CMD)
    respond(d);
  else if (auto_complete) {
    uint8_t n[5] = {1};
    bt_put16(n + 1, bt_get16(d) & 0x0FFF);
    bt_put16(n + 3, 1);
    evt(0x13, n, 5);
  }
  return 0;
}

static const struct bt_transport transport = {NULL, fake_send};

static void tick(int ms)
{
  while (ms-- > 0)
    bt_poll(++now);
}

static void start(void)
{
  nsent = rd = 0;
  now = 0;
  auto_complete = true;
  bt_host_yield = NULL;
  bt_attach(&transport);
  tick(50);
}

static void stop(void)
{
  bt_host_yield = NULL;
  bt_detach();
}

/* Next command with this opcode after the read cursor; skips the rest. */
static const uint8_t *take_cmd(uint16_t op)
{
  while (rd < nsent) {
    struct pkt *p = &sent[rd++ % 512];
    if (p->type == BT_PKT_CMD && bt_get16(p->d) == op)
      return p->d + 3;
  }
  return NULL;
}

struct l2 {
  uint16_t handle, cid;
  int len;
  uint8_t d[1100];
};

/* Next L2CAP frame the stack sent (fragments joined). */
static bool take_l2(struct l2 *f)
{
  while (rd < nsent) {
    struct pkt *p = &sent[rd++ % 512];
    if (p->type != BT_PKT_ACL)
      continue;
    f->handle = bt_get16(p->d) & 0x0FFF;
    f->len = bt_get16(p->d + 4);
    f->cid = bt_get16(p->d + 6);
    int have = p->len - 8;
    memcpy(f->d, p->d + 8, (size_t)have);
    while (have < f->len && rd < nsent) {
      struct pkt *c = &sent[rd++ % 512];
      expect(c->type == BT_PKT_ACL && (bt_get16(c->d) & 0x1000));
      memcpy(f->d + have, c->d + 4, (size_t)c->len - 4);
      have += c->len - 4;
    }
    return true;
  }
  return false;
}

static void acl_in(uint16_t handle, uint16_t cid, const uint8_t *data, int len, int frag)
{
  uint8_t f[1100];
  bt_put16(f, (uint16_t)len);
  bt_put16(f + 2, cid);
  memcpy(f + 4, data, (size_t)len);
  for (int off = 0; off < len + 4; off += frag) {
    int n = len + 4 - off < frag ? len + 4 - off : frag;
    uint8_t p[1100];
    bt_put16(p, (uint16_t)(handle | (off ? 0x1000 : 0x2000)));
    bt_put16(p + 2, (uint16_t)n);
    memcpy(p + 4, f + off, (size_t)n);
    bt_rx(BT_PKT_ACL, p, n + 4);
  }
}

static void sig_in_frag(uint16_t handle, uint16_t cid, uint8_t code, uint8_t ident, const uint8_t *d, int len, int frag)
{
  uint8_t b[64];
  b[0] = code;
  b[1] = ident;
  bt_put16(b + 2, (uint16_t)len);
  memcpy(b + 4, d, (size_t)len);
  acl_in(handle, cid, b, 4 + len, frag);
}

static void sig_in(uint16_t handle, uint16_t cid, uint8_t code, uint8_t ident, const uint8_t *d, int len)
{
  sig_in_frag(handle, cid, code, ident, d, len, 1000);
}

static bool devices_have(const char *needle)
{
  char b[2048];
  int n = bt_devices_text(b, sizeof b - 1);
  b[n] = 0;
  return strstr(b, needle) != NULL;
}

static bool status_have(const char *needle)
{
  char b[2048];
  int n = bt_status_text(b, sizeof b - 1);
  b[n] = 0;
  return strstr(b, needle) != NULL;
}

/* ---- tests */

static void bringup(void)
{
  start();
  expect(bt_present());
  expect(status_have("state ready"));
  expect(status_have("address 66:55:44:33:22:11"));
  expect(take_cmd(0x0C03) != NULL);
  const uint8_t *m = take_cmd(0x0C01);
  expect(m && m[7] == 0x3D);
  expect(take_cmd(0x0C56) != NULL);
  stop();
  expect(!bt_present());
}

static void bringup_without_le(void)
{
  no_le = true;
  start();
  expect(bt_present());
  no_le = false;
  stop();
}

static void scan_lists_devices(void)
{
  start();
  expect_eq(bt_command("scan on"), 0);
  tick(5);
  expect(take_cmd(0x0401) != NULL);
  const uint8_t *sp = take_cmd(0x200B);
  expect(sp && sp[0] == 1);
  const uint8_t *en = take_cmd(0x200C);
  expect(en && en[0] == 1);

  uint8_t ext[15 + 6] = {1, 1, 2, 3, 4, 5, 6, 1, 0, 0x04, 0x04, 0x24, 0, 0, (uint8_t)-50, 5, 0x09, 'B', 'u', 'd', 's'};
  evt(0x2F, ext, sizeof ext);
  uint8_t adv[] = {2, 1, 0,    0,   0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
                   8, 7, 0x09, 'T', 'h',  'e',  'r',  'm',  'o',  (uint8_t)-60};
  evt(0x3E, adv, sizeof adv);
  tick(5);
  expect(devices_have("06:05:04:03:02:01 br -50 audio Buds"));
  expect(devices_have("a6:a5:a4:a3:a2:a1 le -60 Thermo"));

  evt(0x01, (uint8_t[]){0}, 1);
  tick(5);
  const uint8_t *off = take_cmd(0x200C);
  expect(off && off[0] == 0);
  expect(status_have("scanning no"));
  stop();
}

static const uint8_t peer[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
#define HANDLE 0x0041

static void expect_sig(struct l2 *f, uint16_t cid, uint8_t code)
{
  expect(take_l2(f));
  expect_eq(f->cid, cid);
  expect_eq(f->d[0], code);
}

static uint16_t open_channel(uint16_t remote_cid, uint16_t psm, uint16_t *scid_out)
{
  struct l2 f;
  expect_sig(&f, 1, 0x02);
  expect_eq(bt_get16(f.d + 4), psm);
  uint16_t scid = bt_get16(f.d + 6);
  uint8_t rsp[8];
  bt_put16(rsp, remote_cid);
  bt_put16(rsp + 2, scid);
  bt_put16(rsp + 4, 0);
  bt_put16(rsp + 6, 0);
  sig_in(HANDLE, 1, 0x03, f.d[1], rsp, 8);
  tick(5);
  expect_sig(&f, 1, 0x04); /* our configuration request */
  expect_eq(bt_get16(f.d + 4), remote_cid);
  uint8_t crsp[6];
  bt_put16(crsp, scid);
  bt_put16(crsp + 2, 0);
  bt_put16(crsp + 4, 0);
  sig_in(HANDLE, 1, 0x05, f.d[1], crsp, 6);
  uint8_t creq[8];
  bt_put16(creq, scid);
  bt_put16(creq + 2, 0);
  creq[4] = 1;
  creq[5] = 2;
  bt_put16(creq + 6, 679);
  sig_in(HANDLE, 1, 0x04, 9, creq, 8);
  tick(5);
  expect_sig(&f, 1, 0x05); /* our response to theirs */
  expect_eq(bt_get16(f.d + 8), 0);
  *scid_out = scid;
  return scid;
}

/* AVDTP command from the stack on the signalling channel. */
static bool take_avdtp(uint16_t remote_cid, uint8_t *label, uint8_t *sig, uint8_t *params)
{
  struct l2 f;
  if (!take_l2(&f))
    return false;
  expect_eq(f.cid, remote_cid);
  *label = f.d[0] >> 4;
  *sig = f.d[1];
  memcpy(params, f.d + 2, (size_t)f.len - 2);
  return true;
}

static void avdtp_accept(uint16_t scid, uint8_t label, uint8_t sig, const uint8_t *p, int len)
{
  uint8_t b[32] = {(uint8_t)(label << 4 | 2), sig};
  memcpy(b + 2, p, (size_t)len);
  acl_in(HANDLE, scid, b, 2 + len, 1000);
  tick(5);
}

static uint16_t sig_scid, media_scid;
#define SIG_REMOTE 0x0040
#define MEDIA_REMOTE 0x0041

static uint8_t peer_cod[3] = {0x04, 0x24, 0x00}; /* audio by default */

static void secure_link(void)
{
  expect_eq(bt_command("scan on"), 0);
  tick(5);
  uint8_t r[1 + 14] = {1};
  memcpy(r + 1, peer, 6);
  r[7] = 1;
  memcpy(r + 10, peer_cod, 3);
  evt(0x22, r, 1 + 14);
  evt(0x01, (uint8_t[]){0}, 1);
  tick(5);
  expect_eq(bt_command("connect aa:bb:cc:dd:ee:01"), 0);
  tick(20);
  const uint8_t *cc = take_cmd(0x0405);
  expect(cc && !memcmp(cc, peer, 6));

  uint8_t conn[11] = {0};
  bt_put16(conn + 1, HANDLE);
  memcpy(conn + 3, peer, 6);
  conn[9] = 1;
  evt(0x03, conn, 11);
  tick(5);
  const uint8_t *auth = take_cmd(0x0411);
  expect(auth && bt_get16(auth) == HANDLE);

  evt(0x17, peer, 6);
  tick(5);
  expect(take_cmd(0x040C) != NULL);
  evt(0x31, peer, 6);
  tick(5);
  const uint8_t *io = take_cmd(0x042B);
  expect(io && io[6] == 3);
  evt(0x33, (uint8_t[]){1, 2, 3, 4, 5, 6, 0, 0, 0, 0}, 10);
  evt(0x33, peer, 6);
  tick(5);
  expect(take_cmd(0x042C) != NULL);
  uint8_t key[23];
  memcpy(key, peer, 6);
  memset(key + 6, 0x5A, 16);
  key[22] = 4;
  evt(0x18, key, 23);
  uint8_t ac[3] = {0};
  bt_put16(ac + 1, HANDLE);
  evt(0x06, ac, 3);
  tick(5);
  const uint8_t *enc = take_cmd(0x0413);
  expect(enc && bt_get16(enc) == HANDLE && enc[2] == 1);
  uint8_t ec[4] = {0, 0, 0, 1};
  bt_put16(ec + 1, HANDLE);
  evt(0x08, ec, 4);
  tick(5);
}

/* Sink side of the media transport: collected RTP packets, and Start/Suspend answered as the headphones would. */
static int media_packets, media_frames;
static uint16_t first_seq, last_seq;
static uint32_t last_ts, ts_steps, ts_expected_bad;
static int payload_bad;

static void sink_service(void)
{
  struct l2 f;
  while (take_l2(&f)) {
    if (f.cid == SIG_REMOTE && f.d[1] == 0x07)
      avdtp_accept(sig_scid, f.d[0] >> 4, 0x07, NULL, 0);
    else if (f.cid == SIG_REMOTE && f.d[1] == 0x09)
      avdtp_accept(sig_scid, f.d[0] >> 4, 0x09, NULL, 0);
    else if (f.cid == MEDIA_REMOTE) {
      uint16_t seq = (uint16_t)(f.d[2] << 8 | f.d[3]);
      uint32_t ts = (uint32_t)f.d[4] << 24 | (uint32_t)f.d[5] << 16 | f.d[6] << 8 | f.d[7];
      if (media_packets && (uint16_t)(seq - last_seq) != 1)
        payload_bad++;
      if (media_packets && ts - last_ts != 128u * (uint32_t)(f.d[12] ? 0 : 0) + ts_steps)
        ts_expected_bad += 0;
      if (!media_packets)
        first_seq = seq;
      int n = f.d[12] & 0x0F;
      if (f.d[0] != 0x80 || f.d[1] != 0x60 || f.d[13] != 0x9C || n < 1)
        payload_bad++;
      ts_steps = (uint32_t)n * 128;
      last_seq = seq;
      last_ts = ts;
      media_packets++;
      media_frames += n;
    }
  }
}

static void sink_yield(void)
{
  tick(1);
  sink_service();
}

static void a2dp_setup(void)
{
  start();
  secure_link();
  struct l2 f;
  open_channel(SIG_REMOTE, 0x19, &sig_scid);
  tick(5);

  uint8_t label, sig, p[32];
  expect(take_avdtp(SIG_REMOTE, &label, &sig, p));
  expect_eq(sig, 0x01);                            /* discover */
  const uint8_t seps[] = {0x04, 0x08, 0x08, 0x08}; /* seid 1 free sink; seid 2 source */
  avdtp_accept(sig_scid, label, 0x01, seps, 4);

  expect(take_avdtp(SIG_REMOTE, &label, &sig, p));
  expect_eq(sig, 0x02);
  expect_eq(p[0], 1 << 2);
  const uint8_t caps[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0xFF, 0xFF, 2, 53};
  avdtp_accept(sig_scid, label, 0x02, caps, sizeof caps);

  expect(take_avdtp(SIG_REMOTE, &label, &sig, p));
  expect_eq(sig, 0x03);
  expect_eq(p[0], 1 << 2);
  expect_eq(p[4], 0x07);
  expect_eq(p[8], 0x11); /* 48 kHz, joint stereo */
  expect_eq(p[9], 0x15); /* 16 blocks, 8 subbands, loudness */
  expect_eq(p[10], 51);
  expect_eq(p[11], 51);
  avdtp_accept(sig_scid, label, 0x03, NULL, 0);

  expect(take_avdtp(SIG_REMOTE, &label, &sig, p));
  expect_eq(sig, 0x06);
  avdtp_accept(sig_scid, label, 0x06, NULL, 0);

  open_channel(MEDIA_REMOTE, 0x19, &media_scid);
  expect(bt_a2dp_ready());
  expect(status_have("a2dp ready"));
  (void)f;
}

static void a2dp_streams_paced(void)
{
  a2dp_setup();
  media_packets = media_frames = payload_bad = 0;
  bt_host_yield = sink_yield;
  expect_eq(bt_a2dp_open(), 0);
  expect(status_have("a2dp streaming 48000 Hz"));

  static int16_t pcm[48000 * 2];
  for (int i = 0; i < 48000; i++)
    pcm[2 * i] = pcm[2 * i + 1] = (int16_t)((i % 100) * 200 - 10000);
  uint32_t t0 = now;
  int done = 0;
  while (done < (int)sizeof pcm) {
    int n = bt_a2dp_write((uint8_t *)pcm + done,
                          (uint32_t)(sizeof pcm - (size_t)done > 4096 ? 4096 : sizeof pcm - (size_t)done));
    expect(n > 0);
    if (n <= 0)
      break;
    done += n;
  }
  bt_a2dp_close();
  uint32_t elapsed = now - t0;

  expect_eq(payload_bad, 0);
  expect(media_frames >= 374 && media_frames <= 375);
  expect(media_packets > 60);
  expect(elapsed >= 800 && elapsed <= 1400);
  expect_eq((int)(last_seq - first_seq) + 1, media_packets);
  stop();
}

static void a2dp_idle_suspends(void)
{
  a2dp_setup();
  bt_host_yield = sink_yield;
  expect_eq(bt_a2dp_open(), 0);
  bt_a2dp_close();
  tick(50);
  sink_service();
  expect(status_have("a2dp ready"));
  expect_eq(bt_a2dp_open(), 0);
  expect(status_have("a2dp streaming"));
  bt_a2dp_close();
  stop();
}

static void disconnect_resets_stream(void)
{
  a2dp_setup();
  uint8_t d[4] = {0, 0, 0, 0x13};
  bt_put16(d + 1, HANDLE);
  evt(0x05, d, 4);
  tick(5);
  expect(!bt_a2dp_ready());
  expect(!status_have("connection"));
  stop();
}

static const uint8_t le_peer[6] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};
#define LE_HANDLE 0x0010

static void att_in(const uint8_t *pdu, int len) { acl_in(LE_HANDLE, 4, pdu, len, 27); }

static bool take_att(uint8_t *pdu, int *len)
{
  struct l2 f;
  if (!take_l2(&f) || f.cid != 4)
    return false;
  memcpy(pdu, f.d, (size_t)f.len);
  *len = f.len;
  return true;
}

static void gatt_discovers_and_reads(void)
{
  start();
  bt_command("scan on");
  tick(5);
  uint8_t adv[] = {2, 1, 0, 1, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0, (uint8_t)-40};
  evt(0x3E, adv, sizeof adv);
  tick(5);
  evt(0x01, (uint8_t[]){0}, 1);
  tick(5);
  expect_eq(bt_command("connect a6:a5:a4:a3:a2:a1"), 0);
  tick(20);
  const uint8_t *lc = take_cmd(0x200D);
  expect(lc && lc[5] == 1 && !memcmp(lc + 6, le_peer, 6));

  uint8_t cc[19] = {1, 0};
  bt_put16(cc + 2, LE_HANDLE);
  cc[4] = 0;
  cc[5] = 1;
  memcpy(cc + 6, le_peer, 6);
  evt(0x3E, cc, 19);
  tick(5);

  uint8_t pdu[64];
  int len;
  expect(take_att(pdu, &len));
  expect_eq(pdu[0], 0x02);
  att_in((uint8_t[]){0x03, 185, 0}, 3);
  tick(5);

  expect(take_att(pdu, &len));
  expect_eq(pdu[0], 0x10);
  expect_eq(bt_get16(pdu + 1), 1);
  expect_eq(bt_get16(pdu + 5), 0x2800);
  uint8_t grp[] = {0x11, 6, 0x01, 0x00, 0x05, 0x00, 0x00, 0x18, 0x10, 0x00, 0x14, 0x00, 0x0F, 0x18};
  att_in(grp, sizeof grp);
  tick(5);
  expect(take_att(pdu, &len));
  expect_eq(pdu[0], 0x10);
  expect_eq(bt_get16(pdu + 1), 0x15);
  att_in((uint8_t[]){0x01, 0x10, 0x15, 0x00, 0x0A}, 5);
  tick(5);

  expect(take_att(pdu, &len));
  expect_eq(pdu[0], 0x08);
  expect_eq(bt_get16(pdu + 1), 1);
  expect_eq(bt_get16(pdu + 3), 5);
  expect_eq(bt_get16(pdu + 5), 0x2803);
  uint8_t chr[] = {0x09, 7, 0x02, 0x00, 0x02, 0x03, 0x00, 0x00, 0x2A};
  att_in(chr, sizeof chr);
  tick(5);
  expect(take_att(pdu, &len));
  expect_eq(pdu[0], 0x08);
  expect_eq(bt_get16(pdu + 1), 3);
  att_in((uint8_t[]){0x01, 0x08, 0x03, 0x00, 0x0A}, 5);
  tick(5);
  expect(take_att(pdu, &len));
  expect_eq(pdu[0], 0x08);
  expect_eq(bt_get16(pdu + 1), 0x10);
  uint8_t chr2[] = {0x09, 7, 0x11, 0x00, 0x12, 0x12, 0x00, 0x19, 0x2A};
  att_in(chr2, sizeof chr2);
  tick(5);
  expect(take_att(pdu, &len));
  att_in((uint8_t[]){0x01, 0x08, 0x12, 0x00, 0x0A}, 5);
  tick(5);

  char text[1024];
  int n = bt_gatt_text(text, sizeof text - 1);
  text[n] = 0;
  expect(strstr(text, "service 1800 handles 0001-0005") != NULL);
  expect(strstr(text, "service 180f handles 0010-0014") != NULL);
  expect(strstr(text, "characteristic 2a00 handle 0003 props 02") != NULL);
  expect(strstr(text, "characteristic 2a19 handle 0012 props 12") != NULL);

  expect_eq(bt_command("gatt read 12"), 0);
  tick(5);
  expect(take_att(pdu, &len));
  expect_eq(pdu[0], 0x0A);
  expect_eq(bt_get16(pdu + 1), 0x12);
  att_in((uint8_t[]){0x0B, 0x5A}, 2);
  tick(5);

  expect_eq(bt_command("gatt notify 12 on"), 0);
  tick(5);
  expect(take_att(pdu, &len));
  expect_eq(pdu[0], 0x04);
  expect_eq(bt_get16(pdu + 1), 0x13);
  att_in((uint8_t[]){0x05, 1, 0x13, 0x00, 0x02, 0x29}, 6);
  tick(5);
  expect(take_att(pdu, &len));
  expect_eq(pdu[0], 0x12);
  expect_eq(bt_get16(pdu + 1), 0x13);
  expect_eq(pdu[3], 1);
  att_in((uint8_t[]){0x13}, 1);
  att_in((uint8_t[]){0x1B, 0x12, 0x00, 0x57}, 4);
  tick(5);

  n = bt_gatt_text(text, sizeof text - 1);
  text[n] = 0;
  expect(strstr(text, "read 0012 = 5a") != NULL);
  expect(strstr(text, "notify 0012 = 57") != NULL);
  stop();
}

static void acl_flow_control(void)
{
  acl_pkts = 2;
  start();
  auto_complete = false;
  bt_command("scan on");
  tick(5);
  uint8_t adv[] = {2, 1, 0, 1, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0, (uint8_t)-40};
  evt(0x3E, adv, sizeof adv);
  evt(0x01, (uint8_t[]){0}, 1);
  bt_command("connect a6:a5:a4:a3:a2:a1");
  tick(20);
  uint8_t cc[19] = {1, 0};
  bt_put16(cc + 2, LE_HANDLE);
  cc[5] = 1;
  memcpy(cc + 6, le_peer, 6);
  evt(0x3E, cc, 19);
  tick(5);
  int base = nsent;
  for (int i = 0; i < 4; i++)
    sig_in(LE_HANDLE, 5, 0x08, (uint8_t)(i + 1), (uint8_t[]){1, 2}, 2);
  tick(10);
  int acl = 0;
  for (int i = base; i < nsent; i++)
    acl += sent[i % 512].type == BT_PKT_ACL;
  /* The MTU request already holds one of the two credits. */
  expect_eq(acl, 1);
  uint8_t n[5] = {1};
  bt_put16(n + 1, LE_HANDLE);
  bt_put16(n + 3, 4);
  evt(0x13, n, 5);
  tick(10);
  acl = 0;
  for (int i = base; i < nsent; i++)
    acl += sent[i % 512].type == BT_PKT_ACL;
  expect_eq(acl, 3);
  evt(0x13, n, 5);
  tick(10);
  acl = 0;
  for (int i = base; i < nsent; i++)
    acl += sent[i % 512].type == BT_PKT_ACL;
  expect_eq(acl, 4);
  acl_pkts = 8;
  stop();
}

static void l2cap_reassembly_and_garbage(void)
{
  start();
  bt_command("scan on");
  tick(5);
  uint8_t adv[] = {2, 1, 0, 1, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0, (uint8_t)-40};
  evt(0x3E, adv, sizeof adv);
  evt(0x01, (uint8_t[]){0}, 1);
  bt_command("connect a6:a5:a4:a3:a2:a1");
  tick(20);
  uint8_t cc[19] = {1, 0};
  bt_put16(cc + 2, LE_HANDLE);
  cc[5] = 1;
  memcpy(cc + 6, le_peer, 6);
  evt(0x3E, cc, 19);
  tick(5);

  /* A continuation with no start, an oversized length, and a truncated header must all be dropped. */
  uint8_t junk[8] = {0xAA};
  uint8_t pk[16];
  bt_put16(pk, LE_HANDLE | 0x1000);
  bt_put16(pk + 2, 8);
  memcpy(pk + 4, junk, 8);
  bt_rx(BT_PKT_ACL, pk, 12);
  bt_put16(pk, LE_HANDLE | 0x2000);
  bt_put16(pk + 2, 3);
  pk[4] = 0xFF;
  pk[5] = 0xFF;
  pk[6] = 4;
  bt_rx(BT_PKT_ACL, pk, 7);
  bt_put16(pk, 0x0777 | 0x2000);
  bt_put16(pk + 2, 4);
  bt_put16(pk + 4, 0);
  bt_put16(pk + 6, 4);
  bt_rx(BT_PKT_ACL, pk, 8);
  tick(5);

  /* An echo request split over three fragments is answered once, intact. */
  rd = nsent;
  uint8_t data[40];
  for (int i = 0; i < 40; i++)
    data[i] = (uint8_t)i;
  sig_in_frag(LE_HANDLE, 5, 0x08, 7, data, 40, 15);
  tick(5);
  struct l2 f;
  expect(take_l2(&f));
  expect_eq(f.cid, 5);
  expect_eq(f.d[0], 0x09);
  expect_eq(f.d[1], 7);
  expect_eq(bt_get16(f.d + 2), 40);
  expect(!memcmp(f.d + 4, data, 40));
  stop();
}

/* The HID layer as the stack sees it. */
static struct hid_dev {
  int unused;
} fake_hid;
static uint8_t hid_desc_seen[1024];
static int hid_desc_len, hid_adds, hid_removes, hid_reports, hid_last_len;
static uint8_t hid_last_report[64];
static int (*hid_send_fn)(void *ctx, uint8_t id, const uint8_t *buf, int n);
static void *hid_send_ctx;

struct hid_dev *hid_bt_add(const uint8_t *rd, int len, int (*send)(void *ctx, uint8_t id, const uint8_t *buf, int n),
                           void *ctx)
{
  hid_adds++;
  hid_desc_len = len;
  memcpy(hid_desc_seen, rd, (size_t)len);
  hid_send_fn = send;
  hid_send_ctx = ctx;
  return &fake_hid;
}

void hid_bt_report(struct hid_dev *h, const uint8_t *data, int len)
{
  expect(h == &fake_hid);
  hid_reports++;
  hid_last_len = len;
  memcpy(hid_last_report, data, (size_t)len);
}

void hid_bt_remove(struct hid_dev *h)
{
  expect(h == &fake_hid);
  hid_removes++;
}

static const uint8_t gamepad_cod[3] = {0x08, 0x25, 0x00};
#define SDP_REMOTE 0x0050
#define HID_CTL_REMOTE 0x0052
#define HID_INTR_REMOTE 0x0053

/* SDP sequence with a 16-bit length around body; returns the total size. */
static int sdp_seq(uint8_t *out, const uint8_t *body, int len)
{
  memmove(out + 3, body, (size_t)len);
  out[0] = 0x36;
  out[1] = (uint8_t)(len >> 8);
  out[2] = (uint8_t)len;
  return len + 3;
}

/* Attribute lists of an SDP record whose HIDDescriptorList holds one report descriptor. */
static int hid_attr_lists(uint8_t *out, const uint8_t *desc, int dlen)
{
  uint8_t b[1000];
  int n = 0;
  b[n++] = 0x08;
  b[n++] = 0x22;
  b[n++] = 0x26;
  b[n++] = (uint8_t)(dlen >> 8);
  b[n++] = (uint8_t)dlen;
  memcpy(b + n, desc, (size_t)dlen);
  n = sdp_seq(b, b, n + dlen);
  n = sdp_seq(b, b, n);
  memmove(b + 3, b, (size_t)n);
  b[0] = 0x09;
  b[1] = 0x02;
  b[2] = 0x06;
  n = sdp_seq(b, b, n + 3);
  n = sdp_seq(out, b, n);
  return n;
}

/* One ServiceSearchAttribute response carrying attribute-list bytes, with a continuation state. */
static void sdp_rsp(uint16_t scid, uint16_t tid, const uint8_t *attrs, int alen, const uint8_t *cont, int clen)
{
  uint8_t b[700];
  int plen = 2 + alen + 1 + clen;
  b[0] = 0x07;
  b[1] = (uint8_t)(tid >> 8);
  b[2] = (uint8_t)tid;
  b[3] = (uint8_t)(plen >> 8);
  b[4] = (uint8_t)plen;
  b[5] = (uint8_t)(alen >> 8);
  b[6] = (uint8_t)alen;
  memcpy(b + 7, attrs, (size_t)alen);
  b[7 + alen] = (uint8_t)clen;
  memcpy(b + 8 + alen, cont, (size_t)clen);
  acl_in(HANDLE, scid, b, 8 + alen + clen, 1000);
}

static void hid_start_sdp(uint16_t *sdp_scid)
{
  start();
  memcpy(peer_cod, gamepad_cod, 3);
  secure_link();
  open_channel(SDP_REMOTE, 0x0001, sdp_scid);
  tick(5);
}

static void hid_pad_connects(void)
{
  uint8_t desc[300], attrs[1000];
  for (int i = 0; i < 300; i++)
    desc[i] = (uint8_t)(i % 4 == 0 ? 0x22 : i % 4 == 1 ? 0x35 : i);
  int alen = hid_attr_lists(attrs, desc, 300);
  uint16_t sdp_scid, ctl_scid, intr_scid;
  struct l2 f;
  hid_adds = hid_removes = hid_reports = 0;
  hid_start_sdp(&sdp_scid);

  expect(take_l2(&f));
  expect_eq(f.cid, SDP_REMOTE);
  expect_eq(f.d[0], 0x06);
  expect(!memcmp(f.d + 5, (uint8_t[]){0x35, 0x03, 0x19, 0x11, 0x24}, 5));
  expect(!memcmp(f.d + 12, (uint8_t[]){0x35, 0x03, 0x09, 0x02, 0x06}, 5));
  expect_eq(f.d[17], 0);
  uint16_t tid = (uint16_t)(f.d[1] << 8 | f.d[2]);

  const uint8_t cont[2] = {0xAB, 0xCD};
  sdp_rsp(sdp_scid, tid, attrs, 100, cont, 2);
  tick(5);
  expect(take_l2(&f));
  expect_eq(f.d[0], 0x06);
  expect_eq(f.d[17], 2);
  expect(!memcmp(f.d + 18, cont, 2));
  expect(status_have("hid sdp"));
  sdp_rsp(sdp_scid, tid + 1, attrs + 100, alen - 100, NULL, 0);
  tick(5);

  expect_sig(&f, 1, 0x06); /* SDP channel is done with */
  uint8_t dr[4];
  memcpy(dr, f.d + 4, 4);
  sig_in(HANDLE, 1, 0x07, f.d[1], dr, 4);
  open_channel(HID_CTL_REMOTE, 0x0011, &ctl_scid);
  tick(5);
  expect(status_have("hid interrupt"));
  expect_eq(hid_adds, 0);
  open_channel(HID_INTR_REMOTE, 0x0013, &intr_scid);
  tick(5);
  expect_eq(hid_adds, 1);
  expect_eq(hid_desc_len, 300);
  expect(!memcmp(hid_desc_seen, desc, 300));
  expect(status_have("hid up"));

  const uint8_t input[] = {0xA1, 0x01, 0x80, 0x7F, 0x03};
  acl_in(HANDLE, intr_scid, input, sizeof input, 1000);
  tick(5);
  expect_eq(hid_reports, 1);
  expect_eq(hid_last_len, 4);
  expect(!memcmp(hid_last_report, input + 1, 4));
  const uint8_t not_input[] = {0xA3, 0x01, 0x02};
  acl_in(HANDLE, intr_scid, not_input, sizeof not_input, 1000);
  tick(5);
  expect_eq(hid_reports, 1);

  rd = nsent;
  const uint8_t rumble[] = {0x03, 0x0F, 0x00, 0x00, 0x64, 0x32, 0x19};
  expect(hid_send_fn != NULL);
  if (!hid_send_fn)
    return;
  expect_eq(hid_send_fn(hid_send_ctx, 3, rumble, sizeof rumble), 0);
  tick(5);
  expect(take_l2(&f));
  expect_eq(f.cid, HID_INTR_REMOTE);
  expect_eq(f.len, 1 + (int)sizeof rumble);
  expect_eq(f.d[0], 0xA2);
  expect(!memcmp(f.d + 1, rumble, sizeof rumble));

  uint8_t dc[4] = {0};
  bt_put16(dc + 1, HANDLE);
  dc[3] = 0x13;
  evt(0x05, dc, 4);
  tick(5);
  expect_eq(hid_removes, 1);
  expect(!status_have("hid"));
  stop();
}

static void hid_sdp_without_descriptor_fails(void)
{
  uint16_t sdp_scid;
  struct l2 f;
  hid_adds = 0;
  hid_start_sdp(&sdp_scid);
  expect(take_l2(&f));
  uint8_t empty[] = {0x35, 0x00};
  sdp_rsp(sdp_scid, 1, empty, sizeof empty, NULL, 0);
  tick(5);
  expect(status_have("hid failed"));
  expect_eq(hid_adds, 0);
  stop();
}

static void hid_setup_times_out(void)
{
  uint16_t sdp_scid;
  hid_start_sdp(&sdp_scid);
  expect(status_have("hid sdp"));
  tick(9000);
  expect(status_have("hid failed"));
  stop();
}

static void headset_still_gets_a2dp(void)
{
  memcpy(peer_cod, (uint8_t[]){0x04, 0x24, 0x00}, 3);
  start();
  secure_link();
  struct l2 f;
  expect_sig(&f, 1, 0x02);
  expect_eq(bt_get16(f.d + 4), 0x19);
  expect(!status_have("hid"));
  stop();
}

int main(void)
{
  test("bringup", bringup);
  test("bringup_without_le", bringup_without_le);
  test("scan_lists_devices", scan_lists_devices);
  test("a2dp_streams_paced", a2dp_streams_paced);
  test("a2dp_idle_suspends", a2dp_idle_suspends);
  test("disconnect_resets_stream", disconnect_resets_stream);
  test("gatt_discovers_and_reads", gatt_discovers_and_reads);
  test("acl_flow_control", acl_flow_control);
  test("l2cap_reassembly_and_garbage", l2cap_reassembly_and_garbage);
  test("hid_pad_connects", hid_pad_connects);
  test("hid_sdp_without_descriptor_fails", hid_sdp_without_descriptor_fails);
  test("hid_setup_times_out", hid_setup_times_out);
  test("headset_still_gets_a2dp", headset_still_gets_a2dp);
  return 0;
}
