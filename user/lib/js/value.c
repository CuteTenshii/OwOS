/* value.c — runtime values, the object model, the GC glue, and coercions. */
#include "value.h"
#include "interp.h"
#include "ast.h"
#include "owos.h"
#include "fmt.h"
#include "math.h"
#include "gc.h"

/* ------------------------------------------------------- small helpers ---- */

static char *dupz(const char *s, int n) {
    char *d = (char *)malloc((unsigned)n + 1);
    if (!d) return NULL;
    memcpy(d, s, (unsigned)n);
    d[n] = 0;
    return d;
}

/* the libc here has no memcmp */
static int memcmp_(const void *a, const void *b, int n) {
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    int i;
    for (i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

unsigned js_str_hash(const char *s, int len) {
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

/* a canonical array index ("0","1",...,no leading zero) -> 1 and *out */
static int as_index(const char *key, unsigned *out) {
    const char *p = key;
    unsigned v = 0;
    if (!*p) return 0;
    if (*p == '0' && p[1]) return 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        if (v > 429496729u) return 0;       /* overflow guard                    */
        v = v * 10u + (unsigned)(*p - '0');
    }
    *out = v;
    return 1;
}

js_value js_undef(void)        { js_value v; v.type = JV_UNDEF; v.u.obj = NULL; return v; }
js_value js_null(void)         { js_value v; v.type = JV_NULL;  v.u.obj = NULL; return v; }
js_value js_bool(int b)        { js_value v; v.type = JV_BOOL;  v.u.boolean = !!b; return v; }
js_value js_number(double n)   { js_value v; v.type = JV_NUM;   v.u.num = n; return v; }
js_value js_obj_value(js_object *o) { js_value v; if (!o) return js_null(); v.type = JV_OBJ; v.u.obj = o; return v; }

/* ------------------------------------------------------- property map ----- */

static pentry *pmap_find(pmap *m, const char *key) {
    int i;
    for (i = 0; i < m->n; i++)
        if (strcmp(m->e[i].key, key) == 0) return &m->e[i];
    return NULL;
}

static pentry *pmap_insert(pmap *m, const char *key) {
    pentry *pe;
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 8;
        pentry *ne = (pentry *)realloc(m->e, (unsigned)nc * sizeof(pentry));
        if (!ne) return NULL;
        m->e = ne; m->cap = nc;
    }
    pe = &m->e[m->n++];
    pe->key = dupz(key, strlen(key));
    pe->value = js_undef();
    pe->getter = pe->setter = NULL;
    pe->flags = 0;
    return pe;
}

static void pmap_free(pmap *m) {
    int i;
    for (i = 0; i < m->n; i++) free(m->e[i].key);
    if (m->e) free(m->e);
    m->e = NULL; m->n = m->cap = 0;
}

/* ------------------------------------------------------- GC tracer -------- */

static void mark_value(js_value v) {
    if (v.type == JV_OBJ || v.type == JV_SYMBOL) gc_mark(v.u.obj);
    else if (v.type == JV_STR) gc_mark(v.u.str);
}

static void mark_pmap(pmap *m) {
    int i;
    for (i = 0; i < m->n; i++) {
        if (m->e[i].flags & P_ACCESSOR) { gc_mark(m->e[i].getter); gc_mark(m->e[i].setter); }
        else mark_value(m->e[i].value);
    }
}

static void trace(void *obj, int kind) {
    if (kind == GCK_STRING) return;             /* bytes are malloc'd, not GC    */
    if (kind == GCK_ENV) {
        js_env *e = (js_env *)obj;
        gc_mark(e->parent);
        mark_pmap(&e->vars);
        if (e->has_this) mark_value(e->thisv);
        gc_mark(e->home);
        return;
    }
    {
        js_object *o = (js_object *)obj;
        unsigned i;
        gc_mark(o->proto);
        mark_pmap(&o->props);
        for (i = 0; i < o->elen; i++) mark_value(o->elems[i]);
        if (o->fn) { gc_mark(o->fn->closure); gc_mark(o->fn->home); mark_value(o->fn->data); }
    }
}

static void finalize(void *obj, int kind) {
    if (kind == GCK_STRING) { js_string *s = (js_string *)obj; if (s->data) free(s->data); return; }
    if (kind == GCK_ENV)    { js_env *e = (js_env *)obj; pmap_free(&e->vars); return; }
    {
        js_object *o = (js_object *)obj;
        pmap_free(&o->props);
        if (o->elems) free(o->elems);
        if (o->fn) free(o->fn);
        if (o->kind == OBJ_SYMBOL && o->host) free(o->host);   /* the symbol key string */
    }
}

void js_gc_setup(void) { gc_init(trace, finalize); }

/* ------------------------------------------------------- creators --------- */

js_value js_mk_string(js_ctx *ctx, const char *s, int len) {
    js_string *str = (js_string *)gc_alloc(sizeof(js_string), GCK_STRING);
    js_value v;
    (void)ctx;
    str->data = dupz(s ? s : "", len);
    str->len = len;
    str->hash = js_str_hash(s ? s : "", len);
    v.type = JV_STR; v.u.str = str;
    return v;
}

js_value js_mk_cstring(js_ctx *ctx, const char *s) { return js_mk_string(ctx, s, s ? strlen(s) : 0); }

js_object *js_new_object(js_ctx *ctx, js_object *proto) {
    js_object *o = (js_object *)gc_alloc(sizeof(js_object), GCK_OBJECT);
    (void)ctx;
    o->kind = OBJ_PLAIN;
    o->extensible = 1;
    o->proto = proto;
    return o;
}

js_object *js_new_array(js_ctx *ctx, unsigned len) {
    js_object *o;
    gc_push_scope();
    o = (js_object *)gc_keep(gc_alloc(sizeof(js_object), GCK_OBJECT));
    o->kind = OBJ_ARRAY;
    o->extensible = 1;
    o->proto = ctx->array_proto;
    if (len) {
        unsigned i;
        o->elems = (js_value *)malloc(len * sizeof(js_value));
        o->ecap = len; o->elen = len;
        for (i = 0; i < len; i++) o->elems[i] = js_undef();
    }
    gc_pop_scope();
    return o;
}

js_object *js_new_function(js_ctx *ctx, js_node *node, js_env *closure) {
    js_object *o;
    js_func *f;
    gc_push_scope();
    o = (js_object *)gc_keep(gc_alloc(sizeof(js_object), GCK_OBJECT));
    o->kind = OBJ_FUNCTION;
    o->extensible = 1;
    o->proto = ctx->function_proto;
    f = (js_func *)malloc(sizeof(js_func));
    memset(f, 0, sizeof(js_func));
    f->node = node;
    f->closure = closure;
    f->is_arrow = node ? (unsigned char)node->u.func.is_arrow : 0;
    f->name = node && node->u.func.name ? node->u.func.name : "";
    o->fn = f;
    /* non-arrow functions get a fresh .prototype object with a back-ref */
    if (!f->is_arrow) {
        js_object *proto = js_new_object(ctx, ctx->object_proto);
        js_def(proto, "constructor", js_obj_value(o), P_WRITABLE | P_CONFIGURABLE);
        js_def(o, "prototype", js_obj_value(proto), P_WRITABLE);
    }
    js_def(o, "name", js_mk_cstring(ctx, f->name), P_CONFIGURABLE);
    gc_pop_scope();
    return o;
}

js_object *js_new_native(js_ctx *ctx, js_native fn, const char *name, int arity) {
    js_object *o;
    js_func *f;
    gc_push_scope();
    o = (js_object *)gc_keep(gc_alloc(sizeof(js_object), GCK_OBJECT));
    o->kind = OBJ_FUNCTION;
    o->extensible = 1;
    o->proto = ctx->function_proto;
    f = (js_func *)malloc(sizeof(js_func));
    memset(f, 0, sizeof(js_func));
    f->native = fn;
    f->name = name ? name : "";
    o->fn = f;
    js_def(o, "name", js_mk_cstring(ctx, f->name), P_CONFIGURABLE);
    js_def(o, "length", js_number(arity), P_CONFIGURABLE);
    gc_pop_scope();
    return o;
}

js_env *js_new_env(js_ctx *ctx, js_env *parent, int is_fn_scope) {
    js_env *e = (js_env *)gc_alloc(sizeof(js_env), GCK_ENV);
    (void)ctx;
    e->parent = parent;
    e->is_fn_scope = (unsigned char)is_fn_scope;
    e->thisv = js_undef();
    return e;
}

pentry *js_env_lookup(js_env *e, const char *name) {
    for (; e; e = e->parent) {
        pentry *pe = pmap_find(&e->vars, name);
        if (pe) return pe;
    }
    return NULL;
}

void js_env_define(js_env *e, const char *name, js_value v, unsigned char flags) {
    pentry *pe = pmap_find(&e->vars, name);
    if (!pe) pe = pmap_insert(&e->vars, name);
    if (!pe) return;
    pe->value = v;
    pe->getter = pe->setter = NULL;
    pe->flags = flags;
}

js_value js_env_get_this(js_env *e) {
    for (; e; e = e->parent) if (e->has_this) return e->thisv;
    return js_undef();
}

js_object *js_env_get_home(js_env *e) {
    for (; e; e = e->parent) if (e->has_this) return e->home;
    return NULL;
}

/* ------------------------------------------------------- properties ------- */

void js_def(js_object *o, const char *key, js_value val, unsigned char flags) {
    pentry *pe;
    if ((unsigned char)key[0] < 0x20) flags &= (unsigned char)~P_ENUMERABLE;   /* symbol-keyed: never enumerable */
    pe = pmap_find(&o->props, key);
    if (!pe) pe = pmap_insert(&o->props, key);
    if (!pe) return;
    pe->value = val;
    pe->getter = pe->setter = NULL;
    pe->flags = flags;
}

void js_def_accessor(js_object *o, const char *key, js_object *get, js_object *set, unsigned char flags) {
    pentry *pe = pmap_find(&o->props, key);
    if (!pe) pe = pmap_insert(&o->props, key);
    if (!pe) return;
    pe->value = js_undef();
    pe->getter = get; pe->setter = set;
    pe->flags = (unsigned char)(flags | P_ACCESSOR);
}

int js_has_own(js_object *o, const char *key) {
    unsigned idx;
    if (o->kind == OBJ_ARRAY) {
        if (strcmp(key, "length") == 0) return 1;
        if (as_index(key, &idx)) return idx < o->elen && o->elems[idx].type != JV_UNDEF;
    }
    return pmap_find(&o->props, key) != NULL;
}

int js_delete(js_object *o, const char *key) {
    pmap *m = &o->props;
    int i;
    unsigned idx;
    if (o->kind == OBJ_ARRAY && as_index(key, &idx) && idx < o->elen) { o->elems[idx] = js_undef(); return 1; }
    for (i = 0; i < m->n; i++)
        if (strcmp(m->e[i].key, key) == 0) {
            free(m->e[i].key);
            for (; i < m->n - 1; i++) m->e[i] = m->e[i + 1];
            m->n--;
            return 1;
        }
    return 1;
}

/* public shims over the static property map, used by interp.c / builtins.c */
pentry *pmap_find_pub(js_object *o, const char *key) { return pmap_find(&o->props, key); }
int pmap_find_in_env(js_env *e, const char *name) { return pmap_find(&e->vars, name) != NULL; }

unsigned js_array_len(js_object *arr) { return arr->elen; }

void js_array_set_len(js_ctx *ctx, js_object *arr, unsigned len) {
    (void)ctx;
    if (len <= arr->elen) { arr->elen = len; return; }
    if (len > arr->ecap) {
        unsigned nc = arr->ecap ? arr->ecap * 2 : 8;
        while (nc < len) nc *= 2;
        arr->elems = (js_value *)realloc(arr->elems, nc * sizeof(js_value));
        arr->ecap = nc;
    }
    while (arr->elen < len) arr->elems[arr->elen++] = js_undef();
}

void js_array_push(js_ctx *ctx, js_object *arr, js_value v) {
    gc_push_scope();
    if (v.type == JV_OBJ) gc_keep(v.u.obj); else if (v.type == JV_STR) gc_keep(v.u.str);
    if (arr->elen == arr->ecap) {
        unsigned nc = arr->ecap ? arr->ecap * 2 : 8;
        arr->elems = (js_value *)realloc(arr->elems, nc * sizeof(js_value));
        arr->ecap = nc;
    }
    arr->elems[arr->elen++] = v;
    gc_pop_scope();
    (void)ctx;
}

void js_set_index(js_ctx *ctx, js_value obj, unsigned i, js_value val) {
    if (obj.type == JV_OBJ && obj.u.obj->kind == OBJ_ARRAY) {
        js_object *a = obj.u.obj;
        if (i >= a->elen) js_array_set_len(ctx, a, i + 1);
        a->elems[i] = val;
        return;
    }
    {
        char buf[24];
        u64toa((unsigned long long)i, buf, 10);
        js_set(ctx, obj, buf, val);
    }
}

js_value js_get_index(js_ctx *ctx, js_value obj, unsigned i) {
    if (obj.type == JV_OBJ && obj.u.obj->kind == OBJ_ARRAY) {
        js_object *a = obj.u.obj;
        if (i < a->elen) return a->elems[i];
        return js_undef();
    }
    if (obj.type == JV_STR) {
        if ((int)i < obj.u.str->len) return js_mk_string(ctx, obj.u.str->data + i, 1);
        return js_undef();
    }
    {
        char buf[24];
        u64toa((unsigned long long)i, buf, 10);
        return js_get(ctx, obj, buf);
    }
}

static js_object *proto_for_primitive(js_ctx *ctx, js_value v) {
    if (v.type == JV_STR)    return ctx->string_proto;
    if (v.type == JV_NUM)    return ctx->number_proto;
    if (v.type == JV_BOOL)   return ctx->boolean_proto;
    if (v.type == JV_SYMBOL) return ctx->symbol_proto;
    return NULL;
}

js_value js_get(js_ctx *ctx, js_value obj, const char *key) {
    js_object *o;
    unsigned idx;

    if (obj.type == JV_STR) {
        if (strcmp(key, "length") == 0) return js_number(obj.u.str->len);
        if (as_index(key, &idx)) return js_get_index(ctx, obj, idx);
        o = ctx->string_proto;
    } else if (obj.type == JV_NUM || obj.type == JV_BOOL || obj.type == JV_SYMBOL) {
        o = proto_for_primitive(ctx, obj);
    } else if (obj.type == JV_OBJ) {
        o = obj.u.obj;
        if (o->kind == OBJ_ARRAY) {
            if (strcmp(key, "length") == 0) return js_number(o->elen);
            if (as_index(key, &idx)) return js_get_index(ctx, obj, idx);
        }
    } else {
        char msg[160]; int i = 0; const char *p;
        for (p = "cannot read property '"; *p && i < 120; p++) msg[i++] = *p;
        for (p = key; *p && i < 140; p++) msg[i++] = *p;
        for (p = "' of "; *p; p++) msg[i++] = *p;
        for (p = obj.type == JV_NULL ? "null" : "undefined"; *p; p++) msg[i++] = *p;
        msg[i] = '\0';
        return js_throw_type(ctx, msg);
    }

    for (; o; o = o->proto) {
        pentry *pe = pmap_find(&o->props, key);
        if (pe) {
            if (pe->flags & P_ACCESSOR) {
                if (!pe->getter) return js_undef();
                return js_call(ctx, js_obj_value(pe->getter), obj, NULL, 0);
            }
            return pe->value;
        }
    }
    return js_undef();
}

void js_set(js_ctx *ctx, js_value obj, const char *key, js_value val) {
    js_object *o, *p;
    unsigned idx;

    if (obj.type != JV_OBJ) return;             /* set on a primitive is a no-op */
    o = obj.u.obj;

    if (o->kind == OBJ_ARRAY) {
        if (strcmp(key, "length") == 0) { js_array_set_len(ctx, o, js_to_uint32(ctx, val)); return; }
        if (as_index(key, &idx)) { js_set_index(ctx, obj, idx, val); return; }
    }

    /* own data property: update in place */
    {
        pentry *pe = pmap_find(&o->props, key);
        if (pe) {
            if (pe->flags & P_ACCESSOR) { if (pe->setter) js_call(ctx, js_obj_value(pe->setter), obj, &val, 1); return; }
            if (pe->flags & P_WRITABLE) pe->value = val;
            return;
        }
    }
    /* inherited accessor with a setter */
    for (p = o->proto; p; p = p->proto) {
        pentry *pe = pmap_find(&p->props, key);
        if (pe) { if (pe->flags & P_ACCESSOR) { if (pe->setter) js_call(ctx, js_obj_value(pe->setter), obj, &val, 1); return; } break; }
    }
    /* define a new own property */
    if (o->extensible) js_def(o, key, val, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
}

/* ------------------------------------------------------- strings ---------- */

int js_same_string(js_string *a, js_string *b) {
    if (a == b) return 1;
    if (a->len != b->len || a->hash != b->hash) return 0;
    return memcmp_(a->data, b->data, a->len) == 0;
}

js_string *js_str_concat(js_ctx *ctx, js_string *a, js_string *b) {
    int n = a->len + b->len;
    js_string *s;
    gc_push_scope();
    gc_keep(a); gc_keep(b);
    s = (js_string *)gc_alloc(sizeof(js_string), GCK_STRING);
    s->data = (char *)malloc((unsigned)n + 1);
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

int js_to_boolean(js_value v) {
    switch (v.type) {
    case JV_UNDEF: case JV_NULL: return 0;
    case JV_BOOL:  return v.u.boolean;
    case JV_NUM:   return !(v.u.num == 0.0 || v.u.num != v.u.num);   /* 0/-0/NaN -> false */
    case JV_STR:   return v.u.str->len != 0;
    default:       return 1;
    }
}

/* StrToNumber over a byte slice (whitespace-trimmed, 0x/0o/0b, Infinity). */
double js_str_to_num(const char *s, int len) {
    const char *p = s, *end = s + len;
    int neg = 0;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r' || end[-1] == '\f' || end[-1] == '\v')) end--;
    if (p == end) return 0.0;                    /* empty / all-whitespace -> 0   */
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if (end - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        double v = 0; const char *q = p + 2;
        if (q == end) return 0.0 / 0.0;
        for (; q < end; q++) {
            int d;
            if (*q >= '0' && *q <= '9') d = *q - '0';
            else if (*q >= 'a' && *q <= 'f') d = *q - 'a' + 10;
            else if (*q >= 'A' && *q <= 'F') d = *q - 'A' + 10;
            else return 0.0 / 0.0;
            v = v * 16 + d;
        }
        return neg ? -v : v;
    }
    if (end - p == 8 && memcmp_(p, "Infinity", 8) == 0) return neg ? -1.0 / 0.0 : 1.0 / 0.0;
    {
        char buf[340];
        int n = (int)(end - p);
        char *eptr;
        double v;
        if (n <= 0 || n >= (int)sizeof(buf)) return 0.0 / 0.0;
        memcpy(buf, p, (unsigned)n); buf[n] = 0;
        v = strtod(buf, &eptr);
        if (eptr != buf + n) return 0.0 / 0.0;   /* trailing junk -> NaN          */
        return neg ? -v : v;
    }
}

double js_to_number(js_ctx *ctx, js_value v) {
    switch (v.type) {
    case JV_UNDEF: return 0.0 / 0.0;
    case JV_NULL:  return 0.0;
    case JV_BOOL:  return v.u.boolean ? 1.0 : 0.0;
    case JV_NUM:   return v.u.num;
    case JV_STR:   return js_str_to_num(v.u.str->data, v.u.str->len);
    case JV_SYMBOL: js_throw_type(ctx, "cannot convert a Symbol value to a number"); return 0.0 / 0.0;
    default: {
        js_value p = js_to_primitive(ctx, v, 1);
        if (p.type == JV_OBJ) return 0.0 / 0.0;
        return js_to_number(ctx, p);
    }
    }
}

js_value js_to_string(js_ctx *ctx, js_value v) {
    char buf[DTOA_MAX];
    switch (v.type) {
    case JV_UNDEF: return js_mk_cstring(ctx, "undefined");
    case JV_NULL:  return js_mk_cstring(ctx, "null");
    case JV_BOOL:  return js_mk_cstring(ctx, v.u.boolean ? "true" : "false");
    case JV_NUM:   dtoa(v.u.num, buf); return js_mk_cstring(ctx, buf);
    case JV_STR:   return v;
    case JV_SYMBOL: js_throw_type(ctx, "cannot convert a Symbol value to a string"); return js_undef();
    default: {
        js_value p = js_to_primitive(ctx, v, 0);
        if (ctx->has_exc) return js_undef();
        if (p.type == JV_OBJ) { js_throw_type(ctx, "cannot convert object to string"); return js_undef(); }
        return js_to_string(ctx, p);
    }
    }
}

js_value js_to_primitive(js_ctx *ctx, js_value v, int number_hint) {
    const char *first = number_hint ? "valueOf" : "toString";
    const char *second = number_hint ? "toString" : "valueOf";
    int i;
    if (v.type != JV_OBJ) return v;
    for (i = 0; i < 2; i++) {
        const char *name = i == 0 ? first : second;
        js_value m = js_get(ctx, v, name);
        if (ctx->has_exc) return js_undef();
        if (js_is_callable(m)) {
            js_value r = js_call(ctx, m, v, NULL, 0);
            if (ctx->has_exc) return js_undef();
            if (r.type != JV_OBJ) return r;
        }
    }
    js_throw_type(ctx, "cannot convert object to primitive value");
    return js_undef();
}

int js_to_int32(js_ctx *ctx, js_value v) {
    double n = js_to_number(ctx, v);
    double m;
    if (n != n || n == 1.0 / 0.0 || n == -1.0 / 0.0) return 0;
    m = fmod(trunc(n), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int)(unsigned)m;
}

unsigned js_to_uint32(js_ctx *ctx, js_value v) {
    double n = js_to_number(ctx, v);
    double m;
    if (n != n || n == 1.0 / 0.0 || n == -1.0 / 0.0) return 0;
    m = fmod(trunc(n), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (unsigned)m;
}

const char *js_typeof(js_value v) {
    switch (v.type) {
    case JV_UNDEF: return "undefined";
    case JV_NULL:  return "object";
    case JV_BOOL:  return "boolean";
    case JV_NUM:   return "number";
    case JV_STR:   return "string";
    case JV_SYMBOL: return "symbol";
    default:       return v.u.obj->kind == OBJ_FUNCTION ? "function" : "object";
    }
}

/* ------------------------------------------------------- equality --------- */

int js_strict_eq(js_value a, js_value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
    case JV_UNDEF: case JV_NULL: return 1;
    case JV_BOOL:  return a.u.boolean == b.u.boolean;
    case JV_NUM:   return a.u.num == b.u.num;       /* NaN!=NaN, +0==-0 fall out */
    case JV_STR:   return js_same_string(a.u.str, b.u.str);
    default:       return a.u.obj == b.u.obj;
    }
}

/* ------------------------------------------------------- symbols ---------- */

js_value js_mk_symbol(js_ctx *ctx, const char *desc) {
    static unsigned counter = 0;
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
    ks[0] = 1; ks[1] = 's';
    n = u64toa((unsigned long long)counter++, ks + 2, 36);
    ks[2 + n] = 0;
    o->host = ks;
    js_def(o, "@desc", desc ? js_mk_cstring(ctx, desc) : js_undef(), 0);
    gc_pop_scope();
    v.type = JV_SYMBOL; v.u.obj = o;
    return v;
}

const char *js_symbol_key(js_value sym) {
    return (sym.type == JV_SYMBOL && sym.u.obj->host) ? (const char *)sym.u.obj->host : "";
}

int js_same_value_zero(js_value a, js_value b) {
    if (a.type == JV_NUM && b.type == JV_NUM) {
        if (a.u.num != a.u.num && b.u.num != b.u.num) return 1;   /* NaN matches NaN */
        return a.u.num == b.u.num;                                /* +0 matches -0   */
    }
    return js_strict_eq(a, b);
}

/* ------------------------------------------------------- equality --------- */

int js_loose_eq(js_ctx *ctx, js_value a, js_value b) {
    if (a.type == b.type) return js_strict_eq(a, b);
    if ((a.type == JV_NULL && b.type == JV_UNDEF) || (a.type == JV_UNDEF && b.type == JV_NULL)) return 1;
    if (a.type == JV_NUM && b.type == JV_STR) return a.u.num == js_to_number(ctx, b);
    if (a.type == JV_STR && b.type == JV_NUM) return js_to_number(ctx, a) == b.u.num;
    if (a.type == JV_BOOL) { js_value na = js_number(a.u.boolean ? 1 : 0); return js_loose_eq(ctx, na, b); }
    if (b.type == JV_BOOL) { js_value nb = js_number(b.u.boolean ? 1 : 0); return js_loose_eq(ctx, a, nb); }
    if ((a.type == JV_NUM || a.type == JV_STR) && b.type == JV_OBJ) return js_loose_eq(ctx, a, js_to_primitive(ctx, b, 0));
    if (a.type == JV_OBJ && (b.type == JV_NUM || b.type == JV_STR)) return js_loose_eq(ctx, js_to_primitive(ctx, a, 0), b);
    return 0;
}
