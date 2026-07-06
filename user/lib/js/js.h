/* js.h — the OwOS JavaScript engine, public umbrella header.
 *
 * Built for the browser, but kept consumer-agnostic: it parses a JS source
 * string into an AST and (in a later step) will evaluate it. Nothing here knows
 * about the DOM — the DOM bindings live in the browser, layered on top.
 *
 * What works today:
 *   - lexer.h  — a full modern tokenizer (templates, regex, every number and
 *                operator form, ASI bookkeeping).
 *   - ast.h    — the AST node set + arena allocator + an s-expression dumper.
 *   - parser.h — js_parse(): ES5 plus the ES2015 a real-world page relies on —
 *                let/const, arrows, templates, spread/rest, default params,
 *                array+object destructuring, for-of, optional chaining/nullish,
 *                computed/shorthand/method/getter/setter object props, **, classes.
 *   - value.h / interp.h — the tree-walk interpreter: GC-managed values and
 *                objects, lexical scopes + closures + `this`, prototypes, the
 *                full operator coercions, exceptions (try/catch/finally/throw),
 *                functions + `new` + `arguments`, var/function hoisting, classes
 *                (extends/super/constructors/methods/accessors/static/fields),
 *                and a core standard library (console, Math, Object, Array,
 *                String, Number, Boolean, JSON, the Error family, parse* globals).
 *                Run with js_ctx_new() + js_run().
 *
 * Deferred to later increments (the interpreter throws a clear error, or the
 * parser rejects, rather than silently misbehaving):
 *   - RegExp matching: a /re/ literal evaluates to a placeholder object with
 *     .source/.flags but no .test/.exec engine yet.
 *   - generators / yield, async / await / Promise   (need coroutines).
 *   - ES modules: import / export                    (separate loader).
 *   - Date, Map/Set, Symbol, Proxy/Reflect, typed arrays.
 *
 * Class notes (pragmatic, not 100% spec): a derived `this` is shared down the
 * super() chain (pre-ES6 style, no TDZ-before-super), instance fields initialise
 * at constructor entry rather than strictly after super(), and `extends` a native
 * builtin (Error/Array) doesn't fully initialise the builtin's internal state.
 *
 * Known simplifications: strings are byte- (not UTF-16-) indexed, so .length and
 * indexing are exact for ASCII and count bytes for multibyte text; for-of
 * iterates arrays and strings only.
 *
 * Parse usage:
 *   js_arena a; js_arena_init(&a);
 *   js_parse_error e;
 *   js_node *prog = js_parse(src, &a, &e);
 *   if (!prog) { ... e.msg, e.line ... }
 *
 * Run usage (the program's AST must outlive the run — keep the arena alive):
 *   js_ctx *ctx = js_ctx_new(my_print);   // my_print routes console output
 *   js_value result;
 *   if (js_run(ctx, prog, &result) != 0) { ... ctx->exc ... }
 *   js_ctx_free(ctx);
 *   js_arena_free(&a);
 */
#ifndef OWOS_JS_H
#define OWOS_JS_H

#include "ast.h"
#include "lexer.h"
#include "parser.h"

#endif /* OWOS_JS_H */
