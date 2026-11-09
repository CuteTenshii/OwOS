/* Browser bindings: document/window layer over the DOM for the OwOS JS engine (user/lib/js).
 * Scripts run before layout. DOM nodes are wrapper objects whose `host` slot is the dom_node. */

#include "bindings.h"
#include "canvasjs.h"
#include "classlist.h"
#include "dom_query.h"
#include "features/clienthints/clienthints.h"
#include "features/jar/jar.h"
#include "features/mediasession/mediasession.h"
#include "features/notification/notification.h"
#include "features/storage/storage.h"
#include "websocket.h"

js_ctx *g_ctx;
js_object *g_node_proto; /* Node.prototype (ancestor of element/document) */
js_object *g_element_proto;
js_object *g_style_proto;
js_object *g_rellist_proto;   /* DOMTokenList for element.relList, chains to g_classlist_proto */
js_object *g_classlist_proto; /* DOMTokenList.prototype (element.classList) */
js_object *g_url_proto;
js_object *g_sp_proto;
js_object *g_formdata_proto; /* FormData.prototype (shares URLSearchParams' @k/@v) */
js_object *g_event_proto;
js_object *g_html_proto;  /* HTMLElement.prototype (chains to element) */
js_object *g_media_proto; /* HTMLMediaElement.prototype (audio/video) */

/* Standard HTMLxxxElement interfaces: the interface name, the space-separated tag list it wraps */
struct html_iface g_html_ifaces[] = {
    /* `media` = 1 chains to HTMLMediaElement.prototype instead of HTMLElement's */
    {"HTMLScriptElement", "script", false, 0},
    {"HTMLAnchorElement", "a", false, 0},
    {"HTMLImageElement", "img", false, 0},
    {"HTMLInputElement", "input", false, 0},
    {"HTMLButtonElement", "button", false, 0},
    {"HTMLFormElement", "form", false, 0},
    {"HTMLDivElement", "div", false, 0},
    {"HTMLSpanElement", "span", false, 0},
    {"HTMLParagraphElement", "p", false, 0},
    {"HTMLHeadingElement", "h1 h2 h3 h4 h5 h6", false, 0},
    {"HTMLUListElement", "ul", false, 0},
    {"HTMLOListElement", "ol", false, 0},
    {"HTMLLIElement", "li", false, 0},
    {"HTMLLinkElement", "link", false, 0},
    {"HTMLStyleElement", "style", false, 0},
    {"HTMLMetaElement", "meta", false, 0},
    {"HTMLTitleElement", "title", false, 0},
    {"HTMLHeadElement", "head", false, 0},
    {"HTMLBodyElement", "body", false, 0},
    {"HTMLHtmlElement", "html", false, 0},
    {"HTMLIFrameElement", "iframe", false, 0},
    {"HTMLCanvasElement", "canvas", false, 0},
    {"HTMLSelectElement", "select", false, 0},
    {"HTMLOptionElement", "option", false, 0},
    {"HTMLTextAreaElement", "textarea", false, 0},
    {"HTMLLabelElement", "label", false, 0},
    {"HTMLTableElement", "table", false, 0},
    {"HTMLTableRowElement", "tr", false, 0},
    {"HTMLTableCellElement", "td th", false, 0},
    {"HTMLTableSectionElement", "thead tbody tfoot", false, 0},
    {"HTMLPreElement", "pre", false, 0},
    {"HTMLBRElement", "br", false, 0},
    {"HTMLHRElement", "hr", false, 0},
    {"HTMLSourceElement", "source", false, 0},
    {"HTMLTemplateElement", "template", false, 0},
    {"HTMLSlotElement", "slot", false, 0},
    {"HTMLDialogElement", "dialog", false, 0},
    {"HTMLDetailsElement", "details", false, 0},
    {"HTMLPictureElement", "picture", false, 0},
    {"HTMLDataListElement", "datalist", false, 0},
    {"HTMLProgressElement", "progress", false, 0},
    {"HTMLObjectElement", "object", false, 0},
    {"HTMLVideoElement", "video", true, 0},
    {"HTMLAudioElement", "audio", true, 0},
};
const unsigned g_nhtml_ifaces = sizeof g_html_ifaces / sizeof g_html_ifaces[0];
js_object *g_custom_elements;       /* registry: tag name -> class constructor */
js_object *g_module_cache;          /* ES module registry: abs URL -> namespace */
js_object *g_module_src;            /* prefetched module sources: abs URL -> text */
js_object *g_importmap;             /* import map: bare specifier -> URL string */
js_object *g_document_proto;        /* shared by the live doc + DOMParser docs */
js_object *g_response_proto;        /* fetch() Response.prototype */
js_object *g_ws_proto;              /* WebSocket.prototype */
js_object *g_xhr_proto;             /* XMLHttpRequest.prototype */
js_object *g_blob_proto;            /* Blob.prototype */
js_object *g_clipitem_proto;        /* ClipboardItem.prototype */
js_object *g_remote_proto;          /* RemotePlayback.prototype (media.remote) */
js_object *g_remotes;               /* rooted array of live RemotePlayback wrappers */
js_object *g_websockets;            /* rooted array of live WebSocket wrappers */
js_object *g_bchannel_proto;        /* BroadcastChannel.prototype */
js_object *g_bchannels;             /* rooted array of open BroadcastChannel wrappers */
js_object *g_notification_proto;    /* Notification.prototype */
js_object *g_notifications;         /* rooted array of wrappers pending show/close */
js_object *g_abortsignal_proto;     /* AbortSignal.prototype */
js_object *g_abortcontroller_proto; /* AbortController.prototype */
js_object *g_rtc_proto;             /* RTCPeerConnection.prototype (inert stub) */
js_object *g_document;
struct dom_node *g_doc_root;
struct dom_node **g_parsed; /* DOMParser trees, freed at page teardown */
int g_nparsed, g_parsed_cap;
res_batch_fn g_fetch;         /* the batch fetch used to pull modules */
char g_page_url_buf[URL_MAX]; /* the page URL (module-resolution referrer) */

/* The JS context now lives for the whole page (not just the initial script run) */
js_object *g_live_wrappers; /* rooted array pinning every node wrapper and token list */
bool g_has_listeners;       /* any addEventListener fired this page */
int g_page_live;            /* a page context is currently alive */
js_arena **g_arenas;        /* per-script AST arenas (closures need them); grows */
int g_narenas, g_arenas_cap;

/* Timer queue (setTimeout/setInterval/requestAnimationFrame) */
js_object *g_timers; /* rooted: slot -> [callback, ...args] or null */
struct timer_meta *g_tmeta;
int g_tcap;    /* allocated slots in g_tmeta */
int g_ntimers; /* active count (fast empty-check for the pump) */

/* ------------------------------------------------------- small buffer ----- */

static bool sb_need(sbuf *s, int extra)
{
  if (s->n + extra + 1 > s->cap) {
    int c = s->cap ? s->cap * 2 : 128;
    while (c < s->n + extra + 1)
      c *= 2;
    char *b = (char *)realloc(s->b, (unsigned)c);
    if (!b) {
      s->failed = 1;
      return false;
    }
    s->b = b;
    s->cap = c;
  }
  return true;
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

static bool node_is_frag(const struct dom_node *n) { return n->type == DOM_ELEMENT && streq(n->tag, "#frag"); }
bool node_is_comment(const struct dom_node *n) { return n->type == DOM_ELEMENT && streq(n->tag, "#comment"); }

struct dom_node *node_of(js_value v)
{
  if (v.type != JV_OBJ || v.u.obj->kind == OBJ_HOST)
    return 0;
  return (struct dom_node *)v.u.obj->host;
}
/* A DOM node is exposed as a wrapper object whose `host` slot points at the node */
/* whole-word match of `tag` within the space-separated `list` */
bool tag_in_list(const char *list, const char *tag)
{
  int tl = strlen(tag);
  const char *p = list;
  while (*p) {
    const char *s = p;
    while (*p && *p != ' ')
      p++;
    if (p - s == tl && memcmp_js(s, tag, tl) == 0)
      return true;
    while (*p == ' ')
      p++;
  }
  return false;
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
  if (n->host && g_shadowroot_proto)
    proto = g_shadowroot_proto;
  else if (node_is_frag(n) && g_fragment_proto)
    proto = g_fragment_proto;
  else if (n->type == DOM_ELEMENT) {
    js_object *ip = iface_proto_for_tag(n->tag); /* HTMLScriptElement etc. */
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

/* dom.c calls this (via the dom_on_free_wrapper hook) when a node that owns a wrapper is freed */
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

/* lowercase copy into a fixed buffer */
void lower_into(char *dst, int cap, const char *src)
{
  int i = 0;
  for (; src[i] && i < cap - 1; i++)
    dst[i] = lower(src[i]);
  dst[i] = '\0';
}

/* ------------------------------------------------------- text content ----- */

#define SERIALIZE_DEPTH_MAX 256
static void collect_text(struct dom_node *n, sbuf *sb, int depth)
{
  struct dom_node *c;
  if (n->type == DOM_TEXT) {
    if (n->text)
      sb_puts(sb, n->text);
    return;
  }
  if (depth >= SERIALIZE_DEPTH_MAX || node_is_comment(n))
    return;
  for (c = n->first_child; c; c = c->next)
    collect_text(c, sb, depth + 1);
}
static bool tag_is_void(const char *tag)
{
  static const char *const v[] = {"area", "base", "br",    "col",    "embed", "hr",  "img", "input",
                                  "link", "meta", "param", "source", "track", "wbr", 0};
  for (int i = 0; v[i]; i++)
    if (streq(tag, v[i]))
      return true;
  return false;
}
void serialize_node(struct dom_node *c, sbuf *sb, int depth)
{
  struct dom_node *k;
  bool frag = node_is_frag(c);
  if (c->type == DOM_TEXT) {
    if (c->text)
      sb_puts(sb, c->text);
    return;
  }
  if (node_is_comment(c)) {
    const char *d = dom_get_attr(c, "data");
    sb_puts(sb, "<!--");
    sb_puts(sb, d ? d : "");
    sb_puts(sb, "-->");
    return;
  }
  if (!frag) {
    struct dom_attr *a;
    sb_putc(sb, '<');
    sb_puts(sb, dom_qname(c));
    for (a = c->attrs; a; a = a->next) {
      sb_putc(sb, ' ');
      sb_puts(sb, dom_attr_qname(c, a));
      sb_puts(sb, "=\"");
      sb_puts(sb, a->value ? a->value : "");
      sb_putc(sb, '"');
    }
    sb_putc(sb, '>');
    if (tag_is_void(c->tag))
      return;
  }
  if (depth < SERIALIZE_DEPTH_MAX)
    for (k = c->first_child; k; k = k->next)
      serialize_node(k, sb, depth + 1);
  if (!frag) {
    sb_puts(sb, "</");
    sb_puts(sb, dom_qname(c));
    sb_putc(sb, '>');
  }
}
static void serialize(struct dom_node *n, sbuf *sb)
{
  struct dom_node *c;
  for (c = n->first_child; c; c = c->next)
    serialize_node(c, sb, 0);
}

static js_value text_content_value(js_ctx *ctx, struct dom_node *n)
{
  sbuf sb;
  js_value r;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  collect_text(n, &sb, 0);
  r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
void set_text_content(struct dom_node *n, const char *text)
{
  for (struct dom_node *k = n->first_child; k; k = k->next)
    ce_disconnect_tree(k);
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
  for (c = el->first_child; c; c = c->next)
    ce_disconnect_tree(c);
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
  ce_upgrade_children(el);
}

/* ------------------------------------------------------- element natives -- */

static js_value el_tagName(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *name;
  js_value r;
  (void)args;
  (void)argc;
  if (!n)
    return js_undef();
  name = dom_qname(n);
  if (n->tag[0] != '#' && !strcmp(dom_ns(n), DOM_NS_HTML)) {
    char *up = strdup(name);
    if (!up)
      return js_undef();
    for (char *c = up; *c; c++)
      if (*c >= 'a' && *c <= 'z')
        *c = (char)(*c - 32);
    r = js_mk_cstring(ctx, up);
    free(up);
    return r;
  }
  return js_mk_cstring(ctx, name);
}
static js_value el_nodeName(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n && n->type == DOM_TEXT)
    return js_mk_cstring(ctx, "#text");
  if (n && (node_is_frag(n) || n->host))
    return js_mk_cstring(ctx, "#document-fragment");
  if (n && node_is_comment(n))
    return js_mk_cstring(ctx, "#comment");
  return el_tagName(ctx, t, args, argc);
}
static js_value el_nodeType(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  if (!n || n->type == DOM_TEXT)
    return js_number(n ? 3 : 1);
  return js_number(node_is_frag(n) || n->host ? 11 : node_is_comment(n) ? 8 : 1);
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
/* .nonce reflects the content attribute */
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

/* ------------------------------------------------------- classList --------- */
/* DOMTokenList over class="" (classList) or rel="" (relList) */

/* the element behind a token list: its host is the element wrapper, which goes NULL once the node is freed */
static struct dom_node *cl_node(js_value t)
{
  js_object *w = t.type == JV_OBJ ? t.u.obj->host : 0;
  return w ? w->host : 0;
}

/* the attribute a token list object reflects */
static const char *cl_attr(js_value t)
{
  return t.type == JV_OBJ && t.u.obj->proto == g_rellist_proto ? "rel" : "class";
}

/* the node's current token string, or "" (never NULL) */
static const char *cl_class_of(js_value t)
{
  struct dom_node *n = cl_node(t);
  const char *v = n ? dom_get_attr(n, cl_attr(t)) : 0;
  return v ? v : "";
}

/* rebuild `n`'s `attr` as the current tokens (deduped) with `tok` appended */
static void cl_write_add(struct dom_node *n, const char *attr, const char *tok, int tl)
{
  char *classes = strdup(dom_get_attr(n, attr) ? dom_get_attr(n, attr) : "");
  classlist_add(&classes, tok, tl);
  dom_set_attr(n, attr, classes);
  free(classes);
}

/* rebuild `n`'s `attr` as the current tokens minus every copy of `tok` */
static void cl_write_remove(struct dom_node *n, const char *attr, const char *tok, int tl)
{
  char *classes = strdup(dom_get_attr(n, attr) ? dom_get_attr(n, attr) : "");
  classlist_remove(&classes, tok, tl);
  dom_set_attr(n, attr, classes);
  free(classes);
}

/* throws SyntaxError for an empty token, InvalidCharacterError for one with whitespace; true if it threw */
static bool cl_token_invalid(js_ctx *ctx, const char *tok)
{
  if (!*tok) {
    dom_throw(ctx, "SyntaxError", "the token must not be empty");
    return true;
  }
  for (; *tok; tok++)
    if (*tok == ' ' || *tok == '\t' || *tok == '\n' || *tok == '\r' || *tok == '\f') {
      dom_throw(ctx, "InvalidCharacterError", "the token must not contain whitespace");
      return true;
    }
  return false;
}

static const char *cl_tok(js_ctx *ctx, js_value v)
{
  js_value s = js_to_string(ctx, v);
  return s.type == JV_STR ? s.u.str->data : "";
}

/* add (remove=0) or remove every argument; all are validated before any is applied */
static js_value cl_add_remove(js_ctx *ctx, js_value t, js_value *args, int argc, bool remove)
{
  struct dom_node *n = cl_node(t);
  int i;
  if (!n)
    return js_undef();
  for (i = 0; i < argc; i++)
    if (cl_token_invalid(ctx, cl_tok(ctx, args[i])))
      return js_undef();
  for (i = 0; i < argc; i++) {
    const char *tok = cl_tok(ctx, args[i]);
    if (remove)
      cl_write_remove(n, cl_attr(t), tok, (int)strlen(tok));
    else
      cl_write_add(n, cl_attr(t), tok, (int)strlen(tok));
  }
  return js_undef();
}
static js_value cl_add(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return cl_add_remove(ctx, t, args, argc, false);
}
static js_value cl_remove(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return cl_add_remove(ctx, t, args, argc, true);
}

/* toggle(token[, force]): with `force` given, force==true adds / false removes */
static js_value cl_toggle(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = cl_node(t);
  const char *tok = cl_tok(ctx, A(0));
  int tl = (int)strlen(tok), present;
  if (!n || cl_token_invalid(ctx, tok))
    return js_bool(0);
  present = classlist_has(cl_class_of(t), tok, tl);
  if (argc >= 2) {
    bool force = js_to_boolean(args[1]);
    if (force && !present)
      cl_write_add(n, cl_attr(t), tok, tl);
    else if (!force && present)
      cl_write_remove(n, cl_attr(t), tok, tl);
    return js_bool(force);
  }
  if (present) {
    cl_write_remove(n, cl_attr(t), tok, tl);
    return js_bool(0);
  }
  cl_write_add(n, cl_attr(t), tok, tl);
  return js_bool(1);
}

/* replace(old, new): swap the first `old` for `new` (dropping duplicates of either) */
static js_value cl_replace(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = cl_node(t);
  const char *oldt = cl_tok(ctx, A(0)), *newt;
  (void)argc;
  if (!n || cl_token_invalid(ctx, oldt))
    return js_bool(0);
  newt = cl_tok(ctx, A(1));
  if (cl_token_invalid(ctx, newt))
    return js_bool(0);
  {
    const char *attr = cl_attr(t);
    char *classes = strdup(dom_get_attr(n, attr) ? dom_get_attr(n, attr) : "");
    int replaced = classlist_replace(&classes, oldt, newt);
    dom_set_attr(n, attr, classes);
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

/* only relList defines supported tokens (the rels the browser acts on) */
static js_value cl_supports(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  static const char *const known[] = {"stylesheet", "icon", "preload", "preconnect"};
  const char *tok = cl_tok(ctx, A(0));
  int i;
  (void)argc;
  (void)args;
  if (t.type != JV_OBJ || t.u.obj->proto != g_rellist_proto)
    return js_throw_type(ctx, "this token list has no supported tokens");
  for (i = 0; i < (int)(sizeof known / sizeof *known); i++)
    if (streq_ci(tok, known[i]))
      return js_bool(1);
  return js_bool(0);
}

static js_value cl_item(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int idx = js_to_int_sat(ctx, A(0)), len;
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
  struct dom_node *n = cl_node(t);
  if (n)
    dom_set_attr(n, cl_attr(t), arg_str(ctx, A(0)));
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

/* values/keys/entries + Symbol.iterator */
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

static void index_key(char *key, int i)
{
  char tmp[12];
  int m = 0, k = 0;
  do {
    tmp[m++] = (char)('0' + i % 10);
    i /= 10;
  } while (i);
  while (m)
    key[k++] = tmp[--m];
  key[k] = '\0';
}

/* The per-element DOMTokenList is created once and cached on the node, so el.classList === el.classList.
 * Numeric index properties (classList[0]) are re-synced on every access, the engine has no index-getter hook. */
static js_value el_token_list(js_ctx *ctx, js_value t, js_object *proto, const char *attr)
{
  struct dom_node *n = node_of(t);
  void **slot = n ? &n->tokenlist[proto == g_rellist_proto] : 0;
  const char *cur, *b;
  js_object *o;
  char key[12];
  int i, len;
  gc_push_scope();
  if (slot && *slot)
    o = *slot;
  else {
    o = (js_object *)gc_keep(js_new_object(ctx, proto));
    o->host = t.u.obj;
    if (slot) {
      *slot = o;
      if (g_live_wrappers)
        js_array_push(ctx, g_live_wrappers, js_obj_value(o));
    }
  }
  cur = n ? dom_get_attr(n, attr) : 0;
  if (!cur)
    cur = "";
  for (i = 0; (b = classlist_nth(cur, i, &len)); i++) {
    index_key(key, i);
    js_def(o, key, js_mk_string(ctx, b, len), P_ENUMERABLE | P_CONFIGURABLE);
  }
  for (;; i++) {
    index_key(key, i);
    if (js_get(ctx, js_obj_value(o), key).type == JV_UNDEF)
      break;
    js_delete(o, key);
  }
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value el_get_classlist(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return el_token_list(ctx, t, g_classlist_proto, "class");
}
/* a, area, link and form carry rel/relList */
static bool has_rel(const struct dom_node *n)
{
  return n && (!strcmp(n->tag, "a") || !strcmp(n->tag, "area") || !strcmp(n->tag, "link") || !strcmp(n->tag, "form"));
}
static js_value el_get_rel(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *v;
  (void)args;
  (void)argc;
  if (!has_rel(n))
    return js_undef();
  v = dom_get_attr(n, "rel");
  return js_mk_cstring(ctx, v ? v : "");
}
static js_value el_set_rel(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (has_rel(n))
    dom_set_attr(n, "rel", arg_str(ctx, A(0)));
  return js_undef();
}
static js_value el_set_rellist(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (has_rel(n))
    dom_set_attr(n, "rel", arg_str(ctx, A(0)));
  return js_undef();
}
static js_value el_get_rellist(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)args;
  (void)argc;
  if (!has_rel(n))
    return js_undef();
  return el_token_list(ctx, t, g_rellist_proto, "rel");
}
/* .src / .href reflect the attribute resolved to an absolute URL */
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
  if (n && (n->type == DOM_TEXT || node_is_comment(n)))
    el_set_node_value(ctx, t, args, argc);
  else if (n)
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
js_value el_get_attr(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *v = n ? dom_get_attr(n, arg_str(ctx, A(0))) : 0;
  return v ? js_mk_cstring(ctx, v) : js_null();
}
js_value el_set_attr(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n) {
    char *name = strdup(arg_str(ctx, A(0))), *oldv = 0, *val = 0;
    const char *ov = name ? dom_get_attr(n, name) : 0;
    if (name && (!ov || (oldv = strdup(ov)))) {
      val = strdup(arg_str(ctx, A(1)));
      if (val) {
        dom_set_attr(n, name, val);
        ce_attr_changed(n, name, oldv, val); /* observedAttributes hook */
      }
    }
    free(name);
    free(oldv);
    free(val);
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
/* A script inserted into the live document by these DOM bindings */
bool node_connected(struct dom_node *n)
{
  for (; n; n = (struct dom_node *)dom_flat_parent(n))
    if (n == g_doc_root)
      return true;
  return false;
}
static void connect_inserted(struct dom_node *c)
{
  js_value cls;
  if (c->type != DOM_ELEMENT)
    return;
  cls = ce_lookup(c->tag);
  if (cls.type == JV_OBJ)
    ce_upgrade_node(c, cls);
  ce_connect(c);
  ce_upgrade_children(c);
  if (node_connected(c))
    run_inserted_script(c);
}
/* Detach `c` from `parent` and fire disconnectedCallback across its subtree */
void remove_node(struct dom_node *parent, struct dom_node *c)
{
  shadow_node_removing(c);
  dom_remove_child(parent, c);
  ce_disconnect_tree(c);
}
/* Insert `c` (or a fragment's children) into `parent` before `ref`; false on a cycle. */
bool insert_node(js_ctx *ctx, struct dom_node *parent, struct dom_node *c, struct dom_node *ref)
{
  if (dom_is_ancestor(c, parent) || parent->type == DOM_TEXT) {
    dom_throw(ctx, "HierarchyRequestError", "the new child contains the parent");
    return false;
  }
  if (node_is_frag(c)) {
    struct dom_node *k;
    while ((k = c->first_child)) {
      dom_insert_before(parent, k, ref);
      connect_inserted(k);
      shadow_node_inserted(k);
    }
    return true;
  }
  dom_insert_before(parent, c, ref);
  connect_inserted(c);
  shadow_node_inserted(c);
  return true;
}
static js_value el_append(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c = node_of(A(0));
  if (n && c)
    insert_node(ctx, n, c, 0);
  return A(0);
}
static js_value el_remove_child(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c = node_of(A(0));
  (void)ctx;
  if (n && c)
    remove_node(n, c);
  return A(0);
}
static js_value el_insert_before(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c = node_of(A(0)), *ref = node_of(A(1));
  if (n && c)
    insert_node(ctx, n, c, ref); /* null/absent ref => append */
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
  bool elems_only = true;
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
/* --- window.alert / confirm / prompt -------------------------------------- */
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

/* --- per-origin permissions (Notification / Geolocation / Camera / Microphone) */
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

/* The remembered/policy state for a feature WITHOUT prompting */
int perm_state_now(int feature)
{
  char origin[URL_MAX], host[URL_HOST_MAX];
  if (!page_origin(origin, sizeof origin, host, sizeof host))
    return PERM_ASK;
  if (B->pg && perm_policy_blocks(B->pg->perm_policy, feature))
    return PERM_DENIED;
  return perm_lookup(origin, feature);
}

/* Resolve a permission, prompting the user if there is no prior decision */
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

/* --- Content-Security-Policy hooks ----------------------------------------- */
void js_csp_log(const char *what, const char *url, const char *reason)
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

/* Stub installed over the eval/Function globals when 'unsafe-eval' is absent */
js_value dyncode_blocked(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)a;
  (void)n;
  return js_throw_type(ctx, "code generation from strings blocked by Content-Security-Policy");
}

/* ------------------------------------------------------- geometry natives - */
int node_box(js_value t, int *x, int *y, int *w, int *h)
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

/* Wrap a DOM tree root as a Document object (host = root, root->wrapper = doc) */
js_value wrap_document(js_ctx *ctx, struct dom_node *root)
{
  js_object *doc;
  js_value dv;
  if (!root)
    return js_null();
  if (g_nparsed == g_parsed_cap) {
    int nc = g_parsed_cap ? g_parsed_cap * 2 : 16;
    struct dom_node **np = (struct dom_node **)realloc(g_parsed, (unsigned)nc * sizeof *np);
    if (!np) {
      dom_free(root);
      return js_null();
    }
    g_parsed = np;
    g_parsed_cap = nc;
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

/* A script-created or src-less <iframe> exposes a live, initially-empty document */
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

/* contentWindow: a thin window over the same content document */
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

/* The root the document method was called on: a DOMParser document wraps its own tree */
struct dom_node *doc_root_of(js_value t)
{
  struct dom_node *r = node_of(t);
  return r ? r : g_doc_root;
}

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
  return wrap_node(js_dom_find_selector(doc_root_of(t), arg_str(ctx, A(0))));
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
    js_value cls;
    dom_set_ns(n, DOM_NS_HTML);
    cls = ce_lookup(n->tag);
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
static js_value doc_body(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *root = doc_root_of(t), *b = js_dom_find_tag(root, "body");
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
  return wrap_node(js_dom_find_tag(doc_root_of(t), "head"));
}
static js_value doc_active_element(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *b = js_dom_find_tag(doc_root_of(t), "body"); /* no focus tracking */
  (void)ctx;
  (void)args;
  (void)argc;
  return b ? wrap_node(b) : js_null();
}
static js_value doc_root_el(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *root = doc_root_of(t), *h = js_dom_find_tag(root, "html");
  (void)ctx;
  (void)args;
  (void)argc;
  for (h = h ? h : root->first_child; h && h->type != DOM_ELEMENT; h = h->next)
    ;
  return wrap_node(h ? h : root);
}
static js_value doc_get_title(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *ti = js_dom_find_tag(doc_root_of(t), "title");
  (void)args;
  (void)argc;
  return ti ? text_content_value(ctx, ti) : js_mk_cstring(ctx, "");
}
static js_value doc_get_cookie(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct purl u;
  char *buf;
  int cap = 4096, n;
  js_value r;
  (void)t;
  (void)args;
  (void)argc;
  if (!parse_url(g_page_url_buf, &u))
    return js_mk_cstring(ctx, "");
  for (;; cap *= 2) {
    buf = (char *)malloc((unsigned)cap);
    if (!buf)
      return js_mk_cstring(ctx, "");
    buf[0] = '\0';
    n = cookie_script_list(u.host, u.path, u.tls, buf, cap);
    if (n < cap - 1 || cap >= (1 << 24))
      break;
    free(buf);
  }
  r = js_mk_cstring(ctx, buf);
  free(buf);
  return r;
}
static js_value doc_set_cookie(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct purl u;
  (void)t;
  if (parse_url(g_page_url_buf, &u))
    cookie_store_script(u.host, arg_str(ctx, A(0)));
  return js_undef();
}
static js_value doc_set_title(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *root = doc_root_of(t), *ti = js_dom_find_tag(root, "title");
  if (!ti) {
    struct dom_node *head = js_dom_find_tag(root, "head");
    ti = dom_new_element("title");
    if (ti)
      dom_append_child(head ? head : root, ti);
  }
  if (ti)
    set_text_content(ti, arg_str(ctx, A(0)));
  return js_undef();
}

/* DOMParser: parse a string into an off-document tree wrapped as a Document */
static js_value domparser_parse(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *html = arg_str(ctx, A(0));
  struct dom_node *root;
  (void)t;
  root = dom_parse(html, html ? (int)strlen(html) : 0);
  if (root && argc > 1 && !streq_ci(arg_str(ctx, A(1)), "text/html"))
    dom_mark_xml_doc(root);
  return wrap_document(ctx, root);
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

/* Text and attribute values are stored undecoded. XML only knows five named entities, so other
 * references become numeric ones and a bare & is escaped. */
static void xml_escape(sbuf *sb, const char *s, bool attr)
{
  const char *end = s + strlen(s);
  for (; *s; s++) {
    if (*s == '&') {
      int adv, c = decode_entity(s, end, &adv);
      if (adv == 1)
        sb_puts(sb, "&amp;");
      else if (s[1] == '#' || c == '&' || c == '<' || c == '>' || c == '"' || c == '\'')
        sb_putn(sb, s, adv);
      else if (c != -2) {
        char ref[16];
        snprintf(ref, sizeof ref, "&#%d;", c == -1 ? 160 : c);
        sb_puts(sb, ref);
      }
      s += adv - 1;
    } else if (*s == '<')
      sb_puts(sb, "&lt;");
    else if (*s == '>' && !attr)
      sb_puts(sb, "&gt;");
    else if (*s == '"' && attr)
      sb_puts(sb, "&quot;");
    else
      sb_putc(sb, *s);
  }
}
static void xml_decl(sbuf *sb, const char *prefix, int plen, const char *uri)
{
  sb_puts(sb, plen ? " xmlns:" : " xmlns");
  sb_putn(sb, prefix, plen);
  sb_puts(sb, "=\"");
  xml_escape(sb, uri, true);
  sb_putc(sb, '"');
}
static void xml_node(struct dom_node *e, sbuf *sb, int depth)
{
  struct dom_node *k;
  struct dom_attr *a;
  const char *ns, *declared[8];
  int pl, nd = 0, html;
  if (e->type == DOM_TEXT) {
    if (e->text)
      xml_escape(sb, e->text, false);
    return;
  }
  if (node_is_comment(e)) {
    const char *d = dom_get_attr(e, "data");
    sb_puts(sb, "<!--");
    sb_puts(sb, d ? d : "");
    sb_puts(sb, "-->");
    return;
  }
  if (e->tag[0] == '#') {
    if (depth < SERIALIZE_DEPTH_MAX)
      for (k = e->first_child; k; k = k->next)
        xml_node(k, sb, depth + 1);
    return;
  }
  ns = dom_ns(e);
  pl = dom_prefix_len(e->tag);
  html = !strcmp(ns, DOM_NS_HTML);
  sb_putc(sb, '<');
  sb_puts(sb, dom_qname(e));
  if (*ns && strcmp(dom_scope_ns(e->parent, e->tag, pl), ns)) {
    bool own = false;
    for (a = e->attrs; a; a = a->next)
      own |= pl ? !memcmp(a->name, "xmlns:", 6) && (int)strlen(a->name + 6) == pl && !memcmp(a->name + 6, e->tag, pl)
                : !strcmp(a->name, "xmlns");
    if (!own)
      xml_decl(sb, dom_qname(e), pl, ns);
  }
  for (a = e->attrs; a; a = a->next) {
    int apl = dom_prefix_len(a->name);
    const char *ans = dom_attr_ns(a);
    if (apl && *ans && strcmp(ans, DOM_NS_XMLNS) && strcmp(ans, DOM_NS_XML) && nd < 8 &&
        strcmp(dom_scope_ns(e, a->name, apl), ans)) {
      int seen = 0;
      for (int i = 0; i < nd; i++)
        seen |= !memcmp(declared[i], a->name, apl) && declared[i][apl] == ':';
      if (!seen) {
        declared[nd++] = a->name; /* compared up to the colon */
        xml_decl(sb, dom_attr_qname(e, a), apl, ans);
      }
    }
  }
  for (a = e->attrs; a; a = a->next) {
    sb_putc(sb, ' ');
    sb_puts(sb, dom_attr_qname(e, a));
    sb_puts(sb, "=\"");
    xml_escape(sb, a->value ? a->value : "", true);
    sb_putc(sb, '"');
  }
  if (!e->first_child) {
    if (!html)
      sb_puts(sb, "/>");
    else if (tag_is_void(e->tag))
      sb_puts(sb, " />");
    else {
      sb_puts(sb, "></");
      sb_puts(sb, dom_qname(e));
      sb_putc(sb, '>');
    }
    return;
  }
  sb_putc(sb, '>');
  if (depth < SERIALIZE_DEPTH_MAX)
    for (k = e->first_child; k; k = k->next)
      xml_node(k, sb, depth + 1);
  sb_puts(sb, "</");
  sb_puts(sb, dom_qname(e));
  sb_putc(sb, '>');
}
static js_value xmlser_serialize(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(A(0));
  sbuf sb = {0, 0, 0, 0};
  js_value r;
  (void)t;
  if (!n)
    return js_mk_cstring(ctx, "");
  xml_node(n, &sb, 0);
  r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
  free(sb.b);
  return r;
}
static js_value xmlser_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o;
  (void)thisv;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def_native(ctx, o, "serializeToString", xmlser_serialize, 1);
  gc_pop_scope();
  return js_obj_value(o);
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

/* DOMException: an Error subclass whose `code` is the legacy numeric code of a known name (0 otherwise) */
static const struct {
  const char *name;
  int code;
} k_domexc[] = {
    {"IndexSizeError", 1},
    {"HierarchyRequestError", 3},
    {"WrongDocumentError", 4},
    {"InvalidCharacterError", 5},
    {"NoModificationAllowedError", 7},
    {"NotFoundError", 8},
    {"NotSupportedError", 9},
    {"InUseAttributeError", 10},
    {"InvalidStateError", 11},
    {"SyntaxError", 12},
    {"InvalidModificationError", 13},
    {"NamespaceError", 14},
    {"InvalidAccessError", 15},
    {"TypeMismatchError", 17},
    {"SecurityError", 18},
    {"NetworkError", 19},
    {"AbortError", 20},
    {"URLMismatchError", 21},
    {"QuotaExceededError", 22},
    {"TimeoutError", 23},
    {"InvalidNodeTypeError", 24},
    {"DataCloneError", 25},
};
js_value dom_exception(js_ctx *ctx, const char *name, const char *msg)
{
  js_value ctor = js_get(ctx, js_obj_value(ctx->global), "DOMException"), proto = js_undef(), e;
  int code = 0;
  if (ctor.type == JV_OBJ)
    proto = js_get(ctx, ctor, "prototype");
  for (unsigned i = 0; i < sizeof k_domexc / sizeof k_domexc[0]; i++)
    if (!strcmp(k_domexc[i].name, name))
      code = k_domexc[i].code;
  e = js_make_error(ctx, proto.type == JV_OBJ ? proto.u.obj : ctx->error_proto, name, msg);
  js_def(e.u.obj, "code", js_number(code), P_WRITABLE | P_CONFIGURABLE);
  return e;
}
js_value dom_throw(js_ctx *ctx, const char *name, const char *msg)
{
  js_throw(ctx, dom_exception(ctx, name, msg));
  return js_undef();
}
static js_value domexception_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  return dom_exception(ctx, argc > 1 && A(1).type != JV_UNDEF ? arg_str(ctx, A(1)) : "Error",
                       argc > 0 && A(0).type != JV_UNDEF ? arg_str(ctx, A(0)) : "");
}
static void domexception_install(js_ctx *ctx, js_object *win)
{
  js_object *proto = js_new_object(ctx, ctx->error_proto), *ctor;
  gc_push_scope();
  gc_keep(proto);
  ctor = (js_object *)gc_keep(js_new_native(ctx, domexception_ctor, "DOMException", 0));
  js_def(ctor, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(proto, "name", js_mk_cstring(ctx, "Error"), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "DOMException", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
}

/* Web Storage: localStorage persists per origin, sessionStorage lasts as long as the tab. Each area is a Proxy
 * over an object holding the methods; the handler's "\1session" flag picks the area. */
#define STORAGE_SESSION_KEY "\1session"
static int storage_session(js_ctx *ctx, js_value t) { return js_to_boolean(js_get(ctx, t, STORAGE_SESSION_KEY)); }
static js_value storage_getitem(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *v = storage_get(g_page_url_buf, storage_session(ctx, t), arg_str(ctx, A(0)));
  (void)argc;
  return v ? js_mk_cstring(ctx, v) : js_null();
}
static js_value storage_quota_error(js_ctx *ctx)
{
  return dom_throw(ctx, "QuotaExceededError", "The storage quota has been exceeded");
}
static js_value storage_setitem(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char key[1024];
  (void)argc;
  strlcpy(key, arg_str(ctx, A(0)), sizeof key);
  if (storage_set(g_page_url_buf, storage_session(ctx, t), key, arg_str(ctx, js_to_string(ctx, A(1)))) < 0)
    return storage_quota_error(ctx);
  return js_undef();
}
static js_value storage_removeitem(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)argc;
  storage_remove(g_page_url_buf, storage_session(ctx, t), arg_str(ctx, A(0)));
  return js_undef();
}
static js_value storage_clear_all(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  storage_clear(g_page_url_buf, storage_session(ctx, t));
  return js_undef();
}
static js_value storage_key_at(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  double n = js_to_number(ctx, A(0));
  const char *k = n >= 0 && n < 2147483647.0 ? storage_key(g_page_url_buf, storage_session(ctx, t), (int)n) : 0;
  return k ? js_mk_cstring(ctx, k) : js_null();
}

/* Proxy traps: `this` is the handler. Names on the target (methods, Object.prototype) win over stored keys. */
static int storage_reserved(js_ctx *ctx, js_value target, const char *key)
{
  return !strcmp(key, "length") || key[0] == '\1' || js_get(ctx, target, key).type != JV_UNDEF;
}
static js_value storage_trap_get(js_ctx *ctx, js_value h, js_value *args, int argc)
{
  const char *key = arg_str(ctx, A(1)), *v;
  int session = storage_session(ctx, h);
  if (!strcmp(key, STORAGE_SESSION_KEY))
    return js_bool(session);
  if (!strcmp(key, "length"))
    return js_number(storage_length(g_page_url_buf, session));
  if (js_get(ctx, A(0), key).type != JV_UNDEF)
    return js_get(ctx, A(0), key);
  v = key[0] == '\1' ? 0 : storage_get(g_page_url_buf, session, key);
  return v ? js_mk_cstring(ctx, v) : js_undef();
}
static js_value storage_trap_set(js_ctx *ctx, js_value h, js_value *args, int argc)
{
  char key[1024];
  strlcpy(key, arg_str(ctx, A(1)), sizeof key);
  if (!storage_reserved(ctx, A(0), key) &&
      storage_set(g_page_url_buf, storage_session(ctx, h), key, arg_str(ctx, js_to_string(ctx, A(2)))) < 0)
    return storage_quota_error(ctx);
  return js_bool(1);
}
static js_value storage_trap_has(js_ctx *ctx, js_value h, js_value *args, int argc)
{
  const char *key = arg_str(ctx, A(1));
  return js_bool(storage_reserved(ctx, A(0), key) || storage_get(g_page_url_buf, storage_session(ctx, h), key) != 0);
}
static js_value storage_trap_delete(js_ctx *ctx, js_value h, js_value *args, int argc)
{
  const char *key = arg_str(ctx, A(1));
  if (!storage_reserved(ctx, A(0), key))
    storage_remove(g_page_url_buf, storage_session(ctx, h), key);
  return js_bool(1);
}
static js_value storage_trap_ownkeys(js_ctx *ctx, js_value h, js_value *args, int argc)
{
  js_object *arr;
  int session = storage_session(ctx, h);
  const char *k;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (int i = 0; (k = storage_key(g_page_url_buf, session, i)) != 0; i++)
    js_array_push(ctx, arr, js_mk_cstring(ctx, k));
  gc_pop_scope();
  return js_obj_value(arr);
}
static js_value make_storage(js_ctx *ctx, int session)
{
  js_object *target = js_new_object(ctx, ctx->object_proto), *handler;
  js_value proxy_ctor = js_get(ctx, js_obj_value(ctx->global), "Proxy"), pargs[2], p;
  gc_push_scope();
  gc_keep(target);
  handler = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  def_method(target, "getItem", storage_getitem, 1);
  def_method(target, "setItem", storage_setitem, 2);
  def_method(target, "removeItem", storage_removeitem, 1);
  def_method(target, "clear", storage_clear_all, 0);
  def_method(target, "key", storage_key_at, 1);
  js_def(handler, STORAGE_SESSION_KEY, js_bool(session), 0);
  def_method(handler, "get", storage_trap_get, 3);
  def_method(handler, "set", storage_trap_set, 4);
  def_method(handler, "has", storage_trap_has, 2);
  def_method(handler, "deleteProperty", storage_trap_delete, 2);
  def_method(handler, "ownKeys", storage_trap_ownkeys, 1);
  pargs[0] = js_obj_value(target);
  pargs[1] = js_obj_value(handler);
  p = js_construct(ctx, proxy_ctor, pargs, 2);
  gc_pop_scope();
  return p;
}

/* Expose a DOM interface constructor `name` on the global, wired to `proto` */
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

void setup_dom(js_ctx *ctx, const char *page_url)
{
  js_object *win = ctx->global;

  g_node_proto = js_new_object(ctx, ctx->object_proto); /* Node.prototype */
  g_element_proto = js_new_object(ctx, g_node_proto);
  g_style_proto = js_new_object(ctx, ctx->object_proto);
  g_classlist_proto = js_new_object(ctx, ctx->object_proto);
  g_rellist_proto = js_new_object(ctx, g_classlist_proto);
  g_url_proto = js_new_object(ctx, ctx->object_proto);
  g_sp_proto = js_new_object(ctx, ctx->object_proto);
  g_formdata_proto = js_new_object(ctx, ctx->object_proto);
  g_event_proto = js_new_object(ctx, ctx->object_proto);
  g_html_proto = js_new_object(ctx, g_element_proto); /* HTMLElement.prototype */
  g_media_proto = js_new_object(ctx, g_html_proto);   /* HTMLMediaElement.prototype */
  g_custom_elements = js_new_object(ctx, ctx->object_proto);
  g_module_cache = js_new_object(ctx, ctx->object_proto);
  g_module_src = js_new_object(ctx, ctx->object_proto);
  g_importmap = js_new_object(ctx, ctx->object_proto);
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
  gc_add_root((void **)&g_rellist_proto);
  gc_add_root((void **)&g_url_proto);
  gc_add_root((void **)&g_sp_proto);
  gc_add_root((void **)&g_formdata_proto);
  gc_add_root((void **)&g_event_proto);
  gc_add_root((void **)&g_html_proto);
  gc_add_root((void **)&g_media_proto);
  gc_add_root((void **)&g_custom_elements);
  gc_add_root((void **)&g_module_cache);
  gc_add_root((void **)&g_module_src);
  gc_add_root((void **)&g_importmap);
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

  /* The document IS the root node's wrapper */
  g_document->host = g_doc_root;
  if (g_doc_root)
    g_doc_root->wrapper = g_document;
  dom_on_free_wrapper = wrapper_detach;

  setup_style_proto();

  /* DOMTokenList prototype (element.classList) */
  def_acc(g_classlist_proto, "length", cl_length, 0);
  def_acc(g_classlist_proto, "value", cl_get_value, cl_set_value);
  def_method(g_classlist_proto, "add", cl_add, 1);
  def_method(g_classlist_proto, "remove", cl_remove, 1);
  def_method(g_classlist_proto, "toggle", cl_toggle, 1);
  def_method(g_classlist_proto, "replace", cl_replace, 2);
  def_method(g_classlist_proto, "contains", cl_contains, 1);
  def_method(g_classlist_proto, "supports", cl_supports, 1);
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
  def_acc(g_element_proto, "classList", el_get_classlist, el_set_class);
  def_acc(g_element_proto, "rel", el_get_rel, el_set_rel);
  def_acc(g_element_proto, "relList", el_get_rellist, el_set_rellist);
  def_acc(g_element_proto, "textContent", el_get_text, el_set_text);
  def_acc(g_element_proto, "innerText", el_get_text, el_set_text);
  def_acc(g_element_proto, "innerHTML", el_get_html, el_set_html);
  def_acc(g_element_proto, "parentNode", el_parent, 0);
  def_acc(g_element_proto, "parentElement", el_parent, 0);
  def_acc(g_element_proto, "firstChild", el_first_child, 0);
  def_acc(g_element_proto, "firstElementChild", el_first_elem_child, 0);
  def_acc(g_element_proto, "lastElementChild", el_last_elem_child, 0);
  def_acc(g_element_proto, "childElementCount", el_child_count, 0);
  def_acc(g_element_proto, "previousSibling", el_prev, 0);
  def_acc(g_element_proto, "previousElementSibling", el_prev_elem, 0);
  def_acc(g_element_proto, "isConnected", el_is_connected, 0);
  def_acc(g_element_proto, "ownerDocument", el_owner_document, 0);
  def_acc(g_element_proto, "outerHTML", el_outer_html, 0);
  def_acc(g_element_proto, "nodeValue", el_node_value, el_set_node_value);
  def_acc(g_element_proto, "data", el_node_value, el_set_node_value);
  def_acc(g_element_proto, "attributes", el_attributes, 0);
  def_acc(g_element_proto, "lastChild", el_last_child, 0);
  def_acc(g_element_proto, "nextSibling", el_next, 0);
  def_acc(g_element_proto, "nextElementSibling", el_next_elem, 0);
  def_acc(g_element_proto, "children", el_children, 0);
  def_acc(g_element_proto, "childNodes", el_child_nodes, 0);
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
  def_method(g_element_proto, "append", el_append_multi, 1);
  def_method(g_element_proto, "prepend", el_prepend, 1);
  def_method(g_element_proto, "replaceChildren", el_replace_children, 0);
  def_method(g_element_proto, "before", el_before, 1);
  def_method(g_element_proto, "after", el_after, 1);
  def_method(g_element_proto, "replaceWith", el_replace_with, 1);
  def_method(g_element_proto, "replaceChild", el_replace_child, 2);
  def_method(g_element_proto, "remove", el_remove, 0);
  def_method(g_element_proto, "cloneNode", el_clone, 1);
  def_method(g_element_proto, "hasChildNodes", el_has_children, 0);
  def_method(g_element_proto, "getRootNode", el_root_node, 0);
  def_method(g_element_proto, "compareDocumentPosition", el_compare_position, 1);
  def_method(g_element_proto, "insertAdjacentHTML", el_insert_adjacent_html, 2);
  def_method(g_element_proto, "insertAdjacentElement", el_insert_adjacent_el, 2);
  def_method(g_element_proto, "insertAdjacentText", el_insert_adjacent_text, 2);
  def_method(g_element_proto, "getElementsByClassName", el_get_by_class, 1);
  def_method(g_element_proto, "getAttributeNames", el_attr_names, 0);
  def_method(g_element_proto, "hasAttributes", el_has_attrs, 0);
  def_method(g_element_proto, "toggleAttribute", el_toggle_attr, 1);
  def_method(g_element_proto, "click", el_click, 0);
  def_method(g_element_proto, "focus", el_noop, 0);
  def_method(g_element_proto, "blur", el_noop, 0);
  def_method(g_element_proto, "scrollIntoView", el_noop, 0);
  def_method(g_element_proto, "scrollTo", el_noop, 2);
  def_method(g_element_proto, "scroll", el_noop, 2);
  def_method(g_element_proto, "normalize", el_noop, 0);
  def_method(g_element_proto, "webkitMatchesSelector", el_matches, 1);
  def_method(g_element_proto, "msMatchesSelector", el_matches, 1);
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

  /* document: methods on the shared proto so DOMParser documents inherit them */
  def_method(g_document_proto, "getElementById", doc_get_by_id, 1);
  def_method(g_document_proto, "getElementsByTagName", doc_get_by_tag, 1);
  def_method(g_document_proto, "querySelector", doc_query, 1);
  def_method(g_document_proto, "querySelectorAll", doc_query_all, 1);
  def_method(g_document_proto, "createElement", doc_create_el, 1);
  def_method(g_document_proto, "createTextNode", doc_create_text, 1);
  def_method(g_document_proto, "createDocumentFragment", doc_create_frag, 0);
  def_method(g_document_proto, "createComment", doc_create_comment, 1);
  def_method(g_document_proto, "createTreeWalker", doc_create_tree_walker, 3);
  def_method(g_document_proto, "createNodeIterator", doc_create_node_iterator, 3);
  def_method(g_document_proto, "createEvent", doc_create_event, 1);
  def_method(g_document_proto, "importNode", doc_import_node, 2);
  def_method(g_document_proto, "adoptNode", doc_adopt_node, 1);
  def_method(g_document_proto, "getElementsByClassName", doc_get_by_class, 1);
  def_method(g_document_proto, "contains", doc_contains, 1);
  def_method(g_document_proto, "getRootNode", el_owner_document, 0);
  def_method(g_document_proto, "getElementsByName", doc_get_by_name, 1);
  def_method(g_document_proto, "execCommand", doc_false, 1);
  def_method(g_document_proto, "elementFromPoint", doc_null, 2);
  def_method(g_document_proto, "open", el_noop, 0);
  def_method(g_document_proto, "close", el_noop, 0);
  def_method(g_document_proto, "writeln", el_noop, 1);
  def_acc(g_document_proto, "childNodes", el_child_nodes, 0);
  def_acc(g_document_proto, "children", el_children, 0);
  def_acc(g_document_proto, "firstChild", el_first_child, 0);
  def_acc(g_document_proto, "lastChild", el_last_child, 0);
  def_acc(g_document_proto, "firstElementChild", el_first_elem_child, 0);
  def_acc(g_document_proto, "lastElementChild", el_last_elem_child, 0);
  def_acc(g_document_proto, "childElementCount", el_child_count, 0);
  def_acc(g_document_proto, "parentNode", doc_null, 0);
  def_acc(g_document_proto, "parentElement", doc_null, 0);
  def_acc(g_document_proto, "ownerDocument", doc_null, 0);
  def_acc(g_document_proto, "textContent", doc_null, 0);
  def_acc(g_document_proto, "doctype", doc_null, 0);
  def_acc(g_document_proto, "fullscreenElement", doc_null, 0);
  def_acc(g_document_proto, "scrollingElement", doc_root_el, 0);
  def_acc(g_document_proto, "scripts", doc_scripts, 0);
  def_acc(g_document_proto, "forms", doc_forms, 0);
  def_acc(g_document_proto, "images", doc_images, 0);
  def_acc(g_document_proto, "links", doc_links, 0);
  def_acc(g_document_proto, "anchors", el_empty_array, 0);
  def_acc(g_document_proto, "embeds", el_empty_array, 0);
  def_acc(g_document_proto, "plugins", el_empty_array, 0);
  def_acc(g_document_proto, "domain", doc_domain, 0);
  def_acc(g_document_proto, "implementation", doc_implementation, 0);
  js_def(g_document_proto, "characterSet", js_mk_cstring(ctx, "UTF-8"), P_ENUMERABLE);
  js_def(g_document_proto, "charset", js_mk_cstring(ctx, "UTF-8"), P_ENUMERABLE);
  js_def(g_document_proto, "inputEncoding", js_mk_cstring(ctx, "UTF-8"), P_ENUMERABLE);
  js_def(g_document_proto, "contentType", js_mk_cstring(ctx, "text/html"), P_ENUMERABLE);
  js_def(g_document_proto, "compatMode", js_mk_cstring(ctx, "CSS1Compat"), P_ENUMERABLE);
  js_def(g_document_proto, "lastModified", js_mk_cstring(ctx, ""), P_ENUMERABLE);
  js_def(g_document_proto, "designMode", js_mk_cstring(ctx, "off"), P_ENUMERABLE | P_WRITABLE);
  js_def(g_document_proto, "dir", js_mk_cstring(ctx, ""), P_ENUMERABLE | P_WRITABLE);
  def_method(g_document_proto, "addEventListener", el_add_listener, 3);
  def_method(g_document_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_document_proto, "dispatchEvent", el_dispatch, 1);
  def_method(g_document_proto, "write", el_noop, 1);
  def_acc(g_document_proto, "activeElement", doc_active_element, 0);
  def_acc(g_document_proto, "body", doc_body, 0);
  def_acc(g_document_proto, "head", doc_head, 0);
  def_acc(g_document_proto, "documentElement", doc_root_el, 0);
  def_acc(g_document_proto, "title", doc_get_title, doc_set_title);
  def_acc(g_document_proto, "cookie", doc_get_cookie, doc_set_cookie);
  js_def(g_document_proto, "nodeType", js_number(9), P_ENUMERABLE);
  js_def(g_document_proto, "nodeName", js_mk_cstring(ctx, "#document"), P_ENUMERABLE);
  js_def(g_document, "readyState", js_mk_cstring(ctx, "loading"), P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  js_def(g_document, "visibilityState", js_mk_cstring(ctx, "visible"), P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  js_def(g_document, "hidden", js_bool(0), P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  js_def(g_document, "currentScript", js_null(), P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  js_def(g_document, "referrer", js_mk_cstring(ctx, B->referrer), P_WRITABLE | P_CONFIGURABLE | P_ENUMERABLE);
  def_method(g_document, "hasFocus", el_true, 0);
  js_def(g_document, "defaultView", js_obj_value(win), P_CONFIGURABLE | P_ENUMERABLE);

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
    js_def(g_document, "documentURI", js_mk_cstring(ctx, page_url ? page_url : ""), P_WRITABLE | P_CONFIGURABLE);
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
    if (ch_secure_url(page_url)) {
      js_object *uad = js_new_object(ctx, ctx->object_proto); /* NavigatorUAData: secure contexts only */
      gc_keep(uad);
      uad_low_entropy(ctx, uad);
      def_method(uad, "getHighEntropyValues", uad_high_entropy, 1);
      def_method(uad, "toJSON", uad_tojson, 0);
      js_def(nav, "userAgentData", js_obj_value(uad), P_ENUMERABLE);
    }
    mediasession_install(ctx, nav, page_url);
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
  /* ClipboardItem (window global), pairs with navigator.clipboard.read/write */
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
  /* crypto: getRandomValues (RDRAND-backed) + randomUUID. window.crypto and the bare `crypto` global */
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
  js_streams_setup(ctx, win);
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
    js_def_native(ctx, uc, "createObjectURL", url_create_object_url, 1);
    js_def_native(ctx, uc, "revokeObjectURL", url_revoke_object_url, 1);
    js_def(win, "URL", js_obj_value(uc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "URLSearchParams", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* FormData, a name/value multimap over the same @k/@v arrays as URLSearchParams */
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

  /* Popover API on every HTMLElement */
  def_method(g_html_proto, "showPopover", el_show_popover, 0);
  def_method(g_html_proto, "hidePopover", el_hide_popover, 0);
  def_method(g_html_proto, "togglePopover", el_toggle_popover, 1);
  def_acc(g_html_proto, "popover", el_get_popover, el_set_popover);

  /* DOM events: Event prototype, the Event/CustomEvent constructors */
  def_method(g_event_proto, "initEvent", ev_init_event, 3);
  def_method(g_event_proto, "initCustomEvent", ev_init_event, 4);
  def_method(g_event_proto, "preventDefault", ev_prevent, 0);
  def_method(g_event_proto, "composedPath", ev_composed_path, 0);
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
  /* EventTarget: root of everything that takes listeners */
  {
    js_object *tp = js_new_object(ctx, ctx->object_proto);
    js_object *tc;
    js_object *const heirs[] = {g_node_proto, g_abortsignal_proto, g_ws_proto, g_xhr_proto, g_bchannel_proto, win};
    gc_push_scope();
    gc_keep(tp);
    def_method(tp, "addEventListener", el_add_listener, 3);
    def_method(tp, "removeEventListener", el_remove_listener, 3);
    def_method(tp, "dispatchEvent", el_dispatch, 1);
    tc = (js_object *)gc_keep(js_new_native(ctx, eventtarget_ctor, "EventTarget", 0));
    tc->fn->data = js_obj_value(tp);
    js_def(tc, "prototype", js_obj_value(tp), 0);
    js_def(tp, "constructor", js_obj_value(tc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "EventTarget", js_obj_value(tc), P_WRITABLE | P_CONFIGURABLE);
    for (unsigned i = 0; i < sizeof heirs / sizeof heirs[0]; i++)
      if (heirs[i]->proto == ctx->object_proto)
        heirs[i]->proto = tp;
    gc_pop_scope();
  }
  /* Event subclasses. Each gets a prototype chaining to its parent interface's */
  {
    static const struct {
      const char *name;
      int parent;
    } subs[] = {
        {"UIEvent", -1},   /* -> Event.prototype */
        {"MouseEvent", 0}, /* -> UIEvent.prototype */
        {"KeyboardEvent", 0},    {"FocusEvent", 0},    {"InputEvent", 0},
        {"CompositionEvent", 0}, {"PointerEvent", 1}, /* -> MouseEvent.prototype */
        {"WheelEvent", 1},       {"DragEvent", 1},     {"ErrorEvent", -1},
        {"MessageEvent", -1},    {"StorageEvent", -1},
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

  /* Web components: HTMLElement (the base custom classes extend) + customElements */
  {
    js_object *nc = js_new_native(ctx, node_ctor, "Node", 0);
    gc_push_scope();
    gc_keep(nc);
    js_def(nc, "prototype", js_obj_value(g_node_proto), 0);
    js_def(g_node_proto, "constructor", js_obj_value(nc), P_WRITABLE | P_CONFIGURABLE);
    def_node_consts(nc);           /* Node.ELEMENT_NODE etc. (on the interface) */
    def_node_consts(g_node_proto); /* and inherited by instances (node.TEXT_NODE) */
    js_def(win, "Node", js_obj_value(nc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  treewalker_install(ctx, win);
  observers_install(ctx, win);
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
    def_method(ce, "upgrade", ce_upgrade, 1);
    js_def(win, "customElements", js_obj_value(ce), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* Standard DOM interface constructors */
  {
    /* Element/Document/HTMLMediaElement are the shared bases the family chains to */
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
    /* Image is a global alias constructing an <img> */
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
    {
      js_object *dp = iface_proto_for_tag("dialog");
      def_method(dp, "show", dlg_show, 0);
      def_method(dp, "showModal", dlg_show_modal, 0);
      def_method(dp, "close", dlg_close, 1);
      def_method(dp, "requestClose", dlg_request_close, 1);
      def_acc(dp, "open", dlg_get_open, dlg_set_open);
      def_acc(dp, "returnValue", dlg_get_rv, dlg_set_rv);
      dp = iface_proto_for_tag("details");
      def_acc(dp, "open", dlg_get_open, dlg_set_open);
    }
  }
  js_def_native(ctx, win, "DOMParser", domparser_ctor, 0); /* new DOMParser().parseFromString */
  js_def_native(ctx, win, "XMLSerializer", xmlser_ctor, 0);
  xpath_install(ctx, win);
  shadow_install(ctx, win);
  namespace_install();

  /* fetch() + Response.prototype */
  def_method(g_response_proto, "text", resp_text, 0);
  def_method(g_response_proto, "json", resp_json, 0);
  def_method(g_response_proto, "clone", resp_clone, 0);
  {
    js_object *rc;
    gc_push_scope();
    rc = (js_object *)gc_keep(js_new_native(ctx, response_ctor, "Response", 2));
    js_def(rc, "prototype", js_obj_value(g_response_proto), 0);
    js_def(g_response_proto, "constructor", js_obj_value(rc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "Response", js_obj_value(rc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  def_method(win, "fetch", win_fetch, 2);

  /* WebSocket (RFC 6455 over user/lib/websocket): live */
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

  /* BroadcastChannel: same-context message bus */
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
  def_method(g_xhr_proto, "abort", xhr_abort, 0);
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

  canvas_js_setup(ctx, win);

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

  /* RTCPeerConnection: inert (no WebRTC/ICE in OwOS) */
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

  /* history (SPA routers lean on it): inert but present so pushState/back don't throw */
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
  /* matchMedia + queueMicrotask: present but inert */
  def_method(win, "matchMedia", win_matchmedia, 1);
  domexception_install(ctx, win);
  js_def(win, "localStorage", make_storage(ctx, 0), P_WRITABLE | P_CONFIGURABLE);
  js_idb_setup(ctx, win);
  js_def(win, "sessionStorage", make_storage(ctx, 1), P_WRITABLE | P_CONFIGURABLE);
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
  def_acc(win, "outerWidth", win_innerWidth, 0);
  def_acc(win, "outerHeight", win_innerHeight, 0);
  js_def(win, "top", js_obj_value(win), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "parent", js_obj_value(win), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "frames", js_obj_value(win), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "opener", js_null(), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "length", js_number(0), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "name", js_mk_cstring(ctx, ""), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "devicePixelRatio", js_number(1), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "isSecureContext", js_bool(page_url && starts_ci(page_url, "https:")), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "origin", js_get(ctx, js_get(ctx, js_obj_value(win), "location"), "origin"), P_WRITABLE | P_CONFIGURABLE);
  def_method(win, "scrollTo", el_noop, 2);
  def_method(win, "scrollBy", el_noop, 2);
  def_method(win, "scroll", el_noop, 2);
  def_method(win, "open", doc_null, 0);
  def_method(win, "close", el_noop, 0);
  def_method(win, "focus", el_noop, 0);
  def_method(win, "blur", el_noop, 0);

  /* navigator.serviceWorker + caches + Response/Request */
  sw_install(ctx, win, page_url);
  js_worker_setup(ctx, win);
}
