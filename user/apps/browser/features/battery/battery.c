#include "features/battery/battery.h"
#include "features/clienthints/clienthints.h"
#include "jsint.h"

#define CAPACITY_PATH "/sys/power/battery/capacity"
#define STATUS_PATH "/sys/power/battery/status"
#define MINUTES_PATH "/sys/power/battery/minutes"
#define POLL_MS 2000

struct bat_state {
  bool charging;
  double level, charging_time, discharging_time;
};

static js_ctx *ctx;
static js_object *manager;
static struct bat_state cur;
static unsigned last_poll;

static int read_uint(const char *path)
{
  char buf[16];
  int n = readfile(path, buf, sizeof buf - 1), v = 0;
  if (n <= 0 || buf[0] < '0' || buf[0] > '9')
    return -1;
  for (int i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
    v = v * 10 + (buf[i] - '0');
  return v;
}

/* No battery node (desktop, QEMU) reports a full, plugged-in battery, as other browsers do. */
static struct bat_state read_battery(void)
{
  struct bat_state s = {true, 1, 0, 1.0 / 0.0};
  char status[16];
  int pct = read_uint(CAPACITY_PATH), n = readfile(STATUS_PATH, status, sizeof status - 1);
  if (pct < 0 || n <= 0)
    return s;
  status[n - (status[n - 1] == '\n')] = 0;
  s.level = (pct > 100 ? 100 : pct) / 100.0;
  if (streq(status, "Full"))
    return s;
  s.charging_time = 1.0 / 0.0;
  if (!streq(status, "Charging")) {
    int minutes = read_uint(MINUTES_PATH);
    s.charging = false;
    if (minutes > 0)
      s.discharging_time = minutes * 60.0;
  }
  return s;
}

static void publish(const struct bat_state *s)
{
  js_def(manager, "charging", js_bool(s->charging), P_ENUMERABLE);
  js_def(manager, "chargingTime", js_number(s->charging_time), P_ENUMERABLE);
  js_def(manager, "dischargingTime", js_number(s->discharging_time), P_ENUMERABLE);
  js_def(manager, "level", js_number(s->level), P_ENUMERABLE);
}

static void fire(const char *type)
{
  gc_push_scope();
  js_value ev = make_event(type, 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(ctx, ev, "bubbles", js_bool(0));
    js_set(ctx, ev, "target", js_obj_value(manager));
    ev_fire_on(manager, ev, 0);
  }
  gc_pop_scope();
}

static js_value get_battery(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  if (!manager) {
    manager = js_new_object(c, c->object_proto);
    gc_add_root((void **)&manager);
    def_method(manager, "addEventListener", el_add_listener, 3);
    def_method(manager, "removeEventListener", el_remove_listener, 3);
    def_method(manager, "dispatchEvent", el_dispatch, 1);
    cur = read_battery();
    publish(&cur);
    last_poll = msec();
  }
  return js_promise_resolved(c, js_obj_value(manager));
}

void battery_install(js_ctx *c, js_object *navigator, const char *page_url)
{
  ctx = c;
  manager = 0;
  if (ch_secure_url(page_url))
    def_method(navigator, "getBattery", get_battery, 0);
}

void battery_tick(void)
{
  struct bat_state next;
  unsigned now = msec();
  if (!manager || now - last_poll < POLL_MS)
    return;
  last_poll = now;
  next = read_battery();
  if (next.charging == cur.charging && next.level == cur.level && next.charging_time == cur.charging_time &&
      next.discharging_time == cur.discharging_time)
    return;
  struct bat_state old = cur;
  cur = next;
  publish(&cur);
  if (cur.charging != old.charging)
    fire("chargingchange");
  if (cur.level != old.level)
    fire("levelchange");
  if (cur.charging_time != old.charging_time)
    fire("chargingtimechange");
  if (cur.discharging_time != old.discharging_time)
    fire("dischargingtimechange");
}

void battery_teardown(void)
{
  if (manager)
    gc_remove_root((void **)&manager);
  manager = 0;
  ctx = 0;
}
