/* js.c — wires the OwOS JavaScript engine (user/lib/js) into the browser.
 *
 * After the DOM is parsed, html.c calls js_run_document(): it spins up a fresh
 * engine context, installs a small `document`/`window` binding layer over the
 * browser's DOM (browser/dom.h), and runs every inline <script> in document
 * order sharing one global. Scripts run BEFORE layout, so any DOM they mutate
 * is reflected when html.c walks the tree to produce the styled page.
 *
 * A DOM element is exposed to JS as a wrapper object whose `host` slot (the
 * js_object embedder hook) points at the struct dom_node. Wrappers are created
 * fresh per access, so node *identity* across separate lookups is not preserved
 * yet — a known first-cut simplification. console output goes to stdout (fd 1);
 * timers (setTimeout/...) are accepted but do not fire (no event loop yet).
 */
#include "../bmedia.h" /* HTMLMediaElement: <audio>/<video> + new Audio()      */
#include "../cors.h"   /* cors_origin / cors_same_origin: SW scope + cache keys */
#include "../jsint.h"  /* internal shared surface (engine ctx + helpers + A())  */
#include "cast.h"      /* Google Cast sender: HTMLMediaElement.remote backend  */
#include "classlist.h"
#include "clipboard.h" /* clip_put / clip_get for navigator.clipboard          */
#include "dom_query.h"
#include "notification.h"
#include "notify.h"    /* notify_send: surface Notification() on the desktop    */
#include "websocket.h" /* ws_open / ws_poll / ws_send / ws_close (RFC 6455)    */

js_ctx *g_ctx;
static char *sdup(const char *s);
static js_object *g_node_proto; /* Node.prototype (ancestor of element/document) */
static js_object *g_element_proto;
static js_object *g_style_proto;
static js_object *g_classlist_proto; /* DOMTokenList.prototype (element.classList) */
static js_object *g_url_proto;
static js_object *g_sp_proto;
static js_object *g_formdata_proto; /* FormData.prototype (shares URLSearchParams' @k/@v) */
static js_object *g_event_proto;
static js_object *g_html_proto; /* HTMLElement.prototype (chains to element) */
js_object *g_media_proto;       /* HTMLMediaElement.prototype (audio/video)  */

/* Standard HTMLxxxElement interfaces: the interface name, the space-separated
 * tag list it wraps, and (filled at setup) its prototype. Each is exposed as a
 * global constructor so scripts can reference/extend it (e.g. `class X extends
 * HTMLScriptElement`), and wrap_node gives a matching element its interface
 * prototype so `el instanceof HTMLScriptElement` holds. Every prototype chains
 * to HTMLElement.prototype; tags not listed here wrap as a plain HTMLElement.
 */
struct html_iface {
  const char *name;
  const char *tags;
  int media;
  js_object *proto;
};
static struct html_iface g_html_ifaces[] = {
    /* `media` = 1 chains to HTMLMediaElement.prototype instead of
     * HTMLElement's, so <audio>/<video> inherit the media methods and
     * instanceof reflects the HTMLMediaElement base. Everything else chains
     * straight to HTMLElement. */
    {"HTMLScriptElement", "script", 0, 0},
    {"HTMLAnchorElement", "a", 0, 0},
    {"HTMLImageElement", "img", 0, 0},
    {"HTMLInputElement", "input", 0, 0},
    {"HTMLButtonElement", "button", 0, 0},
    {"HTMLFormElement", "form", 0, 0},
    {"HTMLDivElement", "div", 0, 0},
    {"HTMLSpanElement", "span", 0, 0},
    {"HTMLParagraphElement", "p", 0, 0},
    {"HTMLHeadingElement", "h1 h2 h3 h4 h5 h6", 0, 0},
    {"HTMLUListElement", "ul", 0, 0},
    {"HTMLOListElement", "ol", 0, 0},
    {"HTMLLIElement", "li", 0, 0},
    {"HTMLLinkElement", "link", 0, 0},
    {"HTMLStyleElement", "style", 0, 0},
    {"HTMLMetaElement", "meta", 0, 0},
    {"HTMLTitleElement", "title", 0, 0},
    {"HTMLHeadElement", "head", 0, 0},
    {"HTMLBodyElement", "body", 0, 0},
    {"HTMLHtmlElement", "html", 0, 0},
    {"HTMLIFrameElement", "iframe", 0, 0},
    {"HTMLCanvasElement", "canvas", 0, 0},
    {"HTMLSelectElement", "select", 0, 0},
    {"HTMLOptionElement", "option", 0, 0},
    {"HTMLTextAreaElement", "textarea", 0, 0},
    {"HTMLLabelElement", "label", 0, 0},
    {"HTMLTableElement", "table", 0, 0},
    {"HTMLTableRowElement", "tr", 0, 0},
    {"HTMLTableCellElement", "td th", 0, 0},
    {"HTMLTableSectionElement", "thead tbody tfoot", 0, 0},
    {"HTMLPreElement", "pre", 0, 0},
    {"HTMLBRElement", "br", 0, 0},
    {"HTMLHRElement", "hr", 0, 0},
    {"HTMLSourceElement", "source", 0, 0},
    {"HTMLTemplateElement", "template", 0, 0},
    {"HTMLPictureElement", "picture", 0, 0},
    {"HTMLDataListElement", "datalist", 0, 0},
    {"HTMLProgressElement", "progress", 0, 0},
    {"HTMLObjectElement", "object", 0, 0},
    {"HTMLVideoElement", "video", 1, 0},
    {"HTMLAudioElement", "audio", 1, 0},
};
static js_object *g_custom_elements;       /* registry: tag name -> class constructor   */
static js_object *g_module_cache;          /* ES module registry: abs URL -> namespace */
static js_object *g_module_src;            /* prefetched module sources: abs URL -> text */
static js_object *g_document_proto;        /* shared by the live doc + DOMParser docs    */
js_object *g_response_proto;               /* fetch() Response.prototype                  */
static js_object *g_ws_proto;              /* WebSocket.prototype                          */
static js_object *g_xhr_proto;             /* XMLHttpRequest.prototype */
static js_object *g_blob_proto;            /* Blob.prototype */
static js_object *g_clipitem_proto;        /* ClipboardItem.prototype */
js_object *g_remote_proto;                 /* RemotePlayback.prototype (media.remote)      */
js_object *g_remotes;                      /* rooted array of live RemotePlayback wrappers  */
static js_object *g_websockets;            /* rooted array of live WebSocket wrappers */
static js_object *g_bchannel_proto;        /* BroadcastChannel.prototype */
static js_object *g_bchannels;             /* rooted array of open BroadcastChannel wrappers */
static js_object *g_notification_proto;    /* Notification.prototype */
static js_object *g_notifications;         /* rooted array of wrappers pending show/close   */
static js_object *g_abortsignal_proto;     /* AbortSignal.prototype */
static js_object *g_abortcontroller_proto; /* AbortController.prototype */
static js_object *g_rtc_proto;             /* RTCPeerConnection.prototype (inert stub) */
static js_object *g_document;
static struct dom_node *g_doc_root;
static struct dom_node *g_parsed[64]; /* DOMParser trees, freed at page teardown    */
static int g_nparsed;
static res_batch_fn g_fetch;  /* the batch fetch used to pull modules       */
char g_page_url_buf[URL_MAX]; /* the page URL (module-resolution referrer)  */

/* The JS context now lives for the whole page (not just the initial script
 * run), so registered listeners + closures survive until the next navigation.
 * These hold that persistent state; html_page_teardown() reclaims it. */
static js_object *g_live_wrappers; /* rooted array pinning every node wrapper */
static int g_has_listeners;        /* any addEventListener fired this page      */
int g_page_live;                   /* a page context is currently alive         */
static js_arena **g_arenas;        /* per-script AST arenas (closures need them); grows */
static int g_narenas, g_arenas_cap;

/* Timer queue (setTimeout/setInterval/requestAnimationFrame). The engine has no
 * event loop, so timers are driven cooperatively: the worker calls
 * js_timers_pump each idle tick, which fires every timer whose deadline has
 * passed. Slot i holds a callback+extra-args bundle in the rooted array
 * g_timers (so closures stay GC- live); the parallel g_tmeta[] carries the
 * scheduling. A timer id is slot+1. The table grows on demand (a page may arm
 * any number of timers); cleared one-shots free their slot, so it settles at
 * the page's peak concurrent-timer count. */
static js_object *g_timers; /* rooted: slot -> [callback, ...args] or null */
static struct timer_meta {
  unsigned char active, repeat, raf;
  long long due, period; /* ms; due is the next fire time (now_ms scale) */
  char *async;           /* frames captured at the arming call site, so a
                          * throw in the callback names who scheduled it
                          * (js_async_capture; freed in timer_drop)       */
} *g_tmeta;
static int g_tcap;    /* allocated slots in g_tmeta                  */
static int g_ntimers; /* active count (fast empty-check for the pump) */

/* ------------------------------------------------------- small buffer ----- */

static int sb_need(sbuf *s, int extra)
{
  if (s->n + extra + 1 > s->cap) {
    int c = s->cap ? s->cap * 2 : 128;
    while (c < s->n + extra + 1)
      c *= 2;
    char *b = (char *)realloc(s->b, (unsigned)c);
    if (!b) {
      s->failed = 1;
      return 0;
    }
    s->b = b;
    s->cap = c;
  }
  return 1;
}
void sb_putc(sbuf *s, char c)
{
  if (!sb_need(s, 1))
    return;
  s->b[s->n++] = c;
  s->b[s->n] = 0;
}
void sb_puts(sbuf *s, const char *t)
{
  int n = strlen(t);
  if (!sb_need(s, n))
    return;
  memcpy(s->b + s->n, t, (unsigned)n);
  s->n += n;
  s->b[s->n] = 0;
}
void sb_putn(sbuf *s, const char *t, int n)
{
  if (!sb_need(s, n))
    return;
  memcpy(s->b + s->n, t, (unsigned)n);
  s->n += n;
  s->b[s->n] = 0;
}

/* ------------------------------------------------------- node wrappers ---- */

static int memcmp_js(const char *a, const char *b, int n)
{
  int i;
  for (i = 0; i < n; i++)
    if (a[i] != b[i])
      return a[i] < b[i] ? -1 : 1;
  return 0;
}

struct dom_node *node_of(js_value v)
{
  if (v.type != JV_OBJ)
    return 0;
  return (struct dom_node *)v.u.obj->host;
}
/* A DOM node is exposed as a wrapper object whose `host` slot points at the
 * node. The wrapper is cached on the node (dom_node.wrapper) so repeated
 * lookups return the SAME object — node identity, which event listeners depend
 * on. Every wrapper is pinned in g_live_wrappers so its listener closures
 * survive GC for the page. */
/* whole-word match of `tag` within the space-separated `list` */
static int tag_in_list(const char *list, const char *tag)
{
  int tl = strlen(tag);
  const char *p = list;
  while (*p) {
    const char *s = p;
    while (*p && *p != ' ')
      p++;
    if (p - s == tl && memcmp_js(s, tag, tl) == 0)
      return 1;
    while (*p == ' ')
      p++;
  }
  return 0;
}

/* interface prototype for an element tag, or NULL for a plain HTMLElement */
static js_object *iface_proto_for_tag(const char *tag)
{
  unsigned i;
  for (i = 0; i < sizeof g_html_ifaces / sizeof g_html_ifaces[0]; i++)
    if (g_html_ifaces[i].proto && tag_in_list(g_html_ifaces[i].tags, tag))
      return g_html_ifaces[i].proto;
  return 0;
}

js_value wrap_node(struct dom_node *n)
{
  js_object *o, *proto;
  if (!n)
    return js_null();
  if (n->wrapper)
    return js_obj_value((js_object *)n->wrapper);
  proto = g_html_proto ? g_html_proto : g_element_proto;
  if (n->type == DOM_ELEMENT) {
    js_object *ip = iface_proto_for_tag(n->tag); /* HTMLScriptElement etc.  */
    if (ip)
      proto = ip;
  }
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(g_ctx, proto));
  o->host = n;
  n->wrapper = o;
  if (g_live_wrappers)
    js_array_push(g_ctx, g_live_wrappers, js_obj_value(o));
  gc_pop_scope();
  return js_obj_value(o);
}

/* dom.c calls this (via the dom_on_free_wrapper hook) when a node that owns a
 * wrapper is freed mid-page (innerHTML/removeChild), so the surviving wrapper
 * goes inert (node_of -> NULL) rather than dereferencing freed memory. */
static void wrapper_detach(void *wrapper)
{
  if (wrapper)
    ((js_object *)wrapper)->host = 0;
}

const char *arg_str(js_ctx *ctx, js_value v)
{
  js_value s = js_to_string(ctx, v);
  return s.type == JV_STR ? s.u.str->data : "";
}

/* custom-element hooks (defined with the web-components block below) */
static js_value ce_lookup(const char *tag);
static void ce_upgrade_node(struct dom_node *n, js_value cls);
static void ce_connect(struct dom_node *n);
static void ce_attr_changed(struct dom_node *n, const char *name, const char *oldv, const char *newv);

/* lowercase copy into a fixed buffer */
static void lower_into(char *dst, int cap, const char *src)
{
  int i = 0;
  for (; src[i] && i < cap - 1; i++)
    dst[i] = lower(src[i]);
  dst[i] = '\0';
}

/* ------------------------------------------------------- text content ----- */

static void collect_text(struct dom_node *n, sbuf *sb)
{
  struct dom_node *c;
  if (n->type == DOM_TEXT) {
    if (n->text)
      sb_puts(sb, n->text);
    return;
  }
  for (c = n->first_child; c; c = c->next)
    collect_text(c, sb);
}
static void serialize(struct dom_node *n, sbuf *sb)
{
  struct dom_node *c;
  for (c = n->first_child; c; c = c->next) {
    if (c->type == DOM_TEXT) {
      if (c->text)
        sb_puts(sb, c->text);
      continue;
    }
    sb_putc(sb, '<');
    sb_puts(sb, c->tag);
    {
      struct dom_attr *a;
      for (a = c->attrs; a; a = a->next) {
        sb_putc(sb, ' ');
        sb_puts(sb, a->name);
        sb_puts(sb, "=\"");
        sb_puts(sb, a->value ? a->value : "");
        sb_putc(sb, '"');
      }
    }
    sb_putc(sb, '>');
    serialize(c, sb);
    sb_puts(sb, "</");
    sb_puts(sb, c->tag);
    sb_putc(sb, '>');
  }
}

static js_value text_content_value(js_ctx *ctx, struct dom_node *n)
{
  sbuf sb;
  js_value r;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  collect_text(n, &sb);
  r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static void set_text_content(struct dom_node *n, const char *text)
{
  dom_clear_children(n);
  if (text && *text) {
    struct dom_node *t = dom_new_text(text);
    if (t)
      dom_append_child(n, t);
  }
}
static void set_inner_html(struct dom_node *el, const char *html)
{
  struct dom_node *frag, *c;
  dom_clear_children(el);
  frag = dom_parse(html, html ? (int)strlen(html) : 0);
  if (!frag)
    return;
  c = frag->first_child;
  frag->first_child = frag->last_child = 0;
  while (c) {
    struct dom_node *nx = c->next;
    c->next = 0;
    c->parent = 0;
    dom_append_child(el, c);
    c = nx;
  }
  dom_free(frag);
}

/* ------------------------------------------------------- element natives -- */

static js_value el_tagName(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  char up[16];
  int i = 0;
  (void)args;
  (void)argc;
  if (!n)
    return js_undef();
  for (; n->tag[i] && i < 15; i++)
    up[i] = (n->tag[i] >= 'a' && n->tag[i] <= 'z') ? (char)(n->tag[i] - 32) : n->tag[i];
  up[i] = '\0';
  return js_mk_cstring(ctx, up);
}
static js_value el_nodeName(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n && n->type == DOM_TEXT)
    return js_mk_cstring(ctx, "#text");
  return el_tagName(ctx, t, args, argc);
}
static js_value el_nodeType(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return js_number(n && n->type == DOM_TEXT ? 3 : 1);
}
static js_value el_get_id(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *v;
  (void)args;
  (void)argc;
  v = n ? dom_get_attr(n, "id") : 0;
  return js_mk_cstring(ctx, v ? v : "");
}
static js_value el_set_id(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    dom_set_attr(n, "id", arg_str(ctx, A(0)));
  return js_undef();
}
/* .nonce reflects the content attribute. Script loaders (Cloudflare's challenge
 * bootstrap among them) copy the CSP nonce onto an injected <script> via this
 * IDL property, not setAttribute; without reflection the content attribute
 * stays empty and the element's script-src nonce check fails. */
static js_value el_get_nonce(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *v;
  (void)args;
  (void)argc;
  v = n ? dom_get_attr(n, "nonce") : 0;
  return js_mk_cstring(ctx, v ? v : "");
}
static js_value el_set_nonce(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    dom_set_attr(n, "nonce", arg_str(ctx, A(0)));
  return js_undef();
}
static js_value el_get_class(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *v;
  (void)args;
  (void)argc;
  v = n ? dom_get_attr(n, "class") : 0;
  return js_mk_cstring(ctx, v ? v : "");
}
static js_value el_set_class(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    dom_set_attr(n, "class", arg_str(ctx, A(0)));
  return js_undef();
}

/* ------------------------------------------------------- classList ---------
 */
/* DOMTokenList over the element's class="" attribute. A fresh wrapper (host =
 * node) is returned per `.classList` access, like `.style`; the mutating
 * methods read and rewrite the live class attr, so they never go stale.
 * Mutations re-serialize the normalized ordered set (whitespace collapsed,
 * duplicates dropped), matching the DOM's token-list update steps. */

/* the node's current class string, or "" (never NULL) */
static const char *cl_class_of(js_value t)
{
  struct dom_node *n = node_of(t);
  const char *v = n ? dom_get_attr(n, "class") : 0;
  return v ? v : "";
}

/* rebuild `n`'s class attr as the current tokens (deduped) with `tok` appended
 */
static void cl_write_add(struct dom_node *n, const char *tok, int tl)
{
  char *classes = sdup(dom_get_attr(n, "class") ? dom_get_attr(n, "class") : "");
  classlist_add(&classes, tok, tl);
  dom_set_attr(n, "class", classes);
  free(classes);
}

/* rebuild `n`'s class attr as the current tokens minus every copy of `tok` */
static void cl_write_remove(struct dom_node *n, const char *tok, int tl)
{
  char *classes = sdup(dom_get_attr(n, "class") ? dom_get_attr(n, "class") : "");
  classlist_remove(&classes, tok, tl);
  dom_set_attr(n, "class", classes);
  free(classes);
}

static js_value cl_add(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  int i;
  if (!n)
    return js_undef();
  for (i = 0; i < argc; i++) {
    js_value s = js_to_string(ctx, args[i]);
    const char *tok = s.type == JV_STR ? s.u.str->data : "";
    int tl = (int)strlen(tok);
    if (tl)
      cl_write_add(n, tok, tl);
  }
  return js_undef();
}

static js_value cl_remove(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  int i;
  if (!n)
    return js_undef();
  for (i = 0; i < argc; i++) {
    js_value s = js_to_string(ctx, args[i]);
    const char *tok = s.type == JV_STR ? s.u.str->data : "";
    int tl = (int)strlen(tok);
    if (tl)
      cl_write_remove(n, tok, tl);
  }
  return js_undef();
}

/* toggle(token[, force]): with `force` given, force==true adds / false removes
 * (idempotently) and returns force; otherwise flips and returns the new state.
 */
static js_value cl_toggle(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_value s = js_to_string(ctx, A(0));
  const char *tok = s.type == JV_STR ? s.u.str->data : "";
  int tl = (int)strlen(tok), present;
  if (!n || tl == 0)
    return js_bool(0);
  present = classlist_has(cl_class_of(t), tok, tl);
  if (argc >= 2) {
    int force = js_to_boolean(args[1]);
    if (force && !present)
      cl_write_add(n, tok, tl);
    else if (!force && present)
      cl_write_remove(n, tok, tl);
    return js_bool(force);
  }
  if (present) {
    cl_write_remove(n, tok, tl);
    return js_bool(0);
  }
  cl_write_add(n, tok, tl);
  return js_bool(1);
}

/* replace(old, new): swap the first `old` for `new` (dropping duplicates of
 * either), returning whether `old` was present. */
static js_value cl_replace(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_value ov = js_to_string(ctx, A(0)), nv = js_to_string(ctx, A(1));
  const char *oldt = ov.type == JV_STR ? ov.u.str->data : "";
  const char *newt = nv.type == JV_STR ? nv.u.str->data : "";
  int ol = (int)strlen(oldt), nl = (int)strlen(newt);
  (void)argc;
  if (!n || ol == 0 || nl == 0)
    return js_bool(0);
  {
    char *classes = sdup(dom_get_attr(n, "class") ? dom_get_attr(n, "class") : "");
    int replaced = classlist_replace(&classes, oldt, newt);
    dom_set_attr(n, "class", classes);
    free(classes);
    return js_bool(replaced);
  }
}

static js_value cl_contains(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value s = js_to_string(ctx, A(0));
  const char *tok = s.type == JV_STR ? s.u.str->data : "";
  (void)args;
  (void)argc;
  return js_bool(classlist_has(cl_class_of(t), tok, (int)strlen(tok)));
}

static js_value cl_item(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int idx = (int)js_to_number(ctx, A(0)), len;
  const char *b;
  (void)args;
  (void)argc;
  b = idx >= 0 ? classlist_nth(cl_class_of(t), idx, &len) : 0;
  return b ? js_mk_string(ctx, b, len) : js_null();
}

static js_value cl_length(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return js_number(classlist_count(cl_class_of(t)));
}

static js_value cl_get_value(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, cl_class_of(t));
}
static js_value cl_set_value(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    dom_set_attr(n, "class", arg_str(ctx, A(0)));
  return js_undef();
}

static js_value cl_foreach(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value cb = A(0), thisArg = argc >= 2 ? args[1] : js_undef();
  const char *b;
  int i, len;
  if (!js_is_callable(cb))
    return js_undef();
  for (i = 0; (b = classlist_nth(cl_class_of(t), i, &len)); i++) {
    js_value a[3];
    a[0] = js_mk_string(ctx, b, len); /* copied before the callback may mutate */
    a[1] = js_number(i);
    a[2] = t;
    js_call(ctx, cb, thisArg, a, 3);
  }
  return js_undef();
}

/* values/keys/entries + Symbol.iterator: snapshot the tokens into a real array
 * and delegate to its iterator, so token order and pair shape match the DOM. */
static js_value cl_iter_kind(js_ctx *ctx, js_value t, const char *method)
{
  js_object *arr;
  const char *b;
  int i, len;
  js_value av, fn;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; (b = classlist_nth(cl_class_of(t), i, &len)); i++)
    js_array_push(ctx, arr, js_mk_string(ctx, b, len));
  av = js_obj_value(arr);
  fn = js_get(ctx, av, method);
  gc_pop_scope();
  return js_call(ctx, fn, av, 0, 0);
}
static js_value cl_values(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return cl_iter_kind(ctx, t, "values");
}
static js_value cl_keys(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return cl_iter_kind(ctx, t, "keys");
}
static js_value cl_entries(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return cl_iter_kind(ctx, t, "entries");
}

static js_value el_get_classlist(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *o;
  struct dom_node *n = node_of(t);
  const char *cur, *b;
  int i, len;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_classlist_proto));
  o->host = n;
  /* Snapshot numeric index properties (classList[0]) — the engine has no
   * index-getter hook, and the wrapper is fresh per access so this is current.
   */
  cur = n ? dom_get_attr(n, "class") : 0;
  if (!cur)
    cur = "";
  for (i = 0; (b = classlist_nth(cur, i, &len)); i++) {
    char key[12];
    int k = 0, d = i, p;
    if (d == 0)
      key[k++] = '0';
    else {
      char tmp[12];
      int m = 0;
      while (d) {
        tmp[m++] = (char)('0' + d % 10);
        d /= 10;
      }
      for (p = m - 1; p >= 0; p--)
        key[k++] = tmp[p];
    }
    key[k] = '\0';
    js_def(o, key, js_mk_string(ctx, b, len), P_ENUMERABLE | P_CONFIGURABLE);
  }
  gc_pop_scope();
  return js_obj_value(o);
}
/* .src / .href reflect the attribute resolved to an absolute URL (as the real
 * DOM does — e.g. `new URL(script.src)` needs an absolute value). Setters write
 * the raw attribute. */
static js_value el_get_url_attr(js_ctx *ctx, js_value t, const char *attr)
{
  struct dom_node *n = node_of(t);
  const char *v;
  char abs[URL_MAX];
  v = n ? dom_get_attr(n, attr) : 0;
  if (!v || !*v)
    return js_mk_cstring(ctx, "");
  resolve_url(g_page_url_buf, v, abs, sizeof abs);
  return js_mk_cstring(ctx, abs);
}
static js_value el_get_src(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return el_get_url_attr(ctx, t, "src");
}
static js_value el_set_src(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    dom_set_attr(n, "src", arg_str(ctx, A(0)));
  return js_undef();
}
static js_value el_get_href(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return el_get_url_attr(ctx, t, "href");
}
static js_value el_set_href(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    dom_set_attr(n, "href", arg_str(ctx, A(0)));
  return js_undef();
}
static js_value el_get_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)args;
  (void)argc;
  if (!n)
    return js_mk_cstring(ctx, "");
  return text_content_value(ctx, n);
}
static js_value el_set_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    set_text_content(n, arg_str(ctx, A(0)));
  return js_undef();
}
static js_value el_get_html(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  sbuf sb;
  js_value r;
  (void)args;
  (void)argc;
  if (!n)
    return js_mk_cstring(ctx, "");
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  serialize(n, &sb);
  r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value el_set_html(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    set_inner_html(n, arg_str(ctx, A(0)));
  return js_undef();
}
static js_value el_get_attr(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *v = n ? dom_get_attr(n, arg_str(ctx, A(0))) : 0;
  return v ? js_mk_cstring(ctx, v) : js_null();
}
static js_value el_set_attr(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n) {
    char name[64], oldv[256], val[1024];
    const char *ov;
    strlcpy(name, arg_str(ctx, A(0)), sizeof name);
    ov = dom_get_attr(n, name);
    strlcpy(oldv, ov ? ov : "", sizeof oldv);
    strlcpy(val, arg_str(ctx, A(1)), sizeof val);
    dom_set_attr(n, name, val);
    ce_attr_changed(n, name, ov ? oldv : 0, val); /* observedAttributes hook */
  }
  return js_undef();
}
static js_value el_has_attr(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  return js_bool(n && dom_get_attr(n, arg_str(ctx, A(0))) != 0);
}
static js_value el_rm_attr(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    dom_remove_attr(n, arg_str(ctx, A(0)));
  return js_undef();
}
/* A script inserted into the live document by these DOM bindings (the
 * analytics/ widget loader idiom: createElement('script') + appendChild) must
 * fetch+run, the way a browser does. Defined near the page runner;
 * forward-declared here. */
static void run_inserted_script(struct dom_node *node);
static int node_connected(struct dom_node *n)
{
  for (; n; n = n->parent)
    if (n == g_doc_root)
      return 1;
  return 0;
}
static js_value el_append(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c = node_of(A(0));
  (void)ctx;
  if (n && c) {
    dom_append_child(n, c);
    if (c->type == DOM_ELEMENT) { /* upgrade + connect a custom el */
      js_value cls = ce_lookup(c->tag);
      if (cls.type == JV_OBJ)
        ce_upgrade_node(c, cls);
      ce_connect(c);
      if (node_connected(c))
        run_inserted_script(c);
    }
  }
  return A(0);
}
static js_value el_remove_child(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c = node_of(A(0));
  (void)ctx;
  if (n && c)
    dom_remove_child(n, c);
  return A(0);
}
static js_value el_insert_before(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c = node_of(A(0)), *ref = node_of(A(1));
  (void)ctx;
  if (n && c) {
    dom_insert_before(n, c, ref); /* null/absent ref => append */
    if (c->type == DOM_ELEMENT) { /* upgrade + connect a custom el */
      js_value cls = ce_lookup(c->tag);
      if (cls.type == JV_OBJ)
        ce_upgrade_node(c, cls);
      ce_connect(c);
      if (node_connected(c))
        run_inserted_script(c);
    }
  }
  return A(0);
}
static js_value el_parent(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return (n && n->parent) ? wrap_node(n->parent) : js_null();
}
static js_value el_first_child(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return n ? wrap_node(n->first_child) : js_null();
}
static js_value el_last_child(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return n ? wrap_node(n->last_child) : js_null();
}
static js_value el_next(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return n ? wrap_node(n->next) : js_null();
}
static js_value el_children(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c;
  js_object *arr;
  int elems_only = 1;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (n)
    for (c = n->first_child; c; c = c->next)
      if (!elems_only || c->type == DOM_ELEMENT)
        js_array_push(ctx, arr, wrap_node(c));
  gc_pop_scope();
  return js_obj_value(arr);
}
/* ------------------------------------------------------- style (CSSOM-lite) -
 * el.style is a thin proxy whose host is the element node; its accessors and
 * methods read/write the element's style="" declaration string, which css.c's
 * css_apply_inline already folds in at layout time. So `el.style.color='red'`
 * (or setProperty) actually restyles the page on the next render. */

static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int span_ci_eq(const char *s, const char *e, const char *lit)
{
  while (s < e && *lit) {
    if (lower(*s) != lower(*lit))
      return 0;
    s++;
    lit++;
  }
  return s == e && !*lit;
}

static js_value style_read(js_ctx *ctx, struct dom_node *node, const char *prop)
{
  const char *style = node ? dom_get_attr(node, "style") : 0;
  const char *p = style;
  while (p && *p) {
    const char *seg = p, *colon;
    while (*p && *p != ';')
      p++;
    colon = seg;
    while (colon < p && *colon != ':')
      colon++;
    if (colon < p) {
      const char *ns = seg, *ne = colon, *vs = colon + 1, *ve = p;
      while (ns < ne && is_sp(*ns))
        ns++;
      while (ne > ns && is_sp(ne[-1]))
        ne--;
      while (vs < ve && is_sp(*vs))
        vs++;
      while (ve > vs && is_sp(ve[-1]))
        ve--;
      if (ns < ne && span_ci_eq(ns, ne, prop))
        return js_mk_string(ctx, vs, (int)(ve - vs));
    }
    if (*p == ';')
      p++;
  }
  return js_mk_cstring(ctx, "");
}

/* Rewrite the element's style="" with `setprop` set to `setval` (NULL/""
 * removes it). Every other declaration is preserved, the changed one moves to
 * the end. */
static void style_rebuild(struct dom_node *node, const char *setprop, const char *setval)
{
  const char *style;
  sbuf sb;
  if (!node)
    return;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  style = dom_get_attr(node, "style");
  {
    const char *p = style;
    while (p && *p) {
      const char *seg = p, *colon;
      while (*p && *p != ';')
        p++;
      colon = seg;
      while (colon < p && *colon != ':')
        colon++;
      if (colon < p) {
        const char *ns = seg, *ne = colon, *vs = colon + 1, *ve = p;
        while (ns < ne && is_sp(*ns))
          ns++;
        while (ne > ns && is_sp(ne[-1]))
          ne--;
        while (vs < ve && is_sp(*vs))
          vs++;
        while (ve > vs && is_sp(ve[-1]))
          ve--;
        if (ns < ne && !span_ci_eq(ns, ne, setprop)) {
          if (sb.n)
            sb_puts(&sb, "; ");
          sb_putn(&sb, ns, (int)(ne - ns));
          sb_puts(&sb, ": ");
          sb_putn(&sb, vs, (int)(ve - vs));
        }
      }
      if (*p == ';')
        p++;
    }
  }
  if (setprop && setval && *setval) {
    if (sb.n)
      sb_puts(&sb, "; ");
    sb_puts(&sb, setprop);
    sb_puts(&sb, ": ");
    sb_puts(&sb, setval);
  }
  dom_set_attr(node, "style", sb.b ? sb.b : "");
  if (sb.b)
    free(sb.b);
}
static void style_write(struct dom_node *node, const char *prop, const char *val) { style_rebuild(node, prop, val); }

#define STYLE_PROP(JSN, CSSN)                                                                                          \
  static js_value style_get_##JSN(js_ctx *ctx, js_value t, js_value *args, int argc)                                   \
  {                                                                                                                    \
    (void)args;                                                                                                        \
    (void)argc;                                                                                                        \
    return style_read(ctx, node_of(t), CSSN);                                                                          \
  }                                                                                                                    \
  static js_value style_set_##JSN(js_ctx *ctx, js_value t, js_value *args, int argc)                                   \
  {                                                                                                                    \
    style_write(node_of(t), CSSN, arg_str(ctx, A(0)));                                                                 \
    return js_undef();                                                                                                 \
  }
STYLE_PROP(color, "color")
STYLE_PROP(background, "background")
STYLE_PROP(backgroundColor, "background-color")
STYLE_PROP(backgroundImage, "background-image")
STYLE_PROP(backgroundPosition, "background-position")
STYLE_PROP(backgroundSize, "background-size")
STYLE_PROP(backgroundRepeat, "background-repeat")
STYLE_PROP(display, "display")
STYLE_PROP(width, "width")
STYLE_PROP(height, "height")
STYLE_PROP(fontWeight, "font-weight")
STYLE_PROP(fontFamily, "font-family")
STYLE_PROP(textAlign, "text-align")
STYLE_PROP(textDecoration, "text-decoration")
#undef STYLE_PROP

static js_value style_get_csstext(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *s = n ? dom_get_attr(n, "style") : 0;
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, s ? s : "");
}
static js_value style_set_csstext(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    dom_set_attr(n, "style", arg_str(ctx, A(0)));
  return js_undef();
}
static js_value style_setprop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  char name[64];
  const char *ns = arg_str(ctx, A(0));
  int i;
  for (i = 0; ns[i] && i < 63; i++)
    name[i] = ns[i];
  name[i] = '\0';
  style_write(n, name, arg_str(ctx, A(1)));
  return js_undef();
}
static js_value style_getprop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return style_read(ctx, node_of(t), arg_str(ctx, A(0)));
}
static js_value style_removeprop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_value old = style_read(ctx, n, arg_str(ctx, A(0)));
  style_rebuild(n, arg_str(ctx, A(0)), 0);
  return old;
}

/* <style>/<link> .sheet: a minimal CSSStyleSheet stub with an empty cssRules
 * list. We don't yet parse the element's text into a live CSSOM, but this keeps
 * feature-detectors that read `style.sheet.cssRules` from throwing (they just
 * report those rule-based features as unsupported). Non-stylesheet nodes: null.
 */
static js_value el_get_sheet(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_object *sheet;
  js_value rules;
  (void)args;
  (void)argc;
  if (!n || n->type != DOM_ELEMENT || !(streq(n->tag, "style") || streq(n->tag, "link")))
    return js_null();
  gc_push_scope();
  sheet = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  rules = js_obj_value(js_new_array(ctx, 0));
  js_def(sheet, "cssRules", rules, P_ENUMERABLE);
  js_def(sheet, "rules", rules, P_ENUMERABLE);
  gc_pop_scope();
  return js_obj_value(sheet);
}
static js_value el_get_style(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *o = js_new_object(ctx, g_style_proto);
  (void)args;
  (void)argc;
  o->host = node_of(t); /* proxy the element's style attr */
  return js_obj_value(o);
}
/* el.dataset: a snapshot object of the element's data-* attributes, keyed in
 * camelCase (data-astro-prefetch -> astroPrefetch). Reads only (fresh per
 * access). */
static js_value el_get_dataset(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_object *o;
  struct dom_attr *a;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  if (n)
    for (a = n->attrs; a; a = a->next) {
      if (starts_ci(a->name, "data-") && a->name[5]) {
        char key[64];
        int ki = 0;
        const char *p = a->name + 5;
        while (*p && ki < 63) {
          if (*p == '-' && p[1]) {
            p++;
            key[ki++] = (p[0] >= 'a' && p[0] <= 'z') ? (char)(p[0] - 32) : p[0];
            p++;
          } else
            key[ki++] = *p++;
        }
        key[ki] = '\0';
        js_def(o, key, js_mk_cstring(ctx, a->value ? a->value : ""), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
      }
    }
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value win_getcomputedstyle(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *o = js_new_object(ctx, g_style_proto);
  (void)t;
  o->host = node_of(A(0)); /* inline styles only (no cascade) */
  return js_obj_value(o);
}
/* window geometry: the content viewport size and the page's scroll offset.
 * Paired with element getBoundingClientRect (same viewport frame), so
 * viewport-visibility math (lazy-load / scroll-spy) works. */
static js_value win_innerWidth(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int w = 0, h = 0;
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  browser_viewport_px(&w, &h);
  return js_number(w);
}
static js_value win_innerHeight(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int w = 0, h = 0;
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  browser_viewport_px(&w, &h);
  return js_number(h);
}
static js_value win_scrollX(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number(0);
}
static js_value win_scrollY(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number(browser_scroll_px());
}

/* window.screen — no separate display metric exists, so the resizable window's
 * client size stands in for the screen (sites use it for analytics/feature
 * sniffing, e.g. screen.width/height). Accessors so a resize is reflected. */
static js_value screen_width(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number(BW);
}
static js_value screen_height(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number(BH);
}

js_value el_noop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_undef();
}
static js_value nav_false(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_bool(0);
}
static js_value el_true(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_bool(1);
}
static js_value el_empty_array(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  return js_obj_value(js_new_array(ctx, 0));
}

/* --- window.alert / confirm / prompt --------------------------------------
 * JS runs on the worker thread, but the modal dialog must run on the MAIN
 * thread: the gfx surface is main-thread-only, and the dialog is parented to
 * the browser window the main task owns. So we hand the request to main via
 * B->dlg_* and block this thread until it finishes — which is exactly
 * window.alert's blocking semantics. (main services B->dlg_req in its event
 * loop; see browser/main.c.) */
static void dlg_scopy(char *d, const char *s, int cap)
{
  int i = 0;
  if (s)
    for (; s[i] && i < cap - 1; i++)
      d[i] = s[i];
  d[i] = 0;
}
static int worker_dialog(int kind, const char *msg, const char *deflt, char *out, int outsz)
{
  if (B->win_id < 0)
    return 0;
  B->dlg_title[0] = 0;
  dlg_scopy(B->dlg_msg, msg, sizeof B->dlg_msg);
  dlg_scopy(B->dlg_text, deflt ? deflt : "", sizeof B->dlg_text);
  B->dlg_ok = 0;
  B->dlg_done = 0;
  B->dlg_req = kind; /* publish last: main sees a complete request */
  while (!B->dlg_done)
    msleep(10); /* block JS; main sets dlg_done (also on window close) */
  B->dlg_req = 0;
  if (out && outsz > 0)
    dlg_scopy(out, B->dlg_text, outsz);
  return B->dlg_ok;
}
static js_value win_alert(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  worker_dialog(1, arg_str(ctx, A(0)), 0, 0, 0);
  return js_undef();
}
static js_value win_confirm(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return js_bool(worker_dialog(2, arg_str(ctx, A(0)), 0, 0, 0));
}
static js_value win_prompt(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  char msg[512], deflt[512];
  dlg_scopy(msg, arg_str(ctx, A(0)), sizeof msg); /* copy before the next arg_str */
  js_value dv = A(1);
  dlg_scopy(deflt, (dv.type != JV_UNDEF && dv.type != JV_NULL) ? arg_str(ctx, dv) : "", sizeof deflt);
  char out[512];
  if (worker_dialog(3, msg, deflt, out, sizeof out))
    return js_mk_cstring(ctx, out);
  return js_null();
}

/* --- per-origin permissions (Notification / Geolocation / Camera / Microphone)
 * The requesting document's origin is g_page_url_buf (worker-thread local). A
 * powerful feature is gated in three stages: the site's own Permissions-Policy
 * (a blocked feature is denied silently), a remembered decision, else a modal
 * Refuse/Allow-once/Allow-always prompt driven on the main thread (dlg_req 4,
 * same handshake as alert/confirm). "Allow always" persists; the rest are
 * session-only. See permissions.c. */
static int page_origin(char *origin, int ocap, char *host, int hcap)
{
  struct purl pu;
  char h[URL_HOST_MAX];
  if (!parse_url(g_page_url_buf, &pu) || !pu.host[0])
    return 0;
  lower_into(h, sizeof h, pu.host);
  dlg_scopy(host, h, hcap);
  {
    int k = 0;
    origin[0] = 0;
    sapp(origin, &k, ocap, pu.tls ? "https://" : "http://");
    sapp(origin, &k, ocap, h);
  }
  return 1;
}

const char *perm_state_str(int st) { return st == PERM_GRANTED ? "granted" : st == PERM_DENIED ? "denied" : "default"; }

/* The remembered/policy state for a feature WITHOUT prompting (for
 * Notification.permission and navigator.permissions.query). */
int perm_state_now(int feature)
{
  char origin[URL_MAX], host[URL_HOST_MAX];
  if (!page_origin(origin, sizeof origin, host, sizeof host))
    return PERM_ASK;
  if (B->pg && perm_policy_blocks(B->pg->perm_policy, feature))
    return PERM_DENIED;
  return perm_lookup(origin, feature);
}

/* Resolve a permission, prompting the user if there is no prior decision.
 * Returns PERM_GRANTED / PERM_DENIED and records the choice. */
int worker_permission(int feature)
{
  char origin[URL_MAX], host[URL_HOST_MAX];
  if (!page_origin(origin, sizeof origin, host, sizeof host))
    return PERM_DENIED; /* no attributable origin */
  if (B->pg && perm_policy_blocks(B->pg->perm_policy, feature))
    return PERM_DENIED; /* the site's own policy blocks it */
  int st = perm_lookup(origin, feature);
  if (st != PERM_ASK)
    return st;
  if (B->win_id < 0)
    return PERM_DENIED;

  {
    int k = 0;
    B->dlg_title[0] = 0;
    sapp(B->dlg_title, &k, sizeof B->dlg_title, host);
  }
  {
    int k = 0;
    B->dlg_msg[0] = 0;
    sapp(B->dlg_msg, &k, sizeof B->dlg_msg, host);
    sapp(B->dlg_msg, &k, sizeof B->dlg_msg, " wants to ");
    sapp(B->dlg_msg, &k, sizeof B->dlg_msg, perm_feature_verb(feature));
    sapp(B->dlg_msg, &k, sizeof B->dlg_msg, ".");
  }
  B->dlg_text[0] = 0;
  B->dlg_ok = -1; /* default (also on browser close) = no answer */
  B->dlg_done = 0;
  B->dlg_req = 4; /* publish last: main sees it complete */
  while (!B->dlg_done)
    msleep(10);
  B->dlg_req = 0;

  int choice = B->dlg_ok; /* -1=no answer 0=Refuse 1=Once 2=Always */
  if (choice < 0)
    return PERM_DENIED; /* dismissed/timed out: deny once, re-ask later */
  int state = choice >= 1 ? PERM_GRANTED : PERM_DENIED;
  perm_remember(origin, feature, state, choice == 2 /* persist only "always" */);
  return state;
}

/* The Observer family (Mutation/Intersection/Resize/Performance): a constructor
 * returning an inert observer. We have no observation event loop, so observe()/
 * disconnect() are no-ops and the callback never fires — but constructing one
 * (a near-universal idiom on modern pages) no longer throws. */
static js_value observer_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o;
  (void)thisv;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def_native(ctx, o, "observe", el_noop, 2);
  js_def_native(ctx, o, "unobserve", el_noop, 1);
  js_def_native(ctx, o, "disconnect", el_noop, 0);
  js_def_native(ctx, o, "takeRecords", el_empty_array, 0);
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value win_matchmedia(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *o;
  (void)t;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(o, "matches", js_bool(0), P_ENUMERABLE);
  js_def(o, "media", js_mk_cstring(ctx, arg_str(ctx, A(0))), P_ENUMERABLE);
  js_def_native(ctx, o, "addEventListener", el_noop, 2);
  js_def_native(ctx, o, "removeEventListener", el_noop, 2);
  js_def_native(ctx, o, "addListener", el_noop, 1);
  js_def_native(ctx, o, "removeListener", el_noop, 1);
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value win_queuemicrotask(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t; /* no loop: run it synchronously now */
  if (argc >= 1 && js_is_callable(args[0]))
    js_call(ctx, args[0], js_undef(), 0, 0);
  return js_undef();
}

/* --- Content-Security-Policy hooks ----------------------------------------- *
 * Script execution, fetch()/WebSocket connections and dynamic code generation
 * are gated against the page's policy (B->pg->csp). Violations log to the
 * devtools console (where CSP reports normally surface). */
static void js_csp_log(const char *what, const char *url, const char *reason)
{
  char m[URL_MAX + 160];
  int n = 0;
  sapp(m, &n, sizeof m, "[CSP] refused to load ");
  sapp(m, &n, sizeof m, what);
  sapp(m, &n, sizeof m, " ");
  sapp(m, &n, sizeof m, url ? url : "");
  if (reason && reason[0]) {
    sapp(m, &n, sizeof m, " (");
    sapp(m, &n, sizeof m, reason);
    sapp(m, &n, sizeof m, ")");
  }
  sapp(m, &n, sizeof m, "\n");
  devtools_console_append(DT_ERROR, m, n);
}

/* Stub installed over the eval/Function globals when 'unsafe-eval' is absent.
 */
static js_value dyncode_blocked(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)a;
  (void)n;
  return js_throw_type(ctx, "code generation from strings blocked by Content-Security-Policy");
}

/* ------------------------------------------------------- fetch ------------- *
 * fetch(url, opts) runs a synchronous HTTP request on the worker (fits the
 * settle-immediately Promise model) and resolves with a Response, or rejects
 * with a TypeError on a connection error. Response.text()/json() return
 * already-settled Promises of the body. */
static js_value resp_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return js_promise_resolved(ctx, js_get(ctx, t, "@body"));
}
static js_value resp_json(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value body = js_get(ctx, t, "@body"), parsed, jsonobj, parsefn;
  (void)args;
  (void)argc;
  jsonobj = js_get(ctx, js_obj_value(ctx->global), "JSON");
  parsefn = js_get(ctx, jsonobj, "parse");
  if (!js_is_callable(parsefn))
    return js_promise_rejected(ctx, js_mk_cstring(ctx, "no JSON.parse"));
  parsed = js_call(ctx, parsefn, jsonobj, &body, 1);
  if (ctx->has_exc) {
    js_value e = ctx->exc;
    ctx->has_exc = 0;
    ctx->exc_root = 0;
    return js_promise_rejected(ctx, e);
  }
  return js_promise_resolved(ctx, parsed);
}
static js_value resp_clone(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return t; /* body isn't consumed here, so self works */
}
js_value hdr_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *name = arg_str(ctx, A(0));
  (void)argc;
  if (streq_ci(name, "content-type"))
    return js_get(ctx, t, "@ctype");
  return js_null();
}
js_value hdr_has(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value v = hdr_get(ctx, t, args, argc);
  return js_bool(v.type == JV_STR);
}
static const char *body_bytes(js_ctx *ctx, js_value v, int *len, char *ctype, int ctcap); /* defined with Blob */

/* Build a Response on g_response_proto from raw bytes. Shared by fetch(), the
 * Cache API and the Response constructor. The returned object is live in the
 * caller's current gc scope only — a caller that allocates afterward must
 * gc_keep it first. `len` < 0 means an empty body. */
js_object *make_response(js_ctx *ctx, const char *url, int status, const char *statustext, const char *ctype,
                         const char *body, int len)
{
  js_object *resp, *hdrs;
  gc_push_scope();
  resp = (js_object *)gc_keep(js_new_object(ctx, g_response_proto));
  js_def(resp, "@body", js_mk_string(ctx, body ? body : "", len < 0 ? 0 : len), 0);
  js_def(resp, "status", js_number(status ? status : 200), P_ENUMERABLE);
  js_def(resp, "ok", js_bool(status >= 200 && status < 300), P_ENUMERABLE);
  js_def(resp, "statusText", js_mk_cstring(ctx, statustext ? statustext : ""), P_ENUMERABLE);
  js_def(resp, "url", js_mk_cstring(ctx, url ? url : ""), P_ENUMERABLE);
  js_def(resp, "redirected", js_bool(0), P_ENUMERABLE);
  js_def(resp, "type", js_mk_cstring(ctx, "basic"), P_ENUMERABLE);
  js_def(resp, "bodyUsed", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  hdrs = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(hdrs, "@ctype", js_mk_cstring(ctx, ctype ? ctype : ""), 0);
  js_def_native(ctx, hdrs, "get", hdr_get, 1);
  js_def_native(ctx, hdrs, "has", hdr_has, 1);
  js_def(resp, "headers", js_obj_value(hdrs), P_ENUMERABLE);
  gc_pop_scope();
  return resp;
}

/* The controlling service worker (if any) answers a fetch() from the page: it
 * dispatches a FetchEvent and, if a handler called respondWith, sets *out to a
 * resolved Promise of the Response and returns 1. Defined in the SW section. */

js_value win_fetch(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char url[URL_MAX], method[16], req_ctype[64], resp_ctype[160];
  char extra_hdrs[1024];
  const char *reqbody = 0;
  int reqlen = 0, status = 0, outlen = 0, eh = 0, with_creds = 0;
  unsigned char *body;
  js_object *resp;
  js_value rv;
  (void)t;
  { /* input is a URL string or a Request object (fetch(new Request(...))) */
    js_value in = A(0);
    const char *s;
    if (in.type == JV_OBJ) {
      js_value u = js_get(ctx, in, "url");
      s = (u.type == JV_STR) ? u.u.str->data : arg_str(ctx, in);
    } else {
      s = arg_str(ctx, in);
    }
    resolve_url(g_page_url_buf, s, url, sizeof url);
  }
  if (B->pg->csp) { /* CSP connect-src */
    char use[URL_MAX], reason[64];
    if (!csp_check_url(B->pg->csp, CSP_CONNECT, url, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
      js_csp_log("connection to", url, reason);
      return js_promise_rejected(
          ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Refused to connect (Content-Security-Policy)"));
    }
    strlcpy(url, use, sizeof url); /* use the upgraded URL */
  }
  method[0] = '\0';
  req_ctype[0] = '\0';
  extra_hdrs[0] = '\0';
  if (argc >= 2 && A(1).type == JV_OBJ) {
    js_value m = js_get(ctx, A(1), "method");
    js_value b = js_get(ctx, A(1), "body");
    js_value sig = js_get(ctx, A(1), "signal");
    js_value h = js_get(ctx, A(1), "headers");
    if (m.type == JV_STR)
      strlcpy(method, m.u.str->data, sizeof method);
    (void)js_to_boolean(js_get(ctx, A(1), "keepalive")); /* sync fetch: inherently satisfied */
    {                                                    /* credentials: "include" sends cookies cross-origin (default
                                                            "same-origin" doesn't) */
      js_value cr = js_get(ctx, A(1), "credentials");
      if (cr.type == JV_STR && streq(cr.u.str->data, "include"))
        with_creds = 1;
    }
    /* Request headers: a plain { name: value } object (a Headers instance is a
     * plain object here too). Content-Type folds into the body's ctype slot to
     * avoid a duplicate header; the rest become a "Name: value\r\n" blob. */
    if (h.type == JV_OBJ) {
      pmap *pm = &h.u.obj->props;
      int i;
      for (i = 0; i < pm->n; i++) {
        const char *k = pm->e[i].key;
        js_value v;
        if (!k || k[0] == '@' || (pm->e[i].flags & P_ACCESSOR))
          continue;
        v = js_to_string(ctx, js_get(ctx, h, k));
        if (v.type != JV_STR)
          continue;
        if (streq_ci(k, "content-type")) {
          strlcpy(req_ctype, v.u.str->data, sizeof req_ctype);
          continue;
        }
        sapp(extra_hdrs, &eh, sizeof extra_hdrs, k);
        sapp(extra_hdrs, &eh, sizeof extra_hdrs, ": ");
        sapp(extra_hdrs, &eh, sizeof extra_hdrs, v.u.str->data);
        sapp(extra_hdrs, &eh, sizeof extra_hdrs, "\r\n");
      }
    }
    /* An already-aborted signal short-circuits before touching the network
     * (the fetch runs synchronously, so this is the only cancellation point).
     */
    if (sig.type == JV_OBJ && js_to_boolean(js_get(ctx, sig, "aborted"))) {
      js_value reason = js_get(ctx, sig, "reason");
      if (reason.type == JV_UNDEF)
        reason = js_make_error(ctx, ctx->error_proto, "AbortError", "The operation was aborted");
      return js_promise_rejected(ctx, reason);
    }
    /* Body last, just before the request: body_bytes may hand back a pointer
     * into a transient string (URLSearchParams), so no JS allocation must run
     * between here and browser_cors_fetch. */
    if (b.type != JV_UNDEF && b.type != JV_NULL)
      reqbody = body_bytes(ctx, b, &reqlen, req_ctype,
                           sizeof req_ctype); /* string/Blob/ArrayBuffer/TypedArray/URLSearchParams
                                               */
  }
  if (!method[0] && A(0).type == JV_OBJ) { /* method carried on a Request */
    js_value m = js_get(ctx, A(0), "method");
    if (m.type == JV_STR)
      strlcpy(method, m.u.str->data, sizeof method);
  }
  /* A controlling service worker gets first crack at the request. If it
   * answered (respondWith), skip the network entirely. Reentrancy is guarded so
   * the SW's own network fallback — fetch(event.request) — reaches the wire. */
  if (sw_intercept_fetch(ctx, url, method[0] ? method : "GET", &rv))
    return rv;
  {
    struct http_req rq = {0};
    struct http_resp rs = {0};
    rq.url = url;
    rq.method = method[0] ? method : "GET";
    rq.body = (const unsigned char *)reqbody;
    rq.body_len = reqlen;
    rq.ctype = req_ctype[0] ? req_ctype : 0;
    rq.extra_hdrs = extra_hdrs[0] ? extra_hdrs : 0;
    body = browser_cors_fetch(g_page_url_buf, with_creds, &rq, &rs);
    status = rs.status;
    outlen = rs.len;
    strlcpy(resp_ctype, rs.ctype, sizeof resp_ctype);
  }
  if (!body)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Failed to fetch"));
  gc_push_scope();
  resp = (js_object *)gc_keep(
      make_response(ctx, url, status, status == 200 ? "OK" : "", resp_ctype, (const char *)body, outlen));
  free(body);
  rv = js_promise_resolved(ctx, js_obj_value(resp));
  gc_pop_scope();
  return rv;
}

/* History API, wired to the browser's per-tab back/forward history. The JS side
 * runs on the worker, so back/forward/go just post a request the main thread
 * acts on (it owns the history + nav buttons); length reads it back. pushState/
 * replaceState update the address bar + the live location.href without
 * navigating (an SPA's soft URL change), then a real Back re-fetches the
 * previous entry. */
static js_value his_back(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  B->hist_go = -1;
  B->seq++;
  return js_undef();
}
static js_value his_forward(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  B->hist_go = 1;
  B->seq++;
  return js_undef();
}
static js_value his_go(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int d = argc >= 1 ? (int)js_to_number(ctx, args[0]) : 0;
  (void)t;
  B->hist_go = d < 0 ? -1 : (d > 0 ? 1 : 0);
  if (B->hist_go)
    B->seq++;
  return js_undef();
}
static js_value his_length(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  return js_number(B->hist_len > 0 ? B->hist_len : 1);
}
static void his_nav(js_ctx *ctx, js_value t, js_value *args, int argc, int replace)
{
  if (argc >= 3 && args[2].type != JV_UNDEF && args[2].type != JV_NULL) {
    char url[URL_MAX];
    int k = 0;
    resolve_url(g_page_url_buf, arg_str(ctx, A(2)), url, sizeof url);
    {
      js_value loc = js_get(ctx, js_obj_value(ctx->global), "location");
      js_set(ctx, loc, "href", js_mk_cstring(ctx, url));
    }
    if (g_document)
      js_set(ctx, js_obj_value(g_document), "URL", js_mk_cstring(ctx, url));
    for (; url[k] && k < URL_MAX - 1; k++)
      B->hist_url[k] = url[k];
    B->hist_url[k] = '\0';
    B->hist_replace = replace;
    B->hist_nav = 1;
    B->seq++;
  }
  if (t.type == JV_OBJ)
    js_set(ctx, t, "state", argc >= 1 ? args[0] : js_null());
}
static js_value his_pushstate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  his_nav(ctx, t, args, argc, 0);
  return js_undef();
}
static js_value his_replacestate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  his_nav(ctx, t, args, argc, 1);
  return js_undef();
}

/* location.assign/replace/reload: a real navigation, posted to the main thread.
 */
static js_value loc_assign(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)argc;
  resolve_url(g_page_url_buf, arg_str(ctx, A(0)), B->loc_url, sizeof B->loc_url);
  B->loc_nav = 1;
  B->seq++;
  return js_undef();
}
static js_value loc_reload(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  strlcpy(B->loc_url, g_page_url_buf, sizeof B->loc_url);
  B->loc_nav = 1;
  B->seq++;
  return js_undef();
}
static js_value loc_tostring(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return js_get(ctx, t, "href");
}
static js_value el_get_by_tag(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_object *arr;
  char tag[32];
  lower_into(tag, sizeof tag, arg_str(ctx, A(0)));
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (n)
    js_dom_find_all_tag(n, tag, arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
static js_value el_query(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  return wrap_node(js_dom_query(n, arg_str(ctx, A(0))));
}
static js_value el_query_all(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_object *arr;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (n)
    js_dom_query_all(n, arg_str(ctx, A(0)), arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
static int node_matches_sel(struct dom_node *n, const char *sel) { return js_dom_matches(n, sel); }
static js_value el_matches(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  return js_bool(n && node_matches_sel(n, arg_str(ctx, A(0))));
}
static js_value el_closest(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  char sel[128];
  strlcpy(sel, arg_str(ctx, A(0)), sizeof sel);
  for (; n; n = n->parent)
    if (n->type == DOM_ELEMENT && node_matches_sel(n, sel))
      return wrap_node(n);
  return js_null();
}
static js_value el_is_same_node(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  return js_bool(node_of(t) && node_of(t) == node_of(A(0)));
}
static js_value el_contains(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *o = node_of(A(0));
  (void)ctx;
  for (; o; o = o->parent)
    if (o == n)
      return js_bool(1);
  return js_bool(0);
}

/* ------------------------------------------------------- geometry natives - *
 * The browser is a line-flow renderer with no real box model, so element pixel
 * geometry is reconstructed from the wrapped-line table (browser_range_box, in
 * main.c). offsetWidth/offsetHeight/offsetTop/clientWidth etc. are document-
 * relative; getBoundingClientRect is viewport-relative (page scroll
 * subtracted), matching event.clientX/clientY. All values are approximate:
 * exact for a span on one text line, the content column otherwise. */
static int node_box(js_value t, int *x, int *y, int *w, int *h)
{
  struct dom_node *n = node_of(t);
  int s, e;
  *x = *y = *w = *h = 0;
  if (!n || !html_node_span(n, &s, &e))
    return 0;
  return browser_range_box(s, e, x, y, w, h);
}
static js_value el_offsetWidth(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int x, y, w, h;
  (void)ctx;
  (void)args;
  (void)argc;
  node_box(t, &x, &y, &w, &h);
  return js_number(w);
}
static js_value el_offsetHeight(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int x, y, w, h;
  (void)ctx;
  (void)args;
  (void)argc;
  node_box(t, &x, &y, &w, &h);
  return js_number(h);
}
static js_value el_offsetTop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int x, y, w, h;
  (void)ctx;
  (void)args;
  (void)argc;
  node_box(t, &x, &y, &w, &h);
  return js_number(y);
}
static js_value el_offsetLeft(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int x, y, w, h;
  (void)ctx;
  (void)args;
  (void)argc;
  node_box(t, &x, &y, &w, &h);
  return js_number(x);
}
static js_value el_scrollTop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number(0); /* elements aren't scroll containers; page scroll is window.scrollY */
}
static js_value el_get_bounding_rect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int x = 0, y = 0, w = 0, h = 0;
  (void)args;
  (void)argc;
  node_box(t, &x, &y, &w, &h);
  int top = y - browser_scroll_px(), left = x; /* document -> viewport (client) */
  gc_push_scope();
  {
    js_object *r = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    unsigned f = P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE;
    js_def(r, "x", js_number(left), f);
    js_def(r, "y", js_number(top), f);
    js_def(r, "left", js_number(left), f);
    js_def(r, "top", js_number(top), f);
    js_def(r, "width", js_number(w), f);
    js_def(r, "height", js_number(h), f);
    js_def(r, "right", js_number(left + w), f);
    js_def(r, "bottom", js_number(top + h), f);
    gc_pop_scope();
    return js_obj_value(r);
  }
}

/* ------------------------------------------------------- iframe content --- */

/* Wrap a DOM tree root as a Document object (host = root, root->wrapper = doc),
 * tracked in g_parsed so page teardown frees it. The document methods are
 * root-relative, so the returned object answers
 * createElement/getElementsByTagName/ head/body against `root`. Returns js_null
 * on OOM. */
static js_value wrap_document(js_ctx *ctx, struct dom_node *root)
{
  js_object *doc;
  js_value dv;
  if (!root)
    return js_null();
  if (g_nparsed >= 64) {
    dom_free(root);
    return js_null();
  }
  g_parsed[g_nparsed++] = root;
  gc_push_scope();
  doc = (js_object *)gc_keep(js_new_object(ctx, g_document_proto));
  doc->host = root;
  root->wrapper = doc;
  dv = js_obj_value(doc);
  gc_pop_scope();
  return dv;
}

/* A script-created or src-less <iframe> exposes a live, initially-empty
 * document
 * (#document > html > head + body) so framework shims that inject into
 * a.contentDocument.head — e.g. the Cloudflare challenge bootstrap — work. The
 * sub-document is cached on the iframe wrapper (hidden "@cdoc" key) so each
 * access returns the SAME Document; we never load the frame's src here. */
static js_value el_content_document(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *root, *html, *head, *body;
  js_value dv;
  (void)args;
  (void)argc;
  if (!n || !streq(n->tag, "iframe") || t.type != JV_OBJ)
    return js_null();
  if (js_has_own(t.u.obj, "@cdoc"))
    return js_get(ctx, t, "@cdoc");

  root = dom_new_element("#document");
  html = dom_new_element("html");
  head = dom_new_element("head");
  body = dom_new_element("body");
  if (!root || !html || !head || !body) {
    if (root)
      dom_free(root);
    if (html)
      dom_free(html);
    if (head)
      dom_free(head);
    if (body)
      dom_free(body);
    return js_null();
  }
  dom_append_child(root, html);
  dom_append_child(html, head);
  dom_append_child(html, body);

  dv = wrap_document(ctx, root); /* roots `root` in g_parsed */
  if (dv.type != JV_OBJ)
    return js_null();
  js_def(t.u.obj, "@cdoc", dv, P_WRITABLE | P_CONFIGURABLE);
  return dv;
}

/* contentWindow: a thin window over the same content document, with the usual
 * self-referential window/self/document so `a.contentWindow.document` resolves
 * to the same Document as `a.contentDocument`. Cached on the wrapper ("@cwin").
 */
static js_value el_content_window(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_value docv;
  js_object *fw;
  js_value wv;
  (void)args;
  (void)argc;
  if (!n || !streq(n->tag, "iframe") || t.type != JV_OBJ)
    return js_null();
  if (js_has_own(t.u.obj, "@cwin"))
    return js_get(ctx, t, "@cwin");

  docv = el_content_document(ctx, t, 0, 0);
  if (docv.type != JV_OBJ)
    return js_null();
  gc_push_scope();
  fw = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  wv = js_obj_value(fw);
  js_def(fw, "document", docv, P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  js_def(fw, "window", wv, P_WRITABLE | P_CONFIGURABLE);
  js_def(fw, "self", wv, P_WRITABLE | P_CONFIGURABLE);
  js_def(t.u.obj, "@cwin", wv, P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
  return wv;
}

/* ------------------------------------------------------- document natives - */

/* The root the document method was called on: a DOMParser document wraps its
 * own tree (host = parsed root); the live document falls back to g_doc_root. So
 * every query below works on either the page or a parsed off-document tree. */
static struct dom_node *doc_root_of(js_value t)
{
  struct dom_node *r = node_of(t);
  return r ? r : g_doc_root;
}
static struct dom_node *doc_first_tag_in(struct dom_node *root, const char *tag) { return js_dom_find_tag(root, tag); }
static struct dom_node *doc_first_tag(const char *tag) { return doc_first_tag_in(g_doc_root, tag); }

static js_value doc_get_by_id(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return wrap_node(js_dom_find_id(doc_root_of(t), arg_str(ctx, A(0))));
}
static js_value doc_get_by_tag(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *arr;
  char tag[32];
  lower_into(tag, sizeof tag, arg_str(ctx, A(0)));
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_dom_find_all_tag(doc_root_of(t), tag, arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
static js_value doc_query(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return wrap_node(js_dom_query(doc_root_of(t), arg_str(ctx, A(0))));
}
static js_value doc_query_all(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *arr;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_dom_query_all(doc_root_of(t), arg_str(ctx, A(0)), arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
static js_value doc_create_el(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = dom_new_element(arg_str(ctx, A(0)));
  js_value wv = wrap_node(n);
  (void)t;
  if (n) {
    js_value cls = ce_lookup(n->tag);
    if (cls.type == JV_OBJ)
      ce_upgrade_node(n, cls);
  }
  return wv;
}
static js_value doc_create_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  return wrap_node(dom_new_text(arg_str(ctx, A(0))));
}
/* createElementNS(ns, qualifiedName): the DOM tracks no namespace, so the
 * element is keyed by its qualified name (arg 1); custom-element upgrade is
 * HTML-only, so skip it for a namespaced create. */
static js_value doc_create_el_ns(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return wrap_node(dom_new_element(arg_str(ctx, A(1))));
}
static js_value doc_body(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *root = doc_root_of(t), *b = doc_first_tag_in(root, "body");
  (void)ctx;
  (void)args;
  (void)argc;
  return wrap_node(b ? b : root);
}
static js_value doc_head(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return wrap_node(doc_first_tag_in(doc_root_of(t), "head"));
}
static js_value doc_active_element(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *b = doc_first_tag_in(doc_root_of(t), "body"); /* no focus tracking */
  (void)ctx;
  (void)args;
  (void)argc;
  return b ? wrap_node(b) : js_null();
}
static js_value doc_root_el(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *root = doc_root_of(t), *h = doc_first_tag_in(root, "html");
  (void)ctx;
  (void)args;
  (void)argc;
  return wrap_node(h ? h : root);
}
static js_value doc_get_title(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *ti = doc_first_tag_in(doc_root_of(t), "title");
  (void)args;
  (void)argc;
  return ti ? text_content_value(ctx, ti) : js_mk_cstring(ctx, "");
}
static js_value doc_set_title(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *root = doc_root_of(t), *ti = doc_first_tag_in(root, "title");
  if (!ti) {
    struct dom_node *head = doc_first_tag_in(root, "head");
    ti = dom_new_element("title");
    if (ti)
      dom_append_child(head ? head : root, ti);
  }
  if (ti)
    set_text_content(ti, arg_str(ctx, A(0)));
  return js_undef();
}

/* DOMParser: parse a string into an off-document tree wrapped as a Document
 * (the doc methods above are root-relative, so querySelector/body/... operate
 * on it). The tree is tracked and freed at page teardown. */
static js_value domparser_parse(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *html = arg_str(ctx, A(0));
  struct dom_node *root;
  js_object *doc;
  js_value dv;
  (void)t;
  root = dom_parse(html, html ? (int)strlen(html) : 0);
  if (!root)
    return js_null();
  if (g_nparsed >= 64) {
    dom_free(root);
    return js_null();
  } /* table full: don't leak */
  g_parsed[g_nparsed++] = root; /* freed in js_page_end */
  gc_push_scope();
  doc = (js_object *)gc_keep(js_new_object(ctx, g_document_proto));
  doc->host = root;
  root->wrapper = doc; /* wrap_node(root) == doc */
  dv = js_obj_value(doc);
  gc_pop_scope();
  return dv;
}
static js_value domparser_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o;
  (void)thisv;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def_native(ctx, o, "parseFromString", domparser_parse, 2);
  gc_pop_scope();
  return js_obj_value(o);
}

/* ------------------------------------------------------- URL -------------- *
 * The WHATWG `URL` + `URLSearchParams` Web APIs, layered over the existing
 * curl/url.h parser (`parse_url`/`resolve_url`). Components are computed once
 * at construction and exposed as plain data properties; this is a snapshot
 * model — writing `url.pathname` does not re-serialize `url.href`, and
 * `url.searchParams` is a detached copy of the query (mutating it does not
 * update the URL). That covers the overwhelmingly common read path real pages
 * use. */

static void span_copy(char *dst, int cap, const char *s, const char *e)
{
  int n = (int)(e - s), i;
  if (n > cap - 1)
    n = cap - 1;
  if (n < 0)
    n = 0;
  for (i = 0; i < n; i++)
    dst[i] = s[i];
  dst[i] = '\0';
}
static void uint_to_str(char *dst, unsigned v)
{
  char tmp[12];
  int i = 0, j = 0;
  if (v == 0) {
    dst[0] = '0';
    dst[1] = '\0';
    return;
  }
  while (v) {
    tmp[i++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (i)
    dst[j++] = tmp[--i];
  dst[j] = '\0';
}

static int hexval(int c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
/* percent-decode [s, s+len) into a fresh malloc'd string; '+' -> space */
static char *url_decode(const char *s, int len)
{
  char *out = (char *)malloc((unsigned)(len > 0 ? len : 0) + 1);
  int i, j = 0;
  if (!out)
    return 0;
  for (i = 0; i < len; i++) {
    if (s[i] == '%' && i + 2 < len) {
      int h = hexval((unsigned char)s[i + 1]), l = hexval((unsigned char)s[i + 2]);
      if (h >= 0 && l >= 0) {
        out[j++] = (char)(h * 16 + l);
        i += 2;
        continue;
      }
    }
    out[j++] = s[i] == '+' ? ' ' : s[i];
  }
  out[j] = '\0';
  return out;
}
static int sp_unreserved(int c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '~' || c == '*';
}
/* application/x-www-form-urlencoded component encoding (space -> '+') */
static void sb_put_form(sbuf *s, const char *t)
{
  static const char *hex = "0123456789ABCDEF";
  for (; *t; t++) {
    unsigned char c = (unsigned char)*t;
    if (c == ' ')
      sb_putc(s, '+');
    else if (sp_unreserved(c))
      sb_putc(s, (char)c);
    else {
      sb_putc(s, '%');
      sb_putc(s, hex[c >> 4]);
      sb_putc(s, hex[c & 15]);
    }
  }
}

static js_object *sp_arr(js_ctx *ctx, js_value t, const char *key)
{
  js_value a = js_get(ctx, t, key);
  return a.type == JV_OBJ ? a.u.obj : 0;
}

/* parse a query string (no leading '?') into a fresh URLSearchParams object */
static js_value make_search_params(js_ctx *ctx, const char *query)
{
  js_object *sp, *ka, *va;
  const char *p = query;
  gc_push_scope();
  sp = (js_object *)gc_keep(js_new_object(ctx, g_sp_proto));
  ka = (js_object *)gc_keep(js_new_array(ctx, 0));
  va = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_def(sp, "@k", js_obj_value(ka), 0);
  js_def(sp, "@v", js_obj_value(va), 0);
  while (p && *p) {
    const char *amp = p, *eq;
    while (*amp && *amp != '&')
      amp++;
    eq = p;
    while (eq < amp && *eq != '=')
      eq++;
    if (amp > p) {
      char *k = url_decode(p, (int)(eq - p));
      char *v = url_decode(eq < amp ? eq + 1 : amp, eq < amp ? (int)(amp - eq - 1) : 0);
      if (k) {
        js_array_push(ctx, ka, js_mk_cstring(ctx, k));
        free(k);
      }
      if (v) {
        js_array_push(ctx, va, js_mk_cstring(ctx, v));
        free(v);
      }
    }
    p = *amp ? amp + 1 : amp;
  }
  gc_pop_scope();
  return js_obj_value(sp);
}

static js_value sp_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v");
  char name[256];
  unsigned i, n;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  if (!ka || !va)
    return js_null();
  n = js_array_len(ka);
  for (i = 0; i < n; i++) {
    js_value k = js_get_index(ctx, js_obj_value(ka), i);
    if (k.type == JV_STR && streq(k.u.str->data, name))
      return js_get_index(ctx, js_obj_value(va), i);
  }
  return js_null();
}
static js_value sp_get_all(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v"), *out;
  char name[256];
  unsigned i, n;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  gc_push_scope();
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (ka && va) {
    n = js_array_len(ka);
    for (i = 0; i < n; i++) {
      js_value k = js_get_index(ctx, js_obj_value(ka), i);
      if (k.type == JV_STR && streq(k.u.str->data, name))
        js_array_push(ctx, out, js_get_index(ctx, js_obj_value(va), i));
    }
  }
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value sp_has(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value v = sp_get(ctx, t, args, argc);
  return js_bool(v.type != JV_NULL);
}
static js_value sp_append(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v");
  if (ka && va) {
    gc_push_scope();
    js_array_push(ctx, ka, js_to_string(ctx, A(0)));
    js_array_push(ctx, va, js_to_string(ctx, A(1)));
    gc_pop_scope();
  }
  return js_undef();
}
/* rebuild @k/@v keeping only entries not matching `name`; if `keepfirst`, the
 * first match survives with its value replaced by `repl` */
static void sp_rebuild(js_ctx *ctx, js_value t, const char *name, int keepfirst, js_value repl)
{
  js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v"), *nk, *nv;
  unsigned i, n;
  int seen = 0;
  if (!ka || !va)
    return;
  gc_push_scope();
  nk = (js_object *)gc_keep(js_new_array(ctx, 0));
  nv = (js_object *)gc_keep(js_new_array(ctx, 0));
  n = js_array_len(ka);
  for (i = 0; i < n; i++) {
    js_value k = js_get_index(ctx, js_obj_value(ka), i);
    int match = k.type == JV_STR && streq(k.u.str->data, name);
    if (match && keepfirst && !seen) {
      seen = 1;
      js_array_push(ctx, nk, k);
      js_array_push(ctx, nv, repl);
    } else if (!match) {
      js_array_push(ctx, nk, k);
      js_array_push(ctx, nv, js_get_index(ctx, js_obj_value(va), i));
    }
  }
  if (keepfirst && !seen) {
    js_array_push(ctx, nk, js_mk_cstring(ctx, name));
    js_array_push(ctx, nv, repl);
  }
  js_def(t.u.obj, "@k", js_obj_value(nk), 0);
  js_def(t.u.obj, "@v", js_obj_value(nv), 0);
  gc_pop_scope();
}
static js_value sp_set(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[256];
  js_value repl;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  gc_push_scope();
  repl = js_to_string(ctx, A(1));
  if (repl.type == JV_STR)
    gc_keep(repl.u.str);
  sp_rebuild(ctx, t, name, 1, repl);
  gc_pop_scope();
  return js_undef();
}
static js_value sp_delete(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[256];
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  sp_rebuild(ctx, t, name, 0, js_undef());
  return js_undef();
}
static js_value sp_for_each(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v");
  js_value cb = A(0), cbargs[3];
  unsigned i, n;
  if (!js_is_callable(cb) || !ka || !va)
    return js_undef();
  n = js_array_len(ka);
  for (i = 0; i < n; i++) {
    cbargs[0] = js_get_index(ctx, js_obj_value(va), i); /* value */
    cbargs[1] = js_get_index(ctx, js_obj_value(ka), i); /* key   */
    cbargs[2] = t;
    js_call(ctx, cb, js_undef(), cbargs, 3);
    if (ctx->has_exc)
      break;
  }
  return js_undef();
}
static js_value sp_tostring(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v");
  sbuf sb;
  js_value r;
  unsigned i, n;
  (void)args;
  (void)argc;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  if (ka && va) {
    n = js_array_len(ka);
    for (i = 0; i < n; i++) {
      js_value k = js_get_index(ctx, js_obj_value(ka), i);
      js_value v = js_get_index(ctx, js_obj_value(va), i);
      if (sb.n)
        sb_putc(&sb, '&');
      sb_put_form(&sb, k.type == JV_STR ? k.u.str->data : "");
      sb_putc(&sb, '=');
      sb_put_form(&sb, v.type == JV_STR ? v.u.str->data : "");
    }
  }
  r = js_mk_cstring(ctx, sb.b ? sb.b : "");
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value sp_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  const char *q = arg_str(ctx, A(0));
  (void)thisv;
  if (*q == '?')
    q++;
  return make_search_params(ctx, q);
}

/* --------------------------------------------------------------- FormData --
 * FormData is a name/value multimap — the same shape as URLSearchParams — so it
 * reuses the @k/@v arrays and the sp_* methods. `new FormData(formEl)` seeds it
 * from the form's successful controls. Serialized as urlencoded when used as a
 * fetch body: there is no file input here, so multipart carries no benefit. */
static void fd_collect(js_ctx *ctx, struct dom_node *n, js_object *ka, js_object *va)
{
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type == DOM_ELEMENT) {
      const char *tag = c->tag, *name = dom_get_attr(c, "name");
      const char *type = dom_get_attr(c, "type");
      if (name && name[0] && (streq(tag, "input") || streq(tag, "select") || streq(tag, "textarea"))) {
        int skip = type && (streq(type, "submit") || streq(type, "button") || streq(type, "reset") ||
                            streq(type, "file") || streq(type, "image"));
        if (!skip) {
          if (type && (streq(type, "checkbox") || streq(type, "radio"))) {
            if (dom_get_attr(c, "checked")) {
              const char *v = dom_get_attr(c, "value");
              js_array_push(ctx, ka, js_mk_cstring(ctx, name));
              js_array_push(ctx, va, js_mk_cstring(ctx, v ? v : "on"));
            }
          } else {
            const char *v = dom_get_attr(c, "value");
            js_array_push(ctx, ka, js_mk_cstring(ctx, name));
            js_array_push(ctx, va, js_mk_cstring(ctx, v ? v : ""));
          }
        }
      }
    }
    fd_collect(ctx, c, ka, va);
  }
}
static js_value fd_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *fd, *ka, *va;
  (void)thisv;
  gc_push_scope();
  fd = (js_object *)gc_keep(js_new_object(ctx, g_formdata_proto));
  ka = (js_object *)gc_keep(js_new_array(ctx, 0));
  va = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_def(fd, "@k", js_obj_value(ka), 0);
  js_def(fd, "@v", js_obj_value(va), 0);
  if (argc >= 1 && A(0).type == JV_OBJ) {
    struct dom_node *form = node_of(A(0));
    if (form && streq(form->tag, "form"))
      fd_collect(ctx, form, ka, va);
  }
  gc_pop_scope();
  return js_obj_value(fd);
}

/* ------------------------------------------------------- HTMLFormElement ---
 * form.submit() navigates without firing 'submit'; requestSubmit() fires it
 * first (and honors an optional submitter's formaction/formmethod). Both defer
 * to the worker's browser_form_submit, which builds the GET query / POST body.
 */
static js_value form_submit(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  if (n)
    browser_form_submit(n, 0);
  return js_undef();
}
static js_value form_request_submit(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *submitter = 0;
  (void)ctx;
  if (argc >= 1 && A(0).type == JV_OBJ)
    submitter = node_of(A(0));
  if (submitter)
    browser_form_submit(submitter, 1);
  else if (n)
    browser_form_submit(n, 1);
  return js_undef();
}
/* form.elements: a fresh array of the form's listed controls, also keyed by
 * each control's name/id for named access (form.elements.foo). */
static void fe_collect(js_ctx *ctx, struct dom_node *n, js_object *arr)
{
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type == DOM_ELEMENT) {
      const char *tag = c->tag;
      if (streq(tag, "input") || streq(tag, "select") || streq(tag, "textarea") || streq(tag, "button") ||
          streq(tag, "fieldset") || streq(tag, "output") || streq(tag, "object")) {
        js_value w = wrap_node(c);
        const char *nm = dom_get_attr(c, "name"), *id = dom_get_attr(c, "id");
        js_array_push(ctx, arr, w);
        if (nm && nm[0] && !streq(nm, "length"))
          js_def(arr, nm, w, 0);
        else if (id && id[0] && !streq(id, "length"))
          js_def(arr, id, w, 0);
      }
    }
    fe_collect(ctx, c, arr);
  }
}
static js_value form_elements(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_object *arr;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (n)
    fe_collect(ctx, n, arr);
  gc_pop_scope();
  return js_obj_value(arr);
}

/* locate the path/query/fragment of an absolute URL (pointers into `abs`) */
static void url_parts(const char *abs, const char **path_s, const char **path_e, const char **query, const char **frag)
{
  const char *p = abs;
  /* skip "scheme://" if present */
  while (*p && !(p[0] == ':' && p[1] == '/' && p[2] == '/')) {
    if (*p == '/' || *p == '?' || *p == '#')
      break; /* no authority */
    p++;
  }
  if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
    p += 3;
    while (*p && *p != '/' && *p != '?' && *p != '#')
      p++; /* past authority */
  } else {
    p = abs;
  }
  *path_s = p;
  while (*p && *p != '?' && *p != '#')
    p++;
  *path_e = p;
  if (*p == '?') {
    *query = p;
    while (*p && *p != '#')
      p++;
  } else
    *query = 0;
  *frag = (*p == '#') ? p : 0;
}

/* Populate `o` with the WHATWG URL component properties of absolute URL `abs`
 * (href/protocol/host/hostname/port/pathname/search/hash/origin +
 * searchParams). Shared by the URL constructor and window.location. Bad URLs
 * yield blank parts. */
static void fill_url_props(js_ctx *ctx, js_object *o, const char *abs)
{
  char protocol[16], host[URL_HOST_MAX + 8], port[12], origin[URL_HOST_MAX + 24];
  char pathbuf[URL_PATH_MAX], searchbuf[URL_PATH_MAX], hashbuf[URL_PATH_MAX];
  struct purl pu;
  const char *path_s, *path_e, *query, *frag;
  unsigned defport;
  sbuf href;
  if (!parse_url(abs, &pu)) {
    pu.tls = 0;
    pu.host[0] = '\0';
    pu.port = 80;
  }

  strlcpy(protocol, pu.tls ? "https:" : "http:", sizeof protocol);
  defport = pu.tls ? 443u : 80u;
  if ((unsigned)pu.port == defport)
    port[0] = '\0';
  else
    uint_to_str(port, (unsigned)pu.port);

  if (port[0]) {
    strlcpy(host, pu.host, sizeof host);
    {
      int l = (int)strlen(host);
      host[l] = ':';
      strlcpy(host + l + 1, port, (int)sizeof host - l - 1);
    }
  } else
    strlcpy(host, pu.host, sizeof host);

  url_parts(abs, &path_s, &path_e, &query, &frag);
  if (path_e > path_s)
    span_copy(pathbuf, sizeof pathbuf, path_s, path_e);
  else
    strlcpy(pathbuf, "/", sizeof pathbuf);
  if (query)
    span_copy(searchbuf, sizeof searchbuf, query, frag ? frag : query + strlen(query));
  else
    searchbuf[0] = '\0';
  if (frag)
    strlcpy(hashbuf, frag, sizeof hashbuf);
  else
    hashbuf[0] = '\0';

  {
    int l;
    strlcpy(origin, protocol, sizeof origin);
    l = (int)strlen(origin);
    origin[l] = '/';
    origin[l + 1] = '/';
    strlcpy(origin + l + 2, host, (int)sizeof origin - l - 2);
  }

  gc_push_scope();
  gc_keep(o);
  href.b = 0;
  href.n = 0;
  href.cap = 0;
  sb_puts(&href, origin);
  sb_puts(&href, pathbuf);
  sb_puts(&href, searchbuf);
  sb_puts(&href, hashbuf);
  js_def(o, "href", js_mk_cstring(ctx, href.b ? href.b : ""), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "protocol", js_mk_cstring(ctx, protocol), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "hostname", js_mk_cstring(ctx, pu.host), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "host", js_mk_cstring(ctx, host), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "port", js_mk_cstring(ctx, port), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "pathname", js_mk_cstring(ctx, pathbuf), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "search", js_mk_cstring(ctx, searchbuf), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "hash", js_mk_cstring(ctx, hashbuf), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "origin", js_mk_cstring(ctx, origin), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "username", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "password", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "searchParams", make_search_params(ctx, searchbuf[0] == '?' ? searchbuf + 1 : searchbuf),
         P_ENUMERABLE | P_CONFIGURABLE);
  if (href.b)
    free(href.b);
  gc_pop_scope();
}

/* Anchor (and any href-bearing element) URL decomposition: .protocol/.host/
 * .hostname/.port/.pathname/.search/.hash/.origin, computed from the element's
 * href resolved against the page URL (read-only — the common DOM use). */
enum { AURL_PROTOCOL, AURL_HOSTNAME, AURL_PORT, AURL_HOST, AURL_ORIGIN, AURL_PATHNAME, AURL_SEARCH, AURL_HASH };
static js_value anchor_url_part(js_ctx *ctx, js_value t, int part)
{
  struct dom_node *n = node_of(t);
  const char *href = n ? dom_get_attr(n, "href") : 0;
  char abs[URL_MAX];
  struct purl pu;
  const char *path_s, *path_e, *query, *frag;
  char buf[URL_PATH_MAX];
  unsigned defport;
  if (!href || !*href)
    return js_mk_cstring(ctx, "");
  resolve_url(g_page_url_buf, href, abs, sizeof abs);
  if (!parse_url(abs, &pu))
    return js_mk_cstring(ctx, "");
  defport = pu.tls ? 443u : 80u;
  switch (part) {
  case AURL_PROTOCOL:
    return js_mk_cstring(ctx, pu.tls ? "https:" : "http:");
  case AURL_HOSTNAME:
    return js_mk_cstring(ctx, pu.host);
  case AURL_PORT: {
    char p[12];
    if ((unsigned)pu.port == defport)
      return js_mk_cstring(ctx, "");
    uint_to_str(p, (unsigned)pu.port);
    return js_mk_cstring(ctx, p);
  }
  case AURL_HOST: {
    char host[URL_HOST_MAX + 8];
    strlcpy(host, pu.host, sizeof host);
    if ((unsigned)pu.port != defport) {
      int l = (int)strlen(host);
      host[l] = ':';
      uint_to_str(host + l + 1, (unsigned)pu.port);
    }
    return js_mk_cstring(ctx, host);
  }
  case AURL_ORIGIN: {
    char o[URL_HOST_MAX + 24];
    int l;
    strlcpy(o, pu.tls ? "https://" : "http://", sizeof o);
    l = (int)strlen(o);
    strlcpy(o + l, pu.host, (int)sizeof o - l);
    if ((unsigned)pu.port != defport) {
      int m = (int)strlen(o);
      o[m] = ':';
      uint_to_str(o + m + 1, (unsigned)pu.port);
    }
    return js_mk_cstring(ctx, o);
  }
  default:
    break;
  }
  url_parts(abs, &path_s, &path_e, &query, &frag);
  if (part == AURL_PATHNAME) {
    if (path_e > path_s)
      span_copy(buf, sizeof buf, path_s, path_e);
    else
      strlcpy(buf, "/", sizeof buf);
  } else if (part == AURL_SEARCH) {
    if (query)
      span_copy(buf, sizeof buf, query, frag ? frag : query + strlen(query));
    else
      buf[0] = '\0';
  } else if (part == AURL_HASH) {
    if (frag)
      strlcpy(buf, frag, sizeof buf);
    else
      buf[0] = '\0';
  } else
    buf[0] = '\0';
  return js_mk_cstring(ctx, buf);
}
#define AURL_ACC(name, code)                                                                                           \
  static js_value el_get_##name(js_ctx *ctx, js_value t, js_value *args, int argc)                                     \
  {                                                                                                                    \
    (void)args;                                                                                                        \
    (void)argc;                                                                                                        \
    return anchor_url_part(ctx, t, code);                                                                              \
  }
AURL_ACC(protocol, AURL_PROTOCOL)
AURL_ACC(hostname, AURL_HOSTNAME)
AURL_ACC(port, AURL_PORT)
AURL_ACC(host, AURL_HOST)
AURL_ACC(origin, AURL_ORIGIN)
AURL_ACC(pathname, AURL_PATHNAME)
AURL_ACC(search, AURL_SEARCH)
AURL_ACC(hash, AURL_HASH)
#undef AURL_ACC

static js_value url_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  char input[URL_MAX], abs[URL_MAX];
  struct purl pu;
  js_object *o;
  js_value ov;
  (void)thisv;
  strlcpy(input, arg_str(ctx, A(0)), sizeof input);
  if (argc >= 2 && A(1).type != JV_UNDEF && A(1).type != JV_NULL) {
    char base[URL_MAX];
    strlcpy(base, arg_str(ctx, A(1)), sizeof base);
    resolve_url(base, input, abs, sizeof abs);
  } else {
    strlcpy(abs, input, sizeof abs);
  }
  if (!parse_url(abs, &pu))
    return js_throw_type(ctx, "Invalid URL");
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_url_proto));
  fill_url_props(ctx, o, abs);
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}
static js_value url_tostring(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return js_get(ctx, t, "href");
}

/* ------------------------------------------------------- DOM events -------- *
 * Listeners are stored on the target object (a node wrapper, the document, or
 * window) under a hidden own "@ev" array of {type, fn, capture} records.
 * Dispatch walks the node's ancestor chain to build the propagation path, then
 * runs the capture phase (outermost -> target), the target, and the bubble
 * phase (target -> outermost), honouring stopPropagation/preventDefault. The
 * whole thing runs on the worker thread (the only thread that touches the
 * engine). */

static js_value report_exc_v(js_ctx *ctx); /* fwd: defined with the runner below */
static void js_set_event_script(const char *type);

/* The own "@ev" listener array of `target`, created on demand when `create`. */
static js_object *listeners_of(js_object *target, int create)
{
  pentry *pe = pmap_find_pub(target, "@ev");
  if (pe && pe->value.type == JV_OBJ)
    return pe->value.u.obj;
  if (!create)
    return 0;
  {
    js_object *arr;
    gc_push_scope();
    arr = (js_object *)gc_keep(js_new_array(g_ctx, 0));
    js_def(target, "@ev", js_obj_value(arr), 0);
    gc_pop_scope();
    return arr;
  }
}

int ev_flag(js_value ev, const char *k) { return js_to_boolean(js_get(g_ctx, ev, k)); }

/* Invoke the listeners on `obj` whose capture flag equals `want_capture` and
 * whose type matches the event's. currentTarget is set to `obj` for the
 * duration. */
void ev_fire_on(js_object *obj, js_value ev, int want_capture)
{
  js_object *list = listeners_of(obj, 0);
  js_value evty;
  unsigned i, n;
  gc_push_scope();
  if (list)
    gc_keep(list); /* a handler may replace @ev; pin this copy */
  js_set(g_ctx, ev, "currentTarget", js_obj_value(obj));
  evty = js_get(g_ctx, ev, "type");
  n = list ? js_array_len(list) : 0;
  for (i = 0; i < n; i++) {
    js_value e = js_get_index(g_ctx, js_obj_value(list), i);
    js_value ty, fn, a;
    if (e.type != JV_OBJ)
      continue;
    if (js_to_boolean(js_get(g_ctx, e, "capture")) != want_capture)
      continue;
    ty = js_get(g_ctx, e, "type");
    if (!(ty.type == JV_STR && evty.type == JV_STR && streq(ty.u.str->data, evty.u.str->data)))
      continue;
    fn = js_get(g_ctx, e, "fn");
    if (!js_is_callable(fn))
      continue;
    a = ev;
    js_set_event_script(evty.type == JV_STR ? evty.u.str->data : 0);
    js_call(g_ctx, fn, js_obj_value(obj), &a, 1);
    if (g_ctx->has_exc)
      report_exc_v(g_ctx);
  }
  /* on<type> property handler (onload/onclick/onchange/…): a plain callable
   * property, fired in the non-capture phase alongside addEventListener
   * listeners. Many pages wire their entry point via `onload = fn` (a
   * property), not addEventListener — without this it never runs. */
  if (!want_capture && evty.type == JV_STR && evty.u.str->len > 0 && evty.u.str->len < 40) {
    char onname[48];
    int k = 0;
    const char *ty = evty.u.str->data;
    onname[k++] = 'o';
    onname[k++] = 'n';
    while (*ty && k < 47)
      onname[k++] = *ty++;
    onname[k] = 0;
    js_value h = js_get(g_ctx, js_obj_value(obj), onname);
    if (js_is_callable(h)) {
      js_value a = ev;
      js_set_event_script(evty.u.str->data);
      js_call(g_ctx, h, js_obj_value(obj), &a, 1);
      if (g_ctx->has_exc)
        report_exc_v(g_ctx);
    }
  }
  gc_pop_scope();
}

/* Dispatch `ev` starting at wrapper `startobj` (its host node anchors the
 * path). Returns 1 if a listener called preventDefault. */
static int dispatch_from(js_object *startobj, js_value ev)
{
  js_object *path[128];
  int np = 0, i;
  struct dom_node *target = (struct dom_node *)startobj->host;
  gc_push_scope();
  gc_keep(startobj);
  /* hold ev across the calls below */
  if (ev.type == JV_OBJ)
    gc_keep(ev.u.obj);
  js_set(g_ctx, ev, "target", js_obj_value(startobj));

  if (target) {
    for (struct dom_node *n = target; n && np < 126; n = n->parent)
      if (n->wrapper)
        path[np++] = (js_object *)n->wrapper;
  } else {
    path[np++] = startobj; /* window/detached: just itself */
  }
  /* window (global) is the outermost target, after the document */
  if (g_ctx->global && np < 127 && path[np ? np - 1 : 0] != g_ctx->global)
    path[np++] = g_ctx->global;

  for (i = np - 1; i >= 1 && !ev_flag(ev, "@sp"); i--)
    ev_fire_on(path[i], ev, 1); /* capture */
  if (!ev_flag(ev, "@sp")) {
    ev_fire_on(path[0], ev, 1);
    ev_fire_on(path[0], ev, 0);
  } /* target */
  for (i = 1; i < np && !ev_flag(ev, "@sp"); i++)
    ev_fire_on(path[i], ev, 0); /* bubble  */

  gc_pop_scope();
  return ev_flag(ev, "@dp");
}

static js_value ev_prevent(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ) {
    js_set(ctx, t, "@dp", js_bool(1));
    js_set(ctx, t, "defaultPrevented", js_bool(1));
  }
  return js_undef();
}
static js_value ev_stop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ)
    js_set(ctx, t, "@sp", js_bool(1));
  return js_undef();
}

/* Build an Event object on g_event_proto. `init` (or undefined) supplies
 * bubbles/ cancelable/detail when present (CustomEvent / new Event(type,
 * init)). */
js_value make_event(const char *type, int x, int y, js_value init)
{
  js_object *e;
  int bubbles = 1, cancelable = 1;
  js_value detail = js_undef();
  if (init.type == JV_OBJ) {
    js_value b = js_get(g_ctx, init, "bubbles");
    js_value c = js_get(g_ctx, init, "cancelable");
    if (b.type != JV_UNDEF)
      bubbles = js_to_boolean(b);
    if (c.type != JV_UNDEF)
      cancelable = js_to_boolean(c);
    detail = js_get(g_ctx, init, "detail");
  }
  gc_push_scope();
  if (init.type == JV_OBJ)
    gc_keep(init.u.obj);
  e = (js_object *)gc_keep(js_new_object(g_ctx, g_event_proto));
  js_def(e, "type", js_mk_cstring(g_ctx, type), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "bubbles", js_bool(bubbles), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "cancelable", js_bool(cancelable), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "defaultPrevented", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "target", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "currentTarget", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "clientX", js_number(x), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "clientY", js_number(y), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "detail", detail, P_WRITABLE | P_ENUMERABLE);
  js_def(e, "@dp", js_bool(0), P_WRITABLE);
  js_def(e, "@sp", js_bool(0), P_WRITABLE);
  gc_pop_scope();
  return js_obj_value(e);
}

js_value el_add_listener(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char type[64];
  js_value fn = A(1);
  int capture = 0;
  if (t.type != JV_OBJ || !js_is_callable(fn))
    return js_undef();
  strlcpy(type, arg_str(ctx, A(0)), sizeof type);
  if (argc >= 3) {
    js_value o = A(2);
    if (o.type == JV_BOOL)
      capture = o.u.boolean;
    else if (o.type == JV_OBJ)
      capture = js_to_boolean(js_get(ctx, o, "capture"));
  }
  gc_push_scope();
  gc_keep(t.u.obj);
  if (fn.type == JV_OBJ)
    gc_keep(fn.u.obj);
  {
    js_object *list = listeners_of(t.u.obj, 1);
    js_object *e;
    gc_keep(list);
    e = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(e, "type", js_mk_cstring(ctx, type), 0);
    js_def(e, "fn", fn, 0);
    js_def(e, "capture", js_bool(capture), 0);
    js_array_push(ctx, list, js_obj_value(e));
  }
  gc_pop_scope();
  g_has_listeners = 1;
  return js_undef();
}
js_value el_remove_listener(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char type[64];
  js_value fn = A(1);
  js_object *list, *nl;
  unsigned i, n;
  int capture = 0;
  if (t.type != JV_OBJ)
    return js_undef();
  strlcpy(type, arg_str(ctx, A(0)), sizeof type);
  if (argc >= 3) {
    js_value o = A(2);
    if (o.type == JV_BOOL)
      capture = o.u.boolean;
    else if (o.type == JV_OBJ)
      capture = js_to_boolean(js_get(ctx, o, "capture"));
  }
  list = listeners_of(t.u.obj, 0);
  if (!list)
    return js_undef();
  gc_push_scope();
  gc_keep(t.u.obj);
  gc_keep(list);
  if (fn.type == JV_OBJ)
    gc_keep(fn.u.obj);
  nl = (js_object *)gc_keep(js_new_array(ctx, 0));
  n = js_array_len(list);
  for (i = 0; i < n; i++) {
    js_value e = js_get_index(ctx, js_obj_value(list), i);
    js_value ty = js_get(ctx, e, "type"), ef = js_get(ctx, e, "fn");
    int ec = js_to_boolean(js_get(ctx, e, "capture"));
    int match = ty.type == JV_STR && streq(ty.u.str->data, type) && js_strict_eq(ef, fn) && ec == capture;
    if (!match)
      js_array_push(ctx, nl, e);
  }
  js_def(t.u.obj, "@ev", js_obj_value(nl), 0);
  gc_pop_scope();
  return js_undef();
}
js_value el_dispatch(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value ev = A(0);
  (void)argc;
  if (t.type != JV_OBJ || ev.type != JV_OBJ)
    return js_bool(1);
  return js_bool(!dispatch_from(t.u.obj, ev));
}
static js_value event_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  return make_event(arg_str(ctx, A(0)), 0, 0, A(1));
}

/* The init-dict fields carried by the Event subclasses (UIEvent/MouseEvent/
 * KeyboardEvent/WheelEvent/...). Each has the same name on the event object, so
 * event_sub_ctor overlays whichever the caller supplied onto the base Event. */
static const char *const k_event_init_keys[] = {
    "view",      "detail",      "clientX",       "clientY",   "screenX", "screenY", "pageX",
    "pageY",     "movementX",   "movementY",     "button",    "buttons", "ctrlKey", "shiftKey",
    "altKey",    "metaKey",     "relatedTarget", "key",       "code",    "keyCode", "which",
    "charCode",  "repeat",      "location",      "deltaX",    "deltaY",  "deltaZ",  "deltaMode",
    "pointerId", "pointerType", "pressure",      "isPrimary", "data",
};

/* Constructor shared by every Event subclass. It builds a base Event,
 * re-parents it to the subclass prototype (carried in fn->data) so instanceof
 * reflects the concrete interface, then overlays the interface-specific
 * init-dict fields. */
static js_value event_sub_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value ev = make_event(arg_str(ctx, A(0)), 0, 0, A(1));
  js_value proto = ctx->native_data;
  (void)thisv;
  if (ev.type != JV_OBJ)
    return ev;
  if (proto.type == JV_OBJ)
    ev.u.obj->proto = proto.u.obj;
  if (argc >= 2 && A(1).type == JV_OBJ) {
    unsigned i;
    gc_push_scope();
    gc_keep(ev.u.obj);
    gc_keep(A(1).u.obj);
    for (i = 0; i < sizeof k_event_init_keys / sizeof k_event_init_keys[0]; i++) {
      js_value v = js_get(ctx, A(1), k_event_init_keys[i]);
      if (v.type != JV_UNDEF)
        js_def(ev.u.obj, k_event_init_keys[i], v, P_WRITABLE | P_ENUMERABLE);
    }
    gc_pop_scope();
  }
  return ev;
}

/* C entry: the worker resolved a content click to `target`; fire `type` on it.
 * Returns 1 if a listener called preventDefault (so navigation is suppressed).
 */
int js_dispatch_dom_event(struct dom_node *target, const char *type, int x, int y)
{
  js_value tgt, ev;
  int prevented;
  if (!g_page_live || !target)
    return 0;
  gc_push_scope();
  tgt = wrap_node(target);
  if (tgt.type != JV_OBJ) {
    gc_pop_scope();
    return 0;
  }
  gc_keep(tgt.u.obj);
  ev = make_event(type, x, y, js_undef());
  if (ev.type == JV_OBJ)
    gc_keep(ev.u.obj);
  prevented = dispatch_from(tgt.u.obj, ev);
  gc_pop_scope();
  return prevented;
}

int js_has_listeners(void) { return g_page_live && g_has_listeners; }

/* ------------------------------------------------------- web components ---- *
 * customElements.define(name, class) registers a tag; HTMLElement is the base a
 * custom class extends. Upgrading a node means pointing its wrapper's prototype
 * at the class's, running the constructor ON that wrapper (the engine inits
 * fields + runs super() on the given `this`), then firing connectedCallback
 * when it's in the tree. Existing matching nodes upgrade at define time;
 * createElement upgrades a defined tag; appendChild fires connectedCallback as
 * nodes connect. */

static js_value htmlelement_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)ctx;
  (void)thisv;
  (void)args;
  (void)argc; /* super(): a no-op base */
  return js_undef();
}

static int ce_is_upgraded(js_object *w) { return w && pmap_find_pub(w, "@ce") != 0; }
static int ce_connected(struct dom_node *n)
{
  for (; n; n = n->parent)
    if (n == g_doc_root)
      return 1;
  return 0;
}
static js_value ce_lookup(const char *tag)
{
  pentry *pe = g_custom_elements ? pmap_find_pub(g_custom_elements, tag) : 0;
  return pe ? pe->value : js_undef();
}
static void ce_call_cb(js_object *w, const char *cb, js_value *args, int n)
{
  js_value f;
  if (!w)
    return;
  f = js_get(g_ctx, js_obj_value(w), cb);
  if (!js_is_callable(f))
    return;
  gc_push_scope();
  gc_keep(w);
  {
    int i;
    for (i = 0; i < n; i++)
      if (args[i].type == JV_OBJ)
        gc_keep(args[i].u.obj);
  }
  js_set_cur_script(cb); /* the lifecycle callback name */
  js_call(g_ctx, f, js_obj_value(w), args, n);
  if (g_ctx->has_exc)
    report_exc_v(g_ctx);
  gc_pop_scope();
}

/* Upgrade `n` to its custom class `cls`: retarget the wrapper's prototype and
 * run the constructor on it. Idempotent. Does not fire connectedCallback. */
static void ce_upgrade_node(struct dom_node *n, js_value cls)
{
  js_object *w;
  js_value wv, proto;
  if (!n || cls.type != JV_OBJ)
    return;
  wv = wrap_node(n);
  if (wv.type != JV_OBJ)
    return;
  w = wv.u.obj;
  if (ce_is_upgraded(w))
    return;
  gc_push_scope();
  gc_keep(w);
  gc_keep(cls.u.obj);
  proto = js_get(g_ctx, cls, "prototype");
  if (proto.type == JV_OBJ)
    w->proto = proto.u.obj;
  js_def(w, "@ce", js_bool(1), 0);
  js_set_cur_script("<custom element constructor>");
  js_call(g_ctx, cls, wv, 0, 0); /* constructor + fields + super() */
  if (g_ctx->has_exc)
    report_exc_v(g_ctx);
  gc_pop_scope();
  /* spec order: after the constructor, attributeChangedCallback for each
   * observed attribute already present (ce_attr_changed filters to the observed
   * set). */
  {
    struct dom_attr *a;
    for (a = n->attrs; a; a = a->next)
      ce_attr_changed(n, a->name, 0, a->value);
  }
}
/* Fire connectedCallback once, when an upgraded element is in the document. */
static void ce_connect(struct dom_node *n)
{
  js_object *w = n ? (js_object *)n->wrapper : 0;
  if (!w || !ce_is_upgraded(w) || pmap_find_pub(w, "@conn"))
    return;
  if (!ce_connected(n))
    return;
  js_def(w, "@conn", js_bool(1), 0);
  ce_call_cb(w, "connectedCallback", 0, 0);
}
static void ce_upgrade_connect(struct dom_node *n, js_value cls)
{
  ce_upgrade_node(n, cls);
  ce_connect(n);
}
static void ce_upgrade_tree(struct dom_node *root, const char *name, js_value cls)
{
  struct dom_node *c;
  for (c = root->first_child; c; c = c->next) {
    if (c->type == DOM_ELEMENT) {
      if (streq(c->tag, name))
        ce_upgrade_connect(c, cls);
      ce_upgrade_tree(c, name, cls);
    }
  }
}

/* setAttribute hook: fire attributeChangedCallback for an observed attribute.
 */
static void ce_attr_changed(struct dom_node *n, const char *name, const char *oldv, const char *newv)
{
  js_object *w = n ? (js_object *)n->wrapper : 0;
  js_value ctor, obs;
  unsigned i, len;
  int observed = 0;
  if (!w || !ce_is_upgraded(w))
    return;
  ctor = js_get(g_ctx, js_obj_value(w), "constructor");
  if (ctor.type != JV_OBJ)
    return;
  obs = js_get(g_ctx, ctor, "observedAttributes");
  if (!js_is_array(obs))
    return;
  len = js_array_len(obs.u.obj);
  for (i = 0; i < len; i++) {
    js_value e = js_get_index(g_ctx, obs, i);
    if (e.type == JV_STR && streq(e.u.str->data, name)) {
      observed = 1;
      break;
    }
  }
  if (!observed)
    return;
  gc_push_scope();
  {
    js_value a[3];
    a[0] = js_mk_cstring(g_ctx, name);
    gc_keep(a[0].u.str);
    a[1] = oldv ? js_mk_cstring(g_ctx, oldv) : js_null();
    if (a[1].type == JV_STR)
      gc_keep(a[1].u.str);
    a[2] = newv ? js_mk_cstring(g_ctx, newv) : js_null();
    if (a[2].type == JV_STR)
      gc_keep(a[2].u.str);
    ce_call_cb(w, "attributeChangedCallback", a, 3);
  }
  gc_pop_scope();
}

static js_value ce_define(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[64];
  js_value cls = A(1);
  (void)t;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  if (!name[0] || !js_is_callable(cls))
    return js_undef();
  js_def(g_custom_elements, name, cls, 0);
  if (g_doc_root)
    ce_upgrade_tree(g_doc_root, name, cls);
  return js_undef();
}
static js_value ce_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[64];
  pentry *pe;
  (void)t;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  pe = pmap_find_pub(g_custom_elements, name);
  return pe ? pe->value : js_undef();
}
static js_value ce_when_defined(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc; /* define is synchronous: already settled */
  return js_promise_resolved(ctx, js_undef());
}

/* ------------------------------------------------------- ES modules -------- *
 * The engine calls browser_load_module() for `import` (static + dynamic). It
 * resolves the specifier against the importing module (or the page), fetches
 * the source over the same batch path scripts/CSS use, parses + executes it
 * ONCE in a module scope, and caches the namespace by absolute URL. Synchronous
 * — which fits the engine's no-event-loop model; dynamic import() just wraps
 * the result in an already-settled Promise. Only URL-like specifiers resolve
 * (no import maps). */

static js_value report_exc_v(js_ctx *ctx); /* (defined with the runner below) */

/* Keep a parsed script's arena alive for the page (closures reference its AST).
 * The list grows on demand — a page may load any number of scripts/modules. */
int arenas_push(js_arena *ar)
{
  if (g_narenas == g_arenas_cap) {
    int nc = g_arenas_cap ? g_arenas_cap * 2 : 16;
    js_arena **na = (js_arena **)realloc(g_arenas, (unsigned)nc * sizeof *na);
    if (!na)
      return 0;
    g_arenas = na;
    g_arenas_cap = nc;
  }
  g_arenas[g_narenas++] = ar;
  return 1;
}

/* Append a ":line:col" (or ":line") source position to `sb`, for SyntaxError
 * reports. Nothing is written when the parser gave no line. */
static void sb_put_pos(sbuf *sb, int line, int col)
{
  char n[16];
  int i;
  if (line <= 0)
    return;
  i = 0;
  uapp(n, &i, sizeof n, (unsigned)line);
  sb_putc(sb, ':');
  sb_puts(sb, n);
  if (col > 0) {
    i = 0;
    uapp(n, &i, sizeof n, (unsigned)col);
    sb_putc(sb, ':');
    sb_puts(sb, n);
  }
}

static js_node *module_compile(const char *code, const char *url)
{
  js_arena *ar;
  js_node *prog;
  js_parse_error pe;
  ar = (js_arena *)malloc(sizeof *ar);
  if (!ar)
    return 0;
  js_arena_init(ar);
  prog = js_parse(code, ar, &pe);
  if (!prog) {
    sbuf sb;
    sb.b = 0;
    sb.n = 0;
    sb.cap = 0;
    sb_puts(&sb, "[js] module SyntaxError in ");
    sb_puts(&sb, url ? url : "(inline)");
    sb_put_pos(&sb, pe.line, pe.col);
    sb_puts(&sb, ": ");
    sb_puts(&sb, pe.msg ? pe.msg : "parse error");
    sb_putc(&sb, '\n');
    devtools_console_append(DT_ERROR, sb.b, sb.n);
    if (sb.b)
      free(sb.b);
    js_arena_free(ar);
    free(ar);
    return 0;
  }
  if (!arenas_push(ar)) {
    js_arena_free(ar);
    free(ar);
    return 0;
  }
  return prog;
}

/* True if the identifier word `w` (with a word boundary before it) ends at `q`,
 * skipping whitespace — used to spot `from '…'` / `import '…'` specifiers. */
static char *sdup(const char *s); /* defined below */
static int mod_word_before(const char *src, const char *q, const char *w)
{
  const char *e = q;
  while (e > src && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
    e--;
  int wl = (int)strlen(w);
  if (e - src < wl)
    return 0;
  for (int i = 0; i < wl; i++)
    if (e[-wl + i] != w[i])
      return 0;
  if (e - src > wl) {
    char c = e[-wl - 1];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9'))
      return 0;
  }
  return 1;
}

/* Scan a module's source for its static import specifiers and batch-fetch the
 * uncached ones in ONE parallel g_fetch (multiplexed like stylesheets), caching
 * the sources in g_module_src. This turns a deep import graph from N serial
 * fetches into breadth-first parallel ones. Best-effort: a missed or spurious
 * hit just yields a serial fetch (or a wasted one) at import time — the
 * loader's own fetch fallback keeps it correct regardless of what the scan
 * finds. */
#define MODULE_PREFETCH_MAX 192
static void module_prefetch_deps(const char *base, const char *src)
{
  char *urls[MODULE_PREFETCH_MAX];
  int n = 0;
  if (!g_fetch)
    return;
  for (const char *p = src; *p && n < MODULE_PREFETCH_MAX; p++) {
    if (*p != '\'' && *p != '"')
      continue;
    char qc = *p;
    const char *s = p + 1;
    const char *e = s;
    while (*e && *e != qc)
      e++;
    if (*e != qc)
      break; /* unterminated: stop scanning */
    if (e > s && (*s == '.' || *s == '/') && (mod_word_before(src, p, "from") || mod_word_before(src, p, "import"))) {
      char spec[URL_MAX];
      int k = 0;
      for (const char *c = s; c < e && k < URL_MAX - 1; c++)
        spec[k++] = *c;
      spec[k] = 0;
      char abs[URL_MAX];
      resolve_url(base, spec, abs, sizeof abs);
      if (!pmap_find_pub(g_module_src, abs) && !pmap_find_pub(g_module_cache, abs)) {
        int dup = 0;
        for (int i = 0; i < n; i++)
          if (streq(urls[i], abs)) {
            dup = 1;
            break;
          }
        if (!dup)
          urls[n++] = sdup(abs);
      }
    }
    p = e; /* skip the string body */
  }
  if (n) {
    unsigned char *bodies[MODULE_PREFETCH_MAX];
    int lens[MODULE_PREFETCH_MAX];
    for (int i = 0; i < n; i++) {
      bodies[i] = 0;
      lens[i] = 0;
    }
    g_fetch(urls, n, bodies, lens);
    for (int i = 0; i < n; i++) {
      if (bodies[i] && lens[i] > 0) {
        char *code = (char *)malloc((unsigned)lens[i] + 1);
        if (code) {
          memcpy(code, bodies[i], (unsigned)lens[i]);
          code[lens[i]] = 0;
          js_def(g_module_src, urls[i], js_mk_cstring(g_ctx, code), 0);
          free(code);
        }
      }
      if (bodies[i])
        free(bodies[i]);
      free(urls[i]);
    }
  }
}

static js_object *browser_load_module(js_ctx *ctx, const char *spec, void *ud)
{
  char abs[URL_MAX];
  const char *referrer = ctx->cur_module_url ? ctx->cur_module_url : g_page_url_buf;
  pentry *cached, *srcent;
  unsigned char *bodies[1];
  int lens[1];
  char *urls[1];
  js_node *prog;
  js_object *exports;
  char *code = 0;
  (void)ud;
  resolve_url(referrer, spec, abs, sizeof abs);
  cached = pmap_find_pub(g_module_cache, abs);
  if (cached && cached->value.type == JV_OBJ)
    return cached->value.u.obj;

  /* source: use the prefetched copy (batched with its siblings) if present,
   * else fall back to a serial fetch. */
  srcent = pmap_find_pub(g_module_src, abs);
  if (srcent && srcent->value.type == JV_STR) {
    int L = srcent->value.u.str->len;
    code = (char *)malloc((unsigned)L + 1);
    if (code) {
      memcpy(code, srcent->value.u.str->data, (unsigned)L);
      code[L] = 0;
    }
  }
  if (!code) {
    if (!g_fetch)
      return 0;
    urls[0] = abs;
    bodies[0] = 0;
    lens[0] = 0;
    g_fetch(urls, 1, bodies, lens);
    if (!bodies[0] || lens[0] <= 0) {
      if (bodies[0])
        free(bodies[0]);
      return 0;
    }
    code = (char *)malloc((unsigned)lens[0] + 1);
    if (!code) {
      free(bodies[0]);
      return 0;
    }
    memcpy(code, bodies[0], (unsigned)lens[0]);
    code[lens[0]] = 0;
    free(bodies[0]);
  }
  module_prefetch_deps(abs, code); /* parallel breadth-first fetch of its imports */
  prog = module_compile(code, abs);
  free(code);
  if (!prog)
    return 0;
  gc_push_scope();
  exports = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(g_module_cache, abs, js_obj_value(exports), 0); /* cache before exec (cycles) */
  js_run_module(ctx, prog, exports, abs);
  if (ctx->has_exc)
    report_exc_v(ctx);
  gc_pop_scope();
  return exports;
}

/* ------------------------------------------------------- setup ------------ */

void def_method(js_object *o, const char *name, js_native fn, int arity) { js_def_native(g_ctx, o, name, fn, arity); }
void def_acc(js_object *proto, const char *name, js_native get, js_native set)
{
  gc_push_scope();
  {
    js_object *g = get ? (js_object *)gc_keep(js_new_native(g_ctx, get, name, 0)) : 0;
    js_object *s = set ? (js_object *)gc_keep(js_new_native(g_ctx, set, name, 1)) : 0;
    js_def_accessor(proto, name, g, s, P_CONFIGURABLE | P_ENUMERABLE);
  }
  gc_pop_scope();
}

/* ------------------------------------------------------- timers ----------- */

/* Register a timer; returns its id (slot+1), or 0 if the table is full or the
 * callback isn't callable. `raf` requests requestAnimationFrame semantics (the
 * callback is invoked with a single timestamp argument). */
static int timer_add(js_ctx *ctx, js_value cb, js_value *extra, int nextra, int delay, int repeat, int raf)
{
  int i, k;
  if (!g_timers || !js_is_callable(cb))
    return 0;
  for (i = 0; i < g_tcap; i++)
    if (!g_tmeta[i].active)
      break;
  if (i == g_tcap) { /* no free slot: grow the table */
    int nc = g_tcap ? g_tcap * 2 : 16;
    struct timer_meta *nt = (struct timer_meta *)realloc(g_tmeta, (unsigned)nc * sizeof *nt);
    if (!nt)
      return 0;
    for (k = g_tcap; k < nc; k++) {
      nt[k].active = nt[k].repeat = nt[k].raf = 0;
      nt[k].due = nt[k].period = 0;
    }
    g_tmeta = nt;
    g_tcap = nc;
  }
  gc_push_scope();
  {
    js_object *bundle = (js_object *)gc_keep(js_new_array(ctx, 0));
    js_array_push(ctx, bundle, cb); /* [0] = callback           */
    for (k = 0; k < nextra; k++)
      js_array_push(ctx, bundle, extra[k]); /* [1..] args */
    js_set_index(ctx, js_obj_value(g_timers), (unsigned)i, js_obj_value(bundle));
  }
  gc_pop_scope();
  g_tmeta[i].active = 1;
  g_tmeta[i].repeat = repeat ? 1 : 0;
  g_tmeta[i].raf = raf ? 1 : 0;
  g_tmeta[i].period = delay < 0 ? 0 : delay;
  g_tmeta[i].due = now_ms() + g_tmeta[i].period;
  g_tmeta[i].async = js_async_capture(ctx); /* who armed this timer */
  g_ntimers++;
  return i + 1;
}
static void timer_drop(int i)
{
  if (i < 0 || i >= g_tcap || !g_tmeta[i].active)
    return;
  g_tmeta[i].active = 0;
  if (g_tmeta[i].async) {
    free(g_tmeta[i].async);
    g_tmeta[i].async = 0;
  }
  if (g_timers)
    js_set_index(g_ctx, js_obj_value(g_timers), (unsigned)i, js_null());
  if (g_ntimers > 0)
    g_ntimers--;
}
static js_value win_set_timeout(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int delay = argc > 1 ? (int)js_to_number(ctx, args[1]) : 0;
  (void)t;
  return js_number(timer_add(ctx, A(0), args + (argc > 2 ? 2 : argc), argc > 2 ? argc - 2 : 0, delay, 0, 0));
}
static js_value win_set_interval(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int delay = argc > 1 ? (int)js_to_number(ctx, args[1]) : 0;
  (void)t;
  return js_number(timer_add(ctx, A(0), args + (argc > 2 ? 2 : argc), argc > 2 ? argc - 2 : 0, delay, 1, 0));
}
static js_value win_request_anim(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return js_number(timer_add(ctx, A(0), 0, 0, 16, 0, 1)); /* ~one 60fps frame */
}
static js_value win_clear_timer(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  if (argc > 0 && args[0].type == JV_NUM)
    timer_drop((int)args[0].u.num - 1);
  return js_undef();
}

/* Fire every timer whose deadline has passed. Called by the worker each idle
 * tick (browser engine is worker-only). Returns the number of callbacks fired
 * so the caller can repaint/relayout if any of them mutated the DOM. */
int js_timers_pump(void)
{
  int i, fired = 0;
  long long now;
  if (!g_page_live || !g_ctx || g_ntimers == 0)
    return 0;
  now = now_ms();
  for (i = 0; i < g_tcap; i++) {
    js_value bundle, cb, argv[8];
    int nargs = 0;
    if (!g_tmeta[i].active || g_tmeta[i].due > now)
      continue;
    bundle = js_get_index(g_ctx, js_obj_value(g_timers), (unsigned)i);
    if (bundle.type != JV_OBJ) {
      timer_drop(i);
      continue;
    }
    gc_push_scope();
    gc_keep(bundle.u.obj);
    cb = js_get_index(g_ctx, bundle, 0);
    if (g_tmeta[i].raf) {
      argv[nargs++] = js_number((double)now);
    } else {
      unsigned bl = js_array_len(bundle.u.obj), k;
      for (k = 1; k < bl && nargs < 8; k++)
        argv[nargs++] = js_get_index(g_ctx, bundle, k);
    }
    int raf = g_tmeta[i].raf;
    char *async = g_tmeta[i].async; /* scheduling-site frames; realloc-safe local */
    int oneshot = !g_tmeta[i].repeat;
    if (g_tmeta[i].repeat)
      g_tmeta[i].due = now + g_tmeta[i].period; /* reschedule from now (no drift pileup) */
    else {
      g_tmeta[i].async = 0; /* take ownership before the drop frees it */
      timer_drop(i);        /* one-shot: free the slot before firing */
    }
    if (js_is_callable(cb)) {
      char *prev = js_async_enter(g_ctx, async); /* callback throws → stack names the arm site */
      js_set_cur_script(raf ? "<requestAnimationFrame callback>" : "<timer callback>");
      js_call(g_ctx, cb, js_obj_value(g_ctx->global), argv, nargs);
      js_async_leave(g_ctx, prev);
      if (g_ctx->has_exc)
        report_exc(g_ctx);
    }
    if (oneshot)
      free(async); /* repeat timers keep it in the slot */
    gc_pop_scope();
    fired++;
  }
  return fired;
}

/* ------------------------------------------------------- AbortController -- *
 * AbortController/AbortSignal (DOM standard). A signal is a plain EventTarget
 * on g_abortsignal_proto, so addEventListener/removeEventListener/dispatchEvent
 * come from the shared @ev listener API. abort() flips `aborted`, records
 * `reason`, and fires "abort" exactly once (onabort handler + listeners).
 * fetch() consults an already-aborted signal up front and rejects with the
 * reason — the request runs synchronously on the worker, so there is no
 * mid-flight cancellation point. */

/* A fresh, not-yet-aborted AbortSignal. */
static js_object *new_abort_signal(js_ctx *ctx)
{
  js_object *s = js_new_object(ctx, g_abortsignal_proto);
  js_def(s, "aborted", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(s, "reason", js_undef(), P_WRITABLE | P_ENUMERABLE);
  js_def(s, "onabort", js_null(), P_WRITABLE | P_ENUMERABLE);
  return s;
}

/* Abort `signal` with `reason` (an AbortError when none is given), once: set
 * the state then fire "abort" at the onabort handler and any listeners. */
static void signal_abort(js_ctx *ctx, js_object *signal, js_value reason)
{
  js_value ev, h;
  if (!signal || js_to_boolean(js_get(ctx, js_obj_value(signal), "aborted")))
    return;
  gc_push_scope();
  gc_keep(signal);
  if (reason.type == JV_UNDEF)
    reason = js_make_error(ctx, ctx->error_proto, "AbortError", "signal is aborted without reason");
  if (reason.type == JV_OBJ)
    gc_keep(reason.u.obj);
  js_set(ctx, js_obj_value(signal), "aborted", js_bool(1));
  js_set(ctx, js_obj_value(signal), "reason", reason);
  ev = make_event("abort", 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(ctx, ev, "target", js_obj_value(signal));
    js_set(ctx, ev, "currentTarget", js_obj_value(signal));
  }
  h = js_get(ctx, js_obj_value(signal), "onabort");
  if (js_is_callable(h)) {
    js_value a = ev;
    js_set_event_script("abort");
    js_call(ctx, h, js_obj_value(signal), &a, 1);
    if (ctx->has_exc)
      report_exc(ctx);
  }
  ev_fire_on(signal, ev, 0);
  gc_pop_scope();
}

static js_value sig_throw_if_aborted(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ && js_to_boolean(js_get(ctx, t, "aborted")))
    js_throw(ctx, js_get(ctx, t, "reason"));
  return js_undef();
}

/* AbortSignal is not directly constructable; use AbortSignal.abort()/.timeout()
 * or an AbortController's `.signal`. */
static js_value abortsignal_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  (void)args;
  (void)argc;
  return js_throw_type(ctx, "Illegal constructor");
}

/* Static AbortSignal.abort(reason?) -> an already-aborted signal. */
static js_value sig_static_abort(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *s;
  js_value rv;
  (void)t;
  gc_push_scope();
  s = (js_object *)gc_keep(new_abort_signal(ctx));
  signal_abort(ctx, s, A(0));
  rv = js_obj_value(s);
  gc_pop_scope();
  return rv;
}

static js_value abortcontroller_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *self, *sig;
  js_value rv;
  (void)thisv;
  (void)args;
  (void)argc; /* native ctor builds its own object */
  gc_push_scope();
  self = (js_object *)gc_keep(js_new_object(ctx, g_abortcontroller_proto));
  sig = (js_object *)gc_keep(new_abort_signal(ctx));
  js_def(self, "signal", js_obj_value(sig), P_ENUMERABLE);
  rv = js_obj_value(self);
  gc_pop_scope();
  return rv;
}

static js_value ctrl_abort(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value sig;
  (void)argc;
  if (t.type != JV_OBJ)
    return js_undef();
  sig = js_get(ctx, t, "signal");
  if (sig.type == JV_OBJ)
    signal_abort(ctx, sig.u.obj, A(0));
  return js_undef();
}

/* ------------------------------------------------- RTCPeerConnection -------
 */
/* OwOS has no WebRTC/ICE stack. Sites `new` RTCPeerConnection to
 * feature-detect, probe for local IPs, or start a video call; a missing
 * constructor makes the
 * `||` vendor-prefix chain resolve to undefined and `new undefined()` aborts
 * the page with "value is not a constructor". This inert stub lets those
 * scripts run to completion — no ICE candidate is ever emitted, so nothing is
 * gathered. */

static js_value rtc_noop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_undef();
}

static js_value rtc_create_data_channel(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *dc;
  js_value rv;
  (void)t;
  gc_push_scope();
  dc = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(dc, "label", argc > 0 ? js_to_string(ctx, A(0)) : js_mk_cstring(ctx, ""), P_ENUMERABLE);
  js_def(dc, "readyState", js_mk_cstring(ctx, "connecting"), P_ENUMERABLE);
  js_def(dc, "onopen", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(dc, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(dc, "onclose", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def_native(ctx, dc, "send", rtc_noop, 1);
  js_def_native(ctx, dc, "close", rtc_noop, 0);
  js_def_native(ctx, dc, "addEventListener", rtc_noop, 3);
  js_def_native(ctx, dc, "removeEventListener", rtc_noop, 3);
  rv = js_obj_value(dc);
  gc_pop_scope();
  return rv;
}

/* createOffer/createAnswer: the legacy (successCb, failureCb) form stays inert
 * — never invoking the callback means no description is applied and no
 * candidates gather. The modern promise form resolves with an empty session
 * description the page can hand to setLocalDescription. */
static js_value rtc_offer_like(js_ctx *ctx, js_value *args, int argc, const char *type)
{
  js_object *o;
  js_value rv;
  if (js_is_callable(A(0)))
    return js_undef();
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(o, "type", js_mk_cstring(ctx, type), P_ENUMERABLE);
  js_def(o, "sdp", js_mk_cstring(ctx, ""), P_ENUMERABLE);
  rv = js_promise_resolved(ctx, js_obj_value(o));
  gc_pop_scope();
  return rv;
}
static js_value rtc_create_offer(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return rtc_offer_like(ctx, args, argc, "offer");
}
static js_value rtc_create_answer(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return rtc_offer_like(ctx, args, argc, "answer");
}

/* setLocalDescription/setRemoteDescription/addIceCandidate: inert for the
 * legacy (desc, successCb, failureCb) form; a resolved promise for the modern
 * form. */
static js_value rtc_set_desc(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  if (js_is_callable(A(1)))
    return js_undef();
  return js_promise_resolved(ctx, js_undef());
}

static js_value rtc_get_stats(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *o;
  js_value rv;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def_native(ctx, o, "forEach", rtc_noop, 1); /* an empty stats report */
  js_def_native(ctx, o, "values", rtc_noop, 0);
  rv = js_promise_resolved(ctx, js_obj_value(o));
  gc_pop_scope();
  return rv;
}

static js_value rtc_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *self;
  js_value rv;
  (void)thisv;
  (void)args;
  (void)argc; /* native ctor builds its own object */
  gc_push_scope();
  self = (js_object *)gc_keep(js_new_object(ctx, g_rtc_proto));
  js_def(self, "iceGatheringState", js_mk_cstring(ctx, "new"), P_ENUMERABLE);
  js_def(self, "iceConnectionState", js_mk_cstring(ctx, "new"), P_ENUMERABLE);
  js_def(self, "connectionState", js_mk_cstring(ctx, "new"), P_ENUMERABLE);
  js_def(self, "signalingState", js_mk_cstring(ctx, "stable"), P_ENUMERABLE);
  js_def(self, "localDescription", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "remoteDescription", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "onicecandidate", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "ondatachannel", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "oniceconnectionstatechange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "onconnectionstatechange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "onsignalingstatechange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "onnegotiationneeded", js_null(), P_WRITABLE | P_ENUMERABLE);
  rv = js_obj_value(self);
  gc_pop_scope();
  return rv;
}

/* ------------------------------------------------------- WebSocket -------- */
/* A WebSocket wrapper's `host` slot points at the ws_conn (NULL once closed or
 * if the connect failed). Every wrapper is pinned in g_websockets so it stays
 * GC-live and the pump can reach it. The library blocks only for the connect +
 * handshake (in the constructor, on the worker thread — same as fetch); after
 * that the socket is polled non-blocking by js_websockets_pump each idle tick,
 * which turns inbound frames into open/message/close/error events. */

static ws_conn *ws_of(js_value t) { return (t.type == JV_OBJ) ? (ws_conn *)t.u.obj->host : 0; }

/* Fire a non-bubbling `type` event on a target object: its on<type> handler
 * property and any addEventListener listeners. For "message", `data` becomes
 * event.data. Shared by WebSocket, XMLHttpRequest, AbortSignal. */
void emit_event(js_object *w, const char *type, js_value data)
{
  js_value ev;
  char on[24];
  gc_push_scope();
  gc_keep(w);
  ev = make_event(type, 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(g_ctx, ev, "target", js_obj_value(w));
    js_set(g_ctx, ev, "currentTarget", js_obj_value(w));
    if (data.type != JV_UNDEF)
      js_def(ev.u.obj, "data", data, P_WRITABLE | P_ENUMERABLE);
  }
  on[0] = 'o';
  on[1] = 'n';
  strlcpy(on + 2, type, (int)sizeof on - 2);
  {
    js_value h = js_get(g_ctx, js_obj_value(w), on);
    if (js_is_callable(h)) {
      js_value a = ev;
      js_set_event_script(type);
      js_call(g_ctx, h, js_obj_value(w), &a, 1);
      if (g_ctx->has_exc)
        report_exc(g_ctx);
    }
  }
  ev_fire_on(w, ev, 0); /* addEventListener listeners */
  gc_pop_scope();
}

static js_value ws_send_m(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  ws_conn *c = ws_of(t);
  js_value d = A(0);
  (void)argc;
  if (!c || ws_state(c) != WS_OPEN)
    return js_undef();
  if (d.type == JV_STR) {
    ws_send(c, d.u.str->data, d.u.str->len, 1);
  } else { /* no ArrayBuffer yet: coerce to text */
    js_value s = js_to_string(ctx, d);
    if (s.type == JV_STR)
      ws_send(c, s.u.str->data, s.u.str->len, 1);
  }
  return js_undef();
}

static js_value ws_close_m(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  ws_conn *c = ws_of(t);
  int code = (argc > 0 && args[0].type == JV_NUM) ? (int)args[0].u.num : 1000;
  const char *reason = (argc > 1) ? arg_str(ctx, A(1)) : "";
  if (c && ws_state(c) != WS_CLOSED) {
    ws_close(c, code, reason);
    js_set(ctx, t, "readyState", js_number(WS_CLOSING));
  }
  return js_undef();
}

static js_value ws_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  const char *url = arg_str(ctx, A(0));
  char protos[256];
  ws_conn *c;
  js_object *o;
  js_value ov;
  (void)thisv;
  protos[0] = 0;
  if (argc > 1) { /* a subprotocol string or array */
    js_value p = A(1);
    if (p.type == JV_STR) {
      strlcpy(protos, p.u.str->data, sizeof protos);
    } else if (p.type == JV_OBJ) {
      unsigned n = js_array_len(p.u.obj), i;
      int k = 0; /* page-controlled: bound each append */
      for (i = 0; i < n; i++) {
        const char *s = arg_str(ctx, js_get_index(ctx, p, i));
        if (!*s)
          continue;
        if (k)
          sapp(protos, &k, sizeof protos, ",");
        sapp(protos, &k, sizeof protos, s);
      }
    }
  }

  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_ws_proto));
  int ws_blocked = 0;
  if (B->pg->csp) { /* CSP connect-src (ws->http for matching) */
    char norm[URL_MAX], use[URL_MAX], reason[64];
    int k = 0;
    norm[0] = '\0';
    if (starts_ci(url, "wss://")) {
      sapp(norm, &k, sizeof norm, "https://");
      sapp(norm, &k, sizeof norm, url + 6);
    } else if (starts_ci(url, "ws://")) {
      sapp(norm, &k, sizeof norm, "http://");
      sapp(norm, &k, sizeof norm, url + 5);
    } else {
      sapp(norm, &k, sizeof norm, url);
    }
    if (!csp_check_url(B->pg->csp, CSP_CONNECT, norm, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
      js_csp_log("WebSocket to", url, reason);
      ws_blocked = 1;
    }
  }
  c = ws_blocked ? 0 : ws_open(url, protos[0] ? protos : 0); /* blocks for connect + handshake */
  o->host = c;
  js_def(o, "url", js_mk_cstring(ctx, url), P_ENUMERABLE);
  js_def(o, "readyState", js_number(c ? WS_OPEN : WS_CLOSED), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "bufferedAmount", js_number(0), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "protocol", js_mk_cstring(ctx, c ? ws_protocol(c) : ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "extensions", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "binaryType", js_mk_cstring(ctx, "arraybuffer"), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onopen", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onclose", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "@open_sent", js_bool(0), P_WRITABLE);    /* "open" delivered (pump) */
  js_def(o, "@dead", js_bool(c ? 0 : 1), P_WRITABLE); /* connect failed: emit error*/
  if (g_websockets)
    js_array_push(ctx, g_websockets, js_obj_value(o));
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}

/* Drive every live WebSocket: deliver the deferred "open", drain inbound
 * messages, and surface a close/error. Returns the number of events fired so
 * the worker can relayout/repaint when a handler mutated the DOM. */
int js_websockets_pump(void)
{
  int fired = 0;
  unsigned i, n;
  if (!g_page_live || !g_ctx || !g_websockets)
    return 0;
  n = js_array_len(g_websockets);
  for (i = 0; i < n; i++) {
    js_value wv = js_get_index(g_ctx, js_obj_value(g_websockets), i);
    js_object *w;
    ws_conn *c;
    if (wv.type != JV_OBJ)
      continue;
    w = wv.u.obj;
    c = (ws_conn *)w->host;

    if (!c) { /* failed connect, or already closed */
      if (js_to_boolean(js_get(g_ctx, wv, "@dead"))) {
        js_set(g_ctx, wv, "@dead", js_bool(0));
        js_set(g_ctx, wv, "readyState", js_number(WS_CLOSED));
        emit_event(w, "error", js_undef());
        emit_event(w, "close", js_undef());
        fired++;
      }
      continue;
    }

    if (!js_to_boolean(js_get(g_ctx, wv, "@open_sent"))) {
      js_set(g_ctx, wv, "@open_sent", js_bool(1));
      js_set(g_ctx, wv, "readyState", js_number(WS_OPEN));
      emit_event(w, "open", js_undef());
      fired++;
      c = (ws_conn *)w->host; /* a handler may have closed it */
      if (!c)
        continue;
    }

    for (;;) {
      const uint8_t *data = 0;
      int dlen = 0, is_text = 0, ev;
      ev = ws_poll(c, &data, &dlen, &is_text);
      if (ev == WS_EV_MESSAGE) {
        emit_event(w, "message", js_mk_string(g_ctx, (const char *)data, dlen));
        fired++;
        if (w->host != c)
          break; /* handler closed it mid-drain */
        continue;
      }
      if (ev == WS_EV_CLOSE || ev == WS_EV_ERROR) {
        if (ev == WS_EV_ERROR)
          emit_event(w, "error", js_undef());
        js_set(g_ctx, wv, "readyState", js_number(WS_CLOSED));
        emit_event(w, "close", js_undef());
        ws_free(c);
        w->host = 0;
        fired++;
      }
      break; /* WS_EV_NONE, or terminal */
    }
  }
  return fired;
}

/* ------------------------------------------------------- BroadcastChannel - *
 * Same-context BroadcastChannel: every open channel is pinned in g_bchannels;
 * postMessage delivers a MessageEvent to every OTHER open channel sharing the
 * name (never back to the sender, per spec). Cross-process delivery is out of
 * scope — one page's JS context is the whole "bus". Delivery is synchronous:
 * the engine has no event loop, matching how WebSocket/XHR events are fired.
 * A closed channel stays in the array (filtered by @closed at post time), the
 * same convention g_websockets uses for dead sockets. */

/* A MessageEvent over the shared Event.prototype (so
 * preventDefault/stopPropagation exist), carrying the standard message fields.
 * `origin` is the page's origin. */
js_value make_message_event(js_value data)
{
  js_object *e;
  js_value loc, org;
  const char *origin;
  loc = js_get(g_ctx, js_obj_value(g_ctx->global), "location");
  org = (loc.type == JV_OBJ) ? js_get(g_ctx, loc, "origin") : js_undef();
  origin = (org.type == JV_STR) ? org.u.str->data : "";
  gc_push_scope();
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  e = (js_object *)gc_keep(js_new_object(g_ctx, g_event_proto));
  js_def(e, "type", js_mk_cstring(g_ctx, "message"), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "data", data, P_WRITABLE | P_ENUMERABLE);
  js_def(e, "origin", js_mk_cstring(g_ctx, origin), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "lastEventId", js_mk_cstring(g_ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "source", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "bubbles", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "cancelable", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "defaultPrevented", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "target", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "currentTarget", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "@dp", js_bool(0), P_WRITABLE);
  js_def(e, "@sp", js_bool(0), P_WRITABLE);
  gc_pop_scope();
  return js_obj_value(e);
}

/* Deliver a fresh message event to one receiving channel. ev_fire_on runs both
 * its addEventListener("message") listeners and the onmessage property handler.
 */
static void bc_deliver(js_object *w, js_value data)
{
  js_value ev;
  gc_push_scope();
  gc_keep(w);
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  ev = make_message_event(data);
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(g_ctx, ev, "target", js_obj_value(w));
    js_set(g_ctx, ev, "currentTarget", js_obj_value(w));
    ev_fire_on(w, ev, 0);
  }
  gc_pop_scope();
}

/* postMessage(msg): fan out to every other open same-named channel. The payload
 * is passed by reference — the engine has no structuredClone for cross-channel
 * cloning here, so receivers share the sender's object (adequate for feature
 * detection and single-page use). */
static js_value bc_post_m(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value data = A(0), namev;
  const char *name;
  unsigned i, n;
  (void)argc;
  if (t.type != JV_OBJ || !g_bchannels)
    return js_undef();
  if (js_to_boolean(js_get(ctx, t, "@closed")))
    return js_undef(); /* no-op after close */
  namev = js_get(ctx, t, "name");
  name = namev.type == JV_STR ? namev.u.str->data : "";
  gc_push_scope();
  gc_keep(t.u.obj);
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  n = js_array_len(g_bchannels);
  for (i = 0; i < n; i++) {
    js_value cv = js_get_index(ctx, js_obj_value(g_bchannels), i);
    js_value wn;
    js_object *w;
    if (cv.type != JV_OBJ)
      continue;
    w = cv.u.obj;
    if (w == t.u.obj)
      continue; /* never self */
    if (js_to_boolean(js_get(ctx, cv, "@closed")))
      continue;
    wn = js_get(ctx, cv, "name");
    if (!(wn.type == JV_STR && streq(wn.u.str->data, name)))
      continue;
    bc_deliver(w, data);
  }
  gc_pop_scope();
  return js_undef();
}

static js_value bc_close_m(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ)
    js_set(ctx, t, "@closed", js_bool(1));
  return js_undef();
}

static js_value bchannel_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  const char *name = arg_str(ctx, A(0));
  js_object *o;
  js_value ov;
  (void)thisv;
  (void)argc; /* native ctor builds its own object */
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_bchannel_proto));
  js_def(o, "name", js_mk_cstring(ctx, name), P_ENUMERABLE); /* readonly */
  js_def(o, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onmessageerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "@closed", js_bool(0), P_WRITABLE);
  if (g_bchannels)
    js_array_push(ctx, g_bchannels, js_obj_value(o));
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}

/* --- navigator.geolocation ------------------------------------------------
 * Gated by PERM_GEOLOCATION. OwOS has no GPS, so a granted request still fails
 * the fix with POSITION_UNAVAILABLE; a denied one reports PERMISSION_DENIED —
 * both spec-correct outcomes routed to the error callback. */
static js_value geo_error_obj(js_ctx *ctx, int code, const char *msg)
{
  js_object *e = js_new_object(ctx, ctx->object_proto);
  js_def(e, "code", js_number(code), P_ENUMERABLE);
  js_def(e, "message", js_mk_cstring(ctx, msg), P_ENUMERABLE);
  js_def(e, "PERMISSION_DENIED", js_number(1), P_ENUMERABLE);
  js_def(e, "POSITION_UNAVAILABLE", js_number(2), P_ENUMERABLE);
  js_def(e, "TIMEOUT", js_number(3), P_ENUMERABLE);
  return js_obj_value(e);
}
static js_value geo_get_current(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value err_cb = A(1), e;
  (void)t;
  int st = worker_permission(PERM_GEOLOCATION);
  gc_push_scope();
  e = st == PERM_GRANTED ? geo_error_obj(ctx, 2, "Location information is unavailable.")
                         : geo_error_obj(ctx, 1, "User denied Geolocation");
  if (e.type == JV_OBJ)
    gc_keep(e.u.obj);
  if (js_is_callable(err_cb)) {
    js_value a = e;
    js_call(ctx, err_cb, js_undef(), &a, 1);
    if (ctx->has_exc)
      report_exc(ctx);
  }
  gc_pop_scope();
  return js_undef();
}
static js_value geo_watch(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  geo_get_current(ctx, t, args, argc); /* one fix attempt, same as a single call */
  return js_number(1);                 /* a watch id; clearWatch is a no-op       */
}

/* --- navigator.mediaDevices.getUserMedia ----------------------------------
 * Gated by PERM_CAMERA (video) + PERM_MICROPHONE (audio): a denied feature ->
 * NotAllowedError; all granted but no capture hardware -> NotFoundError. */
static js_value md_get_user_media(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value c = A(0);
  int want_audio = 0, want_video = 0;
  (void)t;
  if (c.type == JV_OBJ) {
    want_audio = js_to_boolean(js_get(ctx, c, "audio"));
    want_video = js_to_boolean(js_get(ctx, c, "video"));
  }
  if (!want_audio && !want_video)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError",
                                                  "At least one of audio and video must be requested"));
  if (want_video && worker_permission(PERM_CAMERA) != PERM_GRANTED)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "NotAllowedError", "Permission denied"));
  if (want_audio && worker_permission(PERM_MICROPHONE) != PERM_GRANTED)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "NotAllowedError", "Permission denied"));
  return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "NotFoundError", "Requested device not found"));
}
static js_value md_get_display_media(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "NotAllowedError", "Permission denied"));
}
static js_value md_enumerate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  return js_promise_resolved(ctx, js_obj_value(js_new_array(ctx, 0))); /* no devices */
}

/* --- navigator.permissions.query ------------------------------------------
 * Reports the remembered state, never prompts (per spec). Unknown feature names
 * resolve "prompt" (clipboard is granted — the browser backs it). */
static js_value perm_query(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value d = A(0), rv;
  const char *name = d.type == JV_OBJ ? arg_str(ctx, js_get(ctx, d, "name")) : "";
  const char *state = "prompt";
  int f = perm_feature_from_token(name);
  js_object *ps;
  (void)t;
  if (f >= 0) {
    int st = perm_state_now(f);
    state = st == PERM_GRANTED ? "granted" : st == PERM_DENIED ? "denied" : "prompt";
  } else if (streq_ci(name, "clipboard-read") || streq_ci(name, "clipboard-write")) {
    state = "granted";
  }
  gc_push_scope();
  ps = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(ps, "state", js_mk_cstring(ctx, state), P_ENUMERABLE);
  js_def(ps, "name", js_mk_cstring(ctx, name), P_ENUMERABLE);
  js_def(ps, "onchange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def_native(ctx, ps, "addEventListener", el_noop, 2);
  js_def_native(ctx, ps, "removeEventListener", el_noop, 2);
  rv = js_promise_resolved(ctx, js_obj_value(ps));
  gc_pop_scope();
  return rv;
}

/* ------------------------------------------------------- crypto ----------- *
 * crypto.getRandomValues / crypto.randomUUID over the shared userspace CSPRNG
 * (rng_bytes in the libc: RDRAND directly, no kernel trap). */

/* Fill an integer TypedArray's backing bytes in place; returns the same array.
 */
static js_value crypto_getrandomvalues(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value v = A(0);
  js_typedarray *ta;
  js_arraybuffer *ab;
  int nbytes;
  (void)t;
  if (v.type != JV_OBJ || v.u.obj->kind != OBJ_TYPEDARRAY)
    return js_throw_type(ctx, "Failed to execute 'getRandomValues': argument is not an "
                              "integer-typed TypedArray");
  ta = (js_typedarray *)v.u.obj->host;
  if (!ta || ta->type < 0 || ta->type == TA_F32 || ta->type == TA_F64)
    return js_make_error(ctx, ctx->type_error_proto, "TypeMismatchError",
                         "getRandomValues requires an integer-typed TypedArray");
  nbytes = ta->length * ta_elem_size(ta->type);
  if (nbytes > 65536)
    return js_make_error(ctx, ctx->error_proto, "QuotaExceededError",
                         "getRandomValues can only generate a maximum of 65536 bytes");
  ab = (js_arraybuffer *)ta->buffer->host;
  if (ab && nbytes > 0)
    rng_bytes(ab->data + ta->offset, nbytes);
  return v;
}
static js_value crypto_randomuuid(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  unsigned char b[16];
  char s[37];
  static const char hx[] = "0123456789abcdef";
  int i, p = 0;
  (void)t;
  (void)args;
  (void)argc;
  rng_bytes(b, 16);
  b[6] = (unsigned char)((b[6] & 0x0f) | 0x40); /* version 4 */
  b[8] = (unsigned char)((b[8] & 0x3f) | 0x80); /* variant 1 */
  for (i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      s[p++] = '-';
    s[p++] = hx[b[i] >> 4];
    s[p++] = hx[b[i] & 15];
  }
  s[p] = '\0';
  return js_mk_cstring(ctx, s);
}

/* ------------------------------------------------------- XMLHttpRequest ---- *
 * The classic XHR transport, over the same synchronous browser_cors_fetch path
 * as fetch() (the engine has no event loop). open() records the verb/URL; each
 * setRequestHeader() appends to a header blob; send() runs the request to
 * completion, then walks readyState 1->2->3->4 firing readystatechange + the
 * load/error/loadend events. So even "async" XHR settles within send(), which
 * suits a page's settle-immediately expectations here. */

static js_value xhr_open(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char method[16], url[URL_MAX];
  if (t.type != JV_OBJ)
    return js_undef();
  strlcpy(method, arg_str(ctx, A(0)), sizeof method);
  resolve_url(g_page_url_buf, arg_str(ctx, A(1)), url, sizeof url);
  js_def(t.u.obj, "@method", js_mk_cstring(ctx, method[0] ? method : "GET"), 0);
  js_def(t.u.obj, "@url", js_mk_cstring(ctx, url), 0);
  js_def(t.u.obj, "@hdrs", js_mk_cstring(ctx, ""), 0);
  js_def(t.u.obj, "@ctype", js_mk_cstring(ctx, ""), 0);
  js_set(ctx, t, "readyState", js_number(1)); /* OPENED */
  emit_event(t.u.obj, "readystatechange", js_undef());
  return js_undef();
}
static js_value xhr_setheader(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *name = arg_str(ctx, A(0));
  char val[1024];
  if (t.type != JV_OBJ)
    return js_undef();
  strlcpy(val, arg_str(ctx, A(1)), sizeof val);
  if (streq_ci(name, "content-type")) {
    js_def(t.u.obj, "@ctype", js_mk_cstring(ctx, val), 0);
    return js_undef();
  }
  { /* append "Name: value\r\n" to the accumulated request-header blob */
    js_value cur = js_get(ctx, t, "@hdrs");
    sbuf sb;
    sb.b = 0;
    sb.n = 0;
    sb.cap = 0;
    if (cur.type == JV_STR)
      sb_putn(&sb, cur.u.str->data, cur.u.str->len);
    sb_puts(&sb, name);
    sb_puts(&sb, ": ");
    sb_puts(&sb, val);
    sb_puts(&sb, "\r\n");
    js_def(t.u.obj, "@hdrs", js_mk_string(ctx, sb.b ? sb.b : "", sb.n), 0);
    if (sb.b)
      free(sb.b);
  }
  return js_undef();
}
static js_value xhr_setrt(js_ctx *ctx, js_value t, int state)
{
  js_set(ctx, t, "readyState", js_number(state));
  emit_event(t.u.obj, "readystatechange", js_undef());
  return js_undef();
}
static js_value xhr_send(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char url[URL_MAX], method[16], req_ctype[160], resp_ctype[160];
  char *reqbody = 0;
  int reqlen = 0, status = 0, outlen = 0;
  unsigned char *body;
  const char *hdrs;
  if (t.type != JV_OBJ)
    return js_undef();
  strlcpy(url, arg_str(ctx, js_get(ctx, t, "@url")), sizeof url);
  strlcpy(method, arg_str(ctx, js_get(ctx, t, "@method")), sizeof method);
  strlcpy(req_ctype, arg_str(ctx, js_get(ctx, t, "@ctype")), sizeof req_ctype);
  if (B->pg->csp) { /* CSP connect-src */
    char use[URL_MAX], reason[64];
    if (!csp_check_url(B->pg->csp, CSP_CONNECT, url, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
      js_csp_log("connection to", url, reason);
      js_set(ctx, t, "status", js_number(0));
      xhr_setrt(ctx, t, 4);
      emit_event(t.u.obj, "error", js_undef());
      emit_event(t.u.obj, "loadend", js_undef());
      return js_undef();
    }
    strlcpy(url, use, sizeof url);
  }
  if (argc >= 1 && A(0).type == JV_STR) {
    reqbody = A(0).u.str->data;
    reqlen = A(0).u.str->len;
    if (!req_ctype[0])
      strlcpy(req_ctype, "text/plain;charset=UTF-8", sizeof req_ctype);
  }
  {
    js_value h = js_get(ctx, t, "@hdrs");
    hdrs = h.type == JV_STR ? h.u.str->data : 0;
  }
  {
    int with_creds = js_to_boolean(js_get(ctx, t, "withCredentials"));
    struct http_req rq = {0};
    struct http_resp rs = {0};
    rq.url = url;
    rq.method = method[0] ? method : "GET";
    rq.body = (const unsigned char *)reqbody;
    rq.body_len = reqlen;
    rq.ctype = req_ctype[0] ? req_ctype : 0;
    rq.extra_hdrs = hdrs && hdrs[0] ? hdrs : 0;
    body = browser_cors_fetch(g_page_url_buf, with_creds, &rq, &rs);
    status = rs.status;
    outlen = rs.len;
    strlcpy(resp_ctype, rs.ctype, sizeof resp_ctype);
  }
  gc_push_scope();
  gc_keep(t.u.obj);
  js_def(t.u.obj, "@respctype", js_mk_cstring(ctx, resp_ctype), 0);
  js_set(ctx, t, "responseURL", js_mk_cstring(ctx, url));
  if (!body) { /* network error */
    js_set(ctx, t, "status", js_number(0));
    xhr_setrt(ctx, t, 4);
    emit_event(t.u.obj, "error", js_undef());
    emit_event(t.u.obj, "loadend", js_undef());
    gc_pop_scope();
    return js_undef();
  }
  {
    js_value text = js_mk_string(ctx, (const char *)body, outlen);
    gc_keep(text.u.str);
    free(body);
    js_set(ctx, t, "status", js_number(status ? status : 200));
    js_set(ctx, t, "statusText", js_mk_cstring(ctx, status == 200 ? "OK" : status == 204 ? "No Content" : ""));
    js_set(ctx, t, "responseText", text);
    { /* .response honours responseType: "json" parses, else the text */
      js_value rt = js_get(ctx, t, "responseType");
      if (rt.type == JV_STR && streq(rt.u.str->data, "json")) {
        js_value jsonobj = js_get(ctx, js_obj_value(ctx->global), "JSON");
        js_value parsefn = js_get(ctx, jsonobj, "parse");
        js_value parsed = js_is_callable(parsefn) ? js_call(ctx, parsefn, jsonobj, &text, 1) : js_null();
        if (ctx->has_exc) {
          ctx->has_exc = 0;
          ctx->exc_root = 0;
          parsed = js_null();
        }
        js_set(ctx, t, "response", parsed);
      } else {
        js_set(ctx, t, "response", text);
      }
    }
    xhr_setrt(ctx, t, 2); /* HEADERS_RECEIVED */
    xhr_setrt(ctx, t, 3); /* LOADING */
    xhr_setrt(ctx, t, 4); /* DONE */
    emit_event(t.u.obj, "load", js_undef());
    emit_event(t.u.obj, "loadend", js_undef());
  }
  gc_pop_scope();
  return js_undef();
}
static js_value xhr_getheader(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *name = arg_str(ctx, A(0));
  if (t.type == JV_OBJ && streq_ci(name, "content-type")) {
    js_value c = js_get(ctx, t, "@respctype");
    if (c.type == JV_STR && c.u.str->len)
      return c;
  }
  return js_null();
}
static js_value xhr_getallheaders(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value c = t.type == JV_OBJ ? js_get(ctx, t, "@respctype") : js_undef();
  (void)args;
  (void)argc;
  if (c.type == JV_STR && c.u.str->len) {
    sbuf sb;
    js_value r;
    sb.b = 0;
    sb.n = 0;
    sb.cap = 0;
    sb_puts(&sb, "content-type: ");
    sb_putn(&sb, c.u.str->data, c.u.str->len);
    sb_puts(&sb, "\r\n");
    r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
    if (sb.b)
      free(sb.b);
    return r;
  }
  return js_mk_cstring(ctx, "");
}
static js_value xhr_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o;
  js_value ov;
  (void)thisv;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_xhr_proto));
  js_def(o, "readyState", js_number(0), P_WRITABLE | P_ENUMERABLE); /* UNSENT */
  js_def(o, "status", js_number(0), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "statusText", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "responseText", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "response", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "responseType", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "responseURL", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "timeout", js_number(0), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "withCredentials", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onreadystatechange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onload", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onloadend", js_null(), P_WRITABLE | P_ENUMERABLE);
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}

/* ------------------------------------------------------- Blob + sendBeacon */

/* Append the raw bytes of one Blob "part" (string, ArrayBuffer, TypedArray,
 * Blob, else its String() form) to `sb`. */
static void blob_append_part(js_ctx *ctx, sbuf *sb, js_value v)
{
  if (v.type == JV_STR) {
    sb_putn(sb, v.u.str->data, v.u.str->len);
    return;
  }
  if (v.type == JV_OBJ) {
    js_object *o = v.u.obj;
    if (o->proto == g_blob_proto) {
      js_value b = js_get(ctx, v, "@bytes");
      if (b.type == JV_STR)
        sb_putn(sb, b.u.str->data, b.u.str->len);
      return;
    }
    if (o->kind == OBJ_ARRAYBUFFER && o->host) {
      js_arraybuffer *ab = (js_arraybuffer *)o->host;
      sb_putn(sb, (const char *)ab->data, ab->byteLength);
      return;
    }
    if (o->kind == OBJ_TYPEDARRAY && o->host) {
      js_typedarray *ta = (js_typedarray *)o->host;
      js_arraybuffer *ab = (js_arraybuffer *)ta->buffer->host;
      sb_putn(sb, (const char *)ab->data + ta->offset, ta->length * ta_elem_size(ta->type < 0 ? TA_U8 : ta->type));
      return;
    }
  }
  {
    js_value s = js_to_string(ctx, v);
    if (s.type == JV_STR)
      sb_putn(sb, s.u.str->data, s.u.str->len);
  }
}

/* Make a Blob object holding `data[0..len)` with content type `type`. */
static js_value make_blob(js_ctx *ctx, const char *data, int len, const char *type)
{
  js_object *o;
  js_value ov;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_blob_proto));
  js_def(o, "@bytes", js_mk_string(ctx, data ? data : "", len), 0);
  js_def(o, "size", js_number(len), P_ENUMERABLE);
  js_def(o, "type", js_mk_cstring(ctx, type ? type : ""), P_ENUMERABLE);
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}
static js_value blob_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  sbuf sb;
  js_value parts = A(0), rv;
  char type[128];
  (void)thisv;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  type[0] = '\0';
  gc_push_scope();
  if (parts.type == JV_OBJ) { /* an array (or array-like) of parts */
    unsigned i,
        n = parts.u.obj->kind == OBJ_ARRAY ? parts.u.obj->elen : js_to_uint32(ctx, js_get(ctx, parts, "length"));
    for (i = 0; i < n; i++)
      blob_append_part(ctx, &sb, js_get_index(ctx, parts, i));
  }
  if (argc >= 2 && A(1).type == JV_OBJ) {
    js_value tp = js_get(ctx, A(1), "type");
    if (tp.type == JV_STR)
      strlcpy(type, tp.u.str->data, sizeof type);
  }
  rv = make_blob(ctx, sb.b ? sb.b : "", sb.n, type);
  if (sb.b)
    free(sb.b);
  gc_pop_scope();
  return rv;
}
static js_value blob_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return js_promise_resolved(ctx, js_get(ctx, t, "@bytes"));
}
static js_value blob_arraybuffer(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value b = js_get(ctx, t, "@bytes"), u8ctor, lenarg, view, buf;
  int i, len;
  (void)args;
  (void)argc;
  if (b.type != JV_STR)
    return js_promise_resolved(ctx, js_undef());
  len = b.u.str->len;
  gc_push_scope();
  gc_keep(b.u.str);
  u8ctor = js_get(ctx, js_obj_value(ctx->global), "Uint8Array");
  lenarg = js_number(len);
  view = js_construct(ctx, u8ctor, &lenarg, 1); /* a fresh Uint8Array(len) */
  if (view.type == JV_OBJ) {
    gc_keep(view.u.obj);
    for (i = 0; i < len; i++)
      js_set_index(ctx, view, (unsigned)i, js_number((unsigned char)b.u.str->data[i]));
  }
  buf = js_get(ctx, view, "buffer");
  gc_pop_scope();
  return js_promise_resolved(ctx, buf);
}
static js_value blob_slice(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value b = js_get(ctx, t, "@bytes");
  int len = b.type == JV_STR ? b.u.str->len : 0;
  int s = argc > 0 && A(0).type != JV_UNDEF ? (int)js_to_number(ctx, A(0)) : 0;
  int e = argc > 1 && A(1).type != JV_UNDEF ? (int)js_to_number(ctx, A(1)) : len;
  char type[128];
  type[0] = '\0';
  if (s < 0)
    s += len;
  if (s < 0)
    s = 0;
  else if (s > len)
    s = len;
  if (e < 0)
    e += len;
  if (e < 0)
    e = 0;
  else if (e > len)
    e = len;
  if (e < s)
    e = s;
  if (argc > 2 && A(2).type == JV_STR)
    strlcpy(type, A(2).u.str->data, sizeof type);
  return make_blob(ctx, b.u.str->data + s, e - s, type);
}

/* Extract the request bytes + a default content-type from a body value (string,
 * Blob, ArrayBuffer/TypedArray, URLSearchParams). `ctype` is left untouched
 * when the body implies none. Bytes point into a GC object the caller must
 * keep. */
static const char *body_bytes(js_ctx *ctx, js_value v, int *len, char *ctype, int ctcap)
{
  if (v.type == JV_STR) {
    *len = v.u.str->len;
    if (ctype && !ctype[0])
      strlcpy(ctype, "text/plain;charset=UTF-8", ctcap);
    return v.u.str->data;
  }
  if (v.type == JV_OBJ) {
    js_object *o = v.u.obj;
    if (o->proto == g_blob_proto) {
      js_value b = js_get(ctx, v, "@bytes"), tp = js_get(ctx, v, "type");
      if (ctype && !ctype[0] && tp.type == JV_STR && tp.u.str->len)
        strlcpy(ctype, tp.u.str->data, ctcap);
      *len = b.type == JV_STR ? b.u.str->len : 0;
      return b.type == JV_STR ? b.u.str->data : "";
    }
    if (o->kind == OBJ_ARRAYBUFFER && o->host) {
      js_arraybuffer *ab = (js_arraybuffer *)o->host;
      *len = ab->byteLength;
      return (const char *)ab->data;
    }
    if (o->kind == OBJ_TYPEDARRAY && o->host) {
      js_typedarray *ta = (js_typedarray *)o->host;
      js_arraybuffer *ab = (js_arraybuffer *)ta->buffer->host;
      *len = ta->length * ta_elem_size(ta->type < 0 ? TA_U8 : ta->type);
      return (const char *)ab->data + ta->offset;
    }
    if (o->proto == g_sp_proto) {
      js_value s = sp_tostring(ctx, v, 0, 0);
      if (ctype && !ctype[0])
        strlcpy(ctype, "application/x-www-form-urlencoded;charset=UTF-8", ctcap);
      *len = s.type == JV_STR ? s.u.str->len : 0;
      return s.type == JV_STR ? s.u.str->data : "";
    }
    /* FormData: no file input here, so serialize as urlencoded rather than
     * multipart. */
    if (o->proto == g_formdata_proto) {
      js_value s = sp_tostring(ctx, v, 0, 0);
      if (ctype && !ctype[0])
        strlcpy(ctype, "application/x-www-form-urlencoded;charset=UTF-8", ctcap);
      *len = s.type == JV_STR ? s.u.str->len : 0;
      return s.type == JV_STR ? s.u.str->data : "";
    }
  }
  *len = 0;
  return "";
}

/* navigator.sendBeacon(url, data) -> boolean. A fire-and-forget POST (the
 * response is discarded). Runs synchronously on the worker like fetch/XHR. */
static js_value nav_sendbeacon(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char url[URL_MAX], ctype[160];
  const char *body;
  int len = 0;
  unsigned char *resp;
  js_value data = A(1);
  (void)t;
  if (argc < 1)
    return js_bool(0);
  resolve_url(g_page_url_buf, arg_str(ctx, A(0)), url, sizeof url);
  if (B->pg->csp) {
    char use[URL_MAX], reason[64];
    if (!csp_check_url(B->pg->csp, CSP_CONNECT, url, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
      js_csp_log("beacon to", url, reason);
      return js_bool(0);
    }
    strlcpy(url, use, sizeof url);
  }
  ctype[0] = '\0';
  gc_push_scope();
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  else if (data.type == JV_STR)
    gc_keep(data.u.str);
  body = (argc >= 2 && data.type != JV_UNDEF && data.type != JV_NULL) ? body_bytes(ctx, data, &len, ctype, sizeof ctype)
                                                                      : "";
  { /* no-cors fire-and-forget: send and discard, no CORS gating on the reply */
    struct http_req rq = {0};
    struct http_resp rs = {0};
    rq.url = url;
    rq.method = "POST";
    rq.body = (const unsigned char *)body;
    rq.body_len = len;
    rq.ctype = ctype[0] ? ctype : 0;
    resp = browser_fetch(&rq, &rs);
  }
  gc_pop_scope();
  if (resp)
    free(resp);
  return js_bool(resp != 0); /* queued (sent) */
}

/* ------------------------------------------------- Async Clipboard API */

/* clip_get tags every clip with a MIME type; readText() only surfaces text. */
static int clip_mime_is_text(const char *m)
{
  return m[0] == 't' && m[1] == 'e' && m[2] == 'x' && m[3] == 't' && m[4] == '/';
}

/* Resolve a ClipboardItem entry (a Blob, a string, or a settled Promise of
 * either) to its raw bytes. Returns the backing js_string (caller keeps it via
 * the GC scope) and copies the entry's MIME into `type` when it carries one. */
static js_string *clip_entry_bytes(js_ctx *ctx, js_value v, char *type, int tcap)
{
  js_value settled;
  if (v.type == JV_OBJ && js_promise_inspect(ctx, v, &settled) == 1)
    v = settled;
  if (v.type == JV_OBJ && v.u.obj->proto == g_blob_proto) {
    js_value b = js_get(ctx, v, "@bytes"), tp = js_get(ctx, v, "type");
    if (type && tp.type == JV_STR && tp.u.str->len)
      strlcpy(type, tp.u.str->data, tcap);
    return b.type == JV_STR ? b.u.str : 0;
  }
  {
    js_value s = js_to_string(ctx, v);
    return s.type == JV_STR ? s.u.str : 0;
  }
}

/* new ClipboardItem({ "text/plain": blobOrString, ... }) — keeps the dictionary
 * verbatim (@data) so getType() can hand back each representation lazily. */
static js_value clipitem_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o, *types, *dict;
  js_value items = A(0), ov;
  int i;
  (void)thisv;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_clipitem_proto));
  types = (js_object *)gc_keep(js_new_array(ctx, 0));
  dict = items.type == JV_OBJ ? items.u.obj : js_new_object(ctx, ctx->object_proto);
  gc_keep(dict);
  js_def(o, "@data", js_obj_value(dict), 0);
  for (i = 0; i < dict->props.n; i++) {
    pentry *pe = &dict->props.e[i];
    if (!(pe->flags & P_ENUMERABLE) || (unsigned char)pe->key[0] < ' ')
      continue; /* skip symbol keys */
    js_array_push(ctx, types, js_mk_cstring(ctx, pe->key));
  }
  js_def(o, "types", js_obj_value(types), P_ENUMERABLE);
  js_def(o, "presentationStyle", js_mk_cstring(ctx, "unspecified"), P_ENUMERABLE);
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}

/* item.getType(mime) -> Promise<Blob> of that representation. */
static js_value clipitem_gettype(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value data = js_get(ctx, t, "@data"), e, settled;
  const char *m = arg_str(ctx, A(0));
  if (data.type != JV_OBJ)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "NotFoundError", "The type was not found"));
  e = js_get(ctx, data, m);
  if (e.type == JV_OBJ && js_promise_inspect(ctx, e, &settled) == 1)
    e = settled;
  if (e.type == JV_UNDEF || e.type == JV_NULL)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "NotFoundError", "The type was not found"));
  if (e.type == JV_OBJ && e.u.obj->proto == g_blob_proto)
    return js_promise_resolved(ctx, e);
  {
    js_value s = js_to_string(ctx, e);
    return js_promise_resolved(
        ctx, make_blob(ctx, s.type == JV_STR ? s.u.str->data : "", s.type == JV_STR ? s.u.str->len : 0, m));
  }
}

/* ClipboardItem.supports(type) — the formats the clipboard server round-trips.
 */
static js_value clipitem_supports(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *m = arg_str(ctx, A(0));
  (void)t;
  return js_bool(streq(m, "text/plain") || streq(m, "text/html") || streq(m, "image/png"));
}

/* navigator.clipboard.writeText(text) -> Promise<undefined>. */
static js_value clip_writetext(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value s = js_to_string(ctx, A(0));
  (void)t;
  if (s.type != JV_STR)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "writeText: invalid text"));
  if (clip_put_text(s.u.str->data, s.u.str->len) != 0)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "NotAllowedError", "Clipboard write failed"));
  return js_promise_resolved(ctx, js_undef());
}

/* navigator.clipboard.readText() -> Promise<string> of the newest text clip
 * (empty string when the clipboard holds no text, mirroring real browsers). */
static js_value clip_readtext(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char mime[64];
  void *data = 0;
  int len = 0;
  js_value rv;
  (void)t;
  (void)args;
  (void)argc;
  if (clip_get(0, mime, sizeof mime, &data, &len) != 0)
    return js_promise_resolved(ctx, js_mk_cstring(ctx, ""));
  rv = clip_mime_is_text(mime) ? js_mk_string(ctx, data ? (const char *)data : "", len) : js_mk_cstring(ctx, "");
  if (data)
    free(data);
  return js_promise_resolved(ctx, rv);
}

/* navigator.clipboard.write([ClipboardItem,...]) -> Promise<undefined>. The
 * kernel clipboard stores one blob per clip, so we write a single
 * representation per item — text/plain when offered (best cross-app paste),
 * else the item's first format. */
static js_value clip_write(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value items = A(0), item, data, entry;
  js_string *bytes;
  char mime[64];
  int rc, i;
  (void)t;
  if (items.type != JV_OBJ)
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "write: expected ClipboardItems"));
  item = items.u.obj->kind == OBJ_ARRAY ? js_get_index(ctx, items, 0) : items;
  if (item.type != JV_OBJ)
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "write: empty ClipboardItem list"));
  data = js_get(ctx, item, "@data");
  if (data.type != JV_OBJ)
    data = item; /* tolerate a bare dict */
  entry = js_get(ctx, data, "text/plain");
  if (entry.type != JV_UNDEF && entry.type != JV_NULL) {
    strlcpy(mime, "text/plain", sizeof mime);
  } else {
    js_object *d = data.u.obj;
    mime[0] = '\0';
    for (i = 0; i < d->props.n; i++) {
      pentry *pe = &d->props.e[i];
      if (!(pe->flags & P_ENUMERABLE) || (unsigned char)pe->key[0] < ' ')
        continue;
      if (streq(pe->key, "@data"))
        continue;
      strlcpy(mime, pe->key, sizeof mime);
      entry = js_get(ctx, data, pe->key);
      break;
    }
    if (!mime[0])
      return js_promise_resolved(ctx, js_undef()); /* nothing to write */
  }
  gc_push_scope();
  bytes = clip_entry_bytes(ctx, entry, 0, 0);
  if (bytes)
    gc_keep(bytes);
  rc = bytes ? clip_put(mime, bytes->data, bytes->len) : clip_put(mime, "", 0);
  gc_pop_scope();
  if (rc != 0)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "NotAllowedError", "Clipboard write failed"));
  return js_promise_resolved(ctx, js_undef());
}

/* navigator.clipboard.read() -> Promise<ClipboardItem[]> of the newest clip. */
static js_value clip_read(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char mime[64];
  void *data = 0;
  int len = 0;
  js_object *arr;
  js_value rv;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (clip_get(0, mime, sizeof mime, &data, &len) == 0) {
    js_object *dict = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_value dictarg, item;
    js_def(dict, mime, make_blob(ctx, data ? (const char *)data : "", len, mime),
           P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    dictarg = js_obj_value(dict);
    item = clipitem_ctor(ctx, js_undef(), &dictarg, 1);
    if (item.type == JV_OBJ)
      js_array_push(ctx, arr, item);
  }
  if (data)
    free(data);
  rv = js_obj_value(arr);
  gc_pop_scope();
  return js_promise_resolved(ctx, rv);
}

/* The DOM `Node` interface: not constructable, but exposed for the `Node.*`
 * nodeType constants and `x instanceof Node` (feature-detected by many libs).
 */
static js_value node_ctor(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_undef();
}
static void def_node_consts(js_object *o)
{
  js_def(o, "ELEMENT_NODE", js_number(1), 0);
  js_def(o, "ATTRIBUTE_NODE", js_number(2), 0);
  js_def(o, "TEXT_NODE", js_number(3), 0);
  js_def(o, "CDATA_SECTION_NODE", js_number(4), 0);
  js_def(o, "ENTITY_REFERENCE_NODE", js_number(5), 0);
  js_def(o, "ENTITY_NODE", js_number(6), 0);
  js_def(o, "PROCESSING_INSTRUCTION_NODE", js_number(7), 0);
  js_def(o, "COMMENT_NODE", js_number(8), 0);
  js_def(o, "DOCUMENT_NODE", js_number(9), 0);
  js_def(o, "DOCUMENT_TYPE_NODE", js_number(10), 0);
  js_def(o, "DOCUMENT_FRAGMENT_NODE", js_number(11), 0);
  js_def(o, "NOTATION_NODE", js_number(12), 0);
}

/* --- CSSStyleDeclaration: generic property access backed by the element's
 * style="" attribute. One getter/setter template, bound via fn->data to each
 * property's CSS name, so `prop in style`, `style[prop]`, `style[prop]=v` and
 * `style.length` behave like a real CSSStyleDeclaration for the standard set
 * (feature-detection libs like css3test's Supports rely on exactly this). */
static js_value style_gen_get(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value d = ctx->native_data;
  (void)a;
  (void)n;
  return style_read(ctx, node_of(t), d.type == JV_STR ? d.u.str->data : "");
}
static js_value style_gen_set(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value d = ctx->native_data;
  (void)argc;
  style_write(node_of(t), d.type == JV_STR ? d.u.str->data : "", arg_str(ctx, A(0)));
  return js_undef();
}
static js_value style_gen_length(js_ctx *ctx, js_value t, js_value *a, int n)
{
  struct dom_node *nd = node_of(t);
  const char *p = nd ? dom_get_attr(nd, "style") : 0;
  int count = 0;
  (void)ctx;
  (void)a;
  (void)n;
  while (p && *p) {
    const char *seg = p, *colon;
    while (*p && *p != ';')
      p++;
    colon = seg;
    while (colon < p && *colon != ':')
      colon++;
    if (colon < p) { /* count only "name : value" pairs */
      const char *ns = seg, *ne = colon, *vs = colon + 1, *ve = p;
      while (ns < ne && is_sp(*ns))
        ns++;
      while (ne > ns && is_sp(ne[-1]))
        ne--;
      while (vs < ve && is_sp(*vs))
        vs++;
      while (ve > vs && is_sp(ve[-1]))
        ve--;
      if (ns < ne && vs < ve)
        count++;
    }
    if (*p == ';')
      p++;
  }
  return js_number(count);
}
/* camelCase JS property name -> CSS dash-case (cssFloat -> float). */
static void style_js_to_css(const char *js, char *out, int cap)
{
  int o = 0;
  if (streq(js, "cssFloat")) {
    const char *f = "float";
    while (*f && o < cap - 1)
      out[o++] = *f++;
    out[o] = 0;
    return;
  }
  for (int i = 0; js[i] && o < cap - 2; i++) {
    char c = js[i];
    if (c >= 'A' && c <= 'Z') {
      out[o++] = '-';
      out[o++] = (char)(c + 32);
    } else
      out[o++] = c;
  }
  out[o] = 0;
}
static void def_style_prop(const char *jsName)
{
  char css[64];
  style_js_to_css(jsName, css, sizeof css);
  gc_push_scope();
  {
    js_object *g = (js_object *)gc_keep(js_new_native(g_ctx, style_gen_get, jsName, 0));
    js_object *s = (js_object *)gc_keep(js_new_native(g_ctx, style_gen_set, jsName, 1));
    g->fn->data = js_mk_cstring(g_ctx, css);
    s->fn->data = js_mk_cstring(g_ctx, css);
    js_def_accessor(g_style_proto, jsName, g, s, P_CONFIGURABLE | P_ENUMERABLE);
  }
  gc_pop_scope();
}
/* The standard CSS property set exposed on CSSStyleDeclaration (camelCase). Not
 * exhaustive, but broad enough that `prop in style` answers correctly for the
 * common properties; the ~16 with layout-specific accessors are refined below.
 */
static const char *const k_css_props[] = {
    "alignContent",
    "alignItems",
    "alignSelf",
    "all",
    "animation",
    "animationDelay",
    "animationDirection",
    "animationDuration",
    "animationFillMode",
    "animationIterationCount",
    "animationName",
    "animationPlayState",
    "animationTimingFunction",
    "appearance",
    "aspectRatio",
    "backdropFilter",
    "backfaceVisibility",
    "background",
    "backgroundAttachment",
    "backgroundBlendMode",
    "backgroundClip",
    "backgroundColor",
    "backgroundImage",
    "backgroundOrigin",
    "backgroundPosition",
    "backgroundRepeat",
    "backgroundSize",
    "blockSize",
    "border",
    "borderBottom",
    "borderBottomColor",
    "borderBottomLeftRadius",
    "borderBottomRightRadius",
    "borderBottomStyle",
    "borderBottomWidth",
    "borderCollapse",
    "borderColor",
    "borderImage",
    "borderImageOutset",
    "borderImageRepeat",
    "borderImageSlice",
    "borderImageSource",
    "borderImageWidth",
    "borderLeft",
    "borderLeftColor",
    "borderLeftStyle",
    "borderLeftWidth",
    "borderRadius",
    "borderRight",
    "borderRightColor",
    "borderRightStyle",
    "borderRightWidth",
    "borderSpacing",
    "borderStyle",
    "borderTop",
    "borderTopColor",
    "borderTopLeftRadius",
    "borderTopRightRadius",
    "borderTopStyle",
    "borderTopWidth",
    "borderWidth",
    "bottom",
    "boxDecorationBreak",
    "boxShadow",
    "boxSizing",
    "breakAfter",
    "breakBefore",
    "breakInside",
    "captionSide",
    "caretColor",
    "clear",
    "clip",
    "clipPath",
    "color",
    "columnCount",
    "columnFill",
    "columnGap",
    "columnRule",
    "columnRuleColor",
    "columnRuleStyle",
    "columnRuleWidth",
    "columnSpan",
    "columnWidth",
    "columns",
    "content",
    "counterIncrement",
    "counterReset",
    "cursor",
    "direction",
    "display",
    "emptyCells",
    "filter",
    "flex",
    "flexBasis",
    "flexDirection",
    "flexFlow",
    "flexGrow",
    "flexShrink",
    "flexWrap",
    "cssFloat",
    "float",
    "font",
    "fontFamily",
    "fontFeatureSettings",
    "fontKerning",
    "fontSize",
    "fontSizeAdjust",
    "fontStretch",
    "fontStyle",
    "fontVariant",
    "fontWeight",
    "gap",
    "grid",
    "gridArea",
    "gridAutoColumns",
    "gridAutoFlow",
    "gridAutoRows",
    "gridColumn",
    "gridColumnEnd",
    "gridColumnGap",
    "gridColumnStart",
    "gridGap",
    "gridRow",
    "gridRowEnd",
    "gridRowGap",
    "gridRowStart",
    "gridTemplate",
    "gridTemplateAreas",
    "gridTemplateColumns",
    "gridTemplateRows",
    "height",
    "hyphens",
    "inlineSize",
    "inset",
    "justifyContent",
    "justifyItems",
    "justifySelf",
    "left",
    "letterSpacing",
    "lineHeight",
    "listStyle",
    "listStyleImage",
    "listStylePosition",
    "listStyleType",
    "margin",
    "marginBottom",
    "marginLeft",
    "marginRight",
    "marginTop",
    "mask",
    "maxHeight",
    "maxWidth",
    "minHeight",
    "minWidth",
    "mixBlendMode",
    "objectFit",
    "objectPosition",
    "opacity",
    "order",
    "outline",
    "outlineColor",
    "outlineOffset",
    "outlineStyle",
    "outlineWidth",
    "overflow",
    "overflowWrap",
    "overflowX",
    "overflowY",
    "padding",
    "paddingBottom",
    "paddingLeft",
    "paddingRight",
    "paddingTop",
    "pageBreakAfter",
    "pageBreakBefore",
    "pageBreakInside",
    "perspective",
    "perspectiveOrigin",
    "placeContent",
    "placeItems",
    "placeSelf",
    "pointerEvents",
    "position",
    "quotes",
    "resize",
    "right",
    "rowGap",
    "scrollBehavior",
    "tabSize",
    "tableLayout",
    "textAlign",
    "textAlignLast",
    "textCombineUpright",
    "textDecoration",
    "textDecorationColor",
    "textDecorationLine",
    "textDecorationStyle",
    "textIndent",
    "textJustify",
    "textOrientation",
    "textOverflow",
    "textShadow",
    "textTransform",
    "top",
    "transform",
    "transformOrigin",
    "transformStyle",
    "transition",
    "transitionDelay",
    "transitionDuration",
    "transitionProperty",
    "transitionTimingFunction",
    "unicodeBidi",
    "userSelect",
    "verticalAlign",
    "visibility",
    "whiteSpace",
    "width",
    "willChange",
    "wordBreak",
    "wordSpacing",
    "wordWrap",
    "writingMode",
    "zIndex",
};

/* Web Storage (localStorage/sessionStorage): in-memory only — not persisted
 * across reloads, but enough for pages that stash a preference (items live in a
 * hidden "@items" backing object). getItem returns null for a missing key. */
static js_value storage_getitem(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value items = js_get(ctx, t, "@items"), v;
  (void)argc;
  if (items.type != JV_OBJ)
    return js_null();
  v = js_get(ctx, items, arg_str(ctx, A(0)));
  return v.type == JV_UNDEF ? js_null() : js_to_string(ctx, v);
}
static js_value storage_setitem(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value items = js_get(ctx, t, "@items");
  (void)argc;
  if (items.type == JV_OBJ)
    js_set(ctx, items, arg_str(ctx, A(0)), js_to_string(ctx, A(1)));
  return js_undef();
}
static js_value storage_removeitem(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value items = js_get(ctx, t, "@items");
  (void)argc;
  if (items.type == JV_OBJ)
    js_set(ctx, items, arg_str(ctx, A(0)), js_undef());
  return js_undef();
}
static js_value storage_clear(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  js_set(ctx, t, "@items", js_obj_value(js_new_object(ctx, ctx->object_proto)));
  return js_undef();
}
static js_value make_storage(js_ctx *ctx)
{
  js_object *s = js_new_object(ctx, ctx->object_proto);
  gc_push_scope();
  gc_keep(s);
  js_def(s, "@items", js_obj_value(js_new_object(ctx, ctx->object_proto)), P_WRITABLE);
  def_method(s, "getItem", storage_getitem, 1);
  def_method(s, "setItem", storage_setitem, 2);
  def_method(s, "removeItem", storage_removeitem, 1);
  def_method(s, "clear", storage_clear, 0);
  gc_pop_scope();
  return js_obj_value(s);
}

/* CSS.supports(): recognise a property by name (values aren't validated — we
 * report a known property as supported, matching the style-object behaviour).
 * `selector(...)` conditions report false (no selector-support detection yet).
 */
static int ci_has(const char *hay, const char *needle); /* defined below */
static void css_norm(const char *s, char *out, int cap)
{
  int o = 0; /* lowercase, strip dashes */
  for (int i = 0; s[i] && o < cap - 1; i++) {
    char c = s[i];
    if (c == '-')
      continue;
    out[o++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  out[o] = 0;
}
static int css_prop_known(const char *prop)
{
  /* The table is a compile-time constant, so normalize it once and reuse. */
  static char norm_props[sizeof k_css_props / sizeof k_css_props[0]][64];
  static int norm_built;
  char pn[64];
  css_norm(prop, pn, sizeof pn);
  if (!pn[0])
    return 0;
  if (!norm_built) {
    for (unsigned i = 0; i < sizeof k_css_props / sizeof k_css_props[0]; i++)
      css_norm(k_css_props[i], norm_props[i], sizeof norm_props[i]);
    norm_built = 1;
  }
  for (unsigned i = 0; i < sizeof k_css_props / sizeof k_css_props[0]; i++)
    if (streq(pn, norm_props[i]))
      return 1;
  return 0;
}
static js_value css_supports(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  if (argc >= 2) /* CSS.supports(prop, value) */
    return js_bool(css_prop_known(arg_str(ctx, A(0))));
  { /* CSS.supports("cond string") */
    const char *cond = arg_str(ctx, A(0));
    char prop[64];
    int i = 0;
    const char *p = cond;
    if (ci_has(cond, "selector("))
      return js_bool(0);
    while (*p == ' ' || *p == '(')
      p++;
    while (*p && *p != ':' && i < 63)
      prop[i++] = *p++;
    while (i > 0 && prop[i - 1] == ' ')
      i--;
    prop[i] = 0;
    if (*p != ':' || i == 0)
      return js_bool(0);
    return js_bool(css_prop_known(prop));
  }
}

/* Expose a DOM interface constructor `name` on the global, wired to `proto`:
 * name.prototype === proto and proto.constructor === name. The body is the
 * no-op HTMLElement base ctor — only the prototype chain matters for
 * reference/instanceof. */
void define_iface(js_ctx *ctx, js_object *win, const char *name, js_object *proto)
{
  js_object *ctor = js_new_native(ctx, htmlelement_ctor, name, 0);
  gc_push_scope();
  gc_keep(ctor);
  gc_keep(proto);
  js_def(ctor, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, name, js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
}

static void setup_dom(js_ctx *ctx, const char *page_url)
{
  js_object *win = ctx->global;

  g_node_proto = js_new_object(ctx, ctx->object_proto); /* Node.prototype */
  g_element_proto = js_new_object(ctx, g_node_proto);
  g_style_proto = js_new_object(ctx, ctx->object_proto);
  g_classlist_proto = js_new_object(ctx, ctx->object_proto);
  g_url_proto = js_new_object(ctx, ctx->object_proto);
  g_sp_proto = js_new_object(ctx, ctx->object_proto);
  g_formdata_proto = js_new_object(ctx, ctx->object_proto);
  g_event_proto = js_new_object(ctx, ctx->object_proto);
  g_html_proto = js_new_object(ctx, g_element_proto); /* HTMLElement.prototype */
  g_media_proto = js_new_object(ctx, g_html_proto);   /* HTMLMediaElement.prototype */
  g_custom_elements = js_new_object(ctx, ctx->object_proto);
  g_module_cache = js_new_object(ctx, ctx->object_proto);
  g_module_src = js_new_object(ctx, ctx->object_proto);
  g_document_proto = js_new_object(ctx, g_node_proto);
  g_response_proto = js_new_object(ctx, ctx->object_proto);
  g_ws_proto = js_new_object(ctx, ctx->object_proto);
  g_notification_proto = js_new_object(ctx, ctx->object_proto);
  g_xhr_proto = js_new_object(ctx, ctx->object_proto);
  g_blob_proto = js_new_object(ctx, ctx->object_proto);
  g_remote_proto = js_new_object(ctx, ctx->object_proto); /* RemotePlayback.prototype */
  g_remotes = js_new_array(ctx, 0);
  g_abortsignal_proto = js_new_object(ctx, ctx->object_proto);
  g_abortcontroller_proto = js_new_object(ctx, ctx->object_proto);
  g_rtc_proto = js_new_object(ctx, ctx->object_proto);
  g_document = js_new_object(ctx, g_document_proto);
  g_live_wrappers = js_new_array(ctx, 0);
  g_websockets = js_new_array(ctx, 0);
  g_bchannel_proto = js_new_object(ctx, ctx->object_proto);
  g_bchannels = js_new_array(ctx, 0);
  g_notifications = js_new_array(ctx, 0);
  g_timers = js_new_array(ctx, 0);
  g_tmeta = 0;
  g_tcap = 0;
  g_ntimers = 0;
  g_nparsed = 0;
  gc_add_root((void **)&g_node_proto);
  gc_add_root((void **)&g_element_proto);
  gc_add_root((void **)&g_style_proto);
  gc_add_root((void **)&g_classlist_proto);
  gc_add_root((void **)&g_url_proto);
  gc_add_root((void **)&g_sp_proto);
  gc_add_root((void **)&g_formdata_proto);
  gc_add_root((void **)&g_event_proto);
  gc_add_root((void **)&g_html_proto);
  gc_add_root((void **)&g_media_proto);
  gc_add_root((void **)&g_custom_elements);
  gc_add_root((void **)&g_module_cache);
  gc_add_root((void **)&g_module_src);
  gc_add_root((void **)&g_document_proto);
  gc_add_root((void **)&g_response_proto);
  gc_add_root((void **)&g_ws_proto);
  gc_add_root((void **)&g_xhr_proto);
  gc_add_root((void **)&g_blob_proto);
  gc_add_root((void **)&g_remote_proto);
  gc_add_root((void **)&g_remotes);
  gc_add_root((void **)&g_abortsignal_proto);
  gc_add_root((void **)&g_abortcontroller_proto);
  gc_add_root((void **)&g_rtc_proto);
  gc_add_root((void **)&g_document);
  gc_add_root((void **)&g_live_wrappers);
  gc_add_root((void **)&g_websockets);
  gc_add_root((void **)&g_bchannel_proto);
  gc_add_root((void **)&g_bchannels);
  gc_add_root((void **)&g_notification_proto);
  gc_add_root((void **)&g_notifications);
  gc_add_root((void **)&g_timers);
  js_set_module_loader(ctx, browser_load_module, 0);

  /* The document IS the root node's wrapper, so events bubbling to the top
   * reach document.addEventListener handlers, and node.parentNode chains
   * terminate at `document`. (window/global is appended past it during
   * dispatch.) */
  g_document->host = g_doc_root;
  if (g_doc_root)
    g_doc_root->wrapper = g_document;
  dom_on_free_wrapper = wrapper_detach;

  /* CSSStyleDeclaration-lite: accessors + methods over the style="" attribute
   */
  /* Full standard CSS property set (generic style-attr backed accessors) +
   * length, so `prop in style` / `style[prop]` / `style.length` work. The
   * layout-specific accessors below then refine the handful that matter. */
  for (unsigned pi = 0; pi < sizeof k_css_props / sizeof k_css_props[0]; pi++)
    def_style_prop(k_css_props[pi]);
  def_acc(g_style_proto, "length", style_gen_length, 0);
  def_acc(g_style_proto, "color", style_get_color, style_set_color);
  def_acc(g_style_proto, "background", style_get_background, style_set_background);
  def_acc(g_style_proto, "backgroundColor", style_get_backgroundColor, style_set_backgroundColor);
  def_acc(g_style_proto, "backgroundImage", style_get_backgroundImage, style_set_backgroundImage);
  def_acc(g_style_proto, "backgroundPosition", style_get_backgroundPosition, style_set_backgroundPosition);
  def_acc(g_style_proto, "backgroundSize", style_get_backgroundSize, style_set_backgroundSize);
  def_acc(g_style_proto, "backgroundRepeat", style_get_backgroundRepeat, style_set_backgroundRepeat);
  def_acc(g_style_proto, "display", style_get_display, style_set_display);
  def_acc(g_style_proto, "width", style_get_width, style_set_width);
  def_acc(g_style_proto, "height", style_get_height, style_set_height);
  def_acc(g_style_proto, "fontWeight", style_get_fontWeight, style_set_fontWeight);
  def_acc(g_style_proto, "fontFamily", style_get_fontFamily, style_set_fontFamily);
  def_acc(g_style_proto, "textAlign", style_get_textAlign, style_set_textAlign);
  def_acc(g_style_proto, "textDecoration", style_get_textDecoration, style_set_textDecoration);
  def_acc(g_style_proto, "cssText", style_get_csstext, style_set_csstext);
  def_method(g_style_proto, "setProperty", style_setprop, 2);
  def_method(g_style_proto, "getPropertyValue", style_getprop, 1);
  def_method(g_style_proto, "removeProperty", style_removeprop, 1);

  /* DOMTokenList prototype (element.classList) */
  def_acc(g_classlist_proto, "length", cl_length, 0);
  def_acc(g_classlist_proto, "value", cl_get_value, cl_set_value);
  def_method(g_classlist_proto, "add", cl_add, 1);
  def_method(g_classlist_proto, "remove", cl_remove, 1);
  def_method(g_classlist_proto, "toggle", cl_toggle, 1);
  def_method(g_classlist_proto, "replace", cl_replace, 2);
  def_method(g_classlist_proto, "contains", cl_contains, 1);
  def_method(g_classlist_proto, "item", cl_item, 1);
  def_method(g_classlist_proto, "forEach", cl_foreach, 1);
  def_method(g_classlist_proto, "values", cl_values, 0);
  def_method(g_classlist_proto, "keys", cl_keys, 0);
  def_method(g_classlist_proto, "entries", cl_entries, 0);
  def_method(g_classlist_proto, "toString", cl_get_value, 0);
  {
    js_object *f = js_new_native(ctx, cl_values, "[Symbol.iterator]", 0);
    js_def(g_classlist_proto, js_symbol_key(ctx->sym_iterator), js_obj_value(f), P_CONFIGURABLE);
  }

  /* element prototype */
  def_acc(g_element_proto, "tagName", el_tagName, 0);
  def_acc(g_element_proto, "nodeName", el_nodeName, 0);
  def_acc(g_element_proto, "nodeType", el_nodeType, 0);
  def_acc(g_element_proto, "sheet", el_get_sheet, 0);
  def_acc(g_element_proto, "id", el_get_id, el_set_id);
  def_acc(g_element_proto, "nonce", el_get_nonce, el_set_nonce);
  def_acc(g_element_proto, "className", el_get_class, el_set_class);
  def_acc(g_element_proto, "classList", el_get_classlist, 0);
  def_acc(g_element_proto, "textContent", el_get_text, el_set_text);
  def_acc(g_element_proto, "innerText", el_get_text, el_set_text);
  def_acc(g_element_proto, "innerHTML", el_get_html, el_set_html);
  def_acc(g_element_proto, "parentNode", el_parent, 0);
  def_acc(g_element_proto, "parentElement", el_parent, 0);
  def_acc(g_element_proto, "firstChild", el_first_child, 0);
  def_acc(g_element_proto, "firstElementChild", el_first_child, 0);
  def_acc(g_element_proto, "lastChild", el_last_child, 0);
  def_acc(g_element_proto, "nextSibling", el_next, 0);
  def_acc(g_element_proto, "nextElementSibling", el_next, 0);
  def_acc(g_element_proto, "children", el_children, 0);
  def_acc(g_element_proto, "childNodes", el_children, 0);
  def_acc(g_element_proto, "src", el_get_src, el_set_src);
  def_acc(g_element_proto, "href", el_get_href, el_set_href);
  /* HTMLAnchorElement/HTMLAreaElement URL decomposition (read-only) */
  def_acc(g_element_proto, "protocol", el_get_protocol, 0);
  def_acc(g_element_proto, "host", el_get_host, 0);
  def_acc(g_element_proto, "hostname", el_get_hostname, 0);
  def_acc(g_element_proto, "port", el_get_port, 0);
  def_acc(g_element_proto, "pathname", el_get_pathname, 0);
  def_acc(g_element_proto, "search", el_get_search, 0);
  def_acc(g_element_proto, "hash", el_get_hash, 0);
  def_acc(g_element_proto, "origin", el_get_origin, 0);
  def_acc(g_element_proto, "contentDocument", el_content_document, 0);
  def_acc(g_element_proto, "contentWindow", el_content_window, 0);
  def_acc(g_element_proto, "style", el_get_style, 0);
  def_acc(g_element_proto, "dataset", el_get_dataset, 0);
  def_acc(g_element_proto, "offsetWidth", el_offsetWidth, 0);
  def_acc(g_element_proto, "offsetHeight", el_offsetHeight, 0);
  def_acc(g_element_proto, "offsetTop", el_offsetTop, 0);
  def_acc(g_element_proto, "offsetLeft", el_offsetLeft, 0);
  def_acc(g_element_proto, "clientWidth", el_offsetWidth, 0); /* no borders modeled */
  def_acc(g_element_proto, "clientHeight", el_offsetHeight, 0);
  def_acc(g_element_proto, "scrollWidth", el_offsetWidth, 0); /* no overflow modeled */
  def_acc(g_element_proto, "scrollHeight", el_offsetHeight, 0);
  def_acc(g_element_proto, "scrollTop", el_scrollTop, 0);
  def_acc(g_element_proto, "scrollLeft", el_scrollTop, 0);
  def_method(g_element_proto, "getAttribute", el_get_attr, 1);
  def_method(g_element_proto, "setAttribute", el_set_attr, 2);
  def_method(g_element_proto, "hasAttribute", el_has_attr, 1);
  def_method(g_element_proto, "removeAttribute", el_rm_attr, 1);
  def_method(g_element_proto, "appendChild", el_append, 1);
  def_method(g_element_proto, "append", el_append, 1);
  def_method(g_element_proto, "removeChild", el_remove_child, 1);
  def_method(g_element_proto, "insertBefore", el_insert_before, 2);
  def_method(g_element_proto, "getElementsByTagName", el_get_by_tag, 1);
  def_method(g_element_proto, "querySelector", el_query, 1);
  def_method(g_element_proto, "querySelectorAll", el_query_all, 1);
  def_method(g_element_proto, "matches", el_matches, 1);
  def_method(g_element_proto, "closest", el_closest, 1);
  def_method(g_element_proto, "isSameNode", el_is_same_node, 1);
  def_method(g_element_proto, "contains", el_contains, 1);
  def_method(g_element_proto, "addEventListener", el_add_listener, 3);
  def_method(g_element_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_element_proto, "dispatchEvent", el_dispatch, 1);
  def_method(g_element_proto, "getBoundingClientRect", el_get_bounding_rect, 0);

  setup_media_proto(ctx);  /* HTMLMediaElement props/methods + the Audio ctor */
  setup_remote_proto(ctx); /* RemotePlayback (media.remote) methods + events */

  /* document: methods on the shared proto so DOMParser documents inherit them
   * (they're root-relative — they query whichever document is `this`). */
  def_method(g_document_proto, "getElementById", doc_get_by_id, 1);
  def_method(g_document_proto, "getElementsByTagName", doc_get_by_tag, 1);
  def_method(g_document_proto, "querySelector", doc_query, 1);
  def_method(g_document_proto, "querySelectorAll", doc_query_all, 1);
  def_method(g_document_proto, "createElement", doc_create_el, 1);
  def_method(g_document_proto, "createElementNS", doc_create_el_ns, 2);
  def_method(g_document_proto, "createTextNode", doc_create_text, 1);
  def_method(g_document_proto, "addEventListener", el_add_listener, 3);
  def_method(g_document_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_document_proto, "dispatchEvent", el_dispatch, 1);
  def_method(g_document_proto, "write", el_noop, 1);
  def_acc(g_document_proto, "activeElement", doc_active_element, 0);
  def_acc(g_document_proto, "body", doc_body, 0);
  def_acc(g_document_proto, "head", doc_head, 0);
  def_acc(g_document_proto, "documentElement", doc_root_el, 0);
  def_acc(g_document_proto, "title", doc_get_title, doc_set_title);
  js_def(g_document, "readyState", js_mk_cstring(ctx, "loading"), P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  js_def(g_document, "visibilityState", js_mk_cstring(ctx, "visible"), P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  js_def(g_document, "hidden", js_bool(0), P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  js_def(g_document, "currentScript", js_null(), P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  js_def(g_document, "referrer", js_mk_cstring(ctx, B->referrer), P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  def_method(g_document, "hasFocus", el_true, 0);

  /* window / globals */
  js_def(win, "document", js_obj_value(g_document), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "window", js_obj_value(win), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "self", js_obj_value(win), P_WRITABLE | P_CONFIGURABLE);
  {
    js_object *loc = js_new_object(ctx, ctx->object_proto);
    gc_push_scope();
    gc_keep(loc);
    fill_url_props(ctx, loc, page_url ? page_url : ""); /* href/origin/pathname/... */
    def_method(loc, "assign", loc_assign, 1);
    def_method(loc, "replace", loc_assign, 1);
    def_method(loc, "reload", loc_reload, 0);
    def_method(loc, "toString", loc_tostring, 0);
    js_def(g_document, "location", js_obj_value(loc), P_WRITABLE | P_CONFIGURABLE);
    js_def(g_document, "URL", js_mk_cstring(ctx, page_url ? page_url : ""), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "location", js_obj_value(loc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  {
    js_object *nav = js_new_object(ctx, ctx->object_proto);
    js_object *langs;
    gc_push_scope();
    gc_keep(nav);
    js_def(nav, "userAgent", js_mk_cstring(ctx, browser_ua()), P_ENUMERABLE);
    js_def(nav, "appName", js_mk_cstring(ctx, "Netscape"), P_ENUMERABLE); /* legacy, widely sniffed */
    js_def(nav, "appCodeName", js_mk_cstring(ctx, "Mozilla"), P_ENUMERABLE);
    js_def(nav, "appVersion", js_mk_cstring(ctx, browser_ua_version()), P_ENUMERABLE); /* UA minus "Mozilla/" */
    js_def(nav, "product", js_mk_cstring(ctx, "Gecko"), P_ENUMERABLE);
    js_def(nav, "productSub", js_mk_cstring(ctx, "20030107"), P_ENUMERABLE);
    js_def(nav, "vendor", js_mk_cstring(ctx, "OwOS"), P_ENUMERABLE);
    js_def(nav, "vendorSub", js_mk_cstring(ctx, ""), P_ENUMERABLE);
    js_def(nav, "platform", js_mk_cstring(ctx, "OwOS x86"), P_ENUMERABLE);
    js_def(nav, "oscpu", js_mk_cstring(ctx, "OwOS i686"), P_ENUMERABLE);
    js_def(nav, "language", js_mk_cstring(ctx, "en-US"), P_ENUMERABLE);
    langs = js_new_array(ctx, 0);
    gc_keep(langs);
    js_array_push(ctx, langs, js_mk_cstring(ctx, "en-US"));
    js_array_push(ctx, langs, js_mk_cstring(ctx, "en"));
    js_def(nav, "languages", js_obj_value(langs), P_ENUMERABLE);
    js_def(nav, "onLine", js_bool(1), P_ENUMERABLE);
    js_def(nav, "cookieEnabled", js_bool(1), P_ENUMERABLE);
    js_def(nav, "doNotTrack", js_null(), P_ENUMERABLE);
    js_def(nav, "hardwareConcurrency", js_number(1), P_ENUMERABLE);
    js_def(nav, "maxTouchPoints", js_number(0), P_ENUMERABLE);
    js_def(nav, "deviceMemory", js_number(2), P_ENUMERABLE);
    js_def(nav, "pdfViewerEnabled", js_bool(0), P_ENUMERABLE);
    js_def(nav, "webdriver", js_bool(0), P_ENUMERABLE);
    def_method(nav, "javaEnabled", nav_false, 0);
    def_method(nav, "taintEnabled", nav_false, 0);
    def_method(nav, "sendBeacon", nav_sendbeacon, 2);
    def_method(nav, "vibrate", nav_false, 1);
    {
      js_object *conn = js_new_object(ctx, ctx->object_proto); /* NetworkInformation */
      gc_keep(conn);
      js_def(conn, "effectiveType", js_mk_cstring(ctx, "4g"), P_ENUMERABLE);
      js_def(conn, "saveData", js_bool(0), P_ENUMERABLE);
      js_def(conn, "rtt", js_number(50), P_ENUMERABLE);
      js_def(conn, "downlink", js_number(10), P_ENUMERABLE);
      def_method(conn, "addEventListener", el_noop, 2);
      js_def(nav, "connection", js_obj_value(conn), P_ENUMERABLE);
    }
    {
      js_object *clip = js_new_object(ctx, ctx->object_proto); /* navigator.clipboard */
      gc_keep(clip);
      def_method(clip, "writeText", clip_writetext, 1);
      def_method(clip, "readText", clip_readtext, 0);
      def_method(clip, "write", clip_write, 1);
      def_method(clip, "read", clip_read, 0);
      def_method(clip, "addEventListener", el_noop, 2);
      def_method(clip, "removeEventListener", el_noop, 2);
      js_def(nav, "clipboard", js_obj_value(clip), P_ENUMERABLE);
    }
    {
      js_object *geo = js_new_object(ctx, ctx->object_proto); /* navigator.geolocation */
      gc_keep(geo);
      def_method(geo, "getCurrentPosition", geo_get_current, 3);
      def_method(geo, "watchPosition", geo_watch, 3);
      def_method(geo, "clearWatch", el_noop, 1);
      js_def(nav, "geolocation", js_obj_value(geo), P_ENUMERABLE);
    }
    {
      js_object *md = js_new_object(ctx, ctx->object_proto); /* navigator.mediaDevices */
      gc_keep(md);
      def_method(md, "getUserMedia", md_get_user_media, 1);
      def_method(md, "getDisplayMedia", md_get_display_media, 1);
      def_method(md, "enumerateDevices", md_enumerate, 0);
      def_method(md, "addEventListener", el_noop, 2);
      def_method(md, "removeEventListener", el_noop, 2);
      js_def(nav, "mediaDevices", js_obj_value(md), P_ENUMERABLE);
    }
    {
      js_object *perm = js_new_object(ctx, ctx->object_proto); /* navigator.permissions */
      gc_keep(perm);
      def_method(perm, "query", perm_query, 1);
      js_def(nav, "permissions", js_obj_value(perm), P_ENUMERABLE);
    }
    js_def(win, "navigator", js_obj_value(nav), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  {
    js_object *scr = js_new_object(ctx, ctx->object_proto); /* window.screen */
    gc_push_scope();
    gc_keep(scr);
    def_acc(scr, "width", screen_width, 0);
    def_acc(scr, "height", screen_height, 0);
    def_acc(scr, "availWidth", screen_width, 0);
    def_acc(scr, "availHeight", screen_height, 0);
    js_def(scr, "availLeft", js_number(0), P_ENUMERABLE);
    js_def(scr, "availTop", js_number(0), P_ENUMERABLE);
    js_def(scr, "colorDepth", js_number(24), P_ENUMERABLE);
    js_def(scr, "pixelDepth", js_number(24), P_ENUMERABLE);
    js_def(win, "screen", js_obj_value(scr), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  /* ClipboardItem (window global) — pairs with navigator.clipboard.read/write
   */
  {
    js_object *cic;
    g_clipitem_proto = js_new_object(ctx, ctx->object_proto);
    gc_add_root((void **)&g_clipitem_proto);
    gc_push_scope();
    def_method(g_clipitem_proto, "getType", clipitem_gettype, 1);
    cic = (js_object *)gc_keep(js_new_native(ctx, clipitem_ctor, "ClipboardItem", 1));
    def_method(cic, "supports", clipitem_supports, 1);
    js_def(cic, "prototype", js_obj_value(g_clipitem_proto), 0);
    js_def(g_clipitem_proto, "constructor", js_obj_value(cic), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "ClipboardItem", js_obj_value(cic), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  /* crypto: getRandomValues (RDRAND-backed) + randomUUID. window.crypto and the
   * bare `crypto` global resolve to the same object (window IS the global). */
  {
    js_object *cr = js_new_object(ctx, ctx->object_proto);
    gc_push_scope();
    gc_keep(cr);
    def_method(cr, "getRandomValues", crypto_getrandomvalues, 1);
    def_method(cr, "randomUUID", crypto_randomuuid, 0);
    js_def(win, "crypto", js_obj_value(cr), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  js_webapi_setup(ctx, win);
  /* URL + URLSearchParams (WHATWG Web APIs over curl/url.h) */
  def_method(g_url_proto, "toString", url_tostring, 0);
  def_method(g_url_proto, "toJSON", url_tostring, 0);
  def_method(g_sp_proto, "get", sp_get, 1);
  def_method(g_sp_proto, "getAll", sp_get_all, 1);
  def_method(g_sp_proto, "has", sp_has, 1);
  def_method(g_sp_proto, "append", sp_append, 2);
  def_method(g_sp_proto, "set", sp_set, 2);
  def_method(g_sp_proto, "delete", sp_delete, 1);
  def_method(g_sp_proto, "forEach", sp_for_each, 1);
  def_method(g_sp_proto, "toString", sp_tostring, 0);
  {
    js_object *uc = js_new_native(ctx, url_ctor, "URL", 1);
    js_object *sc = js_new_native(ctx, sp_ctor, "URLSearchParams", 1);
    gc_push_scope();
    gc_keep(uc);
    gc_keep(sc);
    js_def(uc, "prototype", js_obj_value(g_url_proto), 0);
    js_def(g_url_proto, "constructor", js_obj_value(uc), P_WRITABLE | P_CONFIGURABLE);
    js_def(sc, "prototype", js_obj_value(g_sp_proto), 0);
    js_def(g_sp_proto, "constructor", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "URL", js_obj_value(uc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "URLSearchParams", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* FormData — a name/value multimap over the same @k/@v arrays as
   * URLSearchParams, so it shares the sp_* accessors. */
  def_method(g_formdata_proto, "get", sp_get, 1);
  def_method(g_formdata_proto, "getAll", sp_get_all, 1);
  def_method(g_formdata_proto, "has", sp_has, 1);
  def_method(g_formdata_proto, "append", sp_append, 2);
  def_method(g_formdata_proto, "set", sp_set, 2);
  def_method(g_formdata_proto, "delete", sp_delete, 1);
  def_method(g_formdata_proto, "forEach", sp_for_each, 1);
  {
    js_object *fc = js_new_native(ctx, fd_ctor, "FormData", 1);
    gc_push_scope();
    gc_keep(fc);
    js_def(fc, "prototype", js_obj_value(g_formdata_proto), 0);
    js_def(g_formdata_proto, "constructor", js_obj_value(fc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "FormData", js_obj_value(fc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* HTMLFormElement: programmatic submission + the elements collection. */
  {
    js_object *fp = iface_proto_for_tag("form");
    if (fp) {
      def_method(fp, "submit", form_submit, 0);
      def_method(fp, "requestSubmit", form_request_submit, 1);
      def_acc(fp, "elements", form_elements, 0);
    }
  }

  /* DOM events: Event prototype, the Event/CustomEvent constructors, and the
   * window-level listener API (document + element protos got theirs above). */
  def_method(g_event_proto, "preventDefault", ev_prevent, 0);
  def_method(g_event_proto, "stopPropagation", ev_stop, 0);
  def_method(g_event_proto, "stopImmediatePropagation", ev_stop, 0);
  {
    js_object *ec = js_new_native(ctx, event_ctor, "Event", 1);
    gc_push_scope();
    gc_keep(ec);
    js_def(ec, "prototype", js_obj_value(g_event_proto), 0);
    js_def(g_event_proto, "constructor", js_obj_value(ec), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "Event", js_obj_value(ec), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "CustomEvent", js_obj_value(ec), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  /* Event subclasses. Each gets a prototype chaining to its parent interface's
   * (so `mouseEvent instanceof UIEvent` holds up the whole DOM event hierarchy)
   * and a constructor sharing event_sub_ctor, with its prototype in fn->data.
   */
  {
    static const struct {
      const char *name;
      int parent;
    } subs[] = {
        {"UIEvent", -1},   /* -> Event.prototype       */
        {"MouseEvent", 0}, /* -> UIEvent.prototype     */
        {"KeyboardEvent", 0},    {"FocusEvent", 0},   {"InputEvent", 0},
        {"CompositionEvent", 0}, {"PointerEvent", 1}, /* -> MouseEvent.prototype
                                                       */
        {"WheelEvent", 1},       {"DragEvent", 1},
    };
    js_object *protos[sizeof subs / sizeof subs[0]];
    unsigned i;
    for (i = 0; i < sizeof subs / sizeof subs[0]; i++) {
      js_object *parent = subs[i].parent < 0 ? g_event_proto : protos[subs[i].parent];
      js_object *proto = js_new_object(ctx, parent);
      js_object *ctor;
      gc_push_scope();
      gc_keep(proto);
      ctor = (js_object *)gc_keep(js_new_native(ctx, event_sub_ctor, subs[i].name, 1));
      ctor->fn->data = js_obj_value(proto); /* the re-parent target */
      js_def(ctor, "prototype", js_obj_value(proto), 0);
      js_def(proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
      js_def(win, subs[i].name, js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
      protos[i] = proto; /* reachable via win from here on */
      gc_pop_scope();
    }
  }
  def_method(win, "addEventListener", el_add_listener, 3);
  def_method(win, "removeEventListener", el_remove_listener, 3);
  def_method(win, "dispatchEvent", el_dispatch, 1);

  /* Web components: HTMLElement (the base custom classes extend) +
   * customElements */
  {
    js_object *nc = js_new_native(ctx, node_ctor, "Node", 0);
    gc_push_scope();
    gc_keep(nc);
    js_def(nc, "prototype", js_obj_value(g_node_proto), 0);
    js_def(g_node_proto, "constructor", js_obj_value(nc), P_WRITABLE | P_CONFIGURABLE);
    def_node_consts(nc);           /* Node.ELEMENT_NODE etc. (on the interface)  */
    def_node_consts(g_node_proto); /* and inherited by instances (node.TEXT_NODE) */
    js_def(win, "Node", js_obj_value(nc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  {
    js_object *he = js_new_native(ctx, htmlelement_ctor, "HTMLElement", 0);
    js_object *ce = js_new_object(ctx, ctx->object_proto);
    gc_push_scope();
    gc_keep(he);
    gc_keep(ce);
    js_def(he, "prototype", js_obj_value(g_html_proto), 0);
    js_def(g_html_proto, "constructor", js_obj_value(he), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "HTMLElement", js_obj_value(he), P_WRITABLE | P_CONFIGURABLE);
    def_method(ce, "define", ce_define, 2);
    def_method(ce, "get", ce_get, 1);
    def_method(ce, "whenDefined", ce_when_defined, 1);
    js_def(win, "customElements", js_obj_value(ce), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* Standard DOM interface constructors (Element/Document, HTMLMediaElement and
   * the HTMLxxxElement family) so pages can reference and extend them;
   * wrap_node hands each element its matching prototype so `el instanceof
   * HTMLScriptElement` holds. Every HTMLxxxElement.prototype chains to
   * HTMLElement.prototype. */
  {
    /* Element/Document/HTMLMediaElement are the shared bases the family chains
     * to (they map to no single tag, so they stay out of the registry). */
    define_iface(ctx, win, "Element", g_element_proto);
    define_iface(ctx, win, "Document", g_document_proto);
    define_iface(ctx, win, "HTMLMediaElement", g_media_proto);

    unsigned i;
    for (i = 0; i < sizeof g_html_ifaces / sizeof g_html_ifaces[0]; i++) {
      js_object *parent = g_html_ifaces[i].media ? g_media_proto : g_html_proto;
      js_object *proto = js_new_object(ctx, parent);
      gc_push_scope();
      gc_keep(proto);
      define_iface(ctx, win, g_html_ifaces[i].name, proto);
      g_html_ifaces[i].proto = proto;
      gc_add_root((void **)&g_html_ifaces[i].proto);
      gc_pop_scope();
    }
    /* Image is a global alias constructing an <img>; its prototype is
     * HTMLImageElement.prototype so `new Image() instanceof HTMLImageElement`
     * (and instanceof Image) both hold. */
    for (i = 0; i < sizeof g_html_ifaces / sizeof g_html_ifaces[0]; i++) {
      if (!streq(g_html_ifaces[i].name, "HTMLImageElement"))
        continue;
      js_object *ic = js_new_native(ctx, image_ctor, "Image", 2);
      gc_push_scope();
      gc_keep(ic);
      js_def(ic, "prototype", js_obj_value(g_html_ifaces[i].proto), 0);
      js_def(win, "Image", js_obj_value(ic), P_WRITABLE | P_CONFIGURABLE);
      gc_pop_scope();
      break;
    }
  }
  js_def_native(ctx, win, "DOMParser", domparser_ctor, 0); /* new DOMParser().parseFromString */

  /* fetch() + Response.prototype */
  def_method(g_response_proto, "text", resp_text, 0);
  def_method(g_response_proto, "json", resp_json, 0);
  def_method(g_response_proto, "clone", resp_clone, 0);
  def_method(win, "fetch", win_fetch, 2);

  /* WebSocket (RFC 6455 over user/lib/websocket): live, driven by the worker's
   * js_websockets_pump each idle tick. */
  def_method(g_ws_proto, "send", ws_send_m, 1);
  def_method(g_ws_proto, "close", ws_close_m, 2);
  def_method(g_ws_proto, "addEventListener", el_add_listener, 3);
  def_method(g_ws_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_ws_proto, "dispatchEvent", el_dispatch, 1);
  js_def(g_ws_proto, "CONNECTING", js_number(WS_CONNECTING), P_ENUMERABLE);
  js_def(g_ws_proto, "OPEN", js_number(WS_OPEN), P_ENUMERABLE);
  js_def(g_ws_proto, "CLOSING", js_number(WS_CLOSING), P_ENUMERABLE);
  js_def(g_ws_proto, "CLOSED", js_number(WS_CLOSED), P_ENUMERABLE);
  {
    js_object *wc = js_new_native(ctx, ws_ctor, "WebSocket", 1);
    gc_push_scope();
    gc_keep(wc);
    js_def(wc, "prototype", js_obj_value(g_ws_proto), 0);
    js_def(g_ws_proto, "constructor", js_obj_value(wc), P_WRITABLE | P_CONFIGURABLE);
    js_def(wc, "CONNECTING", js_number(WS_CONNECTING), P_ENUMERABLE);
    js_def(wc, "OPEN", js_number(WS_OPEN), P_ENUMERABLE);
    js_def(wc, "CLOSING", js_number(WS_CLOSING), P_ENUMERABLE);
    js_def(wc, "CLOSED", js_number(WS_CLOSED), P_ENUMERABLE);
    js_def(win, "WebSocket", js_obj_value(wc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* BroadcastChannel: same-context message bus (postMessage fans out to other
   * open same-named channels via the shared EventTarget machinery). */
  def_method(g_bchannel_proto, "postMessage", bc_post_m, 1);
  def_method(g_bchannel_proto, "close", bc_close_m, 0);
  def_method(g_bchannel_proto, "addEventListener", el_add_listener, 3);
  def_method(g_bchannel_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_bchannel_proto, "dispatchEvent", el_dispatch, 1);
  {
    js_object *bc = js_new_native(ctx, bchannel_ctor, "BroadcastChannel", 1);
    gc_push_scope();
    gc_keep(bc);
    js_def(bc, "prototype", js_obj_value(g_bchannel_proto), 0);
    js_def(g_bchannel_proto, "constructor", js_obj_value(bc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "BroadcastChannel", js_obj_value(bc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  js_notification_setup(ctx, win, g_notification_proto, g_notifications);

  /* XMLHttpRequest (synchronous transport over browser_cors_fetch) */
  def_method(g_xhr_proto, "open", xhr_open, 2);
  def_method(g_xhr_proto, "setRequestHeader", xhr_setheader, 2);
  def_method(g_xhr_proto, "send", xhr_send, 1);
  def_method(g_xhr_proto, "abort", el_noop, 0);
  def_method(g_xhr_proto, "overrideMimeType", el_noop, 1);
  def_method(g_xhr_proto, "getResponseHeader", xhr_getheader, 1);
  def_method(g_xhr_proto, "getAllResponseHeaders", xhr_getallheaders, 0);
  def_method(g_xhr_proto, "addEventListener", el_add_listener, 3);
  def_method(g_xhr_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_xhr_proto, "dispatchEvent", el_dispatch, 1);
  js_def(g_xhr_proto, "UNSENT", js_number(0), P_ENUMERABLE);
  js_def(g_xhr_proto, "OPENED", js_number(1), P_ENUMERABLE);
  js_def(g_xhr_proto, "HEADERS_RECEIVED", js_number(2), P_ENUMERABLE);
  js_def(g_xhr_proto, "LOADING", js_number(3), P_ENUMERABLE);
  js_def(g_xhr_proto, "DONE", js_number(4), P_ENUMERABLE);
  {
    js_object *xc = js_new_native(ctx, xhr_ctor, "XMLHttpRequest", 0);
    gc_push_scope();
    gc_keep(xc);
    js_def(xc, "prototype", js_obj_value(g_xhr_proto), 0);
    js_def(g_xhr_proto, "constructor", js_obj_value(xc), P_WRITABLE | P_CONFIGURABLE);
    js_def(xc, "UNSENT", js_number(0), P_ENUMERABLE);
    js_def(xc, "OPENED", js_number(1), P_ENUMERABLE);
    js_def(xc, "HEADERS_RECEIVED", js_number(2), P_ENUMERABLE);
    js_def(xc, "LOADING", js_number(3), P_ENUMERABLE);
    js_def(xc, "DONE", js_number(4), P_ENUMERABLE);
    js_def(win, "XMLHttpRequest", js_obj_value(xc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* Blob (bytes + type), enough for sendBeacon / fetch bodies */
  def_method(g_blob_proto, "text", blob_text, 0);
  def_method(g_blob_proto, "arrayBuffer", blob_arraybuffer, 0);
  def_method(g_blob_proto, "slice", blob_slice, 2);
  {
    js_object *bc = js_new_native(ctx, blob_ctor, "Blob", 1);
    gc_push_scope();
    gc_keep(bc);
    js_def(bc, "prototype", js_obj_value(g_blob_proto), 0);
    js_def(g_blob_proto, "constructor", js_obj_value(bc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "Blob", js_obj_value(bc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* AbortController / AbortSignal (used by fetch() and event listeners). */
  def_method(g_abortsignal_proto, "addEventListener", el_add_listener, 3);
  def_method(g_abortsignal_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_abortsignal_proto, "dispatchEvent", el_dispatch, 1);
  def_method(g_abortsignal_proto, "throwIfAborted", sig_throw_if_aborted, 0);
  def_method(g_abortcontroller_proto, "abort", ctrl_abort, 1);
  {
    js_object *sc = js_new_native(ctx, abortsignal_ctor, "AbortSignal", 0);
    js_object *cc = js_new_native(ctx, abortcontroller_ctor, "AbortController", 0);
    gc_push_scope();
    gc_keep(sc);
    gc_keep(cc);
    js_def(sc, "prototype", js_obj_value(g_abortsignal_proto), 0);
    js_def(g_abortsignal_proto, "constructor", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
    js_def_native(ctx, sc, "abort", sig_static_abort, 1); /* AbortSignal.abort(reason?) */
    js_def(cc, "prototype", js_obj_value(g_abortcontroller_proto), 0);
    js_def(g_abortcontroller_proto, "constructor", js_obj_value(cc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "AbortSignal", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "AbortController", js_obj_value(cc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* RTCPeerConnection: inert (no WebRTC/ICE in OwOS). Present under all vendor
   * prefixes so IP-leak probes and video-chat feature detection run past
   * `new RTCPeerConnection(...)` instead of throwing "not a constructor"; it
   * simply never gathers ICE candidates. */
  def_method(g_rtc_proto, "createDataChannel", rtc_create_data_channel, 1);
  def_method(g_rtc_proto, "createOffer", rtc_create_offer, 0);
  def_method(g_rtc_proto, "createAnswer", rtc_create_answer, 0);
  def_method(g_rtc_proto, "setLocalDescription", rtc_set_desc, 1);
  def_method(g_rtc_proto, "setRemoteDescription", rtc_set_desc, 1);
  def_method(g_rtc_proto, "addIceCandidate", rtc_set_desc, 1);
  def_method(g_rtc_proto, "getStats", rtc_get_stats, 0);
  def_method(g_rtc_proto, "addTrack", rtc_noop, 2);
  def_method(g_rtc_proto, "removeTrack", rtc_noop, 1);
  def_method(g_rtc_proto, "getSenders", el_empty_array, 0);
  def_method(g_rtc_proto, "getReceivers", el_empty_array, 0);
  def_method(g_rtc_proto, "getTransceivers", el_empty_array, 0);
  def_method(g_rtc_proto, "restartIce", rtc_noop, 0);
  def_method(g_rtc_proto, "close", rtc_noop, 0);
  def_method(g_rtc_proto, "addEventListener", el_add_listener, 3);
  def_method(g_rtc_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_rtc_proto, "dispatchEvent", el_dispatch, 1);
  {
    js_object *rc = js_new_native(ctx, rtc_ctor, "RTCPeerConnection", 1);
    gc_push_scope();
    gc_keep(rc);
    js_def(rc, "prototype", js_obj_value(g_rtc_proto), 0);
    js_def(g_rtc_proto, "constructor", js_obj_value(rc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "RTCPeerConnection", js_obj_value(rc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "webkitRTCPeerConnection", js_obj_value(rc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "mozRTCPeerConnection", js_obj_value(rc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* history (SPA routers lean on it): inert but present so pushState/back don't
   * throw. The page doesn't re-route, but its scripts run past these calls. */
  {
    js_object *hist = js_new_object(ctx, ctx->object_proto);
    gc_push_scope();
    gc_keep(hist);
    js_def(hist, "state", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(hist, "scrollRestoration", js_mk_cstring(ctx, "auto"), P_WRITABLE | P_ENUMERABLE);
    def_acc(hist, "length", his_length, 0); /* live: the tab's depth */
    def_method(hist, "pushState", his_pushstate, 3);
    def_method(hist, "replaceState", his_replacestate, 3);
    def_method(hist, "back", his_back, 0);
    def_method(hist, "forward", his_forward, 0);
    def_method(hist, "go", his_go, 1);
    js_def(win, "history", js_obj_value(hist), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  /* the Observer family + matchMedia + queueMicrotask: present but inert */
  {
    js_object *mo = js_new_native(ctx, observer_ctor, "MutationObserver", 1);
    js_object *io = js_new_native(ctx, observer_ctor, "IntersectionObserver", 1);
    js_object *ro = js_new_native(ctx, observer_ctor, "ResizeObserver", 1);
    js_object *po = js_new_native(ctx, observer_ctor, "PerformanceObserver", 1);
    gc_push_scope();
    gc_keep(mo);
    gc_keep(io);
    gc_keep(ro);
    gc_keep(po);
    js_def(win, "MutationObserver", js_obj_value(mo), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "IntersectionObserver", js_obj_value(io), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "ResizeObserver", js_obj_value(ro), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "PerformanceObserver", js_obj_value(po), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  def_method(win, "matchMedia", win_matchmedia, 1);
  js_def(win, "localStorage", make_storage(ctx), P_WRITABLE | P_CONFIGURABLE);
  js_idb_setup(ctx, win);
  js_def(win, "sessionStorage", make_storage(ctx), P_WRITABLE | P_CONFIGURABLE);
  { /* CSS.supports() */
    js_object *css = js_new_object(ctx, ctx->object_proto);
    gc_push_scope();
    gc_keep(css);
    def_method(css, "supports", css_supports, 2);
    js_def(win, "CSS", js_obj_value(css), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  def_method(win, "queueMicrotask", win_queuemicrotask, 1);
  def_method(win, "requestIdleCallback", el_noop, 1);
  def_method(win, "cancelIdleCallback", el_noop, 1);

  /* timers: real, driven cooperatively by js_timers_pump on the worker tick */
  def_method(win, "setTimeout", win_set_timeout, 2);
  def_method(win, "setInterval", win_set_interval, 2);
  def_method(win, "clearTimeout", win_clear_timer, 1);
  def_method(win, "clearInterval", win_clear_timer, 1);
  def_method(win, "requestAnimationFrame", win_request_anim, 1);
  def_method(win, "cancelAnimationFrame", win_clear_timer, 1);
  def_method(win, "alert", win_alert, 1);
  def_method(win, "confirm", win_confirm, 1);
  def_method(win, "prompt", win_prompt, 2);
  def_method(win, "getComputedStyle", win_getcomputedstyle, 1);
  def_acc(win, "innerWidth", win_innerWidth, 0);
  def_acc(win, "innerHeight", win_innerHeight, 0);
  def_acc(win, "scrollX", win_scrollX, 0);
  def_acc(win, "pageXOffset", win_scrollX, 0);
  def_acc(win, "scrollY", win_scrollY, 0);
  def_acc(win, "pageYOffset", win_scrollY, 0);

  /* navigator.serviceWorker + caches + Response/Request; re-instates a
   * controlling worker registered on a previous page (must run last, after
   * fetch/Promise/etc. are on the global). */
  sw_install(ctx, win, page_url);
}

/* ------------------------------------------------------- runner ----------- */

/* console.* output goes to the devtools Console — the canonical sink, as in a
 * real browser, not the process stdout/serial. The engine's log level maps to
 * the panel's colouring (warn yellow, error red). */
static void js_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  int dl = level == JS_LOG_ERROR ? DT_ERROR : level == JS_LOG_WARN ? DT_WARN : DT_LOG;
  devtools_console_append(dl, s, len);
}

/* The script/context currently executing, for error attribution in the devtools
 * Console — a script URL, "<page> (inline script #N)", or an async label like
 * "<event listener>"/"<timer callback>". Set at each execution entry point. */
static char g_cur_script[URL_MAX];
void js_set_cur_script(const char *label)
{
  int k = 0;
  g_cur_script[0] = '\0';
  if (label)
    sapp(g_cur_script, &k, sizeof g_cur_script, label);
  /* Point the engine at the same buffer so a captured stack's outermost frame
   * names this script/async entry (the sites that build g_cur_script inline
   * mutate the same storage, so this one binding tracks them all). */
  if (g_ctx)
    js_set_script(g_ctx, g_cur_script);
}
/* Attribute the current execution to a named event's listener, e.g. a thrown
 * error inside an onclick shows "in <click listener>". */
static void js_set_event_script(const char *type)
{
  char buf[64];
  int k = 0;
  sapp(buf, &k, sizeof buf, "<");
  sapp(buf, &k, sizeof buf, (type && type[0]) ? type : "event");
  sapp(buf, &k, sizeof buf, " listener>");
  js_set_cur_script(buf);
}

void report_exc(js_ctx *ctx)
{
  js_value e = ctx->exc;
  sbuf sb;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  int have_stack = 0;
  ctx->has_exc = 0;
  sb_puts(&sb, "[js] uncaught ");
  if (e.type == JV_OBJ) {
    js_value stk = js_get(ctx, e, "stack");
    if (stk.type == JV_STR && stk.u.str->len) { /* name: msg + frames */
      sb_puts(&sb, stk.u.str->data);            /* stack ends in '\n' */
      have_stack = 1;
    } else {
      js_value nm = js_get(ctx, e, "name"), msg = js_get(ctx, e, "message");
      if (nm.type == JV_STR)
        sb_puts(&sb, nm.u.str->data);
      if (msg.type == JV_STR && msg.u.str->len) {
        sb_puts(&sb, ": ");
        sb_puts(&sb, msg.u.str->data);
      }
    }
  } else if (e.type == JV_STR)
    sb_puts(&sb, e.u.str->data);
  else
    sb_puts(&sb, "exception");
  if (!have_stack)
    sb_putc(&sb, '\n');
  /* A captured .stack already ends in its "    at <script>" origin line (and,
   * for a deferred callback, the "scheduled from" chain back to the arming
   * script), so only a bare throw (a string/number with no stack) needs the
   * script footer. */
  if (!have_stack && g_cur_script[0]) {
    sb_puts(&sb, "    in ");
    sb_puts(&sb, g_cur_script);
    sb_putc(&sb, '\n');
  }
  devtools_console_append(DT_ERROR, sb.b, sb.n);
  if (sb.b)
    free(sb.b);
}
static js_value report_exc_v(js_ctx *ctx)
{
  report_exc(ctx);
  return js_undef();
}

/* Devtools console REPL: parse and run one expression/statement against the
 * live page context, echoing the input and the result (or exception) to the
 * console. Worker-thread only — g_ctx is worker-owned. The AST arena is
 * retained for the page's lifetime (a typed closure/timer may outlive this
 * call), matching how page scripts keep their arenas. A page with no script
 * context gets a note. */
void js_repl_run(const char *src)
{
  js_arena *ar;
  js_node *prog;
  js_parse_error pe;
  js_value res;

  if (!src || !*src)
    return;

  {
    sbuf in;
    in.b = 0;
    in.n = 0;
    in.cap = 0; /* echo the prompt line */
    sb_puts(&in, "> ");
    sb_puts(&in, src);
    sb_putc(&in, '\n');
    devtools_console_append(DT_LOG, in.b, in.n);
    if (in.b)
      free(in.b);
  }

  if (!g_page_live || !g_ctx) {
    static const char note[] = "(no page script context — open a page that runs JS first)\n";
    devtools_console_append(DT_WARN, note, (int)sizeof note - 1);
    return;
  }

  ar = (js_arena *)malloc(sizeof *ar);
  if (!ar)
    return;
  js_arena_init(ar);
  prog = js_parse(src, ar, &pe);
  if (!prog) {
    sbuf sb;
    sb.b = 0;
    sb.n = 0;
    sb.cap = 0;
    sb_puts(&sb, "SyntaxError: ");
    sb_puts(&sb, pe.msg ? pe.msg : "parse error");
    sb_put_pos(&sb, pe.line, pe.col);
    sb_putc(&sb, '\n');
    devtools_console_append(DT_ERROR, sb.b, sb.n);
    if (sb.b)
      free(sb.b);
    js_arena_free(ar);
    free(ar);
    return;
  }
  if (!arenas_push(ar)) {
    js_arena_free(ar);
    free(ar);
    return;
  } /* keep the AST alive */

  if (js_run(g_ctx, prog, &res) != 0) {
    report_exc(g_ctx);
    return;
  }

  {
    js_value s = js_to_string(g_ctx, res); /* may itself throw (toString) */
    if (g_ctx->has_exc) {
      report_exc(g_ctx);
      return;
    }
    sbuf sb;
    sb.b = 0;
    sb.n = 0;
    sb.cap = 0;
    if (s.type == JV_STR && s.u.str)
      sb_puts(&sb, s.u.str->data);
    sb_putc(&sb, '\n');
    devtools_console_append(DT_LOG, sb.b, sb.n);
    if (sb.b)
      free(sb.b);
  }
}

/* ci substring test, for the <script type=...> filter */
static int ci_has(const char *hay, const char *needle)
{
  int nl = strlen(needle);
  for (; *hay; hay++) {
    int i = 0;
    while (i < nl && lower(hay[i]) == lower(needle[i]))
      i++;
    if (i == nl)
      return 1;
  }
  return 0;
}
static int script_is_module(struct dom_node *s)
{
  const char *type = dom_get_attr(s, "type");
  return type && ci_has(type, "module");
}
static int script_is_js(struct dom_node *s)
{
  const char *type = dom_get_attr(s, "type");
  if (!type || !*type)
    return 1;
  if (ci_has(type, "module"))
    return 0; /* handled as a module instead    */
  return ci_has(type, "javascript") || ci_has(type, "ecmascript");
}

static int script_node(struct dom_node *c)
{
  return c->type == DOM_ELEMENT && streq(c->tag, "script") && (script_is_js(c) || script_is_module(c));
}
static int count_scripts(struct dom_node *n)
{
  struct dom_node *c;
  int k = 0;
  for (c = n->first_child; c; c = c->next) {
    if (script_node(c))
      k++;
    k += count_scripts(c);
  }
  return k;
}
static void collect_scripts(struct dom_node *n, struct dom_node **out, int *count, int cap)
{
  struct dom_node *c;
  for (c = n->first_child; c && *count < cap; c = c->next) {
    if (script_node(c))
      out[(*count)++] = c; /* classic or module, inline or external */
    collect_scripts(c, out, count, cap);
  }
}
static char *sdup(const char *s)
{
  int n = strlen(s);
  char *d = (char *)malloc((unsigned)n + 1);
  if (d)
    memcpy(d, s, (unsigned)n + 1);
  return d;
}
static char *script_text(struct dom_node *s)
{
  sbuf sb;
  struct dom_node *c;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  for (c = s->first_child; c; c = c->next)
    if (c->type == DOM_TEXT && c->text)
      sb_puts(&sb, c->text);
  if (!sb.b) {
    sb.b = (char *)malloc(1);
    if (sb.b)
      sb.b[0] = 0;
  }
  return sb.b;
}

/* Execute a <script> connected to the live document via the JS DOM API
 * (createElement + appendChild/insertBefore) — the loader idiom for analytics,
 * widgets and polyfills. Classic + module; an external `src` is fetched
 * synchronously like a module. Runs at most once per node (an "@scriptRan"
 * marker on the wrapper). Scripts produced by innerHTML go through the C tree
 * API, not these bindings, so they correctly never reach here. */
static void run_inserted_script(struct dom_node *node)
{
  js_value w;
  char *code = 0;
  const char *src;
  js_arena *ar;
  js_parse_error pe;
  js_node *prog;
  int is_module;

  if (!g_page_live || !g_ctx || !node || node->type != DOM_ELEMENT)
    return;
  if (!streq(node->tag, "script"))
    return;
  if (!script_is_js(node) && !script_is_module(node))
    return; /* data block, e.g. application/json */

  w = wrap_node(node);
  if (w.type == JV_OBJ && js_has_own(w.u.obj, "@scriptRan"))
    return;
  if (w.type == JV_OBJ)
    js_def(w.u.obj, "@scriptRan", js_bool(1), 0);

  is_module = script_is_module(node);
  src = dom_get_attr(node, "src");

  if (src && *src) {
    char abs[URL_MAX];
    unsigned char *bodies[1];
    int lens[1];
    char *urls[1];
    resolve_url(g_page_url_buf, src, abs, sizeof abs);
    if (B->pg->csp) {
      char use[URL_MAX], reason[64];
      if (!csp_check_script_el(B->pg->csp, abs, g_page_url_buf, dom_get_attr(node, "nonce"), 0, use, sizeof use, reason,
                               sizeof reason)) {
        js_csp_log("a script", abs, reason);
        return;
      }
      strlcpy(abs, use, sizeof abs); /* upgraded URL */
    }
    if (!g_fetch)
      return;
    urls[0] = abs;
    bodies[0] = 0;
    lens[0] = 0;
    g_fetch(urls, 1, bodies, lens);
    if (!bodies[0] || lens[0] <= 0) {
      sbuf sb;
      sb.b = 0;
      sb.n = 0;
      sb.cap = 0;
      sb_puts(&sb, "[js] failed to load script ");
      sb_puts(&sb, abs);
      sb_putc(&sb, '\n');
      devtools_console_append(DT_ERROR, sb.b, sb.n);
      if (sb.b)
        free(sb.b);
      if (bodies[0])
        free(bodies[0]);
      return;
    }
    code = (char *)malloc((unsigned)lens[0] + 1);
    if (code) {
      memcpy(code, bodies[0], (unsigned)lens[0]);
      code[lens[0]] = 0;
    }
    free(bodies[0]);
    js_set_cur_script(abs);
  } else {
    code = script_text(node);
    js_set_cur_script("(dynamic inline script)");
  }
  if (!code)
    return;
  if (!*code) {
    free(code);
    return;
  }

  ar = (js_arena *)malloc(sizeof *ar);
  if (!ar) {
    free(code);
    return;
  }
  js_arena_init(ar);
  prog = js_parse(code, ar, &pe);
  free(code);
  if (!prog) {
    sbuf sb;
    sb.b = 0;
    sb.n = 0;
    sb.cap = 0;
    sb_puts(&sb, "[js] SyntaxError: ");
    sb_puts(&sb, pe.msg ? pe.msg : "parse error");
    sb_put_pos(&sb, pe.line, pe.col);
    sb_putc(&sb, '\n');
    devtools_console_append(DT_ERROR, sb.b, sb.n);
    if (sb.b)
      free(sb.b);
    js_arena_free(ar);
    free(ar);
    return;
  }
  if (!arenas_push(ar)) {
    js_arena_free(ar);
    free(ar);
    return;
  }
  if (is_module) {
    js_object *exports;
    gc_push_scope();
    exports = (js_object *)gc_keep(js_new_object(g_ctx, g_ctx->object_proto));
    if (js_run_module(g_ctx, prog, exports, g_page_url_buf) != 0)
      report_exc(g_ctx);
    gc_pop_scope();
  } else {
    js_value res;
    js_set(g_ctx, js_obj_value(g_document), "currentScript", wrap_node(node));
    if (js_run(g_ctx, prog, &res) != 0)
      report_exc(g_ctx);
    js_set(g_ctx, js_obj_value(g_document), "currentScript", js_null());
  }
}

/* Fire DOMContentLoaded on the document and load on the window, advancing
 * document.readyState as a real page does. Called once, after scripts run. */
static void fire_lifecycle(void)
{
  js_value ev;
  js_set(g_ctx, js_obj_value(g_document), "readyState", js_mk_cstring(g_ctx, "interactive"));
  gc_push_scope();
  ev = make_event("DOMContentLoaded", 0, 0, js_undef());
  if (ev.type == JV_OBJ)
    gc_keep(ev.u.obj);
  dispatch_from(g_document, ev);
  gc_pop_scope();
  js_set(g_ctx, js_obj_value(g_document), "readyState", js_mk_cstring(g_ctx, "complete"));
  gc_push_scope();
  ev = make_event("load", 0, 0, js_undef());
  if (ev.type == JV_OBJ)
    gc_keep(ev.u.obj);
  dispatch_from(g_ctx->global, ev);
  gc_pop_scope();
  if (g_ctx->has_exc)
    report_exc(g_ctx);
}

/* Run a page's scripts and LEAVE the engine context alive for the page's
 * lifetime (so listeners + closures survive). Reclaimed by js_page_end() on the
 * next navigation. A page with no scripts builds no context — the zero-overhead
 * path for static pages, which keeps today's navigation behaviour unchanged. */
/* Script execution ordering (HTML spec, simplified for a fully-fetched batch):
 * pass 0 runs classic scripts (inline or external, no defer) and `async`
 * externals in document order; pass 1 runs deferred scripts — external `defer`
 * plus every module — after them, still in document order. `async` overrides
 * defer/module (runs in pass 0); defer/async are ignored on inline non-module
 * scripts. */
static int script_deferred(struct dom_node *s)
{
  const char *src = dom_get_attr(s, "src");
  int external = src && *src;
  if (external && dom_get_attr(s, "async"))
    return 0; /* async: first pass, as-fetched */
  if (script_is_module(s))
    return 1; /* modules default to deferred    */
  if (external && dom_get_attr(s, "defer"))
    return 1; /* defer: external scripts only   */
  return 0;
}

void js_run_document(struct dom_node *root, const char *page_url, res_batch_fn fetch)
{
  struct dom_node **scripts;
  int *body_idx; /* script i -> index in urls[], or -1 */
  char **urls;
  unsigned char **bodies;
  int *lens;
  int ns = 0, nurls = 0, i, cap;

  if (!root)
    return;
  cap = count_scripts(root); /* exact count -> exact allocations */
  if (cap == 0)
    return; /* no JS: skip the engine entirely */

  scripts = (struct dom_node **)malloc((unsigned)cap * sizeof *scripts);
  body_idx = (int *)malloc((unsigned)cap * sizeof *body_idx);
  urls = (char **)malloc((unsigned)cap * sizeof *urls);
  bodies = (unsigned char **)malloc((unsigned)cap * sizeof *bodies);
  lens = (int *)malloc((unsigned)cap * sizeof *lens);
  if (!scripts || !body_idx || !urls || !bodies || !lens) {
    free(scripts);
    free(body_idx);
    free(urls);
    free(bodies);
    free(lens);
    return;
  }
  collect_scripts(root, scripts, &ns, cap);

  /* Resolve every <script src> to an absolute URL (document order) and batch-
   * fetch them up front over the same path stylesheets use. */
  for (i = 0; i < ns; i++) {
    const char *src = dom_get_attr(scripts[i], "src");
    body_idx[i] = -1;
    if (src && *src) {
      char abs[URL_MAX];
      resolve_url(page_url, src, abs, sizeof abs);
      if (B->pg->csp) { /* CSP script-src for external scripts */
        char use[URL_MAX], reason[64];
        if (!csp_check_script_el(B->pg->csp, abs, page_url, dom_get_attr(scripts[i], "nonce"), 1, use, sizeof use,
                                 reason, sizeof reason)) {
          js_csp_log("the script", abs, reason);
          body_idx[i] = -2; /* blocked: skip fetch + execution */
          continue;
        }
        strlcpy(abs, use, sizeof abs); /* upgraded URL */
      }
      urls[nurls] = sdup(abs);
      body_idx[i] = nurls;
      nurls++;
    }
  }
  for (i = 0; i < nurls; i++) {
    bodies[i] = 0;
    lens[i] = 0;
  }
  if (fetch && nurls)
    fetch(urls, nurls, bodies, lens);

  g_doc_root = root;
  g_ctx = js_ctx_new(js_print);
  if (!g_ctx) {
    for (i = 0; i < nurls; i++) {
      if (urls[i])
        free(urls[i]);
      if (bodies[i])
        free(bodies[i]);
    }
    free(scripts);
    free(body_idx);
    free(urls);
    free(bodies);
    free(lens);
    g_doc_root = 0;
    return;
  }
  g_ctx->now_ms = now_ms;
  js_set_stack_limit(g_ctx, browser_js_stack_floor()); /* bound recursion to the worker stack */
  js_set_script(g_ctx, g_cur_script);                  /* origin frame for captured stacks */
  g_has_listeners = 0;
  setup_dom(g_ctx, page_url);
  g_fetch = fetch; /* the loader uses it for modules */
  strlcpy(g_page_url_buf, page_url ? page_url : "", sizeof g_page_url_buf);
  g_page_live = 1;

  /* CSP: without 'unsafe-eval', neutralise the dynamic-code globals so the
   * page's own scripts can't generate code from strings. */
  if (B->pg->csp && !csp_allows_eval(B->pg->csp)) {
    js_def_native(g_ctx, g_ctx->global, "eval", dyncode_blocked, 1);
    js_def_native(g_ctx, g_ctx->global, "Function", dyncode_blocked, 1);
  }

  int inline_n = 0;
  /* Two ordered passes: pass 0 = classic + async (document order), pass 1 =
   * deferred scripts + modules (document order). See script_deferred(). */
  for (int pass = 0; pass < 2; pass++)
    for (i = 0; i < ns; i++) {
      char *code;
      js_parse_error pe;
      js_node *prog;
      js_arena *ar;
      if (body_idx[i] == -2)
        continue; /* CSP-blocked external (already logged) */
      if (script_deferred(scripts[i]) != pass)
        continue; /* run in its ordering pass */
      /* Label this script for error attribution: its src URL, or the page URL
       * plus an inline-script ordinal. */
      if (body_idx[i] >= 0 && urls[body_idx[i]]) {
        js_set_cur_script(urls[body_idx[i]]);
      } else {
        int k = 0;
        g_cur_script[0] = '\0';
        sapp(g_cur_script, &k, sizeof g_cur_script, page_url ? page_url : "(inline)");
        sapp(g_cur_script, &k, sizeof g_cur_script, " (inline script #");
        uapp(g_cur_script, &k, sizeof g_cur_script, (unsigned)(++inline_n));
        sapp(g_cur_script, &k, sizeof g_cur_script, ")");
      }
      if (body_idx[i] >= 0) { /* external script: use the fetched body */
        int bi = body_idx[i];
        if (!bodies[bi] || lens[bi] <= 0) {
          sbuf sb;
          sb.b = 0;
          sb.n = 0;
          sb.cap = 0;
          sb_puts(&sb, "[js] failed to load script ");
          sb_puts(&sb, urls[bi] ? urls[bi] : "");
          sb_putc(&sb, '\n');
          devtools_console_append(DT_ERROR, sb.b, sb.n);
          if (sb.b)
            free(sb.b);
          continue;
        }
        code = (char *)malloc((unsigned)lens[bi] + 1);
        if (!code)
          continue;
        memcpy(code, bodies[bi], (unsigned)lens[bi]);
        code[lens[bi]] = 0;
      } else {
        code = script_text(scripts[i]);
      }
      if (!code)
        continue;
      if (!*code) {
        free(code);
        continue;
      }
      if (body_idx[i] < 0 && B->pg->csp) { /* CSP script-src for inline scripts */
        char reason[64];
        if (!csp_check_inline(B->pg->csp, CSP_SCRIPT, dom_get_attr(scripts[i], "nonce"), (const unsigned char *)code,
                              (int)strlen(code), reason, sizeof reason)) {
          js_csp_log("an inline script", g_cur_script, reason);
          free(code);
          continue;
        }
      }
      ar = (js_arena *)malloc(sizeof *ar);
      if (!ar) {
        free(code);
        continue;
      }
      js_arena_init(ar);
      prog = js_parse(code, ar, &pe);
      free(code);
      if (!prog) {
        sbuf sb;
        sb.b = 0;
        sb.n = 0;
        sb.cap = 0;
        sb_puts(&sb, "[js] SyntaxError: ");
        sb_puts(&sb, pe.msg ? pe.msg : "parse error");
        if (g_cur_script[0]) {
          sb_puts(&sb, "\n    in ");
          sb_puts(&sb, g_cur_script);
          sb_put_pos(&sb, pe.line, pe.col);
        }
        sb_putc(&sb, '\n');
        devtools_console_append(DT_ERROR, sb.b, sb.n);
        if (sb.b)
          free(sb.b);
        js_arena_free(ar);
        free(ar);
        continue;
      }
      if (!arenas_push(ar)) {
        js_arena_free(ar);
        free(ar);
        continue;
      } /* AST kept alive for the page */
      if (script_is_module(scripts[i])) { /* <script type=module> */
        js_object *exports;
        const char *murl = (body_idx[i] >= 0) ? urls[body_idx[i]] : page_url;
        gc_push_scope();
        exports = (js_object *)gc_keep(js_new_object(g_ctx, g_ctx->object_proto));
        if (body_idx[i] >= 0)
          js_def(g_module_cache, urls[body_idx[i]], js_obj_value(exports), 0);
        if (js_run_module(g_ctx, prog, exports, murl) != 0)
          report_exc(g_ctx);
        gc_pop_scope();
      } else {
        js_value res;
        /* document.currentScript points at the executing classic script (null
         * for modules + outside execution), as scripts like analytics expect.
         */
        js_set(g_ctx, js_obj_value(g_document), "currentScript", wrap_node(scripts[i]));
        if (js_run(g_ctx, prog, &res) != 0)
          report_exc(g_ctx);
        js_set(g_ctx, js_obj_value(g_document), "currentScript", js_null());
      }
    }

  for (i = 0; i < nurls; i++) {
    if (urls[i])
      free(urls[i]);
    if (bodies[i])
      free(bodies[i]);
  }
  free(scripts);
  free(body_idx);
  free(urls);
  free(bodies);
  free(lens);

  fire_lifecycle();
}

/* Reclaim the page's JS context: drop roots, free the heap, free the script
 * ASTs. Idempotent — safe to call when no page context is live (static pages).
 * The caller frees the DOM AFTER this returns (wrappers reference nodes via
 * host). */
void js_page_end(void)
{
  int i;
  if (!g_page_live)
    return;
  dom_on_free_wrapper = 0;     /* DOM teardown shouldn't call back */
  js_cast_page_end();          /* stop casting before the ctx dies */
  if (g_websockets && g_ctx) { /* close + free live sockets first */
    unsigned n = js_array_len(g_websockets), k;
    for (k = 0; k < n; k++) {
      js_value wv = js_get_index(g_ctx, js_obj_value(g_websockets), k);
      if (wv.type == JV_OBJ && wv.u.obj->host) {
        ws_free((ws_conn *)wv.u.obj->host);
        wv.u.obj->host = 0;
      }
    }
  }
  gc_remove_root((void **)&g_node_proto);
  gc_remove_root((void **)&g_element_proto);
  gc_remove_root((void **)&g_style_proto);
  gc_remove_root((void **)&g_classlist_proto);
  gc_remove_root((void **)&g_url_proto);
  gc_remove_root((void **)&g_sp_proto);
  gc_remove_root((void **)&g_formdata_proto);
  gc_remove_root((void **)&g_event_proto);
  gc_remove_root((void **)&g_html_proto);
  gc_remove_root((void **)&g_media_proto);
  gc_remove_root((void **)&g_custom_elements);
  gc_remove_root((void **)&g_module_cache);
  gc_remove_root((void **)&g_module_src);
  gc_remove_root((void **)&g_document_proto);
  gc_remove_root((void **)&g_response_proto);
  gc_remove_root((void **)&g_ws_proto);
  gc_remove_root((void **)&g_xhr_proto);
  gc_remove_root((void **)&g_blob_proto);
  gc_remove_root((void **)&g_remote_proto);
  gc_remove_root((void **)&g_remotes);
  gc_remove_root((void **)&g_clipitem_proto);
  gc_remove_root((void **)&g_abortsignal_proto);
  gc_remove_root((void **)&g_abortcontroller_proto);
  gc_remove_root((void **)&g_rtc_proto);
  gc_remove_root((void **)&g_document);
  gc_remove_root((void **)&g_live_wrappers);
  gc_remove_root((void **)&g_websockets);
  gc_remove_root((void **)&g_bchannel_proto);
  gc_remove_root((void **)&g_bchannels);
  gc_remove_root((void **)&g_notification_proto);
  gc_remove_root((void **)&g_notifications);
  js_notification_teardown();
  gc_remove_root((void **)&g_timers);
  js_idb_teardown();
  js_webapi_teardown();
  {
    unsigned k;
    for (k = 0; k < sizeof g_html_ifaces / sizeof g_html_ifaces[0]; k++) {
      gc_remove_root((void **)&g_html_ifaces[k].proto);
      g_html_ifaces[k].proto = 0;
    }
  }
  g_timers = 0;
  if (g_tmeta) {
    int i;
    for (i = 0; i < g_tcap; i++)
      if (g_tmeta[i].async)
        free(g_tmeta[i].async);
    free(g_tmeta);
    g_tmeta = 0;
  }
  g_tcap = 0;
  g_ntimers = 0;
  g_media_proto = 0;
  g_node_proto = g_clipitem_proto = 0;
  g_element_proto = g_style_proto = g_classlist_proto = g_url_proto = g_sp_proto = g_formdata_proto = 0;
  g_event_proto = g_html_proto = g_custom_elements = g_document = g_live_wrappers = 0;
  g_module_cache = g_document_proto = g_response_proto = 0;
  g_module_src = 0;
  g_ws_proto = g_websockets = 0;
  g_bchannel_proto = g_bchannels = 0;
  g_xhr_proto = 0;
  g_blob_proto = 0;
  g_abortsignal_proto = g_abortcontroller_proto = 0;
  g_rtc_proto = 0;
  sw_teardown(); /* SW roots/objects (C store persists) */
  g_fetch = 0;
  if (g_ctx) {
    js_ctx_free(g_ctx);
    g_ctx = 0;
  } /* frees the whole JS heap         */
  for (i = 0; i < g_narenas; i++) {
    js_arena_free(g_arenas[i]);
    free(g_arenas[i]);
  }
  if (g_arenas) {
    free(g_arenas);
    g_arenas = 0;
  }
  g_narenas = 0;
  g_arenas_cap = 0;
  for (i = 0; i < g_nparsed; i++)
    dom_free(g_parsed[i]); /* DOMParser trees */
  g_nparsed = 0;
  g_has_listeners = 0;
  g_page_live = 0;
  g_doc_root = 0;
}
