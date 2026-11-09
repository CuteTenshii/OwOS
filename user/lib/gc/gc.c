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

struct range {
  void **base;
  int *count;
};

/* One collector instance: everything a thread's contexts share. */
struct gc_heap {
  gc_obj *young; /* freshly allocated                    */
  gc_obj *old;   /* survived >= 1 collection             */
  unsigned young_bytes, old_bytes;
  unsigned alloc_since;
  unsigned threshold;       /* nursery budget before a GC  */
  unsigned major_threshold; /* old_bytes that forces major */
  int force_major;          /* set when a barrier couldn't remember */
  int rescan_kind;          /* kind scanned wholesale when old      */
  int collecting_minor;     /* gc_mark skips old objects while set  */
  gc_tracer tracer;
  gc_finalizer finalizer;
  gc_stats_t stats;
  /* roots: addresses of pointer variables. */
  void ***roots;
  int nroots, caproots;
  /* root ranges: a base array plus a live count. */
  struct range *ranges;
  int nranges, capranges;
  /* handle scopes: pinned local pointers, with per-scope saved high-water marks. */
  void **handles;
  int nhandles, caphandles;
  int *scopes;
  int nscopes, capscopes;
  int lost_scopes; /* pushes that hit OOM: their pops must not close an outer scope */
  /* grey worklist. */
  void **grey;
  int grey_sp, grey_cap, grey_overflow;
  /* remembered set: old objects that hold an old->young pointer. */
  gc_obj **remset;
  int remset_n, remset_cap;
  /* old objects of the rescan kind, scanned wholesale each minor. */
  gc_obj **rescan;
  int rescan_n, rescan_cap;
  void (*oom)(void *);
  void *oom_arg;
};

static inline gc_obj *hdr(void *payload) { return (gc_obj *)payload - 1; }
static inline void *body(gc_obj *o) { return (void *)(o + 1); }

/* The heap of the calling thread. A thread that never attaches one uses main_heap, so single-threaded
 * embedders see the old global collector. */
static gc_heap main_heap = {.threshold = MIN_THRESHOLD, .major_threshold = MIN_MAJOR, .rescan_kind = -1};
static __thread gc_heap *thread_heap;

static inline gc_heap *cur_heap(void) { return thread_heap ? thread_heap : &main_heap; }

void gc_heap_attach(gc_heap *h) { thread_heap = h; }

void gc_heap_detach(void) { thread_heap = 0; }

gc_heap *gc_heap_new(void)
{
  gc_heap *h = malloc(sizeof *h);
  if (h) {
    memset(h, 0, sizeof *h);
    h->threshold = MIN_THRESHOLD;
    h->major_threshold = MIN_MAJOR;
    h->rescan_kind = -1;
  }
  return h;
}

void gc_heap_free(gc_heap *h)
{
  for (gc_obj *list = h->young; list;) {
    gc_obj *next = list->next;
    if (h->finalizer)
      h->finalizer(body(list), list->kind);
    free(list);
    list = next;
  }
  for (gc_obj *list = h->old; list;) {
    gc_obj *next = list->next;
    if (h->finalizer)
      h->finalizer(body(list), list->kind);
    free(list);
    list = next;
  }
  free(h->roots);
  free(h->ranges);
  free(h->handles);
  free(h->scopes);
  free(h->grey);
  free(h->remset);
  free(h->rescan);
  free(h);
}

int gc_ready(void) { return cur_heap()->tracer != 0; }

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

int raise(int sig);

void gc_heap_on_oom(gc_heap *h, void (*fn)(void *), void *arg)
{
  h->oom = fn;
  h->oom_arg = arg;
}

void gc_oom(void)
{
  gc_heap *H = cur_heap();
  void (*fn)(void *) = H->oom;
  H->oom = 0; /* a second failure while recovering is fatal */
  if (fn)
    fn(H->oom_arg);
  raise(6); /* SIGABRT: crash handler drops the page */
}

void gc_init(gc_tracer trace, gc_finalizer finalize)
{
  gc_heap *H = cur_heap();
  H->tracer = trace;
  H->finalizer = finalize;
  H->young = H->old = 0;
  H->young_bytes = H->old_bytes = H->alloc_since = 0;
  H->threshold = MIN_THRESHOLD;
  H->major_threshold = MIN_MAJOR;
  H->force_major = 0;
  H->collecting_minor = 0;
  H->nroots = H->nranges = H->nhandles = H->nscopes = H->lost_scopes = 0;
  H->grey_sp = 0;
  H->remset_n = H->rescan_n = 0;
}

void gc_set_rescan_kind(int kind) { cur_heap()->rescan_kind = kind; }

/* ------------------------------------------------------------------ marking */
static void mark(gc_heap *H, void *p)
{
  if (!p)
    return;
  gc_obj *h = hdr(p);
  if (H->collecting_minor && h->gen == GEN_OLD)
    return; /* old is assumed live  */
  if (h->mark != WHITE)
    return; /* already grey or black */
  h->mark = GREY;
  if (H->grey_sp < H->grey_cap) {
    H->grey[H->grey_sp++] = p;
    return;
  }
  if (grow((void ***)&H->grey, &H->grey_cap, H->grey_sp + 1))
    H->grey[H->grey_sp++] = p;
  else
    H->grey_overflow = 1; /* rescan pass will find it              */
}

void gc_mark(void *p) { mark(cur_heap(), p); }

static void mark_roots(gc_heap *H)
{
  for (int i = 0; i < H->nroots; i++)
    mark(H, *H->roots[i]);
  for (int i = 0; i < H->nranges; i++) {
    int n = *H->ranges[i].count;
    for (int j = 0; j < n; j++)
      mark(H, H->ranges[i].base[j]);
  }
  for (int i = 0; i < H->nhandles; i++)
    mark(H, H->handles[i]);
}

/* Drain the grey worklist, tracing each object. `full` selects the fallback
 * rescan list: only the young list can hold grey objects during a minor. */
static void drain(gc_heap *H, int full)
{
  for (;;) {
    while (H->grey_sp > 0) {
      void *o = H->grey[--H->grey_sp];
      gc_obj *h = hdr(o);
      if (h->mark == BLACK)
        continue;
      h->mark = BLACK;
#ifdef GC_PROFILE
      H->stats.marked++;
#endif
      if (H->tracer)
        H->tracer(o, h->kind);
    }
    if (!H->grey_overflow)
      return;
    /* Worklist couldn't hold everything: sweep for stranded grey objects,
     * blacken and trace them. Marks only move white->grey->black, so repeated
     * passes make monotonic progress and terminate. */
    H->grey_overflow = 0;
    int found = 0;
    for (int pass = 0; pass < 2; pass++) {
      gc_obj *list = pass == 0 ? H->young : H->old;
      if (pass == 1 && !full)
        break; /* old holds no grey during a minor */
      for (gc_obj *o = list; o; o = o->next)
        if (o->mark == GREY) {
          o->mark = BLACK;
          if (H->tracer)
            H->tracer(body(o), o->kind);
          found = 1;
        }
    }
    if (!found && H->grey_sp == 0)
      return;
  }
}

/* Sweep one generation list: free the white objects, and run `on_survivor` for
 * each survivor (resetting its mark). Returns bytes freed and advances *pbytes.
 */
static unsigned sweep(gc_heap *H, gc_obj **list, unsigned *pbytes, void (*on_survivor)(gc_obj *))
{
  unsigned freed = 0;
  gc_obj **pp = list;
  while (*pp) {
    gc_obj *o = *pp;
#ifdef GC_PROFILE
    H->stats.swept++;
#endif
    if (o->mark == WHITE) {
      *pp = o->next;
      if (H->finalizer)
        H->finalizer(body(o), o->kind);
      *pbytes -= o->size;
      freed += o->size;
#ifdef GC_PROFILE
      H->stats.freed++;
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

static void note_rescan(gc_heap *H, gc_obj *o)
{
  if (o->kind == H->rescan_kind && grow((void ***)&H->rescan, &H->rescan_cap, H->rescan_n + 1))
    H->rescan[H->rescan_n++] = o;
}

/* A minor sweep unlinks each young survivor and promotes it to the old list. */
static void promote_survivor(gc_obj *o) { (void)o; /* handled inline in gc_minor: promotion needs to relink lists */ }

static void set_threshold(gc_heap *H)
{
  unsigned want = H->old_bytes;
  if (want < MIN_THRESHOLD)
    want = MIN_THRESHOLD;
  if (want > NURSERY_CAP)
    want = NURSERY_CAP;
  H->threshold = want;
  H->alloc_since = 0;
}

static void clear_remset(gc_heap *H)
{
  for (int i = 0; i < H->remset_n; i++)
    H->remset[i]->rem = 0;
  H->remset_n = 0;
}

/* ---------------------------------------------------------------- minor GC */
static void gc_minor(gc_heap *H)
{
  H->stats.collections++;
  H->stats.minors++;
#ifdef GC_PROFILE
  double t0 = gc_now_ms();
#endif
  H->collecting_minor = 1;
  H->grey_sp = 0;
  H->grey_overflow = 0;

  mark_roots(H);
  /* old->young edges: the remembered set, plus every old rescan-kind object. */
  for (int i = 0; i < H->remset_n; i++)
    if (H->tracer)
      H->tracer(body(H->remset[i]), H->remset[i]->kind);
  for (int i = 0; i < H->rescan_n; i++)
    if (H->tracer)
      H->tracer(body(H->rescan[i]), H->rescan[i]->kind);
  drain(H, 0);

  /* Sweep the young list: free the dead, promote survivors to old. */
  {
    gc_obj **pp = &H->young;
    while (*pp) {
      gc_obj *o = *pp;
#ifdef GC_PROFILE
      H->stats.swept++;
#endif
      if (o->mark == WHITE) {
        *pp = o->next;
        if (H->finalizer)
          H->finalizer(body(o), o->kind);
        H->young_bytes -= o->size;
#ifdef GC_PROFILE
        H->stats.freed++;
#endif
        free(o);
      } else {
        o->mark = WHITE;
        o->gen = GEN_OLD;
        H->young_bytes -= o->size;
        H->old_bytes += o->size;
        *pp = o->next; /* unlink from young                    */
        o->next = H->old;
        H->old = o; /* prepend to old                       */
        note_rescan(H, o);
      }
    }
  }

  clear_remset(H); /* every remembered old->young is now old->old       */
  H->collecting_minor = 0;
  set_threshold(H);
#ifdef GC_PROFILE
  {
    double _d = gc_now_ms() - t0;
    H->stats.ms += _d;
    if (_d > H->stats.max_ms)
      H->stats.max_ms = _d;
  }
#endif
}

/* ---------------------------------------------------------------- major GC */
static unsigned gc_major(gc_heap *H)
{
  H->stats.collections++;
  H->stats.majors++;
#ifdef GC_PROFILE
  double t0 = gc_now_ms();
#endif
  H->collecting_minor = 0;
  H->grey_sp = 0;
  H->grey_overflow = 0;

  mark_roots(H);
  drain(H, 1);

  /* Sweep old first (survivors keep their generation), then young (survivors
   * promote to old). Rebuild the rescan list from the surviving old objects. */
  unsigned freed = sweep(H, &H->old, &H->old_bytes, promote_survivor);
  {
    gc_obj **pp = &H->young;
    while (*pp) {
      gc_obj *o = *pp;
#ifdef GC_PROFILE
      H->stats.swept++;
#endif
      if (o->mark == WHITE) {
        *pp = o->next;
        if (H->finalizer)
          H->finalizer(body(o), o->kind);
        H->young_bytes -= o->size;
        freed += o->size;
#ifdef GC_PROFILE
        H->stats.freed++;
#endif
        free(o);
      } else {
        o->mark = WHITE;
        o->gen = GEN_OLD;
        H->young_bytes -= o->size;
        H->old_bytes += o->size;
        *pp = o->next;
        o->next = H->old;
        H->old = o;
      }
    }
  }

  /* Rebuild the rescan list and clear stale remembered-set flags: both are
   * repopulated by promotion and the barrier from here on. */
  H->rescan_n = 0;
  for (gc_obj *o = H->old; o; o = o->next) {
    o->rem = 0;
    note_rescan(H, o);
  }
  H->remset_n = 0;

  H->force_major = 0;
  H->major_threshold = H->old_bytes * 2;
  if (H->major_threshold < MIN_MAJOR)
    H->major_threshold = MIN_MAJOR;
  set_threshold(H);
#ifdef GC_PROFILE
  {
    double _d = gc_now_ms() - t0;
    H->stats.ms += _d;
    if (_d > H->stats.max_ms)
      H->stats.max_ms = _d;
  }
#endif
  return freed;
}

/* Public: force a full reclamation. */
unsigned gc_collect(void) { return gc_major(cur_heap()); }

/* Choose a collection when the nursery budget is spent. */
static void gc_step(gc_heap *H)
{
  if (H->force_major || H->old_bytes > H->major_threshold)
    gc_major(H);
  else
    gc_minor(H);
}

/* ------------------------------------------------------------ write barrier */
void gc_write_barrier(void *container)
{
  gc_heap *H = cur_heap();
  if (!container)
    return;
  gc_obj *h = hdr(container);
  if (h->gen != GEN_OLD || h->rem)
    return;
  if (h->kind == H->rescan_kind)
    return; /* scanned wholesale; no need to log */
  if (grow((void ***)&H->remset, &H->remset_cap, H->remset_n + 1)) {
    h->rem = 1;
    H->remset[H->remset_n++] = h;
  } else {
    H->force_major = 1; /* can't remember this edge: next GC must be a major */
  }
}

/* --------------------------------------------------------------- allocation */
void *gc_alloc(unsigned size, int kind)
{
  gc_heap *H = cur_heap();
#ifdef GC_TORTURE
  /* Collect on every allocation, forcing a major periodically. Every object is
   * then one allocation from reclamation, so a missing write barrier or rooting
   * bug surfaces immediately as a use-after-free (run under AddressSanitizer).
   */
  if (H->tracer) {
    static unsigned tc;
    if ((++tc & 63) == 0)
      H->force_major = 1;
    gc_step(H);
  }
#else
  if (H->alloc_since + size > H->threshold)
    gc_step(H);
#endif

  gc_obj *o = malloc(sizeof(gc_obj) + size);
  if (!o) {
    gc_major(H);
    o = malloc(sizeof(gc_obj) + size);
    if (!o) {
      gc_oom();
      return 0;
    }
  }

  memset(o, 0, sizeof(gc_obj) + size); /* zero header + payload (NULL kids) */
  o->size = size;
  o->kind = (unsigned char)kind;
  o->mark = WHITE;
  o->gen = GEN_YOUNG;
  o->next = H->young;
  H->young = o;

  H->young_bytes += size;
  H->alloc_since += size;
  return body(o);
}

/* -------------------------------------------------------------------- roots */
void gc_add_root(void **slot)
{
  gc_heap *H = cur_heap();
  if (!grow((void ***)&H->roots, &H->caproots, H->nroots + 1))
    return;
  H->roots[H->nroots++] = slot;
}

void gc_remove_root(void **slot)
{
  gc_heap *H = cur_heap();
  for (int i = 0; i < H->nroots; i++)
    if (H->roots[i] == slot) {
      H->roots[i] = H->roots[--H->nroots];
      return;
    }
}

void gc_add_root_range(void **base, int *count)
{
  gc_heap *H = cur_heap();
  if (H->nranges == H->capranges) {
    int nc = H->capranges ? H->capranges * 2 : 8;
    struct range *nr = realloc(H->ranges, (unsigned)nc * sizeof(struct range));
    if (!nr)
      return;
    H->ranges = nr;
    H->capranges = nc;
  }
  H->ranges[H->nranges].base = base;
  H->ranges[H->nranges].count = count;
  H->nranges++;
}

void gc_remove_root_range(void **base)
{
  gc_heap *H = cur_heap();
  for (int i = 0; i < H->nranges; i++)
    if (H->ranges[i].base == base) {
      H->ranges[i] = H->ranges[--H->nranges];
      return;
    }
}

/* ------------------------------------------------------------ handle scopes */
void gc_push_scope(void)
{
  gc_heap *H = cur_heap();
  if (!grow((void ***)&H->scopes, &H->capscopes, H->nscopes + 1)) {
    H->lost_scopes++;
    return;
  }
  H->scopes[H->nscopes++] = H->nhandles;
}

void gc_pop_scope(void)
{
  gc_heap *H = cur_heap();
  if (H->lost_scopes > 0)
    H->lost_scopes--;
  else if (H->nscopes > 0)
    H->nhandles = H->scopes[--H->nscopes];
}

void *gc_keep(void *obj)
{
  gc_heap *H = cur_heap();
  if (grow((void ***)&H->handles, &H->caphandles, H->nhandles + 1))
    H->handles[H->nhandles++] = obj;
  return obj;
}

void gc_scope_pos(int *nh, int *ns)
{
  gc_heap *H = cur_heap();
  *nh = H->nhandles;
  *ns = H->nscopes;
}

void gc_scope_take(int nh, int ns, gc_stash *st)
{
  gc_heap *H = cur_heap();
  st->nh = H->nhandles - nh;
  st->ns = H->nscopes - ns;
  st->h = st->nh ? malloc((unsigned)st->nh * sizeof *st->h) : NULL;
  st->s = st->ns ? malloc((unsigned)st->ns * sizeof *st->s) : NULL;
  if ((st->nh && !st->h) || (st->ns && !st->s))
    gc_oom();
  for (int i = 0; i < st->nh; i++)
    st->h[i] = H->handles[nh + i];
  for (int i = 0; i < st->ns; i++)
    st->s[i] = H->scopes[ns + i] - nh;
  H->nhandles = nh;
  H->nscopes = ns;
}

void gc_scope_put(gc_stash *st)
{
  gc_heap *H = cur_heap();
  int base = H->nhandles;
  if (!grow((void ***)&H->handles, &H->caphandles, H->nhandles + st->nh) ||
      !grow((void ***)&H->scopes, &H->capscopes, H->nscopes + st->ns))
    gc_oom();
  for (int i = 0; i < st->nh; i++)
    H->handles[H->nhandles++] = st->h[i];
  for (int i = 0; i < st->ns; i++)
    H->scopes[H->nscopes++] = st->s[i] + base;
  gc_scope_drop(st);
}

void gc_scope_trace(const gc_stash *st)
{
  for (int i = 0; i < st->nh; i++)
    gc_mark(st->h[i]);
}

void gc_scope_drop(gc_stash *st)
{
  free(st->h);
  free(st->s);
  st->h = NULL;
  st->s = NULL;
  st->nh = st->ns = 0;
}

/* --------------------------------------------------------------------- misc */
unsigned gc_bytes(void)
{
  gc_heap *H = cur_heap();
  return H->young_bytes + H->old_bytes;
}
void gc_get_stats(gc_stats_t *out)
{
  if (out)
    *out = cur_heap()->stats;
}
void gc_set_threshold(unsigned bytes) { cur_heap()->threshold = bytes ? bytes : MIN_THRESHOLD; }
