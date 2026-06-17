/* gc.c — precise mark-sweep collector. See gc.h for the contract.
 *
 * Each object carries a 16-byte header (kept 16 so the payload stays 8-aligned
 * for doubles) and is threaded onto one all-objects list. Marking is iterative
 * over an explicit grey worklist — never C recursion — so a deep object graph
 * can't overflow the user stack; if the worklist itself can't grow under memory
 * pressure, a whole-heap rescan fallback finishes the job. Sweep walks the list
 * and frees the white objects back to the K&R heap.
 */

#include "owos.h"
#include "gc.h"

enum { WHITE = 0, GREY = 1, BLACK = 2 };

typedef struct gc_obj {
	struct gc_obj *next;
	unsigned       size;            /* payload bytes                            */
	int            mark;
	int            kind;
} gc_obj;                           /* 16 bytes on ILP32 -> 8-aligned payload   */

#define DEFAULT_THRESHOLD (256u * 1024u)
#define MIN_THRESHOLD     (64u * 1024u)

static gc_obj      *objects;
static unsigned     live_bytes;
static unsigned     alloc_since;
static unsigned     threshold = DEFAULT_THRESHOLD;
static gc_tracer    tracer;
static gc_finalizer finalizer;

/* roots: addresses of pointer variables. */
static void      ***roots;
static int          nroots, caproots;
/* root ranges: a base array plus a live count. */
struct range { void **base; int *count; };
static struct range *ranges;
static int           nranges, capranges;
/* handle scopes: pinned local pointers, with per-scope saved high-water marks. */
static void        **handles;
static int           nhandles, caphandles;
static int          *scopes;
static int           nscopes, capscopes;
/* grey worklist. */
static void        **grey;
static int           grey_sp, grey_cap, grey_overflow;

static inline gc_obj *hdr(void *payload) { return (gc_obj *)payload - 1; }

/* Grow a void* array to hold one more; returns 0 on OOM (caller copes). */
static int grow(void ***a, int *cap, int need)
{
	if (need <= *cap) return 1;
	int nc = *cap ? *cap * 2 : 32;
	while (nc < need) nc *= 2;
	void **na = realloc(*a, (unsigned)nc * sizeof(void *));
	if (!na) return 0;
	*a = na; *cap = nc;
	return 1;
}

void gc_init(gc_tracer trace, gc_finalizer finalize)
{
	tracer = trace;
	finalizer = finalize;
	objects = 0;
	live_bytes = alloc_since = 0;
	threshold = DEFAULT_THRESHOLD;
	nroots = nranges = nhandles = nscopes = 0;
	grey_sp = 0;
}

/* ------------------------------------------------------------------ marking */
void gc_mark(void *p)
{
	if (!p) return;
	gc_obj *h = hdr(p);
	if (h->mark != WHITE) return;       /* already grey or black                */
	h->mark = GREY;
	if (grey_sp < grey_cap) { grey[grey_sp++] = p; return; }
	if (grow((void ***)&grey, &grey_cap, grey_sp + 1)) grey[grey_sp++] = p;
	else grey_overflow = 1;             /* rescan pass will find it             */
}

static void mark_roots(void)
{
	for (int i = 0; i < nroots; i++) gc_mark(*roots[i]);
	for (int i = 0; i < nranges; i++) {
		int n = *ranges[i].count;
		for (int j = 0; j < n; j++) gc_mark(ranges[i].base[j]);
	}
	for (int i = 0; i < nhandles; i++) gc_mark(handles[i]);
}

static void drain(void)
{
	for (;;) {
		while (grey_sp > 0) {
			void *o = grey[--grey_sp];
			gc_obj *h = hdr(o);
			if (h->mark == BLACK) continue;
			h->mark = BLACK;
			if (tracer) tracer(o, h->kind);
		}
		if (!grey_overflow) return;
		/* Worklist couldn't hold everything: sweep the heap for stranded grey
		 * objects, blacken and trace them. Marks only move white->grey->black,
		 * so repeated passes make monotonic progress and terminate. */
		grey_overflow = 0;
		int found = 0;
		for (gc_obj *o = objects; o; o = o->next) {
			if (o->mark == GREY) {
				o->mark = BLACK;
				if (tracer) tracer((void *)(o + 1), o->kind);
				found = 1;
			}
		}
		if (!found && grey_sp == 0) return;
	}
}

unsigned gc_collect(void)
{
	grey_sp = 0;
	grey_overflow = 0;

	mark_roots();
	drain();

	unsigned freed = 0;
	gc_obj **pp = &objects;
	while (*pp) {
		gc_obj *o = *pp;
		if (o->mark == WHITE) {
			*pp = o->next;
			if (finalizer) finalizer((void *)(o + 1), o->kind);
			live_bytes -= o->size;
			freed += o->size;
			free(o);
		} else {
			o->mark = WHITE;            /* reset for the next cycle             */
			pp = &o->next;
		}
	}

	alloc_since = 0;
	unsigned want = live_bytes * 2;
	if (want < MIN_THRESHOLD) want = MIN_THRESHOLD;
	threshold = want;
	return freed;
}

/* --------------------------------------------------------------- allocation */
void *gc_alloc(unsigned size, int kind)
{
	if (alloc_since + size > threshold) gc_collect();

	gc_obj *o = malloc(sizeof(gc_obj) + size);
	if (!o) { gc_collect(); o = malloc(sizeof(gc_obj) + size); if (!o) return 0; }

	memset(o, 0, sizeof(gc_obj) + size);   /* zero header + payload (NULL kids) */
	o->size = size;
	o->kind = kind;
	o->mark = WHITE;
	o->next = objects;
	objects = o;

	live_bytes += size;
	alloc_since += size;
	return (void *)(o + 1);
}

/* -------------------------------------------------------------------- roots */
void gc_add_root(void **slot)
{
	if (!grow((void ***)&roots, &caproots, nroots + 1)) return;
	roots[nroots++] = slot;
}

void gc_remove_root(void **slot)
{
	for (int i = 0; i < nroots; i++)
		if (roots[i] == slot) { roots[i] = roots[--nroots]; return; }
}

void gc_add_root_range(void **base, int *count)
{
	if (nranges == capranges) {
		int nc = capranges ? capranges * 2 : 8;
		struct range *nr = realloc(ranges, (unsigned)nc * sizeof(struct range));
		if (!nr) return;
		ranges = nr; capranges = nc;
	}
	ranges[nranges].base = base;
	ranges[nranges].count = count;
	nranges++;
}

void gc_remove_root_range(void **base)
{
	for (int i = 0; i < nranges; i++)
		if (ranges[i].base == base) { ranges[i] = ranges[--nranges]; return; }
}

/* ------------------------------------------------------------ handle scopes */
void gc_push_scope(void)
{
	if (!grow((void ***)&scopes, &capscopes, nscopes + 1)) return;
	scopes[nscopes++] = nhandles;
}

void gc_pop_scope(void)
{
	if (nscopes > 0) nhandles = scopes[--nscopes];
}

void *gc_keep(void *obj)
{
	if (grow((void ***)&handles, &caphandles, nhandles + 1)) handles[nhandles++] = obj;
	return obj;
}

/* --------------------------------------------------------------------- misc */
unsigned gc_bytes(void) { return live_bytes; }
void gc_set_threshold(unsigned bytes) { threshold = bytes ? bytes : MIN_THRESHOLD; }
