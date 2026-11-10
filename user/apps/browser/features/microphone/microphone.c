/* microphone.c: the HDA capture stream behind /sys/class/sound/card0 as a capture device for MediaStream
 * (mediastream.c). The kernel keeps about a second of PCM; this side keeps its own read position, hands each chunk to
 * the subscribed sinks (Web Audio source nodes) and skips ahead when it falls too far behind. */

#include "features/microphone/microphone.h"

#define DIR "/sys/class/sound/card0/"
#define CAPTURE_PATH DIR "capture"
#define PCM_PATH DIR "capture_pcm"

#define RATE 48000
#define CHUNK 8192
#define MAX_LAG 65536          /* bytes behind the device before skipping ahead */
#define KEEP_LAG 9600          /* where a skip lands: 100 ms behind the device */
#define RESTART_AT 0xC0000000u /* read offsets are 32 bits: restart the capture before they wrap */
#define POLL_MS 10
#define MAX_SUBS 8

struct sub {
  int handle;
  unsigned serial;
  mic_sink fn;
};

struct capture {
  bool capturing;
  unsigned long long pos;
};

static js_ctx *ctx;
static char device_id[17];
static bool running;
static unsigned rpos, last_poll;
static struct sub subs[MAX_SUBS];
static int last_handle;
static short silence[CHUNK / 2];

static bool starts(const char *s, const char *prefix)
{
  while (*prefix)
    if (*s++ != *prefix++)
      return false;
  return true;
}

/* False if the capture node is gone */
static bool read_capture(struct capture *c)
{
  char text[96];
  int n = readfile(CAPTURE_PATH, text, sizeof text - 1);
  if (n < 0)
    return false;
  text[n] = 0;
  c->capturing = starts(text, "capturing");
  c->pos = 0;
  for (const char *p = strchr(text, '\n'); p && *p; p++)
    if (starts(p, "\npos ")) {
      for (p += 5; *p >= '0' && *p <= '9'; p++)
        c->pos = c->pos * 10 + (unsigned)(*p - '0');
      break;
    }
  return true;
}

/* Begin a capture and read from where it is now */
static bool begin(void)
{
  struct capture c;
  if (writefile(CAPTURE_PATH, "start", 5) < 0 || !read_capture(&c))
    return false;
  rpos = (unsigned)c.pos & ~1u;
  return true;
}

static void deliver(const short *pcm, int n)
{
  for (int i = 0; i < MAX_SUBS; i++) {
    struct sub *s = &subs[i];
    if (!s->handle)
      continue;
    if (!mediastream_serial_live(s->serial))
      s->handle = 0;
    else
      s->fn(ctx, s->handle, mediastream_serial_enabled(s->serial) ? pcm : silence, n);
  }
}

static bool mic_tick(unsigned now)
{
  struct capture c;
  unsigned char buf[CHUNK];
  int n;
  if (now - last_poll < POLL_MS)
    return true;
  last_poll = now;
  if (!read_capture(&c))
    return false;
  if (!c.capturing || rpos >= RESTART_AT) {
    if (c.capturing)
      writefile(CAPTURE_PATH, "stop", 4);
    begin();
    return true;
  }
  if (c.pos < rpos)
    rpos = (unsigned)c.pos & ~1u;
  else if (c.pos - rpos > MAX_LAG)
    rpos = (unsigned)(c.pos - KEEP_LAG) & ~1u;
  n = c.pos - rpos > CHUNK ? CHUNK : (int)(c.pos - rpos);
  if (n <= 1)
    return true;
  n = readfile_at(PCM_PATH, rpos, buf, (unsigned)n);
  if (n < 2)
    return true;
  n &= ~1;
  rpos += (unsigned)n;
  deliver((const short *)buf, n / 2);
  return true;
}

/* ---- subscribers */

int mic_attach(js_value track, mic_sink fn)
{
  unsigned serial = mediastream_track_serial(track);
  for (int i = 0; serial && i < MAX_SUBS; i++)
    if (!subs[i].handle) {
      subs[i].handle = ++last_handle;
      subs[i].serial = serial;
      subs[i].fn = fn;
      return subs[i].handle;
    }
  return 0;
}

void mic_detach(int handle)
{
  for (int i = 0; handle && i < MAX_SUBS; i++)
    if (subs[i].handle == handle)
      subs[i].handle = 0;
}

/* ---- the capture device */

static bool mic_present(void)
{
  char probe[8];
  return readfile(CAPTURE_PATH, probe, sizeof probe) >= 0;
}

static const char *mic_label(void) { return "Microphone"; }
static const char *mic_id(void) { return device_id; }
static bool mic_running(void) { return running; }

/* One fixed format, so only constraints that exclude it fail; the processing switches cannot be turned on */
static const char *mic_choose(js_value constraints, bool shared, struct ms_pick *out)
{
  static const char *const processing[] = {"echoCancellation", "noiseSuppression", "autoGainControl"};
  struct ms_range range;
  (void)shared;
  memset(out, 0, sizeof *out);
  if (constraints.type != JV_OBJ)
    return 0;
  if (!mediastream_device_matches(constraints, device_id))
    return "deviceId";
  mediastream_read_range(js_get(ctx, constraints, "sampleRate"), &range);
  if (!mediastream_in_range(&range, RATE))
    return "sampleRate";
  mediastream_read_range(js_get(ctx, constraints, "channelCount"), &range);
  if (!mediastream_in_range(&range, 1))
    return "channelCount";
  for (unsigned i = 0; i < sizeof processing / sizeof *processing; i++) {
    js_value v = js_get(ctx, constraints, processing[i]);
    if (v.type == JV_OBJ && js_to_boolean(js_get(ctx, v, "exact")))
      return processing[i];
  }
  return 0;
}

static bool mic_open(const struct ms_pick *pick)
{
  (void)pick;
  if (!running && !begin())
    return false;
  running = true;
  return true;
}

static void mic_close(void)
{
  if (running)
    writefile(CAPTURE_PATH, "stop", 4);
  running = false;
}

static void mic_settings(js_object *o, const struct ms_pick *pick)
{
  (void)pick;
  js_def(o, "deviceId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "groupId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "sampleRate", js_number(RATE), P_ENUMERABLE);
  js_def(o, "sampleSize", js_number(16), P_ENUMERABLE);
  js_def(o, "channelCount", js_number(1), P_ENUMERABLE);
  js_def(o, "echoCancellation", js_bool(0), P_ENUMERABLE);
  js_def(o, "noiseSuppression", js_bool(0), P_ENUMERABLE);
  js_def(o, "autoGainControl", js_bool(0), P_ENUMERABLE);
}

static void mic_capabilities(js_object *o)
{
  static const char *const processing[] = {"echoCancellation", "noiseSuppression", "autoGainControl"};
  js_def(o, "deviceId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "groupId", js_mk_cstring(ctx, device_id), P_ENUMERABLE);
  js_def(o, "sampleRate", mediastream_range(RATE, RATE), P_ENUMERABLE);
  js_def(o, "sampleSize", mediastream_range(16, 16), P_ENUMERABLE);
  js_def(o, "channelCount", mediastream_range(1, 1), P_ENUMERABLE);
  for (unsigned i = 0; i < sizeof processing / sizeof *processing; i++) {
    js_object *only_off = (js_object *)gc_keep(js_new_array(ctx, 0));
    js_array_push(ctx, only_off, js_bool(0));
    js_def(o, processing[i], js_obj_value(only_off), P_ENUMERABLE);
  }
}

static void mic_init(js_ctx *c, const char *page_url)
{
  ctx = c;
  running = false;
  rpos = 0;
  last_poll = msec();
  last_handle = 0;
  memset(subs, 0, sizeof subs);
  mediastream_device_id(page_url, "mic0", device_id);
}

static void mic_teardown(void)
{
  mic_close();
  memset(subs, 0, sizeof subs);
}

const struct ms_device *microphone_device(void)
{
  static const struct ms_device d = {"audio",     "audioinput", PERM_MICROPHONE,  mic_init, mic_present,
                                     mic_label,   mic_id,       mic_choose,       mic_open, mic_close,
                                     mic_running, mic_settings, mic_capabilities, mic_tick, mic_teardown};
  return &d;
}
