/* bluetooth.c: Web Bluetooth (navigator.bluetooth) over the kernel's /sys/class/bluetooth/hci0 files. A GATT client for
 * one device at a time: requestDevice scans and shows a chooser, then connect, services, characteristics, reads,
 * writes and notifications. Descriptors, included services and the per-service access list are not implemented. */

#include "features/bluetooth/bluetooth.h"
#include "features/clienthints/clienthints.h"
#include "jsint.h"
#include <stdarg.h>
#include <stdint.h>

#define DIR "/sys/class/bluetooth/hci0/"
#define CONTROL_PATH DIR "control"
#define STATUS_PATH DIR "status"
#define LE_DEVICES_PATH DIR "le_devices"
#define GATT_PATH DIR "gatt"
#define EVENTS_PATH DIR "gatt_events"

#define MAX_DEVS 8
#define MAX_OPS 8
#define MAX_SUBS 16
#define MAX_SVC 16
#define MAX_CHR 48
#define MAX_CANDS 16
#define MAX_FILTERS 8
#define MAX_FILTER_SVC 4
#define MAX_WRITE 244
#define MAX_VALUE 512
#define TICK_MS 25
#define SLOW_MS 200
#define SCAN_MIN_MS 4000
#define SCAN_MAX_MS 10000
#define CONNECT_MS 45000
#define OP_MS 12000
#define UUID_LEN 37
#define NAME_LEN 32
#define ADDR_LEN 18

#define PROP_READ 0x02
#define PROP_WRITE_NO_RSP 0x04
#define PROP_WRITE 0x08
#define PROP_NOTIFY_ANY 0x30

enum {
  H_DEV = 0,
  H_SRV = H_DEV + MAX_DEVS,
  H_OPP = H_SRV + MAX_DEVS,
  H_OPC = H_OPP + MAX_OPS,
  H_SUB = H_OPC + MAX_OPS,
  H_SVC = H_SUB + MAX_SUBS,
  H_CHR = H_SVC + MAX_SVC,
  H_REQ = H_CHR + MAX_CHR,
  H_CONN = H_REQ + 1,
  H_N = H_CONN + 1
};

enum { OPK_READ, OPK_WRITE, OPK_WRITE_CMD, OPK_NOTIFY_ON, OPK_NOTIFY_OFF };

struct adv {
  char addr[ADDR_LEN], name[NAME_LEN], uuids[4][UUID_LEN];
  int nuuid;
};

struct filter {
  char services[MAX_FILTER_SVC][UUID_LEN], name[NAME_LEN], prefix[NAME_LEN];
  int nservices;
  bool has_name, has_prefix;
};

struct dev {
  char addr[ADDR_LEN], name[NAME_LEN];
};

struct op {
  bool used, inflight;
  uint8_t kind;
  uint32_t tag;
  uint16_t handle, len;
  uint8_t data[MAX_WRITE];
  unsigned sent;
};

struct gsvc {
  char uuid[UUID_LEN];
  int start, end;
};

struct gchr {
  char uuid[UUID_LEN];
  int handle, props;
};

struct alias {
  const char *name;
  unsigned id;
};

static js_ctx *ctx;
static js_object *held[H_N];
static int held_n = H_N;

static struct dev devs[MAX_DEVS];
static int ndevs;
static struct op ops[MAX_OPS];
static uint32_t tag_counter, last_seq;
static unsigned last_tick;

static struct {
  bool active, all;
  struct filter filters[MAX_FILTERS];
  int nfilters;
  unsigned start, last_poll;
} req;

static struct {
  int dev; /* index into devs, -1 when none */
  bool pending, seen, connected;
  unsigned start, last_poll;
} conn = {.dev = -1};

/* ---- text helpers */

static const char *after(const char *s, const char *prefix)
{
  while (*prefix)
    if (*s++ != *prefix++)
      return 0;
  return s;
}

static int hexv(char c)
{
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

static int hexn(const char *s, int n)
{
  int v = 0;
  for (int i = 0; i < n; i++) {
    int d = hexv(s[i]);
    if (d < 0)
      return -1;
    v = v * 16 + d;
  }
  return v;
}

static const char *next_line(const char *s)
{
  const char *nl = strchr(s, '\n');
  return nl ? nl + 1 : s + strlen(s);
}

/* Copy the field at *p (up to a space or the end of the line) and step past its delimiter. */
static int field(const char **p, char *dst, int cap)
{
  const char *s = *p;
  int n = 0;
  while (s[n] && s[n] != ' ' && s[n] != '\n')
    n++;
  if (n >= cap)
    return -1;
  memcpy(dst, s, (unsigned)n);
  dst[n] = 0;
  *p = s[n] == ' ' ? s + n + 1 : s + n;
  return n;
}

static int slurp(const char *path, char *buf, int cap)
{
  int n = readfile(path, buf, cap - 1);
  if (n < 0)
    n = 0;
  buf[n] = 0;
  return n;
}

static int command(const char *fmt, ...)
{
  char line[800];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  return writefile(CONTROL_PATH, line, strlen(line)) < 0 ? -1 : 0;
}

/* ---- UUIDs */

static const struct alias service_names[] = {{"generic_access", 0x1800},
                                             {"generic_attribute", 0x1801},
                                             {"immediate_alert", 0x1802},
                                             {"link_loss", 0x1803},
                                             {"tx_power", 0x1804},
                                             {"current_time", 0x1805},
                                             {"device_information", 0x180a},
                                             {"heart_rate", 0x180d},
                                             {"battery_service", 0x180f},
                                             {"blood_pressure", 0x1810},
                                             {"human_interface_device", 0x1812},
                                             {"environmental_sensing", 0x181a},
                                             {0, 0}};

static const struct alias characteristic_names[] = {{"device_name", 0x2a00},
                                                    {"appearance", 0x2a01},
                                                    {"alert_level", 0x2a06},
                                                    {"tx_power_level", 0x2a07},
                                                    {"battery_level", 0x2a19},
                                                    {"temperature_measurement", 0x2a1c},
                                                    {"model_number_string", 0x2a24},
                                                    {"serial_number_string", 0x2a25},
                                                    {"firmware_revision_string", 0x2a26},
                                                    {"hardware_revision_string", 0x2a27},
                                                    {"software_revision_string", 0x2a28},
                                                    {"manufacturer_name_string", 0x2a29},
                                                    {"heart_rate_measurement", 0x2a37},
                                                    {"body_sensor_location", 0x2a38},
                                                    {"heart_rate_control_point", 0x2a39},
                                                    {"hid_information", 0x2a4a},
                                                    {"report_map", 0x2a4b},
                                                    {"hid_control_point", 0x2a4c},
                                                    {"report", 0x2a4d},
                                                    {"pnp_id", 0x2a50},
                                                    {0, 0}};

static void short_uuid(char out[UUID_LEN], unsigned id)
{
  snprintf(out, UUID_LEN, "%08x-0000-1000-8000-00805f9b34fb", id);
}

/* A UUID as the kernel prints it (four hex digits or dashed), in the canonical 36 character form. */
static void canon_kernel(char out[UUID_LEN], const char *tok)
{
  int len = (int)strlen(tok);
  if (len == 4) {
    short_uuid(out, (unsigned)hexn(tok, 4));
    return;
  }
  strlcpy(out, tok, UUID_LEN);
}

/* A UUID argument (number, 128-bit string or well-known name); false if it is none of those. */
static bool uuid_arg(js_value v, const struct alias *names, char out[UUID_LEN])
{
  if (v.type == JV_NUM) {
    if (v.u.num < 0 || v.u.num > 4294967295.0)
      return false;
    short_uuid(out, (unsigned)v.u.num);
    return true;
  }
  if (v.type != JV_STR)
    return false;
  const char *s = arg_str(ctx, v);
  if (strlen(s) == 36) {
    for (int i = 0; i < 36; i++) {
      bool dash = i == 8 || i == 13 || i == 18 || i == 23;
      if (dash ? s[i] != '-' : hexv(s[i]) < 0)
        return false;
      out[i] = s[i] >= 'A' && s[i] <= 'F' ? (char)(s[i] + 32) : s[i];
    }
    out[36] = 0;
    return true;
  }
  for (; names && names->name; names++)
    if (streq(s, names->name)) {
      short_uuid(out, names->id);
      return true;
    }
  return false;
}

static js_value uuid_native(js_ctx *c, js_value *args, int argc, const struct alias *names)
{
  char u[UUID_LEN];
  if (!uuid_arg(A(0), names, u))
    return js_throw_type(c, "Invalid UUID");
  return js_mk_cstring(c, u);
}

static js_value uuid_canonical(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)t;
  return uuid_native(c, args, argc, 0);
}

static js_value uuid_service(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)t;
  return uuid_native(c, args, argc, service_names);
}

static js_value uuid_characteristic(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)t;
  return uuid_native(c, args, argc, characteristic_names);
}

/* ---- JS helpers */

static js_value reject(const char *name, const char *msg)
{
  return js_promise_rejected(ctx, dom_exception(ctx, name, msg));
}

static js_value reject_type(const char *msg)
{
  return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", msg));
}

static js_object *new_obj(void) { return js_new_object(ctx, ctx->object_proto); }

static int int_prop(js_value o, const char *key) { return (int)js_to_number(ctx, js_get(ctx, o, key)); }

static js_object *obj_prop(js_value o, const char *key)
{
  js_value v = js_get(ctx, o, key);
  return v.type == JV_OBJ ? v.u.obj : 0;
}

static js_value make_dataview(const uint8_t *d, int n)
{
  js_value ab = js_new_arraybuffer(ctx, d, n), dv;
  gc_push_scope();
  if (ab.type == JV_OBJ)
    gc_keep(ab.u.obj);
  dv = js_construct(ctx, js_get(ctx, js_obj_value(ctx->global), "DataView"), &ab, 1);
  gc_pop_scope();
  return dv;
}

static void set_value(js_object *chr, const uint8_t *d, int n)
{
  gc_push_scope();
  js_value dv = make_dataview(d, n);
  if (dv.type == JV_OBJ) {
    gc_keep(dv.u.obj);
    js_def(chr, "value", dv, P_WRITABLE | P_ENUMERABLE);
  }
  gc_pop_scope();
}

static void add_event_target(js_object *o)
{
  def_method(o, "addEventListener", el_add_listener, 3);
  def_method(o, "removeEventListener", el_remove_listener, 3);
  def_method(o, "dispatchEvent", el_dispatch, 1);
}

/* Fire a bubbling event at target, then on each object of path in turn. */
static void fire(const char *type, js_object *target, js_object *const path[3])
{
  gc_push_scope();
  js_value ev = make_event(type, 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(ctx, ev, "bubbles", js_bool(1));
    js_set(ctx, ev, "target", js_obj_value(target));
    for (int i = 0; i < 3 && path[i]; i++)
      ev_fire_on(path[i], ev, 0);
  }
  gc_pop_scope();
}

static void fire_value_changed(js_object *chr)
{
  js_object *service = obj_prop(js_obj_value(chr), "service");
  js_object *device = service ? obj_prop(js_obj_value(service), "device") : 0;
  js_object *const path[3] = {chr, service, device};
  fire("characteristicvaluechanged", chr, path);
}

/* ---- adapter files */

static int parse_advs(struct adv *out, int cap)
{
  static char text[3072];
  int n = 0;
  slurp(LE_DEVICES_PATH, text, sizeof text);
  for (const char *l = text; *l && n < cap; l = next_line(l)) {
    struct adv *a = &out[n];
    const char *p = l;
    char skip[16], list[160];
    memset(a, 0, sizeof *a);
    if (field(&p, a->addr, ADDR_LEN) != 17 || field(&p, skip, sizeof skip) < 0 || field(&p, skip, sizeof skip) < 0 ||
        field(&p, list, sizeof list) < 0)
      continue;
    for (const char *u = list; *u && a->nuuid < 4;) {
      char tok[UUID_LEN];
      int len = 0;
      while (u[len] && u[len] != ',')
        len++;
      if (len < UUID_LEN) {
        memcpy(tok, u, (unsigned)len);
        tok[len] = 0;
        if (len == 4 || len == 36)
          canon_kernel(a->uuids[a->nuuid++], tok);
      }
      u += len + (u[len] == ',');
    }
    for (int i = 0; p[i] && p[i] != '\n' && i < NAME_LEN - 1; i++)
      a->name[i] = p[i];
    n++;
  }
  return n;
}

static bool conn_listed(const char *addr)
{
  static char text[1024];
  char needle[48];
  slurp(STATUS_PATH, text, sizeof text);
  snprintf(needle, sizeof needle, "connection %s le", addr);
  return strstr(text, needle) != 0;
}

static bool gatt_ready(void)
{
  static char text[8192];
  slurp(GATT_PATH, text, sizeof text);
  return strstr(text, "state ready") != 0;
}

static void parse_gatt(struct gsvc *svcs, int *nsvc, struct gchr *chrs, int *nchr)
{
  static char text[8192];
  *nsvc = *nchr = 0;
  slurp(GATT_PATH, text, sizeof text);
  for (const char *l = text; *l; l = next_line(l)) {
    const char *p;
    char u[UUID_LEN];
    if ((p = after(l, "service ")) && *nsvc < MAX_SVC) {
      if (field(&p, u, sizeof u) < 0 || !(p = after(p, "handles ")) || hexn(p, 4) < 0 || p[4] != '-' ||
          hexn(p + 5, 4) < 0)
        continue;
      canon_kernel(svcs[*nsvc].uuid, u);
      svcs[*nsvc].start = hexn(p, 4);
      svcs[*nsvc].end = hexn(p + 5, 4);
      (*nsvc)++;
    } else if ((p = after(l, "  characteristic ")) && *nchr < MAX_CHR) {
      const char *q;
      if (field(&p, u, sizeof u) < 0 || !(q = after(p, "handle ")) || hexn(q, 4) < 0 || !(q = after(q + 5, "props ")) ||
          hexn(q, 2) < 0)
        continue;
      canon_kernel(chrs[*nchr].uuid, u);
      chrs[*nchr].handle = hexn(p + 7, 4);
      chrs[*nchr].props = hexn(q, 2);
      (*nchr)++;
    }
  }
}

static bool parse_dec(const char **p, uint32_t *v)
{
  const char *s = *p;
  *v = 0;
  while (*s >= '0' && *s <= '9')
    *v = *v * 10 + (uint32_t)(*s++ - '0');
  if (s == *p || *s != ' ')
    return false;
  *p = s + 1;
  return true;
}

/* ---- connection state */

static int dev_of(js_value o) { return int_prop(o, "@dev"); }

static void set_connected(int d, bool on) { js_set(ctx, js_obj_value(held[H_SRV + d]), "connected", js_bool(on)); }

static js_value need_connected(int d)
{
  if (conn.connected && conn.dev == d)
    return js_undef();
  return reject("NetworkError", "GATT Server is disconnected. Cannot perform GATT operations. (Re)connect first with "
                                "`device.gatt.connect`.");
}

static js_value gatt_error(int code)
{
  char msg[40];
  if (code == 0x02 || code == 0x03)
    return dom_exception(ctx, "NotSupportedError", "GATT operation not permitted.");
  if (code == 0x05 || code == 0x08 || code == 0x0F)
    return dom_exception(ctx, "NotAllowedError", "GATT operation not authorized.");
  if (code == 0xFE)
    return dom_exception(ctx, "NotSupportedError", "GATT Error: Not supported.");
  snprintf(msg, sizeof msg, "GATT Error: 0x%02x.", (unsigned)code);
  return dom_exception(ctx, "NetworkError", msg);
}

static void op_settle(int i, bool rejected, js_value v)
{
  js_object *p = held[H_OPP + i];
  gc_push_scope();
  if (p)
    gc_keep(p);
  if (v.type == JV_OBJ)
    gc_keep(v.u.obj);
  ops[i].used = ops[i].inflight = false;
  held[H_OPP + i] = held[H_OPC + i] = 0;
  if (p)
    js_promise_settle(ctx, p, rejected, v);
  gc_pop_scope();
}

static void submit_next(void)
{
  int best = -1;
  for (int i = 0; i < MAX_OPS; i++) {
    if (ops[i].inflight)
      return;
    if (ops[i].used && (best < 0 || ops[i].tag < ops[best].tag))
      best = i;
  }
  if (best < 0)
    return;
  struct op *o = &ops[best];
  char hex[2 * MAX_WRITE + 1];
  int rc;
  if (o->kind == OPK_READ) {
    rc = command("att %u read %x", o->tag, o->handle);
  } else if (o->kind == OPK_WRITE || o->kind == OPK_WRITE_CMD) {
    for (int i = 0; i < o->len; i++)
      snprintf(hex + 2 * i, 3, "%02x", o->data[i]);
    rc = command("att %u %s %x %s", o->tag, o->kind == OPK_WRITE ? "write" : "writecmd", o->handle, hex);
  } else {
    rc = command("att %u notify %x %s", o->tag, o->handle, o->kind == OPK_NOTIFY_ON ? "on" : "off");
  }
  if (rc < 0) {
    op_settle(best, true, dom_exception(ctx, "NetworkError", "GATT operation failed."));
    return;
  }
  o->inflight = true;
  o->sent = msec();
}

static js_value queue_op(int kind, js_value chr, const uint8_t *data, int len)
{
  int i = 0;
  while (i < MAX_OPS && ops[i].used)
    i++;
  if (i == MAX_OPS)
    return reject("NetworkError", "Too many pending GATT operations.");
  js_object *p = js_promise_new(ctx);
  struct op *o = &ops[i];
  memset(o, 0, sizeof *o);
  o->used = true;
  o->kind = (uint8_t)kind;
  o->tag = ++tag_counter;
  o->handle = (uint16_t)int_prop(chr, "@h");
  o->len = (uint16_t)len;
  if (len)
    memcpy(o->data, data, (unsigned)len);
  held[H_OPP + i] = p;
  held[H_OPC + i] = chr.u.obj;
  submit_next();
  return js_obj_value(p);
}

static int sub_find(int handle)
{
  for (int i = 0; i < MAX_SUBS; i++)
    if (held[H_SUB + i] && int_prop(js_obj_value(held[H_SUB + i]), "@h") == handle)
      return i;
  return -1;
}

static void sub_add(js_object *chr)
{
  if (sub_find(int_prop(js_obj_value(chr), "@h")) >= 0)
    return;
  for (int i = 0; i < MAX_SUBS; i++)
    if (!held[H_SUB + i]) {
      held[H_SUB + i] = chr;
      return;
    }
}

/* A result or notification from the kernel; returns 1 if page script may have run. */
static int on_event(uint32_t tag, const char *kind, int handle, const uint8_t *d, int n)
{
  if (streq(kind, "notify")) {
    int s = sub_find(handle);
    if (s < 0)
      return 0;
    js_object *chr = held[H_SUB + s];
    gc_push_scope();
    gc_keep(chr);
    set_value(chr, d, n);
    fire_value_changed(chr);
    gc_pop_scope();
    return 1;
  }
  for (int i = 0; i < MAX_OPS; i++) {
    if (!ops[i].inflight || ops[i].tag != tag)
      continue;
    js_object *chr = held[H_OPC + i];
    int op_kind = ops[i].kind;
    gc_push_scope();
    gc_keep(chr);
    if (streq(kind, "err")) {
      op_settle(i, true, gatt_error(n ? d[0] : 0xFF));
    } else if (op_kind == OPK_READ) {
      set_value(chr, d, n);
      js_value v = js_get(ctx, js_obj_value(chr), "value");
      op_settle(i, false, v);
      fire_value_changed(chr);
    } else if (op_kind == OPK_NOTIFY_ON || op_kind == OPK_NOTIFY_OFF) {
      if (op_kind == OPK_NOTIFY_ON) {
        sub_add(chr);
      } else {
        int s = sub_find(int_prop(js_obj_value(chr), "@h"));
        if (s >= 0)
          held[H_SUB + s] = 0;
      }
      op_settle(i, false, js_obj_value(chr));
    } else {
      op_settle(i, false, js_undef());
    }
    gc_pop_scope();
    return 1;
  }
  return 0;
}

static int pump_events(void)
{
  static char text[2400];
  static uint8_t bytes[MAX_VALUE];
  uint32_t max = 0;
  int work = 0;
  slurp(EVENTS_PATH, text, sizeof text);
  for (const char *l = text; *l; l = next_line(l)) {
    char kind[8], htok[8], hex[2 * MAX_VALUE + 2];
    const char *p = l;
    uint32_t seq, tag;
    if (!parse_dec(&p, &seq) || !parse_dec(&p, &tag) || field(&p, kind, sizeof kind) < 0 ||
        field(&p, htok, sizeof htok) != 4 || field(&p, hex, sizeof hex) < 0)
      continue;
    int n = (int)strlen(hex) / 2;
    for (int i = 0; i < n; i++)
      bytes[i] = (uint8_t)hexn(hex + 2 * i, 2);
    if (seq > last_seq)
      work += on_event(tag, kind, hexn(htok, 4), bytes, n);
    if (seq > max)
      max = seq;
  }
  if (max) {
    command("att ack %u", max);
    if (max > last_seq)
      last_seq = max;
  }
  return work;
}

/* The link went away (or script disconnected): fail what was waiting and tell the page. */
static void drop_connection(void)
{
  int d = conn.dev;
  bool was = conn.connected;
  js_object *pending = held[H_CONN];
  if (d < 0)
    return;
  conn.dev = -1;
  conn.connected = conn.pending = conn.seen = false;
  held[H_CONN] = 0;
  gc_push_scope();
  if (pending) {
    gc_keep(pending);
    js_promise_settle(ctx, pending, 1, dom_exception(ctx, "NetworkError", "Connection attempt failed."));
  }
  for (int i = 0; i < MAX_OPS; i++)
    if (ops[i].used)
      op_settle(i, true, dom_exception(ctx, "NetworkError", "GATT Server is disconnected."));
  for (int i = 0; i < MAX_SUBS; i++)
    held[H_SUB + i] = 0;
  for (int i = 0; i < MAX_SVC; i++)
    held[H_SVC + i] = 0;
  for (int i = 0; i < MAX_CHR; i++)
    held[H_CHR + i] = 0;
  set_connected(d, false);
  if (was) {
    js_object *dev = held[H_DEV + d];
    js_object *const path[3] = {dev, 0, 0};
    fire("gattserverdisconnected", dev, path);
  }
  gc_pop_scope();
}

static int connect_step(unsigned now)
{
  if (now - conn.last_poll < SLOW_MS)
    return 0;
  conn.last_poll = now;
  bool listed = conn_listed(devs[conn.dev].addr);
  if (listed)
    conn.seen = true;
  if (listed && gatt_ready()) {
    js_object *p = held[H_CONN];
    int d = conn.dev;
    conn.pending = false;
    conn.connected = true;
    last_seq = 0;
    held[H_CONN] = 0;
    gc_push_scope();
    if (p)
      gc_keep(p);
    set_connected(d, true);
    if (p)
      js_promise_settle(ctx, p, 0, js_obj_value(held[H_SRV + d]));
    gc_pop_scope();
    return 1;
  }
  if ((conn.seen && !listed) || now - conn.start > CONNECT_MS) {
    if (listed)
      command("disconnect");
    drop_connection();
    return 1;
  }
  return 0;
}

static int connected_step(unsigned now)
{
  int work = 0;
  if (now - conn.last_poll >= SLOW_MS) {
    conn.last_poll = now;
    if (!conn_listed(devs[conn.dev].addr)) {
      drop_connection();
      return 1;
    }
  }
  work += pump_events();
  for (int i = 0; i < MAX_OPS && conn.connected; i++)
    if (ops[i].inflight && now - ops[i].sent > OP_MS) {
      op_settle(i, true, dom_exception(ctx, "NetworkError", "GATT operation timed out."));
      work++;
    }
  if (conn.connected)
    submit_next();
  return work;
}

/* ---- device, service and characteristic objects */

static js_value server_connect(js_ctx *c, js_value t, js_value *args, int argc);
static js_value server_disconnect(js_ctx *c, js_value t, js_value *args, int argc);
static js_value server_services(js_ctx *c, js_value t, js_value *args, int argc);
static js_value server_service(js_ctx *c, js_value t, js_value *args, int argc);

static js_object *device_obj(int d)
{
  if (held[H_DEV + d])
    return held[H_DEV + d];
  gc_push_scope();
  js_object *dev = gc_keep(new_obj()), *srv = gc_keep(new_obj());
  js_def(dev, "id", js_mk_cstring(ctx, devs[d].addr), P_ENUMERABLE);
  js_def(dev, "name", devs[d].name[0] ? js_mk_cstring(ctx, devs[d].name) : js_null(), P_ENUMERABLE);
  js_def(dev, "gatt", js_obj_value(srv), P_ENUMERABLE);
  js_def(dev, "ongattserverdisconnected", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(dev, "@dev", js_number(d), 0);
  add_event_target(dev);
  js_def(srv, "device", js_obj_value(dev), P_ENUMERABLE);
  js_def(srv, "connected", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(srv, "@dev", js_number(d), 0);
  def_method(srv, "connect", server_connect, 0);
  def_method(srv, "disconnect", server_disconnect, 0);
  def_method(srv, "getPrimaryService", server_service, 1);
  def_method(srv, "getPrimaryServices", server_services, 0);
  held[H_DEV + d] = dev;
  held[H_SRV + d] = srv;
  gc_pop_scope();
  return dev;
}

static js_value service_chars(js_ctx *c, js_value t, js_value *args, int argc);
static js_value service_char(js_ctx *c, js_value t, js_value *args, int argc);

static js_object *service_obj(const struct gsvc *s, int d)
{
  for (int i = 0; i < MAX_SVC; i++)
    if (held[H_SVC + i] && int_prop(js_obj_value(held[H_SVC + i]), "@start") == s->start)
      return held[H_SVC + i];
  for (int i = 0; i < MAX_SVC; i++) {
    if (held[H_SVC + i])
      continue;
    gc_push_scope();
    js_object *o = gc_keep(new_obj());
    js_def(o, "device", js_obj_value(held[H_DEV + d]), P_ENUMERABLE);
    js_def(o, "uuid", js_mk_cstring(ctx, s->uuid), P_ENUMERABLE);
    js_def(o, "isPrimary", js_bool(1), P_ENUMERABLE);
    js_def(o, "@start", js_number(s->start), 0);
    js_def(o, "@end", js_number(s->end), 0);
    js_def(o, "@dev", js_number(d), 0);
    def_method(o, "getCharacteristic", service_char, 1);
    def_method(o, "getCharacteristics", service_chars, 0);
    add_event_target(o);
    held[H_SVC + i] = o;
    gc_pop_scope();
    return o;
  }
  return 0;
}

static js_value chr_read(js_ctx *c, js_value t, js_value *args, int argc);
static js_value chr_write(js_ctx *c, js_value t, js_value *args, int argc);
static js_value chr_write_rsp(js_ctx *c, js_value t, js_value *args, int argc);
static js_value chr_write_cmd(js_ctx *c, js_value t, js_value *args, int argc);
static js_value chr_notify_on(js_ctx *c, js_value t, js_value *args, int argc);
static js_value chr_notify_off(js_ctx *c, js_value t, js_value *args, int argc);
static js_value chr_descriptors(js_ctx *c, js_value t, js_value *args, int argc);

static js_object *char_obj(const struct gchr *ch, js_object *service, int d)
{
  for (int i = 0; i < MAX_CHR; i++)
    if (held[H_CHR + i] && int_prop(js_obj_value(held[H_CHR + i]), "@h") == ch->handle)
      return held[H_CHR + i];
  for (int i = 0; i < MAX_CHR; i++) {
    if (held[H_CHR + i])
      continue;
    static const struct {
      const char *name;
      int bit;
    } flags[] = {{"broadcast", 0x01}, {"read", 0x02},     {"writeWithoutResponse", 0x04},     {"write", 0x08},
                 {"notify", 0x10},    {"indicate", 0x20}, {"authenticatedSignedWrites", 0x40}};
    gc_push_scope();
    js_object *o = gc_keep(new_obj()), *props = gc_keep(new_obj());
    for (unsigned k = 0; k < sizeof flags / sizeof *flags; k++)
      js_def(props, flags[k].name, js_bool(ch->props & flags[k].bit), P_ENUMERABLE);
    js_def(props, "reliableWrite", js_bool(0), P_ENUMERABLE);
    js_def(props, "writableAuxiliaries", js_bool(0), P_ENUMERABLE);
    js_def(o, "service", js_obj_value(service), P_ENUMERABLE);
    js_def(o, "uuid", js_mk_cstring(ctx, ch->uuid), P_ENUMERABLE);
    js_def(o, "properties", js_obj_value(props), P_ENUMERABLE);
    js_def(o, "value", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(o, "oncharacteristicvaluechanged", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(o, "@h", js_number(ch->handle), 0);
    js_def(o, "@props", js_number(ch->props), 0);
    js_def(o, "@dev", js_number(d), 0);
    def_method(o, "readValue", chr_read, 0);
    def_method(o, "writeValue", chr_write, 1);
    def_method(o, "writeValueWithResponse", chr_write_rsp, 1);
    def_method(o, "writeValueWithoutResponse", chr_write_cmd, 1);
    def_method(o, "startNotifications", chr_notify_on, 0);
    def_method(o, "stopNotifications", chr_notify_off, 0);
    def_method(o, "getDescriptor", chr_descriptors, 1);
    def_method(o, "getDescriptors", chr_descriptors, 0);
    add_event_target(o);
    held[H_CHR + i] = o;
    gc_pop_scope();
    return o;
  }
  return 0;
}

/* ---- GATT server natives */

static js_value server_connect(js_ctx *c, js_value t, js_value *args, int argc)
{
  int d = dev_of(t);
  (void)args;
  (void)argc;
  if (conn.connected && conn.dev == d)
    return js_promise_resolved(c, t);
  if (conn.dev >= 0)
    return reject("NetworkError", conn.dev == d ? "A connection attempt is already in progress."
                                                : "Another device is already connected.");
  if (command("connect %s", devs[d].addr) < 0)
    return reject("NetworkError", "Connection attempt failed.");
  js_object *p = js_promise_new(c);
  held[H_CONN] = p;
  conn.dev = d;
  conn.pending = true;
  conn.seen = false;
  conn.start = msec();
  conn.last_poll = 0;
  return js_obj_value(p);
}

static js_value server_disconnect(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)args;
  (void)argc;
  if (conn.dev == dev_of(t)) {
    command("disconnect");
    drop_connection();
  }
  return js_undef();
}

static js_value services_matching(js_value t, js_value *args, int argc, bool many)
{
  int d = dev_of(t), nsvc, nchr;
  struct gsvc svcs[MAX_SVC];
  struct gchr chrs[MAX_CHR];
  char want[UUID_LEN];
  bool filter = argc > 0 && A(0).type != JV_UNDEF;
  js_value r = need_connected(d);
  if (r.type != JV_UNDEF)
    return r;
  if (filter && !uuid_arg(A(0), service_names, want))
    return reject_type("Invalid Service name.");
  parse_gatt(svcs, &nsvc, chrs, &nchr);
  gc_push_scope();
  js_object *list = gc_keep(js_new_array(ctx, 0));
  for (int i = 0; i < nsvc; i++) {
    if (filter && !streq(svcs[i].uuid, want))
      continue;
    js_object *o = service_obj(&svcs[i], d);
    if (o)
      js_array_push(ctx, list, js_obj_value(o));
  }
  if (js_array_len(list) == 0) {
    char msg[96];
    snprintf(msg, sizeof msg, "No Services matching UUID %s found in Device.", filter ? want : "(any)");
    r = reject("NotFoundError", msg);
  } else {
    r = js_promise_resolved(ctx, many ? js_obj_value(list) : js_get_index(ctx, js_obj_value(list), 0));
  }
  gc_pop_scope();
  return r;
}

static js_value server_service(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  if (argc < 1)
    return reject_type("1 argument required, but only 0 present.");
  return services_matching(t, args, argc, false);
}

static js_value server_services(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  return services_matching(t, args, argc, true);
}

/* ---- service natives */

static js_value chars_matching(js_value t, js_value *args, int argc, bool many)
{
  int d = dev_of(t), nsvc, nchr;
  struct gsvc svcs[MAX_SVC];
  struct gchr chrs[MAX_CHR];
  char want[UUID_LEN];
  int start = int_prop(t, "@start"), end = int_prop(t, "@end");
  bool filter = argc > 0 && A(0).type != JV_UNDEF;
  js_value r = need_connected(d);
  if (r.type != JV_UNDEF)
    return r;
  if (filter && !uuid_arg(A(0), characteristic_names, want))
    return reject_type("Invalid Characteristic name.");
  parse_gatt(svcs, &nsvc, chrs, &nchr);
  gc_push_scope();
  js_object *list = gc_keep(js_new_array(ctx, 0));
  for (int i = 0; i < nchr; i++) {
    if (chrs[i].handle < start || chrs[i].handle > end || (filter && !streq(chrs[i].uuid, want)))
      continue;
    js_object *o = char_obj(&chrs[i], t.u.obj, d);
    if (o)
      js_array_push(ctx, list, js_obj_value(o));
  }
  if (js_array_len(list) == 0) {
    char msg[96];
    snprintf(msg, sizeof msg, "No Characteristics matching UUID %s found in Service.", filter ? want : "(any)");
    r = reject("NotFoundError", msg);
  } else {
    r = js_promise_resolved(ctx, many ? js_obj_value(list) : js_get_index(ctx, js_obj_value(list), 0));
  }
  gc_pop_scope();
  return r;
}

static js_value service_char(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  if (argc < 1)
    return reject_type("1 argument required, but only 0 present.");
  return chars_matching(t, args, argc, false);
}

static js_value service_chars(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  return chars_matching(t, args, argc, true);
}

/* ---- characteristic natives */

static js_value chr_read(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value r = need_connected(dev_of(t));
  (void)c;
  (void)args;
  (void)argc;
  if (r.type != JV_UNDEF)
    return r;
  if (!(int_prop(t, "@props") & PROP_READ))
    return reject("NotSupportedError", "GATT operation not permitted.");
  return queue_op(OPK_READ, t, 0, 0);
}

/* mode 0: with response if the characteristic does it, else without; 1: with response; 2: without */
static js_value write_value(js_value t, js_value *args, int argc, int mode)
{
  int n, props = int_prop(t, "@props");
  js_value r = need_connected(dev_of(t));
  if (r.type != JV_UNDEF)
    return r;
  const unsigned char *b = view_bytes(A(0), &n);
  if (!b)
    return reject_type("The provided value is not an ArrayBuffer or ArrayBufferView.");
  bool with = props & PROP_WRITE, without = props & PROP_WRITE_NO_RSP;
  int kind = mode == 2 ? OPK_WRITE_CMD : mode == 1 ? OPK_WRITE : with ? OPK_WRITE : OPK_WRITE_CMD;
  if (!(kind == OPK_WRITE ? with : without))
    return reject("NotSupportedError", "GATT operation not permitted.");
  if (n < 1 || n > MAX_WRITE)
    return reject("InvalidModificationError", "Value must be between 1 and 244 bytes.");
  return queue_op(kind, t, b, n);
}

static js_value chr_write(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  return write_value(t, args, argc, 0);
}

static js_value chr_write_rsp(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  return write_value(t, args, argc, 1);
}

static js_value chr_write_cmd(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  return write_value(t, args, argc, 2);
}

static js_value notifications(js_value t, int kind)
{
  js_value r = need_connected(dev_of(t));
  if (r.type != JV_UNDEF)
    return r;
  if (!(int_prop(t, "@props") & PROP_NOTIFY_ANY))
    return reject("NotSupportedError", "GATT Error: Not supported.");
  return queue_op(kind, t, 0, 0);
}

static js_value chr_notify_on(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)args;
  (void)argc;
  return notifications(t, OPK_NOTIFY_ON);
}

static js_value chr_notify_off(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)args;
  (void)argc;
  return notifications(t, OPK_NOTIFY_OFF);
}

static js_value chr_descriptors(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)t;
  (void)args;
  (void)argc;
  return reject("NotSupportedError", "GATT descriptors are not supported.");
}

/* ---- navigator.bluetooth */

static bool filter_matches(const struct filter *f, const struct adv *a)
{
  for (int i = 0; i < f->nservices; i++) {
    bool found = false;
    for (int k = 0; k < a->nuuid; k++)
      found |= streq(f->services[i], a->uuids[k]);
    if (!found)
      return false;
  }
  if (f->has_name && !streq(f->name, a->name))
    return false;
  return !f->has_prefix || after(a->name, f->prefix);
}

static bool adv_matches(const struct adv *a)
{
  if (req.all)
    return true;
  for (int i = 0; i < req.nfilters; i++)
    if (filter_matches(&req.filters[i], a))
      return true;
  return false;
}

/* Fill f from a filter dictionary; returns an error message, or 0. */
static const char *parse_filter(js_value o, struct filter *f)
{
  memset(f, 0, sizeof *f);
  if (o.type != JV_OBJ)
    return "A filter must be an object.";
  js_value services = js_get(ctx, o, "services"), name = js_get(ctx, o, "name"), prefix = js_get(ctx, o, "namePrefix");
  if (services.type == JV_OBJ && js_is_array(services)) {
    unsigned n = js_array_len(services.u.obj);
    if (n > MAX_FILTER_SVC)
      return "Too many services in a filter.";
    for (unsigned i = 0; i < n; i++)
      if (!uuid_arg(js_get_index(ctx, services, i), service_names, f->services[f->nservices++]))
        return "Invalid Service name.";
  } else if (services.type != JV_UNDEF) {
    return "'services' must be an array.";
  }
  if (name.type != JV_UNDEF) {
    strlcpy(f->name, arg_str(ctx, name), NAME_LEN);
    f->has_name = true;
  }
  if (prefix.type != JV_UNDEF) {
    strlcpy(f->prefix, arg_str(ctx, prefix), NAME_LEN);
    f->has_prefix = f->prefix[0] != 0;
    if (!f->has_prefix)
      return "'namePrefix' must not be empty.";
  }
  if (!f->nservices && !f->has_name && !f->has_prefix)
    return "A filter must restrict the device in some way.";
  return 0;
}

static js_value bt_request_device(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value o = A(0), filters = o.type == JV_OBJ ? js_get(c, o, "filters") : js_undef();
  char probe[16];
  (void)t;
  if (req.active)
    return reject("NotAllowedError", "A requestDevice() chooser is already open.");
  req.all = o.type == JV_OBJ && js_to_boolean(js_get(c, o, "acceptAllDevices"));
  req.nfilters = 0;
  if (filters.type != JV_UNDEF) {
    if (filters.type != JV_OBJ || !js_is_array(filters) || js_array_len(filters.u.obj) > MAX_FILTERS)
      return reject_type("'filters' must be an array of up to 8 filters.");
    for (unsigned i = 0; i < js_array_len(filters.u.obj); i++) {
      const char *err = parse_filter(js_get_index(c, filters, i), &req.filters[req.nfilters]);
      if (err)
        return reject_type(err);
      req.nfilters++;
    }
  }
  if (req.all == (req.nfilters > 0))
    return reject_type("Either 'filters' should be present or 'acceptAllDevices' should be true, but not both.");
  if (readfile(STATUS_PATH, probe, sizeof probe) < 0)
    return reject("NotFoundError", "Bluetooth adapter not available.");
  if (command("scan on") < 0)
    return reject("NetworkError", "Failed to start the Bluetooth scan.");
  js_object *p = js_promise_new(c);
  held[H_REQ] = p;
  req.active = true;
  req.start = msec();
  req.last_poll = 0;
  return js_obj_value(p);
}

static int add_device(const struct adv *a)
{
  int slot = -1;
  for (int i = 0; i < ndevs; i++)
    if (streq(devs[i].addr, a->addr))
      slot = i;
  if (slot < 0) {
    slot = ndevs < MAX_DEVS ? ndevs++ : (conn.dev == 0 ? 1 : 0);
    held[H_DEV + slot] = held[H_SRV + slot] = 0;
  }
  strlcpy(devs[slot].addr, a->addr, ADDR_LEN);
  strlcpy(devs[slot].name, a->name, NAME_LEN);
  return slot;
}

static void page_host(char *out, int cap)
{
  const char *u = g_page_url_buf, *p = strstr(u, "://");
  int n = 0;
  p = p ? p + 3 : u;
  while (p[n] && p[n] != '/' && p[n] != '?' && n < cap - 1) {
    out[n] = p[n];
    n++;
  }
  out[n] = 0;
}

static int finish_scan(void)
{
  static struct adv advs[24];
  int cand[MAX_CANDS], nc = 0, n = parse_advs(advs, 24);
  js_object *p = held[H_REQ];
  char items[1024], msg[128], host[64];
  int len = 0;
  command("scan off");
  req.active = false;
  held[H_REQ] = 0;
  gc_push_scope();
  gc_keep(p);
  for (int i = 0; i < n && nc < MAX_CANDS; i++)
    if (adv_matches(&advs[i]))
      cand[nc++] = i;
  if (!nc) {
    js_promise_settle(ctx, p, 1, dom_exception(ctx, "NotFoundError", "Bluetooth device not found."));
    gc_pop_scope();
    return 1;
  }
  items[0] = 0;
  for (int i = 0; i < nc && len < (int)sizeof items - 100; i++)
    len += snprintf(items + len, sizeof items - (unsigned)len, "%s (%s)\n",
                    advs[cand[i]].name[0] ? advs[cand[i]].name : "Unknown device", advs[cand[i]].addr);
  page_host(host, sizeof host);
  snprintf(msg, sizeof msg, "%s wants to pair with a Bluetooth device.", host[0] ? host : "This page");
  int pick = worker_dialog(5, msg, items, 0, 0);
  if (pick < 0 || pick >= nc) {
    js_promise_settle(ctx, p, 1, dom_exception(ctx, "NotAllowedError", "User cancelled the requestDevice() chooser."));
  } else {
    js_object *dev = device_obj(add_device(&advs[cand[pick]]));
    js_promise_settle(ctx, p, 0, js_obj_value(dev));
  }
  gc_pop_scope();
  return 1;
}

static int scan_step(unsigned now)
{
  static struct adv advs[24];
  if (now - req.last_poll < SLOW_MS)
    return 0;
  req.last_poll = now;
  unsigned elapsed = now - req.start;
  int matches = 0, n = parse_advs(advs, 24);
  for (int i = 0; i < n; i++)
    matches += adv_matches(&advs[i]);
  if (elapsed >= SCAN_MAX_MS || (elapsed >= SCAN_MIN_MS && matches > 0))
    return finish_scan();
  return 0;
}

static js_value bt_availability(js_ctx *c, js_value t, js_value *args, int argc)
{
  char probe[16];
  (void)t;
  (void)args;
  (void)argc;
  return js_promise_resolved(c, js_bool(readfile(STATUS_PATH, probe, sizeof probe) >= 0));
}

static js_value bt_get_devices(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value r;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  js_object *list = gc_keep(js_new_array(c, 0));
  for (int i = 0; i < ndevs; i++)
    js_array_push(c, list, js_obj_value(device_obj(i)));
  r = js_promise_resolved(c, js_obj_value(list));
  gc_pop_scope();
  return r;
}

int bluetooth_tick(void)
{
  unsigned now = msec();
  int work = 0;
  if (!ctx || now - last_tick < TICK_MS)
    return 0;
  last_tick = now;
  if (req.active)
    work += scan_step(now);
  if (conn.pending)
    work += connect_step(now);
  else if (conn.connected)
    work += connected_step(now);
  return work;
}

void bluetooth_install(js_ctx *c, js_object *navigator, const char *page_url)
{
  if (!ch_secure_url(page_url))
    return;
  ctx = c;
  memset(held, 0, sizeof held);
  memset(ops, 0, sizeof ops);
  memset(&req, 0, sizeof req);
  memset(&conn, 0, sizeof conn);
  conn.dev = -1;
  ndevs = 0;
  tag_counter = last_seq = 0;
  last_tick = msec();
  gc_add_root_range((void **)held, &held_n);

  js_object *bt = new_obj(), *uuid = new_obj();
  def_method(bt, "requestDevice", bt_request_device, 1);
  def_method(bt, "getAvailability", bt_availability, 0);
  def_method(bt, "getDevices", bt_get_devices, 0);
  add_event_target(bt);
  js_def(navigator, "bluetooth", js_obj_value(bt), P_ENUMERABLE);
  def_method(uuid, "canonicalUUID", uuid_canonical, 1);
  def_method(uuid, "getService", uuid_service, 1);
  def_method(uuid, "getCharacteristic", uuid_characteristic, 1);
  js_def(c->global, "BluetoothUUID", js_obj_value(uuid), P_WRITABLE | P_CONFIGURABLE);
}

void bluetooth_teardown(void)
{
  if (!ctx)
    return;
  if (req.active)
    command("scan off");
  if (conn.dev >= 0)
    command("disconnect");
  gc_remove_root_range((void **)held);
  ctx = 0;
}
