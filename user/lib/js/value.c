/* value.c — runtime values, the object model, the GC glue, and coercions. */
#include "value.h"
#include "ast.h"
#include "bigint.h"
#include "fmt.h"
#include "gc.h"
#include "interp.h"
#include "math.h"
#include "owos.h"

/* ------------------------------------------------------- small helpers ---- */

static char *dupz(const char *s, int n)
{
  char *d = (char *)malloc((unsigned)n + 1);
  if (!d)
    return NULL;
  memcpy(d, s, (unsigned)n);
  d[n] = 0;
  return d;
}

/* the libc here has no memcmp */
static int memcmp_(const void *a, const void *b, int n)
{
  const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
  int i;
  for (i = 0; i < n; i++)
    if (x[i] != y[i])
      return (int)x[i] - (int)y[i];
  return 0;
}

unsigned js_str_hash(const char *s, int len)
{
  unsigned h = 2166136261u;
  int i;
  for (i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 16777619u;
  }
  return h;
}

/* Canonical array index (no leading zero): returns 1 and sets *out. */
#define JS_ARRAY_MAX_LEN (1u << 26)

/* a canonical array index ("0","1",...,no leading zero) -> 1 and *out */
static int as_index(const char *key, unsigned *out)
{
  const char *p = key;
  unsigned v = 0;
  if (!*p)
    return 0;
  if (*p == '0' && p[1])
    return 0;
  for (; *p; p++) {
    if (*p < '0' || *p > '9')
      return 0;
    if (v > 429496729u)
      return 0; /* overflow guard                    */
    v = v * 10u + (unsigned)(*p - '0');
  }
  *out = v;
  return 1;
}

js_value js_undef(void)
{
  js_value v;
  v.type = JV_UNDEF;
  v.u.obj = NULL;
  return v;
}
js_value js_null(void)
{
  js_value v;
  v.type = JV_NULL;
  v.u.obj = NULL;
  return v;
}
js_value js_bool(int b)
{
  js_value v;
  v.type = JV_BOOL;
  v.u.boolean = !!b;
  return v;
}
js_value js_number(double n)
{
  js_value v;
  v.type = JV_NUM;
  v.u.num = n;
  return v;
}
js_value js_obj_value(js_object *o)
{
  js_value v;
  if (!o)
    return js_null();
  v.type = JV_OBJ;
  v.u.obj = o;
  return v;
}

/* ------------------------------------------------------- property map ----- */

/* Build the hash index once a map is large enough for the linear scan to hurt:
 * property lookup dominates JS-heavy pages, and objects like the global scope,
 * `window`, `document` and script-side caches accumulate hundreds of keys.
 * Below this many entries a linear scan of the cache-hot `e` array wins. */
#define PMAP_HASH_MIN 16

static unsigned pmap_keyhash(const char *k)
{
  unsigned h = 2166136261u; /* FNV-1a over the key bytes */
  for (; *k; k++) {
    h ^= (unsigned char)*k;
    h *= 16777619u;
  }
  return h;
}

/* (Re)build idx from the current entry array. Entry indices are stable across
 * the `e` realloc in pmap_insert, so callers never need to touch idx on grow.
 * On OOM the map degrades to linear scan (idx stays NULL). */
static void pmap_reindex(pmap *m)
{
  int cap = 16, i;
  unsigned mask;
  int *idx;
  while (cap < m->n * 2)
    cap <<= 1;
  idx = (int *)malloc((unsigned)cap * sizeof(int));
  if (!idx) {
    free(m->idx);
    m->idx = NULL;
    m->idxcap = 0;
    return;
  }
  for (i = 0; i < cap; i++)
    idx[i] = 0;
  free(m->idx);
  m->idx = idx;
  m->idxcap = cap;
  mask = (unsigned)cap - 1;
  for (i = 0; i < m->n; i++) {
    unsigned b = m->e[i].hash & mask;
    while (idx[b])
      b = (b + 1) & mask;
    idx[b] = i + 1;
  }
}

static int pmap_find_idx_hash(pmap *m, const char *key, unsigned hash)
{
  int i;
  if (m->idx) {
    unsigned mask = (unsigned)m->idxcap - 1;
    unsigned b = hash & mask;
    for (;;) {
      int slot = m->idx[b];
      if (!slot)
        return -1;
      if (m->e[slot - 1].hash == hash && strcmp(m->e[slot - 1].key, key) == 0)
        return slot - 1;
      b = (b + 1) & mask;
    }
  }
  for (i = 0; i < m->n; i++)
    if (m->e[i].hash == hash && strcmp(m->e[i].key, key) == 0)
      return i;
  return -1;
}

static int pmap_find_idx(pmap *m, const char *key) { return pmap_find_idx_hash(m, key, pmap_keyhash(key)); }

static pentry *pmap_find(pmap *m, const char *key)
{
  int i = pmap_find_idx(m, key);
  return i < 0 ? NULL : &m->e[i];
}

/* Public: entry index of `key` (or -1). Backs the interpreter's identifier
 * inline cache, which records the slot to skip future scope-chain scans. */
int pmap_index(pmap *m, const char *key) { return pmap_find_idx(m, key); }
int pmap_index_hash(pmap *m, const char *key, unsigned hash) { return pmap_find_idx_hash(m, key, hash); }

static pentry *pmap_insert(pmap *m, const char *key)
{
  pentry *pe;
  if (m->n == m->cap) {
    int nc = m->cap ? m->cap * 2 : 8;
    pentry *ne = (pentry *)realloc(m->e, (unsigned)nc * sizeof(pentry));
    if (!ne)
      return NULL;
    m->e = ne;
    m->cap = nc;
  }
  pe = &m->e[m->n++];
  pe->key = dupz(key, strlen(key));
  if (!pe->key) {
    m->n--;
    return NULL;
  }
  pe->value = js_undef();
  pe->hash = pmap_keyhash(pe->key);
  pe->getter = pe->setter = NULL;
  pe->flags = 0;
  /* Keep the hash index current: build it once the map crosses the threshold,
   * grow+rehash before it passes half full, else drop the new entry into its
   * bucket. The entry's array index is stable across the realloc above. */
  if (m->idx) {
    if (m->n * 2 > m->idxcap)
      pmap_reindex(m);
    else {
      unsigned mask = (unsigned)m->idxcap - 1;
      unsigned b = pe->hash & mask;
      while (m->idx[b])
        b = (b + 1) & mask;
      m->idx[b] = m->n; /* (n-1) + 1 */
    }
  } else if (m->n >= PMAP_HASH_MIN) {
    pmap_reindex(m);
  }
  return pe;
}

static void pmap_free(pmap *m)
{
  int i;
  for (i = 0; i < m->n; i++)
    free(m->e[i].key);
  if (m->e)
    free(m->e);
  if (m->idx)
    free(m->idx);
  m->e = NULL;
  m->idx = NULL;
  m->n = m->cap = m->idxcap = 0;
}

/* ------------------------------------------------------- GC tracer -------- */

static void mark_value(js_value v)
{
  if (v.type == JV_OBJ || v.type == JV_SYMBOL || v.type == JV_BIGINT)
    gc_mark(v.u.obj);
  else if (v.type == JV_STR)
    gc_mark(v.u.str);
}

static void mark_pmap(pmap *m)
{
  int i;
  for (i = 0; i < m->n; i++) {
    if (m->e[i].flags & P_ACCESSOR) {
      gc_mark(m->e[i].getter);
      gc_mark(m->e[i].setter);
    } else
      mark_value(m->e[i].value);
  }
}

static void trace(void *obj, int kind)
{
  if (kind == GCK_STRING)
    return; /* bytes are malloc'd, not GC    */
  if (kind == GCK_ENV) {
    js_env *e = (js_env *)obj;
    gc_mark(e->parent);
    mark_pmap(&e->vars);
    if (e->has_this)
      mark_value(e->thisv);
    gc_mark(e->home);
    return;
  }
  {
    js_object *o = (js_object *)obj;
    unsigned i;
    gc_mark(o->proto);
    mark_pmap(&o->props);
    for (i = 0; i < o->elen; i++)
      mark_value(o->elems[i]);
    if (o->fn) {
      gc_mark(o->fn->closure);
      gc_mark(o->fn->home);
      mark_value(o->fn->data);
    }
    if (o->kind == OBJ_TYPEDARRAY && o->host)
      gc_mark(((js_typedarray *)o->host)->buffer);
    if (o->kind == OBJ_GENERATOR && o->host) {
      js_generator *g = (js_generator *)o->host;
      gc_mark(g->env);
      mark_value(g->xfer);
      gc_scope_trace(&g->parked);
    }
  }
}

void (*js_host_finalizer)(js_object *o);

static void finalize(void *obj, int kind)
{
  if (kind == GCK_STRING) {
    js_string *s = (js_string *)obj;
    if (s->data)
      free(s->data);
    return;
  }
  if (kind == GCK_ENV) {
    js_env *e = (js_env *)obj;
    pmap_free(&e->vars);
    return;
  }
  {
    js_object *o = (js_object *)obj;
    pmap_free(&o->props);
    if (o->elems)
      free(o->elems);
    if (o->fn)
      free(o->fn);
    if (o->kind == OBJ_SYMBOL && o->host)
      free(o->host); /* the symbol key string */
    if (o->kind == OBJ_BIGINT && o->host)
      bi_free((bigint *)o->host);
    if (o->kind == OBJ_ARRAYBUFFER && o->host) {
      js_arraybuffer *ab = (js_arraybuffer *)o->host;
      if (ab->data) {
        if (ab->shared)
          js_sab_release(ab->data);
        else
          free(ab->data);
      }
      free(ab);
    }
    if (o->kind == OBJ_TYPEDARRAY && o->host)
      free(o->host); /* the view; its buffer is GC'd separately */
    if (o->kind == OBJ_GENERATOR && o->host) {
      js_generator *g = (js_generator *)o->host;
      gc_scope_drop(&g->parked);
      for (int i = 0; i < g->nheld; i++)
        free(*g->held[i]);
      free(g->held);
      free(g->stack);
      free(g);
    }
    if (o->kind == OBJ_HOST && o->host && js_host_finalizer)
      js_host_finalizer(o);
  }
}

typedef struct {
  unsigned refs;
  unsigned pad[3]; /* keeps the bytes 16-aligned */
} sab_header;

unsigned char *js_sab_alloc(int n)
{
  sab_header *h = (sab_header *)calloc(1, sizeof *h + (unsigned)(n > 0 ? n : 1));
  if (!h)
    return NULL;
  h->refs = 1;
  return (unsigned char *)(h + 1);
}

void js_sab_retain(unsigned char *data) { __atomic_fetch_add(&((sab_header *)data - 1)->refs, 1, __ATOMIC_RELAXED); }

void js_sab_release(unsigned char *data)
{
  sab_header *h = (sab_header *)data - 1;
  if (__atomic_fetch_sub(&h->refs, 1, __ATOMIC_ACQ_REL) == 1)
    free(h);
}

void js_gc_setup(void)
{
  /* Every js_ctx on a thread shares that thread's GC heap (the browser holds one context per tab, all
   * coexisting, each rooting its own world), so initialize it once: a later js_ctx_new must not re-init
   * it, or it would wipe the other contexts' roots and objects. js_ctx_free drops a context's roots and
   * collects, draining its objects from the heap. */
  if (gc_ready())
    return;
  gc_init(trace, finalize);
  gc_set_rescan_kind(GCK_ARRAY);
}

/* ------------------------------------------------------- creators --------- */

js_value js_mk_string(js_ctx *ctx, const char *s, int len)
{
  js_string *str = (js_string *)gc_alloc(sizeof(js_string), GCK_STRING);
  js_value v;
  (void)ctx;
  str->data = dupz(s ? s : "", len);
  str->len = len;
  str->hash = js_str_hash(s ? s : "", len);
  v.type = JV_STR;
  v.u.str = str;
  return v;
}

js_value js_mk_cstring(js_ctx *ctx, const char *s) { return js_mk_string(ctx, s, s ? strlen(s) : 0); }

/* Wrap a bignum as the JV_BIGINT primitive. Takes ownership of `b`: the
 * OBJ_BIGINT cell that carries it is GC-managed and its finalizer bi_free()s
 * `b`. A NULL `b` (an arithmetic size-cap / OOM signal) surfaces as a
 * RangeError. */
js_value js_mk_bigint(js_ctx *ctx, bigint *b)
{
  js_object *o;
  js_value v;
  if (!b) {
    js_throw_range(ctx, "Maximum BigInt size exceeded");
    return js_undef();
  }
  o = js_new_object(ctx, ctx->bigint_proto);
  o->kind = OBJ_BIGINT;
  o->host = b;
  v.type = JV_BIGINT;
  v.u.obj = o;
  return v;
}

/* The bignum behind a JV_BIGINT primitive or an Object(bigint) box, else NULL.
 */
bigint *js_bigint_of(js_value v)
{
  if (v.type == JV_BIGINT)
    return (bigint *)v.u.obj->host;
  if (v.type == JV_OBJ && v.u.obj->kind == OBJ_BIGINT)
    return (bigint *)v.u.obj->host;
  return NULL;
}

js_object *js_new_object(js_ctx *ctx, js_object *proto)
{
  js_object *o = (js_object *)gc_alloc(sizeof(js_object), GCK_OBJECT);
  (void)ctx;
  o->kind = OBJ_PLAIN;
  o->extensible = 1;
  o->proto = proto;
  return o;
}

js_object *js_new_array(js_ctx *ctx, unsigned len)
{
  js_object *o;
  gc_push_scope();
  o = (js_object *)gc_keep(gc_alloc(sizeof(js_object), GCK_ARRAY));
  o->kind = OBJ_ARRAY;
  o->extensible = 1;
  o->proto = ctx->array_proto;
  if (len) {
    unsigned i;
    o->elems = (js_value *)malloc(len * sizeof(js_value));
    if (!o->elems) {
      gc_pop_scope();
      return o;
    }
    o->ecap = len;
    o->elen = len;
    for (i = 0; i < len; i++)
      o->elems[i] = js_undef();
  }
  gc_pop_scope();
  return o;
}

js_object *js_new_function(js_ctx *ctx, js_node *node, js_env *closure)
{
  js_object *o;
  js_func *f;
  gc_push_scope();
  o = (js_object *)gc_keep(gc_alloc(sizeof(js_object), GCK_OBJECT));
  o->kind = OBJ_FUNCTION;
  o->extensible = 1;
  o->proto = ctx->function_proto;
  f = (js_func *)malloc(sizeof(js_func));
  if (!f)
    gc_oom();
  memset(f, 0, sizeof(js_func));
  f->node = node;
  f->closure = closure;
  f->is_arrow = node ? (unsigned char)node->u.func.is_arrow : 0;
  f->name = node && node->u.func.name ? node->u.func.name : "";
  o->fn = f;
  /* non-arrow functions get a fresh .prototype object with a back-ref */
  if (node && node->u.func.is_generator) {
    js_object *base = node->u.func.is_async ? ctx->async_generator_proto : ctx->generator_proto;
    js_def(o, "prototype", js_obj_value(js_new_object(ctx, base)), P_WRITABLE);
  } else if (!f->is_arrow) {
    js_object *proto = js_new_object(ctx, ctx->object_proto);
    js_def(proto, "constructor", js_obj_value(o), P_WRITABLE | P_CONFIGURABLE);
    js_def(o, "prototype", js_obj_value(proto), P_WRITABLE);
  }
  js_def(o, "name", js_mk_cstring(ctx, f->name), P_CONFIGURABLE);
  /* .length is the count of leading params before the first default or rest */
  {
    int arity = 0, i;
    for (i = 0; node && i < node->u.func.nparams; i++) {
      js_node *p = node->u.func.params[i];
      if (p->type == JS_REST || p->type == JS_ASSIGN_PATTERN)
        break;
      arity++;
    }
    js_def(o, "length", js_number(arity), P_CONFIGURABLE);
  }
  gc_pop_scope();
  return o;
}

js_object *js_new_native(js_ctx *ctx, js_native fn, const char *name, int arity)
{
  js_object *o;
  js_func *f;
  gc_push_scope();
  o = (js_object *)gc_keep(gc_alloc(sizeof(js_object), GCK_OBJECT));
  o->kind = OBJ_FUNCTION;
  o->extensible = 1;
  o->proto = ctx->function_proto;
  f = (js_func *)malloc(sizeof(js_func));
  if (!f)
    gc_oom();
  memset(f, 0, sizeof(js_func));
  f->native = fn;
  f->name = name ? name : "";
  o->fn = f;
  js_def(o, "name", js_mk_cstring(ctx, f->name), P_CONFIGURABLE);
  js_def(o, "length", js_number(arity), P_CONFIGURABLE);
  gc_pop_scope();
  return o;
}

js_env *js_new_env(js_ctx *ctx, js_env *parent, int is_fn_scope)
{
  js_env *e = (js_env *)gc_alloc(sizeof(js_env), GCK_ENV);
  (void)ctx;
  e->parent = parent;
  e->is_fn_scope = (unsigned char)is_fn_scope;
  e->thisv = js_undef();
  return e;
}

pentry *js_env_lookup(js_env *e, const char *name)
{
  for (; e; e = e->parent) {
    pentry *pe = pmap_find(&e->vars, name);
    if (pe)
      return pe;
  }
  return NULL;
}

void js_env_define(js_env *e, const char *name, js_value v, unsigned char flags)
{
  pentry *pe = pmap_find(&e->vars, name);
  if (!pe)
    pe = pmap_insert(&e->vars, name);
  if (!pe)
    return;
  pe->value = v;
  pe->getter = pe->setter = NULL;
  pe->flags = flags;
  gc_write_barrier(e); /* an old scope may now hold a young binding value */
}

js_value js_env_get_this(js_env *e)
{
  for (; e; e = e->parent)
    if (e->has_this)
      return e->thisv;
  return js_undef();
}

js_object *js_env_get_home(js_env *e)
{
  for (; e; e = e->parent)
    if (e->has_this)
      return e->home;
  return NULL;
}

/* ------------------------------------------------------- properties ------- */

void js_def(js_object *o, const char *key, js_value val, unsigned char flags)
{
  pentry *pe;
  if ((unsigned char)key[0] < 0x20)
    flags &= (unsigned char)~P_ENUMERABLE; /* symbol-keyed: never enumerable */
  pe = pmap_find(&o->props, key);
  if (!pe)
    pe = pmap_insert(&o->props, key);
  if (!pe)
    return;
  pe->value = val;
  pe->getter = pe->setter = NULL;
  pe->flags = flags;
  gc_write_barrier(o); /* an old object may now hold a young property value */
}

void js_def_accessor(js_object *o, const char *key, js_object *get, js_object *set, unsigned char flags)
{
  pentry *pe = pmap_find(&o->props, key);
  if (!pe)
    pe = pmap_insert(&o->props, key);
  if (!pe)
    return;
  pe->value = js_undef();
  pe->getter = get;
  pe->setter = set;
  pe->flags = (unsigned char)(flags | P_ACCESSOR);
  gc_write_barrier(o); /* an old object may now hold a young getter/setter */
}

int js_has_own(js_object *o, const char *key)
{
  unsigned idx;
  if (o->kind == OBJ_ARRAY) {
    if (strcmp(key, "length") == 0)
      return 1;
    if (as_index(key, &idx))
      return idx < o->elen;
  }
  return pmap_find(&o->props, key) != NULL;
}

int js_delete(js_object *o, const char *key)
{
  pmap *m = &o->props;
  int i;
  unsigned idx;
  if (o->kind == OBJ_ARRAY && as_index(key, &idx) && idx < o->elen) {
    o->elems[idx] = js_undef();
    return 1;
  }
  for (i = 0; i < m->n; i++)
    if (strcmp(m->e[i].key, key) == 0) {
      free(m->e[i].key);
      for (; i < m->n - 1; i++)
        m->e[i] = m->e[i + 1];
      m->n--;
      /* Compaction shifted every later entry's index, so the hash index is
       * stale: rebuild it if the map is still large, else drop it. */
      if (m->idx) {
        if (m->n >= PMAP_HASH_MIN)
          pmap_reindex(m);
        else {
          free(m->idx);
          m->idx = NULL;
          m->idxcap = 0;
        }
      }
      return 1;
    }
  return 1;
}

/* public shims over the static property map, used by interp.c / builtins.c */
pentry *pmap_find_pub(js_object *o, const char *key) { return pmap_find(&o->props, key); }
int pmap_find_in_env(js_env *e, const char *name) { return pmap_find(&e->vars, name) != NULL; }

unsigned js_array_len(js_object *arr) { return arr->elen; }

void js_array_set_len(js_ctx *ctx, js_object *arr, unsigned len)
{
  (void)ctx;
  if (len <= arr->elen) {
    arr->elen = len;
    return;
  }
  if (len > JS_ARRAY_MAX_LEN)
    return;
  if (len > arr->ecap) {
    unsigned nc = arr->ecap ? arr->ecap * 2 : 8;
    while (nc < len)
      nc *= 2;
    js_value *elems = (js_value *)realloc(arr->elems, nc * sizeof *elems);
    if (!elems)
      return;
    arr->elems = elems;
    arr->ecap = nc;
  }
  while (arr->elen < len)
    arr->elems[arr->elen++] = js_undef();
}

void js_array_push(js_ctx *ctx, js_object *arr, js_value v)
{
  gc_push_scope();
  if (v.type == JV_OBJ)
    gc_keep(v.u.obj);
  else if (v.type == JV_STR)
    gc_keep(v.u.str);
  if (arr->elen == arr->ecap) {
    unsigned nc = arr->ecap ? arr->ecap * 2 : 8;
    js_value *elems = (js_value *)realloc(arr->elems, nc * sizeof *elems);
    if (!elems) {
      gc_pop_scope();
      return;
    }
    arr->elems = elems;
    arr->ecap = nc;
  }
  arr->elems[arr->elen++] = v;
  gc_pop_scope();
  (void)ctx;
}

int ta_elem_size(int type)
{
  switch (type) {
  case TA_I8:
  case TA_U8:
  case TA_U8C:
    return 1;
  case TA_I16:
  case TA_U16:
  case TA_F16:
    return 2;
  case TA_F64:
  case TA_I64:
  case TA_U64:
    return 8;
  default:
    return 4; /* I32/U32/F32 */
  }
}

js_value js_bigint_from_u64(js_ctx *ctx, unsigned lo, unsigned hi, int is_signed)
{
  bigint *r;
  if (is_signed || !(hi >> 31))
    return js_mk_bigint(ctx, bi_from_i64((long long)((unsigned long long)hi << 32 | lo)));
  {
    bigint *h = bi_from_i64(hi), *t = h ? bi_shl(h, 32) : 0, *l = bi_from_i64(lo);
    r = t && l ? bi_add(t, l) : 0;
    bi_free(h);
    bi_free(t);
    bi_free(l);
  }
  return js_mk_bigint(ctx, r);
}

int js_bigint_to_u64(js_ctx *ctx, js_value v, unsigned *lo, unsigned *hi)
{
  bigint *b = js_bigint_of(v), *u;
  if (!b) {
    js_throw_type(ctx, "Cannot convert value to a BigInt");
    return 0;
  }
  u = bi_as_uintn(64, b);
  if (!u) {
    js_throw_range(ctx, "Maximum BigInt size exceeded");
    return 0;
  }
  *lo = u->len > 0 ? u->limb[0] : 0;
  *hi = u->len > 1 ? u->limb[1] : 0;
  bi_free(u);
  return 1;
}

double js_f16_decode(unsigned h)
{
  int e = (h >> 10) & 31;
  double m = (double)(h & 1023), v;
  if (e == 0)
    v = ldexp(m, -24);
  else if (e == 31)
    v = m ? NAN : INFINITY;
  else
    v = ldexp(1024.0 + m, e - 25);
  return (h & 0x8000) ? -v : v;
}

/* round to nearest, ties to even */
unsigned js_f16_encode(double d)
{
  unsigned sign = signbit(d) ? 0x8000 : 0, q;
  double a = d < 0 ? -d : d, x, frac;
  int e = 0;
  if (isnan(d))
    return 0x7e00;
  if (a >= 65520.0)
    return sign | 0x7c00;
  if (a < ldexp(1.0, -14)) {
    x = ldexp(a, 24);
  } else {
    while (a >= 2.0) {
      a /= 2.0;
      e++;
    }
    while (a < 1.0) {
      a *= 2.0;
      e--;
    }
    x = (a - 1.0) * 1024.0;
    e += 15;
  }
  q = (unsigned)x;
  frac = x - (double)q;
  if (frac > 0.5 || (frac == 0.5 && (q & 1)))
    q++;
  return sign | (e > 0 ? ((unsigned)e << 10) + q : q);
}

js_value ta_get(js_ctx *ctx, js_object *view, unsigned idx)
{
  js_typedarray *ta = (js_typedarray *)view->host;
  js_arraybuffer *ab;
  unsigned char *p;
  if (!ta || ta->type < 0 || idx >= (unsigned)ta_length(ta))
    return js_undef();
  ab = (js_arraybuffer *)ta->buffer->host;
  p = ab->data + ta->offset + idx * (unsigned)ta_elem_size(ta->type);
  switch (ta->type) {
  case TA_I8:
    return js_number((double)*(signed char *)p);
  case TA_U8:
  case TA_U8C:
    return js_number((double)*p);
  case TA_I16: {
    short x;
    memcpy(&x, p, 2);
    return js_number((double)x);
  }
  case TA_U16: {
    unsigned short x;
    memcpy(&x, p, 2);
    return js_number((double)x);
  }
  case TA_I32: {
    int x;
    memcpy(&x, p, 4);
    return js_number((double)x);
  }
  case TA_U32: {
    unsigned x;
    memcpy(&x, p, 4);
    return js_number((double)x);
  }
  case TA_F32: {
    float x;
    memcpy(&x, p, 4);
    return js_number((double)x);
  }
  case TA_F64: {
    double x;
    memcpy(&x, p, 8);
    return js_number(x);
  }
  case TA_F16: {
    unsigned short x;
    memcpy(&x, p, 2);
    return js_number(js_f16_decode(x));
  }
  case TA_I64:
  case TA_U64: {
    unsigned w[2];
    memcpy(w, p, 8);
    return js_bigint_from_u64(ctx, w[0], w[1], ta->type == TA_I64);
  }
  }
  return js_undef();
}

void ta_set(js_ctx *ctx, js_object *view, unsigned idx, js_value v)
{
  js_typedarray *ta = (js_typedarray *)view->host;
  js_arraybuffer *ab;
  unsigned char *p;
  if (!ta || ta->type < 0 || idx >= (unsigned)ta_length(ta))
    return;
  ab = (js_arraybuffer *)ta->buffer->host;
  p = ab->data + ta->offset + idx * (unsigned)ta_elem_size(ta->type);
  if (ta->type == TA_F32) {
    float f = (float)js_to_number(ctx, v);
    memcpy(p, &f, 4);
    return;
  }
  if (ta->type == TA_F64) {
    double d = js_to_number(ctx, v);
    memcpy(p, &d, 8);
    return;
  }
  if (ta->type == TA_F16) {
    unsigned short h = (unsigned short)js_f16_encode(js_to_number(ctx, v));
    memcpy(p, &h, 2);
    return;
  }
  if (ta->type == TA_I64 || ta->type == TA_U64) {
    unsigned w[2];
    if (js_bigint_to_u64(ctx, v, &w[0], &w[1]))
      memcpy(p, w, 8);
    return;
  }
  if (ta->type == TA_U8C) { /* ToUint8Clamp */
    double d = js_to_number(ctx, v);
    int x;
    if (d != d || d <= 0)
      x = 0;
    else if (d >= 255)
      x = 255;
    else
      x = (int)(d + 0.5);
    *p = (unsigned char)x;
    return;
  }
  { /* integer kinds: ToUint32 then store the low byte(s) (little-endian x86) */
    unsigned u = js_to_uint32(ctx, v);
    memcpy(p, &u, (unsigned)ta_elem_size(ta->type));
  }
}

void js_set_index(js_ctx *ctx, js_value obj, unsigned i, js_value val)
{
  if (obj.type == JV_OBJ && obj.u.obj->kind == OBJ_ARRAY) {
    js_object *a = obj.u.obj;
    if (i >= a->elen) {
      js_array_set_len(ctx, a, i + 1);
      if (i >= a->elen) {
        js_throw_range(ctx, "out of memory");
        return;
      }
    }
    a->elems[i] = val;
    return;
  }
  if (obj.type == JV_OBJ && obj.u.obj->kind == OBJ_TYPEDARRAY) {
    ta_set(ctx, obj.u.obj, i, val);
    return;
  }
  {
    char buf[24];
    u64toa((unsigned long long)i, buf, 10);
    js_set(ctx, obj, buf, val);
  }
}

js_value js_get_index(js_ctx *ctx, js_value obj, unsigned i)
{
  if (obj.type == JV_OBJ && obj.u.obj->kind == OBJ_ARRAY) {
    js_object *a = obj.u.obj;
    if (i < a->elen)
      return a->elems[i];
    return js_undef();
  }
  if (obj.type == JV_OBJ && obj.u.obj->kind == OBJ_TYPEDARRAY)
    return ta_get(ctx, obj.u.obj, i);
  if (obj.type == JV_STR) {
    if ((int)i < obj.u.str->len)
      return js_mk_string(ctx, obj.u.str->data + i, 1);
    return js_undef();
  }
  {
    char buf[24];
    u64toa((unsigned long long)i, buf, 10);
    return js_get(ctx, obj, buf);
  }
}

static js_object *proto_for_primitive(js_ctx *ctx, js_value v)
{
  if (v.type == JV_STR)
    return ctx->string_proto;
  if (v.type == JV_NUM)
    return ctx->number_proto;
  if (v.type == JV_BOOL)
    return ctx->boolean_proto;
  if (v.type == JV_SYMBOL)
    return ctx->symbol_proto;
  if (v.type == JV_BIGINT)
    return ctx->bigint_proto;
  return NULL;
}

js_value js_get(js_ctx *ctx, js_value obj, const char *key)
{
  js_object *o;
  unsigned idx;

  if (obj.type == JV_STR) {
    if (strcmp(key, "length") == 0)
      return js_number(obj.u.str->len);
    if (as_index(key, &idx))
      return js_get_index(ctx, obj, idx);
    o = ctx->string_proto;
  } else if (obj.type == JV_NUM || obj.type == JV_BOOL || obj.type == JV_SYMBOL || obj.type == JV_BIGINT) {
    o = proto_for_primitive(ctx, obj);
  } else if (obj.type == JV_OBJ) {
    o = obj.u.obj;
    if (o->kind == OBJ_PROXY)
      return js_proxy_get(ctx, o, key, obj);
    if (o->kind == OBJ_ARRAY) {
      if (strcmp(key, "length") == 0)
        return js_number(o->elen);
      if (as_index(key, &idx))
        return js_get_index(ctx, obj, idx);
    }
    if (o->kind == OBJ_TYPEDARRAY && as_index(key, &idx))
      return ta_get(ctx, o, idx);
  } else {
    char msg[160];
    int i = 0;
    const char *p;
    for (p = "cannot read property '"; *p && i < 120; p++)
      msg[i++] = *p;
    for (p = key; *p && i < 140; p++)
      msg[i++] = *p;
    for (p = "' of "; *p; p++)
      msg[i++] = *p;
    for (p = obj.type == JV_NULL ? "null" : "undefined"; *p; p++)
      msg[i++] = *p;
    msg[i] = '\0';
    return js_throw_type(ctx, msg);
  }

  for (; o; o = o->proto) {
    pentry *pe = pmap_find(&o->props, key);
    if (pe) {
      if (pe->flags & P_ACCESSOR) {
        if (!pe->getter)
          return js_undef();
        return js_call(ctx, js_obj_value(pe->getter), obj, NULL, 0);
      }
      return pe->value;
    }
  }
  return js_undef();
}

void js_set(js_ctx *ctx, js_value obj, const char *key, js_value val)
{
  js_object *o, *p;
  unsigned idx;

  if (obj.type != JV_OBJ)
    return; /* set on a primitive is a no-op */
  o = obj.u.obj;

  if (o->kind == OBJ_PROXY) {
    js_proxy_set(ctx, o, key, val, obj);
    return;
  }
  if (o->kind == OBJ_ARRAY) {
    if (strcmp(key, "length") == 0) {
      js_array_set_len(ctx, o, js_to_uint32(ctx, val));
      return;
    }
    if (as_index(key, &idx)) {
      js_set_index(ctx, obj, idx, val);
      return;
    }
  }
  if (o->kind == OBJ_TYPEDARRAY && as_index(key, &idx)) {
    ta_set(ctx, o, idx, val);
    return;
  }

  /* own data property: update in place */
  {
    pentry *pe = pmap_find(&o->props, key);
    if (pe) {
      if (pe->flags & P_ACCESSOR) {
        if (pe->setter)
          js_call(ctx, js_obj_value(pe->setter), obj, &val, 1);
        return;
      }
      if (pe->flags & P_WRITABLE) {
        pe->value = val;
        gc_write_barrier(o);
      }
      return;
    }
  }
  /* inherited accessor with a setter */
  for (p = o->proto; p; p = p->proto) {
    pentry *pe = pmap_find(&p->props, key);
    if (pe) {
      if (pe->flags & P_ACCESSOR) {
        if (pe->setter)
          js_call(ctx, js_obj_value(pe->setter), obj, &val, 1);
        return;
      }
      break;
    }
  }
  /* define a new own property */
  if (o->extensible)
    js_def(o, key, val, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
}

/* ------------------------------------------------------- Proxy ------------ */

int js_is_proxy(js_object *o) { return o->kind == OBJ_PROXY; }
static js_value proxy_target(js_object *p)
{
  pentry *pe = pmap_find(&p->props, "@t");
  return pe ? pe->value : js_undef();
}
static js_value proxy_handler(js_object *p)
{
  pentry *pe = pmap_find(&p->props, "@h");
  return pe ? pe->value : js_undef();
}
js_object *js_proxy_ownkeys_target(js_object *p)
{
  js_value t = proxy_target(p);
  return t.type == JV_OBJ ? t.u.obj : 0;
}
int js_proxy_callable(js_object *o)
{
  js_value t = proxy_target(o);
  while (t.type == JV_OBJ && t.u.obj->kind == OBJ_PROXY)
    t = proxy_target(t.u.obj);
  return t.type == JV_OBJ && t.u.obj->kind == OBJ_FUNCTION;
}
/* the named trap function from the handler, or undefined (missing/revoked) */
static js_value proxy_trap(js_ctx *ctx, js_object *p, const char *name)
{
  js_value h = proxy_handler(p);
  if (h.type != JV_OBJ)
    return js_undef();
  return js_get(ctx, h, name);
}
static int proxy_live(js_ctx *ctx, js_object *p, js_value *target)
{
  js_value t = proxy_target(p);
  char probe;
  if (ctx->stack_limit && &probe < ctx->stack_limit) {
    js_throw_range(ctx, "maximum call stack size exceeded");
    return 0;
  }
  if (t.type != JV_OBJ) {
    js_throw_type(ctx, "Cannot perform operation on a proxy that has been revoked");
    return 0;
  }
  *target = t;
  return 1;
}
js_value js_proxy_get(js_ctx *ctx, js_object *p, const char *key, js_value receiver)
{
  js_value t, trap;
  if (!proxy_live(ctx, p, &t))
    return js_undef();
  trap = proxy_trap(ctx, p, "get");
  if (js_is_callable(trap)) {
    js_value a[3];
    a[0] = t;
    a[1] = js_mk_cstring(ctx, key);
    a[2] = receiver;
    return js_call(ctx, trap, proxy_handler(p), a, 3);
  }
  return js_get(ctx, t, key);
}
void js_proxy_set(js_ctx *ctx, js_object *p, const char *key, js_value val, js_value receiver)
{
  js_value t, trap;
  if (!proxy_live(ctx, p, &t))
    return;
  trap = proxy_trap(ctx, p, "set");
  if (js_is_callable(trap)) {
    js_value a[4];
    a[0] = t;
    a[1] = js_mk_cstring(ctx, key);
    a[2] = val;
    a[3] = receiver;
    js_call(ctx, trap, proxy_handler(p), a, 4);
    return;
  }
  js_set(ctx, t, key, val);
}
int js_proxy_has(js_ctx *ctx, js_object *p, const char *key)
{
  js_value t, trap;
  if (!proxy_live(ctx, p, &t))
    return 0;
  trap = proxy_trap(ctx, p, "has");
  if (js_is_callable(trap)) {
    js_value a[2];
    a[0] = t;
    a[1] = js_mk_cstring(ctx, key);
    return js_to_boolean(js_call(ctx, trap, proxy_handler(p), a, 2));
  }
  {
    js_object *o;
    for (o = t.u.obj; o; o = o->proto) {
      if (js_has_own(o, key))
        return 1;
      if (o->kind == OBJ_PROXY)
        return js_proxy_has(ctx, o, key);
    }
  }
  return 0;
}
int js_proxy_delete(js_ctx *ctx, js_object *p, const char *key)
{
  js_value t, trap;
  if (!proxy_live(ctx, p, &t))
    return 0;
  trap = proxy_trap(ctx, p, "deleteProperty");
  if (js_is_callable(trap)) {
    js_value a[2];
    a[0] = t;
    a[1] = js_mk_cstring(ctx, key);
    return js_to_boolean(js_call(ctx, trap, proxy_handler(p), a, 2));
  }
  return t.u.obj->kind == OBJ_PROXY ? js_proxy_delete(ctx, t.u.obj, key) : js_delete(t.u.obj, key);
}
js_value js_proxy_apply(js_ctx *ctx, js_object *p, js_value thisv, js_value *args, int argc)
{
  js_value t, trap;
  if (!proxy_live(ctx, p, &t))
    return js_undef();
  trap = proxy_trap(ctx, p, "apply");
  if (js_is_callable(trap)) {
    js_object *arr;
    js_value a[3];
    int i;
    gc_push_scope();
    gc_keep(p);
    gc_keep(t.u.obj);
    if (thisv.type == JV_OBJ)
      gc_keep(thisv.u.obj);
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    for (i = 0; i < argc; i++)
      js_array_push(ctx, arr, args[i]);
    a[0] = t;
    a[1] = thisv;
    a[2] = js_obj_value(arr);
    {
      js_value r = js_call(ctx, trap, proxy_handler(p), a, 3);
      gc_pop_scope();
      return r;
    }
  }
  return js_call(ctx, t, thisv, args, argc);
}
js_value js_proxy_construct(js_ctx *ctx, js_object *p, js_value *args, int argc)
{
  js_value t, trap;
  if (!proxy_live(ctx, p, &t))
    return js_undef();
  trap = proxy_trap(ctx, p, "construct");
  if (js_is_callable(trap)) {
    js_object *arr;
    js_value a[3];
    int i;
    gc_push_scope();
    gc_keep(p);
    gc_keep(t.u.obj);
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    for (i = 0; i < argc; i++)
      js_array_push(ctx, arr, args[i]);
    a[0] = t;
    a[1] = js_obj_value(arr);
    a[2] = js_obj_value(p); /* target, argsList, newTarget */
    {
      js_value r = js_call(ctx, trap, proxy_handler(p), a, 3);
      gc_pop_scope();
      return r;
    }
  }
  return js_construct(ctx, t, args, argc);
}
js_value js_proxy_ownkeys(js_ctx *ctx, js_object *p)
{
  js_value t, trap;
  js_object *arr, *o;
  if (!proxy_live(ctx, p, &t))
    return js_obj_value(js_new_array(ctx, 0));
  trap = proxy_trap(ctx, p, "ownKeys");
  if (js_is_callable(trap)) {
    js_value a[1];
    a[0] = t;
    return js_call(ctx, trap, proxy_handler(p), a, 1);
  }
  o = t.u.obj; /* default: the target's own string keys */
  gc_push_scope();
  gc_keep(o);
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (o->kind == OBJ_ARRAY) {
    unsigned e;
    char ib[24];
    for (e = 0; e < o->elen; e++) {
      u64toa((unsigned long long)e, ib, 10);
      js_array_push(ctx, arr, js_mk_cstring(ctx, ib));
    }
  }
  {
    int i;
    for (i = 0; i < o->props.n; i++) {
      pentry *pe = &o->props.e[i];
      if ((unsigned char)pe->key[0] >= 0x20)
        js_array_push(ctx, arr, js_mk_cstring(ctx, pe->key));
    }
  }
  gc_pop_scope();
  return js_obj_value(arr);
}

/* ------------------------------------------------------- strings ---------- */

int js_same_string(js_string *a, js_string *b)
{
  if (a == b)
    return 1;
  if (a->len != b->len || a->hash != b->hash)
    return 0;
  return memcmp_(a->data, b->data, a->len) == 0;
}

js_string *js_str_concat(js_ctx *ctx, js_string *a, js_string *b)
{
  int n = a->len + b->len;
  js_string *s;
  gc_push_scope();
  gc_keep(a);
  gc_keep(b);
  s = (js_string *)gc_alloc(sizeof(js_string), GCK_STRING);
  s->data = (char *)malloc((unsigned)n + 1);
  if (!s->data)
    gc_oom();
  memcpy(s->data, a->data, (unsigned)a->len);
  memcpy(s->data + a->len, b->data, (unsigned)b->len);
  s->data[n] = 0;
  s->len = n;
  s->hash = js_str_hash(s->data, n);
  gc_pop_scope();
  (void)ctx;
  return s;
}

/* ------------------------------------------------------- coercions -------- */

int js_to_boolean(js_value v)
{
  switch (v.type) {
  case JV_UNDEF:
  case JV_NULL:
    return 0;
  case JV_BOOL:
    return v.u.boolean;
  case JV_NUM:
    return !(v.u.num == 0.0 || v.u.num != v.u.num); /* 0/-0/NaN -> false */
  case JV_STR:
    return v.u.str->len != 0;
  case JV_BIGINT:
    return bi_to_bool(js_bigint_of(v));
  default:
    return 1;
  }
}

/* StrToNumber over a byte slice (whitespace-trimmed, 0x/0o/0b, Infinity). */
double js_str_to_num(const char *s, int len)
{
  const char *p = s, *end = s + len;
  int neg = 0;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v'))
    p++;
  while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r' || end[-1] == '\f' ||
                     end[-1] == '\v'))
    end--;
  if (p == end)
    return 0.0; /* empty / all-whitespace -> 0   */
  if (*p == '+' || *p == '-') {
    neg = (*p == '-');
    p++;
  }
  if (end - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    double v = 0;
    const char *q = p + 2;
    if (q == end)
      return 0.0 / 0.0;
    for (; q < end; q++) {
      int d;
      if (*q >= '0' && *q <= '9')
        d = *q - '0';
      else if (*q >= 'a' && *q <= 'f')
        d = *q - 'a' + 10;
      else if (*q >= 'A' && *q <= 'F')
        d = *q - 'A' + 10;
      else
        return 0.0 / 0.0;
      v = v * 16 + d;
    }
    return neg ? -v : v;
  }
  if (end - p == 8 && memcmp_(p, "Infinity", 8) == 0)
    return neg ? -1.0 / 0.0 : 1.0 / 0.0;
  {
    char buf[340];
    int n = (int)(end - p);
    char *eptr;
    double v;
    if (n <= 0 || n >= (int)sizeof(buf))
      return 0.0 / 0.0;
    memcpy(buf, p, (unsigned)n);
    buf[n] = 0;
    v = strtod(buf, &eptr);
    if (eptr != buf + n)
      return 0.0 / 0.0; /* trailing junk -> NaN          */
    return neg ? -v : v;
  }
}

double js_to_number(js_ctx *ctx, js_value v)
{
  switch (v.type) {
  case JV_UNDEF:
    return 0.0 / 0.0;
  case JV_NULL:
    return 0.0;
  case JV_BOOL:
    return v.u.boolean ? 1.0 : 0.0;
  case JV_NUM:
    return v.u.num;
  case JV_STR:
    return js_str_to_num(v.u.str->data, v.u.str->len);
  case JV_SYMBOL:
    js_throw_type(ctx, "cannot convert a Symbol value to a number");
    return 0.0 / 0.0;
  case JV_BIGINT:
    js_throw_type(ctx, "Cannot convert a BigInt value to a number");
    return 0.0 / 0.0;
  default: {
    js_value p = js_to_primitive(ctx, v, 1);
    if (p.type == JV_OBJ)
      return 0.0 / 0.0;
    return js_to_number(ctx, p);
  }
  }
}

js_value js_to_string(js_ctx *ctx, js_value v)
{
  char buf[DTOA_MAX];
  switch (v.type) {
  case JV_UNDEF:
    return js_mk_cstring(ctx, "undefined");
  case JV_NULL:
    return js_mk_cstring(ctx, "null");
  case JV_BOOL:
    return js_mk_cstring(ctx, v.u.boolean ? "true" : "false");
  case JV_NUM:
    dtoa(v.u.num, buf);
    return js_mk_cstring(ctx, buf);
  case JV_STR:
    return v;
  case JV_SYMBOL:
    js_throw_type(ctx, "cannot convert a Symbol value to a string");
    return js_undef();
  case JV_BIGINT: {
    char *s = bi_to_str(js_bigint_of(v), 10);
    js_value r;
    if (!s) {
      js_throw_range(ctx, "out of memory");
      return js_undef();
    }
    r = js_mk_cstring(ctx, s);
    free(s);
    return r;
  }
  default: {
    js_value p = js_to_primitive(ctx, v, 0);
    if (ctx->has_exc)
      return js_undef();
    if (p.type == JV_OBJ) {
      js_throw_type(ctx, "cannot convert object to string");
      return js_undef();
    }
    return js_to_string(ctx, p);
  }
  }
}

js_value js_to_primitive(js_ctx *ctx, js_value v, int number_hint)
{
  const char *first = number_hint ? "valueOf" : "toString";
  const char *second = number_hint ? "toString" : "valueOf";
  int i;
  if (v.type != JV_OBJ)
    return v;
  for (i = 0; i < 2; i++) {
    const char *name = i == 0 ? first : second;
    js_value m = js_get(ctx, v, name);
    if (ctx->has_exc)
      return js_undef();
    if (js_is_callable(m)) {
      js_value r = js_call(ctx, m, v, NULL, 0);
      if (ctx->has_exc)
        return js_undef();
      if (r.type != JV_OBJ)
        return r;
    }
  }
  js_throw_type(ctx, "cannot convert object to primitive value");
  return js_undef();
}

int js_to_int_sat(js_ctx *ctx, js_value v)
{
  double d = js_to_number(ctx, v);
  if (d != d)
    return 0;
  if (d >= 2147483647.0)
    return 2147483647;
  if (d <= -2147483648.0)
    return -2147483647 - 1;
  return (int)d;
}

int js_to_int32(js_ctx *ctx, js_value v)
{
  double n = js_to_number(ctx, v);
  double m;
  if (n != n || n == 1.0 / 0.0 || n == -1.0 / 0.0)
    return 0;
  m = fmod(trunc(n), 4294967296.0);
  if (m < 0)
    m += 4294967296.0;
  return (int)(unsigned)m;
}

unsigned js_to_uint32(js_ctx *ctx, js_value v)
{
  double n = js_to_number(ctx, v);
  double m;
  if (n != n || n == 1.0 / 0.0 || n == -1.0 / 0.0)
    return 0;
  m = fmod(trunc(n), 4294967296.0);
  if (m < 0)
    m += 4294967296.0;
  return (unsigned)m;
}

const char *js_typeof(js_value v)
{
  switch (v.type) {
  case JV_UNDEF:
    return "undefined";
  case JV_NULL:
    return "object";
  case JV_BOOL:
    return "boolean";
  case JV_NUM:
    return "number";
  case JV_STR:
    return "string";
  case JV_SYMBOL:
    return "symbol";
  case JV_BIGINT:
    return "bigint";
  default:
    return js_is_callable(v) ? "function" : "object";
  }
}

/* ------------------------------------------------------- equality --------- */

int js_strict_eq(js_value a, js_value b)
{
  if (a.type != b.type)
    return 0;
  switch (a.type) {
  case JV_UNDEF:
  case JV_NULL:
    return 1;
  case JV_BOOL:
    return a.u.boolean == b.u.boolean;
  case JV_NUM:
    return a.u.num == b.u.num; /* NaN!=NaN, +0==-0 fall out */
  case JV_STR:
    return js_same_string(a.u.str, b.u.str);
  case JV_BIGINT:
    return bi_cmp(js_bigint_of(a), js_bigint_of(b)) == 0;
  default:
    return a.u.obj == b.u.obj;
  }
}

/* ------------------------------------------------------- symbols ---------- */

static unsigned symbol_counter;

js_value js_mk_symbol(js_ctx *ctx, const char *desc)
{
  js_object *o;
  js_value v;
  char *ks;
  int n;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->symbol_proto));
  o->kind = OBJ_SYMBOL;
  /* unique key string: '\x01' 's' + base36 id (the control byte makes ordinary
   * enumeration skip symbol-keyed properties). */
  ks = (char *)malloc(48);
  if (!ks)
    gc_oom();
  ks[0] = 1;
  ks[1] = 's';
  n = u64toa((unsigned long long)__atomic_fetch_add(&symbol_counter, 1, __ATOMIC_RELAXED), ks + 2, 36);
  ks[2 + n] = 0;
  o->host = ks;
  js_def(o, "@desc", desc ? js_mk_cstring(ctx, desc) : js_undef(), 0);
  v.type = JV_SYMBOL;
  v.u.obj = o;
  /* Record it so Object.getOwnPropertySymbols can map a property's internal
   * key string back to its Symbol. (The registry is created in js_ctx_new
   * just before the first symbol; guard for the brief window before that.) */
  if (ctx->sym_registry)
    js_array_push(ctx, ctx->sym_registry, v);
  gc_pop_scope();
  return v;
}

const char *js_symbol_key(js_value sym)
{
  return (sym.type == JV_SYMBOL && sym.u.obj->host) ? (const char *)sym.u.obj->host : "";
}

int js_same_value_zero(js_value a, js_value b)
{
  if (a.type == JV_NUM && b.type == JV_NUM) {
    if (a.u.num != a.u.num && b.u.num != b.u.num)
      return 1;                /* NaN matches NaN */
    return a.u.num == b.u.num; /* +0 matches -0   */
  }
  return js_strict_eq(a, b);
}

/* ------------------------------------------------------- equality --------- */

/* exact BigInt == Number: true only when the number is the same integer value
 */
static int bigint_eq_double(const bigint *b, double d)
{
  int ok, eq;
  bigint *bd;
  if (d != d || d == 1.0 / 0.0 || d == -1.0 / 0.0 || floor(d) != d)
    return 0;
  bd = bi_from_double(d, &ok);
  if (!ok || !bd) {
    if (bd)
      bi_free(bd);
    return 0;
  }
  eq = bi_cmp(b, bd) == 0;
  bi_free(bd);
  return eq;
}

/* exact BigInt == String: parse the string as a BigInt, then compare */
static int bigint_eq_string(const bigint *b, js_string *s)
{
  int ok, eq;
  bigint *t = bi_from_str(s->data, s->len, &ok);
  if (!ok || !t) {
    if (t)
      bi_free(t);
    return 0;
  }
  eq = bi_cmp(b, t) == 0;
  bi_free(t);
  return eq;
}

int js_loose_eq(js_ctx *ctx, js_value a, js_value b)
{
  if (a.type == b.type)
    return js_strict_eq(a, b);
  if ((a.type == JV_NULL && b.type == JV_UNDEF) || (a.type == JV_UNDEF && b.type == JV_NULL))
    return 1;
  if (a.type == JV_NUM && b.type == JV_STR)
    return a.u.num == js_to_number(ctx, b);
  if (a.type == JV_STR && b.type == JV_NUM)
    return js_to_number(ctx, a) == b.u.num;
  if (a.type == JV_BIGINT && b.type == JV_NUM)
    return bigint_eq_double(js_bigint_of(a), b.u.num);
  if (a.type == JV_NUM && b.type == JV_BIGINT)
    return bigint_eq_double(js_bigint_of(b), a.u.num);
  if (a.type == JV_BIGINT && b.type == JV_STR)
    return bigint_eq_string(js_bigint_of(a), b.u.str);
  if (a.type == JV_STR && b.type == JV_BIGINT)
    return bigint_eq_string(js_bigint_of(b), a.u.str);
  if (a.type == JV_BOOL) {
    js_value na = js_number(a.u.boolean ? 1 : 0);
    return js_loose_eq(ctx, na, b);
  }
  if (b.type == JV_BOOL) {
    js_value nb = js_number(b.u.boolean ? 1 : 0);
    return js_loose_eq(ctx, a, nb);
  }
  if ((a.type == JV_NUM || a.type == JV_STR || a.type == JV_BIGINT) && b.type == JV_OBJ)
    return js_loose_eq(ctx, a, js_to_primitive(ctx, b, 0));
  if (a.type == JV_OBJ && (b.type == JV_NUM || b.type == JV_STR || b.type == JV_BIGINT))
    return js_loose_eq(ctx, js_to_primitive(ctx, a, 0), b);
  return 0;
}
