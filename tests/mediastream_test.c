/* Host test for navigator.mediaDevices, MediaStream/MediaStreamTrack, camera frame decoding and microphone capture
 * (features/mediastream, camera, microphone) against a fake /sys. The three sources are built into this one unit. */
#define OWOS_SUITE_TEST
#include "jsint.h"
#include "uuid.h"
#define msec cam_fake_msec
#define readfile cam_fake_readfile
#define readfile_at cam_fake_readfile_at
#define writefile cam_fake_writefile
#define make_event cam_fake_make_event
#define ev_fire_on cam_fake_ev_fire_on
#define el_add_listener cam_fake_add_listener
#define el_remove_listener cam_fake_remove_listener
#define el_dispatch cam_fake_dispatch
#define dom_exception cam_fake_dom_exception
#define worker_permission cam_fake_worker_permission
#define perm_state_now cam_fake_perm_state_now
#define ch_secure_url cam_fake_secure_url
#define illegal_ctor cam_fake_illegal_ctor
#define uuid_generate_v4 cam_fake_uuid_v4
#define uuid_to_string cam_fake_uuid_to_string
unsigned cam_fake_msec(void);
int cam_fake_readfile(const char *name, char *buf, int max);
int cam_fake_readfile_at(const char *name, unsigned off, void *buf, unsigned len);
int cam_fake_writefile(const char *name, const void *buf, int len);
js_value cam_fake_make_event(const char *type, int x, int y, js_value init);
void cam_fake_ev_fire_on(js_object *obj, js_value ev, int want_capture);
js_value cam_fake_add_listener(js_ctx *c, js_value t, js_value *a, int n);
js_value cam_fake_remove_listener(js_ctx *c, js_value t, js_value *a, int n);
js_value cam_fake_dispatch(js_ctx *c, js_value t, js_value *a, int n);
js_value cam_fake_dom_exception(js_ctx *c, const char *name, const char *msg);
int cam_fake_worker_permission(int feature);
int cam_fake_perm_state_now(int feature);
bool cam_fake_secure_url(const char *url);
js_value cam_fake_illegal_ctor(js_ctx *c, js_value t, js_value *a, int n);
void cam_fake_uuid_v4(uuid_t out);
void cam_fake_uuid_to_string(const uuid_t u, char *out, int upper);
#define ctx ms_ctx
#include "features/mediastream/mediastream.c"
#undef ctx
#define ctx cam_ctx
#define device_id cam_device_id
#include "features/camera/camera.c"
#undef ctx
#undef device_id
#undef DIR
#define ctx mic_ctx
#define device_id mic_device_id
#include "features/microphone/microphone.c"
#undef ctx
#undef device_id
#undef INFO_PATH
#undef CONTROL_PATH
#undef FRAME_PATH
#undef CAPTURE_PATH
#undef PCM_PATH
#define INFO_PATH "/sys/class/video/cam0/info"
#define CONTROL_PATH "/sys/class/video/cam0/control"
#define FRAME_PATH "/sys/class/video/cam0/frame"
#define CAPTURE_PATH "/sys/class/sound/card0/capture"
#define PCM_PATH "/sys/class/sound/card0/capture_pcm"
#include "jpeg_fixtures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned clock_ms = 1000;
unsigned cam_fake_msec(void) { return clock_ms; }

static const char *const INFO_TEXT = "name Test Cam\n"
                                     "id 046d:0825\n"
                                     "format MJPG 1280x720 30 15\n"
                                     "format MJPG 640x480 30 15\n"
                                     "format YUY2 640x480 30 15\n"
                                     "format YUY2 320x240 30\n"
                                     "format NV12 160x120 15\n";

static bool present, start_fails, mic_there, mic_start_fails, mic_capturing;
static unsigned long long mic_pos;
#define MIC_RING 96000 /* what the fake kernel retains */
static char control_state[96];
static char cmds[64][96];
static int ncmds, prompts, payload_reads, pcm_reads;
static int perm_answer[PERM_FEATURE_COUNT], perm_now[PERM_FEATURE_COUNT];
static unsigned frame_seq, frame_w, frame_h;
static char frame_fourcc[5];
static unsigned char frame_payload[4096];
static unsigned frame_len;
static unsigned uuid_counter;

bool cam_fake_secure_url(const char *url) { return !strncmp(url, "https:", 6); }

int cam_fake_worker_permission(int feature)
{
  prompts++;
  return perm_answer[feature];
}
int cam_fake_perm_state_now(int feature) { return perm_now[feature]; }

js_value cam_fake_illegal_ctor(js_ctx *c, js_value t, js_value *a, int n)
{
  (void)t;
  (void)a;
  (void)n;
  return js_throw_type(c, "Illegal constructor");
}

void cam_fake_uuid_v4(uuid_t out)
{
  memset(out, 0, 16);
  out[14] = (unsigned char)(++uuid_counter >> 8);
  out[15] = (unsigned char)uuid_counter;
}
void cam_fake_uuid_to_string(const uuid_t u, char *out, int upper)
{
  (void)upper;
  snprintf(out, UUID_STR_LEN, "00000000-0000-0000-0000-00000000%02x%02x", u[14], u[15]);
}

js_value cam_fake_dom_exception(js_ctx *c, const char *name, const char *msg)
{
  return js_make_error(c, c->type_error_proto, name, msg);
}

js_value cam_fake_make_event(const char *type, int x, int y, js_value init)
{
  js_object *e = js_new_object(g_ctx, g_ctx->object_proto);
  (void)x;
  (void)y;
  (void)init;
  js_def(e, "type", js_mk_cstring(g_ctx, type), P_WRITABLE | P_ENUMERABLE);
  return js_obj_value(e);
}

void cam_fake_ev_fire_on(js_object *obj, js_value ev, int want_capture)
{
  char on[64] = "on";
  js_value h;
  (void)want_capture;
  strcat(on, js_get(g_ctx, ev, "type").u.str->data);
  h = js_get(g_ctx, js_obj_value(obj), on);
  if (js_is_callable(h))
    js_call(g_ctx, h, js_obj_value(obj), &ev, 1);
}

js_value cam_fake_add_listener(js_ctx *c, js_value t, js_value *a, int n)
{
  (void)c;
  (void)t;
  (void)a;
  (void)n;
  return js_undef();
}
js_value cam_fake_remove_listener(js_ctx *c, js_value t, js_value *a, int n)
{
  return cam_fake_add_listener(c, t, a, n);
}
js_value cam_fake_dispatch(js_ctx *c, js_value t, js_value *a, int n) { return cam_fake_add_listener(c, t, a, n); }

/* Sample i of the fake microphone stream */
static short mic_sample(unsigned long long i) { return (short)((i * 7 + 3) % 20011) - 10000; }

int cam_fake_readfile(const char *name, char *buf, int max)
{
  if (!strcmp(name, CAPTURE_PATH)) {
    char text[96];
    if (!mic_there)
      return -1;
    snprintf(text, sizeof text, "%s\npos %llu\n", mic_capturing ? "capturing 48000 1" : "idle", mic_pos);
    int n = (int)strlen(text) < max ? (int)strlen(text) : max;
    memcpy(buf, text, (size_t)n);
    return n;
  }
  const char *text = !present                      ? 0
                     : !strcmp(name, INFO_PATH)    ? INFO_TEXT
                     : !strcmp(name, CONTROL_PATH) ? control_state
                                                   : 0;
  if (!text)
    return -1;
  int n = (int)strlen(text) < max ? (int)strlen(text) : max;
  memcpy(buf, text, (size_t)n);
  return n;
}

int cam_fake_readfile_at(const char *name, unsigned off, void *buf, unsigned len)
{
  unsigned char all[HDR_LEN + sizeof frame_payload];
  if (!strcmp(name, PCM_PATH)) {
    if (!mic_there || !mic_capturing)
      return -1;
    unsigned long long oldest = mic_pos > MIC_RING ? mic_pos - MIC_RING : 0, start = off < oldest ? oldest : off;
    if (start >= mic_pos)
      return 0;
    unsigned n = mic_pos - start < len ? (unsigned)(mic_pos - start) : len;
    for (unsigned i = 0; i < n; i++) {
      unsigned long long b = start + i;
      ((unsigned char *)buf)[i] = (unsigned char)(mic_sample(b / 2) >> (8 * (b & 1)));
    }
    pcm_reads++;
    return (int)n;
  }
  if (!present || strcmp(name, FRAME_PATH))
    return -1;
  unsigned hdr[6] = {frame_seq, 0, frame_w, frame_h, frame_len, 0};
  memcpy(&hdr[1], frame_fourcc, 4);
  memcpy(all, hdr, HDR_LEN);
  memcpy(all + HDR_LEN, frame_payload, frame_len);
  unsigned total = HDR_LEN + frame_len;
  payload_reads += off >= HDR_LEN;
  if (off >= total)
    return 0;
  unsigned n = total - off < len ? total - off : len;
  memcpy(buf, all + off, n);
  return (int)n;
}

int cam_fake_writefile(const char *name, const void *buf, int len)
{
  char line[96];
  if (!strcmp(name, CAPTURE_PATH)) {
    snprintf(cmds[ncmds++ % 64], 96, "mic %.*s", len, (const char *)buf);
    if (!mic_there || (mic_start_fails && !strncmp(buf, "start", 5)))
      return -1;
    mic_capturing = !strncmp(buf, "start", 5);
    mic_pos = 0;
    return 0;
  }
  if (strcmp(name, CONTROL_PATH) || len >= (int)sizeof line)
    return -1;
  memcpy(line, buf, (size_t)len);
  line[len] = 0;
  snprintf(cmds[ncmds++ % 64], 96, "%s", line);
  if (!strcmp(line, "stop")) {
    strcpy(control_state, "idle\n");
    return 0;
  }
  if (start_fails || strncmp(line, "start ", 6))
    return -1;
  char fourcc[8];
  unsigned w, h, fps;
  if (sscanf(line + 6, "%7s %u %u %u", fourcc, &w, &h, &fps) != 4)
    return -1;
  char want[48];
  snprintf(want, sizeof want, "format %s %ux%u", fourcc, w, h);
  const char *entry = strstr(INFO_TEXT, want);
  char fps_text[16];
  snprintf(fps_text, sizeof fps_text, " %u", fps);
  const char *eol = entry ? strchr(entry, '\n') : 0;
  if (!entry || !eol || !memmem(entry, (size_t)(eol - entry), fps_text, strlen(fps_text)))
    return -1;
  snprintf(control_state, sizeof control_state, "streaming %s %u %u %u\n", fourcc, w, h, fps);
  return 0;
}

static const char *last_cmd(void) { return ncmds ? cmds[(ncmds - 1) % 64] : ""; }

static int count_cmds(const char *prefix)
{
  int n = 0;
  for (int i = 0; i < ncmds && i < 64; i++)
    n += !strncmp(cmds[i], prefix, strlen(prefix));
  return n;
}

static js_object *navigator;
static js_arena arenas[1024];
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
    if (js_run(g_ctx, program, &result) != 0) {
      js_value m = js_get(g_ctx, g_ctx->exc, "message");
      printf("  uncaught: %s\n", m.type == JV_STR ? m.u.str->data : "(none)");
      expect(0);
      g_ctx->has_exc = 0;
    }
}

static void tick(int n)
{
  while (n-- > 0) {
    clock_ms += 20;
    mediastream_tick();
  }
}

static void set_frame(const char *fourcc, unsigned w, unsigned h, const void *data, unsigned len)
{
  memcpy(frame_fourcc, fourcc, 4);
  frame_fourcc[4] = 0;
  frame_w = w;
  frame_h = h;
  memcpy(frame_payload, data, len);
  frame_len = len;
  frame_seq++;
}

static void setup_url(const char *url)
{
  mediastream_teardown();
  present = true;
  start_fails = mic_start_fails = false;
  mic_there = mic_capturing = false;
  mic_pos = 0;
  strcpy(control_state, "idle\n");
  ncmds = prompts = payload_reads = 0;
  frame_seq = frame_len = 0;
  for (int f = 0; f < PERM_FEATURE_COUNT; f++)
    perm_answer[f] = PERM_GRANTED, perm_now[f] = PERM_ASK;
  js_object *nav = js_new_object(g_ctx, g_ctx->object_proto);
  js_def(g_ctx->global, "navigator", js_obj_value(nav), P_WRITABLE);
  navigator = nav;
  mediastream_install(g_ctx, navigator, url);
  run("var R = {}, L = [];"
      "function done(k, p) { p.then(function (v) { R[k] = ['ok', v]; }, function (e) { R[k] = ['err', e.name, "
      "e.constraint]; }); }"
      "function gum(c) { return navigator.mediaDevices.getUserMedia(c); }"
      "function vid(c) { return navigator.mediaDevices.getUserMedia({video: c}); }");
}

static void setup(void) { setup_url("https://example.com/page"); }

static void test_secure_context_and_surface(void)
{
  setup();
  run("var md = navigator.mediaDevices;"
      "expect(typeof md.getUserMedia === 'function' && typeof md.enumerateDevices === 'function');"
      "expect(typeof md.getDisplayMedia === 'function' && md.getSupportedConstraints().frameRate === true);"
      "expect(typeof MediaStream === 'function' && typeof MediaStreamTrack === 'function');"
      "var bad = 0; try { new MediaStreamTrack(); } catch (e) { bad = 1; } expect(bad === 1);"
      "done('disp', md.getDisplayMedia({video: true}));");
  run("expect(R.disp[1] === 'NotAllowedError');");
  setup_url("http://example.com/");
  run("expect(navigator.mediaDevices === undefined);");
}

static void check_pick(const char *constraints, const char *expect_cmd)
{
  char js[512];
  ncmds = 0;
  snprintf(js, sizeof js, "done('p', vid(%s));", constraints);
  run(js);
  run("if (!(R.p && R.p[0] === 'ok')) throw new Error('gum: ' + JSON.stringify(R.p)); "
      "R.p[1].getVideoTracks()[0].stop();");
  if (strcmp(cmds[0], expect_cmd))
    printf("  constraints %s started \"%s\", wanted \"%s\"\n", constraints, cmds[0], expect_cmd);
  expect_string(cmds[0], expect_cmd);
  expect_string(last_cmd(), "stop");
}

static void test_constraint_selection(void)
{
  setup();
  check_pick("true", "start MJPG 640 480 30");
  check_pick("{}", "start MJPG 640 480 30");
  check_pick("{width: 1280}", "start MJPG 1280 720 30");
  check_pick("{width: {ideal: 1280}, height: {ideal: 720}}", "start MJPG 1280 720 30");
  check_pick("{width: {exact: 320}}", "start YUY2 320 240 30");
  check_pick("{width: {min: 1000}}", "start MJPG 1280 720 30");
  check_pick("{height: {ideal: 700}}", "start MJPG 1280 720 30");
  check_pick("{height: {max: 300}}", "start YUY2 320 240 30");
  check_pick("{frameRate: {exact: 15}}", "start MJPG 640 480 15");
  check_pick("{width: {ideal: 160}, frameRate: {ideal: 15}}", "start NV12 160 120 15");
  check_pick("{width: {exact: 1280}, height: {exact: 720}, frameRate: 15}", "start MJPG 1280 720 15");
  check_pick("{facingMode: {exact: 'environment'}}", "start MJPG 640 480 30");

  ncmds = 0;
  run("done('w', vid({width: {exact: 999}})); done('h', vid({height: {exact: 1}}));"
      "done('f', vid({frameRate: {exact: 60}})); done('d', vid({deviceId: {exact: 'nope'}}));"
      "done('mx', vid({width: {max: 100}}));");
  run("expect(R.w[1] === 'OverconstrainedError' && R.w[2] === 'width');"
      "expect(R.h[1] === 'OverconstrainedError' && R.h[2] === 'height');"
      "expect(R.f[1] === 'OverconstrainedError' && R.f[2] === 'frameRate');"
      "expect(R.d[1] === 'OverconstrainedError' && R.d[2] === 'deviceId');"
      "expect(R.mx[1] === 'OverconstrainedError' && R.mx[2] === 'width');");
  expect_eq(ncmds, 0);

  perm_now[PERM_CAMERA] = PERM_GRANTED;
  run("done('devs', navigator.mediaDevices.enumerateDevices());");
  run("var id = R.devs[1][0].deviceId; expect(id.length === 16);"
      "done('ok', vid({deviceId: {exact: id}})); done('arr', vid({deviceId: {exact: ['x', id]}}));");
  run("expect(R.ok[0] === 'ok' && R.arr[0] === 'ok');");
  expect_eq(count_cmds("start"), 1);
}

static void test_permissions_and_errors(void)
{
  setup();
  run("done('none', navigator.mediaDevices.getUserMedia());"
      "done('empty', navigator.mediaDevices.getUserMedia({}));"
      "done('off', navigator.mediaDevices.getUserMedia({video: false, audio: false}));");
  run("expect(R.none[1] === 'TypeError' && R.empty[1] === 'TypeError' && R.off[1] === 'TypeError');");
  expect_eq(prompts, 0);

  run("done('mic', navigator.mediaDevices.getUserMedia({audio: true}));"
      "done('both', navigator.mediaDevices.getUserMedia({audio: true, video: true}));");
  run("expect(R.mic[1] === 'NotFoundError' && R.both[1] === 'NotFoundError');");
  expect_eq(prompts, 0);
  expect_eq(ncmds, 0);

  perm_answer[PERM_CAMERA] = PERM_DENIED;
  run("done('deny', vid(true));");
  run("expect(R.deny[1] === 'NotAllowedError');");
  expect_eq(prompts, 1);
  expect_eq(ncmds, 0);

  perm_answer[PERM_CAMERA] = PERM_GRANTED;
  start_fails = true;
  run("done('busy', vid(true));");
  run("expect(R.busy[1] === 'NotReadableError');");
  start_fails = false;

  present = false;
  prompts = 0;
  run("done('absent', vid(true));");
  run("expect(R.absent[1] === 'NotFoundError');");
  expect_eq(prompts, 0);
}

static void test_track_lifecycle(void)
{
  setup();
  run("done('s', vid(true));");
  run("expect(R.s[0] === 'ok'); var s = R.s[1], t = s.getVideoTracks()[0];"
      "expect(s instanceof MediaStream && t instanceof MediaStreamTrack && s.active === true);"
      "expect(s.getTracks().length === 1 && s.getAudioTracks().length === 0 && s.getTracks()[0] === t);"
      "expect(t.kind === 'video' && t.label === 'Test Cam' && t.readyState === 'live' && t.enabled === true);"
      "expect(t.muted === false && s.id.length === 36 && t.id.length === 36 && s.id !== t.id);"
      "expect(s.getTrackById(t.id) === t && s.getTrackById('zzz') === null);"
      "var st = t.getSettings();"
      "expect(st.width === 640 && st.height === 480 && st.frameRate === 30 && st.deviceId.length === 16);"
      "var cp = t.getCapabilities();"
      "expect(cp.width.min === 160 && cp.width.max === 1280 && cp.height.min === 120 && cp.height.max === 720);"
      "expect(cp.frameRate.min === 15 && cp.frameRate.max === 30);"
      "expect(typeof t.getConstraints() === 'object');");
  expect_string(last_cmd(), "start MJPG 640 480 30");

  run("var c = t.clone(); expect(c !== t && c.id !== t.id && c.readyState === 'live');"
      "var s2 = new MediaStream([t, c]); expect(s2.getTracks().length === 2 && s2.active);"
      "s2.removeTrack(c); expect(s2.getTracks().length === 1);"
      "s2.addTrack(c); s2.addTrack(c); expect(s2.getTracks().length === 2);"
      "var s3 = s.clone(); expect(s3.getTracks().length === 1 && s3.getTracks()[0] !== t);"
      "s3.getTracks()[0].stop();");
  expect_eq(count_cmds("stop"), 0);
  run("t.stop(); expect(t.readyState === 'ended' && s.active === false);");
  expect_eq(count_cmds("stop"), 0);
  run("c.stop(); expect(c.readyState === 'ended' && s2.active === false);");
  expect_eq(count_cmds("stop"), 1);
  run("c.stop(); t.stop();");
  expect_eq(count_cmds("stop"), 1);

  ncmds = 0;
  run("done('again', vid(true));");
  run("expect(R.again[0] === 'ok'); var t2 = R.again[1].getTracks()[0]; expect(t2.getSettings().width === 640);");
  expect_string(last_cmd(), "start MJPG 640 480 30");
  run("var dead = t2.clone(); t2.stop(); var ended = dead.clone(); expect(ended.readyState === 'live'); dead.stop();");
  run("var gone = t2.clone(); expect(gone.readyState === 'ended');");
}

static void test_camera_loss_ends_tracks(void)
{
  setup();
  run("done('s', vid(true));");
  run("var t = R.s[1].getTracks()[0]; t.onended = function (e) { L.push('ended'); };"
      "var u = t.clone(); u.onended = function () { L.push('ended2'); };");
  tick(3);
  run("expect(L.length === 0 && t.readyState === 'live');");
  present = false;
  tick(3);
  run("expect(L.length === 2 && L[0] === 'ended' && t.readyState === 'ended' && u.readyState === 'ended');"
      "expect(R.s[1].active === false);");
  present = true;
  ncmds = 0;
  run("done('re', vid(true));");
  run("expect(R.re[0] === 'ok');");
  expect_string(last_cmd(), "start MJPG 640 480 30");
}

static void test_apply_constraints(void)
{
  setup();
  run("done('s', vid(true));");
  run("var t = R.s[1].getTracks()[0];"
      "done('a', t.applyConstraints({width: {exact: 1280}}));");
  run("expect(R.a[0] === 'ok' && t.getSettings().width === 1280 && t.getSettings().height === 720);");
  expect_string(last_cmd(), "start MJPG 1280 720 30");
  run("done('b', t.applyConstraints({width: {exact: 777}}));");
  run("expect(R.b[1] === 'OverconstrainedError' && R.b[2] === 'width' && t.getSettings().width === 1280);");
  run("var c = t.clone(); done('c', t.applyConstraints({width: {exact: 640}}));");
  run("expect(R.c[1] === 'OverconstrainedError' && t.getSettings().width === 1280);");
  run("done('d', t.applyConstraints({width: {exact: 1280}}));");
  run("expect(R.d[0] === 'ok');");
  expect_eq(count_cmds("start"), 2);
}

static void test_enumerate_devices(void)
{
  setup();
  run("done('before', navigator.mediaDevices.enumerateDevices());");
  run("var d = R.before[1]; expect(d.length === 1 && d[0].kind === 'videoinput');"
      "expect(d[0].label === '' && d[0].deviceId === '' && d[0].groupId === '');");
  perm_now[PERM_CAMERA] = PERM_GRANTED;
  run("done('after', navigator.mediaDevices.enumerateDevices());");
  run("var d = R.after[1]; expect(d[0].label === 'Test Cam' && d[0].deviceId.length === 16 && d[0].groupId === "
      "d[0].deviceId);");
  perm_now[PERM_CAMERA] = PERM_ASK;
  run("done('live', vid(true));");
  run("done('during', navigator.mediaDevices.enumerateDevices());");
  run("expect(R.during[1][0].label === 'Test Cam');"
      "var a = R.during[1][0].deviceId, tid = R.live[1].getTracks()[0].getSettings().deviceId; expect(a === tid);");
  setup_url("https://other.example/");
  perm_now[PERM_CAMERA] = PERM_GRANTED;
  run("done('other', navigator.mediaDevices.enumerateDevices());");
  run("expect(R.other[1][0].deviceId.length === 16);");
  present = false;
  run("done('none', navigator.mediaDevices.enumerateDevices());");
  run("expect(R.none[1].length === 0);");
}

static void expect_px(const unsigned char *rgba, int w, int x, int y, int r, int g, int b)
{
  const unsigned char *p = rgba + (y * w + x) * 4;
  if (p[0] != r || p[1] != g || p[2] != b || p[3] != 255)
    printf("  pixel (%d,%d) is %d,%d,%d,%d, wanted %d,%d,%d,255\n", x, y, p[0], p[1], p[2], p[3], r, g, b);
  expect(p[0] == r && p[1] == g && p[2] == b && p[3] == 255);
}

static void open_camera(const char *constraints)
{
  char js[256];
  snprintf(js, sizeof js, "done('s', vid(%s)); var t = R.s[1].getTracks()[0];", constraints);
  run(js);
}

static void test_yuy2_and_nv12_frames(void)
{
  int w, h;
  unsigned seq, seq0;
  setup();
  expect(camera_frame(&w, &h, &seq) == 0);
  open_camera("{width: 320}");
  tick(2);
  expect(camera_frame(&w, &h, &seq) == 0);

  static const unsigned char yuy2[] = {16, 128, 235, 128, 81, 90, 81, 240, 145, 54, 145, 34, 41, 240, 41, 110};
  set_frame("YUY2", 4, 2, yuy2, sizeof yuy2);
  tick(1);
  const unsigned char *f = camera_frame(&w, &h, &seq0);
  expect(f != 0 && w == 4 && h == 2);
  if (f) {
    expect_px(f, 4, 0, 0, 0, 0, 0);
    expect_px(f, 4, 1, 0, 255, 255, 255);
    expect_px(f, 4, 2, 0, 255, 0, 0);
    expect_px(f, 4, 3, 0, 255, 0, 0);
    expect_px(f, 4, 0, 1, 0, 255, 1);
    expect_px(f, 4, 1, 1, 0, 255, 1);
    expect_px(f, 4, 2, 1, 0, 0, 255);
    expect_px(f, 4, 3, 1, 0, 0, 255);
  }
  expect_eq(payload_reads, 1);
  tick(3);
  expect_eq(payload_reads, 1);

  static const unsigned char nv12[] = {16, 235, 81, 81, 126, 126, 81, 81, 128, 128, 90, 240};
  set_frame("NV12", 4, 2, nv12, sizeof nv12);
  tick(1);
  f = camera_frame(&w, &h, &seq);
  expect(f != 0 && seq != seq0 && w == 4 && h == 2);
  if (f) {
    expect_px(f, 4, 0, 0, 0, 0, 0);
    expect_px(f, 4, 1, 0, 255, 255, 255);
    expect_px(f, 4, 2, 0, 255, 0, 0);
    expect_px(f, 4, 3, 0, 255, 0, 0);
    expect_px(f, 4, 0, 1, 128, 128, 128);
    expect_px(f, 4, 1, 1, 128, 128, 128);
    expect_px(f, 4, 2, 1, 255, 0, 0);
    expect_px(f, 4, 3, 1, 255, 0, 0);
  }

  static const unsigned char short_frame[] = {16, 128, 235, 128};
  unsigned bad_seq = seq;
  set_frame("YUY2", 4, 2, short_frame, sizeof short_frame);
  tick(1);
  camera_frame(&w, &h, &seq);
  expect(seq == bad_seq && w == 4);

  run("t.enabled = false; expect(t.enabled === false);");
  f = camera_frame(&w, &h, &seq);
  expect(f != 0 && seq != bad_seq);
  if (f) {
    expect_px(f, 4, 1, 0, 0, 0, 0);
    expect_px(f, 4, 2, 1, 0, 0, 0);
  }
  run("t.enabled = true;");
  f = camera_frame(&w, &h, &seq);
  if (f)
    expect_px(f, 4, 1, 0, 255, 255, 255);

  run("t.stop();");
  expect(camera_frame(&w, &h, &seq) != 0);
}

/* A webcam MJPEG frame: the fixture without its Huffman tables */
static unsigned strip_dht(const unsigned char *src, unsigned len, unsigned char *dst)
{
  unsigned n = 2;
  memcpy(dst, src, 2);
  for (unsigned i = 2; i < len;) {
    unsigned seg = src[i + 1] == 0xDA ? len - i : 2u + (src[i + 2] << 8 | src[i + 3]);
    if (src[i + 1] != 0xC4) {
      memcpy(dst + n, src + i, seg);
      n += seg;
    }
    i += seg;
  }
  return n;
}

static void test_mjpeg_frames(void)
{
  int w, h;
  unsigned seq;
  unsigned char stripped[sizeof jpg_b444];
  setup();
  open_camera("true");
  unsigned n = strip_dht(jpg_b444, sizeof jpg_b444, stripped);
  expect(n < sizeof jpg_b444 && n < sizeof frame_payload);
  set_frame("MJPG", 640, 480, stripped, n);
  tick(1);
  const unsigned char *f = camera_frame(&w, &h, &seq);
  expect(f != 0 && w == 35 && h == 19);
  if (f) {
    const unsigned char *p = f + (0 * 35 + 34) * 4;
    expect(p[0] >= 245 && p[1] <= 10 && p[2] >= 215 && p[2] <= 245 && p[3] == 255);
    p = f + (18 * 35 + 0) * 4;
    expect(p[0] <= 10 && p[1] >= 245 && p[2] >= 60 && p[2] <= 85);
  }
  unsigned first = seq;
  set_frame("MJPG", 640, 480, "garbage", 7);
  tick(1);
  f = camera_frame(&w, &h, &seq);
  expect(f != 0 && seq == first && w == 35);
  run("t.stop();");
}

static void test_stall_restarts_camera(void)
{
  setup();
  open_camera("true");
  ncmds = 0;
  tick(10);
  expect_eq(ncmds, 0);
  strcpy(control_state, "idle\n");
  tick(100);
  expect_string(last_cmd(), "start MJPG 640 480 30");
  expect_eq(count_cmds("start"), 1);
  run("t.stop();");
}

static void test_page_teardown_stops_camera(void)
{
  int w, h;
  unsigned seq;
  setup();
  open_camera("true");
  static const unsigned char px2[] = {16, 128, 235, 128};
  set_frame("YUY2", 2, 1, px2, sizeof px2);
  tick(1);
  expect(camera_frame(&w, &h, &seq) != 0);
  ncmds = 0;
  mediastream_teardown();
  expect_string(last_cmd(), "stop");
  expect(camera_frame(&w, &h, &seq) == 0);
  expect(!mediastream_is_stream(js_undef()));
}

static void test_audio_user_media(void)
{
  setup();
  run("done('a', gum({audio: true})); done('av', gum({audio: true, video: true}));");
  run("expect(R.a[1] === 'NotFoundError' && R.av[1] === 'NotFoundError');");
  expect_eq(prompts, 0);
  expect_eq(ncmds, 0);

  mic_there = true;
  perm_answer[PERM_MICROPHONE] = PERM_DENIED;
  run("done('deny', gum({audio: true}));");
  run("expect(R.deny[1] === 'NotAllowedError');");
  expect_eq(prompts, 1);
  expect_eq(count_cmds("mic start"), 0);

  perm_answer[PERM_MICROPHONE] = PERM_GRANTED;
  mic_start_fails = true;
  run("done('busy', gum({audio: true}));");
  run("expect(R.busy[1] === 'NotReadableError');");
  mic_start_fails = false;

  prompts = ncmds = 0;
  run("done('s', gum({audio: true}));");
  run("expect(R.s[0] === 'ok'); var s = R.s[1], t = s.getAudioTracks()[0];"
      "expect(s.getTracks().length === 1 && s.getVideoTracks().length === 0 && s.getTracks()[0] === t);"
      "expect(t.kind === 'audio' && t.label === 'Microphone' && t.readyState === 'live' && t.enabled === true);"
      "var st = t.getSettings();"
      "expect(st.sampleRate === 48000 && st.channelCount === 1 && st.sampleSize === 16 && st.deviceId.length === 16);"
      "expect(st.echoCancellation === false && st.noiseSuppression === false && st.autoGainControl === false);"
      "var cp = t.getCapabilities();"
      "expect(cp.sampleRate.min === 48000 && cp.sampleRate.max === 48000 && cp.channelCount.max === 1);"
      "expect(cp.echoCancellation.length === 1 && cp.echoCancellation[0] === false);");
  expect_eq(prompts, 1);
  expect_string(last_cmd(), "mic start");
  run("t.stop(); expect(t.readyState === 'ended' && s.active === false);");
  expect_string(last_cmd(), "mic stop");
  expect_eq(count_cmds("mic stop"), 1);

  run("done('r1', gum({audio: {sampleRate: {exact: 44100}}})); done('r2', gum({audio: {channelCount: {exact: 2}}}));"
      "done('r3', gum({audio: {echoCancellation: {exact: true}}})); done('r4', gum({audio: {deviceId: {exact: 'x'}}}));"
      "done('r5', gum({audio: {sampleRate: {min: 96000}}}));");
  run("expect(R.r1[1] === 'OverconstrainedError' && R.r1[2] === 'sampleRate');"
      "expect(R.r2[1] === 'OverconstrainedError' && R.r2[2] === 'channelCount');"
      "expect(R.r3[1] === 'OverconstrainedError' && R.r3[2] === 'echoCancellation');"
      "expect(R.r4[1] === 'OverconstrainedError' && R.r4[2] === 'deviceId');"
      "expect(R.r5[1] === 'OverconstrainedError' && R.r5[2] === 'sampleRate');");
  ncmds = 0;
  run("done('ok', gum({audio: {sampleRate: 44100, channelCount: 2, echoCancellation: true, noiseSuppression: {ideal: "
      "true},"
      "autoGainControl: false, deviceId: {ideal: 'zzz'}}}));");
  run("var ok = R.ok[1].getAudioTracks()[0];"
      "expect(R.ok[0] === 'ok' && ok.getSettings().echoCancellation === false && ok.getSettings().sampleRate === "
      "48000);"
      "expect(ok.getConstraints().channelCount === 2);"
      "done('apply', ok.applyConstraints({sampleRate: {exact: 8000}})); done('apply2', "
      "ok.applyConstraints({channelCount: 1}));");
  run("expect(R.apply[1] === 'OverconstrainedError' && R.apply[2] === 'sampleRate' && R.apply2[0] === 'ok');"
      "ok.stop();");
  expect_eq(count_cmds("mic start"), 1);

  run("done('id', navigator.mediaDevices.enumerateDevices());");
  perm_now[PERM_MICROPHONE] = PERM_GRANTED;
  run("var open = gum({audio: true}); done('open', open);");
  run("var mid = R.open[1].getAudioTracks()[0].getSettings().deviceId; done('right', gum({audio: {deviceId: {exact: "
      "mid}}}));"
      "done('arr', gum({audio: {deviceId: {exact: ['nope', mid]}}}));");
  run("expect(R.right[0] === 'ok' && R.arr[0] === 'ok');");
}

static void test_audio_and_video_together(void)
{
  setup();
  mic_there = true;
  run("done('av', gum({audio: true, video: {width: 1280}}));");
  run("var av = R.av[1]; expect(R.av[0] === 'ok' && av.getTracks().length === 2);"
      "expect(av.getAudioTracks().length === 1 && av.getVideoTracks().length === 1 && av.getTracks()[0].kind === "
      "'audio');"
      "expect(av.getVideoTracks()[0].getSettings().width === 1280 && av.getAudioTracks()[0].getSettings().sampleRate "
      "=== 48000);"
      "expect(av.getTrackById(av.getVideoTracks()[0].id) === av.getVideoTracks()[0]);");
  expect_eq(prompts, 2);
  expect_eq(count_cmds("mic start"), 1);
  expect_eq(count_cmds("start MJPG 1280 720"), 1);
  run("av.getVideoTracks()[0].stop(); expect(av.active === true);");
  expect_eq(count_cmds("stop"), 1);
  expect_eq(count_cmds("mic stop"), 0);
  run("var c = av.clone(); expect(c.getTracks().length === 2 && c.getTracks()[0].kind === 'audio');"
      "av.getAudioTracks()[0].stop();");
  expect_eq(count_cmds("mic stop"), 0);
  run("c.getAudioTracks()[0].stop();");
  expect_eq(count_cmds("mic stop"), 1);

  ncmds = prompts = 0;
  perm_answer[PERM_MICROPHONE] = PERM_DENIED;
  run("done('micno', gum({audio: true, video: true}));");
  run("expect(R.micno[1] === 'NotAllowedError');");
  expect_eq(count_cmds("mic start") + count_cmds("start"), 0);

  perm_answer[PERM_MICROPHONE] = PERM_GRANTED;
  perm_answer[PERM_CAMERA] = PERM_DENIED;
  run("done('camno', gum({audio: true, video: true}));");
  run("expect(R.camno[1] === 'NotAllowedError');");
  expect_eq(count_cmds("mic start") + count_cmds("start"), 0);

  perm_answer[PERM_CAMERA] = PERM_GRANTED;
  run("done('over', gum({audio: true, video: {width: {exact: 999}}}));");
  run("expect(R.over[1] === 'OverconstrainedError' && R.over[2] === 'width');");
  expect_eq(count_cmds("mic start") + count_cmds("start"), 0);

  present = false;
  prompts = 0;
  run("done('nocam', gum({audio: true, video: true})); done('micok', gum({audio: true}));");
  run("expect(R.nocam[1] === 'NotFoundError' && R.micok[0] === 'ok');");
  expect_eq(prompts, 1);
  expect_eq(count_cmds("start"), 0);
}

static void test_enumerate_audio_devices(void)
{
  setup();
  mic_there = true;
  run("done('before', navigator.mediaDevices.enumerateDevices());");
  run("var d = R.before[1]; expect(d.length === 2 && d[0].kind === 'audioinput' && d[1].kind === 'videoinput');"
      "expect(d[0].label === '' && d[0].deviceId === '' && d[1].label === '');");
  perm_now[PERM_MICROPHONE] = PERM_GRANTED;
  run("done('mic', navigator.mediaDevices.enumerateDevices());");
  run("var d = R.mic[1]; expect(d[0].label === 'Microphone' && d[0].deviceId.length === 16 && d[1].label === '');"
      "expect(d[0].groupId === d[0].deviceId && d[0].deviceId !== '');");
  mic_there = false;
  run("done('nomic', navigator.mediaDevices.enumerateDevices());");
  run("expect(R.nomic[1].length === 1 && R.nomic[1][0].kind === 'videoinput');");
  setup();
  mic_there = true;
  run("done('live', gum({audio: true}));");
  run("done('during', navigator.mediaDevices.enumerateDevices());");
  run("expect(R.during[1][0].label === 'Microphone' && R.during[1][1].label === '');"
      "expect(R.during[1][0].deviceId === R.live[1].getTracks()[0].getSettings().deviceId);");
}

static short got[65536];
static int ngot, sink_calls, sink_handle;

static void test_sink(js_ctx *c, int handle, const short *pcm, int n)
{
  (void)c;
  sink_handle = handle;
  sink_calls++;
  if (ngot + n <= (int)(sizeof got / sizeof *got))
    memcpy(got + ngot, pcm, (size_t)n * sizeof *got);
  ngot += n;
}

static js_value mic_track(void) { return js_get(g_ctx, js_obj_value(g_ctx->global), "mt"); }

static int sink;

static void open_mic(void)
{
  setup();
  mic_there = true;
  run("done('s', gum({audio: true}));");
  run("window.mt = R.s[1].getAudioTracks()[0];");
  ngot = sink_calls = 0;
  sink = mic_attach(mic_track(), test_sink);
  expect(sink > 0);
}

static void advance(unsigned long long bytes)
{
  mic_pos += bytes;
  tick(1);
}

/* got[0..ngot) is the stream from sample `first` on, without gaps */
static void expect_stream(unsigned long long first)
{
  int bad = -1;
  for (int i = 0; i < ngot && i < (int)(sizeof got / sizeof *got) && bad < 0; i++)
    if (got[i] != mic_sample(first + (unsigned)i))
      bad = i;
  if (bad >= 0)
    printf("  sample %d is %d, wanted %d\n", bad, got[bad], mic_sample(first + (unsigned)bad));
  expect(bad < 0);
}

static void test_mic_pcm_stream(void)
{
  open_mic();
  advance(960);
  expect_eq(ngot, 480);
  expect_stream(0);

  advance(1001);
  expect_eq(ngot, 980);
  advance(1);
  expect_eq(ngot, 981);
  expect_stream(0);

  ngot = 0;
  advance(20000);
  tick(4);
  expect_eq(ngot, 10000);
  expect_stream(981);
  unsigned long long next = 981 + 10000;

  ngot = 0;
  advance(0);
  expect_eq(ngot, 0);
  mic_detach(sink);
  advance(960);
  expect_eq(ngot, 0);
  expect(pcm_reads > 0);
  int reads = pcm_reads;
  advance(960);
  expect(pcm_reads > reads);
  sink = mic_attach(mic_track(), test_sink);
  next += 960;
  advance(960);
  expect_stream(next);
  expect_eq(ngot, 480);
  next += 480;

  run("mt.stop();");
  expect_string(last_cmd(), "mic stop");
  expect(mic_attach(mic_track(), test_sink) == 0);
  int calls = sink_calls;
  advance(960);
  expect_eq(sink_calls, calls);
}

static void test_mic_falls_behind(void)
{
  open_mic();
  advance(300000);
  tick(3);
  unsigned long long first = (300000 - 9600) / 2;
  expect_eq(ngot, 4800);
  expect_stream(first);

  ngot = 0;
  mic_pos = 100;
  tick(1);
  advance(960);
  expect_eq(ngot, 480);
  expect_stream(50);
}

static void test_mic_disabled_is_silent(void)
{
  open_mic();
  advance(960);
  expect_stream(0);
  run("mt.enabled = false;");
  ngot = 0;
  advance(960);
  expect_eq(ngot, 480);
  int silent = 1;
  for (int i = 0; i < ngot; i++)
    silent &= got[i] == 0;
  expect(silent);
  run("mt.enabled = true;");
  ngot = 0;
  advance(960);
  expect_eq(ngot, 480);
  expect_stream(960);
}

static void test_mic_restarts(void)
{
  open_mic();
  advance(960);
  expect_eq(count_cmds("mic start"), 1);
  mic_capturing = false;
  mic_pos = 0;
  tick(1);
  expect_eq(count_cmds("mic start"), 2);
  expect(mic_capturing);
  ngot = 0;
  advance(960);
  expect_eq(ngot, 480);
  expect_stream(0);

  mic_pos = 0xC0000000ull + 20000;
  tick(3);
  expect_eq(count_cmds("mic stop"), 1);
  expect_eq(count_cmds("mic start"), 3);
  expect_eq((int)mic_pos, 0);
  ngot = 0;
  advance(960);
  expect_stream(0);
  expect_eq(ngot, 480);
}

static void test_mic_loss_ends_tracks(void)
{
  open_mic();
  run("mt.onended = function () { L.push('ended'); }; var other = mt.clone();");
  advance(960);
  mic_there = false;
  tick(2);
  run("expect(L.length === 1 && mt.readyState === 'ended' && other.readyState === 'ended' && R.s[1].active === "
      "false);");
  int calls = sink_calls;
  mic_there = true;
  mic_capturing = true;
  advance(960);
  expect_eq(sink_calls, calls);
}

int main(void)
{
  g_ctx = js_ctx_new(con_print);
  js_def(g_ctx->global, "window", js_obj_value(g_ctx->global), P_WRITABLE);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

  test("surface and secure-context gating", test_secure_context_and_surface);
  test("constraint selection", test_constraint_selection);
  test("permissions and error paths", test_permissions_and_errors);
  test("track and stream lifecycle", test_track_lifecycle);
  test("camera loss ends the tracks", test_camera_loss_ends_tracks);
  test("applyConstraints", test_apply_constraints);
  test("enumerateDevices label gating", test_enumerate_devices);
  test("YUY2 and NV12 frames", test_yuy2_and_nv12_frames);
  test("MJPEG frames without Huffman tables", test_mjpeg_frames);
  test("a stalled camera is restarted", test_stall_restarts_camera);
  test("page teardown stops the camera", test_page_teardown_stops_camera);
  test("microphone getUserMedia, constraints and settings", test_audio_user_media);
  test("microphone together with the camera", test_audio_and_video_together);
  test("audioinput devices and label gating", test_enumerate_audio_devices);
  test("microphone PCM position handling", test_mic_pcm_stream);
  test("microphone skips ahead when it falls behind", test_mic_falls_behind);
  test("a disabled audio track is silent", test_mic_disabled_is_silent);
  test("microphone capture restarts", test_mic_restarts);
  test("microphone loss ends the tracks", test_mic_loss_ends_tracks);

  js_ctx_free(g_ctx);
  return 0;
}
