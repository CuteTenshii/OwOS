/* webapi_test.c — host harness proving the browser's performance / TextEncoder /
 * TextDecoder Web APIs (user/apps/gui/browser/js.c) behave correctly against the
 * real OwOS JS engine (user/lib/js). The native implementations are static inside
 * js.c (coupled to browser.h), so this test re-registers faithful equivalents on a
 * real engine context and drives them through actual JavaScript — the same
 * Uint8Array / ArrayBuffer machinery js.c relies on.
 *
 *   cc -std=gnu11 -Iscripts -Iuser/lib -Iuser/lib/js -Iuser/lib/math -Iuser/lib/gc \
 *      scripts/webapi_test.c user/lib/js/lexer.c user/lib/js/parser.c \
 *      user/lib/js/ast.c user/lib/js/value.c user/lib/js/interp.c \
 *      user/lib/js/builtins.c user/lib/js/bigint.c user/lib/math/math.c \
 *      user/lib/math/fmt.c user/lib/gc/gc.c -o /tmp/webapi_test && /tmp/webapi_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "parser.h"
#include "interp.h"
#include "value.h"
#include "gc.h"

/* ---- the exact transcoding + wiring from js.c, re-expressed for the host ---- */

static const unsigned short k_cp1252_hi[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};
enum { DEC_UTF8, DEC_UTF16LE, DEC_UTF16BE, DEC_LATIN1 };

static int host_utf8_encode(unsigned cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | (cp >> 6 & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | cp >> 18); out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
static int host_utf8_decode(const char *s, const char *end, unsigned *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && s + 1 < end && (s[1] & 0xC0) == 0x80) { *cp = ((unsigned)(c & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && s + 2 < end && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) { *cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3; }
    if ((c & 0xF8) == 0xF0 && s + 3 < end && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) { *cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) | ((unsigned)(s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4; }
    *cp = 0xFFFD; return 1;
}

typedef struct { char *b; int n, cap; } sbuf;
static void sb_putn(sbuf *s, const char *t, int n) {
    if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2; s->b = realloc(s->b, s->cap); }
    memcpy(s->b + s->n, t, n); s->n += n; s->b[s->n] = 0;
}

static js_value make_uint8array(js_ctx *ctx, const unsigned char *data, int len) {
    js_value u8ctor, lenarg, view;
    if (len < 0) len = 0;
    u8ctor = js_get(ctx, js_obj_value(ctx->global), "Uint8Array");
    lenarg = js_number(len);
    view = js_construct(ctx, u8ctor, &lenarg, 1);
    if (data && len && view.type == JV_OBJ && view.u.obj->kind == OBJ_TYPEDARRAY && view.u.obj->host) {
        js_typedarray *ta = (js_typedarray *)view.u.obj->host;
        if (ta->buffer && ta->buffer->host) {
            js_arraybuffer *ab = (js_arraybuffer *)ta->buffer->host;
            int n = len;
            if (ta->offset + n > ab->byteLength) n = ab->byteLength - ta->offset;
            if (n > 0) memcpy(ab->data + ta->offset, data, (unsigned)n);
        }
    }
    return view;
}
static const unsigned char *view_bytes(js_value v, int *len) {
    *len = 0;
    if (v.type != JV_OBJ || !v.u.obj->host) return 0;
    if (v.u.obj->kind == OBJ_ARRAYBUFFER) { js_arraybuffer *ab = v.u.obj->host; *len = ab->byteLength; return ab->data; }
    if (v.u.obj->kind == OBJ_TYPEDARRAY) {
        js_typedarray *ta = v.u.obj->host;
        js_arraybuffer *ab;
        if (!ta->buffer || !ta->buffer->host) return 0;
        ab = ta->buffer->host;
        *len = ta->length * ta_elem_size(ta->type < 0 ? TA_U8 : ta->type);
        return ab->data + ta->offset;
    }
    return 0;
}

static js_value textenc_encode(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_value s = js_to_string(ctx, argc > 0 ? args[0] : js_mk_cstring(ctx, "")), r;
    (void)t;
    gc_push_scope();
    if (s.type == JV_STR) gc_keep(s.u.str);
    r = s.type == JV_STR ? make_uint8array(ctx, (const unsigned char *)s.u.str->data, s.u.str->len)
                         : make_uint8array(ctx, (const unsigned char *)"", 0);
    gc_pop_scope();
    return r;
}
static js_value textdec_decode(js_ctx *ctx, js_value t, js_value *args, int argc) {
    int len = 0, kind = (int)js_to_number(ctx, js_get(ctx, t, "@kind")), i;
    const unsigned char *in = view_bytes(argc > 0 ? args[0] : js_undef(), &len);
    sbuf sb = { 0, 0, 0 };
    js_value r;
    if (!in) return js_mk_cstring(ctx, "");
    if (kind == DEC_UTF8) {
        for (i = 0; i < len; ) {
            unsigned cp; char tmp[4]; int m, k = host_utf8_decode((const char *)in + i, (const char *)in + len, &cp);
            if (k <= 0) k = 1;
            m = host_utf8_encode(cp, tmp); sb_putn(&sb, tmp, m); i += k;
        }
    } else if (kind == DEC_UTF16LE || kind == DEC_UTF16BE) {
        int be = kind == DEC_UTF16BE;
        for (i = 0; i + 1 < len; i += 2) {
            unsigned u = be ? (unsigned)(in[i] << 8 | in[i+1]) : (unsigned)(in[i] | in[i+1] << 8), cp = u;
            char tmp[4]; int m;
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < len) {
                unsigned lo = be ? (unsigned)(in[i+2] << 8 | in[i+3]) : (unsigned)(in[i+2] | in[i+3] << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; } else cp = 0xFFFD;
            } else if (u >= 0xD800 && u <= 0xDFFF) cp = 0xFFFD;
            m = host_utf8_encode(cp, tmp); sb_putn(&sb, tmp, m);
        }
    } else {
        for (i = 0; i < len; i++) {
            unsigned b = in[i], cp = (b >= 0x80 && b <= 0x9F) ? k_cp1252_hi[b - 0x80] : b;
            char tmp[4]; int m = host_utf8_encode(cp, tmp); sb_putn(&sb, tmp, m);
        }
    }
    r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
    free(sb.b);
    return r;
}
static int decoder_kind(const char *b) {
    if (!b[0] || !strcmp(b, "utf-8") || !strcmp(b, "utf8")) return DEC_UTF8;
    if (!strcmp(b, "utf-16") || !strcmp(b, "utf-16le")) return DEC_UTF16LE;
    if (!strcmp(b, "utf-16be")) return DEC_UTF16BE;
    return DEC_LATIN1;
}
static js_value textdec_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    js_object *o = js_new_object(ctx, ctx->object_proto);
    const char *label = argc > 0 && args[0].type == JV_STR ? args[0].u.str->data : "";
    (void)thisv;
    js_def(o, "@kind", js_number(decoder_kind(label)), 0);
    js_def_native(ctx, o, "decode", textdec_decode, 1);
    return js_obj_value(o);
}
static js_value textenc_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    js_object *o = js_new_object(ctx, ctx->object_proto);
    (void)thisv; (void)args; (void)argc;
    js_def_native(ctx, o, "encode", textenc_encode, 1);
    return js_obj_value(o);
}

static long long g_clock = 1000;
static js_value perf_now(js_ctx *ctx, js_value t, js_value *a, int n) {
    (void)t; (void)a; (void)n;
    return js_number((double)(g_clock += 5));   /* monotonic, advances each call */
}

static int g_fail;
static void con_print(js_ctx *ctx, int level, const char *s, int len) {
    (void)ctx;
    if (level >= 3 || (len >= 4 && !strncmp(s, "FAIL", 4))) g_fail = 1;
    printf("%.*s\n", len, s);
}

static void run(js_ctx *ctx, const char *src) {
    js_arena a; js_parse_error err = { 0, 0 }; js_node *prog; js_value result;
    js_arena_init(&a);
    prog = js_parse(src, &a, &err);
    if (!prog) { printf("PARSE ERROR: %s\n", err.msg ? err.msg : "?"); g_fail = 1; js_arena_free(&a); return; }
    if (js_run(ctx, prog, &result) != 0) {
        js_value msg = js_get(ctx, ctx->exc, "message");
        printf("UNCAUGHT: %s\n", msg.type == JV_STR ? msg.u.str->data : "(err)");
        g_fail = 1;
    }
    js_arena_free(&a);
}

int main(void) {
    js_ctx *ctx = js_ctx_new(con_print);
    js_object *te = js_new_native(ctx, textenc_ctor, "TextEncoder", 0);
    js_object *td = js_new_native(ctx, textdec_ctor, "TextDecoder", 0);
    js_object *pf = js_new_object(ctx, ctx->object_proto);
    js_def(ctx->global, "TextEncoder", js_obj_value(te), P_WRITABLE | P_CONFIGURABLE);
    js_def(ctx->global, "TextDecoder", js_obj_value(td), P_WRITABLE | P_CONFIGURABLE);
    js_def_native(ctx, pf, "now", perf_now, 0);
    js_def(pf, "timeOrigin", js_number(1720000000000.0), P_ENUMERABLE);
    js_def(ctx->global, "performance", js_obj_value(pf), P_WRITABLE | P_CONFIGURABLE);

    run(ctx,
        "function ok(c,m){ console.log((c?'ok  ':'FAIL ')+m); }\n"
        /* encode: ASCII */
        "var e=new TextEncoder();\n"
        "var a=e.encode('Hi!');\n"
        "ok(a instanceof Uint8Array,'encode returns Uint8Array');\n"
        "ok(a.length===3 && a[0]===72 && a[1]===105 && a[2]===33,'encode ASCII bytes');\n"
        /* encode: multibyte (euro sign built from its UTF-8 bytes = engine model) */
        "var eu=String.fromCharCode(0x20AC);\n"          /* engine stores E2 82 AC */
        "var b=e.encode(eu);\n"
        "ok(b.length===3 && b[0]===0xE2 && b[1]===0x82 && b[2]===0xAC,'encode euro -> 3 UTF-8 bytes');\n"
        /* round-trip decode utf-8 */
        "var d=new TextDecoder();\n"
        "ok(d.decode(a)==='Hi!','utf-8 decode round-trips ASCII');\n"
        "ok(d.decode(b)===eu,'utf-8 decode round-trips euro');\n"
        /* decode utf-16le: 'AB' = 41 00 42 00 */
        "var u16=new Uint8Array([0x41,0,0x42,0]);\n"
        "ok(new TextDecoder('utf-16le').decode(u16)==='AB','utf-16le decode');\n"
        /* decode utf-16be: 00 41 00 42 */
        "var u16b=new Uint8Array([0,0x41,0,0x42]);\n"
        "ok(new TextDecoder('utf-16be').decode(u16b)==='AB','utf-16be decode');\n"
        /* utf-16le surrogate pair: U+1F600 = D83D DE00 -> F0 9F 98 80 */
        "var emo=new Uint8Array([0x3D,0xD8,0x00,0xDE]);\n"
        "var ed=new TextDecoder('utf-16le').decode(emo);\n"
        "ok(ed.length===4 && ed.charCodeAt(0)===0xF0,'utf-16 surrogate pair -> 4 UTF-8 bytes');\n"
        /* windows-1252: 0x80 -> euro, 0xE9 -> e-acute */
        "var w=new Uint8Array([0x80,0xE9]);\n"
        "var wd=new TextDecoder('windows-1252').decode(w);\n"
        "ok(wd===String.fromCharCode(0x20AC)+String.fromCharCode(0xE9),'windows-1252 decode');\n"
        /* performance.now monotonic + timeOrigin */
        "var t1=performance.now(), t2=performance.now();\n"
        "ok(typeof t1==='number' && t2>t1,'performance.now monotonic');\n"
        "ok(performance.timeOrigin>0,'performance.timeOrigin present');\n"
    );

    js_ctx_free(ctx);
    printf(g_fail ? "\n=== FAILED ===\n" : "\n=== ALL PASS ===\n");
    return g_fail ? 1 : 0;
}
