/* Host test for the browser's DOM bindings (document.cookie, node API, fragments, timers). */
#define OWOS_SUITE_BINDINGS
#include "dom.h"
#include "features/jar/jar.h"
#include "jsint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern long long fake_now_ms;
int js_timers_pump(void);

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

static const char page[] = "<html><head><title>t</title></head><body><div id=a class='x "
                           "y'>hi<span>s</span></div><script>var ready=1</script></body></html>";

static struct dom_node *root;

static void test_document_cookie_reads_writes(void)
{
  cookie_store("example.com", "sid=secret; HttpOnly");
  run_script("expect(document.cookie==='', 'httponly hidden from reads');\n"
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
}

static void test_document_node_properties(void)
{
  run_script("expect(document.nodeType===9, 'nodeType');\n"
             "expect(document.nodeName==='#document', 'nodeName');\n"
             "expect(document.defaultView===window, 'defaultView');\n"
             "expect(window.top===window && window.parent===window, 'top/parent');\n"
             "expect(typeof document.implementation.createHTMLDocument('x').body==='object', 'createHTMLDocument');\n"
             "expect(document.documentURI==='https://example.com/dir/p.html', 'documentURI');\n");
}

static void test_fragments_cloning_and_tree(void)
{
  run_script(
      "var d=document, a=d.getElementById('a');\n"
      "var f=d.createDocumentFragment(); f.appendChild(d.createElement('i')); f.appendChild(d.createElement('b'));\n"
      "expect(f.nodeType===11, 'fragment nodeType');\n"
      "a.appendChild(f);\n"
      "expect(a.childNodes.length===4 && f.childNodes.length===0, 'fragment children move: '+a.childNodes.length);\n"
      "expect(a.firstChild.nodeType===3 && a.firstElementChild.tagName==='SPAN', 'firstChild is text, "
      "firstElementChild is span');\n"
      "expect(a.lastElementChild.tagName==='B' && a.childElementCount===3, 'lastElementChild/count');\n"
      "var c=a.cloneNode(true); expect(c!==a && c.parentNode===null && c.outerHTML===a.outerHTML, 'deep clone');\n"
      "expect(a.cloneNode(false).childNodes.length===0, 'shallow clone');\n"
      "var s=a.querySelector('span'); s.before('x'); s.after(d.createElement('u'));\n"
      "expect(s.previousSibling.nodeValue==='x' && s.nextElementSibling.tagName==='U', 'before/after');\n"
      "s.replaceWith('r'); expect(a.querySelector('span')===null, 'replaceWith');\n"
      "a.insertAdjacentHTML('beforeend','<em>e</em>'); expect(a.lastElementChild.tagName==='EM', "
      "'insertAdjacentHTML');\n"
      "a.querySelector('em').remove(); expect(a.querySelector('em')===null, 'remove');\n"
      "expect(d.createComment('c').nodeType===8 && d.createComment('c').nodeValue==='c', 'comment');\n"
      "var t=d.createTextNode('abc'); t.textContent='xyz'; expect(t.nodeValue==='xyz', 'text node textContent');\n");
}

static void test_cyclic_insertion_is_rejected(void)
{
  run_script("var d=document, p=d.createElement('div'), q=d.createElement('div'), r=d.createElement('div');\n"
             "p.appendChild(q); q.appendChild(r);\n"
             "var threw=false; try{ r.appendChild(p) }catch(e){ threw=true }\n"
             "expect(threw, 'appendChild of an ancestor throws');\n"
             "threw=false; try{ p.insertBefore(p, null) }catch(e){ threw=true }\n"
             "expect(threw, 'appendChild of self throws');\n"
             "q.insertBefore(r, r);\n"
             "expect(q.firstChild===r && r.nextSibling===null, 'insertBefore(x, x) is a no-op');\n"
             "expect(p.outerHTML.length>0, 'tree still serializes');\n");
}

static void test_dialog_open_state_returnvalue(void)
{
  run_script(
      "var d=document, dlg=d.createElement('dialog'), log=[];\n"
      "d.body.appendChild(dlg);\n"
      "dlg.addEventListener('close', function(){ log.push('close') });\n"
      "dlg.addEventListener('cancel', function(){ log.push('cancel') });\n"
      "expect(dlg.open===false && dlg.returnValue==='', 'closed by default');\n"
      "dlg.showModal();\n"
      "expect(dlg.open===true && dlg.matches(':modal'), 'showModal opens modally');\n"
      "dlg.close('ok');\n"
      "expect(dlg.open===false && dlg.returnValue==='ok' && log.join()==='close', 'close sets returnValue and fires "
      "close');\n"
      "dlg.close('again'); expect(dlg.returnValue==='ok' && log.length===1, 'closing a closed dialog is a no-op');\n"
      "dlg.show(); expect(dlg.open && !dlg.matches(':modal'), 'show is non-modal');\n"
      "var veto=true; dlg.addEventListener('cancel', function(e){ if(veto) e.preventDefault() });\n"
      "dlg.requestClose(); expect(dlg.open===true && log.join()==='close,cancel', 'cancel can be prevented');\n"
      "veto=false; dlg.requestClose(); expect(dlg.open===false, 'requestClose closes');\n");
}

static void test_popover_api_show_hide(void)
{
  run_script(
      "var d=document, a=d.createElement('div'), b=d.createElement('div'), log=[];\n"
      "a.id='pa'; b.id='pb'; a.popover=''; b.setAttribute('popover','auto'); d.body.appendChild(a); "
      "d.body.appendChild(b);\n"
      "expect(a.popover==='auto' && d.createElement('p').popover===null, 'popover reflects the attribute');\n"
      "a.addEventListener('beforetoggle', function(e){ log.push('before:'+e.newState) });\n"
      "a.addEventListener('toggle', function(e){ log.push('toggle:'+e.newState) });\n"
      "a.showPopover();\n"
      "expect(a.matches(':popover-open') && log.join()==='before:open,toggle:open', 'showPopover opens: '+log);\n"
      "b.showPopover();\n"
      "expect(b.matches(':popover-open') && !a.matches(':popover-open'), 'opening an auto popover closes the "
      "others');\n"
      "expect(b.togglePopover()===false && !b.matches(':popover-open'), 'togglePopover closes');\n"
      "expect(b.togglePopover(true)===true, 'togglePopover(force)');\n"
      "var btn=d.createElement('button'); btn.setAttribute('popovertarget','pa'); d.body.appendChild(btn);\n"
      "btn.click();\n"
      "expect(a.matches(':popover-open') && !b.matches(':popover-open'), 'popovertarget invoker toggles its "
      "target and dismisses others');\n"
      "d.body.click();\n"
      "expect(!a.matches(':popover-open'), 'clicking outside light-dismisses');\n"
      "var m=d.createElement('div'); m.setAttribute('popover','manual'); d.body.appendChild(m);\n"
      "m.showPopover(); d.body.click(); expect(m.matches(':popover-open'), 'manual popovers ignore light "
      "dismiss');\n"
      "var plain=d.createElement('div'); plain.showPopover(); expect(!plain.matches(':popover-open'), 'no popover "
      "attribute: no-op');\n");
}

static void test_custom_elements_lifecycle_and(void)
{
  run_script("var log=[];\n"
             "class XFoo extends HTMLElement {\n"
             "  connectedCallback(){ log.push('c') } disconnectedCallback(){ log.push('d') }\n"
             "}\n"
             "customElements.define('x-foo', XFoo);\n"
             "var e=document.createElement('x-foo'); document.body.appendChild(e);\n"
             "expect(log.join()==='c', 'connected on insert');\n"
             "var wrap=document.createElement('div'); wrap.appendChild(document.createElement('x-foo'));\n"
             "e.remove(); expect(log.join()==='c,d', 'disconnected on remove');\n"
             "document.body.appendChild(e); document.body.innerHTML=''; expect(log.join()==='c,d,c,d', 'disconnected "
             "on innerHTML clear: '+log);\n"
             "var err=''; try{ customElements.define('nodash', class extends HTMLElement{}) }catch(x){ err=x.name }\n"
             "expect(err==='SyntaxError', 'name needs a hyphen: '+err);\n"
             "err=''; try{ customElements.define('x-foo', class extends HTMLElement{}) }catch(x){ err=x.name }\n"
             "expect(err==='NotSupportedError', 'redefinition throws: '+err);\n");
}

static void test_timers_unused_slots_hold(void)
{
  run_script("var n=0; setTimeout(function(){ n++ }, 10); setInterval(function(){ n++ }, 1000);\n");
  fake_now_ms += 20;
  expect(js_timers_pump() == 1);

  js_page_end();
  dom_free(root);
  free(B->pg);
  free(B);
}

int main(void)
{
  B = calloc(1, sizeof *B);
  B->pg = calloc(1, 1 << 16);
  root = dom_parse(page, (int)strlen(page));
  js_run_document(root, "https://example.com/dir/p.html", fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

  test("document.cookie reads, writes and hides HttpOnly", test_document_cookie_reads_writes);
  test("document node properties", test_document_node_properties);
  test("fragments, cloning and tree editing", test_fragments_cloning_and_tree);
  test("cyclic insertion is rejected", test_cyclic_insertion_is_rejected);
  test("dialog open state, returnValue and events", test_dialog_open_state_returnvalue);
  test("popover API: show, hide, toggle, events and light dismiss", test_popover_api_show_hide);
  test("custom elements: lifecycle and registry validation", test_custom_elements_lifecycle_and);
  test("timers: unused slots hold no stale pointers at teardown", test_timers_unused_slots_hold);

  return 0;
}
