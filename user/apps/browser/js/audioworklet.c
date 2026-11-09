/* The AudioWorkletGlobalScope of one context, on its worker thread (webworker.c starts it): registerProcessor,
 * AudioWorkletProcessor, the graph of worklet nodes and the PCM sink. The thread renders one 128-frame quantum at a
 * time; only process() is JavaScript. The page drives it with messages (see webaudio.c):
 *   module {id,url}                load a module script, answer with the processors it registered
 *   node {id,name,nin,nout,ochan,auto,options,pdata,port}
 *   conn / disc {from,out,to,inp}  to is 0 for the destination, null in disc means any
 *   p {id,name,op,a,b,c}           AudioParam automation
 *   state {run,time}               start or stop rendering at the given context time */

#include "bindings.h"
#include "math.h"

#define QUANTUM 128
#define RATE 48000
#define MAXCH 32
#define BLOCK 4 /* quanta per device write */

enum { EV_SET, EV_LIN, EV_EXP, EV_TGT };

struct pev {
  int type;
  double time, v, v0, tc;
};

struct aparam {
  char name[64];
  double value, min, max;
  int krate, many;
  struct pev *ev;
  int nev, cap;
  float *one, *block;
};

struct chans {
  int n;
  float *ch[MAXCH];
};

struct anode {
  int id, nin, nout, auto_out, active, dead, slot;
  int ochan[MAXCH];
  struct chans *in, *out;
  int *in_vis; /* channel count of the array the processor sees for each input */
  struct aparam *params;
  int nparams;
};

struct conn {
  int from, out, to, inp;
};

static __thread struct anode **nodes, **order;
static __thread int nnodes, capnodes;
static __thread struct conn *conns;
static __thread int nconns, capconns;
static __thread int dirty, running;
static __thread long long frame;
static __thread float left[QUANTUM], right[QUANTUM];

static __thread short pcm[QUANTUM * BLOCK * 2];
static __thread int pcm_frames, device_open;
static __thread unsigned device_retry, pace_t0;
static __thread unsigned long long paced;

/* ---- PCM sink ------------------------------------------------------------ */

static short to_s16(float v)
{
  v *= 32767.0f;
  if (v != v)
    return 0;
  return (short)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
}

static void sink_stop(void)
{
  if (device_open)
    audio_close();
  device_open = 0;
  pcm_frames = 0;
  paced = 0;
}

/* Hand the buffered quanta to the sound device, which paces the thread; without one, sleep in real time */
static void sink_flush(void)
{
  int total = pcm_frames * 4, off = 0;
  pcm_frames = 0;
  if (!device_open && (int)(msec() - device_retry) >= 0) {
    device_open = audio_open() == 0;
    if (!device_open)
      device_retry = msec() + 500;
    paced = 0;
  }
  if (device_open) {
    while (off < total) {
      int w = audio_write((char *)pcm + off, total - off);
      if (w <= 0) {
        audio_close();
        device_open = 0;
        device_retry = msec() + 500;
        return;
      }
      off += w;
    }
    return;
  }
  if (!paced)
    pace_t0 = msec();
  paced += (unsigned)(total / 4);
  {
    unsigned due = (unsigned)(paced * 1000 / RATE), now = msec() - pace_t0;
    if (due > now)
      msleep(due - now);
  }
}

/* One quantum; a NULL channel is silence */
static void sink_write(const float *l, const float *r)
{
  short *o = pcm + pcm_frames * 2;
  int i;
  for (i = 0; i < QUANTUM; i++) {
    o[2 * i] = l ? to_s16(l[i]) : 0;
    o[2 * i + 1] = r ? to_s16(r[i]) : 0;
  }
  pcm_frames += QUANTUM;
  if (pcm_frames == QUANTUM * BLOCK)
    sink_flush();
}

/* ---- helpers ------------------------------------------------------------- */

static js_value global_of(js_ctx *ctx) { return js_obj_value(ctx->global); }

static const char *str_of(js_value v) { return v.type == JV_STR ? v.u.str->data : ""; }

static double num_of(js_value v, double dflt) { return v.type == JV_NUM ? v.u.num : dflt; }

static int id_of(js_value v, int dflt) { return v.type == JV_NUM ? (int)v.u.num : dflt; }

static js_value hidden(js_ctx *ctx, const char *name) { return js_get(ctx, global_of(ctx), name); }

/* A new zeroed Float32Array of one quantum, with its bytes through *data */
static js_value new_quantum(js_ctx *ctx, float **data)
{
  js_value v = js_new_typedarray(ctx, TA_F32, QUANTUM);
  int len = 0;
  *data = (float *)view_bytes(v, &len);
  return v;
}

/* The text of the pending exception, which is cleared */
static js_value take_exception(js_ctx *ctx)
{
  js_value e = ctx->exc, m = e.type == JV_OBJ ? js_get(ctx, e, "message") : e;
  js_value s = js_to_string(ctx, m);
  ctx->has_exc = 0;
  ctx->exc_root = 0;
  return s;
}

static js_value make_msg(js_ctx *ctx, const char *type)
{
  js_object *o = js_new_object(ctx, ctx->object_proto);
  js_def(o, "t", js_mk_cstring(ctx, type), P_ENUMERABLE | P_WRITABLE);
  return js_obj_value(o);
}

static js_value throw_dom(js_ctx *ctx, const char *name, const char *msg)
{
  js_throw(ctx, dom_exception(ctx, name, msg));
  return js_undef();
}

static struct anode *find_node(int id)
{
  int i;
  for (i = 0; i < nnodes; i++)
    if (nodes[i]->id == id)
      return nodes[i];
  return 0;
}

static struct aparam *find_param(struct anode *n, const char *name)
{
  int i;
  for (i = 0; i < n->nparams; i++)
    if (streq(n->params[i].name, name))
      return &n->params[i];
  return 0;
}

/* ---- parameter timelines ------------------------------------------------- */

/* Events: set, lin and exp end at (time, v); tgt approaches v from v0 with time constant tc */
static double value_at(struct aparam *p, double t)
{
  struct pev *prev = 0, *next = 0;
  int i;
  for (i = 0; i < p->nev; i++) {
    if (p->ev[i].time <= t)
      prev = &p->ev[i];
    else {
      next = &p->ev[i];
      break;
    }
  }
  if (!prev)
    return p->value;
  if (prev->type == EV_TGT)
    return prev->v + (prev->v0 - prev->v) * exp(-(t - prev->time) / prev->tc);
  if (next && (next->type == EV_LIN || next->type == EV_EXP)) {
    double dt = next->time - prev->time, r;
    if (dt <= 0)
      return next->v;
    r = (t - prev->time) / dt;
    if (next->type == EV_LIN)
      return prev->v + (next->v - prev->v) * r;
    if (prev->v * next->v <= 0)
      return prev->v;
    return prev->v * pow(next->v / prev->v, r);
  }
  return prev->v;
}

static void add_event(struct aparam *p, struct pev e)
{
  int i = p->nev;
  if (p->nev + 2 > p->cap) {
    int cap = p->cap ? p->cap * 2 : 8;
    struct pev *ev = (struct pev *)realloc(p->ev, (unsigned)cap * sizeof *ev);
    if (!ev)
      return;
    p->ev = ev;
    p->cap = cap;
  }
  if ((e.type == EV_LIN || e.type == EV_EXP) && !p->nev) {
    struct pev s = {EV_SET, (double)frame / RATE, p->value, 0, 0};
    p->ev[p->nev++] = s;
    i = 1;
  }
  if (e.type == EV_TGT)
    e.v0 = value_at(p, e.time);
  while (i > 0 && p->ev[i - 1].time > e.time)
    i--;
  memmove(p->ev + i + 1, p->ev + i, (unsigned)(p->nev - i) * sizeof *p->ev);
  p->ev[i] = e;
  p->nev++;
}

static double clamp_to(struct aparam *p, double v) { return v < p->min ? p->min : v > p->max ? p->max : v; }

/* The parameter for the quantum starting at t0: one value, or one per frame (p->many) */
static float *param_block(struct aparam *p, double t0)
{
  double t1 = t0 + (double)QUANTUM / RATE, a, b;
  int j, varying = 0;
  while (p->nev > 1 && p->ev[1].time <= t0) {
    memmove(p->ev, p->ev + 1, (unsigned)(p->nev - 1) * sizeof *p->ev);
    p->nev--;
  }
  if (p->nev == 1 && p->ev[0].type == EV_SET && p->ev[0].time <= t0) {
    p->value = p->ev[0].v;
    p->nev = 0;
  }
  p->many = 0;
  if (!p->nev) {
    p->one[0] = (float)clamp_to(p, p->value);
    return p->one;
  }
  if (p->krate) {
    p->one[0] = (float)clamp_to(p, value_at(p, t0));
    return p->one;
  }
  for (j = 0; j < p->nev && !varying; j++)
    if (p->ev[j].time > t0 && p->ev[j].time < t1)
      varying = 1;
  a = value_at(p, t0);
  b = value_at(p, t0 + (double)(QUANTUM - 1) / RATE);
  if (!varying && a == b) {
    p->one[0] = (float)clamp_to(p, a);
    return p->one;
  }
  for (j = 0; j < QUANTUM; j++)
    p->block[j] = (float)clamp_to(p, value_at(p, t0 + (double)j / RATE));
  p->many = 1;
  return p->block;
}

static void param_op(js_ctx *ctx, js_value m)
{
  struct anode *n = find_node(id_of(js_get(ctx, m, "id"), -1));
  struct aparam *p = n ? find_param(n, str_of(js_get(ctx, m, "name"))) : 0;
  const char *op = str_of(js_get(ctx, m, "op"));
  double a = num_of(js_get(ctx, m, "a"), 0), b = num_of(js_get(ctx, m, "b"), 0), c = num_of(js_get(ctx, m, "c"), 0);
  if (!p)
    return;
  if (streq(op, "value")) {
    struct pev e = {EV_SET, (double)frame / RATE, a, 0, 0};
    p->value = a;
    add_event(p, e);
  } else if (streq(op, "set") || streq(op, "lin") || streq(op, "exp")) {
    struct pev e = {streq(op, "set") ? EV_SET : streq(op, "lin") ? EV_LIN : EV_EXP, b, a, 0, 0};
    add_event(p, e);
  } else if (streq(op, "tgt")) {
    struct pev e = {EV_TGT, b, a, 0, c > 0 ? c : 1e-9};
    add_event(p, e);
  } else if (streq(op, "cancel") || streq(op, "hold")) {
    double v = value_at(p, a);
    int i;
    for (i = p->nev - 1; i >= 0; i--)
      if (p->ev[i].time >= a) {
        memmove(p->ev + i, p->ev + i + 1, (unsigned)(p->nev - i - 1) * sizeof *p->ev);
        p->nev--;
      }
    if (streq(op, "hold")) {
      struct pev e = {EV_SET, a, v, 0, 0};
      add_event(p, e);
    }
  } else if (streq(op, "rate"))
    p->krate = streq(str_of(js_get(ctx, m, "a")), "k-rate");
}

/* ---- processors ---------------------------------------------------------- */

static js_value awp_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value port = hidden(ctx, "@pending");
  (void)args;
  (void)argc;
  if (port.type != JV_OBJ || thisv.type != JV_OBJ)
    return js_throw_type(ctx, "Illegal constructor");
  js_def(thisv.u.obj, "port", port, P_WRITABLE | P_CONFIGURABLE);
  js_set(ctx, global_of(ctx), "@pending", js_undef());
  return thisv;
}

/* The parameter descriptors of a processor class, normalised, or NULL with an exception pending */
static js_object *descriptors_of(js_ctx *ctx, js_value cls)
{
  js_value raw = js_get(ctx, cls, "parameterDescriptors");
  js_object *list = js_new_array(ctx, 0);
  unsigned i, k, n;
  gc_push_scope();
  gc_keep(list);
  if (raw.type == JV_UNDEF) {
    gc_pop_scope();
    return list;
  }
  if (!js_is_array(raw)) {
    js_throw_type(ctx, "The parameterDescriptors property is not an array");
    gc_pop_scope();
    return 0;
  }
  gc_keep(raw.u.obj);
  n = js_array_len(raw.u.obj);
  for (i = 0; i < n; i++) {
    js_value d = js_get_index(ctx, raw, i), name, rate;
    double def0, lo, hi;
    js_object *o;
    if (d.type != JV_OBJ) {
      js_throw_type(ctx, "A parameter descriptor is not an object");
      gc_pop_scope();
      return 0;
    }
    name = js_to_string(ctx, js_get(ctx, d, "name"));
    gc_keep(name.u.str);
    for (k = 0; k < js_array_len(list); k++)
      if (streq(str_of(js_get(ctx, js_get_index(ctx, js_obj_value(list), k), "name")), str_of(name))) {
        js_throw(ctx, dom_exception(ctx, "NotSupportedError", "Duplicate parameter name"));
        gc_pop_scope();
        return 0;
      }
    def0 = js_get(ctx, d, "defaultValue").type == JV_UNDEF ? 0 : js_to_number(ctx, js_get(ctx, d, "defaultValue"));
    lo = js_get(ctx, d, "minValue").type == JV_UNDEF ? -3.4028234663852886e38
                                                     : js_to_number(ctx, js_get(ctx, d, "minValue"));
    hi = js_get(ctx, d, "maxValue").type == JV_UNDEF ? 3.4028234663852886e38
                                                     : js_to_number(ctx, js_get(ctx, d, "maxValue"));
    rate = js_get(ctx, d, "automationRate");
    rate = rate.type == JV_UNDEF ? js_mk_cstring(ctx, "a-rate") : js_to_string(ctx, rate);
    gc_keep(rate.u.str);
    if (!streq(str_of(rate), "a-rate") && !streq(str_of(rate), "k-rate")) {
      js_throw_type(ctx, "Invalid automationRate");
      gc_pop_scope();
      return 0;
    }
    if (!(lo <= def0 && def0 <= hi)) {
      js_throw(ctx, dom_exception(ctx, "InvalidStateError", "Invalid parameter bounds"));
      gc_pop_scope();
      return 0;
    }
    o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(o, "name", name, P_ENUMERABLE | P_WRITABLE);
    js_def(o, "defaultValue", js_number(def0), P_ENUMERABLE | P_WRITABLE);
    js_def(o, "minValue", js_number(lo), P_ENUMERABLE | P_WRITABLE);
    js_def(o, "maxValue", js_number(hi), P_ENUMERABLE | P_WRITABLE);
    js_def(o, "automationRate", rate, P_ENUMERABLE | P_WRITABLE);
    js_array_push(ctx, list, js_obj_value(o));
  }
  gc_pop_scope();
  return list;
}

static js_value register_processor(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value name = js_to_string(ctx, A(0)), cls = A(1), proto, registry = hidden(ctx, "@registry");
  js_object *params, *entry, *fresh;
  (void)t;
  if (ctx->has_exc)
    return js_undef();
  if (!str_of(name)[0])
    return throw_dom(ctx, "NotSupportedError", "The processor name is empty");
  if (js_get(ctx, registry, str_of(name)).type != JV_UNDEF)
    return throw_dom(ctx, "NotSupportedError", "The processor name is already registered");
  proto = js_is_callable(cls) ? js_get(ctx, cls, "prototype") : js_undef();
  if (proto.type != JV_OBJ)
    return js_throw_type(ctx, "The second argument is not a constructor");
  if (!js_is_callable(js_get(ctx, proto, "process")))
    return js_throw_type(ctx, "The processor has no process() method");
  gc_push_scope();
  gc_keep(name.u.str);
  params = descriptors_of(ctx, cls);
  if (!params) {
    gc_pop_scope();
    return js_undef();
  }
  gc_keep(params);
  entry = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(entry, "ctor", cls, P_WRITABLE);
  js_def(entry, "params", js_obj_value(params), P_WRITABLE);
  js_set(ctx, registry, str_of(name), js_obj_value(entry));
  fresh = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(fresh, "name", name, P_ENUMERABLE | P_WRITABLE);
  js_def(fresh, "params", js_obj_value(params), P_ENUMERABLE | P_WRITABLE);
  js_array_push(ctx, hidden(ctx, "@fresh").u.obj, js_obj_value(fresh));
  gc_pop_scope();
  return js_undef();
}

/* ---- nodes ---------------------------------------------------------------- */

static void fail_node(js_ctx *ctx, struct anode *n, js_value message)
{
  js_value m = make_msg(ctx, "error");
  gc_push_scope();
  gc_keep(m.u.obj);
  gc_keep(message.u.str);
  js_def(m.u.obj, "id", js_number(n->id), P_ENUMERABLE | P_WRITABLE);
  js_def(m.u.obj, "message", message, P_ENUMERABLE | P_WRITABLE);
  n->dead = 1;
  js_worklet_post(ctx, m);
  gc_pop_scope();
}

/* A new array of the first `n` buffers of a pool (or empty), kept in `holder` at `index` */
static void show_channels(js_ctx *ctx, js_value holder, int index, js_value pool, int n)
{
  js_object *a = js_new_array(ctx, 0);
  int i;
  gc_push_scope();
  gc_keep(a);
  for (i = 0; i < n; i++)
    js_array_push(ctx, a, js_get_index(ctx, pool, (unsigned)i));
  js_set_index(ctx, holder, (unsigned)index, js_obj_value(a));
  gc_pop_scope();
}

/* The Float32Array behind output `o` of node `n` has `ch` channels */
static void shape_output(js_ctx *ctx, struct anode *n, js_value rec, int o, int ch)
{
  js_object *a = js_new_array(ctx, 0);
  int i;
  gc_push_scope();
  gc_keep(a);
  for (i = 0; i < ch; i++) {
    float *data;
    js_value q = new_quantum(ctx, &data);
    js_array_push(ctx, a, q);
    n->out[o].ch[i] = data;
  }
  n->out[o].n = ch;
  js_set_index(ctx, js_get(ctx, rec, "outputs"), (unsigned)o, js_obj_value(a));
  gc_pop_scope();
}

static void make_node(js_ctx *ctx, js_value m)
{
  js_value name = js_get(ctx, m, "name"), reg, params, ochan = js_get(ctx, m, "ochan"), pdata = js_get(ctx, m, "pdata");
  js_value options = js_get(ctx, m, "options"), port = js_get(ctx, m, "port"), procv;
  js_object *rec, *inputs, *outputs, *pools, *parameters, *p1, *pm;
  struct anode *n;
  int i, np;
  reg = js_get(ctx, hidden(ctx, "@registry"), str_of(name));
  if (reg.type != JV_OBJ)
    return;
  params = js_get(ctx, reg, "params");
  np = params.type == JV_OBJ ? (int)js_array_len(params.u.obj) : 0;
  n = (struct anode *)calloc(1, sizeof *n);
  if (!n)
    return;
  n->id = id_of(js_get(ctx, m, "id"), 0);
  n->nin = id_of(js_get(ctx, m, "nin"), 0);
  n->nout = id_of(js_get(ctx, m, "nout"), 0);
  n->auto_out = js_to_boolean(js_get(ctx, m, "auto"));
  n->active = 1;
  n->nparams = np;
  n->in = (struct chans *)calloc((unsigned)n->nin + 1, sizeof *n->in);
  n->out = (struct chans *)calloc((unsigned)n->nout + 1, sizeof *n->out);
  n->in_vis = (int *)calloc((unsigned)n->nin + 1, sizeof *n->in_vis);
  n->params = (struct aparam *)calloc((unsigned)np + 1, sizeof *n->params);
  if (nnodes == capnodes && n->in && n->out && n->in_vis && n->params) {
    int cap = capnodes ? capnodes * 2 : 8;
    struct anode **a = (struct anode **)realloc(nodes, (unsigned)cap * sizeof *a);
    if (a)
      nodes = a;
    if (a) {
      struct anode **o = (struct anode **)realloc(order, (unsigned)cap * sizeof *o);
      if (o) {
        order = o;
        capnodes = cap;
      }
    }
  }
  if (nnodes == capnodes || !n->in || !n->out || !n->in_vis || !n->params) {
    free(n->in);
    free(n->out);
    free(n->in_vis);
    free(n->params);
    free(n);
    return;
  }
  gc_push_scope();
  rec = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  inputs = (js_object *)gc_keep(js_new_array(ctx, 0));
  outputs = (js_object *)gc_keep(js_new_array(ctx, 0));
  pools = (js_object *)gc_keep(js_new_array(ctx, 0));
  parameters = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  p1 = (js_object *)gc_keep(js_new_array(ctx, 0));
  pm = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_def(rec, "inputs", js_obj_value(inputs), P_WRITABLE);
  js_def(rec, "outputs", js_obj_value(outputs), P_WRITABLE);
  js_def(rec, "pools", js_obj_value(pools), P_WRITABLE);
  js_def(rec, "parameters", js_obj_value(parameters), P_WRITABLE);
  js_def(rec, "one", js_obj_value(p1), P_WRITABLE);
  js_def(rec, "many", js_obj_value(pm), P_WRITABLE);
  n->slot = (int)js_array_len(hidden(ctx, "@nodes").u.obj);
  js_array_push(ctx, hidden(ctx, "@nodes").u.obj, js_obj_value(rec));
  for (i = 0; i < n->nin; i++) {
    js_array_push(ctx, inputs, js_obj_value(js_new_array(ctx, 0)));
    js_array_push(ctx, pools, js_obj_value(js_new_array(ctx, 0)));
  }
  for (i = 0; i < n->nout; i++) {
    js_array_push(ctx, outputs, js_undef());
    shape_output(ctx, n, js_obj_value(rec), i, n->auto_out ? 1 : id_of(js_get_index(ctx, ochan, (unsigned)i), 1));
  }
  for (i = 0; i < np; i++) {
    js_value d = js_get_index(ctx, params, (unsigned)i), init;
    struct aparam *p = &n->params[i];
    js_value pn = js_get(ctx, d, "name"), v1, vm;
    int one_len = 0;
    const char *pname = str_of(pn);
    strlcpy(p->name, pname, sizeof p->name);
    p->min = num_of(js_get(ctx, d, "minValue"), 0);
    p->max = num_of(js_get(ctx, d, "maxValue"), 0);
    p->krate = streq(str_of(js_get(ctx, d, "automationRate")), "k-rate");
    init = pdata.type == JV_OBJ ? js_get(ctx, pdata, pname) : js_undef();
    p->value = init.type == JV_NUM ? init.u.num : num_of(js_get(ctx, d, "defaultValue"), 0);
    v1 = js_new_typedarray(ctx, TA_F32, 1);
    p->one = (float *)view_bytes(v1, &one_len);
    gc_keep(v1.u.obj);
    vm = new_quantum(ctx, &p->block);
    gc_keep(vm.u.obj);
    js_array_push(ctx, p1, v1);
    js_array_push(ctx, pm, vm);
    js_set(ctx, js_obj_value(parameters), pname, v1);
  }
  nodes[nnodes++] = n;
  dirty = 1;
  js_set(ctx, global_of(ctx), "@pending", port);
  procv = js_construct(ctx, js_get(ctx, reg, "ctor"), &options, 1);
  js_set(ctx, global_of(ctx), "@pending", js_undef());
  if (ctx->has_exc)
    fail_node(ctx, n, take_exception(ctx));
  else {
    js_def(rec, "proc", procv, P_WRITABLE);
  }
  gc_pop_scope();
}

/* ---- the graph ------------------------------------------------------------- */

/* Run order: sources first; nodes in a cycle go last */
static void relink(void)
{
  int *indeg = (int *)calloc((unsigned)nnodes + 1, sizeof *indeg);
  int *seen = (int *)calloc((unsigned)nnodes + 1, sizeof *seen);
  int i, k, head = 0, tail = 0;
  struct anode **queue = order;
  dirty = 0;
  if (!indeg || !seen) {
    free(indeg);
    free(seen);
    for (i = 0; i < nnodes; i++)
      order[i] = nodes[i];
    return;
  }
  for (i = 0; i < nconns; i++)
    if (conns[i].to != 0)
      for (k = 0; k < nnodes; k++)
        if (nodes[k]->id == conns[i].to)
          indeg[k]++;
  for (i = 0; i < nnodes; i++)
    if (!indeg[i]) {
      queue[tail++] = nodes[i];
      seen[i] = 1;
    }
  while (head < tail) {
    struct anode *cur = queue[head++];
    for (i = 0; i < nconns; i++) {
      if (conns[i].from != cur->id || conns[i].to == 0)
        continue;
      for (k = 0; k < nnodes; k++)
        if (nodes[k]->id == conns[i].to && --indeg[k] == 0 && !seen[k]) {
          queue[tail++] = nodes[k];
          seen[k] = 1;
        }
    }
  }
  for (i = 0; i < nnodes; i++)
    if (!seen[i])
      queue[tail++] = nodes[i];
  free(indeg);
  free(seen);
}

/* The widest output feeding input `inp` of node `id` (0 when nothing does) */
static int input_channels(int id, int inp)
{
  int i, ch = 0;
  for (i = 0; i < nconns; i++) {
    struct anode *s;
    if (conns[i].to != id || conns[i].inp != inp)
      continue;
    s = find_node(conns[i].from);
    if (s && conns[i].out < s->nout && s->out[conns[i].out].n > ch)
      ch = s->out[conns[i].out].n;
  }
  return ch;
}

static void sum_into(float *dst, const float *src)
{
  int i;
  for (i = 0; i < QUANTUM; i++)
    dst[i] += src[i];
}

/* Sum the connections into input `inp` of `n`: speakers rules, a mono source feeds the first two channels */
static void mix_input(js_ctx *ctx, struct anode *n, js_value rec, int inp)
{
  int ch = input_channels(n->id, inp), i, j;
  js_value pool = js_get_index(ctx, js_get(ctx, rec, "pools"), (unsigned)inp);
  struct chans *in = &n->in[inp];
  while (in->n < ch) {
    float *data;
    js_value q = new_quantum(ctx, &data);
    js_array_push(ctx, pool.u.obj, q);
    in->ch[in->n++] = data;
  }
  if (n->in_vis[inp] != ch) {
    show_channels(ctx, js_get(ctx, rec, "inputs"), inp, pool, ch);
    n->in_vis[inp] = ch;
  }
  for (j = 0; j < ch; j++)
    memset(in->ch[j], 0, sizeof(float) * QUANTUM);
  for (i = 0; i < nconns; i++) {
    struct anode *s;
    struct chans *o;
    if (conns[i].to != n->id || conns[i].inp != inp)
      continue;
    s = find_node(conns[i].from);
    if (!s || conns[i].out >= s->nout)
      continue;
    o = &s->out[conns[i].out];
    if (o->n == 1 && ch > 1) {
      sum_into(in->ch[0], o->ch[0]);
      sum_into(in->ch[1], o->ch[0]);
      continue;
    }
    for (j = 0; j < o->n && j < ch; j++)
      sum_into(in->ch[j], o->ch[j]);
  }
}

static int has_active_input(struct anode *n)
{
  int i;
  for (i = 0; i < nconns; i++) {
    struct anode *s;
    if (conns[i].to != n->id)
      continue;
    s = find_node(conns[i].from);
    if (s && s->active && !s->dead)
      return 1;
  }
  return 0;
}

static void run_node(js_ctx *ctx, struct anode *n, double t0)
{
  js_value rec = js_get_index(ctx, hidden(ctx, "@nodes"), (unsigned)n->slot), proc = js_get(ctx, rec, "proc"), fn, ret;
  js_value argv[3], parameters;
  int i, j;
  for (i = 0; i < n->nout; i++) {
    if (n->auto_out) {
      int ch = n->nin ? input_channels(n->id, 0) : 0;
      if (ch < 1)
        ch = 1;
      if (n->out[i].n != ch)
        shape_output(ctx, n, rec, i, ch);
    }
    for (j = 0; j < n->out[i].n; j++)
      memset(n->out[i].ch[j], 0, sizeof(float) * QUANTUM);
  }
  if (n->dead || proc.type != JV_OBJ || (!n->active && !has_active_input(n)))
    return;
  gc_push_scope();
  gc_keep(rec.u.obj);
  gc_keep(proc.u.obj);
  for (i = 0; i < n->nin; i++)
    mix_input(ctx, n, rec, i);
  parameters = js_get(ctx, rec, "parameters");
  for (i = 0; i < n->nparams; i++) {
    struct aparam *p = &n->params[i];
    js_value src;
    param_block(p, t0);
    src = js_get_index(ctx, js_get(ctx, rec, p->many ? "many" : "one"), (unsigned)i);
    js_set(ctx, parameters, p->name, src);
  }
  argv[0] = js_get(ctx, rec, "inputs");
  argv[1] = js_get(ctx, rec, "outputs");
  argv[2] = parameters;
  fn = js_get(ctx, proc, "process");
  ret = js_is_callable(fn) ? js_call(ctx, fn, proc, argv, 3) : js_undef();
  if (ctx->has_exc)
    fail_node(ctx, n, take_exception(ctx));
  else if (!js_to_boolean(ret))
    n->active = 0;
  gc_pop_scope();
}

bool js_worklet_render(js_ctx *ctx)
{
  double t0 = (double)frame / RATE;
  int i, dest = 0;
  if (!running)
    return false;
  if (dirty)
    relink();
  if (!nnodes)
    return false;
  for (i = 0; i < nnodes; i++)
    run_node(ctx, order[i], t0);
  memset(left, 0, sizeof left);
  memset(right, 0, sizeof right);
  for (i = 0; i < nconns; i++) {
    struct anode *s;
    struct chans *o;
    if (conns[i].to != 0)
      continue;
    s = find_node(conns[i].from);
    if (!s || conns[i].out >= s->nout || !s->out[conns[i].out].n)
      continue;
    o = &s->out[conns[i].out];
    sum_into(left, o->ch[0]);
    sum_into(right, o->n > 1 ? o->ch[1] : o->ch[0]);
    dest = 1;
  }
  sink_write(dest ? left : 0, dest ? right : 0);
  frame += QUANTUM;
  return true;
}

/* ---- messages --------------------------------------------------------------- */

static void connect_nodes(js_ctx *ctx, js_value m)
{
  struct conn c = {id_of(js_get(ctx, m, "from"), 0), id_of(js_get(ctx, m, "out"), 0), id_of(js_get(ctx, m, "to"), 0),
                   id_of(js_get(ctx, m, "inp"), 0)};
  int i;
  for (i = 0; i < nconns; i++)
    if (!memcmp(&conns[i], &c, sizeof c))
      return;
  if (nconns == capconns) {
    int cap = capconns ? capconns * 2 : 8;
    struct conn *a = (struct conn *)realloc(conns, (unsigned)cap * sizeof *a);
    if (!a)
      return;
    conns = a;
    capconns = cap;
  }
  conns[nconns++] = c;
  dirty = 1;
}

static void disconnect_nodes(js_ctx *ctx, js_value m)
{
  js_value from = js_get(ctx, m, "from"), out = js_get(ctx, m, "out"), to = js_get(ctx, m, "to"),
           inp = js_get(ctx, m, "inp");
  int i;
  for (i = nconns - 1; i >= 0; i--) {
    struct conn *c = &conns[i];
    if (c->from == id_of(from, -1) && (out.type != JV_NUM || c->out == (int)out.u.num) &&
        (to.type != JV_NUM || c->to == (int)to.u.num) && (inp.type != JV_NUM || c->inp == (int)inp.u.num)) {
      memmove(c, c + 1, (unsigned)(nconns - i - 1) * sizeof *c);
      nconns--;
    }
  }
  dirty = 1;
}

static void add_module(js_ctx *ctx, js_value m)
{
  js_value reply = make_msg(ctx, "module"), fresh = hidden(ctx, "@fresh");
  js_value ename = js_undef(), emsg = js_undef();
  int bad;
  gc_push_scope();
  gc_keep(reply.u.obj);
  bad = js_worklet_add_module(ctx, str_of(js_get(ctx, m, "url")));
  if (bad && ctx->has_exc) {
    js_value e = ctx->exc;
    ename = js_to_string(ctx, js_get(ctx, e, "name"));
    gc_keep(ename.u.str);
    emsg = take_exception(ctx);
    gc_keep(emsg.u.str);
  }
  js_def(reply.u.obj, "id", js_get(ctx, m, "id"), P_ENUMERABLE | P_WRITABLE);
  js_def(reply.u.obj, "ok", js_bool(!bad), P_ENUMERABLE | P_WRITABLE);
  js_def(reply.u.obj, "name", ename.type == JV_STR ? ename : js_mk_cstring(ctx, ""), P_ENUMERABLE | P_WRITABLE);
  js_def(reply.u.obj, "message", emsg.type == JV_STR ? emsg : js_mk_cstring(ctx, ""), P_ENUMERABLE | P_WRITABLE);
  js_def(reply.u.obj, "reg", fresh, P_ENUMERABLE | P_WRITABLE);
  js_set(ctx, global_of(ctx), "@fresh", js_obj_value(js_new_array(ctx, 0)));
  js_worklet_post(ctx, reply);
  gc_pop_scope();
}

static js_value on_message(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value m = js_get(ctx, A(0), "data");
  const char *type;
  (void)t;
  if (m.type != JV_OBJ)
    return js_undef();
  type = str_of(js_get(ctx, m, "t"));
  if (streq(type, "module"))
    add_module(ctx, m);
  else if (streq(type, "node"))
    make_node(ctx, m);
  else if (streq(type, "conn"))
    connect_nodes(ctx, m);
  else if (streq(type, "disc"))
    disconnect_nodes(ctx, m);
  else if (streq(type, "p"))
    param_op(ctx, m);
  else if (streq(type, "state")) {
    int run = js_to_boolean(js_get(ctx, m, "run"));
    if (run && !running)
      frame = (long long)(num_of(js_get(ctx, m, "time"), 0) * RATE + 0.5);
    if (!run)
      sink_stop();
    running = run;
  }
  return js_undef();
}

static js_value get_frame(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number((double)frame);
}

static js_value get_time(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number((double)frame / RATE);
}

void js_worklet_install(js_ctx *ctx)
{
  js_object *g = ctx->global, *ctor, *proto;
  gc_push_scope();
  js_def(g, "@registry", js_obj_value(js_new_object(ctx, ctx->object_proto)), 0);
  js_def(g, "@fresh", js_obj_value(js_new_array(ctx, 0)), P_WRITABLE);
  js_def(g, "@nodes", js_obj_value(js_new_array(ctx, 0)), 0);
  js_def(g, "@pending", js_undef(), P_WRITABLE);
  js_def(g, "sampleRate", js_number(RATE), P_CONFIGURABLE);
  js_def_accessor(g, "currentFrame", (js_object *)gc_keep(js_new_native(ctx, get_frame, "currentFrame", 0)), 0,
                  P_CONFIGURABLE);
  js_def_accessor(g, "currentTime", (js_object *)gc_keep(js_new_native(ctx, get_time, "currentTime", 0)), 0,
                  P_CONFIGURABLE);
  ctor = (js_object *)gc_keep(js_new_native(ctx, awp_ctor, "AudioWorkletProcessor", 0));
  proto = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(ctor, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(g, "AudioWorkletProcessor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def_native(ctx, g, "registerProcessor", register_processor, 2);
  js_def_native(ctx, g, "onmessage", on_message, 1);
  gc_pop_scope();
}

/* The thread is ending: stop the sink and free the graph */
void js_worklet_end(void)
{
  int i, j;
  sink_stop();
  for (i = 0; i < nnodes; i++) {
    for (j = 0; j < nodes[i]->nparams; j++)
      free(nodes[i]->params[j].ev);
    free(nodes[i]->params);
    free(nodes[i]->in);
    free(nodes[i]->out);
    free(nodes[i]->in_vis);
    free(nodes[i]);
  }
  free(nodes);
  free(order);
  free(conns);
  nodes = order = 0;
  conns = 0;
  nnodes = capnodes = nconns = capconns = 0;
}
