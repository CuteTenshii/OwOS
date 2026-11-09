/* Host test for IndexedDB persistence: databases survive a page being replaced, only committed work survives, and a
 * damaged file is ignored. The same origin's file is shared by every page the test loads. */
#define OWOS_SUITE_BINDINGS
#include "dom.h"
#include "features/persist/persist.h"
#include "jsint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int js_idb_pump(void);
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

/* Pump the database events until `condition` holds, for at most three seconds */
static int wait_for(const char *condition)
{
  int i;
  for (i = 0; i < 600; i++) {
    js_idb_pump();
    if (check(condition))
      return 1;
    msleep(5);
  }
  return 0;
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

static void test_databases_survive_a_reload(void)
{
  run_script("var done1=false, errs=[], open=indexedDB.open('app',2);\n"
             "open.onupgradeneeded=function(){ var db=open.result;\n"
             "  var s=db.createObjectStore('items',{keyPath:'id'}); s.createIndex('byName','name',{unique:false});\n"
             "  s.add({id:1,name:'a',data:new Uint8Array([1,2,3]),when:new Date(86400000)});\n"
             "  s.add({id:2,name:'b',nested:{list:[1,2,{x:3}]}});\n"
             "  var log=db.createObjectStore('log',{autoIncrement:true}); log.add('first'); log.add('second');\n"
             "  var kv=db.createObjectStore('kv'); kv.put('num',7); kv.put('str','s'); kv.put('date',new Date(5)); "
             "kv.put('arr',[1,'x']); };\n"
             "open.onsuccess=function(){ open.result.close(); done1=true; };\n"
             "open.onerror=function(){ errs.push('open'); done1=true; };\n");
  expect(wait_for("done1"));
  run_script("expect(errs.length===0, 'created: '+errs.join());\n");
  reload_page();
  run_script(
      "var r2={}, done2=false, o=indexedDB.open('app');\n"
      "o.onupgradeneeded=function(){ r2.upgraded=true; };\n"
      "o.onsuccess=function(){ var db=o.result, names=[], i; r2.version=db.version;\n"
      "  for (i=0;i<db.objectStoreNames.length;i++) names.push(db.objectStoreNames.item(i)); "
      "r2.stores=names.sort().join();\n"
      "  var tx=db.transaction(['items','log','kv'],'readwrite'), items=tx.objectStore('items');\n"
      "  items.getAll().onsuccess=function(e){ var a=e.target.result; r2.n=a.length; "
      "r2.bytes=Array.prototype.join.call(a[0].data); "
      "r2.date=a[0].when.getTime(); r2.nested=a[1].nested.list[2].x; };\n"
      "  items.index('byName').get('b').onsuccess=function(e){ r2.byName=e.target.result.id; };\n"
      "  tx.objectStore('log').add('third').onsuccess=function(e){ r2.nextKey=e.target.result; };\n"
      "  tx.objectStore('kv').getAllKeys().onsuccess=function(e){ r2.kv=e.target.result.map(function(k){ "
      "return k instanceof Date ? 'd'+k.getTime() : Array.isArray(k) ? 'a'+k.join('/') : typeof k+k; }).join(); };\n"
      "  tx.oncomplete=function(){ db.close(); done2=true; }; };\n");
  expect(wait_for("done2"));
  run_script(
      "expect(!r2.upgraded && r2.version===2, 'reopened without an upgrade at the stored version');\n"
      "expect(r2.stores==='items,kv,log', 'stores: '+r2.stores);\n"
      "expect(r2.n===2 && r2.bytes==='1,2,3' && r2.date===86400000 && r2.nested===3, 'values: '+JSON.stringify(r2));\n"
      "expect(r2.byName===2, 'indexes are rebuilt: '+r2.byName);\n"
      "expect(r2.nextKey===3, 'the key generator continues: '+r2.nextKey);\n"
      "expect(r2.kv==='number7,d5,strings,a1/x', 'keys keep their types and order: '+r2.kv);\n");
  reload_page();
  run_script("var r3={}, done3=false, o3=indexedDB.open('app');\n"
             "o3.onsuccess=function(){ var tx=o3.result.transaction('log'); "
             "tx.objectStore('log').getAll().onsuccess=function(e){ "
             "r3.log=e.target.result.join(); }; tx.oncomplete=function(){ o3.result.close(); done3=true; }; };\n");
  expect(wait_for("done3"));
  run_script("expect(r3.log==='first,second,third', 'a commit made in the second page persists: '+r3.log);\n");
}

static void test_only_committed_work_survives(void)
{
  run_script("var ready=false, o=indexedDB.open('draft',1);\n"
             "o.onupgradeneeded=function(){ o.result.createObjectStore('s',{keyPath:'id'}).add({id:1}); };\n"
             "o.onsuccess=function(){ window.db=o.result; ready=true; };\n");
  expect(wait_for("ready"));
  run_script("window.db.transaction('s','readwrite').objectStore('s').add({id:2});\n"); /* never pumped: still open at
                                                                                           the end */
  reload_page();
  run_script(
      "var seen=null, p=indexedDB.open('draft');\n"
      "p.onsuccess=function(){ var t=p.result.transaction('s'); t.objectStore('s').getAllKeys().onsuccess=function(e){ "
      "seen=e.target.result.join(); }; };\n");
  expect(wait_for("seen!==null"));
  run_script("expect(seen==='1', 'an unfinished transaction is dropped with its page: '+seen);\n");
}

static void test_delete_and_listing(void)
{
  run_script("var listed=null, gone=false;\n"
             "indexedDB.databases().then(function(l){ listed=l.map(function(d){ return d.name+':'+d.version; "
             "}).sort().join(); });\n"
             "indexedDB.deleteDatabase('draft').onsuccess=function(){ gone=true; };\n");
  expect(wait_for("gone"));
  run_script("expect(listed==='app:2,draft:1', 'databases() lists what was stored: '+listed);\n");
  reload_page();
  run_script("var after=null, fresh=null;\n"
             "indexedDB.databases().then(function(l){ after=l.map(function(d){ return d.name; }).join(); });\n"
             "var q=indexedDB.open('draft');\n"
             "q.onupgradeneeded=function(e){ fresh=e.oldVersion+'>'+e.newVersion; };\n"
             "q.onsuccess=function(){ q.result.close(); };\n");
  expect(wait_for("fresh!==null"));
  run_script("expect(after==='app', 'a deleted database is gone after a reload: '+after);\n"
             "expect(fresh==='0>1', 'and opens as a new one: '+fresh);\n");
}

static void test_damaged_file_is_ignored(void)
{
  char path[160];
  expect(persist_path("idb", PAGE_URL, path, sizeof path) == 0);
  js_page_end();
  expect(persist_write(path, "not a database", 14) == 0);
  {
    FILE *f = fopen(path, "r+b"); /* flip a byte inside the checked region */
    if (f) {
      fseek(f, 12, SEEK_SET);
      fputc('X', f);
      fclose(f);
    }
  }
  js_run_document(dom_parse(page, (int)strlen(page)), PAGE_URL, fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);
  run_script("var up=null, w=indexedDB.open('app'); w.onupgradeneeded=function(e){ up=e.oldVersion; }; "
             "w.onsuccess=function(){ w.result.close(); };\n");
  expect(wait_for("up!==null"));
  run_script("expect(up===0, 'a damaged file leaves a clean slate: '+up);\n");
}

static void test_opaque_origin_does_not_persist(void)
{
  char path[160];
  expect(persist_path("idb", "about:blank", path, sizeof path) != 0);
  expect(persist_path("idb", "data:text/html,x", path, sizeof path) != 0);
}

int main(void)
{
  B = calloc(1, sizeof *B);
  B->pg = calloc(1, 1 << 16);
  js_run_document(dom_parse(page, (int)strlen(page)), PAGE_URL, fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

  test("databases survive a reload", test_databases_survive_a_reload);
  test("only committed work survives", test_only_committed_work_survives);
  test("databases() and deleteDatabase persist", test_delete_and_listing);
  test("a damaged file is ignored", test_damaged_file_is_ignored);
  test("an opaque origin does not persist", test_opaque_origin_does_not_persist);

  return 0;
}
