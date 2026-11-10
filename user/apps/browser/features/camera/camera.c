/* camera.c: the USB camera behind /sys/class/video/cam0 as a capture device for MediaStream (mediastream.c): format
 * and frame rate selection from video constraints, the stream on the kernel side, and frame decoding to RGBA. */

#include "features/camera/camera.h"
#include "features/media/bmedia.h"
#include "jpeg.h"
#include <stddef.h>

#define DIR "/sys/class/video/cam0/"
#define INFO_PATH DIR "info"
#define CONTROL_PATH DIR "control"
#define FRAME_PATH DIR "frame"

#define MAX_FMTS 32
#define MAX_FPS 8
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

struct cons {
  struct ms_range w, h, fps;
  bool device_ok;
};

struct cand {
  int fmt, fps;
};

static js_ctx *ctx;
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

static void read_cons(js_value video, struct cons *c)
{
  memset(c, 0, sizeof *c);
  c->device_ok = mediastream_device_matches(video, device_id);
  if (video.type != JV_OBJ)
    return;
  mediastream_read_range(js_get(ctx, video, "width"), &c->w);
  mediastream_read_range(js_get(ctx, video, "height"), &c->h);
  mediastream_read_range(js_get(ctx, video, "frameRate"), &c->fps);
}

static double rel_dist(double v, double ideal)
{
  double d = v > ideal ? v - ideal : ideal - v, m = v > ideal ? v : ideal;
  return m > 0 ? d / m : 0;
}

static double ideal_of(const struct ms_range *r, double fallback)
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
    if (!mediastream_in_range(&c->w, f->w) || !mediastream_in_range(&c->h, f->h) ||
        !mediastream_in_range(&c->fps, all[i].fps))
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
      any |= p == 0   ? mediastream_in_range(&c->w, f->w)
             : p == 1 ? mediastream_in_range(&c->h, f->h)
                      : mediastream_in_range(&c->fps, all[i].fps);
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
  if (!cam.rgba || !cam.w)
    return 0;
  bool blank = mediastream_live("video") && !mediastream_any_enabled("video");
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

/* ---- the capture device */

static void pick_of(const struct cand *c, struct ms_pick *out)
{
  out->v[0] = c->fmt;
  out->v[1] = c->fps;
  out->v[2] = dev.f[c->fmt].w;
  out->v[3] = dev.f[c->fmt].h;
}

static const char *cam_choose(js_value constraints, bool shared, struct ms_pick *out)
{
  struct cons cons;
  struct cand best;
  read_cons(constraints, &cons);
  const char *failed = choose(&cons, shared, &best);
  if (!failed)
    pick_of(&best, out);
  return failed;
}

static bool cam_open(const struct ms_pick *p)
{
  return (cam.running && p->v[0] == cam.fmt && p->v[1] == cam.fps) || cam_start(p->v[0], p->v[1]);
}

static void cam_settings(js_object *o, const struct ms_pick *p)
{
  double w = p->v[2], h = p->v[3];
  js_def(o, "width", js_number(w), P_ENUMERABLE);
  js_def(o, "height", js_number(h), P_ENUMERABLE);
  js_def(o, "frameRate", js_number(p->v[1]), P_ENUMERABLE);
  js_def(o, "aspectRatio", js_number(h > 0 ? w / h : 0), P_ENUMERABLE);
  js_def(o, "deviceId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "groupId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "resizeMode", js_mk_cstring(ctx, "none"), P_ENUMERABLE);
}

static void cam_capabilities(js_object *o)
{
  double minw = 1e9, maxw = 0, minh = 1e9, maxh = 0, minf = 1e9, maxf = 0;
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
  js_object *modes = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_array_push(ctx, modes, js_mk_cstring(ctx, "none"));
  js_def(o, "width", mediastream_range(minw, maxw), P_ENUMERABLE);
  js_def(o, "height", mediastream_range(minh, maxh), P_ENUMERABLE);
  js_def(o, "frameRate", mediastream_range(minf, maxf), P_ENUMERABLE);
  js_def(o, "aspectRatio", mediastream_range(maxh > 0 ? minw / maxh : 0, minh > 0 ? maxw / minh : 0), P_ENUMERABLE);
  js_def(o, "deviceId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "groupId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "resizeMode", js_obj_value(modes), P_ENUMERABLE);
}

static bool cam_tick(unsigned now)
{
  if (now - cam.last_poll < POLL_MS)
    return true;
  cam.last_poll = now;
  if (!poll_frame(now))
    return false;
  if (cam.running && now - cam.last_new > STALL_MS) {
    char state[96];
    int n = readfile(CONTROL_PATH, state, sizeof state - 1);
    cam.last_new = now;
    if (n >= 4 && !memcmp(state, "idle", 4))
      cam_start(cam.fmt, cam.fps);
  }
  return true;
}

static void cam_init(js_ctx *c, const char *page_url)
{
  ctx = c;
  memset(&cam, 0, sizeof cam);
  memset(&dev, 0, sizeof dev);
  cam.last_poll = msec();
  mediastream_device_id(page_url, "cam0", device_id);
}

static void cam_teardown(void)
{
  cam_stop();
  free(cam.rgba);
  free(cam.raw);
  free(cam.black);
  memset(&cam, 0, sizeof cam);
}

static bool cam_running(void) { return cam.running; }
static const char *cam_label(void) { return dev.name; }
static const char *cam_id(void) { return device_id; }

const struct ms_device *camera_device(void)
{
  static const struct ms_device d = {"video",     "videoinput", PERM_CAMERA,      cam_init, load_info,
                                     cam_label,   cam_id,       cam_choose,       cam_open, cam_stop,
                                     cam_running, cam_settings, cam_capabilities, cam_tick, cam_teardown};
  return &d;
}
