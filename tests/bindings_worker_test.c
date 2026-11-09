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

/* Worker sources too long for a data: URL come from here, through a Blob */
static const char net_worker[] =
    "var out={}, ctl=new AbortController()\n"
    "var "
    "A=fetch(\"https://example.com/echo?x=1\",{method:\"POST\",headers:{\"X-T\":\"1\",\"Content-Type\":\"text/"
    "x\"},body:\"hello\"}).then(function(r){ out.status=r.status; out.ok=r.ok; out.ct=r.headers.get(\"content-type\"); "
    "out.xf=r.headers.get(\"x-fixture\"); out.cookie=r.headers.get(\"set-cookie\"); out.url=r.url; "
    "out.st=r.statusText; return r.text(); }).then(function(t){ out.text=t; })\n"
    "var B=fetch(\"https://example.com/missing\").then(function(r){ out.m=r.status+\"/\"+r.ok+\"/\"+r.statusText; "
    "return r.text(); }).then(function(t){ out.mt=t; })\n"
    "var C=fetch(\"https://example.com/other\").then(function(){ out.bad=\"resolved\"; },function(e){ out.bad=e.name; "
    "})\n"
    "var D=fetch(\"https://example.com/echo\",{signal:ctl.signal}).then(function(){ out.ab=\"resolved\"; "
    "},function(e){ out.ab=e.name; }); ctl.abort()\n"
    "var E=fetch(\"data:text/plain,inline\").then(function(r){ return r.text(); }).then(function(t){ out.data=t; })\n"
    "var F=new Response(JSON.stringify({k:[1,2]}),{status:201,statusText:\"Made\",headers:{\"X-R\":\"1\"}}), "
    "cl=F.clone()\n"
    "var G=F.json().then(function(j){ out.json=j.k.join(); return cl.text(); }).then(function(t){ out.clone=t.length; "
    "out.used=F.bodyUsed; })\n"
    "var H=Response.json({a:1}).text().then(function(t){ out.rj=t; })\n"
    "var rq=new Request(\"https://example.com/echo\",{method:\"put\",body:new URLSearchParams({a:\"b c\"})}); "
    "out.rm=rq.method; out.rct=rq.headers.get(\"content-type\")\n"
    "var x=new XMLHttpRequest(), states=[]; x.onreadystatechange=function(){ states.push(x.readyState); }\n"
    "var I=new Promise(function(res){ x.onload=function(){ "
    "out.xhr=x.status+\":\"+x.responseText.indexOf(\"GET\")+\":\"+x.getResponseHeader(\"X-Fixture\"); res(); "
    "};x.open(\"GET\",\"https://example.com/echo\"); x.send(); })\n"
    "var sx=new XMLHttpRequest(); sx.open(\"GET\",\"https://example.com/missing\",false); sx.send(); "
    "out.sync=sx.status+sx.responseText\n"
    "var J=new Promise(function(res){ var ws=new WebSocket(\"ws://example.com/echo\"), got=[]; "
    "ws.binaryType=\"arraybuffer\";ws.onopen=function(){ out.wsp=ws.protocol; ws.send(\"hi\"); ws.send(new "
    "Uint8Array([7,8])); };ws.onmessage=function(e){ got.push(typeof e.data===\"string\" ? e.data : (e.data instanceof "
    "ArrayBuffer ? new Uint8Array(e.data).join() : \"?\")); if(got.length===2) ws.close(1000); "
    "};ws.onclose=function(e){ out.ws=got.join(\"|\"); out.wsc=e.code+\"/\"+ws.readyState; res(); }; })\n"
    "var K=new Promise(function(res){ var w2=new WebSocket(\"ws://example.com/nothing\"), ev=[]; "
    "w2.onerror=function(){ ev.push(\"e\"); }; w2.onclose=function(){ ev.push(\"c\"); out.wsbad=ev.join(); res(); }; "
    "})\n"
    "var mc=new MessageChannel(), L=new Promise(function(res){ mc.port1.onmessage=function(e){ "
    "out.mc=e.data.n+\":\"+e.data.m.size; res(); }; mc.port2.postMessage({n:1,m:new Map([[1,2]])}); })\n"
    "Promise.all([A,B,C,D,E,G,H,I,J,K,L]).then(function(){ out.states=states.join(); postMessage(out); },function(e){ "
    "postMessage({fail:String(e)}); });\n";

static js_value test_fixture(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, net_worker);
}

static void test_worker_network(void)
{
  run_script("var net=null, nerr=null, nw=new Worker(URL.createObjectURL(new Blob([fixture()])));\n"
             "nw.onmessage=function(e){ net=e.data; }; nw.onerror=function(e){ nerr=e.message; };\n");
  expect(wait_for("net!==null || nerr!==null"));
  run_script(
      "expect(nerr===null && net.fail===undefined, 'no failure: '+nerr+' '+(net&&net.fail));\n"
      "expect(net.status===200 && net.ok===true && net.st==='OK' && net.ct==='text/plain' && net.xf==='yes', 'fetch "
      "response: '+JSON.stringify(net));\n"
      "expect(net.cookie===null, 'Set-Cookie is hidden from scripts');\n"
      "expect(net.text.indexOf('POST https://example.com/echo?x=1 text/x')===0 && net.text.indexOf('[hello]')>0 && "
      "net.text.indexOf('x-t: 1')>0, 'fetch sends method, headers and body: '+net.text);\n"
      "expect(net.url==='https://example.com/echo?x=1', 'response url');\n"
      "expect(net.m==='404/false/Not Found' && net.mt==='nope', 'error statuses resolve: '+net.m);\n"
      "expect(net.bad==='TypeError', 'network failures reject with TypeError: '+net.bad);\n"
      "expect(net.ab==='AbortError', 'abort rejects with AbortError: '+net.ab);\n"
      "expect(net.data==='inline', 'data: URLs');\n"
      "expect(net.json==='1,2' && net.clone===11 && net.used===true, 'Response json, clone, bodyUsed: "
      "'+JSON.stringify([net.json,net.clone,net.used]));\n"
      "expect(net.rj==='{\"a\":1}' && net.rm==='PUT' && net.rct.indexOf('application/x-www-form-urlencoded')===0, "
      "'Response.json and Request');\n"
      "expect(net.xhr==='200:0:yes' && net.states==='1,2,3,4', 'XMLHttpRequest: '+net.xhr+' '+net.states);\n"
      "expect(net.sync==='404nope', 'synchronous XMLHttpRequest: '+net.sync);\n"
      "expect(net.wsp==='echo.v1' && net.ws==='hi|7,8' && net.wsc==='1005/3', 'WebSocket echo: '+net.ws+' '+net.wsc);\n"
      "expect(net.wsbad==='e,c', 'a failed WebSocket fires error then close: '+net.wsbad);\n"
      "expect(net.mc==='1:1', 'MessageChannel inside a worker: '+net.mc);\n");
}

static void test_worker_streams(void)
{
  run_script("var st=null, sw=new Worker('data:text/javascript,"
             "var out={}, done=[]; var rs=new ReadableStream({start:function(c){ c.enqueue(new Uint8Array([104,105])); "
             "c.enqueue(new Uint8Array([33])); c.close(); }});"
             "done.push(new Response(rs).text().then(function(t){ out.fromStream=t; }));"
             "done.push(new Blob([\"streamed\"]).stream().getReader().read().then(function(r){ out.blob=new "
             "TextDecoder().decode(r.value); }));"
             "var tr=new TransformStream({transform:function(chunk,c){ c.enqueue(chunk.toUpperCase()); }}), "
             "w=tr.writable.getWriter(); w.write(\"abc\"); w.close();"
             "done.push(tr.readable.getReader().read().then(function(r){ out.transform=r.value; }));"
             "done.push(new Blob([new Uint8Array(200000)]).stream().getReader().read().then(function(r){ "
             "out.chunk=r.value.length; }));"
             "done.push(fetch(\"data:text/plain,body\").then(function(r){ return r.body.getReader().read(); "
             "}).then(function(r){ out.body=new TextDecoder().decode(r.value); }));"
             "Promise.all(done).then(function(){ out.stream=typeof ReadableStream; postMessage(out); },function(e){ "
             "postMessage({fail:String(e)}); });');\n"
             "sw.onmessage=function(e){ st=e.data; }; sw.onerror=function(e){ st={fail:e.message}; };\n");
  expect(wait_for("st!==null"));
  run_script("expect(st.fail===undefined, 'no failure: '+st.fail);\n"
             "expect(st.fromStream==='hi!', 'a Response built from a ReadableStream reads back: '+st.fromStream);\n"
             "expect(st.blob==='streamed' && st.transform==='ABC' && st.body==='body', 'Blob.stream, TransformStream, "
             "Response.body: '+JSON.stringify(st));\n"
             "expect(st.stream==='function', 'the stream classes are globals');\n"
             "expect(st.chunk===200000, 'a stream over bytes in memory delivers them as one chunk: '+st.chunk);\n");
}

static void test_worker_misc_apis(void)
{
  run_script("var mi=null, mw=new Worker('data:text/javascript,var out={}; "
             "performance.mark(\"a\"); var m=performance.measure(\"m\",\"a\"); "
             "out.perf=m.entryType+\":\"+(m.duration>=0)+\":\"+performance.getEntriesByName(\"a\").length+\":\"+("
             "performance.timeOrigin>0); "
             "var errs=[]; self.addEventListener(\"error\",function(e){ errs.push(\"listener:\"+e.message); });"
             "self.onerror=function(msg){ errs.push(\"onerror:\"+msg); return true; };"
             "setTimeout(function(){ throw new Error(\"boom\"); },1);"
             "var fr=new FileReader(), seq=[]; fr.onloadend=function(){ out.fr=fr.result+\"|\"+seq.join(); };"
             "fr.onload=function(){ seq.push(\"load\"); }; fr.onloadstart=function(){ seq.push(\"start\"); }; "
             "fr.readAsText(new Blob([\"fr text\"]));"
             "var fr2=new FileReader(); fr2.onload=function(){ out.url=fr2.result; }; fr2.readAsDataURL(new "
             "Blob([\"hi\"],{type:\"text/plain\"}));"
             "setTimeout(function(){ out.errs=errs.join(); postMessage(out); },40);');\n"
             "var seenErr=0; mw.onmessage=function(e){ mi=e.data; }; mw.onerror=function(e){ seenErr++; };\n");
  expect(wait_for("mi!==null"));
  run_script(
      "expect(mi.perf==='measure:true:1:true', 'performance marks and measures: '+mi.perf);\n"
      "expect(mi.errs==='onerror:boom,listener:boom', 'uncaught errors reach self.onerror and listeners: '+mi.errs);\n"
      "expect(seenErr===0, 'a handled error does not reach the page: '+seenErr);\n"
      "expect(mi.fr==='fr text|start,load', 'FileReader readAsText and its events: '+mi.fr);\n"
      "expect(mi.url==='data:text/plain;base64,aGk=', 'FileReader readAsDataURL: '+mi.url);\n");
}

static void test_ports_cross(void)
{
  run_script(
      "var got1=[], got2=[], pw=new Worker('data:text/javascript,onmessage=function(e){ if (e.data.port) { var "
      "p=e.data.port; p.onmessage=function(m){ p.postMessage(\"echo:\"+m.data); }; "
      "var mc=new MessageChannel(); mc.port1.onmessage=function(m){ mc.port1.postMessage(m.data+\"!\"); }; "
      "postMessage({back:mc.port2}); } };');\n"
      "var ch=new MessageChannel(); ch.port1.onmessage=function(m){ got1.push(m.data); };\n"
      "pw.onmessage=function(e){ var bp=e.data.back; bp.onmessage=function(m){ got2.push(m.data); }; "
      "bp.postMessage(\"ping\"); };\n"
      "pw.postMessage({port:ch.port2},[ch.port2]); ch.port1.postMessage(\"hi\"); ch.port1.postMessage(\"there\");\n");
  expect(wait_for("got1.length>=2 && got2.length>=1"));
  run_script(
      "expect(got1.join()==='echo:hi,echo:there', 'a port moved into the worker answers the page: '+got1.join());\n"
      "expect(got2.join()==='ping!', 'a port moved out of the worker answers the worker: '+got2.join());\n"
      "ch.port1.close();\n");
}

static void test_event_ports(void)
{
  run_script(
      "var c1=new MessageChannel(), c2=new MessageChannel(), epp=null, plain=null, moved=[];\n"
      "c1.port2.onmessage=function(e){ if (e.data==='plain') plain=e; else epp=e; };\n"
      "c1.port1.postMessage('plain'); c1.port1.postMessage({p:c2.port2},[c2.port2]);\n"
      "var pw=new Worker('data:text/javascript,var mc=new MessageChannel(), got=[]; mc.port2.onmessage=function(e){ "
      "got.push(e.ports.length+e.data); if (got.length===2) postMessage(got.join()); }; "
      "mc.port1.postMessage(\"a\",[]); var cc=new MessageChannel(); mc.port1.postMessage(\"b\",[cc.port2]); "
      "onmessage=function(e){ postMessage({n:e.ports.length, same:e.ports[0]===e.data.p, back:cc.port1}, [cc.port1]); "
      "};'), wres=null;\n"
      "pw.onmessage=function(e){ if (typeof e.data==='string') wres=e.data; else moved.push(e); };\n"
      "var mine=new MessageChannel(); pw.postMessage({p:mine.port2},[mine.port2]);\n");
  expect(wait_for("epp!==null && plain!==null && moved.length>=1 && wres!==null"));
  run_script("expect(plain.ports.length===0, 'a message without transferred ports has an empty ports array');\n"
             "expect(epp.ports.length===1 && epp.ports[0]===c2.port2 && epp.data.p===c2.port2, 'a transferred page "
             "port arrives in event.ports by identity');\n"
             "expect(wres==='0a,1b', 'ports transferred inside a worker show up in event.ports: '+wres);\n"
             "var m=moved[0]; expect(m.data.n===1 && m.data.same===true, 'the worker saw the page port in event.ports: "
             "'+JSON.stringify(m.data));\n"
             "expect(m.ports.length===1 && m.ports[0]===m.data.back, 'a port sent out of a worker arrives in "
             "event.ports');\n");
}

static void test_nested_workers(void)
{
  run_script(
      "var nres=null, nerr=null, beats=null, ctr=new Int32Array(new SharedArrayBuffer(8));\n"
      "var inner='onmessage=function(e){ postMessage(e.data*2); };';\n"
      "var outer='var w=new Worker(URL.createObjectURL(new Blob([\"'+inner+'\"]))); w.onmessage=function(e){ "
      "postMessage({echo:e.data, ok:e instanceof MessageEvent && e.target===w}); }; "
      "var b=new Worker(URL.createObjectURL(new Blob([\"throw new Error(String.fromCharCode(98,111,111,109));\"]))); "
      "b.onerror=function(e){ postMessage({err:e.message, ev:e instanceof ErrorEvent}); }; "
      "var h=new Worker(URL.createObjectURL(new Blob([\"onmessage=function(e){ var ia=new Int32Array(e.data); "
      "setInterval(function(){ Atomics.add(ia,0,1); },2); };\"]))); h.postMessage(self.ctrBuf); "
      "onmessage=function(e){ if (e.data.buf) { self.ctrBuf=e.data.buf; h.postMessage(e.data.buf); } else "
      "w.postMessage(e.data); };';\n"
      "var ow=new Worker(URL.createObjectURL(new Blob([outer])));\n"
      "ow.onmessage=function(e){ if (e.data.echo!==undefined) nres=e.data; else nerr=e.data; };\n"
      "ow.postMessage({buf:ctr.buffer}); ow.postMessage(21);\n");
  expect(wait_for("nres!==null && nerr!==null && Atomics.load(ctr,0)>3"));
  run_script("expect(nres.echo===42 && nres.ok===true, 'a nested worker answers its parent: '+JSON.stringify(nres));\n"
             "expect(nerr.err.indexOf('boom')>=0 && nerr.ev===true, 'a nested worker error reaches the parent: "
             "'+JSON.stringify(nerr));\n"
             "ow.terminate(); var at=Atomics.load(ctr,0);\n");
  settle();
  run_script("var after1=Atomics.load(ctr,0);\n");
  settle();
  expect(check("Atomics.load(ctr,0)===after1"));
}

static void test_broadcast_channel(void)
{
  run_script(
      "var pgot=[], wgot=null, ctl=new BroadcastChannel('ch'), other=new BroadcastChannel('other'), self2=new "
      "BroadcastChannel('ch');\n"
      "ctl.onmessage=function(e){ pgot.push(e.data); }; other.onmessage=function(){ pgot.push('wrong'); };\n"
      "var src='var a=new BroadcastChannel(\"ch\"), b=new BroadcastChannel(\"ch\"), n=0; "
      "a.onmessage=function(e){ postMessage({from:\"page\", v:e.data.v}); }; "
      "b.onmessage=function(e){ postMessage({from:\"sibling\", v:e.data.v}); }; "
      "onmessage=function(e){ if (e.data==\"go\") a.postMessage({v:\"fromworker\"}); }; postMessage(\"ready\");';\n"
      "var w=new Worker(URL.createObjectURL(new Blob([src]))), wmsgs=[]; w.onmessage=function(e){ wmsgs.push(e.data); "
      "};\n");
  expect(wait_for("wmsgs.length>=1"));
  run_script("var obj={v:'frompage'}; self2.postMessage(obj); obj.v='changed'; w.postMessage('go');\n");
  expect(wait_for("wmsgs.length>=4 && pgot.length>=2"));
  run_script("expect(pgot.length===2 && pgot[0].v==='frompage' && pgot[1].v==='fromworker', 'the page hears its own "
             "other channel and a worker channel, other names stay quiet: '+JSON.stringify(pgot));\n"
             "var got=wmsgs.slice(1).map(function(m){ return m.from+':'+m.v; }).sort().join();\n"
             "expect(got==='page:frompage,sibling:frompage,sibling:fromworker', 'workers hear the page and each other, "
             "not themselves: '+got);\n"
             "expect(got.indexOf('changed')<0, 'a message is copied when it is posted');\n");
}

static void test_worker_url_edges(void)
{
  run_script("var ue=null, uw=new Worker('data:text/javascript,var r=[]; function U(a,b){ try { return new "
             "URL(a,b).href; } catch(e) { return \"ERR\"; } }"
             "r.push(U(\"http://[::1]:8080/p\"), U(\"HTTP://EXAMPLE.com:80/A b/%41?q=\\u00e9 x#f g\"), "
             "U(\"mailto:a@b.c?subject=hi\"), U(\"data:text/plain,hi\"),"
             "U(\"https://h\"), U(\"/a/../b/./c/..\",\"http://h/x/y\"), U(\"../../..\",\"http://h/a/b/c/d\"), "
             "U(\"x\"), U(\"\"), "
             "U(\"?\",\"http://h/p?q\"), U(\"#\",\"http://h/p#a\"), U(\"\",\"http://h/p?\"), U(\"http://h/p?#\"), "
             "U(\"//h2/p\",\"https://h/\"), U(\"file:///etc/passwd\"), U(\"http://u:p@h/\"), U(\"  "
             "http://trim.example/ \"), U(\"y\",\"data:text/plain,hi\"));"
             "var e1=new URL(\"http://h/p?q#f\"); e1.search=\"\"; r.push(e1.href); var e2=new URL(\"http://h/p\"); "
             "e2.hash=\"#\"; r.push(e2.href); var u=new URL(\"https://e.com/p\"); u.port=\"8443\"; u.hash=\"x\"; "
             "u.search=\"a=b\"; u.pathname=\"/q "
             "r\"; u.hostname=\"Z.com\"; r.push(u.href);"
             "postMessage(r);');\n"
             "uw.onmessage=function(e){ ue=e.data; };\n");
  expect(wait_for("ue!==null"));
  run_script(
      "var "
      "want=['http://[::1]:8080/p','http://example.com/A%20b/"
      "%41?q=%C3%A9%20x#f%20g','mailto:a@b.c?subject=hi','data:text/plain,hi','https://h/',"
      "'http://h/b/','http://h/','ERR','ERR','http://h/p?','http://h/p#','http://h/p?','http://h/p?#','https://h2/"
      "p','file:///etc/passwd','http://u:p@h/','http://trim.example/"
      "','ERR',"
      "'http://h/p#f','http://h/p#','https://z.com:8443/q%20r?a=b#x'];\n"
      "for (var i=0;i<want.length;i++) expect(ue[i]===want[i], 'URL case '+i+': got '+ue[i]+' want '+want[i]);\n");
}

static void test_message_dispatch(void)
{
  run_script(
      "var md=null, mdw=new Worker('data:text/javascript,var log=[]; onmessage=function(e){ "
      "log.push(\"on:\"+e.data+\":\"+(e.target===self)+(e instanceof MessageEvent)+e.ports.length); };"
      "addEventListener(\"message\",function(e){ log.push(\"a\"); addEventListener(\"message\",function(){ "
      "log.push(\"late\"); }); });"
      "addEventListener(\"message\",function(e){ log.push(\"once\"); },{once:true});"
      "addEventListener(\"message\",{handleEvent:function(e){ log.push(\"obj\"); e.stopImmediatePropagation(); }});"
      "addEventListener(\"message\",function(e){ log.push(\"never\"); });"
      "setTimeout(function(){ postMessage(log.join()); },60);');\n"
      "mdw.onmessage=function(e){ md=e.data; }; mdw.onerror=function(e){ md=\"E:\"+e.message; };\n"
      "mdw.postMessage(1); mdw.postMessage(2);\n");
  expect(wait_for("md!==null"));
  run_script("expect(md==='on:1:truetrue0,a,once,obj,on:2:truetrue0,a,obj', 'handler, listeners, once, handleEvent, "
             "stopImmediatePropagation: '+md);\n");
}

static void test_headers_order(void)
{
  run_script("var hk=null, hw=new Worker('data:text/javascript,var h=new Headers({B:\"1\",a:\"2\"}); var "
             "first=Array.from(h.keys()).join(); h.set(\"c\",\"3\"); h.delete(\"a\"); h.append(\"B\",\"x\");"
             "var bad=[]; [\"\",\"a b\",\"x:y\"].forEach(function(n){ try { h.get(n); bad.push(\"no\"); } catch(e) { "
             "bad.push(e.name); } });"
             "postMessage([first, Array.from(h.keys()).join(), h.get(\"b\"), Array.from(h.entries()).join(\";\"), "
             "bad.join()]);');\n"
             "hw.onmessage=function(e){ hk=e.data; };\n");
  expect(wait_for("hk!==null"));
  run_script("expect(hk.join('|')==='a,b|b,c|1, x|b,1, x;c,3|TypeError,TypeError,TypeError', 'Headers stay sorted "
             "after changes: '+hk.join('|'));\n");
}

static void test_blob_crosses(void)
{
  run_script("var bl=null, bw=new Worker('data:text/javascript,onmessage=function(e){ var b=e.data; "
             "b.text().then(function(t){ "
             "postMessage({echo:new Blob([t+\\\"!\\\"],{type:\\\"text/x\\\"}), size:b.size, type:b.type, isBlob:b "
             "instanceof Blob}); }); };');\n"
             "bw.onmessage=function(e){ bl=e.data; };\n"
             "bw.postMessage(new Blob([\"abc\"],{type:\"text/plain\"}));\n");
  expect(wait_for("bl!==null"));
  run_script("expect(bl.size===3 && bl.type==='text/plain' && bl.isBlob===true, 'a page Blob arrives as a Blob in the "
             "worker');\n"
             "expect(bl.echo instanceof Blob && bl.echo.size===4 && bl.echo.type==='text/x', 'and a worker Blob as a "
             "page Blob');\n");
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

/* Free the page's worker slots between tests */
static void terminate_workers(void)
{
  run_script("__workers.forEach(function(w){ w.terminate(); }); __workers.length=0;\n");
}

static void worker_test(const char *name, void (*fn)(void))
{
  test(name, fn);
  terminate_workers();
}

static const char page[] = "<html><body><script>var ready=1</script></body></html>";

int main(void)
{
  B = calloc(1, sizeof *B);
  B->pg = calloc(1, 1 << 16);
  struct dom_node *root = dom_parse(page, (int)strlen(page));
  js_run_document(root, "https://example.com/dir/p.html", fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);
  js_def_native(g_ctx, g_ctx->global, "fixture", test_fixture, 0);
  run_script("var __workers=[]; (function(){ var W=Worker; Worker=function(a,b){ var w=new W(a,b); __workers.push(w); "
             "return w; };"
             "Worker.prototype=W.prototype; })();\n");

  worker_test("postMessage round trip and ordering", test_round_trip);
  worker_test("self.addEventListener and structured clone", test_self_listeners_and_clone);
  worker_test("terminate and close", test_terminate_and_close);
  worker_test("importScripts, location and name", test_import_scripts_and_location);
  worker_test("module workers", test_module_worker);
  worker_test("worker timers", test_worker_timers);
  worker_test("worker globals: text coding, events, aborts, crypto", test_worker_globals);
  worker_test("worker web APIs: URL, Blob, FormData, Headers", test_worker_web_apis);
  worker_test("worker network: fetch, XMLHttpRequest, WebSocket, MessageChannel", test_worker_network);
  worker_test("worker streams", test_worker_streams);
  worker_test("performance marks, error events, FileReader", test_worker_misc_apis);
  worker_test("MessagePorts move between page and worker", test_ports_cross);
  worker_test("URL edge cases", test_worker_url_edges);
  worker_test("message events reach onmessage and every listener in order", test_message_dispatch);
  worker_test("Headers order and name checks", test_headers_order);
  worker_test("Blobs cross between page and worker", test_blob_crosses);
  worker_test("transferred buffers and collections", test_transfer_and_collections);
  worker_test("SharedArrayBuffer and Atomics with a worker", test_shared_memory);
  worker_test("MessageChannel and MessagePort", test_message_channel);
  worker_test("MessageEvent.ports", test_event_ports);
  worker_test("nested workers", test_nested_workers);
  worker_test("BroadcastChannel across the page and workers", test_broadcast_channel);
  worker_test("ErrorEvent and MessageEvent", test_event_interfaces);
  worker_test("script and construction errors", test_errors);

  test("page end stops workers stuck in a loop or a wait", test_page_end_stops_stuck_workers);
  dom_free(root);
  free(B->pg);
  free(B);
  return 0;
}
