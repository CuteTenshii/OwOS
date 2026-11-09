/* Host test for the Streams API: stream semantics, CompressionStream/DecompressionStream, Blob.stream(). */
#define OWOS_SUITE_BINDINGS
#include "dom.h"
#include "jsint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const char page[] = "<html><head><title>t</title></head><body><script>var ready=1</script></body></html>";

/* Helpers shared by the scripts: drain a stream into one Uint8Array / array and report how it ended */
#define HELPERS                                                                                                        \
  "function drain(rs){ var o={chunks:[],done:false,error:null}, r=rs.getReader(), x;\n"                                \
  "  for(;;){ x=null; r.read().then(function(v){ x=v; },function(e){ o.error=e; }); if(!x) break;\n"                   \
  "    if(x.done){ o.done=true; break; } o.chunks.push(x.value); } return o; }\n"                                      \
  "function cat(cs){ var n=0,i,at=0; for(i=0;i<cs.length;i++) n+=cs[i].length; var u=new Uint8Array(n);\n"             \
  "  for(i=0;i<cs.length;i++){ u.set(cs[i],at); at+=cs[i].length; } return u; }\n"                                     \
  "function same(a,b){ if(a.length!==b.length) return false; for(var i=0;i<a.length;i++) if(a[i]!==b[i]) return "      \
  "false; return true; }\n"                                                                                            \
  "function one(u8){ return new ReadableStream({start:function(c){ c.enqueue(u8); c.close(); }}); }\n"                 \
  "function through(rs,t){ return drain(rs.pipeThrough(t)); }\n"

static void test_reading_and_queueing(void)
{
  run_script(HELPERS
             "var rs=new ReadableStream({start:function(c){ c.enqueue(1); c.enqueue(2); c.close(); }});\n"
             "var o=drain(rs); expect(o.done && o.chunks.join()==='1,2', 'chunks then done');\n"
             "expect(rs.locked, 'a reader keeps the stream locked');\n"
             "var i=0, p=drain(new ReadableStream({pull:function(c){ if(i<5000) c.enqueue(i++); else c.close(); }}));\n"
             "expect(p.done && p.chunks.length===5000 && p.chunks[4999]===4999, 'pull-driven source of 5000 chunks');\n"
             "var e=drain(new ReadableStream({start:function(c){ c.enqueue(1); c.error(new Error('boom')); }}));\n"
             "expect(e.error && e.error.message==='boom' && e.chunks.length===0, 'error discards queued chunks');\n"
             "var s2=new ReadableStream({start:function(c){ c.enqueue(1); }}), r2=s2.getReader(), threw=false;\n"
             "try{ s2.getReader(); }catch(x){ threw=x instanceof TypeError; } expect(threw, 'second reader throws');\n"
             "r2.releaseLock(); expect(!s2.locked, 'releaseLock unlocks');\n"
             "var why=null; new ReadableStream({cancel:function(r){ why=r; }}).cancel('stop');\n"
             "expect(why==='stop', 'cancel reaches the source');\n"
             "var d=null; new ReadableStream({start:function(c){ d=c.desiredSize; }}, new "
             "ByteLengthQueuingStrategy({highWaterMark:8}));\n"
             "expect(d===8, 'desiredSize follows the strategy');\n"
             "var f=drain(ReadableStream.from([7,8,9])); expect(f.chunks.join()==='7,8,9', 'ReadableStream.from');\n"
             "var t=new ReadableStream({start:function(c){ c.enqueue(1); c.enqueue(2); c.close(); }}).tee();\n"
             "var a=drain(t[0]), b=drain(t[1]); expect(a.chunks.join()==='1,2' && b.chunks.join()==='1,2' && a.done && "
             "b.done, 'tee');\n");
}

static void test_writing_and_piping(void)
{
  run_script(
      HELPERS
      "var got=[], closed=false;\n"
      "var ws=new WritableStream({write:function(x){ got.push(x); }, close:function(){ closed=true; }});\n"
      "var w=ws.getWriter(); w.write('a'); w.write('b'); w.close();\n"
      "expect(got.join()==='a,b' && closed, 'writes reach the sink in order, then close');\n"
      "var late=null; w.write('c').then(null,function(e){ late=e; }); expect(late instanceof TypeError, 'write after "
      "close rejects');\n"
      "var gate, seen=[], ws2=new WritableStream({write:function(x){ seen.push(x); return new Promise(function(r){ "
      "gate=r; }); }},\n"
      "  {highWaterMark:2}), w2=ws2.getWriter();\n"
      "expect(w2.desiredSize===2, 'desiredSize starts at the high-water mark');\n"
      "w2.write(1); w2.write(2); expect(w2.desiredSize===0, 'queued writes use up desiredSize');\n"
      "var ready=false; w2.ready.then(function(){ ready=true; }); expect(!ready, 'ready waits under backpressure');\n"
      "gate(); expect(ready && seen.length===2, 'ready resolves once the sink drains');\n"
      "var out=[]; var up=new TransformStream({transform:function(x,c){ c.enqueue(x*2); }, flush:function(c){ "
      "c.enqueue(-1); }});\n"
      "var n=0, done=false;\n"
      "new ReadableStream({pull:function(c){ if(n<3000) c.enqueue(n++); else c.close(); }}).pipeThrough(up)\n"
      "  .pipeTo(new WritableStream({write:function(x){ out.push(x); }})).then(function(){ done=true; });\n"
      "expect(done && out.length===3001 && out[2999]===5998 && out[3000]===-1, 'pipeThrough a TransformStream with "
      "flush');\n"
      "var abortReason=null, failed=null;\n"
      "new ReadableStream({start:function(c){ c.error(new Error('src')); }})\n"
      "  .pipeTo(new WritableStream({abort:function(r){ abortReason=r.message; }})).then(null,function(e){ "
      "failed=e.message; });\n"
      "expect(abortReason==='src' && failed==='src', 'a failing source aborts the destination');\n"
      "var cancelled=null;\n"
      "new ReadableStream({pull:function(c){ c.enqueue(1); }, cancel:function(r){ cancelled=r.message; }})\n"
      "  .pipeTo(new WritableStream({write:function(){ throw new Error('sink'); }})).then(null,function(){});\n"
      "expect(cancelled==='sink', 'a failing sink cancels the source');\n"
      "var th=false; try{ new ReadableStream().pipeThrough({readable:1,writable:2}); }catch(e){ th=e instanceof "
      "TypeError; }\n"
      "expect(th, 'pipeThrough validates its argument');\n");
}

static void test_native_core(void)
{
  run_script(
      HELPERS
      "function msg(f){ try { f(); return \"none\"; } catch (e) { return e.name + \":\" + e.message; } }\n"
      "function rejmsg(p){ var m=\"pending\"; p.then(function(){ m=\"ok\"; }, function(e){ m=e.name + \":\" + "
      "e.message; }); return m; }\n"
      "var id=new TransformStream(), w=id.writable.getWriter(); w.write(\"a\"); w.write(\"b\"); w.close();\n"
      "var o=drain(id.readable); expect(o.done && o.chunks.join()===\"a,b\", \"a TransformStream without a transformer "
      "passes chunks through: \"+o.chunks.join());\n"
      "var id2=new TransformStream(null), w2=id2.writable.getWriter(); w2.write(1); w2.close();\n"
      "o=drain(id2.readable); expect(o.done && o.chunks.join()===\"1\", \"a null transformer is an identity too\");\n"
      "class Counting extends ReadableStream { constructor(n){ super({start:function(c){ for(var i=0;i<n;i++) "
      "c.enqueue(i); c.close(); }}); this.n=n; } total(){ return this.n; } }\n"
      "var cs=new Counting(3); o=drain(cs);\n"
      "expect(cs instanceof ReadableStream && cs instanceof Counting && cs.total()===3 && o.chunks.join()===\"0,1,2\", "
      "\"a ReadableStream subclass works\");\n"
      "var wrote=[]; class Sink extends WritableStream { constructor(){ super({write:function(c){ wrote.push(c); }}); "
      "} }\n"
      "var sk=new Sink(), sw=sk.getWriter(); sw.write(\"x\"); expect(sk instanceof WritableStream && "
      "wrote.join()===\"x\", \"a WritableStream subclass works\");\n"
      "class Up extends TransformStream { constructor(){ super({transform:function(c,ct){ ct.enqueue(c.toUpperCase()); "
      "}}); } }\n"
      "var up=new Up(), uw=up.writable.getWriter(); uw.write(\"a\"); uw.close(); o=drain(up.readable);\n"
      "expect(up instanceof Up && o.chunks.join()===\"A\", \"a TransformStream subclass works\");\n"
      "var rp=ReadableStream.prototype, dr=Object.getPrototypeOf(new ReadableStream().getReader()), "
      "wp=Object.getPrototypeOf(new WritableStream().getWriter());\n"
      "expect(msg(function(){ rp.getReader.call({}); })===\"TypeError:Illegal invocation\", \"getReader checks its "
      "receiver\");\n"
      "expect(rejmsg(rp.cancel.call({}))===\"TypeError:Illegal invocation\", \"cancel rejects for a foreign "
      "receiver\");\n"
      "expect(rejmsg(dr.read.call({}))===\"TypeError:Illegal invocation\", \"read rejects for a foreign receiver\");\n"
      "expect(rejmsg(wp.write.call({},1))===\"TypeError:Illegal invocation\", \"write rejects for a foreign "
      "receiver\");\n"
      "expect(msg(function(){ new ReadableStreamDefaultController(); })===\"TypeError:Illegal constructor\", "
      "\"controllers have no public constructor\");\n"
      "expect(msg(function(){ new ReadableStreamDefaultReader({}); })===\"TypeError:Illegal invocation\", \"a reader "
      "needs a stream\");\n"
      "expect(msg(function(){ new WritableStreamDefaultWriter(new ReadableStream()); })===\"TypeError:Illegal "
      "invocation\", \"a writer needs a writable stream\");\n"
      "var churn=function(){ var j; for(var i=0;i<3000;i++) j={a:i,b:[i]}; return j; };\n"
      "var held=[]; for(var i=0;i<40;i++){ (function(i){ var cc; var s=new ReadableStream({start:function(c){ cc=c; "
      "c.enqueue(\"k\"+i); c.enqueue({n:i}); }}); churn(); held.push(s.getReader()); })(i); }\n"
      "churn(); var ok=true;\n"
      "for(i=0;i<40;i++){ var got=[]; held[i].read().then(function(v){ got.push(v.value); }); "
      "held[i].read().then(function(v){ got.push(v.value); }); if(got[0]!==\"k\"+i || !got[1] || got[1].n!==i) "
      "ok=false; }\n"
      "expect(ok, \"queued chunks survive collection\");\n"
      "var sink=[], ws3=new WritableStream({write:function(c){ sink.push(c); churn(); }}), w3=ws3.getWriter();\n"
      "for(i=0;i<50;i++) w3.write({v:i}); expect(sink.length===50 && sink[49].v===49 && sink[0].v===0, \"writes "
      "survive collection\");\n"
      "var tr=new TransformStream({transform:function(c,ct){ churn(); ct.enqueue({t:c}); }}), "
      "tw=tr.writable.getWriter(); for(i=0;i<30;i++) tw.write(i); tw.close();\n"
      "o=drain(tr.readable); expect(o.done && o.chunks.length===30 && o.chunks[29].t===29, \"transformed chunks "
      "survive collection\");\n"
      "var closedWrites=0, closeCalls=0, wsA=new WritableStream({write:function(){ closedWrites++; }, "
      "close:function(){ closeCalls++; }});\n"
      "var done1=false; new ReadableStream({start:function(c){ c.enqueue(1); c.close(); "
      "}}).pipeTo(wsA,{preventClose:true}).then(function(){ done1=true; });\n"
      "expect(done1 && closedWrites===1 && closeCalls===0 && !wsA.locked, \"preventClose keeps the destination open "
      "and unlocks it\");\n"
      "var aborted=false, wsB=new WritableStream({abort:function(){ aborted=true; }}), pa=null;\n"
      "new ReadableStream({start:function(c){ c.error(new Error(\"boom\")); "
      "}}).pipeTo(wsB,{preventAbort:true}).then(null,function(e){ pa=e.message; });\n"
      "expect(pa===\"boom\" && !aborted, \"preventAbort leaves the destination alone when the source fails\");\n"
      "var canceled=false, pc=null;\n"
      "new ReadableStream({pull:function(c){ c.enqueue(1); }, cancel:function(){ canceled=true; }}).pipeTo(new "
      "WritableStream({write:function(){ throw new Error(\"bad\"); }}),{preventCancel:true}).then(null,function(e){ "
      "pc=e.message; });\n"
      "expect(pc===\"bad\" && !canceled, \"preventCancel leaves the source alone when the destination fails\");\n"
      "var ac=new AbortController(), abortSeen=null, cancelSeen=null, res=null;\n"
      "var srcS=new ReadableStream({cancel:function(r){ cancelSeen=r; }}), dstS=new WritableStream({abort:function(r){ "
      "abortSeen=r; }});\n"
      "srcS.pipeTo(dstS,{signal:ac.signal}).then(null,function(e){ res=e; }); ac.abort(\"stop\");\n"
      "expect(res===\"stop\" && abortSeen===\"stop\" && cancelSeen===\"stop\", \"an aborted signal aborts the "
      "destination and cancels the source: \"+res+\" \"+abortSeen+\" \"+cancelSeen);\n"
      "var pre=new AbortController(); pre.abort(\"early\"); var res2=null;\n"
      "new ReadableStream().pipeTo(new WritableStream(),{signal:pre.signal}).then(null,function(e){ res2=e; });\n"
      "expect(res2===\"early\", \"a signal that is already aborted rejects the pipe\");\n"
      "var pulls=0, hw=new ReadableStream({pull:function(c){ pulls++; c.enqueue(pulls); }},{highWaterMark:3});\n"
      "expect(pulls===3, \"pull runs until the queue reaches the high-water mark: \"+pulls);\n"
      "var rr=hw.getReader(); rr.read(); expect(pulls===4, \"reading makes room for one more pull: \"+pulls);\n"
      "var ds=null; new ReadableStream({start:function(c){ c.enqueue(\"ab\"); ds=c; "
      "}},{highWaterMark:5,size:function(x){ return x.length; }});\n"
      "expect(ds.desiredSize===3, \"desiredSize counts chunk sizes: \"+ds.desiredSize);\n"
      "var bytesErr=msg(function(){ new ReadableStream({type:\"bytes\",start:function(c){ c.enqueue(\"str\"); }}); "
      "});\n"
      "expect(bytesErr===\"TypeError:Chunk must be an ArrayBuffer view\", \"a bytes stream refuses chunks that are not "
      "views: \"+bytesErr);\n"
      "var sizeErr=null, wsC=new WritableStream({}, {size:function(){ return -1; }}); var wc=wsC.getWriter(); "
      "wc.write(1).then(null,function(e){ sizeErr=e.name; });\n"
      "expect(sizeErr===\"RangeError\", \"a negative chunk size rejects the write and errors the stream: \"+sizeErr);\n"
      "var sig=null, ctl=null, wsD=new WritableStream({start:function(c){ ctl=c; }}); sig=ctl.signal;\n"
      "expect(sig.aborted===false && ctl.signal===sig, \"the controller signal is stable\");\n"
      "var wd=wsD.getWriter(); wd.abort(\"gone\"); expect(sig.aborted===true && sig.reason===\"gone\", \"aborting "
      "fires the controller signal\");\n"
      "var ready=false; var rsE=new ReadableStream({start:function(c){ c.enqueue(1); c.close(); }}); var "
      "re=rsE.getReader(); re.closed.then(function(){ ready=true; }); expect(!ready, \"closed waits for the queue to "
      "drain\");\n"
      "re.read(); expect(ready, \"closed resolves once the last chunk is read\");\n"
      "re.releaseLock(); expect(rejmsg(re.read())===\"TypeError:Reader has been released\", \"a released reader "
      "rejects reads\");\n"
      "var ts2=new TransformStream({transform:function(c,ct){ ct.terminate(); }}), tw2=ts2.writable.getWriter(); "
      "tw2.write(1); var tr2=drain(ts2.readable);\n"
      "expect(tr2.done, \"terminate closes the readable side\");\n"
      "var tsErr=new TransformStream({transform:function(){ throw new Error(\"tx\"); }}), "
      "twe=tsErr.writable.getWriter(); twe.write(1).then(null,function(){}); var te=drain(tsErr.readable);\n"
      "expect(te.error && te.error.message===\"tx\", \"a throwing transform errors the readable side\");\n"
      "var gate, wsF=new WritableStream({write:function(){ return new Promise(function(r){ gate=r; }); "
      "}},{highWaterMark:5}), wf=wsF.getWriter();\n"
      "var r1=\"pending\", r2=\"pending\", r3=\"pending\";\n"
      "wf.write(1).then(function(){ r1=\"ok\"; },function(e){ r1=e; });\n"
      "wf.write(2).then(function(){ r2=\"ok\"; },function(e){ r2=e; });\n"
      "wf.write(3).then(function(){ r3=\"ok\"; },function(e){ r3=e; });\n"
      "wf.abort(\"kill\");\n"
      "expect(r1===\"pending\" && r2===\"kill\" && r3===\"kill\", \"aborting rejects the queued writes at once: "
      "\"+r1+\" \"+r2+\" \"+r3);\n"
      "gate();\n"
      "expect(r1===\"ok\", \"the write in flight still completes: \"+r1);\n");
}

/* gzip, zlib and raw DEFLATE of 3 x "hello hello hello hello streams\n", produced with the reference zlib */
#define FIXTURES                                                                                                       \
  "var GZ=new "                                                                                                        \
  "Uint8Array([31,139,8,0,0,0,0,0,0,255,203,72,205,201,201,87,200,192,32,139,75,138,82,19,115,139,185,48,101,72,147,"  \
  "7,0,255,69,44,176,96,0,0,0]);\n"                                                                                    \
  "var ZL=new "                                                                                                        \
  "Uint8Array([120,156,203,72,205,201,201,87,200,192,32,139,75,138,82,19,115,139,185,48,101,72,147,7,0,197,58,35,140]" \
  ");\n"                                                                                                               \
  "var RAW=new Uint8Array([203,72,205,201,201,87,200,192,32,139,75,138,82,19,115,139,185,48,101,72,147,7,0]);\n"       \
  "var TEXT=new TextEncoder().encode('hello hello hello hello streams\\nhello hello hello hello streams\\nhello "      \
  "hello hello hello streams\\n');\n"

static void test_decompression(void)
{
  run_script(HELPERS FIXTURES
             "expect(same(through(one(GZ),new DecompressionStream('gzip')).chunks[0],TEXT), 'gzip from the reference "
             "encoder');\n"
             "expect(same(through(one(ZL),new DecompressionStream('deflate')).chunks[0],TEXT), 'zlib from the "
             "reference encoder');\n"
             "expect(same(through(one(RAW),new DecompressionStream('deflate-raw')).chunks[0],TEXT), 'raw deflate from "
             "the reference encoder');\n"
             "var split=new ReadableStream({start:function(c){ for(var i=0;i<GZ.length;i+=7) "
             "c.enqueue(GZ.slice(i,i+7)); c.close(); }});\n"
             "expect(same(cat(through(split,new DecompressionStream('gzip')).chunks),TEXT), 'input split into 7-byte "
             "chunks');\n"
             "var bad=GZ.slice(); bad[bad.length-8]^=1;\n"
             "var o=through(one(bad),new DecompressionStream('gzip')); expect(o.error instanceof TypeError, 'bad CRC "
             "is a TypeError');\n"
             "o=through(one(GZ.slice(0,20)),new DecompressionStream('gzip')); expect(o.error instanceof TypeError, "
             "'truncated gzip is a TypeError');\n"
             "o=through(one(cat([GZ,new Uint8Array([0])])),new DecompressionStream('gzip')); expect(o.error instanceof "
             "TypeError, 'trailing byte is a TypeError');\n"
             "o=through(one(ZL),new DecompressionStream('gzip')); expect(o.error instanceof TypeError, 'wrong "
             "container is a TypeError');\n"
             "bad=ZL.slice(); bad[bad.length-1]^=1; o=through(one(bad),new DecompressionStream('deflate')); "
             "expect(o.error instanceof TypeError, 'bad Adler-32 is a TypeError');\n"
             "o=through(new ReadableStream({start:function(c){ c.close(); }}),new DecompressionStream('deflate-raw')); "
             "expect(o.error instanceof TypeError, 'empty input is a TypeError');\n"
             "o=through(one(RAW.slice(0,10)),new DecompressionStream('deflate-raw')); expect(o.error instanceof "
             "TypeError, 'truncated raw deflate is a TypeError');\n"
             "var th=false; try{ new DecompressionStream('br'); }catch(e){ th=e instanceof TypeError; } expect(th, "
             "'unknown format rejected');\n"
             "o=through(one('text'),new DecompressionStream('gzip')); expect(o.error instanceof TypeError, 'a string "
             "chunk is a TypeError');\n"
             "expect(typeof DecompressionStream==='function' && 'readable' in DecompressionStream.prototype && "
             "'writable' in DecompressionStream.prototype, 'interface shape');\n");
}

static void test_compression_round_trip(void)
{
  run_script(
      HELPERS FIXTURES
      "var formats=['gzip','deflate','deflate-raw'], i;\n"
      "var big=new Uint8Array(200000); for(i=0;i<big.length;i++) big[i]=(i*7+(i>>8)*13+(i%251))&255;\n"
      "for(i=0;i<formats.length;i++){\n"
      "  var f=formats[i];\n"
      "  var c=through(one(TEXT),new CompressionStream(f));\n"
      "  expect(c.done && c.chunks.length===1 && c.chunks[0].length<TEXT.length, f+' compresses repetitive text');\n"
      "  var d=through(one(c.chunks[0]),new DecompressionStream(f)); expect(same(d.chunks[0],TEXT), f+' round trip');\n"
      "  var parts=new ReadableStream({start:function(ctl){ ctl.enqueue(big.subarray(0,70000)); "
      "ctl.enqueue(big.subarray(70000)); ctl.close(); }});\n"
      "  var rt=drain(parts.pipeThrough(new CompressionStream(f)).pipeThrough(new DecompressionStream(f)));\n"
      "  expect(rt.done && same(cat(rt.chunks),big), f+' round trip of 200000 bytes in two chunks');\n"
      "}\n"
      "var g=through(one(TEXT),new CompressionStream('gzip')).chunks[0];\n"
      "expect(g[0]===31 && g[1]===139 && g[2]===8, 'gzip magic');\n"
      "var z=through(one(TEXT),new CompressionStream('deflate')).chunks[0];\n"
      "expect(z[0]===120 && ((z[0]<<8)|z[1])%31===0, 'zlib header');\n"
      "var e=through(new ReadableStream({start:function(c){ c.close(); }}),new CompressionStream('gzip'));\n"
      "var back=through(one(e.chunks[0]),new DecompressionStream('gzip'));\n"
      "expect(e.done && back.done && back.chunks.length===0, 'empty input round trip');\n");
}

static void test_blob_and_text_streams(void)
{
  run_script(HELPERS FIXTURES "var blob=new Blob([TEXT]); var s=blob.stream(); expect(s instanceof ReadableStream, "
                              "'Blob.stream returns a ReadableStream');\n"
                              "expect(same(cat(drain(blob.stream()).chunks),TEXT), 'Blob.stream yields the bytes');\n"
                              "expect(same(cat(through(new Blob([GZ]).stream(),new "
                              "DecompressionStream('gzip')).chunks),TEXT), 'Blob.stream into DecompressionStream');\n"
                              "expect(drain(new Blob([]).stream()).chunks.length===0, 'empty Blob streams nothing');\n"
                              "var enc=through(new ReadableStream({start:function(c){ c.enqueue('he'); c.enqueue('llo "
                              "\\u20ac'); c.enqueue('\\ud83d\\ude00!'); c.close(); }}),new TextEncoderStream());\n"
                              "expect(same(cat(enc.chunks),new TextEncoder().encode('hello \\u20ac\\ud83d\\ude00!')), "
                              "'TextEncoderStream encodes each chunk');\n"
                              "var bytes=new TextEncoder().encode('h\\u00e9llo \\u20ac'), pieces=[];\n"
                              "for(var i=0;i<bytes.length;i++) pieces.push(bytes.slice(i,i+1));\n"
                              "var dec=through(new ReadableStream({start:function(c){ for(var j=0;j<pieces.length;j++) "
                              "c.enqueue(pieces[j]); c.close(); }}),new TextDecoderStream());\n"
                              "expect(dec.chunks.join('')==='h\\u00e9llo \\u20ac', 'TextDecoderStream reassembles "
                              "split UTF-8: '+dec.chunks.join('|'));\n"
                              "expect(new TextDecoderStream().encoding==='utf-8' && new "
                              "TextEncoderStream().encoding==='utf-8', 'encoding attribute');\n"
                              "var desc=Object.getOwnPropertyDescriptor(window,'ReadableStream'); expect(desc && "
                              "desc.value===ReadableStream, 'globals become data properties once used');\n");
}

static void test_response_bodies(void)
{
  run_script(
      HELPERS FIXTURES
      "var got={};\n"
      "function take(k,p){ p.then(function(v){ got[k]=v; },function(e){ got[k+'Err']=e; }); }\n"
      "var src=function(){ return one(GZ).pipeThrough(new DecompressionStream('gzip')); };\n"
      "var r=new Response(src()); expect(r.body instanceof ReadableStream && r.bodyUsed===false, 'stream body is "
      "exposed');\n"
      "take('buf',r.arrayBuffer()); expect(got.buf instanceof ArrayBuffer && same(new Uint8Array(got.buf),TEXT), "
      "'arrayBuffer drains the stream');\n"
      "expect(r.bodyUsed===true, 'bodyUsed after reading');\n"
      "take('again',r.arrayBuffer()); expect(got.againErr instanceof TypeError, 'a second read fails');\n"
      "take('txt',new Response(src()).text()); expect(got.txt==='hello hello hello hello streams\\nhello hello hello "
      "hello streams\\nhello hello hello hello streams\\n', 'text decodes UTF-8');\n"
      "take('js',new Response(new ReadableStream({start:function(c){ c.enqueue(new TextEncoder().encode('{\"a\":')); "
      "c.enqueue(new TextEncoder().encode('[1,2]}')); c.close(); }})).json());\n"
      "expect(got.js && got.js.a.join()==='1,2', 'json across chunks');\n"
      "take('blob',new Response(one(RAW),{headers:{'content-type':'application/octet-stream'}}).blob());\n"
      "expect(got.blob instanceof Blob && got.blob.size===RAW.length && got.blob.type==='application/octet-stream', "
      "'blob keeps size and type');\n"
      "take('bytes',new Response(new Uint8Array([1,2,3,255])).arrayBuffer());\n"
      "expect(got.bytes.byteLength===4 && new Uint8Array(got.bytes)[3]===255, 'typed array body keeps raw bytes');\n"
      "take('ab',new Response(GZ.buffer).arrayBuffer()); expect(same(new Uint8Array(got.ab),GZ), 'ArrayBuffer body');\n"
      "take('fromblob',new Response(new Blob([TEXT],{type:'text/x'})).blob()); expect(got.fromblob.type==='text/x' && "
      "got.fromblob.size===TEXT.length, 'Blob body keeps its type');\n"
      "take('str',new Response('plain').text()); expect(got.str==='plain', 'string body');\n"
      "var piped=new Response(new Blob([GZ]).stream().pipeThrough(new DecompressionStream('gzip')));\n"
      "take('piped',piped.text()); expect(got.piped.length===TEXT.length, 'Blob.stream into DecompressionStream into "
      "Response');\n"
      "var rb=new Response('abc').body; expect(rb instanceof ReadableStream, 'string body streams');\n"
      "expect(cat(drain(new Response('abc').body).chunks).length===3, 'string body bytes');\n"
      "var bad=new Response(new ReadableStream({start:function(c){ c.enqueue('not bytes'); c.close(); }})); "
      "take('badc',bad.arrayBuffer());\n"
      "expect(got.badcErr instanceof TypeError, 'non-byte chunks reject');\n");
}

int main(void)
{
  B = calloc(1, sizeof *B);
  B->pg = calloc(1, 1 << 16);
  js_run_document(dom_parse(page, (int)strlen(page)), "https://example.com/p.html", fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

  test("reading, queueing, tee and cancel", test_reading_and_queueing);
  test("writing, backpressure and piping", test_writing_and_piping);
  test("native core: subclasses, brand checks, collection, pipe options", test_native_core);
  test("DecompressionStream", test_decompression);
  test("CompressionStream round trips", test_compression_round_trip);
  test("Blob.stream, TextEncoderStream and TextDecoderStream", test_blob_and_text_streams);
  test("Response bodies from streams and buffers", test_response_bodies);

  return 0;
}
