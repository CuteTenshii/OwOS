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
 *   be reachable from a root at that moment (gc_alloc may collect first). Bracket
 *   such code with gc_push_scope()/gc_pop_scope() and gc_keep() each local.
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

/* Called from a tracer to mark a referenced object reachable. NULL is ignored.
 * Must be passed a pointer returned by gc_alloc (a precise collector trusts its
 * tracer; do not pass interior or non-GC pointers). */
void gc_mark(void *obj);

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
void  gc_push_scope(void);
void  gc_pop_scope(void);
void *gc_keep(void *obj);

/* Force a full collection now; returns the number of payload bytes reclaimed. */
unsigned gc_collect(void);

/* Live payload bytes currently tracked. */
unsigned gc_bytes(void);
/* Collect automatically once this many bytes have been allocated since the last
 * collection. The collector also raises this adaptively to ~2x the live set. */
void gc_set_threshold(unsigned bytes);

#endif /* OWOS_GC_H */
