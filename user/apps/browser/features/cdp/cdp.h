/* cdp.h: a Chrome DevTools Protocol endpoint for the browser.
 *
 * The browser becomes a CDP *target*: a real Chromium DevTools frontend (or
 * Puppeteer / chrome-remote-interface) connects over WebSocket and drives the
 * same DOM / JS / Network capabilities the in-window devtools panel uses, so the
 * heavyweight inspector UI lives in Chromium, not in this window.
 *
 * Wire shape (cdp.c):
 *   - an HTTP discovery endpoint on port 9222 (`/json`, `/json/version`,
 *     `/json/list`) that a client hits to find the debuggable page and its
 *     `webSocketDebuggerUrl`;
 *   - a WebSocket server on `/devtools/page/<id>` carrying JSON-RPC messages.
 *
 * Threading: a dedicated I/O thread owns the socket (WS framing + two byte
 * queues); every protocol command is dispatched on the browser's worker thread
 * (`cdp_worker_tick`) because the DOM and JS engine are worker-owned. Events are
 * formatted and enqueued from whatever thread produced them (console/network
 * hooks): only heap JSON, no engine access, so they are thread-agnostic. */
#ifndef OWOS_BROWSER_CDP_H
#define OWOS_BROWSER_CDP_H

struct net_log; /* browser.h */

/* Start the CDP listener thread (port 9222). Idempotent; call once at startup. */
void cdp_init(void);

/* Worker-thread pump: parse + dispatch any queued protocol commands against the
 * live DOM/JS, and emit navigation-lifecycle events. No-op with no client. */
void cdp_worker_tick(void);

/* True while a DevTools/Puppeteer client is attached; lets the event hooks skip
 * their formatting work when nobody is listening. */
int cdp_connected(void);

/* Event hooks, safe from any thread (they only build + enqueue JSON). `text` is a
 * complete console line (NUL-terminated, no trailing newline). The network pair
 * mirrors the devtools rings: begin logs an in-flight request, finish resolves it
 * from the net_log outcome. */
void cdp_console_line(int level, const char *text);
void cdp_net_begin(int id, const char *method, const char *url, const char *ctype);
void cdp_net_finish(int id, const struct net_log *e);
/* Cache a response body (keyed by the devtools request id) for Network.getResponseBody;
 * a no-op when no client is attached. cdp_net_reset drops the cache (on navigation). */
void cdp_net_body(int id, const void *data, int len, const char *ctype);
void cdp_net_reset(void);

/* Overlay.setInspectMode picker (page→DOM direction, feeds the main-thread click
 * handler). cdp_picking() gates the pane hit-test in input.c alongside the in-window
 * devtools picker. cdp_pick_cancel() disarms on Escape. */
int cdp_picking(void);
void cdp_pick_commit_offset(int off);
void cdp_pick_cancel(void);

#endif /* OWOS_BROWSER_CDP_H */
