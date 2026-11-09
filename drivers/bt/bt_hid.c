/* bt_hid.c: Bluetooth HID host for BR/EDR game controllers and other input devices: fetch the report descriptor over
 * SDP, open the HID control and interrupt channels, pass input reports to the HID layer and send its output reports
 * (rumble, LEDs) back. */

#include "../usb/hid.h"
#include "bt_int.h"

#define PSM_SDP 0x0001
#define PSM_HID_CTRL 0x0011
#define PSM_HID_INTR 0x0013
#define SDP_SSA_REQ 0x06
#define SDP_SSA_RSP 0x07
#define SDP_MAX_ATTR_BYTES 0x01F0
#define SDP_CONT_MAX 16
#define DESC_MAX 2048
#define HIDP_DATA_INPUT 0xA1
#define HIDP_DATA_OUTPUT 0xA2
#define OUT_SLOTS 4
#define OUT_MAX 80
#define STAGE_TIMEOUT_MS 8000
#define MAX_PADS 2

enum { H_IDLE, H_SDP, H_CTRL, H_INTR, H_UP, H_FAILED };

struct out_msg {
  uint8_t len;
  uint8_t data[OUT_MAX];
};

struct pad {
  struct bt_conn *conn;
  int stage;
  uint32_t stage_at;
  struct l2cap_chan *sdp, *ctl, *intr;
  uint16_t tid;
  uint8_t cont[SDP_CONT_MAX], cont_len;
  uint8_t *rsp;
  uint16_t rsp_len;
  uint8_t *desc;
  int desc_len;
  struct hid_dev *hid;
  struct out_msg out[OUT_SLOTS];
  volatile int out_head, out_tail;
};

static struct pad pads[MAX_PADS];

static const char *const stage_names[] = {"idle", "sdp", "control", "interrupt", "up", "failed"};

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put_be16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void stage_set(struct pad *p, int stage)
{
  p->stage = stage;
  p->stage_at = bt_now();
}

static void pad_free_buffers(struct pad *p)
{
  bt_free(p->rsp);
  bt_free(p->desc);
  p->rsp = p->desc = NULL;
  p->rsp_len = 0;
  p->desc_len = 0;
}

static void pad_close_chans(struct pad *p)
{
  struct l2cap_chan *chans[] = {p->sdp, p->ctl, p->intr};
  for (unsigned i = 0; i < sizeof chans / sizeof chans[0]; i++)
    if (chans[i])
      l2cap_close(chans[i]);
}

static void pad_fail(struct pad *p, const char *why)
{
  if (p->stage == H_FAILED || p->stage == H_IDLE)
    return;
  bt_log("hid: %s", why);
  if (p->hid) {
    hid_bt_remove(p->hid);
    p->hid = NULL;
  }
  pad_free_buffers(p);
  stage_set(p, H_FAILED);
  pad_close_chans(p);
}

/* ---- SDP: the HIDDescriptorList attribute of the HID service */

static void sdp_request(struct pad *p)
{
  static const uint8_t body[] = {0x35, 0x03, 0x19, 0x11, 0x24, SDP_MAX_ATTR_BYTES >> 8, SDP_MAX_ATTR_BYTES & 0xFF,
                                 0x35, 0x03, 0x09, 0x02, 0x06};
  uint8_t r[5 + sizeof body + 1 + SDP_CONT_MAX] = {SDP_SSA_REQ};
  int n = 5;
  memcpy(r + n, body, sizeof body);
  n += (int)sizeof body;
  r[n++] = p->cont_len;
  memcpy(r + n, p->cont, p->cont_len);
  n += p->cont_len;
  put_be16(r + 1, ++p->tid);
  put_be16(r + 3, (uint16_t)(n - 5));
  l2cap_send(p->sdp, r, n);
}

/* Header of the SDP data element at p: its type, payload offset and payload length; false if malformed. */
static bool sdp_elem(const uint8_t *p, int len, int *type, int *off, int *dl)
{
  int size = p[0] & 7;
  if (len < 1)
    return false;
  *type = p[0] >> 3;
  if (*type == 0) {
    *off = 1;
    *dl = 0;
  } else if (size < 5) {
    *off = 1;
    *dl = 1 << size;
  } else if (size == 5 && len >= 2) {
    *off = 2;
    *dl = p[1];
  } else if (size == 6 && len >= 3) {
    *off = 3;
    *dl = be16(p + 1);
  } else {
    return false;
  }
  return *off + *dl <= len;
}

/* The report descriptor inside the attribute lists: the text string that follows the uint8 0x22 (report). */
static const uint8_t *sdp_find_desc(const uint8_t *p, int len, int *dl, int depth)
{
  bool report_type = false;
  while (len > 0) {
    int type, off, n;
    if (!sdp_elem(p, len, &type, &off, &n))
      return NULL;
    if ((type == 6 || type == 7) && depth < 8) {
      const uint8_t *r = sdp_find_desc(p + off, n, dl, depth + 1);
      if (r)
        return r;
      report_type = false;
    } else if (type == 4 && report_type) {
      *dl = n;
      return p + off;
    } else {
      report_type = type == 1 && n == 1 && p[off] == 0x22;
    }
    p += off + n;
    len -= off + n;
  }
  return NULL;
}

static void ctl_state(struct l2cap_chan *ch);
static void intr_state(struct l2cap_chan *ch);
static void intr_rx(struct l2cap_chan *ch, const uint8_t *d, int len);

static void sdp_done(struct pad *p)
{
  int dl = 0;
  const uint8_t *d = sdp_find_desc(p->rsp, p->rsp_len, &dl, 0);
  if (!d || dl <= 0 || dl > DESC_MAX) {
    pad_fail(p, "no HID report descriptor from SDP");
    return;
  }
  p->desc = bt_alloc((uint32_t)dl);
  if (!p->desc) {
    pad_fail(p, "out of memory");
    return;
  }
  memcpy(p->desc, d, (uint32_t)dl);
  p->desc_len = dl;
  bt_free(p->rsp);
  p->rsp = NULL;
  p->rsp_len = 0;
  stage_set(p, H_CTRL);
  l2cap_close(p->sdp);
  p->ctl = l2cap_connect(p->conn, PSM_HID_CTRL, NULL, ctl_state, p);
  if (!p->ctl)
    pad_fail(p, "no channel for HID control");
}

static void sdp_rx(struct l2cap_chan *ch, const uint8_t *d, int len)
{
  struct pad *p = ch->user;
  if (p->stage != H_SDP)
    return;
  if (len < 8 || d[0] != SDP_SSA_RSP || 5 + be16(d + 3) > len) {
    pad_fail(p, "SDP search failed");
    return;
  }
  int alen = be16(d + 5);
  if (7 + alen + 1 > len || p->rsp_len + alen > DESC_MAX + 64) {
    pad_fail(p, "SDP response malformed");
    return;
  }
  if (!p->rsp)
    p->rsp = bt_alloc(DESC_MAX + 64);
  if (!p->rsp) {
    pad_fail(p, "out of memory");
    return;
  }
  memcpy(p->rsp + p->rsp_len, d + 7, (uint32_t)alen);
  p->rsp_len = (uint16_t)(p->rsp_len + alen);
  p->cont_len = d[7 + alen];
  if (p->cont_len > SDP_CONT_MAX || 7 + alen + 1 + p->cont_len > len) {
    pad_fail(p, "SDP continuation malformed");
    return;
  }
  memcpy(p->cont, d + 7 + alen + 1, p->cont_len);
  p->stage_at = bt_now();
  if (p->cont_len)
    sdp_request(p);
  else
    sdp_done(p);
}

static void sdp_state(struct l2cap_chan *ch)
{
  struct pad *p = ch->user;
  if (ch->open) {
    sdp_request(p);
    return;
  }
  p->sdp = NULL;
  if (p->stage == H_SDP)
    pad_fail(p, "SDP channel refused");
}

/* ---- HID channels */

static void ctl_state(struct l2cap_chan *ch)
{
  struct pad *p = ch->user;
  if (ch->open) {
    stage_set(p, H_INTR);
    p->intr = l2cap_connect(p->conn, PSM_HID_INTR, intr_rx, intr_state, p);
    if (!p->intr)
      pad_fail(p, "no channel for HID interrupt");
    return;
  }
  p->ctl = NULL;
  pad_fail(p, "HID control channel closed");
}

static int send_output(void *ctx, uint8_t id, const uint8_t *buf, int n)
{
  struct pad *p = ctx;
  int rc = -1;
  (void)id;
  if (n < 1 || n > OUT_MAX - 1)
    return -1;
  uintptr_t f = bt_irq_save();
  int next = (p->out_head + 1) % OUT_SLOTS;
  if (next != p->out_tail) {
    struct out_msg *m = &p->out[p->out_head];
    m->data[0] = HIDP_DATA_OUTPUT;
    memcpy(m->data + 1, buf, (uint32_t)n);
    m->len = (uint8_t)(n + 1);
    p->out_head = next;
    rc = 0;
  }
  bt_irq_restore(f);
  return rc;
}

static void intr_state(struct l2cap_chan *ch)
{
  struct pad *p = ch->user;
  if (ch->open) {
    p->hid = hid_bt_add(p->desc, p->desc_len, send_output, p);
    pad_free_buffers(p);
    if (!p->hid)
      pad_fail(p, "unusable HID report descriptor");
    else
      stage_set(p, H_UP);
    return;
  }
  p->intr = NULL;
  pad_fail(p, "HID interrupt channel closed");
}

static void intr_rx(struct l2cap_chan *ch, const uint8_t *d, int len)
{
  struct pad *p = ch->user;
  if (p->hid && len >= 2 && d[0] == HIDP_DATA_INPUT)
    hid_bt_report(p->hid, d + 1, len - 1);
}

/* ---- connection hooks */

void bthid_conn_ready(struct bt_conn *c)
{
  struct pad *p = NULL;
  for (int i = 0; i < MAX_PADS && !p; i++)
    if (pads[i].stage == H_IDLE || (pads[i].stage == H_FAILED && pads[i].conn == c))
      p = &pads[i];
  if (!p)
    return;
  memset(p, 0, sizeof *p);
  p->conn = c;
  stage_set(p, H_SDP);
  p->sdp = l2cap_connect(c, PSM_SDP, sdp_rx, sdp_state, p);
  if (!p->sdp)
    pad_fail(p, "no channel for SDP");
}

void bthid_conn_down(struct bt_conn *c)
{
  for (int i = 0; i < MAX_PADS; i++) {
    struct pad *p = &pads[i];
    if (p->conn != c)
      continue;
    if (p->hid)
      hid_bt_remove(p->hid);
    pad_free_buffers(p);
    memset(p, 0, sizeof *p);
  }
}

void bthid_poll(uint32_t now)
{
  for (int i = 0; i < MAX_PADS; i++) {
    struct pad *p = &pads[i];
    if (p->stage >= H_SDP && p->stage < H_UP && now - p->stage_at > STAGE_TIMEOUT_MS)
      pad_fail(p, "HID setup timed out");
    while (p->stage == H_UP && p->out_tail != p->out_head && p->intr && l2cap_room(p->intr)) {
      struct out_msg *m = &p->out[p->out_tail];
      l2cap_send(p->intr, m->data, m->len);
      p->out_tail = (p->out_tail + 1) % OUT_SLOTS;
    }
  }
}

int bthid_status_text(char *buf, int max)
{
  int n = 0;
  for (int i = 0; i < MAX_PADS && n < max; i++)
    if (pads[i].stage != H_IDLE)
      n += bt_snprintf(buf + n, (uint32_t)(max - n), "hid %s\n", stage_names[pads[i].stage]);
  return n < max ? n : max;
}
