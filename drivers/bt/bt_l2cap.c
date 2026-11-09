/* bt_l2cap.c: L2CAP over ACL: frame reassembly, signalling and channels (fixed ATT, connection-oriented AVDTP). */

#include "bt_int.h"

#define MAX_CHANS 8
#define DEFAULT_MTU 672
#define CID_DYNAMIC 0x40

#define SIG_COMMAND_REJECT 0x01
#define SIG_CONN_REQ 0x02
#define SIG_CONN_RSP 0x03
#define SIG_CONFIG_REQ 0x04
#define SIG_CONFIG_RSP 0x05
#define SIG_DISCONN_REQ 0x06
#define SIG_DISCONN_RSP 0x07
#define SIG_ECHO_REQ 0x08
#define SIG_ECHO_RSP 0x09
#define SIG_INFO_REQ 0x0A
#define SIG_INFO_RSP 0x0B
#define SIG_LE_PARAM_REQ 0x12
#define SIG_LE_PARAM_RSP 0x13

static struct l2cap_chan chans[MAX_CHANS];
static uint16_t next_cid = CID_DYNAMIC;
static uint8_t last_ident;

static uint8_t new_ident(void)
{
  if (++last_ident == 0)
    last_ident = 1;
  return last_ident;
}

static struct l2cap_chan *chan_alloc(struct bt_conn *c)
{
  for (int i = 0; i < MAX_CHANS; i++) {
    if (chans[i].used)
      continue;
    memset(&chans[i], 0, sizeof chans[i]);
    chans[i].used = true;
    chans[i].conn = c;
    chans[i].out_mtu = DEFAULT_MTU;
    return &chans[i];
  }
  return NULL;
}

static struct l2cap_chan *chan_by_scid(const struct bt_conn *c, uint16_t scid)
{
  for (int i = 0; i < MAX_CHANS; i++)
    if (chans[i].used && chans[i].conn == c && chans[i].scid == scid)
      return &chans[i];
  return NULL;
}

static void chan_release(struct l2cap_chan *ch)
{
  ch->open = false;
  if (ch->on_state)
    ch->on_state(ch);
  ch->used = false;
}

int l2cap_send_cid(struct bt_conn *c, uint16_t cid, const uint8_t *data, int len)
{
  uint8_t buf[BT_RX_MAX];
  if (len + 4 > (int)sizeof buf)
    return -1;
  bt_put16(buf, (uint16_t)len);
  bt_put16(buf + 2, cid);
  memcpy(buf + 4, data, (uint32_t)len);
  return hci_acl_send(c, buf, len + 4);
}

static void sig_send(struct bt_conn *c, uint8_t code, uint8_t ident, const uint8_t *data, int len)
{
  uint8_t cmd[4 + 64];
  if (len > 64)
    return;
  cmd[0] = code;
  cmd[1] = ident;
  bt_put16(cmd + 2, (uint16_t)len);
  memcpy(cmd + 4, data, (uint32_t)len);
  l2cap_send_cid(c, c->le ? L2CAP_CID_LE_SIGNALING : L2CAP_CID_SIGNALING, cmd, 4 + len);
}

static void send_config_req(struct l2cap_chan *ch)
{
  uint8_t p[4];
  bt_put16(p, ch->dcid);
  bt_put16(p + 2, 0);
  sig_send(ch->conn, SIG_CONFIG_REQ, new_ident(), p, 4);
}

/* on_state runs when the channel becomes usable (open) and again when it goes away (not open). */
static void maybe_open(struct l2cap_chan *ch)
{
  if (ch->cfg == 3 && !ch->open) {
    ch->open = true;
    if (ch->on_state)
      ch->on_state(ch);
  }
}

static void sig_command(struct bt_conn *c, uint8_t code, uint8_t ident, const uint8_t *d, int len)
{
  uint8_t r[8];
  struct l2cap_chan *ch;

  switch (code) {
  case SIG_CONN_REQ:
    if (len >= 4) {
      bt_put16(r, 0);
      bt_put16(r + 2, bt_get16(d + 2));
      bt_put16(r + 4, 2); /* PSM not supported */
      bt_put16(r + 6, 0);
      sig_send(c, SIG_CONN_RSP, ident, r, 8);
    }
    break;
  case SIG_CONN_RSP:
    if (len >= 8 && (ch = chan_by_scid(c, bt_get16(d + 2)))) {
      uint16_t result = bt_get16(d + 4);
      if (result == 0) {
        ch->dcid = bt_get16(d);
        send_config_req(ch);
      } else if (result != 1) {
        chan_release(ch);
      }
    }
    break;
  case SIG_CONFIG_REQ:
    if (len >= 4 && (ch = chan_by_scid(c, bt_get16(d)))) {
      for (int i = 4; i + 2 <= len && i + 2 + d[i + 1] <= len; i += 2 + d[i + 1])
        if ((d[i] & 0x7F) == 1 && d[i + 1] == 2)
          ch->out_mtu = bt_get16(d + i + 2);
      bt_put16(r, ch->dcid);
      bt_put16(r + 2, 0);
      bt_put16(r + 4, 0);
      sig_send(c, SIG_CONFIG_RSP, ident, r, 6);
      ch->cfg |= 2;
      maybe_open(ch);
    }
    break;
  case SIG_CONFIG_RSP:
    if (len >= 6 && (ch = chan_by_scid(c, bt_get16(d)))) {
      if (bt_get16(d + 4) == 0) {
        ch->cfg |= 1;
        maybe_open(ch);
      } else {
        chan_release(ch);
      }
    }
    break;
  case SIG_DISCONN_REQ:
    if (len >= 4 && (ch = chan_by_scid(c, bt_get16(d + 2)))) {
      memcpy(r, d, 4);
      sig_send(c, SIG_DISCONN_RSP, ident, r, 4);
      chan_release(ch);
    }
    break;
  case SIG_DISCONN_RSP:
    if (len >= 4 && (ch = chan_by_scid(c, bt_get16(d + 2))))
      chan_release(ch);
    break;
  case SIG_ECHO_REQ:
    sig_send(c, SIG_ECHO_RSP, ident, d, len);
    break;
  case SIG_INFO_REQ:
    if (len >= 2) {
      memcpy(r, d, 2);
      bt_put16(r + 2, 1); /* not supported */
      sig_send(c, SIG_INFO_RSP, ident, r, 4);
    }
    break;
  case SIG_LE_PARAM_REQ:
    bt_put16(r, 1); /* rejected */
    sig_send(c, SIG_LE_PARAM_RSP, ident, r, 2);
    break;
  case SIG_COMMAND_REJECT:
    bt_log("l2cap command rejected");
    break;
  }
}

static void sig_rx(struct bt_conn *c, const uint8_t *d, int len)
{
  while (len >= 4) {
    int l = bt_get16(d + 2);
    if (4 + l > len)
      return;
    sig_command(c, d[0], d[1], d + 4, l);
    d += 4 + l;
    len -= 4 + l;
  }
}

static void frame_rx(struct bt_conn *c, const uint8_t *f, int len)
{
  uint16_t cid = bt_get16(f + 2);
  const uint8_t *p = f + 4;
  int plen = len - 4;
  if (cid == L2CAP_CID_SIGNALING || cid == L2CAP_CID_LE_SIGNALING) {
    sig_rx(c, p, plen);
    return;
  }
  if (cid == L2CAP_CID_SMP && c->le) {
    smp_rx(c, p, plen);
    return;
  }
  struct l2cap_chan *ch = chan_by_scid(c, cid);
  if (ch && ch->open && ch->on_data)
    ch->on_data(ch, p, plen);
}

void l2cap_acl_rx(struct bt_conn *c, uint16_t flags, const uint8_t *data, int len)
{
  if ((flags & 3) == 1) {
    if (!c->rx_need || c->rx_len + len > c->rx_need)
      return;
  } else {
    c->rx_len = 0;
    c->rx_need = 0;
    if (len < 4)
      return;
    int need = 4 + bt_get16(data);
    if (need > BT_RX_MAX || len > need)
      return;
    c->rx_need = (uint16_t)need;
  }
  memcpy(c->rx + c->rx_len, data, (uint32_t)len);
  c->rx_len += (uint16_t)len;
  if (c->rx_len == c->rx_need) {
    c->rx_need = 0;
    frame_rx(c, c->rx, c->rx_len);
  }
}

struct l2cap_chan *l2cap_connect(struct bt_conn *c, uint16_t psm,
                                 void (*on_data)(struct l2cap_chan *, const uint8_t *, int),
                                 void (*on_state)(struct l2cap_chan *), void *user)
{
  struct l2cap_chan *ch = chan_alloc(c);
  if (!ch)
    return NULL;
  ch->psm = psm;
  ch->scid = next_cid++;
  ch->on_data = on_data;
  ch->on_state = on_state;
  ch->user = user;
  uint8_t p[4];
  bt_put16(p, psm);
  bt_put16(p + 2, ch->scid);
  sig_send(c, SIG_CONN_REQ, new_ident(), p, 4);
  return ch;
}

struct l2cap_chan *l2cap_att(struct bt_conn *c) { return chan_by_scid(c, L2CAP_CID_ATT); }

int l2cap_send(struct l2cap_chan *ch, const uint8_t *data, int len)
{
  if (!ch->open || len > ch->out_mtu)
    return -1;
  return l2cap_send_cid(ch->conn, ch->dcid, data, len);
}

bool l2cap_room(const struct l2cap_chan *ch) { return ch->open && hci_acl_room(ch->conn, 4); }

void l2cap_close(struct l2cap_chan *ch)
{
  if (!ch->dcid) {
    chan_release(ch);
    return;
  }
  uint8_t p[4];
  bt_put16(p, ch->dcid);
  bt_put16(p + 2, ch->scid);
  sig_send(ch->conn, SIG_DISCONN_REQ, new_ident(), p, 4);
}

void l2cap_conn_up(struct bt_conn *c)
{
  if (!c->le)
    return;
  struct l2cap_chan *ch = chan_alloc(c);
  if (!ch)
    return;
  ch->open = true;
  ch->scid = ch->dcid = L2CAP_CID_ATT;
  ch->out_mtu = 23;
  ch->cfg = 3;
}

void l2cap_conn_down(struct bt_conn *c)
{
  for (int i = 0; i < MAX_CHANS; i++)
    if (chans[i].used && chans[i].conn == c)
      chan_release(&chans[i]);
}
