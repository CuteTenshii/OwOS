/* mediactl.c - /bin/mediactl: inspect and drive the active media session through mediad */
#include "mediactl.h"
#include "opt.h"
#include "owos.h"
#include <stdbool.h>

#define ARGS "COMMAND [ARG]"
#define SUMMARY                                                                                                        \
  "Commands:\n"                                                                                                        \
  "  status          print the active session\n"                                                                       \
  "  play|pause|toggle|stop|next|prev\n"                                                                               \
  "  seek MS         seek forward (or back with -b) by MS milliseconds\n"                                              \
  "  seekto MS       seek to an absolute position\n"                                                                   \
  "  watch           print status changes\n"                                                                           \
  "  demo            register a fake session and print its commands"

static const struct opt opts[] = {
    {'b', "back", OPT_FLAG, 'b', 0, "seek backwards"},
    {'i', "interval", OPT_ARG, 'i', "MS", "watch: poll interval in milliseconds (default 500)"},
    {'t', "time", OPT_ARG, 't', "SECONDS", "demo: how long to run (default 30)"},
    {'h', "help", OPT_FLAG, 'h', 0, "show this help"},
};

static const char *state_name(int s)
{
  return s == MEDIA_PLAYING ? "playing" : s == MEDIA_PAUSED ? "paused" : "stopped";
}

static int cmd_status(void)
{
  struct media_info in;
  int r = mediactl_status(&in);
  if (r < 0) {
    puts("mediactl: mediad is not running\n");
    return 2;
  }
  if (!r) {
    puts("no active session\n");
    return 1;
  }
  printf("app: %s\ntitle: %s\nartist: %s\nalbum: %s\nstate: %s\nposition: %u ms / %u ms\n", in.app, in.title, in.artist,
         in.album, state_name(in.state), in.pos_ms, in.dur_ms);
  return 0;
}

static bool same(const struct media_info *a, const struct media_info *b)
{
  return a->state == b->state && streq(a->app, b->app) && streq(a->title, b->title) && streq(a->artist, b->artist) &&
         streq(a->album, b->album);
}

static __attribute__((noreturn)) void cmd_watch(unsigned interval)
{
  struct media_info cur, last = {0};
  int last_r = -2;
  for (;;) {
    int r = mediactl_status(&cur);
    if (r != last_r || (r == 1 && !same(&cur, &last))) {
      if (r < 0)
        puts("mediad is not running\n");
      else if (!r)
        puts("no active session\n");
      else
        printf("[%s] %s: %s - %s (%s) %u/%u ms\n", state_name(cur.state), cur.app, cur.artist, cur.title, cur.album,
               cur.pos_ms, cur.dur_ms);
      last_r = r;
      if (r == 1)
        last = cur;
    }
    msleep(interval);
  }
}

static int cmd_demo(unsigned seconds)
{
  int id = mediactl_open("demo");
  if (id < 0) {
    puts("mediactl: mediad is not running\n");
    return 2;
  }
  struct media_info in = {0};
  strlcpy(in.app, "demo", sizeof in.app);
  strlcpy(in.title, "Demo Track", sizeof in.title);
  strlcpy(in.artist, "Demo Artist", sizeof in.artist);
  strlcpy(in.album, "Demo Album", sizeof in.album);
  in.state = MEDIA_PLAYING;
  in.dur_ms = 180000;
  in.caps = ~0u;
  mediactl_update(id, &in);
  printf("demo: session %d registered\n", id);

  unsigned end = uptime() + seconds;
  while (uptime() < end) {
    unsigned arg;
    int c;
    while ((c = mediactl_poll(id, &arg)) != MEDIA_CMD_NONE) {
      printf("demo: command %d arg %u\n", c, arg);
      if (c == MEDIA_CMD_PLAY)
        in.state = MEDIA_PLAYING;
      else if (c == MEDIA_CMD_PAUSE)
        in.state = MEDIA_PAUSED;
      else if (c == MEDIA_CMD_TOGGLE)
        in.state = in.state == MEDIA_PLAYING ? MEDIA_PAUSED : MEDIA_PLAYING;
      else if (c == MEDIA_CMD_STOP)
        in.state = MEDIA_STOPPED;
      else if (c == MEDIA_CMD_SEEK_TO)
        in.pos_ms = arg;
      mediactl_update(id, &in);
    }
    msleep(100);
  }
  mediactl_close(id);
  puts("demo: closed\n");
  return 0;
}

int main(int argc, char **argv)
{
  static const struct {
    const char *name;
    int cmd;
  } simple[] = {
      {"play", MEDIA_CMD_PLAY}, {"pause", MEDIA_CMD_PAUSE}, {"toggle", MEDIA_CMD_TOGGLE},
      {"stop", MEDIA_CMD_STOP}, {"next", MEDIA_CMD_NEXT},   {"prev", MEDIA_CMD_PREV},
  };
  bool back = false;
  unsigned interval = 500, seconds = 30;
  const char *pos[2] = {0, 0};
  int npos = 0;

  struct optstate st;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (o == 'h') {
      opt_print_help("mediactl", ARGS, SUMMARY, opts, OPT_N(opts));
      return 0;
    } else if (o == 'b') {
      back = true;
    } else if (o == 'i' || o == 't') {
      unsigned v;
      if (opt_uint(st.arg, &v) || (o == 'i' && !v)) {
        opt_print_usage("mediactl", ARGS, opts, OPT_N(opts));
        return 1;
      }
      if (o == 'i')
        interval = v;
      else
        seconds = v;
    } else if (o == OPT_POS && npos < 2) {
      pos[npos++] = st.arg;
    } else {
      opt_print_usage("mediactl", ARGS, opts, OPT_N(opts));
      return 1;
    }
  }
  if (!npos) {
    opt_print_usage("mediactl", ARGS, opts, OPT_N(opts));
    return 1;
  }

  const char *name = pos[0];
  if (streq(name, "status"))
    return cmd_status();
  if (streq(name, "watch"))
    cmd_watch(interval);
  if (streq(name, "demo"))
    return cmd_demo(seconds);

  int cmd = MEDIA_CMD_NONE;
  unsigned arg = 0;
  for (int i = 0; i < OPT_N(simple); i++)
    if (streq(name, simple[i].name))
      cmd = simple[i].cmd;
  if (streq(name, "seek") || streq(name, "seekto")) {
    if (!pos[1] || opt_uint(pos[1], &arg)) {
      opt_print_usage("mediactl", ARGS, opts, OPT_N(opts));
      return 1;
    }
    cmd = streq(name, "seekto") ? MEDIA_CMD_SEEK_TO : back ? MEDIA_CMD_SEEK_BACK : MEDIA_CMD_SEEK_FWD;
  }
  if (cmd == MEDIA_CMD_NONE) {
    opt_print_usage("mediactl", ARGS, opts, OPT_N(opts));
    return 1;
  }
  if (mediactl_send(cmd, arg) < 0) {
    puts("mediactl: no active session\n");
    return 1;
  }
  return 0;
}
