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
char *form_decode(const char *s, int len)
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
void sb_put_form(sbuf *s, const char *t)
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

/* ---- URL parsing for workers: the components of an absolute URL, resolved against an optional base ---- */

static char *url_rchr(const char *s, char c)
{
  const char *last = 0;
  for (; *s; s++)
    if (*s == c)
      last = s;
  return (char *)last;
}

static bool url_has_scheme(const char *s)
{
  if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')))
    return false;
  for (s++; *s; s++) {
    if (*s == ':')
      return true;
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '+' || *s == '.' ||
          *s == '-'))
      return false;
  }
  return false;
}

/* the default port of a special scheme, "" for file:, NULL for any other scheme */
static const char *url_default_port(const char *protocol)
{
  static const struct {
    const char *scheme, *port;
  } table[] = {{"http:", "80"}, {"https:", "443"}, {"ftp:", "21"}, {"ws:", "80"}, {"wss:", "443"}, {"file:", ""}};
  for (unsigned i = 0; i < sizeof table / sizeof *table; i++)
    if (streq(protocol, table[i].scheme))
      return table[i].port;
  return 0;
}

/* Percent-encode every byte outside the printable ASCII range and the ones `extra` lists */
static void url_escape(char *dst, int cap, const char *src, const char *extra)
{
  static const char hx[] = "0123456789ABCDEF";
  int n = 0;
  for (; *src && n < cap - 4; src++) {
    unsigned char c = (unsigned char)*src;
    if (c < 33 || c > 126 || strchr(extra, c)) {
      dst[n++] = '%';
      dst[n++] = hx[c >> 4];
      dst[n++] = hx[c & 15];
    } else
      dst[n++] = (char)c;
  }
  dst[n] = 0;
}

/* Escape a pathname (0), query (1) or fragment (2) the way the URL parser does */
void url_escape_kind(char *dst, int cap, const char *src, int kind)
{
  url_escape(dst, cap, src, kind == 0 ? "\"<>`{}?#" : kind == 1 ? "\"<>#'" : "\"<>`");
}

static void url_lower(char *s)
{
  for (; *s; s++)
    if (*s >= 'A' && *s <= 'Z')
      *s = (char)(*s + 32);
}

/* the query as it is written: an empty one that was present still shows its "?" */
static const char *url_search_of(const struct url_parts *u) { return u->has_search && !u->search[0] ? "?" : u->search; }

static bool url_parse_absolute(const char *input, struct url_parts *r)
{
  char buf[URL_MAX], *rest, *hash, *query, *colon, *slash, *auth, *at;
  const char *dp;
  unsigned n = (unsigned)strlen(input);
  if (n >= sizeof buf)
    return false;
  memcpy(buf, input, n + 1);
  memset(r, 0, sizeof *r);
  hash = strchr(buf, '#');
  if (hash) {
    char tmp[URL_MAX];
    url_escape(tmp, sizeof tmp, hash, "\"<>`");
    snprintf(r->hash, sizeof r->hash, "%s", strcmp(tmp, "#") ? tmp : "");
    r->has_hash = true;
    *hash = 0;
  }
  query = strchr(buf, '?');
  if (query) {
    char tmp[URL_MAX];
    url_escape(tmp, sizeof tmp, query, "\"<>#'");
    snprintf(r->search, sizeof r->search, "%s", strcmp(tmp, "?") ? tmp : "");
    r->has_search = true;
    *query = 0;
  }
  colon = strchr(buf, ':');
  if (!colon || colon == buf || colon - buf >= (long)sizeof r->protocol - 1)
    return false;
  memcpy(r->protocol, buf, (unsigned)(colon - buf) + 1);
  url_lower(r->protocol);
  rest = colon + 1;
  dp = url_default_port(r->protocol);
  if (rest[0] == '/' && rest[1] == '/') {
    rest += 2;
    slash = strchr(rest, '/');
    snprintf(r->pathname, sizeof r->pathname, "%s", slash ? slash : "");
    if (slash)
      *slash = 0;
    auth = rest;
    at = url_rchr(auth, '@');
    if (at) {
      char *pw = auth;
      while (pw < at && *pw != ':')
        pw++;
      if (pw == at)
        pw = 0;
      *at = 0;
      if (pw) {
        *pw = 0;
        snprintf(r->password, sizeof r->password, "%s", pw + 1);
      }
      snprintf(r->username, sizeof r->username, "%s", auth);
      auth = at + 1;
    }
    if (auth[0] == '[') {
      char *end = strchr(auth, ']');
      if (end) {
        if (end[1] == ':')
          snprintf(r->port, sizeof r->port, "%s", end + 2);
        end[1] = 0;
      }
    } else if ((colon = url_rchr(auth, ':'))) {
      snprintf(r->port, sizeof r->port, "%s", colon + 1);
      *colon = 0;
    }
    snprintf(r->hostname, sizeof r->hostname, "%s", auth);
    url_lower(r->hostname);
    if (dp && r->port[0] && streq(r->port, dp))
      r->port[0] = 0;
    if (!r->pathname[0] && dp)
      strcpy(r->pathname, "/");
  } else {
    snprintf(r->pathname, sizeof r->pathname, "%s", rest);
    r->opaque = true;
  }
  if (!r->opaque) {
    char tmp[URL_MAX];
    url_escape(tmp, sizeof tmp, r->pathname, "\"<>`{}?#");
    snprintf(r->pathname, sizeof r->pathname, "%s", tmp);
  }
  return true;
}

/* RFC 3986 dot-segment removal, in place */
static void url_remove_dots(char *path)
{
  char *seg[256], *p = path, *out[256];
  int nseg = 0, nout = 0, i;
  char result[URL_MAX];
  unsigned at = 0;
  for (;;) {
    char *slash = strchr(p, '/');
    if (nseg == 256)
      return;
    seg[nseg++] = p;
    if (!slash)
      break;
    *slash = 0;
    p = slash + 1;
  }
  for (i = 0; i < nseg; i++) {
    bool last = i == nseg - 1;
    if (streq(seg[i], "..")) {
      if (nout > 1)
        nout--;
      if (last)
        out[nout++] = (char *)"";
    } else if (streq(seg[i], ".")) {
      if (last)
        out[nout++] = (char *)"";
    } else
      out[nout++] = seg[i];
  }
  for (i = 0; i < nout; i++) {
    unsigned l = (unsigned)strlen(out[i]);
    if (at + l + 2 >= sizeof result)
      return;
    if (i)
      result[at++] = '/';
    memcpy(result + at, out[i], l);
    at += l;
  }
  result[at] = 0;
  strcpy(path, result);
}

/* `input` resolved against `base_href` (NULL: it must be absolute); false for an invalid URL */
bool url_parse(const char *input, const char *base_href, struct url_parts *out)
{
  struct url_parts base;
  char ref[URL_MAX], abs[URL_MAX], path[URL_MAX], tail_buf[URL_MAX], *cut, *hash, *q;
  const char *tail = tail_buf, *query = "";
  unsigned len;
  while (*input == ' ' || *input == '\t' || *input == '\n' || *input == '\r')
    input++;
  len = (unsigned)strlen(input);
  while (len && (input[len - 1] == ' ' || input[len - 1] == '\t' || input[len - 1] == '\n' || input[len - 1] == '\r'))
    len--;
  if (len >= sizeof ref)
    return false;
  memcpy(ref, input, len);
  ref[len] = 0;
  if (url_has_scheme(ref))
    return url_parse_absolute(ref, out);
  if (!base_href || !url_parse_absolute(base_href, &base))
    return false;
  if (ref[0] == '/' && ref[1] == '/') {
    snprintf(abs, sizeof abs, "%s%s", base.protocol, ref);
    return url_parse_absolute(abs, out);
  }
  hash = strchr(ref, '#');
  snprintf(tail_buf, sizeof tail_buf, "%s", hash ? hash : "");
  cut = ref;
  {
    char saved = hash ? *hash : 0;
    if (hash)
      *hash = 0;
    q = strchr(cut, '?');
    if (q)
      query = q;
    snprintf(path, sizeof path, "%.*s", q ? (int)(q - cut) : (int)strlen(cut), cut);
    if (base.opaque) {
      bool ok = ref[0] == 0 && tail[0] == '#';
      if (hash)
        *hash = saved;
      if (!ok)
        return false;
      snprintf(abs, sizeof abs, "%s%s%s%s", base.protocol, base.pathname, url_search_of(&base), tail);
      return url_parse_absolute(abs, out);
    }
    {
      char root[URL_MAX + 600];
      snprintf(root, sizeof root, "%s//%s%s%s%s%s%s%s", base.protocol, base.username, base.password[0] ? ":" : "",
               base.password, base.username[0] || base.password[0] ? "@" : "", base.hostname, base.port[0] ? ":" : "",
               base.port);
      if (!path[0]) {
        snprintf(abs, sizeof abs, "%s%s%s%s", root, base.pathname, !query[0] && !cut[0] ? url_search_of(&base) : query,
                 tail);
      } else {
        if (path[0] != '/') {
          char merged[URL_MAX];
          char *ls = url_rchr(base.pathname, '/');
          snprintf(merged, sizeof merged, "%.*s%s", ls ? (int)(ls - base.pathname) + 1 : 0, base.pathname, path);
          snprintf(path, sizeof path, "%s", merged);
        }
        url_remove_dots(path);
        snprintf(abs, sizeof abs, "%s%s%s%s", root, path, query, tail);
      }
    }
    if (hash)
      *hash = saved;
  }
  return url_parse_absolute(abs, out);
}

static js_object *sp_arr(js_ctx *ctx, js_value t, const char *key)
{
  js_value a = js_get(ctx, t, key);
  return a.type == JV_OBJ ? a.u.obj : 0;
}

#define JS(...) #__VA_ARGS__
/* URL and URLSearchParams, in JavaScript: the same source runs on the page and in dedicated workers (webworker.c) */
const char url_classes_js[] =
#include "url.inc"
    ;

/* N.urlParse(input, base): the URL's components as an object, or null for an invalid URL */
static js_value url_n_parse(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value in = js_to_string(ctx, A(0)), base = A(1).type == JV_UNDEF ? js_undef() : js_to_string(ctx, A(1)), v;
  struct url_parts u;
  js_object *o;
  (void)t;
  if (in.type != JV_STR || !url_parse(in.u.str->data, base.type == JV_STR ? base.u.str->data : 0, &u))
    return js_null();
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(o, "protocol", js_mk_cstring(ctx, u.protocol), P_ENUMERABLE | P_WRITABLE);
  js_def(o, "username", js_mk_cstring(ctx, u.username), P_ENUMERABLE | P_WRITABLE);
  js_def(o, "password", js_mk_cstring(ctx, u.password), P_ENUMERABLE | P_WRITABLE);
  js_def(o, "hostname", js_mk_cstring(ctx, u.hostname), P_ENUMERABLE | P_WRITABLE);
  js_def(o, "port", js_mk_cstring(ctx, u.port), P_ENUMERABLE | P_WRITABLE);
  js_def(o, "pathname", js_mk_cstring(ctx, u.pathname), P_ENUMERABLE | P_WRITABLE);
  js_def(o, "search", js_mk_cstring(ctx, u.search), P_ENUMERABLE | P_WRITABLE);
  js_def(o, "hash", js_mk_cstring(ctx, u.hash), P_ENUMERABLE | P_WRITABLE);
  js_def(o, "opaque", js_bool(u.opaque), P_ENUMERABLE | P_WRITABLE);
  js_def(o, "hasSearch", js_bool(u.has_search), P_ENUMERABLE | P_WRITABLE);
  js_def(o, "hasHash", js_bool(u.has_hash), P_ENUMERABLE | P_WRITABLE);
  v = js_obj_value(o);
  gc_pop_scope();
  return v;
}

/* N.urlEscape(text, kind): percent-encode text as a pathname (0), query (1) or fragment (2) */
static js_value url_n_escape(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value in = js_to_string(ctx, A(0));
  char out[URL_MAX];
  (void)t;
  if (in.type != JV_STR)
    return js_undef();
  url_escape_kind(out, sizeof out, in.u.str->data, js_to_int_sat(ctx, A(1)));
  return js_mk_cstring(ctx, out);
}

/* N.queryParse("a=1&b=2"): [[name, value], ...] */
static js_value url_n_query_parse(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value q = js_to_string(ctx, A(0)), list;
  const char *p;
  (void)t;
  if (q.type != JV_STR)
    return js_undef();
  p = q.u.str->data;
  if (*p == '?')
    p++;
  gc_push_scope();
  gc_keep(q.u.str);
  list = js_obj_value(gc_keep(js_new_array(ctx, 0)));
  while (*p) {
    const char *amp = strchr(p, '&'), *end = amp ? amp : p + strlen(p), *eq = p;
    while (eq < end && *eq != '=')
      eq++;
    if (end > p) {
      char *k = form_decode(p, (int)(eq - p)),
           *v = form_decode(eq < end ? eq + 1 : end, eq < end ? (int)(end - eq - 1) : 0);
      js_object *pair = js_new_array(ctx, 0);
      gc_push_scope();
      gc_keep(pair);
      js_array_push(ctx, pair, js_mk_cstring(ctx, k ? k : ""));
      js_array_push(ctx, pair, js_mk_cstring(ctx, v ? v : ""));
      js_array_push(ctx, list.u.obj, js_obj_value(pair));
      gc_pop_scope();
      free(k);
      free(v);
    }
    p = amp ? amp + 1 : end;
  }
  gc_pop_scope();
  return list;
}

/* N.queryString([[name, value], ...]): application/x-www-form-urlencoded */
static js_value url_n_query_string(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  sbuf sb = {0, 0, 0};
  unsigned i, n = A(0).type == JV_OBJ ? js_array_len(A(0).u.obj) : 0;
  js_value r;
  (void)t;
  for (i = 0; i < n; i++) {
    js_value pair = js_get_index(ctx, A(0), i), k = js_to_string(ctx, js_get_index(ctx, pair, 0)),
             v = js_to_string(ctx, js_get_index(ctx, pair, 1));
    if (i)
      sb_putc(&sb, '&');
    sb_put_form(&sb, k.type == JV_STR ? k.u.str->data : "");
    sb_putc(&sb, '=');
    sb_put_form(&sb, v.type == JV_STR ? v.u.str->data : "");
  }
  r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
  free(sb.b);
  return r;
}


void url_natives_install(js_ctx *ctx, js_object *n)
{
  js_def_native(ctx, n, "urlParse", url_n_parse, 2);
  js_def_native(ctx, n, "urlEscape", url_n_escape, 2);
  js_def_native(ctx, n, "queryParse", url_n_query_parse, 1);
  js_def_native(ctx, n, "queryString", url_n_query_string, 1);
}

js_value url_classes_install(js_ctx *ctx)
{
  js_arena *ar = (js_arena *)malloc(sizeof *ar);
  js_parse_error pe;
  js_node *prog;
  js_value fn = js_undef(), cls = js_undef(), args[2], uc, sc;
  js_object *n;
  if (!ar)
    return cls;
  gc_push_scope();
  n = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  url_natives_install(ctx, n);
  js_arena_init(ar);
  prog = js_parse(url_classes_js, ar, &pe);
  if (prog && js_run(ctx, prog, &fn) == 0 && js_is_callable(fn)) {
    args[0] = js_obj_value(ctx->global);
    args[1] = js_obj_value(n);
    cls = js_call(ctx, fn, js_undef(), args, 2);
  }
  if (!prog || !arenas_push(ar)) {
    js_arena_free(ar);
    free(ar);
  }
  if (cls.type == JV_OBJ) {
    uc = js_get(ctx, cls, "URL");
    sc = js_get(ctx, cls, "URLSearchParams");
    js_def(ctx->global, "@URLSearchParams", sc, P_WRITABLE);
    js_def_native(ctx, uc.u.obj, "createObjectURL", url_create_object_url, 1);
    js_def_native(ctx, uc.u.obj, "revokeObjectURL", url_revoke_object_url, 1);
  }
  gc_pop_scope();
  return cls;
}

js_value make_search_params(js_ctx *ctx, const char *query)
{
  js_value ctor = js_get(ctx, js_obj_value(ctx->global), "@URLSearchParams"), q;
  if (!js_is_callable(ctor))
    return js_undef();
  gc_push_scope();
  q = js_mk_cstring(ctx, query);
  gc_keep(q.u.str);
  q = js_construct(ctx, ctor, &q, 1);
  gc_pop_scope();
  return q;
}

js_object *search_params_proto(js_ctx *ctx)
{
  js_value ctor = js_get(ctx, js_obj_value(ctx->global), "@URLSearchParams"), p;
  p = js_is_callable(ctor) ? js_get(ctx, ctor, "prototype") : js_undef();
  return p.type == JV_OBJ ? p.u.obj : 0;
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
/* The value a FormData stores: a Blob becomes a File (named by the filename argument, or "blob"); anything else
 * is stringified. A URLSearchParams always stringifies. */
static js_value form_value(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value v = A(1);
  if (t.u.obj->proto != g_formdata_proto || v.type != JV_OBJ || !is_blob(v.u.obj))
    return js_to_string(ctx, v);
  if (argc < 3 && v.u.obj->proto == g_file_proto)
    return v;
  js_value b = js_get(ctx, v, "@bytes"), tp = js_get(ctx, v, "type");
  return make_file(ctx, b.type == JV_STR ? b.u.str->data : "", b.type == JV_STR ? b.u.str->len : 0,
                   tp.type == JV_STR ? tp.u.str->data : "", argc > 2 ? arg_str(ctx, A(2)) : "blob", "",
                   (double)now_ms());
}
js_value sp_append(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *ka = sp_arr(ctx, t, "@k"), *va = sp_arr(ctx, t, "@v");
  if (ka && va) {
    gc_push_scope();
    js_array_push(ctx, ka, js_to_string(ctx, A(0)));
    js_array_push(ctx, va, form_value(ctx, t, args, argc));
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
  repl = form_value(ctx, t, args, argc);
  if (repl.type == JV_STR)
    gc_keep(repl.u.str);
  else if (repl.type == JV_OBJ)
    gc_keep(repl.u.obj);
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
/* --------------------------------------------------------------- FormData -- */
static void fd_collect(js_ctx *ctx, struct dom_node *n, js_object *ka, js_object *va)
{
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type == DOM_ELEMENT) {
      const char *tag = c->tag, *name = dom_get_attr(c, "name");
      const char *type = dom_get_attr(c, "type");
      if (name && name[0] && (streq(tag, "input") || streq(tag, "select") || streq(tag, "textarea"))) {
        bool skip =
            type && (streq(type, "submit") || streq(type, "button") || streq(type, "reset") || streq(type, "image"));
        if (type && streq(type, "file")) {
          int nf = c->files ? c->files->n : 0;
          for (int i = 0; i < (nf ? nf : 1); i++) { /* no file chosen: one empty File */
            js_array_push(ctx, ka, js_mk_cstring(ctx, name));
            js_array_push(ctx, va, dom_file_value(ctx, nf ? c->files : 0, i));
          }
        } else if (!skip) {
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
