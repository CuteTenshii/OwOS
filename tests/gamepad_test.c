/* Host test for navigator.getGamepads, the gamepad events and rumble (features/gamepad). */
#define OWOS_SUITE_TEST
#include "jsint.h"
#define msec fake_msec
#define readfile fake_readfile
#define writefile fake_writefile
unsigned fake_msec(void);
int fake_readfile(const char *name, char *buf, int max);
int fake_writefile(const char *name, const void *buf, int len);
#include "features/gamepad/gamepad.c"
#include "features/gamepad/gamepad_state.c"
#include <stdio.h>
#include <string.h>

static unsigned now;
static char state_text[128], rumble_text[8] = "1\n", rumble_written[48];
static bool pad_attached;

unsigned fake_msec(void) { return now; }
int fake_readfile(const char *name, char *buf, int max)
{
  const char *src = !strcmp(name, STATE_PATH) ? (pad_attached ? state_text : 0) : rumble_text;
  if (!src || (int)strlen(src) > max)
    return -1;
  memcpy(buf, src, strlen(src));
  return (int)strlen(src);
}
int fake_writefile(const char *name, const void *buf, int len)
{
  if (strcmp(name, RUMBLE_PATH) || len >= (int)sizeof rumble_written)
    return -1;
  memcpy(rumble_written, buf, (size_t)len);
  rumble_written[len] = 0;
  return 0;
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

static void poll(const char *line)
{
  now += POLL_MS;
  if (line)
    snprintf(state_text, sizeof state_text, "%s", line);
  pad_attached = line != 0;
  gamepad_tick();
}

static void test_state_parsing(void)
{
  struct gp_state st;
  expect(gp_parse("7 5 3 -32768 32767 0 100 -100 8 0 0\n", &st));
  expect_eq(st.seq, 7);
  expect(gp_button(&st, 0) && !gp_button(&st, 1) && gp_button(&st, 2));
  expect(!gp_button(&st, 16) && !gp_button(&st, 17) && !gp_button(&st, 18) && gp_button(&st, 19));
  expect(gp_axis(&st, 0) == -1 && gp_axis(&st, 1) < 1 && gp_axis(&st, 1) > 0.999);
  expect(!gp_parse("7 5 3 1 2", &st));
  expect(!gp_parse("", &st));
  expect(!gp_parse("a b c d e f g h i j k", &st));
}

static void test_hat_directions(void)
{
  static const char up[9][5] = {"0000", "1000", "1001", "0001", "0101", "0100", "0110", "0010", "1010"};
  for (int hat = 0; hat < 9; hat++) {
    struct gp_state st = {0, 0, hat, {0}};
    for (int i = 0; i < 4; i++)
      expect_eq(gp_button(&st, GP_PAD_BUTTONS + i), up[hat][i] == '1');
  }
}

static void test_hidden_until_pressed_then_events(void)
{
  run("var log=[]; "
      "window.ongamepadconnected=function(e){log.push('c'+e.gamepad.index+e.gamepad.connected+e.gamepad.buttons[0]."
      "pressed);};"
      "window.ongamepaddisconnected=function(e){log.push('d'+e.gamepad.connected);};");
  poll("1 0 0 0 0 0 0 0 0 0 0");
  run("var g=navigator.getGamepads(); expect(g.length===4 && g[0]===null && g[1]===null); expect(log.length===0);");
  poll("2 1 0 16384 -16384 0 0 0 0 0 0");
  run("expect(log.length===1 && log[0]==='c0truetrue');"
      "var p=navigator.getGamepads()[0];"
      "expect(p.id==='OwOS Gamepad' && p.index===0 && p.connected===true && p.mapping==='');"
      "expect(p.axes.length===6 && p.axes[0]===0.5 && p.axes[1]===-0.5);"
      "expect(p.buttons.length===20 && p.buttons[0].pressed && p.buttons[0].value===1 && !p.buttons[1].pressed);");
  poll("3 0 1 0 0 0 0 0 0 0 0");
  run("expect(log.length===1);"
      "var q=navigator.getGamepads()[0]; expect(q.buttons[16].pressed && !q.buttons[17].pressed);");
  poll(0);
  run("expect(log.length===2 && log[1]==='dfalse'); expect(navigator.getGamepads()[0]===null);");
}

static void test_rumble(void)
{
  poll("4 1 0 0 0 0 0 0 0 0 0");
  run("var a=navigator.getGamepads()[0].vibrationActuator; expect(a && a.type==='dual-rumble');"
      "expect(typeof "
      "a.playEffect('dual-rumble',{duration:200,strongMagnitude:1,weakMagnitude:0.5}).then==='function');");
  expect_string(rumble_written, "65535 32767 200");
  run("navigator.getGamepads()[0].vibrationActuator.playEffect('trigger-rumble',{duration:50,strongMagnitude:1});");
  expect_string(rumble_written, "65535 32767 200");
  expect(rumbling);
  run("navigator.getGamepads()[0].vibrationActuator.reset();");
  expect_string(rumble_written, "0 0 0");
  expect(!rumbling);
  poll(0);

  strcpy(rumble_text, "0\n");
  poll("5 0 0 0 0 0 0 0 0 0 0");
  poll("6 1 0 0 0 0 0 0 0 0 0");
  run("expect(navigator.getGamepads()[0].vibrationActuator===null);");
  poll(0);
}

static void test_teardown_stops_rumble(void)
{
  rumble_text[0] = '1';
  poll("7 1 0 0 0 0 0 0 0 0 0");
  rumble(1, 1, 1000);
  gamepad_teardown();
  expect_string(rumble_written, "0 0 0");
}

int main(void)
{
  g_ctx = js_ctx_new(con_print);
  navigator = js_new_object(g_ctx, g_ctx->object_proto);
  js_def(g_ctx->global, "navigator", js_obj_value(navigator), P_WRITABLE);
  js_def(g_ctx->global, "window", js_obj_value(g_ctx->global), P_WRITABLE);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);
  gamepad_install(g_ctx, navigator);

  test("state line parsing", test_state_parsing);
  test("hat to d-pad buttons", test_hat_directions);
  test("pad is hidden until pressed, connect and disconnect events", test_hidden_until_pressed_then_events);
  test("rumble through the actuator", test_rumble);
  test("teardown stops a running rumble", test_teardown_stops_rumble);

  js_ctx_free(g_ctx);
  return 0;
}
