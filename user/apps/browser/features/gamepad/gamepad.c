#include "features/gamepad/gamepad.h"
#include "features/gamepad/gamepad_state.h"
#include "jsint.h"

#define STATE_PATH "/sys/class/input/js0/state"
#define RUMBLE_PATH "/sys/class/input/js0/rumble"
#define ID_PATH "/sys/class/input/js0/id"
#define POLL_MS 8
#define SLOTS 4
#define PRESS_THRESHOLD 0.5

static js_ctx *ctx;
static js_object *actuator;
static struct gp_state cur;
static struct gp_id pad_id;
static bool present, visible, can_rumble, rumbling;
static unsigned last_poll;

static void rumble(double strong, double weak, double ms)
{
  char cmd[40];
  int s = (int)(strong * 65535), w = (int)(weak * 65535);
  rumbling = ms > 0 && (s || w);
  snprintf(cmd, sizeof cmd, "%d %d %d", s, w, (int)ms);
  writefile(RUMBLE_PATH, cmd, (int)strlen(cmd));
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static js_value actuator_play(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value p = A(1);
  double dur = 0, strong = 0, weak = 0;
  (void)t;
  if (!streq(arg_str(c, A(0)), "dual-rumble"))
    return js_promise_rejected(c, js_make_error(c, c->type_error_proto, "TypeError", "Unsupported effect type"));
  if (p.type == JV_OBJ) {
    dur = js_to_number(c, js_get(c, p, "duration"));
    strong = clamp01(js_to_number(c, js_get(c, p, "strongMagnitude")));
    weak = clamp01(js_to_number(c, js_get(c, p, "weakMagnitude")));
  }
  rumble(strong, weak, dur != dur || dur < 0 ? 0 : dur);
  return js_promise_resolved(c, js_mk_cstring(c, "complete"));
}

static js_value actuator_reset(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  rumble(0, 0, 0);
  return js_promise_resolved(c, js_mk_cstring(c, "complete"));
}

static js_value snapshot(bool connected)
{
  js_object *pad = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_object *axes = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_object *buttons = (js_object *)gc_keep(js_new_array(ctx, 0));
  enum gp_kind kind = pad_id.kind;
  char name[96];
  for (int i = 0; i < gp_naxes(kind); i++)
    js_array_push(ctx, axes, js_number(gp_axis(kind, &cur, i)));
  for (int i = 0; i < gp_nbuttons(kind); i++) {
    js_object *b = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    double value = gp_button(kind, &cur, i);
    js_def(b, "pressed", js_bool(value > PRESS_THRESHOLD), P_ENUMERABLE);
    js_def(b, "touched", js_bool(value > 0), P_ENUMERABLE);
    js_def(b, "value", js_number(value), P_ENUMERABLE);
    js_array_push(ctx, buttons, js_obj_value(b));
  }
  if (kind == GP_GENERIC)
    snprintf(name, sizeof name, "OwOS Gamepad");
  else if (pad_id.vid)
    snprintf(name, sizeof name, "%s (STANDARD GAMEPAD Vendor: %04x Product: %04x)", pad_id.label, pad_id.vid,
             pad_id.pid);
  else
    snprintf(name, sizeof name, "%s (STANDARD GAMEPAD)", pad_id.label);
  js_def(pad, "id", js_mk_cstring(ctx, name), P_ENUMERABLE);
  js_def(pad, "index", js_number(0), P_ENUMERABLE);
  js_def(pad, "connected", js_bool(connected), P_ENUMERABLE);
  js_def(pad, "mapping", js_mk_cstring(ctx, gp_standard(kind) ? "standard" : ""), P_ENUMERABLE);
  js_def(pad, "timestamp", js_number(msec()), P_ENUMERABLE);
  js_def(pad, "axes", js_obj_value(axes), P_ENUMERABLE);
  js_def(pad, "buttons", js_obj_value(buttons), P_ENUMERABLE);
  js_def(pad, "vibrationActuator", can_rumble ? js_obj_value(actuator) : js_null(), P_ENUMERABLE);
  return js_obj_value(pad);
}

static void fire(const char *type, bool connected)
{
  gc_push_scope();
  js_value pad = snapshot(connected);
  js_value ev = make_event(type, 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(ctx, ev, "bubbles", js_bool(0));
    js_set(ctx, ev, "target", js_obj_value(ctx->global));
    js_def(ev.u.obj, "gamepad", pad, P_ENUMERABLE);
    ev_fire_on(ctx->global, ev, 0);
  }
  gc_pop_scope();
}

static js_value get_gamepads(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_object *list;
  js_value v;
  (void)t;
  (void)args;
  (void)argc;
  gamepad_tick();
  gc_push_scope();
  list = (js_object *)gc_keep(js_new_array(c, 0));
  for (int i = 0; i < SLOTS; i++)
    js_array_push(c, list, i == 0 && present && visible ? snapshot(true) : js_null());
  v = js_obj_value(list);
  gc_pop_scope();
  return v;
}

void gamepad_install(js_ctx *c, js_object *navigator)
{
  ctx = c;
  present = visible = rumbling = false;
  last_poll = msec() - POLL_MS;
  actuator = js_new_object(c, c->object_proto);
  gc_add_root((void **)&actuator);
  js_def(actuator, "type", js_mk_cstring(c, "dual-rumble"), P_ENUMERABLE);
  def_method(actuator, "playEffect", actuator_play, 2);
  def_method(actuator, "reset", actuator_reset, 0);
  def_method(navigator, "getGamepads", get_gamepads, 0);
}

void gamepad_tick(void)
{
  char line[128];
  unsigned now = msec();
  int n;
  if (!ctx || now - last_poll < POLL_MS)
    return;
  last_poll = now;
  n = readfile(STATE_PATH, line, sizeof line - 1);
  if (n > 0)
    line[n] = 0;
  if (n <= 0 || !gp_parse(line, &cur)) {
    if (present && visible)
      fire("gamepaddisconnected", false);
    present = visible = false;
    return;
  }
  if (!present) {
    char r[4], id[64];
    int len = readfile(ID_PATH, id, sizeof id - 1);
    present = true;
    if (len > 0)
      id[len] = 0;
    if (len <= 0 || !gp_parse_id(id, &pad_id))
      pad_id.kind = GP_GENERIC;
    can_rumble = readfile(RUMBLE_PATH, r, sizeof r) > 0 && r[0] == '1';
  }
  if (!visible && gp_active(&cur)) {
    visible = true;
    fire("gamepadconnected", true);
  }
}

void gamepad_teardown(void)
{
  if (rumbling)
    rumble(0, 0, 0);
  if (ctx)
    gc_remove_root((void **)&actuator);
  ctx = 0;
  actuator = 0;
}
