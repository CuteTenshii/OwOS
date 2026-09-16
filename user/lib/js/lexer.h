/* lexer.h — JavaScript tokenizer for the OwOS browser's JS engine.
 *
 * One pass over NUL-terminated UTF-8 source, producing a stream of jtoken. The
 * lexer cooks string and template escapes into arena memory, so a token's
 * `value` is the decoded text, not the raw slice. Two pieces of context the
 * tokenizer cannot decide alone live here because they need the parser:
 *   - regex vs. divide: `/` is a regex literal in expression position and the
 *     `/` operator otherwise; we track the previous significant token to
 * choose.
 *   - template substitutions: a `}` closing a `${ ... }` must resume template
 *     scanning, so a small brace-context stack remembers which `}` does that.
 */
#ifndef OWOS_JS_LEXER_H
#define OWOS_JS_LEXER_H

#include "ast.h" /* js_arena */

#ifndef NULL
#define NULL ((void *)0)
#endif

enum jtok {
  T_EOF = 0,
  T_ERROR,

  /* literals / names */
  T_IDENT,
  T_NUM,
  T_BIGINT,
  T_STR,
  T_REGEX,
  T_TEMPLATE,        /* `no substitution`                 */
  T_TEMPLATE_HEAD,   /* `head${                            */
  T_TEMPLATE_MIDDLE, /* }middle${                          */
  T_TEMPLATE_TAIL,   /* }tail`                             */

  /* reserved keywords (contextual ones — let/of/async/await/get/set/static/
   * yield — stay T_IDENT and the parser matches them by spelling) */
  T_BREAK,
  T_CASE,
  T_CATCH,
  T_CLASS,
  T_CONST,
  T_CONTINUE,
  T_DEBUGGER,
  T_DEFAULT,
  T_DELETE,
  T_DO,
  T_ELSE,
  T_EXPORT,
  T_EXTENDS,
  T_FINALLY,
  T_FOR,
  T_FUNCTION,
  T_IF,
  T_IMPORT,
  T_IN,
  T_INSTANCEOF,
  T_NEW,
  T_RETURN,
  T_SUPER,
  T_SWITCH,
  T_THIS,
  T_THROW,
  T_TRY,
  T_TYPEOF,
  T_VAR,
  T_VOID,
  T_WHILE,
  T_WITH,
  T_NULL,
  T_TRUE,
  T_FALSE,

  /* punctuators */
  T_LBRACE,
  T_RBRACE,
  T_LPAREN,
  T_RPAREN,
  T_LBRACK,
  T_RBRACK,
  T_DOT,
  T_ELLIPSIS,
  T_SEMI,
  T_COMMA,
  T_LT,
  T_GT,
  T_LE,
  T_GE,
  T_EQEQ,
  T_NE,
  T_EQEQEQ,
  T_NEEQ,
  T_PLUS,
  T_MINUS,
  T_STAR,
  T_PERCENT,
  T_STARSTAR,
  T_SLASH,
  T_PLUSPLUS,
  T_MINUSMINUS,
  T_LTLT,
  T_GTGT,
  T_GTGTGT,
  T_AMP,
  T_BAR,
  T_CARET,
  T_BANG,
  T_TILDE,
  T_AMPAMP,
  T_BARBAR,
  T_QQ,
  T_QUESTION,
  T_QDOT,
  T_COLON,
  T_ARROW,
  T_EQ,
  T_PLUS_EQ,
  T_MINUS_EQ,
  T_STAR_EQ,
  T_SLASH_EQ,
  T_PERCENT_EQ,
  T_STARSTAR_EQ,
  T_LTLT_EQ,
  T_GTGT_EQ,
  T_GTGTGT_EQ,
  T_AMP_EQ,
  T_BAR_EQ,
  T_CARET_EQ,
  T_AMPAMP_EQ,
  T_BARBAR_EQ,
  T_QQ_EQ
};

typedef struct {
  int kind; /* enum jtok                                  */
  int line;
  int col;            /* 1-based column of the token's first char   */
  int newline_before; /* a line terminator preceded this token (ASI)*/
  const char *start;  /* raw slice into source                      */
  int len;            /* raw slice length                           */
  double num;         /* T_NUM / T_BIGINT numeric value             */
  const char *value;  /* cooked text: T_STR/T_TEMPLATE*, ident name */
  int value_len;
  const char *flags; /* T_REGEX flag string (cooked, NUL-term)     */
} jtoken;

typedef struct {
  const char *src;
  const char *p;
  const char *end;
  int line;
  const char *line_start; /* first char of the current line, for columns */
  js_arena *arena;
  int prev_kind;            /* last non-trivial token kind, for regex test*/
  int saw_newline;          /* newline seen since the last token          */
  unsigned char brace[128]; /* brace-context stack: 0 = {, 1 = ${         */
  int brace_sp;
  unsigned char paren[128]; /* per-'(' flag: 1 = control-flow header paren */
  int paren_sp;
  int prev_rparen_ctrl;  /* last token was a ')' closing if/for/while/with */
  int prev_rbrace_block; /* last token was a '}' closing a block (not an object
                            literal) */
  const char *err;       /* set on a lexical error; token kind T_ERROR */
} jlexer;

void js_lexer_init(jlexer *lx, const char *src, js_arena *arena);

/* Produce the next token into *out. After T_ERROR, lx->err holds the message
 * and further calls keep returning T_EOF. */
void js_lex_next(jlexer *lx, jtoken *out);

/* Human spelling of a token kind, for parser error messages and the AST dump.
 * Operator kinds return the operator text ("+", "===", "=>"); category kinds
 * return a name ("identifier", "number", "string"). */
const char *js_tok_spell(int kind);

#endif /* OWOS_JS_LEXER_H */
