#define OWOS_SUITE_TEST
#include "jsint.h"
#define msec bt_fake_msec
#define readfile bt_fake_readfile
#define writefile bt_fake_writefile
#define make_event bt_fake_make_event
#define ev_fire_on bt_fake_ev_fire_on
#define el_add_listener bt_fake_add_listener
#define el_remove_listener bt_fake_remove_listener
#define el_dispatch bt_fake_dispatch
#define dom_exception bt_fake_dom_exception
#define worker_dialog bt_fake_worker_dialog
#define ch_secure_url bt_fake_secure_url
#define g_page_url_buf bt_fake_page_url
unsigned bt_fake_msec(void);
int bt_fake_readfile(const char *name, char *buf, int max);
int bt_fake_writefile(const char *name, const void *buf, int len);
js_value bt_fake_make_event(const char *type, int x, int y, js_value init);
void bt_fake_ev_fire_on(js_object *obj, js_value ev, int want_capture);
js_value bt_fake_add_listener(js_ctx *c, js_value t, js_value *a, int n);
js_value bt_fake_remove_listener(js_ctx *c, js_value t, js_value *a, int n);
js_value bt_fake_dispatch(js_ctx *c, js_value t, js_value *a, int n);
js_value bt_fake_dom_exception(js_ctx *c, const char *name, const char *msg);
int bt_fake_worker_dialog(int kind, const char *msg, const char *deflt, char *out, int outsz);
bool bt_fake_secure_url(const char *url);
extern char bt_fake_page_url[];
#include "features/bluetooth/bluetooth.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char bt_fake_page_url[256] = "https://example.com/page";

static unsigned clock_ms = 1000;
unsigned bt_fake_msec(void) { return clock_ms; }

static char le_text[1024], gatt_text[2048];
static char cmds[64][900];
static int ncmds;
static bool adapter, link_up, connect_works, drop_acks;
static struct {
  unsigned seq, tag;
  char text[1100];
} evq[16];
static int nev;
static unsigned ev_seq;
static int dialog_pick, dialog_calls;
static char dialog_items[1024], dialog_msg[256];

bool bt_fake_secure_url(const char *url) { return !strncmp(url, "https:", 6); }

js_value bt_fake_dom_exception(js_ctx *c, const char *name, const char *msg)
{
  return js_make_error(c, c->type_error_proto, name, msg);
}

int bt_fake_worker_dialog(int kind, const char *msg, const char *deflt, char *out, int outsz)
{
  (void)out;
  (void)outsz;
  expect_eq(kind, 5);
  dialog_calls++;
  snprintf(dialog_msg, sizeof dialog_msg, "%s", msg);
  snprintf(dialog_items, sizeof dialog_items, "%s", deflt);
  return dialog_pick;
}

js_value bt_fake_make_event(const char *type, int x, int y, js_value init)
{
  js_object *e = js_new_object(g_ctx, g_ctx->object_proto);
  (void)x;
  (void)y;
  (void)init;
  js_def(e, "type", js_mk_cstring(g_ctx, type), P_WRITABLE | P_ENUMERABLE);
  return js_obj_value(e);
}

void bt_fake_ev_fire_on(js_object *obj, js_value ev, int want_capture)
{
  char on[64] = "on";
  js_value h;
  (void)want_capture;
  strcat(on, js_get(g_ctx, ev, "type").u.str->data);
  h = js_get(g_ctx, js_obj_value(obj), on);
  if (js_is_callable(h))
    js_call(g_ctx, h, js_obj_value(obj), &ev, 1);
}

js_value bt_fake_add_listener(js_ctx *c, js_value t, js_value *a, int n)
{
  (void)c;
  (void)t;
  (void)a;
  (void)n;
  return js_undef();
}
js_value bt_fake_remove_listener(js_ctx *c, js_value t, js_value *a, int n) { return bt_fake_add_listener(c, t, a, n); }
js_value bt_fake_dispatch(js_ctx *c, js_value t, js_value *a, int n) { return bt_fake_add_listener(c, t, a, n); }

static void inject(unsigned tag, const char *kind, unsigned handle, const char *hex)
{
  evq[nev].seq = ++ev_seq;
  evq[nev].tag = tag;
  snprintf(evq[nev].text, sizeof evq[nev].text, "%u %u %s %04x %s\n", ev_seq, tag, kind, handle, hex);
  nev++;
}

int bt_fake_readfile(const char *name, char *buf, int max)
{
  char text[3000] = "";
  if (!adapter)
    return -1;
  if (!strcmp(name, STATUS_PATH))
    snprintf(text, sizeof text, "state ready\naddress 66:55:44:33:22:11\n%s",
             link_up ? "connection a6:a5:a4:a3:a2:a1 le encrypted\n" : "");
  else if (!strcmp(name, LE_DEVICES_PATH))
    snprintf(text, sizeof text, "%s", le_text);
  else if (!strcmp(name, GATT_PATH))
    snprintf(text, sizeof text, "%s", link_up ? gatt_text : "");
  else if (!strcmp(name, EVENTS_PATH))
    for (int i = 0; i < nev; i++)
      strcat(text, evq[i].text);
  int n = (int)strlen(text) < max ? (int)strlen(text) : max;
  memcpy(buf, text, (size_t)n);
  return n;
}

int bt_fake_writefile(const char *name, const void *buf, int len)
{
  char line[900];
  if (strcmp(name, CONTROL_PATH) || len >= (int)sizeof line)
    return -1;
  memcpy(line, buf, (size_t)len);
  line[len] = 0;
  snprintf(cmds[ncmds++ % 64], 900, "%s", line);
  if (!strncmp(line, "connect ", 8) && connect_works) {
    link_up = true;
    nev = 0;
    ev_seq = 0;
  } else if (!strcmp(line, "disconnect")) {
    link_up = false;
  } else if (!strncmp(line, "att ack ", 8) && !drop_acks) {
    unsigned seq = (unsigned)atoi(line + 8), keep = 0;
    for (int i = 0; i < nev; i++)
      if (evq[i].seq > seq)
        evq[keep++] = evq[i];
    nev = (int)keep;
  }
  return len;
}

static const char *last_cmd(void) { return ncmds ? cmds[(ncmds - 1) % 64] : ""; }

static unsigned last_tag(void)
{
  const char *c = last_cmd();
  return !strncmp(c, "att ", 4) ? (unsigned)atoi(c + 4) : 0;
}

static bool sent(const char *prefix)
{
  for (int i = 0; i < ncmds && i < 64; i++)
    if (!strncmp(cmds[i], prefix, strlen(prefix)))
      return true;
  return false;
}

static js_object *navigator;
static js_arena arenas[1024];
static int narenas;

static void con_print(js_ctx *c, int level, const char *s, int len)
{
  (void)c;
  (void)level;
  (void)s;
  (void)len;
}

static js_value test_expect(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)t;
  expect(argc > 0 && js_to_boolean(args[0]));
  return js_undef();
}

static void run(const char *source)
{
  js_arena *arena = &arenas[narenas++];
  js_parse_error error = {0, 0};
  js_node *program;
  js_value result;
  js_arena_init(arena);
  program = js_parse(source, arena, &error);
  expect(program != 0);
  if (program)
    expect(js_run(g_ctx, program, &result) == 0);
}

static void tick(int n)
{
  while (n-- > 0) {
    clock_ms += 30;
    bluetooth_tick();
  }
}

static void setup(void)
{
  bluetooth_teardown();
  adapter = connect_works = true;
  drop_acks = false;
  link_up = false;
  ncmds = nev = 0;
  ev_seq = 0;
  dialog_pick = 0;
  dialog_calls = 0;
  snprintf(le_text, sizeof le_text,
           "a6:a5:a4:a3:a2:a1 1 -40 180f Batt\nb6:b5:b4:b3:b2:b1 0 -50 - Mouse\n"
           "c6:c5:c4:c3:c2:c1 1 -60 1812 Keyboard\n");
  snprintf(gatt_text, sizeof gatt_text,
           "state ready\nservice 180f handles 0010-0016\n  characteristic 2a19 handle 0012 props 1a\n"
           "  characteristic 2a1c handle 0015 props 20\n");
  bluetooth_install(g_ctx, navigator, "https://example.com/page");
  run("var R = {}, L = [];"
      "function done(k, p) { p.then(function (v) { R[k] = ['ok', v]; }, function (e) { R[k] = ['err', e.name]; }); }");
}

/* A device chosen and connected, with the battery service and both characteristics fetched into globals. */
static void connected(void)
{
  setup();
  run("done('dev', navigator.bluetooth.requestDevice({filters: [{services: ['battery_service']}]}));");
  tick(200);
  run("expect(R.dev && R.dev[0] === 'ok'); var dev = R.dev[1]; done('conn', dev.gatt.connect());");
  tick(10);
  run("expect(R.conn && R.conn[0] === 'ok' && dev.gatt.connected === true);"
      "dev.ongattserverdisconnected = function (e) { L.push('disc'); };"
      "done('svc', dev.gatt.getPrimaryService('battery_service'));");
  run("expect(R.svc[0] === 'ok'); var svc = R.svc[1]; done('chr', svc.getCharacteristic('battery_level'));"
      "done('chr2', svc.getCharacteristic(0x2a1c));");
  run("expect(R.chr[0] === 'ok' && R.chr2[0] === 'ok'); var chr = R.chr[1], ind = R.chr2[1];"
      "chr.oncharacteristicvaluechanged = function (e) { L.push('v' + e.target.value.getUint8(0)); };");
}

static void test_uuids_and_context(void)
{
  setup();
  run("expect(BluetoothUUID.canonicalUUID(0x180f) === '0000180f-0000-1000-8000-00805f9b34fb');"
      "expect(BluetoothUUID.getService('battery_service') === '0000180f-0000-1000-8000-00805f9b34fb');"
      "expect(BluetoothUUID.getCharacteristic('battery_level') === '00002a19-0000-1000-8000-00805f9b34fb');"
      "var bad = 0; try { BluetoothUUID.getService('nope'); } catch (e) { bad = 1; } expect(bad === 1);");
  js_object *insecure = js_new_object(g_ctx, g_ctx->object_proto);
  bluetooth_teardown();
  bluetooth_install(g_ctx, insecure, "http://example.com/");
  expect(js_get(g_ctx, js_obj_value(insecure), "bluetooth").type == JV_UNDEF);
  bluetooth_teardown();
}

static void test_request_device_validation(void)
{
  setup();
  run("done('none', navigator.bluetooth.requestDevice());"
      "done('both', navigator.bluetooth.requestDevice({acceptAllDevices: true, filters: [{name: 'x'}]}));"
      "done('empty', navigator.bluetooth.requestDevice({filters: [{}]}));"
      "done('badsvc', navigator.bluetooth.requestDevice({filters: [{services: ['nope']}]}));"
      "done('notarray', navigator.bluetooth.requestDevice({filters: 5}));");
  run("expect(R.none[1] === 'TypeError' && R.both[1] === 'TypeError' && R.empty[1] === 'TypeError');"
      "expect(R.badsvc[1] === 'TypeError' && R.notarray[1] === 'TypeError');");
  adapter = false;
  run("done('absent', navigator.bluetooth.requestDevice({acceptAllDevices: true})); done('avail', "
      "navigator.bluetooth.getAvailability());");
  run("expect(R.absent[1] === 'NotFoundError' && R.avail[1] === false);");
  adapter = true;
  run("done('avail2', navigator.bluetooth.getAvailability());");
  run("expect(R.avail2[1] === true);");
  expect(!sent("scan on"));
}

static void test_chooser(void)
{
  setup();
  run("done('dev', navigator.bluetooth.requestDevice({filters: [{services: ['battery_service']}]}));");
  expect(sent("scan on"));
  tick(50);
  run("expect(!R.dev);");
  expect_eq(dialog_calls, 0);
  tick(120);
  expect_eq(dialog_calls, 1);
  expect(sent("scan off"));
  expect_string(dialog_items, "Batt (a6:a5:a4:a3:a2:a1)\n");
  expect(strstr(dialog_msg, "example.com") != NULL);
  run("expect(R.dev[0] === 'ok' && R.dev[1].id === 'a6:a5:a4:a3:a2:a1' && R.dev[1].name === 'Batt');"
      "expect(typeof R.dev[1].gatt.connect === 'function' && R.dev[1].gatt.connected === false);");
  run("done('again', navigator.bluetooth.requestDevice({filters: [{services: ['battery_service']}]}));"
      "done('overlap', navigator.bluetooth.requestDevice({acceptAllDevices: true}));");
  run("expect(R.overlap[1] === 'NotAllowedError');");
  dialog_pick = -1;
  tick(300);
  run("expect(R.again[1] === 'NotAllowedError');");

  run("done('all', navigator.bluetooth.requestDevice({acceptAllDevices: true}));");
  dialog_pick = 1;
  tick(200);
  expect_string(dialog_items, "Batt (a6:a5:a4:a3:a2:a1)\nMouse (b6:b5:b4:b3:b2:b1)\nKeyboard (c6:c5:c4:c3:c2:c1)\n");
  run("expect(R.all[0] === 'ok' && R.all[1].id === 'b6:b5:b4:b3:b2:b1' && R.all[1].name === 'Mouse');");

  dialog_calls = 0;
  run("done('prefix', navigator.bluetooth.requestDevice({filters: [{namePrefix: 'Mou'}, {name: 'zzz'}]}));");
  dialog_pick = 0;
  tick(200);
  expect_string(dialog_items, "Mouse (b6:b5:b4:b3:b2:b1)\n");
  run("expect(R.prefix[0] === 'ok' && R.prefix[1].name === 'Mouse');");

  run("done('none', navigator.bluetooth.requestDevice({filters: [{name: 'zzz'}]}));");
  dialog_calls = 0;
  tick(500);
  run("expect(R.none[1] === 'NotFoundError');");
  expect_eq(dialog_calls, 0);
  expect(sent("scan off"));
}

static void test_connect_and_discovery(void)
{
  connected();
  expect(sent("connect a6:a5:a4:a3:a2:a1"));
  run("done('conn2', dev.gatt.connect());");
  run("expect(R.conn2[0] === 'ok' && R.conn2[1] === dev.gatt);");
  run("done('none', dev.gatt.getPrimaryService('heart_rate')); done('all', dev.gatt.getPrimaryServices());"
      "done('badname', dev.gatt.getPrimaryService('bogus')); done('same', dev.gatt.getPrimaryService(0x180f));");
  run("expect(R.none[1] === 'NotFoundError' && R.badname[1] === 'TypeError');"
      "expect(R.all[1].length === 1 && R.all[1][0] === svc && R.same[1] === svc);"
      "expect(svc.uuid === '0000180f-0000-1000-8000-00805f9b34fb' && svc.isPrimary === true && svc.device === dev);");
  run("done('chars', svc.getCharacteristics()); done('missing', svc.getCharacteristic('heart_rate_measurement'));"
      "done('again', svc.getCharacteristic('battery_level')); done('desc', chr.getDescriptor(0x2902));");
  run("expect(R.chars[1].length === 2 && R.chars[1][0] === chr && R.chars[1][1] === ind && R.again[1] === chr);"
      "expect(R.missing[1] === 'NotFoundError' && R.desc[1] === 'NotSupportedError');"
      "expect(chr.uuid === '00002a19-0000-1000-8000-00805f9b34fb' && chr.service === svc && chr.value === null);"
      "var p = chr.properties; expect(p.read && p.write && p.notify && !p.indicate && !p.writeWithoutResponse);"
      "expect(ind.properties.indicate && !ind.properties.read);");

  run("dev.gatt.disconnect();");
  run("expect(dev.gatt.connected === false && L.length === 1 && L[0] === 'disc');"
      "done('after', chr.readValue()); done('svcafter', dev.gatt.getPrimaryServices());");
  run("expect(R.after[1] === 'NetworkError' && R.svcafter[1] === 'NetworkError');");
  expect(sent("disconnect"));
}

static void test_read_write_and_errors(void)
{
  connected();
  run("done('r1', chr.readValue()); done('r2', chr.readValue());");
  expect(!strncmp(last_cmd(), "att 1 read 12", 13));
  expect_eq(ncmds, 4);
  inject(last_tag(), "read", 0x12, "5a");
  tick(3);
  expect_eq(ncmds, 6);
  expect(!strncmp(last_cmd(), "att 2 read 12", 13));
  run("expect(R.r1[0] === 'ok' && R.r1[1].getUint8(0) === 0x5a && chr.value === R.r1[1]);"
      "expect(L.length === 1 && L[0] === 'v90');");
  inject(2, "read", 0x12, "ff00");
  tick(3);
  run("expect(R.r2[0] === 'ok' && R.r2[1].byteLength === 2 && R.r2[1].getUint8(0) === 255);");

  run("done('w', chr.writeValue(new Uint8Array([1, 2, 255])));");
  tick(2);
  expect(sent("att 3 write 12 0102ff"));
  inject(3, "write", 0x12, "");
  tick(3);
  run("expect(R.w[0] === 'ok' && R.w[1] === undefined);");

  run("done('nornd', chr.writeValueWithoutResponse(new Uint8Array([1]))); done('big', chr.writeValue(new "
      "Uint8Array(300)));"
      "done('notbuf', chr.writeValue('text')); done('indread', ind.readValue());");
  run("expect(R.nornd[1] === 'NotSupportedError' && R.big[1] === 'InvalidModificationError');"
      "expect(R.notbuf[1] === 'TypeError' && R.indread[1] === 'NotSupportedError');");

  run("done('rerr', chr.readValue());");
  tick(2);
  inject(4, "err", 0x12, "05");
  tick(3);
  run("expect(R.rerr[1] === 'NotAllowedError');");
  run("done('rerr2', chr.readValue());");
  tick(2);
  inject(5, "err", 0x12, "ff");
  tick(3);
  run("expect(R.rerr2[1] === 'NetworkError');");

  run("done('slow', chr.readValue());");
  tick(2);
  tick(500);
  run("expect(R.slow[1] === 'NetworkError');");
}

static void test_notifications(void)
{
  connected();
  run("done('on', chr.startNotifications());");
  tick(2);
  expect(sent("att 1 notify 12 on"));
  inject(1, "write", 0x13, "");
  tick(3);
  run("expect(R.on[0] === 'ok' && R.on[1] === chr);");

  inject(0, "notify", 0x12, "64");
  inject(0, "notify", 0x12, "6e");
  tick(3);
  run("expect(L.length === 2 && L[0] === 'v100' && L[1] === 'v110' && chr.value.getUint8(0) === 110);");
  expect(sent("att ack 3"));
  expect_eq(nev, 0);

  run("done('ind', ind.startNotifications()); done('off', chr.stopNotifications());");
  tick(2);
  expect(sent("att 2 notify 15 on"));
  inject(2, "write", 0x16, "");
  tick(3);
  expect(sent("att 3 notify 12 off"));
  inject(3, "write", 0x13, "");
  tick(3);
  run("expect(R.ind[1] === ind && R.off[1] === chr);");
  inject(0, "notify", 0x12, "01");
  tick(3);
  run("expect(L.length === 2);");

  run("done('on2', chr.startNotifications());");
  tick(2);
  inject(last_tag(), "write", 0x13, "");
  tick(3);
  drop_acks = true;
  inject(0, "notify", 0x12, "02");
  tick(20);
  run("expect(L.length === 3 && L[2] === 'v2');");
  expect_eq(nev, 1);
  drop_acks = false;
  tick(3);
  expect_eq(nev, 0);
  run("expect(L.length === 3);");
}

static void test_link_loss_and_reconnect(void)
{
  connected();
  run("done('on', chr.startNotifications()); done('r', chr.readValue());");
  tick(2);
  link_up = false;
  tick(20);
  run("expect(dev.gatt.connected === false && L.length === 1 && L[0] === 'disc');"
      "expect(R.on[1] === 'NetworkError' && R.r[1] === 'NetworkError');"
      "done('r2', chr.readValue());");
  run("expect(R.r2[1] === 'NetworkError');");

  run("done('again', dev.gatt.connect());");
  tick(10);
  run("expect(R.again[0] === 'ok' && dev.gatt.connected === true);"
      "done('svc2', dev.gatt.getPrimaryService('battery_service'));");
  run("done('r3', R.svc2[1].getCharacteristic('battery_level').then(function (c) { return c.readValue(); }));");
  tick(5);
  inject(last_tag(), "read", 0x12, "07");
  tick(5);
  run("expect(R.r3 && R.r3[0] === 'ok' && R.r3[1].getUint8(0) === 7);");
}

static void test_connect_failures(void)
{
  setup();
  connect_works = false;
  run("done('dev', navigator.bluetooth.requestDevice({acceptAllDevices: true}));");
  tick(200);
  run("var dev = R.dev[1]; done('c1', dev.gatt.connect()); done('c2', dev.gatt.connect());");
  run("expect(R.c2[1] === 'NetworkError');");
  tick(2000);
  run("expect(R.c1[1] === 'NetworkError' && dev.gatt.connected === false);");
  expect(!sent("disconnect"));

  connect_works = true;
  run("done('c3', dev.gatt.connect());");
  tick(10);
  run("expect(R.c3[0] === 'ok');");
  bluetooth_teardown();
  expect_string(last_cmd(), "disconnect");
}

int main(void)
{
  g_ctx = js_ctx_new(con_print);
  navigator = js_new_object(g_ctx, g_ctx->object_proto);
  js_def(g_ctx->global, "navigator", js_obj_value(navigator), P_WRITABLE);
  js_def(g_ctx->global, "window", js_obj_value(g_ctx->global), P_WRITABLE);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

  test("UUID helpers and secure-context gating", test_uuids_and_context);
  test("requestDevice validation", test_request_device_validation);
  test("requestDevice scan, filters and chooser", test_chooser);
  test("connect and service discovery", test_connect_and_discovery);
  test("reads, writes and GATT errors", test_read_write_and_errors);
  test("notifications", test_notifications);
  test("link loss and reconnect", test_link_loss_and_reconnect);
  test("connect failure and teardown", test_connect_failures);

  js_ctx_free(g_ctx);
  return 0;
}
