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
extern const char *const fetch_cache_names[]; /* indexed by enum http_cache_mode */
int fetch_cache_mode(js_ctx *ctx, js_value input, js_value init, const char **err);
extern char g_page_url_buf[];       /* the current page URL (module + SW referrer) */
extern js_object *g_media_proto;
extern js_object *g_remote_proto; /* RemotePlayback.prototype (media.remote) */
extern js_object *g_remotes;      /* rooted array of live RemotePlayback wrappers */

/* ---- shared low-level helpers (defined in js.c) ------------------------- */

/* args[] -> a borrowed C string (coerces via ToString; "" for non-strings) */
const char *arg_str(js_ctx *ctx, js_value v);
js_value dom_exception(js_ctx *ctx, const char *name, const char *msg); /* a DOMException value (not thrown) */
js_value dom_throw(js_ctx *ctx, const char *name, const char *msg);     /* throw one; returns undefined */
int text_decoder_kind(const char *label);                               /* TextDecoder label -> DEC_* kind */
const char *text_decoder_name(int kind);                                /* the canonical encoding name of a kind */
js_value text_decode(js_ctx *ctx, js_value view, int kind, bool ignore_bom, bool fatal); /* bytes -> string */

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
void event_adopt_iface(js_value ev, const char *iface); /* give ev the ErrorEvent/MessageEvent prototype */

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
js_value make_message_event(js_value data, js_value ports); /* a MessageEvent carrying `data` and the array `ports` */
js_object *make_response(js_ctx *ctx, const char *url, int status, const char *statustext, const char *ctype,
                         const char *body, int len); /* fetch Response */
void notif_app_name(char *buf, int cap);             /* the app name for a Notification  */
int perm_state_now(int feature);                     /* current permission state (PERM_*) */
const char *perm_state_str(int state);
int worker_permission(int feature);
int worker_dialog(int kind, const char *msg, const char *deflt, char *out, int outsz);
void report_exc(js_ctx *ctx);                                          /* print + clear a pending exception */
js_value win_fetch(js_ctx *ctx, js_value t, js_value *args, int argc); /* window.fetch */
void emit_event(js_object *w, const char *type, js_value data);        /* fire a plain Event on w */

/* ---- subsystem setup entry points --------------------------------------- */
void js_idb_setup(js_ctx *ctx, js_object *win);     /* IndexedDB (js_indexeddb.c) */
void js_idb_teardown(void);                         /* unroot the page's IDB state  */
void js_webapi_setup(js_ctx *ctx, js_object *win);  /* performance + Encoding (js_webapi.c) */
void js_streams_setup(js_ctx *ctx, js_object *win); /* Streams API, lazily compiled (js/streams.c) */
void js_filedata_setup(js_ctx *ctx, js_object *win); /* FileReader and DataTransfer, lazily compiled (js/filedata.c) */
void js_webaudio_setup(js_ctx *ctx,
                       js_object *win); /* AudioContext + AudioWorkletNode, lazily compiled (js/webaudio.c) */
js_value js_audio_thread(js_ctx *ctx);  /* a hidden worker running an AudioWorkletGlobalScope */
/* the AudioWorkletGlobalScope on its worker thread (js/audioworklet.c, hooks in js/webworker.c) */
void js_worklet_install(js_ctx *ctx);
bool js_worklet_render(js_ctx *ctx);
void js_worklet_end(void);

void js_worklet_post(js_ctx *ctx, js_value v);
int js_worklet_add_module(js_ctx *ctx, const char *spec);
js_value js_streams_run(js_ctx *ctx, js_arena **arena); /* the Streams classes as an exports object, for any context */
enum { CONSUME_BUFFER, CONSUME_TEXT, CONSUME_BLOB, CONSUME_JSON };
/* A Response body init as raw bytes in a string. A ReadableStream is returned through *stream instead; ctype is
 * overwritten for Blob, BufferSource and stream bodies. */
js_value response_init_body(js_ctx *ctx, js_value body, js_value *stream, char *ctype, int ctcap);
/* Read a Response body to the end: a promise of an ArrayBuffer, string, Blob or parsed JSON value */
js_value response_consume(js_ctx *ctx, js_value resp, int kind);
js_value response_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc); /* new Response(body, init) */
void js_webapi_teardown(void);                                                 /* unroot the Encoding-API prototypes  */

/* A global observer class: ctor native plus its prototype (rooted), from js/webapi.c */
js_object *observer_class(js_ctx *ctx, js_object *win, const char *name, js_native ctor, js_object **proto);

/* HTMLMediaElement + Remote Playback + new Image()/Audio() (js_media.c) */
void setup_media_proto(js_ctx *ctx);
void setup_remote_proto(js_ctx *ctx);
js_value image_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc);
void js_cast_toggle(struct dom_node *n); /* controls-bar cast button: connect or disconnect */
void js_cast_page_end(void); /* tear the page's Cast session down   */

/* Service workers (js_serviceworker.c) */
void sw_install(js_ctx *ctx, js_object *win, const char *page_url);
void sw_teardown(void);  /* drop the per-page SW JS objects */
int sw_cache_pump(void); /* write Cache API changes to disk now and then */
void sw_cache_worker_setup(js_ctx *ctx, js_object *n,
                           const char *origin); /* the natives a worker's Cache API stands on */
int sw_intercept_fetch(js_ctx *ctx, const char *url, const char *method, js_value *out);

/* Dedicated workers (js/webworker.c) */
void js_worker_setup(js_ctx *ctx, js_object *win);
void js_worker_teardown(void);
void js_port_enqueue(js_value port, js_value data, js_value ports); /* async message delivery to a MessagePort */
int js_on_worker_thread(void);
struct wthread;
void js_bc_dispatch(const char *name, js_clone *c, js_object *sender,
                    struct wthread *from); /* BroadcastChannel fan-out */
int js_local_clone(js_ctx *ctx, js_value v, js_value transfer, js_value *data,
                   js_value *ports); /* a clone inside this context */
void js_msgchannel_setup(js_ctx *ctx, js_object *win);
int js_is_port(js_value v);          /* a MessagePort of the page */
js_object *js_new_port(js_ctx *ctx); /* an unattached MessagePort */
/* Ports whose other end is in a worker (js/webworker.c): posting and closing go across the thread boundary */
void js_port_remote_post(js_ctx *ctx, js_value port, js_value data, js_value transfer);
void js_port_remote_close(js_ctx *ctx, js_value port);
int worker_port_write(js_ctx *ctx, js_value v, char kind[16], unsigned char **bytes, int *len);
js_value worker_port_read(js_ctx *ctx, const char *kind, const unsigned char *bytes, int len);
void js_msgchannel_teardown(void);

#endif /* BROWSER_JSINT_H */
