/* js.c — wires the OwOS JavaScript engine (user/lib/js) into the browser.
 *
 * After the DOM is parsed, html.c calls js_run_document(): it spins up a fresh
 * engine context, installs a small `document`/`window` binding layer over the
 * browser's DOM (browser/dom.h), and runs every inline <script> in document
 * order sharing one global. Scripts run BEFORE layout, so any DOM they mutate is
 * reflected when html.c walks the tree to produce the styled page.
 *
 * A DOM element is exposed to JS as a wrapper object whose `host` slot (the
 * js_object embedder hook) points at the struct dom_node. Wrappers are created
 * fresh per access, so node *identity* across separate lookups is not preserved
 * yet — a known first-cut simplification. console output goes to stdout (fd 1);
 * timers (setTimeout/...) are accepted but do not fire (no event loop yet).
 */
#include "browser.h"      /* owos.h, dom.h (DOM + lower/streq_ci), struct browser */
#include "interp.h"       /* js_ctx, js_run, js_call, js_ctx_new, js_def_native   */
#include "parser.h"       /* js_parse, js_arena                                   */
#include "gc.h"           /* gc_add_root / gc_remove_root / scopes                */
#include "websocket.h"    /* ws_open / ws_poll / ws_send / ws_close (RFC 6455)    */

#define A(i) ((i) < argc ? args[i] : js_undef())


static js_ctx    *g_ctx;
static js_object *g_element_proto;
static js_object *g_style_proto;
static js_object *g_url_proto;
static js_object *g_sp_proto;
static js_object *g_event_proto;
static js_object *g_html_proto;        /* HTMLElement.prototype (chains to element) */
static js_object *g_custom_elements;   /* registry: tag name -> class constructor   */
static js_object *g_module_cache;      /* ES module registry: abs URL -> namespace  */
static js_object *g_document_proto;    /* shared by the live doc + DOMParser docs    */
static js_object *g_response_proto;    /* fetch() Response.prototype                  */
static js_object *g_ws_proto;          /* WebSocket.prototype                          */
static js_object *g_websockets;        /* rooted array of live WebSocket wrappers      */
static js_object *g_abortsignal_proto; /* AbortSignal.prototype                        */
static js_object *g_abortcontroller_proto; /* AbortController.prototype                */
static js_object *g_document;
static struct dom_node *g_doc_root;
static struct dom_node *g_parsed[64];  /* DOMParser trees, freed at page teardown    */
static int g_nparsed;
static res_batch_fn g_fetch;           /* the batch fetch used to pull modules       */
static char g_page_url_buf[URL_MAX];   /* the page URL (module-resolution referrer)  */

/* The JS context now lives for the whole page (not just the initial script run),
 * so registered listeners + closures survive until the next navigation. These
 * hold that persistent state; html_page_teardown() reclaims it. */
static js_object *g_live_wrappers;     /* rooted array pinning every node wrapper  */
static int        g_has_listeners;     /* any addEventListener fired this page      */
static int        g_page_live;         /* a page context is currently alive         */
static js_arena **g_arenas;            /* per-script AST arenas (closures need them); grows */
static int        g_narenas, g_arenas_cap;

/* Timer queue (setTimeout/setInterval/requestAnimationFrame). The engine has no
 * event loop, so timers are driven cooperatively: the worker calls js_timers_pump
 * each idle tick, which fires every timer whose deadline has passed. Slot i holds
 * a callback+extra-args bundle in the rooted array g_timers (so closures stay GC-
 * live); the parallel g_tmeta[] carries the scheduling. A timer id is slot+1. The
 * table grows on demand (a page may arm any number of timers); cleared one-shots
 * free their slot, so it settles at the page's peak concurrent-timer count. */
static js_object *g_timers;            /* rooted: slot -> [callback, ...args] or null */
static struct timer_meta {
    unsigned char active, repeat, raf;
    long long      due, period;        /* ms; due is the next fire time (now_ms scale) */
} *g_tmeta;
static int        g_tcap;              /* allocated slots in g_tmeta                  */
static int        g_ntimers;           /* active count (fast empty-check for the pump) */

/* ------------------------------------------------------- small buffer ----- */

typedef struct { char *b; int n, cap; } sbuf;
static void sb_need(sbuf *s, int extra) {
    if (s->n + extra + 1 > s->cap) {
        int c = s->cap ? s->cap * 2 : 128;
        while (c < s->n + extra + 1) c *= 2;
        s->b = (char *)realloc(s->b, (unsigned)c);
        s->cap = c;
    }
}
static void sb_putc(sbuf *s, char c) { sb_need(s, 1); s->b[s->n++] = c; s->b[s->n] = 0; }
static void sb_puts(sbuf *s, const char *t) { int n = strlen(t); sb_need(s, n); memcpy(s->b + s->n, t, (unsigned)n); s->n += n; s->b[s->n] = 0; }
static void sb_putn(sbuf *s, const char *t, int n) { sb_need(s, n); memcpy(s->b + s->n, t, (unsigned)n); s->n += n; s->b[s->n] = 0; }

/* ------------------------------------------------------- node wrappers ---- */

static int memcmp_js(const char *a, const char *b, int n) {
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static struct dom_node *node_of(js_value v) {
    if (v.type != JV_OBJ) return 0;
    return (struct dom_node *)v.u.obj->host;
}
/* A DOM node is exposed as a wrapper object whose `host` slot points at the node.
 * The wrapper is cached on the node (dom_node.wrapper) so repeated lookups return
 * the SAME object — node identity, which event listeners depend on. Every wrapper
 * is pinned in g_live_wrappers so its listener closures survive GC for the page. */
static js_value wrap_node(struct dom_node *n) {
    js_object *o;
    if (!n) return js_null();
    if (n->wrapper) return js_obj_value((js_object *)n->wrapper);
    gc_push_scope();
    o = (js_object *)gc_keep(js_new_object(g_ctx, g_element_proto));
    o->host = n;
    n->wrapper = o;
    if (g_live_wrappers) js_array_push(g_ctx, g_live_wrappers, js_obj_value(o));
    gc_pop_scope();
    return js_obj_value(o);
}

/* dom.c calls this (via the dom_on_free_wrapper hook) when a node that owns a
 * wrapper is freed mid-page (innerHTML/removeChild), so the surviving wrapper goes
 * inert (node_of -> NULL) rather than dereferencing freed memory. */
static void wrapper_detach(void *wrapper) {
    if (wrapper) ((js_object *)wrapper)->host = 0;
}

static const char *arg_str(js_ctx *ctx, js_value v) {
    js_value s = js_to_string(ctx, v);
    return s.type == JV_STR ? s.u.str->data : "";
}

/* custom-element hooks (defined with the web-components block below) */
static js_value ce_lookup(const char *tag);
static void     ce_upgrade_node(struct dom_node *n, js_value cls);
static void     ce_connect(struct dom_node *n);
static void     ce_attr_changed(struct dom_node *n, const char *name, const char *oldv, const char *newv);
static void     copy_str(char *dst, int cap, const char *s);

/* lowercase copy into a fixed buffer */
static void lower_into(char *dst, int cap, const char *src) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = lower(src[i]);
    dst[i] = '\0';
}

/* ------------------------------------------------------- DOM queries ------ */

typedef int (*match_fn)(struct dom_node *, const char *);

static int match_id(struct dom_node *n, const char *v) { const char *a = dom_get_attr(n, "id"); return a && streq(a, v); }
static int match_tag(struct dom_node *n, const char *v) { return streq(v, "*") || streq(n->tag, v); }
static int match_class(struct dom_node *n, const char *v) {
    const char *a = dom_get_attr(n, "class");
    int vl = strlen(v);
    if (!a) return 0;
    while (*a) {
        const char *s;
        while (*a == ' ' || *a == '\t' || *a == '\n') a++;
        s = a;
        while (*a && *a != ' ' && *a != '\t' && *a != '\n') a++;
        if (a - s == vl && memcmp_js(s, v, vl) == 0) return 1;
    }
    return 0;
}

static struct dom_node *find_first(struct dom_node *n, match_fn m, const char *v) {
    struct dom_node *c;
    for (c = n->first_child; c; c = c->next) {
        struct dom_node *r;
        if (c->type == DOM_ELEMENT && m(c, v)) return c;
        r = find_first(c, m, v);
        if (r) return r;
    }
    return 0;
}
static void find_all(struct dom_node *n, match_fn m, const char *v, js_object *arr) {
    struct dom_node *c;
    for (c = n->first_child; c; c = c->next) {
        if (c->type == DOM_ELEMENT && m(c, v)) js_array_push(g_ctx, arr, wrap_node(c));
        find_all(c, m, v, arr);
    }
}

/* split a simple selector into (matcher, token-in-buf) */
static match_fn parse_selector(const char *sel, char *buf, int cap) {
    char kind = 0;
    int i = 0;
    while (*sel == ' ') sel++;
    if (*sel == '#' || *sel == '.') { kind = *sel; sel++; }
    for (; sel[i] && sel[i] != ' ' && sel[i] != '>' && i < cap - 1; i++)
        buf[i] = kind ? sel[i] : lower(sel[i]);
    buf[i] = '\0';
    return kind == '#' ? match_id : kind == '.' ? match_class : match_tag;
}

/* ------------------------------------------------------- text content ----- */

static void collect_text(struct dom_node *n, sbuf *sb) {
    struct dom_node *c;
    if (n->type == DOM_TEXT) { if (n->text) sb_puts(sb, n->text); return; }
    for (c = n->first_child; c; c = c->next) collect_text(c, sb);
}
static void serialize(struct dom_node *n, sbuf *sb) {
    struct dom_node *c;
    for (c = n->first_child; c; c = c->next) {
        if (c->type == DOM_TEXT) { if (c->text) sb_puts(sb, c->text); continue; }
        sb_putc(sb, '<'); sb_puts(sb, c->tag);
        { struct dom_attr *a; for (a = c->attrs; a; a = a->next) { sb_putc(sb, ' '); sb_puts(sb, a->name); sb_puts(sb, "=\""); sb_puts(sb, a->value ? a->value : ""); sb_putc(sb, '"'); } }
        sb_putc(sb, '>');
        serialize(c, sb);
        sb_puts(sb, "</"); sb_puts(sb, c->tag); sb_putc(sb, '>');
    }
}

static js_value text_content_value(js_ctx *ctx, struct dom_node *n) {
    sbuf sb; js_value r;
    sb.b = 0; sb.n = 0; sb.cap = 0;
    collect_text(n, &sb);
    r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
    if (sb.b) free(sb.b);
    return r;
}
static void set_text_content(struct dom_node *n, const char *text) {
    dom_clear_children(n);
    if (text && *text) { struct dom_node *t = dom_new_text(text); if (t) dom_append_child(n, t); }
}
static void set_inner_html(struct dom_node *el, const char *html) {
    struct dom_node *frag, *c;
    dom_clear_children(el);
    frag = dom_parse(html, html ? (int)strlen(html) : 0);
    if (!frag) return;
    c = frag->first_child;
    frag->first_child = frag->last_child = 0;
    while (c) { struct dom_node *nx = c->next; c->next = 0; c->parent = 0; dom_append_child(el, c); c = nx; }
    dom_free(frag);
}

/* ------------------------------------------------------- element natives -- */

static js_value el_tagName(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    char up[16]; int i = 0;
    (void)args; (void)argc;
    if (!n) return js_undef();
    for (; n->tag[i] && i < 15; i++) up[i] = (n->tag[i] >= 'a' && n->tag[i] <= 'z') ? (char)(n->tag[i] - 32) : n->tag[i];
    up[i] = '\0';
    return js_mk_cstring(ctx, up);
}
static js_value el_nodeName(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    if (n && n->type == DOM_TEXT) return js_mk_cstring(ctx, "#text");
    return el_tagName(ctx, t, args, argc);
}
static js_value el_nodeType(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    (void)ctx; (void)args; (void)argc;
    return js_number(n && n->type == DOM_TEXT ? 3 : 1);
}
static js_value el_get_id(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t); const char *v;
    (void)args; (void)argc;
    v = n ? dom_get_attr(n, "id") : 0;
    return js_mk_cstring(ctx, v ? v : "");
}
static js_value el_set_id(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    if (n) dom_set_attr(n, "id", arg_str(ctx, A(0)));
    return js_undef();
}
static js_value el_get_class(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t); const char *v;
    (void)args; (void)argc;
    v = n ? dom_get_attr(n, "class") : 0;
    return js_mk_cstring(ctx, v ? v : "");
}
static js_value el_set_class(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    if (n) dom_set_attr(n, "class", arg_str(ctx, A(0)));
    return js_undef();
}
/* .src / .href reflect the attribute resolved to an absolute URL (as the real DOM
 * does — e.g. `new URL(script.src)` needs an absolute value). Setters write the
 * raw attribute. */
static js_value el_get_url_attr(js_ctx *ctx, js_value t, const char *attr) {
    struct dom_node *n = node_of(t); const char *v;
    char abs[URL_MAX];
    v = n ? dom_get_attr(n, attr) : 0;
    if (!v || !*v) return js_mk_cstring(ctx, "");
    resolve_url(g_page_url_buf, v, abs, sizeof abs);
    return js_mk_cstring(ctx, abs);
}
static js_value el_get_src(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)args; (void)argc; return el_get_url_attr(ctx, t, "src");
}
static js_value el_set_src(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    if (n) dom_set_attr(n, "src", arg_str(ctx, A(0)));
    return js_undef();
}
static js_value el_get_href(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)args; (void)argc; return el_get_url_attr(ctx, t, "href");
}
static js_value el_set_href(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    if (n) dom_set_attr(n, "href", arg_str(ctx, A(0)));
    return js_undef();
}
static js_value el_get_text(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    (void)args; (void)argc;
    if (!n) return js_mk_cstring(ctx, "");
    return text_content_value(ctx, n);
}
static js_value el_set_text(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    if (n) set_text_content(n, arg_str(ctx, A(0)));
    return js_undef();
}
static js_value el_get_html(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    sbuf sb; js_value r;
    (void)args; (void)argc;
    if (!n) return js_mk_cstring(ctx, "");
    sb.b = 0; sb.n = 0; sb.cap = 0;
    serialize(n, &sb);
    r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
    if (sb.b) free(sb.b);
    return r;
}
static js_value el_set_html(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    if (n) set_inner_html(n, arg_str(ctx, A(0)));
    return js_undef();
}
static js_value el_get_attr(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    const char *v = n ? dom_get_attr(n, arg_str(ctx, A(0))) : 0;
    return v ? js_mk_cstring(ctx, v) : js_null();
}
static js_value el_set_attr(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    if (n) {
        char name[64], oldv[256], val[1024];
        const char *ov;
        copy_str(name, sizeof name, arg_str(ctx, A(0)));
        ov = dom_get_attr(n, name);
        copy_str(oldv, sizeof oldv, ov ? ov : "");
        copy_str(val, sizeof val, arg_str(ctx, A(1)));
        dom_set_attr(n, name, val);
        ce_attr_changed(n, name, ov ? oldv : 0, val);     /* observedAttributes hook */
    }
    return js_undef();
}
static js_value el_has_attr(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    return js_bool(n && dom_get_attr(n, arg_str(ctx, A(0))) != 0);
}
static js_value el_rm_attr(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    if (n) dom_remove_attr(n, arg_str(ctx, A(0)));
    return js_undef();
}
static js_value el_append(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t), *c = node_of(A(0));
    (void)ctx;
    if (n && c) {
        dom_append_child(n, c);
        if (c->type == DOM_ELEMENT) {                /* upgrade + connect a custom el */
            js_value cls = ce_lookup(c->tag);
            if (cls.type == JV_OBJ) ce_upgrade_node(c, cls);
            ce_connect(c);
        }
    }
    return A(0);
}
static js_value el_remove_child(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t), *c = node_of(A(0));
    (void)ctx;
    if (n && c) dom_remove_child(n, c);
    return A(0);
}
static js_value el_parent(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    (void)ctx; (void)args; (void)argc;
    return n && n->parent && n->parent != g_doc_root ? wrap_node(n->parent) : (n && n->parent ? wrap_node(n->parent) : js_null());
}
static js_value el_first_child(js_ctx *ctx, js_value t, js_value *args, int argc) { struct dom_node *n = node_of(t); (void)ctx; (void)args; (void)argc; return n ? wrap_node(n->first_child) : js_null(); }
static js_value el_last_child(js_ctx *ctx, js_value t, js_value *args, int argc) { struct dom_node *n = node_of(t); (void)ctx; (void)args; (void)argc; return n ? wrap_node(n->last_child) : js_null(); }
static js_value el_next(js_ctx *ctx, js_value t, js_value *args, int argc) { struct dom_node *n = node_of(t); (void)ctx; (void)args; (void)argc; return n ? wrap_node(n->next) : js_null(); }
static js_value el_children(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t), *c;
    js_object *arr;
    int elems_only = 1;
    (void)args; (void)argc;
    gc_push_scope();
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    if (n) for (c = n->first_child; c; c = c->next) if (!elems_only || c->type == DOM_ELEMENT) js_array_push(ctx, arr, wrap_node(c));
    gc_pop_scope();
    return js_obj_value(arr);
}
/* ------------------------------------------------------- style (CSSOM-lite) -
 * el.style is a thin proxy whose host is the element node; its accessors and
 * methods read/write the element's style="" declaration string, which css.c's
 * css_apply_inline already folds in at layout time. So `el.style.color='red'`
 * (or setProperty) actually restyles the page on the next render. */

static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int span_ci_eq(const char *s, const char *e, const char *lit) {
    while (s < e && *lit) { if (lower(*s) != lower(*lit)) return 0; s++; lit++; }
    return s == e && !*lit;
}

static js_value style_read(js_ctx *ctx, struct dom_node *node, const char *prop) {
    const char *style = node ? dom_get_attr(node, "style") : 0;
    const char *p = style;
    while (p && *p) {
        const char *seg = p, *colon;
        while (*p && *p != ';') p++;
        colon = seg;
        while (colon < p && *colon != ':') colon++;
        if (colon < p) {
            const char *ns = seg, *ne = colon, *vs = colon + 1, *ve = p;
            while (ns < ne && is_sp(*ns)) ns++;
            while (ne > ns && is_sp(ne[-1])) ne--;
            while (vs < ve && is_sp(*vs)) vs++;
            while (ve > vs && is_sp(ve[-1])) ve--;
            if (ns < ne && span_ci_eq(ns, ne, prop)) return js_mk_string(ctx, vs, (int)(ve - vs));
        }
        if (*p == ';') p++;
    }
    return js_mk_cstring(ctx, "");
}

/* Rewrite the element's style="" with `setprop` set to `setval` (NULL/"" removes
 * it). Every other declaration is preserved, the changed one moves to the end. */
static void style_rebuild(struct dom_node *node, const char *setprop, const char *setval) {
    const char *style;
    sbuf sb;
    if (!node) return;
    sb.b = 0; sb.n = 0; sb.cap = 0;
    style = dom_get_attr(node, "style");
    {
        const char *p = style;
        while (p && *p) {
            const char *seg = p, *colon;
            while (*p && *p != ';') p++;
            colon = seg;
            while (colon < p && *colon != ':') colon++;
            if (colon < p) {
                const char *ns = seg, *ne = colon, *vs = colon + 1, *ve = p;
                while (ns < ne && is_sp(*ns)) ns++;
                while (ne > ns && is_sp(ne[-1])) ne--;
                while (vs < ve && is_sp(*vs)) vs++;
                while (ve > vs && is_sp(ve[-1])) ve--;
                if (ns < ne && !span_ci_eq(ns, ne, setprop)) {
                    if (sb.n) sb_puts(&sb, "; ");
                    sb_putn(&sb, ns, (int)(ne - ns));
                    sb_puts(&sb, ": ");
                    sb_putn(&sb, vs, (int)(ve - vs));
                }
            }
            if (*p == ';') p++;
        }
    }
    if (setprop && setval && *setval) {
        if (sb.n) sb_puts(&sb, "; ");
        sb_puts(&sb, setprop);
        sb_puts(&sb, ": ");
        sb_puts(&sb, setval);
    }
    dom_set_attr(node, "style", sb.b ? sb.b : "");
    if (sb.b) free(sb.b);
}
static void style_write(struct dom_node *node, const char *prop, const char *val) { style_rebuild(node, prop, val); }

#define STYLE_PROP(JSN, CSSN) \
static js_value style_get_##JSN(js_ctx *ctx, js_value t, js_value *args, int argc) { (void)args; (void)argc; return style_read(ctx, node_of(t), CSSN); } \
static js_value style_set_##JSN(js_ctx *ctx, js_value t, js_value *args, int argc) { style_write(node_of(t), CSSN, arg_str(ctx, A(0))); return js_undef(); }
STYLE_PROP(color, "color")
STYLE_PROP(background, "background")
STYLE_PROP(backgroundColor, "background-color")
STYLE_PROP(display, "display")
STYLE_PROP(width, "width")
STYLE_PROP(height, "height")
STYLE_PROP(fontWeight, "font-weight")
STYLE_PROP(fontFamily, "font-family")
STYLE_PROP(textAlign, "text-align")
STYLE_PROP(textDecoration, "text-decoration")
#undef STYLE_PROP

static js_value style_get_csstext(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    const char *s = n ? dom_get_attr(n, "style") : 0;
    (void)args; (void)argc;
    return js_mk_cstring(ctx, s ? s : "");
}
static js_value style_set_csstext(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    if (n) dom_set_attr(n, "style", arg_str(ctx, A(0)));
    return js_undef();
}
static js_value style_setprop(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    char name[64];
    const char *ns = arg_str(ctx, A(0));
    int i;
    for (i = 0; ns[i] && i < 63; i++) name[i] = ns[i];
    name[i] = '\0';
    style_write(n, name, arg_str(ctx, A(1)));
    return js_undef();
}
static js_value style_getprop(js_ctx *ctx, js_value t, js_value *args, int argc) {
    return style_read(ctx, node_of(t), arg_str(ctx, A(0)));
}
static js_value style_removeprop(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    js_value old = style_read(ctx, n, arg_str(ctx, A(0)));
    style_rebuild(n, arg_str(ctx, A(0)), 0);
    return old;
}

static js_value el_get_style(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_object *o = js_new_object(ctx, g_style_proto);
    (void)args; (void)argc;
    o->host = node_of(t);                        /* proxy the element's style attr */
    return js_obj_value(o);
}
/* el.dataset: a snapshot object of the element's data-* attributes, keyed in
 * camelCase (data-astro-prefetch -> astroPrefetch). Reads only (fresh per access). */
static js_value el_get_dataset(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    js_object *o;
    struct dom_attr *a;
    (void)args; (void)argc;
    gc_push_scope();
    o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    if (n) for (a = n->attrs; a; a = a->next) {
        if (starts_ci(a->name, "data-") && a->name[5]) {
            char key[64]; int ki = 0;
            const char *p = a->name + 5;
            while (*p && ki < 63) {
                if (*p == '-' && p[1]) { p++; key[ki++] = (p[0] >= 'a' && p[0] <= 'z') ? (char)(p[0] - 32) : p[0]; p++; }
                else key[ki++] = *p++;
            }
            key[ki] = '\0';
            js_def(o, key, js_mk_cstring(ctx, a->value ? a->value : ""), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
        }
    }
    gc_pop_scope();
    return js_obj_value(o);
}
static js_value win_getcomputedstyle(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_object *o = js_new_object(ctx, g_style_proto);
    (void)t;
    o->host = node_of(A(0));                      /* inline styles only (no cascade) */
    return js_obj_value(o);
}
/* window geometry: the content viewport size and the page's scroll offset. Paired
 * with element getBoundingClientRect (same viewport frame), so viewport-visibility
 * math (lazy-load / scroll-spy) works. */
static js_value win_innerWidth(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int w = 0, h = 0; (void)ctx; (void)t; (void)args; (void)argc;
    browser_viewport_px(&w, &h); return js_number(w);
}
static js_value win_innerHeight(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int w = 0, h = 0; (void)ctx; (void)t; (void)args; (void)argc;
    browser_viewport_px(&w, &h); return js_number(h);
}
static js_value win_scrollX(js_ctx *ctx, js_value t, js_value *args, int argc) { (void)ctx; (void)t; (void)args; (void)argc; return js_number(0); }
static js_value win_scrollY(js_ctx *ctx, js_value t, js_value *args, int argc) { (void)ctx; (void)t; (void)args; (void)argc; return js_number(browser_scroll_px()); }

static js_value el_noop(js_ctx *ctx, js_value t, js_value *args, int argc) { (void)ctx; (void)t; (void)args; (void)argc; return js_undef(); }
static js_value nav_false(js_ctx *ctx, js_value t, js_value *args, int argc) { (void)ctx; (void)t; (void)args; (void)argc; return js_bool(0); }
static js_value el_true(js_ctx *ctx, js_value t, js_value *args, int argc) { (void)ctx; (void)t; (void)args; (void)argc; return js_bool(1); }
static js_value el_empty_array(js_ctx *ctx, js_value t, js_value *args, int argc) { (void)t; (void)args; (void)argc; return js_obj_value(js_new_array(ctx, 0)); }

/* The Observer family (Mutation/Intersection/Resize/Performance): a constructor
 * returning an inert observer. We have no observation event loop, so observe()/
 * disconnect() are no-ops and the callback never fires — but constructing one (a
 * near-universal idiom on modern pages) no longer throws. */
static js_value observer_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    js_object *o;
    (void)thisv; (void)args; (void)argc;
    gc_push_scope();
    o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def_native(ctx, o, "observe", el_noop, 2);
    js_def_native(ctx, o, "unobserve", el_noop, 1);
    js_def_native(ctx, o, "disconnect", el_noop, 0);
    js_def_native(ctx, o, "takeRecords", el_empty_array, 0);
    gc_pop_scope();
    return js_obj_value(o);
}
static js_value win_matchmedia(js_ctx *ctx, js_value t, js_value *args, int argc) {
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
static js_value win_queuemicrotask(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)t;                                  /* no loop: run it synchronously now */
    if (argc >= 1 && js_is_callable(args[0])) js_call(ctx, args[0], js_undef(), 0, 0);
    return js_undef();
}

/* --- Content-Security-Policy hooks ----------------------------------------- *
 * Script execution, fetch()/WebSocket connections and dynamic code generation are
 * gated against the page's policy (B->csp). Violations log to the devtools console
 * (where CSP reports normally surface). */
static void js_csp_log(const char *what, const char *url, const char *reason)
{
    char m[URL_MAX + 160]; int n = 0;
    sapp(m, &n, sizeof m, "[CSP] refused to load ");
    sapp(m, &n, sizeof m, what); sapp(m, &n, sizeof m, " ");
    sapp(m, &n, sizeof m, url ? url : "");
    if (reason && reason[0]) { sapp(m, &n, sizeof m, " ("); sapp(m, &n, sizeof m, reason); sapp(m, &n, sizeof m, ")"); }
    sapp(m, &n, sizeof m, "\n");
    devtools_console_append(DT_ERROR, m, n);
}

/* Stub installed over the eval/Function globals when 'unsafe-eval' is absent. */
static js_value dyncode_blocked(js_ctx *ctx, js_value t, js_value *a, int n)
{
    (void)t; (void)a; (void)n;
    return js_throw_type(ctx, "code generation from strings blocked by Content-Security-Policy");
}

/* ------------------------------------------------------- fetch ------------- *
 * fetch(url, opts) runs a synchronous HTTP request on the worker (fits the
 * settle-immediately Promise model) and resolves with a Response, or rejects with
 * a TypeError on a connection error. Response.text()/json() return already-settled
 * Promises of the body. */
static js_value resp_text(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)args; (void)argc;
    return js_promise_resolved(ctx, js_get(ctx, t, "@body"));
}
static js_value resp_json(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_value body = js_get(ctx, t, "@body"), parsed, jsonobj, parsefn;
    (void)args; (void)argc;
    jsonobj = js_get(ctx, js_obj_value(ctx->global), "JSON");
    parsefn = js_get(ctx, jsonobj, "parse");
    if (!js_is_callable(parsefn)) return js_promise_rejected(ctx, js_mk_cstring(ctx, "no JSON.parse"));
    parsed = js_call(ctx, parsefn, jsonobj, &body, 1);
    if (ctx->has_exc) { js_value e = ctx->exc; ctx->has_exc = 0; ctx->exc_root = 0; return js_promise_rejected(ctx, e); }
    return js_promise_resolved(ctx, parsed);
}
static js_value resp_clone(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)ctx; (void)args; (void)argc;
    return t;                                 /* body isn't consumed here, so self works */
}
static js_value hdr_get(js_ctx *ctx, js_value t, js_value *args, int argc) {
    const char *name = arg_str(ctx, A(0));
    (void)argc;
    if (streq_ci(name, "content-type")) return js_get(ctx, t, "@ctype");
    return js_null();
}
static js_value hdr_has(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_value v = hdr_get(ctx, t, args, argc);
    return js_bool(v.type == JV_STR);
}
static js_value win_fetch(js_ctx *ctx, js_value t, js_value *args, int argc) {
    char url[URL_MAX], method[16], req_ctype[64], resp_ctype[160];
    char *reqbody = 0;
    int reqlen = 0, status = 0, outlen = 0;
    unsigned char *body;
    js_object *resp, *hdrs;
    js_value rv;
    (void)t;
    resolve_url(g_page_url_buf, arg_str(ctx, A(0)), url, sizeof url);
    if (B->csp) {                                  /* CSP connect-src */
        char use[URL_MAX], reason[64];
        if (!csp_check_url(B->csp, CSP_CONNECT, url, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
            js_csp_log("connection to", url, reason);
            return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Refused to connect (Content-Security-Policy)"));
        }
        copy_str(url, sizeof url, use);            /* use the upgraded URL */
    }
    method[0] = '\0'; req_ctype[0] = '\0';
    if (argc >= 2 && A(1).type == JV_OBJ) {
        js_value m = js_get(ctx, A(1), "method");
        js_value b = js_get(ctx, A(1), "body");
        js_value sig = js_get(ctx, A(1), "signal");
        if (m.type == JV_STR) copy_str(method, sizeof method, m.u.str->data);
        if (b.type == JV_STR) { reqbody = b.u.str->data; reqlen = b.u.str->len; copy_str(req_ctype, sizeof req_ctype, "text/plain;charset=UTF-8"); }
        /* An already-aborted signal short-circuits before touching the network
         * (the fetch runs synchronously, so this is the only cancellation point). */
        if (sig.type == JV_OBJ && js_to_boolean(js_get(ctx, sig, "aborted"))) {
            js_value reason = js_get(ctx, sig, "reason");
            if (reason.type == JV_UNDEF)
                reason = js_make_error(ctx, ctx->error_proto, "AbortError", "The operation was aborted");
            return js_promise_rejected(ctx, reason);
        }
    }
    body = browser_fetch_url(url, method[0] ? method : "GET",
                             (const unsigned char *)reqbody, reqlen, reqbody ? req_ctype : 0,
                             "*/*", &status, resp_ctype, sizeof resp_ctype, &outlen);
    if (!body)
        return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Failed to fetch"));
    gc_push_scope();
    resp = (js_object *)gc_keep(js_new_object(ctx, g_response_proto));
    js_def(resp, "@body", js_mk_string(ctx, (const char *)body, outlen), 0);
    free(body);
    js_def(resp, "status", js_number(status ? status : 200), P_ENUMERABLE);
    js_def(resp, "ok", js_bool(status >= 200 && status < 300), P_ENUMERABLE);
    js_def(resp, "statusText", js_mk_cstring(ctx, status == 200 ? "OK" : ""), P_ENUMERABLE);
    js_def(resp, "url", js_mk_cstring(ctx, url), P_ENUMERABLE);
    js_def(resp, "redirected", js_bool(0), P_ENUMERABLE);
    js_def(resp, "type", js_mk_cstring(ctx, "basic"), P_ENUMERABLE);
    hdrs = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(hdrs, "@ctype", js_mk_cstring(ctx, resp_ctype), 0);
    js_def_native(ctx, hdrs, "get", hdr_get, 1);
    js_def_native(ctx, hdrs, "has", hdr_has, 1);
    js_def(resp, "headers", js_obj_value(hdrs), P_ENUMERABLE);
    rv = js_promise_resolved(ctx, js_obj_value(resp));
    gc_pop_scope();
    return rv;
}

/* History API, wired to the browser's per-tab back/forward history. The JS side
 * runs on the worker, so back/forward/go just post a request the main thread acts
 * on (it owns the history + nav buttons); length reads it back. pushState/
 * replaceState update the address bar + the live location.href without navigating
 * (an SPA's soft URL change), then a real Back re-fetches the previous entry. */
static js_value his_back(js_ctx *ctx, js_value t, js_value *args, int argc) { (void)ctx; (void)t; (void)args; (void)argc; B->hist_go = -1; B->seq++; return js_undef(); }
static js_value his_forward(js_ctx *ctx, js_value t, js_value *args, int argc) { (void)ctx; (void)t; (void)args; (void)argc; B->hist_go = 1; B->seq++; return js_undef(); }
static js_value his_go(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int d = argc >= 1 ? (int)js_to_number(ctx, args[0]) : 0;
    (void)t;
    B->hist_go = d < 0 ? -1 : (d > 0 ? 1 : 0);
    if (B->hist_go) B->seq++;
    return js_undef();
}
static js_value his_length(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)t; (void)args; (void)argc;
    return js_number(B->hist_len > 0 ? B->hist_len : 1);
}
static void his_nav(js_ctx *ctx, js_value t, js_value *args, int argc, int replace) {
    if (argc >= 3 && args[2].type != JV_UNDEF && args[2].type != JV_NULL) {
        char url[URL_MAX];
        int k = 0;
        resolve_url(g_page_url_buf, arg_str(ctx, A(2)), url, sizeof url);
        { js_value loc = js_get(ctx, js_obj_value(ctx->global), "location"); js_set(ctx, loc, "href", js_mk_cstring(ctx, url)); }
        if (g_document) js_set(ctx, js_obj_value(g_document), "URL", js_mk_cstring(ctx, url));
        for (; url[k] && k < URL_MAX - 1; k++) B->hist_url[k] = url[k];
        B->hist_url[k] = '\0';
        B->hist_replace = replace;
        B->hist_nav = 1; B->seq++;
    }
    if (t.type == JV_OBJ) js_set(ctx, t, "state", argc >= 1 ? args[0] : js_null());
}
static js_value his_pushstate(js_ctx *ctx, js_value t, js_value *args, int argc) { his_nav(ctx, t, args, argc, 0); return js_undef(); }
static js_value his_replacestate(js_ctx *ctx, js_value t, js_value *args, int argc) { his_nav(ctx, t, args, argc, 1); return js_undef(); }

/* location.assign/replace/reload: a real navigation, posted to the main thread. */
static js_value loc_assign(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)t; (void)argc;
    resolve_url(g_page_url_buf, arg_str(ctx, A(0)), B->loc_url, sizeof B->loc_url);
    B->loc_nav = 1; B->seq++;
    return js_undef();
}
static js_value loc_reload(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)ctx; (void)t; (void)args; (void)argc;
    copy_str(B->loc_url, sizeof B->loc_url, g_page_url_buf);
    B->loc_nav = 1; B->seq++;
    return js_undef();
}
static js_value loc_tostring(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)args; (void)argc;
    return js_get(ctx, t, "href");
}
static js_value el_get_by_tag(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    js_object *arr;
    char tag[32];
    lower_into(tag, sizeof tag, arg_str(ctx, A(0)));
    gc_push_scope();
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    if (n) find_all(n, match_tag, tag, arr);
    gc_pop_scope();
    return js_obj_value(arr);
}
static js_value el_query(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    char buf[64];
    match_fn m = parse_selector(arg_str(ctx, A(0)), buf, sizeof buf);
    return n ? wrap_node(find_first(n, m, buf)) : js_null();
}
static js_value el_query_all(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    char buf[64];
    js_object *arr;
    match_fn m = parse_selector(arg_str(ctx, A(0)), buf, sizeof buf);
    gc_push_scope();
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    if (n) find_all(n, m, buf, arr);
    gc_pop_scope();
    return js_obj_value(arr);
}
static int node_matches_sel(struct dom_node *n, const char *sel) {
    char buf[64];
    match_fn m = parse_selector(sel, buf, sizeof buf);
    return n && n->type == DOM_ELEMENT && m(n, buf);
}
static js_value el_matches(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    return js_bool(n && node_matches_sel(n, arg_str(ctx, A(0))));
}
static js_value el_closest(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t);
    char sel[128];
    copy_str(sel, sizeof sel, arg_str(ctx, A(0)));
    for (; n; n = n->parent)
        if (n->type == DOM_ELEMENT && node_matches_sel(n, sel)) return wrap_node(n);
    return js_null();
}
static js_value el_is_same_node(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)ctx;
    return js_bool(node_of(t) && node_of(t) == node_of(A(0)));
}
static js_value el_contains(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = node_of(t), *o = node_of(A(0));
    (void)ctx;
    for (; o; o = o->parent) if (o == n) return js_bool(1);
    return js_bool(0);
}

/* ------------------------------------------------------- geometry natives - *
 * The browser is a line-flow renderer with no real box model, so element pixel
 * geometry is reconstructed from the wrapped-line table (browser_range_box, in
 * main.c). offsetWidth/offsetHeight/offsetTop/clientWidth etc. are document-
 * relative; getBoundingClientRect is viewport-relative (page scroll subtracted),
 * matching event.clientX/clientY.
 * All values are approximate: exact for a span on one text line, the content
 * column otherwise. */
static int node_box(js_value t, int *x, int *y, int *w, int *h) {
    struct dom_node *n = node_of(t);
    int s, e;
    *x = *y = *w = *h = 0;
    if (!n || !html_node_span(n, &s, &e)) return 0;
    return browser_range_box(s, e, x, y, w, h);
}
static js_value el_offsetWidth(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int x, y, w, h; (void)ctx; (void)args; (void)argc;
    node_box(t, &x, &y, &w, &h); return js_number(w);
}
static js_value el_offsetHeight(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int x, y, w, h; (void)ctx; (void)args; (void)argc;
    node_box(t, &x, &y, &w, &h); return js_number(h);
}
static js_value el_offsetTop(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int x, y, w, h; (void)ctx; (void)args; (void)argc;
    node_box(t, &x, &y, &w, &h); return js_number(y);
}
static js_value el_offsetLeft(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int x, y, w, h; (void)ctx; (void)args; (void)argc;
    node_box(t, &x, &y, &w, &h); return js_number(x);
}
static js_value el_scrollTop(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)ctx; (void)t; (void)args; (void)argc;
    return js_number(0);   /* elements aren't scroll containers; page scroll is window.scrollY */
}
static js_value el_get_bounding_rect(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int x = 0, y = 0, w = 0, h = 0; (void)args; (void)argc;
    node_box(t, &x, &y, &w, &h);
    int top = y - browser_scroll_px(), left = x;   /* document -> viewport (client) */
    gc_push_scope();
    {
        js_object *r = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
        unsigned f = P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE;
        js_def(r, "x",      js_number(left),     f);
        js_def(r, "y",      js_number(top),      f);
        js_def(r, "left",   js_number(left),     f);
        js_def(r, "top",    js_number(top),      f);
        js_def(r, "width",  js_number(w),        f);
        js_def(r, "height", js_number(h),        f);
        js_def(r, "right",  js_number(left + w), f);
        js_def(r, "bottom", js_number(top + h),  f);
        gc_pop_scope();
        return js_obj_value(r);
    }
}

/* ------------------------------------------------------- document natives - */

/* The root the document method was called on: a DOMParser document wraps its own
 * tree (host = parsed root); the live document falls back to g_doc_root. So every
 * query below works on either the page or a parsed off-document tree. */
static struct dom_node *doc_root_of(js_value t) {
    struct dom_node *r = node_of(t);
    return r ? r : g_doc_root;
}
static struct dom_node *doc_first_tag_in(struct dom_node *root, const char *tag) {
    return root ? find_first(root, match_tag, tag) : 0;
}
static struct dom_node *doc_first_tag(const char *tag) { return doc_first_tag_in(g_doc_root, tag); }

static js_value doc_get_by_id(js_ctx *ctx, js_value t, js_value *args, int argc) {
    return wrap_node(find_first(doc_root_of(t), match_id, arg_str(ctx, A(0))));
}
static js_value doc_get_by_tag(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_object *arr;
    char tag[32];
    lower_into(tag, sizeof tag, arg_str(ctx, A(0)));
    gc_push_scope();
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    find_all(doc_root_of(t), match_tag, tag, arr);
    gc_pop_scope();
    return js_obj_value(arr);
}
static js_value doc_query(js_ctx *ctx, js_value t, js_value *args, int argc) {
    char buf[64];
    match_fn m = parse_selector(arg_str(ctx, A(0)), buf, sizeof buf);
    return wrap_node(find_first(doc_root_of(t), m, buf));
}
static js_value doc_query_all(js_ctx *ctx, js_value t, js_value *args, int argc) {
    char buf[64];
    js_object *arr;
    match_fn m = parse_selector(arg_str(ctx, A(0)), buf, sizeof buf);
    gc_push_scope();
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    find_all(doc_root_of(t), m, buf, arr);
    gc_pop_scope();
    return js_obj_value(arr);
}
static js_value doc_create_el(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *n = dom_new_element(arg_str(ctx, A(0)));
    js_value wv = wrap_node(n);
    (void)t;
    if (n) { js_value cls = ce_lookup(n->tag); if (cls.type == JV_OBJ) ce_upgrade_node(n, cls); }
    return wv;
}
static js_value doc_create_text(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)ctx; (void)t;
    return wrap_node(dom_new_text(arg_str(ctx, A(0))));
}
static js_value doc_body(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *root = doc_root_of(t), *b = doc_first_tag_in(root, "body");
    (void)ctx; (void)args; (void)argc;
    return wrap_node(b ? b : root);
}
static js_value doc_head(js_ctx *ctx, js_value t, js_value *args, int argc) { (void)ctx; (void)args; (void)argc; return wrap_node(doc_first_tag_in(doc_root_of(t), "head")); }
static js_value doc_active_element(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *b = doc_first_tag_in(doc_root_of(t), "body");  /* no focus tracking */
    (void)ctx; (void)args; (void)argc;
    return b ? wrap_node(b) : js_null();
}
static js_value doc_root_el(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *root = doc_root_of(t), *h = doc_first_tag_in(root, "html");
    (void)ctx; (void)args; (void)argc;
    return wrap_node(h ? h : root);
}
static js_value doc_get_title(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *ti = doc_first_tag_in(doc_root_of(t), "title");
    (void)args; (void)argc;
    return ti ? text_content_value(ctx, ti) : js_mk_cstring(ctx, "");
}
static js_value doc_set_title(js_ctx *ctx, js_value t, js_value *args, int argc) {
    struct dom_node *root = doc_root_of(t), *ti = doc_first_tag_in(root, "title");
    if (!ti) {
        struct dom_node *head = doc_first_tag_in(root, "head");
        ti = dom_new_element("title");
        if (ti) dom_append_child(head ? head : root, ti);
    }
    if (ti) set_text_content(ti, arg_str(ctx, A(0)));
    return js_undef();
}

/* DOMParser: parse a string into an off-document tree wrapped as a Document (the
 * doc methods above are root-relative, so querySelector/body/... operate on it).
 * The tree is tracked and freed at page teardown. */
static js_value domparser_parse(js_ctx *ctx, js_value t, js_value *args, int argc) {
    const char *html = arg_str(ctx, A(0));
    struct dom_node *root;
    js_object *doc;
    js_value dv;
    (void)t;
    root = dom_parse(html, html ? (int)strlen(html) : 0);
    if (!root) return js_null();
    if (g_nparsed < 64) g_parsed[g_nparsed++] = root;       /* freed in js_page_end */
    gc_push_scope();
    doc = (js_object *)gc_keep(js_new_object(ctx, g_document_proto));
    doc->host = root;
    root->wrapper = doc;                                     /* wrap_node(root) == doc */
    dv = js_obj_value(doc);
    gc_pop_scope();
    return dv;
}
static js_value domparser_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    js_object *o;
    (void)thisv; (void)args; (void)argc;
    gc_push_scope();
    o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def_native(ctx, o, "parseFromString", domparser_parse, 2);
    gc_pop_scope();
    return js_obj_value(o);
}

/* ------------------------------------------------------- URL -------------- *
 * The WHATWG `URL` + `URLSearchParams` Web APIs, layered over the existing
 * curl/url.h parser (`parse_url`/`resolve_url`). Components are computed once at
 * construction and exposed as plain data properties; this is a snapshot model —
 * writing `url.pathname` does not re-serialize `url.href`, and `url.searchParams`
 * is a detached copy of the query (mutating it does not update the URL). That
 * covers the overwhelmingly common read path real pages use. */

static void copy_str(char *dst, int cap, const char *s) {
    int i = 0;
    for (; s[i] && i < cap - 1; i++) dst[i] = s[i];
    dst[i] = '\0';
}
static void span_copy(char *dst, int cap, const char *s, const char *e) {
    int n = (int)(e - s), i;
    if (n > cap - 1) n = cap - 1;
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) dst[i] = s[i];
    dst[i] = '\0';
}
static void uint_to_str(char *dst, unsigned v) {
    char tmp[12]; int i = 0, j = 0;
    if (v == 0) { dst[0] = '0'; dst[1] = '\0'; return; }
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) dst[j++] = tmp[--i];
    dst[j] = '\0';
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* percent-decode [s, s+len) into a fresh malloc'd string; '+' -> space */
static char *url_decode(const char *s, int len) {
    char *out = (char *)malloc((unsigned)(len > 0 ? len : 0) + 1);
    int i, j = 0;
    if (!out) return 0;
    for (i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len) {
            int h = hexval((unsigned char)s[i + 1]), l = hexval((unsigned char)s[i + 2]);
            if (h >= 0 && l >= 0) { out[j++] = (char)(h * 16 + l); i += 2; continue; }
        }
        out[j++] = s[i] == '+' ? ' ' : s[i];
    }
    out[j] = '\0';
    return out;
}
static int sp_unreserved(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == '*';
}
/* application/x-www-form-urlencoded component encoding (space -> '+') */
static void sb_put_form(sbuf *s, const char *t) {
    static const char *hex = "0123456789ABCDEF";
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if (c == ' ') sb_putc(s, '+');
        else if (sp_unreserved(c)) sb_putc(s, (char)c);
        else { sb_putc(s, '%'); sb_putc(s, hex[c >> 4]); sb_putc(s, hex[c & 15]); }
    }
}

static js_object *sp_arr(js_ctx *ctx, js_value t, const char *key) {
    js_value a = js_get(ctx, t, key);
    return a.type == JV_OBJ ? a.u.obj : 0;
}

/* parse a query string (no leading '?') into a fresh URLSearchParams object */
static js_value make_search_params(js_ctx *ctx, const char *query) {
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
        while (*amp && *amp != '&') amp++;
        eq = p;
        while (eq < amp && *eq != '=') eq++;
        if (amp > p) {
            char *k = url_decode(p, (int)(eq - p));
            char *v = url_decode(eq < amp ? eq + 1 : amp, eq < amp ? (int)(amp - eq - 1) : 0);
            if (k) { js_array_push(ctx, ka, js_mk_cstring(ctx, k)); free(k); }
            if (v) { js_array_push(ctx, va, js_mk_cstring(ctx, v)); free(v); }
        }
        p = *amp ? amp + 1 : amp;
    }
    gc_pop_scope();
    return js_obj_value(sp);
}

static js_value sp_get(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v");
    char name[256]; unsigned i, n;
    copy_str(name, sizeof name, arg_str(ctx, A(0)));
    if (!ka || !va) return js_null();
    n = js_array_len(ka);
    for (i = 0; i < n; i++) {
        js_value k = js_get_index(ctx, js_obj_value(ka), i);
        if (k.type == JV_STR && streq(k.u.str->data, name)) return js_get_index(ctx, js_obj_value(va), i);
    }
    return js_null();
}
static js_value sp_get_all(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v"), *out;
    char name[256]; unsigned i, n;
    copy_str(name, sizeof name, arg_str(ctx, A(0)));
    gc_push_scope();
    out = (js_object *)gc_keep(js_new_array(ctx, 0));
    if (ka && va) {
        n = js_array_len(ka);
        for (i = 0; i < n; i++) {
            js_value k = js_get_index(ctx, js_obj_value(ka), i);
            if (k.type == JV_STR && streq(k.u.str->data, name)) js_array_push(ctx, out, js_get_index(ctx, js_obj_value(va), i));
        }
    }
    gc_pop_scope();
    return js_obj_value(out);
}
static js_value sp_has(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_value v = sp_get(ctx, t, args, argc);
    return js_bool(v.type != JV_NULL);
}
static js_value sp_append(js_ctx *ctx, js_value t, js_value *args, int argc) {
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
static void sp_rebuild(js_ctx *ctx, js_value t, const char *name, int keepfirst, js_value repl) {
    js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v"), *nk, *nv;
    unsigned i, n;
    int seen = 0;
    if (!ka || !va) return;
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
    if (keepfirst && !seen) { js_array_push(ctx, nk, js_mk_cstring(ctx, name)); js_array_push(ctx, nv, repl); }
    js_def(t.u.obj, "@k", js_obj_value(nk), 0);
    js_def(t.u.obj, "@v", js_obj_value(nv), 0);
    gc_pop_scope();
}
static js_value sp_set(js_ctx *ctx, js_value t, js_value *args, int argc) {
    char name[256];
    js_value repl;
    copy_str(name, sizeof name, arg_str(ctx, A(0)));
    gc_push_scope();
    repl = js_to_string(ctx, A(1));
    if (repl.type == JV_STR) gc_keep(repl.u.str);
    sp_rebuild(ctx, t, name, 1, repl);
    gc_pop_scope();
    return js_undef();
}
static js_value sp_delete(js_ctx *ctx, js_value t, js_value *args, int argc) {
    char name[256];
    copy_str(name, sizeof name, arg_str(ctx, A(0)));
    sp_rebuild(ctx, t, name, 0, js_undef());
    return js_undef();
}
static js_value sp_for_each(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v");
    js_value cb = A(0), cbargs[3];
    unsigned i, n;
    if (!js_is_callable(cb) || !ka || !va) return js_undef();
    n = js_array_len(ka);
    for (i = 0; i < n; i++) {
        cbargs[0] = js_get_index(ctx, js_obj_value(va), i);   /* value */
        cbargs[1] = js_get_index(ctx, js_obj_value(ka), i);   /* key   */
        cbargs[2] = t;
        js_call(ctx, cb, js_undef(), cbargs, 3);
        if (ctx->has_exc) break;
    }
    return js_undef();
}
static js_value sp_tostring(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v");
    sbuf sb; js_value r; unsigned i, n;
    (void)args; (void)argc;
    sb.b = 0; sb.n = 0; sb.cap = 0;
    if (ka && va) {
        n = js_array_len(ka);
        for (i = 0; i < n; i++) {
            js_value k = js_get_index(ctx, js_obj_value(ka), i);
            js_value v = js_get_index(ctx, js_obj_value(va), i);
            if (sb.n) sb_putc(&sb, '&');
            sb_put_form(&sb, k.type == JV_STR ? k.u.str->data : "");
            sb_putc(&sb, '=');
            sb_put_form(&sb, v.type == JV_STR ? v.u.str->data : "");
        }
    }
    r = js_mk_cstring(ctx, sb.b ? sb.b : "");
    if (sb.b) free(sb.b);
    return r;
}
static js_value sp_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    const char *q = arg_str(ctx, A(0));
    (void)thisv;
    if (*q == '?') q++;
    return make_search_params(ctx, q);
}

/* locate the path/query/fragment of an absolute URL (pointers into `abs`) */
static void url_parts(const char *abs, const char **path_s, const char **path_e,
                      const char **query, const char **frag) {
    const char *p = abs;
    /* skip "scheme://" if present */
    while (*p && !(p[0] == ':' && p[1] == '/' && p[2] == '/')) {
        if (*p == '/' || *p == '?' || *p == '#') break;   /* no authority */
        p++;
    }
    if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
        p += 3;
        while (*p && *p != '/' && *p != '?' && *p != '#') p++;   /* past authority */
    } else {
        p = abs;
    }
    *path_s = p;
    while (*p && *p != '?' && *p != '#') p++;
    *path_e = p;
    if (*p == '?') { *query = p; while (*p && *p != '#') p++; } else *query = 0;
    *frag = (*p == '#') ? p : 0;
}

/* Populate `o` with the WHATWG URL component properties of absolute URL `abs`
 * (href/protocol/host/hostname/port/pathname/search/hash/origin + searchParams).
 * Shared by the URL constructor and window.location. Bad URLs yield blank parts. */
static void fill_url_props(js_ctx *ctx, js_object *o, const char *abs) {
    char protocol[16], host[URL_HOST_MAX + 8], port[12], origin[URL_HOST_MAX + 24];
    char pathbuf[URL_PATH_MAX], searchbuf[URL_PATH_MAX], hashbuf[URL_PATH_MAX];
    struct purl pu;
    const char *path_s, *path_e, *query, *frag;
    unsigned defport;
    sbuf href;
    if (!parse_url(abs, &pu)) { pu.tls = 0; pu.host[0] = '\0'; pu.port = 80; }

    copy_str(protocol, sizeof protocol, pu.tls ? "https:" : "http:");
    defport = pu.tls ? 443u : 80u;
    if ((unsigned)pu.port == defport) port[0] = '\0';
    else uint_to_str(port, (unsigned)pu.port);

    if (port[0]) { copy_str(host, sizeof host, pu.host); { int l = (int)strlen(host); host[l] = ':'; copy_str(host + l + 1, (int)sizeof host - l - 1, port); } }
    else copy_str(host, sizeof host, pu.host);

    url_parts(abs, &path_s, &path_e, &query, &frag);
    if (path_e > path_s) span_copy(pathbuf, sizeof pathbuf, path_s, path_e);
    else copy_str(pathbuf, sizeof pathbuf, "/");
    if (query) span_copy(searchbuf, sizeof searchbuf, query, frag ? frag : query + strlen(query));
    else searchbuf[0] = '\0';
    if (frag) copy_str(hashbuf, sizeof hashbuf, frag);
    else hashbuf[0] = '\0';

    { int l; copy_str(origin, sizeof origin, protocol); l = (int)strlen(origin); origin[l] = '/'; origin[l + 1] = '/'; copy_str(origin + l + 2, (int)sizeof origin - l - 2, host); }

    gc_push_scope();
    gc_keep(o);
    href.b = 0; href.n = 0; href.cap = 0;
    sb_puts(&href, origin); sb_puts(&href, pathbuf); sb_puts(&href, searchbuf); sb_puts(&href, hashbuf);
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
    js_def(o, "searchParams", make_search_params(ctx, searchbuf[0] == '?' ? searchbuf + 1 : searchbuf), P_ENUMERABLE | P_CONFIGURABLE);
    if (href.b) free(href.b);
    gc_pop_scope();
}

static js_value url_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    char input[URL_MAX], abs[URL_MAX];
    struct purl pu;
    js_object *o;
    js_value ov;
    (void)thisv;
    copy_str(input, sizeof input, arg_str(ctx, A(0)));
    if (argc >= 2 && A(1).type != JV_UNDEF && A(1).type != JV_NULL) {
        char base[URL_MAX];
        copy_str(base, sizeof base, arg_str(ctx, A(1)));
        resolve_url(base, input, abs, sizeof abs);
    } else {
        copy_str(abs, sizeof abs, input);
    }
    if (!parse_url(abs, &pu)) return js_throw_type(ctx, "Invalid URL");
    gc_push_scope();
    o = (js_object *)gc_keep(js_new_object(ctx, g_url_proto));
    fill_url_props(ctx, o, abs);
    ov = js_obj_value(o);
    gc_pop_scope();
    return ov;
}
static js_value url_tostring(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)args; (void)argc;
    return js_get(ctx, t, "href");
}

/* ------------------------------------------------------- DOM events -------- *
 * Listeners are stored on the target object (a node wrapper, the document, or
 * window) under a hidden own "@ev" array of {type, fn, capture} records. Dispatch
 * walks the node's ancestor chain to build the propagation path, then runs the
 * capture phase (outermost -> target), the target, and the bubble phase
 * (target -> outermost), honouring stopPropagation/preventDefault. The whole
 * thing runs on the worker thread (the only thread that touches the engine). */

static js_value report_exc_v(js_ctx *ctx);   /* fwd: defined with the runner below */
static void js_set_cur_script(const char *label);     /* error attribution (below)  */
static void js_set_event_script(const char *type);

/* The own "@ev" listener array of `target`, created on demand when `create`. */
static js_object *listeners_of(js_object *target, int create) {
    pentry *pe = pmap_find_pub(target, "@ev");
    if (pe && pe->value.type == JV_OBJ) return pe->value.u.obj;
    if (!create) return 0;
    {
        js_object *arr;
        gc_push_scope();
        arr = (js_object *)gc_keep(js_new_array(g_ctx, 0));
        js_def(target, "@ev", js_obj_value(arr), 0);
        gc_pop_scope();
        return arr;
    }
}

static int ev_flag(js_value ev, const char *k) {
    return js_to_boolean(js_get(g_ctx, ev, k));
}

/* Invoke the listeners on `obj` whose capture flag equals `want_capture` and whose
 * type matches the event's. currentTarget is set to `obj` for the duration. */
static void ev_fire_on(js_object *obj, js_value ev, int want_capture) {
    js_object *list = listeners_of(obj, 0);
    js_value evty;
    unsigned i, n;
    if (!list) return;
    gc_push_scope();
    gc_keep(list);                       /* a handler may replace @ev; pin this copy */
    js_set(g_ctx, ev, "currentTarget", js_obj_value(obj));
    evty = js_get(g_ctx, ev, "type");
    n = js_array_len(list);
    for (i = 0; i < n; i++) {
        js_value e = js_get_index(g_ctx, js_obj_value(list), i);
        js_value ty, fn, a;
        if (e.type != JV_OBJ) continue;
        if (js_to_boolean(js_get(g_ctx, e, "capture")) != want_capture) continue;
        ty = js_get(g_ctx, e, "type");
        if (!(ty.type == JV_STR && evty.type == JV_STR && streq(ty.u.str->data, evty.u.str->data))) continue;
        fn = js_get(g_ctx, e, "fn");
        if (!js_is_callable(fn)) continue;
        a = ev;
        js_set_event_script(evty.type == JV_STR ? evty.u.str->data : 0);
        js_call(g_ctx, fn, js_obj_value(obj), &a, 1);
        if (g_ctx->has_exc) report_exc_v(g_ctx);
    }
    gc_pop_scope();
}

/* Dispatch `ev` starting at wrapper `startobj` (its host node anchors the path).
 * Returns 1 if a listener called preventDefault. */
static int dispatch_from(js_object *startobj, js_value ev) {
    js_object *path[128];
    int np = 0, i;
    struct dom_node *target = (struct dom_node *)startobj->host;
    gc_push_scope();
    gc_keep(startobj);
    /* hold ev across the calls below */
    if (ev.type == JV_OBJ) gc_keep(ev.u.obj);
    js_set(g_ctx, ev, "target", js_obj_value(startobj));

    if (target) {
        for (struct dom_node *n = target; n && np < 126; n = n->parent)
            if (n->wrapper) path[np++] = (js_object *)n->wrapper;
    } else {
        path[np++] = startobj;            /* window/detached: just itself */
    }
    /* window (global) is the outermost target, after the document */
    if (g_ctx->global && np < 127 && path[np ? np - 1 : 0] != g_ctx->global)
        path[np++] = g_ctx->global;

    for (i = np - 1; i >= 1 && !ev_flag(ev, "@sp"); i--) ev_fire_on(path[i], ev, 1);   /* capture */
    if (!ev_flag(ev, "@sp")) { ev_fire_on(path[0], ev, 1); ev_fire_on(path[0], ev, 0); } /* target */
    for (i = 1; i < np && !ev_flag(ev, "@sp"); i++) ev_fire_on(path[i], ev, 0);          /* bubble  */

    gc_pop_scope();
    return ev_flag(ev, "@dp");
}

static js_value ev_prevent(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)args; (void)argc;
    if (t.type == JV_OBJ) { js_set(ctx, t, "@dp", js_bool(1)); js_set(ctx, t, "defaultPrevented", js_bool(1)); }
    return js_undef();
}
static js_value ev_stop(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)args; (void)argc;
    if (t.type == JV_OBJ) js_set(ctx, t, "@sp", js_bool(1));
    return js_undef();
}

/* Build an Event object on g_event_proto. `init` (or undefined) supplies bubbles/
 * cancelable/detail when present (CustomEvent / new Event(type, init)). */
static js_value make_event(const char *type, int x, int y, js_value init) {
    js_object *e;
    int bubbles = 1, cancelable = 1;
    js_value detail = js_undef();
    if (init.type == JV_OBJ) {
        js_value b = js_get(g_ctx, init, "bubbles");
        js_value c = js_get(g_ctx, init, "cancelable");
        if (b.type != JV_UNDEF) bubbles = js_to_boolean(b);
        if (c.type != JV_UNDEF) cancelable = js_to_boolean(c);
        detail = js_get(g_ctx, init, "detail");
    }
    gc_push_scope();
    if (init.type == JV_OBJ) gc_keep(init.u.obj);
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

static js_value el_add_listener(js_ctx *ctx, js_value t, js_value *args, int argc) {
    char type[64];
    js_value fn = A(1);
    int capture = 0;
    if (t.type != JV_OBJ || !js_is_callable(fn)) return js_undef();
    copy_str(type, sizeof type, arg_str(ctx, A(0)));
    if (argc >= 3) {
        js_value o = A(2);
        if (o.type == JV_BOOL) capture = o.u.boolean;
        else if (o.type == JV_OBJ) capture = js_to_boolean(js_get(ctx, o, "capture"));
    }
    gc_push_scope();
    gc_keep(t.u.obj);
    if (fn.type == JV_OBJ) gc_keep(fn.u.obj);
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
static js_value el_remove_listener(js_ctx *ctx, js_value t, js_value *args, int argc) {
    char type[64];
    js_value fn = A(1);
    js_object *list, *nl;
    unsigned i, n;
    int capture = 0;
    if (t.type != JV_OBJ) return js_undef();
    copy_str(type, sizeof type, arg_str(ctx, A(0)));
    if (argc >= 3) {
        js_value o = A(2);
        if (o.type == JV_BOOL) capture = o.u.boolean;
        else if (o.type == JV_OBJ) capture = js_to_boolean(js_get(ctx, o, "capture"));
    }
    list = listeners_of(t.u.obj, 0);
    if (!list) return js_undef();
    gc_push_scope();
    gc_keep(t.u.obj); gc_keep(list);
    if (fn.type == JV_OBJ) gc_keep(fn.u.obj);
    nl = (js_object *)gc_keep(js_new_array(ctx, 0));
    n = js_array_len(list);
    for (i = 0; i < n; i++) {
        js_value e = js_get_index(ctx, js_obj_value(list), i);
        js_value ty = js_get(ctx, e, "type"), ef = js_get(ctx, e, "fn");
        int ec = js_to_boolean(js_get(ctx, e, "capture"));
        int match = ty.type == JV_STR && streq(ty.u.str->data, type) &&
                    js_strict_eq(ef, fn) && ec == capture;
        if (!match) js_array_push(ctx, nl, e);
    }
    js_def(t.u.obj, "@ev", js_obj_value(nl), 0);
    gc_pop_scope();
    return js_undef();
}
static js_value el_dispatch(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_value ev = A(0);
    (void)argc;
    if (t.type != JV_OBJ || ev.type != JV_OBJ) return js_bool(1);
    return js_bool(!dispatch_from(t.u.obj, ev));
}
static js_value event_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    (void)thisv;
    return make_event(arg_str(ctx, A(0)), 0, 0, A(1));
}

/* C entry: the worker resolved a content click to `target`; fire `type` on it.
 * Returns 1 if a listener called preventDefault (so navigation is suppressed). */
int js_dispatch_dom_event(struct dom_node *target, const char *type, int x, int y) {
    js_value tgt, ev;
    int prevented;
    if (!g_page_live || !target) return 0;
    gc_push_scope();
    tgt = wrap_node(target);
    if (tgt.type != JV_OBJ) { gc_pop_scope(); return 0; }
    gc_keep(tgt.u.obj);
    ev = make_event(type, x, y, js_undef());
    if (ev.type == JV_OBJ) gc_keep(ev.u.obj);
    prevented = dispatch_from(tgt.u.obj, ev);
    gc_pop_scope();
    return prevented;
}

int js_has_listeners(void) { return g_page_live && g_has_listeners; }

/* ------------------------------------------------------- web components ---- *
 * customElements.define(name, class) registers a tag; HTMLElement is the base a
 * custom class extends. Upgrading a node means pointing its wrapper's prototype at
 * the class's, running the constructor ON that wrapper (the engine inits fields +
 * runs super() on the given `this`), then firing connectedCallback when it's in
 * the tree. Existing matching nodes upgrade at define time; createElement upgrades
 * a defined tag; appendChild fires connectedCallback as nodes connect. */

static js_value htmlelement_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    (void)ctx; (void)thisv; (void)args; (void)argc;        /* super(): a no-op base */
    return js_undef();
}

static int ce_is_upgraded(js_object *w) { return w && pmap_find_pub(w, "@ce") != 0; }
static int ce_connected(struct dom_node *n) {
    for (; n; n = n->parent) if (n == g_doc_root) return 1;
    return 0;
}
static js_value ce_lookup(const char *tag) {
    pentry *pe = g_custom_elements ? pmap_find_pub(g_custom_elements, tag) : 0;
    return pe ? pe->value : js_undef();
}
static void ce_call_cb(js_object *w, const char *cb, js_value *args, int n) {
    js_value f;
    if (!w) return;
    f = js_get(g_ctx, js_obj_value(w), cb);
    if (!js_is_callable(f)) return;
    gc_push_scope();
    gc_keep(w);
    { int i; for (i = 0; i < n; i++) if (args[i].type == JV_OBJ) gc_keep(args[i].u.obj); }
    js_set_cur_script(cb);                          /* the lifecycle callback name */
    js_call(g_ctx, f, js_obj_value(w), args, n);
    if (g_ctx->has_exc) report_exc_v(g_ctx);
    gc_pop_scope();
}

/* Upgrade `n` to its custom class `cls`: retarget the wrapper's prototype and run
 * the constructor on it. Idempotent. Does not fire connectedCallback. */
static void ce_upgrade_node(struct dom_node *n, js_value cls) {
    js_object *w;
    js_value wv, proto;
    if (!n || cls.type != JV_OBJ) return;
    wv = wrap_node(n);
    if (wv.type != JV_OBJ) return;
    w = wv.u.obj;
    if (ce_is_upgraded(w)) return;
    gc_push_scope();
    gc_keep(w); gc_keep(cls.u.obj);
    proto = js_get(g_ctx, cls, "prototype");
    if (proto.type == JV_OBJ) w->proto = proto.u.obj;
    js_def(w, "@ce", js_bool(1), 0);
    js_set_cur_script("<custom element constructor>");
    js_call(g_ctx, cls, wv, 0, 0);                 /* constructor + fields + super() */
    if (g_ctx->has_exc) report_exc_v(g_ctx);
    gc_pop_scope();
    /* spec order: after the constructor, attributeChangedCallback for each observed
     * attribute already present (ce_attr_changed filters to the observed set). */
    { struct dom_attr *a; for (a = n->attrs; a; a = a->next) ce_attr_changed(n, a->name, 0, a->value); }
}
/* Fire connectedCallback once, when an upgraded element is in the document. */
static void ce_connect(struct dom_node *n) {
    js_object *w = n ? (js_object *)n->wrapper : 0;
    if (!w || !ce_is_upgraded(w) || pmap_find_pub(w, "@conn")) return;
    if (!ce_connected(n)) return;
    js_def(w, "@conn", js_bool(1), 0);
    ce_call_cb(w, "connectedCallback", 0, 0);
}
static void ce_upgrade_connect(struct dom_node *n, js_value cls) {
    ce_upgrade_node(n, cls);
    ce_connect(n);
}
static void ce_upgrade_tree(struct dom_node *root, const char *name, js_value cls) {
    struct dom_node *c;
    for (c = root->first_child; c; c = c->next) {
        if (c->type == DOM_ELEMENT) {
            if (streq(c->tag, name)) ce_upgrade_connect(c, cls);
            ce_upgrade_tree(c, name, cls);
        }
    }
}

/* setAttribute hook: fire attributeChangedCallback for an observed attribute. */
static void ce_attr_changed(struct dom_node *n, const char *name, const char *oldv, const char *newv) {
    js_object *w = n ? (js_object *)n->wrapper : 0;
    js_value ctor, obs;
    unsigned i, len;
    int observed = 0;
    if (!w || !ce_is_upgraded(w)) return;
    ctor = js_get(g_ctx, js_obj_value(w), "constructor");
    if (ctor.type != JV_OBJ) return;
    obs = js_get(g_ctx, ctor, "observedAttributes");
    if (!js_is_array(obs)) return;
    len = js_array_len(obs.u.obj);
    for (i = 0; i < len; i++) {
        js_value e = js_get_index(g_ctx, obs, i);
        if (e.type == JV_STR && streq(e.u.str->data, name)) { observed = 1; break; }
    }
    if (!observed) return;
    gc_push_scope();
    {
        js_value a[3];
        a[0] = js_mk_cstring(g_ctx, name); gc_keep(a[0].u.str);
        a[1] = oldv ? js_mk_cstring(g_ctx, oldv) : js_null(); if (a[1].type == JV_STR) gc_keep(a[1].u.str);
        a[2] = newv ? js_mk_cstring(g_ctx, newv) : js_null(); if (a[2].type == JV_STR) gc_keep(a[2].u.str);
        ce_call_cb(w, "attributeChangedCallback", a, 3);
    }
    gc_pop_scope();
}

static js_value ce_define(js_ctx *ctx, js_value t, js_value *args, int argc) {
    char name[64];
    js_value cls = A(1);
    (void)t;
    copy_str(name, sizeof name, arg_str(ctx, A(0)));
    if (!name[0] || !js_is_callable(cls)) return js_undef();
    js_def(g_custom_elements, name, cls, 0);
    if (g_doc_root) ce_upgrade_tree(g_doc_root, name, cls);
    return js_undef();
}
static js_value ce_get(js_ctx *ctx, js_value t, js_value *args, int argc) {
    char name[64];
    pentry *pe;
    (void)t;
    copy_str(name, sizeof name, arg_str(ctx, A(0)));
    pe = pmap_find_pub(g_custom_elements, name);
    return pe ? pe->value : js_undef();
}
static js_value ce_when_defined(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)t; (void)args; (void)argc;        /* define is synchronous: already settled */
    return js_promise_resolved(ctx, js_undef());
}

/* ------------------------------------------------------- ES modules -------- *
 * The engine calls browser_load_module() for `import` (static + dynamic). It
 * resolves the specifier against the importing module (or the page), fetches the
 * source over the same batch path scripts/CSS use, parses + executes it ONCE in a
 * module scope, and caches the namespace by absolute URL. Synchronous — which fits
 * the engine's no-event-loop model; dynamic import() just wraps the result in an
 * already-settled Promise. Only URL-like specifiers resolve (no import maps). */

static js_value report_exc_v(js_ctx *ctx);   /* (defined with the runner below) */

/* Keep a parsed script's arena alive for the page (closures reference its AST).
 * The list grows on demand — a page may load any number of scripts/modules. */
static int arenas_push(js_arena *ar) {
    if (g_narenas == g_arenas_cap) {
        int nc = g_arenas_cap ? g_arenas_cap * 2 : 16;
        js_arena **na = (js_arena **)realloc(g_arenas, (unsigned)nc * sizeof *na);
        if (!na) return 0;
        g_arenas = na; g_arenas_cap = nc;
    }
    g_arenas[g_narenas++] = ar;
    return 1;
}

static js_node *module_compile(const char *code, const char *url) {
    js_arena *ar;
    js_node *prog;
    js_parse_error pe;
    ar = (js_arena *)malloc(sizeof *ar);
    if (!ar) return 0;
    js_arena_init(ar);
    prog = js_parse(code, ar, &pe);
    if (!prog) {
        sbuf sb; sb.b = 0; sb.n = 0; sb.cap = 0;
        sb_puts(&sb, "[js] module SyntaxError in ");
        sb_puts(&sb, url ? url : "(inline)");
        if (pe.line > 0) { char ln[16]; int li = 0; uapp(ln, &li, sizeof ln, (unsigned)pe.line); sb_putc(&sb, ':'); sb_puts(&sb, ln); }
        sb_puts(&sb, ": ");
        sb_puts(&sb, pe.msg ? pe.msg : "parse error");
        sb_putc(&sb, '\n');
        devtools_console_append(DT_ERROR, sb.b, sb.n);
        if (sb.b) free(sb.b);
        js_arena_free(ar); free(ar);
        return 0;
    }
    if (!arenas_push(ar)) { js_arena_free(ar); free(ar); return 0; }
    return prog;
}

static js_object *browser_load_module(js_ctx *ctx, const char *spec, void *ud) {
    char abs[URL_MAX];
    const char *referrer = ctx->cur_module_url ? ctx->cur_module_url : g_page_url_buf;
    pentry *cached;
    unsigned char *bodies[1];
    int lens[1];
    char *urls[1];
    js_node *prog;
    js_object *exports;
    char *code;
    (void)ud;
    resolve_url(referrer, spec, abs, sizeof abs);
    cached = pmap_find_pub(g_module_cache, abs);
    if (cached && cached->value.type == JV_OBJ) return cached->value.u.obj;
    if (!g_fetch) return 0;
    urls[0] = abs; bodies[0] = 0; lens[0] = 0;
    g_fetch(urls, 1, bodies, lens);
    if (!bodies[0] || lens[0] <= 0) { if (bodies[0]) free(bodies[0]); return 0; }
    code = (char *)malloc((unsigned)lens[0] + 1);
    if (!code) { free(bodies[0]); return 0; }
    memcpy(code, bodies[0], (unsigned)lens[0]); code[lens[0]] = 0;
    free(bodies[0]);
    prog = module_compile(code, abs);
    free(code);
    if (!prog) return 0;
    gc_push_scope();
    exports = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(g_module_cache, abs, js_obj_value(exports), 0);   /* cache before exec (cycles) */
    js_run_module(ctx, prog, exports, abs);
    if (ctx->has_exc) report_exc_v(ctx);
    gc_pop_scope();
    return exports;
}

/* ------------------------------------------------------- setup ------------ */

static void def_method(js_object *o, const char *name, js_native fn, int arity) { js_def_native(g_ctx, o, name, fn, arity); }
static void def_acc(js_object *proto, const char *name, js_native get, js_native set) {
    gc_push_scope();
    {
        js_object *g = get ? (js_object *)gc_keep(js_new_native(g_ctx, get, name, 0)) : 0;
        js_object *s = set ? (js_object *)gc_keep(js_new_native(g_ctx, set, name, 1)) : 0;
        js_def_accessor(proto, name, g, s, P_CONFIGURABLE | P_ENUMERABLE);
    }
    gc_pop_scope();
}

/* ------------------------------------------------------- timers ----------- */

static void report_exc(js_ctx *ctx);   /* defined below, near the page runner */

/* Register a timer; returns its id (slot+1), or 0 if the table is full or the
 * callback isn't callable. `raf` requests requestAnimationFrame semantics (the
 * callback is invoked with a single timestamp argument). */
static int timer_add(js_ctx *ctx, js_value cb, js_value *extra, int nextra, int delay, int repeat, int raf) {
    int i, k;
    if (!g_timers || !js_is_callable(cb)) return 0;
    for (i = 0; i < g_tcap; i++) if (!g_tmeta[i].active) break;
    if (i == g_tcap) {                                   /* no free slot: grow the table */
        int nc = g_tcap ? g_tcap * 2 : 16;
        struct timer_meta *nt = (struct timer_meta *)realloc(g_tmeta, (unsigned)nc * sizeof *nt);
        if (!nt) return 0;
        for (k = g_tcap; k < nc; k++) { nt[k].active = nt[k].repeat = nt[k].raf = 0; nt[k].due = nt[k].period = 0; }
        g_tmeta = nt; g_tcap = nc;
    }
    gc_push_scope();
    {
        js_object *bundle = (js_object *)gc_keep(js_new_array(ctx, 0));
        js_array_push(ctx, bundle, cb);                 /* [0] = callback           */
        for (k = 0; k < nextra; k++) js_array_push(ctx, bundle, extra[k]);  /* [1..] args */
        js_set_index(ctx, js_obj_value(g_timers), (unsigned)i, js_obj_value(bundle));
    }
    gc_pop_scope();
    g_tmeta[i].active = 1;
    g_tmeta[i].repeat = repeat ? 1 : 0;
    g_tmeta[i].raf = raf ? 1 : 0;
    g_tmeta[i].period = delay < 0 ? 0 : delay;
    g_tmeta[i].due = now_ms() + g_tmeta[i].period;
    g_ntimers++;
    return i + 1;
}
static void timer_drop(int i) {
    if (i < 0 || i >= g_tcap || !g_tmeta[i].active) return;
    g_tmeta[i].active = 0;
    if (g_timers) js_set_index(g_ctx, js_obj_value(g_timers), (unsigned)i, js_null());
    if (g_ntimers > 0) g_ntimers--;
}
static js_value win_set_timeout(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int delay = argc > 1 ? (int)js_to_number(ctx, args[1]) : 0;
    (void)t;
    return js_number(timer_add(ctx, A(0), args + (argc > 2 ? 2 : argc), argc > 2 ? argc - 2 : 0, delay, 0, 0));
}
static js_value win_set_interval(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int delay = argc > 1 ? (int)js_to_number(ctx, args[1]) : 0;
    (void)t;
    return js_number(timer_add(ctx, A(0), args + (argc > 2 ? 2 : argc), argc > 2 ? argc - 2 : 0, delay, 1, 0));
}
static js_value win_request_anim(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)t;
    return js_number(timer_add(ctx, A(0), 0, 0, 16, 0, 1));   /* ~one 60fps frame */
}
static js_value win_clear_timer(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)ctx; (void)t;
    if (argc > 0 && args[0].type == JV_NUM) timer_drop((int)args[0].u.num - 1);
    return js_undef();
}

/* Fire every timer whose deadline has passed. Called by the worker each idle
 * tick (browser engine is worker-only). Returns the number of callbacks fired
 * so the caller can repaint/relayout if any of them mutated the DOM. */
int js_timers_pump(void) {
    int i, fired = 0;
    long long now;
    if (!g_page_live || !g_ctx || g_ntimers == 0) return 0;
    now = now_ms();
    for (i = 0; i < g_tcap; i++) {
        js_value bundle, cb, argv[8];
        int nargs = 0;
        if (!g_tmeta[i].active || g_tmeta[i].due > now) continue;
        bundle = js_get_index(g_ctx, js_obj_value(g_timers), (unsigned)i);
        if (bundle.type != JV_OBJ) { timer_drop(i); continue; }
        gc_push_scope();
        gc_keep(bundle.u.obj);
        cb = js_get_index(g_ctx, bundle, 0);
        if (g_tmeta[i].raf) {
            argv[nargs++] = js_number((double)now);
        } else {
            unsigned bl = js_array_len(bundle.u.obj), k;
            for (k = 1; k < bl && nargs < 8; k++) argv[nargs++] = js_get_index(g_ctx, bundle, k);
        }
        if (g_tmeta[i].repeat)
            g_tmeta[i].due = now + g_tmeta[i].period;   /* reschedule from now (no drift pileup) */
        else
            timer_drop(i);                              /* one-shot: free the slot before firing */
        if (js_is_callable(cb)) {
            js_set_cur_script(g_tmeta[i].raf ? "<requestAnimationFrame callback>" : "<timer callback>");
            js_call(g_ctx, cb, js_obj_value(g_ctx->global), argv, nargs);
            if (g_ctx->has_exc) report_exc(g_ctx);
        }
        gc_pop_scope();
        fired++;
    }
    return fired;
}

/* ------------------------------------------------------- AbortController -- *
 * AbortController/AbortSignal (DOM standard). A signal is a plain EventTarget on
 * g_abortsignal_proto, so addEventListener/removeEventListener/dispatchEvent come
 * from the shared @ev listener API. abort() flips `aborted`, records `reason`, and
 * fires "abort" exactly once (onabort handler + listeners). fetch() consults an
 * already-aborted signal up front and rejects with the reason — the request runs
 * synchronously on the worker, so there is no mid-flight cancellation point. */

/* A fresh, not-yet-aborted AbortSignal. */
static js_object *new_abort_signal(js_ctx *ctx) {
    js_object *s = js_new_object(ctx, g_abortsignal_proto);
    js_def(s, "aborted", js_bool(0), P_WRITABLE | P_ENUMERABLE);
    js_def(s, "reason", js_undef(), P_WRITABLE | P_ENUMERABLE);
    js_def(s, "onabort", js_null(), P_WRITABLE | P_ENUMERABLE);
    return s;
}

/* Abort `signal` with `reason` (an AbortError when none is given), once: set the
 * state then fire "abort" at the onabort handler and any listeners. */
static void signal_abort(js_ctx *ctx, js_object *signal, js_value reason) {
    js_value ev, h;
    if (!signal || js_to_boolean(js_get(ctx, js_obj_value(signal), "aborted"))) return;
    gc_push_scope();
    gc_keep(signal);
    if (reason.type == JV_UNDEF)
        reason = js_make_error(ctx, ctx->error_proto, "AbortError", "signal is aborted without reason");
    if (reason.type == JV_OBJ) gc_keep(reason.u.obj);
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
        if (ctx->has_exc) report_exc(ctx);
    }
    ev_fire_on(signal, ev, 0);
    gc_pop_scope();
}

static js_value sig_throw_if_aborted(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)args; (void)argc;
    if (t.type == JV_OBJ && js_to_boolean(js_get(ctx, t, "aborted")))
        js_throw(ctx, js_get(ctx, t, "reason"));
    return js_undef();
}

/* AbortSignal is not directly constructable; use AbortSignal.abort()/.timeout()
 * or an AbortController's `.signal`. */
static js_value abortsignal_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    (void)thisv; (void)args; (void)argc;
    return js_throw_type(ctx, "Illegal constructor");
}

/* Static AbortSignal.abort(reason?) -> an already-aborted signal. */
static js_value sig_static_abort(js_ctx *ctx, js_value t, js_value *args, int argc) {
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

static js_value abortcontroller_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    js_object *self, *sig;
    js_value rv;
    (void)thisv; (void)args; (void)argc;       /* native ctor builds its own object */
    gc_push_scope();
    self = (js_object *)gc_keep(js_new_object(ctx, g_abortcontroller_proto));
    sig = (js_object *)gc_keep(new_abort_signal(ctx));
    js_def(self, "signal", js_obj_value(sig), P_ENUMERABLE);
    rv = js_obj_value(self);
    gc_pop_scope();
    return rv;
}

static js_value ctrl_abort(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_value sig;
    (void)argc;
    if (t.type != JV_OBJ) return js_undef();
    sig = js_get(ctx, t, "signal");
    if (sig.type == JV_OBJ) signal_abort(ctx, sig.u.obj, A(0));
    return js_undef();
}

/* ------------------------------------------------------- WebSocket -------- */
/* A WebSocket wrapper's `host` slot points at the ws_conn (NULL once closed or
 * if the connect failed). Every wrapper is pinned in g_websockets so it stays
 * GC-live and the pump can reach it. The library blocks only for the connect +
 * handshake (in the constructor, on the worker thread — same as fetch); after
 * that the socket is polled non-blocking by js_websockets_pump each idle tick,
 * which turns inbound frames into open/message/close/error events. */

static ws_conn *ws_of(js_value t) {
    return (t.type == JV_OBJ) ? (ws_conn *)t.u.obj->host : 0;
}

/* Fire `type` on a WebSocket wrapper: the on<type> handler property and any
 * addEventListener listeners. For "message", `data` becomes event.data. */
static void ws_emit(js_object *w, const char *type, js_value data) {
    js_value ev;
    char on[24];
    gc_push_scope();
    gc_keep(w);
    ev = make_event(type, 0, 0, js_undef());
    if (ev.type == JV_OBJ) {
        gc_keep(ev.u.obj);
        js_set(g_ctx, ev, "target", js_obj_value(w));
        js_set(g_ctx, ev, "currentTarget", js_obj_value(w));
        if (data.type != JV_UNDEF) js_def(ev.u.obj, "data", data, P_WRITABLE | P_ENUMERABLE);
    }
    on[0] = 'o'; on[1] = 'n'; copy_str(on + 2, (int)sizeof on - 2, type);
    {
        js_value h = js_get(g_ctx, js_obj_value(w), on);
        if (js_is_callable(h)) {
            js_value a = ev;
            js_set_event_script(type);
            js_call(g_ctx, h, js_obj_value(w), &a, 1);
            if (g_ctx->has_exc) report_exc(g_ctx);
        }
    }
    ev_fire_on(w, ev, 0);                       /* addEventListener listeners */
    gc_pop_scope();
}

static js_value ws_send_m(js_ctx *ctx, js_value t, js_value *args, int argc) {
    ws_conn *c = ws_of(t);
    js_value d = A(0);
    (void)argc;
    if (!c || ws_state(c) != WS_OPEN) return js_undef();
    if (d.type == JV_STR) {
        ws_send(c, d.u.str->data, d.u.str->len, 1);
    } else {                                    /* no ArrayBuffer yet: coerce to text */
        js_value s = js_to_string(ctx, d);
        if (s.type == JV_STR) ws_send(c, s.u.str->data, s.u.str->len, 1);
    }
    return js_undef();
}

static js_value ws_close_m(js_ctx *ctx, js_value t, js_value *args, int argc) {
    ws_conn *c = ws_of(t);
    int code = (argc > 0 && args[0].type == JV_NUM) ? (int)args[0].u.num : 1000;
    const char *reason = (argc > 1) ? arg_str(ctx, A(1)) : "";
    if (c && ws_state(c) != WS_CLOSED) {
        ws_close(c, code, reason);
        js_set(ctx, t, "readyState", js_number(WS_CLOSING));
    }
    return js_undef();
}

static js_value ws_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    const char *url = arg_str(ctx, A(0));
    char protos[256];
    ws_conn *c;
    js_object *o;
    js_value ov;
    (void)thisv;
    protos[0] = 0;
    if (argc > 1) {                             /* a subprotocol string or array */
        js_value p = A(1);
        if (p.type == JV_STR) {
            copy_str(protos, sizeof protos, p.u.str->data);
        } else if (p.type == JV_OBJ) {
            unsigned n = js_array_len(p.u.obj), i;
            for (i = 0; i < n; i++) {
                const char *s = arg_str(ctx, js_get_index(ctx, p, i));
                if (!*s) continue;
                if (protos[0]) strcat(protos, ",");
                strcat(protos, s);
            }
        }
    }

    gc_push_scope();
    o = (js_object *)gc_keep(js_new_object(ctx, g_ws_proto));
    int ws_blocked = 0;
    if (B->csp) {                               /* CSP connect-src (ws->http for matching) */
        char norm[URL_MAX], use[URL_MAX], reason[64]; int k = 0; norm[0] = '\0';
        if (starts_ci(url, "wss://"))     { sapp(norm, &k, sizeof norm, "https://"); sapp(norm, &k, sizeof norm, url + 6); }
        else if (starts_ci(url, "ws://")) { sapp(norm, &k, sizeof norm, "http://");  sapp(norm, &k, sizeof norm, url + 5); }
        else                              { sapp(norm, &k, sizeof norm, url); }
        if (!csp_check_url(B->csp, CSP_CONNECT, norm, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
            js_csp_log("WebSocket to", url, reason);
            ws_blocked = 1;
        }
    }
    c = ws_blocked ? 0 : ws_open(url, protos[0] ? protos : 0);   /* blocks for connect + handshake */
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
    js_def(o, "@open_sent", js_bool(0), P_WRITABLE);   /* "open" delivered (pump)   */
    js_def(o, "@dead", js_bool(c ? 0 : 1), P_WRITABLE);/* connect failed: emit error*/
    if (g_websockets) js_array_push(ctx, g_websockets, js_obj_value(o));
    ov = js_obj_value(o);
    gc_pop_scope();
    return ov;
}

/* Drive every live WebSocket: deliver the deferred "open", drain inbound
 * messages, and surface a close/error. Returns the number of events fired so the
 * worker can relayout/repaint when a handler mutated the DOM. */
int js_websockets_pump(void) {
    int fired = 0;
    unsigned i, n;
    if (!g_page_live || !g_ctx || !g_websockets) return 0;
    n = js_array_len(g_websockets);
    for (i = 0; i < n; i++) {
        js_value wv = js_get_index(g_ctx, js_obj_value(g_websockets), i);
        js_object *w;
        ws_conn *c;
        if (wv.type != JV_OBJ) continue;
        w = wv.u.obj;
        c = (ws_conn *)w->host;

        if (!c) {                               /* failed connect, or already closed */
            if (js_to_boolean(js_get(g_ctx, wv, "@dead"))) {
                js_set(g_ctx, wv, "@dead", js_bool(0));
                js_set(g_ctx, wv, "readyState", js_number(WS_CLOSED));
                ws_emit(w, "error", js_undef());
                ws_emit(w, "close", js_undef());
                fired++;
            }
            continue;
        }

        if (!js_to_boolean(js_get(g_ctx, wv, "@open_sent"))) {
            js_set(g_ctx, wv, "@open_sent", js_bool(1));
            js_set(g_ctx, wv, "readyState", js_number(WS_OPEN));
            ws_emit(w, "open", js_undef());
            fired++;
            c = (ws_conn *)w->host;             /* a handler may have closed it */
            if (!c) continue;
        }

        for (;;) {
            const uint8_t *data = 0;
            int dlen = 0, is_text = 0, ev;
            ev = ws_poll(c, &data, &dlen, &is_text);
            if (ev == WS_EV_MESSAGE) {
                ws_emit(w, "message", js_mk_string(g_ctx, (const char *)data, dlen));
                fired++;
                if (w->host != c) break;        /* handler closed it mid-drain */
                continue;
            }
            if (ev == WS_EV_CLOSE || ev == WS_EV_ERROR) {
                if (ev == WS_EV_ERROR) ws_emit(w, "error", js_undef());
                js_set(g_ctx, wv, "readyState", js_number(WS_CLOSED));
                ws_emit(w, "close", js_undef());
                ws_free(c);
                w->host = 0;
                fired++;
            }
            break;                              /* WS_EV_NONE, or terminal */
        }
    }
    return fired;
}

static void setup_dom(js_ctx *ctx, const char *page_url) {
    js_object *win = ctx->global;

    g_element_proto = js_new_object(ctx, ctx->object_proto);
    g_style_proto = js_new_object(ctx, ctx->object_proto);
    g_url_proto = js_new_object(ctx, ctx->object_proto);
    g_sp_proto = js_new_object(ctx, ctx->object_proto);
    g_event_proto = js_new_object(ctx, ctx->object_proto);
    g_html_proto = js_new_object(ctx, g_element_proto);     /* HTMLElement.prototype */
    g_custom_elements = js_new_object(ctx, ctx->object_proto);
    g_module_cache = js_new_object(ctx, ctx->object_proto);
    g_document_proto = js_new_object(ctx, ctx->object_proto);
    g_response_proto = js_new_object(ctx, ctx->object_proto);
    g_ws_proto = js_new_object(ctx, ctx->object_proto);
    g_abortsignal_proto = js_new_object(ctx, ctx->object_proto);
    g_abortcontroller_proto = js_new_object(ctx, ctx->object_proto);
    g_document = js_new_object(ctx, g_document_proto);
    g_live_wrappers = js_new_array(ctx, 0);
    g_websockets = js_new_array(ctx, 0);
    g_timers = js_new_array(ctx, 0);
    g_tmeta = 0; g_tcap = 0; g_ntimers = 0;
    g_nparsed = 0;
    gc_add_root((void **)&g_element_proto);
    gc_add_root((void **)&g_style_proto);
    gc_add_root((void **)&g_url_proto);
    gc_add_root((void **)&g_sp_proto);
    gc_add_root((void **)&g_event_proto);
    gc_add_root((void **)&g_html_proto);
    gc_add_root((void **)&g_custom_elements);
    gc_add_root((void **)&g_module_cache);
    gc_add_root((void **)&g_document_proto);
    gc_add_root((void **)&g_response_proto);
    gc_add_root((void **)&g_ws_proto);
    gc_add_root((void **)&g_abortsignal_proto);
    gc_add_root((void **)&g_abortcontroller_proto);
    gc_add_root((void **)&g_document);
    gc_add_root((void **)&g_live_wrappers);
    gc_add_root((void **)&g_websockets);
    gc_add_root((void **)&g_timers);
    js_set_module_loader(ctx, browser_load_module, 0);

    /* The document IS the root node's wrapper, so events bubbling to the top reach
     * document.addEventListener handlers, and node.parentNode chains terminate at
     * `document`. (window/global is appended past it during dispatch.) */
    g_document->host = g_doc_root;
    if (g_doc_root) g_doc_root->wrapper = g_document;
    dom_on_free_wrapper = wrapper_detach;

    /* CSSStyleDeclaration-lite: accessors + methods over the style="" attribute */
    def_acc(g_style_proto, "color", style_get_color, style_set_color);
    def_acc(g_style_proto, "background", style_get_background, style_set_background);
    def_acc(g_style_proto, "backgroundColor", style_get_backgroundColor, style_set_backgroundColor);
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

    /* element prototype */
    def_acc(g_element_proto, "tagName", el_tagName, 0);
    def_acc(g_element_proto, "nodeName", el_nodeName, 0);
    def_acc(g_element_proto, "nodeType", el_nodeType, 0);
    def_acc(g_element_proto, "id", el_get_id, el_set_id);
    def_acc(g_element_proto, "className", el_get_class, el_set_class);
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
    def_acc(g_element_proto, "style", el_get_style, 0);
    def_acc(g_element_proto, "dataset", el_get_dataset, 0);
    def_acc(g_element_proto, "offsetWidth",  el_offsetWidth, 0);
    def_acc(g_element_proto, "offsetHeight", el_offsetHeight, 0);
    def_acc(g_element_proto, "offsetTop",    el_offsetTop, 0);
    def_acc(g_element_proto, "offsetLeft",   el_offsetLeft, 0);
    def_acc(g_element_proto, "clientWidth",  el_offsetWidth, 0);   /* no borders modeled */
    def_acc(g_element_proto, "clientHeight", el_offsetHeight, 0);
    def_acc(g_element_proto, "scrollWidth",  el_offsetWidth, 0);   /* no overflow modeled */
    def_acc(g_element_proto, "scrollHeight", el_offsetHeight, 0);
    def_acc(g_element_proto, "scrollTop",    el_scrollTop, 0);
    def_acc(g_element_proto, "scrollLeft",   el_scrollTop, 0);
    def_method(g_element_proto, "getAttribute", el_get_attr, 1);
    def_method(g_element_proto, "setAttribute", el_set_attr, 2);
    def_method(g_element_proto, "hasAttribute", el_has_attr, 1);
    def_method(g_element_proto, "removeAttribute", el_rm_attr, 1);
    def_method(g_element_proto, "appendChild", el_append, 1);
    def_method(g_element_proto, "append", el_append, 1);
    def_method(g_element_proto, "removeChild", el_remove_child, 1);
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

    /* document: methods on the shared proto so DOMParser documents inherit them
     * (they're root-relative — they query whichever document is `this`). */
    def_method(g_document_proto, "getElementById", doc_get_by_id, 1);
    def_method(g_document_proto, "getElementsByTagName", doc_get_by_tag, 1);
    def_method(g_document_proto, "querySelector", doc_query, 1);
    def_method(g_document_proto, "querySelectorAll", doc_query_all, 1);
    def_method(g_document_proto, "createElement", doc_create_el, 1);
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
    def_method(g_document, "hasFocus", el_true, 0);

    /* window / globals */
    js_def(win, "document", js_obj_value(g_document), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "window", js_obj_value(win), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "self", js_obj_value(win), P_WRITABLE | P_CONFIGURABLE);
    {
        js_object *loc = js_new_object(ctx, ctx->object_proto);
        gc_push_scope(); gc_keep(loc);
        fill_url_props(ctx, loc, page_url ? page_url : "");   /* href/origin/pathname/... */
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
        gc_push_scope(); gc_keep(nav);
        js_def(nav, "userAgent", js_mk_cstring(ctx, "Mozilla/5.0 (OwOS; x86) OwOSBrowser/1.0"), P_ENUMERABLE);
        js_def(nav, "appName", js_mk_cstring(ctx, "Netscape"), P_ENUMERABLE);     /* legacy, widely sniffed */
        js_def(nav, "appCodeName", js_mk_cstring(ctx, "Mozilla"), P_ENUMERABLE);
        js_def(nav, "appVersion", js_mk_cstring(ctx, "5.0 (OwOS)"), P_ENUMERABLE);
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
        def_method(nav, "sendBeacon", nav_false, 2);
        def_method(nav, "vibrate", nav_false, 1);
        {
            js_object *conn = js_new_object(ctx, ctx->object_proto);   /* NetworkInformation */
            gc_keep(conn);
            js_def(conn, "effectiveType", js_mk_cstring(ctx, "4g"), P_ENUMERABLE);
            js_def(conn, "saveData", js_bool(0), P_ENUMERABLE);
            js_def(conn, "rtt", js_number(50), P_ENUMERABLE);
            js_def(conn, "downlink", js_number(10), P_ENUMERABLE);
            def_method(conn, "addEventListener", el_noop, 2);
            js_def(nav, "connection", js_obj_value(conn), P_ENUMERABLE);
        }
        js_def(win, "navigator", js_obj_value(nav), P_WRITABLE | P_CONFIGURABLE);
        gc_pop_scope();
    }
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
        gc_push_scope(); gc_keep(uc); gc_keep(sc);
        js_def(uc, "prototype", js_obj_value(g_url_proto), 0);
        js_def(g_url_proto, "constructor", js_obj_value(uc), P_WRITABLE | P_CONFIGURABLE);
        js_def(sc, "prototype", js_obj_value(g_sp_proto), 0);
        js_def(g_sp_proto, "constructor", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
        js_def(win, "URL", js_obj_value(uc), P_WRITABLE | P_CONFIGURABLE);
        js_def(win, "URLSearchParams", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
        gc_pop_scope();
    }

    /* DOM events: Event prototype, the Event/CustomEvent constructors, and the
     * window-level listener API (document + element protos got theirs above). */
    def_method(g_event_proto, "preventDefault", ev_prevent, 0);
    def_method(g_event_proto, "stopPropagation", ev_stop, 0);
    def_method(g_event_proto, "stopImmediatePropagation", ev_stop, 0);
    {
        js_object *ec = js_new_native(ctx, event_ctor, "Event", 1);
        gc_push_scope(); gc_keep(ec);
        js_def(ec, "prototype", js_obj_value(g_event_proto), 0);
        js_def(g_event_proto, "constructor", js_obj_value(ec), P_WRITABLE | P_CONFIGURABLE);
        js_def(win, "Event", js_obj_value(ec), P_WRITABLE | P_CONFIGURABLE);
        js_def(win, "CustomEvent", js_obj_value(ec), P_WRITABLE | P_CONFIGURABLE);
        gc_pop_scope();
    }
    def_method(win, "addEventListener", el_add_listener, 3);
    def_method(win, "removeEventListener", el_remove_listener, 3);
    def_method(win, "dispatchEvent", el_dispatch, 1);

    /* Web components: HTMLElement (the base custom classes extend) + customElements */
    {
        js_object *he = js_new_native(ctx, htmlelement_ctor, "HTMLElement", 0);
        js_object *ce = js_new_object(ctx, ctx->object_proto);
        gc_push_scope(); gc_keep(he); gc_keep(ce);
        js_def(he, "prototype", js_obj_value(g_html_proto), 0);
        js_def(g_html_proto, "constructor", js_obj_value(he), P_WRITABLE | P_CONFIGURABLE);
        js_def(win, "HTMLElement", js_obj_value(he), P_WRITABLE | P_CONFIGURABLE);
        def_method(ce, "define", ce_define, 2);
        def_method(ce, "get", ce_get, 1);
        def_method(ce, "whenDefined", ce_when_defined, 1);
        js_def(win, "customElements", js_obj_value(ce), P_WRITABLE | P_CONFIGURABLE);
        gc_pop_scope();
    }
    js_def_native(ctx, win, "DOMParser", domparser_ctor, 0);   /* new DOMParser().parseFromString */

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
        gc_push_scope(); gc_keep(wc);
        js_def(wc, "prototype", js_obj_value(g_ws_proto), 0);
        js_def(g_ws_proto, "constructor", js_obj_value(wc), P_WRITABLE | P_CONFIGURABLE);
        js_def(wc, "CONNECTING", js_number(WS_CONNECTING), P_ENUMERABLE);
        js_def(wc, "OPEN", js_number(WS_OPEN), P_ENUMERABLE);
        js_def(wc, "CLOSING", js_number(WS_CLOSING), P_ENUMERABLE);
        js_def(wc, "CLOSED", js_number(WS_CLOSED), P_ENUMERABLE);
        js_def(win, "WebSocket", js_obj_value(wc), P_WRITABLE | P_CONFIGURABLE);
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
        gc_push_scope(); gc_keep(sc); gc_keep(cc);
        js_def(sc, "prototype", js_obj_value(g_abortsignal_proto), 0);
        js_def(g_abortsignal_proto, "constructor", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
        js_def_native(ctx, sc, "abort", sig_static_abort, 1);   /* AbortSignal.abort(reason?) */
        js_def(cc, "prototype", js_obj_value(g_abortcontroller_proto), 0);
        js_def(g_abortcontroller_proto, "constructor", js_obj_value(cc), P_WRITABLE | P_CONFIGURABLE);
        js_def(win, "AbortSignal", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
        js_def(win, "AbortController", js_obj_value(cc), P_WRITABLE | P_CONFIGURABLE);
        gc_pop_scope();
    }

    /* history (SPA routers lean on it): inert but present so pushState/back don't
     * throw. The page doesn't re-route, but its scripts run past these calls. */
    {
        js_object *hist = js_new_object(ctx, ctx->object_proto);
        gc_push_scope(); gc_keep(hist);
        js_def(hist, "state", js_null(), P_WRITABLE | P_ENUMERABLE);
        js_def(hist, "scrollRestoration", js_mk_cstring(ctx, "auto"), P_WRITABLE | P_ENUMERABLE);
        def_acc(hist, "length", his_length, 0);          /* live: the tab's depth */
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
        gc_push_scope(); gc_keep(mo); gc_keep(io); gc_keep(ro); gc_keep(po);
        js_def(win, "MutationObserver", js_obj_value(mo), P_WRITABLE | P_CONFIGURABLE);
        js_def(win, "IntersectionObserver", js_obj_value(io), P_WRITABLE | P_CONFIGURABLE);
        js_def(win, "ResizeObserver", js_obj_value(ro), P_WRITABLE | P_CONFIGURABLE);
        js_def(win, "PerformanceObserver", js_obj_value(po), P_WRITABLE | P_CONFIGURABLE);
        gc_pop_scope();
    }
    def_method(win, "matchMedia", win_matchmedia, 1);
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
    def_method(win, "alert", el_noop, 1);
    def_method(win, "getComputedStyle", win_getcomputedstyle, 1);
    def_acc(win, "innerWidth", win_innerWidth, 0);
    def_acc(win, "innerHeight", win_innerHeight, 0);
    def_acc(win, "scrollX", win_scrollX, 0);
    def_acc(win, "pageXOffset", win_scrollX, 0);
    def_acc(win, "scrollY", win_scrollY, 0);
    def_acc(win, "pageYOffset", win_scrollY, 0);
}

/* ------------------------------------------------------- runner ----------- */

/* console.* output goes to the devtools Console — the canonical sink, as in a
 * real browser, not the process stdout/serial. The engine's log level maps to
 * the panel's colouring (warn yellow, error red). */
static void js_print(js_ctx *ctx, int level, const char *s, int len)
{
	(void)ctx;
	int dl = level == JS_LOG_ERROR ? DT_ERROR
	       : level == JS_LOG_WARN  ? DT_WARN : DT_LOG;
	devtools_console_append(dl, s, len);
}

/* The script/context currently executing, for error attribution in the devtools
 * Console — a script URL, "<page> (inline script #N)", or an async label like
 * "<event listener>"/"<timer callback>". Set at each execution entry point. */
static char g_cur_script[URL_MAX];
static void js_set_cur_script(const char *label)
{
    int k = 0; g_cur_script[0] = '\0';
    if (label) sapp(g_cur_script, &k, sizeof g_cur_script, label);
}
/* Attribute the current execution to a named event's listener, e.g. a thrown
 * error inside an onclick shows "in <click listener>". */
static void js_set_event_script(const char *type)
{
    char buf[64]; int k = 0;
    sapp(buf, &k, sizeof buf, "<");
    sapp(buf, &k, sizeof buf, (type && type[0]) ? type : "event");
    sapp(buf, &k, sizeof buf, " listener>");
    js_set_cur_script(buf);
}

static void report_exc(js_ctx *ctx) {
    js_value e = ctx->exc;
    sbuf sb; sb.b = 0; sb.n = 0; sb.cap = 0;
    int have_stack = 0;
    ctx->has_exc = 0;
    sb_puts(&sb, "[js] uncaught ");
    if (e.type == JV_OBJ) {
        js_value stk = js_get(ctx, e, "stack");
        if (stk.type == JV_STR && stk.u.str->len) {          /* name: msg + frames */
            sb_puts(&sb, stk.u.str->data);                   /* stack ends in '\n' */
            have_stack = 1;
        } else {
            js_value nm = js_get(ctx, e, "name"), msg = js_get(ctx, e, "message");
            if (nm.type == JV_STR) sb_puts(&sb, nm.u.str->data);
            if (msg.type == JV_STR && msg.u.str->len) { sb_puts(&sb, ": "); sb_puts(&sb, msg.u.str->data); }
        }
    } else if (e.type == JV_STR) sb_puts(&sb, e.u.str->data);
    else sb_puts(&sb, "exception");
    if (!have_stack) sb_putc(&sb, '\n');
    if (g_cur_script[0]) { sb_puts(&sb, "    in "); sb_puts(&sb, g_cur_script); sb_putc(&sb, '\n'); }
    devtools_console_append(DT_ERROR, sb.b, sb.n);
    if (sb.b) free(sb.b);
}
static js_value report_exc_v(js_ctx *ctx) { report_exc(ctx); return js_undef(); }

/* ci substring test, for the <script type=...> filter */
static int ci_has(const char *hay, const char *needle) {
    int nl = strlen(needle);
    for (; *hay; hay++) { int i = 0; while (i < nl && lower(hay[i]) == lower(needle[i])) i++; if (i == nl) return 1; }
    return 0;
}
static int script_is_module(struct dom_node *s) {
    const char *type = dom_get_attr(s, "type");
    return type && ci_has(type, "module");
}
static int script_is_js(struct dom_node *s) {
    const char *type = dom_get_attr(s, "type");
    if (!type || !*type) return 1;
    if (ci_has(type, "module")) return 0;          /* handled as a module instead    */
    return ci_has(type, "javascript") || ci_has(type, "ecmascript");
}

static int script_node(struct dom_node *c) {
    return c->type == DOM_ELEMENT && streq(c->tag, "script") && (script_is_js(c) || script_is_module(c));
}
static int count_scripts(struct dom_node *n) {
    struct dom_node *c; int k = 0;
    for (c = n->first_child; c; c = c->next) { if (script_node(c)) k++; k += count_scripts(c); }
    return k;
}
static void collect_scripts(struct dom_node *n, struct dom_node **out, int *count, int cap) {
    struct dom_node *c;
    for (c = n->first_child; c && *count < cap; c = c->next) {
        if (script_node(c)) out[(*count)++] = c;   /* classic or module, inline or external */
        collect_scripts(c, out, count, cap);
    }
}
static char *sdup(const char *s) {
    int n = strlen(s);
    char *d = (char *)malloc((unsigned)n + 1);
    if (d) memcpy(d, s, (unsigned)n + 1);
    return d;
}
static char *script_text(struct dom_node *s) {
    sbuf sb; struct dom_node *c;
    sb.b = 0; sb.n = 0; sb.cap = 0;
    for (c = s->first_child; c; c = c->next) if (c->type == DOM_TEXT && c->text) sb_puts(&sb, c->text);
    if (!sb.b) { sb.b = (char *)malloc(1); if (sb.b) sb.b[0] = 0; }
    return sb.b;
}

/* Fire DOMContentLoaded on the document and load on the window, advancing
 * document.readyState as a real page does. Called once, after scripts run. */
static void fire_lifecycle(void) {
    js_value ev;
    js_set(g_ctx, js_obj_value(g_document), "readyState", js_mk_cstring(g_ctx, "interactive"));
    gc_push_scope();
    ev = make_event("DOMContentLoaded", 0, 0, js_undef());
    if (ev.type == JV_OBJ) gc_keep(ev.u.obj);
    dispatch_from(g_document, ev);
    gc_pop_scope();
    js_set(g_ctx, js_obj_value(g_document), "readyState", js_mk_cstring(g_ctx, "complete"));
    gc_push_scope();
    ev = make_event("load", 0, 0, js_undef());
    if (ev.type == JV_OBJ) gc_keep(ev.u.obj);
    dispatch_from(g_ctx->global, ev);
    gc_pop_scope();
    if (g_ctx->has_exc) report_exc(g_ctx);
}

/* Run a page's scripts and LEAVE the engine context alive for the page's lifetime
 * (so listeners + closures survive). Reclaimed by js_page_end() on the next
 * navigation. A page with no scripts builds no context — the zero-overhead path
 * for static pages, which keeps today's navigation behaviour unchanged. */
void js_run_document(struct dom_node *root, const char *page_url, res_batch_fn fetch) {
    struct dom_node **scripts;
    int *body_idx;                                  /* script i -> index in urls[], or -1 */
    char **urls;
    unsigned char **bodies;
    int *lens;
    int ns = 0, nurls = 0, i, cap;

    if (!root) return;
    cap = count_scripts(root);                      /* exact count -> exact allocations */
    if (cap == 0) return;                           /* no JS: skip the engine entirely */

    scripts  = (struct dom_node **)malloc((unsigned)cap * sizeof *scripts);
    body_idx = (int *)            malloc((unsigned)cap * sizeof *body_idx);
    urls     = (char **)          malloc((unsigned)cap * sizeof *urls);
    bodies   = (unsigned char **) malloc((unsigned)cap * sizeof *bodies);
    lens     = (int *)            malloc((unsigned)cap * sizeof *lens);
    if (!scripts || !body_idx || !urls || !bodies || !lens) {
        free(scripts); free(body_idx); free(urls); free(bodies); free(lens);
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
            if (B->csp) {                          /* CSP script-src for external scripts */
                char use[URL_MAX], reason[64];
                if (!csp_check_url(B->csp, CSP_SCRIPT, abs, page_url, use, sizeof use, reason, sizeof reason)) {
                    js_csp_log("the script", abs, reason);
                    body_idx[i] = -2;              /* blocked: skip fetch + execution */
                    continue;
                }
                copy_str(abs, sizeof abs, use);    /* upgraded URL */
            }
            urls[nurls] = sdup(abs);
            body_idx[i] = nurls;
            nurls++;
        }
    }
    for (i = 0; i < nurls; i++) { bodies[i] = 0; lens[i] = 0; }
    if (fetch && nurls) fetch(urls, nurls, bodies, lens);

    g_doc_root = root;
    g_ctx = js_ctx_new(js_print);
    if (!g_ctx) {
        for (i = 0; i < nurls; i++) { if (urls[i]) free(urls[i]); if (bodies[i]) free(bodies[i]); }
        free(scripts); free(body_idx); free(urls); free(bodies); free(lens);
        g_doc_root = 0;
        return;
    }
    g_ctx->now_ms = now_ms;
    g_has_listeners = 0;
    setup_dom(g_ctx, page_url);
    g_fetch = fetch;                                /* the loader uses it for modules */
    copy_str(g_page_url_buf, sizeof g_page_url_buf, page_url ? page_url : "");
    g_page_live = 1;

    /* CSP: without 'unsafe-eval', neutralise the dynamic-code globals so the page's
     * own scripts can't generate code from strings. */
    if (B->csp && !csp_allows_eval(B->csp)) {
        js_def_native(g_ctx, g_ctx->global, "eval", dyncode_blocked, 1);
        js_def_native(g_ctx, g_ctx->global, "Function", dyncode_blocked, 1);
    }

    int inline_n = 0;
    for (i = 0; i < ns; i++) {
        char *code;
        js_parse_error pe;
        js_node *prog;
        js_arena *ar;
        if (body_idx[i] == -2) continue;            /* CSP-blocked external (already logged) */
        /* Label this script for error attribution: its src URL, or the page URL
         * plus an inline-script ordinal. */
        if (body_idx[i] >= 0 && urls[body_idx[i]]) {
            js_set_cur_script(urls[body_idx[i]]);
        } else {
            int k = 0; g_cur_script[0] = '\0';
            sapp(g_cur_script, &k, sizeof g_cur_script, page_url ? page_url : "(inline)");
            sapp(g_cur_script, &k, sizeof g_cur_script, " (inline script #");
            uapp(g_cur_script, &k, sizeof g_cur_script, (unsigned)(++inline_n));
            sapp(g_cur_script, &k, sizeof g_cur_script, ")");
        }
        if (body_idx[i] >= 0) {                     /* external script: use the fetched body */
            int bi = body_idx[i];
            if (!bodies[bi] || lens[bi] <= 0) {
                sbuf sb; sb.b = 0; sb.n = 0; sb.cap = 0;
                sb_puts(&sb, "[js] failed to load script ");
                sb_puts(&sb, urls[bi] ? urls[bi] : "");
                sb_putc(&sb, '\n');
                devtools_console_append(DT_ERROR, sb.b, sb.n);
                if (sb.b) free(sb.b);
                continue;
            }
            code = (char *)malloc((unsigned)lens[bi] + 1);
            if (!code) continue;
            memcpy(code, bodies[bi], (unsigned)lens[bi]);
            code[lens[bi]] = 0;
        } else {
            code = script_text(scripts[i]);
        }
        if (!code) continue;
        if (!*code) { free(code); continue; }
        if (body_idx[i] < 0 && B->csp) {            /* CSP script-src for inline scripts */
            char reason[64];
            if (!csp_check_inline(B->csp, CSP_SCRIPT, dom_get_attr(scripts[i], "nonce"),
                                  (const unsigned char *)code, (int)strlen(code), reason, sizeof reason)) {
                js_csp_log("an inline script", g_cur_script, reason);
                free(code);
                continue;
            }
        }
        ar = (js_arena *)malloc(sizeof *ar);
        if (!ar) { free(code); continue; }
        js_arena_init(ar);
        prog = js_parse(code, ar, &pe);
        free(code);
        if (!prog) {
            sbuf sb; sb.b = 0; sb.n = 0; sb.cap = 0;
            sb_puts(&sb, "[js] SyntaxError: ");
            sb_puts(&sb, pe.msg ? pe.msg : "parse error");
            if (g_cur_script[0]) {
                sb_puts(&sb, "\n    in "); sb_puts(&sb, g_cur_script);
                if (pe.line > 0) { char ln[16]; int li = 0; uapp(ln, &li, sizeof ln, (unsigned)pe.line); sb_putc(&sb, ':'); sb_puts(&sb, ln); }
            }
            sb_putc(&sb, '\n');
            devtools_console_append(DT_ERROR, sb.b, sb.n);
            if (sb.b) free(sb.b);
            js_arena_free(ar); free(ar);
            continue;
        }
        if (!arenas_push(ar)) { js_arena_free(ar); free(ar); continue; }   /* AST kept alive for the page */
        if (script_is_module(scripts[i])) {         /* <script type=module> */
            js_object *exports;
            const char *murl = (body_idx[i] >= 0) ? urls[body_idx[i]] : page_url;
            gc_push_scope();
            exports = (js_object *)gc_keep(js_new_object(g_ctx, g_ctx->object_proto));
            if (body_idx[i] >= 0) js_def(g_module_cache, urls[body_idx[i]], js_obj_value(exports), 0);
            if (js_run_module(g_ctx, prog, exports, murl) != 0) report_exc(g_ctx);
            gc_pop_scope();
        } else {
            js_value res;
            /* document.currentScript points at the executing classic script (null
             * for modules + outside execution), as scripts like analytics expect. */
            js_set(g_ctx, js_obj_value(g_document), "currentScript", wrap_node(scripts[i]));
            if (js_run(g_ctx, prog, &res) != 0) report_exc(g_ctx);
            js_set(g_ctx, js_obj_value(g_document), "currentScript", js_null());
        }
    }

    for (i = 0; i < nurls; i++) { if (urls[i]) free(urls[i]); if (bodies[i]) free(bodies[i]); }
    free(scripts); free(body_idx); free(urls); free(bodies); free(lens);

    fire_lifecycle();
}

/* Reclaim the page's JS context: drop roots, free the heap, free the script ASTs.
 * Idempotent — safe to call when no page context is live (static pages). The
 * caller frees the DOM AFTER this returns (wrappers reference nodes via host). */
void js_page_end(void) {
    int i;
    if (!g_page_live) return;
    dom_on_free_wrapper = 0;                         /* DOM teardown shouldn't call back */
    if (g_websockets && g_ctx) {                     /* close + free live sockets first */
        unsigned n = js_array_len(g_websockets), k;
        for (k = 0; k < n; k++) {
            js_value wv = js_get_index(g_ctx, js_obj_value(g_websockets), k);
            if (wv.type == JV_OBJ && wv.u.obj->host) {
                ws_free((ws_conn *)wv.u.obj->host);
                wv.u.obj->host = 0;
            }
        }
    }
    gc_remove_root((void **)&g_element_proto);
    gc_remove_root((void **)&g_style_proto);
    gc_remove_root((void **)&g_url_proto);
    gc_remove_root((void **)&g_sp_proto);
    gc_remove_root((void **)&g_event_proto);
    gc_remove_root((void **)&g_html_proto);
    gc_remove_root((void **)&g_custom_elements);
    gc_remove_root((void **)&g_module_cache);
    gc_remove_root((void **)&g_document_proto);
    gc_remove_root((void **)&g_response_proto);
    gc_remove_root((void **)&g_ws_proto);
    gc_remove_root((void **)&g_abortsignal_proto);
    gc_remove_root((void **)&g_abortcontroller_proto);
    gc_remove_root((void **)&g_document);
    gc_remove_root((void **)&g_live_wrappers);
    gc_remove_root((void **)&g_websockets);
    gc_remove_root((void **)&g_timers);
    g_timers = 0;
    if (g_tmeta) { free(g_tmeta); g_tmeta = 0; }
    g_tcap = 0; g_ntimers = 0;
    g_element_proto = g_style_proto = g_url_proto = g_sp_proto = 0;
    g_event_proto = g_html_proto = g_custom_elements = g_document = g_live_wrappers = 0;
    g_module_cache = g_document_proto = g_response_proto = 0;
    g_ws_proto = g_websockets = 0;
    g_abortsignal_proto = g_abortcontroller_proto = 0;
    g_fetch = 0;
    if (g_ctx) { js_ctx_free(g_ctx); g_ctx = 0; }    /* frees the whole JS heap         */
    for (i = 0; i < g_narenas; i++) { js_arena_free(g_arenas[i]); free(g_arenas[i]); }
    if (g_arenas) { free(g_arenas); g_arenas = 0; }
    g_narenas = 0; g_arenas_cap = 0;
    for (i = 0; i < g_nparsed; i++) dom_free(g_parsed[i]);   /* DOMParser trees */
    g_nparsed = 0;
    g_has_listeners = 0;
    g_page_live = 0;
    g_doc_root = 0;
}
