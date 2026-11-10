/* cdp.c: Chrome DevTools Protocol endpoint (see cdp.h for the model).
 *
 * The browser is the CDP *target*. An I/O thread owns one TCP listener on port
 * 9222: it answers the HTTP `/json*` discovery probes a client uses to find the
 * page, and upgrades `/devtools/page/...` to a WebSocket that carries JSON-RPC.
 * Client frames are masked; server frames are not (RFC 6455). Every protocol
 * command is parsed + dispatched on the browser's WORKER thread (cdp_worker_tick)
 * because it touches the worker-owned DOM/JS; the I/O thread only does WS framing
 * and shuttles two byte queues. Events are formatted wherever they arise and
 * enqueued: heap JSON only, no engine access, so thread-agnostic. */
#include "cdp.h"
#include "base64.h"
#include "browser.h"
#include "crypto.h"
#include "css.h" /* struct css_match/css_style: CSS.getMatchedStylesForNode */
#include "dom.h"
#include "features/storage/storage.h"
#include "json.h"
#include "layout.h" /* struct lbox / layout_pool: DOM.getBoxModel */
#include "mutex.h"

double strtod(const char *s, char **end); /* $(MATH), for a numeric eval result */

#define CDP_PORT 9222
#define PAGE_WS_PREFIX "/devtools/page/"
#define BROWSER_WS_PREFIX "/devtools/browser/"
#define CDP_WS_MAX (8u << 20) /* reject a single WS message larger than this */

/* --- sessions (one WebSocket client = one session) ------------------------- *
 * DevTools opens a browser- and a page-level socket at once, and several clients
 * (a DevTools window plus Puppeteer) may attach together, so each connection owns
 * its inbound/outbound queues and its own set of enabled domains. Command replies
 * go back to the originating session; async events (console, network, navigation)
 * broadcast to every session that enabled the relevant domain. One mutex guards the
 * whole table and all queues; critical sections are tiny (a pointer move). */
#define CDP_MAX_SESSIONS 4
#define INQ_MAX 128   /* client commands awaiting dispatch, per session */
#define OUTQ_MAX 1024 /* responses + events awaiting the wire, per session */

struct cdp_session {
  volatile int used;
  char target[33]; /* the tab it attached to; "" for the browser endpoint, which follows the active tab */
  volatile int rt_enabled, page_enabled, dom_enabled, net_enabled, log_enabled, css_enabled;
  char *inq[INQ_MAX];
  int in_head, in_count;
  char *outq[OUTQ_MAX];
  int out_head, out_count;
};
static struct cdp_session sessions[CDP_MAX_SESSIONS];
static char g_browser_id[33];
static mutex_t sess_lk = MUTEX_INIT;
static volatile int cdp_nsessions; /* # attached sessions (event hooks fast-skip on 0) */

/* Inspect-element picker arm (Overlay.setInspectMode), per-session. The main thread
 * feeds the pane offset (cdp_pick_commit); the worker resolves it to a DOM node and
 * emits Overlay.inspectNodeRequested. Only one session can pick at a time. */
static volatile int cdp_pick_sess = -1;   /* session index that armed the picker, -1 = none */
static volatile int cdp_pick_commit = -1; /* main->worker: pane offset clicked */

int cdp_connected(void) { return cdp_nsessions > 0; }
int cdp_picking(void) { return cdp_pick_sess >= 0; }
void cdp_pick_commit_offset(int off)
{
  if (cdp_pick_sess >= 0 && off >= 0)
    cdp_pick_commit = off;
}
void cdp_pick_cancel(void)
{
  cdp_pick_sess = -1;
  cdp_pick_commit = -1;
}

/* Async-event gating domains for a broadcast. */
enum { EV_RUNTIME, EV_PAGE, EV_DOM, EV_NETWORK, EV_LOG, EV_ALWAYS };
static int sess_wants(const struct cdp_session *s, int dom)
{
  switch (dom) {
  case EV_RUNTIME:
    return s->rt_enabled;
  case EV_PAGE:
    return s->page_enabled;
  case EV_DOM:
    return s->dom_enabled;
  case EV_NETWORK:
    return s->net_enabled;
  case EV_LOG:
    return s->log_enabled;
  default:
    return 1;
  }
}

static char *cdp_sdup(const char *s)
{
  if (!s)
    return 0;
  int n = (int)strlen(s);
  char *d = malloc((unsigned)n + 1);
  if (d)
    memcpy(d, s, (unsigned)n + 1);
  return d;
}

/* Queue ops. The _l variants require sess_lk held. */
static void sess_in_push_l(struct cdp_session *s, char *m)
{
  if (!m)
    return;
  if (s->in_count >= INQ_MAX) {
    free(s->inq[s->in_head]);
    s->in_head = (s->in_head + 1) % INQ_MAX;
    s->in_count--;
  }
  s->inq[(s->in_head + s->in_count) % INQ_MAX] = m;
  s->in_count++;
}
static void sess_out_push_l(struct cdp_session *s, char *m)
{
  if (!m)
    return;
  if (s->out_count >= OUTQ_MAX) {
    free(s->outq[s->out_head]);
    s->out_head = (s->out_head + 1) % OUTQ_MAX;
    s->out_count--;
  }
  s->outq[(s->out_head + s->out_count) % OUTQ_MAX] = m;
  s->out_count++;
}
static void sess_in_push(struct cdp_session *s, char *m)
{
  mutex_lock(&sess_lk);
  sess_in_push_l(s, m);
  mutex_unlock(&sess_lk);
}
static char *sess_in_pop(struct cdp_session *s)
{
  char *m = 0;
  mutex_lock(&sess_lk);
  if (s->in_count) {
    m = s->inq[s->in_head];
    s->in_head = (s->in_head + 1) % INQ_MAX;
    s->in_count--;
  }
  mutex_unlock(&sess_lk);
  return m;
}
static char *sess_out_pop(struct cdp_session *s)
{
  char *m = 0;
  mutex_lock(&sess_lk);
  if (s->out_count) {
    m = s->outq[s->out_head];
    s->out_head = (s->out_head + 1) % OUTQ_MAX;
    s->out_count--;
  }
  mutex_unlock(&sess_lk);
  return m;
}
static void sess_drain_l(struct cdp_session *s)
{
  while (s->in_count) {
    free(s->inq[s->in_head]);
    s->in_head = (s->in_head + 1) % INQ_MAX;
    s->in_count--;
  }
  while (s->out_count) {
    free(s->outq[s->out_head]);
    s->out_head = (s->out_head + 1) % OUTQ_MAX;
    s->out_count--;
  }
  s->in_head = s->out_head = 0;
}
static struct cdp_session *sess_alloc(void)
{
  struct cdp_session *s = 0;
  mutex_lock(&sess_lk);
  for (int i = 0; i < CDP_MAX_SESSIONS; i++)
    if (!sessions[i].used) {
      s = &sessions[i];
      s->in_head = s->in_count = s->out_head = s->out_count = 0;
      s->rt_enabled = s->page_enabled = s->dom_enabled = s->net_enabled = s->log_enabled = s->css_enabled = 0;
      s->target[0] = '\0';
      s->used = 1;
      cdp_nsessions++;
      break;
    }
  mutex_unlock(&sess_lk);
  return s;
}
static void sess_free(struct cdp_session *s)
{
  mutex_lock(&sess_lk);
  sess_drain_l(s);
  s->rt_enabled = s->page_enabled = s->dom_enabled = s->net_enabled = s->log_enabled = s->css_enabled = 0;
  s->used = 0;
  if (cdp_nsessions > 0)
    cdp_nsessions--;
  {
    int idx = (int)(s - sessions);
    if (cdp_pick_sess == idx)
      cdp_pick_sess = -1;
  }
  mutex_unlock(&sess_lk);
}

/* Commands and events reach a page session only while its tab is the one with a live DOM */
static int sess_on_active(const struct cdp_session *s) { return !s->target[0] || streq(s->target, B->pg->target_id); }

/* Set by a command handler (worker thread only) to answer with a CDP error instead of its result */
static const char *g_cmd_error;

/* Reply `{"id":id,"error":{"code":-32000,"message":msg}}`, CDP's generic server error */
static void reply_error(struct cdp_session *s, double id, const char *msg)
{
  json_value *msg_o = json_new_object(), *err = json_new_object();
  if (!msg_o || !err) {
    json_free(msg_o);
    json_free(err);
    return;
  }
  json_object_put(err, "code", json_new_number(-32000));
  json_object_put(err, "message", json_new_string(msg));
  json_object_put(msg_o, "id", json_new_number(id));
  json_object_put(msg_o, "error", err);
  char *str = json_encode(msg_o, 0, 0);
  json_free(msg_o);
  if (str) {
    mutex_lock(&sess_lk);
    sess_out_push_l(s, str);
    mutex_unlock(&sess_lk);
  }
}

/* Reply `{"id":id,"result":result}` to the originating session (takes result). */
static void reply_result(struct cdp_session *s, double id, json_value *result)
{
  json_value *msg = json_new_object();
  if (!msg) {
    json_free(result);
    return;
  }
  json_object_put(msg, "id", json_new_number(id));
  json_object_put(msg, "result", result ? result : json_new_object());
  char *str = json_encode(msg, 0, 0);
  json_free(msg);
  if (str) {
    mutex_lock(&sess_lk);
    sess_out_push_l(s, str);
    mutex_unlock(&sess_lk);
  }
}
/* Send a `{"method":..,"params":..}` event to ONE session (takes params). Used for
 * command-triggered events like Runtime.enable's executionContextCreated. */
static void emit_to_session(struct cdp_session *s, const char *method, json_value *params)
{
  json_value *msg = json_new_object();
  if (!msg) {
    json_free(params);
    return;
  }
  json_object_put(msg, "method", json_new_string(method));
  json_object_put(msg, "params", params ? params : json_new_object());
  char *str = json_encode(msg, 0, 0);
  json_free(msg);
  if (str) {
    mutex_lock(&sess_lk);
    sess_out_push_l(s, str);
    mutex_unlock(&sess_lk);
  }
}
/* Broadcast an async event to every session that enabled domain `dom` (takes
 * params). Encoded once; a private copy is queued per target session. */
static void broadcast_event(int dom, const char *method, json_value *params)
{
  if (!cdp_nsessions) {
    json_free(params);
    return;
  }
  json_value *msg = json_new_object();
  if (!msg) {
    json_free(params);
    return;
  }
  json_object_put(msg, "method", json_new_string(method));
  json_object_put(msg, "params", params ? params : json_new_object());
  char *str = json_encode(msg, 0, 0);
  json_free(msg);
  if (!str)
    return;
  mutex_lock(&sess_lk);
  for (int i = 0; i < CDP_MAX_SESSIONS; i++)
    if (sessions[i].used && sess_wants(&sessions[i], dom) && sess_on_active(&sessions[i]))
      sess_out_push_l(&sessions[i], cdp_sdup(str));
  mutex_unlock(&sess_lk);
  free(str);
}

/* --- WebSocket server framing ---------------------------------------------- */
static int send_all(int fd, const void *buf, int len)
{
  const char *p = (const char *)buf;
  int off = 0;
  while (off < len) {
    int w = net_send(fd, p + off, len - off);
    if (w <= 0)
      return -1;
    off += w;
  }
  return 0;
}
/* One unmasked server frame (opcode `op`, payload data[len]), FIN set. */
static int ws_send_frame(int fd, int op, const void *data, int len)
{
  unsigned char h[10];
  int hn = 0;
  h[0] = (unsigned char)(0x80 | (op & 0x0f));
  if (len < 126) {
    h[1] = (unsigned char)len;
    hn = 2;
  } else if (len < 65536) {
    h[1] = 126;
    h[2] = (unsigned char)(len >> 8);
    h[3] = (unsigned char)(len & 0xff);
    hn = 4;
  } else {
    h[1] = 127;
    for (int i = 0; i < 8; i++)
      h[2 + i] = (unsigned char)(((unsigned long long)len >> (56 - 8 * i)) & 0xff);
    hn = 10;
  }
  if (send_all(fd, h, hn) < 0)
    return -1;
  if (len && send_all(fd, data, len) < 0)
    return -1;
  return 0;
}
static int ws_send_text(int fd, const char *s) { return ws_send_frame(fd, 0x1, s, (int)strlen(s)); }

/* Append `n` bytes to a growable byte buffer. Returns 0 on OOM. */
static int buf_append(unsigned char **buf, int *len, int *cap, const unsigned char *src, int n)
{
  if (*len + n > *cap) {
    int nc = *cap ? *cap : 4096;
    while (nc < *len + n)
      nc *= 2;
    unsigned char *p = realloc(*buf, (unsigned)nc);
    if (!p)
      return 0;
    *buf = p;
    *cap = nc;
  }
  memcpy(*buf + *len, src, (unsigned)n);
  *len += n;
  return 1;
}

/* Parse every complete frame at the front of rx (masked client frames), acting on
 * each. A finished text message is NUL-terminated and pushed to the inbound queue.
 * Returns 0 to keep going, -1 to close the connection (Close frame / protocol
 * error). `msg`/`msgop` accumulate a possibly-fragmented message across calls. */
static int ws_parse(struct cdp_session *sess, int fd, unsigned char **rxp, int *rxlenp, unsigned char **msg,
                    int *msglen, int *msgcap, int *msgop)
{
  unsigned char *rx = *rxp;
  int rxlen = *rxlenp;
  int consumed_total = 0;
  int rc = 0;

  while (rxlen - consumed_total >= 2) {
    unsigned char *f = rx + consumed_total;
    int avail = rxlen - consumed_total;
    int fin = f[0] & 0x80, op = f[0] & 0x0f;
    int masked = f[1] & 0x80, l = f[1] & 0x7f;
    int hn = 2;
    unsigned long long plen;
    if (l < 126)
      plen = (unsigned)l;
    else if (l == 126) {
      if (avail < 4)
        break;
      plen = ((unsigned)f[2] << 8) | f[3];
      hn = 4;
    } else {
      if (avail < 10)
        break;
      plen = 0;
      for (int i = 0; i < 8; i++)
        plen = (plen << 8) | f[2 + i];
      hn = 10;
    }
    if (plen > CDP_WS_MAX) {
      rc = -1;
      break;
    }
    unsigned char *mask = 0;
    if (masked) {
      if (avail < hn + 4)
        break;
      mask = f + hn;
      hn += 4;
    }
    if ((unsigned long long)(avail - hn) < plen)
      break; /* await full payload */

    unsigned char *payload = f + hn;
    if (masked)
      for (unsigned long long i = 0; i < plen; i++)
        payload[i] ^= mask[i & 3];

    if (op == 0x8) {
      rc = -1;
      break;
    } /* close */
    else if (op == 0x9) {
      if (ws_send_frame(fd, 0xA, payload, (int)plen) < 0) {
        rc = -1;
        break;
      }
    } else if (op == 0xA) {                           /* pong: ignore */
    } else if (op == 0x1 || op == 0x2 || op == 0x0) { /* text/binary/continuation */
      if (op != 0x0)
        *msgop = op;
      if (!buf_append(msg, msglen, msgcap, payload, (int)plen)) {
        rc = -1;
        break;
      }
      if (fin) {
        if (*msgop == 0x1 && *msglen >= 0) { /* a complete text message */
          char *s = malloc((unsigned)*msglen + 1);
          if (s) {
            memcpy(s, *msg, (unsigned)*msglen);
            s[*msglen] = '\0';
            sess_in_push(sess, s);
          }
        }
        *msglen = 0;
        *msgop = 0;
      }
    }
    consumed_total += hn + (int)plen;
  }

  if (consumed_total > 0) {
    memmove(rx, rx + consumed_total, (unsigned)(rxlen - consumed_total));
    *rxlenp = rxlen - consumed_total;
  }
  return rc;
}

/* Full-duplex WS session on `sess`: pump inbound frames -> its queue, drain its
 * outbound queue -> frames, until the peer closes or errors. Blocks the calling
 * pool worker for the session's lifetime. */
static void serve_ws(struct cdp_session *sess, int fd)
{
  unsigned char *rx = 0;
  int rxlen = 0, rxcap = 0;
  unsigned char *msg = 0;
  int msglen = 0, msgcap = 0, msgop = 0;
  unsigned char tmp[4096];

  for (;;) {
    int n = net_recv_nb(fd, tmp, sizeof tmp);
    if (n < 0)
      break; /* EOF / error */
    if (n > 0) {
      if (!buf_append(&rx, &rxlen, &rxcap, tmp, n))
        break;
      if (ws_parse(sess, fd, &rx, &rxlen, &msg, &msglen, &msgcap, &msgop) < 0)
        break;
    }
    char *o;
    int sent_err = 0;
    while ((o = sess_out_pop(sess))) {
      int r = ws_send_text(fd, o);
      free(o);
      if (r < 0) {
        sent_err = 1;
        break;
      }
    }
    if (sent_err)
      break;
    if (n == 0)
      msleep(4);
  }

  free(rx);
  free(msg);
}

/* --- HTTP discovery + WS handshake ----------------------------------------- */

/* Case-insensitive lookup of header `name` in a raw request buffer; copies its
 * value (trimmed) into out. Returns 1 if found. */
static int http_header(const char *req, const char *name, char *out, int cap)
{
  int nl = (int)strlen(name);
  out[0] = '\0';
  for (const char *p = req; *p;) {
    const char *e = p;
    while (*e && *e != '\n')
      e++;
    int match = 1;
    for (int i = 0; i < nl; i++)
      if (lower(p[i]) != lower(name[i])) {
        match = 0;
        break;
      }
    if (match && p[nl] == ':') {
      const char *v = p + nl + 1;
      while (*v == ' ' || *v == '\t')
        v++;
      const char *t = e;
      if (t > v && t[-1] == '\r')
        t--;
      int k = 0;
      while (v < t && k < cap - 1)
        out[k++] = *v++;
      out[k] = '\0';
      return 1;
    }
    p = *e ? e + 1 : e;
  }
  return 0;
}

static void http_send(int fd, const char *status, const char *ctype, const char *body)
{
  char hdr[256];
  int n = 0;
  int blen = body ? (int)strlen(body) : 0;
  sapp(hdr, &n, sizeof hdr, "HTTP/1.1 ");
  sapp(hdr, &n, sizeof hdr, status);
  sapp(hdr, &n, sizeof hdr, "\r\nContent-Type: ");
  sapp(hdr, &n, sizeof hdr, ctype);
  sapp(hdr, &n, sizeof hdr, "\r\nContent-Length: ");
  uapp(hdr, &n, sizeof hdr, (unsigned)blen);
  sapp(hdr, &n, sizeof hdr, "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
  send_all(fd, hdr, n);
  if (blen)
    send_all(fd, body, blen);
  /* The connection worker net_close()s next; the kernel's graceful FIN handshake
   * (tcp.c) flushes the response before the socket goes away, so no drain here. */
}

/* Build one CDP target-info object for a tab's page. `host` is the client's
 * Host: header, so the ws/frontend URLs point back at wherever it connected. */
static json_value *target_json(const char *host, const struct page *pg)
{
  char ws[URL_MAX], fe[URL_MAX];
  int n;
  n = 0;
  sapp(ws, &n, sizeof ws, "ws://");
  sapp(ws, &n, sizeof ws, host);
  sapp(ws, &n, sizeof ws, PAGE_WS_PREFIX);
  sapp(ws, &n, sizeof ws, pg->target_id);
  n = 0;
  sapp(fe, &n, sizeof fe, host);
  sapp(fe, &n, sizeof fe, PAGE_WS_PREFIX);
  sapp(fe, &n, sizeof fe, pg->target_id);

  json_value *o = json_new_object();
  json_object_put(o, "id", json_new_string(pg->target_id));
  json_object_put(o, "type", json_new_string("page"));
  json_object_put(o, "title", json_new_string(pg->doc_title[0] ? pg->doc_title : "OwOS"));
  json_object_put(o, "url", json_new_string(pg->cur_url[0] ? pg->cur_url : "about:blank"));
  json_object_put(o, "description", json_new_string(""));
  json_object_put(o, "webSocketDebuggerUrl", json_new_string(ws));
  {
    char d[URL_MAX + 64];
    int k = 0;
    sapp(d, &k, sizeof d, "/devtools/inspector.html?ws=");
    sapp(d, &k, sizeof d, fe);
    json_object_put(o, "devtoolsFrontendUrl", json_new_string(d));
  }
  return o;
}

struct target_list {
  const char *host;
  json_value *arr;
};
static void list_target(const struct page *pg, bool active, void *ctx)
{
  struct target_list *tl = ctx;
  json_value *o = target_json(tl->host, pg);
  (void)active;
  if (json_array_push(tl->arr, o) != 0)
    json_free(o);
}

static void serve_http(int fd, const char *req)
{
  /* Request line: METHOD SP PATH SP HTTP/x */
  char path[256];
  int pn = 0;
  const char *p = req;
  while (*p && *p != ' ')
    p++; /* skip method */
  if (*p == ' ')
    p++;
  while (*p && *p != ' ' && *p != '\r' && *p != '\n' && pn < (int)sizeof path - 1)
    path[pn++] = *p++;
  path[pn] = '\0';

  char host[128];
  if (!http_header(req, "Host", host, sizeof host) || !host[0]) {
    int k = 0;
    sapp(host, &k, sizeof host, "localhost:9222");
  }

  char upg[64], key[128];
  int is_ws = http_header(req, "Upgrade", upg, sizeof upg) && contains_ci(upg, "websocket") &&
              http_header(req, "Sec-WebSocket-Key", key, sizeof key) && key[0];
  if (is_ws && starts_ci(path, "/devtools/")) {
    const char *page_id = 0;
    if (starts_ci(path, PAGE_WS_PREFIX))
      page_id = path + sizeof PAGE_WS_PREFIX - 1;
    if (page_id ? !tabs_has_target(page_id)
                : !(starts_ci(path, BROWSER_WS_PREFIX) && streq(path + sizeof BROWSER_WS_PREFIX - 1, g_browser_id))) {
      http_send(fd, "404 Not Found", "text/plain", "cdp: no such target\n");
      return;
    }
    struct cdp_session *s = sess_alloc();
    if (!s) {
      http_send(fd, "503 Service Unavailable", "text/plain", "cdp: too many sessions\n");
      return;
    }
    if (page_id)
      strlcpy(s->target, page_id, sizeof s->target);

    char cat[192];
    int k = 0;
    sapp(cat, &k, sizeof cat, key);
    sapp(cat, &k, sizeof cat, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"); /* RFC 6455 GUID */
    unsigned char dg[SHA1_DIGEST];
    sha1(cat, (unsigned)strlen(cat), dg);
    char accept[64];
    base64_encode(dg, SHA1_DIGEST, accept);

    char resp[256];
    int n = 0;
    sapp(resp, &n, sizeof resp,
         "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ");
    sapp(resp, &n, sizeof resp, accept);
    sapp(resp, &n, sizeof resp, "\r\n\r\n");
    if (send_all(fd, resp, n) == 0)
      serve_ws(s, fd);
    sess_free(s);
    return;
  }

  if (starts_ci(path, "/json/version")) {
    json_value *o = json_new_object();
    json_object_put(o, "Browser", json_new_string("OwOS-Browser/" OWOS_VERSION));
    json_object_put(o, "Protocol-Version", json_new_string("1.3"));
    json_object_put(o, "User-Agent", json_new_string(browser_ua()));
    json_object_put(o, "V8-Version", json_new_string("0"));
    json_object_put(o, "WebKit-Version", json_new_string("0"));
    char wsurl[URL_MAX];
    int k = 0;
    sapp(wsurl, &k, sizeof wsurl, "ws://");
    sapp(wsurl, &k, sizeof wsurl, host);
    sapp(wsurl, &k, sizeof wsurl, BROWSER_WS_PREFIX);
    sapp(wsurl, &k, sizeof wsurl, g_browser_id);
    json_object_put(o, "webSocketDebuggerUrl", json_new_string(wsurl));
    char *body = json_encode(o, 0, 0);
    json_free(o);
    http_send(fd, "200 OK", "application/json; charset=UTF-8", body ? body : "{}");
    free(body);
    return;
  }
  if (starts_ci(path, "/json")) { /* /json and /json/list both list targets */
    struct target_list tl = {host, json_new_array()};
    tabs_each_page(list_target, &tl);
    json_value *arr = tl.arr;
    char *body = json_encode(arr, 0, 0);
    json_free(arr);
    http_send(fd, "200 OK", "application/json; charset=UTF-8", body ? body : "[]");
    free(body);
    return;
  }
  http_send(fd, "404 Not Found", "text/plain", "Not Found\n");
}

/* --- listener + connection thread pool -------------------------------------- *
 * The acceptor thread only accepts and hands each connection to a free pool
 * worker, so a long-lived WebSocket session never blocks HTTP discovery and slow
 * clients can't stall the accept loop. Only the acceptor writes a non-(-1) fd into
 * a slot and only that slot's worker clears it, so the handoff needs no lock. */
#define CDP_NWORKERS 4
#define CDP_WSTACK 16384
static unsigned char cdp_wstack[CDP_NWORKERS][CDP_WSTACK] __attribute__((aligned(16)));
static volatile int cdp_wfd[CDP_NWORKERS]; /* fd handed to worker i, -1 = idle */
static int cdp_wnext;                      /* worker startup index claim        */

/* 1 once the request head's blank line (CRLF CRLF) has arrived */
static int head_complete(const char *req, int n)
{
  for (int i = 3; i < n; i++)
    if (req[i - 3] == '\r' && req[i - 2] == '\n' && req[i - 1] == '\r' && req[i] == '\n')
      return 1;
  return 0;
}

static void cdp_conn_worker(void)
{
  int me;
  mutex_lock(&sess_lk);
  me = cdp_wnext++;
  mutex_unlock(&sess_lk);
  for (;;) {
    int fd = cdp_wfd[me];
    if (fd < 0) {
      msleep(5);
      continue;
    }
    /* Read the request head (blocking) up to the blank line, then serve it. */
    char req[8192];
    int rn = 0;
    while (rn < (int)sizeof req - 1) {
      int r = net_recv(fd, req + rn, (int)sizeof req - 1 - rn);
      if (r <= 0)
        break;
      rn += r;
      req[rn] = '\0';
      if (head_complete(req, rn))
        break;
    }
    req[rn] = '\0';
    if (rn > 0)
      serve_http(fd, req); /* HTTP response, or blocks in serve_ws */
    net_close(fd);
    cdp_wfd[me] = -1; /* free the slot for the next connection */
  }
}

static void cdp_io_fn(void)
{
  int lfd = net_listen(CDP_PORT);
  if (lfd < 0) {
    printf("browser: remote debugging unavailable, port %d is in use\n", CDP_PORT);
    return;
  }
  printf("browser: remote debugging listening on port %d\n", CDP_PORT);
  for (int i = 0; i < CDP_NWORKERS; i++) {
    cdp_wfd[i] = -1;
    thread_create(cdp_conn_worker, &cdp_wstack[i][CDP_WSTACK]);
  }
  for (;;) {
    int fd = net_accept(lfd);
    if (fd < 0) {
      msleep(50);
      continue;
    }
    int placed = 0;
    for (int tries = 0; tries < 100 && !placed; tries++) {
      for (int i = 0; i < CDP_NWORKERS; i++)
        if (cdp_wfd[i] == -1) {
          cdp_wfd[i] = fd;
          placed = 1;
          break;
        }
      if (!placed)
        msleep(5);
    }
    if (!placed)
      net_close(fd); /* every worker busy: reject the connection */
  }
}

/* --- protocol command dispatch (worker thread) ----------------------------- */

/* scheme://host portion of `url` (no path), or the url unchanged if it has none. */
static void origin_of(const char *url, char *out, int cap)
{
  out[0] = '\0';
  if (!url || !url[0])
    return;
  const char *s = url;
  const char *sep = 0;
  for (const char *p = url; p[0] && p[1] && p[2]; p++)
    if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
      sep = p + 3;
      break;
    }
  int k = 0;
  if (!sep) {
    while (s[k] && k < cap - 1) {
      out[k] = s[k];
      k++;
    }
    out[k] = '\0';
    return;
  }
  const char *e = sep;
  while (*e && *e != '/')
    e++; /* host ends at path */
  while (s < e && k < cap - 1)
    out[k++] = *s++;
  out[k] = '\0';
}

static json_value *build_frame(void)
{
  json_value *f = json_new_object();
  char origin[URL_MAX];
  origin_of(B->pg->cur_url, origin, sizeof origin);
  json_object_put(f, "id", json_new_string(B->pg->target_id)); /* a main frame shares its target's id, as in Chrome */
  json_object_put(f, "loaderId", json_new_string(B->pg->loader_id));
  json_object_put(f, "url", json_new_string(B->pg->cur_url[0] ? B->pg->cur_url : "about:blank"));
  json_object_put(f, "securityOrigin", json_new_string(origin));
  json_object_put(f, "mimeType", json_new_string("text/html"));
  return f;
}
static json_value *build_context(void)
{
  json_value *c = json_new_object();
  char origin[URL_MAX];
  origin_of(B->pg->cur_url, origin, sizeof origin);
  json_object_put(c, "id", json_new_number(1));
  json_object_put(c, "origin", json_new_string(origin));
  json_object_put(c, "name", json_new_string(""));
  json_object_put(c, "uniqueId", json_new_string("owos-ctx-1"));
  json_value *aux = json_new_object();
  json_object_put(aux, "isDefault", json_new_bool(1));
  json_object_put(aux, "type", json_new_string("default"));
  json_object_put(aux, "frameId", json_new_string(B->pg->target_id));
  json_object_put(c, "auxData", aux);
  return c;
}
/* Emit Runtime.executionContextCreated. `s` non-NULL -> just that session (a
 * Runtime.enable reply); NULL -> broadcast to every Runtime-enabled session (a
 * navigation created a fresh context). */
static void emit_context_created(struct cdp_session *s)
{
  json_value *p = json_new_object();
  json_object_put(p, "context", build_context());
  if (s)
    emit_to_session(s, "Runtime.executionContextCreated", p);
  else
    broadcast_event(EV_RUNTIME, "Runtime.executionContextCreated", p);
}

/* Classify a Runtime.evaluate string result into a CDP RemoteObject so the console
 * renders numbers/booleans/null natively (not quoted strings). */
static int looks_numeric(const char *s)
{
  if (!s || !*s)
    return 0;
  int i = 0, dig = 0, dot = 0, e = 0;
  if (s[i] == '-' || s[i] == '+')
    i++;
  for (; s[i]; i++) {
    char c = s[i];
    if (c >= '0' && c <= '9')
      dig = 1;
    else if (c == '.') {
      if (dot || e)
        return 0;
      dot = 1;
    } else if (c == 'e' || c == 'E') {
      if (e || !dig)
        return 0;
      e = 1;
      if (s[i + 1] == '+' || s[i + 1] == '-')
        i++;
    } else
      return 0;
  }
  return dig;
}
static json_value *remote_object(const char *val)
{
  json_value *o = json_new_object();
  if (!val) {
    json_object_put(o, "type", json_new_string("undefined"));
    return o;
  }
  if (streq(val, "undefined")) {
    json_object_put(o, "type", json_new_string("undefined"));
    return o;
  }
  if (streq(val, "null")) {
    json_object_put(o, "type", json_new_string("object"));
    json_object_put(o, "subtype", json_new_string("null"));
    json_object_put(o, "value", json_new_null());
    return o;
  }
  if (streq(val, "true") || streq(val, "false")) {
    json_object_put(o, "type", json_new_string("boolean"));
    json_object_put(o, "value", json_new_bool(streq(val, "true")));
    return o;
  }
  if (looks_numeric(val)) {
    json_object_put(o, "type", json_new_string("number"));
    json_object_put(o, "value", json_new_number(strtod(val, 0)));
    json_object_put(o, "description", json_new_string(val));
    return o;
  }
  json_object_put(o, "type", json_new_string("string"));
  json_object_put(o, "value", json_new_string(val));
  return o;
}

/* --- DOM tree serialization (nodeId registry) ------------------------------ */
static struct dom_node **cdp_nodes;
static int cdp_nnodes, cdp_nodes_cap;
static int node_register(struct dom_node *n)
{
  if (cdp_nnodes >= cdp_nodes_cap) {
    int nc = cdp_nodes_cap ? cdp_nodes_cap * 2 : 256;
    struct dom_node **p = realloc(cdp_nodes, (unsigned)nc * sizeof *p);
    if (!p)
      return cdp_nnodes; /* reuse the last id on OOM */
    cdp_nodes = p;
    cdp_nodes_cap = nc;
  }
  cdp_nodes[cdp_nnodes++] = n;
  return cdp_nnodes; /* id = 1-based index */
}
/* Resolve a CDP nodeId back to its DOM node (registry filled by DOM.getDocument). */
static struct dom_node *node_by_id(int id) { return (id >= 1 && id <= cdp_nnodes) ? cdp_nodes[id - 1] : 0; }
/* Next element after `n` in document order, within `root`'s subtree (excluding
 * root itself, as querySelector does), that matches `l`; NULL when exhausted. */
static struct dom_node *next_match(const struct sel_list *l, struct dom_node *n, struct dom_node *root)
{
  for (;;) {
    if (n->first_child) {
      n = n->first_child;
    } else {
      while (n != root && !n->next)
        n = n->parent;
      if (n == root)
        return 0;
      n = n->next;
    }
    if (n->type == DOM_ELEMENT && css_sel_match(l, n))
      return n;
  }
}
static int text_is_ws(const struct dom_node *n)
{
  const char *s = n->text;
  if (!s)
    return 1;
  for (; *s; s++)
    if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
      return 0;
  return 1;
}
static int child_shown(const struct dom_node *c)
{
  return c->type == DOM_ELEMENT || (c->type == DOM_TEXT && !text_is_ws(c));
}
static void upper_tag(const char *tag, char *out, int cap)
{
  int k = 0;
  for (; tag[k] && k < cap - 1; k++) {
    char c = tag[k];
    out[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
  }
  out[k] = '\0';
}
static json_value *dom_serialize(struct dom_node *n, int depth, int maxdepth, int *budget)
{
  if (*budget <= 0)
    return 0;
  (*budget)--;
  int id = node_register(n);
  int is_doc = n->type == DOM_ELEMENT && n->tag[0] == '#'; /* the "#document" root */
  json_value *o = json_new_object();
  json_object_put(o, "nodeId", json_new_number(id));
  json_object_put(o, "backendNodeId", json_new_number(id));
  json_object_put(o, "nodeType", json_new_number(is_doc ? 9 : (n->type == DOM_TEXT ? 3 : 1)));
  if (is_doc) {
    json_object_put(o, "nodeName", json_new_string("#document"));
    json_object_put(o, "localName", json_new_string(""));
    json_object_put(o, "nodeValue", json_new_string(""));
  } else if (n->type == DOM_TEXT) {
    json_object_put(o, "nodeName", json_new_string("#text"));
    json_object_put(o, "localName", json_new_string(""));
    json_object_put(o, "nodeValue", json_new_string(n->text ? n->text : ""));
  } else {
    char up[24];
    upper_tag(n->tag, up, sizeof up);
    json_object_put(o, "nodeName", json_new_string(up));
    json_object_put(o, "localName", json_new_string(n->tag));
    json_object_put(o, "nodeValue", json_new_string(""));
    json_value *attrs = json_new_array();
    for (struct dom_attr *a = n->attrs; a; a = a->next) {
      json_array_push(attrs, json_new_string(a->name));
      json_array_push(attrs, json_new_string(a->value ? a->value : ""));
    }
    json_object_put(o, "attributes", attrs);
  }

  /* The <!DOCTYPE> is no DOM node here; it is kept on the document root and shown as a first child (type 10). */
  const char *dt = is_doc ? dom_doctype(n) : 0;
  int nch = dt ? 1 : 0;
  for (struct dom_node *c = n->first_child; c; c = c->next)
    if (child_shown(c))
      nch++;
  json_object_put(o, "childNodeCount", json_new_number(nch));

  if (nch && depth < maxdepth) {
    json_value *kids = json_new_array();
    if (dt) {
      int did = node_register(0); /* no backing node; resolves to nothing */
      json_value *d = json_new_object();
      json_object_put(d, "nodeId", json_new_number(did));
      json_object_put(d, "backendNodeId", json_new_number(did));
      json_object_put(d, "nodeType", json_new_number(10));
      json_object_put(d, "nodeName", json_new_string(dt));
      json_object_put(d, "localName", json_new_string(""));
      json_object_put(d, "nodeValue", json_new_string(""));
      json_object_put(d, "name", json_new_string(dt));
      json_object_put(d, "publicId", json_new_string(""));
      json_object_put(d, "systemId", json_new_string(""));
      json_array_push(kids, d);
    }
    for (struct dom_node *c = n->first_child; c; c = c->next) {
      if (!child_shown(c))
        continue;
      json_value *kv = dom_serialize(c, depth + 1, maxdepth, budget);
      if (kv)
        json_array_push(kids, kv);
      else
        break;
    }
    json_object_put(o, "children", kids);
  }
  return o;
}

/* Navigate the page: worker->main handoff, same as JS location.assign so history
 * and the address bar update. */
static void cdp_navigate(const char *url)
{
  if (!url || !url[0])
    return;
  strlcpy(B->req_url, url, sizeof B->req_url);
  B->req_method[0] = '\0';
  B->req_body_len = 0;
  B->req_pending = 1;
  B->seq++;
}

/* Parse a "prop: value; prop: value" declaration block into a CDP CSSStyle
 * (cssProperties [{name,value}] + cssText), the shape the Styles pane renders. */
static json_value *cssstyle_from_text(const char *raw)
{
  json_value *st = json_new_object();
  json_value *props = json_new_array();
  for (const char *seg = raw ? raw : ""; *seg;) {
    const char *e = seg;
    while (*e && *e != ';')
      e++;
    const char *c = seg;
    while (c < e && *c != ':')
      c++;
    if (c < e) {
      const char *ns = seg;
      while (ns < c && (*ns == ' ' || *ns == '\t' || *ns == '\n'))
        ns++;
      const char *ne = c;
      while (ne > ns && (ne[-1] == ' ' || ne[-1] == '\t'))
        ne--;
      const char *vs = c + 1;
      while (vs < e && (*vs == ' ' || *vs == '\t'))
        vs++;
      const char *ve = e;
      while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n'))
        ve--;
      if (ne > ns && ve > vs) {
        char nm[96], vl[256];
        int nl = (int)(ne - ns);
        if (nl > 95)
          nl = 95;
        memcpy(nm, ns, (unsigned)nl);
        nm[nl] = '\0';
        int vln = (int)(ve - vs);
        if (vln > 255)
          vln = 255;
        memcpy(vl, vs, (unsigned)vln);
        vl[vln] = '\0';
        json_value *pr = json_new_object();
        json_object_put(pr, "name", json_new_string(nm));
        json_object_put(pr, "value", json_new_string(vl));
        json_array_push(props, pr);
      }
    }
    seg = *e ? e + 1 : e;
  }
  json_object_put(st, "cssProperties", props);
  json_object_put(st, "shorthandEntries", json_new_array());
  json_object_put(st, "cssText", json_new_string(raw ? raw : ""));
  return st;
}

/* Append one {name,value} entry to a computed-style array. */
static void cs_add(json_value *arr, const char *name, const char *value)
{
  json_value *p = json_new_object();
  json_object_put(p, "name", json_new_string(name));
  json_object_put(p, "value", json_new_string(value));
  json_array_push(arr, p);
}
static void cs_px(json_value *arr, const char *name, int v)
{
  if (v < 0)
    return;
  char b[32];
  int n = 0;
  uapp(b, &n, sizeof b, (unsigned)v);
  sapp(b, &n, sizeof b, "px");
  cs_add(arr, name, b);
}
static void cs_color(json_value *arr, const char *name, int c)
{
  if (c < 0 || (unsigned)c == BG_NONE)
    return;
  static const char hx[] = "0123456789abcdef";
  char b[8];
  int k = 0;
  b[k++] = '#';
  for (int sh = 20; sh >= 0; sh -= 4)
    b[k++] = hx[(c >> sh) & 15];
  b[k] = '\0';
  cs_add(arr, name, b);
}

/* CSS.getComputedStyleForNode: a flat computed list from html_compute_style: the
 * subset the engine models, enough to populate DevTools' Computed pane. */
static json_value *computed_style(struct dom_node *n)
{
  json_value *arr = json_new_array();
  if (!n || n->type != DOM_ELEMENT)
    return arr;
  struct css_style s;
  html_compute_style(n, &s);
  cs_add(arr, "display",
         s.grid == 1                 ? "grid"
         : s.flex == 1               ? "flex"
         : s.disp == CSS_DISP_INLINE ? "inline"
         : s.disp == CSS_DISP_NONE   ? "none"
                                     : "block");
  if (s.fam[0])
    cs_add(arr, "font-family", s.fam);
  cs_px(arr, "font-size", s.font_px);
  cs_color(arr, "color", s.color);
  cs_color(arr, "background-color", s.bg);
  if (s.bold == 1)
    cs_add(arr, "font-weight", "bold");
  if (s.under == 1)
    cs_add(arr, "text-decoration", "underline");
  if (s.center == 1)
    cs_add(arr, "text-align", "center");
  cs_px(arr, "width", s.width);
  cs_px(arr, "height", s.height);
  cs_px(arr, "border-radius", s.radius);
  return arr;
}

/* --- response-body store (for Network.getResponseBody) --------------------- *
 * Recent response bodies keyed by request id, filled from the fetch completion paths only while a client is
 * attached. Bodies are kept whole: the oldest are evicted to stay within the budget, and one larger than the
 * whole budget isn't kept at all, so a client gets an error rather than a truncated body. */
#define CDP_BODY_BUDGET (32u << 20)
struct cdp_body {
  int id;
  unsigned char *data;
  int len;
};
static struct cdp_body *cdp_bodies; /* oldest first */
static int cdp_nbodies, cdp_bodies_cap;
static unsigned cdp_body_bytes;
static mutex_t body_lk = MUTEX_INIT;

static void body_drop_l(int i)
{
  cdp_body_bytes -= (unsigned)cdp_bodies[i].len;
  free(cdp_bodies[i].data);
  memmove(&cdp_bodies[i], &cdp_bodies[i + 1], (unsigned)(cdp_nbodies - i - 1) * sizeof *cdp_bodies);
  cdp_nbodies--;
}

void cdp_net_body(int id, const void *data, int len, const char *ctype)
{
  (void)ctype;
  if (!cdp_nsessions || !data || len < 0 || (unsigned)len > CDP_BODY_BUDGET)
    return;
  unsigned char *copy = malloc((unsigned)(len ? len : 1));
  if (!copy)
    return;
  memcpy(copy, data, (unsigned)len);
  mutex_lock(&body_lk);
  for (int i = 0; i < cdp_nbodies; i++)
    if (cdp_bodies[i].id == id) {
      body_drop_l(i);
      break;
    }
  while (cdp_nbodies && cdp_body_bytes + (unsigned)len > CDP_BODY_BUDGET)
    body_drop_l(0);
  if (cdp_nbodies == cdp_bodies_cap) {
    int nc = cdp_bodies_cap ? cdp_bodies_cap * 2 : 32;
    struct cdp_body *grown = realloc(cdp_bodies, (unsigned)nc * sizeof *grown);
    if (!grown) {
      mutex_unlock(&body_lk);
      free(copy);
      return;
    }
    cdp_bodies = grown;
    cdp_bodies_cap = nc;
  }
  cdp_bodies[cdp_nbodies].id = id;
  cdp_bodies[cdp_nbodies].data = copy;
  cdp_bodies[cdp_nbodies].len = len;
  cdp_nbodies++;
  cdp_body_bytes += (unsigned)len;
  mutex_unlock(&body_lk);
}
void cdp_net_reset(void)
{
  mutex_lock(&body_lk);
  while (cdp_nbodies)
    body_drop_l(cdp_nbodies - 1);
  mutex_unlock(&body_lk);
}
/* Copy out the stored body for `id` (caller frees *out); returns its length or -1. */
static int cdp_body_dup(int id, unsigned char **out)
{
  int n = -1;
  mutex_lock(&body_lk);
  for (int i = 0; i < cdp_nbodies; i++)
    if (cdp_bodies[i].id == id) {
      unsigned char *c = malloc((unsigned)(cdp_bodies[i].len ? cdp_bodies[i].len : 1));
      if (c) {
        memcpy(c, cdp_bodies[i].data, (unsigned)cdp_bodies[i].len);
        *out = c;
        n = cdp_bodies[i].len;
      }
      break;
    }
  mutex_unlock(&body_lk);
  return n;
}

static const char *res_type(const char *ct);

/* Four corners of a rectangle as a CDP quad array: [x1,y1, x2,y2, x3,y3, x4,y4]. */
static json_value *quad(int x, int y, int w, int h)
{
  json_value *q = json_new_array();
  json_array_push(q, json_new_number(x));
  json_array_push(q, json_new_number(y));
  json_array_push(q, json_new_number(x + w));
  json_array_push(q, json_new_number(y));
  json_array_push(q, json_new_number(x + w));
  json_array_push(q, json_new_number(y + h));
  json_array_push(q, json_new_number(x));
  json_array_push(q, json_new_number(y + h));
  return q;
}

/* Dispatch one method; returns the `result` object to wrap (never NULL; an empty
 * object is the permissive answer for anything we don't specifically model, so a
 * DevTools frontend keeps initializing instead of stalling on an error). */
/* Target.TargetInfo for one tab's page */
struct target_infos {
  const char *only; /* fill `one` with just this target, or push every tab onto `arr` */
  json_value *arr, *one;
};
static void add_target_info(const struct page *pg, bool active, void *ctx)
{
  struct target_infos *ti = ctx;
  if (ti->only && !streq(ti->only, pg->target_id))
    return;
  int attached = 0;
  for (int i = 0; i < CDP_MAX_SESSIONS; i++)
    if (sessions[i].used && (streq(sessions[i].target, pg->target_id) || (!sessions[i].target[0] && active)))
      attached = 1;
  json_value *o = json_new_object();
  json_object_put(o, "targetId", json_new_string(pg->target_id));
  json_object_put(o, "type", json_new_string("page"));
  json_object_put(o, "title", json_new_string(pg->doc_title[0] ? pg->doc_title : "OwOS"));
  json_object_put(o, "url", json_new_string(pg->cur_url[0] ? pg->cur_url : "about:blank"));
  json_object_put(o, "attached", json_new_bool(attached));
  json_object_put(o, "browserContextId", json_new_string("owos-ctx"));
  if (ti->only)
    ti->one = o;
  else if (json_array_push(ti->arr, o) != 0)
    json_free(o);
}

static json_value *handle_method(struct cdp_session *sess, const char *m, json_value *params)
{
  json_value *r = json_new_object();

  if (streq(m, "Runtime.enable")) {
    sess->rt_enabled = 1;
    emit_context_created(sess);
    return r;
  }
  if (streq(m, "Log.enable")) {
    sess->log_enabled = 1;
    return r;
  }
  if (streq(m, "Page.enable")) {
    sess->page_enabled = 1;
    return r;
  }
  if (streq(m, "DOM.enable")) {
    sess->dom_enabled = 1;
    return r;
  }
  if (streq(m, "Network.enable")) {
    sess->net_enabled = 1;
    return r;
  }
  if (streq(m, "CSS.enable")) {
    sess->css_enabled = 1;
    return r;
  }

  if (streq(m, "Runtime.evaluate") || streq(m, "Runtime.callFunctionOn")) {
    const char *expr = json_as_string(json_object_get(params, "expression"));
    const char *fn = json_as_string(json_object_get(params, "functionDeclaration"));
    char *call = 0;
    if (!expr && fn) { /* call it with no arguments; `arguments`/objectId aren't modelled */
      int n = 0, cap = (int)strlen(fn) + 8;
      call = malloc((unsigned)cap);
      if (call) {
        sapp(call, &n, cap, "(");
        sapp(call, &n, cap, fn);
        sapp(call, &n, cap, ")()");
      }
      expr = call;
    }
    int is_exc = 0;
    char *val = expr ? js_page_eval(expr, &is_exc) : 0;
    free(call);
    if (!is_exc)
      json_object_put(r, "result", remote_object(val ? val : "undefined"));
    if (is_exc) {
      json_value *err = json_new_object();
      json_object_put(err, "type", json_new_string("object"));
      json_object_put(err, "subtype", json_new_string("error"));
      json_object_put(err, "className", json_new_string("Error"));
      json_object_put(err, "description", json_new_string(val ? val : "error"));
      json_object_put(r, "result", err);
      json_value *ed = json_new_object();
      json_object_put(ed, "exceptionId", json_new_number(1));
      json_object_put(ed, "text", json_new_string("Uncaught"));
      json_object_put(ed, "lineNumber", json_new_number(0));
      json_object_put(ed, "columnNumber", json_new_number(0));
      json_value *ex = json_new_object();
      json_object_put(ex, "type", json_new_string("object"));
      json_object_put(ex, "subtype", json_new_string("error"));
      json_object_put(ex, "className", json_new_string("Error"));
      json_object_put(ex, "description", json_new_string(val ? val : "error"));
      json_object_put(ed, "exception", ex);
      json_object_put(r, "exceptionDetails", ed);
    }
    free(val);
    return r;
  }
  if (streq(m, "Runtime.getProperties")) {
    json_object_put(r, "result", json_new_array());
    return r;
  }

  if (streq(m, "Page.navigate")) {
    const char *url = json_as_string(json_object_get(params, "url"));
    cdp_navigate(url);
    json_object_put(r, "frameId", json_new_string(B->pg->target_id));
    json_object_put(r, "loaderId", json_new_string(B->pg->loader_id));
    return r;
  }
  if (streq(m, "Page.reload")) {
    cdp_navigate(B->pg->cur_url);
    return r;
  }
  if (streq(m, "Page.getFrameTree") || streq(m, "Page.getResourceTree")) {
    json_value *tree = json_new_object();
    json_object_put(tree, "frame", build_frame());
    json_object_put(tree, "childFrames", json_new_array());
    if (streq(m, "Page.getResourceTree")) {
      json_object_put(tree, "resources", json_new_array()); /* the browser keeps no subresource registry */
    }
    json_object_put(r, "frameTree", tree);
    return r;
  }
  if (streq(m, "Page.getNavigationHistory")) {
    json_object_put(r, "currentIndex", json_new_number(0));
    json_value *entries = json_new_array();
    json_value *e = json_new_object();
    json_object_put(e, "id", json_new_number(0));
    json_object_put(e, "url", json_new_string(B->pg->cur_url[0] ? B->pg->cur_url : "about:blank"));
    json_object_put(e, "userTypedURL", json_new_string(B->pg->cur_url));
    json_object_put(e, "title", json_new_string(B->pg->doc_title));
    json_object_put(e, "transitionType", json_new_string("typed"));
    json_array_push(entries, e);
    json_object_put(r, "entries", entries);
    return r;
  }
  if (streq(m, "Page.createIsolatedWorld")) {
    json_object_put(r, "executionContextId", json_new_number(1));
    return r;
  }
  if (streq(m, "Page.getLayoutMetrics")) {
    json_value *cs = json_new_object();
    json_object_put(cs, "pageX", json_new_number(0));
    json_object_put(cs, "pageY", json_new_number(0));
    json_object_put(cs, "clientWidth", json_new_number(1024));
    json_object_put(cs, "clientHeight", json_new_number(768));
    json_object_put(r, "layoutViewport", cs);
    return r;
  }

  if (streq(m, "DOM.getDocument")) {
    struct dom_node *root = html_dom_root();
    cdp_nnodes = 0;
    int budget = 4000;
    json_value *node = root ? dom_serialize(root, 0, 40, &budget) : 0;
    if (!node) { /* no page yet: a bare document node */
      node = json_new_object();
      json_object_put(node, "nodeId", json_new_number(1));
      json_object_put(node, "backendNodeId", json_new_number(1));
      json_object_put(node, "nodeType", json_new_number(9));
      json_object_put(node, "nodeName", json_new_string("#document"));
      json_object_put(node, "localName", json_new_string(""));
      json_object_put(node, "nodeValue", json_new_string(""));
      json_object_put(node, "childNodeCount", json_new_number(0));
    }
    json_object_put(r, "root", node);
    return r;
  }
  if (streq(m, "DOM.querySelector")) {
    struct dom_node *n = node_by_id((int)json_as_number(json_object_get(params, "nodeId"), -1));
    const char *sel = json_as_string(json_object_get(params, "selector"));
    struct sel_list *l = (n && sel && sel[0]) ? css_sel_parse(sel) : 0;
    struct dom_node *found = l ? next_match(l, n, n) : 0;
    css_sel_free(l);
    json_object_put(r, "nodeId", json_new_number(found ? node_register(found) : 0));
    return r;
  }
  if (streq(m, "DOM.querySelectorAll")) {
    struct dom_node *n = node_by_id((int)json_as_number(json_object_get(params, "nodeId"), -1));
    const char *sel = json_as_string(json_object_get(params, "selector"));
    json_value *ids = json_new_array();
    struct sel_list *l = (n && sel && sel[0]) ? css_sel_parse(sel) : 0;
    for (struct dom_node *c = l ? next_match(l, n, n) : 0; c; c = next_match(l, c, n))
      json_array_push(ids, json_new_number(node_register(c)));
    css_sel_free(l);
    json_object_put(r, "nodeIds", ids);
    return r;
  }
  if (streq(m, "DOM.resolveNode")) {
    int nid = (int)json_as_number(json_object_get(params, "nodeId"), -1);
    if (nid < 0)
      nid = (int)json_as_number(json_object_get(params, "backendNodeId"), -1);
    struct dom_node *n = node_by_id(nid);
    json_value *obj = json_new_object();
    json_object_put(obj, "type", json_new_string("object"));
    json_object_put(obj, "subtype", json_new_string("node"));
    {
      char oid[32];
      int k = 0;
      sapp(oid, &k, sizeof oid, "node-");
      uapp(oid, &k, sizeof oid, (unsigned)nid);
      json_object_put(obj, "objectId", json_new_string(oid));
    }
    if (n && n->type == DOM_ELEMENT) {
      char desc[128];
      int k = 0;
      sapp(desc, &k, sizeof desc, n->tag[0] ? n->tag : "?");
      const char *id = dom_get_attr(n, "id");
      if (id && id[0]) {
        sapp(desc, &k, sizeof desc, "#");
        sapp(desc, &k, sizeof desc, id);
      }
      json_object_put(obj, "description", json_new_string(desc));
    }
    json_object_put(r, "object", obj);
    return r;
  }
  if (streq(m, "DOM.getBoxModel")) {
    struct dom_node *n = node_by_id((int)json_as_number(json_object_get(params, "nodeId"), -1));
    if (n) {
      int nb = 0;
      struct lbox *pool = layout_pool(&nb);
      struct lbox *lb = 0;
      for (int i = 0; i < nb; i++)
        if (pool[i].node == n) {
          lb = &pool[i];
          break;
        }
      if (lb && lb->x >= 0 && lb->w > 0 && lb->h > 0) {
        int mx = lb->x, my = lb->y, mw = lb->w, mh = lb->h; /* margin box */
        int bx = mx + lb->margin.l, by = my + lb->margin.t;
        int bw = mw - lb->margin.l - lb->margin.r, bh = mh - lb->margin.t - lb->margin.b;
        int sx = bx + lb->border.l, sy = by + lb->border.t;
        int sw = bw - lb->border.l - lb->border.r, sh = bh - lb->border.t - lb->border.b;
        /* Content box: use cx/cy/cw/ch from the layout pass. */
        int cx = lb->cx, cy = lb->cy, cw = lb->cw, ch = lb->ch;
        json_value *model = json_new_object();
        json_object_put(model, "content", quad(cx, cy, cw, ch));
        json_object_put(model, "padding", quad(sx, sy, sw, sh));
        json_object_put(model, "border", quad(bx, by, bw, bh));
        json_object_put(model, "margin", quad(mx, my, mw, mh));
        json_object_put(model, "width", json_new_number(cw));
        json_object_put(model, "height", json_new_number(ch));
        json_object_put(r, "model", model);
      }
    }
    return r;
  }
  if (streq(m, "DOM.describeNode")) {
    /* Minimal: enough shape to not error the frontend. */
    return r;
  }

  if (streq(m, "Overlay.setInspectMode")) {
    const char *mode = json_as_string(json_object_get(params, "mode"));
    int idx = (int)(sess - sessions);
    if (mode && (streq(mode, "searchForNode") || streq(mode, "searchForUAShadowDOM")))
      cdp_pick_sess = idx;
    else if (cdp_pick_sess == idx)
      cdp_pick_sess = -1;
    return r;
  }

  if (streq(m, "CSS.getMatchedStylesForNode")) {
    struct dom_node *n = node_by_id((int)json_as_number(json_object_get(params, "nodeId"), -1));
    if (n) {
      const char *inl = dom_get_attr(n, "style");
      if (inl && inl[0])
        json_object_put(r, "inlineStyle", cssstyle_from_text(inl));
    }
    /* Rules carry no stylesheet origin in this engine, so none gets a styleSheetId or source range. */
    json_value *rules = json_new_array();
    if (n && n->type == DOM_ELEMENT) {
      struct css_match mm[64];
      int nm = html_match_styles(n, mm, 64);
      for (int i = 0; i < nm; i++) {
        json_value *rule = json_new_object();
        json_value *sl = json_new_object();
        json_value *sels = json_new_array();
        json_value *s1 = json_new_object();
        json_object_put(s1, "text", json_new_string(mm[i].selector));
        json_array_push(sels, s1);
        json_object_put(sl, "selectors", sels);
        json_object_put(sl, "text", json_new_string(mm[i].selector));
        json_object_put(rule, "selectorList", sl);
        json_object_put(rule, "origin", json_new_string("regular"));
        json_object_put(rule, "style", cssstyle_from_text(mm[i].raw && mm[i].raw[0] ? mm[i].raw : ""));
        json_value *mcr = json_new_object();
        json_object_put(mcr, "rule", rule);
        json_value *msel = json_new_array();
        json_array_push(msel, json_new_number(0));
        json_object_put(mcr, "matchingSelectors", msel);
        json_array_push(rules, mcr);
      }
    }
    json_object_put(r, "matchedCSSRules", rules);
    json_object_put(r, "pseudoElements", json_new_array());
    json_object_put(r, "inherited", json_new_array());
    json_object_put(r, "cssKeyframesRules", json_new_array());
    return r;
  }
  if (streq(m, "CSS.getInlineStylesForNode")) {
    struct dom_node *n = node_by_id((int)json_as_number(json_object_get(params, "nodeId"), -1));
    const char *inl = n ? dom_get_attr(n, "style") : 0;
    json_object_put(r, "inlineStyle", cssstyle_from_text(inl && inl[0] ? inl : ""));
    return r;
  }
  if (streq(m, "CSS.getComputedStyleForNode")) {
    struct dom_node *n = node_by_id((int)json_as_number(json_object_get(params, "nodeId"), -1));
    json_object_put(r, "computedStyle", computed_style(n));
    return r;
  }
  if (streq(m, "Network.getCookies") || streq(m, "Storage.getCookies")) {
    json_value *arr = json_new_array();
    for (struct cookie *c = B->cookies; c; c = c->next) {
      json_value *ck = json_new_object();
      int vlen = (c->name ? (int)strlen(c->name) : 0) + (c->val ? (int)strlen(c->val) : 0);
      json_object_put(ck, "name", json_new_string(c->name));
      json_object_put(ck, "value", json_new_string(c->val));
      json_object_put(ck, "domain", json_new_string(c->host));
      json_object_put(ck, "path", json_new_string("/"));
      json_object_put(ck, "expires", json_new_number(-1));
      json_object_put(ck, "size", json_new_number(vlen));
      json_object_put(ck, "httpOnly", json_new_bool(0));
      json_object_put(ck, "secure", json_new_bool(0));
      json_object_put(ck, "session", json_new_bool(1));
      json_object_put(ck, "sameSite", json_new_string("Lax"));
      json_array_push(arr, ck);
    }
    json_object_put(r, "cookies", arr);
    return r;
  }
  if (streq(m, "DOMStorage.getDOMStorageItems")) {
    int is_local = json_as_bool(json_object_get(json_object_get(params, "storageId"), "isLocalStorage"), 1);
    json_value *entries = json_new_array();
    const char *k;
    for (int i = 0; (k = storage_key(B->pg->cur_url, !is_local, i)); i++) {
      const char *v = storage_get(B->pg->cur_url, !is_local, k);
      json_value *pair = json_new_array();
      json_array_push(pair, json_new_string(k));
      json_array_push(pair, json_new_string(v ? v : ""));
      json_array_push(entries, pair);
    }
    json_object_put(r, "entries", entries);
    return r;
  }

  if (streq(m, "Network.getResponseBody")) {
    const char *rid = json_as_string(json_object_get(params, "requestId"));
    int id = 0;
    if (rid) {
      const char *p = rid;
      while (*p && (*p < '0' || *p > '9'))
        p++;
      while (*p >= '0' && *p <= '9')
        id = id * 10 + (*p++ - '0');
    }
    unsigned char *body = 0;
    int n = cdp_body_dup(id, &body);
    char *b64 = n >= 0 ? malloc((unsigned)base64_encoded_len(n) + 1) : 0;
    if (!b64) {
      g_cmd_error = "No data found for resource with given identifier";
    } else {
      base64_encode(body, n, b64);
      json_object_put(r, "body", json_new_string(b64));
      json_object_put(r, "base64Encoded", json_new_bool(1));
      free(b64);
    }
    free(body);
    return r;
  }

  if (streq(m, "Target.getTargets")) {
    struct target_infos ti = {0, json_new_array(), 0};
    tabs_each_page(add_target_info, &ti);
    json_object_put(r, "targetInfos", ti.arr);
    return r;
  }
  if (streq(m, "Target.getTargetInfo")) {
    const char *want = json_as_string(json_object_get(params, "targetId"));
    struct target_infos ti = {want ? want : sess->target[0] ? sess->target : B->pg->target_id, 0, 0};
    tabs_each_page(add_target_info, &ti);
    if (ti.one)
      json_object_put(r, "targetInfo", ti.one);
    else
      g_cmd_error = "No target with given id found";
    return r;
  }
  if (streq(m, "Target.getBrowserContexts")) {
    json_object_put(r, "browserContextIds", json_new_array());
    return r;
  }

  if (streq(m, "Browser.getVersion")) {
    json_object_put(r, "protocolVersion", json_new_string("1.3"));
    json_object_put(r, "product", json_new_string("OwOS-Browser/" OWOS_VERSION));
    json_object_put(r, "revision", json_new_string(OWOS_VERSION));
    json_object_put(r, "userAgent", json_new_string(browser_ua()));
    json_object_put(r, "jsVersion", json_new_string("0"));
    return r;
  }
  if (streq(m, "Debugger.enable")) {
    json_object_put(r, "debuggerId", json_new_string("owos-dbg-1"));
    return r;
  }

  return r; /* permissive default: empty result */
}

static void dispatch_one(struct cdp_session *sess, const char *text)
{
  json_error err;
  json_value *req = json_parse(text, -1, &err);
  if (!req)
    return;
  json_value *jid = json_object_get(req, "id");
  const char *method = json_as_string(json_object_get(req, "method"));
  json_value *params = json_object_get(req, "params");
  int has_id = jid && json_typeof(jid) == JSON_NUMBER;
  double id = json_as_number(jid, -1);
  if (method && !sess_on_active(sess) && !starts_ci(method, "Target.") && !starts_ci(method, "Browser.")) {
    if (has_id) /* a background tab has no live DOM or JS to inspect */
      reply_error(sess, id, "Target is not the active tab");
  } else if (method) {
    g_cmd_error = 0;
    json_value *result = handle_method(sess, method, params);
    if (has_id && g_cmd_error) {
      reply_error(sess, id, g_cmd_error);
      json_free(result);
    } else if (has_id)
      reply_result(sess, id, result);
    else
      json_free(result);
  }
  json_free(req);
}

/* --- navigation-lifecycle detection (worker) ------------------------------- *
 * The page state is global, so a change broadcasts to every session that enabled
 * the matching domain (not per-session). */
static char lc_url[URL_MAX];
static int lc_loading = -1, lc_inited;
static void lifecycle_tick(void)
{
  const char *url = B->pg->cur_url;
  if (!lc_inited) {
    strlcpy(lc_url, url, sizeof lc_url);
    lc_loading = B->loading;
    lc_inited = 1;
    return;
  }

  if (!streq(url, lc_url)) {
    strlcpy(lc_url, url, sizeof lc_url);
    broadcast_event(EV_RUNTIME, "Runtime.executionContextsCleared", json_new_object());
    {
      json_value *p = json_new_object();
      json_object_put(p, "frame", build_frame());
      broadcast_event(EV_PAGE, "Page.frameNavigated", p);
    }
    emit_context_created(0); /* 0 -> broadcast to Runtime-enabled sessions */
    broadcast_event(EV_DOM, "DOM.documentUpdated", json_new_object());
  }
  if (B->loading != lc_loading) {
    if (lc_loading == 1 && B->loading == 0) {
      {
        json_value *p = json_new_object();
        json_object_put(p, "timestamp", json_new_number(0));
        broadcast_event(EV_PAGE, "Page.loadEventFired", p);
      }
      broadcast_event(EV_DOM, "DOM.documentUpdated", json_new_object());
    }
    lc_loading = B->loading;
  }
}

void cdp_worker_tick(void)
{
  if (!cdp_nsessions && cdp_pick_sess < 0)
    return;
  lifecycle_tick();

  /* Resolve a CDP inspect-mode pick: the main thread set cdp_pick_commit with the
   * pane offset; resolve it to a DOM node and emit Overlay.inspectNodeRequested.
   * Mirror of devtools_pick_tick, same worker-thread DOM resolver. */
  if (cdp_pick_commit >= 0 && cdp_pick_sess >= 0 && cdp_pick_sess < CDP_MAX_SESSIONS) {
    int off = cdp_pick_commit;
    cdp_pick_commit = -1;
    struct dom_node *n = html_node_at_offset(off);
    if (n && n->type == DOM_TEXT && n->parent)
      n = n->parent;
    if (n) {
      int nid = node_register(n);
      json_value *p = json_new_object();
      json_object_put(p, "backendNodeId", json_new_number(nid));
      json_object_put(p, "nodeId", json_new_number(nid));
      emit_to_session(&sessions[cdp_pick_sess], "Overlay.inspectNodeRequested", p);
    }
    cdp_pick_sess = -1;
  }

  for (int i = 0; i < CDP_MAX_SESSIONS; i++) {
    struct cdp_session *s = &sessions[i];
    if (!s->used)
      continue;
    char *msg;
    int budget = 16;
    while (budget-- > 0 && (msg = sess_in_pop(s))) {
      dispatch_one(s, msg);
      free(msg);
    }
  }
}

/* --- event hooks (any thread) ---------------------------------------------- */
void cdp_console_line(int level, const char *text)
{
  if (!cdp_nsessions || !text)
    return;
  const char *type = level >= 2 ? "error" : (level == 1 ? "warning" : "log");
  json_value *p = json_new_object();
  json_object_put(p, "type", json_new_string(type));
  json_value *args = json_new_array();
  json_value *a0 = json_new_object();
  json_object_put(a0, "type", json_new_string("string"));
  json_object_put(a0, "value", json_new_string(text));
  json_array_push(args, a0);
  json_object_put(p, "args", args);
  json_object_put(p, "executionContextId", json_new_number(1));
  json_object_put(p, "timestamp", json_new_number(0));
  broadcast_event(EV_RUNTIME, "Runtime.consoleAPICalled", p);
}

static void reqid_str(int id, char *out, int cap)
{
  int n = 0;
  sapp(out, &n, cap, "req-");
  uapp(out, &n, cap, (unsigned)id);
}

/* Parse a raw response-header block into a CDP headers object (status line, which
 * has no ':', is naturally skipped). */
/* Parse a raw header block into a CDP headers object, full length; a repeated name (Set-Cookie) joins its values
 * with '\n' as CDP expects. A line without ':' (a status line) is skipped. */
static json_value *headers_obj(const char *raw)
{
  json_value *o = json_new_object();
  if (!raw)
    return o;
  for (const char *p = raw; *p;) {
    const char *e = p;
    while (*e && *e != '\n')
      e++;
    const char *t = e;
    if (t > p && t[-1] == '\r')
      t--;
    const char *c = p;
    while (c < t && *c != ':')
      c++;
    if (c < t && c > p) {
      const char *v = c + 1;
      while (v < t && (*v == ' ' || *v == '\t'))
        v++;
      int kl = (int)(c - p), vl = (int)(t - v);
      char *key = malloc((unsigned)kl + 1);
      if (key) {
        memcpy(key, p, (unsigned)kl);
        key[kl] = '\0';
        const char *prev = json_as_string(json_object_get(o, key));
        int pl = prev ? (int)strlen(prev) + 1 : 0;
        char *val = malloc((unsigned)(pl + vl) + 1);
        if (val) {
          if (prev) {
            memcpy(val, prev, (unsigned)pl - 1);
            val[pl - 1] = '\n';
          }
          memcpy(val + pl, v, (unsigned)vl);
          val[pl + vl] = '\0';
          json_object_set(o, key, json_new_stringn(val, pl + vl));
          free(val);
        }
        free(key);
      }
    }
    p = *e ? e + 1 : e;
  }
  return o;
}
static const char *res_type(const char *ct)
{
  if (!ct || !ct[0])
    return "Other";
  if (contains_ci(ct, "html"))
    return "Document";
  if (contains_ci(ct, "css"))
    return "Stylesheet";
  if (contains_ci(ct, "javascript") || contains_ci(ct, "ecmascript"))
    return "Script";
  if (starts_ci(ct, "image/"))
    return "Image";
  if (contains_ci(ct, "font"))
    return "Font";
  return "Other";
}

/* CDP timestamps are monotonic seconds (a double). DevTools renders "Pending" in
 * the Time column for any request whose duration (loadingFinished − requestWillBeSent)
 * is <= 0, so the begin/finish stamps must be real and strictly increasing. */
static double cdp_now(void) { return (double)msec() / 1000.0; }

/* Remember each request's start stamp so cdp_net_finish can report a positive
 * duration even for the one-shot begin->finish path (which has no elapsed wall
 * time of its own; it carries the measured duration in net_log.ms instead). */
#define CDP_TS_MAX 64
static struct {
  int id;
  double t0;
} cdp_ts[CDP_TS_MAX];
static int cdp_ts_head;
static void cdp_ts_set(int id, double t)
{
  for (int i = 0; i < CDP_TS_MAX; i++)
    if (cdp_ts[i].id == id) {
      cdp_ts[i].t0 = t;
      return;
    }
  cdp_ts[cdp_ts_head].id = id;
  cdp_ts[cdp_ts_head].t0 = t;
  cdp_ts_head = (cdp_ts_head + 1) % CDP_TS_MAX;
}
static double cdp_ts_get(int id) /* -1 if unknown (row evicted from the ring) */
{
  for (int i = 0; i < CDP_TS_MAX; i++)
    if (cdp_ts[i].id == id)
      return cdp_ts[i].t0;
  return -1;
}

void cdp_net_begin(int id, const char *method, const char *url, const char *ctype)
{
  if (!cdp_nsessions)
    return;
  char rid[24];
  reqid_str(id, rid, sizeof rid);
  double t0 = cdp_now();
  cdp_ts_set(id, t0);
  json_value *p = json_new_object();
  json_object_put(p, "requestId", json_new_string(rid));
  json_object_put(p, "loaderId", json_new_string(B->pg->loader_id));
  json_object_put(p, "documentURL", json_new_string(B->pg->cur_url[0] ? B->pg->cur_url : "about:blank"));
  json_value *rq = json_new_object();
  json_object_put(rq, "url", json_new_string(url ? url : ""));
  json_object_put(rq, "method", json_new_string(method ? method : "GET"));
  json_object_put(rq, "headers", json_new_object());
  json_object_put(p, "request", rq);
  json_object_put(p, "timestamp", json_new_number(t0));
  json_object_put(p, "wallTime", json_new_number((double)now_ms() / 1000.0));
  json_value *ini = json_new_object();
  json_object_put(ini, "type", json_new_string("other"));
  json_object_put(p, "initiator", ini);
  json_object_put(p, "type", json_new_string(res_type(ctype)));
  broadcast_event(EV_NETWORK, "Network.requestWillBeSent", p);
}

void cdp_net_finish(int id, const struct net_log *e)
{
  if (!cdp_nsessions || !e)
    return;
  char rid[24];
  reqid_str(id, rid, sizeof rid);

  /* End stamp = start + a strictly-positive elapsed: the real wall time if it's
   * measurable, else the net_log-reported duration, floored so DevTools never sees
   * a zero (which it draws as "Pending"). */
  double t0 = cdp_ts_get(id);
  if (t0 < 0)
    t0 = cdp_now();
  double elapsed = cdp_now() - t0;
  double reported = e->ms > 0 ? (double)e->ms / 1000.0 : 0;
  if (reported > elapsed)
    elapsed = reported;
  if (elapsed < 0.001)
    elapsed = 0.001;
  double tend = t0 + elapsed;

  /* The request headers are only known once the request ran, so they follow as CDP's "extra info"; DevTools drops
   * the event without associatedCookies and connectTiming. The request line before them isn't a header. */
  const char *hdrs = e->req_hdr ? strchr(e->req_hdr, '\n') : 0;
  if (hdrs && hdrs[1]) {
    json_value *xi = json_new_object();
    json_object_put(xi, "requestId", json_new_string(rid));
    json_object_put(xi, "associatedCookies", json_new_array());
    json_object_put(xi, "headers", headers_obj(hdrs + 1));
    json_value *ct = json_new_object();
    json_object_put(ct, "requestTime", json_new_number(t0));
    json_object_put(xi, "connectTiming", ct);
    broadcast_event(EV_NETWORK, "Network.requestWillBeSentExtraInfo", xi);
  }

  if (e->state == NET_FAILED || e->state == NET_CANCELLED) {
    json_value *p = json_new_object();
    json_object_put(p, "requestId", json_new_string(rid));
    json_object_put(p, "timestamp", json_new_number(tend));
    json_object_put(p, "type", json_new_string(res_type(e->ctype)));
    json_object_put(
        p, "errorText",
        json_new_string(e->reason ? e->reason : (e->state == NET_CANCELLED ? "net::ERR_ABORTED" : "net::ERR_FAILED")));
    json_object_put(p, "canceled", json_new_bool(e->state == NET_CANCELLED));
    broadcast_event(EV_NETWORK, "Network.loadingFailed", p);
    return;
  }

  json_value *p = json_new_object();
  json_object_put(p, "requestId", json_new_string(rid));
  json_object_put(p, "loaderId", json_new_string(B->pg->loader_id));
  json_object_put(p, "timestamp", json_new_number(tend));
  json_object_put(p, "type", json_new_string(res_type(e->ctype)));
  json_value *resp = json_new_object();
  json_object_put(resp, "url", json_new_string(e->url ? e->url : ""));
  json_object_put(resp, "status", json_new_number(e->status));
  json_object_put(resp, "statusText", json_new_string(""));
  json_object_put(resp, "mimeType", json_new_string(e->ctype ? e->ctype : ""));
  json_object_put(resp, "headers", headers_obj(e->resp_hdr));
  json_object_put(resp, "encodedDataLength", json_new_number(e->from_cache ? 0 : e->size));
  json_object_put(resp, "fromDiskCache", json_new_bool(e->from_cache));
  json_object_put(p, "response", resp);
  if (e->from_cache) { /* DevTools labels the row "(disk cache)" */
    json_value *sc = json_new_object();
    json_object_put(sc, "requestId", json_new_string(rid));
    broadcast_event(EV_NETWORK, "Network.requestServedFromCache", sc);
  }
  broadcast_event(EV_NETWORK, "Network.responseReceived", p);

  json_value *fin = json_new_object();
  json_object_put(fin, "requestId", json_new_string(rid));
  json_object_put(fin, "timestamp", json_new_number(tend));
  json_object_put(fin, "encodedDataLength", json_new_number(e->from_cache ? 0 : e->size));
  broadcast_event(EV_NETWORK, "Network.loadingFinished", fin);
}

/* --- startup --------------------------------------------------------------- */
static unsigned char cdp_stack[32768] __attribute__((aligned(16)));
static int cdp_started;
void cdp_init(void)
{
  if (cdp_started)
    return;
  cdp_started = 1;
  make_unique_id(g_browser_id);
  thread_create(cdp_io_fn, &cdp_stack[sizeof cdp_stack]);
}
