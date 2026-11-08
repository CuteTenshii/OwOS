/* Host test for the browser's DOM bindings (document.cookie, node API, fragments, timers). */
#define OWOS_SUITE_BINDINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "features/jar/jar.h"
#include "jsint.h"

extern long long fake_now_ms;
int js_timers_pump(void);

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

static void run_script(const char *source)
{
	js_arena *arena = malloc(sizeof *arena);
	js_parse_error error = { 0, 0 };
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
	(void)urls; (void)n; (void)bodies; (void)lens;
}

static const char page[] =
	"<html><head><title>t</title></head><body><div id=a class='x y'>hi<span>s</span></div><script>var ready=1</script></body></html>";

int main(void)
{
	struct dom_node *root;
	B = calloc(1, sizeof *B);
	B->pg = calloc(1, 1 << 16);
	root = dom_parse(page, (int)strlen(page));
	js_run_document(root, "https://example.com/dir/p.html", fetch_none);
	js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

	test("document.cookie reads, writes and hides HttpOnly");
	cookie_store("example.com", "sid=secret; HttpOnly");
	run_script(
		"expect(document.cookie==='', 'httponly hidden from reads');\n"
		"document.cookie='a=1';\n"
		"document.cookie='b=2; Path=/';\n"
		"function jar(){ return document.cookie.split('; ').sort().join(';') }\n"
		"expect(jar()==='a=1;b=2', 'script cookies listed: '+document.cookie);\n"
		"document.cookie='sid=hijack';\n"
		"document.cookie='c=3; HttpOnly';\n"
		"expect(jar()==='a=1;b=2', 'script cannot set HttpOnly cookies');\n"
		"document.cookie='a=; Max-Age=0';\n"
		"expect(jar()==='b=2', 'Max-Age=0 deletes');\n");
	{
		char header[128] = "";
		cookie_header_tls("example.com", "/", 1, header, sizeof header);
		expect(strstr(header, "sid=secret") != 0);
		expect(strstr(header, "c=3") == 0);
	}

	test("document node properties");
	run_script(
		"expect(document.nodeType===9, 'nodeType');\n"
		"expect(document.nodeName==='#document', 'nodeName');\n"
		"expect(document.defaultView===window, 'defaultView');\n"
		"expect(window.top===window && window.parent===window, 'top/parent');\n"
		"expect(typeof document.implementation.createHTMLDocument('x').body==='object', 'createHTMLDocument');\n"
		"expect(document.documentURI==='https://example.com/dir/p.html', 'documentURI');\n");

	test("fragments, cloning and tree editing");
	run_script(
		"var d=document, a=d.getElementById('a');\n"
		"var f=d.createDocumentFragment(); f.appendChild(d.createElement('i')); f.appendChild(d.createElement('b'));\n"
		"expect(f.nodeType===11, 'fragment nodeType');\n"
		"a.appendChild(f);\n"
		"expect(a.childNodes.length===4 && f.childNodes.length===0, 'fragment children move: '+a.childNodes.length);\n"
		"expect(a.firstChild.nodeType===3 && a.firstElementChild.tagName==='SPAN', 'firstChild is text, firstElementChild is span');\n"
		"expect(a.lastElementChild.tagName==='B' && a.childElementCount===3, 'lastElementChild/count');\n"
		"var c=a.cloneNode(true); expect(c!==a && c.parentNode===null && c.outerHTML===a.outerHTML, 'deep clone');\n"
		"expect(a.cloneNode(false).childNodes.length===0, 'shallow clone');\n"
		"var s=a.querySelector('span'); s.before('x'); s.after(d.createElement('u'));\n"
		"expect(s.previousSibling.nodeValue==='x' && s.nextElementSibling.tagName==='U', 'before/after');\n"
		"s.replaceWith('r'); expect(a.querySelector('span')===null, 'replaceWith');\n"
		"a.insertAdjacentHTML('beforeend','<em>e</em>'); expect(a.lastElementChild.tagName==='EM', 'insertAdjacentHTML');\n"
		"a.querySelector('em').remove(); expect(a.querySelector('em')===null, 'remove');\n"
		"expect(d.createComment('c').nodeType===8 && d.createComment('c').nodeValue==='c', 'comment');\n"
		"var t=d.createTextNode('abc'); t.textContent='xyz'; expect(t.nodeValue==='xyz', 'text node textContent');\n");

	test("cyclic insertion is rejected");
	run_script(
		"var d=document, p=d.createElement('div'), q=d.createElement('div'), r=d.createElement('div');\n"
		"p.appendChild(q); q.appendChild(r);\n"
		"var threw=false; try{ r.appendChild(p) }catch(e){ threw=true }\n"
		"expect(threw, 'appendChild of an ancestor throws');\n"
		"threw=false; try{ p.insertBefore(p, null) }catch(e){ threw=true }\n"
		"expect(threw, 'appendChild of self throws');\n"
		"q.insertBefore(r, r);\n"
		"expect(q.firstChild===r && r.nextSibling===null, 'insertBefore(x, x) is a no-op');\n"
		"expect(p.outerHTML.length>0, 'tree still serializes');\n");

	test("timers: unused slots hold no stale pointers at teardown");
	run_script("var n=0; setTimeout(function(){ n++ }, 10); setInterval(function(){ n++ }, 1000);\n");
	fake_now_ms += 20;
	expect(js_timers_pump() == 1);

	js_page_end();
	dom_free(root);
	free(B->pg);
	free(B);
	return 0;
}
