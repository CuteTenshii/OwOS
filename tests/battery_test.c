/* Host test for navigator.getBattery and the BatteryManager events (features/battery). */
#define OWOS_SUITE_TEST
#include "jsint.h"
#define msec battery_msec
#define readfile battery_readfile
#define make_event battery_make_event
#define ev_fire_on battery_ev_fire_on
#define el_add_listener battery_el_add_listener
#define el_remove_listener battery_el_remove_listener
#define el_dispatch battery_el_dispatch
#define ch_secure_url battery_ch_secure_url
unsigned battery_msec(void);
int battery_readfile(const char *name, char *buf, int max);
js_value make_event(const char *type, int x, int y, js_value init);
void ev_fire_on(js_object *obj, js_value ev, int want_capture);
js_value el_add_listener(js_ctx *c, js_value t, js_value *args, int argc);
js_value el_remove_listener(js_ctx *c, js_value t, js_value *args, int argc);
js_value el_dispatch(js_ctx *c, js_value t, js_value *args, int argc);
bool ch_secure_url(const char *url);
#include "features/battery/battery.c"
#include <stdio.h>
#include <string.h>

static unsigned now;
static char capacity_text[8], status_text[16], minutes_text[8];

unsigned battery_msec(void) { return now; }
int battery_readfile(const char *name, char *buf, int max)
{
  const char *src = !strcmp(name, CAPACITY_PATH) ? capacity_text : !strcmp(name, STATUS_PATH) ? status_text : minutes_text;
  if (!src[0] || (int)strlen(src) > max)
    return -1;
  memcpy(buf, src, strlen(src));
  return (int)strlen(src);
}

js_value make_event(const char *type, int x, int y, js_value init)
{
  js_object *e = js_new_object(g_ctx, g_ctx->object_proto);
  (void)x;
  (void)y;
  (void)init;
  js_def(e, "type", js_mk_cstring(g_ctx, type), P_WRITABLE | P_ENUMERABLE);
  return js_obj_value(e);
}
void ev_fire_on(js_object *obj, js_value ev, int want_capture)
{
  char on[48] = "on";
  js_value h;
  (void)want_capture;
  strcat(on, js_get(g_ctx, ev, "type").u.str->data);
  h = js_get(g_ctx, js_obj_value(obj), on);
  if (js_is_callable(h))
    js_call(g_ctx, h, js_obj_value(obj), &ev, 1);
}
js_value el_add_listener(js_ctx *c, js_value t, js_value *args, int argc) { return js_undef(); }
js_value el_remove_listener(js_ctx *c, js_value t, js_value *args, int argc) { return js_undef(); }
js_value el_dispatch(js_ctx *c, js_value t, js_value *args, int argc) { return js_undef(); }
bool ch_secure_url(const char *url) { return !strncmp(url, "https:", 6); }

static js_object *navigator;
static js_arena arenas[16];
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

static void set_battery(const char *pct, const char *status, const char *minutes)
{
  snprintf(capacity_text, sizeof capacity_text, "%s", pct);
  snprintf(status_text, sizeof status_text, "%s", status);
  snprintf(minutes_text, sizeof minutes_text, "%s", minutes);
}

static void poll(void)
{
  now += POLL_MS;
  battery_tick();
}

static void test_no_battery(void)
{
  set_battery("", "", "");
  run("var bm; navigator.getBattery().then(function (b) { bm = b; });");
  run("expect(bm.charging === true && bm.level === 1 && bm.chargingTime === 0 && bm.dischargingTime === Infinity);");
}

static void test_events_and_values(void)
{
  set_battery("80\n", "Discharging\n", "90\n");
  run("var log = [];"
      "['chargingchange', 'levelchange', 'chargingtimechange', 'dischargingtimechange'].forEach(function (t) {"
      "  bm['on' + t] = function (e) { log.push(t); };"
      "});");
  poll();
  run("expect(bm.charging === false && bm.level === 0.8 && bm.dischargingTime === 5400 && bm.chargingTime === Infinity);"
      "expect(log.join() === 'chargingchange,levelchange,chargingtimechange,dischargingtimechange');"
      "log.length = 0;");
  poll();
  run("expect(log.length === 0);");

  set_battery("79\n", "Discharging\n", "85\n");
  poll();
  run("expect(log.join() === 'levelchange,dischargingtimechange' && bm.level === 0.79);"
      "log.length = 0;");

  set_battery("79\n", "Charging\n", "0\n");
  poll();
  run("expect(bm.charging === true && bm.chargingTime === Infinity && bm.dischargingTime === Infinity);"
      "expect(log.join() === 'chargingchange,dischargingtimechange');"
      "log.length = 0;");

  set_battery("100\n", "Full\n", "0\n");
  poll();
  run("expect(bm.charging === true && bm.level === 1 && bm.chargingTime === 0);"
      "expect(log.join() === 'levelchange,chargingtimechange');");
}

static void test_poll_interval(void)
{
  set_battery("50\n", "Discharging\n", "60\n");
  run("var n = 0; bm.onlevelchange = function () { n++; };");
  now += POLL_MS - 1;
  battery_tick();
  run("expect(n === 0);");
  now++;
  battery_tick();
  run("expect(n === 1);");
}

static void test_same_manager(void)
{
  run("var same; navigator.getBattery().then(function (b) { same = b === bm; });");
  run("expect(same);");
}

int main(void)
{
  g_ctx = js_ctx_new(con_print);
  navigator = js_new_object(g_ctx, g_ctx->object_proto);
  js_def(g_ctx->global, "navigator", js_obj_value(navigator), P_WRITABLE);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);
  battery_install(g_ctx, navigator, "http://example.com/");
  run("expect(navigator.getBattery === undefined);");
  battery_install(g_ctx, navigator, "https://example.com/");

  test("a missing battery reads as full and plugged in", test_no_battery);
  test("values and change events", test_events_and_values);
  test("polling interval", test_poll_interval);
  test("getBattery resolves the same manager", test_same_manager);

  battery_teardown();
  js_ctx_free(g_ctx);
  return 0;
}
