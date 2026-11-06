/* cast — a terminal front end for the Cast library (user/lib/cast).
 *
 *   cast [discover]                 list Cast receivers on the LAN
 *   cast play <name> <url> [type]   cast a media URL to a receiver and monitor
 * it
 *
 * <name> matches a device by a case-insensitive substring of its friendly name
 * (or a 0-based index); [type] is an optional MIME hint (e.g. video/mp4). This
 * exercises the whole pipeline (mDNS -> TLS channel -> Default Media Receiver)
 * without the browser; there is no Cast device under QEMU/SLIRP, so `discover`
 * there simply reports none.
 */
#include "cast.h"
#include "owos.h"
#include <stdbool.h>

static bool is_uint(const char *s)
{
  if (!*s)
    return false;
  for (; *s; s++)
    if (*s < '0' || *s > '9')
      return false;
  return true;
}

static int atoi_simple(const char *s)
{
  int v = 0;
  for (; *s >= '0' && *s <= '9'; s++)
    v = v * 10 + (*s - '0');
  return v;
}

static void print_devices(struct cast_device *d, int n)
{
  for (int i = 0; i < n; i++)
    printf("  [%d] %s  (%s)  %u.%u.%u.%u:%u\n", i, d[i].name[0] ? d[i].name : "(unnamed)",
           d[i].model[0] ? d[i].model : "?", d[i].ip[0], d[i].ip[1], d[i].ip[2], d[i].ip[3], d[i].port);
}

/* Pick a device by numeric index or a name substring. -1 if none matches. */
static int pick(struct cast_device *d, int n, const char *sel)
{
  if (is_uint(sel)) {
    int idx = atoi_simple(sel);
    return (idx >= 0 && idx < n) ? idx : -1;
  }
  for (int i = 0; i < n; i++)
    if (contains_ci(d[i].name, sel))
      return i;
  return -1;
}

static const char *state_name(int st)
{
  switch (st) {
  case CAST_PLAYING:
    return "PLAYING";
  case CAST_PAUSED:
    return "PAUSED";
  case CAST_BUFFERING:
    return "BUFFERING";
  default:
    return "IDLE";
  }
}

static void put_clock(double sec) /* prints m:ss, no float formatting */
{
  int t = (int)sec, m = t / 60, s = t % 60;
  printf("%d:%02d", m, s);
}

static int cmd_discover(void)
{
  struct cast_device d[16];
  int n;
  printf("Scanning for Cast devices...\n");
  n = cast_discover(d, 16, 3000);
  if (n < 0) {
    printf("cast: discovery interrupted\n");
    return 1;
  }
  if (n == 0) {
    printf("No Cast devices found.\n");
    return 0;
  }
  printf("Found %d device%s:\n", n, n == 1 ? "" : "s");
  print_devices(d, n);
  return 0;
}

static int cmd_play(const char *sel, const char *url, const char *type)
{
  struct cast_device d[16];
  cast_session *s;
  struct cast_status st;
  bool seen_playing = false;
  int n, idx;

  printf("Scanning for Cast devices...\n");
  n = cast_discover(d, 16, 3000);
  if (n <= 0) {
    printf("No Cast devices found.\n");
    return 1;
  }
  idx = pick(d, n, sel);
  if (idx < 0) {
    printf("cast: no device matching '%s'\n", sel);
    print_devices(d, n);
    return 1;
  }

  printf("Connecting to %s...\n", d[idx].name);
  s = cast_connect(&d[idx], 10000);
  if (!s) {
    printf("cast: connection failed\n");
    return 1;
  }

  printf("Loading %s ...\n", url);
  if (cast_load(s, url, type, url, 10000)) {
    printf("cast: load failed\n");
    cast_close(s);
    return 1;
  }
  printf("Now casting. (kill this process to stop)\n");

  /* Monitor: print a status line each second until the track ends or the
   * receiver disconnects. */
  for (;;) {
    cast_get_status(s, &st);
    if (st.conn == CAST_DISCONNECTED) {
      printf("\nReceiver disconnected.\n");
      break;
    }
    printf("\r%-9s ", state_name(st.state));
    put_clock(st.current_time);
    if (st.duration > 0) {
      printf(" / ");
      put_clock(st.duration);
    }
    printf("   ");
    if (st.state == CAST_PLAYING)
      seen_playing = true;
    if (seen_playing && st.state == CAST_IDLE) {
      printf("\nPlayback ended.\n");
      break;
    }
    msleep(1000);
  }
  cast_close(s);
  return 0;
}

int main(int argc, char **argv)
{
  const char *cmd = argc > 1 ? argv[1] : "discover";

  if (streq(cmd, "discover") || streq(cmd, "list") || streq(cmd, "scan"))
    return cmd_discover();
  if (streq(cmd, "play")) {
    if (argc < 4) {
      printf("usage: cast play <name|index> <url> [content-type]\n");
      return 1;
    }
    return cmd_play(argv[2], argv[3], argc > 4 ? argv[4] : "");
  }
  printf("usage:\n  cast discover\n  cast play <name|index> <url> "
         "[content-type]\n");
  return 1;
}
