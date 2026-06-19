/* ast.h — JavaScript abstract syntax tree + arena for the OwOS JS engine.
 *
 * Every node is one `js_node`: a type tag, an operator field (a jtok kind, for
 * the operator nodes), a source line, and a union of the per-type payload. A
 * tree-walk interpreter wants uniform, long-lived nodes more than it wants a
 * distinct struct per production, so this is deliberately one tagged union.
 *
 * All nodes — and the strings they point at — are bump-allocated from a
 * js_arena and outlive parsing for as long as the program runs; the whole tree
 * is released at once with js_arena_free(). Runtime *values* are a separate
 * concern (the GC), not these nodes.
 */
#ifndef OWOS_JS_AST_H
#define OWOS_JS_AST_H

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---------------------------------------------------------------- arena ---- */

typedef struct arena_block arena_block;
typedef struct { arena_block *head; } js_arena;

void  js_arena_init(js_arena *a);
void  js_arena_free(js_arena *a);
void *js_arena_alloc(js_arena *a, unsigned n);          /* zeroed, 8-aligned   */
char *js_arena_strndup(js_arena *a, const char *s, int n);

/* ---------------------------------------------------------------- nodes ---- */

enum js_node_type {
    /* literals & primaries */
    JS_NUM, JS_STR, JS_REGEX, JS_BOOL, JS_NULL, JS_IDENT, JS_THIS, JS_SUPER,
    JS_TEMPLATE, JS_TAGGED_TEMPLATE, JS_ARRAY, JS_OBJECT,

    /* operators */
    JS_UNARY,      /* prefix !  ~  +  -  typeof  void  delete           */
    JS_UPDATE,     /* ++ / -- (u.unary.prefix says pre/post)            */
    JS_BINARY,     /* arithmetic/relational/equality/shift/bitwise/in/instanceof */
    JS_LOGICAL,    /* && || ??                                          */
    JS_ASSIGN,     /* = and all compound assignments (op in node.op)    */
    JS_COND,       /* ?:                                                */
    JS_SEQUENCE,   /* comma operator                                    */

    /* access / invocation */
    JS_MEMBER, JS_CALL, JS_NEW, JS_SPREAD,

    /* async / generators */
    JS_AWAIT,      /* await expr (u.one.arg)                              */
    JS_YIELD,      /* yield / yield* expr (u.one.arg; op = 1 for yield*)  */

    /* functions & classes */
    JS_FUNC, JS_CLASS,

    /* binding patterns (destructuring) */
    JS_ARRAY_PATTERN, JS_OBJECT_PATTERN, JS_ASSIGN_PATTERN, JS_REST,

    /* statements */
    JS_PROGRAM, JS_BLOCK, JS_VAR_DECL, JS_EXPR_STMT, JS_EMPTY,
    JS_IF, JS_FOR, JS_FOR_IN, JS_WHILE, JS_DO_WHILE,
    JS_RETURN, JS_BREAK, JS_CONTINUE, JS_THROW, JS_TRY, JS_SWITCH,
    JS_LABELED, JS_DEBUGGER,

    /* ES modules */
    JS_IMPORT,       /* import declaration (u.imp)                          */
    JS_EXPORT,       /* export declaration (u.exp)                          */
    JS_IMPORT_CALL,  /* dynamic import(expr) (u.one.arg) -> Promise         */
    JS_IMPORT_META   /* import.meta                                         */
};

/* object-literal / object-pattern property kinds */
enum { JS_PROP_INIT, JS_PROP_GET, JS_PROP_SET, JS_PROP_METHOD, JS_PROP_SPREAD };
/* class member kinds */
enum { JS_M_METHOD, JS_M_GET, JS_M_SET, JS_M_FIELD, JS_M_CTOR };
/* var-declaration kinds */
enum { JS_DECL_VAR, JS_DECL_LET, JS_DECL_CONST };

typedef struct js_node js_node;

typedef struct {
    js_node      *key;        /* ident/string/number node, or a computed expr  */
    js_node      *value;      /* value, function (method/get/set), or pattern  */
    unsigned char kind;       /* JS_PROP_*                                     */
    unsigned char computed;   /* key was [expr]                                */
    unsigned char shorthand;  /* { x } / { x = d }                             */
} js_prop;

typedef struct {
    js_node *id;              /* identifier or binding pattern                 */
    js_node *init;            /* initializer, or NULL                          */
} js_declor;

/* one import/export name mapping: the external name and the local name (e.g.
 * `imported as local`, or `local as exported`). Arena-duplicated, NUL-terminated. */
typedef struct { const char *imported; const char *local; } js_binding;

typedef struct {
    js_node  *test;           /* NULL for `default:`                           */
    js_node **body;
    int       nbody;
} js_case;

typedef struct {
    js_node      *key;
    js_node      *value;      /* function node, or field initializer / NULL    */
    unsigned char kind;       /* JS_M_*                                        */
    unsigned char is_static;
    unsigned char computed;
} js_member;

struct js_node {
    int type;                 /* enum js_node_type                             */
    int op;                   /* jtok kind for UNARY/UPDATE/BINARY/LOGICAL/ASSIGN */
    int line;
    union {
        double num;                                   /* NUM; BOOL (0/1)       */
        struct { const char *s; int len; } str;       /* STR, IDENT, LABELED via str */
        struct { const char *pattern, *flags; } regex;

        struct { js_node **items; int count; } list;  /* PROGRAM/BLOCK/ARRAY/SEQUENCE */
        struct { js_node *test, *cons, *alt; } iff;   /* IF, COND              */
        struct { js_node *test, *body; } loop;        /* WHILE, DO_WHILE       */
        struct { js_node *arg; } one;                 /* RETURN/THROW/EXPR_STMT/SPREAD/REST/paren */
        struct { const char *label; js_node *body; } labeled; /* LABELED; BREAK/CONTINUE use label */

        struct { js_node *left, *right; } bin;        /* BINARY/LOGICAL/ASSIGN/TAGGED_TEMPLATE */
        struct { js_node *operand; int prefix; } unary; /* UNARY, UPDATE       */
        struct { js_node *callee; js_node **args; int nargs; int optional; } call; /* CALL, NEW */
        struct { js_node *obj, *prop; int computed, optional; } member;        /* MEMBER */

        struct { const char *name; js_node **params; int nparams; js_node *body;
                 int is_arrow, is_expr_body, is_async, is_generator; } func; /* FUNC */
        struct { js_node *id, *super; js_member *members; int nmembers; } cls;  /* CLASS */
        struct { js_prop *props; int count; } object; /* OBJECT, OBJECT_PATTERN */

        struct { js_declor *decls; int count; int kind; } var;  /* VAR_DECL    */
        struct { js_node *init, *test, *update, *body; } forc;  /* FOR         */
        struct { js_node *left, *right, *body; int of; } forin; /* FOR_IN/FOR_OF (of=1) */
        struct { js_node *block, *param, *handler, *finalizer; } tc;  /* TRY    */
        struct { js_node *disc; js_case *cases; int count; } sw;     /* SWITCH  */
        struct { js_node **quasis; int nquasis; js_node **exprs; int nexprs; } tmpl; /* TEMPLATE */
        struct { js_node *id, *init; } assignpat;     /* ASSIGN_PATTERN        */

        /* ES modules */
        struct { const char *spec; const char *defname; const char *nsname;
                 js_binding *names; int nnames; } imp;        /* IMPORT          */
        struct { js_node *decl; js_node *defexpr; const char *spec;
                 js_binding *names; int nnames; int star; } exp; /* EXPORT       */
    } u;
};

js_node *js_node_new(js_arena *a, int type, int line);

/* Render a tree as a compact, parenthesized s-expression into a freshly
 * malloc()'d NUL-terminated string (caller free()s). For tests and debugging;
 * the format is not stable API. */
char *js_ast_dump(js_node *n);

#endif /* OWOS_JS_AST_H */
