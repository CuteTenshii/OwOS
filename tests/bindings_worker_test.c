/* Host test for dedicated Workers, which run on real threads: script load, postMessage both ways, importScripts,
 * timers, shared memory, terminate, errors.
 * Worker sources arrive as data: URLs, which the host stub returns verbatim after the comma. */
#define OWOS_SUITE_BINDINGS
#include "dom.h"
#include "jsint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int js_workers_pump(void);
void msleep(unsigned ms);

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

/* Evaluate a page-side expression as a boolean */
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

/* Workers run on their own threads: pump the page side until `condition` holds, for at most three seconds */
static int wait_for(const char *condition)
{
  int i;
  for (i = 0; i < 600; i++) {
    js_workers_pump();
    if (check(condition))
      return 1;
    msleep(5);
  }
  return 0;
}

/* Let anything still in flight arrive, so a later check can assert that something never did */
static void settle(void)
{
  int i;
  for (i = 0; i < 40; i++) {
    js_workers_pump();
    msleep(5);
  }
}

static void test_round_trip(void)
{
  run_script("var got=[], w=new Worker('data:text/javascript,"
             "var base=10; onmessage=function(e){ postMessage({sum:base+e.data.n, same:self===this}); };');\n"
             "expect(w instanceof Worker, 'instanceof');\n"
             "w.onmessage=function(e){ got.push(e.data); };\n"
             "w.postMessage({n:1}); w.postMessage({n:2});\n"
             "expect(got.length===0, 'delivery is asynchronous');\n");
  expect(wait_for("got.length===2"));
  run_script("expect(got[0].sum===11 && got[1].sum===12, 'order and values preserved');\n"
             "expect(got[0].same===true, 'handlers run with this === self');\n"
             "expect(typeof onmessage==='undefined' || onmessage===null, 'worker onmessage did not leak to window');\n"
             "expect(typeof base==='undefined', 'worker globals stay out of window');\n");
}

static void test_self_listeners_and_clone(void)
{
  run_script("var out=null, payload={a:[1,2]}, w2=new Worker('data:text/javascript,"
             "self.addEventListener(\"message\",function(e){ e.data.a.push(3); self.postMessage(e.data); });');\n"
             "w2.addEventListener('message',function(e){ out=e.data; });\n"
             "w2.postMessage(payload);\n");
  expect(wait_for("out!==null"));
  run_script("expect(out.a.length===3, 'listener reply');\n"
             "expect(payload.a.length===2, 'message is cloned, not shared');\n"
             "var dce=''; try{ w2.postMessage(function(){}); }catch(e){ dce=e.name; }\n"
             "expect(dce==='DataCloneError', 'functions cannot be posted: '+dce);\n");
}

static void test_terminate_and_close(void)
{
  run_script("var n1=0, n2=0, t=new Worker('data:text/javascript,onmessage=function(){postMessage(1)};'),\n"
             "c=new Worker('data:text/javascript,onmessage=function(){postMessage(1);close();};');\n"
             "t.onmessage=function(){n1++}; c.onmessage=function(){n2++};\n"
             "t.postMessage(0); t.terminate(); c.postMessage(0); c.postMessage(0);\n");
  expect(wait_for("n2>=1"));
  settle();
  run_script("expect(n1===0, 'terminate drops pending traffic');\n"
             "expect(n2===1, 'close() stops the worker: '+n2);\n");
}

static void test_import_scripts_and_location(void)
{
  run_script("var r=null, w3=new Worker('data:text/javascript,"
             "importScripts(\"data:text/javascript,var shared=42; self.fromLib=7;\");"
             "postMessage([shared, typeof location.href, self.name, self.fromLib]);', {name:'nm'});\n"
             "w3.onmessage=function(e){ r=e.data; };\n");
  expect(wait_for("r!==null"));
  run_script(
      "expect(r[0]===42, 'importScripts shares the worker globals');\n"
      "expect(r[1]==='string' && r[2]==='nm', 'location and name');\n"
      "expect(r[3]===7 && typeof fromLib==='undefined' && typeof shared==='undefined', 'nothing leaks to window');\n"
      "expect(self===window, 'window.self is untouched');\n");
}

static void test_module_worker(void)
{
  run_script("var mr=[], mw=new Worker('data:text/javascript,const base=5; onmessage=e=>postMessage(base+e.data);"
             "self.postMessage(\"up\");', {type:'module'});\n"
             "mw.onmessage=function(e){ mr.push(e.data); };\n"
             "mw.postMessage(1);\n");
  expect(wait_for("mr.length===2"));
  run_script("expect(mr[0]==='up' && mr[1]===6, 'module worker replies: '+mr.join());\n"
             "expect(typeof base==='undefined', 'module top level stays out of window');\n");
}

static void test_worker_timers(void)
{
  run_script("var ticks=[], tw=new Worker('data:text/javascript,"
             "var n=0; var iv=setInterval(function(){ postMessage(\"i\"+(++n)); if(n===3) clearInterval(iv); },1);"
             "setTimeout(function(){ postMessage(\"late\"); },30); setTimeout(function(){ postMessage(\"early\"); },1);"
             "var gone=setTimeout(function(){ postMessage(\"cleared\"); },5); clearTimeout(gone);"
             "setTimeout(function(a,b){ postMessage(a+b); },2,\"x\",\"y\");');\n"
             "tw.onmessage=function(e){ ticks.push(e.data); };\n");
  expect(wait_for("ticks.indexOf('late')>=0"));
  run_script("expect(ticks.indexOf('i1')>=0 && ticks.indexOf('i3')>=0 && ticks.indexOf('i4')<0, 'interval ran and "
             "stopped: '+ticks);\n"
             "expect(ticks.indexOf('cleared')<0, 'clearTimeout cancels');\n"
             "expect(ticks.indexOf('xy')>=0, 'timer arguments');\n"
             "expect(ticks.indexOf('early')<ticks.indexOf('late'), 'timers fire in order');\n");
}

static void test_worker_globals(void)
{
  run_script(
      "var gl=null, gw=new Worker('data:text/javascript,"
      "var enc=new TextEncoder().encode(\"h\\u00e9\u20ac\"), dec=new TextDecoder().decode(enc), "
      "fatal=false; try{ new TextDecoder(\"utf-8\",{fatal:true}).decode(new Uint8Array([255])); }catch(e){ fatal=e "
      "instanceof TypeError; }"
      "var parts=new TextDecoder(), "
      "s1=parts.decode(enc.subarray(0,2),{stream:true})+parts.decode(enc.subarray(2),{stream:true});"
      "var ac=new AbortController(), aborted=0; ac.signal.addEventListener(\"abort\",function(){ aborted++; }); "
      "ac.abort(); ac.abort();"
      "var de=new DOMException(\"m\",\"AbortError\"), uu=crypto.randomUUID(), rv=crypto.getRandomValues(new "
      "Uint8Array(8));"
      "var ev=new CustomEvent(\"x\",{detail:5}), heard=0; self.addEventListener(\"x\",function(e){ heard+=e.detail; "
      "},{once:true});"
      "self.dispatchEvent(ev); self.dispatchEvent(ev);"
      "var micro=[]; queueMicrotask(function(){ micro.push(1); }); micro.push(0);"
      "var tgt=new EventTarget(), t2=0; tgt.addEventListener(\"y\",function(){ t2++; }); tgt.dispatchEvent(new "
      "Event(\"y\"));"
      "setTimeout(function(){ postMessage({enc:enc.length, dec:dec, fatal:fatal, s1:s1, aborted:aborted, "
      "reason:ac.signal.reason.name, "
      "domcode:de.code, uuid:uu.length, rnd:rv.length, heard:heard, micro:micro.join(), t2:t2, "
      "inst:[de instanceof Error, ev instanceof Event, self===globalThis, typeof WorkerGlobalScope]}); },1);');\n"
      "gw.onmessage=function(e){ gl=e.data; };\n");
  expect(wait_for("gl!==null"));
  run_script(
      "expect(gl.enc===6 && gl.dec==='h\\u00e9\u20ac', 'TextEncoder and TextDecoder round trip: '+gl.enc+' '+gl.dec);\n"
      "expect(gl.fatal===true, 'fatal decoding throws TypeError');\n"
      "expect(gl.s1==='h\\u00e9\u20ac', 'streaming decode across a split character: '+gl.s1);\n"
      "expect(gl.aborted===1 && gl.reason==='AbortError', 'AbortController aborts once');\n"
      "expect(gl.domcode===20 && gl.uuid===36 && gl.rnd===8, 'DOMException code, randomUUID, getRandomValues');\n"
      "expect(gl.heard===5 && gl.micro==='0,1' && gl.t2===1, 'events, once, microtasks, EventTarget: '+gl.heard+' "
      "'+gl.micro);\n"
      "expect(gl.inst[0] && gl.inst[1] && gl.inst[2], 'instanceof and self');\n");
}

static void test_worker_web_apis(void)
{
  run_script(
      "var wa=null, werr=null, ww2=new Worker('data:text/javascript,"
      "var u=new URL(\"../a/b.html?x=1&y=%20z#frag\",\"https://User:pw@Example.com:8443/dir/p.html\");"
      "var u2=new URL(\"https://example.com:443/\"); u.searchParams.append(\"q\",\"a b&c\"); "
      "var sp=new URLSearchParams(\"a=1&b=2&a=3\"); sp.set(\"b\",\"x y\"); sp.sort();"
      "var bad=false; try{ new URL(\"nonsense\"); }catch(e){ bad=e instanceof TypeError; }"
      "var b=new Blob([\"ab\",new Uint8Array([99,100]),new Blob([\"e\"])],{type:\"Text/Plain\"}), "
      "sl=b.slice(1,4,\"x/y\"), "
      "fd=new FormData(); fd.append(\"k\",\"v\"); fd.append(\"f\",b,\"n.txt\");"
      "var h=new Headers({\"Content-Type\":\"a/b\"}); h.append(\"X-A\",\"1\"); h.append(\"x-a\",\"2\");"
      "var rel=[new URL(\"../x\",\"http://a/b/c/d\").href, new URL(\"?z\",\"http://a/b?y\").href, "
      "new URL(\"#h\",\"http://a/b?y\").href, new URL(\"//e/f\",\"https://a/b\").href, "
      "new URL(\"./g/./h/../i\",\"http://a/b/c\").href, new URL(\"/r\",\"http://a/b/c\").href].join(\" \");"
      "var burl=URL.createObjectURL(b); "
      "Promise.all([b.text(), sl.text(), b.arrayBuffer()]).then(function(r){ postMessage({href:u.href, "
      "origin:u.origin, "
      "host:u.host, user:u.username, path:u.pathname, search:u.search, hash:u.hash, port2:u2.port, href2:u2.href, "
      "sp:sp.toString(), spn:sp.size, bad:bad, text:r[0], slice:r[1], ab:r[2].byteLength, size:b.size, type:b.type, "
      "rel:rel, slt:sl.type, fdk:fd.get(\"k\"), fdf:fd.get(\"f\").name, hct:h.get(\"content-type\"), "
      "hx:h.get(\"X-A\"), "
      "hkeys:Array.from(h.keys()).join(), burl:burl.indexOf(\"blob:\")===0}); });');\n"
      "ww2.onmessage=function(e){ wa=e.data; }; ww2.onerror=function(e){ werr=e.message; };\n");
  expect(wait_for("wa!==null || werr!==null"));
  run_script(
      "expect(werr===null, 'no worker error: '+werr);\n"
      "expect(wa.href==='https://User:pw@example.com:8443/a/b.html?x=1&y=+z&q=a+b%26c#frag', 'URL resolves against its "
      "base: '+wa.href);\n"
      "expect(wa.origin==='https://example.com:8443' && wa.host==='example.com:8443' && wa.user==='User', 'origin, "
      "host, userinfo');\n"
      "expect(wa.path==='/a/b.html' && wa.search.indexOf('q=a+b%26c')>0 && wa.hash==='#frag', 'searchParams update the "
      "URL: '+wa.search);\n"
      "expect(wa.port2==='' && wa.href2==='https://example.com/', 'default port is dropped: '+wa.href2);\n"
      "expect(wa.sp==='a=1&a=3&b=x+y' && wa.spn===3 && wa.bad===true, 'URLSearchParams and invalid URLs: '+wa.sp);\n"
      "expect(wa.text==='abcde' && wa.slice==='bcd' && wa.ab===5 && wa.size===5, 'Blob parts, slice, arrayBuffer');\n"
      "expect(wa.type==='text/plain' && wa.slt==='x/y', 'Blob types');\n"
      "expect(wa.fdk==='v' && wa.fdf==='n.txt', 'FormData wraps a Blob as a File');\n"
      "expect(wa.hct==='a/b' && wa.hx==='1, 2' && wa.hkeys==='content-type,x-a', 'Headers: '+wa.hkeys);\n"
      "expect(wa.burl===true, 'object URLs');\n"
      "expect(wa.rel==='http://a/b/x http://a/b?z http://a/b?y#h https://e/f http://a/b/g/i http://a/r', 'relative "
      "references: '+wa.rel);\n");
}

static void test_transfer_and_collections(void)
{
  run_script("var tr=null, tbuf=new ArrayBuffer(16), tv=new Uint8Array(tbuf); tv[3]=5;\n"
             "var tw=new Worker('data:text/javascript,onmessage=function(e){ var b=e.data.buf, v=new Uint8Array(b);"
             "v[4]=7; postMessage({buf:b, sum:v[3]+v[4], keys:Array.from(e.data.m.keys()), has:e.data.s.has(\"q\"), "
             "re:e.data.re.test(\"AB\")}, [b]); };');\n"
             "tw.onmessage=function(e){ tr=e.data; };\n"
             "tw.postMessage({buf:tbuf, m:new Map([[1,2],[\"k\",3]]), s:new Set([\"q\"]), re:/ab/i}, [tbuf]);\n"
             "expect(tbuf.byteLength===0 && tv.length===0, 'the page buffer is detached by the transfer');\n");
  expect(wait_for("tr!==null"));
  run_script("expect(tr.buf.byteLength===16 && tr.sum===12, 'the worker saw the bytes and sent them back: '+tr.sum);\n"
             "expect(tr.keys.join()==='1,k' && tr.has===true && tr.re===true, 'Map, Set and RegExp arrive');\n");
}

static void test_shared_memory(void)
{
  run_script("var sab=new SharedArrayBuffer(16), ia=new Int32Array(sab), sdone=false, woke=null,\n"
             "sw=new Worker('data:text/javascript,onmessage=function(e){ var a=new Int32Array(e.data);"
             "Atomics.store(a,1,7); for (var i=0;i<1000;i++) Atomics.add(a,0,1); postMessage(\"done\"); };');\n"
             "sw.onmessage=function(){ sdone=true; };\n"
             "sw.postMessage(sab);\n");
  expect(wait_for("sdone"));
  run_script("expect(Atomics.load(ia,0)===1000 && Atomics.load(ia,1)===7, 'worker wrote the page buffer: '+ia[0]);\n"
             "var threw=false; try{ Atomics.wait(ia,2,0,1); }catch(e){ threw=e instanceof TypeError; }\n"
             "expect(threw, 'the page thread cannot block in Atomics.wait');\n"
             "var ww=new Worker('data:text/javascript,onmessage=function(e){ var a=new Int32Array(e.data);"
             "postMessage(Atomics.wait(a,2,0)); };');\n"
             "ww.onmessage=function(e){ woke=e.data; };\n"
             "ww.postMessage(sab);\n");
  msleep(50);
  run_script("Atomics.store(ia,2,1); Atomics.notify(ia,2);\n");
  expect(wait_for("woke!==null"));
  run_script("expect(woke==='ok' || woke==='not-equal', 'a blocked worker is woken by the page: '+woke);\n");
}

/* Leaving the page must not wait for a worker that never returns to its event loop */
static void test_page_end_stops_stuck_workers(void)
{
  struct timespec t0, t1;
  double elapsed_ms;
  run_script(
      "var spin=new Worker('data:text/javascript,try{ while(true){} }catch(e){} finally{ postMessage(1); }'),\n"
      "blocked=new Worker('data:text/javascript,Atomics.wait(new Int32Array(new SharedArrayBuffer(8)),0,0);');\n");
  msleep(50);
  clock_gettime(CLOCK_MONOTONIC, &t0);
  js_page_end();
  clock_gettime(CLOCK_MONOTONIC, &t1);
  elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
  expect(elapsed_ms < 500);
}

static void test_message_channel(void)
{
  run_script("var mc=new MessageChannel(), seen=[], back=[];\n"
             "expect(mc instanceof MessageChannel && mc.port1 instanceof MessagePort, 'instanceof');\n"
             "mc.port2.onmessage=function(e){ seen.push(e.data); expect(e instanceof MessageEvent, 'MessageEvent'); "
             "mc.port2.postMessage('pong'); };\n"
             "mc.port1.addEventListener('message',function(e){ back.push(e.data); });\n"
             "var obj={n:1}; mc.port1.postMessage(obj); mc.port1.postMessage(2); obj.n=9;\n"
             "expect(seen.length===0, 'asynchronous');\n");
  expect(wait_for("back.length>=2"));
  run_script("expect(seen.length===2 && seen[0].n===1 && seen[1]===2, 'ordered, cloned: '+JSON.stringify(seen));\n"
             "expect(back.length===2 && back[0]==='pong', 'reply');\n"
             "mc.port1.close(); mc.port2.postMessage('x'); mc.port1.postMessage('y');\n");
  settle();
  run_script("expect(seen.length===2 && back.length===2, 'closed ports drop traffic');\n"
             "var threw=false; try{ new MessagePort(); }catch(e){ threw=true; } expect(threw, 'MessagePort is not "
             "constructible');\n");
}

static void test_event_interfaces(void)
{
  run_script(
      "var ee=new ErrorEvent('error',{message:'m',filename:'f.js',lineno:3,colno:4,error:7});\n"
      "expect(ee instanceof Event && ee.type==='error' && ee.message==='m' && ee.filename==='f.js', 'ErrorEvent');\n"
      "expect(ee.lineno===3 && ee.colno===4 && ee.error===7, 'ErrorEvent position and error');\n"
      "var me=new MessageEvent('message',{data:{a:1},origin:'o',lastEventId:'i'});\n"
      "expect(me instanceof Event && me.data.a===1 && me.origin==='o' && me.lastEventId==='i', 'MessageEvent');\n"
      "var got=null, bw=new Worker('data:text/javascript,throw 1;');\n"
      "bw.onerror=function(e){ got=e; };\n");
  expect(wait_for("got!==null"));
  run_script("expect(got instanceof ErrorEvent && typeof got.message==='string' && got.filename.indexOf('data:')===0, "
             "'worker error is an ErrorEvent');\n");
}

static void test_errors(void)
{
  run_script("var errs=0, bad=new Worker('data:text/javascript,throw new Error(\"boom\");');\n"
             "bad.onerror=function(e){ errs++; };\n"
             "var syn=new Worker('data:text/javascript,function (((');\n"
             "syn.addEventListener('error',function(){ errs++; });\n"
             "var threw=false; try{ new Worker('http://other.example/w.js'); }catch(e){ threw=true; }\n"
             "expect(threw, 'cross-origin script is rejected');\n");
  expect(wait_for("errs>=2"));
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
  test("module workers", test_module_worker);
  test("worker timers", test_worker_timers);
  test("worker globals: text coding, events, aborts, crypto", test_worker_globals);
  test("worker web APIs: URL, Blob, FormData, Headers", test_worker_web_apis);
  test("transferred buffers and collections", test_transfer_and_collections);
  test("SharedArrayBuffer and Atomics with a worker", test_shared_memory);
  test("MessageChannel and MessagePort", test_message_channel);
  test("ErrorEvent and MessageEvent", test_event_interfaces);
  test("script and construction errors", test_errors);

  test("page end stops workers stuck in a loop or a wait", test_page_end_stops_stuck_workers);
  dom_free(root);
  free(B->pg);
  free(B);
  return 0;
}
