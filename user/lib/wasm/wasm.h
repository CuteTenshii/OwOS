/* wasm.h: a WebAssembly engine: binary decoder, validator, bytecode compiler and interpreter.
 *
 * Independent of the JS engine. Supported: the MVP plus sign extension, saturating float-to-int, multi-value,
 * bulk memory, mutable globals, reference types (funcref and externref, several tables) and 128-bit SIMD.
 * Not supported: threads/shared memory, exceptions, tail calls, memory64, multiple memories, relaxed SIMD.
 *
 * Values live in 64-bit stack slots (a v128 takes two). funcref is a wasm_func pointer and externref an
 * interned handle (0 = null); the embedder maps handles to its own values through the wasm_exec callbacks.
 *
 *   wasm_module *m = wasm_module_new(bytes, len, err, sizeof err);
 *   wasm_instance *i = wasm_instance_new(m);
 *   ... fill i->funcs/tables/mems/globals[0 .. imports) ...
 *   wasm_instance_define(i);          own functions, tables, memories, globals, segments
 *   wasm_instance_apply_segments(i);  active element and data segments
 *   wasm_call(exec, i->funcs[n], args, results);
 */
#ifndef OWOS_WASM_H
#define OWOS_WASM_H

#include <stddef.h>
#include <stdint.h>

enum {
  WASM_I32 = 0x7f,
  WASM_I64 = 0x7e,
  WASM_F32 = 0x7d,
  WASM_F64 = 0x7c,
  WASM_V128 = 0x7b,
  WASM_FUNCREF = 0x70,
  WASM_EXTERNREF = 0x6f
};
enum { WASM_KIND_FUNC, WASM_KIND_TABLE, WASM_KIND_MEMORY, WASM_KIND_GLOBAL };

#define WASM_PAGE 65536u
#define WASM_MAX_PAGES 32767u
#define WASM_MAX_TABLE 10000000u

typedef struct wasm_module wasm_module;
typedef struct wasm_instance wasm_instance;
typedef struct wasm_func wasm_func;
typedef struct wasm_table wasm_table;
typedef struct wasm_memory wasm_memory;
typedef struct wasm_global wasm_global;
typedef struct wasm_exec wasm_exec;

/* The payload of an externref held in a table or global; all zero is null. The embedder gives it meaning. */
typedef struct {
  uint64_t w[2];
} wasm_extern;

typedef struct {
  uint32_t np, nr;         /* parameter and result counts          */
  uint32_t pslots, rslots; /* the same, in 64-bit stack slots      */
  const uint8_t *p, *r;    /* value types                          */
} wasm_functype;

typedef struct {
  uint32_t min, max;
  uint8_t has_max;
} wasm_limits;

typedef struct {
  uint8_t elem; /* WASM_FUNCREF or WASM_EXTERNREF */
  wasm_limits lim;
} wasm_tabletype;

typedef struct {
  uint8_t type, mut;
} wasm_globaltype;

/* A constant expression: op is 0x41-0x44 (i32/i64/f32/f64.const), 0xfd (v128.const), 0x23 (global.get v[0]),
 * 0xd0 (ref.null) or 0xd2 (ref.func v[0]) */
typedef struct {
  uint8_t op;
  uint64_t v[2];
} wasm_constexpr;

typedef struct {
  char *module, *name;
  uint8_t kind;
  uint32_t type; /* function: index into types */
  wasm_tabletype table;
  wasm_limits mem;
  wasm_globaltype global;
} wasm_import;

typedef struct {
  char *name;
  uint8_t kind;
  uint32_t idx;
} wasm_export;

typedef struct {
  char *name;
  uint8_t *bytes;
  uint32_t len;
} wasm_custom;

typedef struct {
  uint8_t mode; /* 0 active, 1 passive, 2 declarative */
  uint8_t type; /* WASM_FUNCREF or WASM_EXTERNREF */
  uint32_t table;
  wasm_constexpr offset;
  uint32_t n;
  int32_t *funcs; /* function indices, -1 for ref.null */
} wasm_elem;

typedef struct {
  uint8_t active;
  wasm_constexpr offset;
  uint32_t len;
  uint8_t *bytes;
} wasm_data;

typedef struct {
  uint32_t *code;
  uint32_t ncode;
  uint32_t nlocals;  /* slots: parameters and locals */
  uint32_t maxstack; /* operand stack slots above the locals */
} wasm_code;

struct wasm_module {
  uint32_t ntypes;
  wasm_functype *types;
  uint32_t nimports;
  wasm_import *imports;
  uint32_t nfunc_imports, ntable_imports, nmem_imports, nglobal_imports;
  uint32_t nfuncs, ntables, nmems, nglobals; /* including imports */
  uint32_t *func_type;                       /* per function: type index */
  wasm_tabletype *tables;
  wasm_limits *mems;
  wasm_globaltype *globals;
  wasm_constexpr *global_init; /* per defined global */
  uint32_t nexports;
  wasm_export *exports;
  int64_t start; /* function index, or -1 */
  uint32_t nelems;
  wasm_elem *elems;
  uint32_t ndatas;
  wasm_data *datas;
  uint32_t ncustom;
  wasm_custom *custom;
  wasm_code *code; /* per defined function */
  void *arena;
};

struct wasm_func {
  const wasm_functype *type;
  wasm_instance *inst;   /* where it is defined, or for a host function the importing instance */
  const wasm_code *code; /* NULL for a host function */
  uint32_t index;        /* within inst */
  void *js;              /* embedder cache: the exported wrapper, or the host callable */
};

struct wasm_table {
  uint8_t type;
  uint32_t n, max;
  uint8_t has_max;
  wasm_func **f;  /* funcref elements   */
  wasm_extern *x; /* externref elements */
  void *self;     /* embedder object that owns this table */
};

struct wasm_memory {
  uint8_t *data;
  uint64_t size;
  uint32_t pages, max;
  uint8_t has_max;
  void (*on_grow)(wasm_memory *m); /* called after the bytes moved or grew */
  void *self;                      /* embedder object that owns this memory */
};

struct wasm_global {
  uint8_t type, mut;
  uint64_t v[2]; /* a wasm_extern for an externref global */
  void *self;
};

struct wasm_instance {
  const wasm_module *module;
  void *self; /* embedder object for this instance */
  wasm_func **funcs;
  wasm_table **tables;
  wasm_memory **mems;
  wasm_global **globals;
  wasm_func *store;    /* backing for host stubs and defined functions */
  wasm_func ***elem_f; /* per element segment: its funcref items */
  uint32_t *elem_n;    /* and their count, 0 after elem.drop     */
  uint32_t nelems;
  uint8_t *data_dropped;
};

typedef struct {
  wasm_func *fn;
  const uint32_t *pc;
  uint64_t *fp;
} wasm_frame;

struct wasm_exec {
  uint64_t *stack, *stack_end, *sp;
  wasm_frame *frames;
  uint32_t nframes, maxframes;
  void *ud;
  unsigned short fpu_cw; /* x87 control word to give back to the host */
  const char *trap;      /* set when a call returns -1 */
  /* Host functions: read args from slots, write results to slots; nonzero means the host threw. */
  int (*host_call)(void *ud, wasm_func *f, uint64_t *slots);
  /* externref <-> handle (handle 0 is null; the same value may intern to different handles) */
  uint64_t (*xr_intern)(void *ud, const wasm_extern *v);
  void (*xr_value)(void *ud, uint64_t handle, wasm_extern *out);
};

wasm_module *wasm_module_new(const uint8_t *bytes, size_t len, char *err, size_t errn);
void wasm_module_free(wasm_module *m);
int wasm_functype_eq(const wasm_functype *a, const wasm_functype *b);

wasm_instance *wasm_instance_new(const wasm_module *m);
void wasm_instance_free(wasm_instance *i);
/* Create defined functions, tables, memories and globals and the element segments; 0, or -1 when out of memory */
int wasm_instance_define(wasm_instance *i);
/* Run active element and data segments; NULL, or the trap message */
const char *wasm_instance_apply_segments(wasm_instance *i);
void wasm_eval_const(const wasm_instance *i, const wasm_constexpr *e, uint64_t out[2]);

/* Memory: grow returns the old page count or -1 */
int wasm_memory_alloc(wasm_memory *m, uint32_t pages); /* 0 or -1 */
int64_t wasm_memory_grow(wasm_memory *m, uint32_t delta);
void wasm_memory_free(wasm_memory *m);
int wasm_table_alloc(wasm_table *t, uint32_t n);
int64_t wasm_table_grow(wasm_table *t, uint32_t delta, wasm_func *f, const wasm_extern *x);
void wasm_table_free(wasm_table *t);

/* Does a memory or table of size `cur` and the given maximum satisfy an import's limits? */
int wasm_limits_match(uint32_t cur, int has_max, uint32_t max, const wasm_limits *want);

/* Execution: args/results are slot arrays (a v128 takes two). 0 ok, -1 trap (x->trap), -2 a host call failed */
wasm_exec *wasm_exec_new(void *ud);
void wasm_exec_free(wasm_exec *x);
int wasm_call(wasm_exec *x, wasm_func *f, const uint64_t *args, uint64_t *results);

#endif
