/* about.c, the About page */
#include "settings.h"

static char about_line[10][72]; /* one formatted line per row */
static int about_n;

static void aline(const char *s)
{
  if (about_n >= 10)
    return;
  int i = 0;
  while (s[i] && i < 71) {
    about_line[about_n][i] = s[i];
    i++;
  }
  about_line[about_n][i] = 0;
  about_n++;
}

void about_load(void)
{
  char buf[512], v[96], v2[32], line[72];
  about_n = 0;

  aline("OwOS");
  aline("");

  if (read_proc("/sys/class/dmi/id/product_name", v, sizeof v) > 0) {
    strcpy(line, "Host: ");
    strcat(line, v);
    aline(line);
  }

  if (read_proc("/proc/cpuinfo", buf, sizeof buf) > 0) {
    strcpy(line, "CPU: ");
    if (field(buf, "model", v, sizeof v))
      strcat(line, v);
    else
      strcat(line, "unknown");
    if (field(buf, "mhz", v2, sizeof v2)) {
      strcat(line, " @ ");
      strcat(line, v2);
      strcat(line, " MHz");
    }
    aline(line);
  }

  if (read_proc("/proc/meminfo", buf, sizeof buf) > 0) {
    unsigned used = 0, total = 0;
    if (field(buf, "used", v, sizeof v))
      for (const char *p = v; *p >= '0' && *p <= '9'; p++)
        used = used * 10 + (*p - '0');
    if (field(buf, "total", v, sizeof v))
      for (const char *p = v; *p >= '0' && *p <= '9'; p++)
        total = total * 10 + (*p - '0');
    strcpy(line, "Memory: ");
    uitoa(used / 1024, v);
    strcat(line, v);
    strcat(line, " / ");
    uitoa(total / 1024, v);
    strcat(line, v);
    strcat(line, " MiB");
    aline(line);
  }

  if (read_proc("/sys/firmware/type", v, sizeof v) > 0) {
    strcpy(line, "Firmware: ");
    strcat(line, v);
    aline(line);
  }

  if (read_proc("/proc/uptime", v, sizeof v) > 0) {
    strcpy(line, "Uptime: ");
    strcat(line, v);
    strcat(line, " s");
    aline(line);
  }
}

void about_draw(int cx, int cw)
{
  int maxw = cw - 32;
  for (int i = 0; i < about_n; i++)
    gfx_ui_text_clip(cx + 16, 16 + i * 20, about_line[i], i == 0 ? FG_WHITE : FG_TEXT, maxw);
}
