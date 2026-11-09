/* parser.c — JavaScript recursive-descent + precedence-climbing parser.
 *
 * Errors are reported by setting p->failed and unwinding with NULL returns
 * (no setjmp in this freestanding environment). Every helper that can fail
 * returns NULL on failure and the callers stop building once p->failed is set.
 *
 * Arrow functions and destructuring assignment use the standard cover-grammar
 * trick: a parenthesized list / array / object literal is parsed as an
 * expression first and reinterpreted into binding patterns when a `=>` or `=`
 * reveals it was actually a parameter list or assignment target.
 */
#include "parser.h"
#include "lexer.h"
#include "owos.h"
#include "value.h"

typedef struct {
  jlexer lx;
  jtoken cur;
  jtoken ahead, ahead2;
  int n_ahead; /* buffered lookahead tokens: 0, 1, or 2          */
  js_arena *arena;
  int failed;
  const char *errmsg;
  int errline;
  int errcol;
  int in_async;          /* inside an async function body (await is a kw)  */
  int in_generator;      /* inside a generator body (yield is a kw)        */
  int pending_async;     /* the function/arrow about to be parsed is async */
  int pending_generator; /* the method about to be parsed is a generator   */
  int depth;             /* live recursion depth, bounded to spare the stack */
  int sdepth;            /* depth of the unary/binary/new operand chains */
} js_parser;

/* The recursive-descent core costs one frame-ladder per source nesting level
 * (~900 B at -O0). Cap nesting so a pathological/minified-to-death input fails
 * with a clean SyntaxError instead of overflowing the user stack and faulting.
 * Kept comfortably under USER_STACK_PAGES worth of frames (see
 * kernel/loader.h). */
#define JS_MAX_DEPTH 700

/* Lowest native SP the parser may recurse to (js_set_stack_limit); NULL = depth cap only. */
void js_parser_set_stack_floor(const void *low) { gc_heap_slots()[JS_SLOT_PARSE_FLOOR] = (void *)low; }
static int below_floor(const char *sp)
{
  const char *floor = gc_heap_slots()[JS_SLOT_PARSE_FLOOR];
  return floor && sp < floor;
}

/* ------------------------------------------------------- token stream ----- */

static void advance(js_parser *p)
{
  if (p->n_ahead > 0) {
    p->cur = p->ahead;
    p->ahead = p->ahead2;
    p->n_ahead--;
  } else
    js_lex_next(&p->lx, &p->cur);
  if (p->cur.kind == T_ERROR && !p->failed) {
    p->failed = 1;
    p->errmsg = p->lx.err ? p->lx.err : "lexical error";
    p->errline = p->cur.line;
    p->errcol = p->cur.col;
  }
}

static jtoken *peek(js_parser *p)
{
  if (p->n_ahead < 1) {
    js_lex_next(&p->lx, &p->ahead);
    p->n_ahead = 1;
  }
  return &p->ahead;
}
static jtoken *peek2(js_parser *p)
{
  if (p->n_ahead < 1) {
    js_lex_next(&p->lx, &p->ahead);
    p->n_ahead = 1;
  }
  if (p->n_ahead < 2) {
    js_lex_next(&p->lx, &p->ahead2);
    p->n_ahead = 2;
  }
  return &p->ahead2;
}

static void *perr(js_parser *p, const char *msg)
{
  if (!p->failed) {
    p->failed = 1;
    p->errmsg = msg;
    p->errline = p->cur.line;
    p->errcol = p->cur.col;
  }
  return NULL;
}

static int at(js_parser *p, int kind) { return p->cur.kind == kind; }
static int eat(js_parser *p, int kind)
{
  if (p->cur.kind == kind) {
    advance(p);
    return 1;
  }
  return 0;
}
static int expect(js_parser *p, int kind, const char *msg)
{
  if (p->cur.kind == kind) {
    advance(p);
    return 1;
  }
  perr(p, msg);
  return 0;
}

/* contextual-keyword tests (these tokens lex as T_IDENT) */
static int word_is(jtoken *t, const char *w, int n)
{
  int i;
  if (t->kind != T_IDENT || t->value_len != n)
    return 0;
  for (i = 0; i < n; i++)
    if (t->value[i] != w[i])
      return 0;
  return 1;
}
static int is_let(js_parser *p) { return word_is(&p->cur, "let", 3); }
static int is_of(js_parser *p) { return word_is(&p->cur, "of", 2); }

static int node_word_is(js_node *n, const char *w)
{
  int i, len = 0;
  while (w[len])
    len++;
  if (n->type != JS_IDENT || n->u.str.len != len)
    return 0;
  for (i = 0; i < len; i++)
    if (n->u.str.s[i] != w[i])
      return 0;
  return 1;
}

/* ------------------------------------------------------- node helpers ----- */

static js_node *mk(js_parser *p, int type) { return js_node_new(p->arena, type, p->cur.line, p->cur.col); }

static int grow_array(void **ptr, int *cap, int need, unsigned item_size)
{
  if (need <= *cap)
    return 1;
  int next = *cap ? *cap * 2 : 8;
  while (next < need)
    next *= 2;
  void *p = realloc(*ptr, (unsigned)next * item_size);
  if (!p)
    return 0;
  *ptr = p;
  *cap = next;
  return 1;
}

static js_node *ident_from(js_parser *p, const char *s, int len, int line, int col)
{
  js_node *n = js_node_new(p->arena, JS_IDENT, line, col);
  n->u.str.s = s;
  n->u.str.len = len;
  n->u.str.hash = js_str_hash(s, len);
  return n;
}

/* consume the current identifier-like token as an identifier node */
static js_node *eat_ident(js_parser *p)
{
  js_node *n = ident_from(p, p->cur.value, p->cur.value_len, p->cur.line, p->cur.col);
  advance(p);
  return n;
}

/* growable js_node* vector (temporary; frozen into the arena) */
typedef struct {
  js_node **a;
  int n, cap;
  int failed;
} nvec;
static void nv_push(nvec *v, js_node *x)
{
  if (v->failed || !grow_array((void **)&v->a, &v->cap, v->n + 1, sizeof *v->a)) {
    v->failed = 1;
    return;
  }
  v->a[v->n++] = x;
}
static js_node **nv_freeze(js_parser *p, nvec *v, int *count)
{
  js_node **d = NULL;
  if (v->failed) {
    p->failed = 1;
    if (!p->errmsg)
      p->errmsg = "out of memory";
    if (v->a)
      free(v->a);
    *count = 0;
    return NULL;
  }
  *count = v->n;
  if (v->n) {
    d = (js_node **)js_arena_alloc(p->arena, (unsigned)v->n * sizeof(js_node *));
    memcpy(d, v->a, (unsigned)v->n * sizeof(js_node *));
  }
  if (v->a)
    free(v->a);
  return d;
}

/* ------------------------------------------------------- forward decls ---- */

static js_node *parse_statement(js_parser *p);
static js_node *parse_block(js_parser *p);
static js_node *parse_expression(js_parser *p, int no_in);
static js_node *parse_assignment(js_parser *p, int no_in);
static js_node *parse_binding_target(js_parser *p);
static js_node *parse_function(js_parser *p, int is_decl);
static js_node *parse_class(js_parser *p, int is_decl);
static js_node *parse_template(js_parser *p);
static js_node *reinterpret_pattern(js_parser *p, js_node *n);

/* ------------------------------------------------------- patterns --------- */

static js_node *parse_array_pattern(js_parser *p)
{
  js_node *node;
  nvec els = {0};
  els.a = NULL;
  els.n = 0;
  els.cap = 0;
  advance(p); /* '[' */
  while (!at(p, T_RBRACK) && !p->failed) {
    if (at(p, T_COMMA)) {
      nv_push(&els, NULL);
      advance(p);
      continue;
    } /* hole */
    if (at(p, T_ELLIPSIS)) {
      js_node *r = mk(p, JS_REST);
      advance(p);
      r->u.one.arg = parse_binding_target(p);
      nv_push(&els, r);
      break; /* rest is the last element */
    }
    {
      js_node *t = parse_binding_target(p);
      if (eat(p, T_EQ)) {
        js_node *d = mk(p, JS_ASSIGN_PATTERN);
        d->u.assignpat.id = t;
        d->u.assignpat.init = parse_assignment(p, 0);
        t = d;
      }
      nv_push(&els, t);
    }
    if (!eat(p, T_COMMA))
      break;
  }
  expect(p, T_RBRACK, "expected ']' in array pattern");
  node = mk(p, JS_ARRAY_PATTERN);
  node->u.list.items = nv_freeze(p, &els, &node->u.list.count);
  return p->failed ? NULL : node;
}

/* a property key; sets *computed if it was [expr] */
static js_node *parse_property_key(js_parser *p, int *computed)
{
  *computed = 0;
  if (at(p, T_LBRACK)) {
    js_node *k;
    advance(p);
    k = parse_assignment(p, 0);
    expect(p, T_RBRACK, "expected ']' after computed key");
    *computed = 1;
    return k;
  }
  if (at(p, T_STR)) {
    js_node *k = mk(p, JS_STR);
    k->u.str.s = p->cur.value;
    k->u.str.len = p->cur.value_len;
    advance(p);
    return k;
  }
  if (at(p, T_NUM) || at(p, T_BIGINT)) {
    js_node *k = mk(p, JS_NUM);
    k->u.num = p->cur.num;
    advance(p);
    return k;
  }
  if (p->cur.value)
    return eat_ident(p); /* identifier name or keyword used as name */
  return perr(p, "expected property name");
}

static js_node *parse_object_pattern(js_parser *p)
{
  js_node *node;
  js_prop *props = NULL;
  int n = 0, cap = 0;
  advance(p); /* '{' */
  while (!at(p, T_RBRACE) && !p->failed) {
    js_prop pr;
    pr.kind = JS_PROP_INIT;
    pr.computed = 0;
    pr.shorthand = 0;
    pr.key = NULL;
    pr.value = NULL;
    if (at(p, T_ELLIPSIS)) {
      advance(p);
      pr.kind = JS_PROP_SPREAD;
      pr.value = parse_binding_target(p);
      if (!grow_array((void **)&props, &cap, n + 1, sizeof *props)) {
        perr(p, "out of memory");
        break;
      }
      props[n++] = pr;
      break;
    }
    {
      int comp = 0;
      pr.key = parse_property_key(p, &comp);
      pr.computed = (unsigned char)comp;
    }
    if (eat(p, T_COLON)) {
      pr.value = parse_binding_target(p);
    } else {
      pr.shorthand = 1;
      pr.value = pr.key; /* {x} -> bind x */
    }
    if (eat(p, T_EQ)) {
      js_node *d = mk(p, JS_ASSIGN_PATTERN);
      d->u.assignpat.id = pr.value;
      d->u.assignpat.init = parse_assignment(p, 0);
      pr.value = d;
    }
    if (!grow_array((void **)&props, &cap, n + 1, sizeof *props)) {
      perr(p, "out of memory");
      break;
    }
    props[n++] = pr;
    if (!eat(p, T_COMMA))
      break;
  }
  expect(p, T_RBRACE, "expected '}' in object pattern");
  node = mk(p, JS_OBJECT_PATTERN);
  if (p->failed) {
    if (props)
      free(props);
    return NULL;
  }
  node->u.object.count = n;
  if (n) {
    node->u.object.props = (js_prop *)js_arena_alloc(p->arena, (unsigned)n * sizeof(js_prop));
    memcpy(node->u.object.props, props, (unsigned)n * sizeof(js_prop));
  }
  if (props)
    free(props);
  return node;
}

static js_node *parse_binding_target(js_parser *p)
{
  if (at(p, T_LBRACK))
    return parse_array_pattern(p);
  if (at(p, T_LBRACE))
    return parse_object_pattern(p);
  if (at(p, T_IDENT))
    return eat_ident(p);
  return perr(p, "expected a binding name or pattern");
}

/* ------------------------------------------------------- expressions ------ */

static int binop_prec(int op, int no_in)
{
  switch (op) {
  case T_QQ:
    return 1;
  case T_BARBAR:
    return 2;
  case T_AMPAMP:
    return 3;
  case T_BAR:
    return 4;
  case T_CARET:
    return 5;
  case T_AMP:
    return 6;
  case T_EQEQ:
  case T_NE:
  case T_EQEQEQ:
  case T_NEEQ:
    return 7;
  case T_LT:
  case T_GT:
  case T_LE:
  case T_GE:
  case T_INSTANCEOF:
    return 8;
  case T_IN:
    return no_in ? 0 : 8;
  case T_LTLT:
  case T_GTGT:
  case T_GTGTGT:
    return 9;
  case T_PLUS:
  case T_MINUS:
    return 10;
  case T_STAR:
  case T_SLASH:
  case T_PERCENT:
    return 11;
  case T_STARSTAR:
    return 12;
  default:
    return 0;
  }
}

static js_node *parse_arguments(js_parser *p, js_node ***out, int *count)
{
  nvec args = {0};
  args.a = NULL;
  args.n = 0;
  args.cap = 0;
  advance(p); /* '(' */
  while (!at(p, T_RPAREN) && !p->failed) {
    if (at(p, T_ELLIPSIS)) {
      js_node *s = mk(p, JS_SPREAD);
      advance(p);
      s->u.one.arg = parse_assignment(p, 0);
      nv_push(&args, s);
    } else {
      nv_push(&args, parse_assignment(p, 0));
    }
    if (!eat(p, T_COMMA))
      break;
  }
  expect(p, T_RPAREN, "expected ')' after arguments");
  *out = nv_freeze(p, &args, count);
  return NULL;
}

static js_node *parse_primary(js_parser *p);

/* member / call / optional-chaining / tagged-template tail */
static js_node *parse_tail(js_parser *p, js_node *e, int allow_call)
{
  for (;;) {
    if (p->failed)
      return NULL;
    if (at(p, T_DOT)) {
      js_node *m = mk(p, JS_MEMBER);
      advance(p);
      if (!p->cur.value)
        return perr(p, "expected property name after '.'");
      m->u.member.obj = e;
      m->u.member.prop = eat_ident(p);
      m->u.member.computed = 0;
      e = m;
    } else if (at(p, T_QDOT)) {
      js_node *m = mk(p, JS_MEMBER);
      advance(p);
      m->u.member.obj = e;
      m->u.member.optional = 1;
      if (at(p, T_LPAREN) && allow_call) {
        js_node *c = mk(p, JS_CALL);
        c->u.call.callee = e;
        c->u.call.optional = 1;
        parse_arguments(p, &c->u.call.args, &c->u.call.nargs);
        e = c;
        continue;
      }
      if (at(p, T_LBRACK)) {
        advance(p);
        m->u.member.prop = parse_expression(p, 0);
        expect(p, T_RBRACK, "expected ']'");
        m->u.member.computed = 1;
      } else {
        if (!p->cur.value)
          return perr(p, "expected property name after '?.'");
        m->u.member.prop = eat_ident(p);
        m->u.member.computed = 0;
      }
      e = m;
    } else if (at(p, T_LBRACK)) {
      js_node *m = mk(p, JS_MEMBER);
      advance(p);
      m->u.member.obj = e;
      m->u.member.prop = parse_expression(p, 0);
      m->u.member.computed = 1;
      expect(p, T_RBRACK, "expected ']'");
      e = m;
    } else if (at(p, T_LPAREN) && allow_call) {
      js_node *c = mk(p, JS_CALL);
      c->u.call.callee = e;
      parse_arguments(p, &c->u.call.args, &c->u.call.nargs);
      e = c;
    } else if ((at(p, T_TEMPLATE) || at(p, T_TEMPLATE_HEAD)) && allow_call) {
      js_node *t = mk(p, JS_TAGGED_TEMPLATE);
      t->u.bin.left = e;
      t->u.bin.right = parse_template(p);
      e = t;
    } else {
      return e;
    }
  }
}

static js_node *parse_new_inner(js_parser *p);
static js_node *parse_new(js_parser *p)
{
  js_node *r;
  char probe;
  if (++p->sdepth > 4 * JS_MAX_DEPTH || below_floor(&probe)) {
    --p->sdepth;
    return perr(p, "expression nesting too deep");
  }
  r = parse_new_inner(p);
  --p->sdepth;
  return r;
}
static js_node *parse_new_inner(js_parser *p)
{
  js_node *node, *callee;
  int line = p->cur.line, col = p->cur.col;
  advance(p);         /* 'new' */
  if (at(p, T_DOT)) { /* new.target */
    advance(p);
    if (!word_is(&p->cur, "target", 6))
      return perr(p, "expected 'target' after 'new.'");
    advance(p);
    return ident_from(p, "new.target", 10, line, col);
  }
  if (at(p, T_NEW))
    callee = parse_new(p);
  else
    callee = parse_tail(p, parse_primary(p), 0); /* member access, but not a call */
  if (p->failed)
    return NULL;
  node = js_node_new(p->arena, JS_NEW, line, col);
  node->u.call.callee = callee;
  if (at(p, T_LPAREN))
    parse_arguments(p, &node->u.call.args, &node->u.call.nargs);
  return node;
}

static js_node *parse_lhs(js_parser *p)
{
  js_node *e;
  if (at(p, T_NEW))
    e = parse_new(p);
  else
    e = parse_primary(p);
  if (p->failed)
    return NULL;
  return parse_tail(p, e, 1);
}

static js_node *parse_unary_inner(js_parser *p);
static js_node *parse_unary(js_parser *p)
{
  js_node *r;
  char probe;
  if (++p->sdepth > 4 * JS_MAX_DEPTH || below_floor(&probe)) {
    --p->sdepth;
    return perr(p, "expression nesting too deep");
  }
  r = parse_unary_inner(p);
  --p->sdepth;
  return r;
}
static js_node *parse_unary_inner(js_parser *p)
{
  int k = p->cur.kind;
  if (p->in_async && at(p, T_IDENT) && word_is(&p->cur, "await", 5)) {
    js_node *n = mk(p, JS_AWAIT);
    advance(p);
    n->u.one.arg = parse_unary(p);
    return p->failed ? NULL : n;
  }
  if (k == T_BANG || k == T_TILDE || k == T_PLUS || k == T_MINUS || k == T_TYPEOF || k == T_VOID || k == T_DELETE) {
    js_node *n = mk(p, JS_UNARY);
    n->op = k;
    advance(p);
    n->u.unary.operand = parse_unary(p);
    n->u.unary.prefix = 1;
    return n;
  }
  if (k == T_PLUSPLUS || k == T_MINUSMINUS) {
    js_node *n = mk(p, JS_UPDATE);
    n->op = k;
    advance(p);
    n->u.unary.operand = parse_unary(p);
    n->u.unary.prefix = 1;
    return n;
  }
  {
    js_node *e = parse_lhs(p);
    if (p->failed)
      return NULL;
    if ((at(p, T_PLUSPLUS) || at(p, T_MINUSMINUS)) && !p->cur.newline_before) {
      js_node *n = mk(p, JS_UPDATE);
      n->op = p->cur.kind;
      n->u.unary.operand = e;
      n->u.unary.prefix = 0;
      advance(p);
      return n;
    }
    return e;
  }
}

static js_node *parse_binary_inner(js_parser *p, int min_prec, int no_in);
static js_node *parse_binary(js_parser *p, int min_prec, int no_in)
{
  js_node *r;
  char probe;
  if (++p->sdepth > 4 * JS_MAX_DEPTH || below_floor(&probe)) {
    --p->sdepth;
    return perr(p, "expression nesting too deep");
  }
  r = parse_binary_inner(p, min_prec, no_in);
  --p->sdepth;
  return r;
}
static js_node *parse_binary_inner(js_parser *p, int min_prec, int no_in)
{
  js_node *left = parse_unary(p);
  if (p->failed)
    return NULL;
  for (;;) {
    int op = p->cur.kind;
    int prec = binop_prec(op, no_in);
    int next_min, line, col;
    js_node *right, *node;
    if (prec == 0 || prec <= min_prec)
      break;
    line = p->cur.line;
    col = p->cur.col;
    advance(p);
    next_min = (op == T_STARSTAR) ? prec - 1 : prec; /* ** is right-associative */
    right = parse_binary(p, next_min, no_in);
    if (p->failed)
      return NULL;
    node = js_node_new(p->arena, (op == T_AMPAMP || op == T_BARBAR || op == T_QQ) ? JS_LOGICAL : JS_BINARY, line, col);
    node->op = op;
    node->u.bin.left = left;
    node->u.bin.right = right;
    left = node;
  }
  return left;
}

static js_node *parse_conditional(js_parser *p, int no_in)
{
  js_node *test = parse_binary(p, 0, no_in);
  if (p->failed)
    return NULL;
  if (at(p, T_QUESTION)) {
    js_node *n = mk(p, JS_COND);
    advance(p);
    n->u.iff.test = test;
    n->u.iff.cons = parse_assignment(p, 0);
    expect(p, T_COLON, "expected ':' in conditional expression");
    n->u.iff.alt = parse_assignment(p, no_in);
    return p->failed ? NULL : n;
  }
  return test;
}

static int is_assign_op(int k) { return k == T_EQ || (k >= T_PLUS_EQ && k <= T_QQ_EQ); }

/* arrow body after the '=>' has been consumed */
static js_node *finish_arrow(js_parser *p, js_node **params, int nparams, int line, int col)
{
  js_node *fn = js_node_new(p->arena, JS_FUNC, line, col);
  int saved = p->in_async, saved_gen = p->in_generator;
  fn->u.func.is_arrow = 1;
  fn->u.func.is_async = p->pending_async;
  fn->u.func.params = params;
  fn->u.func.nparams = nparams;
  p->in_async = p->pending_async; /* await is a keyword in an async arrow */
  p->in_generator = 0;            /* an arrow can't yield for its enclosing generator */
  p->pending_async = 0;
  if (at(p, T_LBRACE)) {
    fn->u.func.body = parse_block(p);
    fn->u.func.is_expr_body = 0;
  } else {
    fn->u.func.body = parse_assignment(p, 0);
    fn->u.func.is_expr_body = 1;
  }
  p->in_async = saved;
  p->in_generator = saved_gen;
  return p->failed ? NULL : fn;
}

static js_node *parse_assignment_inner(js_parser *p, int no_in);
static js_node *parse_assignment(js_parser *p, int no_in)
{
  js_node *r;
  char probe;
  if (++p->depth > JS_MAX_DEPTH || below_floor(&probe)) {
    --p->depth;
    return perr(p, "expression nesting too deep");
  }
  r = parse_assignment_inner(p, no_in);
  --p->depth;
  return r;
}
static js_node *parse_assignment_inner(js_parser *p, int no_in)
{
  js_node *left;

  /* yield / yield* (only inside a generator body) */
  if (p->in_generator && at(p, T_IDENT) && word_is(&p->cur, "yield", 5)) {
    js_node *n = mk(p, JS_YIELD);
    advance(p);
    n->op = 0;
    if (at(p, T_STAR)) {
      n->op = 1;
      advance(p);
    } /* yield* delegate */
    if (!at(p, T_SEMI) && !at(p, T_RPAREN) && !at(p, T_RBRACK) && !at(p, T_RBRACE) && !at(p, T_COMMA) &&
        !at(p, T_COLON) && !at(p, T_EOF) && !p->cur.newline_before)
      n->u.one.arg = parse_assignment(p, no_in);
    return p->failed ? NULL : n;
  }

  /* async function expression / async arrow */
  if (at(p, T_IDENT) && word_is(&p->cur, "async", 5) && !peek(p)->newline_before) {
    int pk = peek(p)->kind;
    /* Only the async *arrow* forms are handled here; `async function` exprs
     * are parsed at the primary level (parse_primary) so they also work as a
     * unary operand / call callee (`!async function(){}()`). */
    if (pk == T_LPAREN) {
      advance(p);
      p->pending_async = 1; /* -> primary's paren arrow */
    } else if (pk == T_IDENT && peek2(p)->kind == T_ARROW) {
      advance(p);
      p->pending_async = 1; /* -> single-ident arrow */
    }
  }

  /* x => ...  (single-identifier arrow) */
  if (at(p, T_IDENT) && peek(p)->kind == T_ARROW) {
    int line = p->cur.line, col = p->cur.col;
    js_node **params = (js_node **)js_arena_alloc(p->arena, sizeof(js_node *));
    params[0] = eat_ident(p);
    advance(p); /* '=>' */
    return finish_arrow(p, params, 1, line, col);
  }

  left = parse_conditional(p, no_in);
  if (p->failed)
    return NULL;

  if (is_assign_op(p->cur.kind)) {
    int op = p->cur.kind;
    int line = p->cur.line, col = p->cur.col;
    js_node *n;
    if (op == T_EQ && (left->type == JS_ARRAY || left->type == JS_OBJECT)) {
      left = reinterpret_pattern(p, left); /* destructuring assignment target */
      if (p->failed)
        return NULL;
    }
    advance(p);
    n = js_node_new(p->arena, JS_ASSIGN, line, col);
    n->op = op;
    n->u.bin.left = left;
    n->u.bin.right = parse_assignment(p, no_in);
    return p->failed ? NULL : n;
  }
  return left;
}

static js_node *parse_expression(js_parser *p, int no_in)
{
  js_node *first = parse_assignment(p, no_in);
  if (p->failed)
    return NULL;
  if (!at(p, T_COMMA))
    return first;
  {
    js_node *seq;
    nvec v = {0};
    v.a = NULL;
    v.n = 0;
    v.cap = 0;
    nv_push(&v, first);
    while (eat(p, T_COMMA)) {
      nv_push(&v, parse_assignment(p, no_in));
      if (p->failed)
        break;
    }
    seq = mk(p, JS_SEQUENCE);
    seq->u.list.items = nv_freeze(p, &v, &seq->u.list.count);
    return p->failed ? NULL : seq;
  }
}

/* '(' ... — parenthesized expression or arrow parameter list (cover grammar) */
static js_node *parse_paren_or_arrow(js_parser *p)
{
  int line = p->cur.line, col = p->cur.col;
  nvec items = {0};
  items.a = NULL;
  items.n = 0;
  items.cap = 0;
  int saw_rest = 0;
  advance(p); /* '(' */
  while (!at(p, T_RPAREN) && !p->failed) {
    if (at(p, T_ELLIPSIS)) {
      js_node *r = mk(p, JS_REST);
      advance(p);
      r->u.one.arg = parse_binding_target(p);
      nv_push(&items, r);
      saw_rest = 1;
      break;
    }
    nv_push(&items, parse_assignment(p, 0));
    if (!eat(p, T_COMMA))
      break;
  }
  expect(p, T_RPAREN, "expected ')'");
  if (p->failed) {
    if (items.a)
      free(items.a);
    return NULL;
  }

  if (at(p, T_ARROW)) {
    js_node **params;
    int nparams, i;
    advance(p); /* '=>' */
    params = nv_freeze(p, &items, &nparams);
    for (i = 0; i < nparams; i++) {
      params[i] = reinterpret_pattern(p, params[i]);
      if (p->failed)
        return NULL;
    }
    return finish_arrow(p, params, nparams, line, col);
  }

  /* not an arrow: a parenthesized expression (drop any pending `async`) */
  p->pending_async = 0;
  if (saw_rest) {
    if (items.a)
      free(items.a);
    return perr(p, "rest parameter outside arrow function");
  }
  if (items.n == 0) {
    if (items.a)
      free(items.a);
    return perr(p, "expected expression");
  }
  if (items.n == 1) {
    js_node *e = items.a[0];
    free(items.a);
    return e;
  }
  {
    js_node *seq = js_node_new(p->arena, JS_SEQUENCE, line, col);
    seq->u.list.items = nv_freeze(p, &items, &seq->u.list.count);
    return seq;
  }
}

static js_node *parse_array_literal(js_parser *p)
{
  js_node *node;
  nvec els = {0};
  els.a = NULL;
  els.n = 0;
  els.cap = 0;
  advance(p); /* '[' */
  while (!at(p, T_RBRACK) && !p->failed) {
    if (at(p, T_COMMA)) {
      nv_push(&els, NULL);
      advance(p);
      continue;
    } /* elision */
    if (at(p, T_ELLIPSIS)) {
      js_node *s = mk(p, JS_SPREAD);
      advance(p);
      s->u.one.arg = parse_assignment(p, 0);
      nv_push(&els, s);
    } else {
      nv_push(&els, parse_assignment(p, 0));
    }
    if (!eat(p, T_COMMA))
      break;
  }
  expect(p, T_RBRACK, "expected ']' in array literal");
  node = mk(p, JS_ARRAY);
  node->u.list.items = nv_freeze(p, &els, &node->u.list.count);
  return p->failed ? NULL : node;
}

/* a method/accessor function body: '(' params ')' block */
static js_node *parse_method_function(js_parser *p)
{
  js_node *fn = mk(p, JS_FUNC);
  js_node **params;
  int n;
  int sa = p->in_async, sg = p->in_generator;
  nvec ps = {0};
  ps.a = NULL;
  ps.n = 0;
  ps.cap = 0;
  fn->u.func.is_async = p->pending_async;
  fn->u.func.is_generator = p->pending_generator;
  p->in_async = p->pending_async;
  p->in_generator = p->pending_generator;
  p->pending_async = p->pending_generator = 0;
  expect(p, T_LPAREN, "expected '(' for method parameters");
  while (!at(p, T_RPAREN) && !p->failed) {
    if (at(p, T_ELLIPSIS)) {
      js_node *r = mk(p, JS_REST);
      advance(p);
      r->u.one.arg = parse_binding_target(p);
      nv_push(&ps, r);
      break;
    }
    {
      js_node *t = parse_binding_target(p);
      if (eat(p, T_EQ)) {
        js_node *d = mk(p, JS_ASSIGN_PATTERN);
        d->u.assignpat.id = t;
        d->u.assignpat.init = parse_assignment(p, 0);
        t = d;
      }
      nv_push(&ps, t);
    }
    if (!eat(p, T_COMMA))
      break;
  }
  expect(p, T_RPAREN, "expected ')' after parameters");
  params = nv_freeze(p, &ps, &n);
  fn->u.func.params = params;
  fn->u.func.nparams = n;
  fn->u.func.body = parse_block(p);
  p->in_async = sa;
  p->in_generator = sg;
  return p->failed ? NULL : fn;
}

/* Consume a leading `async`/`*` (in either order) before a method name, setting
 * the pending flags parse_method_function reads. Returns 1 if any was consumed.
 */
static int method_prefix(js_parser *p)
{
  int got = 0;
  if (word_is(&p->cur, "async", 5) && !peek(p)->newline_before) {
    int k = peek(p)->kind;
    if (k != T_LPAREN && k != T_COLON && k != T_COMMA && k != T_RBRACE && k != T_EQ) {
      advance(p);
      p->pending_async = 1;
      got = 1;
    }
  }
  if (at(p, T_STAR)) {
    advance(p);
    p->pending_generator = 1;
    got = 1;
  }
  return got;
}

/* true if the contextual "get"/"set" at cur actually introduces an accessor
 * (i.e. it is followed by a real property key, not used as a key itself) */
static int starts_accessor(js_parser *p)
{
  int k;
  if (!(word_is(&p->cur, "get", 3) || word_is(&p->cur, "set", 3)))
    return 0;
  k = peek(p)->kind;
  return !(k == T_COLON || k == T_COMMA || k == T_RBRACE || k == T_LPAREN || k == T_EQ);
}

static js_node *parse_object_literal(js_parser *p)
{
  js_node *node;
  js_prop *props = NULL;
  int n = 0, cap = 0;
  advance(p); /* '{' */
  while (!at(p, T_RBRACE) && !p->failed) {
    js_prop pr;
    pr.kind = JS_PROP_INIT;
    pr.computed = 0;
    pr.shorthand = 0;
    pr.key = NULL;
    pr.value = NULL;
    if (at(p, T_ELLIPSIS)) {
      advance(p);
      pr.kind = JS_PROP_SPREAD;
      pr.value = parse_assignment(p, 0);
    } else if (starts_accessor(p)) {
      int isget = word_is(&p->cur, "get", 3);
      advance(p);
      pr.kind = isget ? JS_PROP_GET : JS_PROP_SET;
      {
        int comp = 0;
        pr.key = parse_property_key(p, &comp);
        pr.computed = (unsigned char)comp;
      }
      pr.value = parse_method_function(p);
    } else {
      int comp = 0;
      int ismeth = method_prefix(p); /* async / * generator method   */
      pr.key = parse_property_key(p, &comp);
      pr.computed = comp;
      if (ismeth || at(p, T_LPAREN)) { /* method shorthand */
        pr.kind = JS_PROP_METHOD;
        pr.value = parse_method_function(p);
      } else if (eat(p, T_COLON)) {
        pr.value = parse_assignment(p, 0);
      } else { /* { x } or { x = default } */
        pr.shorthand = 1;
        pr.value = pr.key;
        if (eat(p, T_EQ)) {
          js_node *d = mk(p, JS_ASSIGN_PATTERN);
          d->u.assignpat.id = pr.key;
          d->u.assignpat.init = parse_assignment(p, 0);
          pr.value = d;
        }
      }
    }
    if (!grow_array((void **)&props, &cap, n + 1, sizeof *props)) {
      perr(p, "out of memory");
      break;
    }
    props[n++] = pr;
    if (!eat(p, T_COMMA))
      break;
  }
  expect(p, T_RBRACE, "expected '}' in object literal");
  node = mk(p, JS_OBJECT);
  if (p->failed) {
    if (props)
      free(props);
    return NULL;
  }
  node->u.object.count = n;
  if (n) {
    node->u.object.props = (js_prop *)js_arena_alloc(p->arena, (unsigned)n * sizeof(js_prop));
    memcpy(node->u.object.props, props, (unsigned)n * sizeof(js_prop));
  }
  if (props)
    free(props);
  return node;
}

static js_node *parse_template(js_parser *p)
{
  js_node *node;
  nvec quasis = {0}, exprs = {0};
  quasis.a = NULL;
  quasis.n = 0;
  quasis.cap = 0;
  exprs.a = NULL;
  exprs.n = 0;
  exprs.cap = 0;

  if (at(p, T_TEMPLATE)) {
    js_node *q = mk(p, JS_STR);
    q->u.str.s = p->cur.value;
    q->u.str.len = p->cur.value_len;
    nv_push(&quasis, q);
    advance(p);
  } else { /* T_TEMPLATE_HEAD */
    js_node *q = mk(p, JS_STR);
    q->u.str.s = p->cur.value;
    q->u.str.len = p->cur.value_len;
    nv_push(&quasis, q);
    for (;;) {
      advance(p); /* consume HEAD/MIDDLE -> substitution */
      nv_push(&exprs, parse_expression(p, 0));
      if (p->failed)
        break;
      if (at(p, T_TEMPLATE_MIDDLE) || at(p, T_TEMPLATE_TAIL)) {
        js_node *qq = mk(p, JS_STR);
        qq->u.str.s = p->cur.value;
        qq->u.str.len = p->cur.value_len;
        nv_push(&quasis, qq);
        if (at(p, T_TEMPLATE_TAIL)) {
          advance(p);
          break;
        }
      } else {
        perr(p, "malformed template literal");
        break;
      }
    }
  }
  node = mk(p, JS_TEMPLATE);
  node->u.tmpl.quasis = nv_freeze(p, &quasis, &node->u.tmpl.nquasis);
  node->u.tmpl.exprs = nv_freeze(p, &exprs, &node->u.tmpl.nexprs);
  return p->failed ? NULL : node;
}

static js_node *parse_primary(js_parser *p)
{
  switch (p->cur.kind) {
  case T_NUM: {
    js_node *n = mk(p, JS_NUM);
    n->u.num = p->cur.num;
    advance(p);
    return n;
  }
  case T_BIGINT: {
    /* keep the source digits (sans the trailing 'n') so the interpreter can
     * build the exact bignum — the token's double approximation would lose it
     */
    js_node *n = mk(p, JS_BIGINT);
    n->u.str.s = js_arena_strndup(p->arena, p->cur.start, p->cur.len > 0 ? p->cur.len - 1 : 0);
    if (!n->u.str.s)
      p->failed = 1;
    n->u.str.len = p->cur.len > 0 ? p->cur.len - 1 : 0;
    advance(p);
    return n;
  }
  case T_STR: {
    js_node *n = mk(p, JS_STR);
    n->u.str.s = p->cur.value;
    n->u.str.len = p->cur.value_len;
    advance(p);
    return n;
  }
  case T_REGEX: {
    js_node *n = mk(p, JS_REGEX);
    n->u.regex.pattern = p->cur.value;
    n->u.regex.flags = p->cur.flags ? p->cur.flags : "";
    advance(p);
    return n;
  }
  case T_TRUE:
  case T_FALSE: {
    js_node *n = mk(p, JS_BOOL);
    n->u.num = (p->cur.kind == T_TRUE);
    advance(p);
    return n;
  }
  case T_NULL: {
    js_node *n = mk(p, JS_NULL);
    advance(p);
    return n;
  }
  case T_THIS: {
    js_node *n = mk(p, JS_THIS);
    advance(p);
    return n;
  }
  case T_SUPER: {
    js_node *n = mk(p, JS_SUPER);
    advance(p);
    return n;
  }
  case T_IDENT:
    /* `async function(){}` as an expression. Handled here (the primary level)
     * so it works in every expression context — as a unary operand or call
     * callee too, e.g. `!async function(){}()`. The async *arrow* forms stay
     * at assignment precedence (parse_assignment_inner). */
    if (p->cur.value && word_is(&p->cur, "async", 5) && peek(p)->kind == T_FUNCTION && !peek(p)->newline_before) {
      advance(p); /* `async` */
      p->pending_async = 1;
      return parse_function(p, 0);
    }
    /* `#name` (a private brand) only appears in expression position as the
     * left of `#x in obj`; emit it as the string key it maps to. */
    if (p->cur.value && p->cur.value[0] == '#') {
      js_node *k = mk(p, JS_STR);
      k->u.str.s = p->cur.value;
      k->u.str.len = p->cur.value_len;
      advance(p);
      return k;
    }
    return eat_ident(p);
  case T_LBRACK:
    return parse_array_literal(p);
  case T_LBRACE:
    return parse_object_literal(p);
  case T_LPAREN:
    return parse_paren_or_arrow(p);
  case T_FUNCTION:
    return parse_function(p, 0);
  case T_CLASS:
    return parse_class(p, 0);
  case T_IMPORT: { /* import(expr) / import.meta */
    js_node *n;
    advance(p);
    if (eat(p, T_DOT)) {
      if (!word_is(&p->cur, "meta", 4))
        return perr(p, "expected 'meta' after 'import.'");
      advance(p);
      return mk(p, JS_IMPORT_META);
    }
    expect(p, T_LPAREN, "expected '(' after 'import'");
    n = mk(p, JS_IMPORT_CALL);
    n->u.one.arg = parse_assignment(p, 0);
    expect(p, T_RPAREN, "expected ')' after import argument");
    return p->failed ? NULL : n;
  }
  case T_TEMPLATE:
  case T_TEMPLATE_HEAD:
    return parse_template(p);
  default:
    return perr(p, "expected an expression");
  }
}

/* ------------------------------------------------------- pattern coercion - */

static js_node *reinterpret_pattern(js_parser *p, js_node *n)
{
  if (!n)
    return NULL;
  switch (n->type) {
  case JS_IDENT:
  case JS_MEMBER: /* valid only in destructuring assignment */
  case JS_ARRAY_PATTERN:
  case JS_OBJECT_PATTERN:
  case JS_ASSIGN_PATTERN:
  case JS_REST:
    return n;
  case JS_SPREAD:
    n->type = JS_REST;
    n->u.one.arg = reinterpret_pattern(p, n->u.one.arg);
    return p->failed ? NULL : n;
  case JS_ASSIGN:
    if (n->op != T_EQ)
      return perr(p, "invalid destructuring target");
    {
      js_node *d = js_node_new(p->arena, JS_ASSIGN_PATTERN, n->line, n->col);
      d->u.assignpat.id = reinterpret_pattern(p, n->u.bin.left);
      d->u.assignpat.init = n->u.bin.right;
      return p->failed ? NULL : d;
    }
  case JS_ARRAY: {
    int i;
    for (i = 0; i < n->u.list.count; i++)
      if (n->u.list.items[i]) {
        n->u.list.items[i] = reinterpret_pattern(p, n->u.list.items[i]);
        if (p->failed)
          return NULL;
      }
    n->type = JS_ARRAY_PATTERN;
    return n;
  }
  case JS_OBJECT: {
    int i;
    for (i = 0; i < n->u.object.count; i++) {
      js_prop *pr = &n->u.object.props[i];
      pr->value = reinterpret_pattern(p, pr->value);
      if (p->failed)
        return NULL;
    }
    n->type = JS_OBJECT_PATTERN;
    return n;
  }
  default:
    return perr(p, "invalid destructuring target");
  }
}

/* ------------------------------------------------------- functions -------- */

static js_node *parse_params_into(js_parser *p, js_node ***out, int *count)
{
  nvec ps = {0};
  ps.a = NULL;
  ps.n = 0;
  ps.cap = 0;
  expect(p, T_LPAREN, "expected '(' before parameters");
  while (!at(p, T_RPAREN) && !p->failed) {
    if (at(p, T_ELLIPSIS)) {
      js_node *r = mk(p, JS_REST);
      advance(p);
      r->u.one.arg = parse_binding_target(p);
      nv_push(&ps, r);
      break;
    }
    {
      js_node *t = parse_binding_target(p);
      if (eat(p, T_EQ)) {
        js_node *d = mk(p, JS_ASSIGN_PATTERN);
        d->u.assignpat.id = t;
        d->u.assignpat.init = parse_assignment(p, 0);
        t = d;
      }
      nv_push(&ps, t);
    }
    if (!eat(p, T_COMMA))
      break;
  }
  expect(p, T_RPAREN, "expected ')' after parameters");
  *out = nv_freeze(p, &ps, count);
  return NULL;
}

static js_node *parse_function(js_parser *p, int is_decl)
{
  js_node *fn;
  int line = p->cur.line, col = p->cur.col;
  int sa = p->in_async, sg = p->in_generator;
  int is_async = p->pending_async, is_gen = 0;
  p->pending_async = 0;
  advance(p); /* 'function' */
  if (at(p, T_STAR)) {
    is_gen = 1;
    advance(p);
  }
  fn = js_node_new(p->arena, JS_FUNC, line, col);
  fn->u.func.is_async = is_async;
  fn->u.func.is_generator = is_gen;
  if (at(p, T_IDENT)) {
    fn->u.func.name = p->cur.value;
    advance(p);
  } else if (is_decl)
    return perr(p, "function declaration requires a name");
  parse_params_into(p, &fn->u.func.params, &fn->u.func.nparams);
  if (p->failed)
    return NULL;
  p->in_async = is_async;
  p->in_generator = is_gen;
  fn->u.func.body = parse_block(p);
  p->in_async = sa;
  p->in_generator = sg;
  return p->failed ? NULL : fn;
}

static js_node *parse_class(js_parser *p, int is_decl)
{
  js_node *node;
  js_member *members = NULL;
  int n = 0, cap = 0;
  int line = p->cur.line, col = p->cur.col;
  advance(p); /* 'class' */
  node = js_node_new(p->arena, JS_CLASS, line, col);
  if (at(p, T_IDENT))
    node->u.cls.id = eat_ident(p);
  else if (is_decl)
    return perr(p, "class declaration requires a name");
  if (eat(p, T_EXTENDS)) {
    node->u.cls.super = parse_lhs(p);
    if (p->failed)
      return NULL;
  }
  expect(p, T_LBRACE, "expected '{' to open class body");
  while (!at(p, T_RBRACE) && !p->failed) {
    js_member m;
    if (eat(p, T_SEMI))
      continue; /* stray separators */
    m.kind = JS_M_METHOD;
    m.is_static = 0;
    m.computed = 0;
    m.key = NULL;
    m.value = NULL;
    if (word_is(&p->cur, "static", 6) && peek(p)->kind != T_LPAREN && peek(p)->kind != T_EQ) {
      m.is_static = 1;
      advance(p);
    }
    if (m.is_static && at(p, T_LBRACE)) { /* static initialization block */
      js_node *fn = js_node_new(p->arena, JS_FUNC, line, col);
      fn->u.func.name = NULL;
      fn->u.func.params = NULL;
      fn->u.func.nparams = 0;
      fn->u.func.is_arrow = fn->u.func.is_expr_body = fn->u.func.is_async = fn->u.func.is_generator = 0;
      fn->u.func.body = parse_block(p);
      m.kind = JS_M_STATIC_BLOCK;
      m.value = fn;
      if (!grow_array((void **)&members, &cap, n + 1, sizeof *members)) {
        perr(p, "out of memory");
        break;
      }
      members[n++] = m;
      continue;
    }
    {
      int ismeth = method_prefix(p); /* async / * generator method   */
      if (!ismeth && starts_accessor(p)) {
        int isget = word_is(&p->cur, "get", 3);
        advance(p);
        m.kind = isget ? JS_M_GET : JS_M_SET;
        {
          int comp = 0;
          m.key = parse_property_key(p, &comp);
          m.computed = (unsigned char)comp;
        }
        m.value = parse_method_function(p);
      } else {
        {
          int comp = 0;
          m.key = parse_property_key(p, &comp);
          m.computed = (unsigned char)comp;
        }
        if (ismeth || at(p, T_LPAREN)) {
          if (!ismeth && !m.computed && m.key->type == JS_IDENT && node_word_is(m.key, "constructor"))
            m.kind = JS_M_CTOR;
          else
            m.kind = JS_M_METHOD;
          m.value = parse_method_function(p);
        } else { /* class field */
          m.kind = JS_M_FIELD;
          if (eat(p, T_EQ))
            m.value = parse_assignment(p, 0);
          else
            m.value = NULL;
          if (!at(p, T_RBRACE)) {
            if (!(p->cur.newline_before || at(p, T_SEMI))) {
              perr(p, "expected ';' after class field");
              break;
            }
            eat(p, T_SEMI);
          }
        }
      }
    }
    if (!grow_array((void **)&members, &cap, n + 1, sizeof *members)) {
      perr(p, "out of memory");
      break;
    }
    members[n++] = m;
  }
  expect(p, T_RBRACE, "expected '}' to close class body");
  if (p->failed) {
    if (members)
      free(members);
    return NULL;
  }
  node->u.cls.nmembers = n;
  if (n) {
    node->u.cls.members = (js_member *)js_arena_alloc(p->arena, (unsigned)n * sizeof(js_member));
    memcpy(node->u.cls.members, members, (unsigned)n * sizeof(js_member));
  }
  if (members)
    free(members);
  return node;
}

/* ------------------------------------------------------- statements ------- */

static int semicolon(js_parser *p)
{
  if (at(p, T_SEMI)) {
    advance(p);
    return 1;
  }
  if (at(p, T_RBRACE) || at(p, T_EOF) || p->cur.newline_before)
    return 1; /* ASI */
  perr(p, "expected ';'");
  return 0;
}

static js_node *parse_block(js_parser *p)
{
  js_node *node;
  nvec body = {0};
  body.a = NULL;
  body.n = 0;
  body.cap = 0;
  expect(p, T_LBRACE, "expected '{'");
  while (!at(p, T_RBRACE) && !at(p, T_EOF) && !p->failed)
    nv_push(&body, parse_statement(p));
  expect(p, T_RBRACE, "expected '}'");
  node = mk(p, JS_BLOCK);
  node->u.list.items = nv_freeze(p, &body, &node->u.list.count);
  return p->failed ? NULL : node;
}

static js_node *parse_var_decl(js_parser *p, int kind, int consume_semi, int no_in)
{
  js_node *node;
  js_declor *decls = NULL;
  int n = 0, cap = 0;
  advance(p); /* var/const/let token */
  do {
    js_declor d;
    d.id = parse_binding_target(p);
    d.init = NULL;
    if (eat(p, T_EQ))
      d.init = parse_assignment(p, no_in);
    if (!grow_array((void **)&decls, &cap, n + 1, sizeof *decls)) {
      perr(p, "out of memory");
      break;
    }
    decls[n++] = d;
  } while (eat(p, T_COMMA) && !p->failed);
  if (consume_semi)
    semicolon(p);
  node = mk(p, JS_VAR_DECL);
  node->u.var.kind = kind;
  if (p->failed) {
    if (decls)
      free(decls);
    return NULL;
  }
  node->u.var.count = n;
  node->u.var.decls = (js_declor *)js_arena_alloc(p->arena, (unsigned)n * sizeof(js_declor));
  memcpy(node->u.var.decls, decls, (unsigned)n * sizeof(js_declor));
  free(decls);
  return node;
}

static js_node *parse_for(js_parser *p)
{
  js_node *node;
  int line = p->cur.line, col = p->cur.col;
  js_node *init = NULL;
  int is_await = 0;
  advance(p); /* 'for' */
  if (at(p, T_IDENT) && word_is(&p->cur, "await", 5)) {
    advance(p); /* `for await` */
    is_await = 1;
  }
  expect(p, T_LPAREN, "expected '(' after 'for'");

  if (at(p, T_SEMI)) {
    init = NULL; /* for(;;) */
  } else if (at(p, T_VAR) || at(p, T_CONST) || is_let(p)) {
    int kind = at(p, T_VAR) ? JS_DECL_VAR : at(p, T_CONST) ? JS_DECL_CONST : JS_DECL_LET;
    init = parse_var_decl(p, kind, 0, 1);
    if (p->failed)
      return NULL;
    if (at(p, T_IN) || is_of(p)) {
      int of = is_of(p);
      advance(p);
      node = js_node_new(p->arena, JS_FOR_IN, line, col);
      node->u.forin.of = of ? 1 + is_await : 0; /* 2 = for await */
      node->u.forin.left = init;
      node->u.forin.right = of ? parse_assignment(p, 0) : parse_expression(p, 0);
      expect(p, T_RPAREN, "expected ')' after for-header");
      node->u.forin.body = parse_statement(p);
      return p->failed ? NULL : node;
    }
  } else {
    init = parse_expression(p, 1);
    if (p->failed)
      return NULL;
    if (at(p, T_IN) || is_of(p)) {
      int of = is_of(p);
      js_node *left = reinterpret_pattern(p, init);
      if (p->failed)
        return NULL;
      advance(p);
      node = js_node_new(p->arena, JS_FOR_IN, line, col);
      node->u.forin.of = of ? 1 + is_await : 0; /* 2 = for await */
      node->u.forin.left = left;
      node->u.forin.right = of ? parse_assignment(p, 0) : parse_expression(p, 0);
      expect(p, T_RPAREN, "expected ')' after for-header");
      node->u.forin.body = parse_statement(p);
      return p->failed ? NULL : node;
    }
    /* C-style for: keep the init as a raw expression node */
  }

  expect(p, T_SEMI, "expected ';' in for-header");
  node = js_node_new(p->arena, JS_FOR, line, col);
  node->u.forc.init = init;
  node->u.forc.test = at(p, T_SEMI) ? NULL : parse_expression(p, 0);
  expect(p, T_SEMI, "expected ';' in for-header");
  node->u.forc.update = at(p, T_RPAREN) ? NULL : parse_expression(p, 0);
  expect(p, T_RPAREN, "expected ')' after for-header");
  node->u.forc.body = parse_statement(p);
  return p->failed ? NULL : node;
}

static js_node *parse_if(js_parser *p)
{
  js_node *node = mk(p, JS_IF);
  advance(p); /* 'if' */
  expect(p, T_LPAREN, "expected '(' after 'if'");
  node->u.iff.test = parse_expression(p, 0);
  expect(p, T_RPAREN, "expected ')' after condition");
  node->u.iff.cons = parse_statement(p);
  node->u.iff.alt = eat(p, T_ELSE) ? parse_statement(p) : NULL;
  return p->failed ? NULL : node;
}

static js_node *parse_while(js_parser *p)
{
  js_node *node = mk(p, JS_WHILE);
  advance(p);
  expect(p, T_LPAREN, "expected '(' after 'while'");
  node->u.loop.test = parse_expression(p, 0);
  expect(p, T_RPAREN, "expected ')' after condition");
  node->u.loop.body = parse_statement(p);
  return p->failed ? NULL : node;
}

static js_node *parse_do(js_parser *p)
{
  js_node *node = mk(p, JS_DO_WHILE);
  advance(p);
  node->u.loop.body = parse_statement(p);
  expect(p, T_WHILE, "expected 'while' after do-body");
  expect(p, T_LPAREN, "expected '(' after 'while'");
  node->u.loop.test = parse_expression(p, 0);
  expect(p, T_RPAREN, "expected ')' after condition");
  eat(p, T_SEMI);
  return p->failed ? NULL : node;
}

static js_node *parse_switch(js_parser *p)
{
  js_node *node;
  js_case *cases = NULL;
  int n = 0, cap = 0;
  js_node *disc;
  int line = p->cur.line, col = p->cur.col;
  advance(p); /* 'switch' */
  expect(p, T_LPAREN, "expected '(' after 'switch'");
  disc = parse_expression(p, 0);
  expect(p, T_RPAREN, "expected ')' after switch value");
  expect(p, T_LBRACE, "expected '{' to open switch body");
  while (!at(p, T_RBRACE) && !at(p, T_EOF) && !p->failed) {
    js_case c;
    nvec body = {0};
    body.a = NULL;
    body.n = 0;
    body.cap = 0;
    if (eat(p, T_CASE)) {
      c.test = parse_expression(p, 0);
    } else if (eat(p, T_DEFAULT)) {
      c.test = NULL;
    } else {
      perr(p, "expected 'case' or 'default'");
      break;
    }
    expect(p, T_COLON, "expected ':' after case label");
    while (!at(p, T_CASE) && !at(p, T_DEFAULT) && !at(p, T_RBRACE) && !at(p, T_EOF) && !p->failed)
      nv_push(&body, parse_statement(p));
    c.body = nv_freeze(p, &body, &c.nbody);
    if (!grow_array((void **)&cases, &cap, n + 1, sizeof *cases)) {
      perr(p, "out of memory");
      break;
    }
    cases[n++] = c;
  }
  expect(p, T_RBRACE, "expected '}' to close switch body");
  node = js_node_new(p->arena, JS_SWITCH, line, col);
  if (p->failed) {
    if (cases)
      free(cases);
    return NULL;
  }
  node->u.sw.disc = disc;
  node->u.sw.count = n;
  if (n) {
    node->u.sw.cases = (js_case *)js_arena_alloc(p->arena, (unsigned)n * sizeof(js_case));
    memcpy(node->u.sw.cases, cases, (unsigned)n * sizeof(js_case));
  }
  if (cases)
    free(cases);
  return node;
}

static js_node *parse_try(js_parser *p)
{
  js_node *node = mk(p, JS_TRY);
  advance(p); /* 'try' */
  node->u.tc.block = parse_block(p);
  if (eat(p, T_CATCH)) {
    if (eat(p, T_LPAREN)) {
      node->u.tc.param = parse_binding_target(p);
      expect(p, T_RPAREN, "expected ')' after catch binding");
    }
    node->u.tc.handler = parse_block(p);
  }
  if (eat(p, T_FINALLY))
    node->u.tc.finalizer = parse_block(p);
  if (!node->u.tc.handler && !node->u.tc.finalizer)
    return perr(p, "try needs a catch or finally");
  return p->failed ? NULL : node;
}

/* ------------------------------------------------------- ES modules ------- */

static const char *arena_str(js_parser *p, const char *s, int n)
{
  const char *d = js_arena_strndup(p->arena, s, n);
  if (!d)
    p->failed = 1;
  return d;
}
/* a name token inside an import/export clause: an identifier, or `default`. */
static const char *read_name_token(js_parser *p)
{
  const char *s;
  if (at(p, T_IDENT)) {
    s = arena_str(p, p->cur.value, p->cur.value_len);
    advance(p);
    return s;
  }
  if (at(p, T_DEFAULT)) {
    advance(p);
    return "default";
  }
  perr(p, "expected a name");
  return 0;
}
static const char *parse_module_spec(js_parser *p)
{
  const char *s;
  if (!at(p, T_STR)) {
    perr(p, "expected module specifier string");
    return 0;
  }
  s = arena_str(p, p->cur.value, p->cur.value_len);
  advance(p);
  return s;
}
/* `{ a, b as c }` -> bindings; .imported is the first name, .local the `as`
 * name (or the same). Import and export read the two fields per their own
 * direction. */
static js_binding *parse_named_clause(js_parser *p, int *count)
{
  js_binding *list = NULL;
  int n = 0, cap = 0;
  js_binding *out;
  expect(p, T_LBRACE, "expected '{'");
  while (!at(p, T_RBRACE) && !p->failed) {
    js_binding b;
    b.imported = read_name_token(p);
    b.local = b.imported;
    if (word_is(&p->cur, "as", 2)) {
      advance(p);
      b.local = read_name_token(p);
    }
    if (p->failed)
      break;
    if (!grow_array((void **)&list, &cap, n + 1, sizeof *list)) {
      perr(p, "out of memory");
      break;
    }
    list[n++] = b;
    if (!eat(p, T_COMMA))
      break;
  }
  expect(p, T_RBRACE, "expected '}'");
  *count = n;
  out = n ? (js_binding *)js_arena_alloc(p->arena, (unsigned)n * sizeof(js_binding)) : NULL;
  if (n)
    memcpy(out, list, (unsigned)n * sizeof(js_binding));
  if (list)
    free(list);
  return out;
}

static js_node *parse_import_decl(js_parser *p)
{
  js_node *n = mk(p, JS_IMPORT);
  advance(p); /* 'import' */
  n->u.imp.spec = 0;
  n->u.imp.defname = 0;
  n->u.imp.nsname = 0;
  n->u.imp.names = 0;
  n->u.imp.nnames = 0;
  if (at(p, T_STR)) { /* import "spec";  (side effect) */
    n->u.imp.spec = parse_module_spec(p);
    semicolon(p);
    return p->failed ? NULL : n;
  }
  if (at(p, T_IDENT)) { /* default import */
    n->u.imp.defname = arena_str(p, p->cur.value, p->cur.value_len);
    advance(p);
    eat(p, T_COMMA);
  }
  if (at(p, T_STAR)) { /* * as ns */
    advance(p);
    if (!word_is(&p->cur, "as", 2))
      return perr(p, "expected 'as' after '*'");
    advance(p);
    if (!at(p, T_IDENT))
      return perr(p, "expected namespace name");
    n->u.imp.nsname = arena_str(p, p->cur.value, p->cur.value_len);
    advance(p);
  } else if (at(p, T_LBRACE)) {
    n->u.imp.names = parse_named_clause(p, &n->u.imp.nnames);
  }
  if (!word_is(&p->cur, "from", 4))
    return perr(p, "expected 'from' in import");
  advance(p);
  n->u.imp.spec = parse_module_spec(p);
  semicolon(p);
  return p->failed ? NULL : n;
}

static js_node *parse_export_decl(js_parser *p)
{
  js_node *n = mk(p, JS_EXPORT);
  advance(p); /* 'export' */
  n->u.exp.decl = 0;
  n->u.exp.defexpr = 0;
  n->u.exp.spec = 0;
  n->u.exp.names = 0;
  n->u.exp.nnames = 0;
  n->u.exp.star = 0;
  if (at(p, T_DEFAULT)) { /* export default ... */
    advance(p);
    if (at(p, T_FUNCTION))
      n->u.exp.defexpr = parse_function(p, 0);
    else if (at(p, T_CLASS))
      n->u.exp.defexpr = parse_class(p, 0);
    else if (word_is(&p->cur, "async", 5) && peek(p)->kind == T_FUNCTION) {
      advance(p);
      p->pending_async = 1;
      n->u.exp.defexpr = parse_function(p, 0);
    } else {
      n->u.exp.defexpr = parse_assignment(p, 0);
      semicolon(p);
    }
    return p->failed ? NULL : n;
  }
  if (at(p, T_STAR)) { /* export * from "spec" */
    advance(p);
    n->u.exp.star = 1;
    if (!word_is(&p->cur, "from", 4))
      return perr(p, "expected 'from' after 'export *'");
    advance(p);
    n->u.exp.spec = parse_module_spec(p);
    semicolon(p);
    return p->failed ? NULL : n;
  }
  if (at(p, T_LBRACE)) { /* export { a, b as c } [from "spec"] */
    n->u.exp.names = parse_named_clause(p, &n->u.exp.nnames);
    if (word_is(&p->cur, "from", 4)) {
      advance(p);
      n->u.exp.spec = parse_module_spec(p);
    }
    semicolon(p);
    return p->failed ? NULL : n;
  }
  if (at(p, T_VAR))
    n->u.exp.decl = parse_var_decl(p, JS_DECL_VAR, 1, 0);
  else if (at(p, T_CONST))
    n->u.exp.decl = parse_var_decl(p, JS_DECL_CONST, 1, 0);
  else if (is_let(p))
    n->u.exp.decl = parse_var_decl(p, JS_DECL_LET, 1, 0);
  else if (at(p, T_FUNCTION))
    n->u.exp.decl = parse_function(p, 1);
  else if (at(p, T_CLASS))
    n->u.exp.decl = parse_class(p, 1);
  else if (word_is(&p->cur, "async", 5) && peek(p)->kind == T_FUNCTION) {
    advance(p);
    p->pending_async = 1;
    n->u.exp.decl = parse_function(p, 1);
  } else
    return perr(p, "unexpected token after 'export'");
  return p->failed ? NULL : n;
}

static js_node *parse_statement_inner(js_parser *p);
static js_node *parse_statement(js_parser *p)
{
  js_node *r;
  char probe;
  if (++p->depth > JS_MAX_DEPTH || below_floor(&probe)) {
    --p->depth;
    return perr(p, "statement nesting too deep");
  }
  r = parse_statement_inner(p);
  --p->depth;
  return r;
}
static js_node *parse_statement_inner(js_parser *p)
{
  switch (p->cur.kind) {
  case T_LBRACE:
    return parse_block(p);
  case T_SEMI: {
    js_node *n = mk(p, JS_EMPTY);
    advance(p);
    return n;
  }
  case T_VAR:
    return parse_var_decl(p, JS_DECL_VAR, 1, 0);
  case T_CONST:
    return parse_var_decl(p, JS_DECL_CONST, 1, 0);
  case T_FUNCTION:
    return parse_function(p, 1);
  case T_CLASS:
    return parse_class(p, 1);
  case T_IF:
    return parse_if(p);
  case T_FOR:
    return parse_for(p);
  case T_WHILE:
    return parse_while(p);
  case T_DO:
    return parse_do(p);
  case T_SWITCH:
    return parse_switch(p);
  case T_TRY:
    return parse_try(p);
  case T_DEBUGGER: {
    js_node *n = mk(p, JS_DEBUGGER);
    advance(p);
    semicolon(p);
    return p->failed ? NULL : n;
  }
  case T_RETURN: {
    js_node *n = mk(p, JS_RETURN);
    advance(p);
    if (!at(p, T_SEMI) && !at(p, T_RBRACE) && !at(p, T_EOF) && !p->cur.newline_before)
      n->u.one.arg = parse_expression(p, 0);
    semicolon(p);
    return p->failed ? NULL : n;
  }
  case T_THROW: {
    js_node *n = mk(p, JS_THROW);
    advance(p);
    if (p->cur.newline_before)
      return perr(p, "illegal newline after 'throw'");
    n->u.one.arg = parse_expression(p, 0);
    semicolon(p);
    return p->failed ? NULL : n;
  }
  case T_BREAK:
  case T_CONTINUE: {
    int brk = p->cur.kind == T_BREAK;
    js_node *n = mk(p, brk ? JS_BREAK : JS_CONTINUE);
    advance(p);
    if (at(p, T_IDENT) && !p->cur.newline_before) {
      n->u.labeled.label = p->cur.value;
      advance(p);
    }
    semicolon(p);
    return p->failed ? NULL : n;
  }
  case T_IMPORT:
    /* `import(` / `import.` are expressions (dynamic import / import.meta);
     * anything else is an import declaration. */
    if (peek(p)->kind == T_LPAREN || peek(p)->kind == T_DOT)
      break;
    return parse_import_decl(p);
  case T_EXPORT:
    return parse_export_decl(p);
  default:
    break;
  }

  /* async function declaration */
  if (word_is(&p->cur, "async", 5) && peek(p)->kind == T_FUNCTION && !peek(p)->newline_before) {
    advance(p); /* 'async' */
    p->pending_async = 1;
    return parse_function(p, 1);
  }

  /* labeled statement:  ident ':' */
  if (at(p, T_IDENT) && peek(p)->kind == T_COLON) {
    js_node *n = mk(p, JS_LABELED);
    n->u.labeled.label = p->cur.value;
    advance(p); /* label */
    advance(p); /* ':' */
    n->u.labeled.body = parse_statement(p);
    return p->failed ? NULL : n;
  }

  /* let declaration (contextual) */
  if (is_let(p)) {
    int k = peek(p)->kind;
    if (k == T_IDENT || k == T_LBRACK || k == T_LBRACE)
      return parse_var_decl(p, JS_DECL_LET, 1, 0);
  }

  {
    js_node *e = parse_expression(p, 0);
    js_node *n;
    if (p->failed)
      return NULL;
    n = mk(p, JS_EXPR_STMT);
    n->u.one.arg = e;
    semicolon(p);
    return p->failed ? NULL : n;
  }
}

/* ------------------------------------------------------- entry ------------ */

js_node *js_parse(const char *src, js_arena *arena, js_parse_error *err)
{
  js_parser p;
  js_node *prog;
  nvec body = {0};
  body.a = NULL;
  body.n = 0;
  body.cap = 0;

  p.arena = arena;
  p.n_ahead = 0;
  p.failed = 0;
  p.errmsg = NULL;
  p.errline = 0;
  p.errcol = 0;
  p.in_async = p.in_generator = p.pending_async = p.pending_generator = 0;
  p.depth = 0;
  p.sdepth = 0;
  js_lexer_init(&p.lx, src, arena);
  advance(&p);

  while (!at(&p, T_EOF) && !p.failed)
    nv_push(&body, parse_statement(&p));

  if (p.failed) {
    if (body.a)
      free(body.a);
    if (err) {
      err->msg = p.errmsg ? p.errmsg : "syntax error";
      err->line = p.errline;
      err->col = p.errcol;
    }
    return NULL;
  }
  prog = js_node_new(arena, JS_PROGRAM, 1, 1);
  prog->u.list.items = nv_freeze(&p, &body, &prog->u.list.count);
  if (err) {
    err->msg = NULL;
    err->line = 0;
    err->col = 0;
  }
  return prog;
}
