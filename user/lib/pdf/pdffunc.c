/* pdffunc.c, PDF function objects: sampled (0), exponential (2), stitching (3), PostScript calculator (4) */
#include "math.h"
#include "owos.h"
#include "pdfint.h"

#define MAXIO 8
#define STACK 100

enum {
  OP_NUM,
  OP_BEGIN,
  OP_END,
  OP_ABS,
  OP_ADD,
  OP_AND,
  OP_ATAN,
  OP_BITSHIFT,
  OP_CEILING,
  OP_COPY,
  OP_COS,
  OP_CVI,
  OP_CVR,
  OP_DIV,
  OP_DUP,
  OP_EQ,
  OP_EXCH,
  OP_EXP,
  OP_FALSE,
  OP_FLOOR,
  OP_GE,
  OP_GT,
  OP_IDIV,
  OP_IF,
  OP_IFELSE,
  OP_INDEX,
  OP_LE,
  OP_LN,
  OP_LOG,
  OP_LT,
  OP_MOD,
  OP_MUL,
  OP_NE,
  OP_NEG,
  OP_NOT,
  OP_OR,
  OP_POP,
  OP_ROLL,
  OP_ROUND,
  OP_SIN,
  OP_SQRT,
  OP_SUB,
  OP_TRUE,
  OP_TRUNCATE,
  OP_XOR,
  OP_COUNT
};

static const char *const op_names[OP_COUNT] = {
    "",       "{",     "}",     "abs", "add",  "and", "atan",  "bitshift", "ceiling", "copy", "cos",  "cvi",
    "cvr",    "div",   "dup",   "eq",  "exch", "exp", "false", "floor",    "ge",      "gt",   "idiv", "if",
    "ifelse", "index", "le",    "ln",  "log",  "lt",  "mod",   "mul",      "ne",      "neg",  "not",  "or",
    "pop",    "roll",  "round", "sin", "sqrt", "sub", "true",  "truncate", "xor"};

struct tk {
  unsigned char op;
  double v;
  int match;
};

struct pfunc {
  int type, nin, nout;
  double domain[MAXIO * 2], range[MAXIO * 2];
  int has_range;
  /* type 0 */
  int size[MAXIO], bps;
  double encode[MAXIO * 2], decode[MAXIO * 2];
  const unsigned char *samples;
  long nsamples;
  /* type 2 */
  double c0[MAXIO], c1[MAXIO], n;
  /* type 3 */
  int nk;
  struct pfunc **k;
  double *bounds, *kenc;
  /* type 4 */
  struct tk *code;
  int ncode;
};

static double arr_num(struct pdf *d, struct pobj a, int i, double def) { return pdf_num(pdf_at(d, a, i), def); }

static int parse_ps(struct pdf *d, struct arena *ar, struct pfunc *f, const unsigned char *src, int len)
{
  struct lex l = {src, len, 0, ar, 0};
  int cap = 64, n = 0, stack[32], sp = 0;
  struct tk *code = arena_alloc(ar, (unsigned)cap * sizeof *code);
  struct pobj t;
  (void)d;
  while (code && pdf_lex(&l, &t)) {
    if (n + 1 >= cap) {
      struct tk *nc = arena_alloc(ar, (unsigned)cap * 2 * sizeof *code);
      if (!nc)
        return 0;
      memcpy(nc, code, (unsigned)n * sizeof *code);
      code = nc;
      cap *= 2;
    }
    struct tk *k = &code[n];
    k->match = -1;
    if (t.t == PO_INT || t.t == PO_REAL) {
      k->op = OP_NUM;
      k->v = pdf_num(t, 0);
    } else if (t.t == PO_BOOL) {
      k->op = t.n ? OP_TRUE : OP_FALSE;
    } else if (t.t == PO_KW && !strcmp(t.u.s, "{")) {
      if (sp >= 32)
        return 0;
      stack[sp++] = n;
      k->op = OP_BEGIN;
    } else if (t.t == PO_KW && !strcmp(t.u.s, "}")) {
      if (!sp)
        return 0;
      int b = stack[--sp];
      code[b].match = n;
      k->op = OP_END;
    } else if (t.t == PO_KW) {
      int op = OP_ABS;
      while (op < OP_COUNT && strcmp(op_names[op], t.u.s))
        op++;
      if (op == OP_COUNT)
        return 0;
      k->op = (unsigned char)op;
    } else
      return 0;
    n++;
  }
  if (!code || sp || n < 2 || code[0].op != OP_BEGIN || code[0].match != n - 1)
    return 0;
  f->code = code;
  f->ncode = n;
  return 1;
}

struct vm {
  double st[STACK];
  int sp, ok, steps;
};

static void vm_run(struct vm *m, const struct tk *code, int from, int to, int depth)
{
  struct {
    int a, b;
  } blk[16];
  int nb = 0;
  double *s = m->st;
  for (int i = from; i < to && m->ok; i++) {
    const struct tk *k = &code[i];
    if (++m->steps > 100000 || depth > 16) {
      m->ok = 0;
      return;
    }
    if (k->op == OP_BEGIN) {
      if (nb >= 16) {
        m->ok = 0;
        return;
      }
      blk[nb].a = i + 1;
      blk[nb++].b = k->match;
      i = k->match;
      continue;
    }
    if (k->op == OP_IF || k->op == OP_IFELSE) {
      int two = k->op == OP_IFELSE;
      if (nb < 1 + two || m->sp < 1) {
        m->ok = 0;
        return;
      }
      int cond = s[--m->sp] != 0;
      if (two || cond) {
        int pick = two && cond ? nb - 2 : nb - 1;
        vm_run(m, code, blk[pick].a, blk[pick].b, depth + 1);
      }
      nb -= 1 + two;
      continue;
    }
    int need = 0;
    switch (k->op) {
    case OP_NUM:
    case OP_TRUE:
    case OP_FALSE:
      if (m->sp >= STACK) {
        m->ok = 0;
        return;
      }
      s[m->sp++] = k->op == OP_NUM ? k->v : k->op == OP_TRUE;
      continue;
    case OP_POP:
    case OP_ABS:
    case OP_CEILING:
    case OP_COS:
    case OP_CVI:
    case OP_CVR:
    case OP_FLOOR:
    case OP_LN:
    case OP_LOG:
    case OP_NEG:
    case OP_NOT:
    case OP_ROUND:
    case OP_SIN:
    case OP_SQRT:
    case OP_TRUNCATE:
    case OP_DUP:
    case OP_INDEX:
    case OP_COPY:
      need = 1;
      break;
    default:
      need = 2;
    }
    if (m->sp < need) {
      m->ok = 0;
      return;
    }
    double b = s[m->sp - 1], a = need == 2 ? s[m->sp - 2] : 0, r = 0;
    switch (k->op) {
    case OP_POP:
      m->sp--;
      continue;
    case OP_DUP:
      if (m->sp >= STACK) {
        m->ok = 0;
        return;
      }
      s[m->sp++] = b;
      continue;
    case OP_EXCH:
      s[m->sp - 1] = a;
      s[m->sp - 2] = b;
      continue;
    case OP_INDEX: {
      int n = (int)b;
      if (n < 0 || n + 2 > m->sp) {
        m->ok = 0;
        return;
      }
      s[m->sp - 1] = s[m->sp - 2 - n];
      continue;
    }
    case OP_COPY: {
      int n = (int)b;
      m->sp--;
      if (n < 0 || n > m->sp || m->sp + n > STACK) {
        m->ok = 0;
        return;
      }
      memcpy(s + m->sp, s + m->sp - n, (unsigned)n * sizeof *s);
      m->sp += n;
      continue;
    }
    case OP_ROLL: {
      int j = (int)b, n = (int)a;
      m->sp -= 2;
      if (n < 0 || n > m->sp)
        m->ok = 0;
      else if (n > 1) {
        double tmp[STACK];
        j = ((j % n) + n) % n;
        for (int q = 0; q < n; q++)
          tmp[(q + j) % n] = s[m->sp - n + q];
        memcpy(s + m->sp - n, tmp, (unsigned)n * sizeof *s);
      }
      continue;
    }
    case OP_ABS:
      r = fabs(b);
      break;
    case OP_CEILING:
      r = ceil(b);
      break;
    case OP_COS:
      r = cos(b * 3.14159265358979323846 / 180);
      break;
    case OP_SIN:
      r = sin(b * 3.14159265358979323846 / 180);
      break;
    case OP_CVI:
    case OP_TRUNCATE:
      r = trunc(b);
      break;
    case OP_CVR:
      r = b;
      break;
    case OP_FLOOR:
      r = floor(b);
      break;
    case OP_LN:
      r = b > 0 ? log(b) : 0;
      break;
    case OP_LOG:
      r = b > 0 ? log10(b) : 0;
      break;
    case OP_NEG:
      r = -b;
      break;
    case OP_NOT:
      r = b == 0;
      break;
    case OP_ROUND:
      r = floor(b + 0.5);
      break;
    case OP_SQRT:
      r = b > 0 ? sqrt(b) : 0;
      break;
    case OP_ADD:
      r = a + b;
      break;
    case OP_SUB:
      r = a - b;
      break;
    case OP_MUL:
      r = a * b;
      break;
    case OP_DIV:
      r = b != 0 ? a / b : 0;
      break;
    case OP_IDIV:
      r = (int)b ? (double)((int)a / (int)b) : 0;
      break;
    case OP_MOD:
      r = (int)b ? (double)((int)a % (int)b) : 0;
      break;
    case OP_EXP:
      r = pow(a, b);
      break;
    case OP_ATAN: {
      r = atan2(a, b) * 180 / 3.14159265358979323846;
      if (r < 0)
        r += 360;
      break;
    }
    case OP_BITSHIFT:
      r = (int)b >= 0 ? (double)((int)a << ((int)b & 31)) : (double)((int)a >> ((-(int)b) & 31));
      break;
    case OP_AND:
      r = (double)((int)a & (int)b);
      break;
    case OP_OR:
      r = (double)((int)a | (int)b);
      break;
    case OP_XOR:
      r = (double)((int)a ^ (int)b);
      break;
    case OP_EQ:
      r = a == b;
      break;
    case OP_NE:
      r = a != b;
      break;
    case OP_GT:
      r = a > b;
      break;
    case OP_GE:
      r = a >= b;
      break;
    case OP_LT:
      r = a < b;
      break;
    case OP_LE:
      r = a <= b;
      break;
    default:
      m->ok = 0;
      return;
    }
    if (need == 2)
      m->sp--;
    s[m->sp - 1] = r;
  }
}

struct pfunc *pdf_func_load(struct pdf *d, struct arena *ar, struct pobj fn)
{
  static int depth;
  fn = pdf_deref(d, fn);
  if (!pdf_is_dict(fn) || depth > 6)
    return 0;
  struct pfunc *f = arena_alloc(ar, sizeof *f);
  if (!f)
    return 0;
  f->type = (int)pdf_num(pdf_get(d, fn, "FunctionType"), -1);
  struct pobj dom = pdf_get(d, fn, "Domain"), rng = pdf_get(d, fn, "Range");
  f->nin = dom.t == PO_ARR ? dom.n / 2 : 0;
  if (f->nin < 1 || f->nin > MAXIO)
    return 0;
  for (int i = 0; i < f->nin * 2; i++)
    f->domain[i] = arr_num(d, dom, i, 0);
  if (rng.t == PO_ARR) {
    f->has_range = 1;
    f->nout = rng.n / 2 > MAXIO ? MAXIO : rng.n / 2;
    for (int i = 0; i < f->nout * 2; i++)
      f->range[i] = arr_num(d, rng, i, 0);
  }
  if (f->type == 0 || f->type == 4) {
    if (!f->has_range || f->nout < 1)
      return 0;
    int len = 0;
    unsigned char *data = pdf_stream_data(d, fn, &len, 0, 0);
    if (!data)
      return 0;
    unsigned char *copy = arena_alloc(ar, (unsigned)len + 1);
    if (copy)
      memcpy(copy, data, (unsigned)len);
    free(data);
    if (!copy)
      return 0;
    if (f->type == 4)
      return parse_ps(d, ar, f, copy, len) ? f : 0;
    struct pobj sz = pdf_get(d, fn, "Size"), enc = pdf_get(d, fn, "Encode"), dec = pdf_get(d, fn, "Decode");
    f->bps = (int)pdf_num(pdf_get(d, fn, "BitsPerSample"), 8);
    long total = 1;
    for (int i = 0; i < f->nin; i++) {
      f->size[i] = (int)arr_num(d, sz, i, 0);
      if (f->size[i] < 1 || f->size[i] > 4096)
        return 0;
      total *= f->size[i];
      if (total > (1 << 24))
        return 0;
    }
    for (int i = 0; i < f->nin; i++) {
      f->encode[i * 2] = arr_num(d, enc, i * 2, 0);
      f->encode[i * 2 + 1] = arr_num(d, enc, i * 2 + 1, f->size[i] - 1);
    }
    for (int i = 0; i < f->nout * 2; i++)
      f->decode[i] = arr_num(d, dec, i, f->range[i]);
    if (f->bps < 1 || f->bps > 32 || total * f->nout * f->bps / 8 > len)
      return 0;
    f->samples = copy;
    f->nsamples = total * f->nout;
    return f;
  }
  if (f->type == 2) {
    struct pobj c0 = pdf_get(d, fn, "C0"), c1 = pdf_get(d, fn, "C1");
    f->n = pdf_num(pdf_get(d, fn, "N"), 1);
    f->nout = c0.t == PO_ARR && c0.n > 0 ? (c0.n > MAXIO ? MAXIO : c0.n) : 1;
    for (int i = 0; i < f->nout; i++) {
      f->c0[i] = c0.t == PO_ARR ? arr_num(d, c0, i, 0) : 0;
      f->c1[i] = c1.t == PO_ARR ? arr_num(d, c1, i, 1) : 1;
    }
    return f;
  }
  if (f->type == 3) {
    struct pobj fns = pdf_get(d, fn, "Functions"), bnd = pdf_get(d, fn, "Bounds"), enc = pdf_get(d, fn, "Encode");
    f->nk = fns.t == PO_ARR ? fns.n : 0;
    if (f->nk < 1 || f->nk > 256)
      return 0;
    f->k = arena_alloc(ar, (unsigned)f->nk * sizeof *f->k);
    f->bounds = arena_alloc(ar, (unsigned)f->nk * sizeof *f->bounds);
    f->kenc = arena_alloc(ar, (unsigned)f->nk * 2 * sizeof *f->kenc);
    if (!f->k || !f->bounds || !f->kenc)
      return 0;
    depth++;
    for (int i = 0; i < f->nk; i++) {
      f->k[i] = pdf_func_load(d, ar, fns.u.a[i]);
      if (i < f->nk - 1)
        f->bounds[i] = arr_num(d, bnd, i, 0);
      f->kenc[i * 2] = arr_num(d, enc, i * 2, 0);
      f->kenc[i * 2 + 1] = arr_num(d, enc, i * 2 + 1, 1);
      if (!f->k[i]) {
        depth--;
        return 0;
      }
    }
    depth--;
    f->nout = f->k[0]->nout;
    return f;
  }
  return 0;
}

static double sample_at(const struct pfunc *f, long idx)
{
  long bit = idx * f->bps;
  unsigned long long v = 0;
  for (int i = 0; i < f->bps; i += 8) {
    int take = f->bps - i < 8 ? f->bps - i : 8;
    unsigned b = f->samples[(bit + i) >> 3];
    int off = (int)((bit + i) & 7);
    b = (b >> (8 - off - take)) & ((1u << take) - 1);
    v = (v << take) | b;
  }
  double max = f->bps >= 32 ? 4294967295.0 : (double)((1ull << f->bps) - 1);
  return (double)v / max;
}

static double clampd(double v, double lo, double hi) { return lo > hi ? v : v < lo ? lo : v > hi ? hi : v; }

int pdf_func_eval(const struct pfunc *f, const double *in, int nin, double *out, int nout_max)
{
  double x[MAXIO];
  if (!f || nin < f->nin)
    return 0;
  for (int i = 0; i < f->nin; i++)
    x[i] = clampd(in[i], f->domain[i * 2], f->domain[i * 2 + 1]);
  int nout = f->nout < nout_max ? f->nout : nout_max;
  double y[MAXIO] = {0};
  if (f->type == 2) {
    double p = f->n == 1 ? x[0] : pow(x[0], f->n);
    for (int i = 0; i < nout; i++)
      y[i] = f->c0[i] + p * (f->c1[i] - f->c0[i]);
  } else if (f->type == 3) {
    int k = 0;
    while (k < f->nk - 1 && x[0] >= f->bounds[k])
      k++;
    double lo = k == 0 ? f->domain[0] : f->bounds[k - 1], hi = k == f->nk - 1 ? f->domain[1] : f->bounds[k];
    double e0 = f->kenc[k * 2], e1 = f->kenc[k * 2 + 1];
    double v = hi == lo ? e0 : e0 + (x[0] - lo) * (e1 - e0) / (hi - lo);
    return pdf_func_eval(f->k[k], &v, 1, out, nout_max);
  } else if (f->type == 4) {
    struct vm m;
    m.sp = 0;
    m.ok = 1;
    m.steps = 0;
    for (int i = 0; i < f->nin; i++)
      m.st[m.sp++] = x[i];
    vm_run(&m, f->code, 1, f->code[0].match, 0);
    if (!m.ok || m.sp < nout)
      return 0;
    for (int i = 0; i < nout; i++)
      y[i] = m.st[m.sp - nout + i];
  } else if (f->type == 0) {
    double e[MAXIO];
    int i0[MAXIO];
    double fr[MAXIO];
    for (int i = 0; i < f->nin; i++) {
      double d0 = f->domain[i * 2], d1 = f->domain[i * 2 + 1];
      e[i] = d1 == d0 ? f->encode[i * 2]
                      : f->encode[i * 2] + (x[i] - d0) * (f->encode[i * 2 + 1] - f->encode[i * 2]) / (d1 - d0);
      e[i] = clampd(e[i], 0, f->size[i] - 1);
      i0[i] = (int)e[i];
      if (i0[i] >= f->size[i] - 1 && f->size[i] > 1)
        i0[i] = f->size[i] - 2;
      fr[i] = f->size[i] > 1 ? e[i] - i0[i] : 0;
    }
    for (int j = 0; j < nout; j++) {
      double acc = 0;
      int corners = 1 << f->nin;
      for (int c = 0; c < corners; c++) {
        double w = 1;
        long idx = 0, stride = 1;
        for (int i = 0; i < f->nin; i++) {
          int hi = (c >> i) & 1;
          w *= hi ? fr[i] : 1 - fr[i];
          int pos = i0[i] + (hi && f->size[i] > 1 ? 1 : 0);
          idx += pos * stride;
          stride *= f->size[i];
        }
        if (w != 0)
          acc += w * sample_at(f, idx * f->nout + j);
      }
      y[j] = f->decode[j * 2] + acc * (f->decode[j * 2 + 1] - f->decode[j * 2]);
    }
  } else
    return 0;
  for (int i = 0; i < nout; i++)
    out[i] = f->has_range ? clampd(y[i], f->range[i * 2], f->range[i * 2 + 1]) : y[i];
  return nout;
}
