/* bt_gatt.c: ATT/GATT client for one LE connection: service and characteristic discovery, read, write, notify. */

#include "bt_int.h"

#define ATT_ERROR 0x01
#define ATT_MTU_REQ 0x02
#define ATT_MTU_RSP 0x03
#define ATT_FIND_INFO_REQ 0x04
#define ATT_FIND_INFO_RSP 0x05
#define ATT_READ_TYPE_REQ 0x08
#define ATT_READ_TYPE_RSP 0x09
#define ATT_READ_REQ 0x0A
#define ATT_READ_RSP 0x0B
#define ATT_READ_BLOB_REQ 0x0C
#define ATT_READ_BLOB_RSP 0x0D
#define ATT_READ_GROUP_REQ 0x10
#define ATT_READ_GROUP_RSP 0x11
#define ATT_WRITE_REQ 0x12
#define ATT_WRITE_RSP 0x13
#define ATT_WRITE_CMD 0x52
#define ATT_NOTIFY 0x1B
#define ATT_INDICATE 0x1D
#define ATT_CONFIRM 0x1E

#define ERR_INSUFF_AUTH 0x05
#define ERR_INSUFF_ENC 0x0F
#define ERR_NOT_FOUND 0x0A
#define UUID_PRIMARY 0x2800
#define UUID_CHARACTERISTIC 0x2803
#define UUID_CCCD 0x2902
#define CLIENT_MTU 247
#define REQ_TIMEOUT_MS 5000

#define MAX_SVC 16
#define MAX_CHR 48
#define MAX_LOG 8
#define MAX_OPS 4
#define MAX_DATA 244 /* largest write: the ATT MTU we ask for minus the 3 byte header */
#define MAX_VALUE 512
#define MAX_EVENTS 16
#define EVENTS_TEXT_MAX 2000
#define ERR_TIMEOUT 0xFF
#define ERR_BAD_LENGTH 0x0D
#define ATT_ERR_NO_CCCD 0xFE

struct uuid {
  uint8_t len; /* 2 or 16 */
  uint8_t v[16];
};

struct svc {
  uint16_t start, end;
  struct uuid uuid;
};

struct chr {
  uint16_t handle, value;
  uint8_t props;
  struct uuid uuid;
};

enum { OP_READ, OP_WRITE, OP_NOTIFY, OP_WRITE_CMD };

struct op {
  uint8_t kind;
  bool ev; /* result goes to the event queue under tag instead of the text log */
  uint16_t handle;
  uint32_t tag;
  uint8_t len;
  uint8_t data[MAX_DATA];
};

enum { EV_READ, EV_WRITE, EV_NOTIFY, EV_ERR };

struct event {
  uint32_t seq, tag;
  uint8_t kind;
  uint16_t handle, len;
  uint8_t data[MAX_VALUE];
};

enum { ST_IDLE, ST_MTU, ST_SERVICES, ST_CHARS, ST_READY };

static struct {
  struct bt_conn *c;
  struct l2cap_chan *att;
  int stage;
  bool waiting;
  uint32_t sent_at;
  uint16_t cursor;
  int nsvc, nchr, svc_idx;
  struct svc svc[MAX_SVC];
  struct chr chr[MAX_CHR];
  struct op ops[MAX_OPS];
  int ops_head, ops_tail;
  struct op cur;
  bool cur_active, cur_finding, sec_wait;
  uint8_t last[3 + MAX_DATA], last_len;
  uint8_t acc[MAX_VALUE];
  uint16_t acc_len;
  char log[MAX_LOG][64];
  int log_n;
} g;

static void log_line(const char *fmt, ...)
{
  va_list ap;
  char *dst = g.log[g.log_n++ % MAX_LOG];
  va_start(ap, fmt);
#if __STDC_HOSTED__
  vsnprintf(dst, 64, fmt, ap);
#else
  kvsnprintf(dst, 64, fmt, ap);
#endif
  va_end(ap);
}

static struct event events[MAX_EVENTS];
static int ev_count;
static uint32_t ev_seq;

static void event_push(uint32_t tag, uint8_t kind, uint16_t handle, const uint8_t *d, int len)
{
  if (ev_count == MAX_EVENTS) {
    memmove(events, events + 1, sizeof events - sizeof events[0]);
    ev_count--;
  }
  struct event *e = &events[ev_count++];
  e->seq = ++ev_seq;
  e->tag = tag;
  e->kind = kind;
  e->handle = handle;
  e->len = (uint16_t)(len < MAX_VALUE ? len : MAX_VALUE);
  memcpy(e->data, d, e->len);
}

static void op_done(uint8_t kind, const uint8_t *d, int len)
{
  if (g.cur.ev)
    event_push(g.cur.tag, kind, g.cur.handle, d, len);
  g.cur_active = g.cur_finding = false;
}

static void op_failed(uint8_t code)
{
  if (g.cur.ev) {
    event_push(g.cur.tag, EV_ERR, g.cur.handle, &code, 1);
  } else {
    log_line("%04x: error %02x", g.cur.handle, code);
  }
  g.cur_active = g.cur_finding = false;
}

static int hex_text(char *out, int max, const uint8_t *d, int len)
{
  int n = 0;
  for (int i = 0; i < len && n + 3 < max; i++)
    n += bt_snprintf(out + n, (uint32_t)(max - n), "%02x%s", d[i], i + 1 < len ? " " : "");
  return n;
}

static void value_log(const char *tag, uint16_t handle, const uint8_t *d, int len)
{
  char hex[48];
  hex[0] = 0;
  hex_text(hex, sizeof hex, d, len > 14 ? 14 : len);
  log_line("%s %04x = %s%s", tag, handle, hex, len > 14 ? " ..." : "");
}

static void att_send(const uint8_t *pdu, int len)
{
  if (g.att && l2cap_send(g.att, pdu, len) == 0) {
    g.waiting = true;
    g.sent_at = bt_now();
    g.last_len = (uint8_t)(len < (int)sizeof g.last ? len : 0);
    memcpy(g.last, pdu, g.last_len);
  }
}

static struct uuid uuid_from(const uint8_t *p, int len)
{
  struct uuid u = {(uint8_t)len, {0}};
  memcpy(u.v, p, (uint32_t)len);
  return u;
}

static void send_group_req(void)
{
  uint8_t p[7] = {ATT_READ_GROUP_REQ};
  bt_put16(p + 1, g.cursor);
  bt_put16(p + 3, 0xFFFF);
  bt_put16(p + 5, UUID_PRIMARY);
  att_send(p, 7);
}

static void send_chars_req(void)
{
  uint8_t p[7] = {ATT_READ_TYPE_REQ};
  bt_put16(p + 1, g.cursor);
  bt_put16(p + 3, g.svc[g.svc_idx].end);
  bt_put16(p + 5, UUID_CHARACTERISTIC);
  att_send(p, 7);
}

static void next_service_chars(void)
{
  if (++g.svc_idx >= g.nsvc) {
    g.stage = ST_READY;
    bt_log("gatt discovery done: %d services, %d characteristics", g.nsvc, g.nchr);
    return;
  }
  g.cursor = g.svc[g.svc_idx].start;
  send_chars_req();
}

static void discovery_done_services(void)
{
  g.stage = ST_CHARS;
  g.svc_idx = -1;
  next_service_chars();
}

static void on_error(const uint8_t *d, int len)
{
  uint8_t op = d[0];
  if (len < 4)
    return;
  if ((d[3] == ERR_INSUFF_AUTH || d[3] == ERR_INSUFF_ENC) && !g.c->secure && !g.sec_wait && g.last_len &&
      smp_start(g.c) == 0) {
    g.sec_wait = true;
    return;
  }
  if (g.stage == ST_SERVICES && op == ATT_READ_GROUP_REQ)
    discovery_done_services();
  else if (g.stage == ST_CHARS && op == ATT_READ_TYPE_REQ)
    next_service_chars();
  else if (g.stage == ST_MTU) {
    g.stage = ST_SERVICES;
    g.cursor = 1;
    send_group_req();
  } else if (g.cur_active) {
    op_failed(d[3]);
  }
}

static void on_group_rsp(const uint8_t *d, int len)
{
  int el = d[1];
  if (el != 6 && el != 20)
    return;
  uint16_t last = 0;
  for (int i = 2; i + el <= len; i += el) {
    if (g.nsvc < MAX_SVC) {
      g.svc[g.nsvc].start = bt_get16(d + i);
      g.svc[g.nsvc].end = bt_get16(d + i + 2);
      g.svc[g.nsvc].uuid = uuid_from(d + i + 4, el - 4);
      g.nsvc++;
    }
    last = bt_get16(d + i + 2);
  }
  if (last == 0xFFFF || last == 0 || g.nsvc >= MAX_SVC) {
    discovery_done_services();
  } else {
    g.cursor = last + 1;
    send_group_req();
  }
}

static void on_type_rsp(const uint8_t *d, int len)
{
  int el = d[1];
  if (el != 7 && el != 21)
    return;
  uint16_t last = 0;
  for (int i = 2; i + el <= len; i += el) {
    if (g.nchr < MAX_CHR) {
      struct chr *c = &g.chr[g.nchr++];
      c->handle = bt_get16(d + i);
      c->props = d[i + 2];
      c->value = bt_get16(d + i + 3);
      c->uuid = uuid_from(d + i + 5, el - 5);
    }
    last = bt_get16(d + i);
  }
  if (last == 0 || last >= g.svc[g.svc_idx].end || g.nchr >= MAX_CHR) {
    next_service_chars();
  } else {
    g.cursor = last + 1;
    send_chars_req();
  }
}

static void write_value(uint16_t handle, const uint8_t *v, int len)
{
  uint8_t p[3 + MAX_DATA];
  p[0] = ATT_WRITE_REQ;
  bt_put16(p + 1, handle);
  memcpy(p + 3, v, (uint32_t)len);
  att_send(p, 3 + len);
}

/* CCCD value enabling notifications, or indications for a characteristic that only has those. */
static uint8_t cccd_value(uint16_t value_handle, bool on)
{
  for (int i = 0; i < g.nchr; i++)
    if (g.chr[i].value == value_handle)
      return !on ? 0 : g.chr[i].props & 0x10 ? 1 : 2;
  return on ? 1 : 0;
}

static void on_find_info_rsp(const uint8_t *d, int len)
{
  int el = d[1] == 1 ? 4 : 18;
  for (int i = 2; i + el <= len; i += el)
    if (d[1] == 1 && bt_get16(d + i + 2) == UUID_CCCD) {
      uint8_t v[2] = {cccd_value(g.cur.handle, g.cur.data[0]), 0};
      g.cur_finding = false;
      g.cur.handle = bt_get16(d + i);
      write_value(g.cur.handle, v, 2);
      return;
    }
  if (g.cur.ev)
    op_failed(ATT_ERR_NO_CCCD);
  else
    log_line("%04x: no notification descriptor", g.cur.handle);
  g.cur_active = g.cur_finding = false;
}

/* A read answer; a full-size one may be the first part of a long value, so ask for the rest. */
static void on_read_rsp(const uint8_t *d, int len)
{
  int n = len - 1, room = MAX_VALUE - g.acc_len;
  if (n > room)
    n = room;
  memcpy(g.acc + g.acc_len, d + 1, (uint32_t)n);
  g.acc_len = (uint16_t)(g.acc_len + n);
  if (g.cur.ev && len == g.att->out_mtu && g.acc_len < MAX_VALUE) {
    uint8_t p[5] = {ATT_READ_BLOB_REQ};
    bt_put16(p + 1, g.cur.handle);
    bt_put16(p + 3, g.acc_len);
    att_send(p, 5);
    return;
  }
  if (!g.cur.ev)
    value_log("read", g.cur.handle, g.acc, g.acc_len);
  op_done(EV_READ, g.acc, g.acc_len);
}

static void att_rx(struct l2cap_chan *ch, const uint8_t *d, int len)
{
  (void)ch;
  if (len < 1)
    return;
  if (d[0] == ATT_NOTIFY || d[0] == ATT_INDICATE) {
    if (len >= 3) {
      value_log(d[0] == ATT_NOTIFY ? "notify" : "indicate", bt_get16(d + 1), d + 3, len - 3);
      event_push(0, EV_NOTIFY, bt_get16(d + 1), d + 3, len - 3);
    }
    if (d[0] == ATT_INDICATE) {
      uint8_t c = ATT_CONFIRM;
      l2cap_send(g.att, &c, 1);
    }
    return;
  }
  g.waiting = false;
  switch (d[0]) {
  case ATT_ERROR:
    on_error(d + 1, len - 1);
    break;
  case ATT_MTU_RSP:
    g.cursor = 1;
    if (len >= 3 && g.att) {
      uint16_t m = bt_get16(d + 1);
      g.att->out_mtu = m < CLIENT_MTU ? (m < 23 ? 23 : m) : CLIENT_MTU;
    }
    g.stage = ST_SERVICES;
    send_group_req();
    break;
  case ATT_READ_GROUP_RSP:
    if (g.stage == ST_SERVICES && len >= 2)
      on_group_rsp(d, len);
    break;
  case ATT_READ_TYPE_RSP:
    if (g.stage == ST_CHARS && len >= 2)
      on_type_rsp(d, len);
    break;
  case ATT_READ_RSP:
  case ATT_READ_BLOB_RSP:
    if (g.cur_active)
      on_read_rsp(d, len);
    break;
  case ATT_WRITE_RSP:
    if (g.cur_active) {
      if (!g.cur.ev)
        log_line("%04x: written", g.cur.handle);
      op_done(EV_WRITE, NULL, 0);
    }
    break;
  case ATT_FIND_INFO_RSP:
    if (g.cur_finding && len >= 2)
      on_find_info_rsp(d, len);
    break;
  }
}

static void att_state(struct l2cap_chan *ch) { (void)ch; }

void gatt_conn_up(struct bt_conn *c)
{
  if (g.c)
    return;
  memset(&g, 0, sizeof g);
  ev_count = 0;
  ev_seq = 0;
  g.c = c;
  g.att = l2cap_att(c);
  if (!g.att)
    return;
  g.att->on_data = att_rx;
  g.att->on_state = att_state;
  g.stage = ST_MTU;
  uint8_t p[3] = {ATT_MTU_REQ};
  bt_put16(p + 1, CLIENT_MTU);
  att_send(p, 3);
}

void gatt_secured(struct bt_conn *c, bool ok)
{
  if (g.c != c || !g.sec_wait)
    return;
  g.sec_wait = false;
  if (ok) {
    att_send(g.last, g.last_len);
  } else {
    g.cur_active = g.cur_finding = false;
    g.stage = ST_READY;
  }
}

void gatt_conn_down(struct bt_conn *c)
{
  if (g.c == c)
    memset(&g, 0, sizeof g);
}

static void op_start(void)
{
  struct op *o = &g.cur;
  g.cur_active = true;
  g.acc_len = 0;
  uint8_t p[5];
  if (o->kind == OP_READ) {
    p[0] = ATT_READ_REQ;
    bt_put16(p + 1, o->handle);
    att_send(p, 3);
  } else if (o->kind == OP_WRITE || o->kind == OP_WRITE_CMD) {
    if (o->len > g.att->out_mtu - 3) {
      op_failed(ERR_BAD_LENGTH);
    } else if (o->kind == OP_WRITE) {
      write_value(o->handle, o->data, o->len);
    } else {
      uint8_t w[3 + MAX_DATA] = {ATT_WRITE_CMD};
      bt_put16(w + 1, o->handle);
      memcpy(w + 3, o->data, o->len);
      if (l2cap_send(g.att, w, 3 + o->len) == 0)
        op_done(EV_WRITE, NULL, 0);
      else
        op_failed(ERR_TIMEOUT);
    }
  } else {
    uint16_t end = o->handle;
    for (int i = 0; i < g.nsvc; i++)
      if (o->handle >= g.svc[i].start && o->handle <= g.svc[i].end)
        end = g.svc[i].end;
    g.cur_finding = true;
    p[0] = ATT_FIND_INFO_REQ;
    bt_put16(p + 1, o->handle + 1);
    bt_put16(p + 3, end);
    att_send(p, 5);
  }
}

void gatt_poll(void)
{
  if (!g.c)
    return;
  if (g.waiting && bt_now() - g.sent_at > REQ_TIMEOUT_MS) {
    bt_log("gatt request timed out");
    g.waiting = false;
    if (g.cur_active)
      op_failed(ERR_TIMEOUT);
    g.cur_active = g.cur_finding = false;
    if (g.stage != ST_READY)
      g.stage = ST_READY;
  }
  if (g.stage == ST_READY && !g.waiting && !g.cur_active && g.ops_tail != g.ops_head) {
    g.cur = g.ops[g.ops_tail];
    g.ops_tail = (g.ops_tail + 1) % MAX_OPS;
    op_start();
  }
}

static int hex_digit(char c)
{
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

static int parse_hex(const char **s, unsigned max_digits)
{
  int v = 0;
  unsigned n = 0;
  while (**s && **s != ' ') {
    int d = hex_digit(**s);
    if (d < 0 || ++n > max_digits)
      return -1;
    v = v * 16 + d;
    (*s)++;
  }
  return n ? v : -1;
}

static bool parse_uint(const char **s, uint32_t *v)
{
  const char *p = *s;
  *v = 0;
  while (*p >= '0' && *p <= '9' && *v < 0x10000000)
    *v = *v * 10 + (uint32_t)(*p++ - '0');
  if (p == *s || (*p && *p != ' '))
    return false;
  *s = p;
  return true;
}

/* Bytes as hex pairs, optionally separated by spaces. */
static bool parse_bytes(const char *s, struct op *o)
{
  while (*s) {
    if (*s == ' ') {
      s++;
      continue;
    }
    int hi = hex_digit(s[0]), lo = s[0] ? hex_digit(s[1]) : -1;
    if (hi < 0 || lo < 0 || o->len >= sizeof o->data)
      return false;
    o->data[o->len++] = (uint8_t)(hi << 4 | lo);
    s += 2;
  }
  return o->len > 0;
}

/* "read|write|notify <handle> ..." for the text log; with ev, "<tag> read|write|writecmd|notify <handle> ..." for the
 * event queue, and "ack <seq>" to retire events up to seq. */
int gatt_command(const char *args, bool ev)
{
  struct op o;
  memset(&o, 0, sizeof o);
  if (ev && bt_has(args, "ack ")) {
    uint32_t seq;
    args += 4;
    if (!parse_uint(&args, &seq) || *args)
      return -1;
    int keep = 0;
    for (int i = 0; i < ev_count; i++)
      if (events[i].seq > seq)
        events[keep++] = events[i];
    ev_count = keep;
    return 0;
  }
  if (!g.c || g.stage != ST_READY)
    return -1;
  o.ev = ev;
  if (ev) {
    if (!parse_uint(&args, &o.tag) || *args++ != ' ')
      return -1;
  }
  if (bt_has(args, "read ")) {
    o.kind = OP_READ;
    args += 5;
  } else if (bt_has(args, "write ")) {
    o.kind = OP_WRITE;
    args += 6;
  } else if (ev && bt_has(args, "writecmd ")) {
    o.kind = OP_WRITE_CMD;
    args += 9;
  } else if (bt_has(args, "notify ")) {
    o.kind = OP_NOTIFY;
    args += 7;
  } else {
    return -1;
  }
  int handle = parse_hex(&args, 4);
  if (handle <= 0)
    return -1;
  o.handle = (uint16_t)handle;
  if (o.kind != OP_READ) {
    if (*args++ != ' ')
      return -1;
    if (o.kind == OP_NOTIFY) {
      o.data[0] = !strcmp(args, "on");
      if (!o.data[0] && strcmp(args, "off"))
        return -1;
    } else if (!parse_bytes(args, &o)) {
      return -1;
    }
  }
  int next = (g.ops_head + 1) % MAX_OPS;
  if (next == g.ops_tail)
    return -1;
  g.ops[g.ops_head] = o;
  g.ops_head = next;
  return 0;
}

/* A 16-bit UUID as four hex digits, a 128-bit one in the dashed form. */
void bt_uuid_text(char *out, int max, const uint8_t *v, int len)
{
  if (len == 2) {
    bt_snprintf(out, (uint32_t)max, "%04x", bt_get16(v));
    return;
  }
  int n = 0;
  for (int i = 15; i >= 0; i--)
    n += bt_snprintf(out + n, (uint32_t)(max - n), "%02x%s", v[i], i == 12 || i == 10 || i == 8 || i == 6 ? "-" : "");
}

int bt_gatt_text(char *buf, int max)
{
  int n = 0;
  char u[40];
  if (!g.c)
    return 0;
  n += bt_snprintf(buf + n, (uint32_t)(max - n), "state %s\n", g.stage == ST_READY ? "ready" : "discovering");
  for (int i = 0; i < g.nsvc && n < max; i++) {
    bt_uuid_text(u, sizeof u, g.svc[i].uuid.v, g.svc[i].uuid.len);
    n += bt_snprintf(buf + n, (uint32_t)(max - n), "service %s handles %04x-%04x\n", u, g.svc[i].start, g.svc[i].end);
    for (int k = 0; k < g.nchr && n < max; k++) {
      struct chr *c = &g.chr[k];
      if (c->handle < g.svc[i].start || c->handle > g.svc[i].end)
        continue;
      bt_uuid_text(u, sizeof u, c->uuid.v, c->uuid.len);
      n += bt_snprintf(buf + n, (uint32_t)(max - n), "  characteristic %s handle %04x props %02x\n", u, c->value,
                       c->props);
    }
  }
  int first = g.log_n > MAX_LOG ? g.log_n - MAX_LOG : 0;
  for (int i = first; i < g.log_n && n < max; i++)
    n += bt_snprintf(buf + n, (uint32_t)(max - n), "%s\n", g.log[i % MAX_LOG]);
  return n < max ? n : max;
}

/* Pending results and notifications, oldest first: "<seq> <tag> read|write|notify|err <handle> <hex>". Reading does not
 * consume them; "att ack <seq>" does. Sequence numbers restart with each connection. */
int bt_gatt_events_text(char *buf, int max)
{
  static const char *const kinds[] = {"read", "write", "notify", "err"};
  int n = 0;
  if (max > EVENTS_TEXT_MAX)
    max = EVENTS_TEXT_MAX;
  for (int i = 0; i < ev_count; i++) {
    const struct event *e = &events[i];
    if (n + 40 + 2 * e->len >= max)
      break;
    n += bt_snprintf(buf + n, (uint32_t)(max - n), "%u %u %s %04x ", e->seq, e->tag, kinds[e->kind], e->handle);
    for (int k = 0; k < e->len; k++)
      n += bt_snprintf(buf + n, (uint32_t)(max - n), "%02x", e->data[k]);
    buf[n++] = '\n';
  }
  return n;
}
