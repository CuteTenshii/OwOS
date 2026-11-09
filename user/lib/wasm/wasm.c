/* wasm.c: WebAssembly binary decoder, validator and bytecode compiler. See wasm.h.
 *
 * Decoding failures unwind with longjmp to wasm_module_new; everything the module owns comes from one chain of
 * malloc blocks, and the compiler's scratch vectors hang off the context, so a failure frees it all at one spot. */
#include "wasm.h"
#include "owos.h"
#include "wasm_int.h"

enum { MAX_LOCALS = 50000, MAX_CODE_WORDS = 1 << 26, MAX_NAME = 100000 };
enum { K_FUNC, K_BLOCK, K_LOOP, K_IF, K_ELSE, K_TRY, K_CATCH, K_TRYTABLE };
enum { MEM_NONE, MEM_FIRST };

typedef struct blk {
  struct blk *next;
  uint32_t pad[3];
} blk;

typedef struct {
  uint8_t kind, unreachable, dead;
  const wasm_functype *bt;
  uint32_t height, sheight;
  uint32_t patch; /* forward branch fixups, as index + 1 into code */
  uint32_t loop_pc;
  uint32_t else_patch; /* the JMP_UNLESS target word of an `if`, index + 1 */
  uint32_t start_pc;   /* pc where the block begins (the marker word of a try) */
  uint32_t body_end;   /* try: pc where its protected body ends */
  uint32_t cl_begin;   /* try_table: its catch clauses are tcl[cl_begin, cl_end) */
  uint32_t cl_end;
  uint8_t hidden; /* a catch arm keeps its exnref in one slot below its operands */
} ctrl;

/* A catch clause of try_table; label is an absolute index into the control stack */
typedef struct {
  uint8_t kind;
  int32_t tag;
  uint32_t label;
} tclause;

typedef struct {
  wasm_module *m;
  const uint8_t *base, *p, *end;
  jmp_buf jb;
  char *err;
  size_t errn;
  int has_datacount;
  uint32_t datacount;
  /* function compiler */
  uint8_t *vt;
  uint32_t nvt, capvt;
  ctrl *cs;
  uint32_t ncs, capcs;
  uint32_t *code;
  uint32_t ncode, capcode;
  uint32_t sh, maxsh;
  uint32_t *lslot;
  uint8_t *ltype;
  wasm_handler *hv;
  uint32_t nhv, caphv;
  tclause *tcl;
  uint32_t ntcl, captcl;
  uint32_t nloc, caploc, nlocal_slots;
  wasm_functype fbt; /* the function's results as the type of its outermost block */
} C;

static unsigned slots_of(int t) { return t == WASM_V128 ? 2 : 1; }

/* ---- errors and memory --------------------------------------------------- */

static size_t put_str(char *d, size_t n, size_t at, const char *s)
{
  while (*s && at + 1 < n)
    d[at++] = *s++;
  if (at < n)
    d[at] = 0;
  return at;
}

static void fail(C *c, const char *msg)
{
  if (c->err && c->errn) {
    char num[16];
    size_t at = put_str(c->err, c->errn, 0, msg);
    unsigned off = (unsigned)(c->p - c->base), l = 0, i;
    at = put_str(c->err, c->errn, at, " @+");
    do
      num[l++] = (char)('0' + off % 10);
    while ((off /= 10) && l < sizeof num - 1);
    for (i = 0; i < l; i++) {
      char one[2] = {num[l - 1 - i], 0};
      at = put_str(c->err, c->errn, at, one);
    }
  }
  longjmp(c->jb, 1);
}

static void *ma(C *c, size_t n)
{
  blk *b;
  if (n > 0x7fffff00u)
    fail(c, "allocation too large");
  b = (blk *)calloc(1, (unsigned)(sizeof(blk) + n + 1));
  if (!b)
    fail(c, "out of memory");
  b->next = (blk *)c->m->arena;
  c->m->arena = b;
  return b + 1;
}

static void *vec_grow(C *c, void *old, uint32_t *cap, uint32_t need, size_t esz)
{
  uint32_t ncap = *cap ? *cap : 64;
  void *nv;
  while (ncap < need)
    ncap *= 2;
  nv = realloc(old, (unsigned)(ncap * esz));
  if (!nv)
    fail(c, "out of memory");
  *cap = ncap;
  return nv;
}

/* ---- reader -------------------------------------------------------------- */

static void need(C *c, size_t n)
{
  if ((size_t)(c->end - c->p) < n)
    fail(c, "unexpected end");
}

static uint8_t rd_u8(C *c)
{
  need(c, 1);
  return *c->p++;
}

static uint32_t rd_u32(C *c)
{
  uint32_t r = 0;
  int i;
  for (i = 0; i < 5; i++) {
    uint8_t b = rd_u8(c);
    if (i == 4 && (b & 0xf0))
      fail(c, "invalid LEB128");
    r |= (uint32_t)(b & 0x7f) << (7 * i);
    if (!(b & 0x80))
      return r;
  }
  fail(c, "invalid LEB128");
  return 0;
}

static uint64_t rd_u64(C *c)
{
  uint64_t r = 0;
  int i;
  for (i = 0; i < 10; i++) {
    uint8_t b = rd_u8(c);
    if (i == 9 && (b & 0xfe))
      fail(c, "invalid LEB128");
    r |= (uint64_t)(b & 0x7f) << (7 * i);
    if (!(b & 0x80))
      return r;
  }
  fail(c, "invalid LEB128");
  return 0;
}

static int64_t rd_sleb(C *c, int maxbytes, int bits)
{
  uint64_t r = 0;
  int i, sh = 0;
  for (i = 0; i < maxbytes; i++) {
    uint8_t b = rd_u8(c);
    r |= (uint64_t)(b & 0x7f) << sh;
    sh += 7;
    if (!(b & 0x80)) {
      int64_t v;
      if (i == 9 && b != 0 && b != 0x7f)
        fail(c, "invalid LEB128");
      if (sh < 64 && (b & 0x40))
        r |= ~(uint64_t)0 << sh;
      v = (int64_t)r;
      if (bits < 64 && (v < -((int64_t)1 << (bits - 1)) || v >= ((int64_t)1 << (bits - 1))))
        fail(c, "invalid LEB128");
      return v;
    }
  }
  fail(c, "invalid LEB128");
  return 0;
}

static int32_t rd_s32(C *c) { return (int32_t)rd_sleb(c, 5, 32); }
static int64_t rd_s64(C *c) { return rd_sleb(c, 10, 64); }

static uint32_t rd_count(C *c)
{
  uint32_t n = rd_u32(c);
  if (n > (uint32_t)(c->end - c->p))
    fail(c, "count exceeds section size");
  return n;
}

static int valid_utf8(const uint8_t *s, uint32_t n)
{
  uint32_t i = 0;
  while (i < n) {
    uint8_t b = s[i];
    uint32_t len, cp;
    if (b < 0x80) {
      i++;
      continue;
    }
    if (b >= 0xc2 && b <= 0xdf) {
      len = 2;
      cp = b & 0x1f;
    } else if (b >= 0xe0 && b <= 0xef) {
      len = 3;
      cp = b & 0x0f;
    } else if (b >= 0xf0 && b <= 0xf4) {
      len = 4;
      cp = b & 0x07;
    } else
      return 0;
    if (i + len > n)
      return 0;
    for (uint32_t k = 1; k < len; k++) {
      if ((s[i + k] & 0xc0) != 0x80)
        return 0;
      cp = (cp << 6) | (s[i + k] & 0x3f);
    }
    if ((len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)))
      return 0;
    i += len;
  }
  return 1;
}

static char *rd_name(C *c)
{
  uint32_t n = rd_u32(c);
  char *s;
  if (n > MAX_NAME)
    fail(c, "name too long");
  need(c, n);
  if (!valid_utf8(c->p, n))
    fail(c, "invalid UTF-8 string");
  s = (char *)ma(c, n + 1);
  memcpy(s, c->p, n);
  c->p += n;
  return s;
}

static uint8_t rd_valtype(C *c)
{
  uint8_t t = rd_u8(c);
  if (t == WASM_I32 || t == WASM_I64 || t == WASM_F32 || t == WASM_F64 || t == WASM_V128 || t == WASM_FUNCREF ||
      t == WASM_EXTERNREF || t == WASM_EXNREF)
    return t;
  c->p--;
  fail(c, "invalid value type");
  return 0;
}

/* The heap type of ref.null: also exnref */
static uint8_t rd_nulltype(C *c)
{
  uint8_t t = rd_u8(c);
  if (t == WASM_FUNCREF || t == WASM_EXTERNREF || t == WASM_EXNREF)
    return t;
  c->p--;
  fail(c, "invalid reference type");
  return 0;
}

static uint8_t rd_reftype(C *c)
{
  uint8_t t = rd_u8(c);
  if (t == WASM_FUNCREF || t == WASM_EXTERNREF)
    return t;
  c->p--;
  fail(c, "invalid reference type");
  return 0;
}

/* Table limits (flags 0 and 1) or memory limits (also shared and 64-bit) */
static void rd_limits(C *c, wasm_limits *l, uint64_t cap, const char *what, int is_memory)
{
  uint8_t f = rd_u8(c);
  int has_max = f & 1;
  memset(l, 0, sizeof *l);
  if (f > (is_memory ? 7 : 1)) {
    c->p--;
    fail(c, "invalid limits flags");
  }
  l->shared = (f >> 1) & 1;
  l->is64 = (f >> 2) & 1;
  if (l->shared && !has_max)
    fail(c, "shared memory must have a maximum");
  if (l->is64)
    cap = (uint64_t)1 << 48;
  l->min = l->is64 ? rd_u64(c) : rd_u32(c);
  l->has_max = (uint8_t)has_max;
  l->max = has_max ? (l->is64 ? rd_u64(c) : rd_u32(c)) : 0;
  if (l->min > cap || (has_max && l->max > cap))
    fail(c, what);
  if (has_max && l->max < l->min)
    fail(c, "maximum is less than the minimum");
}

/* ---- types --------------------------------------------------------------- */

static const uint8_t VT1[8] = {WASM_I32,  WASM_I64,     WASM_F32,       WASM_F64,
                               WASM_V128, WASM_FUNCREF, WASM_EXTERNREF, WASM_EXNREF};
static const wasm_functype FT_EMPTY = {0, 0, 0, 0, NULL, NULL};
static const wasm_functype FT_1[8] = {
    {0, 1, 0, 1, NULL, &VT1[0]}, {0, 1, 0, 1, NULL, &VT1[1]}, {0, 1, 0, 1, NULL, &VT1[2]}, {0, 1, 0, 1, NULL, &VT1[3]},
    {0, 1, 0, 2, NULL, &VT1[4]}, {0, 1, 0, 1, NULL, &VT1[5]}, {0, 1, 0, 1, NULL, &VT1[6]}, {0, 1, 0, 1, NULL, &VT1[7]}};

static int vt_index(int t)
{
  switch (t) {
  case WASM_I32:
    return 0;
  case WASM_I64:
    return 1;
  case WASM_F32:
    return 2;
  case WASM_F64:
    return 3;
  case WASM_V128:
    return 4;
  case WASM_FUNCREF:
    return 5;
  case WASM_EXTERNREF:
    return 6;
  case WASM_EXNREF:
    return 7;
  }
  return -1;
}

static int wasm_results_eq(const wasm_functype *a, const wasm_functype *b)
{
  return a->nr == b->nr && !memcmp(a->r, b->r, a->nr);
}

int wasm_functype_eq(const wasm_functype *a, const wasm_functype *b)
{
  return a == b || (a->np == b->np && a->nr == b->nr && !memcmp(a->p, b->p, a->np) && !memcmp(a->r, b->r, a->nr));
}

static void sec_types(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i, j;
  m->types = (wasm_functype *)ma(c, n * sizeof *m->types);
  m->ntypes = n;
  for (i = 0; i < n; i++) {
    wasm_functype *t = &m->types[i];
    uint8_t *p, *r;
    if (rd_u8(c) != 0x60) {
      c->p--;
      fail(c, "expected function type form 0x60");
    }
    t->np = rd_count(c);
    p = (uint8_t *)ma(c, t->np);
    for (j = 0; j < t->np; j++) {
      p[j] = rd_valtype(c);
      t->pslots += slots_of(p[j]);
    }
    t->nr = rd_count(c);
    r = (uint8_t *)ma(c, t->nr);
    for (j = 0; j < t->nr; j++) {
      r[j] = rd_valtype(c);
      t->rslots += slots_of(r[j]);
    }
    t->p = p;
    t->r = r;
  }
}

/* ---- constant expressions ------------------------------------------------ */

/* Parse one constant expression of type `want` into *e */
static void rd_const(C *c, wasm_constexpr *e, int want, uint32_t nglobals_visible)
{
  enum { MAX_OPS = 64 };
  wasm_module *m = c->m;
  wasm_cop ops[MAX_OPS];
  uint8_t ts[MAX_OPS];
  uint32_t n = 0, depth = 0;
  for (;;) {
    wasm_cop *o;
    int got = 0, arith = 0;
    uint8_t op = rd_u8(c);
    if (op == 0x0b)
      break;
    if (n >= MAX_OPS)
      fail(c, "constant expression too long");
    o = &ops[n];
    memset(o, 0, sizeof *o);
    o->op = op;
    switch (op) {
    case 0x41:
      o->v[0] = (uint32_t)rd_s32(c);
      got = WASM_I32;
      break;
    case 0x42:
      o->v[0] = (uint64_t)rd_s64(c);
      got = WASM_I64;
      break;
    case 0x43:
      need(c, 4);
      memcpy(&o->v[0], c->p, 4);
      c->p += 4;
      got = WASM_F32;
      break;
    case 0x44:
      need(c, 8);
      memcpy(&o->v[0], c->p, 8);
      c->p += 8;
      got = WASM_F64;
      break;
    case 0xfd:
      if (rd_u32(c) != 0x0c)
        fail(c, "invalid constant expression");
      need(c, 16);
      memcpy(o->v, c->p, 16);
      c->p += 16;
      got = WASM_V128;
      break;
    case 0x23: {
      uint32_t g = rd_u32(c);
      if (g >= nglobals_visible)
        fail(c, "unknown global in constant expression");
      if (m->globals[g].mut)
        fail(c, "constant expression refers to a mutable global");
      o->v[0] = g;
      got = m->globals[g].type;
      break;
    }
    case 0xd0:
      got = rd_reftype(c);
      break;
    case 0xd2: {
      uint32_t f = rd_u32(c);
      if (f >= m->nfuncs)
        fail(c, "unknown function in constant expression");
      o->v[0] = f;
      got = WASM_FUNCREF;
      break;
    }
    case 0x6a:
    case 0x6b:
    case 0x6c:
      arith = WASM_I32;
      break;
    case 0x7c:
    case 0x7d:
    case 0x7e:
      arith = WASM_I64;
      break;
    default:
      c->p--;
      fail(c, "unsupported constant expression");
      return;
    }
    if (arith) {
      if (depth < 2 || ts[depth - 1] != arith || ts[depth - 2] != arith)
        fail(c, "type mismatch in constant expression");
      depth--;
    } else {
      if (depth >= MAX_OPS)
        fail(c, "constant expression too long");
      ts[depth++] = (uint8_t)got;
    }
    n++;
  }
  if (depth != 1 || ts[0] != want)
    fail(c, "type mismatch in constant expression");
  e->n = n;
  e->ops = (wasm_cop *)ma(c, n * sizeof *e->ops);
  memcpy(e->ops, ops, n * sizeof *e->ops);
}

/* ---- sections ------------------------------------------------------------ */

static void sec_imports(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i;
  m->imports = (wasm_import *)ma(c, n * sizeof *m->imports);
  m->nimports = n;
  m->func_type = (uint32_t *)ma(c, n * sizeof *m->func_type);
  m->tables = (wasm_tabletype *)ma(c, n * sizeof *m->tables);
  m->mems = (wasm_limits *)ma(c, n * sizeof *m->mems);
  m->globals = (wasm_globaltype *)ma(c, n * sizeof *m->globals);
  m->tag_type = (uint32_t *)ma(c, n * sizeof *m->tag_type);
  for (i = 0; i < n; i++) {
    wasm_import *im = &m->imports[i];
    im->module = rd_name(c);
    im->name = rd_name(c);
    im->kind = rd_u8(c);
    switch (im->kind) {
    case WASM_KIND_FUNC:
      im->type = rd_u32(c);
      if (im->type >= m->ntypes)
        fail(c, "unknown type");
      m->func_type[m->nfunc_imports++] = im->type;
      break;
    case WASM_KIND_TABLE:
      im->table.elem = rd_reftype(c);
      rd_limits(c, &im->table.lim, WASM_MAX_TABLE, "table size is too large", 0);
      m->tables[m->ntable_imports++] = im->table;
      break;
    case WASM_KIND_MEMORY:
      rd_limits(c, &im->mem, 65536, "memory size is too large", 1);
      m->mems[m->nmem_imports++] = im->mem;
      break;
    case WASM_KIND_GLOBAL:
      im->global.type = rd_valtype(c);
      if (im->global.type == WASM_EXNREF)
        fail(c, "exnref globals are not supported");
      im->global.mut = rd_u8(c);
      if (im->global.mut > 1)
        fail(c, "invalid mutability");
      m->globals[m->nglobal_imports++] = im->global;
      break;
    case WASM_KIND_TAG:
      if (rd_u8(c) != 0)
        fail(c, "invalid tag attribute");
      im->type = rd_u32(c);
      if (im->type >= m->ntypes)
        fail(c, "unknown type");
      if (m->types[im->type].nr)
        fail(c, "tag type must not have results");
      m->tag_type[m->ntag_imports++] = im->type;
      break;
    default:
      c->p--;
      fail(c, "invalid import kind");
    }
  }
  m->nfuncs = m->nfunc_imports;
  m->ntables = m->ntable_imports;
  m->nmems = m->nmem_imports;
  m->nglobals = m->nglobal_imports;
  m->ntags = m->ntag_imports;
}

static void sec_tags(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i;
  uint32_t *t = (uint32_t *)ma(c, (m->ntag_imports + n) * sizeof *t);
  memcpy(t, m->tag_type, m->ntag_imports * sizeof *t);
  for (i = 0; i < n; i++) {
    uint32_t ty;
    if (rd_u8(c) != 0)
      fail(c, "invalid tag attribute");
    ty = rd_u32(c);
    if (ty >= m->ntypes)
      fail(c, "unknown type");
    if (m->types[ty].nr)
      fail(c, "tag type must not have results");
    t[m->ntag_imports + i] = ty;
  }
  m->tag_type = t;
  m->ntags = m->ntag_imports + n;
}

static void sec_functions(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i;
  uint32_t *ft = (uint32_t *)ma(c, (m->nfunc_imports + n) * sizeof *ft);
  memcpy(ft, m->func_type, m->nfunc_imports * sizeof *ft);
  for (i = 0; i < n; i++) {
    uint32_t t = rd_u32(c);
    if (t >= m->ntypes)
      fail(c, "unknown type");
    ft[m->nfunc_imports + i] = t;
  }
  m->func_type = ft;
  m->nfuncs = m->nfunc_imports + n;
}

static void sec_tables(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i;
  wasm_tabletype *t = (wasm_tabletype *)ma(c, (m->ntable_imports + n) * sizeof *t);
  memcpy(t, m->tables, m->ntable_imports * sizeof *t);
  for (i = 0; i < n; i++) {
    wasm_tabletype *d = &t[m->ntable_imports + i];
    d->elem = rd_reftype(c);
    rd_limits(c, &d->lim, WASM_MAX_TABLE, "table size is too large", 0);
  }
  m->tables = t;
  m->ntables = m->ntable_imports + n;
}

static void sec_memories(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i;
  wasm_limits *t;
  t = (wasm_limits *)ma(c, (m->nmem_imports + n) * sizeof *t);
  memcpy(t, m->mems, m->nmem_imports * sizeof *t);
  for (i = 0; i < n; i++)
    rd_limits(c, &t[m->nmem_imports + i], 65536, "memory size is too large", 1);
  m->mems = t;
  m->nmems = m->nmem_imports + n;
}

static void sec_globals(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i;
  wasm_globaltype *g = (wasm_globaltype *)ma(c, (m->nglobal_imports + n) * sizeof *g);
  m->global_init = (wasm_constexpr *)ma(c, n * sizeof *m->global_init);
  memcpy(g, m->globals, m->nglobal_imports * sizeof *g);
  m->globals = g;
  m->nglobals = m->nglobal_imports + n;
  for (i = 0; i < n; i++) {
    wasm_globaltype *d = &g[m->nglobal_imports + i];
    d->type = rd_valtype(c);
    if (d->type == WASM_EXNREF)
      fail(c, "exnref globals are not supported");
    d->mut = rd_u8(c);
    if (d->mut > 1)
      fail(c, "invalid mutability");
    rd_const(c, &m->global_init[i], d->type, m->nglobal_imports + i);
  }
}

static void sec_exports(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i, j;
  m->exports = (wasm_export *)ma(c, n * sizeof *m->exports);
  m->nexports = n;
  for (i = 0; i < n; i++) {
    wasm_export *e = &m->exports[i];
    uint32_t lim;
    e->name = rd_name(c);
    e->kind = rd_u8(c);
    e->idx = rd_u32(c);
    switch (e->kind) {
    case WASM_KIND_FUNC:
      lim = m->nfuncs;
      break;
    case WASM_KIND_TABLE:
      lim = m->ntables;
      break;
    case WASM_KIND_MEMORY:
      lim = m->nmems;
      break;
    case WASM_KIND_GLOBAL:
      lim = m->nglobals;
      break;
    case WASM_KIND_TAG:
      lim = m->ntags;
      break;
    default:
      c->p--;
      fail(c, "invalid export kind");
      return;
    }
    if (e->idx >= lim)
      fail(c, "export index out of range");
    for (j = 0; j < i; j++)
      if (!strcmp(m->exports[j].name, e->name))
        fail(c, "duplicate export name");
  }
}

static void sec_start(C *c)
{
  wasm_module *m = c->m;
  uint32_t f = rd_u32(c);
  const wasm_functype *t;
  if (f >= m->nfuncs)
    fail(c, "unknown start function");
  t = &m->types[m->func_type[f]];
  if (t->np || t->nr)
    fail(c, "start function must take no arguments and return nothing");
  m->start = f;
}

static void sec_elements(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i, j;
  m->elems = (wasm_elem *)ma(c, n * sizeof *m->elems);
  m->nelems = n;
  for (i = 0; i < n; i++) {
    wasm_elem *e = &m->elems[i];
    uint32_t flag = rd_u32(c);
    int exprs = flag & 4, explicit_table = flag & 2;
    if (flag > 7)
      fail(c, "invalid element segment flags");
    e->type = WASM_FUNCREF;
    e->mode = (flag & 1) ? ((flag & 2) ? 2 : 1) : 0;
    if (e->mode == 0) {
      e->table = explicit_table ? rd_u32(c) : 0;
      if (e->table >= m->ntables)
        fail(c, "unknown table");
      rd_const(c, &e->offset, WASM_I32, m->nglobals);
    }
    if (flag & 3) {
      if (exprs) {
        e->type = rd_reftype(c);
      } else if (rd_u8(c) != 0)
        fail(c, "invalid element kind");
    }
    if (e->mode == 0 && m->tables[e->table].elem != e->type)
      fail(c, "element segment type does not match the table");
    e->n = rd_count(c);
    e->funcs = (int32_t *)ma(c, e->n * sizeof *e->funcs);
    for (j = 0; j < e->n; j++) {
      if (!exprs) {
        uint32_t f = rd_u32(c);
        if (f >= m->nfuncs)
          fail(c, "unknown function");
        e->funcs[j] = (int32_t)f;
      } else {
        wasm_constexpr x;
        rd_const(c, &x, e->type, 0);
        if (x.n == 1 && x.ops[0].op == 0xd2)
          e->funcs[j] = (int32_t)x.ops[0].v[0];
        else if (x.n == 1 && x.ops[0].op == 0xd0)
          e->funcs[j] = -1;
        else
          fail(c, "unsupported element expression");
      }
    }
  }
}

static void sec_data(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i;
  if (c->has_datacount && n != c->datacount)
    fail(c, "data count and data section have inconsistent lengths");
  m->datas = (wasm_data *)ma(c, n * sizeof *m->datas);
  m->ndatas = n;
  for (i = 0; i < n; i++) {
    wasm_data *d = &m->datas[i];
    uint32_t flag = rd_u32(c);
    if (flag > 2)
      fail(c, "invalid data segment flags");
    d->active = flag != 1;
    if (d->active) {
      d->mem = flag == 2 ? rd_u32(c) : 0;
      if (d->mem >= m->nmems)
        fail(c, "unknown memory");
      rd_const(c, &d->offset, m->mems[d->mem].is64 ? WASM_I64 : WASM_I32, m->nglobals);
    }
    d->len = rd_u32(c);
    need(c, d->len);
    d->bytes = (uint8_t *)ma(c, d->len);
    memcpy(d->bytes, c->p, d->len);
    c->p += d->len;
  }
}

/* ---- function compiler --------------------------------------------------- */

static void emit(C *c, uint32_t w)
{
  if (c->ncode >= c->capcode) {
    if (c->ncode >= MAX_CODE_WORDS)
      fail(c, "function body too large");
    c->code = (uint32_t *)vec_grow(c, c->code, &c->capcode, c->ncode + 1, sizeof *c->code);
  }
  c->code[c->ncode++] = w;
}

static ctrl *top(C *c) { return &c->cs[c->ncs - 1]; }
static int alive(C *c) { return !top(c)->dead && !top(c)->unreachable; }

static void push(C *c, int t)
{
  if (c->nvt >= c->capvt)
    c->vt = (uint8_t *)vec_grow(c, c->vt, &c->capvt, c->nvt + 1, 1);
  c->vt[c->nvt++] = (uint8_t)t;
  c->sh += slots_of(t);
  if (c->sh > c->maxsh)
    c->maxsh = c->sh;
}

/* Pop an operand; 0 is the unknown type of unreachable code */
static int pop(C *c)
{
  ctrl *f = top(c);
  int t;
  if (c->nvt == f->height) {
    if (f->unreachable)
      return 0;
    fail(c, "type mismatch: not enough operands");
  }
  t = c->vt[--c->nvt];
  c->sh -= slots_of(t);
  return t;
}

static int pop_t(C *c, int want)
{
  int t = pop(c);
  if (t && want && t != want)
    fail(c, "type mismatch");
  return t ? t : want;
}

static void pop_vals(C *c, const uint8_t *ts, uint32_t n)
{
  while (n--)
    pop_t(c, ts[n]);
}

static void push_vals(C *c, const uint8_t *ts, uint32_t n)
{
  uint32_t i;
  for (i = 0; i < n; i++)
    push(c, ts[i]);
}

static void push_ctrl(C *c, int kind, const wasm_functype *bt)
{
  ctrl *f;
  int dead = c->ncs ? !alive(c) : 0;
  pop_vals(c, bt->p, bt->np);
  if (c->ncs >= c->capcs)
    c->cs = (ctrl *)vec_grow(c, c->cs, &c->capcs, c->ncs + 1, sizeof *c->cs);
  f = &c->cs[c->ncs++];
  memset(f, 0, sizeof *f);
  f->kind = (uint8_t)kind;
  f->dead = (uint8_t)dead;
  f->bt = bt;
  f->height = c->nvt;
  f->sheight = c->sh;
  f->loop_pc = c->ncode;
  f->start_pc = c->ncode;
  push_vals(c, bt->p, bt->np);
}

static void set_unreachable(C *c)
{
  ctrl *f = top(c);
  c->nvt = f->height;
  c->sh = f->sheight + f->hidden;
  f->unreachable = 1;
}

/* A fixup chain runs through the unresolved target words: code positions as pos + 1, handler targets as
 * (handler + 1) | 0x80000000 */
static void patch_to(C *c, uint32_t head, uint32_t target)
{
  while (head) {
    if (head & 0x80000000u) {
      uint32_t h = (head & 0x7fffffffu) - 1;
      head = c->hv[h].target;
      c->hv[h].target = target;
    } else {
      uint32_t pos = head - 1;
      head = c->code[pos];
      c->code[pos] = target;
    }
  }
}

static uint32_t add_handler(C *c, wasm_handler h)
{
  if (c->nhv >= c->caphv)
    c->hv = (wasm_handler *)vec_grow(c, c->hv, &c->caphv, c->nhv + 1, sizeof *c->hv);
  c->hv[c->nhv] = h;
  return c->nhv++;
}

/* Point handler hi's target at label frame t: now for a loop, once t ends otherwise */
static void handler_target(C *c, ctrl *t, uint32_t hi)
{
  if (t->kind == K_LOOP)
    c->hv[hi].target = t->loop_pc;
  else {
    c->hv[hi].target = t->patch;
    t->patch = (hi + 1) | 0x80000000u;
  }
}

/* Where a search for a handler resumes when a try delegates to frame t */
static uint32_t resume_pc(const ctrl *t)
{
  if (t->kind == K_FUNC)
    return 0;
  return t->kind == K_TRY || t->kind == K_TRYTABLE ? t->start_pc + 1 : t->start_pc;
}

/* Emit a forward branch target word, to be fixed when frame f ends */
static void emit_fwd(C *c, ctrl *f)
{
  uint32_t pos = c->ncode;
  emit(c, f->patch);
  f->patch = pos + 1;
}

static void emit_target(C *c, ctrl *t)
{
  if (t->kind == K_LOOP)
    emit(c, t->loop_pc);
  else
    emit_fwd(c, t);
}

static uint32_t label_arity(const ctrl *t) { return t->kind == K_LOOP ? t->bt->pslots : t->bt->rslots; }

static void label_types(const ctrl *t, const uint8_t **ts, uint32_t *n)
{
  if (t->kind == K_LOOP) {
    *ts = t->bt->p;
    *n = t->bt->np;
  } else {
    *ts = t->bt->r;
    *n = t->bt->nr;
  }
}

static ctrl *label(C *c, uint32_t depth)
{
  if (depth >= c->ncs)
    fail(c, "unknown label");
  return &c->cs[c->ncs - 1 - depth];
}

/* Emit the (target, arity, dest) triple for a branch to t from the current stack */
static void emit_br_triple(C *c, ctrl *t)
{
  emit_target(c, t);
  emit(c, label_arity(t));
  emit(c, c->nlocal_slots + t->sheight);
}

static const wasm_functype *read_blocktype(C *c)
{
  int64_t v = rd_sleb(c, 5, 33);
  if (v == -64)
    return &FT_EMPTY;
  if (v >= 0) {
    if ((uint64_t)v >= c->m->ntypes)
      fail(c, "unknown type");
    return &c->m->types[v];
  } else {
    int i = vt_index((int)(v & 0x7f));
    if (i < 0)
      fail(c, "invalid block type");
    return &FT_1[i];
  }
}

static int is_ref(int t) { return t == WASM_FUNCREF || t == WASM_EXTERNREF || t == WASM_EXNREF; }

typedef struct {
  uint32_t mem;
  uint64_t off;
  uint32_t align;
  int addr; /* the memory's address type */
} memarg;

static int mem_addr_type(const C *c, uint32_t mem) { return c->m->mems[mem].is64 ? WASM_I64 : WASM_I32; }

static memarg rd_memarg(C *c, uint32_t max_align)
{
  memarg a;
  uint32_t align = rd_u32(c);
  a.mem = 0;
  if (align & 0x40) {
    a.mem = rd_u32(c);
    align &= ~0x40u;
  }
  if (a.mem >= c->m->nmems)
    fail(c, "unknown memory");
  if (align > max_align)
    fail(c, "alignment must not be larger than natural");
  a.align = align;
  a.addr = mem_addr_type(c, a.mem);
  a.off = a.addr == WASM_I64 ? rd_u64(c) : rd_u32(c);
  return a;
}

static void emit_memarg(C *c, const memarg *a)
{
  emit(c, (uint32_t)a->off);
  emit(c, (uint32_t)(a->off >> 32));
  emit(c, a->mem);
}

static uint32_t rd_memidx(C *c)
{
  uint32_t i = rd_u32(c);
  if (i >= c->m->nmems)
    fail(c, "unknown memory");
  return i;
}

static const struct {
  uint8_t lo, hi;
  char a, b, r;
} NUM[] = {
    {0x45, 0x45, 'i', 0, 'i'},   {0x46, 0x4f, 'i', 'i', 'i'}, {0x50, 0x50, 'I', 0, 'i'}, {0x51, 0x5a, 'I', 'I', 'i'},
    {0x5b, 0x60, 'f', 'f', 'i'}, {0x61, 0x66, 'F', 'F', 'i'}, {0x67, 0x69, 'i', 0, 'i'}, {0x6a, 0x78, 'i', 'i', 'i'},
    {0x79, 0x7b, 'I', 0, 'I'},   {0x7c, 0x8a, 'I', 'I', 'I'}, {0x8b, 0x91, 'f', 0, 'f'}, {0x92, 0x98, 'f', 'f', 'f'},
    {0x99, 0x9f, 'F', 0, 'F'},   {0xa0, 0xa6, 'F', 'F', 'F'}, {0xa7, 0xa7, 'I', 0, 'i'}, {0xa8, 0xa9, 'f', 0, 'i'},
    {0xaa, 0xab, 'F', 0, 'i'},   {0xac, 0xad, 'i', 0, 'I'},   {0xae, 0xaf, 'f', 0, 'I'}, {0xb0, 0xb1, 'F', 0, 'I'},
    {0xb2, 0xb3, 'i', 0, 'f'},   {0xb4, 0xb5, 'I', 0, 'f'},   {0xb6, 0xb6, 'F', 0, 'f'}, {0xb7, 0xb8, 'i', 0, 'F'},
    {0xb9, 0xba, 'I', 0, 'F'},   {0xbb, 0xbb, 'f', 0, 'F'},   {0xbc, 0xbc, 'f', 0, 'i'}, {0xbd, 0xbd, 'F', 0, 'I'},
    {0xbe, 0xbe, 'i', 0, 'f'},   {0xbf, 0xbf, 'I', 0, 'F'},   {0xc0, 0xc1, 'i', 0, 'i'}, {0xc2, 0xc4, 'I', 0, 'I'}};

static int ch_type(char ch)
{
  switch (ch) {
  case 'i':
    return WASM_I32;
  case 'I':
    return WASM_I64;
  case 'f':
    return WASM_F32;
  case 'F':
    return WASM_F64;
  case 'v':
    return WASM_V128;
  }
  return 0;
}

/* Memory access operators 0x28-0x3e: value type and natural alignment (log2) */
static const char MEM_TYPE[] = "iIfFiiiiIIIIIIiIfFiiIII";
static const uint8_t MEM_ALIGN[] = {2, 3, 2, 3, 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 2, 3, 2, 3, 0, 1, 0, 1, 2};

/* SIMD operator signatures (opcode after the 0xFD prefix) */
enum {
  SC_NONE,
  SC_UN,
  SC_BIN,
  SC_TER,
  SC_TEST,
  SC_SHIFT,
  SC_SPLAT,
  SC_EXTRACT,
  SC_REPLACE,
  SC_LOAD,
  SC_STORE,
  SC_LOADLANE,
  SC_STORELANE,
  SC_CONST,
  SC_SHUFFLE
};
typedef struct {
  uint8_t cls, scalar, lanes, align;
} simd_sig;

static int in_set(unsigned s, const uint8_t *set)
{
  for (; *set; set++)
    if (s == *set)
      return 1;
  return 0;
}

static simd_sig simd_info(unsigned s)
{
  static const uint8_t un[] = {0x4d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x67, 0x68, 0x69, 0x6a, 0x74, 0x75, 0x7a,
                               0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x87, 0x88, 0x89, 0x8a, 0x94, 0xa0, 0xa1,
                               0xa7, 0xa8, 0xa9, 0xaa, 0xc0, 0xc1, 0xc7, 0xc8, 0xc9, 0xca, 0xe0, 0xe1, 0xe3,
                               0xec, 0xed, 0xef, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff, 0};
  static const uint8_t test[] = {0x53, 0x63, 0x64, 0x83, 0x84, 0xa3, 0xa4, 0xc3, 0xc4, 0};
  static const uint8_t shift[] = {0x6b, 0x6c, 0x6d, 0x8b, 0x8c, 0x8d, 0xab, 0xac, 0xad, 0xcb, 0xcc, 0xcd, 0};
  static const uint8_t bin[] = {
      0x0e, 0x65, 0x66, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x76, 0x77, 0x78, 0x79, 0x7b, 0x82, 0x85, 0x86, 0x8e, 0x8f,
      0x90, 0x91, 0x92, 0x93, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xae, 0xb1, 0xb5, 0xb6, 0xb7,
      0xb8, 0xb9, 0xba, 0xbc, 0xbd, 0xbe, 0xbf, 0xce, 0xd1, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
      0xdf, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0};
  /* extract/replace lane: scalar type and lane count per opcode 0x15..0x22 */
  static const struct {
    char scalar;
    uint8_t lanes, replace;
  } lane[] = {{'i', 16, 0}, {'i', 16, 0}, {'i', 16, 1}, {'i', 8, 0}, {'i', 8, 0}, {'i', 8, 1}, {'i', 4, 0},
              {'i', 4, 1},  {'I', 2, 0},  {'I', 2, 1},  {'f', 4, 0}, {'f', 4, 1}, {'F', 2, 0}, {'F', 2, 1}};
  simd_sig g = {SC_NONE, 0, 0, 0};
  if (s >= 0x100) {
    if (s == 0x100 || (s >= 0x10d && s <= 0x112))
      g.cls = SC_BIN;
    else if (s >= 0x101 && s <= 0x104)
      g.cls = SC_UN;
    else if ((s >= 0x105 && s <= 0x10c) || s == 0x113)
      g.cls = SC_TER;
    return g;
  }
  if (s <= 0x0a) {
    static const uint8_t al[] = {4, 3, 3, 3, 3, 3, 3, 0, 1, 2, 3};
    g.cls = SC_LOAD;
    g.align = al[s];
  } else if (s == 0x0b) {
    g.cls = SC_STORE;
    g.align = 4;
  } else if (s == 0x0c)
    g.cls = SC_CONST;
  else if (s == 0x0d)
    g.cls = SC_SHUFFLE;
  else if (s >= 0x0f && s <= 0x14) {
    g.cls = SC_SPLAT;
    g.scalar = (uint8_t)"iiiIfF"[s - 0x0f];
  } else if (s >= 0x15 && s <= 0x22) {
    g.cls = lane[s - 0x15].replace ? SC_REPLACE : SC_EXTRACT;
    g.scalar = (uint8_t)lane[s - 0x15].scalar;
    g.lanes = lane[s - 0x15].lanes;
  } else if (s >= 0x23 && s <= 0x4c)
    g.cls = SC_BIN;
  else if (s == 0x52)
    g.cls = SC_TER;
  else if (s >= 0x4e && s <= 0x51)
    g.cls = SC_BIN;
  else if (s >= 0x54 && s <= 0x5b) {
    g.cls = s < 0x58 ? SC_LOADLANE : SC_STORELANE;
    g.align = (uint8_t)((s - 0x54) & 3);
    g.lanes = (uint8_t)(16 >> g.align);
  } else if (s == 0x5c || s == 0x5d) {
    g.cls = SC_LOAD;
    g.align = s == 0x5c ? 2 : 3;
  } else if (in_set(s, un))
    g.cls = SC_UN;
  else if (in_set(s, test))
    g.cls = SC_TEST;
  else if (in_set(s, shift))
    g.cls = SC_SHIFT;
  else if (in_set(s, bin))
    g.cls = SC_BIN;
  return g;
}

static void compile_simd(C *c, unsigned s)
{
  simd_sig g = simd_info(s);
  int live = alive(c);
  uint32_t lane, i;
  memarg ma;
  switch (g.cls) {
  case SC_UN:
    pop_t(c, WASM_V128);
    push(c, WASM_V128);
    break;
  case SC_BIN:
    pop_t(c, WASM_V128);
    pop_t(c, WASM_V128);
    push(c, WASM_V128);
    break;
  case SC_TER:
    pop_t(c, WASM_V128);
    pop_t(c, WASM_V128);
    pop_t(c, WASM_V128);
    push(c, WASM_V128);
    break;
  case SC_TEST:
    pop_t(c, WASM_V128);
    push(c, WASM_I32);
    break;
  case SC_SHIFT:
    pop_t(c, WASM_I32);
    pop_t(c, WASM_V128);
    push(c, WASM_V128);
    break;
  case SC_SPLAT:
    pop_t(c, ch_type((char)g.scalar));
    push(c, WASM_V128);
    break;
  case SC_EXTRACT:
  case SC_REPLACE:
    lane = rd_u8(c);
    if (lane >= g.lanes)
      fail(c, "invalid lane index");
    if (g.cls == SC_REPLACE)
      pop_t(c, ch_type((char)g.scalar));
    pop_t(c, WASM_V128);
    push(c, g.cls == SC_REPLACE ? WASM_V128 : ch_type((char)g.scalar));
    if (live) {
      emit(c, OP_FD(s));
      emit(c, lane);
    }
    return;
  case SC_LOAD:
    ma = rd_memarg(c, g.align);
    pop_t(c, ma.addr);
    push(c, WASM_V128);
    if (live) {
      emit(c, OP_FD(s));
      emit_memarg(c, &ma);
    }
    return;
  case SC_STORE:
    ma = rd_memarg(c, g.align);
    pop_t(c, WASM_V128);
    pop_t(c, ma.addr);
    if (live) {
      emit(c, OP_FD(s));
      emit_memarg(c, &ma);
    }
    return;
  case SC_LOADLANE:
  case SC_STORELANE:
    ma = rd_memarg(c, g.align);
    lane = rd_u8(c);
    if (lane >= g.lanes)
      fail(c, "invalid lane index");
    pop_t(c, WASM_V128);
    pop_t(c, ma.addr);
    if (g.cls == SC_LOADLANE)
      push(c, WASM_V128);
    if (live) {
      emit(c, OP_FD(s));
      emit_memarg(c, &ma);
      emit(c, lane);
    }
    return;
  case SC_CONST:
    need(c, 16);
    if (live) {
      emit(c, OP_FD(s));
      for (i = 0; i < 4; i++) {
        uint32_t w;
        memcpy(&w, c->p + 4 * i, 4);
        emit(c, w);
      }
    }
    c->p += 16;
    push(c, WASM_V128);
    return;
  case SC_SHUFFLE:
    need(c, 16);
    for (i = 0; i < 16; i++)
      if (c->p[i] >= 32)
        fail(c, "invalid lane index");
    pop_t(c, WASM_V128);
    pop_t(c, WASM_V128);
    push(c, WASM_V128);
    if (live) {
      emit(c, OP_FD(s));
      for (i = 0; i < 4; i++) {
        uint32_t w;
        memcpy(&w, c->p + 4 * i, 4);
        emit(c, w);
      }
    }
    c->p += 16;
    return;
  default:
    fail(c, "unknown SIMD opcode");
  }
  if (live)
    emit(c, OP_FD(s));
}

/* Atomic operators (0xFE prefix): wait/notify, fence, and loads, stores and read-modify-writes of 8 to 64 bits */
static void compile_atomic(C *c, unsigned s)
{
  static const uint8_t LOG[7] = {2, 3, 0, 1, 0, 1, 2};
  static const uint8_t TY[7] = {WASM_I32, WASM_I64, WASM_I32, WASM_I32, WASM_I64, WASM_I64, WASM_I64};
  int live = alive(c);
  memarg ma;
  if (s == 0x03) {
    if (rd_u8(c) != 0)
      fail(c, "zero byte expected");
    if (live)
      emit(c, OP_FE(s));
    return;
  }
  if (s <= 0x02) {
    ma = rd_memarg(c, s == 2 ? 3 : 2);
    if (ma.align != (s == 2 ? 3u : 2u))
      fail(c, "atomic alignment must be natural");
    if (s == 0) {
      pop_t(c, WASM_I32);
    } else {
      pop_t(c, WASM_I64);
      pop_t(c, s == 1 ? WASM_I32 : WASM_I64);
    }
    pop_t(c, ma.addr);
    push(c, WASM_I32);
  } else if (s >= 0x10 && s <= 0x4e) {
    unsigned g = (s - 0x10) / 7, v = (s - 0x10) % 7;
    int t = TY[v];
    ma = rd_memarg(c, LOG[v]);
    if (ma.align != LOG[v])
      fail(c, "atomic alignment must be natural");
    if (g == 8)
      pop_t(c, t);
    if (g >= 1)
      pop_t(c, t);
    pop_t(c, ma.addr);
    if (g != 1)
      push(c, t);
  } else
    fail(c, "unknown atomic opcode");
  if (live) {
    emit(c, OP_FE(s));
    emit_memarg(c, &ma);
  }
}

static void compile_fc(C *c, unsigned s)
{
  wasm_module *m = c->m;
  int live = alive(c);
  uint32_t a = 0, b = 0;
  switch (s) {
  case 0:
  case 1:
  case 2:
  case 3:
  case 4:
  case 5:
  case 6:
  case 7:
    pop_t(c, (s & 2) ? WASM_F64 : WASM_F32);
    push(c, s < 4 ? WASM_I32 : WASM_I64);
    break;
  case 8: { /* memory.init */
    int addr;
    a = rd_u32(c);
    b = rd_memidx(c);
    addr = mem_addr_type(c, b);
    if (!c->has_datacount)
      fail(c, "data count section required");
    if (a >= c->datacount)
      fail(c, "unknown data segment");
    pop_t(c, WASM_I32);
    pop_t(c, WASM_I32);
    pop_t(c, addr);
    break;
  }
  case 9: /* data.drop */
    a = rd_u32(c);
    if (!c->has_datacount)
      fail(c, "data count section required");
    if (a >= c->datacount)
      fail(c, "unknown data segment");
    break;
  case 10: { /* memory.copy */
    int da, sa;
    a = rd_memidx(c);
    b = rd_memidx(c);
    da = mem_addr_type(c, a);
    sa = mem_addr_type(c, b);
    pop_t(c, da == WASM_I64 && sa == WASM_I64 ? WASM_I64 : WASM_I32);
    pop_t(c, sa);
    pop_t(c, da);
    break;
  }
  case 11: { /* memory.fill */
    int addr;
    a = rd_memidx(c);
    addr = mem_addr_type(c, a);
    pop_t(c, addr);
    pop_t(c, WASM_I32);
    pop_t(c, addr);
    break;
  }
  case 12: /* table.init */
    a = rd_u32(c);
    b = rd_u32(c);
    if (a >= m->nelems)
      fail(c, "unknown element segment");
    if (b >= m->ntables)
      fail(c, "unknown table");
    if (m->elems[a].type != m->tables[b].elem)
      fail(c, "type mismatch");
    pop_t(c, WASM_I32);
    pop_t(c, WASM_I32);
    pop_t(c, WASM_I32);
    break;
  case 13: /* elem.drop */
    a = rd_u32(c);
    if (a >= m->nelems)
      fail(c, "unknown element segment");
    break;
  case 14: /* table.copy */
    a = rd_u32(c);
    b = rd_u32(c);
    if (a >= m->ntables || b >= m->ntables)
      fail(c, "unknown table");
    if (m->tables[a].elem != m->tables[b].elem)
      fail(c, "type mismatch");
    pop_t(c, WASM_I32);
    pop_t(c, WASM_I32);
    pop_t(c, WASM_I32);
    break;
  case 15: /* table.grow */
    a = rd_u32(c);
    if (a >= m->ntables)
      fail(c, "unknown table");
    pop_t(c, WASM_I32);
    pop_t(c, m->tables[a].elem);
    push(c, WASM_I32);
    break;
  case 16: /* table.size */
    a = rd_u32(c);
    if (a >= m->ntables)
      fail(c, "unknown table");
    push(c, WASM_I32);
    break;
  case 17: /* table.fill */
    a = rd_u32(c);
    if (a >= m->ntables)
      fail(c, "unknown table");
    pop_t(c, WASM_I32);
    pop_t(c, m->tables[a].elem);
    pop_t(c, WASM_I32);
    break;
  default:
    fail(c, "unknown 0xFC opcode");
  }
  if (live) {
    emit(c, OP_FC(s));
    if (s == 9 || s == 13 || s == 15 || s == 16 || s == 17)
      emit(c, a);
    else if (s == 8 || s == 10 || s == 12 || s == 14) {
      emit(c, a);
      emit(c, b);
    } else if (s == 11)
      emit(c, a);
  }
}

/* At the end of a try_table: one handler per catch clause */
static void finish_trytable(C *c, ctrl *f)
{
  uint32_t j;
  for (j = f->cl_begin; j < f->cl_end; j++) {
    const tclause *cl = &c->tcl[j];
    ctrl *t = &c->cs[cl->label];
    wasm_handler h;
    uint32_t hi;
    memset(&h, 0, sizeof h);
    h.start = f->start_pc;
    h.end = c->ncode;
    h.tag = cl->tag;
    h.kind = (cl->kind & 1) ? H_CATCH_REF : H_CATCH;
    h.arity = (cl->tag >= 0 ? c->m->types[c->m->tag_type[cl->tag]].pslots : 0) + (cl->kind & 1);
    h.dest = c->nlocal_slots + t->sheight;
    hi = add_handler(c, h);
    handler_target(c, t, hi);
  }
}

static void compile_body(C *c, uint32_t def, const uint8_t *body, uint32_t size)
{
  wasm_module *m = c->m;
  const wasm_functype *ft = &m->types[m->func_type[m->nfunc_imports + def]];
  const uint8_t *bend = body + size;
  uint32_t i, nl, ngroups, total;
  wasm_code *wc = &m->code[def];
  int done = 0;

  c->p = body;
  c->end = bend;
  c->nvt = c->ncs = c->ncode = 0;
  c->nhv = c->ntcl = 0;
  c->sh = c->maxsh = 0;

  /* locals: parameters first, then the declared groups */
  ngroups = rd_count(c);
  total = ft->np;
  {
    const uint8_t *save = c->p;
    for (i = 0; i < ngroups; i++) {
      uint32_t n = rd_u32(c);
      rd_valtype(c);
      if (n > MAX_LOCALS || total + n > MAX_LOCALS)
        fail(c, "too many locals");
      total += n;
    }
    c->p = save;
  }
  c->nloc = total;
  if (total > c->caploc) {
    c->lslot = (uint32_t *)vec_grow(c, c->lslot, &c->caploc, total, sizeof *c->lslot);
    c->ltype = (uint8_t *)realloc(c->ltype, c->caploc);
    if (!c->ltype)
      fail(c, "out of memory");
  }
  nl = 0;
  c->nlocal_slots = 0;
  for (i = 0; i < ft->np; i++, nl++) {
    c->ltype[nl] = ft->p[i];
    c->lslot[nl] = c->nlocal_slots;
    c->nlocal_slots += slots_of(ft->p[i]);
  }
  for (i = 0; i < ngroups; i++) {
    uint32_t n = rd_u32(c), k;
    uint8_t t = rd_valtype(c);
    for (k = 0; k < n; k++, nl++) {
      c->ltype[nl] = t;
      c->lslot[nl] = c->nlocal_slots;
      c->nlocal_slots += slots_of(t);
    }
  }

  c->fbt = *ft;
  c->fbt.np = c->fbt.pslots = 0;
  push_ctrl(c, K_FUNC, &c->fbt);

  while (!done) {
    uint8_t op = rd_u8(c);
    int live = alive(c);
    switch (op) {
    case 0x00:
      if (live)
        emit(c, op);
      set_unreachable(c);
      break;
    case 0x01:
      break;
    case 0x02:
    case 0x03: {
      const wasm_functype *bt = read_blocktype(c);
      push_ctrl(c, op == 0x02 ? K_BLOCK : K_LOOP, bt);
      break;
    }
    case 0x04: {
      const wasm_functype *bt = read_blocktype(c);
      ctrl *f;
      pop_t(c, WASM_I32);
      push_ctrl(c, K_IF, bt);
      f = top(c);
      if (!f->dead) {
        emit(c, OP_JMP_UNLESS);
        f->else_patch = c->ncode + 1;
        emit(c, 0);
      }
      break;
    }
    case 0x05: {
      ctrl *f = top(c);
      if (f->kind != K_IF)
        fail(c, "else without a matching if");
      pop_vals(c, f->bt->r, f->bt->nr);
      if (c->nvt != f->height)
        fail(c, "type mismatch: operands left on stack");
      if (!f->dead) {
        if (!f->unreachable) {
          emit(c, OP_JMP);
          emit_fwd(c, f);
        }
        c->code[f->else_patch - 1] = c->ncode;
        f->else_patch = 0;
      }
      f->kind = K_ELSE;
      f->unreachable = 0;
      c->nvt = f->height;
      c->sh = f->sheight;
      push_vals(c, f->bt->p, f->bt->np);
      break;
    }
    case 0x06: {
      const wasm_functype *bt = read_blocktype(c);
      ctrl *f;
      push_ctrl(c, K_TRY, bt);
      f = top(c);
      f->start_pc = c->ncode;
      if (!f->dead)
        emit(c, 0x01);
      break;
    }
    case 0x1f: {
      const wasm_functype *bt = read_blocktype(c);
      uint32_t ncl = rd_count(c), k, cl_begin = c->ntcl;
      ctrl *f;
      for (k = 0; k < ncl; k++) {
        uint8_t kind = rd_u8(c);
        int32_t tag = -1;
        uint32_t depth, np = 0, tn;
        const uint8_t *ts, *tp = NULL;
        ctrl *t;
        if (kind > 3)
          fail(c, "invalid catch clause");
        if (kind < 2) {
          uint32_t tg = rd_u32(c);
          if (tg >= m->ntags)
            fail(c, "unknown tag");
          tag = (int32_t)tg;
          np = m->types[m->tag_type[tg]].np;
          tp = m->types[m->tag_type[tg]].p;
        }
        depth = rd_u32(c);
        t = label(c, depth);
        label_types(t, &ts, &tn);
        if (tn != np + (kind & 1u) || (np && memcmp(ts, tp, np)) || ((kind & 1) && ts[np] != WASM_EXNREF))
          fail(c, "type mismatch in catch clause");
        if (c->ntcl >= c->captcl)
          c->tcl = (tclause *)vec_grow(c, c->tcl, &c->captcl, c->ntcl + 1, sizeof *c->tcl);
        c->tcl[c->ntcl].kind = kind;
        c->tcl[c->ntcl].tag = tag;
        c->tcl[c->ntcl].label = c->ncs - 1 - depth;
        c->ntcl++;
      }
      push_ctrl(c, K_TRYTABLE, bt);
      f = top(c);
      f->cl_begin = cl_begin;
      f->cl_end = c->ntcl;
      f->start_pc = c->ncode;
      if (!f->dead)
        emit(c, 0x01);
      break;
    }
    case 0x07:
    case 0x19: {
      ctrl *f = top(c);
      uint32_t tg = 0;
      const wasm_functype *tt = NULL;
      if (op == 0x07) {
        tg = rd_u32(c);
        if (tg >= m->ntags)
          fail(c, "unknown tag");
        tt = &m->types[m->tag_type[tg]];
      }
      if (f->kind != K_TRY && f->kind != K_CATCH)
        fail(c, "catch without a matching try");
      pop_vals(c, f->bt->r, f->bt->nr);
      if (c->nvt != f->height)
        fail(c, "type mismatch: operands left on stack");
      if (!f->dead) {
        wasm_handler h;
        if (f->kind == K_TRY)
          f->body_end = c->ncode;
        if (!f->unreachable) {
          if (f->kind == K_CATCH) {
            emit(c, OP_SQUASH);
            emit(c, f->bt->rslots);
          }
          emit(c, OP_JMP);
          emit_fwd(c, f);
        }
        memset(&h, 0, sizeof h);
        h.start = f->start_pc;
        h.end = f->body_end;
        h.tag = tt ? (int32_t)tg : -1;
        h.kind = H_LEGACY;
        h.target = c->ncode;
        h.dest = c->nlocal_slots + f->sheight;
        add_handler(c, h);
      }
      f->kind = K_CATCH;
      f->hidden = 1;
      f->unreachable = 0;
      c->nvt = f->height;
      c->sh = f->sheight + 1;
      if (c->sh > c->maxsh)
        c->maxsh = c->sh;
      if (tt)
        push_vals(c, tt->p, tt->np);
      break;
    }
    case 0x18: {
      uint32_t l = rd_u32(c);
      ctrl *f = top(c), *t;
      const wasm_functype *bt = f->bt;
      if (f->kind != K_TRY)
        fail(c, "delegate without a matching try");
      t = label(c, l + 1);
      pop_vals(c, bt->r, bt->nr);
      if (c->nvt != f->height)
        fail(c, "type mismatch: operands left on stack");
      if (!f->dead) {
        wasm_handler h;
        memset(&h, 0, sizeof h);
        h.start = f->start_pc;
        h.end = c->ncode;
        h.kind = H_DELEGATE;
        h.tag = -1;
        h.resume = resume_pc(t);
        add_handler(c, h);
      }
      patch_to(c, f->patch, c->ncode);
      c->ncs--;
      c->nvt = f->height;
      c->sh = f->sheight;
      push_vals(c, bt->r, bt->nr);
      break;
    }
    case 0x08: {
      uint32_t tg = rd_u32(c);
      const wasm_functype *tt;
      if (tg >= m->ntags)
        fail(c, "unknown tag");
      tt = &m->types[m->tag_type[tg]];
      pop_vals(c, tt->p, tt->np);
      if (live) {
        emit(c, op);
        emit(c, tg);
      }
      set_unreachable(c);
      break;
    }
    case 0x09: {
      ctrl *t = label(c, rd_u32(c));
      if (t->kind != K_CATCH)
        fail(c, "rethrow must target a catch");
      if (live) {
        emit(c, op);
        emit(c, c->nlocal_slots + t->sheight);
      }
      set_unreachable(c);
      break;
    }
    case 0x0a:
      pop_t(c, WASM_EXNREF);
      if (live)
        emit(c, op);
      set_unreachable(c);
      break;
    case 0x0b: {
      ctrl *f = top(c);
      const wasm_functype *bt = f->bt;
      pop_vals(c, bt->r, bt->nr);
      if (c->nvt != f->height)
        fail(c, "type mismatch: operands left on stack");
      if (f->kind == K_CATCH && live) {
        emit(c, OP_SQUASH);
        emit(c, bt->rslots);
      }
      if (f->kind == K_TRYTABLE && !f->dead)
        finish_trytable(c, f);
      if (f->kind == K_IF) {
        /* no else arm: the missing one must pass its parameters through as results */
        if (bt->np != bt->nr || memcmp(bt->p, bt->r, bt->np))
          fail(c, "type mismatch: if without else");
        if (f->else_patch)
          c->code[f->else_patch - 1] = c->ncode;
      }
      if (f->kind == K_FUNC) {
        emit(c, OP_RETURN);
        patch_to(c, f->patch, c->ncode - 1);
        c->ncs--;
        done = 1;
      } else {
        patch_to(c, f->patch, c->ncode);
        c->ncs--;
        c->nvt = f->height;
        c->sh = f->sheight;
        push_vals(c, bt->r, bt->nr);
      }
      break;
    }
    case 0x0c: {
      ctrl *t = label(c, rd_u32(c));
      const uint8_t *ts;
      uint32_t n;
      label_types(t, &ts, &n);
      pop_vals(c, ts, n);
      if (live) {
        if (t->kind == K_FUNC)
          emit(c, OP_RETURN);
        else if (c->sh == t->sheight) {
          emit(c, OP_JMP);
          emit_target(c, t);
        } else {
          emit(c, OP_BR);
          emit_br_triple(c, t);
        }
      }
      set_unreachable(c);
      break;
    }
    case 0x0d: {
      ctrl *t = label(c, rd_u32(c));
      const uint8_t *ts;
      uint32_t n;
      pop_t(c, WASM_I32);
      label_types(t, &ts, &n);
      pop_vals(c, ts, n);
      if (live) {
        if (t->kind == K_FUNC) {
          uint32_t p;
          emit(c, OP_JMP_UNLESS);
          p = c->ncode;
          emit(c, 0);
          emit(c, OP_RETURN);
          c->code[p] = c->ncode;
        } else if (c->sh == t->sheight) {
          emit(c, OP_JMP_IF);
          emit_target(c, t);
        } else {
          emit(c, OP_BR_IF);
          emit_br_triple(c, t);
        }
      }
      push_vals(c, ts, n);
      break;
    }
    case 0x0e: {
      uint32_t n = rd_count(c), k, arity = 0;
      const uint8_t *ts = NULL;
      uint32_t tn = 0;
      pop_t(c, WASM_I32);
      if (live) {
        emit(c, OP_BR_TABLE);
        emit(c, n);
      }
      for (k = 0; k <= n; k++) {
        ctrl *t = label(c, rd_u32(c));
        label_types(t, &ts, &tn);
        if (k == 0)
          arity = tn;
        else if (tn != arity)
          fail(c, "br_table targets have inconsistent arity");
        pop_vals(c, ts, tn);
        push_vals(c, ts, tn);
        if (live)
          emit_br_triple(c, t);
      }
      pop_vals(c, ts, tn);
      set_unreachable(c);
      break;
    }
    case 0x0f:
      pop_vals(c, ft->r, ft->nr);
      if (live)
        emit(c, OP_RETURN);
      set_unreachable(c);
      break;
    case 0x10: {
      uint32_t f = rd_u32(c);
      const wasm_functype *t;
      if (f >= m->nfuncs)
        fail(c, "unknown function");
      t = &m->types[m->func_type[f]];
      pop_vals(c, t->p, t->np);
      push_vals(c, t->r, t->nr);
      if (live) {
        emit(c, op);
        emit(c, f);
      }
      break;
    }
    case 0x12: {
      uint32_t f = rd_u32(c);
      const wasm_functype *t;
      if (f >= m->nfuncs)
        fail(c, "unknown function");
      t = &m->types[m->func_type[f]];
      if (!wasm_results_eq(t, ft))
        fail(c, "return_call callee results do not match the caller");
      pop_vals(c, t->p, t->np);
      if (live) {
        emit(c, op);
        emit(c, f);
      }
      set_unreachable(c);
      break;
    }
    case 0x13: {
      uint32_t ti = rd_u32(c), tb = rd_u32(c);
      const wasm_functype *t;
      if (ti >= m->ntypes)
        fail(c, "unknown type");
      if (tb >= m->ntables)
        fail(c, "unknown table");
      if (m->tables[tb].elem != WASM_FUNCREF)
        fail(c, "return_call_indirect requires a funcref table");
      t = &m->types[ti];
      if (!wasm_results_eq(t, ft))
        fail(c, "return_call_indirect callee results do not match the caller");
      pop_t(c, WASM_I32);
      pop_vals(c, t->p, t->np);
      if (live) {
        emit(c, op);
        emit(c, ti);
        emit(c, tb);
      }
      set_unreachable(c);
      break;
    }
    case 0x11: {
      uint32_t ti = rd_u32(c), tb = rd_u32(c);
      const wasm_functype *t;
      if (ti >= m->ntypes)
        fail(c, "unknown type");
      if (tb >= m->ntables)
        fail(c, "unknown table");
      if (m->tables[tb].elem != WASM_FUNCREF)
        fail(c, "call_indirect requires a funcref table");
      t = &m->types[ti];
      pop_t(c, WASM_I32);
      pop_vals(c, t->p, t->np);
      push_vals(c, t->r, t->nr);
      if (live) {
        emit(c, op);
        emit(c, ti);
        emit(c, tb);
      }
      break;
    }
    case 0x1a: {
      int t = pop(c);
      if (live)
        emit(c, t == WASM_V128 ? OP_DROP2 : op);
      break;
    }
    case 0x1b:
    case 0x1c: {
      int t1, t2;
      if (op == 0x1c) {
        uint32_t n = rd_count(c);
        if (n != 1)
          fail(c, "invalid result arity for select");
        t1 = rd_valtype(c);
      } else
        t1 = 0;
      pop_t(c, WASM_I32);
      t2 = pop(c);
      {
        int t3 = pop(c);
        if (op == 0x1b) {
          if (is_ref(t2) || is_ref(t3))
            fail(c, "select without a type requires numeric operands");
          if (t2 && t3 && t2 != t3)
            fail(c, "type mismatch");
          t1 = t2 ? t2 : t3;
        } else if ((t2 && t2 != t1) || (t3 && t3 != t1))
          fail(c, "type mismatch");
      }
      push(c, t1);
      if (live)
        emit(c, t1 == WASM_V128 ? OP_SELECT2 : 0x1b);
      break;
    }
    case 0x20:
    case 0x21:
    case 0x22: {
      uint32_t x = rd_u32(c);
      int t;
      if (x >= c->nloc)
        fail(c, "unknown local");
      t = c->ltype[x];
      if (op == 0x20)
        push(c, t);
      else {
        pop_t(c, t);
        if (op == 0x22)
          push(c, t);
      }
      if (live) {
        emit(c, t == WASM_V128 ? (op == 0x20 ? OP_LOCAL_GET2 : op == 0x21 ? OP_LOCAL_SET2 : OP_LOCAL_TEE2) : op);
        emit(c, c->lslot[x]);
      }
      break;
    }
    case 0x23:
    case 0x24: {
      uint32_t x = rd_u32(c);
      int t, o;
      if (x >= m->nglobals)
        fail(c, "unknown global");
      t = m->globals[x].type;
      if (op == 0x23) {
        push(c, t);
        o = t == WASM_V128 ? OP_GLOBAL_GET2 : t == WASM_EXTERNREF ? OP_GLOBAL_GET_X : 0x23;
      } else {
        if (!m->globals[x].mut)
          fail(c, "global is immutable");
        pop_t(c, t);
        o = t == WASM_V128 ? OP_GLOBAL_SET2 : t == WASM_EXTERNREF ? OP_GLOBAL_SET_X : 0x24;
      }
      if (live) {
        emit(c, (uint32_t)o);
        emit(c, x);
      }
      break;
    }
    case 0x25:
    case 0x26: {
      uint32_t x = rd_u32(c);
      if (x >= m->ntables)
        fail(c, "unknown table");
      if (op == 0x25) {
        pop_t(c, WASM_I32);
        push(c, m->tables[x].elem);
      } else {
        pop_t(c, m->tables[x].elem);
        pop_t(c, WASM_I32);
      }
      if (live) {
        emit(c, op);
        emit(c, x);
      }
      break;
    }
    case 0x3f:
    case 0x40: {
      uint32_t mi = rd_memidx(c);
      int addr = mem_addr_type(c, mi);
      if (op == 0x40)
        pop_t(c, addr);
      push(c, addr);
      if (live) {
        emit(c, op);
        emit(c, mi);
      }
      break;
    }
    case 0x41:
      push(c, WASM_I32);
      if (live) {
        emit(c, op);
        emit(c, (uint32_t)rd_s32(c));
      } else
        rd_s32(c);
      break;
    case 0x42: {
      uint64_t v = (uint64_t)rd_s64(c);
      push(c, WASM_I64);
      if (live) {
        emit(c, op);
        emit(c, (uint32_t)v);
        emit(c, (uint32_t)(v >> 32));
      }
      break;
    }
    case 0x43: {
      uint32_t v;
      need(c, 4);
      memcpy(&v, c->p, 4);
      c->p += 4;
      push(c, WASM_F32);
      if (live) {
        emit(c, op);
        emit(c, v);
      }
      break;
    }
    case 0x44: {
      uint32_t v[2];
      need(c, 8);
      memcpy(v, c->p, 8);
      c->p += 8;
      push(c, WASM_F64);
      if (live) {
        emit(c, op);
        emit(c, v[0]);
        emit(c, v[1]);
      }
      break;
    }
    case 0xd0:
      push(c, rd_nulltype(c));
      if (live)
        emit(c, op);
      break;
    case 0xd1: {
      int t = pop(c);
      if (t && !is_ref(t))
        fail(c, "ref.is_null requires a reference");
      push(c, WASM_I32);
      if (live)
        emit(c, op);
      break;
    }
    case 0xd2: {
      uint32_t f = rd_u32(c);
      if (f >= m->nfuncs)
        fail(c, "unknown function");
      push(c, WASM_FUNCREF);
      if (live) {
        emit(c, op);
        emit(c, f);
      }
      break;
    }
    case 0xfc:
      compile_fc(c, rd_u32(c));
      break;
    case 0xfd:
      compile_simd(c, rd_u32(c));
      break;
    case 0xfe:
      compile_atomic(c, rd_u32(c));
      break;
    default:
      if (op >= 0x28 && op <= 0x3e) {
        memarg ma = rd_memarg(c, MEM_ALIGN[op - 0x28]);
        int vt = ch_type(MEM_TYPE[op - 0x28]);
        if (op < 0x36) {
          pop_t(c, ma.addr);
          push(c, vt);
        } else {
          pop_t(c, vt);
          pop_t(c, ma.addr);
        }
        if (live) {
          emit(c, op);
          emit_memarg(c, &ma);
        }
      } else if (op >= 0x45 && op <= 0xc4) {
        unsigned k;
        for (k = 0; k < sizeof NUM / sizeof *NUM; k++)
          if (op >= NUM[k].lo && op <= NUM[k].hi)
            break;
        if (k == sizeof NUM / sizeof *NUM)
          fail(c, "unknown opcode");
        if (NUM[k].b)
          pop_t(c, ch_type(NUM[k].b));
        pop_t(c, ch_type(NUM[k].a));
        push(c, ch_type(NUM[k].r));
        if (live)
          emit(c, op);
      } else {
        c->p--;
        fail(c, "unknown opcode");
      }
    }
  }
  if (c->p != bend)
    fail(c, "operators remaining after the end of the function");

  wc->handlers = (wasm_handler *)ma(c, c->nhv * sizeof *wc->handlers);
  memcpy(wc->handlers, c->hv, c->nhv * sizeof *wc->handlers);
  wc->nhandlers = c->nhv;
  wc->code = (uint32_t *)ma(c, c->ncode * sizeof(uint32_t));
  memcpy(wc->code, c->code, c->ncode * sizeof(uint32_t));
  wc->ncode = c->ncode;
  wc->nlocals = c->nlocal_slots;
  wc->maxstack = c->maxsh + 4;
}

static void sec_code(C *c)
{
  wasm_module *m = c->m;
  uint32_t n = rd_count(c), i;
  if (n != m->nfuncs - m->nfunc_imports)
    fail(c, "function and code section have inconsistent lengths");
  m->code = (wasm_code *)ma(c, n * sizeof *m->code);
  for (i = 0; i < n; i++) {
    uint32_t size = rd_u32(c);
    const uint8_t *secend = c->end;
    const uint8_t *body;
    need(c, size);
    body = c->p;
    compile_body(c, i, body, size);
    c->p = body + size;
    c->end = secend;
  }
}

/* ---- module -------------------------------------------------------------- */

static int section_rank(uint8_t id)
{
  switch (id) {
  case 13:
    return 6;
  case 12:
    return 12;
  case 10:
    return 13;
  case 11:
    return 14;
  }
  return id >= 1 && id <= 9 ? (id <= 5 ? id : id + 1) : -1;
}

static void parse_module(C *c)
{
  wasm_module *m = c->m;
  int last = 0;
  static const uint8_t magic[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  if (c->end - c->p < 4 || memcmp(c->p, magic, 4))
    fail(c, "expected magic word 00 61 73 6d");
  if (c->end - c->p < 8 || memcmp(c->p + 4, magic + 4, 4))
    fail(c, "expected version 01 00 00 00");
  c->p += 8;
  m->start = -1;
  while (c->p < c->end) {
    uint8_t id = rd_u8(c);
    uint32_t size = rd_u32(c);
    const uint8_t *secend, *saved_end = c->end;
    int rank;
    need(c, size);
    secend = c->p + size;
    c->end = secend;
    if (id == 0) {
      wasm_custom *cu;
      uint32_t len;
      m->custom = (wasm_custom *)realloc(m->custom, (m->ncustom + 1) * sizeof *m->custom);
      if (!m->custom)
        fail(c, "out of memory");
      cu = &m->custom[m->ncustom];
      cu->name = rd_name(c);
      len = (uint32_t)(secend - c->p);
      cu->bytes = (uint8_t *)ma(c, len);
      memcpy(cu->bytes, c->p, len);
      cu->len = len;
      m->ncustom++;
      c->p = secend;
    } else {
      rank = section_rank(id);
      if (rank < 0)
        fail(c, "unknown section id");
      if (rank <= last)
        fail(c, "unexpected section order");
      last = rank;
      switch (id) {
      case 1:
        sec_types(c);
        break;
      case 2:
        sec_imports(c);
        break;
      case 3:
        sec_functions(c);
        break;
      case 4:
        sec_tables(c);
        break;
      case 5:
        sec_memories(c);
        break;
      case 6:
        sec_globals(c);
        break;
      case 7:
        sec_exports(c);
        break;
      case 8:
        sec_start(c);
        break;
      case 9:
        sec_elements(c);
        break;
      case 12:
        c->datacount = rd_u32(c);
        c->has_datacount = 1;
        break;
      case 13:
        sec_tags(c);
        break;
      case 10:
        sec_code(c);
        break;
      case 11:
        sec_data(c);
        break;
      }
      if (c->p != secend)
        fail(c, "section size mismatch");
    }
    c->end = saved_end;
  }
  if (m->nfuncs - m->nfunc_imports && !m->code)
    fail(c, "function and code section have inconsistent lengths");
  if (c->has_datacount && c->datacount != m->ndatas)
    fail(c, "data count and data section have inconsistent lengths");
}

wasm_module *wasm_module_new(const uint8_t *bytes, size_t len, char *err, size_t errn)
{
  C *c = (C *)calloc(1, sizeof *c);
  wasm_module *m = (wasm_module *)calloc(1, sizeof *m);
  volatile int ok = 0;
  if (!c || !m) {
    free(c);
    free(m);
    if (err && errn)
      put_str(err, errn, 0, "out of memory");
    return NULL;
  }
  c->m = m;
  m->refs = 1;
  c->base = c->p = bytes;
  c->end = bytes + len;
  c->err = err;
  c->errn = errn;
  if (err && errn)
    err[0] = 0;
  if (!setjmp(c->jb)) {
    parse_module(c);
    ok = 1;
  }
  free(c->vt);
  free(c->cs);
  free(c->code);
  free(c->lslot);
  free(c->ltype);
  free(c->hv);
  free(c->tcl);
  free(c);
  if (!ok) {
    wasm_module_free(m);
    return NULL;
  }
  return m;
}

void wasm_module_retain(wasm_module *m) { __atomic_fetch_add(&m->refs, 1, __ATOMIC_RELAXED); }

void wasm_module_free(wasm_module *m)
{
  blk *b;
  if (!m || __atomic_fetch_sub(&m->refs, 1, __ATOMIC_ACQ_REL) != 1)
    return;
  b = (blk *)m->arena;
  while (b) {
    blk *n = b->next;
    free(b);
    b = n;
  }
  free(m->custom);
  free(m);
}
