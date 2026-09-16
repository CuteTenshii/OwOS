/* gc.c — a generational, non-moving mark-sweep collector. See gc.h for the
 * contract.
 *
 * Two generations, threaded onto two lists. New objects are allocated YOUNG; an
 * object that survives a collection is promoted OLD. A *minor* collection
 * traces and sweeps only the young list, treating the old generation as live
 * roots — this is the whole point, since a page's retained DOM is old and never
 * re-traced. A minor collection stays correct via:
 *   - a write barrier (gc_write_barrier) that records every old->young pointer
 * in a remembered set, whose old objects are scanned as extra roots; and
 *   - a "rescan kind" (gc_set_rescan_kind): old objects of that kind are
 * scanned wholesale every minor, for containers (arrays) whose element stores
 * are too scattered to barrier reliably. A *major* collection periodically
 * traces and sweeps both generations to reclaim dead old objects. Marking is
 * iterative over an explicit grey worklist (never C recursion); a whole-heap
 * rescan finishes the job if the worklist can't grow.
 *
 * The header stays 16 bytes on ILP32 so the payload remains 8-aligned for
 * doubles.
 */

#include "gc.h"
#include "owos.h"

#ifdef GC_PROFILE
#include <time.h>
static double gc_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

enum { WHITE = 0, GREY = 1, BLACK = 2 };
enum { GEN_YOUNG = 0, GEN_OLD = 1 };

typedef struct gc_obj {
  struct gc_obj *next;
  unsigned size; /* payload bytes                            */
  unsigned char mark;
  unsigned char gen; /* GEN_YOUNG / GEN_OLD                      */
  unsigned char rem; /* 1 if already in the remembered set       */
  unsigned char kind;
  unsigned _pad; /* keep header 16 -> payload 8-aligned      */
} gc_obj;

#define MIN_THRESHOLD (256u * 1024u)     /* smallest nursery budget */
#define NURSERY_CAP (8u * 1024u * 1024u) /* largest nursery budget          */
#define MIN_MAJOR (4u * 1024u * 1024u)   /* old-gen size that forces a major */

static gc_obj *young; /* freshly allocated                    */
static gc_obj *old;   /* survived >= 1 collection             */
static unsigned young_bytes, old_bytes;
static unsigned alloc_since;
static unsigned threshold = MIN_THRESHOLD;   /* nursery budget before a GC  */
static unsigned major_threshold = MIN_MAJOR; /* old_bytes that forces major */
static int force_major;                      /* set when a barrier couldn't remember */
static int rescan_kind = -1;                 /* kind scanned wholesale when old      */
static int collecting_minor;                 /* gc_mark skips old objects while set  */
static gc_tracer tracer;
static gc_finalizer finalizer;
static gc_stats_t stats;

/* roots: addresses of pointer variables. */
static void ***roots;
static int nroots, caproots;
/* root ranges: a base array plus a live count. */
struct range {
  void **base;
  int *count;
};
static struct range *ranges;
static int nranges, capranges;
/* handle scopes: pinned local pointers, with per-scope saved high-water marks.
 */
static void **handles;
static int nhandles, caphandles;
static int *scopes;
static int nscopes, capscopes;
/* grey worklist. */
static void **grey;
static int grey_sp, grey_cap, grey_overflow;
/* remembered set: old objects that hold an old->young pointer. */
static gc_obj **remset;
static int remset_n, remset_cap;
/* old objects of the rescan kind, scanned wholesale each minor. */
static gc_obj **rescan;
static int rescan_n, rescan_cap;

static inline gc_obj *hdr(void *payload) { return (gc_obj *)payload - 1; }
static inline void *body(gc_obj *o) { return (void *)(o + 1); }

/* Grow a void* array to hold one more; returns 0 on OOM (caller copes). */
static int grow(void ***a, int *cap, int need)
{
  if (need <= *cap)
    return 1;
  int nc = *cap ? *cap * 2 : 32;
  while (nc < need)
    nc *= 2;
  void **na = realloc(*a, (unsigned)nc * sizeof(void *));
  if (!na)
    return 0;
  *a = na;
  *cap = nc;
  return 1;
}

void gc_init(gc_tracer trace, gc_finalizer finalize)
{
  tracer = trace;
  finalizer = finalize;
  young = old = 0;
  young_bytes = old_bytes = alloc_since = 0;
  threshold = MIN_THRESHOLD;
  major_threshold = MIN_MAJOR;
  force_major = 0;
  collecting_minor = 0;
  nroots = nranges = nhandles = nscopes = 0;
  grey_sp = 0;
  remset_n = rescan_n = 0;
}

void gc_set_rescan_kind(int kind) { rescan_kind = kind; }

/* ------------------------------------------------------------------ marking */
void gc_mark(void *p)
{
  if (!p)
    return;
  gc_obj *h = hdr(p);
  if (collecting_minor && h->gen == GEN_OLD)
    return; /* old is assumed live  */
  if (h->mark != WHITE)
    return; /* already grey or black */
  h->mark = GREY;
  if (grey_sp < grey_cap) {
    grey[grey_sp++] = p;
    return;
  }
  if (grow((void ***)&grey, &grey_cap, grey_sp + 1))
    grey[grey_sp++] = p;
  else
    grey_overflow = 1; /* rescan pass will find it              */
}

static void mark_roots(void)
{
  for (int i = 0; i < nroots; i++)
    gc_mark(*roots[i]);
  for (int i = 0; i < nranges; i++) {
    int n = *ranges[i].count;
    for (int j = 0; j < n; j++)
      gc_mark(ranges[i].base[j]);
  }
  for (int i = 0; i < nhandles; i++)
    gc_mark(handles[i]);
}

/* Drain the grey worklist, tracing each object. `full` selects the fallback
 * rescan list: only the young list can hold grey objects during a minor. */
static void drain(int full)
{
  for (;;) {
    while (grey_sp > 0) {
      void *o = grey[--grey_sp];
      gc_obj *h = hdr(o);
      if (h->mark == BLACK)
        continue;
      h->mark = BLACK;
#ifdef GC_PROFILE
      stats.marked++;
#endif
      if (tracer)
        tracer(o, h->kind);
    }
    if (!grey_overflow)
      return;
    /* Worklist couldn't hold everything: sweep for stranded grey objects,
     * blacken and trace them. Marks only move white->grey->black, so repeated
     * passes make monotonic progress and terminate. */
    grey_overflow = 0;
    int found = 0;
    for (int pass = 0; pass < 2; pass++) {
      gc_obj *list = pass == 0 ? young : old;
      if (pass == 1 && !full)
        break; /* old holds no grey during a minor */
      for (gc_obj *o = list; o; o = o->next)
        if (o->mark == GREY) {
          o->mark = BLACK;
          if (tracer)
            tracer(body(o), o->kind);
          found = 1;
        }
    }
    if (!found && grey_sp == 0)
      return;
  }
}

/* Sweep one generation list: free the white objects, and run `on_survivor` for
 * each survivor (resetting its mark). Returns bytes freed and advances *pbytes.
 */
static unsigned sweep(gc_obj **list, unsigned *pbytes, void (*on_survivor)(gc_obj *))
{
  unsigned freed = 0;
  gc_obj **pp = list;
  while (*pp) {
    gc_obj *o = *pp;
#ifdef GC_PROFILE
    stats.swept++;
#endif
    if (o->mark == WHITE) {
      *pp = o->next;
      if (finalizer)
        finalizer(body(o), o->kind);
      *pbytes -= o->size;
      freed += o->size;
#ifdef GC_PROFILE
      stats.freed++;
#endif
      free(o);
    } else {
      o->mark = WHITE;
      on_survivor(o);
      pp = &o->next;
    }
  }
  return freed;
}

static void note_rescan(gc_obj *o)
{
  if (o->kind == rescan_kind && grow((void ***)&rescan, &rescan_cap, rescan_n + 1))
    rescan[rescan_n++] = o;
}

/* A minor sweep unlinks each young survivor and promotes it to the old list. */
static void promote_survivor(gc_obj *o) { (void)o; /* handled inline in gc_minor: promotion needs to relink lists */ }

static void set_threshold(void)
{
  unsigned want = old_bytes;
  if (want < MIN_THRESHOLD)
    want = MIN_THRESHOLD;
  if (want > NURSERY_CAP)
    want = NURSERY_CAP;
  threshold = want;
  alloc_since = 0;
}

static void clear_remset(void)
{
  for (int i = 0; i < remset_n; i++)
    remset[i]->rem = 0;
  remset_n = 0;
}

/* ---------------------------------------------------------------- minor GC */
static void gc_minor(void)
{
  stats.collections++;
  stats.minors++;
#ifdef GC_PROFILE
  double t0 = gc_now_ms();
#endif
  collecting_minor = 1;
  grey_sp = 0;
  grey_overflow = 0;

  mark_roots();
  /* old->young edges: the remembered set, plus every old rescan-kind object. */
  for (int i = 0; i < remset_n; i++)
    if (tracer)
      tracer(body(remset[i]), remset[i]->kind);
  for (int i = 0; i < rescan_n; i++)
    if (tracer)
      tracer(body(rescan[i]), rescan[i]->kind);
  drain(0);

  /* Sweep the young list: free the dead, promote survivors to old. */
  {
    gc_obj **pp = &young;
    while (*pp) {
      gc_obj *o = *pp;
#ifdef GC_PROFILE
      stats.swept++;
#endif
      if (o->mark == WHITE) {
        *pp = o->next;
        if (finalizer)
          finalizer(body(o), o->kind);
        young_bytes -= o->size;
#ifdef GC_PROFILE
        stats.freed++;
#endif
        free(o);
      } else {
        o->mark = WHITE;
        o->gen = GEN_OLD;
        young_bytes -= o->size;
        old_bytes += o->size;
        *pp = o->next; /* unlink from young                    */
        o->next = old;
        old = o; /* prepend to old                       */
        note_rescan(o);
      }
    }
  }

  clear_remset(); /* every remembered old->young is now old->old       */
  collecting_minor = 0;
  set_threshold();
#ifdef GC_PROFILE
  {
    double _d = gc_now_ms() - t0;
    stats.ms += _d;
    if (_d > stats.max_ms)
      stats.max_ms = _d;
  }
#endif
}

/* ---------------------------------------------------------------- major GC */
static unsigned gc_major(void)
{
  stats.collections++;
  stats.majors++;
#ifdef GC_PROFILE
  double t0 = gc_now_ms();
#endif
  collecting_minor = 0;
  grey_sp = 0;
  grey_overflow = 0;

  mark_roots();
  drain(1);

  /* Sweep old first (survivors keep their generation), then young (survivors
   * promote to old). Rebuild the rescan list from the surviving old objects. */
  unsigned freed = sweep(&old, &old_bytes, promote_survivor);
  {
    gc_obj **pp = &young;
    while (*pp) {
      gc_obj *o = *pp;
#ifdef GC_PROFILE
      stats.swept++;
#endif
      if (o->mark == WHITE) {
        *pp = o->next;
        if (finalizer)
          finalizer(body(o), o->kind);
        young_bytes -= o->size;
        freed += o->size;
#ifdef GC_PROFILE
        stats.freed++;
#endif
        free(o);
      } else {
        o->mark = WHITE;
        o->gen = GEN_OLD;
        young_bytes -= o->size;
        old_bytes += o->size;
        *pp = o->next;
        o->next = old;
        old = o;
      }
    }
  }

  /* Rebuild the rescan list and clear stale remembered-set flags: both are
   * repopulated by promotion and the barrier from here on. */
  rescan_n = 0;
  for (gc_obj *o = old; o; o = o->next) {
    o->rem = 0;
    note_rescan(o);
  }
  remset_n = 0;

  force_major = 0;
  major_threshold = old_bytes * 2;
  if (major_threshold < MIN_MAJOR)
    major_threshold = MIN_MAJOR;
  set_threshold();
#ifdef GC_PROFILE
  {
    double _d = gc_now_ms() - t0;
    stats.ms += _d;
    if (_d > stats.max_ms)
      stats.max_ms = _d;
  }
#endif
  return freed;
}

/* Public: force a full reclamation. */
unsigned gc_collect(void) { return gc_major(); }

/* Choose a collection when the nursery budget is spent. */
static void gc_step(void)
{
  if (force_major || old_bytes > major_threshold)
    gc_major();
  else
    gc_minor();
}

/* ------------------------------------------------------------ write barrier */
void gc_write_barrier(void *container)
{
  if (!container)
    return;
  gc_obj *h = hdr(container);
  if (h->gen != GEN_OLD || h->rem)
    return;
  if (h->kind == rescan_kind)
    return; /* scanned wholesale; no need to log */
  if (grow((void ***)&remset, &remset_cap, remset_n + 1)) {
    h->rem = 1;
    remset[remset_n++] = h;
  } else {
    force_major = 1; /* can't remember this edge: next GC must be a major */
  }
}

/* --------------------------------------------------------------- allocation */
void *gc_alloc(unsigned size, int kind)
{
#ifdef GC_TORTURE
  /* Collect on every allocation, forcing a major periodically. Every object is
   * then one allocation from reclamation, so a missing write barrier or rooting
   * bug surfaces immediately as a use-after-free (run under AddressSanitizer).
   */
  if (tracer) {
    static unsigned tc;
    if ((++tc & 63) == 0)
      force_major = 1;
    gc_step();
  }
#else
  if (alloc_since + size > threshold)
    gc_step();
#endif

  gc_obj *o = malloc(sizeof(gc_obj) + size);
  if (!o) {
    gc_major();
    o = malloc(sizeof(gc_obj) + size);
    if (!o)
      return 0;
  }

  memset(o, 0, sizeof(gc_obj) + size); /* zero header + payload (NULL kids) */
  o->size = size;
  o->kind = (unsigned char)kind;
  o->mark = WHITE;
  o->gen = GEN_YOUNG;
  o->next = young;
  young = o;

  young_bytes += size;
  alloc_since += size;
  return body(o);
}

/* -------------------------------------------------------------------- roots */
void gc_add_root(void **slot)
{
  if (!grow((void ***)&roots, &caproots, nroots + 1))
    return;
  roots[nroots++] = slot;
}

void gc_remove_root(void **slot)
{
  for (int i = 0; i < nroots; i++)
    if (roots[i] == slot) {
      roots[i] = roots[--nroots];
      return;
    }
}

void gc_add_root_range(void **base, int *count)
{
  if (nranges == capranges) {
    int nc = capranges ? capranges * 2 : 8;
    struct range *nr = realloc(ranges, (unsigned)nc * sizeof(struct range));
    if (!nr)
      return;
    ranges = nr;
    capranges = nc;
  }
  ranges[nranges].base = base;
  ranges[nranges].count = count;
  nranges++;
}

void gc_remove_root_range(void **base)
{
  for (int i = 0; i < nranges; i++)
    if (ranges[i].base == base) {
      ranges[i] = ranges[--nranges];
      return;
    }
}

/* ------------------------------------------------------------ handle scopes */
void gc_push_scope(void)
{
  if (!grow((void ***)&scopes, &capscopes, nscopes + 1))
    return;
  scopes[nscopes++] = nhandles;
}

void gc_pop_scope(void)
{
  if (nscopes > 0)
    nhandles = scopes[--nscopes];
}

void *gc_keep(void *obj)
{
  if (grow((void ***)&handles, &caphandles, nhandles + 1))
    handles[nhandles++] = obj;
  return obj;
}

/* --------------------------------------------------------------------- misc */
unsigned gc_bytes(void) { return young_bytes + old_bytes; }
void gc_get_stats(gc_stats_t *out)
{
  if (out)
    *out = stats;
}
void gc_set_threshold(unsigned bytes) { threshold = bytes ? bytes : MIN_THRESHOLD; }
