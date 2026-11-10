/* mediastream.c: navigator.mediaDevices (getUserMedia, enumerateDevices), MediaStream and MediaStreamTrack over the
 * capture devices (camera.c, microphone.c). A device is shared by all its tracks and runs while one is live. */

#include "features/mediastream/mediastream.h"
#include "features/camera/camera.h"
#include "features/clienthints/clienthints.h"
#include "features/microphone/microphone.h"
#include "jsint.h"
#include "uuid.h"
#include <stdint.h>

#define MAX_TRACKS 32
#define NDEV 2

enum { H_TRACK_PROTO, H_STREAM_PROTO, H_TRACKS };

static js_ctx *ctx;
static js_object *held[H_TRACKS + MAX_TRACKS];
static int held_n = H_TRACKS + MAX_TRACKS;
static unsigned serials[MAX_TRACKS], last_serial;
static unsigned char slot_dev[MAX_TRACKS];
static const struct ms_device *devs[NDEV];

/* ---- constraints */

void mediastream_read_range(js_value v, struct ms_range *r)
{
  memset(r, 0, sizeof *r);
  if (v.type == JV_NUM) {
    r->ideal_set = true;
    r->ideal = js_to_number(ctx, v);
  } else if (v.type == JV_OBJ) {
    js_value x;
    if ((x = js_get(ctx, v, "ideal")).type != JV_UNDEF)
      r->ideal_set = true, r->ideal = js_to_number(ctx, x);
    if ((x = js_get(ctx, v, "exact")).type != JV_UNDEF)
      r->exact_set = true, r->exact = js_to_number(ctx, x);
    if ((x = js_get(ctx, v, "min")).type != JV_UNDEF)
      r->min_set = true, r->min = js_to_number(ctx, x);
    if ((x = js_get(ctx, v, "max")).type != JV_UNDEF)
      r->max_set = true, r->max = js_to_number(ctx, x);
  }
}

bool mediastream_in_range(const struct ms_range *r, double v)
{
  return (!r->exact_set || v == r->exact) && (!r->min_set || v >= r->min) && (!r->max_set || v <= r->max);
}

bool mediastream_device_matches(js_value constraints, const char *id)
{
  js_value want;
  if (constraints.type != JV_OBJ)
    return true;
  want = js_get(ctx, constraints, "deviceId");
  if (want.type != JV_OBJ || js_is_array(want))
    return true;
  want = js_get(ctx, want, "exact");
  if (want.type == JV_UNDEF)
    return true;
  if (want.type == JV_STR)
    return streq(arg_str(ctx, want), id);
  for (int i = 0; js_is_array(want) && i < (int)js_to_number(ctx, js_get(ctx, want, "length")); i++) {
    js_value e = js_get_index(ctx, want, (unsigned)i);
    if (e.type == JV_STR && streq(arg_str(ctx, e), id))
      return true;
  }
  return false;
}

/* ---- tracks of a device */

static int live_of(int d)
{
  int n = 0;
  for (int i = 0; i < MAX_TRACKS; i++)
    n += held[H_TRACKS + i] && slot_dev[i] == d;
  return n;
}

static int dev_index(const char *kind)
{
  for (int d = 0; d < NDEV; d++)
    if (devs[d] && streq(devs[d]->kind, kind))
      return d;
  return -1;
}

int mediastream_live(const char *kind)
{
  int d = dev_index(kind);
  return d < 0 ? 0 : live_of(d);
}

static bool track_enabled(js_object *t) { return js_to_boolean(js_get(ctx, js_obj_value(t), "_en")); }

bool mediastream_any_enabled(const char *kind)
{
  int d = dev_index(kind);
  for (int i = 0; d >= 0 && i < MAX_TRACKS; i++)
    if (held[H_TRACKS + i] && slot_dev[i] == d && track_enabled(held[H_TRACKS + i]))
      return true;
  return false;
}

static int slot_by_serial(unsigned serial)
{
  for (int i = 0; serial && i < MAX_TRACKS; i++)
    if (serials[i] == serial && held[H_TRACKS + i])
      return i;
  return -1;
}

bool mediastream_serial_live(unsigned serial) { return slot_by_serial(serial) >= 0; }

bool mediastream_serial_enabled(unsigned serial)
{
  int slot = slot_by_serial(serial);
  return slot >= 0 && track_enabled(held[H_TRACKS + slot]);
}

static int slot_of(js_value t)
{
  if (t.type != JV_OBJ)
    return -1;
  js_value tag = js_get(ctx, t, "_cam");
  return slot_by_serial(tag.type == JV_NUM ? (unsigned)js_to_number(ctx, tag) : 0);
}

unsigned mediastream_track_serial(js_value track)
{
  int slot = slot_of(track);
  return slot < 0 ? 0 : serials[slot];
}

static void release_slot(int slot)
{
  int d = slot_dev[slot];
  held[H_TRACKS + slot] = 0;
  serials[slot] = 0;
  if (!live_of(d))
    devs[d]->close();
}

/* ---- JS helpers */

static js_value reject(const char *name, const char *msg)
{
  return js_promise_rejected(ctx, dom_exception(ctx, name, msg));
}

static js_object *new_obj(void) { return js_new_object(ctx, ctx->object_proto); }

static void hide(js_object *o, const char *key, js_value v) { js_def(o, key, v, P_WRITABLE | P_CONFIGURABLE); }

static js_object *obj_of(js_value v) { return v.type == JV_OBJ ? v.u.obj : 0; }

static void add_event_target(js_object *o)
{
  def_method(o, "addEventListener", el_add_listener, 3);
  def_method(o, "removeEventListener", el_remove_listener, 3);
  def_method(o, "dispatchEvent", el_dispatch, 1);
}

static void fire_ended(js_object *track)
{
  gc_push_scope();
  gc_keep(track);
  js_value ev = make_event("ended", 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(ctx, ev, "target", js_obj_value(track));
    ev_fire_on(track, ev, 0);
  }
  gc_pop_scope();
}

/* A device went away: its live tracks end and hear about it. */
static int lose_device(int d)
{
  int fired = 0;
  for (int i = 0; i < MAX_TRACKS; i++) {
    js_object *t = held[H_TRACKS + i];
    if (!t || slot_dev[i] != d)
      continue;
    gc_push_scope();
    gc_keep(t);
    held[H_TRACKS + i] = 0;
    serials[i] = 0;
    fire_ended(t);
    gc_pop_scope();
    fired++;
  }
  devs[d]->close();
  return fired;
}

static void new_uuid(char out[UUID_STR_LEN])
{
  uuid_t u;
  uuid_generate_v4(u);
  uuid_to_string(u, out, 0);
}

js_value mediastream_range(double lo, double hi)
{
  js_object *o = (js_object *)gc_keep(new_obj());
  js_def(o, "min", js_number(lo), P_ENUMERABLE);
  js_def(o, "max", js_number(hi), P_ENUMERABLE);
  return js_obj_value(o);
}

/* ---- MediaStreamTrack */

static void set_pick(js_object *t, const struct ms_pick *p)
{
  hide(t, "_p0", js_number(p->v[0]));
  hide(t, "_p1", js_number(p->v[1]));
  hide(t, "_p2", js_number(p->v[2]));
  hide(t, "_p3", js_number(p->v[3]));
}

static void get_pick(js_value t, struct ms_pick *p)
{
  static const char *const keys[4] = {"_p0", "_p1", "_p2", "_p3"};
  for (int i = 0; i < 4; i++)
    p->v[i] = (int)js_to_number(ctx, js_get(ctx, t, keys[i]));
}

static int device_of(js_value t)
{
  int d = (int)js_to_number(ctx, js_get(ctx, t, "_dev"));
  return d >= 0 && d < NDEV ? d : 0;
}

/* A track on device d; `live` false makes an already ended one (a clone of an ended track). */
static js_object *new_track(int d, const struct ms_pick *pick, js_value constraints, bool live)
{
  int slot = -1;
  char id[UUID_STR_LEN];
  if (live) {
    for (int i = 0; i < MAX_TRACKS && slot < 0; i++)
      if (!held[H_TRACKS + i])
        slot = i;
    if (slot < 0)
      return 0;
  }
  js_object *t = js_new_object(ctx, held[H_TRACK_PROTO]);
  gc_keep(t);
  new_uuid(id);
  js_def(t, "kind", js_mk_cstring(ctx, devs[d]->kind), P_ENUMERABLE);
  js_def(t, "id", js_mk_cstring(ctx, id), P_ENUMERABLE);
  js_def(t, "label", js_mk_cstring(ctx, devs[d]->label()), P_ENUMERABLE);
  js_def(t, "muted", js_bool(0), P_ENUMERABLE);
  js_def(t, "contentHint", js_mk_cstring(ctx, ""), P_ENUMERABLE | P_WRITABLE);
  js_def(t, "onended", js_null(), P_ENUMERABLE | P_WRITABLE);
  hide(t, "_en", js_bool(1));
  hide(t, "_dev", js_number(d));
  hide(t, "_cons", constraints.type == JV_OBJ ? constraints : js_obj_value((js_object *)gc_keep(new_obj())));
  set_pick(t, pick);
  if (live) {
    serials[slot] = ++last_serial;
    slot_dev[slot] = (unsigned char)d;
    held[H_TRACKS + slot] = t;
    hide(t, "_cam", js_number(serials[slot]));
  }
  return t;
}

static js_value track_stop(js_ctx *c, js_value t, js_value *args, int argc)
{
  int slot = slot_of(t);
  (void)c;
  (void)args;
  (void)argc;
  if (slot >= 0)
    release_slot(slot);
  return js_undef();
}

static js_value track_clone(js_ctx *c, js_value t, js_value *args, int argc)
{
  struct ms_pick pick;
  js_value rv = js_undef();
  (void)c;
  (void)args;
  (void)argc;
  if (!obj_of(t))
    return js_throw_type(ctx, "Illegal invocation");
  gc_push_scope();
  get_pick(t, &pick);
  js_object *copy = new_track(device_of(t), &pick, js_get(ctx, t, "_cons"), slot_of(t) >= 0);
  if (copy) {
    hide(copy, "_en", js_get(ctx, t, "_en"));
    rv = js_obj_value(copy);
  }
  gc_pop_scope();
  return rv.type == JV_OBJ ? rv : js_throw_type(ctx, "Too many open capture tracks");
}

static js_value track_get_ready_state(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, slot_of(t) >= 0 ? "live" : "ended");
}

static js_value track_get_enabled(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)args;
  (void)argc;
  return js_get(ctx, t, "_en");
}

static js_value track_set_enabled(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  if (obj_of(t))
    js_set(ctx, t, "_en", js_bool(js_to_boolean(A(0))));
  return js_undef();
}

static js_value track_get_settings(js_ctx *c, js_value t, js_value *args, int argc)
{
  struct ms_pick pick;
  js_value rv;
  (void)c;
  (void)args;
  (void)argc;
  if (!obj_of(t))
    return js_throw_type(ctx, "Illegal invocation");
  get_pick(t, &pick);
  gc_push_scope();
  js_object *o = (js_object *)gc_keep(new_obj());
  devs[device_of(t)]->settings(o, &pick);
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}

static js_value track_get_capabilities(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value rv;
  (void)c;
  (void)args;
  (void)argc;
  if (!obj_of(t))
    return js_throw_type(ctx, "Illegal invocation");
  gc_push_scope();
  js_object *o = (js_object *)gc_keep(new_obj());
  devs[device_of(t)]->capabilities(o);
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}

static js_value track_get_constraints(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)args;
  (void)argc;
  return obj_of(t) ? js_get(ctx, t, "_cons") : js_throw_type(ctx, "Illegal invocation");
}

static js_value overconstrained(const char *constraint)
{
  js_value e =
      dom_exception(ctx, "OverconstrainedError", "The constraints could not be satisfied by the available devices.");
  js_set(ctx, e, "constraint", js_mk_cstring(ctx, constraint));
  return js_promise_rejected(ctx, e);
}

static js_value track_apply_constraints(js_ctx *c, js_value t, js_value *args, int argc)
{
  struct ms_pick pick;
  js_value spec = A(0);
  int slot = slot_of(t), d;
  const char *failed;
  (void)c;
  if (!obj_of(t))
    return js_throw_type(ctx, "Illegal invocation");
  if (slot < 0)
    return js_promise_resolved(ctx, js_undef());
  d = slot_dev[slot];
  if (!devs[d]->present())
    return reject("NotFoundError", "Requested device not found");
  if ((failed = devs[d]->choose(spec, live_of(d) > 1, &pick)) != 0)
    return overconstrained(failed);
  if (!devs[d]->open(&pick))
    return reject("NotReadableError", "Could not start source");
  set_pick(obj_of(t), &pick);
  gc_push_scope();
  hide(obj_of(t), "_cons", spec.type == JV_OBJ ? spec : js_obj_value((js_object *)gc_keep(new_obj())));
  gc_pop_scope();
  return js_promise_resolved(ctx, js_undef());
}

/* ---- MediaStream */

static js_value tracks_of(js_value stream) { return js_get(ctx, stream, "_tracks"); }

static int tracks_len(js_value arr) { return (int)js_to_number(ctx, js_get(ctx, arr, "length")); }

static js_object *new_stream(js_object *tracks)
{
  char id[UUID_STR_LEN];
  js_object *s = js_new_object(ctx, held[H_STREAM_PROTO]);
  gc_keep(s);
  new_uuid(id);
  js_def(s, "id", js_mk_cstring(ctx, id), P_ENUMERABLE);
  js_def(s, "onaddtrack", js_null(), P_ENUMERABLE | P_WRITABLE);
  js_def(s, "onremovetrack", js_null(), P_ENUMERABLE | P_WRITABLE);
  hide(s, "_tracks", js_obj_value(tracks));
  return s;
}

static js_value stream_ctor(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value src = A(0), rv;
  (void)c;
  (void)t;
  gc_push_scope();
  js_object *tracks = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (src.type == JV_OBJ) {
    js_value list = js_is_array(src) ? src : tracks_of(src);
    for (int i = 0; js_is_array(list) && i < tracks_len(list); i++) {
      js_value e = js_get_index(ctx, list, (unsigned)i);
      if (e.type == JV_OBJ && js_get(ctx, e, "kind").type == JV_STR)
        js_array_push(ctx, tracks, e);
    }
  }
  rv = js_obj_value(new_stream(tracks));
  gc_pop_scope();
  return rv;
}

/* The stream's tracks, all of them or only those of `kind` */
static js_value stream_tracks(js_value stream, const char *kind)
{
  js_value list = tracks_of(stream), rv;
  if (!js_is_array(list))
    return js_throw_type(ctx, "Illegal invocation");
  gc_push_scope();
  js_object *copy = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (int i = 0; i < tracks_len(list); i++) {
    js_value e = js_get_index(ctx, list, (unsigned)i);
    if (!kind || streq(arg_str(ctx, js_get(ctx, e, "kind")), kind))
      js_array_push(ctx, copy, e);
  }
  rv = js_obj_value(copy);
  gc_pop_scope();
  return rv;
}

static js_value stream_get_tracks(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)args;
  (void)argc;
  return stream_tracks(t, 0);
}

static js_value stream_get_video_tracks(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)args;
  (void)argc;
  return stream_tracks(t, "video");
}

static js_value stream_get_audio_tracks(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)args;
  (void)argc;
  return stream_tracks(t, "audio");
}

static js_value stream_get_track_by_id(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value list = tracks_of(t);
  char want[UUID_STR_LEN + 8];
  (void)c;
  if (!js_is_array(list))
    return js_throw_type(ctx, "Illegal invocation");
  strlcpy(want, arg_str(ctx, A(0)), sizeof want);
  for (int i = 0; i < tracks_len(list); i++) {
    js_value e = js_get_index(ctx, list, (unsigned)i);
    if (streq(arg_str(ctx, js_get(ctx, e, "id")), want))
      return e;
  }
  return js_null();
}

static js_value stream_add_track(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value list = tracks_of(t), track = A(0);
  (void)c;
  if (!js_is_array(list))
    return js_throw_type(ctx, "Illegal invocation");
  if (track.type != JV_OBJ || js_get(ctx, track, "kind").type != JV_STR)
    return js_throw_type(
        ctx, "Failed to execute 'addTrack' on 'MediaStream': parameter 1 is not of type 'MediaStreamTrack'.");
  for (int i = 0; i < tracks_len(list); i++)
    if (js_get_index(ctx, list, (unsigned)i).u.obj == track.u.obj)
      return js_undef();
  js_array_push(ctx, list.u.obj, track);
  return js_undef();
}

static js_value stream_remove_track(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value list = tracks_of(t), track = A(0);
  (void)c;
  if (!js_is_array(list))
    return js_throw_type(ctx, "Illegal invocation");
  gc_push_scope();
  js_object *kept = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (int i = 0; i < tracks_len(list); i++) {
    js_value e = js_get_index(ctx, list, (unsigned)i);
    if (track.type != JV_OBJ || e.u.obj != track.u.obj)
      js_array_push(ctx, kept, e);
  }
  hide(obj_of(t), "_tracks", js_obj_value(kept));
  gc_pop_scope();
  return js_undef();
}

static js_value stream_clone(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value list = tracks_of(t), rv;
  (void)c;
  (void)args;
  (void)argc;
  if (!js_is_array(list))
    return js_throw_type(ctx, "Illegal invocation");
  gc_push_scope();
  js_object *copy = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (int i = 0; i < tracks_len(list); i++) {
    js_value e = js_get_index(ctx, list, (unsigned)i), cl = track_clone(c, e, 0, 0);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    js_array_push(ctx, copy, cl);
  }
  rv = js_obj_value(new_stream(copy));
  gc_pop_scope();
  return rv;
}

static js_value stream_get_active(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value list = tracks_of(t);
  (void)c;
  (void)args;
  (void)argc;
  for (int i = 0; js_is_array(list) && i < tracks_len(list); i++)
    if (slot_of(js_get_index(ctx, list, (unsigned)i)) >= 0)
      return js_bool(1);
  return js_bool(0);
}

bool mediastream_is_stream(js_value v) { return ctx && v.type == JV_OBJ && js_is_array(js_get(ctx, v, "_tracks")); }

/* ---- mediaDevices */

static bool wants(js_value constraints, const char *kind)
{
  return constraints.type == JV_OBJ && js_to_boolean(js_get(ctx, constraints, kind));
}

static js_value get_user_media(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value spec = A(0), rv;
  struct ms_pick pick[NDEV];
  bool want[NDEV], fresh[NDEV];
  int wanted = 0, free_slots = 0;
  const char *failed;
  (void)c;
  (void)t;
  for (int d = 0; d < NDEV; d++)
    wanted += want[d] = wants(spec, devs[d]->kind);
  if (!wanted)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError",
                                                  "At least one of audio and video must be requested"));
  for (int d = 0; d < NDEV; d++)
    if (want[d] && !devs[d]->present())
      return reject("NotFoundError", "Requested device not found");
  for (int d = 0; d < NDEV; d++)
    if (want[d] && worker_permission(devs[d]->feature) != PERM_GRANTED)
      return reject("NotAllowedError", "Permission denied");
  for (int d = 0; d < NDEV; d++)
    if (want[d] && (failed = devs[d]->choose(js_get(ctx, spec, devs[d]->kind), devs[d]->running(), &pick[d])) != 0)
      return overconstrained(failed);
  for (int i = 0; i < MAX_TRACKS; i++)
    free_slots += !held[H_TRACKS + i];
  if (free_slots < wanted)
    return reject("NotReadableError", "Too many open capture tracks");
  for (int d = 0; d < NDEV; d++) {
    fresh[d] = want[d] && !devs[d]->running();
    if (want[d] && !devs[d]->open(&pick[d])) {
      for (int k = 0; k < d; k++)
        if (fresh[k])
          devs[k]->close();
      return reject("NotReadableError", "Could not start source");
    }
  }
  gc_push_scope();
  js_object *tracks = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (int d = 0; d < NDEV; d++)
    if (want[d])
      js_array_push(ctx, tracks, js_obj_value(new_track(d, &pick[d], js_get(ctx, spec, devs[d]->kind), true)));
  rv = js_promise_resolved(ctx, js_obj_value(new_stream(tracks)));
  gc_pop_scope();
  return rv;
}

static js_value get_display_media(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)t;
  (void)args;
  (void)argc;
  return reject("NotAllowedError", "Permission denied");
}

static js_value enumerate_devices(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value rv;
  (void)c;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  js_object *list = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (int d = 0; d < NDEV; d++) {
    if (!devs[d]->present())
      continue;
    bool known = live_of(d) || perm_state_now(devs[d]->feature) == PERM_GRANTED;
    js_object *e = (js_object *)gc_keep(new_obj());
    js_def(e, "deviceId", js_mk_cstring(ctx, known ? devs[d]->id() : ""), P_ENUMERABLE);
    js_def(e, "kind", js_mk_cstring(ctx, devs[d]->input), P_ENUMERABLE);
    js_def(e, "label", js_mk_cstring(ctx, known ? devs[d]->label() : ""), P_ENUMERABLE);
    js_def(e, "groupId", js_mk_cstring(ctx, known ? devs[d]->id() : ""), P_ENUMERABLE);
    js_array_push(ctx, list, js_obj_value(e));
  }
  rv = js_promise_resolved(ctx, js_obj_value(list));
  gc_pop_scope();
  return rv;
}

static js_value get_supported_constraints(js_ctx *c, js_value t, js_value *args, int argc)
{
  static const char *const names[] = {
      "width",      "height",       "frameRate",        "aspectRatio",      "deviceId",       "groupId",
      "sampleRate", "channelCount", "echoCancellation", "noiseSuppression", "autoGainControl"};
  js_value rv;
  (void)c;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  js_object *o = (js_object *)gc_keep(new_obj());
  for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
    js_def(o, names[i], js_bool(1), P_ENUMERABLE);
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}

/* ---- setup */

void mediastream_device_id(const char *page_url, const char *salt, char out[17])
{
  uint64_t h = 14695981039346656037ull;
  int slashes = 0;
  for (const char *p = page_url; *p && (*p != '/' || ++slashes < 3); p++)
    h = (h ^ (unsigned char)*p) * 1099511628211ull;
  for (const char *p = salt; *p; p++)
    h = (h ^ (unsigned char)*p) * 1099511628211ull;
  snprintf(out, 17, "%08x%08x", (unsigned)(h >> 32), (unsigned)h);
}

static void def_global_ctor(const char *name, js_native fn, js_object *proto)
{
  js_object *ctor = js_new_native(ctx, fn, name, 0);
  gc_push_scope();
  gc_keep(ctor);
  js_def(ctor, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(ctx->global, name, js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
}

void mediastream_install(js_ctx *c, js_object *navigator, const char *page_url)
{
  if (!ch_secure_url(page_url))
    return;
  ctx = c;
  memset(held, 0, sizeof held);
  memset(serials, 0, sizeof serials);
  devs[0] = microphone_device();
  devs[1] = camera_device();
  for (int d = 0; d < NDEV; d++)
    devs[d]->init(c, page_url);
  gc_add_root_range((void **)held, &held_n);

  js_object *track = held[H_TRACK_PROTO] = new_obj(), *stream = held[H_STREAM_PROTO] = new_obj();
  def_method(track, "stop", track_stop, 0);
  def_method(track, "clone", track_clone, 0);
  def_method(track, "getSettings", track_get_settings, 0);
  def_method(track, "getCapabilities", track_get_capabilities, 0);
  def_method(track, "getConstraints", track_get_constraints, 0);
  def_method(track, "applyConstraints", track_apply_constraints, 1);
  def_acc(track, "enabled", track_get_enabled, track_set_enabled);
  def_acc(track, "readyState", track_get_ready_state, 0);
  add_event_target(track);
  def_method(stream, "getTracks", stream_get_tracks, 0);
  def_method(stream, "getVideoTracks", stream_get_video_tracks, 0);
  def_method(stream, "getAudioTracks", stream_get_audio_tracks, 0);
  def_method(stream, "getTrackById", stream_get_track_by_id, 1);
  def_method(stream, "addTrack", stream_add_track, 1);
  def_method(stream, "removeTrack", stream_remove_track, 1);
  def_method(stream, "clone", stream_clone, 0);
  def_acc(stream, "active", stream_get_active, 0);
  add_event_target(stream);
  def_global_ctor("MediaStreamTrack", illegal_ctor, track);
  def_global_ctor("MediaStream", stream_ctor, stream);

  js_object *md = new_obj();
  def_method(md, "getUserMedia", get_user_media, 1);
  def_method(md, "getDisplayMedia", get_display_media, 1);
  def_method(md, "enumerateDevices", enumerate_devices, 0);
  def_method(md, "getSupportedConstraints", get_supported_constraints, 0);
  js_def(md, "ondevicechange", js_null(), P_ENUMERABLE | P_WRITABLE);
  add_event_target(md);
  js_def(navigator, "mediaDevices", js_obj_value(md), P_ENUMERABLE);
}

int mediastream_tick(void)
{
  int fired = 0;
  if (!ctx)
    return 0;
  for (int d = 0; d < NDEV; d++)
    if (live_of(d) && !devs[d]->tick(msec()))
      fired += lose_device(d);
  return fired;
}

void mediastream_teardown(void)
{
  if (!ctx)
    return;
  for (int d = 0; d < NDEV; d++)
    devs[d]->teardown();
  gc_remove_root_range((void **)held);
  memset(held, 0, sizeof held);
  ctx = 0;
}
