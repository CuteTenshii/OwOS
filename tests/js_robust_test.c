/* Engine regression tests: Unicode regexps, array defineProperty, JSON, stack-overflow guards. */
#define OWOS_SUITE_TEST
#include <stdio.h>
#include <stdlib.h>
#include "ast.h"
#include "gc.h"
#include "interp.h"
#include "parser.h"
#include "value.h"

static void con_print(js_ctx *ctx, int level, const char *s, int len)
{
	(void)ctx; (void)level;
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
	js_parse_error error = { 0, 0 };
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

int main(void)
{
	char probe;

	test("regexp matches whole code points, not bytes");
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
		"expect('%String.prototype.indexOf%'.match(/[^%.[\\]]+|\\[(?:(-?\\d+(?:\\.\\d+)?)|([\"'])((?:(?!\\2)[^\\\\]|\\\\.)*?)\\2)\\]|(?=(?:\\.|\\[\\])(?:\\.|\\[\\]|%$))/g).join('|')==='String|prototype|indexOf', 'lodash path regexp');\n",
		0));

	test("regexp compile and match are bounded by the stack");
	expect(run(
		"var r=new RegExp('('.repeat(100000)); expect(r.test('a')===false, 'deep group nesting');\n"
		"var s='ab'.repeat(200000); var t=/^(a|b)*$/.test(s); expect(t===true||t===false, 'long input terminates');\n",
		&probe - 120000));

	test("Object.defineProperty on array elements");
	expect(run(
		"var a=[]; Object.defineProperty(a,0,{value:'Q',enumerable:true,configurable:true,writable:true});\n"
		"expect(a.length===1 && a[0]==='Q' && 0 in a, 'defines an element');\n"
		"expect(0 in [undefined] && !(1 in [undefined]), 'undefined elements are present');\n"
		"var c=Array.prototype.concat.call(['x'],[1,2]); expect(c.length===3 && c[0]==='x', 'concat');\n",
		0));

	test("JSON.stringify");
	expect(run(
		"var c={}; c.self=c; var threw=false; try{ JSON.stringify(c) }catch(e){ threw=e instanceof TypeError }\n"
		"expect(threw, 'circular structure throws TypeError');\n"
		"var arr=[]; arr.push(arr); threw=false; try{ JSON.stringify(arr) }catch(e){ threw=e instanceof TypeError }\n"
		"expect(threw, 'circular array throws TypeError');\n"
		"var shared={n:1}; expect(JSON.stringify({a:shared,b:shared})==='{\"a\":{\"n\":1},\"b\":{\"n\":1}}', 'shared references are fine');\n"
		"expect(JSON.stringify({a:1,b:2,c:3},['a','c'])==='{\"a\":1,\"c\":3}', 'replacer array');\n"
		"expect(JSON.stringify({a:1,b:'x'},function(k,v){ return typeof v==='number'?v*2:v })==='{\"a\":2,\"b\":\"x\"}', 'replacer function');\n"
		"expect(JSON.stringify({a:undefined,f:function(){}})==='{}' && JSON.stringify([undefined])==='[null]', 'omitted members');\n"
		"threw=false; try{ JSON.stringify(1n) }catch(e){ threw=e instanceof TypeError } expect(threw, 'BigInt throws');\n"
		"expect(JSON.stringify({a:[1]},null,2)==='{\\n  \"a\": [\\n    1\\n  ]\\n}', 'indent');\n",
		0));

	test("JSON.parse");
	expect(run(
		"var threw=false; try{ JSON.parse('1 2') }catch(e){ threw=e instanceof SyntaxError } expect(threw, 'trailing text');\n"
		"expect(JSON.parse('[1,{\"a\":2}]',function(k,v){ return typeof v==='number'?v+1:v })[1].a===3, 'reviver');\n"
		"var s=JSON.parse('\"\\\\ud83d\\\\ude00\"'); expect(s==='\\u{1F600}' && encodeURIComponent(s)==='%F0%9F%98%80', 'surrogate pair becomes one character');\n"
		"threw=false; try{ JSON.parse({toString:function(){ throw new Error('x') }}) }catch(e){ threw=e.message==='x' } expect(threw, 'toString throwing');\n",
		0));

	test("String.fromCodePoint and surrogate pairs");
	expect(run(
		"expect(String.fromCodePoint(0x1F600)==='\\u{1F600}', 'fromCodePoint');\n"
		"expect(String.fromCharCode(0xd83d,0xde00)==='\\u{1F600}', 'fromCharCode joins a pair');\n"
		"expect('\\ud83d\\ude00'==='\\u{1F600}', 'literal pair');\n"
		"var threw=false; try{ String.fromCodePoint(-1) }catch(e){ threw=e instanceof RangeError } expect(threw, 'invalid code point');\n",
		0));

	test("deep recursion throws instead of overflowing the native stack");
	expect(run(
		"var o=[]; for(var i=0;i<100000;i++) o=[o];\n"
		"var threw=false; try{ JSON.stringify(o) }catch(e){ threw=e instanceof RangeError } expect(threw, 'JSON.stringify depth');\n"
		"var s=''; for(var i=0;i<100000;i++) s+='['; threw=false;\n"
		"try{ JSON.parse(s) }catch(e){ threw=e instanceof RangeError } expect(threw, 'JSON.parse depth');\n"
		"var src='x=a'; for(var i=0;i<5000;i++) src+='.b'; threw=false;\n"
		"try{ new Function(src)() }catch(e){ threw=true } expect(threw, 'long member chain');\n"
		"var p={}; for(var i=0;i<100000;i++) p=new Proxy(p,{}); threw=false;\n"
		"try{ p.x }catch(e){ threw=e instanceof RangeError } expect(threw, 'proxy chain');\n",
		&probe - 60000));

	test("parser honours the stack floor");
	{
		js_arena arena;
		js_parse_error error = { 0, 0 };
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
	return 0;
}
