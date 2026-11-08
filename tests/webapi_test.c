/* Host test for the browser's production Web API bindings. */
#define OWOS_SUITE_TEST
#include "jsint.h"
#include <stdio.h>

static void con_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  (void)level;
  (void)s;
  (void)len;
}

static js_value test_expect(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  int condition = argc > 0 && js_to_boolean(args[0]);
  (void)thisv;
  if (!condition) {
    js_value message = argc > 1 ? js_to_string(ctx, args[1]) : js_mk_cstring(ctx, "");
    fprintf(stderr, "webapi assertion failed: %s\n", message.type == JV_STR ? message.u.str->data : "(no message)");
  }
  expect(condition);
  return js_undef();
}

static void run(js_ctx *ctx, const char *source)
{
  js_arena arena;
  js_parse_error error = {0, 0};
  js_node *program;
  js_value result;
  js_arena_init(&arena);
  program = js_parse(source, &arena, &error);
  expect(program != 0);
  if (program)
    expect(js_run(ctx, program, &result) == 0);
  js_arena_free(&arena);
}

static void check_text_and_performance(void)
{
  run(g_ctx, "var e=new TextEncoder();\n"
             "var a=e.encode('Hi!');\n"
             "expect(a instanceof Uint8Array,'encoder type');\n"
             "expect(a.length===3 && a[0]===72 && a[1]===105 && a[2]===33,'ASCII encoding');\n"
             "var eu=String.fromCharCode(0x20AC);\n"
             "var b=e.encode(eu);\n"
             "expect(b.length===3 && b[0]===0xE2 && b[1]===0x82 && b[2]===0xAC,'euro encoding');\n"
             "var d=new TextDecoder();\n"
             "expect(d.decode(a)==='Hi!','ASCII decoding');\n"
             "expect(d.decode(b)===eu,'euro decoding');\n"
             "var u16=new Uint8Array([0x41,0,0x42,0]);\n"
             "expect(new TextDecoder('utf-16le').decode(u16)==='AB','UTF-16LE decoding');\n"
             "var u16b=new Uint8Array([0,0x41,0,0x42]);\n"
             "expect(new TextDecoder('utf-16be').decode(u16b)==='AB','UTF-16BE decoding');\n"
             "var emo=new Uint8Array([0x3D,0xD8,0x00,0xDE]);\n"
             "var ed=new TextDecoder('utf-16le').decode(emo);\n"
             "expect(ed.length===4 && ed.charCodeAt(0)===0xF0,'UTF-16 surrogate decoding');\n"
             "var w=new Uint8Array([0x80,0xE9]);\n"
             "var wd=new TextDecoder('windows-1252').decode(w);\n"
             "expect(wd===String.fromCharCode(0x20AC)+String.fromCharCode(0xE9),'Windows-1252 decoding');\n"
             "var t1=performance.now(), t2=performance.now();\n"
             "expect(typeof t1==='number' && t2>t1,'performance.now');\n"
             "expect(performance.timeOrigin>0,'performance.timeOrigin');\n");
}

int main(void)
{
  g_ctx = js_ctx_new(con_print);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);
  js_webapi_setup(g_ctx, g_ctx->global);

  test("TextEncoder, TextDecoder, and performance APIs", check_text_and_performance);

  js_webapi_teardown();
  js_ctx_free(g_ctx);
  return 0;
}
