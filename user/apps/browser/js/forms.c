/* HTMLFormElement. */

#include "bindings.h"

/* ------------------------------------------------------- HTMLFormElement --- */
js_value form_submit(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  if (n)
    browser_form_submit(n, 0);
  return js_undef();
}
js_value form_request_submit(js_ctx *ctx, js_value t, js_value *args, int argc)
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
/* form.elements: a fresh array of the form's listed controls */
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
js_value form_elements(js_ctx *ctx, js_value t, js_value *args, int argc)
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

/* Populate `o` with the WHATWG URL component properties of absolute URL `abs` */
void fill_url_props(js_ctx *ctx, js_object *o, const char *abs)
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

/* Anchor (and any href-bearing element) URL decomposition */
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
  js_value el_get_##name(js_ctx *ctx, js_value t, js_value *args, int argc)                                            \
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

js_value url_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
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
js_value url_tostring(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return js_get(ctx, t, "href");
}
