/* Host harness: run the real OwOS JS parser over files given on argv, report
 * syntax errors. Build:
 *   cc -std=c11 -Iscripts -Iuser/lib/js scripts/parse_test.c \
 *      user/lib/js/lexer.c user/lib/js/parser.c user/lib/js/ast.c -o /tmp/parse_test
 */
#include <stdio.h>
#include <stdlib.h>
#include "ast.h"
#include "parser.h"

static char *slurp(const char *path, long *outlen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0; fclose(f); if (outlen) *outlen = n; return b;
}

int main(int argc, char **argv) {
    int fails = 0;
    for (int i = 1; i < argc; i++) {
        long len = 0;
        char *src = slurp(argv[i], &len);
        if (!src) { printf("READFAIL %s\n", argv[i]); fails++; continue; }
        js_arena a; js_arena_init(&a);
        js_parse_error e; e.msg = NULL; e.line = 0;
        js_node *prog = js_parse(src, &a, &e);
        if (!prog) {
            printf("PARSEFAIL %s:%d  %s\n", argv[i], e.line, e.msg ? e.msg : "(no msg)");
            fails++;
        }
        js_arena_free(&a);
        free(src);
    }
    printf("\n%d file(s) failed to parse\n", fails);
    return fails ? 1 : 0;
}
