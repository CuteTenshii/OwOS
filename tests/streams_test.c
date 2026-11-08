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

int main(void)
{
  B = calloc(1, sizeof *B);
  B->pg = calloc(1, 1 << 16);
  js_run_document(dom_parse(page, (int)strlen(page)), "https://example.com/p.html", fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

  test("reading, queueing, tee and cancel", test_reading_and_queueing);
  test("writing, backpressure and piping", test_writing_and_piping);
  test("DecompressionStream", test_decompression);
  test("CompressionStream round trips", test_compression_round_trip);
  test("Blob.stream, TextEncoderStream and TextDecoderStream", test_blob_and_text_streams);

  return 0;
}
