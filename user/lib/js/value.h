/* value.h — the runtime value and object model for the OwOS JS engine.
 *
 * A js_value is a small tagged struct passed by value. The reference kinds
 * (string, object) hold a pointer to a GC-managed cell; the rest are immediate.
 * Object/string/environment *headers* are owned by the precise mark-sweep GC
 * (user/lib/gc); their variable-size internals (property tables, array element
 * vectors, string bytes) are plain malloc'd buffers that the GC finalizer
 * frees. Because of that, the ONLY points at which a collection can happen are
 * the js_new_* allocators — see interp.c for the rooting discipline that relies
 * on it.
 */
#ifndef OWOS_JS_VALUE_H
#define OWOS_JS_VALUE_H

#ifndef NULL
#define NULL ((void *)0)
#endif

#include "bigint.h" /* the bignum behind the JV_BIGINT primitive    */

struct js_node;               /* ast.h — function bodies are AST nodes        */
typedef struct js_ctx js_ctx; /* interp.h — the engine context                */

typedef struct js_object js_object;
typedef struct js_string js_string;
typedef struct js_env js_env;

typedef enum { JV_UNDEF, JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_OBJ, JV_SYMBOL, JV_BIGINT } js_type;

typedef struct {
  js_type type;
  union {
    int boolean;
    double num;
    js_string *str;
    js_object *obj;
  } u;
} js_value;

/* object "kind" — refines a js_object */
enum {
  OBJ_PLAIN,
  OBJ_ARRAY,
  OBJ_FUNCTION,
  OBJ_ERROR,
  OBJ_SYMBOL,
  OBJ_ARRAYBUFFER,
  OBJ_TYPEDARRAY,
  OBJ_BIGINT,
  OBJ_PROXY,
  OBJ_HOST /* embedder object: `host` is freed through js_host_finalizer */
};

/* typed-array element kinds (index into ta_elem_size). DataView uses type -1.
 */
enum { TA_I8, TA_U8, TA_U8C, TA_I16, TA_U16, TA_I32, TA_U32, TA_F32, TA_F64, TA_I64, TA_U64, TA_F16, TA_NKINDS };
#define TA_DATAVIEW (-1)

/* OBJ_ARRAYBUFFER host: the raw backing bytes. OBJ_TYPEDARRAY host: a view onto
 * an ArrayBuffer (DataView is a view with type == TA_DATAVIEW; `length` is its
 * element count, or byteLength for a DataView). */
typedef struct {
  unsigned char *data;
  int byteLength;
} js_arraybuffer;
typedef struct {
  js_object *buffer;
  int type, offset, length;
} js_typedarray;

/* Called when an OBJ_HOST object is collected; release o->host here. */
extern void (*js_host_finalizer)(js_object *o);

/* property entry flags */
enum { P_ENUMERABLE = 1, P_WRITABLE = 2, P_CONFIGURABLE = 4, P_ACCESSOR = 8 };

typedef struct {
  char *key;         /* malloc'd NUL-terminated property name         */
  unsigned hash;     /* cached hash of key                              */
  js_value value;    /* data value (when !P_ACCESSOR)                 */
  js_object *getter; /* accessor get (or NULL)                        */
  js_object *setter; /* accessor set (or NULL)                        */
  unsigned char flags;
} pentry;

typedef struct {
  pentry *e; /* malloc'd, insertion-ordered                   */
  int n, cap;
  /* Optional open-addressing hash index over `e`, built lazily once the map
   * grows past a small threshold so tiny scopes/objects (the common case) pay
   * nothing. Buckets hold (entry index + 1); 0 means empty. Lookups fall back
   * to a linear scan of `e` while idx is NULL. */
  int *idx;
  int idxcap; /* bucket count (power of two), 0 = no index      */
} pmap;

struct js_string {
  char *data; /* malloc'd UTF-8 bytes (NUL-terminated)         */
  int len;    /* byte length                                   */
  unsigned hash;
};

/* C function backing a native callable */
typedef js_value (*js_native)(js_ctx *ctx, js_value thisv, js_value *args, int argc);

typedef struct {
  struct js_node *node;       /* user function AST (JS_FUNC), or NULL          */
  js_env *closure;            /* defining environment (user functions)         */
  js_native native;           /* native implementation, or NULL                */
  const char *name;           /* for diagnostics / .name                       */
  js_object *home;            /* [[HomeObject]]: the proto a method lives on,
                               * so `super` resolves to home->proto. NULL=none. */
  struct js_node *class_node; /* set on a class constructor (JS_CLASS), so the
                               * instance fields initialise per construction. */
  js_value data;              /* bound value for native closures (e.g. a Promise
                               * resolver knows which promise it settles)       */
  unsigned char is_arrow;     /* arrows take `this` (and home) lexically        */
} js_func;

struct js_object {
  unsigned char kind;
  unsigned char extensible;
  js_object *proto; /* [[Prototype]]                                 */
  pmap props;
  js_value *elems; /* OBJ_ARRAY: malloc'd dense element vector       */
  unsigned elen, ecap;
  js_func *fn; /* OBJ_FUNCTION: malloc'd function record         */
  void *host;  /* embedder hook: a non-GC native pointer (e.g. a
                * DOM node behind a wrapper). The GC ignores it. */
};

struct js_env {
  js_env *parent;
  pmap vars;
  js_value thisv;            /* `this` for this activation (when has_this)     */
  js_object *home;           /* [[HomeObject]] for `super`, set with has_this   */
  unsigned char has_this;    /* a function scope binds `this`; blocks/arrows don't */
  unsigned char is_fn_scope; /* target scope for `var` hoisting */
};

/* GC object kinds passed to gc_alloc. GCK_ARRAY is traced/finalized exactly
 * like GCK_OBJECT (it is a js_object), but the collector treats it as the
 * "rescan kind": an old array's dense `elems` are re-scanned every minor GC, so
 * element stores (scattered across the Array builtins) need no write barrier.
 */
enum { GCK_OBJECT, GCK_STRING, GCK_ENV, GCK_ARRAY };

/* ---- construction ------------------------------------------------------- */

void js_gc_setup(void); /* install the tracer/finalizer */

js_value js_undef(void);
js_value js_null(void);
js_value js_bool(int b);
js_value js_number(double n);
js_value js_mk_string(js_ctx *ctx, const char *s, int len); /* copies bytes  */
js_value js_mk_cstring(js_ctx *ctx, const char *s);         /* strlen-sized   */

/* the JV_BIGINT primitive: wrap a bignum (ownership transfers), or read it back
 */
js_value js_mk_bigint(js_ctx *ctx, bigint *b);
bigint *js_bigint_of(js_value v);

js_object *js_new_object(js_ctx *ctx, js_object *proto);
js_object *js_new_array(js_ctx *ctx, unsigned len);

/* a unique Symbol primitive (JV_SYMBOL). `js_symbol_key` returns the internal
 * property-key string the symbol maps to (control-char-prefixed so ordinary
 * enumeration skips it). */
js_value js_mk_symbol(js_ctx *ctx, const char *desc);
const char *js_symbol_key(js_value sym);
int js_same_value_zero(js_value a, js_value b); /* Map/Set key equality */
js_object *js_new_function(js_ctx *ctx, struct js_node *node, js_env *closure);
js_object *js_new_native(js_ctx *ctx, js_native fn, const char *name, int arity);
js_value js_obj_value(js_object *o);

js_env *js_new_env(js_ctx *ctx, js_env *parent, int is_fn_scope);

/* environment (lexical scope) variable access */
pentry *js_env_lookup(js_env *e, const char *name); /* walk the parent chain */
void js_env_define(js_env *e, const char *name, js_value v, unsigned char flags);
js_value js_env_get_this(js_env *e);   /* nearest bound `this`  */
js_object *js_env_get_home(js_env *e); /* nearest [[HomeObject]] */

/* ---- predicates --------------------------------------------------------- */

static inline int js_is_undef(js_value v) { return v.type == JV_UNDEF; }
int js_proxy_callable(js_object *o); /* a proxy is callable iff its target is */
static inline int js_is_null(js_value v) { return v.type == JV_NULL; }
static inline int js_is_object(js_value v) { return v.type == JV_OBJ; }
static inline int js_is_callable(js_value v)
{
  return v.type == JV_OBJ &&
         (v.u.obj->kind == OBJ_FUNCTION || (v.u.obj->kind == OBJ_PROXY && js_proxy_callable(v.u.obj)));
}
static inline int js_is_array(js_value v) { return v.type == JV_OBJ && v.u.obj->kind == OBJ_ARRAY; }

/* Proxy dispatch — traps in the handler, falling back to the target. Internals
 * live in own props @t (target) / @h (handler), read via direct pmap lookup so
 * dispatch never recurses through js_get on the proxy itself. */
int js_is_proxy(js_object *o);
js_value js_proxy_get(js_ctx *ctx, js_object *p, const char *key, js_value receiver);
void js_proxy_set(js_ctx *ctx, js_object *p, const char *key, js_value val, js_value receiver);
int js_proxy_has(js_ctx *ctx, js_object *p, const char *key);
int js_proxy_delete(js_ctx *ctx, js_object *p, const char *key);
js_value js_proxy_apply(js_ctx *ctx, js_object *p, js_value thisv, js_value *args, int argc);
js_value js_proxy_construct(js_ctx *ctx, js_object *p, js_value *args, int argc);
js_object *js_proxy_ownkeys_target(js_object *p);     /* target for enumeration fallbacks */
js_value js_proxy_ownkeys(js_ctx *ctx, js_object *p); /* string keys (ownKeys trap or target) */

/* ---- properties --------------------------------------------------------- */

/* [[Get]]/[[Set]] walking the prototype chain; key is a NUL-terminated name.
 * js_get returns undefined for a missing property. js_set/define create/update
 * an own property. Accessor properties invoke their getter/setter via ctx. */
js_value js_get(js_ctx *ctx, js_value obj, const char *key);
void js_set(js_ctx *ctx, js_value obj, const char *key, js_value val);
void js_def(js_object *o, const char *key, js_value val, unsigned char flags);
void js_def_accessor(js_object *o, const char *key, js_object *get, js_object *set, unsigned char flags);
int js_has_own(js_object *o, const char *key);
int js_delete(js_object *o, const char *key);
pentry *pmap_find_pub(js_object *o, const char *key);         /* own property entry, or NULL */
int pmap_index(pmap *m, const char *key);                     /* entry index, or -1 (ident cache) */
int pmap_index_hash(pmap *m, const char *key, unsigned hash); /* cached-key lookup */

/* indexed access (arrays + string chars + generic) */
js_value js_get_index(js_ctx *ctx, js_value obj, unsigned i);
void js_set_index(js_ctx *ctx, js_value obj, unsigned i, js_value val);
void js_array_push(js_ctx *ctx, js_object *arr, js_value v);
unsigned js_array_len(js_object *arr);
void js_array_set_len(js_ctx *ctx, js_object *arr, unsigned len);

/* typed-array element access (the backing-byte read/write behind ta[i]) */
int ta_elem_size(int type);
js_value ta_get(js_ctx *ctx, js_object *view, unsigned idx); /* number, BigInt, or undefined */
void ta_set(js_ctx *ctx, js_object *view, unsigned idx, js_value v);

/* 64-bit integer <-> BigInt (lo/hi 32-bit halves) and IEEE binary16 <-> double */
js_value js_bigint_from_u64(js_ctx *ctx, unsigned lo, unsigned hi, int is_signed);
int js_bigint_to_u64(js_ctx *ctx, js_value v, unsigned *lo, unsigned *hi); /* 0 + throws if not a BigInt */
unsigned js_f16_encode(double d);
double js_f16_decode(unsigned h);

/* ---- coercions (ECMAScript abstract operations) ------------------------- */

int js_to_boolean(js_value v);
double js_to_number(js_ctx *ctx, js_value v);
js_value js_to_string(js_ctx *ctx, js_value v); /* -> JV_STR                  */
js_value js_to_primitive(js_ctx *ctx, js_value v, int number_hint);
int js_to_int32(js_ctx *ctx, js_value v);
int js_to_int_sat(js_ctx *ctx, js_value v); /* ToNumber then saturate to int; NaN -> 0 */
unsigned js_to_uint32(js_ctx *ctx, js_value v);
const char *js_typeof(js_value v);

/* equality */
int js_strict_eq(js_value a, js_value b);
int js_loose_eq(js_ctx *ctx, js_value a, js_value b);
int js_same_string(js_string *a, js_string *b);

/* small helpers shared with the interpreter / builtins */
js_string *js_str_concat(js_ctx *ctx, js_string *a, js_string *b);
double js_str_to_num(const char *s, int len);
unsigned js_str_hash(const char *s, int len);

#endif /* OWOS_JS_VALUE_H */
