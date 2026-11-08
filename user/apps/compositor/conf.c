/* conf.c: persisted desktop tunables */
#include "comp.h"

/* Persisted desktop tunables */

/* One row per tunable: config file + key <-> sysfs attribute */
static struct tunable tun[] = {
    {CONF_DISPLAY, "mode", "/sys/class/graphics/fb0/mode", "", 0},
    {CONF_DISPLAY, "rate", DISPLAY_RATE, "", 0},
    {CONF_DISPLAY, "scale", "/sys/class/graphics/fb0/scale", "", 0},
    {CONF_INPUT, "layout", "/sys/class/input/kbd0/layout", "", 0},
    {CONF_INPUT, "mouse", "/sys/class/input/mouse0/speed", "", 0},
    {CONF_SOUND, "sound", "/sys/class/sound/card0/output", "", 0},
};
static const char *const conf_files[] = {CONF_DISPLAY, CONF_INPUT, CONF_SOUND};

int scan_uint(const char *p)
{
  int v = 0;
  while (*p >= '0' && *p <= '9')
    v = v * 10 + (*p++ - '0');
  return v;
}

/* Read a sysfs attribute into `out`, trimming trailing whitespace */
int node_read(const char *path, char *out, int max)
{
  int n = readfile(path, out, max - 1);
  if (n < 0)
    n = 0;
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
    n--;
  out[n] = 0;
  return n;
}

/* Fill each tunable's `cur` from its /etc config ("key=value" lines) */
void conf_load(void)
{
  for (int f = 0; f < NCONF; f++) {
    char buf[256];
    int n = readfile(conf_files[f], buf, sizeof buf - 1);
    if (n <= 0)
      continue;
    buf[n] = 0;
    for (char *p = buf; *p;) {
      char *line = p;
      while (*p && *p != '\n')
        p++;
      if (*p == '\n')
        *p++ = 0;
      char *eq = line;
      while (*eq && *eq != '=')
        eq++;
      if (*eq != '=')
        continue;
      *eq = 0;
      char *val = eq + 1;
      for (int m = strlen(val); m > 0 && (val[m - 1] == '\r' || val[m - 1] == ' ');)
        val[--m] = 0;
      for (int i = 0; i < NTUN; i++)
        if (streq(line, tun[i].key)) {
          int j = 0;
          while (val[j] && j < (int)sizeof tun[i].cur - 1) {
            tun[i].cur[j] = val[j];
            j++;
          }
          tun[i].cur[j] = 0;
          tun[i].known = tun[i].cur[0] != 0;
          break;
        }
    }
  }
}

/* Rewrite `file` from the known tunables that belong to it */
static void conf_save_file(const char *file)
{
  char buf[256], *o = buf, *end = buf + sizeof buf;
  for (int i = 0; i < NTUN; i++) {
    if (tun[i].file != file || !tun[i].known)
      continue;
    const char *k = tun[i].key, *v = tun[i].cur;
    if (o + strlen(k) + strlen(v) + 2 >= end)
      break;
    while (*k)
      *o++ = *k++;
    *o++ = '=';
    while (*v)
      *o++ = *v++;
    *o++ = '\n';
  }
  if (o > buf)
    writefile(file, buf, (int)(o - buf));
}

/* Persist every domain whose tunable changed */
void conf_save(void)
{
  for (int f = 0; f < NCONF; f++)
    conf_save_file(conf_files[f]);
}

/* Push saved values into kernel nodes */
void tun_restore(void)
{
  for (int i = 0; i < NTUN; i++)
    if (tun[i].known)
      writefile(tun[i].node, tun[i].cur, strlen(tun[i].cur));
  for (int i = 0; i < NTUN; i++) {
    char v[32];
    if (node_read(tun[i].node, v, sizeof v) > 0) {
      strcpy(tun[i].cur, v);
      tun[i].known = true;
    }
  }
}

/* Poll the live nodes; update any that changed */
bool tun_poll(void)
{
  bool changed = false;
  char v[32];
  for (int i = 0; i < NTUN; i++) {
    if (node_read(tun[i].node, v, sizeof v) <= 0)
      continue;
    if (!streq(v, tun[i].cur)) {
      strcpy(tun[i].cur, v);
      tun[i].known = true;
      changed = true;
    }
  }
  return changed;
}

/* The live frame-pacing target (Hz) from the kernel */
int rate_now(void)
{
  char v[16];
  if (node_read(DISPLAY_RATE, v, sizeof v) <= 0)
    return 0;
  return scan_uint(v);
}
