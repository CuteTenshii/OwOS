/* Host test for dedicated Workers: script load, async postMessage both ways, importScripts, terminate, errors.
 * Worker sources arrive as data: URLs, which the host stub returns verbatim after the comma. */
#define OWOS_SUITE_BINDINGS
#include "dom.h"
#include "jsint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int js_workers_pump(void);

static js_value test_expect(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  int condition = argc > 0 && js_to_boolean(args[0]);
  (void)thisv;
  if (!condition) {
    js_value message = argc > 1 ? js_to_string(ctx, args[1]) : js_mk_cstring(ctx, "");
    fprintf(stderr, "assertion failed: %s\n", message.type == JV_STR ? message.u.str->data : "(none)");
  }
  expect(condition);
  return js_undef();
}

static void run_script(const char *source)
{
  js_arena *arena = malloc(sizeof *arena);
  js_parse_error error = {0, 0};
  js_node *program;
  js_value result;
  js_arena_init(arena);
  program = js_parse(source, arena, &error);
  expect(program != 0);
  if (program && js_run(g_ctx, program, &result) != 0) {
    js_value message = js_get(g_ctx, g_ctx->exc, "message");
    fprintf(stderr, "uncaught: %s\n", message.type == JV_STR ? message.u.str->data : "(none)");
    expect(0);
    g_ctx->has_exc = 0;
  }
  if (!program || !arenas_push(arena)) {
    js_arena_free(arena);
    free(arena);
  }
}

static void fetch_none(char *const *urls, int n, unsigned char **bodies, int *lens)
{
  (void)urls;
  (void)n;
  (void)bodies;
  (void)lens;
}

static void drain(void)
{
  int i;
  for (i = 0; i < 20; i++)
    js_workers_pump();
}

static void test_round_trip(void)
{
  run_script("var got=[], w=new Worker('data:text/javascript,"
             "var base=10; onmessage=function(e){ postMessage({sum:base+e.data.n, same:self===this}); };');\n"
             "expect(w instanceof Worker, 'instanceof');\n"
             "w.onmessage=function(e){ got.push(e.data); };\n"
             "w.postMessage({n:1}); w.postMessage({n:2});\n"
             "expect(got.length===0, 'delivery is asynchronous');\n");
  drain();
  run_script(
      "expect(got.length===2, 'both replies arrived: '+got.length);\n"
      "expect(got[0].sum===11 && got[1].sum===12, 'order and values preserved');\n"
      "expect(typeof onmessage==='undefined' || onmessage===null, 'worker onmessage did not leak to window');\n");
}

static void test_self_listeners_and_clone(void)
{
  run_script("var out=null, payload={a:[1,2]}, w2=new Worker('data:text/javascript,"
             "self.addEventListener(\"message\",function(e){ e.data.a.push(3); self.postMessage(e.data); });');\n"
             "w2.addEventListener('message',function(e){ out=e.data; });\n"
             "w2.postMessage(payload);\n");
  drain();
  run_script("expect(out && out.a.length===3, 'listener reply');\n"
             "expect(payload.a.length===2, 'message is cloned, not shared');\n");
}

static void test_terminate_and_close(void)
{
  run_script("var n1=0, n2=0, t=new Worker('data:text/javascript,onmessage=function(){postMessage(1)};'),\n"
             "c=new Worker('data:text/javascript,onmessage=function(){postMessage(1);close();};');\n"
             "t.onmessage=function(){n1++}; c.onmessage=function(){n2++};\n"
             "t.postMessage(0); t.terminate(); c.postMessage(0); c.postMessage(0);\n");
  drain();
  run_script("expect(n1===0, 'terminate drops pending traffic');\n"
             "expect(n2===1, 'close() stops the worker: '+n2);\n");
}

static void test_import_scripts_and_location(void)
{
  run_script("var r=null, w3=new Worker('data:text/javascript,"
             "importScripts(\"data:text/javascript,var shared=42;\");"
             "postMessage([shared, typeof location.href, self.name]);', {name:'nm'});\n"
             "w3.onmessage=function(e){ r=e.data; };\n");
  drain();
  run_script("expect(r && r[0]===42, 'importScripts shares globals');\n"
             "expect(r && r[1]==='string' && r[2]==='nm', 'location and name');\n");
}

static void test_errors(void)
{
  run_script("var errs=0, bad=new Worker('data:text/javascript,throw new Error(\"boom\");');\n"
             "bad.onerror=function(e){ errs++; };\n"
             "var syn=new Worker('data:text/javascript,function (((');\n"
             "syn.addEventListener('error',function(){ errs++; });\n"
             "var threw=false; try{ new Worker('http://other.example/w.js'); }catch(e){ threw=true; }\n"
             "expect(threw, 'cross-origin script is rejected');\n");
  drain();
  run_script("expect(errs===2, 'script errors surface as error events: '+errs);\n");
}

static const char page[] = "<html><body><script>var ready=1</script></body></html>";

int main(void)
{
  B = calloc(1, sizeof *B);
  B->pg = calloc(1, 1 << 16);
  struct dom_node *root = dom_parse(page, (int)strlen(page));
  js_run_document(root, "https://example.com/dir/p.html", fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

  test("postMessage round trip and ordering", test_round_trip);
  test("self.addEventListener and structured clone", test_self_listeners_and_clone);
  test("terminate and close", test_terminate_and_close);
  test("importScripts, location and name", test_import_scripts_and_location);
  test("script and construction errors", test_errors);

  js_page_end();
  dom_free(root);
  free(B->pg);
  free(B);
  return 0;
}
