/* wasm_run.c: instances, memories, tables and the bytecode interpreter. See wasm.h. */
#include "owos.h"
#include "wasm.h"
#include "wasm_int.h"

enum { VSTACK_SLOTS = 1 << 17, MAX_FRAMES = 12000 };

static const char *const OOB = "memory access out of bounds";

/* The guest computes in x87 extended precision, which rounds twice on the way to a double. While wasm runs the
 * precision control is narrowed to 53 bits so f32 and f64 operators round once, like IEEE hardware. */
#if defined(__i386__) && !defined(__SSE_MATH__)
static unsigned short fpu_narrow(void)
{
  unsigned short old, now;
  __asm__ volatile("fnstcw %0" : "=m"(old));
  now = (unsigned short)((old & ~0x300) | 0x200);
  __asm__ volatile("fldcw %0" : : "m"(now));
  return old;
}
static void fpu_set(unsigned short cw) { __asm__ volatile("fldcw %0" : : "m"(cw)); }
#else
static unsigned short fpu_narrow(void) { return 0; }
static void fpu_set(unsigned short cw) { (void)cw; }
#endif

/* ---- memories and tables ------------------------------------------------- */

int wasm_memory_alloc(wasm_memory *m, uint32_t pages)
{
  m->data = NULL;
  m->size = 0;
  m->pages = 0;
  if (pages) {
    m->data = (uint8_t *)calloc(1, (unsigned)(pages * WASM_PAGE));
    if (!m->data)
      return -1;
  }
  m->pages = pages;
  m->size = (uint64_t)pages * WASM_PAGE;
  return 0;
}

int64_t wasm_memory_grow(wasm_memory *m, uint32_t delta)
{
  uint32_t old = m->pages, limit = m->has_max ? m->max : WASM_MAX_PAGES;
  uint8_t *nd;
  if (limit > WASM_MAX_PAGES)
    limit = WASM_MAX_PAGES;
  if (delta > limit || old + delta > limit)
    return -1;
  if (delta) {
    nd = (uint8_t *)realloc(m->data, (unsigned)((old + delta) * WASM_PAGE));
    if (!nd)
      return -1;
    memset(nd + (size_t)old * WASM_PAGE, 0, (size_t)delta * WASM_PAGE);
    m->data = nd;
    m->pages = old + delta;
    m->size = (uint64_t)m->pages * WASM_PAGE;
  }
  if (m->on_grow)
    m->on_grow(m);
  return old;
}

void wasm_memory_free(wasm_memory *m)
{
  free(m->data);
  m->data = NULL;
  m->size = 0;
}

int wasm_table_alloc(wasm_table *t, uint32_t n)
{
  t->f = NULL;
  t->x = NULL;
  if (n) {
    if (t->type == WASM_FUNCREF)
      t->f = (wasm_func **)calloc(n, sizeof *t->f);
    else
      t->x = (wasm_extern *)calloc(n, sizeof *t->x);
    if (!t->f && !t->x)
      return -1;
  }
  t->n = n;
  return 0;
}

int64_t wasm_table_grow(wasm_table *t, uint32_t delta, wasm_func *f, const wasm_extern *x)
{
  uint32_t old = t->n, limit = t->has_max ? t->max : WASM_MAX_TABLE, i;
  if (limit > WASM_MAX_TABLE)
    limit = WASM_MAX_TABLE;
  if (delta > limit || old + delta > limit)
    return -1;
  if (delta == 0)
    return old;
  if (t->type == WASM_FUNCREF) {
    wasm_func **nf = (wasm_func **)realloc(t->f, (old + delta) * sizeof *nf);
    if (!nf)
      return -1;
    t->f = nf;
    for (i = old; i < old + delta; i++)
      nf[i] = f;
  } else {
    wasm_extern *nx = (wasm_extern *)realloc(t->x, (old + delta) * sizeof *nx);
    if (!nx)
      return -1;
    t->x = nx;
    for (i = old; i < old + delta; i++)
      nx[i] = *x;
  }
  t->n = old + delta;
  return old;
}

void wasm_table_free(wasm_table *t)
{
  free(t->f);
  free(t->x);
  t->f = NULL;
  t->x = NULL;
  t->n = 0;
}

int wasm_limits_match(uint32_t cur, int has_max, uint32_t max, const wasm_limits *want)
{
  if (cur < want->min)
    return 0;
  if (want->has_max && (!has_max || max > want->max))
    return 0;
  return 1;
}

/* ---- instances ----------------------------------------------------------- */

wasm_instance *wasm_instance_new(const wasm_module *m)
{
  wasm_instance *i = (wasm_instance *)calloc(1, sizeof *i);
  if (!i)
    return NULL;
  i->module = m;
  i->nelems = m->nelems;
  i->funcs = (wasm_func **)calloc(m->nfuncs + 1, sizeof *i->funcs);
  i->tables = (wasm_table **)calloc(m->ntables + 1, sizeof *i->tables);
  i->mems = (wasm_memory **)calloc(m->nmems + 1, sizeof *i->mems);
  i->globals = (wasm_global **)calloc(m->nglobals + 1, sizeof *i->globals);
  i->store = (wasm_func *)calloc(m->nfuncs + 1, sizeof *i->store);
  i->elem_f = (wasm_func ***)calloc(m->nelems + 1, sizeof *i->elem_f);
  i->elem_n = (uint32_t *)calloc(m->nelems + 1, sizeof *i->elem_n);
  i->data_dropped = (uint8_t *)calloc(m->ndatas + 1, 1);
  if (!i->funcs || !i->tables || !i->mems || !i->globals || !i->store || !i->elem_f || !i->elem_n || !i->data_dropped) {
    wasm_instance_free(i);
    return NULL;
  }
  return i;
}

void wasm_instance_free(wasm_instance *i)
{
  uint32_t k;
  if (!i)
    return;
  if (i->elem_f)
    for (k = 0; k < i->nelems; k++)
      free(i->elem_f[k]);
  free(i->funcs);
  free(i->tables);
  free(i->mems);
  free(i->globals);
  free(i->store);
  free(i->elem_f);
  free(i->elem_n);
  free(i->data_dropped);
  free(i);
}

void wasm_eval_const(const wasm_instance *i, const wasm_constexpr *e, uint64_t out[2])
{
  out[0] = out[1] = 0;
  switch (e->op) {
  case 0x23:
    out[0] = i->globals[e->v[0]]->v[0];
    out[1] = i->globals[e->v[0]]->v[1];
    break;
  case 0xd2:
    out[0] = (uint64_t)(uintptr_t)i->funcs[e->v[0]];
    break;
  case 0xd0:
    break;
  default:
    out[0] = e->v[0];
    out[1] = e->v[1];
  }
}

int wasm_instance_define(wasm_instance *i)
{
  const wasm_module *m = i->module;
  uint32_t k, j;
  for (k = m->nfunc_imports; k < m->nfuncs; k++) {
    wasm_func *f = &i->store[k];
    f->type = &m->types[m->func_type[k]];
    f->inst = i;
    f->code = &m->code[k - m->nfunc_imports];
    f->index = k;
    i->funcs[k] = f;
  }
  for (k = m->ntable_imports; k < m->ntables; k++) {
    wasm_table *t = (wasm_table *)calloc(1, sizeof *t);
    if (!t)
      return -1;
    i->tables[k] = t;
    t->type = m->tables[k].elem;
    t->has_max = m->tables[k].lim.has_max;
    t->max = m->tables[k].lim.max;
    if (wasm_table_alloc(t, m->tables[k].lim.min))
      return -1;
  }
  for (k = m->nmem_imports; k < m->nmems; k++) {
    wasm_memory *mem = (wasm_memory *)calloc(1, sizeof *mem);
    if (!mem)
      return -1;
    i->mems[k] = mem;
    mem->has_max = m->mems[k].has_max;
    mem->max = m->mems[k].max;
    if (wasm_memory_alloc(mem, m->mems[k].min))
      return -1;
  }
  for (k = m->nglobal_imports; k < m->nglobals; k++) {
    wasm_global *g = (wasm_global *)calloc(1, sizeof *g);
    if (!g)
      return -1;
    i->globals[k] = g;
    g->type = m->globals[k].type;
    g->mut = m->globals[k].mut;
    wasm_eval_const(i, &m->global_init[k - m->nglobal_imports], g->v);
  }
  for (k = 0; k < m->nelems; k++) {
    const wasm_elem *e = &m->elems[k];
    i->elem_f[k] = (wasm_func **)calloc(e->n + 1, sizeof(wasm_func *));
    if (!i->elem_f[k])
      return -1;
    for (j = 0; j < e->n; j++)
      i->elem_f[k][j] = e->funcs[j] < 0 ? NULL : i->funcs[e->funcs[j]];
    i->elem_n[k] = e->mode == 2 ? 0 : e->n;
  }
  return 0;
}

const char *wasm_instance_apply_segments(wasm_instance *i)
{
  const wasm_module *m = i->module;
  uint32_t k;
  for (k = 0; k < m->nelems; k++) {
    const wasm_elem *e = &m->elems[k];
    uint64_t off[2];
    wasm_table *t;
    if (e->mode != 0)
      continue;
    wasm_eval_const(i, &e->offset, off);
    t = i->tables[e->table];
    if ((uint64_t)(uint32_t)off[0] + e->n > t->n)
      return "table index is out of bounds";
    if (t->type == WASM_FUNCREF)
      memcpy(t->f + (uint32_t)off[0], i->elem_f[k], e->n * sizeof(wasm_func *));
    else
      memset(t->x + (uint32_t)off[0], 0, e->n * sizeof *t->x);
    i->elem_n[k] = 0;
  }
  for (k = 0; k < m->ndatas; k++) {
    const wasm_data *d = &m->datas[k];
    uint64_t off[2];
    if (!d->active)
      continue;
    wasm_eval_const(i, &d->offset, off);
    if ((uint64_t)(uint32_t)off[0] + d->len > i->mems[0]->size)
      return "data segment is out of bounds";
    memcpy(i->mems[0]->data + (uint32_t)off[0], d->bytes, d->len);
    i->data_dropped[k] = 1;
  }
  return NULL;
}

/* ---- execution contexts -------------------------------------------------- */

wasm_exec *wasm_exec_new(void *ud)
{
  wasm_exec *x = (wasm_exec *)calloc(1, sizeof *x);
  if (!x)
    return NULL;
  x->stack = (uint64_t *)malloc(VSTACK_SLOTS * sizeof *x->stack);
  x->frames = (wasm_frame *)malloc(MAX_FRAMES * sizeof *x->frames);
  if (!x->stack || !x->frames) {
    wasm_exec_free(x);
    return NULL;
  }
  x->sp = x->stack;
  x->stack_end = x->stack + VSTACK_SLOTS;
  x->maxframes = MAX_FRAMES;
  x->ud = ud;
  return x;
}

void wasm_exec_free(wasm_exec *x)
{
  if (!x)
    return;
  free(x->stack);
  free(x->frames);
  free(x);
}

/* ---- interpreter --------------------------------------------------------- */

static uint32_t popcnt32(uint32_t v)
{
  v = v - ((v >> 1) & 0x55555555u);
  v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
  v = (v + (v >> 4)) & 0x0f0f0f0fu;
  return (v * 0x01010101u) >> 24;
}

static int run(wasm_exec *x, wasm_func *entry, uint64_t *base)
{
  wasm_func *f = entry, *g;
  wasm_instance *inst = f->inst;
  const uint32_t *code = f->code->code, *pc = code;
  uint64_t *fp = base, *sp;
  wasm_memory *mem;
  const uint32_t entry_frames = x->nframes;
  const char *trap;
  uint32_t i, nl;

#define SETMEM() (mem = inst->module->nmems ? inst->mems[0] : NULL)
#define TRAP(msg)                                                                                                      \
  do {                                                                                                                 \
    trap = (msg);                                                                                                      \
    goto do_trap;                                                                                                      \
  } while (0)

  nl = f->code->nlocals;
  if (fp + nl + f->code->maxstack > x->stack_end)
    TRAP("call stack exhausted");
  for (i = f->type->pslots; i < nl; i++)
    fp[i] = 0;
  sp = fp + nl;
  SETMEM();

  for (;;) {
    uint32_t op = *pc++;
    switch (op) {
    case 0x00:
      TRAP("unreachable");
    case OP_JMP:
      pc = code + *pc;
      break;
    case OP_JMP_IF:
      pc = (uint32_t)*--sp ? code + *pc : pc + 1;
      break;
    case OP_JMP_UNLESS:
      pc = (uint32_t)*--sp ? pc + 1 : code + *pc;
      break;
    case OP_BR:
    do_br:
      {
        uint32_t ar = pc[1];
        uint64_t *dst = fp + pc[2];
        if (ar)
          memmove(dst, sp - ar, ar * sizeof *sp);
        sp = dst + ar;
        pc = code + pc[0];
        break;
      }
    case OP_BR_IF:
      if ((uint32_t)*--sp)
        goto do_br;
      pc += 3;
      break;
    case OP_BR_TABLE: {
      uint32_t n = pc[0], idx = (uint32_t)*--sp;
      pc += 1 + 3 * (idx < n ? idx : n);
      goto do_br;
    }
    case OP_RETURN: {
      uint32_t rs = f->type->rslots;
      memmove(fp, sp - rs, rs * sizeof *sp);
      sp = fp + rs;
      if (x->nframes == entry_frames)
        return 0;
      {
        wasm_frame *fr = &x->frames[--x->nframes];
        f = fr->fn;
        pc = fr->pc;
        fp = fr->fp;
        inst = f->inst;
        code = f->code->code;
        SETMEM();
      }
      break;
    }
    case 0x10:
      g = inst->funcs[*pc++];
      goto call_g;
    case 0x11: {
      uint32_t ti = pc[0];
      wasm_table *t = inst->tables[pc[1]];
      uint32_t idx = (uint32_t)*--sp;
      pc += 2;
      if (idx >= t->n)
        TRAP("table index is out of bounds");
      g = t->f[idx];
      if (!g || !wasm_functype_eq(g->type, &inst->module->types[ti]))
        TRAP("function signature mismatch");
      goto call_g;
    }
    call_g:
      if (!g->code) {
        uint32_t ps = g->type->pslots, rs = g->type->rslots;
        uint64_t *args = sp - ps;
        if (args + rs + 64 > x->stack_end)
          TRAP("call stack exhausted");
        x->sp = args + (ps > rs ? ps : rs);
        fpu_set(x->fpu_cw);
        if (x->host_call(x->ud, g, args)) {
          fpu_narrow();
          return -2;
        }
        fpu_narrow();
        sp = args + rs;
        SETMEM();
        break;
      }
      if (x->nframes >= x->maxframes)
        TRAP("call stack exhausted");
      x->frames[x->nframes].fn = f;
      x->frames[x->nframes].pc = pc;
      x->frames[x->nframes].fp = fp;
      x->nframes++;
      fp = sp - g->type->pslots;
      f = g;
      inst = f->inst;
      code = f->code->code;
      pc = code;
      nl = f->code->nlocals;
      if (fp + nl + f->code->maxstack > x->stack_end)
        TRAP("call stack exhausted");
      for (i = f->type->pslots; i < nl; i++)
        fp[i] = 0;
      sp = fp + nl;
      SETMEM();
      break;

    case 0x1a:
      sp--;
      break;
    case OP_DROP2:
      sp -= 2;
      break;
    case 0x1b:
      sp -= 2;
      if (!(uint32_t)sp[1])
        sp[-1] = sp[0];
      break;
    case OP_SELECT2:
      sp -= 3;
      if (!(uint32_t)sp[2]) {
        sp[-2] = sp[0];
        sp[-1] = sp[1];
      }
      break;

    case 0x20:
      *sp++ = fp[*pc++];
      break;
    case 0x21:
      fp[*pc++] = *--sp;
      break;
    case 0x22:
      fp[*pc++] = sp[-1];
      break;
    case OP_LOCAL_GET2:
      sp[0] = fp[pc[0]];
      sp[1] = fp[pc[0] + 1];
      sp += 2;
      pc++;
      break;
    case OP_LOCAL_SET2:
      sp -= 2;
      fp[pc[0]] = sp[0];
      fp[pc[0] + 1] = sp[1];
      pc++;
      break;
    case OP_LOCAL_TEE2:
      fp[pc[0]] = sp[-2];
      fp[pc[0] + 1] = sp[-1];
      pc++;
      break;
    case 0x23:
      *sp++ = inst->globals[*pc++]->v[0];
      break;
    case 0x24:
      inst->globals[*pc++]->v[0] = *--sp;
      break;
    case OP_GLOBAL_GET2: {
      wasm_global *gl = inst->globals[*pc++];
      sp[0] = gl->v[0];
      sp[1] = gl->v[1];
      sp += 2;
      break;
    }
    case OP_GLOBAL_SET2: {
      wasm_global *gl = inst->globals[*pc++];
      sp -= 2;
      gl->v[0] = sp[0];
      gl->v[1] = sp[1];
      break;
    }
    case OP_GLOBAL_GET_X: {
      wasm_global *gl = inst->globals[*pc++];
      *sp++ = (gl->v[0] | gl->v[1]) ? x->xr_intern(x->ud, (const wasm_extern *)gl->v) : 0;
      break;
    }
    case OP_GLOBAL_SET_X: {
      wasm_global *gl = inst->globals[*pc++];
      uint64_t h = *--sp;
      if (h)
        x->xr_value(x->ud, h, (wasm_extern *)gl->v);
      else
        gl->v[0] = gl->v[1] = 0;
      break;
    }
    case 0x25: {
      wasm_table *t = inst->tables[*pc++];
      uint32_t idx = (uint32_t)sp[-1];
      if (idx >= t->n)
        TRAP("table index is out of bounds");
      if (t->type == WASM_FUNCREF)
        sp[-1] = (uint64_t)(uintptr_t)t->f[idx];
      else
        sp[-1] = (t->x[idx].w[0] | t->x[idx].w[1]) ? x->xr_intern(x->ud, &t->x[idx]) : 0;
      break;
    }
    case 0x26: {
      wasm_table *t = inst->tables[*pc++];
      uint64_t v = sp[-1];
      uint32_t idx = (uint32_t)sp[-2];
      sp -= 2;
      if (idx >= t->n)
        TRAP("table index is out of bounds");
      if (t->type == WASM_FUNCREF)
        t->f[idx] = (wasm_func *)(uintptr_t)v;
      else if (v)
        x->xr_value(x->ud, v, &t->x[idx]);
      else
        memset(&t->x[idx], 0, sizeof t->x[idx]);
      break;
    }

#define EA(sz)                                                                                                         \
  uint64_t ea = (uint64_t)(uint32_t)sp[-1] + *pc++;                                                                    \
  if (ea + (sz) > mem->size)                                                                                           \
    TRAP(OOB);
#define LOAD(T, conv)                                                                                                  \
  {                                                                                                                    \
    EA(sizeof(T));                                                                                                     \
    T v;                                                                                                               \
    memcpy(&v, mem->data + ea, sizeof v);                                                                              \
    sp[-1] = (conv);                                                                                                   \
    break;                                                                                                             \
  }
#define STORE(T)                                                                                                       \
  {                                                                                                                    \
    uint64_t val = sp[-1];                                                                                             \
    T v = (T)val;                                                                                                      \
    sp--;                                                                                                              \
    EA(sizeof(T));                                                                                                     \
    memcpy(mem->data + ea, &v, sizeof v);                                                                              \
    sp--;                                                                                                              \
    break;                                                                                                             \
  }
    case 0x28:
      LOAD(uint32_t, v)
    case 0x29:
      LOAD(uint64_t, v)
    case 0x2a:
      LOAD(uint32_t, v)
    case 0x2b:
      LOAD(uint64_t, v)
    case 0x2c:
      LOAD(int8_t, (uint32_t)(int32_t)v)
    case 0x2d:
      LOAD(uint8_t, v)
    case 0x2e:
      LOAD(int16_t, (uint32_t)(int32_t)v)
    case 0x2f:
      LOAD(uint16_t, v)
    case 0x30:
      LOAD(int8_t, (uint64_t)(int64_t)v)
    case 0x31:
      LOAD(uint8_t, v)
    case 0x32:
      LOAD(int16_t, (uint64_t)(int64_t)v)
    case 0x33:
      LOAD(uint16_t, v)
    case 0x34:
      LOAD(int32_t, (uint64_t)(int64_t)v)
    case 0x35:
      LOAD(uint32_t, v)
    case 0x36:
      STORE(uint32_t)
    case 0x37:
      STORE(uint64_t)
    case 0x38:
      STORE(uint32_t)
    case 0x39:
      STORE(uint64_t)
    case 0x3a:
      STORE(uint8_t)
    case 0x3b:
      STORE(uint16_t)
    case 0x3c:
      STORE(uint8_t)
    case 0x3d:
      STORE(uint16_t)
    case 0x3e:
      STORE(uint32_t)
    case 0x3f:
      *sp++ = mem->pages;
      break;
    case 0x40:
      sp[-1] = (uint32_t)(int32_t)wasm_memory_grow(mem, (uint32_t)sp[-1]);
      break;

    case 0x41:
      *sp++ = *pc++;
      break;
    case 0x42:
    case 0x44:
      *sp++ = (uint64_t)pc[0] | ((uint64_t)pc[1] << 32);
      pc += 2;
      break;
    case 0x43:
      *sp++ = *pc++;
      break;

#define B32(e)                                                                                                         \
  {                                                                                                                    \
    uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];                                                               \
    int32_t sa = (int32_t)a, sb = (int32_t)b;                                                                          \
    (void)sa;                                                                                                          \
    (void)sb;                                                                                                          \
    sp--;                                                                                                              \
    sp[-1] = (uint32_t)(e);                                                                                            \
    break;                                                                                                             \
  }
#define B64(e)                                                                                                         \
  {                                                                                                                    \
    uint64_t b = sp[-1], a = sp[-2];                                                                                   \
    int64_t sa = (int64_t)a, sb = (int64_t)b;                                                                          \
    (void)sa;                                                                                                          \
    (void)sb;                                                                                                          \
    sp--;                                                                                                              \
    sp[-1] = (uint64_t)(e);                                                                                            \
    break;                                                                                                             \
  }
#define U32(e)                                                                                                         \
  {                                                                                                                    \
    uint32_t a = (uint32_t)sp[-1];                                                                                     \
    int32_t sa = (int32_t)a;                                                                                           \
    (void)sa;                                                                                                          \
    sp[-1] = (uint32_t)(e);                                                                                            \
    break;                                                                                                             \
  }
#define U64(e)                                                                                                         \
  {                                                                                                                    \
    uint64_t a = sp[-1];                                                                                               \
    int64_t sa = (int64_t)a;                                                                                           \
    (void)sa;                                                                                                          \
    sp[-1] = (uint64_t)(e);                                                                                            \
    break;                                                                                                             \
  }
#define BF32(e)                                                                                                        \
  {                                                                                                                    \
    float b = f32_of(sp[-1]), a = f32_of(sp[-2]);                                                                      \
    sp--;                                                                                                              \
    sp[-1] = (e);                                                                                                      \
    break;                                                                                                             \
  }
#define BF64(e)                                                                                                        \
  {                                                                                                                    \
    double b = f64_of(sp[-1]), a = f64_of(sp[-2]);                                                                     \
    sp--;                                                                                                              \
    sp[-1] = (e);                                                                                                      \
    break;                                                                                                             \
  }
#define UF32(e)                                                                                                        \
  {                                                                                                                    \
    float a = f32_of(sp[-1]);                                                                                          \
    sp[-1] = (e);                                                                                                      \
    break;                                                                                                             \
  }
#define UF64(e)                                                                                                        \
  {                                                                                                                    \
    double a = f64_of(sp[-1]);                                                                                         \
    sp[-1] = (e);                                                                                                      \
    break;                                                                                                             \
  }

    case 0x45:
      U32(a == 0)
    case 0x46:
      B32(a == b)
    case 0x47:
      B32(a != b)
    case 0x48:
      B32(sa < sb)
    case 0x49:
      B32(a < b)
    case 0x4a:
      B32(sa > sb)
    case 0x4b:
      B32(a > b)
    case 0x4c:
      B32(sa <= sb)
    case 0x4d:
      B32(a <= b)
    case 0x4e:
      B32(sa >= sb)
    case 0x4f:
      B32(a >= b)
    case 0x50:
      U64(a == 0)
    case 0x51:
      B64(a == b)
    case 0x52:
      B64(a != b)
    case 0x53:
      B64(sa < sb)
    case 0x54:
      B64(a < b)
    case 0x55:
      B64(sa > sb)
    case 0x56:
      B64(a > b)
    case 0x57:
      B64(sa <= sb)
    case 0x58:
      B64(a <= b)
    case 0x59:
      B64(sa >= sb)
    case 0x5a:
      B64(a >= b)
    case 0x5b:
      BF32(a == b)
    case 0x5c:
      BF32(a != b)
    case 0x5d:
      BF32(a < b)
    case 0x5e:
      BF32(a > b)
    case 0x5f:
      BF32(a <= b)
    case 0x60:
      BF32(a >= b)
    case 0x61:
      BF64(a == b)
    case 0x62:
      BF64(a != b)
    case 0x63:
      BF64(a < b)
    case 0x64:
      BF64(a > b)
    case 0x65:
      BF64(a <= b)
    case 0x66:
      BF64(a >= b)

    case 0x67:
      U32(a ? (uint32_t)__builtin_clz(a) : 32)
    case 0x68:
      U32(a ? (uint32_t)__builtin_ctz(a) : 32)
    case 0x69:
      U32(popcnt32(a))
    case 0x6a:
      B32(a + b)
    case 0x6b:
      B32(a - b)
    case 0x6c:
      B32(a * b)
    case 0x6d:
      if (!(uint32_t)sp[-1])
        TRAP("divide by zero");
      if ((int32_t)sp[-2] == INT32_MIN && (int32_t)sp[-1] == -1)
        TRAP("divide result unrepresentable");
      B32(sa / sb)
    case 0x6e:
      if (!(uint32_t)sp[-1])
        TRAP("divide by zero");
      B32(a / b)
    case 0x6f:
      if (!(uint32_t)sp[-1])
        TRAP("remainder by zero");
      B32(sb == -1 ? 0 : sa % sb)
    case 0x70:
      if (!(uint32_t)sp[-1])
        TRAP("remainder by zero");
      B32(a % b)
    case 0x71:
      B32(a & b)
    case 0x72:
      B32(a | b)
    case 0x73:
      B32(a ^ b)
    case 0x74:
      B32(a << (b & 31))
    case 0x75:
      B32(sa >> (b & 31))
    case 0x76:
      B32(a >> (b & 31))
    case 0x77:
      B32((a << (b & 31)) | (a >> ((32 - b) & 31)))
    case 0x78:
      B32((a >> (b & 31)) | (a << ((32 - b) & 31)))

    case 0x79:
      U64(a ? (uint64_t)__builtin_clzll(a) : 64)
    case 0x7a:
      U64(a ? (uint64_t)__builtin_ctzll(a) : 64)
    case 0x7b:
      U64(popcnt32((uint32_t)a) + popcnt32((uint32_t)(a >> 32)))
    case 0x7c:
      B64(a + b)
    case 0x7d:
      B64(a - b)
    case 0x7e:
      B64(a * b)
    case 0x7f:
      if (!sp[-1])
        TRAP("divide by zero");
      if ((int64_t)sp[-2] == INT64_MIN && (int64_t)sp[-1] == -1)
        TRAP("divide result unrepresentable");
      B64(sa / sb)
    case 0x80:
      if (!sp[-1])
        TRAP("divide by zero");
      B64(a / b)
    case 0x81:
      if (!sp[-1])
        TRAP("remainder by zero");
      B64(sb == -1 ? 0 : sa % sb)
    case 0x82:
      if (!sp[-1])
        TRAP("remainder by zero");
      B64(a % b)
    case 0x83:
      B64(a & b)
    case 0x84:
      B64(a | b)
    case 0x85:
      B64(a ^ b)
    case 0x86:
      B64(a << (b & 63))
    case 0x87:
      B64(sa >> (b & 63))
    case 0x88:
      B64(a >> (b & 63))
    case 0x89:
      B64((a << (b & 63)) | (a >> ((64 - b) & 63)))
    case 0x8a:
      B64((a >> (b & 63)) | (a << ((64 - b) & 63)))

    case 0x8b:
      sp[-1] &= 0x7fffffffu;
      break;
    case 0x8c:
      sp[-1] ^= 0x80000000u;
      break;
    case 0x8d:
      UF32(of_f32((float)ceil((double)a)))
    case 0x8e:
      UF32(of_f32((float)floor((double)a)))
    case 0x8f:
      UF32(of_f32((float)trunc((double)a)))
    case 0x90:
      UF32(of_f32((float)rint((double)a)))
    case 0x91:
      UF32(of_f32((float)sqrt((double)a)))
    case 0x92:
      BF32(of_f32(a + b))
    case 0x93:
      BF32(of_f32(a - b))
    case 0x94:
      BF32(of_f32(a * b))
    case 0x95:
      BF32(of_f32(a / b))
    case 0x96:
      BF32(of_f32(wasm_f32_min(a, b)))
    case 0x97:
      BF32(of_f32(wasm_f32_max(a, b)))
    case 0x98:
      sp--;
      sp[-1] = (sp[-1] & 0x7fffffffu) | (sp[0] & 0x80000000u);
      break;

    case 0x99:
      sp[-1] &= 0x7fffffffffffffffull;
      break;
    case 0x9a:
      sp[-1] ^= 0x8000000000000000ull;
      break;
    case 0x9b:
      UF64(of_f64(ceil(a)))
    case 0x9c:
      UF64(of_f64(floor(a)))
    case 0x9d:
      UF64(of_f64(trunc(a)))
    case 0x9e:
      UF64(of_f64(rint(a)))
    case 0x9f:
      UF64(of_f64(sqrt(a)))
    case 0xa0:
      BF64(of_f64(a + b))
    case 0xa1:
      BF64(of_f64(a - b))
    case 0xa2:
      BF64(of_f64(a * b))
    case 0xa3:
      BF64(of_f64(a / b))
    case 0xa4:
      BF64(of_f64(wasm_f64_min(a, b)))
    case 0xa5:
      BF64(of_f64(wasm_f64_max(a, b)))
    case 0xa6:
      sp--;
      sp[-1] = (sp[-1] & 0x7fffffffffffffffull) | (sp[0] & 0x8000000000000000ull);
      break;

    case 0xa7:
      sp[-1] = (uint32_t)sp[-1];
      break;
    case 0xa8: {
      uint32_t o;
      if (!wasm_tr_i32s((double)f32_of(sp[-1]), &o))
        TRAP("float unrepresentable in integer range");
      sp[-1] = o;
      break;
    }
    case 0xa9: {
      uint32_t o;
      if (!wasm_tr_i32u((double)f32_of(sp[-1]), &o))
        TRAP("float unrepresentable in integer range");
      sp[-1] = o;
      break;
    }
    case 0xaa: {
      uint32_t o;
      if (!wasm_tr_i32s(f64_of(sp[-1]), &o))
        TRAP("float unrepresentable in integer range");
      sp[-1] = o;
      break;
    }
    case 0xab: {
      uint32_t o;
      if (!wasm_tr_i32u(f64_of(sp[-1]), &o))
        TRAP("float unrepresentable in integer range");
      sp[-1] = o;
      break;
    }
    case 0xac:
      sp[-1] = (uint64_t)(int64_t)(int32_t)sp[-1];
      break;
    case 0xad:
      break;
    case 0xae: {
      uint64_t o;
      if (!wasm_tr_i64s((double)f32_of(sp[-1]), &o))
        TRAP("float unrepresentable in integer range");
      sp[-1] = o;
      break;
    }
    case 0xaf: {
      uint64_t o;
      if (!wasm_tr_i64u((double)f32_of(sp[-1]), &o))
        TRAP("float unrepresentable in integer range");
      sp[-1] = o;
      break;
    }
    case 0xb0: {
      uint64_t o;
      if (!wasm_tr_i64s(f64_of(sp[-1]), &o))
        TRAP("float unrepresentable in integer range");
      sp[-1] = o;
      break;
    }
    case 0xb1: {
      uint64_t o;
      if (!wasm_tr_i64u(f64_of(sp[-1]), &o))
        TRAP("float unrepresentable in integer range");
      sp[-1] = o;
      break;
    }
    case 0xb2:
      sp[-1] = of_f32((float)(int32_t)sp[-1]);
      break;
    case 0xb3:
      sp[-1] = of_f32((float)(uint32_t)sp[-1]);
      break;
    case 0xb4:
      sp[-1] = of_f32((float)(int64_t)sp[-1]);
      break;
    case 0xb5:
      sp[-1] = of_f32((float)sp[-1]);
      break;
    case 0xb6:
      sp[-1] = of_f32((float)f64_of(sp[-1]));
      break;
    case 0xb7:
      sp[-1] = of_f64((double)(int32_t)sp[-1]);
      break;
    case 0xb8:
      sp[-1] = of_f64((double)(uint32_t)sp[-1]);
      break;
    case 0xb9:
      sp[-1] = of_f64((double)(int64_t)sp[-1]);
      break;
    case 0xba:
      sp[-1] = of_f64((double)sp[-1]);
      break;
    case 0xbb:
      sp[-1] = of_f64((double)f32_of(sp[-1]));
      break;
    case 0xbc:
    case 0xbd:
    case 0xbe:
    case 0xbf:
      break;
    case 0xc0:
      sp[-1] = (uint32_t)(int32_t)(int8_t)sp[-1];
      break;
    case 0xc1:
      sp[-1] = (uint32_t)(int32_t)(int16_t)sp[-1];
      break;
    case 0xc2:
      sp[-1] = (uint64_t)(int64_t)(int8_t)sp[-1];
      break;
    case 0xc3:
      sp[-1] = (uint64_t)(int64_t)(int16_t)sp[-1];
      break;
    case 0xc4:
      sp[-1] = (uint64_t)(int64_t)(int32_t)sp[-1];
      break;

    case 0xd0:
      *sp++ = 0;
      break;
    case 0xd1:
      sp[-1] = sp[-1] == 0;
      break;
    case 0xd2:
      *sp++ = (uint64_t)(uintptr_t)inst->funcs[*pc++];
      break;

    case OP_FC(0):
      sp[-1] = wasm_sat_i32s((double)f32_of(sp[-1]));
      break;
    case OP_FC(1):
      sp[-1] = wasm_sat_i32u((double)f32_of(sp[-1]));
      break;
    case OP_FC(2):
      sp[-1] = wasm_sat_i32s(f64_of(sp[-1]));
      break;
    case OP_FC(3):
      sp[-1] = wasm_sat_i32u(f64_of(sp[-1]));
      break;
    case OP_FC(4):
      sp[-1] = wasm_sat_i64s((double)f32_of(sp[-1]));
      break;
    case OP_FC(5):
      sp[-1] = wasm_sat_i64u((double)f32_of(sp[-1]));
      break;
    case OP_FC(6):
      sp[-1] = wasm_sat_i64s(f64_of(sp[-1]));
      break;
    case OP_FC(7):
      sp[-1] = wasm_sat_i64u(f64_of(sp[-1]));
      break;
    case OP_FC(8): {
      uint32_t seg = *pc++, n = (uint32_t)sp[-1], s = (uint32_t)sp[-2], d = (uint32_t)sp[-3];
      uint32_t len = inst->data_dropped[seg] ? 0 : inst->module->datas[seg].len;
      sp -= 3;
      if ((uint64_t)s + n > len || (uint64_t)d + n > mem->size)
        TRAP(OOB);
      memcpy(mem->data + d, inst->module->datas[seg].bytes + s, n);
      break;
    }
    case OP_FC(9):
      inst->data_dropped[*pc++] = 1;
      break;
    case OP_FC(10): {
      uint32_t n = (uint32_t)sp[-1], s = (uint32_t)sp[-2], d = (uint32_t)sp[-3];
      sp -= 3;
      if ((uint64_t)s + n > mem->size || (uint64_t)d + n > mem->size)
        TRAP(OOB);
      memmove(mem->data + d, mem->data + s, n);
      break;
    }
    case OP_FC(11): {
      uint32_t n = (uint32_t)sp[-1], v = (uint32_t)sp[-2], d = (uint32_t)sp[-3];
      sp -= 3;
      if ((uint64_t)d + n > mem->size)
        TRAP(OOB);
      memset(mem->data + d, (int)(v & 0xff), n);
      break;
    }
    case OP_FC(12): {
      uint32_t seg = pc[0];
      wasm_table *t = inst->tables[pc[1]];
      uint32_t n = (uint32_t)sp[-1], s = (uint32_t)sp[-2], d = (uint32_t)sp[-3];
      pc += 2;
      sp -= 3;
      if ((uint64_t)s + n > inst->elem_n[seg])
        TRAP("element segment out of bounds");
      if ((uint64_t)d + n > t->n)
        TRAP("table index is out of bounds");
      if (t->type == WASM_FUNCREF)
        memcpy(t->f + d, inst->elem_f[seg] + s, n * sizeof(wasm_func *));
      else
        memset(t->x + d, 0, n * sizeof *t->x);
      break;
    }
    case OP_FC(13):
      inst->elem_n[*pc++] = 0;
      break;
    case OP_FC(14): {
      wasm_table *td = inst->tables[pc[0]], *ts = inst->tables[pc[1]];
      uint32_t n = (uint32_t)sp[-1], s = (uint32_t)sp[-2], d = (uint32_t)sp[-3];
      pc += 2;
      sp -= 3;
      if ((uint64_t)s + n > ts->n || (uint64_t)d + n > td->n)
        TRAP("table index is out of bounds");
      if (td->type == WASM_FUNCREF)
        memmove(td->f + d, ts->f + s, n * sizeof(wasm_func *));
      else
        memmove(td->x + d, ts->x + s, n * sizeof *td->x);
      break;
    }
    case OP_FC(15): {
      wasm_table *t = inst->tables[*pc++];
      uint32_t n = (uint32_t)sp[-1];
      uint64_t ref = sp[-2];
      wasm_extern ext;
      memset(&ext, 0, sizeof ext);
      if (t->type == WASM_EXTERNREF && ref)
        x->xr_value(x->ud, ref, &ext);
      sp--;
      sp[-1] =
          (uint32_t)(int32_t)wasm_table_grow(t, n, (wasm_func *)(uintptr_t)(t->type == WASM_FUNCREF ? ref : 0), &ext);
      break;
    }
    case OP_FC(16):
      *sp++ = inst->tables[*pc++]->n;
      break;
    case OP_FC(17): {
      wasm_table *t = inst->tables[*pc++];
      uint32_t n = (uint32_t)sp[-1], d = (uint32_t)sp[-3];
      uint64_t ref = sp[-2];
      sp -= 3;
      if ((uint64_t)d + n > t->n)
        TRAP("table index is out of bounds");
      if (t->type == WASM_FUNCREF) {
        for (i = 0; i < n; i++)
          t->f[d + i] = (wasm_func *)(uintptr_t)ref;
      } else {
        wasm_extern ext;
        memset(&ext, 0, sizeof ext);
        if (ref)
          x->xr_value(x->ud, ref, &ext);
        for (i = 0; i < n; i++)
          t->x[d + i] = ext;
      }
      break;
    }

    default:
      if (op >= OP_FD(0) && op <= OP_FD(0xff)) {
        trap = wasm_simd_exec(op - OP_FD(0), &sp, &pc, mem);
        if (trap)
          goto do_trap;
        break;
      }
      TRAP("invalid instruction");
    }
  }
do_trap:
  x->trap = trap;
  return -1;
}

int wasm_call(wasm_exec *x, wasm_func *f, const uint64_t *args, uint64_t *results)
{
  uint64_t *base = x->sp;
  uint32_t base_frames = x->nframes;
  uint32_t ps = f->type->pslots, rs = f->type->rslots;
  int r;
  x->trap = NULL;
  if (base + ps + rs + 64 > x->stack_end) {
    x->trap = "call stack exhausted";
    return -1;
  }
  memcpy(base, args, ps * sizeof *base);
  if (!f->code) {
    x->sp = base + (ps > rs ? ps : rs);
    r = x->host_call(x->ud, f, base) ? -2 : 0;
  } else {
    unsigned short outer = x->fpu_cw;
    x->fpu_cw = fpu_narrow();
    r = run(x, f, base);
    fpu_set(x->fpu_cw);
    x->fpu_cw = outer;
  }
  if (r == 0)
    memcpy(results, base, rs * sizeof *base);
  x->sp = base;
  x->nframes = base_frames;
  return r;
}
