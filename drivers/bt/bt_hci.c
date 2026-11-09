/* bt_hci.c: HCI host: controller bring-up, command flow, inquiry and LE scan, connections, pairing and ACL flow. */

#include "bt_int.h"

#if !__STDC_HOSTED__
#include "entropy.h"
#include "log.h"
#include "sched.h"
#endif

#define OP(ogf, ocf) ((uint16_t)((ogf) << 10 | (ocf)))
#define OP_INQUIRY OP(1, 0x01)
#define OP_INQUIRY_CANCEL OP(1, 0x02)
#define OP_CREATE_CONN OP(1, 0x05)
#define OP_DISCONNECT OP(1, 0x06)
#define OP_REJECT_CONN OP(1, 0x0A)
#define OP_LINK_KEY_REPLY OP(1, 0x0B)
#define OP_LINK_KEY_NEG OP(1, 0x0C)
#define OP_PIN_NEG OP(1, 0x0E)
#define OP_AUTH_REQ OP(1, 0x11)
#define OP_SET_ENCRYPT OP(1, 0x13)
#define OP_REMOTE_NAME OP(1, 0x19)
#define OP_IO_CAP_REPLY OP(1, 0x2B)
#define OP_USER_CONFIRM OP(1, 0x2C)
#define OP_SET_EVENT_MASK OP(3, 0x01)
#define OP_RESET OP(3, 0x03)
#define OP_WRITE_LOCAL_NAME OP(3, 0x13)
#define OP_WRITE_SCAN_ENABLE OP(3, 0x1A)
#define OP_WRITE_COD OP(3, 0x24)
#define OP_WRITE_INQUIRY_MODE OP(3, 0x45)
#define OP_WRITE_SSP OP(3, 0x56)
#define OP_WRITE_LE_HOST OP(3, 0x6D)
#define OP_READ_BUFFER_SIZE OP(4, 0x05)
#define OP_READ_BD_ADDR OP(4, 0x09)
#define OP_LE_SET_EVENT_MASK OP(8, 0x01)
#define OP_LE_READ_BUFFER_SIZE OP(8, 0x02)
#define OP_LE_SET_SCAN_PARAMS OP(8, 0x0B)
#define OP_LE_SET_SCAN_ENABLE OP(8, 0x0C)
#define OP_LE_CREATE_CONN OP(8, 0x0D)
#define OP_LE_CREATE_CONN_CANCEL OP(8, 0x0E)

#define EVT_INQUIRY_COMPLETE 0x01
#define EVT_INQUIRY_RESULT 0x02
#define EVT_CONN_COMPLETE 0x03
#define EVT_CONN_REQUEST 0x04
#define EVT_DISCONN_COMPLETE 0x05
#define EVT_AUTH_COMPLETE 0x06
#define EVT_REMOTE_NAME 0x07
#define EVT_ENCRYPT_CHANGE 0x08
#define EVT_CMD_COMPLETE 0x0E
#define EVT_CMD_STATUS 0x0F
#define EVT_NUM_COMPLETED 0x13
#define EVT_PIN_REQUEST 0x16
#define EVT_LINK_KEY_REQUEST 0x17
#define EVT_LINK_KEY_NOTIFY 0x18
#define EVT_INQUIRY_RESULT_RSSI 0x22
#define EVT_EXT_INQUIRY_RESULT 0x2F
#define EVT_IO_CAP_REQUEST 0x31
#define EVT_USER_CONFIRM_REQUEST 0x33
#define EVT_SSP_COMPLETE 0x36
#define EVT_LE_META 0x3E

#define LE_CONN_COMPLETE 0x01
#define LE_ADV_REPORT 0x02
#define LE_P256_KEY_COMPLETE 0x08
#define LE_DHKEY_COMPLETE 0x09
#define LE_ENH_CONN_COMPLETE 0x0A

#define COD_MAJOR_PERIPHERAL 5

#define CMDQ 16
#define RXQ 16
#define TXQ 32
#define ACL_PAYLOAD_MAX 1024
#define CMD_TIMEOUT_MS 3000
#define MAX_KEYS 8
#define USERQ 8
#define USERQ_LINE 800

struct cmd {
  uint16_t op;
  uint8_t len;
  uint8_t params[248];
};

struct rxpkt {
  uint8_t type;
  uint16_t len;
  uint8_t data[4 + ACL_PAYLOAD_MAX];
};

struct txpkt {
  uint16_t len;
  bool le;
  uint8_t data[4 + ACL_PAYLOAD_MAX];
};

struct key {
  bool used;
  uint8_t addr[6], key[16];
};

enum { ST_OFF, ST_INIT, ST_READY, ST_FAILED };

static struct {
  struct bt_transport t;
  bool attached;
  int state, init_step;
  uint32_t now;
  uint8_t bdaddr[6];
  uint16_t acl_mtu, le_mtu, acl_pkts, le_pkts;
  int credits[2], max_credits[2];

  struct cmd cq[CMDQ];
  int cq_head, cq_tail;
  bool busy;
  uint16_t inflight;
  uint32_t sent_at;

  struct rxpkt *rx;
  volatile int rx_head, rx_tail;
  struct txpkt *tx;
  int tx_head, tx_tail;

  struct bt_dev devs[BT_MAX_DEV];
  bool scan_br, scan_le, scan_want, name_busy;
  struct bt_conn conns[BT_MAX_CONN];
  struct key keys[MAX_KEYS];

  struct {
    bool pending, le, issued;
    uint8_t addr[6], atype;
  } cn;
  uint8_t pairing_addr[6];

  char user[USERQ][USERQ_LINE];
  volatile int user_head, user_tail;
} h;

/* ---- platform */

void *bt_alloc(uint32_t n)
{
#if __STDC_HOSTED__
  return calloc(1, n);
#else
  void *p = kmalloc(n);
  if (p)
    memset(p, 0, n);
  return p;
#endif
}

void bt_free(void *p)
{
#if __STDC_HOSTED__
  free(p);
#else
  kfree(p);
#endif
}

void bt_log(const char *fmt, ...)
{
  char b[128];
  va_list ap;
  va_start(ap, fmt);
#if __STDC_HOSTED__
  vsnprintf(b, sizeof b - 1, fmt, ap);
  if (getenv("BT_DEBUG"))
    fprintf(stderr, "bt: %s\n", b);
#else
  kvsnprintf(b, sizeof b - 2, fmt, ap);
  klog("bt: ");
  klog(b);
  klog("\n");
#endif
  va_end(ap);
}

uint32_t bt_now(void) { return h.now; }

void bt_random(uint8_t *buf, int n)
{
#if __STDC_HOSTED__
  FILE *f = fopen("/dev/urandom", "rb");
  if (f) {
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
      memset(buf, 0, (size_t)n);
    fclose(f);
  }
#else
  entropy_get(buf, (uint32_t)n);
#endif
}

void (*bt_host_yield)(void);

void bt_yield(void)
{
#if __STDC_HOSTED__
  if (bt_host_yield)
    bt_host_yield();
#else
  __asm__ volatile("sti");
  sched_yield();
#endif
}

uintptr_t bt_irq_save(void)
{
#if __STDC_HOSTED__
  return 0;
#else
  uintptr_t f;
  __asm__ volatile("pushf; pop %0; cli" : "=r"(f)::"memory");
  return f;
#endif
}

void bt_irq_restore(uintptr_t f)
{
#if __STDC_HOSTED__
  (void)f;
#else
  __asm__ volatile("push %0; popf" ::"r"(f) : "memory", "cc");
#endif
}

/* ---- commands */

bool hci_cmd(uint16_t op, const void *params, int len)
{
  int next = (h.cq_head + 1) % CMDQ;
  if (next == h.cq_tail || len > (int)sizeof h.cq[0].params)
    return false;
  struct cmd *c = &h.cq[h.cq_head];
  c->op = op;
  c->len = (uint8_t)len;
  if (len)
    memcpy(c->params, params, (uint32_t)len);
  h.cq_head = next;
  return true;
}

static void cmd_pump(void)
{
  if (h.busy || h.cq_head == h.cq_tail)
    return;
  struct cmd *c = &h.cq[h.cq_tail];
  uint8_t pkt[3 + sizeof c->params];
  bt_put16(pkt, c->op);
  pkt[2] = c->len;
  memcpy(pkt + 3, c->params, c->len);
  h.cq_tail = (h.cq_tail + 1) % CMDQ;
  h.busy = true;
  h.inflight = c->op;
  h.sent_at = h.now;
  if (h.t.send(h.t.ctx, BT_PKT_CMD, pkt, 3 + pkt[2]) != 0) {
    bt_log("command %04x send failed", h.inflight);
    h.busy = false;
  }
}

/* ---- devices */

static struct bt_dev *dev_find(const uint8_t *addr, uint8_t type)
{
  for (int i = 0; i < BT_MAX_DEV; i++)
    if (h.devs[i].used && !memcmp(h.devs[i].addr, addr, 6) &&
        (h.devs[i].addr_type == BT_ADDR_BR) == (type == BT_ADDR_BR))
      return &h.devs[i];
  return NULL;
}

static struct bt_dev *dev_get(const uint8_t *addr, uint8_t type)
{
  struct bt_dev *d = dev_find(addr, type);
  if (!d) {
    for (int i = 0; i < BT_MAX_DEV && !d; i++)
      if (!h.devs[i].used)
        d = &h.devs[i];
    if (!d)
      return NULL;
    memset(d, 0, sizeof *d);
    d->used = true;
    memcpy(d->addr, addr, 6);
    d->addr_type = type;
    d->le = type != BT_ADDR_BR;
  }
  return d;
}

static void dev_set_name(struct bt_dev *d, const uint8_t *s, int len)
{
  if (len > BT_NAME_LEN - 1)
    len = BT_NAME_LEN - 1;
  int n = 0;
  for (int i = 0; i < len && s[i]; i++)
    d->name[n++] = s[i] >= 32 && s[i] < 127 ? (char)s[i] : '?';
  d->name[n] = 0;
}

static void dev_add_uuid(struct bt_dev *d, const uint8_t *v, int len)
{
  for (int i = 0; i < d->nuuid; i++)
    if (d->uuids[i].len == len && !memcmp(d->uuids[i].v, v, (uint32_t)len))
      return;
  if (d->nuuid < BT_DEV_UUIDS) {
    d->uuids[d->nuuid].len = (uint8_t)len;
    memcpy(d->uuids[d->nuuid++].v, v, (uint32_t)len);
  }
}

/* Local name (0x09 complete, 0x08 shortened) and service UUIDs (0x02/0x03 16-bit, 0x06/0x07 128-bit) out of EIR or
 * advertising data. */
static void parse_ad(struct bt_dev *d, const uint8_t *ad, int len)
{
  for (int i = 0; i + 1 < len && ad[i];) {
    int l = ad[i];
    if (i + 1 + l > len)
      break;
    if ((ad[i + 1] == 0x09 || (ad[i + 1] == 0x08 && !d->name[0])) && l > 1)
      dev_set_name(d, ad + i + 2, l - 1);
    else if (ad[i + 1] == 0x02 || ad[i + 1] == 0x03)
      for (int k = 0; k + 2 <= l - 1; k += 2)
        dev_add_uuid(d, ad + i + 2 + k, 2);
    else if (ad[i + 1] == 0x06 || ad[i + 1] == 0x07)
      for (int k = 0; k + 16 <= l - 1; k += 16)
        dev_add_uuid(d, ad + i + 2 + k, 16);
    i += 1 + l;
  }
}

static void inquiry_result(const uint8_t *p, int len, int stride, int rssi_off)
{
  int n = p[0];
  for (int i = 0; i < n && 1 + (i + 1) * stride <= len; i++) {
    const uint8_t *r = p + 1 + i * stride;
    struct bt_dev *d = dev_get(r, BT_ADDR_BR);
    if (!d)
      return;
    d->cod = r[9] | r[10] << 8 | r[11] << 16;
    if (rssi_off)
      d->rssi = (int8_t)r[rssi_off];
  }
}

static const char *addr_str(const uint8_t *a, char out[18])
{
  bt_snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
  return out;
}

static bool addr_parse(const char *s, uint8_t addr[6])
{
  for (int i = 5; i >= 0; i--) {
    int v = 0;
    for (int k = 0; k < 2; k++, s++) {
      char c = *s;
      int d = c >= '0' && c <= '9'   ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                     : -1;
      if (d < 0)
        return false;
      v = v * 16 + d;
    }
    addr[i] = (uint8_t)v;
    if (i && *s++ != ':')
      return false;
  }
  return *s == 0;
}

/* ---- connections */

struct bt_conn *hci_conn_by_handle(uint16_t handle)
{
  for (int i = 0; i < BT_MAX_CONN; i++)
    if (h.conns[i].used && h.conns[i].handle == handle)
      return &h.conns[i];
  return NULL;
}

static struct bt_conn *conn_by_addr(const uint8_t *addr)
{
  for (int i = 0; i < BT_MAX_CONN; i++)
    if (h.conns[i].used && !memcmp(h.conns[i].addr, addr, 6))
      return &h.conns[i];
  return NULL;
}

struct bt_conn *hci_connected_br(void)
{
  for (int i = 0; i < BT_MAX_CONN; i++)
    if (h.conns[i].used && !h.conns[i].le)
      return &h.conns[i];
  return NULL;
}

struct bt_conn *hci_connected_le(void)
{
  for (int i = 0; i < BT_MAX_CONN; i++)
    if (h.conns[i].used && h.conns[i].le)
      return &h.conns[i];
  return NULL;
}

static struct bt_conn *conn_add(uint16_t handle, const uint8_t *addr, uint8_t atype, bool le)
{
  for (int i = 0; i < BT_MAX_CONN; i++) {
    struct bt_conn *c = &h.conns[i];
    if (c->used)
      continue;
    uint8_t *rx = c->rx ? c->rx : bt_alloc(BT_RX_MAX);
    if (!rx)
      return NULL;
    memset(c, 0, sizeof *c);
    c->rx = rx;
    c->used = true;
    c->handle = handle;
    c->le = le;
    c->addr_type = atype;
    memcpy(c->addr, addr, 6);
    return c;
  }
  return NULL;
}

void hci_disconnect(struct bt_conn *c)
{
  uint8_t p[3];
  bt_put16(p, c->handle);
  p[2] = 0x13;
  hci_cmd(OP_DISCONNECT, p, 3);
}

bool hci_find_key(const uint8_t *addr, uint8_t key[16])
{
  for (int i = 0; i < MAX_KEYS; i++)
    if (h.keys[i].used && !memcmp(h.keys[i].addr, addr, 6)) {
      memcpy(key, h.keys[i].key, 16);
      return true;
    }
  return false;
}

const uint8_t *hci_local_addr(void) { return h.bdaddr; }

uint32_t hci_dev_cod(const uint8_t *addr)
{
  for (int i = 0; i < BT_MAX_DEV; i++)
    if (h.devs[i].used && !h.devs[i].le && !memcmp(h.devs[i].addr, addr, 6))
      return h.devs[i].cod;
  return 0;
}

const char *hci_dev_name(const uint8_t *addr)
{
  for (int i = 0; i < BT_MAX_DEV; i++)
    if (h.devs[i].used && !h.devs[i].le && !memcmp(h.devs[i].addr, addr, 6))
      return h.devs[i].name;
  return "";
}

static void key_store(const uint8_t *addr, const uint8_t *key)
{
  int slot = -1;
  for (int i = 0; i < MAX_KEYS; i++) {
    if (h.keys[i].used && !memcmp(h.keys[i].addr, addr, 6)) {
      slot = i;
      break;
    }
    if (!h.keys[i].used && slot < 0)
      slot = i;
  }
  if (slot < 0)
    slot = 0;
  h.keys[slot].used = true;
  memcpy(h.keys[slot].addr, addr, 6);
  memcpy(h.keys[slot].key, key, 16);
}

static void key_drop(const uint8_t *addr)
{
  for (int i = 0; i < MAX_KEYS; i++)
    if (h.keys[i].used && !memcmp(h.keys[i].addr, addr, 6))
      h.keys[i].used = false;
}

static void conn_up(struct bt_conn *c)
{
  char a[18];
  bt_log("%s connection %04x to %s", c->le ? "le" : "br", c->handle, addr_str(c->addr, a));
  l2cap_conn_up(c);
  if (c->le) {
    gatt_conn_up(c);
    smp_conn_up(c);
  }
}

static void conn_down(struct bt_conn *c)
{
  avdtp_conn_down(c);
  bthid_conn_down(c);
  gatt_conn_down(c);
  smp_conn_down(c);
  l2cap_conn_down(c);
  c->used = false;
}

/* ---- ACL */

static int pool_of(bool le) { return le && h.le_pkts ? 1 : 0; }

static int tx_free(void) { return (h.tx_tail - h.tx_head - 1 + TXQ) % TXQ; }

bool hci_acl_room(const struct bt_conn *c, int frags)
{
  (void)c;
  return tx_free() >= frags;
}

int hci_acl_send(struct bt_conn *c, const uint8_t *data, int len)
{
  int mtu = c->le && h.le_pkts ? h.le_mtu : h.acl_mtu;
  if (mtu <= 0 || mtu > ACL_PAYLOAD_MAX)
    mtu = mtu <= 0 ? 27 : ACL_PAYLOAD_MAX;
  int frags = (len + mtu - 1) / mtu;
  if (tx_free() < frags)
    return -1;
  for (int off = 0, first = 1; off < len; off += mtu, first = 0) {
    int n = len - off < mtu ? len - off : mtu;
    struct txpkt *t = &h.tx[h.tx_head];
    bt_put16(t->data, (uint16_t)(c->handle | (first ? 0 : 0x1000)));
    bt_put16(t->data + 2, (uint16_t)n);
    memcpy(t->data + 4, data + off, (uint32_t)n);
    t->len = (uint16_t)(4 + n);
    t->le = c->le;
    h.tx_head = (h.tx_head + 1) % TXQ;
  }
  return 0;
}

static void tx_pump(void)
{
  while (h.tx_tail != h.tx_head) {
    struct txpkt *t = &h.tx[h.tx_tail];
    int pool = pool_of(t->le);
    if (h.credits[pool] <= 0)
      return;
    h.credits[pool]--;
    h.tx_tail = (h.tx_tail + 1) % TXQ;
    if (h.t.send(h.t.ctx, BT_PKT_ACL, t->data, t->len) != 0)
      h.credits[pool]++;
  }
}

/* ---- init */

static const uint8_t cod_computer[3] = {0x04, 0x01, 0x00};
static const uint8_t event_mask[8] = {0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D};
static const uint8_t le_event_mask[8] = {0x9F, 0x01};

static void init_step(void)
{
  uint8_t b[248];
  switch (h.init_step++) {
  case 0:
    hci_cmd(OP_RESET, NULL, 0);
    break;
  case 1:
    hci_cmd(OP_READ_BD_ADDR, NULL, 0);
    break;
  case 2:
    hci_cmd(OP_READ_BUFFER_SIZE, NULL, 0);
    break;
  case 3:
    hci_cmd(OP_LE_READ_BUFFER_SIZE, NULL, 0);
    break;
  case 4:
    hci_cmd(OP_SET_EVENT_MASK, event_mask, 8);
    break;
  case 5:
    hci_cmd(OP_LE_SET_EVENT_MASK, le_event_mask, 8);
    break;
  case 6:
    b[0] = 1;
    b[1] = 0;
    hci_cmd(OP_WRITE_LE_HOST, b, 2);
    break;
  case 7:
    b[0] = 1;
    hci_cmd(OP_WRITE_SSP, b, 1);
    break;
  case 8:
    b[0] = 2;
    hci_cmd(OP_WRITE_INQUIRY_MODE, b, 1);
    break;
  case 9:
    hci_cmd(OP_WRITE_COD, cod_computer, 3);
    break;
  case 10:
    memset(b, 0, sizeof b);
    memcpy(b, "OwOS", 4);
    hci_cmd(OP_WRITE_LOCAL_NAME, b, 248);
    break;
  case 11:
    b[0] = 0;
    hci_cmd(OP_WRITE_SCAN_ENABLE, b, 1);
    break;
  default:
    h.state = ST_READY;
    h.credits[0] = h.max_credits[0] = h.acl_pkts;
    h.credits[1] = h.max_credits[1] = h.le_pkts;
    {
      char a[18];
      bt_log("controller ready, address %s, acl %ux%u le %ux%u", addr_str(h.bdaddr, a), h.acl_pkts, h.acl_mtu,
             h.le_pkts, h.le_mtu);
    }
  }
}

/* LE commands are optional: controllers without LE reject them and bring-up goes on. */
static bool init_optional(uint16_t op)
{
  return op == OP_LE_READ_BUFFER_SIZE || op == OP_LE_SET_EVENT_MASK || op == OP_WRITE_LE_HOST;
}

static void cmd_done(uint16_t op, uint8_t status, const uint8_t *ret, int rlen)
{
  if (op == OP_READ_BD_ADDR && !status && rlen >= 7)
    memcpy(h.bdaddr, ret + 1, 6);
  else if (op == OP_READ_BUFFER_SIZE && !status && rlen >= 8) {
    h.acl_mtu = bt_get16(ret + 1);
    h.acl_pkts = bt_get16(ret + 4);
  } else if (op == OP_LE_READ_BUFFER_SIZE && !status && rlen >= 4) {
    h.le_mtu = bt_get16(ret + 1);
    h.le_pkts = ret[3];
  }

  if (h.state == ST_INIT) {
    if (status && !init_optional(op)) {
      bt_log("init command %04x failed: status %02x", op, status);
      h.state = ST_FAILED;
    } else {
      init_step();
    }
    return;
  }
  if (status)
    smp_cmd_failed(op, status);
  if (status && (op == OP_CREATE_CONN || op == OP_LE_CREATE_CONN)) {
    bt_log("connect failed: status %02x", status);
    h.cn.pending = false;
  }
  if (op == OP_REMOTE_NAME && status)
    h.name_busy = false;
}

/* ---- events */

static void scan_le_enable(bool on)
{
  uint8_t p[2] = {on, 0};
  hci_cmd(OP_LE_SET_SCAN_ENABLE, p, 2);
  h.scan_le = on;
}

static void scan_start(void)
{
  static const uint8_t inq[5] = {0x33, 0x8B, 0x9E, 8, 0};
  uint8_t sp[7] = {1, 0x60, 0x00, 0x30, 0x00, 0, 0};
  hci_cmd(OP_INQUIRY, inq, 5);
  h.scan_br = true;
  hci_cmd(OP_LE_SET_SCAN_PARAMS, sp, 7);
  scan_le_enable(true);
}

static void scan_stop(void)
{
  if (h.scan_br)
    hci_cmd(OP_INQUIRY_CANCEL, NULL, 0);
  if (h.scan_le)
    scan_le_enable(false);
  h.scan_br = false;
}

static void on_conn_complete(const uint8_t *p, int len)
{
  if (len < 11)
    return;
  uint8_t status = p[0];
  h.cn.pending = false;
  if (status) {
    bt_log("br connection failed: status %02x", status);
    return;
  }
  struct bt_conn *c = conn_add(bt_get16(p + 1), p + 3, BT_ADDR_BR, false);
  if (!c)
    return;
  conn_up(c);
  uint8_t hd[2];
  bt_put16(hd, c->handle);
  hci_cmd(OP_AUTH_REQ, hd, 2);
}

static void on_le_meta(const uint8_t *p, int len)
{
  if (len < 1)
    return;
  if (p[0] == LE_CONN_COMPLETE || p[0] == LE_ENH_CONN_COMPLETE) {
    if (len < 12)
      return;
    h.cn.pending = false;
    if (p[1]) {
      bt_log("le connection failed: status %02x", p[1]);
      return;
    }
    struct bt_conn *c = conn_add(bt_get16(p + 2), p + 6, p[5], true);
    if (c)
      conn_up(c);
  } else if (p[0] == LE_P256_KEY_COMPLETE && len >= 2) {
    smp_p256_key(p[1], len >= 66 ? p + 2 : NULL);
  } else if (p[0] == LE_DHKEY_COMPLETE && len >= 2) {
    smp_dhkey(p[1], len >= 34 ? p + 2 : NULL);
  } else if (p[0] == LE_ADV_REPORT) {
    int n = len > 1 ? p[1] : 0, off = 2;
    for (int i = 0; i < n && off + 9 <= len; i++) {
      const uint8_t *r = p + off;
      int dl = r[8];
      if (off + 9 + dl + 1 > len)
        break;
      struct bt_dev *d = dev_get(r + 2, r[1]);
      if (d) {
        d->rssi = (int8_t)r[9 + dl];
        parse_ad(d, r + 9, dl);
      }
      off += 9 + dl + 1;
    }
  }
}

static void on_event(const uint8_t *p, int len)
{
  if (len < 2)
    return;
  int plen = p[1] < len - 2 ? p[1] : len - 2;
  const uint8_t *q = p + 2;
  uint8_t reply[24];

  switch (p[0]) {
  case EVT_CMD_COMPLETE:
    if (plen >= 3) {
      uint16_t op = bt_get16(q + 1);
      if (op == h.inflight)
        h.busy = false;
      if (op)
        cmd_done(op, plen > 3 ? q[3] : 0, q + 3, plen - 3);
    }
    break;
  case EVT_CMD_STATUS:
    if (plen >= 4) {
      uint16_t op = bt_get16(q + 2);
      if (op == h.inflight)
        h.busy = false;
      if (q[0])
        cmd_done(op, q[0], NULL, 0);
    }
    break;
  case EVT_INQUIRY_COMPLETE:
    h.scan_br = false;
    if (h.scan_le)
      scan_le_enable(false);
    h.scan_want = false;
    break;
  case EVT_INQUIRY_RESULT:
    if (plen >= 1)
      inquiry_result(q, plen, 14, 0);
    break;
  case EVT_INQUIRY_RESULT_RSSI:
    if (plen >= 1)
      inquiry_result(q, plen, 14, 13);
    break;
  case EVT_EXT_INQUIRY_RESULT:
    if (plen >= 15) {
      struct bt_dev *d = dev_get(q + 1, BT_ADDR_BR);
      if (d) {
        d->cod = q[9] | q[10] << 8 | q[11] << 16;
        d->rssi = (int8_t)q[14];
        parse_ad(d, q + 15, plen - 15);
      }
    }
    break;
  case EVT_REMOTE_NAME:
    h.name_busy = false;
    if (plen >= 7 && !q[0]) {
      struct bt_dev *d = dev_find(q + 1, BT_ADDR_BR);
      if (d)
        dev_set_name(d, q + 7, plen - 7);
    }
    break;
  case EVT_CONN_COMPLETE:
    on_conn_complete(q, plen);
    break;
  case EVT_CONN_REQUEST:
    if (plen >= 6) {
      memcpy(reply, q, 6);
      reply[6] = 0x0F;
      hci_cmd(OP_REJECT_CONN, reply, 7);
    }
    break;
  case EVT_DISCONN_COMPLETE:
    if (plen >= 4 && !q[0]) {
      struct bt_conn *c = hci_conn_by_handle(bt_get16(q + 1));
      if (c) {
        bt_log("connection %04x closed, reason %02x", c->handle, q[3]);
        conn_down(c);
      }
    }
    break;
  case EVT_AUTH_COMPLETE:
    if (plen >= 3) {
      struct bt_conn *c = hci_conn_by_handle(bt_get16(q + 1));
      if (!c)
        break;
      if (q[0]) {
        bt_log("authentication failed: status %02x", q[0]);
        key_drop(c->addr);
        hci_disconnect(c);
      } else {
        uint8_t e[3];
        bt_put16(e, c->handle);
        e[2] = 1;
        hci_cmd(OP_SET_ENCRYPT, e, 3);
      }
    }
    break;
  case EVT_ENCRYPT_CHANGE:
    if (plen >= 4) {
      struct bt_conn *c = hci_conn_by_handle(bt_get16(q + 1));
      if (c && c->le) {
        smp_enc_change(c, q[0], q[3]);
      } else if (c && !q[0] && q[3] && !c->secure) {
        c->secure = true;
        if (((hci_dev_cod(c->addr) >> 8) & 0x1F) == COD_MAJOR_PERIPHERAL)
          bthid_conn_ready(c);
        else
          avdtp_conn_ready(c);
      } else if (c && q[0]) {
        hci_disconnect(c);
      }
    }
    break;
  case EVT_NUM_COMPLETED:
    if (plen >= 1) {
      for (int i = 0; i < q[0] && 1 + 2 * q[0] + 2 * (i + 1) <= plen; i++) {
        struct bt_conn *c = hci_conn_by_handle(bt_get16(q + 1 + 2 * i));
        int pool = pool_of(c && c->le);
        h.credits[pool] += bt_get16(q + 1 + 2 * q[0] + 2 * i);
        if (h.credits[pool] > h.max_credits[pool])
          h.credits[pool] = h.max_credits[pool];
      }
    }
    break;
  case EVT_PIN_REQUEST:
    if (plen >= 6)
      hci_cmd(OP_PIN_NEG, q, 6);
    break;
  case EVT_LINK_KEY_REQUEST:
    if (plen >= 6) {
      memcpy(reply, q, 6);
      if (hci_find_key(q, reply + 6))
        hci_cmd(OP_LINK_KEY_REPLY, reply, 22);
      else
        hci_cmd(OP_LINK_KEY_NEG, q, 6);
    }
    break;
  case EVT_LINK_KEY_NOTIFY:
    if (plen >= 22)
      key_store(q, q + 6);
    break;
  case EVT_IO_CAP_REQUEST:
    if (plen >= 6) {
      memcpy(reply, q, 6);
      reply[6] = 3; /* NoInputNoOutput */
      reply[7] = 0;
      reply[8] = 0x04; /* general bonding */
      hci_cmd(OP_IO_CAP_REPLY, reply, 9);
    }
    break;
  case EVT_USER_CONFIRM_REQUEST:
    if (plen >= 6)
      hci_cmd(OP_USER_CONFIRM, q, 6);
    break;
  case EVT_SSP_COMPLETE:
    if (plen >= 1 && q[0])
      bt_log("pairing failed: status %02x", q[0]);
    break;
  case EVT_LE_META:
    on_le_meta(q, plen);
    break;
  }
}

/* ---- requests */

static void connect_pump(void)
{
  if (!h.cn.pending || h.cn.issued || h.state != ST_READY)
    return;
  if (h.scan_br || h.scan_le) {
    scan_stop();
    return;
  }
  if (h.busy || h.cq_head != h.cq_tail)
    return;
  h.cn.issued = true;
  if (h.cn.le) {
    uint8_t p[25] = {0x60, 0x00, 0x30, 0x00, 0};
    p[5] = h.cn.atype;
    memcpy(p + 6, h.cn.addr, 6);
    p[12] = 0;
    bt_put16(p + 13, 0x18);
    bt_put16(p + 15, 0x28);
    bt_put16(p + 17, 0);
    bt_put16(p + 19, 0xC8);
    bt_put16(p + 21, 0);
    bt_put16(p + 23, 0);
    hci_cmd(OP_LE_CREATE_CONN, p, 25);
  } else {
    uint8_t p[13];
    memcpy(p, h.cn.addr, 6);
    bt_put16(p + 6, 0xCC18);
    p[8] = 1;
    p[9] = 0;
    bt_put16(p + 10, 0);
    p[12] = 1;
    hci_cmd(OP_CREATE_CONN, p, 13);
  }
}

static void name_pump(void)
{
  if (h.name_busy || h.scan_br || h.cn.pending || h.state != ST_READY || h.busy)
    return;
  for (int i = 0; i < BT_MAX_DEV; i++) {
    struct bt_dev *d = &h.devs[i];
    if (d->used && !d->le && !d->name[0] && !d->name_tried) {
      uint8_t p[10];
      d->name_tried = true;
      memcpy(p, d->addr, 6);
      p[6] = 1;
      p[7] = 0;
      bt_put16(p + 8, 0);
      h.name_busy = hci_cmd(OP_REMOTE_NAME, p, 10);
      return;
    }
  }
}

static int run_command(const char *line)
{
  if (h.state != ST_READY)
    return -1;
  if (bt_has(line, "scan on")) {
    if (!h.scan_br && !h.scan_le && !h.cn.pending) {
      for (int i = 0; i < BT_MAX_DEV; i++)
        if (!h.devs[i].used || !conn_by_addr(h.devs[i].addr))
          h.devs[i].used = false;
      scan_start();
    }
    return 0;
  }
  if (bt_has(line, "scan off")) {
    scan_stop();
    return 0;
  }
  if (bt_has(line, "connect ")) {
    uint8_t a[6];
    if (!addr_parse(line + 8, a) || h.cn.pending)
      return -1;
    struct bt_dev *d = dev_find(a, BT_ADDR_BR);
    if (!d)
      for (int i = 0; i < BT_MAX_DEV; i++)
        if (h.devs[i].used && !memcmp(h.devs[i].addr, a, 6))
          d = &h.devs[i];
    if (!d)
      return -1;
    memcpy(h.cn.addr, a, 6);
    h.cn.le = d->le;
    h.cn.atype = d->addr_type == BT_ADDR_BR ? 0 : d->addr_type;
    h.cn.pending = true;
    h.cn.issued = false;
    return 0;
  }
  if (bt_has(line, "disconnect")) {
    for (int i = 0; i < BT_MAX_CONN; i++)
      if (h.conns[i].used)
        hci_disconnect(&h.conns[i]);
    return 0;
  }
  if (bt_has(line, "gatt "))
    return gatt_command(line + 5, false);
  if (bt_has(line, "att "))
    return gatt_command(line + 4, true);
  if (bt_has(line, "unpair ")) {
    uint8_t a[6];
    if (!addr_parse(line + 7, a))
      return -1;
    bool known = smp_unbond(a);
    for (int i = 0; i < MAX_KEYS; i++)
      if (h.keys[i].used && !memcmp(h.keys[i].addr, a, 6))
        known = true;
    struct bt_conn *c = conn_by_addr(a);
    if (!known && !c)
      return -1;
    key_drop(a);
    if (c)
      hci_disconnect(c);
    return 0;
  }
  if (!strcmp(line, "pair")) {
    struct bt_conn *c = hci_connected_le();
    return c ? smp_start(c) : -1;
  }
  return -1;
}

int bt_command(const char *line)
{
  if (!h.attached || !line)
    return -1;
  int n = (int)strlen(line);
  if (n == 0 || n >= USERQ_LINE)
    return -1;
  uintptr_t f = bt_irq_save();
  int next = (h.user_head + 1) % USERQ;
  int rc = -1;
  if (next != h.user_tail) {
    memcpy(h.user[h.user_head], line, (uint32_t)n + 1);
    h.user_head = next;
    rc = 0;
  }
  bt_irq_restore(f);
  return rc;
}

/* ---- transport side */

void bt_attach(const struct bt_transport *t)
{
  if (h.attached)
    return;
  memset(&h, 0, sizeof h);
  h.rx = bt_alloc(RXQ * sizeof *h.rx);
  h.tx = bt_alloc(TXQ * sizeof *h.tx);
  if (!h.rx || !h.tx) {
    bt_free(h.rx);
    bt_free(h.tx);
    memset(&h, 0, sizeof h);
    return;
  }
  h.t = *t;
  h.attached = true;
  h.state = ST_INIT;
  init_step();
}

void bt_detach(void)
{
  if (!h.attached)
    return;
  h.attached = false;
  for (int i = 0; i < BT_MAX_CONN; i++) {
    if (h.conns[i].used)
      conn_down(&h.conns[i]);
    bt_free(h.conns[i].rx);
  }
  smp_reset();
  bt_free(h.rx);
  bt_free(h.tx);
  memset(&h, 0, sizeof h);
}

void bt_rx(int type, const uint8_t *data, int len)
{
  if (!h.attached || len <= 0 || len > (int)sizeof h.rx[0].data)
    return;
  int next = (h.rx_head + 1) % RXQ;
  if (next == h.rx_tail) {
    bt_log("rx queue full, dropping packet");
    return;
  }
  struct rxpkt *r = &h.rx[h.rx_head];
  r->type = (uint8_t)type;
  r->len = (uint16_t)len;
  memcpy(r->data, data, (uint32_t)len);
  h.rx_head = next;
}

void bt_poll(uint32_t now)
{
  if (!h.attached)
    return;
  h.now = now;
  while (h.rx_tail != h.rx_head) {
    struct rxpkt *r = &h.rx[h.rx_tail];
    if (r->type == BT_PKT_EVT) {
      on_event(r->data, r->len);
    } else if (r->type == BT_PKT_ACL && r->len >= 4) {
      uint16_t hf = bt_get16(r->data);
      struct bt_conn *c = hci_conn_by_handle(hf & 0x0FFF);
      if (c)
        l2cap_acl_rx(c, hf >> 12, r->data + 4, r->len - 4);
    }
    h.rx_tail = (h.rx_tail + 1) % RXQ;
  }

  if (h.busy && now - h.sent_at > CMD_TIMEOUT_MS) {
    bt_log("command %04x timed out", h.inflight);
    h.busy = false;
    if (h.state == ST_INIT)
      h.state = ST_FAILED;
  }

  if (h.state == ST_READY) {
    while (h.user_tail != h.user_head) {
      char line[USERQ_LINE];
      memcpy(line, h.user[h.user_tail], USERQ_LINE);
      h.user_tail = (h.user_tail + 1) % USERQ;
      if (run_command(line) != 0)
        bt_log("command rejected: %s", line);
    }
    connect_pump();
    name_pump();
    gatt_poll();
    smp_poll(now);
    avdtp_poll(now);
    bthid_poll(now);
    tx_pump();
  }
  cmd_pump();
}

bool bt_present(void) { return h.attached && h.state == ST_READY; }

/* ---- text views */

int bt_status_text(char *buf, int max)
{
  static const char *const names[] = {"off", "starting", "ready", "failed"};
  char a[18];
  int n = bt_snprintf(buf, (uint32_t)max, "state %s\n", names[h.state]);
  if (h.state == ST_READY)
    n += bt_snprintf(buf + n, (uint32_t)(n < max ? max - n : 0), "address %s\nscanning %s\n", addr_str(h.bdaddr, a),
                     h.scan_br || h.scan_le ? "yes" : "no");
  for (int i = 0; i < BT_MAX_CONN && n < max; i++) {
    struct bt_conn *c = &h.conns[i];
    if (c->used)
      n += bt_snprintf(buf + n, (uint32_t)(max - n), "connection %s %s%s\n", addr_str(c->addr, a), c->le ? "le" : "br",
                       c->secure ? " encrypted" : "");
  }
  n += smp_status_text(buf + n, n < max ? max - n : 0);
  n += avdtp_status_text(buf + n, n < max ? max - n : 0);
  n += bthid_status_text(buf + n, n < max ? max - n : 0);
  return n < max ? n : max;
}

int bt_devices_text(char *buf, int max)
{
  int n = 0;
  for (int i = 0; i < BT_MAX_DEV && n < max; i++) {
    struct bt_dev *d = &h.devs[i];
    char a[18];
    if (!d->used)
      continue;
    uint32_t major = d->le ? 0 : (d->cod >> 8) & 0x1F, minor = (d->cod >> 2) & 0xF;
    const char *kind = major == 4                      ? "audio "
                       : major != COD_MAJOR_PERIPHERAL ? ""
                       : minor == 1 || minor == 2      ? "gamepad "
                                                       : "input ";
    n += bt_snprintf(buf + n, (uint32_t)(max - n), "%s %s %d %s%s\n", addr_str(d->addr, a), d->le ? "le" : "br",
                     d->rssi, kind, d->name);
  }
  return n < max ? n : max;
}

/* LE devices, one per line: "<addr> <addr type> <rssi> <service uuids, comma separated or -> <name>". */
int bt_le_devices_text(char *buf, int max)
{
  int n = 0;
  for (int i = 0; i < BT_MAX_DEV && n < max; i++) {
    const struct bt_dev *d = &h.devs[i];
    char a[18], u[40];
    if (!d->used || !d->le)
      continue;
    n += bt_snprintf(buf + n, (uint32_t)(max - n), "%s %u %d ", addr_str(d->addr, a), d->addr_type, d->rssi);
    for (int k = 0; k < d->nuuid && n < max; k++) {
      bt_uuid_text(u, sizeof u, d->uuids[k].v, d->uuids[k].len);
      n += bt_snprintf(buf + n, (uint32_t)(max - n), "%s%s", k ? "," : "", u);
    }
    if (!d->nuuid && n < max)
      buf[n++] = '-';
    n += bt_snprintf(buf + n, (uint32_t)(max - n), " %s\n", d->name);
  }
  return n < max ? n : max;
}
