/* Host test for <input type=file> in the browser bindings: File, FileList, input.files and FormData with files. */
#define OWOS_SUITE_BINDINGS
#include "dom.h"
#include "jsint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int js_timers_pump(void);

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

static const char page[] =
    "<html><body><form id=f method=post enctype='multipart/form-data'>"
    "<input type=file id=i name=up multiple><input name=t value='x y'>"
    "<input type=file id=e name=none></form><input id=txt><script>var ready=1</script></body></html>";

#define PATH_A "build/tests/run/files_a.txt"
#define PATH_B "build/tests/run/files_b.png"

static struct dom_node *root;

static struct dom_node *by_id(struct dom_node *n, const char *id)
{
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    const char *v = c->type == DOM_ELEMENT ? dom_get_attr(c, "id") : 0;
    if (v && !strcmp(v, id))
      return c;
    struct dom_node *r = by_id(c, id);
    if (r)
      return r;
  }
  return 0;
}

static void write_file(const char *path, const char *data, int len)
{
  FILE *f = fopen(path, "wb");
  expect(f != 0);
  if (f) {
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
  }
}

static void choose(struct dom_node *in, const char *const *paths, const char *const *rels, int n)
{
  struct dom_files *f = calloc(1, sizeof *f);
  f->n = n;
  f->path = calloc((size_t)n, sizeof *f->path);
  f->rel = calloc((size_t)n, sizeof *f->rel);
  for (int i = 0; i < n; i++) {
    f->path[i] = strdup(paths[i]);
    f->rel[i] = strdup(rels[i]);
  }
  dom_files_free(in->files);
  in->files = f;
  js_files_changed(in);
}

static void test_input_files(void)
{
  struct dom_node *in = by_id(root, "i");
  expect(in != 0);
  run_script("var i=document.getElementById('i');\n"
             "expect(i.files.length===0 && i.files.item(0)===null, 'no selection yet');\n"
             "expect(document.getElementById('txt').files===null, 'only file inputs have files');\n");
  write_file(PATH_A, "hello", 5);
  write_file(PATH_B, "\x01\x02\x03", 3);
  const char *paths[] = {PATH_A, PATH_B}, *rels[] = {"", "dir/files_b.png"};
  choose(in, paths, rels, 2);
  run_script("var i=document.getElementById('i'), fl=i.files;\n"
             "expect(fl.length===2 && fl===i.files, 'FileList is stable: '+fl.length);\n"
             "expect(fl[0].name==='files_a.txt' && fl[0].size===5 && fl[0].type==='text/plain', 'name, size, type');\n"
             "expect(fl[0] instanceof File && fl[0] instanceof Blob && fl.item(1)===fl[1], 'File is a Blob');\n"
             "expect(fl[0].webkitRelativePath==='' && fl[1].webkitRelativePath==='dir/files_b.png', 'relative path');\n"
             "expect(fl[1].size===3 && fl[1].type==='image/png', 'type from the extension: '+fl[1].type);\n"
             "expect(Array.prototype.map.call(fl,function(f){return f.name}).join()==='files_a.txt,files_b.png', "
             "'array-like');\n");
  choose(in, paths, rels, 1);
  run_script("expect(document.getElementById('i').files.length===1, 'a new selection replaces the list');\n");
}

static void test_file_constructor(void)
{
  run_script(
      "var f=new File(['ab','c'],'n.txt',{type:'text/plain'});\n"
      "expect(f.name==='n.txt' && f.size===3 && f.type==='text/plain' && f instanceof Blob, 'File(parts, name)');\n"
      "expect(f.webkitRelativePath==='' && typeof f.lastModified==='number', 'File fields');\n"
      "expect(f.lastModified===1720000000000, 'defaults to now: '+f.lastModified);\n"
      "expect(new File([], 'x', {lastModified: 1234}).lastModified===1234, 'lastModified option');\n");
}

static void test_formdata_with_files(void)
{
  struct dom_node *in = by_id(root, "i");
  const char *paths[] = {PATH_A, PATH_B}, *rels[] = {"", ""};
  choose(in, paths, rels, 2);
  run_script(
      "var fd=new FormData(document.getElementById('f'));\n"
      "var ups=fd.getAll('up');\n"
      "expect(ups.length===2 && ups[0].name==='files_a.txt' && ups[1].size===3, 'files become entries');\n"
      "expect(fd.get('t')==='x y', 'text fields stay strings');\n"
      "var none=fd.get('none'); expect(none instanceof File && none.name==='' && none.size===0, 'empty input');\n"
      "fd.append('b', new Blob(['zz']));\n"
      "expect(fd.get('b').name==='blob' && fd.get('b') instanceof File, 'appended Blob is named blob');\n"
      "fd.append('c', new Blob(['zz']), 'c.bin'); expect(fd.get('c').name==='c.bin', 'filename argument');\n"
      "fd.set('b', 'plain'); expect(fd.get('b')==='plain', 'set stringifies other values');\n"
      "window.mp=null; fetch('https://example.com/echo',{method:'POST',body:fd}).then(function(r){ return "
      "r.text(); }).then(function(t){ window.mp=t; });\n");
  for (int i = 0; i < 20; i++)
    js_timers_pump();
  run_script(
      "var all=window.mp; expect(all!==null, 'echo answered');\n"
      "var ct=all.split(' ')[2], t=all.slice(all.indexOf('[')+1, all.lastIndexOf(']'));\n"
      "expect(ct==='multipart/form-data;', 'content type: '+ct);\n"
      "var b=all.match(/boundary=(-+OwOSFormBoundary[0-9a-f]{16})/)[1];\n"
      "expect(t.indexOf('--'+b+'\\r\\nContent-Disposition: form-data; name=\"up\"; filename=\"files_a.txt\"\\r\\n"
      "Content-Type: text/plain\\r\\n\\r\\nhello\\r\\n')===0, 'first file part: '+JSON.stringify(t.slice(0,160)));\n"
      "expect(t.indexOf('name=\"up\"; filename=\"files_b.png\"\\r\\nContent-Type: image/png')>0, "
      "'second file part');\n"
      "expect(t.indexOf('name=\"t\"\\r\\n\\r\\nx y\\r\\n')>0, 'text part');\n"
      "expect(t.indexOf('name=\"none\"; filename=\"\"')>0, 'empty file part');\n"
      "expect(t.slice(-(b.length+6))==='--'+b+'--\\r\\n', 'closing boundary');\n");
}

static void test_filereader(void)
{
  run_script(
      "window.fr={};\n"
      "var seq=[], r=new FileReader();\n"
      "expect(r.readyState===0 && r.result===null && FileReader.DONE===2 && r instanceof EventTarget, 'initial state');\n"
      "['loadstart','progress','load','loadend'].forEach(function(t){ r.addEventListener(t,function(e){ "
      "seq.push(t+':'+e.loaded+'/'+e.total+':'+(e.target===r)+':'+(e instanceof ProgressEvent)); }); });\n"
      "r.onload=function(e){ fr.onload=r.result; };\n"
      "r.readAsText(new Blob(['h\\u00e9llo']));\n"
      "expect(r.readyState===1 && r.result===null && seq.length===1, 'loading: '+seq);\n"
      "var threw=false; try { r.readAsText(new Blob([])); } catch(e) { threw=e.name==='InvalidStateError'; }\n"
      "expect(threw, 'busy reader throws');\n"
      "try { r.readAsText('x'); expect(false,'non-blob'); } catch(e) { expect(e instanceof TypeError,'non-blob'); }\n"
      "fr.seq=seq; fr.r=r;\n"
      "var a=new FileReader(), b=new FileReader(), c=new FileReader(), d=new FileReader();\n"
      "a.onload=function(){ fr.buf=a.result; }; a.readAsArrayBuffer(new Blob([new Uint8Array([1,2,255])]));\n"
      "b.onload=function(){ fr.bin=b.result; }; b.readAsBinaryString(new Blob([new Uint8Array([104,200])]));\n"
      "c.onload=function(){ fr.url=c.result; }; c.readAsDataURL(new Blob(['hi'],{type:'text/plain'}));\n"
      "d.onabort=function(){ fr.abort=d.readyState+':'+d.result; }; d.onload=function(){ fr.aborted=false; };\n"
      "d.readAsText(new Blob(['x'])); d.abort();\n");
  for (int i = 0; i < 20; i++)
    js_timers_pump();
  run_script(
      "expect(fr.seq.join()==='loadstart:0/6:true:true,progress:6/6:true:true,load:6/6:true:true,loadend:6/6:true:true',"
      " 'event order: '+fr.seq);\n"
      "expect(fr.r.readyState===2 && fr.r.result==='h\\u00e9llo' && fr.onload===fr.r.result, 'text result');\n"
      "expect(fr.buf instanceof ArrayBuffer && new Uint8Array(fr.buf).join()==='1,2,255', 'arraybuffer');\n"
      "expect(fr.bin==='h\\u00c8', 'binary string: '+fr.bin);\n"
      "expect(fr.url==='data:text/plain;base64,aGk=', 'data url: '+fr.url);\n"
      "expect(fr.abort==='2:null' && fr.aborted===undefined, 'abort: '+fr.abort);\n");
}

static void test_datatransfer(void)
{
  run_script(
      "var dt=new DataTransfer();\n"
      "expect(dt.dropEffect==='none' && dt.effectAllowed==='none' && dt.items.length===0 && dt.types.length===0, 'empty');\n"
      "dt.setData('text/plain','hello'); dt.setData('Text','again'); dt.setData('URL','# c\\nhttp://a.test/x\\nhttp://b.test');\n"
      "expect(dt.getData('text')==='again' && dt.getData('TEXT/PLAIN')==='again', 'text alias, replaced in place');\n"
      "expect(dt.getData('url')==='http://a.test/x' && dt.getData('missing')==='', 'url alias, missing');\n"
      "expect(dt.types.join()==='text/plain,text/uri-list' && Object.isFrozen(dt.types), 'types');\n"
      "var f=new File(['abc'],'a.txt',{type:'text/plain'});\n"
      "var it=dt.items.add(f);\n"
      "expect(it.kind==='file' && it.getAsFile()===f && dt.files.length===1 && dt.files[0]===f, 'add file');\n"
      "expect(dt.types.join()==='text/plain,text/uri-list,Files' && dt.items.length===3 && dt.items[2]===it, 'Files type');\n"
      "expect(dt.items[0].kind==='string' && dt.items[0].type==='text/plain', 'string item');\n"
      "window.got=null; dt.items[0].getAsString(function(s){ window.got=s; });\n"
      "var threw=false; try { dt.items.add('x','text/plain'); } catch(e) { threw=e.name==='NotSupportedError'; }\n"
      "expect(threw, 'duplicate type');\n"
      "dt.items.add('<b>', 'text/html'); expect(dt.getData('text/html')==='<b>', 'add string');\n"
      "dt.clearData('text'); expect(dt.getData('text')==='' && dt.getData('text/html')==='<b>', 'clearData(format)');\n"
      "dt.items.remove(0); expect(dt.items.length===2 && dt.items[2]===undefined && dt.items[0].kind==='file', 'remove');\n"
      "dt.clearData(); expect(dt.types.join()==='Files' && dt.files.length===1, 'clearData keeps files');\n"
      "dt.items.clear(); expect(dt.items.length===0 && dt.files.length===0 && dt.types.length===0, 'clear');\n"
      "dt.dropEffect='copy'; dt.dropEffect='bogus'; dt.effectAllowed='copyMove'; dt.effectAllowed='bogus';\n"
      "expect(dt.dropEffect==='copy' && dt.effectAllowed==='copyMove', 'effects validated');\n"
      "expect([].concat(Array.from(dt.items)).length===0, 'iterable');\n"
      "var ev=new DragEvent('drop',{dataTransfer:dt}); expect(ev.dataTransfer===dt, 'DragEvent.dataTransfer');\n"
      "var ce=new ClipboardEvent('paste',{clipboardData:dt}); expect(ce.clipboardData===dt && ce instanceof Event, 'ClipboardEvent');\n"
      "var t=0; try { new DataTransferItem(); } catch(e) { t=e instanceof TypeError; } expect(t, 'illegal constructor');\n");
  for (int i = 0; i < 5; i++)
    js_timers_pump();
  run_script("expect(window.got==='again', 'getAsString calls back: '+window.got);\n");
}

int main(void)
{
  B = calloc(1, sizeof *B);
  B->pg = calloc(1, 1 << 16);
  root = dom_parse(page, (int)strlen(page));
  js_run_document(root, "https://example.com/dir/p.html", fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

  test("input.files lists the chosen files as File objects", test_input_files);
  test("File constructor", test_file_constructor);
  test("FormData carries files and serializes as multipart", test_formdata_with_files);
  test("FileReader reads Blobs and fires progress events", test_filereader);
  test("DataTransfer, its items and DragEvent/ClipboardEvent", test_datatransfer);
  return 0;
}
