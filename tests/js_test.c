/* js_test.c — tiny host harness: run each argv script through the real OwOS JS
 * engine and echo console output + any uncaught exception. Used to verify BigInt.
 *
 *   cc -std=gnu11 -Itests -Iuser/lib -Iuser/lib/js -Iuser/lib/math -Iuser/lib/gc \
 *      -Iuser/lib/base64 \
 *      tests/js_test.c user/lib/js/lexer.c user/lib/js/parser.c user/lib/js/ast.c \
 *      user/lib/js/value.c user/lib/js/interp.c user/lib/js/builtins.c \
 *      user/lib/js/bigint.c user/lib/math/math.c user/lib/math/fmt.c \
 *      user/lib/gc/gc.c user/lib/base64/base64.c -o /tmp/js_test \
 *      && /tmp/js_test 'console.log(1n + 2n)'
 */
#include "ast.h"
#include "gc.h"
#include "interp.h"
#include "parser.h"
#include "value.h"
#include <stdio.h>
#include <stdlib.h>

static void con_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  const char *tag = level >= 3 ? "ERR " : level == 2 ? "WARN" : "LOG ";
  printf("[%s] %.*s\n", tag, len, s);
}

static char *slurp(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = (char *)malloc((size_t)n + 1);
  if (!b) {
    fclose(f);
    return NULL;
  }
  if (fread(b, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(b);
    return NULL;
  }
  b[n] = 0;
  fclose(f);
  return b;
}

static void run(const char *src)
{
  js_arena a;
  js_parse_error err = {0, 0};
  js_node *prog;
  js_ctx *ctx;
  js_value result;
  js_arena_init(&a);
  ctx = js_ctx_new(con_print);
  prog = js_parse(src, &a, &err);
  if (!prog) {
    printf("PARSE ERROR: %s\n", err.msg ? err.msg : "?");
    js_ctx_free(ctx);
    js_arena_free(&a);
    return;
  }
  if (js_run(ctx, prog, &result) != 0) {
    js_value nm = js_get(ctx, ctx->exc, "name");
    js_value msg = js_get(ctx, ctx->exc, "message");
    printf("UNCAUGHT %s: %s\n", nm.type == JV_STR ? nm.u.str->data : "Error",
           msg.type == JV_STR ? msg.u.str->data : "(non-string)");
  }
  js_ctx_free(ctx);
  js_arena_free(&a);
}

int main(int argc, char **argv)
{
  int i;
  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && argv[i][1] == 'f' && i + 1 < argc) {
      char *src = slurp(argv[++i]);
      if (!src) {
        printf("cannot read %s\n", argv[i]);
        continue;
      }
      run(src);
      free(src);
    } else {
      run(argv[i]);
    }
  }
  return 0;
}
