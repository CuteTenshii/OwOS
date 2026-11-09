/* Host test for the canvas JS API: getContext('2d'), state, paths, Path2D, gradients, ImageData, drawImage,
 * <img> loading, OffscreenCanvas and export. Pixels are checked by reading the bitmap, not the JS getters. */
#define OWOS_SUITE_BINDINGS
#include "dom.h"
#include "features/canvas/canvas.h"
#include "font.h"
#include "jsint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern long long fake_now_ms;
int js_timers_pump(void);
int js_workers_pump(void);
void msleep(unsigned ms);

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

static const char page[] = "<html><body><canvas id=c width=40 height=30></canvas><canvas id=d></canvas>"
                           "<img id=im><script>var ready=1</script></body></html>";

static struct dom_node *root;

static struct dom_node *find_by_id(struct dom_node *n, const char *id)
{
  const char *have = dom_get_attr(n, "id");
  if (have && !strcmp(have, id))
    return n;
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    struct dom_node *hit = find_by_id(c, id);
    if (hit)
      return hit;
  }
  return 0;
}

static struct canvas_rec *canvas_by_id(const char *id)
{
  struct dom_node *n = find_by_id(root, id);
  return n ? canvas_find(n) : 0;
}

static const unsigned char *pixel(const char *id, int x, int y)
{
  struct canvas_rec *r = canvas_by_id(id);
  expect(r != 0);
  return r ? r->c->px + ((unsigned long)y * r->c->w + x) * 4 : (const unsigned char *)"\0\0\0\0";
}

static void test_context_and_sizes(void)
{
  run_script("var cv=document.getElementById('c'), cx=cv.getContext('2d');\n"
             "expect(cx===cv.getContext('2d'), 'getContext is cached');\n"
             "expect(cx.canvas===cv, 'ctx.canvas');\n"
             "expect(cv.width===40 && cv.height===30, 'size from attributes');\n"
             "var d=document.getElementById('d'); expect(d.width===300 && d.height===150, 'default size');\n"
             "expect(cv.getContext('webgl')===null, 'webgl is unavailable');\n"
             "expect(cx instanceof CanvasRenderingContext2D, 'instanceof');\n"
             "var made=document.createElement('canvas'); made.width=7; made.height=5;\n"
             "var mx=made.getContext('2d'); mx.fillStyle='#fff'; mx.fillRect(0,0,7,5);\n"
             "expect(made.getAttribute('width')==='7', 'width reflects to the attribute');\n"
             "expect(mx.getImageData(6,4,1,1).data[3]===255, 'detached canvas draws');\n"
             "cv.width=20; expect(cv.width===20 && cx.getImageData(0,0,1,1).data[3]===0, 'resize clears');\n"
             "cv.width=40;\n");
  expect(canvas_by_id("c")->c->w == 40);
}

static void test_fill_styles_and_state(void)
{
  run_script("var cx=document.getElementById('c').getContext('2d');\n"
             "cx.fillStyle='red'; expect(cx.fillStyle==='#ff0000', 'named colour normalises: '+cx.fillStyle);\n"
             "cx.fillStyle='rgba(0,128,255,0.5)'; expect(cx.fillStyle==='rgba(0, 128, 255, 0.5)', cx.fillStyle);\n"
             "cx.fillStyle='not a colour'; expect(cx.fillStyle==='rgba(0, 128, 255, 0.5)', 'invalid is ignored');\n"
             "cx.lineWidth=-3; expect(cx.lineWidth===1, 'non-positive lineWidth ignored');\n"
             "cx.globalAlpha=2; expect(cx.globalAlpha===1, 'globalAlpha out of range ignored');\n"
             "cx.lineCap='round'; cx.save(); cx.lineCap='square'; cx.fillStyle='blue';\n"
             "expect(cx.lineCap==='square' && cx.fillStyle==='#0000ff');\n"
             "cx.restore(); expect(cx.lineCap==='round' && cx.fillStyle==='rgba(0, 128, 255, 0.5)', 'restore');\n"
             "cx.globalCompositeOperation='multiply'; expect(cx.globalCompositeOperation==='multiply');\n"
             "cx.globalCompositeOperation='bogus'; expect(cx.globalCompositeOperation==='multiply');\n"
             "cx.font='bold 18px monospace'; expect(cx.font==='bold 18px monospace');\n"
             "cx.font='nonsense'; expect(cx.font==='bold 18px monospace', 'bad font ignored');\n"
             "cx.reset(); expect(cx.lineCap==='butt' && cx.globalCompositeOperation==='source-over', 'reset');\n");
}

static void test_rects_and_transforms(void)
{
  run_script("var cx=document.getElementById('c').getContext('2d');\n"
             "cx.clearRect(0,0,40,30); cx.fillStyle='#00ff00'; cx.fillRect(2,3,4,5);\n"
             "cx.translate(10,10); cx.scale(2,2); cx.fillStyle='#0000ff'; cx.fillRect(1,1,2,2);\n"
             "var m=cx.getTransform(); expect(m.a===2 && m.d===2 && m.e===10 && m.f===10, 'getTransform');\n"
             "cx.setTransform(1,0,0,1,0,0); cx.clearRect(2,3,1,1);\n"
             "cx.setTransform({a:1,b:0,c:0,d:1,e:30,f:20}); cx.fillStyle='#ff0000'; cx.fillRect(0,0,1,1);\n"
             "cx.resetTransform(); expect(cx.getTransform().isIdentity);\n");
  const unsigned char *g = pixel("c", 3, 3), *cleared = pixel("c", 2, 3), *blue = pixel("c", 12, 12);
  expect(g[1] == 255 && g[3] == 255 && g[0] == 0);
  expect(cleared[3] == 0);
  expect(blue[2] == 255 && blue[3] == 255 && pixel("c", 15, 15)[3] == 255 && pixel("c", 16, 16)[3] == 0);
  expect(pixel("c", 30, 20)[0] == 255);
}

static void test_paths_and_path2d(void)
{
  run_script("var cx=document.getElementById('c').getContext('2d');\n"
             "cx.clearRect(0,0,40,30); cx.fillStyle='#000';\n"
             "cx.beginPath(); cx.moveTo(2,2); cx.lineTo(18,2); cx.lineTo(2,18); cx.closePath(); cx.fill();\n"
             "expect(cx.isPointInPath(4,4) && !cx.isPointInPath(16,16), 'isPointInPath');\n"
             "cx.beginPath(); cx.rect(22,2,16,16); cx.rect(26,6,8,8); cx.fill('evenodd');\n"
             "expect(cx.isPointInPath(24,4,'evenodd') && !cx.isPointInPath(30,10,'evenodd'), 'evenodd hole');\n"
             "var p=new Path2D('M2 22 h10 v6 h-10 z'); cx.fill(p);\n"
             "expect(cx.isPointInPath(p,5,25), 'Path2D hit test');\n"
             "var q=new Path2D(p); q.rect(20,22,6,6); cx.fillStyle='#f00'; cx.fill(q);\n"
             "var big=new Path2D(); big.addPath(p, {e:26,f:-4}); cx.fillStyle='#00f'; cx.fill(big);\n"
             "var threw=''; try{ cx.arc(1,1,-5,0,1) }catch(e){ threw=e.name } expect(threw==='IndexSizeError', "
             "'negative radius');\n"
             "var arcs=new Path2D('M0 0 a5 5 0 1 0 0 0.01'); expect(arcs, 'arc command parses');\n");
  expect(pixel("c", 4, 4)[3] == 255 && pixel("c", 16, 16)[3] == 0);
  expect(pixel("c", 24, 4)[3] == 255 && pixel("c", 30, 10)[3] == 0);
  expect(pixel("c", 5, 25)[3] == 255);
  expect(pixel("c", 22, 24)[0] == 255 && pixel("c", 22, 24)[1] == 0);
  expect(pixel("c", 30, 22)[2] == 255);
}

static void test_stroke_and_dash(void)
{
  run_script("var cx=document.getElementById('c').getContext('2d');\n"
             "cx.clearRect(0,0,40,30); cx.strokeStyle='#000'; cx.lineWidth=4;\n"
             "cx.beginPath(); cx.moveTo(2,10); cx.lineTo(38,10); cx.stroke();\n"
             "cx.setLineDash([4,4]); expect(cx.getLineDash().join()==='4,4');\n"
             "cx.lineWidth=2; cx.beginPath(); cx.moveTo(0,20); cx.lineTo(40,20); cx.stroke();\n"
             "cx.setLineDash([3]); expect(cx.getLineDash().join()==='3,3', 'odd list is doubled');\n"
             "cx.setLineDash([1,-1]); expect(cx.getLineDash().join()==='3,3', 'negative entry ignored');\n"
             "cx.setLineDash([]); cx.strokeRect(20.5,24.5,10,3);\n");
  expect(pixel("c", 20, 8)[3] == 255 && pixel("c", 20, 11)[3] == 255 && pixel("c", 20, 7)[3] == 0);
  expect(pixel("c", 2, 20)[3] == 255 && pixel("c", 6, 20)[3] == 0 && pixel("c", 9, 20)[3] == 255);
  expect(pixel("c", 20, 24)[3] == 255);
}

static void test_gradients_and_patterns(void)
{
  run_script("var cx=document.getElementById('c').getContext('2d');\n"
             "cx.clearRect(0,0,40,30);\n"
             "var g=cx.createLinearGradient(0,0,40,0); g.addColorStop(0,'#000'); g.addColorStop(1,'#fff');\n"
             "cx.fillStyle=g; expect(cx.fillStyle===g, 'gradient reads back'); cx.fillRect(0,0,40,10);\n"
             "var threw=''; try{ g.addColorStop(2,'red') }catch(e){ threw=e.name } expect(threw==='IndexSizeError');\n"
             "threw=''; try{ g.addColorStop(0.5,'nope') }catch(e){ threw=e.name } expect(threw==='SyntaxError');\n"
             "var r=cx.createRadialGradient(20,20,0,20,20,8); r.addColorStop(0,'#f00'); r.addColorStop(1,'#00f');\n"
             "cx.fillStyle=r; cx.fillRect(0,10,40,20);\n"
             "var tile=document.createElement('canvas'); tile.width=2; tile.height=1; var tc=tile.getContext('2d');\n"
             "tc.fillStyle='#0f0'; tc.fillRect(0,0,1,1); tc.fillStyle='#00f'; tc.fillRect(1,0,1,1);\n"
             "var pat=cx.createPattern(tile,'repeat'); cx.fillStyle=pat; cx.fillRect(0,28,40,2);\n");
  expect(pixel("c", 1, 5)[0] < 12 && pixel("c", 38, 5)[0] > 235);
  expect(pixel("c", 20, 20)[0] > 220 && pixel("c", 20, 20)[2] < 35);
  expect(pixel("c", 20, 12)[2] > 100 || pixel("c", 20, 12)[0] > 0);
  expect(pixel("c", 0, 29)[1] == 255 && pixel("c", 1, 29)[2] == 255 && pixel("c", 2, 29)[1] == 255);
}

static void test_image_data(void)
{
  run_script("var cx=document.getElementById('c').getContext('2d');\n"
             "cx.clearRect(0,0,40,30);\n"
             "var id=cx.createImageData(2,2); expect(id.width===2 && id.data.length===16 && id.data[3]===0);\n"
             "expect(id.data instanceof Uint8ClampedArray, 'clamped array');\n"
             "id.data[0]=300; id.data[1]=-5; id.data[3]=255; expect(id.data[0]===255 && id.data[1]===0, 'clamps');\n"
             "cx.putImageData(id, 5, 5); var back=cx.getImageData(5,5,1,1);\n"
             "expect(back.data[0]===255 && back.data[3]===255, 'round trip');\n"
             "expect(cx.getImageData(-2,-2,2,2).data[0]===0, 'outside is transparent black');\n"
             "var made=new ImageData(new Uint8ClampedArray([1,2,3,4,5,6,7,8]), 2); expect(made.height===1 && "
             "made.data[5]===6);\n"
             "var threw=''; try{ cx.getImageData(0,0,0,5) }catch(e){ threw=e.name } expect(threw==='IndexSizeError');\n"
             "cx.putImageData(made, 10, 10, 1, 0, 1, 1);\n");
  expect(pixel("c", 5, 5)[0] == 255 && pixel("c", 5, 5)[1] == 0);
  expect(pixel("c", 11, 10)[0] == 5 && pixel("c", 11, 10)[3] == 8); /* the dirty rect lands at dx + dirtyX */
  expect(pixel("c", 10, 10)[3] == 0);
}

static void test_draw_image_sources(void)
{
  run_script(
      "var cx=document.getElementById('c').getContext('2d'), d=document.getElementById('d').getContext('2d');\n"
      "cx.clearRect(0,0,40,30); cx.imageSmoothingEnabled=false;\n"
      "d.fillStyle='#f00'; d.fillRect(0,0,2,2); d.fillStyle='#00f'; d.fillRect(2,0,2,2);\n"
      "cx.drawImage(document.getElementById('d'), 0, 0, 4, 2, 0, 0, 8, 4);\n"
      "var off=new OffscreenCanvas(2,2); var oc=off.getContext('2d'); oc.fillStyle='#0f0'; oc.fillRect(0,0,2,2);\n"
      "expect(off.width===2, 'OffscreenCanvas size');\n"
      "cx.drawImage(off, 20, 20);\n"
      "cx.fillStyle='#ff0'; cx.fillRect(30,0,2,2); cx.drawImage(document.getElementById('c'), 30, 0, 2, 2, 34, 0, 2, "
      "2);\n"
      "var threw=''; try{ cx.drawImage({}, 0, 0) }catch(e){ threw=e.name } expect(threw==='TypeError', 'bad source');\n"
      "var ids=cx.getImageData(0,0,1,1); expect(ids.data[0]===255);\n");
  expect(pixel("c", 1, 1)[0] == 255 && pixel("c", 1, 1)[2] == 0 && pixel("c", 5, 1)[2] == 255 &&
         pixel("c", 7, 3)[2] == 255);
  expect(pixel("c", 8, 1)[3] == 0);
  expect(pixel("c", 20, 20)[1] == 255 && pixel("c", 21, 21)[1] == 255 && pixel("c", 22, 22)[3] == 0);
  expect(pixel("c", 34, 0)[0] == 255 && pixel("c", 34, 0)[1] == 255 && pixel("c", 35, 1)[3] == 255);
}

static void test_offscreen_transfer(void)
{
  run_script(
      "var off=new OffscreenCanvas(4,3), oc=off.getContext('2d'); oc.fillStyle='#f00'; oc.fillRect(0,0,4,3);\n"
      "var bm=off.transferToImageBitmap();\n"
      "expect(bm instanceof ImageBitmap && bm.width===4 && bm.height===3, 'bitmap size');\n"
      "expect(oc.getImageData(1,1,1,1).data[3]===0, 'source is cleared after transfer');\n"
      "var dst=new OffscreenCanvas(4,3).getContext('2d'); dst.drawImage(bm,0,0);\n"
      "expect(dst.getImageData(2,2,1,1).data[0]===255, 'bitmap carries the pixels');\n"
      "var cv=document.createElement('canvas'); cv.id='xfer'; cv.width=6; cv.height=2; document.body.appendChild(cv);\n"
      "var ctrl=cv.transferControlToOffscreen(), name='';\n"
      "expect(ctrl instanceof OffscreenCanvas && ctrl.width===6 && ctrl.height===2, 'placeholder size');\n"
      "var x=ctrl.getContext('2d'); x.fillStyle='#00f'; x.fillRect(0,0,6,2);\n"
      "try{ cv.getContext('2d') }catch(e){ name=e.name } expect(name==='InvalidStateError', 'getContext after "
      "transfer');\n"
      "name=''; try{ cv.width=9 }catch(e){ name=e.name } expect(name==='InvalidStateError', 'resize after transfer');\n"
      "name=''; try{ ctrl.transferToImageBitmap() }catch(e){ name=e.name } expect(name==='InvalidStateError', "
      "'placeholder');\n"
      "var used=document.createElement('canvas'); used.getContext('2d'); name='';\n"
      "try{ used.transferControlToOffscreen() }catch(e){ name=e.name } expect(name==='InvalidStateError', 'context "
      "exists');\n"
      "window.ctrl=ctrl;\n");
  expect(pixel("xfer", 0, 0)[2] == 255);
  run_script("ctrl.width=8; expect(document.getElementById('xfer').getAttribute('width')==='8', 'resize reflects');\n");
  expect(canvas_by_id("xfer")->c->w == 8);
}

/* A transferred canvas is drawn on by the worker thread: it shares the page's surface, not its heap */
static void test_offscreen_in_worker(void)
{
  run_script(
      "var cv=document.createElement('canvas'); cv.id='wk'; cv.width=5; cv.height=2; document.body.appendChild(cv);\n"
      "var w=new Worker('data:text/javascript,onmessage=function(e){ var c=e.data.canvas.getContext(\"2d\"); "
      "c.fillStyle=\"#0f0\"; c.fillRect(0,0,5,2); var o=new OffscreenCanvas(2,2); var d=c.getImageData(4,1,1,1).data; "
      "postMessage(o.width+\"x\"+e.data.canvas.height+\":\"+d[1]); };'); window.reply=null;\n"
      "w.onmessage=function(e){ window.reply=e.data };\n"
      "var off=cv.transferControlToOffscreen(); w.postMessage({canvas:off},[off]);\n");
  for (int i = 0; i < 600; i++) {
    js_workers_pump();
    js_value reply = js_get(g_ctx, js_obj_value(g_ctx->global), "reply");
    if (reply.type == JV_STR)
      break;
    msleep(5);
  }
  run_script("expect(window.reply==='2x2:255', 'worker reply: '+window.reply);\n"
             "w.terminate();\n");
  expect(pixel("wk", 4, 1)[1] == 255);
  /* a canvas the worker made itself: drawn on, read back, and turned into a Blob in the worker */
  run_script("window.own=null; var w2=new Worker('data:text/javascript,var o=new OffscreenCanvas(3,3), "
             "c=o.getContext(\"2d\"); "
             "c.fillStyle=\"#f00\"; c.fillRect(0,0,3,3); var px=c.getImageData(1,1,1,1).data; var tb=\"\"; "
             "try { o.transferToImageBitmap(); } catch(e) { tb=e.name; } "
             "o.convertToBlob().then(function(b){ postMessage({px:px[0]+\",\"+px[3], big:b.size>8, type:b.type, blob:b "
             "instanceof Blob, tb:tb}); });');\n"
             "w2.onmessage=function(e){ window.own=e.data };\n");
  for (int i = 0; i < 600; i++) {
    js_workers_pump();
    if (js_get(g_ctx, js_obj_value(g_ctx->global), "own").type == JV_OBJ)
      break;
    msleep(5);
  }
  run_script("expect(own && own.px==='255,255' && own.big && own.type==='image/png' && own.blob, 'worker-owned canvas: "
             "'+JSON.stringify(own));\n"
             "w2.terminate();\n");
}

static void test_composite_shadow_clip(void)
{
  run_script(
      "var cx=document.getElementById('c').getContext('2d');\n"
      "cx.clearRect(0,0,40,30); cx.fillStyle='#f00'; cx.fillRect(0,0,10,10);\n"
      "cx.globalCompositeOperation='destination-out'; cx.fillRect(2,2,2,2);\n"
      "cx.globalCompositeOperation='source-over';\n"
      "cx.save(); cx.beginPath(); cx.rect(20,0,5,5); cx.clip(); cx.fillStyle='#00f'; cx.fillRect(0,0,40,30); "
      "cx.restore();\n"
      "cx.shadowColor='#000'; cx.shadowOffsetX=5; cx.shadowOffsetY=5; cx.fillStyle='#0f0'; cx.fillRect(12,12,4,4);\n");
  expect(pixel("c", 2, 2)[3] == 0 && pixel("c", 5, 5)[3] == 255);
  expect(pixel("c", 21, 1)[2] == 255 && pixel("c", 26, 1)[3] == 0 && pixel("c", 21, 6)[3] == 0);
  expect(pixel("c", 13, 13)[1] == 255);
  expect(pixel("c", 18, 18)[3] == 255 && pixel("c", 18, 18)[1] == 0); /* hard shadow */
}

static void test_export(void)
{
  fake_now_ms += 0;
  run_script("var cx=document.getElementById('c').getContext('2d'); cx.fillStyle='#123'; cx.fillRect(0,0,40,30);\n"
             "var url=document.getElementById('c').toDataURL();\n"
             "expect(url.indexOf('data:image/png;base64,iVBORw0KGgo')===0, 'png data url: '+url.slice(0,40));\n"
             "var got=null; document.getElementById('c').toBlob(function(b){ got=b });\n"
             "var viaOff=new OffscreenCanvas(3,3); viaOff.getContext('2d').fillRect(0,0,1,1);\n"
             "var cvt=null; viaOff.convertToBlob().then(function(b){ cvt=b });\n"
             "setTimeout(function(){ expect(got && got.type==='image/png' && got.size>100, 'toBlob delivered'); "
             "expect(cvt && cvt.size>50, 'convertToBlob'); window.exportDone=1; }, 5);\n");
  fake_now_ms += 20;
  js_timers_pump();
  js_timers_pump();
  run_script("expect(window.exportDone===1, 'blob callbacks ran');\n");
}

static void test_image_loading(void)
{
  /* "RAW" 2 1 then two RGBA pixels: (100,10,20,127) (30,40,50,127) */
  run_script(
      "var im=document.getElementById('im'), log=[];\n"
      "im.onload=function(){ log.push('load') }; im.onerror=function(){ log.push('error') };\n"
      "im.src='data:image/x-raw,RAW\\x02\\x01\\x64\\x0a\\x14\\x7f\\x1e\\x28\\x32\\x7f';\n"
      "expect(im.naturalWidth===2 && im.naturalHeight===1, 'natural size: '+im.naturalWidth);\n"
      "expect(im.complete && im.width===2, 'complete and width falls back to natural');\n"
      "var cx=document.getElementById('c').getContext('2d'); cx.clearRect(0,0,40,30); cx.imageSmoothingEnabled=false;\n"
      "cx.drawImage(im, 0, 0, 2, 1, 0, 0, 4, 2);\n"
      "var bad=document.createElement('img'); bad.onerror=function(){ log.push('bad') }; "
      "bad.src='data:image/x-raw,junk';\n"
      "expect(bad.naturalWidth===0, 'undecodable image');\n"
      "var bmp=null; createImageBitmap(im).then(function(b){ bmp=b; log.push('bitmap'+b.width) });\n");
  expect(pixel("c", 0, 0)[0] == 100 && pixel("c", 0, 0)[3] == 127 && pixel("c", 3, 1)[0] == 30);
  fake_now_ms += 20;
  js_timers_pump();
  js_timers_pump();
  run_script(
      "expect(log.indexOf('load')>=0 && log.indexOf('bad')>=0 && log.indexOf('error')<0, 'events: '+log.join());\n"
      "expect(log.indexOf('bitmap2')>=0, 'createImageBitmap: '+log.join());\n");
}

static void test_text_metrics(void)
{
  int n = 0;
  FILE *f = fopen("fonts/LiberationSans-Regular.ttf", "rb");
  expect(f != 0);
  if (!f)
    return;
  fseek(f, 0, SEEK_END);
  n = (int)ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *ttf = malloc((size_t)n);
  expect(fread(ttf, 1, (size_t)n, f) == (size_t)n);
  fclose(f);
  struct font *font = font_load(ttf, n);
  expect(font != 0);
  struct canvas_rec *r = canvas_by_id("c");
  r->c->sans = font;
  run_script("var cx=document.getElementById('c').getContext('2d'); cx.clearRect(0,0,40,30);\n"
             "cx.font='16px sans-serif'; var w=cx.measureText('iii').width; expect(w>0 && "
             "cx.measureText('M').width>cx.measureText('i').width);\n"
             "expect(cx.measureText('iii').actualBoundingBoxAscent>0, 'ascent');\n"
             "cx.fillStyle='#000'; cx.textBaseline='top'; cx.fillText('H', 2, 2);\n"
             "cx.textAlign='right'; cx.strokeStyle='#f00'; cx.strokeText('H', 38, 2);\n");
  int ink = 0, red = 0;
  for (int y = 0; y < 30; y++)
    for (int x = 0; x < 40; x++) {
      const unsigned char *p = pixel("c", x, y);
      ink += p[3] > 0 && p[0] == 0;
      red += p[3] > 0 && p[0] == 255;
    }
  expect(ink > 10 && red > 10);
  r->c->sans = 0;
  font_free(font);
  free(ttf);
}

int main(void)
{
  B = calloc(1, sizeof *B);
  B->pg = calloc(1, 1 << 16);
  root = dom_parse(page, (int)strlen(page));
  js_run_document(root, "https://example.com/dir/p.html", fetch_none);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);

  test("getContext, sizes and detached canvases", test_context_and_sizes);
  test("fillStyle, state stack and property validation", test_fill_styles_and_state);
  test("rects and transforms", test_rects_and_transforms);
  test("paths, fill rules and Path2D", test_paths_and_path2d);
  test("strokes and dashes", test_stroke_and_dash);
  test("gradients and patterns", test_gradients_and_patterns);
  test("ImageData round trips", test_image_data);
  test("drawImage from canvas sources", test_draw_image_sources);
  test("OffscreenCanvas transfer to bitmap and from element", test_offscreen_transfer);
  test("OffscreenCanvas inside a worker", test_offscreen_in_worker);
  test("composite operations, clip and shadows", test_composite_shadow_clip);
  test("toDataURL, toBlob and convertToBlob", test_export);
  test("img loading, load events and ImageBitmap", test_image_loading);
  test("text metrics and drawing", test_text_metrics);

  js_page_end();
  dom_free(root);
  free(B->pg);
  free(B);
  return 0;
}
