/* websocket.h — a freestanding RFC 6455 WebSocket client, user space.
 *
 * Layered over the net_* socket API (owos.h): ws_open does the TCP/TLS connect
 * and the HTTP Upgrade handshake (blocking, one round trip), then the link is
 * driven non-blocking — ws_poll reads/reassembles frames and answers control
 * frames, ws_send writes masked frames. The first consumer is the browser's JS
 * WebSocket binding, but nothing here knows about JS: it is a plain client.
 *
 * Reuses crypto.h's SHA-1 for the Sec-WebSocket-Accept check; base64 and the
 * frame-masking RNG are local. Cert verification is whatever net_connect's TLS
 * provides (passive-eavesdrop resistance, not active MITM), same as the rest of
 * the stack.
 */
#ifndef OWOS_WEBSOCKET_H
#define OWOS_WEBSOCKET_H

#include <stdint.h>

/* readyState values, mirroring the WHATWG WebSocket interface. */
enum {
  WS_CONNECTING = 0,
  WS_OPEN = 1,
  WS_CLOSING = 2,
  WS_CLOSED = 3,
};

/* ws_poll result codes. */
enum {
  WS_EV_NONE = 0, /* nothing actionable this poll (drained)                  */
  WS_EV_MESSAGE,  /* a complete message is available (see ws_poll out params)*/
  WS_EV_CLOSE,    /* the peer closed / close handshake finished; conn is done*/
  WS_EV_ERROR,    /* transport or protocol error; conn is done               */
};

typedef struct ws_conn ws_conn;

/* Open a WebSocket to `url` (ws:// or wss://). `protocols` is an optional
 * comma-separated Sec-WebSocket-Protocol list (NULL or "" for none). Performs
 * the connect and Upgrade handshake; returns a connection in state WS_OPEN, or
 * NULL on any failure (bad URL, connect refused, handshake rejected). */
ws_conn *ws_open(const char *url, const char *protocols);

/* Non-blocking poll. Drains the socket, reassembles frames, and answers ping /
 * close control frames. Returns a WS_EV_* code:
 *   WS_EV_MESSAGE -> *out points at the message bytes (owned by the conn, valid
 *                    until the next ws_poll), *outlen its length, *is_text 1
 * for a text message / 0 for binary. WS_EV_CLOSE / WS_EV_ERROR -> the conn is
 * finished; ws_free it. Call repeatedly until WS_EV_NONE to drain every
 * buffered message. The out pointers may be NULL if the caller doesn't need
 * them. */
int ws_poll(ws_conn *c, const uint8_t **out, int *outlen, int *is_text);

/* Send one message. is_text != 0 sends a text frame, 0 a binary frame. The
 * payload is copied and masked per RFC 6455. Returns 0, or -1 on error. */
int ws_send(ws_conn *c, const void *data, int len, int is_text);

/* Begin a close handshake: send a Close frame carrying `code` (0 = omit) and an
 * optional `reason`. The conn moves to WS_CLOSING; a later ws_poll reports
 * WS_EV_CLOSE once the peer's Close arrives or the link drops. */
void ws_close(ws_conn *c, int code, const char *reason);

int ws_state(const ws_conn *c);            /* WS_CONNECTING/OPEN/CLOSING/CLOSED */
const char *ws_protocol(const ws_conn *c); /* negotiated subprotocol, or "" */

/* Close the socket and free every buffer. Safe on a NULL conn. */
void ws_free(ws_conn *c);

#endif /* OWOS_WEBSOCKET_H */
