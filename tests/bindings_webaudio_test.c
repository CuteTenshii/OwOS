/* Host test for AudioContext, audioWorklet.addModule and AudioWorkletNode. The worklet scope runs on a real thread;
 * its PCM goes to the host sink in bindings_host.c, which counts frames and keeps the loudest sample.
 * Module sources arrive through Blob URLs. */
#define OWOS_SUITE_BINDINGS
#include "dom.h"
#include "jsint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int js_workers_pump(void);
int js_timers_pump(void);
void msleep(unsigned ms);
extern int host_audio_available, host_audio_opens, host_audio_peak;
extern long host_audio_frames;

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

static js_value test_frames(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)ctx;
  (void)thisv;
  (void)args;
  (void)argc;
  return js_number((double)__atomic_load_n(&host_audio_frames, __ATOMIC_RELAXED));
}

static js_value test_peak(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)ctx;
  (void)thisv;
  (void)args;
  (void)argc;
  return js_number(__atomic_load_n(&host_audio_peak, __ATOMIC_RELAXED));
}

static js_value test_reset_peak(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)ctx;
  (void)thisv;
  (void)args;
  (void)argc;
  __atomic_store_n(&host_audio_peak, 0, __ATOMIC_RELAXED);
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

/* The worklet runs on its own thread: pump the page side until `condition` holds, for at most three seconds */
static int wait_for(const char *condition)
{
  int i;
  for (i = 0; i < 600; i++) {
    js_workers_pump();
    js_timers_pump();
    if (check(condition))
      return 1;
    msleep(5);
  }
  return 0;
}

static void settle(void)
{
  int i;
  for (i = 0; i < 40; i++) {
    js_workers_pump();
    msleep(5);
  }
}

static void test_context_surface(void)
{
  run_script(
      "var ac=new AudioContext();\n"
      "expect(ac instanceof AudioContext && ac instanceof BaseAudioContext && ac instanceof EventTarget, 'classes');\n"
      "expect(ac.sampleRate===48000 && ac.state==='running', 'rate and state');\n"
      "expect(ac.destination instanceof AudioDestinationNode && ac.destination instanceof AudioNode, 'destination');\n"
      "expect(ac.destination.numberOfInputs===1 && ac.destination.numberOfOutputs===0 && "
      "ac.destination.maxChannelCount>=2, 'destination shape');\n"
      "expect(ac.destination.context===ac && ac.audioWorklet===ac.audioWorklet, 'identity');\n"
      "expect(typeof webkitAudioContext==='function' && ac instanceof webkitAudioContext, 'prefixed alias');\n"
      "var thrown=[]; try{ new BaseAudioContext(); }catch(e){ thrown.push(e.name); }\n"
      "expect(thrown.join()==='TypeError', 'the abstract class cannot be constructed: '+thrown);\n"
      "var t0=ac.currentTime; expect(ac.currentTime>t0, 'time moves while running');\n"
      "ac.close();\n");
  expect(wait_for("ac.state==='closed'"));
  run_script("var t1=ac.currentTime;\n"
             "expect(ac.currentTime===t1, 'time stops when the context is closed');\n"
             "var rej=null; ac.close().then(null,function(e){ rej=e.name; });\n");
  expect(wait_for("rej==='InvalidStateError'"));
}

static void test_state_changes(void)
{
  run_script("var sc=new AudioContext(), changes=[];\n"
             "sc.onstatechange=function(){ changes.push(sc.state); };\n"
             "sc.addEventListener('statechange',function(){ changes.push('l:'+sc.state); });\n"
             "var done=0; sc.suspend().then(function(){ done++; });\n"
             "expect(sc.state==='suspended', 'suspend takes effect at once');\n");
  expect(wait_for("changes.length>=2 && done===1"));
  run_script("var frozen=sc.currentTime;\n"
             "expect(sc.currentTime===frozen, 'suspended time is frozen');\n"
             "sc.resume();\n");
  expect(wait_for("changes.length>=4"));
  run_script(
      "expect(sc.state==='running' && changes.slice().sort().join()==='l:running,l:suspended,running,suspended', "
      "'statechange fires the handler and listeners once per change: '+changes);\n"
      "sc.close();\n");
  settle();
}

static const char k_helpers[] = "function mod(src){ return URL.createObjectURL(new Blob([src])); }\n";

static void test_worklet_output(void)
{
  run_script(k_helpers);
  run_script("var wc=new AudioContext(), wdone=false;\n"
             "var url=mod('class Dc extends AudioWorkletProcessor{ process(i,o){ o[0][0].fill(0.25); return true; } }"
             "registerProcessor(\"dc\",Dc);');\n"
             "wc.audioWorklet.addModule(url).then(function(){ wdone=true; });\n"
             "var early=null; try{ new AudioWorkletNode(wc,'dc'); }catch(e){ early=e.name; }\n"
             "expect(early==='InvalidStateError', 'a name not yet registered is refused: '+early);\n");
  expect(wait_for("wdone"));
  run_script(
      "var again=wc.audioWorklet.addModule(url), f1=frames();\n"
      "var n=new AudioWorkletNode(wc,'dc');\n"
      "expect(n instanceof AudioWorkletNode && n instanceof AudioNode, 'node classes');\n"
      "expect(n.numberOfInputs===1 && n.numberOfOutputs===1 && n.channelCount===2, 'default shape');\n"
      "expect(n.port && typeof n.port.postMessage==='function', 'port');\n"
      "expect(n.parameters.size===0 && typeof n.parameters.get==='function', 'parameters map');\n"
      "expect(n.connect(wc.destination)===wc.destination, 'connect returns its target');\n"
      "var t=[]; try{ n.connect(wc.destination,1); }catch(e){ t.push(e.name); }\n"
      "try{ n.connect({}); }catch(e){ t.push(e.name); }\n"
      "try{ new AudioWorkletNode(wc,'dc',{numberOfInputs:0,numberOfOutputs:0}); }catch(e){ t.push(e.name); }\n"
      "try{ new AudioWorkletNode(wc,'dc',{outputChannelCount:[1,2]}); }catch(e){ t.push(e.name); }\n"
      "expect(t.join()==='IndexSizeError,TypeError,NotSupportedError,IndexSizeError', 'argument checks: '+t);\n");
  expect(wait_for("frames()>f1+2000"));
  run_script("var peak=peakSample();\n"
             "expect(peak>8100 && peak<8300, 'a constant 0.25 reaches the device as 8191: '+peak);\n"
             "expect(wc.currentTime>0, 'context time moves');\n");
  expect(__atomic_load_n(&host_audio_opens, __ATOMIC_RELAXED) == 1);
  run_script("n.disconnect(); resetPeak();\n");
  settle();
  run_script("var base=frames(); resetPeak();\n");
  settle();
  run_script("expect(peakSample()===0, 'a disconnected node is silent: '+peakSample());\n"
             "expect(frames()>base, 'the device keeps being fed');\n"
             "wc.close();\n");
  settle();
}

static void test_ports_and_options(void)
{
  run_script(k_helpers);
  run_script("var pc=new AudioContext(), pl=[];\n"
             "var purl=mod('registerProcessor(\"echo\", class extends AudioWorkletProcessor{ constructor(o){ super(o); "
             "var self=this; this.opts=o.processorOptions; this.port.onmessage=function(e){ "
             "self.port.postMessage({echo:e.data, "
             "opts:self.opts, rate:sampleRate, nin:o.numberOfInputs, nout:o.numberOfOutputs, "
             "cc:o.outputChannelCount||null}); }; } process(){ return true; } });');\n"
             "var ready=false; pc.audioWorklet.addModule(purl).then(function(){ ready=true; });\n");
  expect(wait_for("ready"));
  run_script("var en=new AudioWorkletNode(pc,'echo',{processorOptions:{k:[1,2]},numberOfInputs:0,numberOfOutputs:2,"
             "outputChannelCount:[1,2]});\n"
             "en.port.onmessage=function(e){ pl.push(e.data); };\n"
             "en.port.postMessage({hello:1});\n");
  expect(wait_for("pl.length===1"));
  run_script("var m=pl[0];\n"
             "expect(m.echo.hello===1 && m.opts.k.join()==='1,2', 'message and processorOptions arrive: "
             "'+JSON.stringify(m));\n"
             "expect(m.rate===48000 && m.nin===0 && m.nout===2 && m.cc.join()==='1,2', 'worklet scope and options: "
             "'+JSON.stringify(m));\n"
             "pc.close();\n");
  settle();
}

static void test_parameters(void)
{
  run_script(k_helpers);
  run_script(
      "var qc=new AudioContext(), ql=[], qready=false;\n"
      "var qurl=mod('registerProcessor(\"amp\", class extends AudioWorkletProcessor{ static get "
      "parameterDescriptors(){ "
      "return "
      "[{name:\"gain\",defaultValue:0.5,minValue:0,maxValue:1},{name:\"k\",defaultValue:2,automationRate:\"k-rate\"}]; "
      "} "
      "constructor(o){ super(o); this.n=0; } process(i,o,p){ o[0][0].fill(p.gain[0]*0.5); if(this.n++%10===0){ "
      "this.port.postMessage({f:currentFrame,len:p.gain.length,g:p.gain[p.gain.length-1],k:p.k.length,kv:p.k[0]}); } "
      "return true; } });');\n"
      "qc.audioWorklet.addModule(qurl).then(function(){ qready=true; });\n");
  expect(wait_for("qready"));
  run_script("var qn=new AudioWorkletNode(qc,'amp',{parameterData:{k:7,unknown:1}});\n"
             "var gp=qn.parameters.get('gain');\n"
             "expect(gp instanceof AudioParam && gp.value===0.5 && gp.defaultValue===0.5 && gp.minValue===0 && "
             "gp.maxValue===1, "
             "'descriptor becomes an AudioParam');\n"
             "expect(qn.parameters.size===2 && qn.parameters.get('k').value===7 && "
             "qn.parameters.get('k').automationRate==='k-rate' "
             "&& !qn.parameters.has('unknown'), 'parameterData and rates');\n"
             "expect(Array.from(qn.parameters.keys()).join()==='gain,k', 'keys in descriptor order');\n"
             "qn.port.onmessage=function(e){ ql.push(e.data); };\n"
             "qn.connect(qc.destination);\n");
  expect(wait_for("ql.length>=1"));
  run_script("expect(ql[0].len===1 && ql[0].g===0.5 && ql[0].k===1 && ql[0].kv===7, 'constant parameters are one "
             "value: '+JSON.stringify(ql[0]));\n"
             "var cnt=ql.length; gp.value=1; ql.length=0;\n");
  expect(wait_for("ql.length>=2"));
  run_script(
      "expect(ql[ql.length-1].g===1, 'the value setter takes effect: '+JSON.stringify(ql[ql.length-1]));\n"
      "gp.setValueAtTime(0, qc.currentTime+0.02); gp.linearRampToValueAtTime(1, qc.currentTime+1); ql.length=0;\n");
  expect(wait_for("ql.length>=10"));
  run_script(
      "var gs=ql.map(function(m){ return m.g; }).filter(function(g){ return g<0.999; }), rising=gs.length>=3;\n"
      "for(var i=1;i<gs.length;i++){ if(gs[i]<gs[i-1]) rising=false; }\n"
      "expect(gs[0]<0.5 && rising, 'a linear ramp from 0 rises: '+gs.join());\n"
      "expect(ql.some(function(m){ return m.len===128; }), 'an automated a-rate parameter has one value per frame');\n"
      "var errs=[]; try{ gp.exponentialRampToValueAtTime(0, 1); }catch(e){ errs.push(e.name); }\n"
      "try{ gp.setValueAtTime(NaN, 0); }catch(e){ errs.push(e.name); }\n"
      "try{ gp.setValueAtTime(1, -1); }catch(e){ errs.push(e.name); }\n"
      "expect(errs.join()==='RangeError,TypeError,RangeError', 'validation: '+errs);\n"
      "qc.close();\n");
  settle();
}

static void test_graph(void)
{
  run_script(k_helpers);
  run_script(
      "var gc=new AudioContext(), gdone=false;\n"
      "var gurl=mod('registerProcessor(\"src\", class extends AudioWorkletProcessor{ process(i,o){ o[0][0].fill(0.5); "
      "return true; } });"
      "registerProcessor(\"half\", class extends AudioWorkletProcessor{ process(i,o){ var a=i[0][0], b=o[0][0]; "
      "if(a){ for(var k=0;k<a.length;k++){ b[k]=a[k]*0.5; } } return true; } });');\n"
      "gc.audioWorklet.addModule(gurl).then(function(){ gdone=true; });\n");
  expect(wait_for("gdone"));
  run_script("var s1=new AudioWorkletNode(gc,'src',{numberOfInputs:0}), h1=new AudioWorkletNode(gc,'half');\n"
             "var h2=new AudioWorkletNode(gc,'half');\n"
             "s1.connect(h1); h1.connect(gc.destination); s1.connect(h2); h2.connect(gc.destination);\n"
             "var f0=frames();\n");
  expect(wait_for("frames()>f0+3000"));
  run_script("resetPeak(); f0=frames();\n");
  expect(wait_for("frames()>f0+3000"));
  run_script("var p1=peakSample();\n"
             "expect(p1>16200 && p1<16500, 'two paths of 0.25 sum to 0.5 at the destination: '+p1);\n"
             "h2.disconnect(gc.destination);\n");
  settle();
  run_script("resetPeak(); f0=frames();\n");
  expect(wait_for("frames()>f0+3000"));
  run_script("var p2=peakSample();\n"
             "expect(p2>8100 && p2<8300, 'disconnect(destination) removes just that path: '+p2);\n"
             "gc.suspend();\n");
  settle();
  run_script("var held=frames();\n");
  settle();
  run_script("expect(frames()===held, 'a suspended context renders nothing');\n"
             "gc.resume();\n");
  expect(wait_for("frames()>held"));
  run_script("gc.close();\n");
  settle();
}

static void test_failures(void)
{
  run_script(k_helpers);
  run_script(
      "var fc=new AudioContext(), fr={};\n"
      "fc.audioWorklet.addModule('https://example.com/dir/missing.js').then(null,function(e){ fr.missing=e.name; });\n"
      "fc.audioWorklet.addModule('https://other.example/w.js').then(null,function(e){ fr.cross=e.name; });\n"
      "fc.audioWorklet.addModule(mod('this is not javascript (((')).then(null,function(e){ fr.syntax=e.name; });\n"
      "fc.audioWorklet.addModule(mod('throw new RangeError(\"x\");')).then(null,function(e){ fr.thrown=e.name; });\n"
      "fc.audioWorklet.addModule(mod('registerProcessor(\"\", class extends AudioWorkletProcessor{process(){}});'))"
      ".then(null,function(e){ fr.empty=e.name; });\n"
      "fc.audioWorklet.addModule(mod('registerProcessor(\"nop\", class {});')).then(null,function(e){ "
      "fr.noproc=e.name; });\n"
      "fc.audioWorklet.addModule(mod('class A extends AudioWorkletProcessor{process(){return true;}} "
      "registerProcessor(\"dup\",A); registerProcessor(\"dup\",A);')).then(null,function(e){ fr.dup=e.name; });\n"
      "fc.audioWorklet.addModule(mod('new AudioWorkletProcessor();')).then(null,function(e){ fr.illegal=e.name; });\n");
  expect(wait_for("Object.keys(fr).length===8"));
  run_script(
      "expect(fr.missing==='AbortError' && fr.cross==='AbortError', 'unloadable modules abort: '+JSON.stringify(fr));\n"
      "expect(fr.syntax==='SyntaxError' && fr.thrown==='RangeError', 'module errors pass through: "
      "'+JSON.stringify(fr));\n"
      "expect(fr.empty==='NotSupportedError' && fr.dup==='NotSupportedError', 'bad names: '+JSON.stringify(fr));\n"
      "expect(fr.noproc==='TypeError' && fr.illegal==='TypeError', 'bad processors: '+JSON.stringify(fr));\n"
      "var pe=[], ew=null;\n"
      "var eurl=mod('registerProcessor(\"bad\", class extends AudioWorkletProcessor{ process(){ throw new "
      "Error(\"nope\"); } });');\n"
      "fc.audioWorklet.addModule(eurl).then(function(){ ew=new AudioWorkletNode(fc,'bad'); "
      "ew.onprocessorerror=function(e){ pe.push(e.message); }; ew.connect(fc.destination); });\n");
  expect(wait_for("pe.length>=1"));
  run_script("expect(pe.length===1 && pe[0]==='nope', 'a throwing process() fires processorerror once: '+pe);\n"
             "fc.close();\n"
             "var after=null; fc.audioWorklet.addModule(eurl).then(null,function(e){ after=e.name; });\n");
  expect(wait_for("after==='InvalidStateError'"));
}

static void test_subclass_and_no_device(void)
{
  __atomic_store_n(&host_audio_available, 0, __ATOMIC_RELAXED);
  run_script(k_helpers);
  run_script("var uc=new AudioContext(), ud=false, up=[];\n"
             "uc.audioWorklet.addModule(mod('registerProcessor(\"t\", class extends AudioWorkletProcessor{ process(){ "
             "this.port.postMessage(1); return false; } });')).then(function(){ ud=true; });\n");
  expect(wait_for("ud"));
  run_script(
      "class Tone extends AudioWorkletNode{ constructor(c){ super(c,'t',{numberOfInputs:0}); this.tag='tone'; } }\n"
      "var tn=new Tone(uc); tn.connect(uc.destination); tn.port.onmessage=function(e){ up.push(e.data); };\n"
      "expect(tn instanceof Tone && tn instanceof AudioWorkletNode && tn.tag==='tone', 'a subclass constructs');\n");
  settle();
  run_script("expect(up.length===1, 'returning false ends a source node: '+up.length);\n"
             "uc.close();\n");
  settle();
  __atomic_store_n(&host_audio_available, 1, __ATOMIC_RELAXED);
}

static const char page[] = "<html><body><script>var ready=1</script></body></html>";

int main(void)
{
  B = calloc(1, sizeof *B);
  B->pg = calloc(1, 1 << 16);
  struct dom_node *root = dom_parse(page, (int)strlen(page));
  js_run_document(root, "https://example.com/dir/p.html", fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);
  js_def_native(g_ctx, g_ctx->global, "frames", test_frames, 0);
  js_def_native(g_ctx, g_ctx->global, "peakSample", test_peak, 0);
  js_def_native(g_ctx, g_ctx->global, "resetPeak", test_reset_peak, 0);

  test("AudioContext surface and clock", test_context_surface);
  test("suspend, resume and statechange", test_state_changes);
  test("a worklet node reaches the audio device", test_worklet_output);
  test("processor ports, options and worklet globals", test_ports_and_options);
  test("AudioParam values and automation", test_parameters);
  test("nodes mix into the destination", test_graph);
  test("addModule and processor failures", test_failures);
  test("subclassing AudioWorkletNode, a source that stops", test_subclass_and_no_device);

  dom_free(root);
  free(B->pg);
  free(B);
  return 0;
}
