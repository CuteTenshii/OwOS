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

static void test_replace_children(void)
{
  run_script("var d=document, h=d.createElement('div'); h.innerHTML='<a></a><b></b><i></i>';\n"
             "var b=h.querySelector('b'), s=d.createElement('span');\n"
             "h.replaceChildren(b, s, 'txt');\n"
             "expect(h.childNodes.length===3 && h.firstChild===b && h.childNodes[1]===s && "
             "h.lastChild.nodeValue==='txt', 'new children in order');\n"
             "expect(h.querySelector('a')===null && h.querySelector('i')===null, 'old children removed');\n"
             "h.replaceChildren(); expect(h.childNodes.length===0, 'no arguments empties');\n");
}

static void test_details_summary_toggle(void)
{
  run_script("var d=document, det=d.createElement('details'), sum=d.createElement('summary'), log=[];\n"
             "det.appendChild(sum); det.appendChild(d.createElement('p')); d.body.appendChild(det);\n"
             "det.addEventListener('toggle', function(){ log.push(det.open) });\n"
             "expect(det.open===false, 'closed by default');\n"
             "sum.click(); expect(det.open===true && log.join()==='true', 'summary click opens');\n"
             "sum.click(); expect(det.open===false && log.join()==='true,false', 'and closes');\n"
             "det.open=true; expect(det.hasAttribute('open'), 'open reflects the attribute');\n"
             "d.createElement('p').click();\n");
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

static void test_custom_elements_nested_upgrade(void)
{
  run_script("var log=[];\n"
             "class XNest extends HTMLElement {\n"
             "  kid = document.createElement('i');\n"
             "  connectedCallback(){ log.push('c'); this.append(this.kid) }\n"
             "}\n"
             "customElements.define('x-nest', XNest);\n"
             "var h=document.createElement('div'); h.innerHTML='<p><x-nest></x-nest></p>';\n"
             "expect(log.length===0 && h.firstChild.firstChild.kid, 'innerHTML upgrades detached descendants: '+log);\n"
             "document.body.appendChild(h);\n"
             "expect(log.join()==='c' && h.firstChild.firstChild.childNodes.length===1, 'insert connects descendants: "
             "'+log);\n"
             "var cl=h.cloneNode(true); expect(cl.firstChild.firstChild.kid, 'clone upgrades descendants');\n"
             "document.body.appendChild(cl);\n"
             "expect(log.join()==='c,c', 'cloned subtree connects: '+log);\n"
             "document.body.innerHTML='<section><x-nest></x-nest></section>';\n"
             "expect(log.join()==='c,c,c', 'innerHTML on a connected parent connects: '+log);\n");
}

static void test_blob_object_urls(void)
{
  unsigned char *body;
  char type[64];
  int len = 0;
  js_value url;
  run_script("var u=window.u=URL.createObjectURL(new Blob(['hello',' ','world'],{type:'text/plain'}));\n"
             "expect(u.indexOf('blob:https://example.com/')===0, 'blob: url on the page origin: '+u);\n"
             "expect(URL.createObjectURL(new Blob(['x']))!==u, 'urls are unique');\n"
             "var threw=false; try{ URL.createObjectURL('nope') }catch(e){ threw=true }\n"
             "expect(threw, 'non-Blob argument throws');\n");
  url = js_get(g_ctx, js_obj_value(g_ctx->global), "u");
  expect(url.type == JV_STR);
  if (url.type != JV_STR)
    return;
  body = blobstore_get(url.u.str->data, &len, type, sizeof type);
  expect(body != 0 && len == 11 && memcmp(body, "hello world", 11) == 0);
  expect(strcmp(type, "text/plain") == 0);
  free(body);
  run_script("URL.revokeObjectURL(u);\n");
  expect(blobstore_get(url.u.str->data, &len, type, sizeof type) == 0);
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

static void test_xpath(void)
{
  run_script(
      "var d=new DOMParser().parseFromString(\"<html><head><title>t</title></head><body><div id=a class='x y'>hi"
      "<span>s</span></div><script>var ready=1</script></body></html>\",'text/html'), R=XPathResult;\n"
      "function snap(x,c){ var r=d.evaluate(x,c||d,null,R.ORDERED_NODE_SNAPSHOT_TYPE,null); var o=[];\n"
      "  for(var i=0;i<r.snapshotLength;i++) o.push(r.snapshotItem(i)); return o }\n"
      "expect(snap('//span').length===1 && snap('//span')[0].tagName==='SPAN', 'descendant name test');\n"
      "expect(snap('/html/body/div')[0]===d.getElementById('a'), 'absolute child path');\n"
      "expect(snap(\"//div[@id='a']/span\").length===1, 'attribute predicate');\n"
      "expect(snap('//div[contains(@class,\"y\")]').length===1, 'contains() on attribute');\n"
      "expect(snap('//*[self::title or self::span]').length===2, 'or + self axis');\n"
      "expect(snap('//title | //span').length===2 && snap('//title | //span')[0].tagName==='TITLE', 'union order');\n"
      "expect(snap('//body/*').length===2 && snap('//body/*[last()]')[0].tagName==='SCRIPT', 'last()');\n"
      "expect(snap('//span/ancestor::*').length===3 && snap('//span/ancestor::*')[0].tagName==='HTML', 'ancestor');\n"
      "expect(snap('//span/preceding-sibling::text()')[0].nodeValue==='hi', 'preceding-sibling text()');\n"
      "expect(snap('//head/following::div').length===1, 'following axis');\n"
      "expect(snap('//div/..')[0].tagName==='BODY', 'parent step');\n"
      "var n=d.evaluate('count(//div/node())',d,null,R.NUMBER_TYPE,null).numberValue; expect(n===2, 'count(): '+n);\n"
      "expect(d.evaluate('string(//span)',d,null,R.STRING_TYPE,null).stringValue==='s', 'string()');\n"
      "expect(d.evaluate('//div/@id',d,null,R.STRING_TYPE,null).stringValue==='a', 'attribute string value');\n"
      "expect(d.evaluate('1 + 2 * 3 - 4 div 2 mod 3',d,null,R.NUMBER_TYPE,null).numberValue===5, 'arithmetic');\n"
      "expect(d.evaluate('//span',d,null,R.BOOLEAN_TYPE,null).booleanValue===true, 'boolean');\n"
      "expect(d.evaluate('//div',d,null,R.FIRST_ORDERED_NODE_TYPE,null).singleNodeValue.id==='a', 'first node');\n"
      "var it=d.evaluate('//body/*',d,null,R.ORDERED_NODE_ITERATOR_TYPE,null), c=0; while(it.iterateNext()) c++;\n"
      "expect(c===2, 'iterator');\n"
      "var a=d.getElementById('a');\n"
      "expect(snap('span',a).length===1 && snap('./span',a).length===1, 'relative to context');\n"
      "expect(d.evaluate('//x',d,null,R.ANY_TYPE,null).resultType===R.UNORDERED_NODE_ITERATOR_TYPE, 'ANY node-set');\n"
      "expect(d.evaluate('2*3',d,null,R.ANY_TYPE,null).resultType===R.NUMBER_TYPE, 'ANY number');\n"
      "var e=new XPathEvaluator(), x=e.createExpression('//span');\n"
      "expect(x.evaluate(d,R.ORDERED_NODE_SNAPSHOT_TYPE).snapshotLength===1, 'XPathExpression');\n"
      "expect(e.evaluate('//span',d,null,R.ORDERED_NODE_SNAPSHOT_TYPE,null).snapshotLength===1, 'XPathEvaluator');\n"
      "var threw=false; try{ d.evaluate('//[',d,null,0,null) }catch(err){ threw=err.name==='SyntaxError' }\n"
      "expect(threw, 'bad expression throws SyntaxError');\n"
      "var q=new DOMParser().parseFromString('<p><b>x</b><b>y</b></p>','text/html');\n"
      "expect(q.evaluate('//b[2]',q,null,R.FIRST_ORDERED_NODE_TYPE,null).singleNodeValue.textContent==='y', '[n]');\n");
}

static void test_event_target(void)
{
  run_script("var log=[]; var t=new EventTarget();\n"
             "var h=function(e){ log.push(e.type+':'+(e.target===t)+':'+(this===t)); };\n"
             "t.addEventListener('ping',h); t.addEventListener('ping',h);\n"
             "expect(t.dispatchEvent(new Event('ping')), 'dispatch returns true');\n"
             "expect(log.join()==='ping:true:true,ping:true:true', 'listeners fire with target and this');\n"
             "t.removeEventListener('ping',h); log.length=0; t.dispatchEvent(new Event('ping'));\n"
             "expect(log.length===0, 'removed listener stays silent');\n"
             "var wlog=0; window.addEventListener('ping',function(){ wlog++ });\n"
             "t.addEventListener('ping',function(){}); t.dispatchEvent(new Event('ping',{bubbles:true}));\n"
             "expect(wlog===0, 'plain targets do not bubble to window');\n"
             "var once=0; t.addEventListener('c',function(e){ once++; e.preventDefault(); });\n"
             "expect(t.dispatchEvent(new Event('c',{cancelable:true}))===false, 'preventDefault makes dispatch return "
             "false');\n"
             "class Bus extends EventTarget { constructor(){ super(); this.n=0; } }\n"
             "var b=new Bus(); b.addEventListener('x',function(){ b.n++ }); b.dispatchEvent(new Event('x'));\n"
             "expect(b.n===1 && b instanceof EventTarget && b instanceof Bus, 'subclass dispatches');\n"
             "expect(document.body instanceof EventTarget && window instanceof EventTarget && new "
             "AbortController().signal instanceof EventTarget, 'DOM objects inherit EventTarget');\n");
}

static void test_namespaces(void)
{
  run_script(
      "var SVG='http://www.w3.org/2000/svg', XL='http://www.w3.org/1999/xlink', H='http://www.w3.org/1999/xhtml';\n"
      "var d=document, div=d.createElement('div');\n"
      "expect(div.namespaceURI===H && div.localName==='div' && div.prefix===null, 'createElement is HTML');\n"
      "var s=d.createElementNS(SVG,'svg'); s.appendChild(d.createElementNS(SVG,'path'));\n"
      "expect(s.namespaceURI===SVG && s.tagName==='svg' && s.firstChild.namespaceURI===SVG, 'createElementNS keeps "
      "ns');\n"
      "div.innerHTML='<svg><circle></circle></svg>';\n"
      "expect(div.firstChild.namespaceURI===SVG && div.firstChild.firstChild.namespaceURI===SVG, 'parsed svg "
      "subtree');\n"
      "var p=d.createElementNS(SVG,'x:rect'); expect(p.prefix==='x' && p.localName==='rect' && p.tagName==='x:rect', "
      "'prefix');\n"
      "var bad=false; try{ d.createElementNS(null,'x:y') }catch(e){ bad=e.name==='NamespaceError' }\n"
      "expect(bad, 'prefix without namespace throws');\n"
      "s.setAttributeNS(XL,'xlink:href','#a'); s.setAttributeNS(null,'href','plain');\n"
      "expect(s.getAttributeNS(XL,'href')==='#a' && s.getAttributeNS(null,'href')==='plain', 'attribute ns lookups');\n"
      "expect(s.hasAttributeNS(XL,'href') && !s.hasAttributeNS(XL,'nope') && s.getAttribute('xlink:href')==='#a', "
      "'hasAttributeNS');\n"
      "s.setAttributeNS(XL,'xlink:href','#b'); expect(s.getAttributeNS(XL,'href')==='#b' && s.attributes.length===2, "
      "'update');\n"
      "var at=s.attributes[0]; expect(at.namespaceURI===XL && at.prefix==='xlink' && at.localName==='href', 'attr ns "
      "fields');\n"
      "s.removeAttributeNS(XL,'href'); expect(!s.hasAttributeNS(XL,'href') && s.getAttribute('href')==='plain', "
      "'removeAttributeNS');\n"
      "var c=s.cloneNode(true); expect(c.namespaceURI===SVG && c.firstChild.namespaceURI===SVG, 'clone keeps ns');\n"
      "expect(d.getElementsByTagNameNS(SVG,'*').length===0, 'detached svg is not in the document');\n"
      "d.body.appendChild(s);\n"
      "expect(d.getElementsByTagNameNS(SVG,'path').length===1 && d.getElementsByTagNameNS('*','path').length===1 &&\n"
      "       d.getElementsByTagNameNS(H,'path').length===0, 'getElementsByTagNameNS');\n"
      "expect(s.lookupNamespaceURI(null)===SVG && div.lookupNamespaceURI(null)===H && s.isDefaultNamespace(SVG), "
      "'lookups');\n"
      "var x=new DOMParser().parseFromString('<r xmlns=\"urn:a\" xmlns:q=\"urn:b\"><q:k/><k/></r>','text/xml');\n"
      "var r=x.documentElement;\n"
      "expect(r.namespaceURI==='urn:a' && r.firstChild.namespaceURI==='urn:b' && r.lastChild.namespaceURI==='urn:a', "
      "'xml doc ns');\n"
      "expect(r.lookupNamespaceURI('q')==='urn:b' && r.lookupPrefix('urn:b')==='q', 'lookupPrefix/Namespace');\n"
      "var ser=new XMLSerializer();\n"
      "expect(ser.serializeToString(r)==='<r xmlns=\"urn:a\" xmlns:q=\"urn:b\"><q:k/><k/></r>', 'xml roundtrip: "
      "'+ser.serializeToString(r));\n"
      "var g=d.createElementNS(SVG,'g'); g.appendChild(d.createElementNS(SVG,'path')); "
      "g.setAttributeNS(XL,'xlink:href','#z');\n"
      "expect(ser.serializeToString(g)==='<g xmlns=\"'+SVG+'\" xmlns:xlink=\"'+XL+'\" xlink:href=\"#z\"><path/></g>', "
      "'svg decls: '+ser.serializeToString(g));\n"
      "expect(ser.serializeToString(d.createElement('br'))==='<br xmlns=\"'+H+'\" />', 'html void: "
      "'+ser.serializeToString(d.createElement('br')));\n"
      "var h=d.createElement('p'); h.textContent='a < b'; expect(ser.serializeToString(h).indexOf('a &lt; b')>0, 'text "
      "escaped');\n"
      "var doc=new DOMParser().parseFromString('<r xmlns=\"urn:a\" xmlns:q=\"urn:b\"><q:k/><k/><svg:g "
      "xmlns:svg=\"'+SVG+'\"/></r>','text/xml');\n"
      "var R=XPathResult, res=function(pfx){ return {a:'urn:a',b:'urn:b',s:SVG}[pfx]||null };\n"
      "function n(e,ctx){ return doc.evaluate(e,ctx||doc,res,R.NUMBER_TYPE,null).numberValue }\n"
      "expect(n('count(//b:k)')===1 && n('count(//a:k)')===1 && n('count(//s:g)')===1, 'prefixed name tests: "
      "'+n('count(//b:k)'));\n"
      "expect(n('count(//b:*)')===1 && n('count(//*)')===4, 'prefix wildcard');\n"
      "expect(n('count(//k)')===0, 'unprefixed does not match foreign ns');\n"
      "expect(doc.evaluate('namespace-uri(//b:k)',doc,res,R.STRING_TYPE,null).stringValue==='urn:b', "
      "'namespace-uri()');\n"
      "expect(doc.evaluate('local-name(//b:k)',doc,res,R.STRING_TYPE,null).stringValue==='k', 'local-name()');\n"
      "expect(doc.evaluate('name(//b:k)',doc,res,R.STRING_TYPE,null).stringValue==='q:k', 'name()');\n"
      "expect(doc.evaluate('count(//*[local-name()=\"k\"])',doc,res,R.NUMBER_TYPE,null).numberValue===2, 'local-name "
      "predicate');\n"
      "var "
      "viaNode=doc.evaluate('count(//"
      "q:k)',doc,doc.createNSResolver(doc.documentElement),R.NUMBER_TYPE,null).numberValue;\n"
      "expect(viaNode===1, 'createNSResolver(node)');\n"
      "var obj=doc.evaluate('count(//b:k)',doc,{lookupNamespaceURI:res},R.NUMBER_TYPE,null).numberValue; "
      "expect(obj===1, 'resolver object');\n"
      "var nse=false; try{ doc.evaluate('//zz:k',doc,null,0,null) }catch(e){ nse=e.name==='NamespaceError' }\n"
      "expect(nse, 'unresolved prefix throws NamespaceError');\n"
      "var thr=false; try{ doc.evaluate('//b:k',doc,function(){ throw new Error('boom') },0,null) }catch(e){ "
      "thr=e.message==='boom' }\n"
      "expect(thr, 'resolver exceptions propagate');\n"
      "var re=doc.evaluate('count(//b:k)',doc,function(p){ return doc.evaluate('count(//a:k)',doc,function(){return "
      "'urn:a'},R.NUMBER_TYPE,null).numberValue ? 'urn:b' : null },R.NUMBER_TYPE,null).numberValue;\n"
      "expect(re===1, 'reentrant evaluation from a resolver');\n");
}

static void test_names_keep_case_and_length(void)
{
  run_script(
      "var SVG='http://www.w3.org/2000/svg', d=document, ser=new XMLSerializer();\n"
      "var g=d.createElementNS(SVG,'linearGradient'); g.setAttribute('gradientUnits','userSpaceOnUse');\n"
      "expect(g.tagName==='linearGradient' && g.localName==='linearGradient', 'foreign tag keeps case: '+g.tagName);\n"
      "expect(g.getAttribute('gradientunits')==='userSpaceOnUse' && g.attributes[0].name==='gradientUnits', 'attribute "
      "case');\n"
      "expect(ser.serializeToString(g)==='<linearGradient xmlns=\"'+SVG+'\" gradientUnits=\"userSpaceOnUse\"/>', 'case "
      "in output: '+ser.serializeToString(g));\n"
      "var h=d.createElement('DIV'); expect(h.tagName==='DIV' && h.localName==='div', 'HTML tags stay "
      "uppercase/lowercase');\n"
      "var long='x-'+'very-long-custom-element-name'; var a=d.createElement(long+'-a'), b=d.createElement(long+'-b');\n"
      "expect(a.localName===long+'-a' && b.localName===long+'-b' && a.cloneNode(false).localName===long+'-a', 'long "
      "names are not truncated');\n"
      "var x=new DOMParser().parseFromString('<Feed><Entry id=\"1\"/></Feed>','text/xml');\n"
      "expect(x.documentElement.tagName==='Feed' && x.documentElement.firstChild.tagName==='Entry', 'xml doc keeps "
      "case');\n"
      "expect(x.evaluate('count(/Feed/Entry)',x,null,XPathResult.NUMBER_TYPE,null).numberValue===1, 'xpath is "
      "case-sensitive in xml');\n"
      "expect(x.evaluate('count(/feed/entry)',x,null,XPathResult.NUMBER_TYPE,null).numberValue===0, 'xpath wrong case "
      "does not match');\n"
      "var s=d.createElement('span'); s.innerHTML='a&nbsp;b &amp; c &copy; d &bogus; e';\n"
      "var out=ser.serializeToString(s);\n"
      "expect(out.indexOf('a&#160;b &amp; c &#169; d &amp;bogus; e')>0, 'entities become numeric refs: '+out);\n");
}

static void test_shadow_dom(void)
{
  run_script(
      "var h=document.createElement('div'); document.body.appendChild(h);\n"
      "var r=h.attachShadow({mode:'open'});\n"
      "expect(r instanceof ShadowRoot && r.nodeType===11 && r.host===h && r.mode==='open', 'attachShadow returns a "
      "ShadowRoot');\n"
      "expect(h.shadowRoot===r, 'open root is reachable');\n"
      "var e=''; try{ h.attachShadow({mode:'open'}) }catch(x){ e=x.name }\n"
      "expect(e==='NotSupportedError', 'second attach throws: '+e);\n"
      "e=''; try{ document.createElement('a').attachShadow({mode:'open'}) }catch(x){ e=x.name }\n"
      "expect(e==='NotSupportedError', 'non-host element throws: '+e);\n"
      "var c=document.createElement('x-closed').attachShadow({mode:'closed'});\n"
      "expect(c.host.shadowRoot===null && c.mode==='closed', 'closed root hidden');\n"
      "r.innerHTML='<b id=in>hi</b><slot name=a id=sa></slot><slot id=sd></slot>';\n"
      "expect(r.getElementById('in').textContent==='hi' && r.querySelector('b'), 'queries inside the root');\n"
      "expect(document.getElementById('in')===null && document.querySelector('b')===null, 'document does not "
      "pierce');\n"
      "expect(h.childNodes.length===0, 'shadow children are not light children');\n"
      "h.innerHTML='<span slot=a>A</span>text<p>P</p>';\n"
      "var sa=r.getElementById('sa'), sd=r.getElementById('sd');\n"
      "expect(sa.assignedNodes().length===1 && sa.assignedElements()[0].tagName==='SPAN', 'named slot takes slotted "
      "child');\n"
      "expect(sd.assignedNodes().length===2 && sd.assignedElements().length===1, 'default slot takes the rest');\n"
      "expect(h.firstChild.assignedSlot===sa && h.lastChild.assignedSlot===sd, 'assignedSlot');\n"
      "var ev=[]; h.addEventListener('ping',function(x){ ev.push('h:'+(x.target===h)) });\n"
      "r.querySelector('b').addEventListener('ping',function(x){ ev.push('b:'+(x.target===this)) });\n"
      "r.querySelector('b').dispatchEvent(new Event('ping',{bubbles:true,composed:true}));\n"
      "expect(ev.join()==='b:true,h:true', 'composed event retargets to the host: '+ev);\n"
      "ev.length=0; r.querySelector('b').dispatchEvent(new Event('ping',{bubbles:true}));\n"
      "expect(ev.join()==='b:true', 'non-composed event stops at the root: '+ev);\n"
      "var path; h.addEventListener('pp',function(x){ path=x.composedPath() });\n"
      "r.querySelector('b').dispatchEvent(new Event('pp',{bubbles:true,composed:true}));\n"
      "expect(path[0]===r.querySelector('b') && path.indexOf(r)>0 && path.indexOf(h)>path.indexOf(r) && "
      "path[path.length-1]===window, 'composedPath');\n"
      "var log=[]; class XIn extends HTMLElement { connectedCallback(){ log.push('c') } }\n"
      "customElements.define('x-in',XIn);\n"
      "r.appendChild(document.createElement('x-in'));\n"
      "expect(log.join()==='c', 'custom elements connect inside a connected shadow root: '+log);\n"
      "var late=document.createElement('div'), lr=late.attachShadow({mode:'open'}); lr.innerHTML='<x-in></x-in>';\n"
      "expect(log.length===1, 'not connected while the host is detached'); document.body.appendChild(late);\n"
      "expect(log.length===2, 'connects when the host does: '+log);\n"
      "class XI extends HTMLElement { constructor(){ super(); this.i=this.attachInternals(); } }\n"
      "customElements.define('x-i',XI); var xi=document.createElement('x-i');\n"
      "expect(xi.i.states.add('on')===xi.i.states && xi.i.states.has('on') && xi.i.checkValidity(), "
      "'ElementInternals');\n"
      "e=''; try{ xi.attachInternals() }catch(x){ e=x.name } expect(e==='NotSupportedError', 'attachInternals once');\n"
      "var sh=new CSSStyleSheet(); sh.replaceSync('p{color:red} .a{margin:0}');\n"
      "expect(sh.cssRules.length===2, 'cssRules: '+sh.cssRules.length);\n"
      "r.adoptedStyleSheets=[sh]; expect(r.adoptedStyleSheets[0]===sh, 'adoptedStyleSheets round-trips'); "
      "window.sroot=r;\n");
  {
    js_value rv = js_get(g_ctx, js_obj_value(g_ctx->global), "sroot");
    char *css = rv.type == JV_OBJ ? shadow_adopted_css((struct dom_node *)rv.u.obj->host) : 0;
    expect(css && strstr(css, "p{color:red}"));
    free(css);
  }
}

static void test_template_content_and_declarative_shadow(void)
{
  const char html[] =
      "<div id=h><template shadowrootmode=open><b>x</b><template shadowrootmode=closed></template></template>"
      "<i>light</i><p><template shadowrootmode=closed><u>deep</u></template></p></div>";
  struct dom_node *tree = dom_parse(html, (int)strlen(html)), *h = tree->first_child;
  run_script("var t=document.createElement('template'); t.innerHTML='<i>a</i><b>b</b>';\n"
             "var f=t.content; expect(f.nodeType===11 && f.childNodes.length===2 && t.childNodes.length===0, 'content "
             "takes the children');\n"
             "expect(t.content===f, 'content is stable');\n"
             "var host=document.createElement('x-t'); "
             "host.attachShadow({mode:'open'}).appendChild(t.content.cloneNode(true));\n"
             "expect(host.shadowRoot.querySelector('b') && f.childNodes.length===2, 'stamping a clone leaves the "
             "template intact');\n");
  dom_attach_declarative_shadows(tree);
  expect(h && h->shadow && h->shadow->shadow_open && h->shadow->host == h);
  expect(h->shadow->first_child && streq(h->shadow->first_child->tag, "b"));
  expect(h->first_child && streq(h->first_child->tag, "i")); /* the template itself is gone */
  struct dom_node *p = h->first_child->next;
  expect(p && p->shadow && !p->shadow->shadow_open && streq(p->shadow->first_child->tag, "u"));
  expect(p->first_child == 0);
  expect(streq(h->shadow->first_child->next->tag, "template")); /* a shadow root is not a valid host */
  dom_free(tree);
}

static void test_slotchange(void)
{
  run_script("window.sc=[]; var h=document.createElement('div'); document.body.appendChild(h);\n"
             "var r=h.attachShadow({mode:'open'}); r.innerHTML='<slot id=s></slot><slot name=n id=n></slot>';\n"
             "r.getElementById('s').addEventListener('slotchange',function(e){ sc.push('s:'+e.bubbles+e.composed) });\n"
             "r.getElementById('n').addEventListener('slotchange',function(e){ sc.push('n') });\n"
             "var i=document.createElement('i'); h.appendChild(i); h.appendChild(document.createElement('b'));\n"
             "expect(sc.length===0, 'slotchange is not synchronous');\n"
             "window.h=h; window.i=i;\n");
  js_timers_pump();
  run_script("expect(sc.join()==='s:truefalse', 'one slotchange for the default slot: '+sc);\n"
             "sc.length=0; h.removeChild(i);\n");
  js_timers_pump();
  run_script("expect(sc.join()==='s:truefalse', 'removal fires too: '+sc);\n"
             "sc.length=0; var u=document.createElement('u'); u.slot='n'; h.appendChild(u);\n");
  js_timers_pump();
  run_script("expect(sc.join()==='n', 'named slot: '+sc);\n"
             "sc.length=0; u.slot='';\n");
  js_timers_pump();
  run_script("expect(sc.join()==='n,s:truefalse' || sc.join()==='s:truefalse,n', 'reassigning notifies both: '+sc);\n");
}

static void test_attribute_selectors_in_script(void)
{
  run_script("var d=document.createElement('div'); d.innerHTML='<a id=l href=\"https://x.test/a.html\" rel=\"nofollow "
             "ext\" lang=en-GB data-m=Dark>x</a><i id=n>y</i>';\n"
             "document.body.appendChild(d); var a=d.querySelector('a');\n"
             "expect(d.querySelector('[href]')===a && d.querySelector('a[href^=https]')===a && "
             "d.querySelector('[href$=\".html\"]')===a, 'exists, ^=, $=');\n"
             "expect(d.querySelector('[href*=\"x.test\"]')===a, '*= quoted');\n"
             "expect(d.querySelector('[href*=test]')===a, '*= bare');\n"
             "expect(d.querySelector('[href*=nope]')===null, '*= miss');\n"
             "expect(d.querySelector('[rel~=ext]')===a && d.querySelector('[rel~=no]')===null, '~=');\n"
             "expect(d.querySelector('[lang|=en]')===a && d.querySelector('[data-m=dark]')===null && "
             "d.querySelector('[data-m=dark i]')===a, '|= and i');\n"
             "expect(a.matches('a[href][rel~=nofollow]') && !a.matches('[href=nope]') && a.closest('[href]')===a, "
             "'matches/closest');\n"
             "expect(d.querySelectorAll(':not([href])').length===1 && d.querySelector(':not([href])').id==='n', "
             "':not([attr])');\n");
}

static void test_tree_walker_and_node_iterator(void)
{
  run_script(
      "var d=document, host=d.createElement('section'); host.innerHTML='<div id=tw>hi<span>s</span></div><p></p>';\n"
      "var a=host.firstChild;\n"
      "expect(NodeFilter.SHOW_ALL===0xFFFFFFFF && NodeFilter.SHOW_TEXT===4 && NodeFilter.FILTER_SKIP===3, 'consts');\n"
      "var w=d.createTreeWalker(a, NodeFilter.SHOW_ELEMENT);\n"
      "expect(w.root===a && w.currentNode===a && w.whatToShow===1 && w.filter===null, 'walker state');\n"
      "expect(w.nextNode().tagName==='SPAN' && w.nextNode()===null && w.currentNode.tagName==='SPAN', 'nextNode');\n"
      "expect(w.previousNode()===a && w.previousNode()===null, 'previousNode stops at root');\n"
      "var t=d.createTreeWalker(a, NodeFilter.SHOW_TEXT), s='';\n"
      "while (t.nextNode()) s+=t.currentNode.data;\n"
      "expect(s==='his', 'text nodes: '+s);\n"
      "var all=d.createTreeWalker(host), n=0;\n"
      "while (all.nextNode()) n++;\n"
      "expect(n===5, 'SHOW_ALL count: '+n);\n"
      "var f=d.createTreeWalker(host, NodeFilter.SHOW_ELEMENT, {acceptNode: function(x){ return x.id==='tw' ? "
      "NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT; }});\n"
      "var fc=f.firstChild();\n"
      "expect(fc.tagName==='P' && f.nextSibling()===null, 'REJECT skips the subtree');\n"
      "var g=d.createTreeWalker(host, NodeFilter.SHOW_ELEMENT, function(x){ return x.id==='tw' ? "
      "NodeFilter.FILTER_SKIP : NodeFilter.FILTER_ACCEPT; });\n"
      "var gc=g.firstChild();\n"
      "expect(gc.tagName==='SPAN' && g.parentNode()===host, 'SKIP descends, parentNode may return root');\n"
      "var p=d.createTreeWalker(host); p.currentNode=a.firstChild;\n"
      "expect(p.parentNode()===a && p.lastChild().tagName==='SPAN' && p.previousSibling().nodeType===3, "
      "'currentNode assignment and siblings');\n"
      "var it=d.createNodeIterator(a, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT), seen=[];\n"
      "expect(it.referenceNode===a && it.pointerBeforeReferenceNode===true, 'iterator state');\n"
      "for (var x; x=it.nextNode();) seen.push(x.nodeType);\n"
      "expect(seen.join()==='1,3,1,3', 'iterator order: '+seen.join());\n"
      "expect(it.previousNode().nodeType===3 && it.previousNode().tagName==='SPAN', 'iterator backwards');\n"
      "var threw=false; try { d.createTreeWalker(a,1,function(){ throw new Error('x'); }).nextNode(); } "
      "catch (e) { threw=e.message==='x'; }\n"
      "expect(threw, 'filter exceptions propagate');\n"
      "var bad=false; try { d.createTreeWalker(null); } catch (e) { bad=e instanceof TypeError; }\n"
      "expect(bad, 'root must be a node');\n");
}

static void test_mutation_observer(void)
{
  run_script(
      "window.mlog=[]; window.host=document.createElement('div'); host.innerHTML='<p id=p>a</p><b></b>';\n"
      "document.body.appendChild(host);\n"
      "var p=host.firstChild, text=p.firstChild;\n"
      "window.mo=new MutationObserver(function(recs, obs){ mlog.push(recs.map(function(r){ return r.type+':'+"
      "(r.attributeName||'')+':'+(r.oldValue===null?'-':r.oldValue)+':'+r.addedNodes.length+r.removedNodes.length; })"
      ".join('|')); window.lastObs=obs; });\n"
      "mo.observe(host, {attributes:true, attributeOldValue:true, childList:true, subtree:true, "
      "characterData:true, characterDataOldValue:true});\n"
      "p.setAttribute('class','x'); p.setAttribute('class','y'); p.removeAttribute('class');\n"
      "text.data='b'; host.appendChild(document.createElement('i')); host.removeChild(host.lastChild);\n"
      "expect(mlog.length===0, 'delivery is asynchronous');\n");
  js_timers_pump();
  run_script(
      "expect(mlog.length===1 && lastObs===mo, 'one batched callback: '+mlog.length);\n"
      "expect(mlog[0]==='attributes:class:-:00|attributes:class:x:00|attributes:class:y:00|characterData::a:00|"
      "childList::-:10|childList::-:01', 'records: '+mlog[0]);\n"
      "mo.observe(host, {attributeFilter:['id'], subtree:true}); p.setAttribute('class','z'); p.id='q';\n"
      "var taken=mo.takeRecords(); expect(taken.length===1 && taken[0].attributeName==='id' && taken[0].target===p, "
      "'takeRecords and attributeFilter');\n"
      "mo.disconnect(); p.id='r'; expect(mo.takeRecords().length===0, 'disconnect stops delivery');\n"
      "var narrow=new MutationObserver(function(r){ mlog.push('narrow'+r.length); });\n"
      "narrow.observe(host, {childList:true}); p.setAttribute('k','v'); p.appendChild(document.createElement('u'));\n"
      "host.appendChild(document.createElement('s'));\n");
  js_timers_pump();
  run_script("expect(mlog[1]==='narrow1', 'childList without subtree ignores deeper changes: '+mlog.slice(1));\n"
             "host.innerHTML='';\n"
             "var late=new MutationObserver(function(r){ mlog.push('clear'+r[0].removedNodes.length); });\n"
             "late.observe(host,{childList:true}); host.innerHTML='<i></i><i></i>'; host.innerHTML='';\n");
  js_timers_pump();
  run_script("expect(mlog[2]==='clear2' || mlog.slice(2).join().indexOf('clear')>=0, 'innerHTML reports removals: "
             "'+mlog.slice(2));\n"
             "var bad=false; try { new MutationObserver(1); } catch (e) { bad=e instanceof TypeError; }\n"
             "try { mo.observe(host, {}); bad=bad&&false; } catch (e) { bad=bad&&e instanceof TypeError; }\n"
             "expect(bad, 'constructor and options validation');\n");
}

static void test_resize_observer(void)
{
  extern int test_box_w, test_box_h;
  test_box_w = 100;
  test_box_h = 50;
  run_script(
      "window.ro=[]; window.el=document.createElement('div'); document.body.appendChild(el);\n"
      "window.obs=new ResizeObserver(function(entries, o){ entries.forEach(function(e){ ro.push(e.target===el && "
      "e.contentRect.width+'x'+e.contentRect.height+'/'+e.borderBoxSize[0].inlineSize); }); "
      "expect(o===obs, 'observer argument'); });\n"
      "obs.observe(el); obs.observe(el);\n");
  expect(js_resize_pump() == 1);
  expect(js_resize_pump() == 0);
  test_box_w = 80;
  expect(js_resize_pump() == 1);
  run_script("expect(ro.join()==='100x50/100,80x50/80', 'reports initial and changed sizes: '+ro);\n"
             "obs.unobserve(el);\n");
  test_box_w = 10;
  expect(js_resize_pump() == 0);
  run_script("obs.observe(el);\n");
  expect(js_resize_pump() == 1);
  run_script("obs.disconnect();\n");
  test_box_w = 20;
  expect(js_resize_pump() == 0);
  test_box_w = test_box_h = 0;
}

static void test_rellist(void)
{
  run_script(
      "var l = document.createElement('link');\n"
      "l.setAttribute('rel', 'stylesheet preload');\n"
      "expect(l.relList.length === 2 && l.relList[1] === 'preload', 'relList reads rel');\n"
      "expect(l.relList.contains('preload') && !l.relList.contains('icon'), 'contains');\n"
      "l.relList.add('icon');\n"
      "expect(l.getAttribute('rel') === 'stylesheet preload icon', 'add writes rel');\n"
      "expect(l.getAttribute('class') === null && l.classList.length === 0, 'class untouched');\n"
      "l.relList.remove('preload');\n"
      "expect(l.relList.toggle('x') && l.relList.value === 'stylesheet icon x', 'remove and toggle');\n"
      "l.relList.value = 'alternate';\n"
      "expect(l.getAttribute('rel') === 'alternate', 'value setter');\n"
      "function throws(f, name) { try { f(); } catch (e) { return e.name === name; } return false; }\n"
      "expect(l.rel === 'alternate' && (l.rel = 'icon', l.relList.contains('icon')), 'rel property');\n"
      "expect(document.createElement('div').rel === undefined, 'rel only on a, area, link, form');\n"
      "l.relList.value = 'alternate';\n"
      "expect(throws(function () { l.relList.add(''); }, 'SyntaxError'), 'empty token');\n"
      "expect(throws(function () { l.relList.add('a b'); }, 'InvalidCharacterError'), 'whitespace token');\n"
      "expect(throws(function () { l.relList.toggle(''); }, 'SyntaxError'), 'toggle validates');\n"
      "expect(throws(function () { l.relList.replace('alternate', 'a b'); }, 'InvalidCharacterError'), 'replace "
      "validates');\n"
      "l.relList.add('x', '' + 1);\n"
      "expect(throws(function () { l.relList.add('y', ''); }, 'SyntaxError') && !l.relList.contains('y'), 'add is all "
      "or nothing');\n"
      "expect(l.relList.supports('stylesheet') && l.relList.supports('PRELOAD') && !l.relList.supports('bogus'), "
      "'supports');\n"
      "expect(throws(function () { l.classList.supports('x'); }, 'TypeError'), 'classList has no supported tokens');\n"
      "expect(document.createElement('div').relList === undefined, 'relList only on a, area, link, form');\n"
      "expect(document.createElement('a').relList.length === 0 && document.createElement('form').relList.length === 0, "
      "'other rel owners');\n");
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
  test("replaceChildren keeps nodes passed back in", test_replace_children);
  test("details toggles from its summary", test_details_summary_toggle);
  test("dialog open state, returnValue and events", test_dialog_open_state_returnvalue);
  test("popover API: show, hide, toggle, events and light dismiss", test_popover_api_show_hide);
  test("custom elements: lifecycle and registry validation", test_custom_elements_lifecycle_and);
  test("custom elements: nested elements upgrade on insert, clone and innerHTML", test_custom_elements_nested_upgrade);
  test("URL.createObjectURL stores Blob bytes until revoked", test_blob_object_urls);
  test("XPath: evaluate, XPathEvaluator and XPathResult", test_xpath);
  test("EventTarget: construction, subclassing, dispatch and isolation", test_event_target);
  test("namespaces: DOM, XMLSerializer and XPath", test_namespaces);
  test("names: case and length are preserved, entities serialize as XML", test_names_keep_case_and_length);
  test("Shadow DOM: roots, slots, event retargeting, lifecycle and adopted sheets", test_shadow_dom);
  test("attribute selectors in querySelector, matches and closest", test_attribute_selectors_in_script);
  test("slotchange fires asynchronously for the affected slots", test_slotchange);
  test("template.content and declarative shadow roots", test_template_content_and_declarative_shadow);
  test("TreeWalker, NodeIterator and NodeFilter", test_tree_walker_and_node_iterator);
  test("MutationObserver", test_mutation_observer);
  test("ResizeObserver", test_resize_observer);
  test("relList reflects the rel attribute", test_rellist);
  test("timers: unused slots hold no stale pointers at teardown", test_timers_unused_slots_hold);

  return 0;
}
