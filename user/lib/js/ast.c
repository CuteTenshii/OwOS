/* ast.c — arena allocation, node construction, and an s-expression dumper. */
#include "ast.h"
#include "fmt.h"   /* dtoa for number literals in the dump */
#include "lexer.h" /* js_tok_spell for operator names in the dump */
#include "owos.h"  /* malloc/free, mem*, str*, dtoa via fmt */

/* ---------------------------------------------------------------- arena ----
 */

#define ARENA_BLOCK (64u * 1024u)

struct arena_block {
  arena_block *next;
  unsigned used, cap;
  char data[1]; /* [cap]; flexible tail allocated with the block */
};

void js_arena_init(js_arena *a) { a->head = NULL; }

void js_arena_free(js_arena *a)
{
  arena_block *b = a->head;
  while (b) {
    arena_block *n = b->next;
    free(b);
    b = n;
  }
  a->head = NULL;
}

static arena_block *block_new(unsigned cap)
{
  arena_block *b = (arena_block *)malloc(sizeof(arena_block) + cap);
  if (!b)
    return NULL;
  b->next = NULL;
  b->used = 0;
  b->cap = cap;
  return b;
}

void *js_arena_alloc(js_arena *a, unsigned n)
{
  arena_block *b = a->head;
  n = (n + 7u) & ~7u;
  if (!b || b->used + n > b->cap) {
    unsigned cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;
    arena_block *nb = block_new(cap);
    if (!nb)
      return NULL;
    nb->next = a->head;
    a->head = nb;
    b = nb;
  }
  {
    void *p = b->data + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
  }
}

char *js_arena_strndup(js_arena *a, const char *s, int n)
{
  char *d = (char *)js_arena_alloc(a, (unsigned)n + 1);
  if (!d)
    return NULL;
  if (n > 0)
    memcpy(d, s, (unsigned)n);
  d[n] = 0;
  return d;
}

js_node *js_node_new(js_arena *a, int type, int line, int col)
{
  js_node *n = (js_node *)js_arena_alloc(a, sizeof(js_node));
  if (!n)
    return NULL;
  n->type = type;
  n->op = 0;
  n->line = line;
  n->col = col;
  return n;
}

/* ----------------------------------------------------------------- dump ----
 */

typedef struct {
  char *buf;
  unsigned len, cap;
} sbuf;

static void sb_ensure(sbuf *s, unsigned extra)
{
  if (s->len + extra + 1 <= s->cap)
    return;
  {
    unsigned cap = s->cap ? s->cap * 2 : 256;
    while (cap < s->len + extra + 1)
      cap *= 2;
    s->buf = (char *)realloc(s->buf, cap);
    s->cap = cap;
  }
}

static void sb_puts(sbuf *s, const char *t)
{
  int n = strlen(t);
  sb_ensure(s, (unsigned)n);
  memcpy(s->buf + s->len, t, (unsigned)n);
  s->len += (unsigned)n;
  s->buf[s->len] = 0;
}

static void sb_putc(sbuf *s, char c)
{
  sb_ensure(s, 1);
  s->buf[s->len++] = c;
  s->buf[s->len] = 0;
}

/* A string literal, escaped enough to keep the s-expression readable. */
static void sb_quote(sbuf *s, const char *t, int n)
{
  int i;
  sb_putc(s, '"');
  for (i = 0; i < n; i++) {
    char c = t[i];
    if (c == '"' || c == '\\') {
      sb_putc(s, '\\');
      sb_putc(s, c);
    } else if (c == '\n')
      sb_puts(s, "\\n");
    else if (c == '\t')
      sb_puts(s, "\\t");
    else
      sb_putc(s, c);
  }
  sb_putc(s, '"');
}

static void dump(sbuf *s, js_node *n);

static void dump_list(sbuf *s, js_node **items, int count)
{
  int i;
  for (i = 0; i < count; i++) {
    sb_putc(s, ' ');
    if (items[i])
      dump(s, items[i]);
    else
      sb_puts(s, "hole");
  }
}

static const char *decl_kw(int kind) { return kind == JS_DECL_LET ? "let" : kind == JS_DECL_CONST ? "const" : "var"; }

static void dump_prop(sbuf *s, js_prop *p)
{
  sb_puts(s, " (");
  if (p->kind == JS_PROP_SPREAD) {
    sb_puts(s, "spread ");
    dump(s, p->value);
    sb_putc(s, ')');
    return;
  }
  if (p->kind == JS_PROP_GET)
    sb_puts(s, "get ");
  else if (p->kind == JS_PROP_SET)
    sb_puts(s, "set ");
  else if (p->kind == JS_PROP_METHOD)
    sb_puts(s, "method ");
  else
    sb_puts(s, "prop ");
  if (p->computed) {
    sb_putc(s, '[');
    dump(s, p->key);
    sb_putc(s, ']');
  } else
    dump(s, p->key);
  sb_putc(s, ' ');
  dump(s, p->value);
  sb_putc(s, ')');
}

static void dump(sbuf *s, js_node *n)
{
  char tmp[DTOA_MAX];
  sb_putc(s, '(');
  switch (n->type) {
  case JS_NUM:
    dtoa(n->u.num, tmp);
    sb_puts(s, "num ");
    sb_puts(s, tmp);
    break;
  case JS_BIGINT:
    sb_puts(s, "bigint ");
    sb_puts(s, n->u.str.s);
    sb_putc(s, 'n');
    break;
  case JS_STR:
    sb_puts(s, "str ");
    sb_quote(s, n->u.str.s, n->u.str.len);
    break;
  case JS_REGEX:
    sb_puts(s, "regex /");
    sb_puts(s, n->u.regex.pattern);
    sb_putc(s, '/');
    sb_puts(s, n->u.regex.flags);
    break;
  case JS_BOOL:
    sb_puts(s, n->u.num ? "true" : "false");
    break;
  case JS_NULL:
    sb_puts(s, "null");
    break;
  case JS_THIS:
    sb_puts(s, "this");
    break;
  case JS_SUPER:
    sb_puts(s, "super");
    break;
  case JS_IDENT:
    sb_puts(s, "id ");
    sb_ensure(s, (unsigned)n->u.str.len);
    memcpy(s->buf + s->len, n->u.str.s, (unsigned)n->u.str.len);
    s->len += (unsigned)n->u.str.len;
    s->buf[s->len] = 0;
    break;

  case JS_TEMPLATE: {
    int i;
    sb_puts(s, "template");
    for (i = 0; i < n->u.tmpl.nquasis; i++) {
      sb_putc(s, ' ');
      sb_quote(s, n->u.tmpl.quasis[i]->u.str.s, n->u.tmpl.quasis[i]->u.str.len);
      if (i < n->u.tmpl.nexprs) {
        sb_putc(s, ' ');
        dump(s, n->u.tmpl.exprs[i]);
      }
    }
    break;
  }
  case JS_TAGGED_TEMPLATE:
    sb_puts(s, "tagged ");
    dump(s, n->u.bin.left);
    sb_putc(s, ' ');
    dump(s, n->u.bin.right);
    break;

  case JS_ARRAY:
    sb_puts(s, "array");
    dump_list(s, n->u.list.items, n->u.list.count);
    break;
  case JS_SEQUENCE:
    sb_puts(s, "seq");
    dump_list(s, n->u.list.items, n->u.list.count);
    break;
  case JS_OBJECT: {
    int i;
    sb_puts(s, "object");
    for (i = 0; i < n->u.object.count; i++)
      dump_prop(s, &n->u.object.props[i]);
    break;
  }

  case JS_UNARY:
    sb_puts(s, "unary ");
    sb_puts(s, js_tok_spell(n->op));
    sb_putc(s, ' ');
    dump(s, n->u.unary.operand);
    break;
  case JS_UPDATE:
    sb_puts(s, n->u.unary.prefix ? "preupdate " : "postupdate ");
    sb_puts(s, js_tok_spell(n->op));
    sb_putc(s, ' ');
    dump(s, n->u.unary.operand);
    break;
  case JS_BINARY:
  case JS_LOGICAL:
    sb_puts(s, n->type == JS_LOGICAL ? "logical " : "binary ");
    sb_puts(s, js_tok_spell(n->op));
    sb_putc(s, ' ');
    dump(s, n->u.bin.left);
    sb_putc(s, ' ');
    dump(s, n->u.bin.right);
    break;
  case JS_ASSIGN:
    sb_puts(s, "assign ");
    sb_puts(s, js_tok_spell(n->op));
    sb_putc(s, ' ');
    dump(s, n->u.bin.left);
    sb_putc(s, ' ');
    dump(s, n->u.bin.right);
    break;
  case JS_COND:
    sb_puts(s, "cond ");
    dump(s, n->u.iff.test);
    sb_putc(s, ' ');
    dump(s, n->u.iff.cons);
    sb_putc(s, ' ');
    dump(s, n->u.iff.alt);
    break;

  case JS_MEMBER:
    sb_puts(s, n->u.member.optional ? "member? " : "member ");
    dump(s, n->u.member.obj);
    sb_putc(s, ' ');
    if (n->u.member.computed) {
      sb_putc(s, '[');
      dump(s, n->u.member.prop);
      sb_putc(s, ']');
    } else
      dump(s, n->u.member.prop);
    break;
  case JS_CALL:
    sb_puts(s, n->u.call.optional ? "call? " : "call ");
    dump(s, n->u.call.callee);
    dump_list(s, n->u.call.args, n->u.call.nargs);
    break;
  case JS_NEW:
    sb_puts(s, "new ");
    dump(s, n->u.call.callee);
    dump_list(s, n->u.call.args, n->u.call.nargs);
    break;
  case JS_SPREAD:
    sb_puts(s, "spread ");
    dump(s, n->u.one.arg);
    break;
  case JS_AWAIT:
    sb_puts(s, "await ");
    dump(s, n->u.one.arg);
    break;
  case JS_YIELD:
    sb_puts(s, n->op ? "yield*" : "yield");
    if (n->u.one.arg) {
      sb_putc(s, ' ');
      dump(s, n->u.one.arg);
    }
    break;

  case JS_FUNC: {
    int i;
    sb_puts(s, n->u.func.is_arrow ? "arrow" : "function");
    if (n->u.func.name) {
      sb_putc(s, ' ');
      sb_puts(s, n->u.func.name);
    }
    sb_puts(s, " (params");
    for (i = 0; i < n->u.func.nparams; i++) {
      sb_putc(s, ' ');
      dump(s, n->u.func.params[i]);
    }
    sb_puts(s, ") ");
    dump(s, n->u.func.body);
    break;
  }
  case JS_CLASS: {
    int i;
    sb_puts(s, "class");
    if (n->u.cls.id) {
      sb_putc(s, ' ');
      dump(s, n->u.cls.id);
    }
    if (n->u.cls.super) {
      sb_puts(s, " extends ");
      dump(s, n->u.cls.super);
    }
    for (i = 0; i < n->u.cls.nmembers; i++) {
      js_member *m = &n->u.cls.members[i];
      sb_puts(s, " (");
      if (m->is_static)
        sb_puts(s, "static ");
      sb_puts(s, m->kind == JS_M_GET     ? "get"
                 : m->kind == JS_M_SET   ? "set"
                 : m->kind == JS_M_FIELD ? "field"
                 : m->kind == JS_M_CTOR  ? "ctor"
                                         : "method");
      sb_putc(s, ' ');
      if (m->computed) {
        sb_putc(s, '[');
        dump(s, m->key);
        sb_putc(s, ']');
      } else
        dump(s, m->key);
      if (m->value) {
        sb_putc(s, ' ');
        dump(s, m->value);
      }
      sb_putc(s, ')');
    }
    break;
  }

  case JS_ARRAY_PATTERN:
    sb_puts(s, "arraypat");
    dump_list(s, n->u.list.items, n->u.list.count);
    break;
  case JS_OBJECT_PATTERN: {
    int i;
    sb_puts(s, "objectpat");
    for (i = 0; i < n->u.object.count; i++)
      dump_prop(s, &n->u.object.props[i]);
    break;
  }
  case JS_ASSIGN_PATTERN:
    sb_puts(s, "default ");
    dump(s, n->u.assignpat.id);
    sb_putc(s, ' ');
    dump(s, n->u.assignpat.init);
    break;
  case JS_REST:
    sb_puts(s, "rest ");
    dump(s, n->u.one.arg);
    break;

  case JS_PROGRAM:
    sb_puts(s, "program");
    dump_list(s, n->u.list.items, n->u.list.count);
    break;
  case JS_BLOCK:
    sb_puts(s, "block");
    dump_list(s, n->u.list.items, n->u.list.count);
    break;
  case JS_VAR_DECL: {
    int i;
    sb_puts(s, decl_kw(n->u.var.kind));
    for (i = 0; i < n->u.var.count; i++) {
      sb_puts(s, " (");
      dump(s, n->u.var.decls[i].id);
      if (n->u.var.decls[i].init) {
        sb_putc(s, ' ');
        dump(s, n->u.var.decls[i].init);
      }
      sb_putc(s, ')');
    }
    break;
  }
  case JS_EXPR_STMT:
    sb_puts(s, "expr ");
    dump(s, n->u.one.arg);
    break;
  case JS_EMPTY:
    sb_puts(s, "empty");
    break;
  case JS_IF:
    sb_puts(s, "if ");
    dump(s, n->u.iff.test);
    sb_putc(s, ' ');
    dump(s, n->u.iff.cons);
    if (n->u.iff.alt) {
      sb_putc(s, ' ');
      dump(s, n->u.iff.alt);
    }
    break;
  case JS_FOR:
    sb_puts(s, "for ");
    if (n->u.forc.init)
      dump(s, n->u.forc.init);
    else
      sb_puts(s, "()");
    sb_putc(s, ' ');
    if (n->u.forc.test)
      dump(s, n->u.forc.test);
    else
      sb_puts(s, "()");
    sb_putc(s, ' ');
    if (n->u.forc.update)
      dump(s, n->u.forc.update);
    else
      sb_puts(s, "()");
    sb_putc(s, ' ');
    dump(s, n->u.forc.body);
    break;
  case JS_FOR_IN:
    sb_puts(s, n->u.forin.of ? "forof " : "forin ");
    dump(s, n->u.forin.left);
    sb_putc(s, ' ');
    dump(s, n->u.forin.right);
    sb_putc(s, ' ');
    dump(s, n->u.forin.body);
    break;
  case JS_WHILE:
    sb_puts(s, "while ");
    dump(s, n->u.loop.test);
    sb_putc(s, ' ');
    dump(s, n->u.loop.body);
    break;
  case JS_DO_WHILE:
    sb_puts(s, "dowhile ");
    dump(s, n->u.loop.body);
    sb_putc(s, ' ');
    dump(s, n->u.loop.test);
    break;
  case JS_RETURN:
    sb_puts(s, "return");
    if (n->u.one.arg) {
      sb_putc(s, ' ');
      dump(s, n->u.one.arg);
    }
    break;
  case JS_THROW:
    sb_puts(s, "throw ");
    dump(s, n->u.one.arg);
    break;
  case JS_BREAK:
    sb_puts(s, "break");
    if (n->u.labeled.label) {
      sb_putc(s, ' ');
      sb_puts(s, n->u.labeled.label);
    }
    break;
  case JS_CONTINUE:
    sb_puts(s, "continue");
    if (n->u.labeled.label) {
      sb_putc(s, ' ');
      sb_puts(s, n->u.labeled.label);
    }
    break;
  case JS_TRY:
    sb_puts(s, "try ");
    dump(s, n->u.tc.block);
    if (n->u.tc.handler) {
      sb_puts(s, " catch ");
      if (n->u.tc.param) {
        dump(s, n->u.tc.param);
        sb_putc(s, ' ');
      }
      dump(s, n->u.tc.handler);
    }
    if (n->u.tc.finalizer) {
      sb_puts(s, " finally ");
      dump(s, n->u.tc.finalizer);
    }
    break;
  case JS_SWITCH: {
    int i, j;
    sb_puts(s, "switch ");
    dump(s, n->u.sw.disc);
    for (i = 0; i < n->u.sw.count; i++) {
      js_case *c = &n->u.sw.cases[i];
      sb_puts(s, c->test ? " (case " : " (default");
      if (c->test) {
        dump(s, c->test);
      }
      for (j = 0; j < c->nbody; j++) {
        sb_putc(s, ' ');
        dump(s, c->body[j]);
      }
      sb_putc(s, ')');
    }
    break;
  }
  case JS_LABELED:
    sb_puts(s, "label ");
    sb_puts(s, n->u.labeled.label);
    sb_putc(s, ' ');
    dump(s, n->u.labeled.body);
    break;
  case JS_DEBUGGER:
    sb_puts(s, "debugger");
    break;
  default:
    sb_puts(s, "?");
    break;
  }
  sb_putc(s, ')');
}

char *js_ast_dump(js_node *n)
{
  sbuf s;
  s.buf = NULL;
  s.len = 0;
  s.cap = 0;
  if (n)
    dump(&s, n);
  if (!s.buf) {
    s.buf = (char *)malloc(1);
    if (s.buf)
      s.buf[0] = 0;
  }
  return s.buf;
}
