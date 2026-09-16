/* Host harness: execute a sequence of classic scripts then a module entry with
 * the real OwOS JS engine, using files under a mirror dir as the module store.
 * Reports console output and any uncaught exception.  Build line at bottom.
 *
 *   argv[1]      = mirror dir (e.g. /tmp/c3tmirror)
 *   argv[2..]    = classic scripts to run in order, then LAST arg is the module.
 * Convention: pass "--module <file>" to mark the ES-module entry.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "parser.h"
#include "interp.h"
#include "value.h"
#include "gc.h"

static char g_dir[1024];

/* Root every module's exports for the whole run, mirroring how the browser keeps
 * them alive in its rooted g_module_cache (the engine doesn't root
 * ctx->cur_module_exports itself). Without this, a GC during module execution
 * frees the exports object out from under the interpreter. */
static js_object *g_modcache;   /* rooted below in main */

static char *slurp(const char *path, long *outlen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0; fclose(f); if (outlen) *outlen = n; return b;
}

static void con_print(js_ctx *ctx, int level, const char *s, int len) {
    (void)ctx;
    const char *tag = level >= 3 ? "ERR " : level == 2 ? "WARN" : "LOG ";
    fprintf(stderr, "[console %s] %.*s\n", tag, len, s);
}

/* Resolve a module specifier ("./tests.js", "./tests/x.js") to a mirror path. */
static void spec_to_path(const char *referrer, const char *spec, char *out, int cap) {
    (void)referrer;
    const char *s = spec;
    while (*s == '.' || *s == '/') s++;         /* strip leading ./ or / */
    snprintf(out, (size_t)cap, "%s/%s", g_dir, s);
}

static js_object *load_module(js_ctx *ctx, const char *spec, void *ud) {
    (void)ud;
    char path[1200];
    spec_to_path(ctx->cur_module_url, spec, path, sizeof path);
    long len = 0;
    char *src = slurp(path, &len);
    if (!src) { fprintf(stderr, "[loader] MISS %s -> %s\n", spec, path); return NULL; }
    js_arena *ar = malloc(sizeof *ar); js_arena_init(ar);
    js_parse_error pe; pe.msg = NULL;
    js_node *prog = js_parse(src, ar, &pe);
    free(src);
    if (!prog) { fprintf(stderr, "[loader] PARSEFAIL %s:%d %s\n", path, pe.line, pe.msg ? pe.msg : ""); js_arena_free(ar); free(ar); return NULL; }
    js_object *exports = js_new_object(ctx, ctx->object_proto);
    js_def(g_modcache, path, js_obj_value(exports), 0);   /* root exports for the run */
    js_run_module(ctx, prog, exports, spec);   /* spec used as this module's url (referrer for its own imports) */
    if (ctx->has_exc) {
        js_value ev = ctx->exc;
        js_value msg = js_get(ctx, ev, "message");
        fprintf(stderr, "[loader] EXC while running module %s: %s\n", spec,
                msg.type == JV_STR ? msg.u.str->data : "(non-string)");
    }
    /* arena intentionally leaked: AST must outlive closures (fine for a one-shot). */
    return exports;
}

static int run_classic(js_ctx *ctx, const char *path) {
    long len = 0; char *src = slurp(path, &len);
    if (!src) { fprintf(stderr, "[run] cannot read %s\n", path); return -1; }
    js_arena *ar = malloc(sizeof *ar); js_arena_init(ar);
    js_parse_error pe; pe.msg = NULL;
    js_node *prog = js_parse(src, ar, &pe);
    free(src);
    if (!prog) { fprintf(stderr, "[run] PARSEFAIL %s:%d %s\n", path, pe.line, pe.msg ? pe.msg : ""); return -1; }
    js_value res;
    if (js_run(ctx, prog, &res) != 0) {
        js_value ev = ctx->exc;
        js_value msg = js_get(ctx, ev, "message");
        js_value nm  = js_get(ctx, ev, "name");
        fprintf(stderr, "[run] UNCAUGHT in %s: %s: %s\n", path,
                nm.type == JV_STR ? nm.u.str->data : "Error",
                msg.type == JV_STR ? msg.u.str->data : "(non-string exc)");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <mirrordir> <script...> --module <entry>\n", argv[0]); return 2; }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);

    js_ctx *ctx = js_ctx_new(con_print);
    js_setup_builtins(ctx);
    js_set_module_loader(ctx, load_module, NULL);
    gc_add_root((void **)&g_modcache);
    g_modcache = js_new_object(ctx, ctx->object_proto);

    const char *module_entry = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--module") == 0 && i + 1 < argc) { module_entry = argv[++i]; continue; }
        char path[1200]; snprintf(path, sizeof path, "%s/%s", g_dir, argv[i]);
        fprintf(stderr, "=== classic: %s ===\n", argv[i]);
        run_classic(ctx, path);
        ctx->has_exc = 0; ctx->exc = js_undef();   /* continue after an error, like a browser */
    }

    if (module_entry) {
        char path[1200]; snprintf(path, sizeof path, "%s/%s", g_dir, module_entry);
        fprintf(stderr, "=== module: %s ===\n", module_entry);
        long len = 0; char *src = slurp(path, &len);
        if (src) {
            js_arena *ar = malloc(sizeof *ar); js_arena_init(ar);
            js_parse_error pe; pe.msg = NULL;
            js_node *prog = js_parse(src, ar, &pe);
            free(src);
            if (prog) {
                js_object *exports = js_new_object(ctx, ctx->object_proto);
                js_def(g_modcache, "@entry", js_obj_value(exports), 0);
                js_run_module(ctx, prog, exports, module_entry);
                if (ctx->has_exc) {
                    js_value msg = js_get(ctx, ctx->exc, "message");
                    fprintf(stderr, "[module] UNCAUGHT: %s\n", msg.type == JV_STR ? msg.u.str->data : "(non-string)");
                    ctx->has_exc = 0;
                }
            } else fprintf(stderr, "[module] PARSEFAIL %s:%d %s\n", path, pe.line, pe.msg ? pe.msg : "");
        }
    }

    /* Fire window.onload (css3test wires its entry point there). */
    js_value onload = js_get(ctx, js_obj_value(ctx->global), "onload");
    if (js_is_callable(onload)) {
        fprintf(stderr, "=== firing window.onload ===\n");
        js_call(ctx, onload, js_obj_value(ctx->global), NULL, 0);
        if (ctx->has_exc) {
            js_value msg = js_get(ctx, ctx->exc, "message");
            js_value nm  = js_get(ctx, ctx->exc, "name");
            fprintf(stderr, "[onload] UNCAUGHT: %s: %s\n",
                    nm.type == JV_STR ? nm.u.str->data : "Error",
                    msg.type == JV_STR ? msg.u.str->data : "(non-string)");
            ctx->has_exc = 0;
        }
    } else {
        fprintf(stderr, "=== window.onload is not callable (type %d) ===\n", onload.type);
    }

    /* Report the resulting score text. */
    js_value doc = js_get(ctx, js_obj_value(ctx->global), "document");
    js_value gid = js_get(ctx, doc, "getElementById");
    if (js_is_callable(gid)) {
        js_value arg = js_mk_cstring(ctx, "score");
        js_value el = js_call(ctx, gid, doc, &arg, 1);
        if (el.type == JV_OBJ) {
            js_value tc = js_get(ctx, el, "textContent");
            fprintf(stderr, "\n>>> #score textContent = \"%s\"\n", tc.type == JV_STR ? tc.u.str->data : "(none)");
        }
    }
    return 0;
}
