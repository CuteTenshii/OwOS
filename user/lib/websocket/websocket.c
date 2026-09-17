/* websocket.c — RFC 6455 client over the net_* sockets. See websocket.h. */
#include "websocket.h"
#include "base64.h"
#include "crypto.h"
#include "netconn.h"
#include "owos.h"

/* Per-frame and per-message payload ceilings. A single frame must fit the rx
 * buffer (we wait for the whole payload before parsing), so the rx buffer is
 * allowed to grow to one frame; a reassembled message has its own cap. Both are
 * generous for a hobby browser and bound a hostile server's memory pull. */
#define WS_MAX_FRAME (1u << 20) /* 1 MiB per frame payload   */
#define WS_MAX_MSG (4u << 20)   /* 4 MiB reassembled message */
#define WS_RX_MAX (WS_MAX_FRAME + 64)

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* RFC 6455 §5.2 opcodes. Opcodes with bit 0x8 set (0x8..0xF) are control
 * frames; WS_OP_CTRL_BIT tests that class — note it equals WS_OP_CLOSE's value
 * by coincidence, it is a mask, not the Close opcode. */
#define WS_OP_CONT 0x0
#define WS_OP_TEXT 0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING 0x9
#define WS_OP_PONG 0xA
#define WS_OP_CTRL_BIT 0x8

/* Internal ws_poll continuation: a control/non-final frame was consumed; keep
 * parsing. Not a public WS_EV_* code. */
#define WS_EV_CONTINUE 100

struct ws_conn {
  netconn *conn; /* TCP (ws) or user-space TLS (wss) transport */
  int state;
  char protocol[64]; /* negotiated subprotocol (or "") */

  uint8_t *rx; /* raw inbound bytes not yet parsed into frames */
  int rxlen, rxcap;

  uint8_t *msg; /* reassembly buffer for the current message    */
  int msglen, msgcap;
  int msg_is_text; /* text vs binary of the in-progress message    */
  int msg_ready;   /* msg holds a delivered message; reset next poll */
  int in_frag;     /* inside a fragmented message                   */

  int close_sent; /* a Close frame has been sent                   */
};

/* ----------------------------------------------------------- small helpers */

/* Sec-WebSocket-Accept = base64(SHA1(client_key + GUID)). */
static void compute_accept(const char *key, char *out /* >=29 */)
{
  char in[64];
  uint8_t digest[SHA1_DIGEST];
  int n = 0, i;
  for (i = 0; key[i] && n < (int)sizeof in - 1; i++)
    in[n++] = key[i];
  for (i = 0; WS_GUID[i] && n < (int)sizeof in - 1; i++)
    in[n++] = WS_GUID[i];
  sha1(in, (uint32_t)n, digest);
  base64_encode(digest, SHA1_DIGEST, out);
}

/* --------------------------------------------------------------- URL parse */

static int ws_parse_url(const char *url, int *tls, char *host, int hostcap, int *port, char *path, int pathcap)
{
  const char *p = url;
  int hi = 0, pi = 0;
  if (eq_ci_n(p, "wss://", 6)) {
    *tls = 1;
    p += 6;
  } else if (eq_ci_n(p, "ws://", 5)) {
    *tls = 0;
    p += 5;
  } else
    return 0;

  while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#') {
    if (hi < hostcap - 1)
      host[hi++] = *p;
    p++;
  }
  host[hi] = 0;
  if (hi == 0)
    return 0;

  *port = *tls ? 443 : 80;
  if (*p == ':') {
    int v = 0;
    p++;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + (*p - '0');
      p++;
    }
    if (v > 0 && v < 65536)
      *port = v;
  }

  if (*p == '/' || *p == '?') {
    if (*p == '?' && pi < pathcap - 1)
      path[pi++] = '/'; /* query with empty path */
    while (*p && *p != '#') {
      if (pi < pathcap - 1)
        path[pi++] = *p;
      p++;
    }
  } else {
    if (pi < pathcap - 1)
      path[pi++] = '/';
  }
  path[pi] = 0;
  return 1;
}

/* ------------------------------------------------------- rx / msg buffers */

static int rx_reserve(ws_conn *c, int extra)
{
  int cap;
  uint8_t *nb;
  if (c->rxlen + extra <= c->rxcap)
    return 1;
  cap = c->rxcap ? c->rxcap : 4096;
  while (cap < c->rxlen + extra)
    cap *= 2;
  if (cap > (int)WS_RX_MAX)
    cap = (int)WS_RX_MAX;
  if (cap < c->rxlen + extra)
    return 0; /* would exceed the frame cap */
  nb = (uint8_t *)realloc(c->rx, (unsigned)cap);
  if (!nb)
    return 0;
  c->rx = nb;
  c->rxcap = cap;
  return 1;
}

static void rx_consume(ws_conn *c, int n)
{
  if (n >= c->rxlen) {
    c->rxlen = 0;
    return;
  }
  memmove(c->rx, c->rx + n, (unsigned)(c->rxlen - n));
  c->rxlen -= n;
}

static int msg_append(ws_conn *c, const uint8_t *data, int n)
{
  if (n < 0)
    return -1;
  if ((unsigned)(c->msglen + n) > WS_MAX_MSG)
    return -1;
  if (c->msglen + n + 1 > c->msgcap) {
    int cap = c->msgcap ? c->msgcap : 1024;
    uint8_t *nb;
    while (cap < c->msglen + n + 1)
      cap *= 2;
    nb = (uint8_t *)realloc(c->msg, (unsigned)cap);
    if (!nb)
      return -1;
    c->msg = nb;
    c->msgcap = cap;
  }
  memcpy(c->msg + c->msglen, data, (unsigned)n);
  c->msglen += n;
  c->msg[c->msglen] = 0; /* NUL for text convenience */
  return 0;
}

/* ------------------------------------------------------------ frame output */

static int ws_send_frame(ws_conn *c, int opcode, const void *data, int len)
{
  uint8_t hdr[14];
  uint8_t mask[4];
  uint8_t *frame;
  const uint8_t *p = (const uint8_t *)data;
  int hn = 0, i, r;
  if (!c || !c->conn)
    return -1;

  hdr[hn++] = (uint8_t)(0x80 | (opcode & 0x0f)); /* FIN + opcode */
  if (len < 126) {
    hdr[hn++] = (uint8_t)(0x80 | len);
  } else if (len < 65536) {
    hdr[hn++] = 0x80 | 126;
    hdr[hn++] = (uint8_t)((len >> 8) & 0xff);
    hdr[hn++] = (uint8_t)(len & 0xff);
  } else {
    hdr[hn++] = 0x80 | 127;
    for (i = 7; i >= 0; i--)
      hdr[hn++] = (uint8_t)(((unsigned)len >> (i * 8)) & 0xff);
  }
  rng_bytes(mask, sizeof mask);
  memcpy(hdr + hn, mask, 4);
  hn += 4;

  frame = (uint8_t *)malloc((unsigned)(hn + len));
  if (!frame)
    return -1;
  memcpy(frame, hdr, (unsigned)hn);
  for (i = 0; i < len; i++)
    frame[hn + i] = (uint8_t)(p[i] ^ mask[i & 3]);
  r = nc_write(c->conn, frame, hn + len);
  free(frame);
  return r < 0 ? -1 : 0;
}

/* ----------------------------------------------------------------- handshake
 */

static int status_is_101(const char *buf, int len)
{
  int i = 0;
  while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n')
    i++;
  while (i < len && buf[i] == ' ')
    i++;
  return i + 2 < len && buf[i] == '1' && buf[i + 1] == '0' && buf[i + 2] == '1';
}

/* Extract the value of header `name` from the response block into `out`. */
static int header_value(const char *buf, int len, const char *name, char *out, int outcap)
{
  int nl = strlen(name), i = 0;
  while (i < len) {
    int ls = i, le, v, o = 0;
    while (i < len && buf[i] != '\n')
      i++;
    le = i;
    if (le > ls && buf[le - 1] == '\r')
      le--;
    if (le - ls > nl + 1 && buf[ls + nl] == ':' && eq_ci_n(buf + ls, name, nl)) {
      v = ls + nl + 1;
      while (v < le && (buf[v] == ' ' || buf[v] == '\t'))
        v++;
      while (v < le && o < outcap - 1)
        out[o++] = buf[v++];
      out[o] = 0;
      return 1;
    }
    i++; /* past the '\n' */
  }
  if (outcap > 0)
    out[0] = 0;
  return 0;
}

/* Read the response until the CRLFCRLF header terminator. On success returns 0
 * with *resp and *resp_len describing the whole buffer, and *body_off the offset just past
 * the terminator (bytes there belong to the WebSocket stream). */
static int read_handshake(ws_conn *c, char **resp, int *resp_len, int *body_off)
{
  int cap = 2048, len = 0, end = -1, i;
  char *buf = (char *)malloc((unsigned)cap);
  if (!buf)
    return -1;
  for (;;) {
    int n;
    if (len + 512 > cap) {
      char *nb;
      cap *= 2;
      if (cap > 65536) {
        free(buf);
        return -1;
      } /* runaway header */
      nb = (char *)realloc(buf, (unsigned)cap);
      if (!nb) {
        free(buf);
        return -1;
      }
      buf = nb;
    }
    n = nc_read(c->conn, buf + len, cap - len);
    if (n <= 0) {
      free(buf);
      return -1;
    }
    len += n;
    for (i = (len - n >= 3 ? len - n - 3 : 0); i + 3 < len; i++) {
      if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
        end = i + 4;
        break;
      }
    }
    if (end >= 0)
      break;
  }
  *resp = buf;
  *resp_len = len;
  *body_off = end;
  return 0;
}

ws_conn *ws_open(const char *url, const char *protocols)
{
  int tls = 0, port = 0;
  char host[256], path[600];
  char req[2048];
  char key[28], accept[40], got[40];
  uint8_t keybytes[16];
  int rn = 0;
  ws_conn *c;
  char *resp = 0;
  int resp_len = 0, body_off = 0;

  if (!ws_parse_url(url, &tls, host, sizeof host, &port, path, sizeof path))
    return 0;

  c = (ws_conn *)calloc(1, sizeof *c);
  if (!c)
    return 0;
  c->conn = 0;
  c->state = WS_CONNECTING;
  c->protocol[0] = 0;
  c->conn = nc_open(host, port, tls, 0);
  if (!c->conn) {
    free(c);
    return 0;
  }

  rng_bytes(keybytes, sizeof keybytes);
  base64_encode(keybytes, 16, key);

  /* Build the Upgrade request. */
  {
    char portbuf[8];
    int n = 0;
#define PUT(s)                                                                                                         \
  do {                                                                                                                 \
    const char *_s = (s);                                                                                              \
    while (*_s && n < (int)sizeof req - 1)                                                                             \
      req[n++] = *_s++;                                                                                                \
  } while (0)
    PUT("GET ");
    PUT(path);
    PUT(" HTTP/1.1\r\nHost: ");
    PUT(host);
    if (!((tls && port == 443) || (!tls && port == 80))) {
      int v = port, k = 0;
      char tmp[8];
      if (v == 0)
        tmp[k++] = '0';
      while (v > 0 && k < (int)sizeof tmp) {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
      }
      portbuf[0] = ':';
      {
        int j;
        for (j = 0; j < k; j++)
          portbuf[1 + j] = tmp[k - 1 - j];
        portbuf[1 + k] = 0;
      }
      PUT(portbuf);
    }
    PUT("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    PUT(key);
    PUT("\r\nSec-WebSocket-Version: 13\r\n");
    if (protocols && *protocols) {
      PUT("Sec-WebSocket-Protocol: ");
      PUT(protocols);
      PUT("\r\n");
    }
    PUT("Origin: ");
    PUT(tls ? "https://" : "http://");
    PUT(host);
    PUT("\r\nUser-Agent: OwOS\r\n\r\n");
#undef PUT
    rn = n;
  }

  if (nc_write(c->conn, req, rn) < 0) {
    nc_close(c->conn);
    free(c);
    return 0;
  }

  if (read_handshake(c, &resp, &resp_len, &body_off) != 0) {
    nc_close(c->conn);
    free(c);
    return 0;
  }
  if (!status_is_101(resp, resp_len)) {
    free(resp);
    nc_close(c->conn);
    free(c);
    return 0;
  }

  compute_accept(key, accept);
  if (!header_value(resp, body_off, "Sec-WebSocket-Accept", got, sizeof got) || strlen(got) != strlen(accept) ||
      !eq_ci_n(got, accept, strlen(accept))) {
    free(resp);
    nc_close(c->conn);
    free(c);
    return 0;
  }
  header_value(resp, body_off, "Sec-WebSocket-Protocol", c->protocol, sizeof c->protocol);

  /* Any bytes past the header are the start of the WebSocket stream. */
  if (resp_len > body_off) {
    int extra = resp_len - body_off;
    if (rx_reserve(c, extra)) {
      memcpy(c->rx + c->rxlen, resp + body_off, (unsigned)extra);
      c->rxlen += extra;
    }
  }
  free(resp);

  c->state = WS_OPEN;
  return c;
}

/* ----------------------------------------------------------------- polling */

/* Parse and act on one frame at the front of rx. Returns a WS_EV_* code, or the
 * internal WS_EV_CONTINUE when a control/non-final frame was consumed. Leaves
 * rx untouched (returns WS_EV_NONE) when a whole frame isn't buffered yet. */
static int ws_parse_one(ws_conn *c, const uint8_t **out, int *outlen, int *is_text)
{
  uint8_t *b = c->rx;
  int fin, opcode, masked, hn = 2, total, ret, i;
  uint8_t mk[4] = {0, 0, 0, 0};
  long long plen;
  uint8_t *payload;

  if (c->rxlen < 2)
    return WS_EV_NONE;
  fin = b[0] & 0x80;
  opcode = b[0] & 0x0f;
  masked = b[1] & 0x80;
  plen = b[1] & 0x7f;
  if (plen == 126) {
    if (c->rxlen < 4)
      return WS_EV_NONE;
    plen = ((long long)b[2] << 8) | b[3];
    hn = 4;
  } else if (plen == 127) {
    if (c->rxlen < 10)
      return WS_EV_NONE;
    plen = 0;
    for (i = 2; i < 10; i++)
      plen = (plen << 8) | b[i];
    hn = 10;
  }
  if (plen < 0 || plen > WS_MAX_FRAME)
    return WS_EV_ERROR;
  /* RFC 6455 §5.5: control frames carry ≤125 bytes and are never fragmented.
   * Enforce it so a hostile server can't force a multi-hundred-KiB
   * allocate-mask-echo per ping. */
  if (opcode & WS_OP_CTRL_BIT) {
    if (plen > 125 || !fin)
      return WS_EV_ERROR;
  }
  if (masked) {
    if (c->rxlen < hn + 4)
      return WS_EV_NONE;
    memcpy(mk, b + hn, 4);
    hn += 4;
  }
  if ((long long)c->rxlen < (long long)hn + plen)
    return WS_EV_NONE; /* await full payload */

  payload = b + hn;
  if (masked)
    for (i = 0; i < (int)plen; i++)
      payload[i] ^= mk[i & 3];
  total = hn + (int)plen;

  switch (opcode) {
  case WS_OP_PING: /* ping -> pong */
    ws_send_frame(c, WS_OP_PONG, payload, (int)plen);
    ret = WS_EV_CONTINUE;
    break;
  case WS_OP_PONG: /* pong: ignore */
    ret = WS_EV_CONTINUE;
    break;
  case WS_OP_CLOSE: /* close */
    if (!c->close_sent) {
      ws_send_frame(c, WS_OP_CLOSE, payload, (int)(plen <= 125 ? plen : 0));
      c->close_sent = 1;
    }
    ret = WS_EV_CLOSE;
    break;
  case WS_OP_TEXT:   /* text (start) */
  case WS_OP_BINARY: /* binary (start) */
    c->msglen = 0;
    c->in_frag = 1;
    c->msg_is_text = (opcode == WS_OP_TEXT);
    if (msg_append(c, payload, (int)plen) != 0) {
      ret = WS_EV_ERROR;
      break;
    }
    ret = WS_EV_CONTINUE;
    goto maybe_deliver;
  case WS_OP_CONT: /* continuation */
    if (msg_append(c, payload, (int)plen) != 0) {
      ret = WS_EV_ERROR;
      break;
    }
    ret = WS_EV_CONTINUE;
  maybe_deliver:
    if (fin) {
      c->in_frag = 0;
      c->msg_ready = 1;
      if (out)
        *out = c->msg;
      if (outlen)
        *outlen = c->msglen;
      if (is_text)
        *is_text = c->msg_is_text;
      ret = WS_EV_MESSAGE;
    }
    break;
  default: /* unknown opcode: skip */
    ret = WS_EV_CONTINUE;
    break;
  }

  rx_consume(c, total);
  return ret;
}

int ws_poll(ws_conn *c, const uint8_t **out, int *outlen, int *is_text)
{
  if (out)
    *out = 0;
  if (outlen)
    *outlen = 0;
  if (is_text)
    *is_text = 0;
  if (!c)
    return WS_EV_ERROR;
  if (c->state == WS_CLOSED)
    return WS_EV_NONE;

  if (c->msg_ready) {
    c->msg_ready = 0;
    c->msglen = 0;
  } /* release prior message */

  for (;;) { /* drain the socket */
    int room, n;
    if (!rx_reserve(c, 4096))
      break; /* at the frame cap: parse what we have */
    room = c->rxcap - c->rxlen;
    if (room <= 0)
      break;
    n = nc_read_nb(c->conn, c->rx + c->rxlen, room);
    if (n > 0) {
      c->rxlen += n;
      continue;
    }
    if (n == 0)
      break;              /* would-block */
    c->state = WS_CLOSED; /* EOF / error / interrupted */
    return WS_EV_CLOSE;
  }

  for (;;) {
    int ev = ws_parse_one(c, out, outlen, is_text);
    if (ev == WS_EV_MESSAGE)
      return WS_EV_MESSAGE;
    if (ev == WS_EV_CLOSE) {
      c->state = WS_CLOSED;
      return WS_EV_CLOSE;
    }
    if (ev == WS_EV_ERROR) {
      c->state = WS_CLOSED;
      return WS_EV_ERROR;
    }
    if (ev == WS_EV_NONE)
      return WS_EV_NONE;
    /* WS_EV_CONTINUE: keep parsing buffered frames */
  }
}

/* ------------------------------------------------------------- public ops */

int ws_send(ws_conn *c, const void *data, int len, int is_text)
{
  if (!c || c->state != WS_OPEN || len < 0)
    return -1;
  return ws_send_frame(c, is_text ? WS_OP_TEXT : WS_OP_BINARY, data, len);
}

void ws_close(ws_conn *c, int code, const char *reason)
{
  uint8_t payload[125];
  int pl = 0;
  if (!c || c->state == WS_CLOSED)
    return;
  if (!c->close_sent) {
    if (code) {
      payload[pl++] = (uint8_t)((code >> 8) & 0xff);
      payload[pl++] = (uint8_t)(code & 0xff);
      if (reason) {
        int i, rl = strlen(reason);
        for (i = 0; i < rl && pl < 125; i++)
          payload[pl++] = (uint8_t)reason[i];
      }
    }
    ws_send_frame(c, WS_OP_CLOSE, payload, pl);
    c->close_sent = 1;
  }
  c->state = WS_CLOSING;
}

int ws_state(const ws_conn *c) { return c ? c->state : WS_CLOSED; }

const char *ws_protocol(const ws_conn *c) { return c ? c->protocol : ""; }

void ws_free(ws_conn *c)
{
  if (!c)
    return;
  if (c->conn)
    nc_close(c->conn);
  if (c->rx)
    free(c->rx);
  if (c->msg)
    free(c->msg);
  free(c);
}
