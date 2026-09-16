/* vmclipd.c — VMware host<->guest clipboard bridge, in user space.
 *
 * The kernel exposes the VMware host clipboard as a device channel at
 * /proc/hostclip (read = the host's newest selection, or 0 bytes when nothing
 * is new; write = push guest text up to the host). This daemon bridges that
 * channel to the system clipboard (/bin/clipd, via the clipboard lib): a host
 * copy becomes the newest clip, and a guest copy is offered to the host. Text
 * only — the VMware backdoor channel carries text.
 *
 * `last` remembers whatever text we last bridged in either direction so the two
 * sides don't ping-pong an echo forever. Real VMware only: off VMware the
 * /proc/hostclip read returns -1, so we bow out at once rather than spin. This
 * replaces the old in-kernel vmclip_thread — the kernel no longer talks to
 * clipd. */

#include "clipboard.h"
#include "owos.h"

#define CLIP_MAX 8192

static int same(const char *a, const char *b, int n)
{
  for (int i = 0; i < n; i++)
    if (a[i] != b[i])
      return 0;
  return 1;
}

int main(void)
{
  static char scratch[CLIP_MAX];
  static char last[CLIP_MAX];
  int last_len = 0;

  for (int first = 1;; first = 0) {
    if (!first)
      msleep(1000); /* how often to check the host clipboard */

    /* host -> clipd: the host offered a new selection. A -1 here means we are
     * not on VMware (the node reports it), so there's nothing to bridge. */
    int n = readfile("/proc/hostclip", scratch, sizeof scratch);
    if (n < 0)
      return 0;
    if (n > 0 && (n != last_len || !same(scratch, last, n))) {
      clip_put_text(scratch, n);
      for (int i = 0; i < n; i++)
        last[i] = scratch[i];
      last_len = n;
      continue; /* don't echo it straight back to the host */
    }

    /* clipd -> host: a guest copy changed the newest text clip. */
    char mime[32];
    void *data = 0;
    int len = 0;
    if (clip_get(0, mime, sizeof mime, &data, &len) == 0) {
      int is_text = mime[0] == 't' && mime[1] == 'e' && mime[2] == 'x' && mime[3] == 't' && mime[4] == '/';
      if (is_text && len > 0 && len <= CLIP_MAX && (len != last_len || !same((char *)data, last, len))) {
        writefile("/proc/hostclip", data, len);
        for (int i = 0; i < len; i++)
          last[i] = ((char *)data)[i];
        last_len = len;
      }
      free(data);
    }
  }
  return 0;
}
