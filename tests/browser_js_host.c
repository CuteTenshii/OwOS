#include <stdlib.h>
#include <string.h>
#include "jsint.h"

js_ctx *g_ctx;
void sb_putn(sbuf *b, const char *s, int n) { int c = b->cap ? b->cap : 32; while (c < b->n + n + 1) c *= 2; if (c != b->cap) b->b = realloc(b->b, (size_t)c); b->cap = c; memcpy(b->b + b->n, s, (size_t)n); b->n += n; b->b[b->n] = 0; }
void def_method(js_object *o, const char *n, js_native f, int a) { js_def_native(g_ctx, o, n, f, a); }
void def_acc(js_object *o, const char *n, js_native g, js_native s) { js_def_accessor(o, n, g ? js_new_native(g_ctx, g, n, 0) : 0, s ? js_new_native(g_ctx, s, n, 1) : 0, P_CONFIGURABLE); }
const char *arg_str(js_ctx *c, js_value v) { js_value s = js_to_string(c, v); return s.type == JV_STR ? s.u.str->data : ""; }
js_value el_noop(js_ctx *c, js_value t, js_value *a, int n) { (void)c; (void)t; (void)a; (void)n; return js_undef(); }
unsigned msec(void) { static unsigned clock = 1000; return clock += 5; }
long long now_ms(void) { return 1720000000000LL; }
