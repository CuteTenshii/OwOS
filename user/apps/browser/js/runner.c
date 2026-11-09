/* Page lifecycle: script discovery, execution and teardown. */

#include "bindings.h"
#include "canvasjs.h"
#include "features/mediasession/mediasession.h"
#include "features/notification/notification.h"
#include "websocket.h"

/* ------------------------------------------------------- runner ----------- */

/* What structured clone carries across threads besides plain data: Blobs and OffscreenCanvases */
static int page_clone_write(js_ctx *ctx, js_value v, char kind[16], unsigned char **bytes, int *len, char **meta)
{
  return blob_clone_write(ctx, v, kind, bytes, len, meta) || canvas_clone_write(ctx, v, kind, bytes, len, meta) ||
         worker_port_write(ctx, v, kind, bytes, len);
}

static js_value page_clone_read(js_ctx *ctx, const char *kind, const unsigned char *bytes, int len, const char *meta)
{
  js_value v = blob_clone_read(ctx, kind, bytes, len, meta);
  if (v.type == JV_OBJ)
    return v;
  v = canvas_clone_read(ctx, kind, bytes, len);
  return v.type == JV_OBJ ? v : worker_port_read(ctx, kind, bytes, len);
}

/* console.* output goes to the devtools Console, the canonical sink, as in a real browser */
static void js_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  int dl = level == JS_LOG_ERROR ? DT_ERROR : level == JS_LOG_WARN ? DT_WARN : DT_LOG;
  devtools_console_append(dl, s, len);
}

/* The script/context currently executing */
static char g_cur_script[URL_MAX];
void js_set_cur_script(const char *label)
{
  int k = 0;
  g_cur_script[0] = '\0';
  if (label)
    sapp(g_cur_script, &k, sizeof g_cur_script, label);
  /* Point the engine at the same buffer so a captured stack's outermost frame names this script/async */
  if (g_ctx)
    js_set_script(g_ctx, g_cur_script);
}
const char *js_cur_script(void) { return g_cur_script; }
/* Attribute the current execution to a named event's listener, plus the script that registered it */
void js_set_event_script(const char *type, const char *src)
{
  char buf[320];
  int k = 0;
  sapp(buf, &k, sizeof buf, "<");
  sapp(buf, &k, sizeof buf, (type && type[0]) ? type : "event");
  sapp(buf, &k, sizeof buf, " listener>");
  if (src && src[0]) {
    sapp(buf, &k, sizeof buf, " ");
    sapp(buf, &k, sizeof buf, src);
  }
  js_set_cur_script(buf);
}

void report_exc(js_ctx *ctx)
{
  js_value e = ctx->exc;
  sbuf sb;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  bool have_stack = false;
  ctx->has_exc = 0;
  sb_puts(&sb, "[js] uncaught ");
  if (e.type == JV_OBJ) {
    js_value stk = js_get(ctx, e, "stack");
    if (stk.type == JV_STR && stk.u.str->len) { /* name: msg + frames */
      sb_puts(&sb, stk.u.str->data);            /* stack ends in '\n' */
      have_stack = true;
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
  /* A captured .stack already ends in its " at <script>" origin line */
  if (!have_stack && g_cur_script[0]) {
    sb_puts(&sb, "    in ");
    sb_puts(&sb, g_cur_script);
    sb_putc(&sb, '\n');
  }
  devtools_console_append(DT_ERROR, sb.b, sb.n);
  if (sb.b)
    free(sb.b);
}
js_value report_exc_v(js_ctx *ctx)
{
  report_exc(ctx);
  return js_undef();
}

/* Devtools console REPL: parse and run one expression/statement against the live page context */
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
    static const char note[] = "(no page script context: open a page that runs JS first)\n";
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

static bool script_is_module(struct dom_node *s)
{
  const char *type = dom_get_attr(s, "type");
  return type && contains_ci(type, "module");
}
static bool script_is_js(struct dom_node *s)
{
  const char *type = dom_get_attr(s, "type");
  if (!type || !*type)
    return true;
  if (contains_ci(type, "module"))
    return false; /* handled as a module instead */
  return contains_ci(type, "javascript") || contains_ci(type, "ecmascript");
}

static bool script_node(struct dom_node *c)
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

/* Execute a <script> connected to the live document via the JS DOM API */
void run_inserted_script(struct dom_node *node)
{
  js_value w;
  char *code = 0;
  const char *src;
  js_arena *ar;
  js_parse_error pe;
  js_node *prog;
  bool is_module;

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

/* Fire DOMContentLoaded on the document and load on the window */
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

/* Run a page's scripts and LEAVE the engine context alive for the page's lifetime */
/* Script execution ordering (HTML spec, simplified for a fully-fetched batch) */
static bool script_deferred(struct dom_node *s)
{
  const char *src = dom_get_attr(s, "src");
  bool external = src && *src;
  if (external && dom_get_attr(s, "async"))
    return false; /* async: first pass, as-fetched */
  if (script_is_module(s))
    return true; /* modules default to deferred */
  if (external && dom_get_attr(s, "defer"))
    return true; /* defer: external scripts only */
  return false;
}

static void load_importmaps(struct dom_node *n)
{
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    const char *type = c->type == DOM_ELEMENT && streq(c->tag, "script") ? dom_get_attr(c, "type") : 0;
    if (type && contains_ci(type, "importmap")) {
      char *text = script_text(c);
      js_value json = js_get(g_ctx, js_obj_value(g_ctx->global), "JSON");
      js_value parse = js_get(g_ctx, json, "parse");
      if (text && js_is_callable(parse)) {
        js_value arg = js_mk_cstring(g_ctx, text);
        js_value map = js_call(g_ctx, parse, json, &arg, 1);
        js_value imports = g_ctx->has_exc ? js_undef() : js_get(g_ctx, map, "imports");
        if (g_ctx->has_exc)
          g_ctx->has_exc = 0;
        if (imports.type == JV_OBJ)
          for (int i = 0; i < imports.u.obj->props.n; i++) {
            pentry *pe = &imports.u.obj->props.e[i];
            if (pe->value.type == JV_STR && !js_has_own(g_importmap, pe->key))
              js_def(g_importmap, pe->key, pe->value, 0);
          }
      }
      free(text);
    }
    load_importmaps(c);
  }
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

  /* Resolve every <script src> to an absolute URL */
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
      urls[nurls] = strdup(abs);
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
  g_ctx->clone_write_ext = page_clone_write;
  g_ctx->clone_read_ext = page_clone_read;
  js_set_stack_limit(g_ctx, browser_js_stack_floor()); /* bound recursion to the worker stack */
  js_set_script(g_ctx, g_cur_script);                  /* origin frame for captured stacks */
  g_has_listeners = false;
  setup_dom(g_ctx, page_url);
  g_fetch = fetch; /* the loader uses it for modules */
  strlcpy(g_page_url_buf, page_url ? page_url : "", URL_MAX);
  g_page_live = 1;
  load_importmaps(root);

  /* CSP: without 'unsafe-eval', neutralise the dynamic-code globals so the page's own scripts can't */
  if (B->pg->csp && !csp_allows_eval(B->pg->csp)) {
    js_def_native(g_ctx, g_ctx->global, "eval", dyncode_blocked, 1);
    js_def_native(g_ctx, g_ctx->global, "Function", dyncode_blocked, 1);
  }

  int inline_n = 0;
  /* Two ordered passes: pass 0 = classic + async (document order) */
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
      /* Label this script for error attribution: its src URL */
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
        /* document.currentScript points at the executing classic script */
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

/* Reclaim the page's JS context: drop roots, free the heap, free the script ASTs */
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
  documentapi_teardown();
  gc_remove_root((void **)&g_node_proto);
  gc_remove_root((void **)&g_element_proto);
  gc_remove_root((void **)&g_style_proto);
  gc_remove_root((void **)&g_classlist_proto);
  gc_remove_root((void **)&g_rellist_proto);
  gc_remove_root((void **)&g_url_proto);
  gc_remove_root((void **)&g_sp_proto);
  gc_remove_root((void **)&g_formdata_proto);
  gc_remove_root((void **)&g_event_proto);
  gc_remove_root((void **)&g_html_proto);
  gc_remove_root((void **)&g_media_proto);
  gc_remove_root((void **)&g_custom_elements);
  gc_remove_root((void **)&g_module_cache);
  gc_remove_root((void **)&g_module_src);
  gc_remove_root((void **)&g_importmap);
  gc_remove_root((void **)&g_shadowroot_proto);
  gc_remove_root((void **)&g_fragment_proto);
  gc_remove_root((void **)&g_document_proto);
  gc_remove_root((void **)&g_response_proto);
  gc_remove_root((void **)&g_ws_proto);
  gc_remove_root((void **)&g_xhr_proto);
  gc_remove_root((void **)&g_blob_proto);
  canvas_js_teardown();
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
  mediasession_teardown();
  gc_remove_root((void **)&g_timers);
  js_idb_teardown();
  js_webapi_teardown();
  {
    unsigned k;
    for (k = 0; k < g_nhtml_ifaces; k++) {
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
  g_element_proto = g_style_proto = g_classlist_proto = g_rellist_proto = g_url_proto = g_sp_proto = g_formdata_proto =
      0;
  g_event_proto = g_html_proto = g_custom_elements = g_document = g_live_wrappers = 0;
  g_module_cache = g_document_proto = g_response_proto = 0;
  g_module_src = 0;
  g_importmap = 0;
  g_shadowroot_proto = g_fragment_proto = 0;
  g_ws_proto = g_websockets = 0;
  g_bchannel_proto = g_bchannels = 0;
  g_xhr_proto = 0;
  g_blob_proto = 0;
  g_abortsignal_proto = g_abortcontroller_proto = 0;
  g_rtc_proto = 0;
  js_worker_teardown();
  sw_teardown(); /* SW roots/objects (C store persists) */
  g_fetch = 0;
  if (g_ctx) {
    js_ctx_free(g_ctx);
    g_ctx = 0;
  } /* frees the whole JS heap */
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
  free(g_parsed);
  g_parsed = 0;
  g_nparsed = g_parsed_cap = 0;
  g_has_listeners = false;
  g_page_live = 0;
  g_doc_root = 0;
}
