/* trayd.c: publishes the sound, battery and network tray icons, refreshing them when the state changes */
#include "owos.h"
#include "tray.h"

static int sysfs_int(const char *path)
{
  char b[16];
  int n = readfile(path, b, sizeof b - 1);
  if (n <= 0)
    return -1;
  int v = 0, digits = 0;
  for (int i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++, digits++)
    v = v * 10 + (b[i] - '0');
  return digits ? v : -1;
}

/* Show `icon` under `id`, or remove the entry when icon is NULL; `last` caches what is shown */
static void show(const char *id, const char **last, const char *icon, const char *action)
{
  if (*last == icon)
    return;
  struct tray_icon t = {.pid = getpid(), .id = id, .icon = icon, .action = action};
  if ((icon ? tray_set(&t) : tray_remove(id)) == 0)
    *last = icon;
}

static const char *volume_icon(void)
{
  int vol = sysfs_int("/sys/class/sound/card0/volume");
  if (vol < 0)
    return 0;
  if (vol == 0 || sysfs_int("/sys/class/sound/card0/muted") > 0)
    return "audio-volume-muted";
  return vol < 50 ? "audio-volume-low" : "audio-volume-high";
}

static const char *battery_icon(void)
{
  int pct = sysfs_int("/sys/power/battery/capacity");
  if (pct < 0)
    return 0;
  char st[16] = "";
  readfile("/sys/power/battery/status", st, sizeof st - 1);
  if (st[0] == 'C')
    return "battery-charging";
  return pct <= 20 ? "battery-low" : pct <= 60 ? "battery-medium" : "battery-full";
}

static const char *network_icon(void)
{
  char b[16] = "";
  if (readfile("/sys/class/net/eth0/carrier", b, sizeof b - 1) <= 0 || b[0] != '1')
    return 0;
  readfile("/sys/class/net/eth0/ipaddr", b, sizeof b - 1);
  return streq(b, "0.0.0.0\n") ? "network-ethernet-disconnected" : "network-ethernet";
}

int main(void)
{
  const char *vol = 0, *bat = 0, *net = 0;
  for (;;) {
    show("volume", &vol, volume_icon(), "settings");
    show("battery", &bat, battery_icon(), 0);
    show("network", &net, network_icon(), 0);
    msleep(1000);
  }
}
