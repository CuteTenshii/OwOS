/* HTMLMediaElement, Remote Playback API (Cast) and Image()/Audio() constructors. */
#include <stdbool.h>

#include "cast.h"                  /* Google Cast sender: RemotePlayback backend             */
#include "features/media/bmedia.h" /* HTMLMediaElement backend: media_* over the mixer        */
#include "jsint.h"

/* Single process-wide Cast session state. */
static cast_session *g_cast;     /* the single active Cast session, or NULL      */
static js_object *g_cast_remote; /* the RemotePlayback owning g_cast, or NULL */
static int g_cast_used;          /* a page touched a remote-playback entry point */
static int g_cast_conn_seen;     /* last connection state delivered by the pump  */

/* ---- HTMLMediaElement ---- */
static struct media_el *mel_of(js_value t)
{
  struct dom_node *n = node_of(t);
  return n ? media_for_node(n) : 0;
}
static js_value mel_get_src(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char buf[URL_MAX];
  (void)args;
  (void)argc;
  buf[0] = '\0';
  media_get_src(mel_of(t), buf, sizeof buf);
  return js_mk_cstring(ctx, buf);
}
static js_value mel_set_src(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *raw = arg_str(ctx, A(0));
  char abs[URL_MAX];
  if (!n)
    return js_undef();
  dom_set_attr(n, "src", raw);
  resolve_url(g_page_url_buf, raw, abs, sizeof abs);
  media_set_src(media_for_node(n), abs);
  return js_undef();
}
static js_value mel_get_currentTime(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return js_number(media_current_time(mel_of(t)));
}
static js_value mel_set_currentTime(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  media_set_current_time(mel_of(t), js_to_number(ctx, A(0)));
  return js_undef();
}
static js_value mel_get_duration(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return js_number(media_duration(mel_of(t)));
}
static js_value mel_get_paused(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return js_bool(media_paused(mel_of(t)));
}
static js_value mel_get_ended(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return js_bool(media_ended(mel_of(t)));
}
static js_value mel_get_volume(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return js_number(media_volume(mel_of(t)));
}
static js_value mel_set_volume(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  media_set_volume(mel_of(t), js_to_number(ctx, A(0)));
  return js_undef();
}
static js_value mel_get_muted(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return js_bool(media_muted(mel_of(t)));
}
static js_value mel_set_muted(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  media_set_muted(mel_of(t), js_to_boolean(A(0)));
  return js_undef();
}
static js_value mel_get_loop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return js_bool(media_loop_get(mel_of(t)));
}
static js_value mel_set_loop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  bool on = js_to_boolean(A(0));
  media_set_loop(media_for_node(n), on);
  if (n) {
    if (on)
      dom_set_attr(n, "loop", "");
    else
      dom_remove_attr(n, "loop");
  }
  return js_undef();
}
static js_value mel_get_autoplay(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return js_bool(media_autoplay_get(mel_of(t)));
}
static js_value mel_set_autoplay(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  bool on = js_to_boolean(A(0));
  media_set_autoplay(media_for_node(n), on);
  if (n) {
    if (on)
      dom_set_attr(n, "autoplay", "");
    else
      dom_remove_attr(n, "autoplay");
  }
  return js_undef();
}
static js_value mel_get_controls(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return js_bool(n && dom_get_attr(n, "controls") != 0);
}
static js_value mel_set_controls(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  if (n) {
    if (js_to_boolean(A(0)))
      dom_set_attr(n, "controls", "");
    else
      dom_remove_attr(n, "controls");
  }
  return js_undef();
}
static js_value mel_get_readyState(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return js_number(media_ready_state(mel_of(t)));
}
static js_value mel_get_networkState(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return js_number(media_network_state(mel_of(t)));
}

static js_value mel_play(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  media_play(mel_of(t));
  return js_promise_resolved(ctx, js_undef());
}
static js_value mel_pause(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  media_pause(mel_of(t));
  return js_undef();
}
static js_value mel_load(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  media_load(mel_of(t));
  return js_undef();
}
static js_value mel_can_play_type(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int v = media_can_play_type(arg_str(ctx, A(0)));
  (void)t;
  return js_mk_cstring(ctx, v == MEDIA_PROBABLY ? "probably" : v == MEDIA_MAYBE ? "maybe" : "");
}

/* new Audio(src): a detached audio element, preloaded when src is given. */
static js_value audio_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  struct dom_node *n;
  js_value w;
  (void)thisv;
  n = dom_new_element("audio");
  if (!n)
    return js_throw_type(ctx, "Audio");
  media_new_audio(n);
  w = wrap_node(n); /* g_media_proto, pinned alive */
  if (argc >= 1 && A(0).type != JV_UNDEF && A(0).type != JV_NULL) {
    const char *raw = arg_str(ctx, A(0));
    char abs[URL_MAX];
    struct media_el *m = media_for_node(n);
    dom_set_attr(n, "src", raw);
    resolve_url(g_page_url_buf, raw, abs, sizeof abs);
    media_set_src(m, abs);
    media_load(m); /* new Audio(src) preloads */
  }
  return w;
}

/* new Image(w, h): a detached img with width/height attributes preset. */
js_value image_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  struct dom_node *n;
  js_value w;
  (void)thisv;
  n = dom_new_element("img");
  if (!n)
    return js_throw_type(ctx, "Image");
  w = wrap_node(n); /* HTMLImageElement proto, pinned */
  if (argc >= 1 && A(0).type != JV_UNDEF && A(0).type != JV_NULL)
    dom_set_attr(n, "width", arg_str(ctx, A(0)));
  if (argc >= 2 && A(1).type != JV_UNDEF && A(1).type != JV_NULL)
    dom_set_attr(n, "height", arg_str(ctx, A(1)));
  return w;
}

static js_value mel_get_remote(js_ctx *ctx, js_value t, js_value *args, int argc);

void setup_media_proto(js_ctx *ctx)
{
  g_cast = 0;
  g_cast_remote = 0;
  g_cast_used = 0;
  g_cast_conn_seen = 0;
  def_acc(g_media_proto, "src", mel_get_src, mel_set_src);
  def_acc(g_media_proto, "currentSrc", mel_get_src, 0);
  def_acc(g_media_proto, "currentTime", mel_get_currentTime, mel_set_currentTime);
  def_acc(g_media_proto, "duration", mel_get_duration, 0);
  def_acc(g_media_proto, "paused", mel_get_paused, 0);
  def_acc(g_media_proto, "ended", mel_get_ended, 0);
  def_acc(g_media_proto, "volume", mel_get_volume, mel_set_volume);
  def_acc(g_media_proto, "muted", mel_get_muted, mel_set_muted);
  def_acc(g_media_proto, "loop", mel_get_loop, mel_set_loop);
  def_acc(g_media_proto, "autoplay", mel_get_autoplay, mel_set_autoplay);
  def_acc(g_media_proto, "controls", mel_get_controls, mel_set_controls);
  def_acc(g_media_proto, "readyState", mel_get_readyState, 0);
  def_acc(g_media_proto, "networkState", mel_get_networkState, 0);
  def_acc(g_media_proto, "remote", mel_get_remote, 0); /* Remote Playback API */
  def_method(g_media_proto, "play", mel_play, 0);
  def_method(g_media_proto, "pause", mel_pause, 0);
  def_method(g_media_proto, "load", mel_load, 0);
  def_method(g_media_proto, "canPlayType", mel_can_play_type, 1);
  js_def(g_media_proto, "HAVE_NOTHING", js_number(MRS_NOTHING), P_ENUMERABLE);
  js_def(g_media_proto, "HAVE_METADATA", js_number(MRS_METADATA), P_ENUMERABLE);
  js_def(g_media_proto, "HAVE_CURRENT_DATA", js_number(MRS_CURRENT), P_ENUMERABLE);
  js_def(g_media_proto, "HAVE_FUTURE_DATA", js_number(MRS_FUTURE), P_ENUMERABLE);
  js_def(g_media_proto, "HAVE_ENOUGH_DATA", js_number(MRS_ENOUGH), P_ENUMERABLE);
  {
    js_object *ac = js_new_native(ctx, audio_ctor, "Audio", 1);
    gc_push_scope();
    gc_keep(ac);
    js_def(ac, "prototype", js_obj_value(g_media_proto), 0);
    js_def(g_media_proto, "constructor", js_obj_value(ac), P_WRITABLE | P_CONFIGURABLE);
    js_def(ctx->global, "Audio", js_obj_value(ac), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
}

/* Fire a non-bubbling media event on an element wrapper (worker thread). */
void js_media_fire(struct dom_node *node, const char *type)
{
  if (!g_page_live || !g_ctx || !node || !node->wrapper)
    return;
  emit_event((js_object *)node->wrapper, type, js_undef());
}

/* ---- Remote Playback API ---- */

/* Coarse contentType from the URL extension. */
static const char *cast_ctype_for(const char *url)
{
  int n = url ? (int)strlen(url) : 0, i;
  char ext[8];
  int e = 0;
  for (i = n - 1; i >= 0 && url[i] != '.' && url[i] != '/' && e < 7; i--)
    ext[e++] = lower(url[i]);
  ext[e] = '\0';
  if (streq(ext, "4pm") || streq(ext, "vom") || streq(ext, "4m"))
    return "video/mp4";
  if (streq(ext, "mbew"))
    return "video/webm";
  if (streq(ext, "vko"))
    return "video/ogg";
  if (streq(ext, "3pm"))
    return "audio/mpeg";
  if (streq(ext, "vaw"))
    return "audio/wav";
  if (streq(ext, "ggo") || streq(ext, "aggo"))
    return "audio/ogg";
  if (streq(ext, "ca4m") || streq(ext, "caa"))
    return "audio/aac";
  if (streq(ext, "8u3m"))
    return "application/x-mpegurl"; /* .m3u8 (HLS) */
  return "";
}

/* The media element behind a RemotePlayback (its host is the dom_node). */
static struct dom_node *remote_node(js_value t) { return (t.type == JV_OBJ) ? (struct dom_node *)t.u.obj->host : 0; }

static js_value rp_get_state(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *st = "disconnected";
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ && t.u.obj == g_cast_remote && g_cast) {
    struct cast_status s;
    cast_get_status(g_cast, &s);
    st = s.conn == CAST_CONNECTED ? "connected" : s.conn == CAST_CONNECTING ? "connecting" : "disconnected";
  }
  return js_mk_cstring(ctx, st);
}

/* watchAvailability(cb): register a monitor; the pump delivers the first value. */
static js_value rp_watch_availability(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value cb = A(0), watch;
  (void)argc;
  if (t.type != JV_OBJ)
    return js_promise_rejected(ctx,
                               js_make_error(ctx, ctx->type_error_proto, "TypeError", "watchAvailability: bad target"));
  if (!js_is_callable(cb))
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "watchAvailability: callback required"));
  cast_scan_start();
  g_cast_used = 1;
  watch = js_get(ctx, t, "@watch");
  if (!js_is_array(watch)) {
    watch = js_obj_value(js_new_array(ctx, 0));
    js_def(t.u.obj, "@watch", watch, P_WRITABLE);
  }
  js_array_push(ctx, watch.u.obj, cb);
  js_def(t.u.obj, "@availseen", js_number(-1), P_WRITABLE); /* force a re-report */
  return js_promise_resolved(ctx, js_number((double)js_array_len(watch.u.obj)));
}

static js_value rp_cancel_watch(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value watch = t.type == JV_OBJ ? js_get(ctx, t, "@watch") : js_undef();
  int id = (int)js_to_number(ctx, A(0));
  (void)argc;
  if (js_is_array(watch) && id >= 1 && id <= (int)js_array_len(watch.u.obj))
    js_set_index(ctx, watch, (unsigned)(id - 1), js_null()); /* skipped by the pump */
  return js_promise_resolved(ctx, js_undef());
}

/* prompt(): discover, connect to the first receiver, load and play. */
static js_value rp_prompt(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = remote_node(t);
  struct cast_device dev;
  bool have = false;
  (void)args;
  (void)argc;
  if (t.type != JV_OBJ)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "prompt: bad target"));

  cast_scan_start();
  g_cast_used = 1;
  if (cast_scan_count() > 0 && cast_scan_get(0, &dev))
    have = true;
  if (!have) {
    struct cast_device list[8];
    int c = cast_discover(list, 8, 4000);
    if (c > 0) {
      dev = list[0];
      have = true;
    }
  }
  if (!have)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "NotFoundError", "No available Cast devices"));

  if (g_cast) {
    cast_session *d = g_cast;
    g_cast = 0;
    g_cast_remote = 0;
    cast_close(d);
  }
  g_cast_conn_seen = CAST_DISCONNECTED;
  emit_event(t.u.obj, "connecting", js_undef());

  {
    cast_session *s = cast_connect(&dev, 10000);
    if (!s)
      return js_promise_rejected(
          ctx, js_make_error(ctx, ctx->error_proto, "NotAllowedError", "Could not connect to the Cast device"));
    g_cast = s;
    g_cast_remote = t.u.obj;
    g_cast_conn_seen = CAST_CONNECTED;
  }
  if (n) {
    char src[URL_MAX];
    src[0] = '\0';
    media_get_src(media_for_node(n), src, sizeof src);
    if (src[0])
      cast_load(g_cast, src, cast_ctype_for(src), 0, 8000);
  }
  emit_event(t.u.obj, "connect", js_undef());
  return js_promise_resolved(ctx, js_undef());
}

/* disconnect(): non-standard; stop casting and return to local playback. */
static js_value rp_disconnect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ && g_cast && g_cast_remote == t.u.obj) {
    cast_session *d = g_cast;
    g_cast = 0;
    g_cast_remote = 0;
    g_cast_conn_seen = CAST_DISCONNECTED;
    emit_event(t.u.obj, "disconnect", js_undef());
    cast_close(d); /* blocks up to ~5 s while the reader unwinds */
  }
  return js_undef();
}

/* `remote` accessor: one cached RemotePlayback per element. */
static js_value mel_get_remote(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_value cached;
  js_object *rp;
  (void)args;
  (void)argc;
  if (!n || t.type != JV_OBJ)
    return js_null();
  cached = js_get(ctx, t, "@remote");
  if (cached.type == JV_OBJ)
    return cached;
  gc_push_scope();
  rp = (js_object *)gc_keep(js_new_object(ctx, g_remote_proto));
  rp->host = n;
  js_def(rp, "@watch", js_obj_value(js_new_array(ctx, 0)), 0);
  js_def(rp, "@availseen", js_number(-1), P_WRITABLE);
  js_def(t.u.obj, "@remote", js_obj_value(rp), P_WRITABLE | P_CONFIGURABLE);
  if (g_remotes)
    js_array_push(ctx, g_remotes, js_obj_value(rp));
  gc_pop_scope();
  return js_obj_value(rp);
}

void setup_remote_proto(js_ctx *ctx)
{
  def_acc(g_remote_proto, "state", rp_get_state, 0);
  def_method(g_remote_proto, "watchAvailability", rp_watch_availability, 1);
  def_method(g_remote_proto, "cancelWatchAvailability", rp_cancel_watch, 1);
  def_method(g_remote_proto, "prompt", rp_prompt, 0);
  def_method(g_remote_proto, "disconnect", rp_disconnect, 0);
  def_method(g_remote_proto, "addEventListener", el_add_listener, 2);
  def_method(g_remote_proto, "removeEventListener", el_remove_listener, 2);
  def_method(g_remote_proto, "dispatchEvent", el_dispatch, 1);
}

/* Worker pump: deliver availability and connection events; returns the number fired. */
int js_cast_pump(void)
{
  int fired = 0;
  if (!g_page_live || !g_ctx || !g_cast_used)
    return 0;

  if (g_remotes) {
    int avail = cast_scan_count() > 0 ? 1 : 0;
    unsigned n = js_array_len(g_remotes), i;
    for (i = 0; i < n; i++) {
      js_value rv = js_get_index(g_ctx, js_obj_value(g_remotes), i);
      js_value watch;
      unsigned m, k;
      if (rv.type != JV_OBJ)
        continue;
      watch = js_get(g_ctx, rv, "@watch");
      if (!js_is_array(watch) || js_array_len(watch.u.obj) == 0)
        continue;
      if ((int)js_to_number(g_ctx, js_get(g_ctx, rv, "@availseen")) == avail)
        continue;
      js_def(rv.u.obj, "@availseen", js_number(avail), P_WRITABLE);
      m = js_array_len(watch.u.obj);
      for (k = 0; k < m; k++) {
        js_value cb = js_get_index(g_ctx, watch, k);
        if (js_is_callable(cb)) {
          js_value a = js_bool(avail);
          js_call(g_ctx, cb, js_undef(), &a, 1);
          if (g_ctx->has_exc)
            report_exc(g_ctx);
          fired++;
        }
      }
    }
  }

  if (g_cast && g_cast_remote) {
    struct cast_status s;
    cast_get_status(g_cast, &s);
    if (s.conn != g_cast_conn_seen) {
      g_cast_conn_seen = s.conn;
      if (s.conn == CAST_CONNECTING)
        emit_event(g_cast_remote, "connecting", js_undef());
      else if (s.conn == CAST_CONNECTED)
        emit_event(g_cast_remote, "connect", js_undef());
      else if (s.conn == CAST_DISCONNECTED) {
        cast_session *dead = g_cast;
        js_object *rp = g_cast_remote;
        g_cast = 0;
        g_cast_remote = 0;
        emit_event(rp, "disconnect", js_undef());
        cast_close(dead); /* already unwinding; join is immediate */
      }
      fired++;
    }
  }
  return fired;
}

/* Close any active session at page teardown (navigation). */
void js_cast_page_end(void)
{
  if (g_cast) {
    cast_session *d = g_cast;
    g_cast = 0;
    g_cast_remote = 0;
    cast_close(d);
  }
  g_cast_used = 0;
  g_cast_conn_seen = 0;
}
