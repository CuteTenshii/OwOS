/* cast.c — Google Cast sender protocol over user-space TLS. See cast.h.
 *
 * Wire model: a Cast channel is a TLS stream of frames, each a 4-byte
 * big-endian length followed by a serialized CastMessage protobuf. A
 * CastMessage carries a namespace and a UTF-8 JSON payload; the protocol lives
 * entirely in those JSON messages across four namespaces (connection,
 * heartbeat, receiver, media). We build/parse frames with the general protobuf
 * codec and the payloads with the general JSON tree.
 *
 * Threading: a session owns a reader thread (blocking frame reads → status
 * updates) and a pinger thread. The pinger exists because the kernel's blocking
 * TCP recv self-closes a socket after ~5 s of silence (net.c), and a receiver's
 * own heartbeat is on that same 5 s edge — so we elicit inbound traffic every
 * 2 s to keep the reader's blocking read from ever idling out. All socket
 * writes (control calls on the caller thread, PONGs on the reader thread)
 * serialize on one spinlock; reads are unlocked (TLS rx/tx state is disjoint).
 * Published status is plain fields the consumer polls.
 */
#include "cast.h"
#include "json.h"
#include "mdns.h"
#include "netconn.h"
#include "owos.h"
#include "protobuf.h"

/* ---- Cast namespaces + the Default Media Receiver app id ------------------
 */
#define NS_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_RECEIVER "urn:x-cast:com.google.cast.receiver"
#define NS_MEDIA "urn:x-cast:com.google.cast.media"
#define DEFAULT_APP "CC1AD845" /* the styled Default Media Receiver */
#define SENDER_ID "sender-0"
#define PLATFORM_ID "receiver-0"
#define CAST_PORT 8009
#define FRAME_MAX (128 * 1024) /* sanity cap on an inbound frame    */

/* ---- small spinlock (mirrors user malloc / media.c) -----------------------
 */
static inline int xchg32(volatile int *p, int v)
{
  __asm__ volatile("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
  return v;
}

/* ---- session --------------------------------------------------------------
 */
struct cast_session {
  netconn *nc;
  volatile int wlock; /* serializes socket writes                */

  volatile int quit;
  volatile int conn;  /* CAST_DISCONNECTED/CONNECTING/CONNECTED  */
  volatile int state; /* CAST_IDLE/BUFFERING/PLAYING/PAUSED      */

  volatile int have_transport;
  char transport_id[64];   /* the launched app's session transport    */
  char app_session_id[96]; /* RECEIVER_STATUS applications[].sessionId */

  volatile int have_media;
  volatile int media_session_id;

  /* published playback status (integers to stay volatile-simple) */
  volatile int cur_ms;  /* position, milliseconds                  */
  volatile int dur_ms;  /* duration, milliseconds (0 unknown)      */
  volatile int vol1000; /* receiver volume * 1000                  */
  volatile int muted;

  int req_id; /* next requestId (write-lock held)        */

  int reader_tid, pinger_tid;
  void *reader_stack, *pinger_stack;
};

static void lock_w(cast_session *s)
{
  while (xchg32(&s->wlock, 1))
    msleep(0);
}
static void unlock_w(cast_session *s)
{
  __asm__ volatile("" ::: "memory");
  s->wlock = 0;
}

/* ---- bounded string copy --------------------------------------------------
 */
static void cpstr(char *dst, int cap, const char *src)
{
  int i = 0;
  if (cap <= 0)
    return;
  if (src)
    for (; src[i] && i < cap - 1; i++)
      dst[i] = src[i];
  dst[i] = '\0';
}

/* ======================================================================== */
/* Frame I/O                                                                 */
/* ======================================================================== */

static int write_all(netconn *nc, const uint8_t *p, int n)
{
  int off = 0;
  while (off < n) {
    int w = nc_write(nc, p + off, n - off);
    if (w <= 0)
      return -1;
    off += w;
  }
  return 0;
}

static int read_all(netconn *nc, uint8_t *p, int n)
{
  int off = 0;
  while (off < n) {
    int r = nc_read(nc, p + off, n - off);
    if (r <= 0)
      return -1; /* EOF / error / idle-timeout close */
    off += r;
  }
  return 0;
}

/* Serialize a CastMessage carrying a STRING payload and frame it on the wire.
 * `payload` JSON is consumed (freed here). Returns 0 / -1. */
static int send_msg(cast_session *s, const char *ns, const char *dest, json_value *payload)
{
  char *pj = 0;
  pb_writer w;
  uint8_t *body, *frame;
  uint32_t blen;
  int rc = -1;

  if (!s || !s->nc || !payload) {
    if (payload)
      json_free(payload);
    return -1;
  }
  pj = json_encode(payload, 0, 0);
  json_free(payload);
  if (!pj)
    return -1;

  pb_writer_init(&w);
  pb_write_varint(&w, 1, 0);         /* protocol_version = CASTV2_1_0   */
  pb_write_string(&w, 2, SENDER_ID); /* source_id                       */
  pb_write_string(&w, 3, dest);      /* destination_id                  */
  pb_write_string(&w, 4, ns);        /* namespace                       */
  pb_write_varint(&w, 5, 0);         /* payload_type = STRING           */
  pb_write_string(&w, 6, pj);        /* payload_utf8                    */
  free(pj);
  if (w.error) {
    pb_writer_free(&w);
    return -1;
  }

  body = pb_writer_take(&w, &blen);
  if (!body) {
    pb_writer_free(&w);
    return -1;
  }

  frame = (uint8_t *)malloc(blen + 4);
  if (!frame) {
    free(body);
    return -1;
  }
  frame[0] = (uint8_t)(blen >> 24);
  frame[1] = (uint8_t)(blen >> 16);
  frame[2] = (uint8_t)(blen >> 8);
  frame[3] = (uint8_t)blen;
  memcpy(frame + 4, body, blen);
  free(body);

  lock_w(s);
  rc = write_all(s->nc, frame, (int)(blen + 4));
  unlock_w(s);
  free(frame);
  return rc;
}

/* A payload {"type":TYPE} with an optional requestId. Caller may extend it. */
static json_value *msg_typed(const char *type)
{
  json_value *o = json_new_object();
  if (!o)
    return 0;
  if (json_object_put(o, "type", json_new_string(type))) {
    json_free(o);
    return 0;
  }
  return o;
}

static int next_req(cast_session *s)
{
  int r;
  lock_w(s);
  r = ++s->req_id;
  unlock_w(s);
  return r;
}

/* Read one framed CastMessage; hand back its namespace, source and JSON payload
 * (all NUL-terminated copies the caller frees). Returns 0 / -1. */
static int recv_msg(cast_session *s, char *ns, int ns_cap, char **payload_out)
{
  uint8_t hdr[4];
  uint32_t len;
  uint8_t *buf;
  pb_reader r;

  *payload_out = 0;
  if (ns_cap > 0)
    ns[0] = '\0';
  if (read_all(s->nc, hdr, 4))
    return -1;
  len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
  if (len == 0 || len > FRAME_MAX)
    return -1;
  buf = (uint8_t *)malloc(len);
  if (!buf)
    return -1;
  if (read_all(s->nc, buf, (int)len)) {
    free(buf);
    return -1;
  }

  pb_reader_init(&r, buf, len);
  for (;;) {
    uint32_t field;
    int wt;
    int t = pb_read_tag(&r, &field, &wt);
    if (t <= 0)
      break;
    if (wt == PB_LEN && (field == 4 || field == 6)) {
      const uint8_t *p;
      uint32_t n;
      if (pb_read_bytes(&r, &p, &n) != 1)
        break;
      if (field == 4 && ns_cap > 0) { /* namespace */
        uint32_t i, m = n < (uint32_t)ns_cap - 1 ? n : (uint32_t)ns_cap - 1;
        for (i = 0; i < m; i++)
          ns[i] = (char)p[i];
        ns[m] = '\0';
      } else if (field == 6) { /* payload_utf8 */
        char *pl = (char *)malloc(n + 1);
        if (pl) {
          memcpy(pl, p, n);
          pl[n] = '\0';
          free(*payload_out);
          *payload_out = pl;
        }
      }
    } else {
      if (pb_skip(&r, wt) != 1)
        break;
    }
  }
  free(buf);
  return 0;
}

/* ======================================================================== */
/* Inbound message handling (reader thread)                                  */
/* ======================================================================== */

static int player_state_of(const char *s)
{
  if (!s)
    return CAST_IDLE;
  if (streq(s, "PLAYING"))
    return CAST_PLAYING;
  if (streq(s, "PAUSED"))
    return CAST_PAUSED;
  if (streq(s, "BUFFERING"))
    return CAST_BUFFERING;
  return CAST_IDLE;
}

static void apply_volume(cast_session *s, json_value *vol)
{
  json_value *lv, *mv;
  if (!vol || json_typeof(vol) != JSON_OBJECT)
    return;
  lv = json_object_get(vol, "level");
  mv = json_object_get(vol, "muted");
  if (lv && json_typeof(lv) == JSON_NUMBER) {
    int v = (int)(json_as_number(lv, 1.0) * 1000.0 + 0.5);
    if (v < 0)
      v = 0;
    if (v > 1000)
      v = 1000;
    s->vol1000 = v;
  }
  if (mv)
    s->muted = json_as_bool(mv, s->muted);
}

static void handle_receiver_status(cast_session *s, json_value *root)
{
  json_value *st = json_object_get(root, "status");
  json_value *apps, *app;
  if (!st || json_typeof(st) != JSON_OBJECT)
    return;
  apply_volume(s, json_object_get(st, "volume"));
  apps = json_object_get(st, "applications");
  if (!apps || json_typeof(apps) != JSON_ARRAY)
    return;
  /* Take the first application (the receiver runs one foreground app). */
  app = json_array_get(apps, 0);
  if (!app || json_typeof(app) != JSON_OBJECT)
    return;
  {
    const char *tid = json_as_string(json_object_get(app, "transportId"));
    const char *sid = json_as_string(json_object_get(app, "sessionId"));
    if (sid)
      cpstr(s->app_session_id, sizeof s->app_session_id, sid);
    if (tid && tid[0]) {
      cpstr(s->transport_id, sizeof s->transport_id, tid);
      __asm__ volatile("" ::: "memory");
      s->have_transport = 1;
    }
  }
}

static void handle_media_status(cast_session *s, json_value *root)
{
  json_value *arr = json_object_get(root, "status");
  json_value *item, *media, *dur;
  if (!arr || json_typeof(arr) != JSON_ARRAY)
    return;
  item = json_array_get(arr, 0);
  if (!item || json_typeof(item) != JSON_OBJECT)
    return;
  {
    json_value *msid = json_object_get(item, "mediaSessionId");
    if (msid && json_typeof(msid) == JSON_NUMBER) {
      s->media_session_id = (int)json_as_number(msid, 0);
      __asm__ volatile("" ::: "memory");
      s->have_media = 1;
    }
  }
  s->state = player_state_of(json_as_string(json_object_get(item, "playerState")));
  {
    json_value *ct = json_object_get(item, "currentTime");
    if (ct && json_typeof(ct) == JSON_NUMBER)
      s->cur_ms = (int)(json_as_number(ct, 0) * 1000.0);
  }
  apply_volume(s, json_object_get(item, "volume"));
  media = json_object_get(item, "media");
  if (media && json_typeof(media) == JSON_OBJECT) {
    dur = json_object_get(media, "duration");
    if (dur && json_typeof(dur) == JSON_NUMBER)
      s->dur_ms = (int)(json_as_number(dur, 0) * 1000.0);
  }
}

static void handle_message(cast_session *s, const char *ns, char *payload)
{
  json_value *root;
  const char *type;
  if (!payload)
    return;
  root = json_parse(payload, -1, 0);
  if (!root)
    return;
  type = json_as_string(json_object_get(root, "type"));
  if (type) {
    if (streq(type, "PING")) {
      send_msg(s, NS_HEARTBEAT, PLATFORM_ID, msg_typed("PONG"));
    } else if (streq(type, "RECEIVER_STATUS")) {
      handle_receiver_status(s, root);
    } else if (streq(type, "MEDIA_STATUS")) {
      handle_media_status(s, root);
    } else if (streq(type, "CLOSE")) {
      s->conn = CAST_DISCONNECTED;
    }
  }
  json_free(root);
}

/* ---- thread startup handshake (thread_create passes no argument) ----------
 */
static volatile int g_spawn_lock;
static cast_session *volatile g_spawning;
static volatile int g_spawn_taken;

static cast_session *grab_session(void)
{
  cast_session *s = g_spawning;
  __asm__ volatile("" ::: "memory");
  g_spawn_taken = 1;
  return s;
}

static void reader_thread(void)
{
  cast_session *s = grab_session();
  char ns[96];
  for (;;) {
    char *payload;
    if (s->quit)
      exit(0);
    if (recv_msg(s, ns, sizeof ns, &payload)) {
      s->conn = CAST_DISCONNECTED; /* peer closed / idle timeout */
      exit(0);
    }
    handle_message(s, ns, payload);
    free(payload);
  }
}

static void pinger_thread(void)
{
  cast_session *s = grab_session();
  for (;;) {
    if (s->quit)
      exit(0);
    /* Elicit inbound traffic well under the kernel's ~5 s recv idle close. */
    send_msg(s, NS_HEARTBEAT, PLATFORM_ID, msg_typed("PING"));
    msleep(2000);
  }
}

static int start_thread(cast_session *s, void (*fn)(void), int *tid_out, void **stack_out)
{
  void *st = thread_stack_alloc(64 * 1024);
  int tid;
  if (!st)
    return -1;
  while (xchg32(&g_spawn_lock, 1))
    msleep(0);
  g_spawning = s;
  g_spawn_taken = 0;
  __asm__ volatile("" ::: "memory");
  tid = thread_create(fn, st);
  if (tid < 0) {
    g_spawn_lock = 0;
    thread_stack_free(st);
    return -1;
  }
  while (!g_spawn_taken)
    msleep(0); /* wait until the thread copied s */
  __asm__ volatile("" ::: "memory");
  g_spawn_lock = 0;
  *tid_out = tid;
  *stack_out = st;
  return 0;
}

/* ======================================================================== */
/* Discovery                                                                 */
/* ======================================================================== */

/* Parse the NUL-joined DNS-SD TXT record for a `key=` prefix into `out`. */
static void txt_get(const char *txt, const char *key, char *out, int cap)
{
  int klen = strlen(key);
  const char *p = txt;
  out[0] = '\0';
  /* The caller zero-initializes the buffer, so a trailing empty entry ends
   * the walk even without an explicit length. */
  for (int guard = 0; guard < 32 && *p; guard++) {
    int elen = strlen(p);
    if (starts_ci(p, key) && p[klen] == '=') {
      cpstr(out, cap, p + klen + 1);
      return;
    }
    p += elen + 1;
  }
}

static void device_from_service(struct cast_device *d, const struct mdns_service *m)
{
  memset(d, 0, sizeof *d);
  txt_get(m->txt, "id", d->id, sizeof d->id);
  txt_get(m->txt, "fn", d->name, sizeof d->name);
  txt_get(m->txt, "md", d->model, sizeof d->model);
  if (!d->name[0])
    cpstr(d->name, sizeof d->name, m->instance);
  d->ip[0] = m->ip[0];
  d->ip[1] = m->ip[1];
  d->ip[2] = m->ip[2];
  d->ip[3] = m->ip[3];
  d->port = m->port ? m->port : CAST_PORT;
}

int cast_discover(struct cast_device *out, int max, int timeout_ms)
{
  struct mdns_service *svc;
  int n, i, kept = 0;
  if (max <= 0)
    return 0;
  svc = (struct mdns_service *)malloc(sizeof *svc * max);
  if (!svc)
    return -1;
  memset(svc, 0, sizeof *svc * max);
  n = mdns_browse("_googlecast._tcp.local", svc, max, timeout_ms);
  if (n < 0) {
    free(svc);
    return n;
  }
  for (i = 0; i < n && kept < max; i++) {
    /* A usable entry needs an address; skip half-resolved records. */
    if (!(svc[i].ip[0] || svc[i].ip[1] || svc[i].ip[2] || svc[i].ip[3]))
      continue;
    device_from_service(&out[kept++], &svc[i]);
  }
  free(svc);
  return kept;
}

/* ---- background scanner ---------------------------------------------------
 */
#define SCAN_MAX 16
static volatile int g_scan_lock;
static volatile int g_scan_started;
static volatile unsigned g_scan_gen;
static int g_scan_count;
static struct cast_device g_scan[SCAN_MAX];
static void *g_scan_stack;

static int devices_differ(const struct cast_device *a, int an, const struct cast_device *b, int bn)
{
  int i;
  if (an != bn)
    return 1;
  for (i = 0; i < an; i++)
    if (!streq(a[i].id, b[i].id) || a[i].ip[0] != b[i].ip[0] || a[i].ip[1] != b[i].ip[1] || a[i].ip[2] != b[i].ip[2] ||
        a[i].ip[3] != b[i].ip[3])
      return 1;
  return 0;
}

static void scanner_thread(void)
{
  struct cast_device found[SCAN_MAX];
  for (;;) {
    int n = cast_discover(found, SCAN_MAX, 2000);
    if (n < 0)
      n = 0;
    while (xchg32(&g_scan_lock, 1))
      msleep(0);
    if (devices_differ(found, n, g_scan, g_scan_count)) {
      memcpy(g_scan, found, sizeof(struct cast_device) * (n < SCAN_MAX ? n : SCAN_MAX));
      g_scan_count = n;
      g_scan_gen++;
    }
    g_scan_lock = 0;
    msleep(8000);
  }
}

void cast_scan_start(void)
{
  int tid;
  while (xchg32(&g_scan_lock, 1))
    msleep(0);
  if (g_scan_started) {
    g_scan_lock = 0;
    return;
  }
  g_scan_started = 1;
  g_scan_lock = 0;
  g_scan_stack = thread_stack_alloc(64 * 1024);
  if (!g_scan_stack) {
    g_scan_started = 0;
    return;
  }
  tid = thread_create(scanner_thread, g_scan_stack);
  if (tid < 0) {
    thread_stack_free(g_scan_stack);
    g_scan_stack = 0;
    g_scan_started = 0;
  }
}

unsigned cast_scan_gen(void) { return g_scan_gen; }

int cast_scan_count(void)
{
  int n;
  while (xchg32(&g_scan_lock, 1))
    msleep(0);
  n = g_scan_count;
  g_scan_lock = 0;
  return n;
}

int cast_scan_get(int i, struct cast_device *out)
{
  int ok = 0;
  while (xchg32(&g_scan_lock, 1))
    msleep(0);
  if (i >= 0 && i < g_scan_count) {
    *out = g_scan[i];
    ok = 1;
  }
  g_scan_lock = 0;
  return ok;
}

/* ======================================================================== */
/* Session                                                                   */
/* ======================================================================== */

static int wait_flag(volatile int *flag, int deadline_ms)
{
  while (!*flag) {
    if ((int)(deadline_ms - (int)now_ms()) <= 0)
      return 0;
    msleep(20);
  }
  return 1;
}

cast_session *cast_connect(const struct cast_device *dev, int timeout_ms)
{
  cast_session *s;
  char host[16];
  int deadline;

  if (!dev)
    return 0;
  s = (cast_session *)malloc(sizeof *s);
  if (!s)
    return 0;
  memset(s, 0, sizeof *s);
  s->conn = CAST_CONNECTING;
  s->vol1000 = 1000;

  snprintf(host, sizeof host, "%u.%u.%u.%u", dev->ip[0], dev->ip[1], dev->ip[2], dev->ip[3]);
  s->nc = nc_open(host, dev->port ? dev->port : CAST_PORT, 1 /*TLS*/, 0);
  if (!s->nc) {
    free(s);
    return 0;
  }

  /* Virtual connection to the platform receiver, then launch the app. */
  if (send_msg(s, NS_CONNECTION, PLATFORM_ID, msg_typed("CONNECT")))
    goto fail;
  {
    json_value *o = msg_typed("LAUNCH");
    if (!o)
      goto fail;
    json_object_put(o, "appId", json_new_string(DEFAULT_APP));
    json_object_put(o, "requestId", json_new_number(1));
    if (send_msg(s, NS_RECEIVER, PLATFORM_ID, o))
      goto fail;
  }
  s->req_id = 1;

  if (start_thread(s, reader_thread, &s->reader_tid, &s->reader_stack))
    goto fail;
  if (start_thread(s, pinger_thread, &s->pinger_tid, &s->pinger_stack))
    goto fail_reader;

  deadline = (int)now_ms() + (timeout_ms > 0 ? timeout_ms : 8000);
  if (!wait_flag(&s->have_transport, deadline))
    goto fail_threads;

  /* Open a virtual connection to the launched app's transport. */
  send_msg(s, NS_CONNECTION, s->transport_id, msg_typed("CONNECT"));
  s->conn = CAST_CONNECTED;
  return s;

fail_threads:
  s->quit = 1;
  thread_join(s->pinger_tid);
  thread_stack_free(s->pinger_stack);
fail_reader:
  s->quit = 1;
  thread_join(s->reader_tid);
  thread_stack_free(s->reader_stack);
fail:
  nc_close(s->nc);
  free(s);
  return 0;
}

int cast_load(cast_session *s, const char *url, const char *content_type, const char *title, int timeout_ms)
{
  json_value *o, *media, *meta;
  int deadline;
  if (!s || s->conn != CAST_CONNECTED || !url)
    return -1;

  o = msg_typed("LOAD");
  media = json_new_object();
  if (!o || !media) {
    if (o)
      json_free(o);
    if (media)
      json_free(media);
    return -1;
  }
  json_object_put(media, "contentId", json_new_string(url));
  json_object_put(media, "streamType", json_new_string("BUFFERED"));
  if (content_type && content_type[0])
    json_object_put(media, "contentType", json_new_string(content_type));
  if (title && title[0]) {
    meta = json_new_object();
    if (meta) {
      json_object_put(meta, "metadataType", json_new_number(0)); /* GENERIC */
      json_object_put(meta, "title", json_new_string(title));
      json_object_put(media, "metadata", meta);
    }
  }
  json_object_put(o, "media", media);
  json_object_put(o, "autoplay", json_new_bool(1));
  json_object_put(o, "currentTime", json_new_number(0));
  json_object_put(o, "requestId", json_new_number(next_req(s)));

  s->have_media = 0;
  if (send_msg(s, NS_MEDIA, s->transport_id, o))
    return -1;

  deadline = (int)now_ms() + (timeout_ms > 0 ? timeout_ms : 8000);
  return wait_flag(&s->have_media, deadline) ? 0 : -1;
}

/* A media-namespace control command referencing the current media session. */
static int media_cmd(cast_session *s, const char *type, json_value *extra)
{
  json_value *o;
  if (!s || s->conn != CAST_CONNECTED || !s->have_media) {
    if (extra)
      json_free(extra);
    return -1;
  }
  o = msg_typed(type);
  if (!o) {
    if (extra)
      json_free(extra);
    return -1;
  }
  json_object_put(o, "mediaSessionId", json_new_number(s->media_session_id));
  json_object_put(o, "requestId", json_new_number(next_req(s)));
  if (extra) {
    int i, n = json_object_len(extra);
    for (i = 0; i < n; i++)
      json_object_put(o, json_object_key(extra, i), json_clone(json_object_val(extra, i)));
    json_free(extra);
  }
  return send_msg(s, NS_MEDIA, s->transport_id, o);
}

int cast_play(cast_session *s) { return media_cmd(s, "PLAY", 0); }
int cast_pause(cast_session *s) { return media_cmd(s, "PAUSE", 0); }
int cast_stop(cast_session *s) { return media_cmd(s, "STOP", 0); }

int cast_seek(cast_session *s, double seconds)
{
  json_value *e = json_new_object();
  if (!e)
    return -1;
  json_object_put(e, "currentTime", json_new_number(seconds < 0 ? 0 : seconds));
  return media_cmd(s, "SEEK", e);
}

int cast_set_volume(cast_session *s, double level)
{
  json_value *o, *vol;
  if (!s || s->conn != CAST_CONNECTED)
    return -1;
  if (level < 0)
    level = 0;
  if (level > 1)
    level = 1;
  o = msg_typed("SET_VOLUME");
  vol = json_new_object();
  if (!o || !vol) {
    if (o)
      json_free(o);
    if (vol)
      json_free(vol);
    return -1;
  }
  json_object_put(vol, "level", json_new_number(level));
  json_object_put(o, "volume", vol);
  json_object_put(o, "requestId", json_new_number(next_req(s)));
  return send_msg(s, NS_RECEIVER, PLATFORM_ID, o);
}

int cast_set_muted(cast_session *s, int muted)
{
  json_value *o, *vol;
  if (!s || s->conn != CAST_CONNECTED)
    return -1;
  o = msg_typed("SET_VOLUME");
  vol = json_new_object();
  if (!o || !vol) {
    if (o)
      json_free(o);
    if (vol)
      json_free(vol);
    return -1;
  }
  json_object_put(vol, "muted", json_new_bool(muted ? 1 : 0));
  json_object_put(o, "volume", vol);
  json_object_put(o, "requestId", json_new_number(next_req(s)));
  return send_msg(s, NS_RECEIVER, PLATFORM_ID, o);
}

void cast_get_status(cast_session *s, struct cast_status *out)
{
  if (!out)
    return;
  if (!s) {
    memset(out, 0, sizeof *out);
    out->conn = CAST_DISCONNECTED;
    return;
  }
  out->conn = s->conn;
  out->state = s->state;
  out->current_time = (double)s->cur_ms / 1000.0;
  out->duration = (double)s->dur_ms / 1000.0;
  out->volume = (double)s->vol1000 / 1000.0;
  out->muted = s->muted;
}

void cast_close(cast_session *s)
{
  if (!s)
    return;
  /* Best-effort: stop the receiver app and close the virtual connections. */
  if (s->conn == CAST_CONNECTED && s->app_session_id[0]) {
    json_value *o = msg_typed("STOP");
    if (o) {
      json_object_put(o, "sessionId", json_new_string(s->app_session_id));
      json_object_put(o, "requestId", json_new_number(next_req(s)));
      send_msg(s, NS_RECEIVER, PLATFORM_ID, o);
    }
    send_msg(s, NS_CONNECTION, s->transport_id, msg_typed("CLOSE"));
  }
  send_msg(s, NS_CONNECTION, PLATFORM_ID, msg_typed("CLOSE"));

  /* Stop the pinger first; the reader then unblocks on the socket idle
   * timeout (<=~5 s) and exits. Join both before freeing the transport so
   * neither thread touches a freed netconn. */
  s->quit = 1;
  thread_join(s->pinger_tid);
  thread_stack_free(s->pinger_stack);
  thread_join(s->reader_tid);
  thread_stack_free(s->reader_stack);
  nc_close(s->nc);
  free(s);
}
