/* Engine regression tests: Unicode regexps, array defineProperty, JSON, stack-overflow guards. */
#define OWOS_SUITE_TEST
#include "ast.h"
#include "gc.h"
#include "interp.h"
#include "parser.h"
#include "value.h"
#include <stdio.h>
#include <stdlib.h>

static void con_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  (void)level;
  fprintf(stderr, "[console] %.*s\n", len, s);
}

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

static int can_block; /* run() lets Atomics.wait block */

/* clone(v, transfer): v through js_clone_write and js_clone_read, as a message between two threads would be */
static js_value test_clone(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_clone *c = argc > 0 ? js_clone_write(ctx, args[0], argc > 1 ? args[1] : js_undef()) : NULL;
  js_value v;
  (void)thisv;
  if (!c)
    return js_undef();
  v = js_clone_read(ctx, c);
  js_clone_free(c);
  return v;
}

/* Run `source` in a fresh context; returns nonzero when it ran to completion. */
static int run(const char *source, char *stack_floor)
{
  js_arena arena;
  js_parse_error error = {0, 0};
  js_node *program;
  js_value result;
  int ok = 0;
  js_ctx *ctx = js_ctx_new(con_print);
  ctx->can_block = can_block;
  js_def_native(ctx, ctx->global, "expect", test_expect, 2);
  js_def_native(ctx, ctx->global, "clone", test_clone, 1);
  if (stack_floor) {
    js_set_stack_limit(ctx, stack_floor);
    js_parser_set_stack_floor(stack_floor);
  }
  js_arena_init(&arena);
  program = js_parse(source, &arena, &error);
  if (!program)
    fprintf(stderr, "parse error: %s\n", error.msg ? error.msg : "?");
  else if (js_run(ctx, program, &result) != 0) {
    js_value message = js_get(ctx, ctx->exc, "message");
    fprintf(stderr, "uncaught: %s\n", message.type == JV_STR ? message.u.str->data : "(none)");
  } else
    ok = 1;
  js_arena_free(&arena);
  js_ctx_free(ctx);
  js_parser_set_stack_floor(0);
  return ok;
}

static void check_regexp_code_points(void)
{
  expect(run(
      "expect(!/[\\uD800-\\uDFFF]/.test('1'), 'surrogate class must not match a digit');\n"
      "expect(!/[\\uD800-\\uDFFF]/.test('a\\u{1F600}b'), 'a valid pair is no lone surrogate');\n"
      "expect('%a.b%'.replace(/[^%.]+/g, function(m){ return '<'+m+'>' })==='%<a>.<b>%', 'callback gets the match');\n"
      "expect(/^.$/.test('\\u4e2d') && /^.$/.test('\\u{1F600}'), '. consumes a whole character');\n"
      "expect(/[\\u4e00-\\u9fa5]+/.exec('abc\\u4e2d\\u6587def')[0]==='\\u4e2d\\u6587', 'astral/BMP class range');\n"
      "expect(!/[\\u4e00-\\u9fa5]/.test('a'), 'range misses ASCII');\n"
      "expect(/[^a]/.exec('\\u00e9')[0]==='\\u00e9', 'negated class takes the whole char');\n"
      "expect(/\\u00e9+/.test('\\u00e9\\u00e9'), 'quantifier covers all bytes of a character');\n"
      "expect('\\u00e9\\u00e9\\u00e9'.replace(/\\xe9+/,'x')==='x', '\\\\xe9 escape is U+00E9');\n"
      "expect(/\\s/.test('\\u00a0') && !/\\S/.test('\\u00a0') && /\\S/.test('\\u00e9'), 'unicode spaces');\n"
      "expect(/\\W/.test('\\u00e9') && !/\\w/.test('\\u00e9'), '\\\\w is ASCII only');\n"
      "expect(/[\\s\\S]/.test('\\u00e9') && /[^]/.test('\\n') && !/[]/.test('a'), 'empty and complement classes');\n"
      "expect('\\u65e5\\u672c\\u8a9e'.match(/./g).length===3, 'global match steps by character');\n"
      "expect(/\\u{1F600}/u.test('\\u{1F600}') && /\\ud83d\\ude00/.test('\\u{1F600}'), 'astral escapes');\n"
      "expect('%String.prototype.indexOf%'.match(/"
      "[^%.[\\]]+|\\[(?:(-?\\d+(?:\\.\\d+)?)|([\"'])((?:(?!\\2)[^\\\\]|\\\\.)*?)\\2)\\]|(?=(?:\\.|\\[\\])(?:\\.|\\[\\]|"
      "%$))/g).join('|')==='String|prototype|indexOf', 'lodash path regexp');\n",
      0));
}

static void check_regexp_bounded(void)
{
  char probe;

  expect(run(
      "var r=new RegExp('('.repeat(100000)); expect(r.test('a')===false, 'deep group nesting');\n"
      "var s='ab'.repeat(200000); var t=/^(a|b)*$/.test(s); expect(t===true||t===false, 'long input terminates');\n",
      &probe - 120000));
}

static void check_define_property(void)
{
  expect(run("var a=[]; Object.defineProperty(a,0,{value:'Q',enumerable:true,configurable:true,writable:true});\n"
             "expect(a.length===1 && a[0]==='Q' && 0 in a, 'defines an element');\n"
             "expect(0 in [undefined] && !(1 in [undefined]), 'undefined elements are present');\n"
             "var c=Array.prototype.concat.call(['x'],[1,2]); expect(c.length===3 && c[0]==='x', 'concat');\n",
             0));
  can_block = 1;
  expect(run(
      "var sab=new SharedArrayBuffer(16), i=new Int32Array(sab), b=new BigInt64Array(sab), u8=new Uint8Array(sab);\n"
      "var t=function(f){ try{ f() }catch(e){ return e.constructor.name } };\n"
      "i[2]=1; expect(Atomics.wait(i,2,0,0)==='not-equal' && Atomics.wait(i,2,1,0)==='timed-out', 'wait, no sleep');\n"
      "expect(Atomics.wait(i,2,1,10)==='timed-out', 'wait times out');\n"
      "expect(Atomics.wait(b,0,5n,10)==='not-equal' && Atomics.wait(b,0,0n,10)==='timed-out', 'BigInt64Array wait');\n"
      "expect(t(function(){ Atomics.wait(new Int32Array(4),0,0,0) })==='TypeError' && t(function(){ "
      "Atomics.wait(u8,0,0,0) })==='TypeError', 'wait needs a shared Int32Array');\n",
      0));
  can_block = 0;
}

static void check_json_stringify(void)
{
  expect(run(
      "var c={}; c.self=c; var threw=false; try{ JSON.stringify(c) }catch(e){ threw=e instanceof TypeError }\n"
      "expect(threw, 'circular structure throws TypeError');\n"
      "var arr=[]; arr.push(arr); threw=false; try{ JSON.stringify(arr) }catch(e){ threw=e instanceof TypeError }\n"
      "expect(threw, 'circular array throws TypeError');\n"
      "var shared={n:1}; expect(JSON.stringify({a:shared,b:shared})==='{\"a\":{\"n\":1},\"b\":{\"n\":1}}', 'shared "
      "references are fine');\n"
      "expect(JSON.stringify({a:1,b:2,c:3},['a','c'])==='{\"a\":1,\"c\":3}', 'replacer array');\n"
      "expect(JSON.stringify({a:1,b:'x'},function(k,v){ return typeof v==='number'?v*2:v })==='{\"a\":2,\"b\":\"x\"}', "
      "'replacer function');\n"
      "expect(JSON.stringify({a:undefined,f:function(){}})==='{}' && JSON.stringify([undefined])==='[null]', 'omitted "
      "members');\n"
      "threw=false; try{ JSON.stringify(1n) }catch(e){ threw=e instanceof TypeError } expect(threw, 'BigInt throws');\n"
      "expect(JSON.stringify({a:[1]},null,2)==='{\\n  \"a\": [\\n    1\\n  ]\\n}', 'indent');\n",
      0));
}

static void check_atomics(void)
{
  expect(
      run("var sab=new SharedArrayBuffer(16), i=new Int32Array(sab), u8=new Uint8Array(sab), t=function(f){ try{ f() "
          "}catch(e){ return e.constructor.name } };\n"
          "expect(sab.byteLength===16 && Object.prototype.toString.call(sab)==='[object SharedArrayBuffer]' && !(sab "
          "instanceof ArrayBuffer), 'SharedArrayBuffer');\n"
          "var s2=sab.slice(4,8); expect(s2 instanceof SharedArrayBuffer && s2.byteLength===4, 'slice stays shared');\n"
          "expect(Atomics.store(i,0,5)===5 && Atomics.load(i,0)===5, 'store, load');\n"
          "expect(Atomics.add(i,0,3)===5 && i[0]===8, 'add returns the old value');\n"
          "expect(Atomics.sub(i,0,10)===8 && i[0]===-2, 'sub');\n"
          "i[1]=12; expect(Atomics.and(i,1,10)===12 && i[1]===8 && Atomics.or(i,1,3)===8 && i[1]===11 && "
          "Atomics.xor(i,1,1)===11 && i[1]===10, 'and, or, xor');\n"
          "expect(Atomics.exchange(i,1,7)===10 && i[1]===7, 'exchange');\n"
          "expect(Atomics.compareExchange(i,1,7,9)===7 && i[1]===9 && Atomics.compareExchange(i,1,7,1)===9 && "
          "i[1]===9, 'compareExchange');\n"
          "u8[8]=250; expect(Atomics.add(u8,8,10)===250 && u8[8]===4, 'wraps to the element width');\n"
          "var i8=new Int8Array(new ArrayBuffer(2)); expect(Atomics.compareExchange(i8,0,-1,3)===0 && "
          "Atomics.store(i8,0,255)===255 && Atomics.load(i8,0)===-1 && Atomics.compareExchange(i8,0,255,2)===-1 && "
          "i8[0]===2, 'expected value is narrowed');\n"
          "var b=new BigInt64Array(new SharedArrayBuffer(16)); Atomics.store(b,0,-1n);\n"
          "expect(Atomics.add(b,0,3n)===-1n && b[0]===2n && Atomics.sub(b,0,3n)===2n && b[0]===-1n, '64-bit add and "
          "sub');\n"
          "expect(Atomics.compareExchange(b,0,-1n,7n)===-1n && b[0]===7n && t(function(){ Atomics.add(b,0,1) "
          "})==='TypeError', '64-bit needs BigInt');\n"
          "expect(t(function(){ Atomics.add(new Float64Array(1),0,1) })==='TypeError' && t(function(){ "
          "Atomics.load(new Uint8ClampedArray(1),0) })==='TypeError' && t(function(){ Atomics.load([1],0) "
          "})==='TypeError', 'integer typed arrays only');\n"
          "expect(t(function(){ Atomics.load(i,4) })==='RangeError' && t(function(){ Atomics.load(i,-1) "
          "})==='RangeError', 'index bounds');\n"
          "i[2]=1; expect(t(function(){ Atomics.wait(i,2,1,0) })==='TypeError', 'wait throws where blocking is off');\n"
          "expect(Atomics.notify(i,2,1)===0, 'notify with no waiters');\n"
          "expect(Atomics.isLockFree(4)===true && Atomics.isLockFree(3)===false && String(Atomics)==='[object "
          "Atomics]', 'isLockFree, tag');\n",
          0));
}

static void check_structured_clone(void)
{
  expect(run(
      "var t=function(f){ try{ f() }catch(e){ return e.name } };\n"
      "expect(clone(1.5)===1.5 && clone('hi\\u00e9')==='hi\\u00e9' && clone(true)===true && clone(null)===null && "
      "clone(undefined)===undefined && clone(-0)===0 && 1/clone(-0)===-Infinity, 'primitives');\n"
      "expect(clone(2n**70n)===2n**70n && clone(-5n)===-5n, 'BigInt');\n"
      "var o={a:1,b:[1,2,{c:'x'}],d:{e:null}}, c=clone(o);\n"
      "expect(c!==o && c.b!==o.b && JSON.stringify(c)===JSON.stringify(o), 'plain objects and arrays are copied');\n"
      "var cyc={n:1}; cyc.self=cyc; cyc.list=[cyc]; var cc=clone(cyc);\n"
      "expect(cc.self===cc && cc.list[0]===cc && cc!==cyc, 'cycles');\n"
      "var shared={z:1}, two=clone({x:shared,y:shared}); expect(two.x===two.y, 'shared structure stays shared');\n"
      "var d=new Date(1234567890123), dc=clone(d); expect(dc instanceof Date && dc.getTime()===1234567890123 && "
      "dc!==d, 'Date');\n"
      "var ec=clone(new TypeError('boom')); expect(ec instanceof TypeError && ec.message==='boom', 'Error');\n"
      "var u=new Uint8Array([1,2,3,4,5]), uc=clone(u.subarray(1,4));\n"
      "expect(uc instanceof Uint8Array && uc.length===3 && uc[0]===2 && uc.byteOffset===1 && uc.buffer.byteLength===5 "
      "&& "
      "uc.buffer!==u.buffer, 'typed array views keep offset and length');\n"
      "var ab=new ArrayBuffer(8), pair=clone({a:new Int32Array(ab),b:new Uint8Array(ab)}); pair.a[0]=0x01020304;\n"
      "expect(pair.b[0]===4 && pair.a.buffer===pair.b.buffer, 'views of one buffer still share it');\n"
      "var f64=clone(new Float64Array([1.5,-2])); expect(f64[0]===1.5 && f64[1]===-2, 'Float64Array');\n"
      "var dv=clone(new DataView(new ArrayBuffer(4),1,2)); expect(dv.byteLength===2 && dv.byteOffset===1, "
      "'DataView');\n"
      "var sab=new SharedArrayBuffer(8), s1=new Int32Array(sab), sc=clone({m:s1}).m; Atomics.store(s1,0,7);\n"
      "expect(sc.buffer instanceof SharedArrayBuffer && Atomics.load(sc,0)===7 && sc.buffer!==sab, 'SharedArrayBuffer "
      "is shared, not copied');\n"
      "Atomics.store(sc,1,9); expect(Atomics.load(s1,1)===9, 'writes cross both ways');\n"
      "expect(t(function(){ clone(function(){}) })==='DataCloneError' && t(function(){ clone({f:Symbol()}) "
      "})==='DataCloneError', "
      "'functions and symbols throw DataCloneError');\n"
      "var mp=new Map([[1,'a'],['k',{z:[1]}]]); mp.set(mp,'self'); var mc=clone(mp);\n"
      "expect(mc instanceof Map && mc!==mp && mc.size===3 && mc.get(1)==='a' && mc.get('k').z[0]===1 && "
      "mc.get(mc)==='self', "
      "'Map, including a key that is the map itself');\n"
      "var st=new Set([1,'two',{three:3}]), sc2=clone(st); expect(sc2 instanceof Set && sc2.size===3 && "
      "sc2.has('two'), 'Set');\n"
      "var rc=clone(/a(b+)c/gi); expect(rc instanceof RegExp && rc.source==='a(b+)c' && rc.flags==='gi' && "
      "rc.test('xABBc'), "
      "'RegExp');\n"
      "var ab2=new ArrayBuffer(8), v8=new Uint8Array(ab2); v8[0]=9; var dv2=new DataView(ab2);\n"
      "var moved=clone({b:ab2,v:v8},[ab2]);\n"
      "expect(ab2.byteLength===0 && v8.length===0 && v8[0]===undefined && v8.byteLength===0, 'transfer detaches the "
      "sender');\n"
      "v8.fill(1); v8.reverse(); expect(v8.join()==='' && t(function(){ dv2.getUint8(0) })==='RangeError', 'detached "
      "views stay safe');\n"
      "expect(moved.b.byteLength===8 && moved.v[0]===9 && moved.v.buffer===moved.b, 'the receiver gets the bytes, "
      "views included');\n"
      "var keep=new ArrayBuffer(4); clone(keep,[]); clone(keep,{transfer:[]}); expect(keep.byteLength===4, 'no "
      "transfer, no detach');\n"
      "var mv2=new ArrayBuffer(4), r2=clone([mv2],{transfer:[mv2]}); expect(mv2.byteLength===0 && "
      "r2[0].byteLength===4, 'options form');\n"
      "var bad=new ArrayBuffer(4); t(function(){ clone([bad,function(){}],[bad]) }); expect(bad.byteLength===4, 'a "
      "failed write detaches nothing');\n"
      "var big=[]; for (var i=0;i<1000;i++) big.push({i:i,s:'v'+i}); var bc=clone(big);\n"
      "expect(bc.length===1000 && bc[999].s==='v999', 'large arrays');\n",
      0));
}

static void check_promise_combinators(void)
{
  expect(run("var out=[], r1, r2;\n"
             "var p1=new Promise(function(a){ r1=a }), p2=new Promise(function(a,b){ r2=b });\n"
             "Promise.all([p1,3]).then(function(v){ out.push('all:'+v) });\n"
             "Promise.all([p1,p2]).then(0,function(e){ out.push('allrej:'+e) });\n"
             "Promise.allSettled([p1,p2]).then(function(v){ out.push('settled:'+v.map(function(x){ return x.status "
             "}).join()) });\n"
             "Promise.race([p2,p1]).then(function(v){ out.push('race:'+v) },function(e){ out.push('racerej:'+e) });\n"
             "Promise.any([p2,p1]).then(function(v){ out.push('any:'+v) });\n"
             "Promise.any([p2]).then(0,function(e){ out.push('anyfail:'+e.errors.length+e.name) });\n"
             "Promise.all([]).then(function(v){ out.push('empty:'+v.length) });\n"
             "expect(out.join()==='empty:0', 'nothing settles before the promises do: '+out.join());\n"
             "r2('x'); r1(5);\n"
             "expect(out.indexOf('allrej:x')>=0 && out.indexOf('racerej:x')>=0, 'rejections reach all and race: "
             "'+out.join());\n"
             "expect(out.indexOf('all:5,3')>=0, 'all fulfils with the values in order');\n"
             "expect(out.indexOf('settled:fulfilled,rejected')>=0 && out.indexOf('any:5')>=0 && "
             "out.indexOf('anyfail:1AggregateError')>=0, "
             "'allSettled and any: '+out.join());\n"
             "var late=[], s1; var q1=new Promise(function(a){ s1=a });\n"
             "Promise.all([q1,Promise.resolve(2)]).then(function(v){ late.push(v.join()) });\n"
             "expect(late.length===0, 'all waits for a pending promise'); s1(1); expect(late.join()==='1,2', 'and "
             "settles when it does');\n",
             0));
}

static void check_typed_array_copy(void)
{
  expect(run(
      "var t=function(f){ try{ f() }catch(e){ return e.name } };\n"
      "var a=new Uint8Array([1,2,3,4,5]), b=new Uint8Array(8); b.set(a,2);\n"
      "expect(b.join()==='0,0,1,2,3,4,5,0', 'set copies at an offset: '+b.join());\n"
      "b.set(b.subarray(2,6),0); expect(b.join()==='1,2,3,4,3,4,5,0', 'set handles overlap: '+b.join());\n"
      "var f=new Float32Array(3); f.set(new Uint8Array([7,8,9])); expect(f.join()==='7,8,9', 'set converts between "
      "types');\n"
      "var i16=new Int16Array(2); i16.set([300,-2]); expect(i16.join()==='300,-2', 'set from an array');\n"
      "expect(t(function(){ new Uint8Array(2).set(a) })==='RangeError' && t(function(){ new Uint8Array(4).set([1],4) "
      "})==='RangeError' && "
      "t(function(){ new Uint8Array(4).set([1],-1) })==='RangeError', 'set past the end throws');\n"
      "var s=a.slice(1,4); s[0]=99; expect(s.join()==='99,3,4' && a[1]===2, 'slice copies');\n"
      "var c=new Uint16Array(new Uint16Array([1,500,65535])); expect(c.join()==='1,500,65535', 'copy construction');\n"
      "var u8=new Uint8ClampedArray(new Float64Array([300,-5,1.5])); expect(u8.join()==='255,0,2', 'conversion on copy "
      "construction: '+u8.join());\n",
      0));
}

static void check_string_append(void)
{
  expect(run("var s='', t='', parts=[], mark=0;\n"
             "for (var i=0;i<20000;i++) { s+='k'+i+'='+i+'&'; parts.push('k'+i+'='+i+'&'); if (i===19000) { t=s; "
             "mark=parts.join('').length; } }\n"
             "expect(s===parts.join(''), 'an appended string equals the joined one: '+s.length);\n"
             "expect(t.length===mark && t.slice(-6)==='19000&', 'an older value is not disturbed by later appends');\n"
             "var big=0; for (var j=0;j<300;j++) { var b=new Uint8Array(1<<18); b[0]=j; big=b.length; }\n"
             "expect(big===262144, 'buffers made in a loop are collected as they go');\n",
             0));
}

static void check_string_append_aliasing(void)
{
  expect(
      run("(function(){ var pad='', k; for (k=0;k<300;k++) pad+='p';\n"
          "var sp=''; for (k=0;k<300;k++) sp+=' ';\n"
          "var n=sp; n+='5'; var nt=n; n+='6'; var o0={}; o0[nt]=1; expect(Object.keys(o0)[0]===nt && nt.length===301, "
          "'an aliased string stays NUL terminated: '+Object.keys(o0)[0].length);\n"
          "var s=pad, t, u, w;\n"
          "s+='a'; t=s; s+='b'; u=s; s+='c'; w=s;\n"
          "expect(t===pad+'a' && u===pad+'ab' && w===pad+'abc', 'aliases keep their value: '+t.length+' '+u.length);\n"
          "t+='X'; s+='d';\n"
          "expect(t===pad+'aX' && s===pad+'abcd' && u===pad+'ab', 'diverging appends do not clobber each other');\n"
          "var z=pad; z+='1'; z+=z; z+=2; z+=null; z=z+undefined; z+=true;\n"
          "expect(z===pad+'1'+pad+'1'+'2nullundefinedtrue', 'self append and non-string operands');\n"
          "var g=pad; g+='x'; var cap=function(){ return g; }; var got=cap(); g+='y';\n"
          "expect(got===pad+'x' && g===pad+'xy' && cap()===pad+'xy', 'a closure read is not disturbed');\n"
          "var o={v:pad}; var r=pad; r+='q'; o.v=r; r+='q'; expect(o.v===pad+'q' && r===pad+'qq', 'a stored value is "
          "not disturbed');\n"
          "function f(){ var a=pad; for (var i=0;i<5;i++) a+=i; a=a+'e'; return a; }\n"
          "expect(f()===pad+'01234e' && f()===pad+'01234e', 'function locals');\n"
          "var e=pad; e+='a'; var ev=eval('e+=\"b\"'); e+='c';\n"
          "expect(ev===pad+'ab' && e===pad+'abc', 'a completion value is not disturbed');\n"
          "const cc=pad; var threw=false; try { cc+='x'; } catch (x) { threw=true; } expect(threw && cc===pad, 'const "
          "is not appended');\n"
          "var h=pad; h+='a'; try { h+=(function(){ throw 1; })(); } catch (x) {} h+='b'; expect(h===pad+'ab', "
          "'throwing operand');\n"
          "var m=pad; m+='a'; m+=(m='Z'); expect(m===pad+'aZ', 'reassigned during operand'); })();\n",
          0));
}

static void check_computed_member_gc(void)
{
  expect(run("function churn() { var j; for (var i = 0; i < 4000; i++) j = { a: i, b: [i, i] }; return 1; }\n"
             "var bad = 0; for (var k = 0; k < 60; k++) { var v = [{ x: 1 }, { x: 2 }, { x: 3 }][churn()]; if (!v || "
             "v.x !== 2) bad++; }\n"
             "expect(bad === 0, 'an array literal survives collection while its index is evaluated: ' + bad);\n",
             0));
}

static void check_promise_chain_depth(void)
{
  expect(run(
      "var n=0, done=false;\n"
      "function hop(){ n++; return n<20000 ? Promise.resolve(n).then(hop) : Promise.resolve('end'); }\n"
      "hop().then(function(v){ done=v; });\n"
      "expect(done==='end' && n===20000, 'a long chain of settled promises does not exhaust the stack: '+n+' '+done);\n"
      "var order=[]; Promise.resolve().then(function(){ order.push('a'); }); order.push('sync'); "
      "Promise.resolve().then(function(){ order.push('b'); });\n"
      "expect(order.join()==='a,sync,b', 'shallow reactions still run in place: '+order.join());\n"
      "var arr=[], rs=null, p=new Promise(function(res){ rs=res; }); var chain=p;\n"
      "for (var i=0;i<5000;i++) { (function(i){ chain=chain.then(function(v){ arr.push(i); return v; }); })(i); }\n"
      "rs('go'); expect(arr.length===5000 && arr[4999]===4999 && arr[0]===0, 'a pending chain settles in order: "
      "'+arr.length);\n",
      0));
}

static void check_json_parse(void)
{
  expect(
      run("var threw=false; try{ JSON.parse('1 2') }catch(e){ threw=e instanceof SyntaxError } expect(threw, 'trailing "
          "text');\n"
          "expect(JSON.parse('[1,{\"a\":2}]',function(k,v){ return typeof v==='number'?v+1:v })[1].a===3, 'reviver');\n"
          "var s=JSON.parse('\"\\\\ud83d\\\\ude00\"'); expect(s==='\\u{1F600}' && "
          "encodeURIComponent(s)==='%F0%9F%98%80', 'surrogate pair becomes one character');\n"
          "threw=false; try{ JSON.parse({toString:function(){ throw new Error('x') }}) }catch(e){ "
          "threw=e.message==='x' } expect(threw, 'toString throwing');\n",
          0));
}

static void check_from_code_point(void)
{
  expect(run("expect(String.fromCodePoint(0x1F600)==='\\u{1F600}', 'fromCodePoint');\n"
             "expect(String.fromCharCode(0xd83d,0xde00)==='\\u{1F600}', 'fromCharCode joins a pair');\n"
             "expect('\\ud83d\\ude00'==='\\u{1F600}', 'literal pair');\n"
             "var threw=false; try{ String.fromCodePoint(-1) }catch(e){ threw=e instanceof RangeError } expect(threw, "
             "'invalid code point');\n",
             0));
}

static void check_deep_recursion(void)
{
  char probe;

  expect(run("var o=[]; for(var i=0;i<100000;i++) o=[o];\n"
             "var threw=false; try{ JSON.stringify(o) }catch(e){ threw=e instanceof RangeError } expect(threw, "
             "'JSON.stringify depth');\n"
             "var s='['; while(s.length<100000) s+=s; threw=false;\n"
             "try{ JSON.parse(s) }catch(e){ threw=e instanceof RangeError } expect(threw, 'JSON.parse depth');\n"
             "var src='x=a'; for(var i=0;i<5000;i++) src+='.b'; threw=false;\n"
             "try{ new Function(src)() }catch(e){ threw=true } expect(threw, 'long member chain');\n"
             "var p={}; for(var i=0;i<100000;i++) p=new Proxy(p,{}); threw=false;\n"
             "try{ p.x }catch(e){ threw=e instanceof RangeError } expect(threw, 'proxy chain');\n",
             &probe - 60000));
}

static void check_parser_stack_floor(void)
{
  char probe;

  js_arena arena;
  js_parse_error error = {0, 0};
  char src[1200];
  int i;
  for (i = 0; i < 500; i++)
    src[i] = '[';
  src[500] = '1';
  for (i = 0; i < 500; i++)
    src[501 + i] = ']';
  src[1001] = 0;
  js_arena_init(&arena);
  js_parser_set_stack_floor(&probe - 30000);
  expect(js_parse(src, &arena, &error) == 0);
  js_parser_set_stack_floor(0);
  js_arena_free(&arena);
  js_arena_init(&arena);
  expect(js_parse(src, &arena, &error) != 0);
  js_arena_free(&arena);
}

static void check_dataview(void)
{
  expect(run(
      "var b=new ArrayBuffer(16), v=new DataView(b);\n"
      "v.setUint16(0,0x1234); expect(v.getUint8(0)===0x12 && v.getUint16(0,true)===0x3412, 'endianness');\n"
      "v.setBigInt64(0,-2n); expect(v.getBigInt64(0)===-2n && v.getBigUint64(0)===2n**64n-2n, 'int64 round trip');\n"
      "v.setBigUint64(8,0x0102030405060708n,true); expect(v.getUint8(8)===8 && v.getUint8(15)===1, 'uint64 "
      "little-endian');\n"
      "v.setBigInt64(0,2n**64n+5n); expect(v.getBigUint64(0)===5n, 'int64 wraps modulo 2^64');\n"
      "var threw=false; try{ v.setBigInt64(0,1) }catch(e){ threw=e instanceof TypeError } expect(threw, 'Number is not "
      "a BigInt');\n"
      "threw=false; try{ v.getBigInt64(9) }catch(e){ threw=e instanceof RangeError } expect(threw, 'bounds');\n"
      "var cases=[[0,0],[1,0x3c00],[-2,0xc000],[65504,0x7bff],[65520,0x7c00],[Infinity,0x7c00],[-Infinity,0xfc00],\n"
      "  "
      "[5.960464477539063e-8,1],[6.103515625e-5,0x400],[0.333251953125,0x3555],[2049,0x6800],[2051,0x6802],[1e-9,0]];\n"
      "for(var i=0;i<cases.length;i++){ v.setFloat16(0,cases[i][0]); expect(v.getUint16(0)===cases[i][1], 'f16 encode "
      "'+cases[i][0]); }\n"
      "v.setUint16(0,0x3c00); expect(v.getFloat16(0)===1, 'f16 decode 1');\n"
      "v.setUint16(0,0x0001); expect(v.getFloat16(0)===Math.pow(2,-24), 'f16 smallest subnormal');\n"
      "v.setUint16(0,0x7e00); expect(v.getFloat16(0)!==v.getFloat16(0), 'f16 NaN');\n"
      "v.setFloat16(0,NaN); expect(v.getUint16(0)===0x7e00, 'f16 NaN encode');\n",
      0));
}

static void check_wide_typed_arrays(void)
{
  expect(run(
      "var b=new BigInt64Array(2), u=new BigUint64Array(b.buffer);\n"
      "expect(b.length===2 && b.byteLength===16 && BigInt64Array.BYTES_PER_ELEMENT===8, 'sizes');\n"
      "b[0]=-1n; expect(b[0]===-1n && u[0]===2n**64n-1n, 'signed and unsigned views');\n"
      "b[1]=2n**63n; expect(b[1]===-(2n**63n), 'wraps modulo 2^64');\n"
      "var threw=false; try{ b[0]=1 }catch(e){ threw=e instanceof TypeError } expect(threw, 'Number is not a "
      "BigInt');\n"
      "expect(b[2]===undefined, 'out of range read'); b[2]=1n; expect(b.length===2, 'out of range write ignored');\n"
      "var f=BigUint64Array.from([1n,2n,3n]); expect(f.join()==='1,2,3' && f.indexOf(2n)===1, 'from, join, indexOf');\n"
      "expect(f.reduce(function(a,x){ return a+x }, 0n)===6n, 'reduce');\n"
      "expect(f.map(function(x){ return x*2n }).join()==='2,4,6', 'map');\n"
      "expect(f.slice(1).join()==='2,3' && f.subarray(1,2)[0]===2n, 'slice and subarray');\n"
      "f.fill(7n); expect(f.join()==='7,7,7', 'fill');\n"
      "threw=false; try{ new BigInt64Array(new ArrayBuffer(16),8,2) }catch(e){ threw=e instanceof RangeError } "
      "expect(threw, 'view past the buffer');\n"
      "threw=false; try{ new BigInt64Array(new ArrayBuffer(16),0,2147483647) }catch(e){ threw=e instanceof RangeError "
      "} expect(threw, 'huge length');\n"
      "var h=new Float16Array([1,-2,65504,65520,0.1]);\n"
      "expect(h.byteLength===10 && h[0]===1 && h[1]===-2 && h[2]===65504 && h[3]===Infinity, 'float16 values');\n"
      "expect(Math.abs(h[4]-0.1)<1e-4 && h[4]!==0.1, 'float16 rounds');\n"
      "expect(new Uint16Array(new Float16Array([1]).buffer)[0]===0x3c00, 'float16 bits');\n"
      "var d=new DataView(new ArrayBuffer(8)); threw=false;\n"
      "try{ d.getUint8(2147483647) }catch(e){ threw=e instanceof RangeError } expect(threw, 'DataView offset near "
      "INT_MAX');\n"
      "threw=false; try{ d.getBigInt64(1) }catch(e){ threw=e instanceof RangeError } expect(threw, 'DataView int64 "
      "bounds');\n",
      0));
}

static void check_promise_gc(void)
{
  gc_set_threshold(1);
  expect(run("var n=0, p=Promise.resolve(0);\n"
             "for(var i=0;i<300;i++){ p=p.then(function(v){ return v+1 }); new Promise(function(res){ res(p) "
             "}).then(function(){ n++ }) }\n"
             "expect(n===300, 'reactions survive a collection on every allocation');\n",
             0));
  gc_set_threshold(1 << 20);
}

static void check_generators(void)
{
  expect(run(
      "function* g(a){ var x = yield a; var y = yield x + 1; return x + y }\n"
      "var it = g(1), r = it.next();\n"
      "expect(r.value===1 && r.done===false, 'first yield');\n"
      "r = it.next(10); expect(r.value===11 && !r.done, 'sent value comes back from yield');\n"
      "r = it.next(5); expect(r.value===15 && r.done===true, 'return value ends it');\n"
      "r = it.next(); expect(r.value===undefined && r.done===true, 'finished generator stays done');\n"
      "function* nat(){ var i = 0; while (true) yield i++ }\n"
      "var n = nat(), s = 0; for (var v of n) { if (v > 4) break; s += v }\n"
      "expect(s===10, 'infinite generator under for-of break');\n"
      "expect([...(function*(){ yield 1; yield* [2,3]; yield* (function*(){ yield 4; return 5 })() "
      "})()].join()==='1,2,3,4',"
      " 'spread and yield*');\n"
      "var d = (function*(){ var r = yield* (function*(){ yield 1; return 'in' })(); yield r })();\n"
      "d.next(); expect(d.next().value==='in', 'yield* evaluates to the delegate return value');\n"
      "var o = { *[Symbol.iterator](){ yield 'a'; yield 'b' } };\n"
      "expect(Array.from(o).join('')==='ab', 'generator method as Symbol.iterator');\n"
      "class K { *items(){ yield this.v } constructor(){ this.v = 7 } }\n"
      "expect(new K().items().next().value===7, 'this inside a generator method');\n"
      "expect([...(function*(){ yield* 'ab' })()].join('')==='ab', 'yield* over a string');\n"
      "function* sum(a, b){ return a + b }\n"
      "function* args(){ return sum(yield 1, yield 2) }\n"
      "var ai = args(); ai.next(); ai.next(5); ai = args(); ai.next(); ai.next(5);\n"
      "var gt = nat(); expect(gt[Symbol.iterator]()===gt && Object.prototype.toString.call(gt)==='[object Generator]',"
      " 'iterator protocol surface');\n",
      0));
  expect(run("var log = [];\n"
             "function* g(){ try { yield 1; yield 2 } catch (e) { log.push('c' + e); yield 'recovered' } finally { "
             "log.push('f') } }\n"
             "var it = g(); it.next();\n"
             "expect(it.throw('x').value==='recovered', 'throw() is caught by the body');\n"
             "it.next(); expect(log.join()==='cx,f', 'finally ran');\n"
             "var r = g(); r.next();\n"
             "var out = r.return(9); expect(out.value===9 && out.done===true, 'return() ends the generator');\n"
             "expect(log.join()==='cx,f,f', 'return() runs finally but not catch');\n"
             "var u = g(); var res = u.return(3); expect(res.value===3 && res.done, 'return() before start');\n"
             "var t = g(); var threw = false; try { t.throw('boom') } catch (e) { threw = e==='boom' }\n"
             "expect(threw && t.next().done, 'throw() before start rejects and finishes');\n"
             "function* bad(){ yield 1; throw new Error('in body') }\n"
             "var b = bad(); b.next(); var msg = ''; try { b.next() } catch (e) { msg = e.message }\n"
             "expect(msg==='in body' && b.next().done, 'a throw out of the body propagates');\n"
             "function* self(){ yield it2.next() } var it2 = self(); var re = '';\n"
             "try { it2.next() } catch (e) { re = e.name }\n"
             "expect(re==='TypeError', 'resuming a running generator throws');\n"
             "log = []; function* inner(){ try { yield 1; yield 2 } finally { log.push('inner') } }\n"
             "for (var x of inner()) break; expect(log.join()==='inner', 'for-of break closes the iterator');\n"
             "function* outer(){ try { yield* inner() } finally { log.push('outer') } }\n"
             "log = []; var o = outer(); o.next(); o.return(); expect(log.join()==='inner,outer', 'return() through "
             "yield*');\n",
             0));
}

static void check_async_generators(void)
{
  expect(run(
      "var log = [];\n"
      "async function* ag(){ var x = yield 1; log.push('got ' + x); yield Promise.resolve(2); await null; return 3 }\n"
      "var it = ag(), p = it.next();\n"
      "expect(p instanceof Promise && it[Symbol.asyncIterator]()===it, 'next() returns a promise');\n"
      "p.then(function(r){ log.push(r.value + ':' + r.done) });\n"
      "it.next('a').then(function(r){ log.push(r.value + ':' + r.done) });\n"
      "it.next().then(function(r){ log.push(r.value + ':' + r.done) });\n"
      "it.next().then(function(r){ log.push(r.value + ':' + r.done) });\n"
      "async function run(){ var seen = [];\n"
      "  for await (var v of ag()) seen.push(v);\n"
      "  for await (var w of [Promise.resolve('p'), 'q']) seen.push(w);\n"
      "  async function* outer(){ yield* ag(); yield* [7, 8] }\n"
      "  for await (var z of outer()) seen.push(z);\n"
      "  log.push('seen ' + seen.join()) }\n"
      "run();\n"
      "async function* bad(){ yield 1; throw new Error('boom') }\n"
      "var b = bad(); b.next(); b.next().catch(function(e){ log.push('rej ' + e.message) });\n"
      "var out = log.join('|');\n"
      "expect(out==='1:false|got a|2:false|3:true|undefined:true|got undefined|got undefined|seen 1,2,p,q,1,2,7,8|rej "
      "boom', 'async generator results: ' + out);\n",
      0));
}

static void check_generator_gc(void)
{
  gc_set_threshold(1);
  expect(
      run("function* g(n){ var keep = {n: n}; for (var i = 0; i < 20; i++) { var o = {i: i, s: 'v' + i}; yield o; }\n"
          "  return keep }\n"
          "var gens = [], total = 0;\n"
          "for (var k = 0; k < 20; k++) gens.push(g(k));\n"
          "for (var round = 0; round < 20; round++) for (var k = 0; k < 20; k++) { var r = gens[k].next();\n"
          "  total += r.value.i + r.value.s.length }\n"
          "var last = gens[3].next(); expect(last.done && last.value.n===3, 'parked locals survive collection');\n"
          "expect(total===20*240, 'every parked yield delivered its value intact');\n"
          "for (var j = 0; j < 200; j++) { var abandoned = g(j); abandoned.next() }\n",
          0));
  gc_set_threshold(1 << 20);
}

static void check_generator_stack_guard(void)
{
  expect(run("function* r(){ function f(){ return f() + 1 } try { f() } catch (e) { yield e instanceof RangeError } }\n"
             "expect(r().next().value===true, 'runaway recursion inside a generator throws RangeError');\n",
             0));
}

static void check_date(void)
{
  expect(run(
      "function p(s){ return Date.parse(s); }\n"
      "expect(p('2024-01-02')===1704153600000 && p('2024-01-02T03:04:05.678+01:00')===1704161045678, 'ISO forms');\n"
      "expect(p('2024')===1704067200000 && p('2024-01-02T03:04')===1704164640000, 'partial ISO forms');\n"
      "expect(p('Tue, 02 Jan 2024 15:04:05 GMT')===1704207845000, 'RFC 1123');\n"
      "expect(p('January 2, 2024 3:04:05 PM')===1704207845000 && p('Jan 2, 2024')===1704153600000, 'month names');\n"
      "expect(p('2 Jan 2024 10:00 GMT+0100')===1704186000000 && p('Jan 2 2024 12:00 AM EST')===1704171600000, "
      "'zones');\n"
      "expect(p('1/2/2024')===1704153600000 && p('2024/01/02')===1704153600000, 'numeric dates');\n"
      "expect(p(new Date(5).toString())===0, 'toString output parses back to the second');\n"
      "expect(isNaN(p('garbage')) && isNaN(p('')) && isNaN(p('2024-13-01')) && isNaN(p()), 'invalid input is NaN');\n"
      "expect(new Date('2024-01-02T03:04:05Z').getTime()===1704164645000, 'constructor parses strings');\n"
      "expect(new Date(new Date(5)).getTime()===5 && isNaN(new Date(8.64e15+1)), 'copy and range');\n"
      "expect(Date.UTC(2020,11,31,23,59,59,999)===1609459199999 && Date.UTC(99)===915148800000 && isNaN(Date.UTC()), "
      "'UTC');\n"
      "expect(new Date(2020,12,1).toISOString()==='2021-01-01T00:00:00.000Z', 'month overflow rolls the year');\n"
      "var d=new Date(2024,0,31,10);\n"
      "d.setMonth(1); expect(d.toISOString()==='2024-03-02T10:00:00.000Z', 'setMonth overflows the day');\n"
      "d.setDate(0); expect(d.toISOString()==='2024-02-29T10:00:00.000Z', 'setDate(0)');\n"
      "d.setUTCHours(25,61,61,1001); expect(d.toISOString()==='2024-03-01T02:02:02.001Z', 'setHours carries');\n"
      "expect(d.setFullYear(2020,1,29)===d.getTime() && d.getUTCDate()===29, 'setFullYear returns the time');\n"
      "var y=new Date(0); expect(y.getYear()===70 && y.setYear(99)===y.getTime() && y.getUTCFullYear()===1999 && "
      "(y.setYear(2005), y.getUTCFullYear()===2005), 'getYear and setYear');\n"
      "expect(isNaN(new Date('x').getYear()) && isNaN(new Date(0).setYear(NaN)), 'getYear and setYear on NaN');\n"
      "expect(d.setTime(5)===5 && d.getTime()===5, 'setTime');\n"
      "var s=new Date(0);\n"
      "expect(s.toUTCString()==='Thu, 01 Jan 1970 00:00:00 GMT' && s.toDateString()==='Thu Jan 01 1970', 'UTC and date "
      "strings');\n"
      "expect(s.toString()==='Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)', 'toString');\n"
      "var bad=new Date('x'), thrown=false;\n"
      "expect(String(bad)==='Invalid Date' && isNaN(bad.getFullYear()) && JSON.stringify({a:bad})==='{\"a\":null}', "
      "'invalid dates');\n"
      "expect(isNaN(bad.setHours(1)) && isNaN(bad.getTime()), 'setters keep an invalid date invalid');\n"
      "try { bad.toISOString(); } catch (e) { thrown=e instanceof RangeError; }\n"
      "expect(thrown, 'toISOString throws RangeError');\n"
      "expect(new Date(-62198755200000).toISOString()==='-000001-01-01T00:00:00.000Z', 'expanded years');\n",
      0));
}

static void check_intl_supported_locales(void)
{
  expect(run("var names=['DateTimeFormat','NumberFormat','Collator','PluralRules','RelativeTimeFormat','ListFormat',"
             "'Segmenter'];\n"
             "names.forEach(function(n){ var r=Intl[n].supportedLocalesOf(['en-US','fr']);\n"
             "  expect(r.length===2 && r[0]==='en-US' && r[1]==='fr', n+'.supportedLocalesOf'); });\n"
             "expect(Intl.NumberFormat.supportedLocalesOf('de')[0]==='de' && "
             "Intl.PluralRules.supportedLocalesOf().length===0, "
             "'string and missing argument');\n",
             0));
}

int main(void)
{
  test("regexp matches whole code points, not bytes", check_regexp_code_points);
  test("regexp compile and match are bounded by the stack", check_regexp_bounded);
  test("Object.defineProperty on array elements", check_define_property);
  test("promise reaction objects survive collection", check_promise_gc);
  test("JSON.stringify", check_json_stringify);
  test("DataView BigInt64 and Float16", check_dataview);
  test("BigInt64Array, BigUint64Array and Float16Array", check_wide_typed_arrays);
  test("SharedArrayBuffer and Atomics", check_atomics);
  test("structured clone across heaps", check_structured_clone);
  test("Promise.all, allSettled, race and any with pending promises", check_promise_combinators);
  test("typed array set, slice and copy construction", check_typed_array_copy);
  test("long string appends and buffer garbage", check_string_append);
  test("string appends in place keep aliases intact", check_string_append_aliasing);
  test("a computed member's object survives its key being evaluated", check_computed_member_gc);
  test("long promise chains do not recurse without bound", check_promise_chain_depth);
  test("JSON.parse", check_json_parse);
  test("String.fromCodePoint and surrogate pairs", check_from_code_point);
  test("deep recursion throws instead of overflowing the native stack", check_deep_recursion);
  test("parser honours the stack floor", check_parser_stack_floor);
  test("generators", check_generators);
  test("async generators and for await", check_async_generators);
  test("generator state survives collection", check_generator_gc);
  test("generator bodies are stack-guarded", check_generator_stack_guard);
  test("Date parsing, UTC, setters and string forms", check_date);
  test("Intl supportedLocalesOf", check_intl_supported_locales);
  return 0;
}
