#include <stdbool.h>

#include "features/media/bmedia.h"
#include "features/mediasession/mediasession.h"
#include "jsint.h"
#include "mediactl/mediactl.h"
#include "url.h"

#define PUBLISH_MIN_MS 100
#define PUBLISH_REFRESH_MS 1000
#define SEEK_DRIFT_MS 1500
#define POLL_MS 100
#define OPEN_RETRY_MS 2000
#define DEFAULT_SEEK_MS 10000

enum { PS_NONE, PS_PAUSED, PS_PLAYING };

static const struct {
  const char *name;
  int cmd;
} actions[] = {
    {"play", MEDIA_CMD_PLAY},
    {"pause", MEDIA_CMD_PAUSE},
    {"stop", MEDIA_CMD_STOP},
    {"seekbackward", MEDIA_CMD_SEEK_BACK},
    {"seekforward", MEDIA_CMD_SEEK_FWD},
    {"seekto", MEDIA_CMD_SEEK_TO},
    {"previoustrack", MEDIA_CMD_PREV},
    {"nexttrack", MEDIA_CMD_NEXT},
    {"skipad", MEDIA_CMD_NONE},
    {"hangup", MEDIA_CMD_NONE},
    {"togglemicrophone", MEDIA_CMD_NONE},
    {"togglecamera", MEDIA_CMD_NONE},
    {"previousslide", MEDIA_CMD_NONE},
    {"nextslide", MEDIA_CMD_NONE},
};
#define N_ACTIONS ((int)(sizeof actions / sizeof actions[0]))

static const char *const state_names[] = {"none", "paused", "playing"};

static js_ctx *ctx;
static js_object *meta_proto;
static js_object *handlers;
static js_object *metadata;
static char base_url[URL_MAX];
static int playback_state;

static struct {
  bool set;
  double duration, rate, position;
  unsigned stamp;
} pos_state;

static int sid = -1;
static bool have_last;
static struct media_info last;
static unsigned last_publish, last_poll, next_open;

static js_object *create_object(void) { return js_new_object(ctx, ctx->object_proto); }

static bool is_metadata(js_value v) { return v.type == JV_OBJ && js_has_own(v.u.obj, "@title"); }

static js_value illegal(void) { return js_throw_type(ctx, "Illegal invocation"); }

static bool read_dict_string(js_value dict, const char *key, js_value *out)
{
  js_value v = js_get(ctx, dict, key);
  if (v.type == JV_UNDEF)
    return false;
  *out = js_to_string(ctx, v);
  return true;
}

static bool is_alpha(char c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }

static bool has_scheme(const char *s)
{
  int i = 1;
  if (!is_alpha(s[0]))
    return false;
  while (is_alpha(s[i]) || (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' || s[i] == '.')
    i++;
  return s[i] == ':';
}

static js_value normalize_artwork(js_value list)
{
  js_object *result;
  unsigned i, n;
  char url[URL_MAX];
  js_value out;
  if (list.type == JV_UNDEF || list.type == JV_NULL)
    return js_obj_value(js_new_array(ctx, 0));
  if (!js_is_array(list))
    return js_throw_type(ctx, "Failed to construct 'MediaMetadata': artwork must be a sequence");
  gc_push_scope();
  result = gc_keep(js_new_array(ctx, 0));
  n = js_array_len(list.u.obj);
  for (i = 0; i < n; i++) {
    js_value item = js_get_index(ctx, list, i), src, sizes, type;
    js_object *image;
    if (item.type != JV_OBJ || !read_dict_string(item, "src", &src) || !src.u.str->data[0]) {
      gc_pop_scope();
      return js_throw_type(ctx, "Failed to construct 'MediaMetadata': invalid artwork image");
    }
    gc_keep(src.u.str);
    if (has_scheme(src.u.str->data))
      strlcpy(url, src.u.str->data, sizeof url);
    else
      resolve_url(base_url, src.u.str->data, url, sizeof url);
    image = gc_keep(create_object());
    js_def(image, "src", js_mk_cstring(ctx, url), P_ENUMERABLE | P_WRITABLE);
    if (!read_dict_string(item, "sizes", &sizes))
      sizes = js_mk_cstring(ctx, "");
    gc_keep(sizes.u.str);
    js_def(image, "sizes", sizes, P_ENUMERABLE | P_WRITABLE);
    if (!read_dict_string(item, "type", &type))
      type = js_mk_cstring(ctx, "");
    gc_keep(type.u.str);
    js_def(image, "type", type, P_ENUMERABLE | P_WRITABLE);
    js_array_push(ctx, result, js_obj_value(image));
  }
  out = js_obj_value(result);
  gc_pop_scope();
  return out;
}

static js_value metadata_ctor(js_ctx *c, js_value thisv, js_value *args, int argc)
{
  js_object *object;
  js_value init = A(0), value, artwork;
  static const char *const fields[] = {"title", "artist", "album"};
  int i;
  (void)thisv;
  if (init.type != JV_UNDEF && init.type != JV_NULL && init.type != JV_OBJ)
    return js_throw_type(c, "Failed to construct 'MediaMetadata': parameter 1 is not of type 'MediaMetadataInit'");
  gc_push_scope();
  object = gc_keep(js_new_object(c, meta_proto));
  for (i = 0; i < 3; i++) {
    char key[8] = "@";
    strlcpy(key + 1, fields[i], sizeof key - 1);
    if (init.type != JV_OBJ || !read_dict_string(init, fields[i], &value))
      value = js_mk_cstring(c, "");
    gc_keep(value.u.str);
    js_def(object, key, value, P_WRITABLE);
  }
  artwork = normalize_artwork(init.type == JV_OBJ ? js_get(c, init, "artwork") : js_undef());
  if (c->has_exc) {
    gc_pop_scope();
    return js_undef();
  }
  gc_keep(artwork.u.obj);
  js_def(object, "@artwork", artwork, P_WRITABLE);
  value = js_obj_value(object);
  gc_pop_scope();
  return value;
}

static js_value text_get(js_value thisv, const char *key)
{
  return is_metadata(thisv) ? js_get(ctx, thisv, key) : illegal();
}

static js_value text_set(js_value thisv, const char *key, js_value value)
{
  if (!is_metadata(thisv))
    return illegal();
  gc_push_scope();
  value = js_to_string(ctx, value);
  gc_keep(value.u.str);
  js_set(ctx, thisv, key, value);
  gc_pop_scope();
  return js_undef();
}

#define TEXT_ACCESSORS(name)                                                                                           \
  static js_value get_##name(js_ctx *c, js_value t, js_value *args, int argc)                                          \
  {                                                                                                                    \
    (void)c, (void)args, (void)argc;                                                                                   \
    return text_get(t, "@" #name);                                                                                     \
  }                                                                                                                    \
  static js_value set_##name(js_ctx *c, js_value t, js_value *args, int argc)                                          \
  {                                                                                                                    \
    (void)c;                                                                                                           \
    return text_set(t, "@" #name, A(0));                                                                               \
  }

TEXT_ACCESSORS(title)
TEXT_ACCESSORS(artist)
TEXT_ACCESSORS(album)

static js_value get_artwork(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c, (void)args, (void)argc;
  return text_get(t, "@artwork");
}

static js_value set_artwork(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value artwork;
  (void)c;
  if (!is_metadata(t))
    return illegal();
  artwork = normalize_artwork(A(0));
  if (!c->has_exc) {
    gc_push_scope();
    gc_keep(artwork.u.obj);
    js_set(c, t, "@artwork", artwork);
    gc_pop_scope();
  }
  return js_undef();
}

static js_value session_get_metadata(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c, (void)t, (void)args, (void)argc;
  return metadata ? js_obj_value(metadata) : js_null();
}

static js_value session_set_metadata(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value v = A(0);
  (void)t;
  if (v.type != JV_NULL && !is_metadata(v))
    return js_throw_type(c, "Failed to set the 'metadata' property on 'MediaSession': "
                            "The provided value is not of type 'MediaMetadata'.");
  metadata = v.type == JV_NULL ? 0 : v.u.obj;
  return js_undef();
}

static js_value session_get_state(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)t, (void)args, (void)argc;
  return js_mk_cstring(c, state_names[playback_state]);
}

static js_value session_set_state(js_ctx *c, js_value t, js_value *args, int argc)
{
  const char *s = arg_str(c, A(0));
  int i;
  (void)t;
  for (i = 0; i < 3; i++)
    if (!strcmp(s, state_names[i]))
      playback_state = i;
  return js_undef();
}

static int action_index(const char *name)
{
  int i;
  for (i = 0; i < N_ACTIONS; i++)
    if (!strcmp(actions[i].name, name))
      return i;
  return -1;
}

static js_value session_set_action_handler(js_ctx *c, js_value t, js_value *args, int argc)
{
  const char *name = arg_str(c, A(0));
  js_value fn = A(1);
  int i = action_index(name);
  (void)t;
  if (i < 0)
    return js_throw_type(c, "Failed to execute 'setActionHandler' on 'MediaSession': "
                            "The provided value is not a valid enum value of type MediaSessionAction.");
  if (fn.type == JV_NULL || fn.type == JV_UNDEF)
    js_delete(handlers, actions[i].name);
  else if (js_is_callable(fn))
    js_def(handlers, actions[i].name, fn, P_WRITABLE | P_ENUMERABLE);
  else
    return js_throw_type(c, "Failed to execute 'setActionHandler' on 'MediaSession': "
                            "The callback provided as parameter 2 is not a function.");
  return js_undef();
}

static js_value session_set_position_state(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value state = A(0), v;
  double duration, rate = 1.0, position = 0.0;
  (void)t;
  if (state.type == JV_UNDEF || state.type == JV_NULL) {
    pos_state.set = false;
    return js_undef();
  }
  if (state.type != JV_OBJ)
    return js_throw_type(c, "Failed to execute 'setPositionState' on 'MediaSession': "
                            "The provided value is not of type 'MediaPositionState'.");
  v = js_get(c, state, "duration");
  if (v.type == JV_UNDEF && js_get(c, state, "playbackRate").type == JV_UNDEF &&
      js_get(c, state, "position").type == JV_UNDEF) {
    pos_state.set = false;
    return js_undef();
  }
  if (v.type == JV_UNDEF)
    return js_throw_type(c, "Failed to execute 'setPositionState' on 'MediaSession': "
                            "The provided duration is required.");
  duration = js_to_number(c, v);
  v = js_get(c, state, "playbackRate");
  if (v.type != JV_UNDEF)
    rate = js_to_number(c, v);
  v = js_get(c, state, "position");
  if (v.type != JV_UNDEF)
    position = js_to_number(c, v);
  if (duration != duration || duration < 0)
    return js_throw_type(c, "Failed to execute 'setPositionState' on 'MediaSession': "
                            "The provided duration cannot be negative.");
  if (rate != rate || rate == 0 || rate - rate != 0)
    return js_throw_type(c, "Failed to execute 'setPositionState' on 'MediaSession': "
                            "The provided playbackRate cannot be equal to zero.");
  if (position != position || position < 0 || position > duration)
    return js_throw_type(c, "Failed to execute 'setPositionState' on 'MediaSession': "
                            "The provided position cannot be greater than the duration.");
  pos_state.set = true;
  pos_state.duration = duration;
  pos_state.rate = rate;
  pos_state.position = position;
  pos_state.stamp = msec();
  return js_undef();
}

static js_value session_resolved(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)t, (void)args, (void)argc;
  return js_promise_resolved(c, js_undef());
}

static void clear_page_state(void)
{
  metadata = 0;
  playback_state = PS_NONE;
  pos_state.set = false;
  have_last = false;
  last_publish = last_poll = next_open = 0;
}

void mediasession_install(js_ctx *c, js_object *navigator, const char *page_url)
{
  js_object *constructor, *session;
  ctx = c;
  clear_page_state();
  strlcpy(base_url, page_url ? page_url : "", sizeof base_url);
  gc_push_scope();
  meta_proto = create_object();
  handlers = create_object();
  gc_add_root((void **)&meta_proto);
  gc_add_root((void **)&handlers);
  gc_add_root((void **)&metadata);
  def_acc(meta_proto, "title", get_title, set_title);
  def_acc(meta_proto, "artist", get_artist, set_artist);
  def_acc(meta_proto, "album", get_album, set_album);
  def_acc(meta_proto, "artwork", get_artwork, set_artwork);
  constructor = gc_keep(js_new_native(c, metadata_ctor, "MediaMetadata", 0));
  js_def(constructor, "prototype", js_obj_value(meta_proto), 0);
  js_def(meta_proto, "constructor", js_obj_value(constructor), P_WRITABLE | P_CONFIGURABLE);
  js_def(c->global, "MediaMetadata", js_obj_value(constructor), P_WRITABLE | P_CONFIGURABLE);
  session = gc_keep(create_object());
  def_acc(session, "metadata", session_get_metadata, session_set_metadata);
  def_acc(session, "playbackState", session_get_state, session_set_state);
  def_method(session, "setActionHandler", session_set_action_handler, 2);
  def_method(session, "setPositionState", session_set_position_state, 0);
  def_method(session, "setCameraActive", session_resolved, 1);
  def_method(session, "setMicrophoneActive", session_resolved, 1);
  js_def(navigator, "mediaSession", js_obj_value(session), P_ENUMERABLE);
  gc_pop_scope();
}

static const char *host_of(const char *url, char *out, int cap)
{
  struct purl u;
  out[0] = '\0';
  if (parse_url(url, &u))
    strlcpy(out, u.host, cap);
  return out;
}

static unsigned current_pos_ms(struct media_el *el, unsigned *dur_ms)
{
  double position, duration;
  *dur_ms = 0;
  if (pos_state.set) {
    duration = pos_state.duration;
    position = pos_state.position;
    if (playback_state == PS_PLAYING || (el && !media_paused(el)))
      position += (double)(msec() - pos_state.stamp) * pos_state.rate / 1000.0;
    if (position > duration)
      position = duration;
  } else if (el) {
    duration = media_duration(el);
    position = media_current_time(el);
  } else {
    return 0;
  }
  if (duration == duration && duration < 4e6)
    *dur_ms = (unsigned)(duration * 1000.0);
  return position > 0 ? (unsigned)(position * 1000.0) : 0;
}

static void copy_text(char *dst, int cap, js_value v)
{
  js_value s = js_to_string(ctx, v);
  strlcpy(dst, s.type == JV_STR ? s.u.str->data : "", cap);
}

static bool is_file_url(const char *s)
{
  static const char prefix[] = "file://";
  unsigned i;
  for (i = 0; i < sizeof prefix - 1; i++)
    if (s[i] != prefix[i])
      return false;
  return true;
}

static void fill_artwork(struct media_info *info)
{
  js_value list = js_get(ctx, js_obj_value(metadata), "@artwork"), item, src;
  unsigned i, n;
  if (!js_is_array(list))
    return;
  n = js_array_len(list.u.obj);
  for (i = 0; i < n; i++) {
    item = js_get_index(ctx, list, i);
    src = js_get(ctx, item, "src");
    if (src.type == JV_STR && is_file_url(src.u.str->data)) {
      strlcpy(info->artwork, src.u.str->data + 7, sizeof info->artwork);
      return;
    }
  }
}

static unsigned handler_caps(void)
{
  unsigned caps = 0;
  int i;
  for (i = 0; i < N_ACTIONS; i++)
    if (actions[i].cmd != MEDIA_CMD_NONE && js_is_callable(js_get(ctx, js_obj_value(handlers), actions[i].name)))
      caps |= MEDIA_CAP(actions[i].cmd);
  if (caps & (MEDIA_CAP(MEDIA_CMD_PLAY) | MEDIA_CAP(MEDIA_CMD_PAUSE)))
    caps |= MEDIA_CAP(MEDIA_CMD_TOGGLE);
  return caps;
}

static void build_info(struct media_info *info, struct media_el *el, const char *doc_title, const char *page_url)
{
  char host[URL_HOST_MAX];
  bool playing = el && !media_paused(el);
  memset(info, 0, sizeof *info);
  strlcpy(info->app, "Browser", sizeof info->app);
  if (ctx && metadata) {
    copy_text(info->title, sizeof info->title, js_get(ctx, js_obj_value(metadata), "@title"));
    copy_text(info->artist, sizeof info->artist, js_get(ctx, js_obj_value(metadata), "@artist"));
    copy_text(info->album, sizeof info->album, js_get(ctx, js_obj_value(metadata), "@album"));
    fill_artwork(info);
  } else {
    host_of(page_url, host, sizeof host);
    strlcpy(info->title, doc_title[0] ? doc_title : host, sizeof info->title);
    strlcpy(info->artist, host, sizeof info->artist);
  }
  if (playing || playback_state == PS_PLAYING)
    info->state = MEDIA_PLAYING;
  else if (playback_state == PS_PAUSED || el)
    info->state = MEDIA_PAUSED;
  else
    info->state = MEDIA_STOPPED;
  info->pos_ms = current_pos_ms(el, &info->dur_ms);
  if (ctx)
    info->caps = handler_caps();
  if (el) {
    info->caps |= MEDIA_CAP(MEDIA_CMD_PLAY) | MEDIA_CAP(MEDIA_CMD_PAUSE) | MEDIA_CAP(MEDIA_CMD_TOGGLE) |
                  MEDIA_CAP(MEDIA_CMD_STOP);
    if (info->dur_ms)
      info->caps |= MEDIA_CAP(MEDIA_CMD_SEEK_FWD) | MEDIA_CAP(MEDIA_CMD_SEEK_BACK) | MEDIA_CAP(MEDIA_CMD_SEEK_TO);
  }
}

static bool needs_publish(const struct media_info *info, unsigned now)
{
  struct media_info same_pos = *info;
  unsigned elapsed = now - last_publish, expected, drift;
  if (!have_last)
    return true;
  same_pos.pos_ms = last.pos_ms;
  if (memcmp(&same_pos, &last, sizeof last))
    return elapsed >= PUBLISH_MIN_MS;
  if (info->state != MEDIA_PLAYING)
    return info->pos_ms != last.pos_ms && elapsed >= PUBLISH_MIN_MS;
  expected = last.pos_ms + elapsed;
  drift = info->pos_ms > expected ? info->pos_ms - expected : expected - info->pos_ms;
  return drift > SEEK_DRIFT_MS || elapsed >= PUBLISH_REFRESH_MS;
}

static void call_handler(js_value fn, const char *action, int cmd, unsigned arg)
{
  js_object *details;
  js_value value;
  gc_push_scope();
  details = gc_keep(create_object());
  value = js_obj_value(details);
  js_def(details, "action", js_mk_cstring(ctx, action), P_ENUMERABLE | P_WRITABLE);
  if (cmd == MEDIA_CMD_SEEK_TO)
    js_def(details, "seekTime", js_number((double)arg / 1000.0), P_ENUMERABLE | P_WRITABLE);
  else if ((cmd == MEDIA_CMD_SEEK_FWD || cmd == MEDIA_CMD_SEEK_BACK) && arg)
    js_def(details, "seekOffset", js_number((double)arg / 1000.0), P_ENUMERABLE | P_WRITABLE);
  js_def(details, "fastSeek", js_bool(0), P_ENUMERABLE | P_WRITABLE);
  js_call(ctx, fn, js_undef(), &value, 1);
  if (ctx->has_exc)
    report_exc(ctx);
  gc_pop_scope();
}

static void seek_element(struct media_el *el, double seconds)
{
  double duration = media_duration(el);
  if (seconds < 0)
    seconds = 0;
  if (duration == duration && seconds > duration)
    seconds = duration;
  media_set_current_time(el, seconds);
}

static void act_on_element(struct media_el *el, int cmd, unsigned arg)
{
  double step = (arg ? arg : DEFAULT_SEEK_MS) / 1000.0;
  if (!el)
    return;
  switch (cmd) {
  case MEDIA_CMD_PLAY:
    media_play(el);
    break;
  case MEDIA_CMD_PAUSE:
    media_pause(el);
    break;
  case MEDIA_CMD_STOP:
    media_pause(el);
    media_set_current_time(el, 0);
    break;
  case MEDIA_CMD_SEEK_FWD:
    seek_element(el, media_current_time(el) + step);
    break;
  case MEDIA_CMD_SEEK_BACK:
    seek_element(el, media_current_time(el) - step);
    break;
  case MEDIA_CMD_SEEK_TO:
    seek_element(el, (double)arg / 1000.0);
    break;
  }
}

static void run_command(int cmd, unsigned arg, struct media_el *el)
{
  js_value fn = js_undef();
  const char *name = 0;
  int i;
  if (cmd == MEDIA_CMD_TOGGLE)
    cmd = (el && !media_paused(el)) || (!el && last.state == MEDIA_PLAYING) ? MEDIA_CMD_PAUSE : MEDIA_CMD_PLAY;
  for (i = 0; i < N_ACTIONS; i++)
    if (actions[i].cmd == cmd)
      name = actions[i].name;
  if (!name)
    return;
  if (ctx)
    fn = js_get(ctx, js_obj_value(handlers), name);
  if (js_is_callable(fn))
    call_handler(fn, name, cmd, arg);
  else
    act_on_element(el, cmd, arg);
}

static void close_session(void)
{
  if (sid >= 0)
    mediactl_close(sid);
  sid = -1;
  have_last = false;
}

void mediasession_tick(const char *doc_title, const char *page_url)
{
  struct media_el *el = media_current();
  struct media_info info;
  unsigned now = msec(), arg;
  int cmd, budget = 16;
  if (el && media_ended(el))
    el = 0;
  if (!el && !metadata && playback_state == PS_NONE) {
    close_session();
    return;
  }
  if (sid < 0) {
    if (now < next_open)
      return;
    sid = mediactl_open("Browser");
    if (sid < 0) {
      next_open = now + OPEN_RETRY_MS;
      return;
    }
  }
  build_info(&info, el, doc_title, page_url);
  if (needs_publish(&info, now)) {
    if (mediactl_update(sid, &info) < 0) {
      close_session();
      next_open = now + OPEN_RETRY_MS;
      return;
    }
    last = info;
    have_last = true;
    last_publish = now;
  }
  if (now - last_poll < POLL_MS)
    return;
  last_poll = now;
  while (budget-- > 0 && (cmd = mediactl_poll(sid, &arg)) > MEDIA_CMD_NONE)
    run_command(cmd, arg, el);
}

void mediasession_teardown(void)
{
  close_session();
  if (ctx) {
    gc_remove_root((void **)&meta_proto);
    gc_remove_root((void **)&handlers);
    gc_remove_root((void **)&metadata);
  }
  ctx = 0;
  meta_proto = 0;
  handlers = 0;
  clear_page_state();
}
