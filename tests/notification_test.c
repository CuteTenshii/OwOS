/* notification_test.c — host harness proving the browser's W3C Notification API
 * (user/apps/gui/browser/js.c) behaves correctly against the real OwOS JS engine
 * (user/lib/js). The native implementations are static inside js.c (coupled to
 * browser.h + notifyd), so — as webapi_test.c does — this re-registers faithful
 * equivalents on a real engine context and drives them through actual JavaScript.
 * The only stub is notify_send (no notifyd on the host); the wrapper properties,
 * permission model, requestPermission Promise+callback, and the pump-driven
 * "show"/"close" events are the real logic verified here.
 *
 *   cc -std=gnu11 -Iscripts -Iuser/lib -Iuser/lib/js -Iuser/lib/math -Iuser/lib/gc \
 *      tests/notification_test.c user/lib/js/lexer.c user/lib/js/parser.c \
 *      user/lib/js/ast.c user/lib/js/value.c user/lib/js/interp.c \
 *      user/lib/js/builtins.c user/lib/js/bigint.c user/lib/math/math.c \
 *      user/lib/math/fmt.c user/lib/gc/gc.c -o /tmp/notification_test \
 *      && /tmp/notification_test
 */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "parser.h"
#include "interp.h"
#include "value.h"
#include "gc.h"

#define NOTIFY_MAX_BUTTONS 3

static js_ctx    *g_ctx;
static js_object *g_notification_proto;
static js_object *g_notifications;
static const char *g_notif_permission = "granted";
static int        g_sent;                 /* # of notify_send() calls (stubbed) */
static char       g_last_title[128];
static char       g_last_body[256];

/* stubbed notifyd client: record what a real notify_send would have posted. */
static void stub_notify_send(const char *title, const char *body) {
    g_sent++;
    snprintf(g_last_title, sizeof g_last_title, "%s", title ? title : "");
    snprintf(g_last_body, sizeof g_last_body, "%s", body ? body : "");
}

/* ---- faithful re-expression of the js.c event plumbing (subset) ---- */

static void copy_str(char *dst, int cap, const char *s) {
    int i = 0;
    if (cap <= 0) return;
    if (s) for (; s[i] && i < cap - 1; i++) dst[i] = s[i];
    dst[i] = '\0';
}
static const char *arg_str(js_ctx *ctx, js_value v) {
    js_value s = js_to_string(ctx, v);
    return s.type == JV_STR ? s.u.str->data : "";
}
static int streqs(const char *a, const char *b) { return a && b && !strcmp(a, b); }

/* A minimal Event object (js.c's make_event carries x/y/init; not needed here). */
static js_value make_event(const char *type) {
    js_object *e = js_new_object(g_ctx, g_ctx->object_proto);
    js_def(e, "type", js_mk_cstring(g_ctx, type), P_ENUMERABLE);
    js_def(e, "target", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(e, "currentTarget", js_null(), P_WRITABLE | P_ENUMERABLE);
    return js_obj_value(e);
}

/* the own "@ev" listener array of `target`, created on demand */
static js_object *listeners_of(js_object *target, int create) {
    js_value v = js_get(g_ctx, js_obj_value(target), "@ev");
    if (v.type == JV_OBJ) return v.u.obj;
    if (!create) return 0;
    {
        js_object *arr = js_new_array(g_ctx, 0);
        js_def(target, "@ev", js_obj_value(arr), 0);
        return arr;
    }
}
static js_value el_add_listener(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_object *list;
    js_object *rec;
    if (t.type != JV_OBJ || argc < 2 || !js_is_callable(args[1])) return js_undef();
    list = listeners_of(t.u.obj, 1);
    rec = js_new_object(ctx, ctx->object_proto);
    js_def(rec, "type", js_mk_cstring(ctx, arg_str(ctx, args[0])), 0);
    js_def(rec, "fn", args[1], 0);
    js_array_push(ctx, list, js_obj_value(rec));
    return js_undef();
}

/* emit_event: on<type> handler property + addEventListener listeners (js.c) */
static void emit_event(js_object *w, const char *type, js_value data) {
    js_value ev;
    js_object *list;
    char on[24];
    unsigned i, n;
    gc_push_scope();
    gc_keep(w);
    ev = make_event(type);
    gc_keep(ev.u.obj);
    js_set(g_ctx, ev, "target", js_obj_value(w));
    js_set(g_ctx, ev, "currentTarget", js_obj_value(w));
    if (data.type != JV_UNDEF) js_def(ev.u.obj, "data", data, P_WRITABLE | P_ENUMERABLE);
    on[0] = 'o'; on[1] = 'n'; copy_str(on + 2, (int)sizeof on - 2, type);
    {
        js_value h = js_get(g_ctx, js_obj_value(w), on);
        if (js_is_callable(h)) {
            js_value a = ev;
            js_call(g_ctx, h, js_obj_value(w), &a, 1);
        }
    }
    list = listeners_of(w, 0);
    n = list ? js_array_len(list) : 0;
    for (i = 0; i < n; i++) {
        js_value e = js_get_index(g_ctx, js_obj_value(list), i);
        js_value ty, fn, a;
        if (e.type != JV_OBJ) continue;
        ty = js_get(g_ctx, e, "type");
        if (!(ty.type == JV_STR && streqs(ty.u.str->data, type))) continue;
        fn = js_get(g_ctx, e, "fn");
        if (!js_is_callable(fn)) continue;
        a = ev;
        js_call(g_ctx, fn, js_obj_value(w), &a, 1);
    }
    gc_pop_scope();
}

/* ---- the Notification wiring itself (verbatim logic from js.c) ---- */

static const char *notif_opt_str(js_ctx *ctx, js_value opts, const char *key, const char *dflt) {
    js_value v;
    if (opts.type != JV_OBJ) return dflt;
    v = js_get(ctx, opts, key);
    if (v.type == JV_UNDEF || v.type == JV_NULL) return dflt;
    return arg_str(ctx, v);
}
static int notif_opt_bool(js_ctx *ctx, js_value opts, const char *key) {
    if (opts.type != JV_OBJ) return 0;
    return js_to_boolean(js_get(ctx, opts, key));
}

static js_value notification_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    js_object *o;
    js_value opts, ov;
    const char *title, *body, *icon, *dir;
    (void)thisv;
    if (argc < 1)
        return js_throw_type(ctx, "Failed to construct 'Notification': 1 argument required, but only 0 present.");
    title = arg_str(ctx, argc > 0 ? args[0] : js_undef());
    opts  = argc > 1 ? args[1] : js_undef();
    body  = notif_opt_str(ctx, opts, "body", "");
    icon  = notif_opt_str(ctx, opts, "icon", "");
    dir   = notif_opt_str(ctx, opts, "dir", "auto");

    gc_push_scope();
    o = (js_object *)gc_keep(js_new_object(ctx, g_notification_proto));
    js_def(o, "title", js_mk_cstring(ctx, title), P_ENUMERABLE);
    js_def(o, "body", js_mk_cstring(ctx, body), P_ENUMERABLE);
    js_def(o, "icon", js_mk_cstring(ctx, icon), P_ENUMERABLE);
    js_def(o, "tag", js_mk_cstring(ctx, notif_opt_str(ctx, opts, "tag", "")), P_ENUMERABLE);
    js_def(o, "lang", js_mk_cstring(ctx, notif_opt_str(ctx, opts, "lang", "")), P_ENUMERABLE);
    js_def(o, "dir", js_mk_cstring(ctx, dir), P_ENUMERABLE);
    js_def(o, "data", opts.type == JV_OBJ ? js_get(ctx, opts, "data") : js_null(), P_ENUMERABLE);
    js_def(o, "requireInteraction", js_bool(notif_opt_bool(ctx, opts, "requireInteraction")), P_ENUMERABLE);
    js_def(o, "silent", js_bool(notif_opt_bool(ctx, opts, "silent")), P_ENUMERABLE);
    js_def(o, "onclick", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(o, "onclose", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(o, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(o, "onshow", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(o, "@shown", js_bool(0), P_WRITABLE);
    js_def(o, "@close_req", js_bool(0), P_WRITABLE);
    js_def(o, "@closed", js_bool(0), P_WRITABLE);

    if (streqs(g_notif_permission, "granted") && title[0])
        stub_notify_send(title, body[0] ? body : 0);
    if (g_notifications) js_array_push(ctx, g_notifications, js_obj_value(o));
    ov = js_obj_value(o);
    gc_pop_scope();
    return ov;
}
static js_value notif_close_m(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)args; (void)argc;
    if (t.type == JV_OBJ) js_set(ctx, t, "@close_req", js_bool(1));
    return js_undef();
}
static js_value notif_get_permission(js_ctx *ctx, js_value t, js_value *args, int argc) {
    (void)t; (void)args; (void)argc;
    return js_mk_cstring(ctx, g_notif_permission);
}
static js_value notif_request_permission(js_ctx *ctx, js_value t, js_value *args, int argc) {
    js_value cb = argc > 0 ? args[0] : js_undef(), perm;
    (void)t;
    perm = js_mk_cstring(ctx, g_notif_permission);
    if (js_is_callable(cb)) {
        js_value a = perm;
        js_call(ctx, cb, js_undef(), &a, 1);
    }
    return js_promise_resolved(ctx, perm);
}
static int notifications_pump(void) {
    int fired = 0;
    unsigned i, n;
    if (!g_ctx || !g_notifications) return 0;
    n = js_array_len(g_notifications);
    for (i = 0; i < n; i++) {
        js_value nv = js_get_index(g_ctx, js_obj_value(g_notifications), i);
        js_object *w;
        if (nv.type != JV_OBJ) continue;
        w = nv.u.obj;
        if (!js_to_boolean(js_get(g_ctx, nv, "@shown"))) {
            js_set(g_ctx, nv, "@shown", js_bool(1));
            emit_event(w, "show", js_undef());
            fired++;
        }
        if (js_to_boolean(js_get(g_ctx, nv, "@close_req")) &&
            !js_to_boolean(js_get(g_ctx, nv, "@closed"))) {
            js_set(g_ctx, nv, "@closed", js_bool(1));
            emit_event(w, "close", js_undef());
            fired++;
        }
    }
    return fired;
}

/* native __pump() so JS can drive the worker's cooperative pump between steps */
static js_value host_pump(js_ctx *ctx, js_value t, js_value *a, int n) {
    (void)t; (void)a; (void)n;
    return js_number(notifications_pump());
}
/* native __promiseVal(p): the settled fulfillment value, or undefined */
static js_value host_promise_val(js_ctx *ctx, js_value t, js_value *a, int n) {
    js_value out;
    (void)t;
    if (n > 0 && js_promise_inspect(ctx, a[0], &out) == 1) return out;
    return js_undef();
}

static void con_print(js_ctx *ctx, int level, const char *s, int len) {
	(void)ctx; (void)level; (void)s; (void)len;
}
static js_value test_expect(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
	(void)thisv;
	expect(argc > 0 && js_to_boolean(args[0]));
	return js_undef();
}
static void run(js_ctx *ctx, const char *src) {
    js_arena a; js_parse_error err = { 0, 0 }; js_node *prog; js_value result;
    js_arena_init(&a);
    prog = js_parse(src, &a, &err);
	if (!prog) { expect(0); js_arena_free(&a); return; }
    if (js_run(ctx, prog, &result) != 0) {
        js_value msg = js_get(ctx, ctx->exc, "message");
        printf("UNCAUGHT: %s\n", msg.type == JV_STR ? msg.u.str->data : "(err)");
		expect(0);
    }
    js_arena_free(&a);
}

int main(void) {
    js_object *nc, *pg;
    g_ctx = js_ctx_new(con_print);
    g_notification_proto = js_new_object(g_ctx, g_ctx->object_proto);
    g_notifications = js_new_array(g_ctx, 0);
    gc_add_root((void **)&g_notification_proto);
    gc_add_root((void **)&g_notifications);

    js_def_native(g_ctx, g_notification_proto, "close", notif_close_m, 0);
    js_def_native(g_ctx, g_notification_proto, "addEventListener", el_add_listener, 3);

    nc = js_new_native(g_ctx, notification_ctor, "Notification", 1);
    js_def(nc, "prototype", js_obj_value(g_notification_proto), 0);
    js_def(g_notification_proto, "constructor", js_obj_value(nc), P_WRITABLE | P_CONFIGURABLE);
    pg = js_new_native(g_ctx, notif_get_permission, "permission", 0);
    js_def_accessor(nc, "permission", pg, 0, P_CONFIGURABLE);
    js_def_native(g_ctx, nc, "requestPermission", notif_request_permission, 1);
    js_def(nc, "maxActions", js_number(NOTIFY_MAX_BUTTONS), P_ENUMERABLE);
    js_def(g_ctx->global, "Notification", js_obj_value(nc), P_WRITABLE | P_CONFIGURABLE);
    js_def_native(g_ctx, g_ctx->global, "__pump", host_pump, 0);
    js_def_native(g_ctx, g_ctx->global, "__promiseVal", host_promise_val, 1);
	js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

	test("Notification API and event lifecycle");
    run(g_ctx,
		"\n"
        /* surface */
        "expect(typeof Notification==='function','Notification is a constructor');\n"
        "expect(Notification.permission==='granted','permission is granted');\n"
        "expect(Notification.maxActions===3,'maxActions === 3');\n"
        "expect(typeof Notification.requestPermission==='function','requestPermission present');\n"
        /* requestPermission: legacy callback + Promise both resolve granted */
        "var cbVal=null;\n"
        "var p=Notification.requestPermission(function(v){ cbVal=v; });\n"
        "expect(cbVal==='granted','legacy callback got granted');\n"
        "expect(typeof p.then==='function','requestPermission returns a thenable');\n"
        "expect(__promiseVal(p)==='granted','requestPermission promise fulfils granted');\n"
        /* construction: title required */
        "var threw=false; try { new Notification(); } catch(e){ threw = e instanceof TypeError; }\n"
        "expect(threw,'no-arg construction throws TypeError');\n"
        /* options mapping + defaults */
        "var n=new Notification('Hello',{body:'World',icon:'/i.png',tag:'t',dir:'rtl',requireInteraction:true});\n"
        "expect(n instanceof Notification,'instance is a Notification');\n"
        "expect(n.title==='Hello','title set');\n"
        "expect(n.body==='World','body set');\n"
        "expect(n.icon==='/i.png','icon set');\n"
        "expect(n.tag==='t','tag set');\n"
        "expect(n.dir==='rtl','dir set');\n"
        "expect(n.requireInteraction===true,'requireInteraction set');\n"
        "expect(n.silent===false,'silent defaults false');\n"
        "var n2=new Notification('Bare');\n"
        "expect(n2.body==='' && n2.dir==='auto' && n2.tag==='','defaults for omitted options');\n"
        /* events fire only after the pump, and exactly once */
        "var shows=0, closes=0;\n"
        "n.onshow=function(){ shows++; };\n"
        "n.addEventListener('show', function(){ shows++; });\n"
        "n.onclose=function(){ closes++; };\n"
        "expect(shows===0,'show has not fired before pump');\n"
        "__pump();\n"
        "expect(shows===2,'show fired onshow + listener once each');\n"
        "__pump();\n"
        "expect(shows===2,'show does not re-fire on a second pump');\n"
        "expect(closes===0,'close not fired until close()');\n"
        "n.close();\n"
        "expect(closes===0,'close() defers the event to the pump');\n"
        "__pump();\n"
        "expect(closes===1,'close fired once after pump');\n"
        "__pump();\n"
        "expect(closes===1,'close does not re-fire');\n"
        /* a show handler creating a new Notification is not lost */
        "var chain=0;\n"
        "var trigger=new Notification('Trigger');\n"
        "trigger.onshow=function(){ chain++; if(chain===1){ var x=new Notification('Child'); x.onshow=function(){ chain++; }; } };\n"
        "__pump();\n"          /* fires Trigger.show (creates Child) */
        "__pump();\n"          /* fires Child.show */
        "expect(chain===2,'notification created inside a show handler still gets its show');\n"
    );

    /* C-side: every granted-permission construction posted to (stubbed) notifyd
     * (Hello, Bare, Trigger, Child = 4); the last was Child. The no-arg attempt
     * threw before posting, so it is not counted. */
	expect(g_sent == 4 && streqs(g_last_title, "Child"));

	js_ctx_free(g_ctx);
	return 0;
}
