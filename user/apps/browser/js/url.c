/* URL, URLSearchParams and FormData. */

#include "bindings.h"

/* ------------------------------------------------------- URL -------------- */

void span_copy(char *dst, int cap, const char *s, const char *e)
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
void uint_to_str(char *dst, unsigned v)
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
js_value make_search_params(js_ctx *ctx, const char *query)
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

js_value sp_get(js_ctx *ctx, js_value t, js_value *args, int argc)
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
js_value sp_get_all(js_ctx *ctx, js_value t, js_value *args, int argc)
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
js_value sp_has(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value v = sp_get(ctx, t, args, argc);
  return js_bool(v.type != JV_NULL);
}
js_value sp_append(js_ctx *ctx, js_value t, js_value *args, int argc)
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
/* rebuild @k/@v keeping only entries not matching `name`; if `keepfirst` */
static void sp_rebuild(js_ctx *ctx, js_value t, const char *name, int keepfirst, js_value repl)
{
  js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v"), *nk, *nv;
  unsigned i, n;
  bool seen = false;
  if (!ka || !va)
    return;
  gc_push_scope();
  nk = (js_object *)gc_keep(js_new_array(ctx, 0));
  nv = (js_object *)gc_keep(js_new_array(ctx, 0));
  n = js_array_len(ka);
  for (i = 0; i < n; i++) {
    js_value k = js_get_index(ctx, js_obj_value(ka), i);
    bool match = k.type == JV_STR && streq(k.u.str->data, name);
    if (match && keepfirst && !seen) {
      seen = true;
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
js_value sp_set(js_ctx *ctx, js_value t, js_value *args, int argc)
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
js_value sp_delete(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[256];
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  sp_rebuild(ctx, t, name, 0, js_undef());
  return js_undef();
}
js_value sp_for_each(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v");
  js_value cb = A(0), cbargs[3];
  unsigned i, n;
  if (!js_is_callable(cb) || !ka || !va)
    return js_undef();
  n = js_array_len(ka);
  for (i = 0; i < n; i++) {
    cbargs[0] = js_get_index(ctx, js_obj_value(va), i); /* value */
    cbargs[1] = js_get_index(ctx, js_obj_value(ka), i); /* key */
    cbargs[2] = t;
    js_call(ctx, cb, js_undef(), cbargs, 3);
    if (ctx->has_exc)
      break;
  }
  return js_undef();
}
js_value sp_tostring(js_ctx *ctx, js_value t, js_value *args, int argc)
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
js_value sp_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  const char *q = arg_str(ctx, A(0));
  (void)thisv;
  if (*q == '?')
    q++;
  return make_search_params(ctx, q);
}

/* --------------------------------------------------------------- FormData -- */
static void fd_collect(js_ctx *ctx, struct dom_node *n, js_object *ka, js_object *va)
{
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type == DOM_ELEMENT) {
      const char *tag = c->tag, *name = dom_get_attr(c, "name");
      const char *type = dom_get_attr(c, "type");
      if (name && name[0] && (streq(tag, "input") || streq(tag, "select") || streq(tag, "textarea"))) {
        bool skip = type && (streq(type, "submit") || streq(type, "button") || streq(type, "reset") ||
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
js_value fd_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
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
