/* util.c, string and drawing helpers shared by the Settings pages */
#include "settings.h"

void uitoa(unsigned v, char *out)
{
  char t[12];
  int n = 0;
  if (v == 0) {
    out[0] = '0';
    out[1] = 0;
    return;
  }
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  int i = 0;
  while (n)
    out[i++] = t[--n];
  out[i] = 0;
}

int patoi(const char *s)
{
  int v = 0;
  while (*s == ' ')
    s++;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (*s++ - '0');
  return v;
}

/* Read a /proc file into `buf` (NUL-terminated, trailing newline trimmed) */
int read_proc(const char *name, char *buf, int max)
{
  int n = readfile(name, buf, max - 1);
  if (n < 0)
    n = 0;
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    n--;
  buf[n] = 0;
  return n;
}

/* Copy the text after "<key> " on its line in `buf` into out (trimmed at EOL) */
bool field(const char *buf, const char *key, char *out, int max)
{
  for (const char *p = buf; *p;) {
    int i = 0;
    while (key[i] && p[i] == key[i])
      i++;
    if (!key[i] && p[i] == ' ') {
      const char *v = p + i + 1;
      int n = 0;
      while (v[n] && v[n] != '\n' && n < max - 1) {
        out[n] = v[n];
        n++;
      }
      out[n] = 0;
      return true;
    }
    while (*p && *p != '\n')
      p++;
    if (*p == '\n')
      p++;
  }
  return false;
}
