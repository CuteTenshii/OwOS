/* airplay.c: AirPlay 1 video sender. See airplay.h. */
#include "airplay.h"
#include "mdns.h"
#include "netconn.h"
#include "owos.h"
#include "plist.h"
#include "uuid.h"

#define AIRPLAY_PORT 7000
#define POLL_MS 1000
#define RESP_MAX 8192

static inline int xchg32(volatile int *p, int v) { return __sync_lock_test_and_set(p, v); }

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

/* ---- discovery ---- */

static void txt_get(const char *txt, const char *key, char *out, int cap)
{
  int klen = strlen(key);
  const char *p = txt;
  out[0] = '\0';
  for (int guard = 0; guard < 48 && *p; guard++) {
    int elen = strlen(p);
    if (starts_ci(p, key) && p[klen] == '=') {
      cpstr(out, cap, p + klen + 1);
      return;
    }
    p += elen + 1;
  }
}

/* The AirPlay "features" TXT is hex ("0x5A7FFFF7,0x1E"); bit 0 is video. A missing value counts as capable. */
static int supports_video(const char *txt)
{
  char f[48];
  unsigned v = 0;
  const char *p;
  txt_get(txt, "features", f, sizeof f);
  if (!f[0])
    return 1;
  p = f;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    p += 2;
  for (; *p && *p != ','; p++) {
    int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
    if (d < 0)
      break;
    v = v * 16 + (unsigned)d;
  }
  return (v & 1) != 0;
}

int airplay_discover(struct airplay_device *out, int max, int timeout_ms)
{
  struct mdns_service *svc;
  int n, kept = 0;
  if (max <= 0)
    return 0;
  svc = (struct mdns_service *)malloc(sizeof *svc * max);
  if (!svc)
    return -1;
  memset(svc, 0, sizeof *svc * max);
  n = mdns_browse("_airplay._tcp.local", svc, max, timeout_ms);
  for (int i = 0; i < n && kept < max; i++) {
    struct airplay_device *d = &out[kept];
    if (!(svc[i].ip[0] || svc[i].ip[1] || svc[i].ip[2] || svc[i].ip[3]) || !supports_video(svc[i].txt))
      continue;
    memset(d, 0, sizeof *d);
    txt_get(svc[i].txt, "deviceid", d->id, sizeof d->id);
    txt_get(svc[i].txt, "model", d->model, sizeof d->model);
    cpstr(d->name, sizeof d->name, svc[i].instance);
    memcpy(d->ip, svc[i].ip, 4);
    d->port = svc[i].port ? svc[i].port : AIRPLAY_PORT;
    kept++;
  }
  free(svc);
  return n < 0 ? n : kept;
}

#define SCAN_MAX 16
static volatile int g_scan_lock;
static volatile int g_scan_started;
static volatile unsigned g_scan_gen;
static int g_scan_count;
static struct airplay_device g_scan[SCAN_MAX];

static void scan_lock(void)
{
  while (xchg32(&g_scan_lock, 1))
    msleep(0);
}

static void scanner_thread(void)
{
  struct airplay_device found[SCAN_MAX];
  for (;;) {
    int n = airplay_discover(found, SCAN_MAX, 2000);
    if (n < 0)
      n = 0;
    scan_lock();
    if (n != g_scan_count || memcmp(found, g_scan, sizeof(struct airplay_device) * (unsigned)n) != 0) {
      memcpy(g_scan, found, sizeof(struct airplay_device) * (unsigned)n);
      g_scan_count = n;
      g_scan_gen++;
    }
    g_scan_lock = 0;
    msleep(8000);
  }
}

void airplay_scan_start(void)
{
  void *stack;
  scan_lock();
  if (g_scan_started) {
    g_scan_lock = 0;
    return;
  }
  g_scan_started = 1;
  g_scan_lock = 0;
  stack = thread_stack_alloc(64 * 1024);
  if (!stack || thread_create(scanner_thread, stack) < 0) {
    if (stack)
      thread_stack_free(stack);
    g_scan_started = 0;
  }
}

unsigned airplay_scan_gen(void) { return g_scan_gen; }

int airplay_scan_count(void)
{
  int n;
  scan_lock();
  n = g_scan_count;
  g_scan_lock = 0;
  return n;
}

int airplay_scan_get(int i, struct airplay_device *out)
{
  int ok = 0;
  scan_lock();
  if (i >= 0 && i < g_scan_count) {
    *out = g_scan[i];
    ok = 1;
  }
  g_scan_lock = 0;
  return ok;
}

/* ---- session ---- */

struct airplay_session {
  netconn *nc;
  char host[16];
  int port;
  char sid[UUID_STR_LEN]; /* X-Apple-Session-ID */
  volatile int lock;
  volatile int quit;
  volatile int conn;
  volatile int state;
  volatile int cur_ms, dur_ms;
  volatile int loaded;  /* a /play was accepted */
  volatile int seen_playing;
  int tid;
  void *stack;
};

static void lock_s(airplay_session *s)
{
  while (xchg32(&s->lock, 1))
    msleep(0);
}
static void unlock_s(airplay_session *s)
{
  __asm__ volatile("" ::: "memory");
  s->lock = 0;
}

static int write_all(netconn *nc, const char *p, int n)
{
  for (int off = 0; off < n;) {
    int w = nc_write(nc, p + off, n - off);
    if (w <= 0)
      return -1;
    off += w;
  }
  return 0;
}

/* One request/response on the open connection; the body lands NUL-terminated in resp. Status code or -1. */
static int http_once(airplay_session *s, const char *method, const char *path, const char *ctype, const char *body,
                     char *resp, int cap)
{
  char req[1024], hdr[RESP_MAX];
  int blen = body ? (int)strlen(body) : 0, n, hlen = 0, status, clen = -1, got, end = -1;
  n = snprintf(req, sizeof req,
               "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: MediaControl/1.0\r\nX-Apple-Session-ID: %s\r\n"
               "Content-Length: %d\r\n%s%s%s\r\n",
               method, path, s->host, s->port, s->sid, blen, ctype ? "Content-Type: " : "", ctype ? ctype : "",
               ctype ? "\r\n" : "");
  if (n <= 0 || n >= (int)sizeof req || write_all(s->nc, req, n) || (blen && write_all(s->nc, body, blen)))
    return -1;

  while (hlen < (int)sizeof hdr - 1 && end < 0) {
    int r = nc_read(s->nc, hdr + hlen, (int)sizeof hdr - 1 - hlen);
    if (r <= 0)
      return -1;
    hlen += r;
    hdr[hlen] = '\0';
    for (int i = 0; i + 3 < hlen; i++)
      if (hdr[i] == '\r' && hdr[i + 1] == '\n' && hdr[i + 2] == '\r' && hdr[i + 3] == '\n') {
        end = i + 4;
        break;
      }
  }
  if (end < 0 || !starts_ci(hdr, "HTTP/1."))
    return -1;
  status = 0;
  for (const char *p = hdr + 9; *p >= '0' && *p <= '9'; p++)
    status = status * 10 + (*p - '0');
  for (int i = 0; i < end; i++)
    if ((i == 0 || hdr[i - 1] == '\n') && starts_ci(hdr + i, "Content-Length:")) {
      clen = 0;
      for (const char *p = hdr + i + 15; *p != '\r' && *p; p++)
        if (*p >= '0' && *p <= '9')
          clen = clen * 10 + (*p - '0');
    }
  got = hlen - end;
  if (clen < 0)
    clen = got; /* no length: whatever has arrived */
  if (resp && cap > 0) {
    int take = got < cap - 1 ? got : cap - 1;
    memcpy(resp, hdr + end, (unsigned)take);
    resp[take] = '\0';
    while (got < clen && take < cap - 1) {
      int r = nc_read(s->nc, resp + take, cap - 1 - take > clen - got ? clen - got : cap - 1 - take);
      if (r <= 0)
        return -1;
      take += r;
      got += r;
      resp[take] = '\0';
    }
  }
  for (char sink[256]; got < clen;) { /* drain what did not fit so the next response starts clean */
    int r = nc_read(s->nc, sink, clen - got > (int)sizeof sink ? (int)sizeof sink : clen - got);
    if (r <= 0)
      return -1;
    got += r;
  }
  return status;
}

/* http_once with one reconnect when the receiver dropped an idle connection */
static int http(airplay_session *s, const char *method, const char *path, const char *ctype, const char *body,
                char *resp, int cap)
{
  int st;
  lock_s(s);
  st = s->nc ? http_once(s, method, path, ctype, body, resp, cap) : -1;
  if (st < 0) {
    if (s->nc)
      nc_close(s->nc);
    s->nc = nc_open(s->host, s->port, 0, 0);
    st = s->nc ? http_once(s, method, path, ctype, body, resp, cap) : -1;
  }
  unlock_s(s);
  return st;
}

static void poll_status(airplay_session *s)
{
  static char body[RESP_MAX];
  double dur = 0, pos = 0, rate = 0;
  plist *info;
  int st = http(s, "GET", "/playback-info", 0, 0, body, sizeof body);
  if (st < 0) {
    s->conn = AIRPLAY_DISCONNECTED;
    return;
  }
  info = st == 200 ? plist_parse(body, (int)strlen(body), 0) : 0;
  if (!info || !plist_get_number(plist_dict_get(info, "duration"), &dur)) {
    s->state = s->seen_playing ? AIRPLAY_IDLE : (s->loaded ? AIRPLAY_BUFFERING : AIRPLAY_IDLE);
    plist_free(info);
    return;
  }
  plist_get_number(plist_dict_get(info, "position"), &pos);
  plist_get_number(plist_dict_get(info, "rate"), &rate);
  plist_free(info);
  s->dur_ms = (int)(dur * 1000.0);
  s->cur_ms = (int)(pos * 1000.0);
  if (rate > 0) {
    s->state = AIRPLAY_PLAYING;
    s->seen_playing = 1;
  } else {
    s->state = AIRPLAY_PAUSED;
  }
}

static void poll_thread(void *arg)
{
  airplay_session *s = arg;
  while (!s->quit) {
    for (int slept = 0; slept < POLL_MS && !s->quit; slept += 50)
      msleep(50);
    if (!s->quit && s->loaded)
      poll_status(s);
  }
  exit(0);
}

airplay_session *airplay_connect(const struct airplay_device *dev, int timeout_ms)
{
  airplay_session *s;
  char info[512];
  uuid_t id;
  (void)timeout_ms;
  if (!dev)
    return 0;
  s = (airplay_session *)malloc(sizeof *s);
  if (!s)
    return 0;
  memset(s, 0, sizeof *s);
  snprintf(s->host, sizeof s->host, "%u.%u.%u.%u", dev->ip[0], dev->ip[1], dev->ip[2], dev->ip[3]);
  s->port = dev->port ? dev->port : AIRPLAY_PORT;
  uuid_generate_v4(id);
  uuid_to_string(id, s->sid, 1);
  s->nc = nc_open(s->host, s->port, 0, 0);
  if (!s->nc || http(s, "GET", "/server-info", 0, 0, info, sizeof info) != 200) {
    if (s->nc)
      nc_close(s->nc);
    free(s);
    return 0;
  }
  s->conn = AIRPLAY_CONNECTED;
  s->stack = thread_stack_alloc(64 * 1024);
  s->tid = s->stack ? thread_create_arg(poll_thread, s, s->stack) : -1;
  if (s->tid < 0) {
    if (s->stack)
      thread_stack_free(s->stack);
    nc_close(s->nc);
    free(s);
    return 0;
  }
  return s;
}

/* "12.345" without float formatting */
static void put_secs(char *out, int cap, double v)
{
  int whole, milli;
  if (v < 0)
    v = 0;
  whole = (int)v;
  milli = (int)((v - whole) * 1000.0);
  snprintf(out, cap, "%d.%03d", whole, milli);
}

int airplay_load(airplay_session *s, const char *url, double start)
{
  char body[1280];
  if (!s || s->conn != AIRPLAY_CONNECTED || !url || strlen(url) > 1024)
    return -1;
  snprintf(body, sizeof body, "Content-Location: %s\nStart-Position: 0.0\n", url);
  /* Start-Position is a fraction of the unknown duration, so a nonzero start seeks afterwards */
  if (http(s, "POST", "/play", "text/parameters", body, 0, 0) != 200)
    return -1;
  s->loaded = 1;
  s->seen_playing = 0;
  s->state = AIRPLAY_BUFFERING;
  s->cur_ms = s->dur_ms = 0;
  if (start > 0.5) {
    for (int i = 0; i < 40 && s->dur_ms == 0; i++) { /* wait for the duration before seeking */
      msleep(250);
      poll_status(s);
    }
    airplay_seek(s, start);
  }
  return 0;
}

static int rate(airplay_session *s, int on)
{
  if (!s || s->conn != AIRPLAY_CONNECTED)
    return -1;
  if (http(s, "POST", on ? "/rate?value=1.000000" : "/rate?value=0.000000", 0, 0, 0, 0) != 200)
    return -1;
  s->state = on ? AIRPLAY_PLAYING : AIRPLAY_PAUSED;
  return 0;
}

int airplay_play(airplay_session *s) { return rate(s, 1); }
int airplay_pause(airplay_session *s) { return rate(s, 0); }

int airplay_seek(airplay_session *s, double seconds)
{
  char path[64], pos[24];
  if (!s || s->conn != AIRPLAY_CONNECTED)
    return -1;
  put_secs(pos, sizeof pos, seconds);
  snprintf(path, sizeof path, "/scrub?position=%s", pos);
  if (http(s, "POST", path, 0, 0, 0, 0) != 200)
    return -1;
  s->cur_ms = (int)(seconds * 1000.0);
  return 0;
}

int airplay_stop(airplay_session *s)
{
  if (!s || s->conn != AIRPLAY_CONNECTED)
    return -1;
  s->loaded = 0;
  s->state = AIRPLAY_IDLE;
  return http(s, "POST", "/stop", 0, 0, 0, 0) == 200 ? 0 : -1;
}

void airplay_get_status(airplay_session *s, struct airplay_status *out)
{
  memset(out, 0, sizeof *out);
  if (!s)
    return;
  out->conn = s->conn;
  out->state = s->state;
  out->current_time = (double)s->cur_ms / 1000.0;
  out->duration = (double)s->dur_ms / 1000.0;
}

void airplay_close(airplay_session *s)
{
  if (!s)
    return;
  if (s->loaded)
    airplay_stop(s);
  s->quit = 1;
  thread_join(s->tid);
  thread_stack_free(s->stack);
  if (s->nc)
    nc_close(s->nc);
  free(s);
}
