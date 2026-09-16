/* parser.h — JavaScript parser for the OwOS JS engine.
 *
 * A hand-written recursive-descent parser with a precedence-climbing core for
 * binary operators. It targets ES5 plus the ES2015 a real-world bundle relies
 * on; the deferred constructs are listed in js.h. The result is an arena-owned
 * AST (ast.h); free the whole tree with js_arena_free(arena).
 */
#ifndef OWOS_JS_PARSER_H
#define OWOS_JS_PARSER_H

#include "ast.h"

typedef struct {
  const char *msg; /* NULL when parsing succeeded */
  int line;
  int col; /* 1-based column of the offending token */
} js_parse_error;

/* Parse NUL-terminated UTF-8 `src` into a JS_PROGRAM node allocated from
 * `arena`. Returns the program, or NULL on a syntax error (then *err, if
 * non-NULL, describes it). */
js_node *js_parse(const char *src, js_arena *arena, js_parse_error *err);

#endif /* OWOS_JS_PARSER_H */
