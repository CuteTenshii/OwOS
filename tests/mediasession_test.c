/* Host test for the Web Media Session API module. */
#define OWOS_SUITE_TEST
#define report_exc ms_test_report_exc
#include "features/mediasession/mediasession.c"
#include <stdio.h>
#include <stdlib.h>

static js_object *navigator;
static int opens, updates, closes;
static struct media_info published;
static int queue[16][2], queued, dequeued;
static bool service_up = true;

static int el_present, el_playing, el_ended, plays, pauses;
static double el_time, el_duration = 100;
static char el_token;

int mediactl_open(const char *app)
{
  (void)app;
  if (!service_up)
    return -1;
  opens++;
  return 7;
}
int mediactl_update(int id, const struct media_info *info)
{
  (void)id;
  updates++;
  published = *info;
  return 0;
}
int mediactl_poll(int id, unsigned *arg)
{
  (void)id;
  if (dequeued == queued)
    return MEDIA_CMD_NONE;
  *arg = (unsigned)queue[dequeued][1];
  return queue[dequeued++][0];
}
int mediactl_close(int id)
{
  (void)id;
  closes++;
  return 0;
}

struct media_el *media_current(void) { return el_present ? (struct media_el *)&el_token : 0; }
int media_paused(struct media_el *m)
{
  (void)m;
  return !el_playing;
}
int media_ended(struct media_el *m)
{
  (void)m;
  return el_ended;
}
double media_current_time(struct media_el *m)
{
  (void)m;
  return el_time;
}
void media_set_current_time(struct media_el *m, double sec)
{
  (void)m;
  el_time = sec;
}
double media_duration(struct media_el *m)
{
  (void)m;
  return el_duration;
}
void media_play(struct media_el *m)
{
  (void)m;
  plays++;
  el_playing = 1;
}
void media_pause(struct media_el *m)
{
  (void)m;
  pauses++;
  el_playing = 0;
}

void ms_test_report_exc(js_ctx *c) { c->has_exc = 0; }

static void con_print(js_ctx *c, int level, const char *s, int len)
{
  (void)c;
  (void)level;
  (void)s;
  (void)len;
}
static js_value test_expect(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)t;
  expect(argc > 0 && js_to_boolean(args[0]));
  return js_undef();
}
static js_value native_tick(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)c;
  (void)t;
  (void)args;
  (void)argc;
  mediasession_tick("Doc Title", "https://example.com/page/index.html");
  return js_undef();
}
static js_value native_queue(js_ctx *c, js_value t, js_value *args, int argc)
{
  (void)t;
  queue[queued][0] = (int)js_to_number(c, A(0));
  queue[queued++][1] = (int)js_to_number(c, A(1));
  return js_undef();
}

static js_arena arenas[32];
static int narenas;

static void run(const char *source)
{
  js_arena *arena = &arenas[narenas++];
  js_parse_error error = {0, 0};
  js_node *program;
  js_value result;
  js_arena_init(arena);
  program = js_parse(source, arena, &error);
  expect(program != 0);
  if (program)
    expect(js_run(g_ctx, program, &result) == 0);
}

static void tick_n(int n)
{
  while (n--)
    mediasession_tick("Doc Title", "https://example.com/page/index.html");
}

static void test_mediametadata_attributes_and_artwork(void)
{
  run("const m=new MediaMetadata({title:'T',artist:'A',album:'B',"
      "artwork:[{src:'cover.png',sizes:'96x96',type:'image/png'},{src:'/x.jpg'}]});\n"
      "expect(m.title==='T' && m.artist==='A' && m.album==='B');\n"
      "expect(m.artwork.length===2);\n"
      "expect(m.artwork[0].src==='https://example.com/page/cover.png');\n"
      "expect(m.artwork[0].sizes==='96x96' && m.artwork[0].type==='image/png');\n"
      "expect(m.artwork[1].src==='https://example.com/x.jpg' && m.artwork[1].sizes==='' && m.artwork[1].type==='');\n"
      "const e=new MediaMetadata();\n"
      "expect(e.title==='' && e.artist==='' && e.album==='' && e.artwork.length===0);\n"
      "m.title=42; expect(m.title==='42');\n"
      "m.artwork=[{src:'a.png'}]; expect(m.artwork.length===1);\n"
      "let t1=''; try{ new MediaMetadata({artwork:[{sizes:'1x1'}]}); }catch(x){ t1=x.name; }\n"
      "expect(t1==='TypeError');\n"
      "let t2=''; try{ m.artwork=[5]; }catch(x){ t2=x.name; }\n"
      "expect(t2==='TypeError' && m.artwork.length===1);\n");
}

static void test_mediasession_validation(void)
{
  run("const s=navigator.mediaSession;\n"
      "expect(s.metadata===null && s.playbackState==='none');\n"
      "let n=''; try{ s.metadata={title:'x'}; }catch(x){ n=x.name; }\n"
      "expect(n==='TypeError');\n"
      "s.playbackState='bogus'; expect(s.playbackState==='none');\n"
      "let a=''; try{ s.setActionHandler('explode',function(){}); }catch(x){ a=x.name; }\n"
      "expect(a==='TypeError');\n"
      "let b=''; try{ s.setActionHandler('play',5); }catch(x){ b=x.name; }\n"
      "expect(b==='TypeError');\n"
      "for (const k of ['skipad','hangup','togglemicrophone','togglecamera','previousslide','nextslide'])\n"
      "  s.setActionHandler(k,function(){});\n"
      "function bad(o){ try{ s.setPositionState(o); return ''; }catch(x){ return x.name; } }\n"
      "expect(bad({duration:-1})==='TypeError');\n"
      "expect(bad({duration:10,position:11})==='TypeError');\n"
      "expect(bad({duration:10,position:-1})==='TypeError');\n"
      "expect(bad({duration:10,playbackRate:0})==='TypeError');\n"
      "expect(bad({duration:NaN})==='TypeError');\n"
      "expect(bad({position:1})==='TypeError');\n"
      "expect(bad({duration:10,position:5,playbackRate:2})==='');\n"
      "expect(bad()==='' && bad({})==='');\n"
      "expect(s.setCameraActive(true) instanceof Promise && s.setMicrophoneActive(false) instanceof Promise);\n");
}

static void test_no_session_without_metadata(void)
{
  tick_n(5);
  expect(opens == 0 && updates == 0);
}

static void test_metadata_publishes_once_handlers(void)
{
  run("const s=navigator.mediaSession;\n"
      "globalThis.calls=[];\n"
      "s.metadata=new MediaMetadata({title:'Song',artist:'Band',album:'LP',artwork:[{src:'file:///art/c.png'}]});\n"
      "s.playbackState='playing';\n"
      "s.setActionHandler('play',function(d){calls.push(d);});\n"
      "s.setActionHandler('pause',function(d){calls.push(d);});\n"
      "s.setActionHandler('seekforward',function(d){calls.push(d);});\n"
      "s.setActionHandler('seekto',function(d){calls.push(d);});\n"
      "s.setActionHandler('nexttrack',function(d){calls.push(d);});\n");
  tick_n(5);
  expect(opens == 1 && updates == 1);
  expect_string(published.title, "Song");
  expect_string(published.artist, "Band");
  expect_string(published.album, "LP");
  expect_string(published.artwork, "/art/c.png");
  expect_string(published.app, "Browser");
  expect(published.state == MEDIA_PLAYING);
  expect(published.caps & MEDIA_CAP(MEDIA_CMD_NEXT));
  expect(published.caps & MEDIA_CAP(MEDIA_CMD_TOGGLE));
  expect(!(published.caps & MEDIA_CAP(MEDIA_CMD_STOP)));
  expect(!(published.caps & MEDIA_CAP(MEDIA_CMD_PREV)));
  run("navigator.mediaSession.metadata.title='Other';");
  tick_n(40);
  expect(updates >= 2);
  expect_string(published.title, "Other");

  queue[queued][0] = MEDIA_CMD_SEEK_FWD;
  queue[queued++][1] = 5000;
  queue[queued][0] = MEDIA_CMD_SEEK_TO;
  queue[queued++][1] = 42000;
  queue[queued][0] = MEDIA_CMD_NEXT;
  queue[queued++][1] = 0;
  queue[queued][0] = MEDIA_CMD_TOGGLE;
  queue[queued++][1] = 0;
  tick_n(40);
  run("expect(calls.length===4);\n"
      "expect(calls[0].action==='seekforward' && calls[0].seekOffset===5 && calls[0].fastSeek===false);\n"
      "expect(calls[1].action==='seekto' && calls[1].seekTime===42);\n"
      "expect(calls[2].action==='nexttrack');\n"
      "expect(calls[3].action==='pause');\n");
  expect(plays == 0 && pauses == 0);
}

static void test_commands_fall_back_to(void)
{
  run("navigator.mediaSession.setActionHandler('play',null);navigator.mediaSession.setActionHandler('pause',null);");
  el_present = el_playing = 1;
  queue[queued][0] = MEDIA_CMD_PAUSE;
  queue[queued++][1] = 0;
  queue[queued][0] = MEDIA_CMD_SEEK_TO;
  queue[queued++][1] = 30000;
  queue[queued][0] = MEDIA_CMD_SEEK_BACK;
  queue[queued++][1] = 0;
  queue[queued][0] = MEDIA_CMD_STOP;
  queue[queued++][1] = 0;
  tick_n(40);
  expect(pauses == 2 && el_time == 0);
  expect(published.caps & MEDIA_CAP(MEDIA_CMD_STOP));
  queue[queued][0] = MEDIA_CMD_PLAY;
  queue[queued++][1] = 0;
  queue[queued][0] = MEDIA_CMD_SEEK_FWD;
  queue[queued++][1] = 0;
  tick_n(40);
  expect(plays == 1);
  el_present = el_playing = 0;
}

static void test_teardown_closes_the_session(void)
{
  closes = 0;
  mediasession_teardown();
  expect(closes == 1);
  mediasession_teardown();
  expect(closes == 1);
}

static void test_default_session_from_a(void)
{
  opens = updates = closes = 0;
  mediasession_install(g_ctx, navigator, "https://example.com/page/index.html");
  el_present = el_playing = 1;
  tick_n(5);
  expect(opens == 1 && updates == 1);
  expect_string(published.title, "Doc Title");
  expect_string(published.artist, "example.com");
  expect(published.state == MEDIA_PLAYING);
  expect(published.dur_ms == 100000);
  el_playing = 0;
  tick_n(40);
  expect(published.state == MEDIA_PAUSED && closes == 0);
  el_ended = 1;
  tick_n(2);
  expect(closes == 1);
  el_present = el_ended = 0;
}

static void test_service_down_is_retried(void)
{
  mediasession_teardown();
  mediasession_install(g_ctx, navigator, "https://example.com/");
  service_up = false;
  opens = 0;
  run("navigator.mediaSession.playbackState='paused';");
  tick_n(10);
  expect(opens == 0);
  service_up = true;
  tick_n(1000);
  expect(opens == 1);
}

int main(void)
{
  g_ctx = js_ctx_new(con_print);
  navigator = js_new_object(g_ctx, g_ctx->object_proto);
  js_def(g_ctx->global, "navigator", js_obj_value(navigator), P_WRITABLE);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);
  js_def_native(g_ctx, g_ctx->global, "__tick", native_tick, 0);
  js_def_native(g_ctx, g_ctx->global, "__queue", native_queue, 2);
  mediasession_install(g_ctx, navigator, "https://example.com/page/index.html");

  test("MediaMetadata attributes and artwork normalization", test_mediametadata_attributes_and_artwork);
  test("mediaSession validation", test_mediasession_validation);
  test("no session without metadata, state or media", test_no_session_without_metadata);
  test("metadata publishes once, handlers set caps, commands reach handlers", test_metadata_publishes_once_handlers);
  test("commands fall back to the media element when no handler", test_commands_fall_back_to);
  test("teardown closes the session", test_teardown_closes_the_session);
  test("default session from a playing element, cleared when it ends", test_default_session_from_a);
  test("service down is retried without spamming", test_service_down_is_retried);

  mediasession_teardown();
  js_ctx_free(g_ctx);
  return 0;
}
