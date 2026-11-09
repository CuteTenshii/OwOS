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
#define ATT_READ_GROUP_REQ 0x10
#define ATT_READ_GROUP_RSP 0x11
#define ATT_WRITE_REQ 0x12
#define ATT_WRITE_RSP 0x13
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

enum { OP_READ, OP_WRITE, OP_NOTIFY };

struct op {
  uint8_t kind;
  uint16_t handle;
  uint8_t len;
  uint8_t data[20];
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
  uint8_t last[24], last_len;
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
    log_line("%04x: error %02x", g.cur.handle, d[3]);
    g.cur_active = g.cur_finding = false;
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
  uint8_t p[3 + 20];
  p[0] = ATT_WRITE_REQ;
  bt_put16(p + 1, handle);
  memcpy(p + 3, v, (uint32_t)len);
  att_send(p, 3 + len);
}

static void on_find_info_rsp(const uint8_t *d, int len)
{
  int el = d[1] == 1 ? 4 : 18;
  for (int i = 2; i + el <= len; i += el)
    if (d[1] == 1 && bt_get16(d + i + 2) == UUID_CCCD) {
      uint8_t v[2] = {g.cur.data[0] ? 1 : 0, 0};
      g.cur_finding = false;
      g.cur.handle = bt_get16(d + i);
      write_value(g.cur.handle, v, 2);
      return;
    }
  log_line("%04x: no notification descriptor", g.cur.handle);
  g.cur_active = g.cur_finding = false;
}

static void att_rx(struct l2cap_chan *ch, const uint8_t *d, int len)
{
  (void)ch;
  if (len < 1)
    return;
  if (d[0] == ATT_NOTIFY || d[0] == ATT_INDICATE) {
    if (len >= 3)
      value_log(d[0] == ATT_NOTIFY ? "notify" : "indicate", bt_get16(d + 1), d + 3, len - 3);
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
    if (g.cur_active) {
      value_log("read", g.cur.handle, d + 1, len - 1);
      g.cur_active = false;
    }
    break;
  case ATT_WRITE_RSP:
    if (g.cur_active) {
      log_line("%04x: written", g.cur.handle);
      g.cur_active = false;
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
  uint8_t p[5];
  if (o->kind == OP_READ) {
    p[0] = ATT_READ_REQ;
    bt_put16(p + 1, o->handle);
    att_send(p, 3);
  } else if (o->kind == OP_WRITE) {
    write_value(o->handle, o->data, o->len);
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

static int parse_hex(const char **s, unsigned max_digits)
{
  int v = 0;
  unsigned n = 0;
  while (**s && **s != ' ') {
    char c = **s;
    int d = c >= '0' && c <= '9'   ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                   : -1;
    if (d < 0 || ++n > max_digits)
      return -1;
    v = v * 16 + d;
    (*s)++;
  }
  return n ? v : -1;
}

int gatt_command(const char *args)
{
  struct op o;
  memset(&o, 0, sizeof o);
  if (!g.c || g.stage != ST_READY)
    return -1;
  if (bt_has(args, "read ")) {
    o.kind = OP_READ;
    args += 5;
  } else if (bt_has(args, "write ")) {
    o.kind = OP_WRITE;
    args += 6;
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
    } else {
      while (*args) {
        const char *hs = args;
        int hi = parse_hex(&args, 2);
        if (hi < 0 || o.len >= sizeof o.data || args == hs)
          return -1;
        o.data[o.len++] = (uint8_t)hi;
        if (*args == ' ')
          args++;
      }
      if (!o.len)
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

static void uuid_text(char *out, int max, const struct uuid *u)
{
  if (u->len == 2) {
    bt_snprintf(out, (uint32_t)max, "%04x", bt_get16(u->v));
    return;
  }
  int n = 0;
  for (int i = 15; i >= 0; i--)
    n +=
        bt_snprintf(out + n, (uint32_t)(max - n), "%02x%s", u->v[i], i == 12 || i == 10 || i == 8 || i == 6 ? "-" : "");
}

int bt_gatt_text(char *buf, int max)
{
  int n = 0;
  char u[40];
  if (!g.c)
    return 0;
  for (int i = 0; i < g.nsvc && n < max; i++) {
    uuid_text(u, sizeof u, &g.svc[i].uuid);
    n += bt_snprintf(buf + n, (uint32_t)(max - n), "service %s handles %04x-%04x\n", u, g.svc[i].start, g.svc[i].end);
    for (int k = 0; k < g.nchr && n < max; k++) {
      struct chr *c = &g.chr[k];
      if (c->handle < g.svc[i].start || c->handle > g.svc[i].end)
        continue;
      uuid_text(u, sizeof u, &c->uuid);
      n += bt_snprintf(buf + n, (uint32_t)(max - n), "  characteristic %s handle %04x props %02x\n", u, c->value,
                       c->props);
    }
  }
  int first = g.log_n > MAX_LOG ? g.log_n - MAX_LOG : 0;
  for (int i = first; i < g.log_n && n < max; i++)
    n += bt_snprintf(buf + n, (uint32_t)(max - n), "%s\n", g.log[i % MAX_LOG]);
  return n < max ? n : max;
}
