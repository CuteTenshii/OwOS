/* jsint.h , internal shared surface for the browser's JavaScript bindings. js.c holds the core */
#ifndef BROWSER_JSINT_H
#define BROWSER_JSINT_H

#include "browser.h" /* owos.h, dom.h (DOM + lower/streq_ci), struct browser */
#include "gc.h"      /* gc_add_root / gc_remove_root / scopes                */
#include "interp.h"  /* js_ctx, js_run, js_call, js_ctx_new, js_def_native   */
#include "parser.h"  /* js_parse, js_arena                                   */

/* Nth call argument or undefined */
#define A(i) ((i) < argc ? args[i] : js_undef())

/* ---- shared per-page state (defined in js.c) ---------------------------- */
extern js_ctx *g_ctx;               /* the engine context for the whole current page  */
extern int g_page_live;             /* a page context is currently alive              */
extern js_object *g_response_proto; /* fetch() Response.prototype (fetch + SW share) */
extern char g_page_url_buf[];       /* the current page URL (module + SW referrer) */
extern js_object *g_media_proto;
extern js_object *g_remote_proto; /* RemotePlayback.prototype (media.remote) */
extern js_object *g_remotes;      /* rooted array of live RemotePlayback wrappers */

/* ---- shared low-level helpers (defined in js.c) ------------------------- */

/* args[] -> a borrowed C string (coerces via ToString; "" for non-strings) */
const char *arg_str(js_ctx *ctx, js_value v);

/* the DOM node a wrapper object fronts (NULL if not a node wrapper), and the cached wrapper */
struct dom_node *node_of(js_value v);
js_value wrap_node(struct dom_node *n);

/* define a method / accessor pair on a prototype object */
void def_method(js_object *o, const char *name, js_native fn, int arity);
void def_acc(js_object *proto, const char *name, js_native get, js_native set);

/* EventTarget machinery */
js_value el_add_listener(js_ctx *ctx, js_value t, js_value *args, int argc);
js_value el_remove_listener(js_ctx *ctx, js_value t, js_value *args, int argc);
js_value el_dispatch(js_ctx *ctx, js_value t, js_value *args, int argc);
int ev_flag(js_value ev, const char *k);
void ev_fire_on(js_object *obj, js_value ev, int want_capture);
js_value make_event(const char *type, int x, int y, js_value init);

/* a growable byte buffer (NUL-terminated); realloc-backed, caller frees s->b */
typedef struct {
  char *b;
  int n, cap;
  int failed;
} sbuf;
void sb_putc(sbuf *s, char c);
void sb_puts(sbuf *s, const char *t);
void sb_putn(sbuf *s, const char *t, int n);

/* bytes backing an ArrayBuffer / typed-array / DataView view (NULL if none) */
const unsigned char *view_bytes(js_value v, int *len);

/* expose an interface prototype's constructor as a global on `win` */
void define_iface(js_ctx *ctx, js_object *win, const char *name, js_object *proto);

/* ---- shared helpers used by the service-worker module (defined in js.c) --- */
int arenas_push(js_arena *ar);                                       /* keep a script's AST arena alive  */
js_value el_noop(js_ctx *ctx, js_value t, js_value *args, int argc); /* no-op native */
js_value hdr_get(js_ctx *ctx, js_value t, js_value *args, int argc); /* Headers.get   */
js_value hdr_has(js_ctx *ctx, js_value t, js_value *args, int argc); /* Headers.has   */
void js_set_cur_script(const char *label);                           /* current script name for exc attribution */
js_value make_message_event(js_value data);                          /* a MessageEvent carrying `data` */
js_object *make_response(js_ctx *ctx, const char *url, int status, const char *statustext, const char *ctype,
                         const char *body, int len); /* fetch Response */
void notif_app_name(char *buf, int cap);             /* the app name for a Notification  */
int perm_state_now(int feature);                     /* current permission state (PERM_*) */
const char *perm_state_str(int state);
int worker_permission(int feature);
void report_exc(js_ctx *ctx);                                          /* print + clear a pending exception */
js_value win_fetch(js_ctx *ctx, js_value t, js_value *args, int argc); /* window.fetch */
void emit_event(js_object *w, const char *type, js_value data);        /* fire a plain Event on w */

/* ---- subsystem setup entry points --------------------------------------- */
void js_idb_setup(js_ctx *ctx, js_object *win);     /* IndexedDB (js_indexeddb.c) */
void js_idb_teardown(void);                         /* unroot the page's IDB state  */
void js_webapi_setup(js_ctx *ctx, js_object *win);  /* performance + Encoding (js_webapi.c) */
void js_streams_setup(js_ctx *ctx, js_object *win); /* Streams API, lazily compiled (js/streams.c) */
enum { CONSUME_BUFFER, CONSUME_TEXT, CONSUME_BLOB, CONSUME_JSON };
/* A Response body init as raw bytes in a string. A ReadableStream is returned through *stream instead; ctype is
 * overwritten for Blob, BufferSource and stream bodies. */
js_value response_init_body(js_ctx *ctx, js_value body, js_value *stream, char *ctype, int ctcap);
/* Read a Response body to the end: a promise of an ArrayBuffer, string, Blob or parsed JSON value */
js_value response_consume(js_ctx *ctx, js_value resp, int kind);
js_value response_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc); /* new Response(body, init) */
void js_webapi_teardown(void);                                                 /* unroot the Encoding-API prototypes  */

/* HTMLMediaElement + Remote Playback + new Image()/Audio() (js_media.c) */
void setup_media_proto(js_ctx *ctx);
void setup_remote_proto(js_ctx *ctx);
js_value image_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc);
void js_cast_page_end(void); /* tear the page's Cast session down   */

/* Service workers (js_serviceworker.c) */
void sw_install(js_ctx *ctx, js_object *win, const char *page_url);
void sw_teardown(void); /* drop the per-page SW JS objects */
int sw_intercept_fetch(js_ctx *ctx, const char *url, const char *method, js_value *out);

#endif /* BROWSER_JSINT_H */
