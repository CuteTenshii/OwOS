/* Timers (setTimeout/setInterval/requestAnimationFrame) and AbortController. */

#include "bindings.h"

/* ------------------------------------------------------- timers ----------- */

int g_timer_owner;

/* Register a timer; returns its id (slot+1) */
static int timer_add(js_ctx *ctx, js_value cb, js_value *extra, int nextra, int delay, bool repeat, bool raf)
{
  int i, k;
  if (!g_timers || !js_is_callable(cb))
    return 0;
  for (i = 0; i < g_tcap; i++)
    if (!g_tmeta[i].active)
      break;
  if (i == g_tcap) { /* no free slot: grow the table */
    int nc = g_tcap ? g_tcap * 2 : 16;
    struct timer_meta *nt = (struct timer_meta *)realloc(g_tmeta, (unsigned)nc * sizeof *nt);
    if (!nt)
      return 0;
    for (k = g_tcap; k < nc; k++) {
      nt[k].active = nt[k].repeat = nt[k].raf = false;
      nt[k].due = nt[k].period = 0;
      nt[k].async = 0;
      nt[k].owner = 0;
    }
    g_tmeta = nt;
    g_tcap = nc;
  }
  gc_push_scope();
  {
    js_object *bundle = (js_object *)gc_keep(js_new_array(ctx, 0));
    js_array_push(ctx, bundle, cb); /* [0] = callback */
    for (k = 0; k < nextra; k++)
      js_array_push(ctx, bundle, extra[k]); /* [1..] args */
    js_set_index(ctx, js_obj_value(g_timers), (unsigned)i, js_obj_value(bundle));
  }
  gc_pop_scope();
  g_tmeta[i].active = true;
  g_tmeta[i].repeat = repeat;
  g_tmeta[i].raf = raf;
  g_tmeta[i].period = delay < 0 ? 0 : delay;
  g_tmeta[i].due = now_ms() + g_tmeta[i].period;
  g_tmeta[i].async = js_async_capture(ctx); /* who armed this timer */
  g_tmeta[i].owner = g_timer_owner;
  g_ntimers++;
  return i + 1;
}
static void timer_drop(int i)
{
  if (i < 0 || i >= g_tcap || !g_tmeta[i].active)
    return;
  g_tmeta[i].active = false;
  if (g_tmeta[i].async) {
    free(g_tmeta[i].async);
    g_tmeta[i].async = 0;
  }
  if (g_timers)
    js_set_index(g_ctx, js_obj_value(g_timers), (unsigned)i, js_null());
  if (g_ntimers > 0)
    g_ntimers--;
}
js_value win_set_timeout(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int delay = argc > 1 ? js_to_int_sat(ctx, args[1]) : 0;
  (void)t;
  return js_number(timer_add(ctx, A(0), args + (argc > 2 ? 2 : argc), argc > 2 ? argc - 2 : 0, delay, false, false));
}
js_value win_set_interval(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int delay = argc > 1 ? js_to_int_sat(ctx, args[1]) : 0;
  (void)t;
  return js_number(timer_add(ctx, A(0), args + (argc > 2 ? 2 : argc), argc > 2 ? argc - 2 : 0, delay, true, false));
}
js_value win_request_anim(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return js_number(timer_add(ctx, A(0), 0, 0, 16, false, true)); /* ~one 60fps frame */
}
js_value win_clear_timer(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  if (argc > 0 && args[0].type == JV_NUM)
    timer_drop((int)args[0].u.num - 1);
  return js_undef();
}

void timers_drop_owner(int owner)
{
  int i;
  for (i = 0; i < g_tcap; i++)
    if (g_tmeta[i].active && g_tmeta[i].owner == owner)
      timer_drop(i);
}

/* Fire every timer whose deadline has passed */
int js_timers_pump(void)
{
  int i, fired = 0;
  long long now;
  if (!g_page_live || !g_ctx || g_ntimers == 0)
    return 0;
  now = now_ms();
  for (i = 0; i < g_tcap; i++) {
    js_value bundle, cb, argv[8];
    int nargs = 0;
    if (!g_tmeta[i].active || g_tmeta[i].due > now)
      continue;
    bundle = js_get_index(g_ctx, js_obj_value(g_timers), (unsigned)i);
    if (bundle.type != JV_OBJ) {
      timer_drop(i);
      continue;
    }
    gc_push_scope();
    gc_keep(bundle.u.obj);
    cb = js_get_index(g_ctx, bundle, 0);
    if (g_tmeta[i].raf) {
      argv[nargs++] = js_number((double)now);
    } else {
      unsigned bl = js_array_len(bundle.u.obj), k;
      for (k = 1; k < bl && nargs < 8; k++)
        argv[nargs++] = js_get_index(g_ctx, bundle, k);
    }
    bool raf = g_tmeta[i].raf;
    char *async = g_tmeta[i].async; /* scheduling-site frames; realloc-safe local */
    bool oneshot = !g_tmeta[i].repeat;
    int owner = g_tmeta[i].owner;
    if (g_tmeta[i].repeat)
      g_tmeta[i].due = now + g_tmeta[i].period; /* reschedule from now (no drift pileup) */
    else {
      g_tmeta[i].async = 0; /* take ownership before the drop frees it */
      timer_drop(i);        /* one-shot: free the slot before firing */
    }
    if (js_is_callable(cb)) {
      char *prev = js_async_enter(g_ctx, async); /* callback throws → stack names the arm site */
      js_set_cur_script(raf ? "<requestAnimationFrame callback>" : "<timer callback>");
      g_timer_owner = owner;
      js_call(g_ctx, cb, js_obj_value(g_ctx->global), argv, nargs);
      g_timer_owner = 0;
      js_async_leave(g_ctx, prev);
      if (g_ctx->has_exc)
        report_exc(g_ctx);
    }
    if (oneshot)
      free(async); /* repeat timers keep it in the slot */
    gc_pop_scope();
    fired++;
  }
  return fired;
}

/* ------------------------------------------------------- AbortController -- */

/* A fresh, not-yet-aborted AbortSignal. */
static js_object *new_abort_signal(js_ctx *ctx)
{
  js_object *s = js_new_object(ctx, g_abortsignal_proto);
  js_def(s, "aborted", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(s, "reason", js_undef(), P_WRITABLE | P_ENUMERABLE);
  js_def(s, "onabort", js_null(), P_WRITABLE | P_ENUMERABLE);
  return s;
}

/* Abort `signal` with `reason` (an AbortError when none is given), once */
static void signal_abort(js_ctx *ctx, js_object *signal, js_value reason)
{
  js_value ev, h;
  if (!signal || js_to_boolean(js_get(ctx, js_obj_value(signal), "aborted")))
    return;
  gc_push_scope();
  gc_keep(signal);
  if (reason.type == JV_UNDEF)
    reason = js_make_error(ctx, ctx->error_proto, "AbortError", "signal is aborted without reason");
  if (reason.type == JV_OBJ)
    gc_keep(reason.u.obj);
  js_set(ctx, js_obj_value(signal), "aborted", js_bool(1));
  js_set(ctx, js_obj_value(signal), "reason", reason);
  ev = make_event("abort", 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(ctx, ev, "target", js_obj_value(signal));
    js_set(ctx, ev, "currentTarget", js_obj_value(signal));
  }
  h = js_get(ctx, js_obj_value(signal), "onabort");
  if (js_is_callable(h)) {
    js_value a = ev;
    js_set_event_script("abort");
    js_call(ctx, h, js_obj_value(signal), &a, 1);
    if (ctx->has_exc)
      report_exc(ctx);
  }
  ev_fire_on(signal, ev, 0);
  gc_pop_scope();
}

js_value sig_throw_if_aborted(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ && js_to_boolean(js_get(ctx, t, "aborted")))
    js_throw(ctx, js_get(ctx, t, "reason"));
  return js_undef();
}

/* AbortSignal is not directly constructable */
js_value abortsignal_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  (void)args;
  (void)argc;
  return js_throw_type(ctx, "Illegal constructor");
}

/* Static AbortSignal.abort(reason?) -> an already-aborted signal. */
js_value sig_static_abort(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *s;
  js_value rv;
  (void)t;
  gc_push_scope();
  s = (js_object *)gc_keep(new_abort_signal(ctx));
  signal_abort(ctx, s, A(0));
  rv = js_obj_value(s);
  gc_pop_scope();
  return rv;
}

js_value abortcontroller_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *self, *sig;
  js_value rv;
  (void)thisv;
  (void)args;
  (void)argc; /* native ctor builds its own object */
  gc_push_scope();
  self = (js_object *)gc_keep(js_new_object(ctx, g_abortcontroller_proto));
  sig = (js_object *)gc_keep(new_abort_signal(ctx));
  js_def(self, "signal", js_obj_value(sig), P_ENUMERABLE);
  rv = js_obj_value(self);
  gc_pop_scope();
  return rv;
}

js_value ctrl_abort(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value sig;
  (void)argc;
  if (t.type != JV_OBJ)
    return js_undef();
  sig = js_get(ctx, t, "signal");
  if (sig.type == JV_OBJ)
    signal_abort(ctx, sig.u.obj, A(0));
  return js_undef();
}
