/* gc.h — a precise mark-sweep garbage collector for OwOS user space.
 *
 * It is deliberately general: the collector owns allocation, threading every
 * object onto an all-objects list, and reclaims anything not reachable from the
 * registered roots. It does NOT know your object layout — you supply one tracer
 * callback that, given an object, calls gc_mark() on each GC pointer it holds,
 * and (optionally) a finalizer to release non-GC resources before a free.
 *
 * Precise, not conservative: roots are registered explicitly rather than found
 * by scanning the C stack. That is deterministic and safe under this OS's
 * cooperative scheduler and user threads, where a stack base cannot be assumed.
 * Three ways to keep something alive:
 *   - gc_add_root(&slot)            a long-lived pointer variable (a global)
 *   - gc_add_root_range(base,&n)    a live array of pointer slots (a VM stack)
 *   - gc_push_scope()/gc_keep(p)    C locals held across an allocation
 *
 * The collector calls malloc()/free() (the K&R user heap), so a swept object is
 * really returned to the heap; there is no separate GC arena.
 *
 *   THE ONE RULE: any GC pointer a C local holds across a gc_alloc() call must
 *   be reachable from a root at that moment (gc_alloc may collect first).
 * Bracket such code with gc_push_scope()/gc_pop_scope() and gc_keep() each
 * local.
 */
#ifndef OWOS_GC_H
#define OWOS_GC_H

/* trace: mark every GC pointer `obj` (tag `kind`) holds, via gc_mark().
 * finalize: release non-GC resources owned by `obj` just before it is freed. */
typedef void (*gc_tracer)(void *obj, int kind);
typedef void (*gc_finalizer)(void *obj, int kind);

/* Reset to an empty heap and install the callbacks (finalize may be NULL).
 * Call once before any gc_alloc(). */
void gc_init(gc_tracer trace, gc_finalizer finalize);

/* Allocate a zeroed collectable object of `size` payload bytes tagged `kind`
 * (your own enum). May trigger a collection first. Returns the payload pointer,
 * or NULL only if the heap is exhausted even after collecting. The zero-fill
 * guarantees a half-built object has NULL children, so it is safe to trace if
 * the very next allocation collects. */
void *gc_alloc(unsigned size, int kind);

/* Unrecoverable allocation failure: raises SIGABRT so the embedder can drop the page. */
void gc_oom(void);

/* Called from a tracer to mark a referenced object reachable. NULL is ignored.
 * Must be passed a pointer returned by gc_alloc (a precise collector trusts its
 * tracer; do not pass interior or non-GC pointers). */
void gc_mark(void *obj);

/* Generational write barrier. Call AFTER storing a GC pointer into `container`
 * (an object returned by gc_alloc). It records old->young edges so a minor
 * collection — which does not trace the old generation — still keeps a young
 * object reachable only from an old one. Cheap: a no-op unless `container` is
 * old. Stores into a freshly allocated (young) container need no barrier, but
 * calling it there is harmless. Containers of the "rescan kind" (see below) are
 * exempt — they are scanned wholesale every minor collection instead. */
void gc_write_barrier(void *container);

/* Register one object kind whose instances, once old, are re-scanned in full on
 * every minor collection instead of relying on the write barrier. Use it for a
 * kind whose GC pointers are stored through many scattered call sites (e.g. a
 * dense array's elements), where a per-store barrier would be error-prone. */
void gc_set_rescan_kind(int kind);

/* Roots. */
void gc_add_root(void **slot);
void gc_remove_root(void **slot);
/* A contiguous array of pointer slots scanned [base, base + *count) each cycle;
 * reading *count live means a growing/shrinking VM stack tracks correctly. */
void gc_add_root_range(void **base, int *count);
void gc_remove_root_range(void **base);

/* Handle scopes for C locals. push/pop bracket a scope (LIFO); gc_keep() pins a
 * pointer until the enclosing scope is popped, and returns it for inline use:
 *   gc_push_scope(); Obj *o = gc_keep(gc_alloc(...)); ...; gc_pop_scope(); */
void gc_push_scope(void);
void gc_pop_scope(void);
void *gc_keep(void *obj);

/* Coroutine support: a suspended coroutine's handle scopes must leave the shared
 * LIFO stack while another context runs. gc_scope_pos() reads the current
 * handle/scope depths; gc_scope_take() moves everything pushed above that mark
 * into `st`; gc_scope_put() pushes it back on top of whatever is there now. The
 * owner traces a parked `st` with gc_scope_trace() from its own tracer. */
typedef struct {
  void **h;
  int *s;
  int nh, ns;
} gc_stash;
void gc_scope_pos(int *nh, int *ns);
void gc_scope_take(int nh, int ns, gc_stash *st);
void gc_scope_put(gc_stash *st);
void gc_scope_trace(const gc_stash *st);
void gc_scope_drop(gc_stash *st);

/* Force a full collection now; returns the number of payload bytes reclaimed.
 */
unsigned gc_collect(void);

/* Live payload bytes currently tracked. */
unsigned gc_bytes(void);

/* Cumulative collector statistics (for profiling). */
typedef struct {
  unsigned long collections; /* gc_collect() calls                          */
  unsigned long marked;      /* objects blackened (traced) across all cycles */
  unsigned long swept;       /* all-objects-list nodes visited in sweep      */
  unsigned long freed;       /* objects reclaimed                            */
  unsigned long minors;      /* minor (young-only) collections               */
  unsigned long majors;      /* major (full) collections                     */
  double ms;                 /* wall-clock spent collecting (profiling)      */
  double max_ms;             /* longest single collection (pause time)       */
} gc_stats_t;
void gc_get_stats(gc_stats_t *out);
/* Collect automatically once this many bytes have been allocated since the last
 * collection. The collector also raises this adaptively to ~2x the live set. */
void gc_set_threshold(unsigned bytes);

#endif /* OWOS_GC_H */
