/* js_indexeddb.c: in-memory, per-page IndexedDB driven cooperatively by js_idb_pump. */
#include <stdbool.h>

#include "jsint.h"

/* Records live in a per-store array sorted by key (number < date < string < binary < array).
 * Requests mutate synchronously and queue their event for the next pump tick; a transaction
 * auto-commits once drained, and abort/unhandled error rolls back from an open-time snapshot. */

enum {
  IDBP_DB,
  IDBP_TXN,
  IDBP_STORE,
  IDBP_INDEX,
  IDBP_REQ,
  IDBP_OPENREQ,
  IDBP_CURSOR,
  IDBP_CURSORVAL,
  IDBP_KEYRANGE,
  IDBP_N
};
static js_object *g_idb_proto[IDBP_N];
static js_object *g_idb_dbs;     /* name -> dbrec (the page's databases; null-proto map) */
static js_object *g_idb_pending; /* IDBRequests awaiting event delivery */
static js_object *g_idb_txns;    /* live transactions, scanned for auto-commit/abort      */

#define IDB_ISNAN(x) ((x) != (x))

static js_value idb_gerr(const char *name, const char *msg) { return dom_exception(g_ctx, name, msg); }
static js_value idb_throw(js_ctx *ctx, const char *name, const char *msg) { return dom_throw(ctx, name, msg); }

/* ---- keys: validity, type rank and comparison (spec key ordering) -------- */

static bool idb_is_keyrange(js_value v) { return v.type == JV_OBJ && v.u.obj->proto == g_idb_proto[IDBP_KEYRANGE]; }
static bool idb_is_date(js_value v)
{
  return v.type == JV_OBJ && v.u.obj->kind == OBJ_PLAIN && v.u.obj->proto == g_ctx->date_proto;
}
static bool idb_is_binary(js_value v)
{
  return v.type == JV_OBJ && (v.u.obj->kind == OBJ_ARRAYBUFFER || v.u.obj->kind == OBJ_TYPEDARRAY);
}
static bool idb_valid_key(js_value v, int depth)
{
  if (depth > 64)
    return false;
  if (v.type == JV_NUM)
    return !IDB_ISNAN(v.u.num);
  if (idb_is_date(v))
    return !IDB_ISNAN(js_to_number(g_ctx, js_get(g_ctx, v, "@t")));
  if (v.type == JV_STR)
    return true;
  if (idb_is_binary(v))
    return true;
  if (v.type == JV_OBJ && v.u.obj->kind == OBJ_ARRAY) {
    unsigned i, n = js_array_len(v.u.obj);
    for (i = 0; i < n; i++)
      if (!idb_valid_key(js_get_index(g_ctx, v, i), depth + 1))
        return false;
    return true;
  }
  return false;
}
/* type rank: number 0, date 1, string 2, binary 3, array 4 */
static int idb_key_rank(js_value v)
{
  if (v.type == JV_NUM)
    return 0;
  if (idb_is_date(v))
    return 1;
  if (v.type == JV_STR)
    return 2;
  if (idb_is_binary(v))
    return 3;
  return 4; /* array */
}
static int idb_cmp(js_value a, js_value b)
{
  int ra = idb_key_rank(a), rb = idb_key_rank(b);
  if (ra != rb)
    return ra < rb ? -1 : 1;
  switch (ra) {
  case 0: {
    double x = a.u.num, y = b.u.num;
    return x < y ? -1 : x > y ? 1 : 0;
  }
  case 1: {
    double x = js_to_number(g_ctx, js_get(g_ctx, a, "@t"));
    double y = js_to_number(g_ctx, js_get(g_ctx, b, "@t"));
    return x < y ? -1 : x > y ? 1 : 0;
  }
  case 2: {
    int la = a.u.str->len, lb = b.u.str->len, m = la < lb ? la : lb;
    int c = m ? memcmp(a.u.str->data, b.u.str->data, (unsigned)m) : 0;
    if (c)
      return c < 0 ? -1 : 1;
    return la < lb ? -1 : la > lb ? 1 : 0;
  }
  case 3: {
    int la = 0, lb = 0;
    const unsigned char *pa = view_bytes(a, &la), *pb = view_bytes(b, &lb);
    int m = la < lb ? la : lb, c = (m && pa && pb) ? memcmp(pa, pb, (unsigned)m) : 0;
    if (c)
      return c < 0 ? -1 : 1;
    return la < lb ? -1 : la > lb ? 1 : 0;
  }
  default: { /* array: element-wise, shorter first on a prefix tie */
    unsigned na = js_array_len(a.u.obj), nb = js_array_len(b.u.obj), i, m = na < nb ? na : nb;
    for (i = 0; i < m; i++) {
      int c = idb_cmp(js_get_index(g_ctx, a, i), js_get_index(g_ctx, b, i));
      if (c)
        return c;
    }
    return na < nb ? -1 : na > nb ? 1 : 0;
  }
  }
}

/* ---- structured clone (deep copy so store <-> page share no references) ---
 */

static js_value idb_clone_buffer(js_ctx *ctx, js_object *bufobj)
{
  js_arraybuffer *ab = bufobj ? (js_arraybuffer *)bufobj->host : 0;
  js_value ctor = js_get(ctx, js_obj_value(ctx->global), "ArrayBuffer");
  js_value arg = js_number(ab ? ab->byteLength : 0), out;
  out = js_construct(ctx, ctor, &arg, 1);
  if (out.type == JV_OBJ && out.u.obj->host && ab && ab->byteLength > 0) {
    js_arraybuffer *nb = (js_arraybuffer *)out.u.obj->host;
    if (nb->data)
      memcpy(nb->data, ab->data, (unsigned)ab->byteLength);
  }
  return out;
}
static const char *idb_ta_name(int type)
{
  switch (type) {
  case TA_I8:
    return "Int8Array";
  case TA_U8:
    return "Uint8Array";
  case TA_U8C:
    return "Uint8ClampedArray";
  case TA_I16:
    return "Int16Array";
  case TA_U16:
    return "Uint16Array";
  case TA_I32:
    return "Int32Array";
  case TA_U32:
    return "Uint32Array";
  case TA_F32:
    return "Float32Array";
  case TA_F64:
    return "Float64Array";
  case TA_I64:
    return "BigInt64Array";
  case TA_U64:
    return "BigUint64Array";
  case TA_F16:
    return "Float16Array";
  default:
    return "DataView";
  }
}
static js_value idb_clone(js_ctx *ctx, js_value v, int depth)
{
  if (depth > 200)
    return js_null(); /* cycle/overdeep guard: lossy but safe */
  if (v.type == JV_SYMBOL || js_is_callable(v))
    return idb_throw(ctx, "DataCloneError", "value could not be cloned");
  switch (v.type) {
  case JV_UNDEF:
  case JV_NULL:
  case JV_BOOL:
  case JV_NUM:
  case JV_BIGINT:
    return v;
  case JV_STR:
    return js_mk_string(ctx, v.u.str->data, v.u.str->len);
  default:
    break;
  }
  {
    js_object *o = v.u.obj;
    js_value out;
    gc_push_scope();
    gc_keep(o);
    if (idb_is_date(v)) {
      js_object *d = (js_object *)gc_keep(js_new_object(ctx, ctx->date_proto));
      js_def(d, "@t", js_get(ctx, v, "@t"), 0);
      out = js_obj_value(d);
    } else if (o->proto == ctx->regexp_proto) {
      js_value src = js_get(ctx, v, "source"), fl = js_get(ctx, v, "flags");
      out = js_make_regexp(ctx, src.type == JV_STR ? src.u.str->data : "", fl.type == JV_STR ? fl.u.str->data : "");
    } else if (o->kind == OBJ_ARRAYBUFFER) {
      out = idb_clone_buffer(ctx, o);
    } else if (o->kind == OBJ_TYPEDARRAY) {
      js_typedarray *ta = (js_typedarray *)o->host;
      if (!ta || !ta->buffer) {
        out = js_null();
      } else {
        js_value buf = (js_value){0}, argv[3], ctor;
        buf = idb_clone_buffer(ctx, ta->buffer);
        if (buf.type == JV_OBJ)
          gc_keep(buf.u.obj);
        ctor = js_get(ctx, js_obj_value(ctx->global), idb_ta_name(ta->type));
        argv[0] = buf;
        argv[1] = js_number(ta->offset);
        argv[2] = js_number(ta_length(ta));
        out = js_construct(ctx, ctor, argv, 3);
      }
    } else if (o->kind == OBJ_ARRAY) {
      unsigned i, n = js_array_len(o);
      js_object *a = (js_object *)gc_keep(js_new_array(ctx, 0));
      for (i = 0; i < n; i++) {
        js_value cv = idb_clone(ctx, js_get_index(ctx, v, i), depth + 1);
        if (ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
        js_array_push(ctx, a, cv);
      }
      out = js_obj_value(a);
    } else {
      /* own enumerable data properties only (engine internals are non-enumerable) */
      js_object *n = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
      int i;
      for (i = 0; i < o->props.n; i++) {
        pentry *pe = &o->props.e[i];
        js_value cv;
        if (!(pe->flags & P_ENUMERABLE) || (pe->flags & P_ACCESSOR))
          continue;
        cv = idb_clone(ctx, pe->value, depth + 1);
        if (ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
        js_def(n, pe->key, cv, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
      }
      out = js_obj_value(n);
    }
    gc_pop_scope();
    return out;
  }
}

/* Navigate a dotted path; JV_UNDEF if a hop is missing, an empty path yields the value itself. */
static js_value idb_eval_path(js_ctx *ctx, js_value value, const char *path)
{
  char seg[128];
  js_value cur = value;
  const char *p = path;
  if (!*p)
    return value;
  for (;;) {
    int i = 0;
    while (*p && *p != '.' && i < (int)sizeof seg - 1)
      seg[i++] = *p++;
    seg[i] = 0;
    if (cur.type != JV_OBJ && cur.type != JV_STR)
      return js_undef();
    cur = js_get(ctx, cur, seg);
    if (cur.type == JV_UNDEF)
      return js_undef();
    if (!*p)
      return cur;
    p++; /* skip the '.' */
  }
}
/* keyPath may be a string, an array of strings (a compound key), or null. */
static js_value idb_extract_key(js_ctx *ctx, js_value value, js_value keyPath)
{
  if (keyPath.type == JV_STR)
    return idb_eval_path(ctx, value, keyPath.u.str->data);
  if (keyPath.type == JV_OBJ && keyPath.u.obj->kind == OBJ_ARRAY) {
    unsigned i, n = js_array_len(keyPath.u.obj);
    js_object *arr;
    gc_push_scope();
    if (value.type == JV_OBJ)
      gc_keep(value.u.obj);
    gc_keep(keyPath.u.obj);
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    for (i = 0; i < n; i++) {
      js_value seg = js_get_index(ctx, keyPath, i);
      js_value k = idb_eval_path(ctx, value, seg.type == JV_STR ? seg.u.str->data : "");
      if (k.type == JV_UNDEF) {
        gc_pop_scope();
        return js_undef();
      }
      js_array_push(ctx, arr, k);
    }
    gc_pop_scope();
    return js_obj_value(arr);
  }
  return js_undef();
}
/* Create the intermediate objects along `path` and store `key` at the leaf. */
static void idb_inject_key(js_ctx *ctx, js_value value, const char *path, js_value key)
{
  char seg[128];
  js_value cur = value;
  const char *p = path;
  if (!*p || value.type != JV_OBJ)
    return;
  for (;;) {
    int i = 0;
    while (*p && *p != '.' && i < (int)sizeof seg - 1)
      seg[i++] = *p++;
    seg[i] = 0;
    if (!*p) {
      js_set(ctx, cur, seg, key);
      return;
    }
    {
      js_value next = js_get(ctx, cur, seg);
      if (next.type != JV_OBJ) {
        next = js_obj_value(js_new_object(ctx, ctx->object_proto));
        js_set(ctx, cur, seg, next);
      }
      cur = next;
    }
    p++;
  }
}

/* ---- database / store record access (the persistent-for-the-page store) ---
 */

static js_value idb_dbrec(const char *name)
{
  return g_idb_dbs ? js_get(g_ctx, js_obj_value(g_idb_dbs), name) : js_undef();
}
static js_value idb_storerec(js_value dbrec, const char *sname)
{
  js_value stores = js_get(g_ctx, dbrec, "stores");
  return js_get(g_ctx, stores, sname);
}
static js_object *idb_records(js_value storerec)
{
  js_value r = js_get(g_ctx, storerec, "records");
  return r.type == JV_OBJ ? r.u.obj : 0;
}
/* Binary search for `key`; *found set, returns the match or insertion slot. */
static int idb_find(js_object *recs, js_value key, int *found)
{
  int lo = 0, hi = (int)js_array_len(recs) - 1;
  *found = 0;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    js_value e = js_get_index(g_ctx, js_obj_value(recs), (unsigned)mid);
    int c = idb_cmp(js_get(g_ctx, e, "key"), key);
    if (c == 0) {
      *found = 1;
      return mid;
    }
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return lo;
}
static void idb_arr_insert(js_object *arr, int pos, js_value v)
{
  int i, n = (int)js_array_len(arr);
  js_array_set_len(g_ctx, arr, (unsigned)(n + 1));
  for (i = n; i > pos; i--)
    js_set_index(g_ctx, js_obj_value(arr), (unsigned)i, js_get_index(g_ctx, js_obj_value(arr), (unsigned)(i - 1)));
  js_set_index(g_ctx, js_obj_value(arr), (unsigned)pos, v);
}
static void idb_arr_remove(js_object *arr, int pos)
{
  int i, n = (int)js_array_len(arr);
  for (i = pos; i < n - 1; i++)
    js_set_index(g_ctx, js_obj_value(arr), (unsigned)i, js_get_index(g_ctx, js_obj_value(arr), (unsigned)(i + 1)));
  if (n > 0)
    js_array_set_len(g_ctx, arr, (unsigned)(n - 1));
}

/* Sorted list of a map object's own enumerable keys (store/index names). */
static js_object *idb_sorted_keys(js_object *map)
{
  js_object *arr;
  int i, j;
  gc_push_scope();
  if (map)
    gc_keep(map);
  arr = (js_object *)gc_keep(js_new_array(g_ctx, 0));
  if (map)
    for (i = 0; i < map->props.n; i++)
      if (map->props.e[i].flags & P_ENUMERABLE)
        js_array_push(g_ctx, arr, js_mk_cstring(g_ctx, map->props.e[i].key));
  for (i = 1; i < (int)js_array_len(arr); i++) { /* insertion sort */
    js_value x = js_get_index(g_ctx, js_obj_value(arr), (unsigned)i);
    for (j = i - 1; j >= 0; j--) {
      js_value y = js_get_index(g_ctx, js_obj_value(arr), (unsigned)j);
      if (idb_cmp(y, x) <= 0)
        break;
      js_set_index(g_ctx, js_obj_value(arr), (unsigned)(j + 1), y);
    }
    js_set_index(g_ctx, js_obj_value(arr), (unsigned)(j + 1), x);
  }
  gc_pop_scope();
  return arr;
}
/* A DOMStringList-lite: a sorted array with contains()/item() bolted on. */
static js_value idb_stringlist_contains(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *want = arg_str(ctx, A(0));
  unsigned i, n = t.type == JV_OBJ ? js_array_len(t.u.obj) : 0;
  for (i = 0; i < n; i++) {
    js_value s = js_get_index(ctx, t, i);
    if (s.type == JV_STR && streq(s.u.str->data, want))
      return js_bool(1);
  }
  return js_bool(0);
}
static js_value idb_stringlist_item(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int i = js_to_int_sat(ctx, A(0));
  if (t.type != JV_OBJ || i < 0 || i >= (int)js_array_len(t.u.obj))
    return js_null();
  return js_get_index(ctx, t, (unsigned)i);
}
static js_value idb_stringlist(js_object *namesMap)
{
  js_object *arr = idb_sorted_keys(namesMap);
  gc_push_scope();
  gc_keep(arr);
  def_method(arr, "contains", idb_stringlist_contains, 1);
  def_method(arr, "item", idb_stringlist_item, 1);
  gc_pop_scope();
  return js_obj_value(arr);
}

/* ---- small runtime helpers over the wrappers ----------------------------- */

static void idb_str_field(js_value o, const char *k, char *buf, int cap)
{
  js_value v = (o.type == JV_OBJ) ? js_get(g_ctx, o, k) : js_undef();
  if (v.type == JV_STR)
    strlcpy(buf, v.u.str->data, cap);
  else
    buf[0] = 0;
}
static bool idb_txn_active(js_value txn)
{
  js_value s;
  if (txn.type != JV_OBJ)
    return false;
  s = js_get(g_ctx, txn, "@state");
  return s.type == JV_STR && streq(s.u.str->data, "active");
}
static bool idb_txn_write(js_value txn)
{
  js_value m;
  if (txn.type != JV_OBJ)
    return false;
  m = js_get(g_ctx, txn, "mode");
  return m.type == JV_STR && (streq(m.u.str->data, "readwrite") || streq(m.u.str->data, "versionchange"));
}
static bool idb_txn_isversion(js_value txn)
{
  js_value m;
  if (txn.type != JV_OBJ)
    return false;
  m = js_get(g_ctx, txn, "mode");
  return m.type == JV_STR && streq(m.u.str->data, "versionchange");
}
static void idb_txn_pending(js_value txn, int delta)
{
  double p;
  if (txn.type != JV_OBJ)
    return;
  p = js_to_number(g_ctx, js_get(g_ctx, txn, "@pending"));
  js_set(g_ctx, txn, "@pending", js_number(p + delta));
}
static void idb_enqueue(js_object *req)
{
  if (g_idb_pending && req)
    js_array_push(g_ctx, g_idb_pending, js_obj_value(req));
}

/* Build an IDBRequest (or IDBOpenDBRequest when `open`), pending with no result. */
static js_object *idb_make_request(js_value source, js_value txn, int open)
{
  js_object *req;
  gc_push_scope();
  if (source.type == JV_OBJ)
    gc_keep(source.u.obj);
  if (txn.type == JV_OBJ)
    gc_keep(txn.u.obj);
  req = (js_object *)gc_keep(js_new_object(g_ctx, g_idb_proto[open ? IDBP_OPENREQ : IDBP_REQ]));
  js_def(req, "@result", js_undef(), P_WRITABLE);
  js_def(req, "@error", js_null(), P_WRITABLE);
  js_def(req, "@event", js_mk_cstring(g_ctx, "success"), P_WRITABLE);
  js_def(req, "@txn", txn.type == JV_OBJ ? txn : js_null(), P_WRITABLE);
  js_def(req, "result", js_undef(), P_WRITABLE | P_ENUMERABLE);
  js_def(req, "error", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(req, "source", source.type == JV_OBJ ? source : js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(req, "transaction", txn.type == JV_OBJ ? txn : js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(req, "readyState", js_mk_cstring(g_ctx, "pending"), P_WRITABLE | P_ENUMERABLE);
  js_def(req, "onsuccess", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(req, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  if (open) {
    js_def(req, "onupgradeneeded", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(req, "onblocked", js_null(), P_WRITABLE | P_ENUMERABLE);
  }
  gc_pop_scope();
  return req;
}
/* Queue a request that delivers a success result (counts against its txn). */
static js_value idb_succeed(js_value source, js_value txn, js_value result)
{
  js_object *req;
  gc_push_scope();
  if (source.type == JV_OBJ)
    gc_keep(source.u.obj);
  if (txn.type == JV_OBJ)
    gc_keep(txn.u.obj);
  if (result.type == JV_OBJ)
    gc_keep(result.u.obj);
  req = (js_object *)gc_keep(idb_make_request(source, txn, 0));
  js_set(g_ctx, js_obj_value(req), "@result", result);
  idb_txn_pending(txn, 1);
  idb_enqueue(req);
  gc_pop_scope();
  return js_obj_value(req);
}
/* Queue a request that will deliver an error (bubbles, may abort its txn). */
static js_value idb_fail(js_value source, js_value txn, js_value err)
{
  js_object *req;
  gc_push_scope();
  if (source.type == JV_OBJ)
    gc_keep(source.u.obj);
  if (txn.type == JV_OBJ)
    gc_keep(txn.u.obj);
  if (err.type == JV_OBJ)
    gc_keep(err.u.obj);
  req = (js_object *)gc_keep(idb_make_request(source, txn, 0));
  js_set(g_ctx, js_obj_value(req), "@error", err);
  idb_txn_pending(txn, 1);
  idb_enqueue(req);
  gc_pop_scope();
  return js_obj_value(req);
}

/* A store record entry; always replaced whole so snapshots can hold the old objects. */
static js_value idb_new_entry(js_value key, js_value value)
{
  js_object *e;
  gc_push_scope();
  if (key.type == JV_OBJ)
    gc_keep(key.u.obj);
  if (value.type == JV_OBJ)
    gc_keep(value.u.obj);
  e = (js_object *)gc_keep(js_new_object(g_ctx, 0));
  js_def(e, "key", key, P_WRITABLE);
  js_def(e, "value", value, P_WRITABLE);
  gc_pop_scope();
  return js_obj_value(e);
}
static js_value idb_gen_key(js_value sr)
{
  double g = js_to_number(g_ctx, js_get(g_ctx, sr, "keyGen"));
  if (g < 1)
    g = 1;
  js_set(g_ctx, sr, "keyGen", js_number(g + 1));
  return js_number(g);
}
static void idb_bump_key(js_value sr, js_value k)
{
  double cur, kv, f;
  if (k.type != JV_NUM || IDB_ISNAN(k.u.num))
    return;
  cur = js_to_number(g_ctx, js_get(g_ctx, sr, "keyGen"));
  if (cur < 1)
    cur = 1;
  kv = k.u.num;
  if (kv >= cur) {
    f = (double)(long long)kv;
    js_set(g_ctx, sr, "keyGen", js_number(f + 1));
  }
}

/* Shallow snapshot of a database's stores for rolling back an aborted transaction. */
static js_value idb_snapshot(const char *dbname)
{
  js_value dbrec = idb_dbrec(dbname), stores = js_get(g_ctx, dbrec, "stores");
  js_object *snap;
  gc_push_scope();
  snap = (js_object *)gc_keep(js_new_object(g_ctx, 0));
  if (stores.type == JV_OBJ) {
    js_object *sm = stores.u.obj;
    int i;
    for (i = 0; i < sm->props.n; i++) {
      js_value srv;
      js_object *cs, *rc, *orig, *ic;
      unsigned k, rn;
      if (!(sm->props.e[i].flags & P_ENUMERABLE))
        continue;
      srv = sm->props.e[i].value;
      cs = (js_object *)gc_keep(js_new_object(g_ctx, 0));
      js_def(cs, "name", js_get(g_ctx, srv, "name"), P_WRITABLE);
      js_def(cs, "keyPath", js_get(g_ctx, srv, "keyPath"), P_WRITABLE);
      js_def(cs, "autoIncrement", js_get(g_ctx, srv, "autoIncrement"), P_WRITABLE);
      js_def(cs, "keyGen", js_get(g_ctx, srv, "keyGen"), P_WRITABLE);
      rc = (js_object *)gc_keep(js_new_array(g_ctx, 0));
      orig = idb_records(srv);
      rn = orig ? js_array_len(orig) : 0;
      for (k = 0; k < rn; k++)
        js_array_push(g_ctx, rc, js_get_index(g_ctx, js_obj_value(orig), k));
      js_def(cs, "records", js_obj_value(rc), P_WRITABLE);
      ic = (js_object *)gc_keep(js_new_object(g_ctx, 0));
      {
        js_value ix = js_get(g_ctx, srv, "indexes");
        if (ix.type == JV_OBJ) {
          js_object *im = ix.u.obj;
          int j;
          for (j = 0; j < im->props.n; j++)
            if (im->props.e[j].flags & P_ENUMERABLE)
              js_def(ic, im->props.e[j].key, im->props.e[j].value, P_ENUMERABLE | P_WRITABLE);
        }
      }
      js_def(cs, "indexes", js_obj_value(ic), P_WRITABLE);
      js_def(snap, sm->props.e[i].key, js_obj_value(cs), P_ENUMERABLE | P_WRITABLE);
    }
  }
  gc_pop_scope();
  return js_obj_value(snap);
}
static void idb_restore(const char *dbname, js_value snap)
{
  js_value dbrec = idb_dbrec(dbname);
  js_object *ns;
  if (dbrec.type != JV_OBJ)
    return;
  gc_push_scope();
  ns = (js_object *)gc_keep(js_new_object(g_ctx, 0));
  if (snap.type == JV_OBJ)
    gc_keep(snap.u.obj);
  if (snap.type == JV_OBJ) {
    js_object *sm = snap.u.obj;
    int i;
    for (i = 0; i < sm->props.n; i++) {
      js_value cs;
      js_object *rs, *rc, *ic;
      unsigned k, rn;
      js_value snaprecs, snapix;
      if (!(sm->props.e[i].flags & P_ENUMERABLE))
        continue;
      cs = sm->props.e[i].value;
      rs = (js_object *)gc_keep(js_new_object(g_ctx, 0));
      js_def(rs, "name", js_get(g_ctx, cs, "name"), P_WRITABLE);
      js_def(rs, "keyPath", js_get(g_ctx, cs, "keyPath"), P_WRITABLE);
      js_def(rs, "autoIncrement", js_get(g_ctx, cs, "autoIncrement"), P_WRITABLE);
      js_def(rs, "keyGen", js_get(g_ctx, cs, "keyGen"), P_WRITABLE);
      rc = (js_object *)gc_keep(js_new_array(g_ctx, 0));
      snaprecs = js_get(g_ctx, cs, "records");
      rn = snaprecs.type == JV_OBJ ? js_array_len(snaprecs.u.obj) : 0;
      for (k = 0; k < rn; k++)
        js_array_push(g_ctx, rc, js_get_index(g_ctx, snaprecs, k));
      js_def(rs, "records", js_obj_value(rc), P_WRITABLE);
      ic = (js_object *)gc_keep(js_new_object(g_ctx, 0));
      snapix = js_get(g_ctx, cs, "indexes");
      if (snapix.type == JV_OBJ) {
        js_object *im = snapix.u.obj;
        int j;
        for (j = 0; j < im->props.n; j++)
          if (im->props.e[j].flags & P_ENUMERABLE)
            js_def(ic, im->props.e[j].key, im->props.e[j].value, P_ENUMERABLE | P_WRITABLE);
      }
      js_def(rs, "indexes", js_obj_value(ic), P_WRITABLE);
      js_def(ns, sm->props.e[i].key, js_obj_value(rs), P_ENUMERABLE | P_WRITABLE);
    }
  }
  js_set(g_ctx, dbrec, "stores", js_obj_value(ns));
  gc_pop_scope();
}

/* Is `query` (key or range) satisfied by `key`? Undefined/null matches all. */
static bool idb_query_match(js_value query, js_value key)
{
  if (query.type == JV_UNDEF || query.type == JV_NULL)
    return true;
  if (idb_is_keyrange(query)) {
    js_value lo = js_get(g_ctx, query, "@lower"), hi = js_get(g_ctx, query, "@upper");
    bool loO = js_to_boolean(js_get(g_ctx, query, "@lowerOpen"));
    bool hiO = js_to_boolean(js_get(g_ctx, query, "@upperOpen"));
    if (lo.type != JV_UNDEF) {
      int c = idb_cmp(key, lo);
      if (c < 0 || (c == 0 && loO))
        return false;
    }
    if (hi.type != JV_UNDEF) {
      int c = idb_cmp(key, hi);
      if (c > 0 || (c == 0 && hiO))
        return false;
    }
    return true;
  }
  return idb_cmp(key, query) == 0;
}
static bool idb_query_valid(js_value q) { return idb_is_keyrange(q) || idb_valid_key(q, 0); }

/* ---- wrapper factories --------------------------------------------------- */

static js_value idb_make_db(const char *name, double version)
{
  js_object *db;
  gc_push_scope();
  db = (js_object *)gc_keep(js_new_object(g_ctx, g_idb_proto[IDBP_DB]));
  js_def(db, "@dbname", js_mk_cstring(g_ctx, name), P_WRITABLE);
  js_def(db, "@closed", js_bool(0), P_WRITABLE);
  js_def(db, "@vtxn", js_null(), P_WRITABLE);
  js_def(db, "name", js_mk_cstring(g_ctx, name), P_WRITABLE | P_ENUMERABLE);
  js_def(db, "version", js_number(version), P_WRITABLE | P_ENUMERABLE);
  js_def(db, "onversionchange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(db, "onclose", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(db, "onabort", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(db, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  gc_pop_scope();
  return js_obj_value(db);
}
static js_value idb_make_txn(js_value db, js_object *scope, const char *mode, js_value openreq)
{
  js_object *txn;
  char dbn[128];
  bool write = streq(mode, "readwrite") || streq(mode, "versionchange");
  idb_str_field(db, "@dbname", dbn, sizeof dbn);
  gc_push_scope();
  if (db.type == JV_OBJ)
    gc_keep(db.u.obj);
  if (scope)
    gc_keep(scope);
  if (openreq.type == JV_OBJ)
    gc_keep(openreq.u.obj);
  txn = (js_object *)gc_keep(js_new_object(g_ctx, g_idb_proto[IDBP_TXN]));
  js_def(txn, "@state", js_mk_cstring(g_ctx, "active"), P_WRITABLE);
  js_def(txn, "@pending", js_number(0), P_WRITABLE);
  js_def(txn, "@db", db, P_WRITABLE);
  js_def(txn, "@dbname", js_mk_cstring(g_ctx, dbn), P_WRITABLE);
  js_def(txn, "@scope", js_obj_value(scope ? scope : js_new_array(g_ctx, 0)), P_WRITABLE);
  js_def(txn, "@aborted", js_bool(0), P_WRITABLE);
  js_def(txn, "@abort_fired", js_bool(0), P_WRITABLE);
  js_def(txn, "@openreq", openreq.type == JV_OBJ ? openreq : js_null(), P_WRITABLE);
  js_def(txn, "@snap", write ? idb_snapshot(dbn) : js_null(), P_WRITABLE);
  js_def(txn, "mode", js_mk_cstring(g_ctx, mode), P_WRITABLE | P_ENUMERABLE);
  js_def(txn, "db", db, P_WRITABLE | P_ENUMERABLE);
  js_def(txn, "error", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(txn, "oncomplete", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(txn, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(txn, "onabort", js_null(), P_WRITABLE | P_ENUMERABLE);
  if (g_idb_txns)
    js_array_push(g_ctx, g_idb_txns, js_obj_value(txn));
  gc_pop_scope();
  return js_obj_value(txn);
}
static js_value idb_make_store(js_value txn, const char *dbname, const char *sname)
{
  js_value sr = idb_storerec(idb_dbrec(dbname), sname);
  js_object *st;
  gc_push_scope();
  if (txn.type == JV_OBJ)
    gc_keep(txn.u.obj);
  st = (js_object *)gc_keep(js_new_object(g_ctx, g_idb_proto[IDBP_STORE]));
  js_def(st, "@txn", txn, P_WRITABLE);
  js_def(st, "@dbname", js_mk_cstring(g_ctx, dbname), P_WRITABLE);
  js_def(st, "@sname", js_mk_cstring(g_ctx, sname), P_WRITABLE);
  js_def(st, "name", js_mk_cstring(g_ctx, sname), P_WRITABLE | P_ENUMERABLE);
  js_def(st, "keyPath", sr.type == JV_OBJ ? js_get(g_ctx, sr, "keyPath") : js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(st, "autoIncrement", sr.type == JV_OBJ ? js_get(g_ctx, sr, "autoIncrement") : js_bool(0),
         P_WRITABLE | P_ENUMERABLE);
  js_def(st, "transaction", txn, P_WRITABLE | P_ENUMERABLE);
  gc_pop_scope();
  return js_obj_value(st);
}
static js_value idb_make_index(js_value txn, const char *dbname, const char *sname, const char *iname)
{
  js_value sr = idb_storerec(idb_dbrec(dbname), sname);
  js_value ir = js_get(g_ctx, js_get(g_ctx, sr, "indexes"), iname);
  js_object *ix;
  gc_push_scope();
  if (txn.type == JV_OBJ)
    gc_keep(txn.u.obj);
  ix = (js_object *)gc_keep(js_new_object(g_ctx, g_idb_proto[IDBP_INDEX]));
  js_def(ix, "@txn", txn, P_WRITABLE);
  js_def(ix, "@dbname", js_mk_cstring(g_ctx, dbname), P_WRITABLE);
  js_def(ix, "@sname", js_mk_cstring(g_ctx, sname), P_WRITABLE);
  js_def(ix, "@iname", js_mk_cstring(g_ctx, iname), P_WRITABLE);
  js_def(ix, "name", js_mk_cstring(g_ctx, iname), P_WRITABLE | P_ENUMERABLE);
  js_def(ix, "keyPath", ir.type == JV_OBJ ? js_get(g_ctx, ir, "keyPath") : js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(ix, "unique", ir.type == JV_OBJ ? js_get(g_ctx, ir, "unique") : js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(ix, "multiEntry", ir.type == JV_OBJ ? js_get(g_ctx, ir, "multiEntry") : js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(ix, "objectStore", idb_make_store(txn, dbname, sname), P_WRITABLE | P_ENUMERABLE);
  gc_pop_scope();
  return js_obj_value(ix);
}
static js_value idb_make_keyrange(js_value lo, js_value hi, int loOpen, int hiOpen)
{
  js_object *r;
  gc_push_scope();
  if (lo.type == JV_OBJ)
    gc_keep(lo.u.obj);
  if (hi.type == JV_OBJ)
    gc_keep(hi.u.obj);
  r = (js_object *)gc_keep(js_new_object(g_ctx, g_idb_proto[IDBP_KEYRANGE]));
  js_def(r, "@lower", lo, P_WRITABLE);
  js_def(r, "@upper", hi, P_WRITABLE);
  js_def(r, "@lowerOpen", js_bool(loOpen), P_WRITABLE);
  js_def(r, "@upperOpen", js_bool(hiOpen), P_WRITABLE);
  js_def(r, "lower", lo, P_ENUMERABLE);
  js_def(r, "upper", hi, P_ENUMERABLE);
  js_def(r, "lowerOpen", js_bool(loOpen), P_ENUMERABLE);
  js_def(r, "upperOpen", js_bool(hiOpen), P_ENUMERABLE);
  gc_pop_scope();
  return js_obj_value(r);
}

/* cursor stepping: an ordered {ikey,pkey} list, values read live */

typedef struct {
  js_value txn, sr, ir;
  char db[128], sn[128], in[128];
} idbix;

static js_value idb_pair(js_value ikey, js_value pkey)
{
  js_object *e;
  gc_push_scope();
  if (ikey.type == JV_OBJ)
    gc_keep(ikey.u.obj);
  if (pkey.type == JV_OBJ)
    gc_keep(pkey.u.obj);
  e = (js_object *)gc_keep(js_new_object(g_ctx, 0));
  js_def(e, "ikey", ikey, P_WRITABLE);
  js_def(e, "pkey", pkey, P_WRITABLE);
  gc_pop_scope();
  return js_obj_value(e);
}
static void idb_sort_pairs(js_object *arr)
{
  int i, j, n = (int)js_array_len(arr);
  for (i = 1; i < n; i++) {
    js_value x = js_get_index(g_ctx, js_obj_value(arr), (unsigned)i);
    js_value xi = js_get(g_ctx, x, "ikey"), xp = js_get(g_ctx, x, "pkey");
    for (j = i - 1; j >= 0; j--) {
      js_value y = js_get_index(g_ctx, js_obj_value(arr), (unsigned)j);
      int c = idb_cmp(js_get(g_ctx, y, "ikey"), xi);
      if (c == 0)
        c = idb_cmp(js_get(g_ctx, y, "pkey"), xp);
      if (c <= 0)
        break;
      js_set_index(g_ctx, js_obj_value(arr), (unsigned)(j + 1), y);
    }
    js_set_index(g_ctx, js_obj_value(arr), (unsigned)(j + 1), x);
  }
}
static void idb_dedupe_pairs(js_object *arr)
{ /* keep first of each ikey (sorted) */
  int i, n = (int)js_array_len(arr), m;
  js_object *out;
  gc_push_scope();
  gc_keep(arr);
  out = (js_object *)gc_keep(js_new_array(g_ctx, 0));
  for (i = 0; i < n; i++) {
    js_value e = js_get_index(g_ctx, js_obj_value(arr), (unsigned)i);
    if (i > 0) {
      js_value pe = js_get_index(g_ctx, js_obj_value(arr), (unsigned)(i - 1));
      if (idb_cmp(js_get(g_ctx, pe, "ikey"), js_get(g_ctx, e, "ikey")) == 0)
        continue;
    }
    js_array_push(g_ctx, out, e);
  }
  m = (int)js_array_len(out);
  js_array_set_len(g_ctx, arr, (unsigned)m);
  for (i = 0; i < m; i++)
    js_set_index(g_ctx, js_obj_value(arr), (unsigned)i, js_get_index(g_ctx, js_obj_value(out), (unsigned)i));
  gc_pop_scope();
}
static void idb_reverse(js_object *arr)
{
  int i, n = (int)js_array_len(arr);
  for (i = 0; i < n / 2; i++) {
    js_value a = js_get_index(g_ctx, js_obj_value(arr), (unsigned)i);
    js_value b = js_get_index(g_ctx, js_obj_value(arr), (unsigned)(n - 1 - i));
    js_set_index(g_ctx, js_obj_value(arr), (unsigned)i, b);
    js_set_index(g_ctx, js_obj_value(arr), (unsigned)(n - 1 - i), a);
  }
}
static bool idb_dir_prev(const char *d) { return d && (streq(d, "prev") || streq(d, "prevunique")); }
static bool idb_dir_unique(const char *d) { return d && (streq(d, "nextunique") || streq(d, "prevunique")); }

/* Object-store cursor steps: matching records in key order. */
static js_object *idb_store_steps(js_ctx *ctx, js_value sr, js_value query, bool prev)
{
  js_object *recs = idb_records(sr), *steps;
  unsigned i, n = recs ? js_array_len(recs) : 0;
  gc_push_scope();
  steps = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < n; i++) {
    js_value e = js_get_index(ctx, js_obj_value(recs), i);
    js_value key = js_get(ctx, e, "key");
    if (idb_query_match(query, key))
      js_array_push(ctx, steps, idb_pair(key, key));
  }
  if (prev)
    idb_reverse(steps);
  gc_pop_scope();
  return steps;
}
/* Index cursor steps: (indexKey, primaryKey) pairs, multiEntry expanded, sorted. */
static js_object *idb_index_pairs(js_ctx *ctx, js_value sr, js_value irec, js_value query, bool unique, bool prev)
{
  js_value ikp = js_get(ctx, irec, "keyPath");
  bool multi = js_to_boolean(js_get(ctx, irec, "multiEntry"));
  js_object *recs = idb_records(sr), *pairs;
  unsigned r, rn = recs ? js_array_len(recs) : 0;
  gc_push_scope();
  pairs = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (r = 0; r < rn; r++) {
    js_value e = js_get_index(ctx, js_obj_value(recs), r);
    js_value pkey = js_get(ctx, e, "key");
    js_value ik = idb_extract_key(ctx, js_get(ctx, e, "value"), ikp);
    if (ik.type == JV_UNDEF)
      continue;
    if (multi && ik.type == JV_OBJ && ik.u.obj->kind == OBJ_ARRAY) {
      unsigned a, an = js_array_len(ik.u.obj);
      for (a = 0; a < an; a++) {
        js_value el = js_get_index(ctx, ik, a);
        if (!idb_valid_key(el, 0) || !idb_query_match(query, el))
          continue;
        js_array_push(ctx, pairs, idb_pair(el, pkey));
      }
    } else if (idb_valid_key(ik, 0) && idb_query_match(query, ik)) {
      js_array_push(ctx, pairs, idb_pair(ik, pkey));
    }
  }
  idb_sort_pairs(pairs);
  if (unique)
    idb_dedupe_pairs(pairs);
  if (prev)
    idb_reverse(pairs);
  gc_pop_scope();
  return pairs;
}
/* Advance to the next live step; with hasKey stop at the first step reaching `key`, else move `count`. */
static bool idb_cursor_next(js_value cursor, bool hasKey, js_value key, int count)
{
  js_value steps = js_get(g_ctx, cursor, "@steps");
  int n = steps.type == JV_OBJ ? (int)js_array_len(steps.u.obj) : 0;
  int pos = js_to_int_sat(g_ctx, js_get(g_ctx, cursor, "@pos"));
  bool keyonly = js_to_boolean(js_get(g_ctx, cursor, "@keyonly"));
  char db[128], sn[128], dir[16];
  js_value sr;
  js_object *recs;
  bool prev;
  int moved = 0, p;
  idb_str_field(cursor, "@dbname", db, sizeof db);
  idb_str_field(cursor, "@storename", sn, sizeof sn);
  idb_str_field(cursor, "@dir", dir, sizeof dir);
  prev = idb_dir_prev(dir);
  sr = idb_storerec(idb_dbrec(db), sn);
  recs = idb_records(sr);
  for (p = pos + 1; p < n; p++) {
    js_value step = js_get_index(g_ctx, steps, (unsigned)p);
    js_value ikey = js_get(g_ctx, step, "ikey"), pkey = js_get(g_ctx, step, "pkey");
    int found = 0, idx = recs ? idb_find(recs, pkey, &found) : 0;
    if (!found)
      continue;
    if (hasKey) {
      int c = idb_cmp(ikey, key);
      if (prev ? (c > 0) : (c < 0))
        continue;
    } else {
      moved++;
      if (moved < count)
        continue;
    }
    js_set(g_ctx, cursor, "@pos", js_number(p));
    js_set(g_ctx, cursor, "key", idb_clone(g_ctx, ikey, 0));
    js_set(g_ctx, cursor, "primaryKey", idb_clone(g_ctx, pkey, 0));
    if (!keyonly) {
      js_value e = js_get_index(g_ctx, js_obj_value(recs), (unsigned)idx);
      js_set(g_ctx, cursor, "value", idb_clone(g_ctx, js_get(g_ctx, e, "value"), 0));
    }
    return 1;
  }
  js_set(g_ctx, cursor, "@pos", js_number(n));
  return 0;
}
static js_value idb_open_cursor(js_ctx *ctx, js_value source, js_value txn, js_value sr, const char *dbname,
                                const char *sname, js_value irec, const char *iname, js_value query, const char *dir,
                                bool keyonly)
{
  bool prev = idb_dir_prev(dir), uniq = idb_dir_unique(dir);
  js_object *steps =
      irec.type == JV_OBJ ? idb_index_pairs(ctx, sr, irec, query, uniq, prev) : idb_store_steps(ctx, sr, query, prev);
  js_object *cur, *req;
  gc_push_scope();
  gc_keep(steps);
  if (source.type == JV_OBJ)
    gc_keep(source.u.obj);
  if (txn.type == JV_OBJ)
    gc_keep(txn.u.obj);
  cur = (js_object *)gc_keep(js_new_object(ctx, g_idb_proto[keyonly ? IDBP_CURSOR : IDBP_CURSORVAL]));
  js_def(cur, "@steps", js_obj_value(steps), P_WRITABLE);
  js_def(cur, "@pos", js_number(-1), P_WRITABLE);
  js_def(cur, "@dir", js_mk_cstring(ctx, dir), P_WRITABLE);
  js_def(cur, "@storename", js_mk_cstring(ctx, sname), P_WRITABLE);
  js_def(cur, "@dbname", js_mk_cstring(ctx, dbname), P_WRITABLE);
  js_def(cur, "@iname", (iname && iname[0]) ? js_mk_cstring(ctx, iname) : js_null(), P_WRITABLE);
  js_def(cur, "@keyonly", js_bool(keyonly), P_WRITABLE);
  js_def(cur, "@txn", txn, P_WRITABLE);
  js_def(cur, "direction", js_mk_cstring(ctx, dir), P_WRITABLE | P_ENUMERABLE);
  js_def(cur, "source", source, P_WRITABLE | P_ENUMERABLE);
  js_def(cur, "key", js_undef(), P_WRITABLE | P_ENUMERABLE);
  js_def(cur, "primaryKey", js_undef(), P_WRITABLE | P_ENUMERABLE);
  if (!keyonly)
    js_def(cur, "value", js_undef(), P_WRITABLE | P_ENUMERABLE);
  req = (js_object *)gc_keep(idb_make_request(source, txn, 0));
  js_def(cur, "@req", js_obj_value(req), P_WRITABLE);
  js_def(cur, "request", js_obj_value(req), P_WRITABLE | P_ENUMERABLE);
  {
    bool ok = idb_cursor_next(js_obj_value(cur), false, js_undef(), 1);
    js_set(ctx, js_obj_value(req), "@result", ok ? js_obj_value(cur) : js_null());
  }
  idb_txn_pending(txn, 1);
  idb_enqueue(req);
  gc_pop_scope();
  return js_obj_value(req);
}

/* Resolve and validate a store operation; throws and returns undefined on failure. */
static js_value idb_store_rec_checked(js_ctx *ctx, js_value t, bool write)
{
  js_value txn = js_get(ctx, t, "@txn"), sr;
  char sname[128], db[128];
  idb_str_field(t, "@sname", sname, sizeof sname);
  idb_str_field(t, "@dbname", db, sizeof db);
  if (!idb_txn_active(txn))
    return idb_throw(ctx, "TransactionInactiveError", "transaction is not active");
  if (write && !idb_txn_write(txn))
    return idb_throw(ctx, "ReadOnlyError", "transaction is read-only");
  sr = idb_storerec(idb_dbrec(db), sname);
  if (sr.type != JV_OBJ)
    return idb_throw(ctx, "InvalidStateError", "object store has been deleted");
  return sr;
}
/* Any other record collides with `key` on a unique index. */
static bool idb_violates_unique(js_ctx *ctx, js_value sr, js_value newValue, js_value key)
{
  js_value idxmap = js_get(ctx, sr, "indexes");
  js_object *im, *recs;
  int j;
  unsigned r, rn;
  if (idxmap.type != JV_OBJ)
    return false;
  im = idxmap.u.obj;
  recs = idb_records(sr);
  rn = recs ? js_array_len(recs) : 0;
  for (j = 0; j < im->props.n; j++) {
    js_value irec, ikp, ik;
    bool multi;
    if (!(im->props.e[j].flags & P_ENUMERABLE))
      continue;
    irec = im->props.e[j].value;
    if (!js_to_boolean(js_get(ctx, irec, "unique")))
      continue;
    ikp = js_get(ctx, irec, "keyPath");
    multi = js_to_boolean(js_get(ctx, irec, "multiEntry"));
    ik = idb_extract_key(ctx, newValue, ikp);
    if (ik.type == JV_UNDEF)
      continue;
    for (r = 0; r < rn; r++) {
      js_value e = js_get_index(ctx, js_obj_value(recs), r);
      js_value eik;
      if (idb_cmp(js_get(ctx, e, "key"), key) == 0)
        continue; /* the record being replaced */
      eik = idb_extract_key(ctx, js_get(ctx, e, "value"), ikp);
      if (eik.type == JV_UNDEF)
        continue;
      if (multi && ik.type == JV_OBJ && ik.u.obj->kind == OBJ_ARRAY) {
        unsigned a, an = js_array_len(ik.u.obj);
        for (a = 0; a < an; a++) {
          js_value ne = js_get_index(ctx, ik, a);
          if (eik.type == JV_OBJ && eik.u.obj->kind == OBJ_ARRAY) {
            unsigned b, bn = js_array_len(eik.u.obj);
            for (b = 0; b < bn; b++)
              if (idb_cmp(ne, js_get_index(ctx, eik, b)) == 0)
                return true;
          } else if (idb_cmp(ne, eik) == 0)
            return true;
        }
      } else if (idb_cmp(ik, eik) == 0)
        return true;
    }
  }
  return false;
}
static js_value idb_store_write(js_ctx *ctx, js_value t, js_value *args, int argc, bool isAdd)
{
  js_value txn = js_get(ctx, t, "@txn");
  js_value sr = idb_store_rec_checked(ctx, t, true);
  js_value value, explicitKey, keyPath, key, cval, ckey;
  bool autoInc, inlineKeys;
  int found, pos;
  js_object *recs;
  if (ctx->has_exc)
    return js_undef();
  value = A(0);
  explicitKey = argc >= 2 ? A(1) : js_undef();
  keyPath = js_get(ctx, sr, "keyPath");
  autoInc = js_to_boolean(js_get(ctx, sr, "autoIncrement"));
  inlineKeys = keyPath.type != JV_NULL && keyPath.type != JV_UNDEF;
  if (inlineKeys) {
    if (explicitKey.type != JV_UNDEF)
      return idb_throw(ctx, "DataError", "in-line keys take no explicit key");
    key = idb_extract_key(ctx, value, keyPath);
    if (key.type == JV_UNDEF) {
      if (!autoInc)
        return idb_throw(ctx, "DataError", "value has no key at its key path");
      key = idb_gen_key(sr);
      if (keyPath.type == JV_STR)
        idb_inject_key(ctx, value, keyPath.u.str->data, key);
    } else {
      if (autoInc)
        idb_bump_key(sr, key);
      if (!idb_valid_key(key, 0))
        return idb_throw(ctx, "DataError", "evaluated key is invalid");
    }
  } else if (explicitKey.type != JV_UNDEF) {
    key = explicitKey;
    if (!idb_valid_key(key, 0))
      return idb_throw(ctx, "DataError", "key is invalid");
    if (autoInc)
      idb_bump_key(sr, key);
  } else if (autoInc) {
    key = idb_gen_key(sr);
  } else {
    return idb_throw(ctx, "DataError", "a key is required");
  }
  cval = idb_clone(ctx, value, 0);
  if (ctx->has_exc)
    return js_undef();
  ckey = idb_clone(ctx, key, 0);
  if (ctx->has_exc)
    return js_undef();
  recs = idb_records(sr);
  gc_push_scope();
  if (cval.type == JV_OBJ)
    gc_keep(cval.u.obj);
  if (ckey.type == JV_OBJ)
    gc_keep(ckey.u.obj);
  if (recs)
    gc_keep(recs);
  if (idb_violates_unique(ctx, sr, cval, ckey)) {
    gc_pop_scope();
    return idb_fail(t, txn, idb_gerr("ConstraintError", "unique index constraint violated"));
  }
  pos = idb_find(recs, ckey, &found);
  if (isAdd && found) {
    gc_pop_scope();
    return idb_fail(t, txn, idb_gerr("ConstraintError", "key already exists"));
  }
  if (found)
    js_set_index(ctx, js_obj_value(recs), (unsigned)pos, idb_new_entry(ckey, cval));
  else
    idb_arr_insert(recs, pos, idb_new_entry(ckey, cval));
  gc_pop_scope();
  return idb_succeed(t, txn, idb_clone(ctx, ckey, 0));
}
static js_value idb_store_add(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return idb_store_write(ctx, t, args, argc, true);
}
static js_value idb_store_put(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return idb_store_write(ctx, t, args, argc, false);
}

static js_value idb_store_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn"), sr = idb_store_rec_checked(ctx, t, false), q = A(0);
  js_object *recs;
  unsigned i, n;
  if (ctx->has_exc)
    return js_undef();
  if (!idb_query_valid(q))
    return idb_throw(ctx, "DataError", "invalid key or range");
  recs = idb_records(sr);
  n = recs ? js_array_len(recs) : 0;
  for (i = 0; i < n; i++) {
    js_value e = js_get_index(ctx, js_obj_value(recs), i);
    if (idb_query_match(q, js_get(ctx, e, "key")))
      return idb_succeed(t, txn, idb_clone(ctx, js_get(ctx, e, "value"), 0));
  }
  return idb_succeed(t, txn, js_undef());
}
static js_value idb_store_getkey(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn"), sr = idb_store_rec_checked(ctx, t, false), q = A(0);
  js_object *recs;
  unsigned i, n;
  if (ctx->has_exc)
    return js_undef();
  if (!idb_query_valid(q))
    return idb_throw(ctx, "DataError", "invalid key or range");
  recs = idb_records(sr);
  n = recs ? js_array_len(recs) : 0;
  for (i = 0; i < n; i++) {
    js_value e = js_get_index(ctx, js_obj_value(recs), i);
    js_value k = js_get(ctx, e, "key");
    if (idb_query_match(q, k))
      return idb_succeed(t, txn, idb_clone(ctx, k, 0));
  }
  return idb_succeed(t, txn, js_undef());
}
static js_value idb_store_getall_impl(js_ctx *ctx, js_value t, js_value *args, int argc, bool keys)
{
  js_value txn = js_get(ctx, t, "@txn"), sr = idb_store_rec_checked(ctx, t, false), q = A(0), r;
  int count, got = 0;
  js_object *recs, *out;
  unsigned i, n;
  if (ctx->has_exc)
    return js_undef();
  if (q.type != JV_UNDEF && q.type != JV_NULL && !idb_query_valid(q))
    return idb_throw(ctx, "DataError", "invalid key or range");
  count = (argc >= 2 && A(1).type == JV_NUM && !IDB_ISNAN(A(1).u.num)) ? (int)A(1).u.num : -1;
  recs = idb_records(sr);
  n = recs ? js_array_len(recs) : 0;
  gc_push_scope();
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < n; i++) {
    js_value e;
    if (count >= 0 && got >= count)
      break;
    e = js_get_index(ctx, js_obj_value(recs), i);
    if (!idb_query_match(q, js_get(ctx, e, "key")))
      continue;
    js_array_push(ctx, out, idb_clone(ctx, js_get(ctx, e, keys ? "key" : "value"), 0));
    got++;
  }
  r = idb_succeed(t, txn, js_obj_value(out));
  gc_pop_scope();
  return r;
}
static js_value idb_store_getall(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return idb_store_getall_impl(ctx, t, args, argc, false);
}
static js_value idb_store_getallkeys(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return idb_store_getall_impl(ctx, t, args, argc, true);
}
static js_value idb_store_count(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn"), sr = idb_store_rec_checked(ctx, t, false), q = A(0);
  js_object *recs;
  unsigned i, n;
  int c = 0;
  if (ctx->has_exc)
    return js_undef();
  if (q.type != JV_UNDEF && q.type != JV_NULL && !idb_query_valid(q))
    return idb_throw(ctx, "DataError", "invalid key or range");
  recs = idb_records(sr);
  n = recs ? js_array_len(recs) : 0;
  for (i = 0; i < n; i++) {
    js_value e = js_get_index(ctx, js_obj_value(recs), i);
    if (idb_query_match(q, js_get(ctx, e, "key")))
      c++;
  }
  return idb_succeed(t, txn, js_number(c));
}
static js_value idb_store_delete(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn"), sr = idb_store_rec_checked(ctx, t, true), q = A(0);
  js_object *recs;
  int i;
  if (ctx->has_exc)
    return js_undef();
  if (!idb_query_valid(q))
    return idb_throw(ctx, "DataError", "invalid key or range");
  recs = idb_records(sr);
  for (i = (recs ? (int)js_array_len(recs) : 0) - 1; i >= 0; i--) {
    js_value e = js_get_index(ctx, js_obj_value(recs), (unsigned)i);
    if (idb_query_match(q, js_get(ctx, e, "key")))
      idb_arr_remove(recs, i);
  }
  return idb_succeed(t, txn, js_undef());
}
static js_value idb_store_clear(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn"), sr = idb_store_rec_checked(ctx, t, true);
  js_object *recs;
  (void)args;
  (void)argc;
  if (ctx->has_exc)
    return js_undef();
  recs = idb_records(sr);
  if (recs)
    js_array_set_len(ctx, recs, 0);
  return idb_succeed(t, txn, js_undef());
}
static js_value idb_store_opencursor_impl(js_ctx *ctx, js_value t, js_value *args, int argc, bool keyonly)
{
  js_value txn = js_get(ctx, t, "@txn"), sr = idb_store_rec_checked(ctx, t, false), q = A(0);
  char db[128], sn[128], dirbuf[16];
  const char *dir;
  if (ctx->has_exc)
    return js_undef();
  if (q.type != JV_UNDEF && q.type != JV_NULL && !idb_query_valid(q))
    return idb_throw(ctx, "DataError", "invalid range");
  idb_str_field(t, "@dbname", db, sizeof db);
  idb_str_field(t, "@sname", sn, sizeof sn);
  dir = (argc >= 2 && A(1).type == JV_STR) ? A(1).u.str->data : "next";
  strlcpy(dirbuf, dir, sizeof dirbuf);
  if (!(streq(dirbuf, "next") || streq(dirbuf, "prev") || streq(dirbuf, "nextunique") || streq(dirbuf, "prevunique")))
    return idb_throw(ctx, "TypeError", "invalid cursor direction");
  return idb_open_cursor(ctx, t, txn, sr, db, sn, js_undef(), "", q, dirbuf, keyonly);
}
static js_value idb_store_opencursor(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return idb_store_opencursor_impl(ctx, t, args, argc, false);
}
static js_value idb_store_openkeycursor(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return idb_store_opencursor_impl(ctx, t, args, argc, true);
}
static js_value idb_store_index(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value sr = idb_store_rec_checked(ctx, t, false), idxmap;
  js_value txn = js_get(ctx, t, "@txn");
  char db[128], sn[128], name[128];
  (void)argc;
  if (ctx->has_exc)
    return js_undef();
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  idb_str_field(t, "@dbname", db, sizeof db);
  idb_str_field(t, "@sname", sn, sizeof sn);
  idxmap = js_get(ctx, sr, "indexes");
  if (idxmap.type != JV_OBJ || !js_has_own(idxmap.u.obj, name))
    return idb_throw(ctx, "NotFoundError", "no index with that name");
  return idb_make_index(txn, db, sn, name);
}
static js_value idb_store_createindex(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn"), sr, keyPath, opts, idxmap;
  char db[128], sn[128], name[128];
  bool uniq = false, multi = false;
  if (!idb_txn_active(txn) || !idb_txn_isversion(txn))
    return idb_throw(ctx, "InvalidStateError", "createIndex requires a versionchange transaction");
  sr = idb_store_rec_checked(ctx, t, true);
  if (ctx->has_exc)
    return js_undef();
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  keyPath = A(1);
  opts = A(2);
  if (opts.type == JV_OBJ) {
    uniq = js_to_boolean(js_get(ctx, opts, "unique"));
    multi = js_to_boolean(js_get(ctx, opts, "multiEntry"));
  }
  idb_str_field(t, "@dbname", db, sizeof db);
  idb_str_field(t, "@sname", sn, sizeof sn);
  idxmap = js_get(ctx, sr, "indexes");
  if (idxmap.type != JV_OBJ) {
    idxmap = js_obj_value(js_new_object(ctx, 0));
    js_set(ctx, sr, "indexes", idxmap);
  }
  if (js_has_own(idxmap.u.obj, name))
    return idb_throw(ctx, "ConstraintError", "an index with that name exists");
  {
    js_object *ir;
    js_value kp;
    gc_push_scope();
    ir = (js_object *)gc_keep(js_new_object(ctx, 0));
    kp = (keyPath.type == JV_STR || (keyPath.type == JV_OBJ && keyPath.u.obj->kind == OBJ_ARRAY))
             ? keyPath
             : js_mk_cstring(ctx, arg_str(ctx, keyPath));
    js_def(ir, "name", js_mk_cstring(ctx, name), P_WRITABLE);
    js_def(ir, "keyPath", kp, P_WRITABLE);
    js_def(ir, "unique", js_bool(uniq), P_WRITABLE);
    js_def(ir, "multiEntry", js_bool(multi), P_WRITABLE);
    js_set(ctx, idxmap, name, js_obj_value(ir));
    gc_pop_scope();
  }
  return idb_make_index(txn, db, sn, name);
}
static js_value idb_store_deleteindex(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn"), sr, idxmap;
  char name[128];
  (void)argc;
  if (!idb_txn_active(txn) || !idb_txn_isversion(txn))
    return idb_throw(ctx, "InvalidStateError", "deleteIndex requires a versionchange transaction");
  sr = idb_store_rec_checked(ctx, t, true);
  if (ctx->has_exc)
    return js_undef();
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  idxmap = js_get(ctx, sr, "indexes");
  if (idxmap.type != JV_OBJ || !js_has_own(idxmap.u.obj, name))
    return idb_throw(ctx, "NotFoundError", "no index with that name");
  js_delete(idxmap.u.obj, name);
  return js_undef();
}
static js_value idb_store_indexnames(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char db[128], sn[128];
  js_value ix;
  (void)args;
  (void)argc;
  idb_str_field(t, "@dbname", db, sizeof db);
  idb_str_field(t, "@sname", sn, sizeof sn);
  ix = js_get(ctx, idb_storerec(idb_dbrec(db), sn), "indexes");
  return idb_stringlist(ix.type == JV_OBJ ? ix.u.obj : 0);
}

/* ---- index operations ---------------------------------------------------- */

static bool idb_index_open(js_ctx *ctx, js_value t, idbix *x)
{
  x->txn = js_get(ctx, t, "@txn");
  idb_str_field(t, "@dbname", x->db, sizeof x->db);
  idb_str_field(t, "@sname", x->sn, sizeof x->sn);
  idb_str_field(t, "@iname", x->in, sizeof x->in);
  if (!idb_txn_active(x->txn)) {
    idb_throw(ctx, "TransactionInactiveError", "transaction is not active");
    return false;
  }
  x->sr = idb_storerec(idb_dbrec(x->db), x->sn);
  if (x->sr.type != JV_OBJ) {
    idb_throw(ctx, "InvalidStateError", "object store not found");
    return false;
  }
  x->ir = js_get(ctx, js_get(ctx, x->sr, "indexes"), x->in);
  if (x->ir.type != JV_OBJ) {
    idb_throw(ctx, "InvalidStateError", "index has been deleted");
    return false;
  }
  return true;
}
static js_value idb_value_by_pkey(js_value sr, js_value pkey)
{
  js_object *recs = idb_records(sr);
  int found, idx;
  if (!recs)
    return js_undef();
  idx = idb_find(recs, pkey, &found);
  if (!found)
    return js_undef();
  return js_get(g_ctx, js_get_index(g_ctx, js_obj_value(recs), (unsigned)idx), "value");
}
static js_value idb_index_getall_impl(js_ctx *ctx, js_value t, js_value *args, int argc, int mode)
{
  /* mode: 0 get (first value), 1 getKey (first primaryKey), 2 getAll, 3
   * getAllKeys, 4 count */
  idbix x;
  js_value q = A(0), r;
  js_object *pairs, *out = 0;
  unsigned i, n;
  int count = -1, got = 0;
  if (!idb_index_open(ctx, t, &x))
    return js_undef();
  if (q.type != JV_UNDEF && q.type != JV_NULL && !idb_query_valid(q))
    return idb_throw(ctx, "DataError", "invalid key or range");
  if ((mode == 2 || mode == 3) && argc >= 2 && A(1).type == JV_NUM && !IDB_ISNAN(A(1).u.num))
    count = (int)A(1).u.num;
  pairs = idb_index_pairs(ctx, x.sr, x.ir, q, false, false);
  n = js_array_len(pairs);
  gc_push_scope();
  gc_keep(pairs);
  if (mode == 2 || mode == 3)
    out = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (mode == 4) {
    r = idb_succeed(t, x.txn, js_number((double)n));
    gc_pop_scope();
    return r;
  }
  if (mode == 0 || mode == 1) {
    if (n == 0) {
      r = idb_succeed(t, x.txn, js_undef());
      gc_pop_scope();
      return r;
    }
    {
      js_value p0 = js_get_index(ctx, js_obj_value(pairs), 0);
      js_value pk = js_get(ctx, p0, "pkey");
      r = idb_succeed(t, x.txn, mode == 1 ? idb_clone(ctx, pk, 0) : idb_clone(ctx, idb_value_by_pkey(x.sr, pk), 0));
    }
    gc_pop_scope();
    return r;
  }
  for (i = 0; i < n; i++) {
    js_value p, pk;
    if (count >= 0 && got >= count)
      break;
    p = js_get_index(ctx, js_obj_value(pairs), i);
    pk = js_get(ctx, p, "pkey");
    js_array_push(ctx, out, mode == 3 ? idb_clone(ctx, pk, 0) : idb_clone(ctx, idb_value_by_pkey(x.sr, pk), 0));
    got++;
  }
  r = idb_succeed(t, x.txn, js_obj_value(out));
  gc_pop_scope();
  return r;
}
static js_value idb_index_get(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return idb_index_getall_impl(ctx, t, a, n, 0);
}
static js_value idb_index_getkey(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return idb_index_getall_impl(ctx, t, a, n, 1);
}
static js_value idb_index_getall(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return idb_index_getall_impl(ctx, t, a, n, 2);
}
static js_value idb_index_getallkeys(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return idb_index_getall_impl(ctx, t, a, n, 3);
}
static js_value idb_index_count(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return idb_index_getall_impl(ctx, t, a, n, 4);
}
static js_value idb_index_opencursor_impl(js_ctx *ctx, js_value t, js_value *args, int argc, bool keyonly)
{
  idbix x;
  js_value q = A(0);
  const char *dir;
  char dirbuf[16];
  if (!idb_index_open(ctx, t, &x))
    return js_undef();
  if (q.type != JV_UNDEF && q.type != JV_NULL && !idb_query_valid(q))
    return idb_throw(ctx, "DataError", "invalid range");
  dir = (argc >= 2 && A(1).type == JV_STR) ? A(1).u.str->data : "next";
  strlcpy(dirbuf, dir, sizeof dirbuf);
  if (!(streq(dirbuf, "next") || streq(dirbuf, "prev") || streq(dirbuf, "nextunique") || streq(dirbuf, "prevunique")))
    return idb_throw(ctx, "TypeError", "invalid cursor direction");
  return idb_open_cursor(ctx, t, x.txn, x.sr, x.db, x.sn, x.ir, x.in, q, dirbuf, keyonly);
}
static js_value idb_index_opencursor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return idb_index_opencursor_impl(ctx, t, a, n, false);
}
static js_value idb_index_openkeycursor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return idb_index_opencursor_impl(ctx, t, a, n, true);
}

/* ---- cursor operations --------------------------------------------------- */

static void idb_cursor_requeue(js_ctx *ctx, js_value cursor, js_value txn, bool positioned)
{
  js_value req = js_get(ctx, cursor, "@req");
  if (req.type != JV_OBJ)
    return;
  js_set(ctx, req, "@result", positioned ? cursor : js_null());
  js_set(ctx, req, "@event", js_mk_cstring(ctx, "success"));
  js_set(ctx, req, "readyState", js_mk_cstring(ctx, "pending"));
  idb_txn_pending(txn, 1);
  idb_enqueue(req.u.obj);
}
static js_value idb_cursor_continue(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn");
  bool hasKey = argc >= 1 && A(0).type != JV_UNDEF && A(0).type != JV_NULL, ok;
  if (!idb_txn_active(txn))
    return idb_throw(ctx, "TransactionInactiveError", "transaction is not active");
  if (hasKey && !idb_valid_key(A(0), 0))
    return idb_throw(ctx, "DataError", "invalid key");
  ok = idb_cursor_next(t, hasKey, A(0), 1);
  idb_cursor_requeue(ctx, t, txn, ok);
  return js_undef();
}
static js_value idb_cursor_advance(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn");
  int count = js_to_int_sat(ctx, A(0));
  bool ok;
  (void)argc;
  if (count <= 0)
    return idb_throw(ctx, "TypeError", "advance count must be positive");
  if (!idb_txn_active(txn))
    return idb_throw(ctx, "TransactionInactiveError", "transaction is not active");
  ok = idb_cursor_next(t, false, js_undef(), count);
  idb_cursor_requeue(ctx, t, txn, ok);
  return js_undef();
}
static js_value idb_cursor_update(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn"), sr, pkey, value, keyPath, cval;
  char db[128], sn[128];
  js_object *recs;
  int found, pos;
  (void)argc;
  if (js_to_boolean(js_get(ctx, t, "@keyonly")))
    return idb_throw(ctx, "InvalidStateError", "key-only cursor has no value");
  if (!idb_txn_active(txn))
    return idb_throw(ctx, "TransactionInactiveError", "transaction is not active");
  if (!idb_txn_write(txn))
    return idb_throw(ctx, "ReadOnlyError", "transaction is read-only");
  idb_str_field(t, "@dbname", db, sizeof db);
  idb_str_field(t, "@storename", sn, sizeof sn);
  sr = idb_storerec(idb_dbrec(db), sn);
  if (sr.type != JV_OBJ)
    return idb_throw(ctx, "InvalidStateError", "object store not found");
  pkey = js_get(ctx, t, "primaryKey");
  value = A(0);
  keyPath = js_get(ctx, sr, "keyPath");
  if (keyPath.type != JV_NULL && keyPath.type != JV_UNDEF) {
    js_value ek = idb_extract_key(ctx, value, keyPath);
    if (ek.type == JV_UNDEF || idb_cmp(ek, pkey) != 0)
      return idb_throw(ctx, "DataError", "the modified value's key differs");
  }
  cval = idb_clone(ctx, value, 0);
  if (ctx->has_exc)
    return js_undef();
  if (idb_violates_unique(ctx, sr, cval, pkey))
    return idb_fail(t, txn, idb_gerr("ConstraintError", "unique index constraint violated"));
  recs = idb_records(sr);
  pos = idb_find(recs, pkey, &found);
  if (found)
    js_set_index(ctx, js_obj_value(recs), (unsigned)pos, idb_new_entry(idb_clone(ctx, pkey, 0), cval));
  return idb_succeed(t, txn, idb_clone(ctx, pkey, 0));
}
static js_value idb_cursor_delete(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value txn = js_get(ctx, t, "@txn"), sr, pkey;
  char db[128], sn[128];
  js_object *recs;
  int found, pos;
  (void)args;
  (void)argc;
  if (!idb_txn_active(txn))
    return idb_throw(ctx, "TransactionInactiveError", "transaction is not active");
  if (!idb_txn_write(txn))
    return idb_throw(ctx, "ReadOnlyError", "transaction is read-only");
  idb_str_field(t, "@dbname", db, sizeof db);
  idb_str_field(t, "@storename", sn, sizeof sn);
  sr = idb_storerec(idb_dbrec(db), sn);
  if (sr.type != JV_OBJ)
    return idb_throw(ctx, "InvalidStateError", "object store not found");
  pkey = js_get(ctx, t, "primaryKey");
  recs = idb_records(sr);
  pos = idb_find(recs, pkey, &found);
  if (found)
    idb_arr_remove(recs, pos);
  return idb_succeed(t, txn, js_undef());
}

/* ---- IDBKeyRange --------------------------------------------------------- */

static js_value idb_kr_only(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)argc;
  if (!idb_valid_key(A(0), 0))
    return idb_throw(ctx, "DataError", "invalid key");
  return idb_make_keyrange(A(0), A(0), 0, 0);
}
static js_value idb_kr_lower(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  if (!idb_valid_key(A(0), 0))
    return idb_throw(ctx, "DataError", "invalid key");
  return idb_make_keyrange(A(0), js_undef(), (argc >= 2 && js_to_boolean(A(1))) ? 1 : 0, 0);
}
static js_value idb_kr_upper(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  if (!idb_valid_key(A(0), 0))
    return idb_throw(ctx, "DataError", "invalid key");
  return idb_make_keyrange(js_undef(), A(0), 0, (argc >= 2 && js_to_boolean(A(1))) ? 1 : 0);
}
static js_value idb_kr_bound(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  if (!idb_valid_key(A(0), 0) || !idb_valid_key(A(1), 0))
    return idb_throw(ctx, "DataError", "invalid key");
  if (idb_cmp(A(0), A(1)) > 0)
    return idb_throw(ctx, "DataError", "lower bound exceeds upper bound");
  return idb_make_keyrange(A(0), A(1), (argc >= 3 && js_to_boolean(A(2))) ? 1 : 0,
                           (argc >= 4 && js_to_boolean(A(3))) ? 1 : 0);
}
static js_value idb_kr_includes(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)argc;
  if (!idb_valid_key(A(0), 0))
    return idb_throw(ctx, "DataError", "invalid key");
  return js_bool(idb_query_match(t, A(0)));
}

/* ---- IDBTransaction ------------------------------------------------------ */

static void idb_abort_txn(js_value txn, js_value err)
{
  js_value st = js_get(g_ctx, txn, "@state");
  char dbn[128];
  js_value snap, openreq;
  if (st.type == JV_STR && streq(st.u.str->data, "finished"))
    return;
  if (js_to_boolean(js_get(g_ctx, txn, "@aborted")))
    return;
  idb_str_field(txn, "@dbname", dbn, sizeof dbn);
  snap = js_get(g_ctx, txn, "@snap");
  if (snap.type == JV_OBJ)
    idb_restore(dbn, snap);
  js_set(g_ctx, txn, "@aborted", js_bool(1));
  if (js_get(g_ctx, txn, "error").type == JV_NULL && err.type == JV_OBJ)
    js_set(g_ctx, txn, "error", err);
  openreq = js_get(g_ctx, txn, "@openreq");
  if (openreq.type == JV_OBJ) { /* a versionchange upgrade: unwind it */
    js_value db = js_get(g_ctx, txn, "@db"), dbrec = idb_dbrec(dbn);
    if (db.type == JV_OBJ)
      js_set(g_ctx, db, "@vtxn", js_null());
    if (dbrec.type == JV_OBJ) {
      if (js_to_boolean(js_get(g_ctx, txn, "@newdb")))
        js_delete(g_idb_dbs, dbn);
      else
        js_set(g_ctx, dbrec, "version", js_get(g_ctx, txn, "@snapver"));
    }
    js_set(g_ctx, openreq, "@txn", js_null()); /* detach so the pump won't skip it as aborted */
    js_set(g_ctx, openreq, "transaction", js_null());
    js_set(g_ctx, openreq, "@error", err.type == JV_OBJ ? err : idb_gerr("AbortError", "upgrade aborted"));
    js_set(g_ctx, openreq, "@event", js_mk_cstring(g_ctx, "error"));
    js_set(g_ctx, openreq, "readyState", js_mk_cstring(g_ctx, "pending"));
    idb_enqueue(openreq.u.obj);
  }
}
static js_value idb_txn_objectstore(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[128], dbn[128];
  js_value scope;
  unsigned i, n;
  bool in = false;
  (void)argc;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  idb_str_field(t, "@dbname", dbn, sizeof dbn);
  if (!idb_txn_active(t))
    return idb_throw(ctx, "InvalidStateError", "transaction has finished");
  scope = js_get(ctx, t, "@scope");
  n = scope.type == JV_OBJ ? js_array_len(scope.u.obj) : 0;
  for (i = 0; i < n; i++) {
    js_value s = js_get_index(ctx, scope, i);
    if (s.type == JV_STR && streq(s.u.str->data, name)) {
      in = true;
      break;
    }
  }
  if (!in)
    return idb_throw(ctx, "NotFoundError", "store is not in the transaction's scope");
  if (idb_storerec(idb_dbrec(dbn), name).type != JV_OBJ)
    return idb_throw(ctx, "NotFoundError", "object store not found");
  return idb_make_store(t, dbn, name);
}
static js_value idb_txn_abort(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  idb_abort_txn(t, idb_gerr("AbortError", "the transaction was aborted"));
  return js_undef();
}
static js_value idb_txn_commit(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_undef();
}
static js_value idb_txn_storenames(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value scope = js_get(ctx, t, "@scope"), r;
  js_object *map;
  unsigned i, n;
  (void)args;
  (void)argc;
  gc_push_scope();
  map = (js_object *)gc_keep(js_new_object(ctx, 0)); /* names -> 1, so idb_stringlist can sort them */
  n = scope.type == JV_OBJ ? js_array_len(scope.u.obj) : 0;
  for (i = 0; i < n; i++) {
    js_value s = js_get_index(ctx, scope, i);
    if (s.type == JV_STR)
      js_set(ctx, js_obj_value(map), s.u.str->data, js_bool(1));
  }
  r = idb_stringlist(map);
  gc_pop_scope();
  return r;
}

/* ---- IDBDatabase --------------------------------------------------------- */

static js_value idb_db_createstore(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value vtxn = js_get(ctx, t, "@vtxn"), opts, keyPath = js_null(), dbrec, stores;
  char dbn[128], name[128];
  bool autoInc = false;
  (void)argc;
  if (vtxn.type != JV_OBJ || !idb_txn_active(vtxn))
    return idb_throw(ctx, "InvalidStateError", "createObjectStore requires a versionchange transaction");
  idb_str_field(t, "@dbname", dbn, sizeof dbn);
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  opts = A(1);
  if (opts.type == JV_OBJ) {
    js_value kp = js_get(ctx, opts, "keyPath");
    if (kp.type != JV_UNDEF)
      keyPath = kp;
    autoInc = js_to_boolean(js_get(ctx, opts, "autoIncrement"));
  }
  dbrec = idb_dbrec(dbn);
  stores = js_get(ctx, dbrec, "stores");
  if (stores.type != JV_OBJ) {
    stores = js_obj_value(js_new_object(ctx, 0));
    js_set(ctx, dbrec, "stores", stores);
  }
  if (js_has_own(stores.u.obj, name))
    return idb_throw(ctx, "ConstraintError", "an object store with that name exists");
  {
    js_object *sr;
    gc_push_scope();
    if (keyPath.type == JV_OBJ)
      gc_keep(keyPath.u.obj);
    sr = (js_object *)gc_keep(js_new_object(ctx, 0));
    js_def(sr, "name", js_mk_cstring(ctx, name), P_WRITABLE);
    js_def(sr, "keyPath", keyPath, P_WRITABLE);
    js_def(sr, "autoIncrement", js_bool(autoInc), P_WRITABLE);
    js_def(sr, "keyGen", js_number(1), P_WRITABLE);
    js_def(sr, "records", js_obj_value(js_new_array(ctx, 0)), P_WRITABLE);
    js_def(sr, "indexes", js_obj_value(js_new_object(ctx, 0)), P_WRITABLE);
    js_set(ctx, stores, name, js_obj_value(sr));
    gc_pop_scope();
  }
  {
    js_value scope = js_get(ctx, vtxn, "@scope");
    if (scope.type == JV_OBJ) {
      bool dup = false;
      unsigned i, n = js_array_len(scope.u.obj);
      for (i = 0; i < n; i++) {
        js_value s = js_get_index(ctx, scope, i);
        if (s.type == JV_STR && streq(s.u.str->data, name)) {
          dup = true;
          break;
        }
      }
      if (!dup)
        js_array_push(ctx, scope.u.obj, js_mk_cstring(ctx, name));
    }
  }
  return idb_make_store(vtxn, dbn, name);
}
static js_value idb_db_deletestore(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value vtxn = js_get(ctx, t, "@vtxn"), dbrec, stores;
  char dbn[128], name[128];
  (void)argc;
  if (vtxn.type != JV_OBJ || !idb_txn_active(vtxn))
    return idb_throw(ctx, "InvalidStateError", "deleteObjectStore requires a versionchange transaction");
  idb_str_field(t, "@dbname", dbn, sizeof dbn);
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  dbrec = idb_dbrec(dbn);
  stores = js_get(ctx, dbrec, "stores");
  if (stores.type != JV_OBJ || !js_has_own(stores.u.obj, name))
    return idb_throw(ctx, "NotFoundError", "no object store with that name");
  js_delete(stores.u.obj, name);
  return js_undef();
}
static js_value idb_db_transaction(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char dbn[128], modebuf[16];
  js_value dbrec, arg, txn;
  js_object *scope;
  const char *mode;
  unsigned i, n;
  if (js_to_boolean(js_get(ctx, t, "@closed")))
    return idb_throw(ctx, "InvalidStateError", "the database connection is closed");
  idb_str_field(t, "@dbname", dbn, sizeof dbn);
  dbrec = idb_dbrec(dbn);
  if (dbrec.type != JV_OBJ)
    return idb_throw(ctx, "InvalidStateError", "database not found");
  gc_push_scope();
  scope = (js_object *)gc_keep(js_new_array(ctx, 0));
  arg = A(0);
  if (arg.type == JV_OBJ && arg.u.obj->kind == OBJ_ARRAY) {
    n = js_array_len(arg.u.obj);
    for (i = 0; i < n; i++) {
      char nm[128];
      strlcpy(nm, arg_str(ctx, js_get_index(ctx, arg, i)), sizeof nm);
      js_array_push(ctx, scope, js_mk_cstring(ctx, nm));
    }
  } else {
    char nm[128];
    strlcpy(nm, arg_str(ctx, arg), sizeof nm);
    js_array_push(ctx, scope, js_mk_cstring(ctx, nm));
  }
  n = js_array_len(scope);
  for (i = 0; i < n; i++) {
    js_value s = js_get_index(ctx, js_obj_value(scope), i);
    if (idb_storerec(dbrec, s.u.str->data).type != JV_OBJ) {
      gc_pop_scope();
      return idb_throw(ctx, "NotFoundError", "one or more object stores were not found");
    }
  }
  mode = argc >= 2 && A(1).type == JV_STR ? A(1).u.str->data : "readonly";
  strlcpy(modebuf, mode, sizeof modebuf);
  if (!(streq(modebuf, "readonly") || streq(modebuf, "readwrite"))) {
    gc_pop_scope();
    return idb_throw(ctx, "TypeError", "invalid transaction mode");
  }
  txn = idb_make_txn(t, scope, modebuf, js_null());
  gc_pop_scope();
  return txn;
}
static js_value idb_db_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  js_set(ctx, t, "@closed", js_bool(1));
  return js_undef();
}
static js_value idb_db_storenames(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char dbn[128];
  js_value stores;
  (void)args;
  (void)argc;
  idb_str_field(t, "@dbname", dbn, sizeof dbn);
  stores = js_get(ctx, idb_dbrec(dbn), "stores");
  return idb_stringlist(stores.type == JV_OBJ ? stores.u.obj : 0);
}

/* ---- IDBFactory (window.indexedDB) --------------------------------------- */

static js_value idb_illegal_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)a;
  (void)n;
  return js_throw_type(ctx, "Illegal constructor");
}

static js_value idb_factory_open(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[128];
  bool hasVer, isNew;
  double reqver, curver, newver;
  js_value existing;
  js_object *req;
  (void)t;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  hasVer = argc >= 2 && A(1).type != JV_UNDEF;
  reqver = hasVer ? js_to_number(ctx, A(1)) : 0;
  if (hasVer && (IDB_ISNAN(reqver) || reqver < 1))
    return idb_throw(ctx, "TypeError", "version must be a positive integer");
  existing = idb_dbrec(name);
  isNew = existing.type != JV_OBJ;
  curver = isNew ? 0 : js_to_number(ctx, js_get(ctx, existing, "version"));
  newver = hasVer ? reqver : (isNew ? 1 : curver);
  req = idb_make_request(js_null(), js_null(), 1);
  gc_push_scope();
  gc_keep(req);
  if (hasVer && newver < curver) {
    js_set(ctx, js_obj_value(req), "@error",
           idb_gerr("VersionError", "requested version is older than the existing one"));
    js_set(ctx, js_obj_value(req), "@event", js_mk_cstring(ctx, "error"));
    idb_enqueue(req);
    gc_pop_scope();
    return js_obj_value(req);
  }
  if (isNew) {
    js_object *dbrec = (js_object *)gc_keep(js_new_object(ctx, 0));
    js_def(dbrec, "name", js_mk_cstring(ctx, name), P_WRITABLE | P_ENUMERABLE);
    js_def(dbrec, "version", js_number(0), P_WRITABLE | P_ENUMERABLE);
    js_def(dbrec, "stores", js_obj_value(js_new_object(ctx, 0)), P_WRITABLE | P_ENUMERABLE);
    js_set(ctx, js_obj_value(g_idb_dbs), name, js_obj_value(dbrec));
    existing = js_obj_value(dbrec);
    curver = 0;
  }
  if (newver > curver) { /* run an upgrade transaction */
    js_value db, txn, stores;
    js_object *scope;
    js_set(ctx, existing, "version", js_number(newver));
    db = idb_make_db(name, newver);
    gc_keep(db.u.obj);
    scope = (js_object *)gc_keep(js_new_array(ctx, 0));
    stores = js_get(ctx, existing, "stores");
    if (stores.type == JV_OBJ) {
      js_object *sm = stores.u.obj;
      int i;
      for (i = 0; i < sm->props.n; i++)
        if (sm->props.e[i].flags & P_ENUMERABLE)
          js_array_push(ctx, scope, js_mk_cstring(ctx, sm->props.e[i].key));
    }
    txn = idb_make_txn(db, scope, "versionchange", js_obj_value(req));
    js_set(ctx, txn, "@snapver", js_number(curver));
    js_set(ctx, txn, "@newdb", js_bool(isNew));
    js_set(ctx, db, "@vtxn", txn);
    js_set(ctx, js_obj_value(req), "@result", db);
    js_set(ctx, js_obj_value(req), "result", db);
    js_set(ctx, js_obj_value(req), "@txn", txn);
    js_set(ctx, js_obj_value(req), "transaction", txn);
    js_set(ctx, js_obj_value(req), "@event", js_mk_cstring(ctx, "upgradeneeded"));
    js_set(ctx, js_obj_value(req), "@oldver", js_number(curver));
    js_set(ctx, js_obj_value(req), "@newver", js_number(newver));
    idb_enqueue(req);
  } else {
    js_value db = idb_make_db(name, curver);
    js_set(ctx, js_obj_value(req), "@result", db);
    js_set(ctx, js_obj_value(req), "@event", js_mk_cstring(ctx, "success"));
    idb_enqueue(req);
  }
  gc_pop_scope();
  return js_obj_value(req);
}
static js_value idb_factory_delete(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[128];
  js_object *req;
  (void)t;
  (void)argc;
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  req = idb_make_request(js_null(), js_null(), 1);
  gc_push_scope();
  gc_keep(req);
  if (g_idb_dbs)
    js_delete(g_idb_dbs, name);
  js_set(ctx, js_obj_value(req), "@result", js_undef());
  js_set(ctx, js_obj_value(req), "@event", js_mk_cstring(ctx, "success"));
  idb_enqueue(req);
  gc_pop_scope();
  return js_obj_value(req);
}
static js_value idb_factory_databases(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *arr;
  js_value p;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (g_idb_dbs) {
    int i;
    for (i = 0; i < g_idb_dbs->props.n; i++) {
      js_object *info;
      if (!(g_idb_dbs->props.e[i].flags & P_ENUMERABLE))
        continue;
      info = js_new_object(ctx, ctx->object_proto);
      js_def(info, "name", js_mk_cstring(ctx, g_idb_dbs->props.e[i].key), P_ENUMERABLE | P_WRITABLE);
      js_def(info, "version", js_get(ctx, g_idb_dbs->props.e[i].value, "version"), P_ENUMERABLE | P_WRITABLE);
      js_array_push(ctx, arr, js_obj_value(info));
    }
  }
  p = js_promise_resolved(ctx, js_obj_value(arr));
  gc_pop_scope();
  return p;
}
static js_value idb_factory_cmp(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)argc;
  if (!idb_valid_key(A(0), 0) || !idb_valid_key(A(1), 0))
    return idb_throw(ctx, "DataError", "invalid key");
  return js_number(idb_cmp(A(0), A(1)));
}

/* Fire `ev` at `target`, bubbling to b1 (txn) and b2 (db); 1 if preventDefault was called. */
static bool idb_fire(js_object *target, js_object *b1, js_object *b2, js_value ev)
{
  bool dp;
  gc_push_scope();
  gc_keep(target);
  if (ev.type == JV_OBJ)
    gc_keep(ev.u.obj);
  if (b1)
    gc_keep(b1);
  if (b2)
    gc_keep(b2);
  js_set(g_ctx, ev, "target", js_obj_value(target));
  ev_fire_on(target, ev, 0);
  if (b1 && !ev_flag(ev, "@sp"))
    ev_fire_on(b1, ev, 0);
  if (b2 && !ev_flag(ev, "@sp"))
    ev_fire_on(b2, ev, 0);
  dp = ev_flag(ev, "@dp");
  gc_pop_scope();
  return dp;
}

int js_idb_pump(void)
{
  int fired = 0, guard = 0;
  bool progress;
  if (!g_page_live || !g_ctx || !g_idb_pending)
    return 0;
  do {
    progress = false;
    while (js_array_len(g_idb_pending) > 0) { /* deliver requests, oldest first */
      js_value req = js_get_index(g_ctx, js_obj_value(g_idb_pending), 0);
      js_value txn, evt, err;
      bool isup;
      idb_arr_remove(g_idb_pending, 0);
      if (req.type != JV_OBJ)
        continue;
      gc_push_scope();
      gc_keep(req.u.obj);
      txn = js_get(g_ctx, req, "@txn");
      evt = js_get(g_ctx, req, "@event");
      isup = evt.type == JV_STR && streq(evt.u.str->data, "upgradeneeded");
      if (txn.type == JV_OBJ && js_to_boolean(js_get(g_ctx, txn, "@aborted")) && !isup) {
        gc_pop_scope();
        continue;
      }
      js_set(g_ctx, req, "readyState", js_mk_cstring(g_ctx, "done"));
      err = js_get(g_ctx, req, "@error");
      if (err.type == JV_OBJ) {
        js_object *dbo = 0;
        js_value ev;
        bool prevented;
        js_set(g_ctx, req, "error", err);
        if (txn.type == JV_OBJ) {
          js_value db = js_get(g_ctx, txn, "@db");
          if (db.type == JV_OBJ)
            dbo = db.u.obj;
        }
        ev = make_event("error", 0, 0, js_undef());
        prevented = idb_fire(req.u.obj, txn.type == JV_OBJ ? txn.u.obj : 0, dbo, ev);
        if (!prevented && idb_txn_active(txn))
          idb_abort_txn(txn, err);
      } else {
        js_value ev;
        js_set(g_ctx, req, "result", js_get(g_ctx, req, "@result"));
        if (isup) {
          ev = make_event("upgradeneeded", 0, 0, js_undef());
          js_set(g_ctx, ev, "oldVersion", js_get(g_ctx, req, "@oldver"));
          js_set(g_ctx, ev, "newVersion", js_get(g_ctx, req, "@newver"));
        } else {
          ev = make_event("success", 0, 0, js_undef());
        }
        idb_fire(req.u.obj, 0, 0, ev);
      }
      if (txn.type == JV_OBJ && !isup)
        idb_txn_pending(txn, -1);
      gc_pop_scope();
      fired++;
      progress = true;
    }
    { /* commit/abort transactions whose queue drained */
      int i, n = g_idb_txns ? (int)js_array_len(g_idb_txns) : 0;
      for (i = 0; i < n; i++) {
        js_value txn = js_get_index(g_ctx, js_obj_value(g_idb_txns), (unsigned)i);
        js_value st, openreq;
        double pend;
        if (txn.type != JV_OBJ)
          continue;
        if (js_to_boolean(js_get(g_ctx, txn, "@aborted"))) {
          if (!js_to_boolean(js_get(g_ctx, txn, "@abort_fired"))) {
            js_value db = js_get(g_ctx, txn, "@db");
            js_value ev = make_event("abort", 0, 0, js_undef());
            js_set(g_ctx, txn, "@abort_fired", js_bool(1));
            js_set(g_ctx, txn, "@state", js_mk_cstring(g_ctx, "finished"));
            idb_fire(txn.u.obj, db.type == JV_OBJ ? db.u.obj : 0, 0, ev);
            fired++;
            progress = true;
          }
          continue;
        }
        st = js_get(g_ctx, txn, "@state");
        pend = js_to_number(g_ctx, js_get(g_ctx, txn, "@pending"));
        if (st.type == JV_STR && streq(st.u.str->data, "active") && pend <= 0) {
          js_value ev;
          js_set(g_ctx, txn, "@state", js_mk_cstring(g_ctx, "finished"));
          openreq = js_get(g_ctx, txn, "@openreq");
          if (openreq.type == JV_OBJ) {
            js_value db = js_get(g_ctx, txn, "@db");
            if (db.type == JV_OBJ)
              js_set(g_ctx, db, "@vtxn", js_null());
          }
          ev = make_event("complete", 0, 0, js_undef());
          idb_fire(txn.u.obj, 0, 0, ev);
          fired++;
          progress = true;
          if (openreq.type == JV_OBJ) { /* the upgrade finished: hand the open
                                           request its success */
            js_value sev;
            js_set(g_ctx, openreq, "readyState", js_mk_cstring(g_ctx, "done"));
            js_set(g_ctx, openreq, "result", js_get(g_ctx, txn, "@db"));
            sev = make_event("success", 0, 0, js_undef());
            idb_fire(openreq.u.obj, 0, 0, sev);
            fired++;
          }
        }
      }
    }
  } while (progress && ++guard < 10000);
  if (g_idb_txns) { /* drop finished transactions from the scan list */
    int i, n = (int)js_array_len(g_idb_txns), m;
    js_object *keep;
    gc_push_scope();
    keep = (js_object *)gc_keep(js_new_array(g_ctx, 0));
    for (i = 0; i < n; i++) {
      js_value txn = js_get_index(g_ctx, js_obj_value(g_idb_txns), (unsigned)i);
      if (txn.type == JV_OBJ) {
        js_value st = js_get(g_ctx, txn, "@state");
        if (!(st.type == JV_STR && streq(st.u.str->data, "finished")))
          js_array_push(g_ctx, keep, txn);
      }
    }
    m = (int)js_array_len(keep);
    js_array_set_len(g_ctx, g_idb_txns, (unsigned)m);
    for (i = 0; i < m; i++)
      js_set_index(g_ctx, js_obj_value(g_idb_txns), (unsigned)i, js_get_index(g_ctx, js_obj_value(keep), (unsigned)i));
    gc_pop_scope();
  }
  return fired;
}

/* Build the IndexedDB prototypes and constructors, expose window.indexedDB/IDBKeyRange. */
void js_idb_setup(js_ctx *ctx, js_object *win)
{
  int i;
  for (i = 0; i < IDBP_N; i++) {
    g_idb_proto[i] = js_new_object(ctx, ctx->object_proto);
    gc_add_root((void **)&g_idb_proto[i]);
  }
  g_idb_proto[IDBP_OPENREQ]->proto = g_idb_proto[IDBP_REQ];
  g_idb_proto[IDBP_CURSORVAL]->proto = g_idb_proto[IDBP_CURSOR];
  g_idb_dbs = js_new_object(ctx, 0);
  g_idb_pending = js_new_array(ctx, 0);
  g_idb_txns = js_new_array(ctx, 0);
  gc_add_root((void **)&g_idb_dbs);
  gc_add_root((void **)&g_idb_pending);
  gc_add_root((void **)&g_idb_txns);

  def_method(g_idb_proto[IDBP_REQ], "addEventListener", el_add_listener, 2);
  def_method(g_idb_proto[IDBP_REQ], "removeEventListener", el_remove_listener, 2);
  def_method(g_idb_proto[IDBP_REQ], "dispatchEvent", el_dispatch, 1);

  def_method(g_idb_proto[IDBP_DB], "createObjectStore", idb_db_createstore, 2);
  def_method(g_idb_proto[IDBP_DB], "deleteObjectStore", idb_db_deletestore, 1);
  def_method(g_idb_proto[IDBP_DB], "transaction", idb_db_transaction, 2);
  def_method(g_idb_proto[IDBP_DB], "close", idb_db_close, 0);
  def_method(g_idb_proto[IDBP_DB], "addEventListener", el_add_listener, 2);
  def_method(g_idb_proto[IDBP_DB], "removeEventListener", el_remove_listener, 2);
  def_acc(g_idb_proto[IDBP_DB], "objectStoreNames", idb_db_storenames, 0);

  def_method(g_idb_proto[IDBP_TXN], "objectStore", idb_txn_objectstore, 1);
  def_method(g_idb_proto[IDBP_TXN], "abort", idb_txn_abort, 0);
  def_method(g_idb_proto[IDBP_TXN], "commit", idb_txn_commit, 0);
  def_method(g_idb_proto[IDBP_TXN], "addEventListener", el_add_listener, 2);
  def_method(g_idb_proto[IDBP_TXN], "removeEventListener", el_remove_listener, 2);
  def_acc(g_idb_proto[IDBP_TXN], "objectStoreNames", idb_txn_storenames, 0);

  def_method(g_idb_proto[IDBP_STORE], "add", idb_store_add, 2);
  def_method(g_idb_proto[IDBP_STORE], "put", idb_store_put, 2);
  def_method(g_idb_proto[IDBP_STORE], "get", idb_store_get, 1);
  def_method(g_idb_proto[IDBP_STORE], "getKey", idb_store_getkey, 1);
  def_method(g_idb_proto[IDBP_STORE], "getAll", idb_store_getall, 2);
  def_method(g_idb_proto[IDBP_STORE], "getAllKeys", idb_store_getallkeys, 2);
  def_method(g_idb_proto[IDBP_STORE], "delete", idb_store_delete, 1);
  def_method(g_idb_proto[IDBP_STORE], "clear", idb_store_clear, 0);
  def_method(g_idb_proto[IDBP_STORE], "count", idb_store_count, 1);
  def_method(g_idb_proto[IDBP_STORE], "openCursor", idb_store_opencursor, 2);
  def_method(g_idb_proto[IDBP_STORE], "openKeyCursor", idb_store_openkeycursor, 2);
  def_method(g_idb_proto[IDBP_STORE], "index", idb_store_index, 1);
  def_method(g_idb_proto[IDBP_STORE], "createIndex", idb_store_createindex, 2);
  def_method(g_idb_proto[IDBP_STORE], "deleteIndex", idb_store_deleteindex, 1);
  def_acc(g_idb_proto[IDBP_STORE], "indexNames", idb_store_indexnames, 0);

  def_method(g_idb_proto[IDBP_INDEX], "get", idb_index_get, 1);
  def_method(g_idb_proto[IDBP_INDEX], "getKey", idb_index_getkey, 1);
  def_method(g_idb_proto[IDBP_INDEX], "getAll", idb_index_getall, 2);
  def_method(g_idb_proto[IDBP_INDEX], "getAllKeys", idb_index_getallkeys, 2);
  def_method(g_idb_proto[IDBP_INDEX], "count", idb_index_count, 1);
  def_method(g_idb_proto[IDBP_INDEX], "openCursor", idb_index_opencursor, 2);
  def_method(g_idb_proto[IDBP_INDEX], "openKeyCursor", idb_index_openkeycursor, 2);

  def_method(g_idb_proto[IDBP_CURSOR], "continue", idb_cursor_continue, 1);
  def_method(g_idb_proto[IDBP_CURSOR], "advance", idb_cursor_advance, 1);
  def_method(g_idb_proto[IDBP_CURSOR], "update", idb_cursor_update, 1);
  def_method(g_idb_proto[IDBP_CURSOR], "delete", idb_cursor_delete, 0);

  def_method(g_idb_proto[IDBP_KEYRANGE], "includes", idb_kr_includes, 1);

  {
    js_object *factory = js_new_object(ctx, ctx->object_proto);
    js_object *kr = js_new_native(ctx, idb_illegal_ctor, "IDBKeyRange", 0);
    gc_push_scope();
    gc_keep(factory);
    gc_keep(kr);
    def_method(factory, "open", idb_factory_open, 2);
    def_method(factory, "deleteDatabase", idb_factory_delete, 1);
    def_method(factory, "databases", idb_factory_databases, 0);
    def_method(factory, "cmp", idb_factory_cmp, 2);
    js_def(win, "indexedDB", js_obj_value(factory), P_WRITABLE | P_CONFIGURABLE);
    js_def(kr, "prototype", js_obj_value(g_idb_proto[IDBP_KEYRANGE]), 0);
    js_def(g_idb_proto[IDBP_KEYRANGE], "constructor", js_obj_value(kr), P_WRITABLE | P_CONFIGURABLE);
    def_method(kr, "only", idb_kr_only, 1);
    def_method(kr, "lowerBound", idb_kr_lower, 2);
    def_method(kr, "upperBound", idb_kr_upper, 2);
    def_method(kr, "bound", idb_kr_bound, 4);
    js_def(win, "IDBKeyRange", js_obj_value(kr), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  define_iface(ctx, win, "IDBRequest", g_idb_proto[IDBP_REQ]);
  define_iface(ctx, win, "IDBOpenDBRequest", g_idb_proto[IDBP_OPENREQ]);
  define_iface(ctx, win, "IDBDatabase", g_idb_proto[IDBP_DB]);
  define_iface(ctx, win, "IDBTransaction", g_idb_proto[IDBP_TXN]);
  define_iface(ctx, win, "IDBObjectStore", g_idb_proto[IDBP_STORE]);
  define_iface(ctx, win, "IDBIndex", g_idb_proto[IDBP_INDEX]);
  define_iface(ctx, win, "IDBCursor", g_idb_proto[IDBP_CURSOR]);
  define_iface(ctx, win, "IDBCursorWithValue", g_idb_proto[IDBP_CURSORVAL]);
}

/* Unroot the page's IndexedDB state at navigation teardown. */
void js_idb_teardown(void)
{
  int i;
  for (i = 0; i < IDBP_N; i++) {
    gc_remove_root((void **)&g_idb_proto[i]);
    g_idb_proto[i] = 0;
  }
  gc_remove_root((void **)&g_idb_dbs);
  gc_remove_root((void **)&g_idb_pending);
  gc_remove_root((void **)&g_idb_txns);
  g_idb_dbs = g_idb_pending = g_idb_txns = 0;
}
