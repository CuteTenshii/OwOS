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

/* Run `source` in a fresh context; returns nonzero when it ran to completion. */
static int run(const char *source, char *stack_floor)
{
  js_arena arena;
  js_parse_error error = {0, 0};
  js_node *program;
  js_value result;
  int ok = 0;
  js_ctx *ctx = js_ctx_new(con_print);
  js_def_native(ctx, ctx->global, "expect", test_expect, 2);
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

int main(void)
{
  test("regexp matches whole code points, not bytes", check_regexp_code_points);
  test("regexp compile and match are bounded by the stack", check_regexp_bounded);
  test("Object.defineProperty on array elements", check_define_property);
  test("promise reaction objects survive collection", check_promise_gc);
  test("JSON.stringify", check_json_stringify);
  test("DataView BigInt64 and Float16", check_dataview);
  test("BigInt64Array, BigUint64Array and Float16Array", check_wide_typed_arrays);
  test("JSON.parse", check_json_parse);
  test("String.fromCodePoint and surrogate pairs", check_from_code_point);
  test("deep recursion throws instead of overflowing the native stack", check_deep_recursion);
  test("parser honours the stack floor", check_parser_stack_floor);
  test("generators", check_generators);
  test("generator state survives collection", check_generator_gc);
  test("generator bodies are stack-guarded", check_generator_stack_guard);
  return 0;
}
