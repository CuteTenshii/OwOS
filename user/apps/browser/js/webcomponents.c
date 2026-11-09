/* Custom elements, shadow DOM and ES modules. */

#include "bindings.h"

/* ------------------------------------------------------- web components ---- */

js_value htmlelement_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)ctx;
  (void)thisv;
  (void)args;
  (void)argc; /* super(): a no-op base */
  return js_undef();
}

static bool ce_is_upgraded(js_object *w) { return w && pmap_find_pub(w, "@ce") != 0; }
static bool ce_is_connected(js_object *w)
{
  pentry *pe = pmap_find_pub(w, "@conn");
  return pe && js_to_boolean(pe->value);
}
static bool ce_connected(struct dom_node *n)
{
  for (; n; n = (struct dom_node *)dom_flat_parent(n))
    if (n == g_doc_root)
      return true;
  return false;
}
js_value ce_lookup(const char *tag)
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

/* Upgrade `n` to its custom class `cls` */
void ce_upgrade_node(struct dom_node *n, js_value cls)
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
  /* spec order: after the constructor */
  {
    struct dom_attr *a;
    for (a = n->attrs; a; a = a->next)
      ce_attr_changed(n, a->name, 0, a->value);
  }
}
/* Fire connectedCallback once, when an upgraded element is in the document. */
void ce_connect(struct dom_node *n)
{
  js_object *w = n ? (js_object *)n->wrapper : 0;
  if (!w || !ce_is_upgraded(w) || ce_is_connected(w))
    return;
  if (!ce_connected(n))
    return;
  js_def(w, "@conn", js_bool(1), P_WRITABLE);
  ce_call_cb(w, "connectedCallback", 0, 0);
}
/* Fire disconnectedCallback on `n` and its descendants that were connected. */
void ce_disconnect_tree(struct dom_node *n)
{
  js_object *w;
  if (!n || n->type != DOM_ELEMENT)
    return;
  w = (js_object *)n->wrapper;
  if (w && ce_is_upgraded(w) && ce_is_connected(w)) {
    js_def(w, "@conn", js_bool(0), P_WRITABLE);
    ce_call_cb(w, "disconnectedCallback", 0, 0);
  }
  for (struct dom_node *c = n->first_child; c; c = c->next)
    ce_disconnect_tree(c);
  if (n->shadow)
    ce_disconnect_tree(n->shadow);
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
  if (root->shadow)
    ce_upgrade_tree(root->shadow, name, cls);
}

/* setAttribute hook: fire attributeChangedCallback for an observed attribute */
void ce_attr_changed(struct dom_node *n, const char *name, const char *oldv, const char *newv)
{
  js_object *w = n ? (js_object *)n->wrapper : 0;
  js_value ctor, obs;
  unsigned i, len;
  bool observed = false;
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
      observed = true;
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

/* A valid custom element name: lowercase start, contains '-', no uppercase, not reserved */
static bool ce_valid_name(const char *s)
{
  static const char *const reserved[] = {"annotation-xml", "color-profile",    "font-face",      "font-face-src",
                                         "font-face-uri",  "font-face-format", "font-face-name", "missing-glyph"};
  bool dash = false;
  if (!(s[0] >= 'a' && s[0] <= 'z'))
    return false;
  for (const char *p = s; *p; p++) {
    if (*p >= 'A' && *p <= 'Z')
      return false;
    if (*p == '-')
      dash = true;
  }
  for (unsigned i = 0; i < sizeof reserved / sizeof reserved[0]; i++)
    if (streq(s, reserved[i]))
      return false;
  return dash;
}
js_value ce_define(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[64];
  js_value cls = A(1);
  (void)t;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  if (!js_is_callable(cls))
    return js_throw_type(ctx, "customElements.define: the constructor is not a function");
  if (!ce_valid_name(name))
    return dom_throw(ctx, "SyntaxError", "customElements.define: invalid custom element name");
  if (pmap_find_pub(g_custom_elements, name))
    return dom_throw(ctx, "NotSupportedError", "customElements.define: name already defined");
  js_def(g_custom_elements, name, cls, 0);
  if (g_doc_root)
    ce_upgrade_tree(g_doc_root, name, cls);
  {
    char key[80];
    pentry *pe;
    snprintf(key, sizeof key, "@wait:%s", name);
    pe = pmap_find_pub(g_custom_elements, key);
    if (pe && js_is_array(pe->value)) {
      js_value waiters = pe->value;
      gc_push_scope();
      gc_keep(waiters.u.obj);
      gc_keep(cls.u.obj);
      js_def(g_custom_elements, key, js_undef(), P_WRITABLE);
      for (unsigned i = 0; i < js_array_len(waiters.u.obj); i++)
        js_call(ctx, js_get_index(ctx, waiters, i), js_undef(), &cls, 1);
      gc_pop_scope();
    }
  }
  return js_undef();
}
js_value ce_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[64];
  pentry *pe;
  (void)t;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  pe = pmap_find_pub(g_custom_elements, name);
  return pe ? pe->value : js_undef();
}
static js_object *g_ce_waiters; /* the array the Promise executor appends its resolve function to */
static js_value ce_executor(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  if (g_ce_waiters)
    js_array_push(ctx, g_ce_waiters, A(0));
  return js_undef();
}
js_value ce_when_defined(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[64], key[80];
  pentry *pe;
  js_value cls, promise, exec;
  (void)t;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  pe = pmap_find_pub(g_custom_elements, name);
  if (pe)
    return js_promise_resolved(ctx, pe->value);
  if (!ce_valid_name(name))
    return js_promise_rejected(ctx, dom_exception(ctx, "SyntaxError", "invalid custom element name"));
  snprintf(key, sizeof key, "@wait:%s", name);
  pe = pmap_find_pub(g_custom_elements, key);
  gc_push_scope();
  if (!pe || !js_is_array(pe->value)) {
    js_def(g_custom_elements, key, js_obj_value((js_object *)gc_keep(js_new_array(ctx, 0))), P_WRITABLE);
    pe = pmap_find_pub(g_custom_elements, key);
  }
  g_ce_waiters = pe->value.u.obj;
  exec = js_obj_value((js_object *)gc_keep(js_new_native(ctx, ce_executor, "executor", 2)));
  cls = js_get(ctx, js_obj_value(ctx->global), "Promise");
  promise = js_construct(ctx, cls, &exec, 1);
  g_ce_waiters = 0;
  gc_pop_scope();
  return promise;
}
static void ce_upgrade_subtree(struct dom_node *n)
{
  js_value cls;
  if (n->type != DOM_ELEMENT)
    return;
  cls = ce_lookup(n->tag);
  if (cls.type == JV_OBJ)
    ce_upgrade_connect(n, cls);
  for (struct dom_node *c = n->first_child; c; c = c->next)
    ce_upgrade_subtree(c);
}
void ce_upgrade_children(struct dom_node *n)
{
  for (struct dom_node *c = n->first_child; c; c = c->next)
    ce_upgrade_subtree(c);
  if (n->shadow)
    ce_upgrade_children(n->shadow);
}
js_value ce_upgrade(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(A(0));
  (void)ctx;
  (void)t;
  if (n)
    ce_upgrade_subtree(n);
  return js_undef();
}

/* ------------------------------------------------------- ES modules -------- */

/* Keep a parsed script's arena alive for the page (closures reference its AST) */
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

/* Append a ":line:col" (or ":line") source position to `sb`, for SyntaxError reports */
void sb_put_pos(sbuf *sb, int line, int col)
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

/* True if the identifier word `w` (with a word boundary before it) ends at `q` */
static bool mod_word_before(const char *src, const char *q, const char *w)
{
  const char *e = q;
  while (e > src && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
    e--;
  int wl = (int)strlen(w);
  if (e - src < wl)
    return false;
  for (int i = 0; i < wl; i++)
    if (e[-wl + i] != w[i])
      return false;
  if (e - src > wl) {
    char c = e[-wl - 1];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9'))
      return false;
  }
  return true;
}

/* Resolve a module specifier: import map first (exact key, then "prefix/" keys), else relative to the referrer */
static void module_resolve(const char *referrer, const char *spec, char *out, int cap)
{
  if (spec[0] != '.' && spec[0] != '/') {
    pentry *pe = pmap_find_pub(g_importmap, spec);
    if (pe && pe->value.type == JV_STR) {
      resolve_url(g_page_url_buf, pe->value.u.str->data, out, cap);
      return;
    }
    int sl = (int)strlen(spec);
    for (int i = 0; i < g_importmap->props.n; i++) {
      pentry *pf = &g_importmap->props.e[i];
      int kl = (int)strlen(pf->key);
      if (kl > 0 && pf->key[kl - 1] == '/' && sl > kl && memcmp(spec, pf->key, (unsigned)kl) == 0) {
        char joined[URL_MAX];
        int vl = pf->value.u.str->len < URL_MAX - 1 ? pf->value.u.str->len : URL_MAX - 1;
        memcpy(joined, pf->value.u.str->data, (unsigned)vl);
        strlcpy(joined + vl, spec + kl, sizeof joined - (unsigned)vl);
        resolve_url(g_page_url_buf, joined, out, cap);
        return;
      }
    }
  }
  resolve_url(referrer, spec, out, cap);
}

/* Scan a module's source for its static import specifiers and batch-fetch the uncached ones in ONE */
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
    char spec[URL_MAX];
    int k = 0;
    for (const char *c = s; c < e && k < URL_MAX - 1; c++)
      spec[k++] = *c;
    spec[k] = 0;
    if (e > s && (*s == '.' || *s == '/' || pmap_find_pub(g_importmap, spec)) &&
        (mod_word_before(src, p, "from") || mod_word_before(src, p, "import"))) {
      char abs[URL_MAX];
      module_resolve(base, spec, abs, sizeof abs);
      if (!pmap_find_pub(g_module_src, abs) && !pmap_find_pub(g_module_cache, abs)) {
        bool dup = false;
        for (int i = 0; i < n; i++)
          if (streq(urls[i], abs)) {
            dup = true;
            break;
          }
        if (!dup)
          urls[n++] = strdup(abs);
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

js_object *browser_load_module(js_ctx *ctx, const char *spec, void *ud)
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
  module_resolve(referrer, spec, abs, sizeof abs);
  cached = pmap_find_pub(g_module_cache, abs);
  if (cached && cached->value.type == JV_OBJ)
    return cached->value.u.obj;

  /* source: use the prefetched copy (batched with its siblings) if present */
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
