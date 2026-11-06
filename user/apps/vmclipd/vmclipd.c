/* vmclipd.c, VMware host<->guest clipboard bridge, in user space */

#include "clipboard.h"
#include "owos.h"
#include <stdbool.h>

#define CLIP_MAX 8192

static bool same(const char *a, const char *b, int n)
{
  for (int i = 0; i < n; i++)
    if (a[i] != b[i])
      return false;
  return true;
}

int main(void)
{
  static char scratch[CLIP_MAX];
  static char last[CLIP_MAX];
  int last_len = 0;

  for (bool first = true;; first = false) {
    if (!first)
      msleep(1000); /* how often to check the host clipboard */

    /* host -> clipd: the host offered a new selection */
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

    /* clipd -> host: a guest copy changed the newest text clip */
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
