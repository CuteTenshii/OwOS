/* camera.c: navigator.mediaDevices (getUserMedia, enumerateDevices) and MediaStream/MediaStreamTrack over the kernel's
 * /sys/class/video/cam0 files. One camera shared by every track, video only: there is no microphone capture. */

#include "features/camera/camera.h"
#include "features/clienthints/clienthints.h"
#include "features/media/bmedia.h"
#include "jpeg.h"
#include "jsint.h"
#include "uuid.h"
#include <stddef.h>
#include <stdint.h>

#define DIR "/sys/class/video/cam0/"
#define INFO_PATH DIR "info"
#define CONTROL_PATH DIR "control"
#define FRAME_PATH DIR "frame"

#define MAX_FMTS 32
#define MAX_FPS 8
#define MAX_TRACKS 32
#define HDR_LEN 24
#define MAX_PAYLOAD (16u << 20)
#define MAX_DIM 8192
#define POLL_MS 10
#define STALL_MS 1500
#define DEFAULT_W 640
#define DEFAULT_H 480
#define DEFAULT_FPS 30

struct format {
  char fourcc[5];
  int w, h, nfps, fps[MAX_FPS];
};

struct device {
  char name[64];
  int nfmts;
  struct format f[MAX_FMTS];
};

struct range {
  bool ideal_set, exact_set, min_set, max_set;
  double ideal, exact, min, max;
};

struct cons {
  struct range w, h, fps;
  bool device_ok;
};

struct cand {
  int fmt, fps;
};

enum { H_TRACK_PROTO, H_STREAM_PROTO, H_TRACKS };

static js_ctx *ctx;
static js_object *held[H_TRACKS + MAX_TRACKS];
static int held_n = H_TRACKS + MAX_TRACKS;
static unsigned serials[MAX_TRACKS], last_serial;
static char device_id[17];
static struct device dev;

static struct {
  bool running;
  int fmt, fps;
  unsigned last_seq, frame_seq, last_new, last_poll;
  unsigned char *rgba, *raw, *black;
  size_t rgba_cap, raw_cap, black_cap;
  int w, h, black_w, black_h;
} cam;

/* ---- text helpers */

static const char *after(const char *s, const char *prefix)
{
  while (*prefix)
    if (*s++ != *prefix++)
      return 0;
  return s;
}

static const char *next_line(const char *s)
{
  const char *nl = strchr(s, '\n');
  return nl ? nl + 1 : s + strlen(s);
}

static int number(const char **p)
{
  int v = 0, digits = 0;
  for (; **p >= '0' && **p <= '9'; (*p)++, digits++)
    v = v * 10 + (**p - '0');
  return digits ? v : -1;
}

static unsigned le32(const unsigned char *p)
{
  return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/* ---- device description */

static void parse_format(struct device *d, const char *s)
{
  struct format *f = &d->f[d->nfmts];
  int n = 0;
  while (s[n] && s[n] != ' ' && n < 4)
    n++;
  memcpy(f->fourcc, s, (unsigned)n);
  f->fourcc[n] = 0;
  if (!streq(f->fourcc, "MJPG") && !streq(f->fourcc, "YUY2") && !streq(f->fourcc, "NV12"))
    return;
  s += n;
  if (*s++ != ' ' || (f->w = number(&s)) <= 0 || *s++ != 'x' || (f->h = number(&s)) <= 0)
    return;
  for (f->nfps = 0; *s == ' ' && f->nfps < MAX_FPS;) {
    s++;
    int fps = number(&s);
    if (fps <= 0)
      break;
    f->fps[f->nfps++] = fps;
  }
  if (!f->nfps)
    f->fps[f->nfps++] = DEFAULT_FPS;
  d->nfmts++;
}

static bool load_info(void)
{
  char text[2048];
  int n = readfile(INFO_PATH, text, sizeof text - 1);
  if (n <= 0)
    return false;
  text[n] = 0;
  struct device d;
  memset(&d, 0, sizeof d);
  for (const char *line = text; *line; line = next_line(line)) {
    const char *v;
    if ((v = after(line, "name "))) {
      int len = 0;
      while (v[len] && v[len] != '\n' && len < (int)sizeof d.name - 1)
        len++;
      memcpy(d.name, v, (unsigned)len);
    } else if ((v = after(line, "format ")) && d.nfmts < MAX_FMTS) {
      parse_format(&d, v);
    }
  }
  if (!d.nfmts)
    return false;
  if (!d.name[0])
    strlcpy(d.name, "Camera", sizeof d.name);
  dev = d;
  return true;
}

/* ---- constraints */

static void read_range(js_value v, struct range *r)
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

/* deviceId only constrains through `exact`, as a string or an array of strings */
static bool device_matches(js_value v)
{
  if (v.type == JV_STR)
    return streq(arg_str(ctx, v), device_id);
  if (!js_is_array(v))
    return false;
  int n = (int)js_to_number(ctx, js_get(ctx, v, "length"));
  for (int i = 0; i < n; i++) {
    js_value e = js_get_index(ctx, v, (unsigned)i);
    if (e.type == JV_STR && streq(arg_str(ctx, e), device_id))
      return true;
  }
  return false;
}

static void read_cons(js_value video, struct cons *c)
{
  memset(c, 0, sizeof *c);
  c->device_ok = true;
  if (video.type != JV_OBJ)
    return;
  read_range(js_get(ctx, video, "width"), &c->w);
  read_range(js_get(ctx, video, "height"), &c->h);
  read_range(js_get(ctx, video, "frameRate"), &c->fps);
  js_value id = js_get(ctx, video, "deviceId");
  if (id.type == JV_OBJ && !js_is_array(id)) {
    js_value exact = js_get(ctx, id, "exact");
    if (exact.type != JV_UNDEF)
      c->device_ok = device_matches(exact);
  }
}

static bool in_range(const struct range *r, double v)
{
  return (!r->exact_set || v == r->exact) && (!r->min_set || v >= r->min) && (!r->max_set || v <= r->max);
}

static double rel_dist(double v, double ideal)
{
  double d = v > ideal ? v - ideal : ideal - v, m = v > ideal ? v : ideal;
  return m > 0 ? d / m : 0;
}

static double ideal_of(const struct range *r, double fallback)
{
  return r->ideal_set ? r->ideal : r->exact_set ? r->exact : fallback;
}

static int candidates(struct cand *out, bool shared)
{
  int n = 0;
  for (int f = 0; f < dev.nfmts; f++) {
    if (shared && f != cam.fmt)
      continue;
    for (int k = 0; k < dev.f[f].nfps; k++)
      if (!shared || dev.f[f].fps[k] == cam.fps)
        out[n++] = (struct cand){f, dev.f[f].fps[k]};
  }
  return n;
}

static int format_rank(const struct format *f)
{
  return streq(f->fourcc, "MJPG") ? 0 : streq(f->fourcc, "YUY2") ? 1 : 2;
}

/* Best candidate for the constraints; NULL on success, else the name of a constraint nothing satisfies. With `shared`
 * the camera is in use by other tracks, so only its current format and rate qualify. */
static const char *choose(const struct cons *c, bool shared, struct cand *best)
{
  struct cand all[MAX_FMTS * MAX_FPS];
  int n = candidates(all, shared);
  bool size_given = c->w.ideal_set || c->w.exact_set || c->h.ideal_set || c->h.exact_set;
  double iw = ideal_of(&c->w, size_given ? -1 : DEFAULT_W), ih = ideal_of(&c->h, size_given ? -1 : DEFAULT_H);
  double ifps = ideal_of(&c->fps, DEFAULT_FPS), best_cost = 0;
  bool found = false;

  if (!c->device_ok)
    return "deviceId";
  for (int i = 0; i < n; i++) {
    const struct format *f = &dev.f[all[i].fmt];
    if (!in_range(&c->w, f->w) || !in_range(&c->h, f->h) || !in_range(&c->fps, all[i].fps))
      continue;
    double cost = (iw >= 0 ? rel_dist(f->w, iw) : 0) + (ih >= 0 ? rel_dist(f->h, ih) : 0) + rel_dist(all[i].fps, ifps);
    bool better = !found || cost < best_cost - 1e-9 ||
                  (cost < best_cost + 1e-9 && format_rank(f) < format_rank(&dev.f[best->fmt]));
    if (better) {
      *best = all[i];
      best_cost = cost;
      found = true;
    }
  }
  if (found)
    return 0;
  for (int p = 0; p < 3; p++) {
    bool any = false;
    for (int i = 0; i < n; i++) {
      const struct format *f = &dev.f[all[i].fmt];
      any |= p == 0 ? in_range(&c->w, f->w) : p == 1 ? in_range(&c->h, f->h) : in_range(&c->fps, all[i].fps);
    }
    if (!any)
      return p == 0 ? "width" : p == 1 ? "height" : "frameRate";
  }
  return "width";
}

/* ---- the stream on the kernel side */

static bool cam_start(int fmt, int fps)
{
  char cmd[64];
  snprintf(cmd, sizeof cmd, "start %s %d %d %d", dev.f[fmt].fourcc, dev.f[fmt].w, dev.f[fmt].h, fps);
  if (writefile(CONTROL_PATH, cmd, strlen(cmd)) < 0)
    return false;
  cam.running = true;
  cam.fmt = fmt;
  cam.fps = fps;
  cam.last_seq = 0;
  cam.last_new = msec();
  return true;
}

static void cam_stop(void)
{
  if (cam.running)
    writefile(CONTROL_PATH, "stop", 4);
  cam.running = false;
}

static int live_count(void)
{
  int n = 0;
  for (int i = 0; i < MAX_TRACKS; i++)
    n += held[H_TRACKS + i] != 0;
  return n;
}

static void release_slot(int slot)
{
  held[H_TRACKS + slot] = 0;
  serials[slot] = 0;
  if (!live_count())
    cam_stop();
}

static bool track_enabled(js_object *t) { return js_to_boolean(js_get(ctx, js_obj_value(t), "_en")); }

static bool any_enabled(void)
{
  for (int i = 0; i < MAX_TRACKS; i++)
    if (held[H_TRACKS + i] && track_enabled(held[H_TRACKS + i]))
      return true;
  return false;
}

/* ---- frames */

static unsigned char clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (unsigned char)v; }

static void put_yuv(unsigned char *o, int y, int u, int v)
{
  int c = y - 16, d = u - 128, e = v - 128;
  o[0] = clamp8((298 * c + 409 * e + 128) >> 8);
  o[1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
  o[2] = clamp8((298 * c + 516 * d + 128) >> 8);
  o[3] = 255;
}

static bool size_frame(int w, int h)
{
  size_t need = (size_t)w * (size_t)h * 4;
  if (need > cam.rgba_cap) {
    unsigned char *p = realloc(cam.rgba, (unsigned)need);
    if (!p)
      return false;
    cam.rgba = p;
    cam.rgba_cap = need;
  }
  cam.w = w;
  cam.h = h;
  return true;
}

static bool decode_yuv(const unsigned char *p, unsigned len, int w, int h, bool nv12)
{
  if ((size_t)len < (size_t)w * (size_t)h * (nv12 ? 3 : 4) / 2 || !size_frame(w, h))
    return false;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      unsigned char *o = cam.rgba + ((size_t)y * w + x) * 4;
      if (nv12) {
        const unsigned char *uv = p + (size_t)w * h + (size_t)(y / 2) * w + (x & ~1);
        put_yuv(o, p[(size_t)y * w + x], uv[0], uv[1]);
      } else {
        const unsigned char *q = p + ((size_t)y * w + (x & ~1)) * 2;
        put_yuv(o, q[(x & 1) * 2], q[1], q[3]);
      }
    }
  return true;
}

static bool decode_mjpg(const unsigned char *p, unsigned len)
{
  unsigned char *out;
  unsigned w, h;
  if (jpeg_decode(p, (int)len, 0, &out, &w, &h) != JPEG_OK)
    return false;
  free(cam.rgba);
  cam.rgba = out;
  cam.rgba_cap = (size_t)w * h * 4;
  cam.w = (int)w;
  cam.h = (int)h;
  return true;
}

/* False once the camera has disappeared */
static bool poll_frame(unsigned now)
{
  unsigned char hdr[HDR_LEN];
  char probe[8];
  int n = readfile_at(FRAME_PATH, 0, hdr, HDR_LEN);
  if (n < 0)
    return readfile(INFO_PATH, probe, sizeof probe) >= 0;
  if (n < HDR_LEN)
    return true;
  unsigned seq = le32(hdr), w = le32(hdr + 8), h = le32(hdr + 12), len = le32(hdr + 16);
  if (!seq || !len || seq == cam.last_seq || len > MAX_PAYLOAD || w > MAX_DIM || h > MAX_DIM)
    return true;
  if (len > cam.raw_cap) {
    unsigned char *p = realloc(cam.raw, len);
    if (!p)
      return true;
    cam.raw = p;
    cam.raw_cap = len;
  }
  if (readfile_at(FRAME_PATH, HDR_LEN, cam.raw, len) != (int)len)
    return true;
  cam.last_seq = seq;
  bool ok = !memcmp(hdr + 4, "MJPG", 4)   ? decode_mjpg(cam.raw, len)
            : !memcmp(hdr + 4, "YUY2", 4) ? decode_yuv(cam.raw, len, (int)w, (int)h, false)
            : !memcmp(hdr + 4, "NV12", 4) ? decode_yuv(cam.raw, len, (int)w, (int)h, true)
                                          : false;
  if (ok) {
    cam.frame_seq++;
    cam.last_new = now;
  }
  return true;
}

const unsigned char *camera_frame(int *w, int *h, unsigned *seq)
{
  bool blank = live_count() && !any_enabled();
  if (!cam.rgba || !cam.w)
    return 0;
  *w = cam.w;
  *h = cam.h;
  *seq = cam.frame_seq << 1 | blank;
  if (!blank)
    return cam.rgba;
  if (cam.black_w != cam.w || cam.black_h != cam.h) {
    free(cam.black);
    cam.black = malloc((unsigned)cam.w * (unsigned)cam.h * 4);
    cam.black_w = cam.black_h = 0;
    if (!cam.black)
      return 0;
    for (size_t i = 0; i < (size_t)cam.w * cam.h; i++)
      memcpy(cam.black + i * 4, "\0\0\0\xff", 4);
    cam.black_w = cam.w;
    cam.black_h = cam.h;
  }
  return cam.black;
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

/* The camera went away: every live track ends and hears about it. */
static int lose_camera(void)
{
  int fired = 0;
  cam.running = false;
  for (int i = 0; i < MAX_TRACKS; i++) {
    js_object *t = held[H_TRACKS + i];
    if (!t)
      continue;
    gc_push_scope();
    gc_keep(t);
    held[H_TRACKS + i] = 0;
    serials[i] = 0;
    fire_ended(t);
    gc_pop_scope();
    fired++;
  }
  return fired;
}

static void new_uuid(char out[UUID_STR_LEN])
{
  uuid_t u;
  uuid_generate_v4(u);
  uuid_to_string(u, out, 0);
}

/* ---- MediaStreamTrack */

static int slot_of(js_value t)
{
  js_object *o = obj_of(t);
  if (!o)
    return -1;
  js_value tag = js_get(ctx, t, "_cam");
  unsigned serial = tag.type == JV_NUM ? (unsigned)js_to_number(ctx, tag) : 0;
  for (int i = 0; serial && i < MAX_TRACKS; i++)
    if (serials[i] == serial && held[H_TRACKS + i])
      return i;
  return -1;
}

static void set_settings(js_object *t, const struct cand *c)
{
  hide(t, "_w", js_number(dev.f[c->fmt].w));
  hide(t, "_h", js_number(dev.f[c->fmt].h));
  hide(t, "_f", js_number(c->fps));
}

/* A track on the running camera; `live` false makes an already ended one (a clone of an ended track). */
static js_object *new_track(const struct cand *c, js_value constraints, const char *label, bool live)
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
  js_def(t, "kind", js_mk_cstring(ctx, "video"), P_ENUMERABLE);
  js_def(t, "id", js_mk_cstring(ctx, id), P_ENUMERABLE);
  js_def(t, "label", js_mk_cstring(ctx, label), P_ENUMERABLE);
  js_def(t, "muted", js_bool(0), P_ENUMERABLE);
  js_def(t, "contentHint", js_mk_cstring(ctx, ""), P_ENUMERABLE | P_WRITABLE);
  js_def(t, "onended", js_null(), P_ENUMERABLE | P_WRITABLE);
  hide(t, "_en", js_bool(1));
  hide(t, "_cons", constraints.type == JV_OBJ ? constraints : js_obj_value((js_object *)gc_keep(new_obj())));
  set_settings(t, c);
  if (live) {
    serials[slot] = ++last_serial;
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
  struct cand cur = {cam.fmt, cam.fps};
  js_value rv = js_undef();
  (void)c;
  (void)args;
  (void)argc;
  if (!obj_of(t))
    return js_throw_type(ctx, "Illegal invocation");
  gc_push_scope();
  bool live = slot_of(t) >= 0;
  js_object *copy = new_track(&cur, js_get(ctx, t, "_cons"), arg_str(ctx, js_get(ctx, t, "label")), live);
  if (copy) {
    hide(copy, "_w", js_get(ctx, t, "_w"));
    hide(copy, "_h", js_get(ctx, t, "_h"));
    hide(copy, "_f", js_get(ctx, t, "_f"));
    hide(copy, "_en", js_get(ctx, t, "_en"));
    rv = js_obj_value(copy);
  }
  gc_pop_scope();
  return rv.type == JV_OBJ ? rv : js_throw_type(ctx, "Too many camera tracks");
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
  js_value rv;
  double w, h;
  (void)c;
  (void)args;
  (void)argc;
  if (!obj_of(t))
    return js_throw_type(ctx, "Illegal invocation");
  w = js_to_number(ctx, js_get(ctx, t, "_w"));
  h = js_to_number(ctx, js_get(ctx, t, "_h"));
  gc_push_scope();
  js_object *o = (js_object *)gc_keep(new_obj());
  js_def(o, "width", js_number(w), P_ENUMERABLE);
  js_def(o, "height", js_number(h), P_ENUMERABLE);
  js_def(o, "frameRate", js_get(ctx, t, "_f"), P_ENUMERABLE);
  js_def(o, "aspectRatio", js_number(h > 0 ? w / h : 0), P_ENUMERABLE);
  js_def(o, "deviceId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "groupId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "resizeMode", js_mk_cstring(ctx, "none"), P_ENUMERABLE);
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}

static js_value min_max(double lo, double hi)
{
  js_object *o = (js_object *)gc_keep(new_obj());
  js_def(o, "min", js_number(lo), P_ENUMERABLE);
  js_def(o, "max", js_number(hi), P_ENUMERABLE);
  return js_obj_value(o);
}

static js_value track_get_capabilities(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value rv;
  double minw = 1e9, maxw = 0, minh = 1e9, maxh = 0, minf = 1e9, maxf = 0;
  (void)c;
  (void)args;
  (void)argc;
  if (!obj_of(t))
    return js_throw_type(ctx, "Illegal invocation");
  for (int i = 0; i < dev.nfmts; i++) {
    const struct format *f = &dev.f[i];
    minw = f->w < minw ? f->w : minw;
    maxw = f->w > maxw ? f->w : maxw;
    minh = f->h < minh ? f->h : minh;
    maxh = f->h > maxh ? f->h : maxh;
    for (int k = 0; k < f->nfps; k++) {
      minf = f->fps[k] < minf ? f->fps[k] : minf;
      maxf = f->fps[k] > maxf ? f->fps[k] : maxf;
    }
  }
  gc_push_scope();
  js_object *o = (js_object *)gc_keep(new_obj()), *modes = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_array_push(ctx, modes, js_mk_cstring(ctx, "none"));
  js_def(o, "width", min_max(minw, maxw), P_ENUMERABLE);
  js_def(o, "height", min_max(minh, maxh), P_ENUMERABLE);
  js_def(o, "frameRate", min_max(minf, maxf), P_ENUMERABLE);
  js_def(o, "aspectRatio", min_max(maxh > 0 ? minw / maxh : 0, minh > 0 ? maxw / minh : 0), P_ENUMERABLE);
  js_def(o, "deviceId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "groupId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "resizeMode", js_obj_value(modes), P_ENUMERABLE);
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
  struct cons cons;
  struct cand pick;
  js_value spec = A(0);
  int slot = slot_of(t);
  const char *failed;
  (void)c;
  if (!obj_of(t))
    return js_throw_type(ctx, "Illegal invocation");
  if (slot < 0)
    return js_promise_resolved(ctx, js_undef());
  if (!load_info())
    return reject("NotFoundError", "Requested device not found");
  read_cons(spec, &cons);
  if ((failed = choose(&cons, live_count() > 1, &pick)) != 0)
    return overconstrained(failed);
  if (cam.running && (pick.fmt != cam.fmt || pick.fps != cam.fps) && !cam_start(pick.fmt, pick.fps))
    return reject("NotReadableError", "Could not start video source");
  set_settings(obj_of(t), &pick);
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

static js_value stream_get_tracks(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value list, rv;
  (void)c;
  (void)args;
  (void)argc;
  list = tracks_of(t);
  if (!js_is_array(list))
    return js_throw_type(ctx, "Illegal invocation");
  gc_push_scope();
  js_object *copy = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (int i = 0; i < tracks_len(list); i++)
    js_array_push(ctx, copy, js_get_index(ctx, list, (unsigned)i));
  rv = js_obj_value(copy);
  gc_pop_scope();
  return rv;
}

/* Audio tracks never exist: capture is video only */
static js_value stream_get_audio_tracks(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)t;
  (void)args;
  (void)argc;
  return js_obj_value(js_new_array(ctx, 0));
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

/* ---- mediaDevices */

static bool wants(js_value constraints, const char *kind)
{
  return constraints.type == JV_OBJ && js_to_boolean(js_get(ctx, constraints, kind));
}

static js_value get_user_media(js_ctx *c, js_value t, js_value *args, int argc)
{
  js_value spec = A(0), video, rv;
  struct cons cons;
  struct cand pick;
  const char *failed;
  bool started = false;
  (void)c;
  (void)t;
  video = spec.type == JV_OBJ ? js_get(ctx, spec, "video") : js_undef();
  if (!wants(spec, "audio") && !wants(spec, "video"))
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError",
                                                  "At least one of audio and video must be requested"));
  if (wants(spec, "audio"))
    return perm_state_now(PERM_MICROPHONE) == PERM_DENIED ? reject("NotAllowedError", "Permission denied")
                                                          : reject("NotFoundError", "Requested device not found");
  if (!load_info())
    return reject("NotFoundError", "Requested device not found");
  if (worker_permission(PERM_CAMERA) != PERM_GRANTED)
    return reject("NotAllowedError", "Permission denied");
  read_cons(video, &cons);
  if ((failed = choose(&cons, cam.running, &pick)) != 0)
    return overconstrained(failed);
  if (!cam.running) {
    if (!cam_start(pick.fmt, pick.fps))
      return reject("NotReadableError", "Could not start video source");
    started = true;
  }
  gc_push_scope();
  js_object *track = new_track(&pick, video, dev.name, true);
  if (!track) {
    if (started)
      cam_stop();
    gc_pop_scope();
    return reject("NotReadableError", "Too many open camera tracks");
  }
  js_object *tracks = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_array_push(ctx, tracks, js_obj_value(track));
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
  if (load_info()) {
    bool known = live_count() || perm_state_now(PERM_CAMERA) == PERM_GRANTED;
    js_object *d = (js_object *)gc_keep(new_obj());
    js_def(d, "deviceId", js_mk_cstring(ctx, known ? device_id : ""), P_ENUMERABLE);
    js_def(d, "kind", js_mk_cstring(ctx, "videoinput"), P_ENUMERABLE);
    js_def(d, "label", js_mk_cstring(ctx, known ? dev.name : ""), P_ENUMERABLE);
    js_def(d, "groupId", js_mk_cstring(ctx, known ? device_id : ""), P_ENUMERABLE);
    js_array_push(ctx, list, js_obj_value(d));
  }
  rv = js_promise_resolved(ctx, js_obj_value(list));
  gc_pop_scope();
  return rv;
}

static js_value get_supported_constraints(js_ctx *c, js_value t, js_value *args, int argc)
{
  static const char *const names[] = {"width", "height", "frameRate", "aspectRatio", "deviceId", "groupId"};
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

/* Origin-scoped so two sites cannot correlate the camera through its id */
static void make_device_id(const char *page_url)
{
  uint64_t h = 14695981039346656037ull;
  int slashes = 0;
  for (const char *p = page_url; *p && (*p != '/' || ++slashes < 3); p++)
    h = (h ^ (unsigned char)*p) * 1099511628211ull;
  for (const char *p = "cam0"; *p; p++)
    h = (h ^ (unsigned char)*p) * 1099511628211ull;
  snprintf(device_id, sizeof device_id, "%08x%08x", (unsigned)(h >> 32), (unsigned)h);
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

void camera_install(js_ctx *c, js_object *navigator, const char *page_url)
{
  if (!ch_secure_url(page_url))
    return;
  ctx = c;
  memset(&cam, 0, sizeof cam);
  memset(held, 0, sizeof held);
  memset(serials, 0, sizeof serials);
  memset(&dev, 0, sizeof dev);
  cam.last_poll = msec();
  make_device_id(page_url);
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
  def_method(stream, "getVideoTracks", stream_get_tracks, 0);
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

int camera_tick(void)
{
  unsigned now = msec();
  if (!ctx || !live_count() || now - cam.last_poll < POLL_MS)
    return 0;
  cam.last_poll = now;
  if (!poll_frame(now))
    return lose_camera();
  if (cam.running && now - cam.last_new > STALL_MS) {
    char state[96];
    int n = readfile(CONTROL_PATH, state, sizeof state - 1);
    cam.last_new = now;
    if (n >= 4 && !memcmp(state, "idle", 4))
      cam_start(cam.fmt, cam.fps);
  }
  return 0;
}

void camera_teardown(void)
{
  if (!ctx)
    return;
  cam_stop();
  gc_remove_root_range((void **)held);
  free(cam.rgba);
  free(cam.raw);
  free(cam.black);
  memset(&cam, 0, sizeof cam);
  ctx = 0;
}

bool camera_is_stream(js_value v) { return ctx && v.type == JV_OBJ && js_is_array(js_get(ctx, v, "_tracks")); }
