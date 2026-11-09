/* interp.h — the tree-walk interpreter and engine context for the OwOS JS
 * engine.
 *
 * One js_ctx owns the global object, the prototype objects, the current
 * environment, the pending-exception slot, and an output hook. Create it with a
 * print callback (the embedder routes it to stdout / a terminal / the browser
 * console), run a parsed program, and read back the completion value or the
 * thrown exception.
 *
 * GC rooting (see value.h for why collection only happens inside js_new_*):
 *   - global, cur_env and every prototype are registered as single GC roots, so
 *     the whole reachable graph from the current scope stays live.
 *   - the pending exception is rooted through exc_root.
 *   - transient values an evaluator holds across an allocation are pinned with
 *     gc_push_scope()/gc_keep() (the JS_KEEP helper in interp.c), and a
 * suspended caller's environment is pinned for the duration of a nested call.
 */
#ifndef OWOS_JS_INTERP_H
#define OWOS_JS_INTERP_H

#include "ast.h" /* js_arena, for dynamically compiled code (eval/Function) */
#include "value.h"

struct js_node;

/* console method that produced a line of output, passed to the print hook so an
 * embedder can route/colour it (a terminal might dim debug, the browser
 * devtools colours warn yellow and error red). */
enum js_log_level { JS_LOG_LOG, JS_LOG_INFO, JS_LOG_WARN, JS_LOG_ERROR, JS_LOG_DEBUG };

/* `console.log("%c...", css)` styling is carried in-band in the print stream,
 * so the hook stays a plain (level, text) contract: a `%c` emits JS_STYLE_ON,
 * the (opaque) CSS text, then JS_STYLE_OFF. Subsequent output is styled by that
 * CSS until the next directive. The engine passes the CSS through verbatim —
 * the sink interprets it (the browser devtools maps
 * color/background/font-weight; a plain terminal sink strips the markers).
 * Newlines/these bytes are removed from the CSS payload, so a directive never
 * spans a line. */
#define JS_STYLE_ON 0x01
#define JS_STYLE_OFF 0x02

struct js_ctx {
  js_object *global;
  js_env *global_env;
  js_env *cur_env;

  js_object *object_proto, *function_proto, *array_proto, *string_proto, *number_proto, *boolean_proto, *error_proto,
      *promise_proto, *symbol_proto, *bigint_proto, *map_proto, *set_proto, *weakmap_proto, *weakset_proto, *date_proto,
      *regexp_proto, *generator_proto, *async_generator_proto;
  js_value sym_iterator;       /* the well-known Symbol.iterator                 */
  js_value sym_tostringtag;    /* the well-known Symbol.toStringTag              */
  js_value sym_async_iterator; /* the well-known Symbol.asyncIterator          */
  js_object *sym_registry;     /* array of every Symbol created, so
                                * Object.getOwnPropertySymbols can recover the
                                * symbol object behind a property's internal key */
  js_object *sym_for_reg;      /* the GlobalSymbolRegistry (Symbol.for/keyFor):
                                * a string-key -> Symbol map, shared and not
                                * bound to `this` (the methods are often called
                                * detached, e.g. `var f = Symbol.for; f(x)`)     */
  /* typed arrays: the shared %TypedArray%.prototype, the per-view prototypes
   * (indexed by TA_*), plus ArrayBuffer/DataView prototypes. */
  js_object *typedarray_proto, *arraybuffer_proto, *sharedarraybuffer_proto, *dataview_proto;
  js_object *typed_protos[TA_NKINDS];
  /* error subclass prototypes (TypeError, RangeError, ...) chain to error_proto
   */
  js_object *type_error_proto, *range_error_proto, *syntax_error_proto, *ref_error_proto, *eval_error_proto,
      *uri_error_proto, *aggregate_error_proto;

  int has_exc;           /* a throw is propagating                        */
  int gen_unwind;        /* has_exc is a generator.return() unwinding: finally blocks
                          * run but catch clauses do not                              */
  js_generator *cur_gen; /* the generator body now running, or NULL    */
  js_value exc;
  void *exc_root; /* GC-rooted: the exc's object/string, or NULL    */

  void (*print)(js_ctx *ctx, int level, const char *s, int len); /* level: enum js_log_level */
  long long (*now_ms)(void);                                     /* wall-clock ms since the epoch, for Date (embedder
                                                                  * sets it; NULL -> Date treats the epoch as 0)     */

  int depth;              /* call recursion guard                          */
  char *stack_limit;      /* lowest native SP the tree-walk may use; below
                           * it a RangeError is thrown instead of smashing
                           * past the C stack (NULL = no check). Set by the
                           * embedder via js_set_stack_limit.               */
  const char *frames[64]; /* active call frames (function names), innermost
                           * last — captured into an Error's .stack         */
  int frame_line[64];     /* source line of each frame's active call site   */
  int frame_col[64];
  int nframes;
  int cur_line, cur_col;  /* location of the node currently evaluating —
                           * the innermost frame's spot in a stack trace    */
  char *async_stack;      /* while a deferred callback runs (a timer/event
                           * job), the formatted frames captured at the point
                           * it was scheduled; capture_stack appends them so a
                           * throw names the scheduling site, not just the
                           * callback body. Not owned — the embedder installs
                           * and clears it around the call (js_async_*).      */
  const char *cur_script; /* embedder-set label of the top-level script or
                           * async entry currently executing (a source URL, or
                           * "<timer callback>"/"<click listener>"). The engine
                           * emits it as the outermost frame of every captured
                           * stack, so a trace bottoms out at its origin even
                           * when no JS function is on the stack (a timer armed
                           * at script top level). Snapshotted into each async
                           * capture, so a grafted "scheduled from" segment
                           * names the script that armed it. Not owned; the
                           * embedder keeps the storage valid (js_set_script). */
  unsigned rng;           /* Math.random state                             */
  int can_block;          /* Atomics.wait may block this context's thread */
  js_irq *irq;            /* another thread's way to stop this context (NULL: none) */
  /* Embedder objects that structured clone should carry across (a Blob, say): write returns nonzero when it
   * recognised `v`, filling a short `kind`, malloc'd bytes and an optional malloc'd `meta` string; read rebuilds one.
   */
  int (*clone_write_ext)(js_ctx *ctx, js_value v, char kind[16], unsigned char **bytes, int *len, char **meta);
  js_value (*clone_read_ext)(js_ctx *ctx, const char *kind, const unsigned char *bytes, int len, const char *meta);
  js_object *prom_queue; /* promise reactions deferred because the stack was already deep (flat entries of 5 values) */
  unsigned prom_head, prom_depth;
  void *re_cache;           /* compiled-RegExp cache (builtins.c)         */
  unsigned ident_cache_gen; /* generation for the JS_IDENT inline cache; bumped
                             * by direct eval, which can inject a binding into an
                             * existing scope and so invalidate cached slots     */
  js_value native_data;     /* the called native's fn->data (bound closure);
                             * a native reads it at entry before any sub-call */

  /* arenas for code compiled at runtime (eval, the Function constructor); kept
   * for the context's lifetime since functions defined there outlive the call.
   */
  js_arena **dyn_arenas;
  int dyn_n, dyn_cap;

  /* ES modules: the embedder's loader (resolve+fetch+execute a specifier,
   * returns the module namespace), and the module currently executing —
   * `export` writes land in cur_module_exports, `import.meta.url` reads
   * cur_module_url. */
  js_object *(*load_module)(js_ctx *ctx, const char *spec, void *ud);
  void *load_module_ud;
  js_object *cur_module_exports;
  const char *cur_module_url;
};

/* Create/destroy an engine. `print` may be NULL (output is then discarded).
 * NOTE: js_ctx_new resets the GC heap, so only one context may be live at once.
 */
js_ctx *js_ctx_new(void (*print)(js_ctx *ctx, int level, const char *s, int len));
void js_ctx_free(js_ctx *ctx);

/* Set the lowest native stack address the interpreter may recurse to before it
 * throws a RangeError. The tree-walk evaluator and native callbacks recurse on
 * the C stack, so an embedder with a fixed/bounded thread stack must pass a
 * limit (its stack floor + headroom for the throw path) or a deeply recursive
 * script overflows the stack into neighbouring memory. NULL disables the check.
 */
void js_set_stack_limit(js_ctx *ctx, void *low_limit);
/* Lets another thread stop a context: it sets `stop`, and every evaluation step then unwinds with an exception
 * that try/catch and finally cannot intercept. A thread blocked in Atomics.wait publishes its futex word in
 * `waiting` so the stopper can wake it. */
struct js_irq {
  int stop;
  unsigned *waiting;
};
static inline int js_irq_stopped(const js_irq *irq) { return irq && __atomic_load_n(&irq->stop, __ATOMIC_ACQUIRE); }

/* Structured clone across heaps: js_clone_write flattens a value into pointer-free bytes (a SharedArrayBuffer
 * store is retained, not copied), js_clone_read rebuilds it in any context, js_clone_free drops the bytes and
 * the store references. Write returns NULL with a DataCloneError pending for functions and host objects.
 * `transfer` is postMessage's transfer list (an array, {transfer: array} or undefined): the ArrayBuffers in it
 * are moved, not copied, and detached in the sender once the message is written. */
typedef struct js_clone js_clone;
js_clone *js_clone_write(js_ctx *ctx, js_value v, js_value transfer);
js_value js_clone_read(js_ctx *ctx, const js_clone *c);
void js_clone_free(js_clone *c);
js_clone *
js_clone_dup(const js_clone *c); /* a second copy, NULL when the message holds transferred buffers or wasm objects */

/* A fresh ArrayBuffer holding a copy of `data` (zeroed when NULL) */
js_value js_new_arraybuffer(js_ctx *ctx, const void *data, int byte_length);

/* An ArrayBuffer over bytes that `owner` keeps; the buffer keeps `owner` alive and never frees the bytes.
 * js_arraybuffer_detach empties such a buffer once the bytes moved or went away. */
js_value js_arraybuffer_external(js_ctx *ctx, unsigned char *data, int byte_length, js_object *owner);
void js_arraybuffer_detach(js_object *buf);

/* The WebAssembly global (wasm_js.c), set up with the other builtins */
void js_setup_wasm(js_ctx *ctx);
/* Make compiling and instantiating WebAssembly fail with a CompileError (a Content-Security-Policy refusal) */
void js_wasm_block(js_ctx *ctx);

/* A SharedArrayBuffer object in ctx over an existing store, which it retains. Lets one thread hand a buffer to
 * another thread's context: retain on the sending side, wrap in the receiver, then release. */
js_value js_sab_wrap(js_ctx *ctx, unsigned char *data, int byte_length);
void js_regex_cache_free(js_ctx *ctx); /* drop the context's compiled-RegExp cache */

/* Run a parsed program. Returns 0 on normal completion (the last expression
 * value, if any, in *result when non-NULL); returns -1 if an exception escaped
 * (ctx->exc holds it). */
int js_run(js_ctx *ctx, struct js_node *program, js_value *result);

/* ES modules: install the embedder's module loader, and run a parsed module
 * body with its own top-level scope; top-level `export`s are recorded into
 * `exports` (the module's namespace object) and `import.meta.url` reads `url`.
 */
void js_set_module_loader(js_ctx *ctx, js_object *(*fn)(js_ctx *, const char *, void *), void *ud);
int js_run_module(js_ctx *ctx, struct js_node *program, js_object *exports, const char *url);

/* Invoke a callable. On a thrown exception these return undefined with
 * ctx->has_exc set; callers in native code must check and propagate. */
js_value js_call(js_ctx *ctx, js_value fn, js_value thisv, js_value *args, int argc);
js_value js_construct(js_ctx *ctx, js_value fn, js_value *args, int argc);
/* A zeroed typed array (TA_* element kind) of `length` elements over a fresh ArrayBuffer. */
js_value js_new_typedarray(js_ctx *ctx, int type, int length);

/* Async stack chaining, for embedders that defer a callback (a timer job, a
 * queued event) past the call that scheduled it. js_async_capture() snapshots
 * the current call frames — plus any async parent already installed, so nested
 * deferrals chain — into a malloc'd string the embedder stores next to the
 * callback (free it when the scheduled entry is discarded). Right before
 * invoking the callback the embedder installs it with js_async_enter() and
 * restores the previous with js_async_leave(); any Error created while the
 * callback runs then carries the scheduling frames after its own, so an
 * uncaught error names the code that armed the callback. */
char *js_async_capture(js_ctx *ctx);
char *js_async_enter(js_ctx *ctx, char *captured); /* returns the previous, for js_async_leave */
void js_async_leave(js_ctx *ctx, char *prev);

/* Label the top-level script / async entry now executing, so it appears as the
 * outermost frame of any stack captured from here (see js_ctx.cur_script). The
 * engine stores the pointer, not a copy — keep `label` valid until the next
 * call (a stable buffer or a string literal). Pass NULL to clear. */
void js_set_script(js_ctx *ctx, const char *label);

/* Exceptions. js_throw sets the pending exception; the *_error helpers build
 * and throw a standard Error subclass and return undefined for convenient
 * `return`. */
void js_throw(js_ctx *ctx, js_value err);
js_value js_throw_error(js_ctx *ctx, js_object *proto, const char *name, const char *msg);
js_value js_throw_type(js_ctx *ctx, const char *msg);
js_value js_throw_range(js_ctx *ctx, const char *msg);
js_value js_make_error(js_ctx *ctx, js_object *proto, const char *name, const char *msg);

/* The two string-compiling globals, defined in interp.c (they need the parser
 * and the statement executor) and registered by builtins.c. */
js_value js_builtin_eval(js_ctx *ctx, js_value thisv, js_value *args, int argc);
js_value js_builtin_function(js_ctx *ctx, js_value thisv, js_value *args, int argc);

/* Promise plumbing exposed to the interpreter for async/await (builtins.c). */
js_object *js_promise_new(js_ctx *ctx); /* a pending promise */
void js_promise_settle(js_ctx *ctx, js_object *p, int rejected,
                       js_value v);                                  /* fulfil or reject, once; no thenable adoption */
void js_promise_resolve_with(js_ctx *ctx, js_object *p, js_value v); /* the promise's resolve function */
void js_promise_react(js_ctx *ctx, js_object *p, js_value on_fulfilled, js_value on_rejected);
js_value js_promise_resolved(js_ctx *ctx, js_value v); /* a fulfilled promise of v */
js_value js_promise_rejected(js_ctx *ctx, js_value v); /* a rejected promise of v  */
/* -1 = not a promise; 0 pending, 1 fulfilled, 2 rejected. *out = settled value.
 */
int js_promise_inspect(js_ctx *ctx, js_value v, js_value *out);

/* Create %GeneratorPrototype% (next/return/throw), run once from js_setup_builtins. */
void js_setup_generators(js_ctx *ctx);

/* builtins.c — populate the global object and the prototype objects. */
void js_setup_builtins(js_ctx *ctx);

/* Build a RegExp object (on ctx->regexp_proto) from a source + flag string.
 * Shared by the `/.../ ` literal (interp.c) and the RegExp constructor. */
js_value js_make_regexp(js_ctx *ctx, const char *source, const char *flags);

/* Convenience used widely by builtins to define a native method on an object.
 */
void js_def_native(js_ctx *ctx, js_object *o, const char *name, js_native fn, int arity);

#endif /* OWOS_JS_INTERP_H */
