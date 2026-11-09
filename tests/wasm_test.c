/* wasm_test.c: the WebAssembly engine and its JavaScript API.
 *
 * The script in wasm_vectors.h (see tests/wasm/gen.js) is run by the real JS engine. It instantiates modules built
 * from the .wat files in tests/wasm, checks the API against results recorded from V8, and runs a numeric and a SIMD
 * module through every operator, comparing a hash of the outputs with V8's. A collection is forced around each check so
 * a missing GC root shows up as a wrong answer or a crash.
 *
 *   scripts/run-tests.sh wasm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OWOS_SUITE_TEST
#include "ast.h"
#include "gc.h"
#include "interp.h"
#include "parser.h"
#include "value.h"
#include "wasm_vectors.h"

static void con_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  (void)level;
  printf("%.*s\n", len, s);
}

static js_value native_gc(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)ctx;
  (void)thisv;
  (void)args;
  (void)argc;
  gc_collect();
  return js_undef();
}

static char *join_script(void)
{
  size_t n = 1;
  char *src, *p;
  int i;
  for (i = 0; k_wasm_script[i]; i++)
    n += strlen(k_wasm_script[i]);
  p = src = (char *)malloc(n);
  for (i = 0; k_wasm_script[i]; i++) {
    size_t l = strlen(k_wasm_script[i]);
    memcpy(p, k_wasm_script[i], l);
    p += l;
  }
  *p = 0;
  return src;
}

static void run_script(void)
{
  js_arena a;
  js_parse_error err = {0, 0};
  js_value result, report;
  char *src = join_script();
  js_node *prog;
  js_ctx *ctx;

  js_arena_init(&a);
  ctx = js_ctx_new(con_print);
  js_def_native(ctx, ctx->global, "gc", native_gc, 0);
  js_def(ctx->global, "__result", js_undef(), P_WRITABLE);
  prog = js_parse(src, &a, &err);
  expect(prog != NULL);
  if (prog) {
    expect(js_run(ctx, prog, &result) == 0);
    if (ctx->has_exc) {
      js_value msg = js_get(ctx, ctx->exc, "message");
      printf("uncaught: %s\n", msg.type == JV_STR ? msg.u.str->data : "?");
    }
    report = js_get(ctx, js_obj_value(ctx->global), "__result");
    expect(report.type == JV_STR);
    if (report.type == JV_STR) {
      if (report.u.str->len)
        printf("failed checks:\n%s\n", report.u.str->data);
      expect(report.u.str->len == 0);
    }
  }
  js_ctx_free(ctx);
  js_arena_free(&a);
  free(src);
}

/* Under a policy without 'wasm-unsafe-eval' compiling fails with a CompileError and validate still answers */
static void blocked_by_csp(void)
{
  static const char src[] =
      "var m = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]), out = [];\n"
      "try { new WebAssembly.Module(m); out.push('module'); } catch (e) { out.push(e.name); }\n"
      "try { new WebAssembly.Instance({}); out.push('instance'); } catch (e) { out.push(e.name); }\n"
      "WebAssembly.compile(m).then(function () { out.push('compiled'); }, function (e) { out.push(e.name); });\n"
      "WebAssembly.instantiate(m).then(function () { out.push('instantiated'); }, function (e) { out.push(e.name); "
      "});\n"
      "out.push(WebAssembly.validate(m));\n"
      "globalThis.tail = out;\n";
  js_arena a;
  js_parse_error err = {0, 0};
  js_value result, tail;
  js_node *prog;
  js_ctx *ctx;

  js_arena_init(&a);
  ctx = js_ctx_new(con_print);
  js_wasm_block(ctx);
  prog = js_parse(src, &a, &err);
  expect(prog != NULL);
  if (prog) {
    expect(js_run(ctx, prog, &result) == 0);
    tail = js_get(ctx, js_obj_value(ctx->global), "tail");
    expect_string(js_to_string(ctx, tail).u.str->data, "CompileError,CompileError,CompileError,CompileError,true");
  }
  js_ctx_free(ctx);
  js_arena_free(&a);
}

int main(void)
{
  test("wasm api, operators and simd against v8", run_script);
  test("wasm blocked by csp", blocked_by_csp);
  return 0;
}
