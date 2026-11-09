/* Host test for Cache API persistence: caches survive a page being replaced, changes (puts, deletes, whole caches)
 * persist, and a damaged file is ignored. */
#define OWOS_SUITE_BINDINGS
#include "dom.h"
#include "features/persist/persist.h"
#include "jsint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void js_page_end(void);
void msleep(unsigned ms);

#define PAGE_URL "https://example.com/dir/p.html"

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

static int check(const char *expression)
{
  js_arena *arena = malloc(sizeof *arena);
  js_parse_error error = {0, 0};
  js_node *program;
  js_value result = js_undef();
  int truth = 0;
  js_arena_init(arena);
  program = js_parse(expression, arena, &error);
  if (program && js_run(g_ctx, program, &result) == 0)
    truth = js_to_boolean(result);
  g_ctx->has_exc = 0;
  if (!program || !arenas_push(arena)) {
    js_arena_free(arena);
    free(arena);
  }
  return truth;
}

static void fetch_none(char *const *urls, int n, unsigned char **bodies, int *lens)
{
  (void)urls;
  (void)n;
  (void)bodies;
  (void)lens;
}

static const char page[] = "<html><head><title>t</title></head><body><script>var ready=1</script></body></html>";

/* The page goes away (committed databases are written) and a new one takes its place */
static void reload_page(void)
{
  js_page_end();
  js_run_document(dom_parse(page, (int)strlen(page)), PAGE_URL, fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);
}

static void test_caches_survive_a_reload(void)
{
  run_script(
      "caches.open('v1').then(function(c){ c.put('/a.txt', new "
      "Response('hello',{status:200,headers:{'Content-Type':'text/plain'}})); "
      "c.put('/b.bin', new Response(new Uint8Array([0,1,2,255]))); c.put('/gone', new Response('x')); });\n"
      "caches.open('v2').then(function(c){ c.put('/c', new Response('three',{status:201,statusText:'Created'})); });\n"
      "caches.open('empty');\n");
  reload_page();
  run_script(
      "var r2={};\n"
      "caches.keys().then(function(k){ r2.keys=k.join(); });\n"
      "caches.match('/a.txt').then(function(r){ r2.status=r.status; r2.type=r.headers.get('content-type'); return "
      "r.text(); })"
      ".then(function(t){ r2.text=t; });\n"
      "caches.open('v1').then(function(c){ return c.match('/b.bin'); }).then(function(r){ return r.arrayBuffer(); })"
      ".then(function(b){ r2.bin=Array.prototype.join.call(new Uint8Array(b)); });\n"
      "caches.open('v2').then(function(c){ return c.match('/c'); }).then(function(r){ r2.s3=r.status+r.statusText; "
      "});\n");
  run_script("expect(r2.keys==='v1,v2,empty', 'cache names come back: '+r2.keys);\n"
             "expect(r2.status===200 && r2.type.indexOf('text/plain')===0 && r2.text==='hello', 'a response keeps its "
             "status, type and body: '+JSON.stringify(r2));\n"
             "expect(r2.bin==='0,1,2,255', 'binary bodies are intact: '+r2.bin);\n"
             "expect(r2.s3==='201Created', 'status text: '+r2.s3);\n");
}

/* Each change below is the only one its page makes, so it alone must mark the origin for writing */
static void test_each_kind_of_change_persists(void)
{
  run_script("caches.open('v1').then(function(c){ c.put('/added', new Response('new')); });\n");
  reload_page();
  run_script("var p1=null; caches.match('/added').then(function(r){ p1=r!==undefined; });\n"
             "expect(p1===true, 'a put into an existing cache persists: '+p1);\n"
             "caches.open('v1').then(function(c){ c.delete('/b.bin'); });\n");
  reload_page();
  run_script("var p2=null; caches.match('/b.bin').then(function(r){ p2=r===undefined; });\n"
             "expect(p2===true, 'an entry delete persists: '+p2);\n"
             "caches.delete('v2');\n");
  reload_page();
  run_script("var p3=null; caches.keys().then(function(k){ p3=k.join(); });\n"
             "expect(p3==='v1,empty', 'a cache delete persists: '+p3);\n");
}

static void test_damaged_file_is_ignored(void)
{
  char path[160];
  expect(persist_path("cache", PAGE_URL, path, sizeof path) == 0);
  js_page_end();
  {
    FILE *f = fopen(path, "r+b"); /* flip a byte inside the checked region */
    expect(f != 0);
    if (f) {
      fseek(f, 14, SEEK_SET);
      fputc('X', f);
      fclose(f);
    }
  }
  js_run_document(dom_parse(page, (int)strlen(page)), PAGE_URL, fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);
  run_script("var k=null; caches.keys().then(function(l){ k=l.length; });\n"
             "expect(k===0, 'a damaged file leaves no caches: '+k);\n");
}

int main(void)
{
  B = calloc(1, sizeof *B);
  B->pg = calloc(1, 1 << 16);
  js_run_document(dom_parse(page, (int)strlen(page)), PAGE_URL, fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

  test("caches survive a reload", test_caches_survive_a_reload);
  test("each kind of change persists", test_each_kind_of_change_persists);
  test("a damaged file is ignored", test_damaged_file_is_ignored);

  return 0;
}
