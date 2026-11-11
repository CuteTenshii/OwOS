/* sources.c, one loader per Device Hub category: /sys and /proc text in, rows out */
#include "devhub.h"
#include "owos.h"

#define TEXT_MAX 8192
static char text[TEXT_MAX];

/* Read a whole file into buf, NUL-terminated with trailing newlines trimmed; -1 if it can't be read. */
static int slurp(const char *path, char *buf, int max)
{
  int n = readfile(path, buf, max - 1);
  if (n < 0) {
    buf[0] = 0;
    return -1;
  }
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    n--;
  buf[n] = 0;
  return n;
}

/* A one-line attribute; empty when it is missing. */
static const char *attr(const char *path)
{
  static char v[128];
  slurp(path, v, sizeof v);
  return v;
}

static bool dir_has(const char *dir, const char *name)
{
  struct dirent_u d;
  for (int i = 0; readdir(dir, i, &d) == 1; i++)
    if (streq(d.name, name))
      return true;
  return false;
}

static void add(struct page *pg, int kind, const char *label, const char *value)
{
  if (pg->n >= MAX_ROWS)
    return;
  struct row *r = &pg->rows[pg->n++];
  r->kind = (unsigned char)kind;
  r->indent = 0;
  dh_copy(r->label, label ? label : "", sizeof r->label);
  dh_copy(r->value, value ? value : "", sizeof r->value);
  dh_tidy(r->label);
  dh_tidy(r->value);
}

static void head(struct page *pg, const char *title) { add(pg, ROW_HEAD, title, 0); }
static void note(struct page *pg, const char *s) { add(pg, ROW_NOTE, 0, s); }
static void fail(struct page *pg, const char *path)
{
  char s[96] = "Couldn't read ";
  dh_append(s, path, sizeof s);
  add(pg, ROW_ERROR, 0, s);
}

/* A label/value row, left out when the kernel reported nothing for it. */
static void field(struct page *pg, const char *label, const char *value)
{
  if (value && value[0])
    add(pg, ROW_FIELD, label, value);
}

static void field_or(struct page *pg, const char *label, const char *value, const char *fallback)
{
  add(pg, ROW_FIELD, label, value && value[0] ? value : fallback);
}

/* value + suffix, e.g. "60" + " Hz" */
static void field_unit(struct page *pg, const char *label, const char *value, const char *unit)
{
  char s[64];
  if (!value || !value[0])
    return;
  dh_copy(s, value, sizeof s);
  dh_append(s, unit, sizeof s);
  add(pg, ROW_FIELD, label, s);
}

static void utoa_s(unsigned v, char *out)
{
  char t[12];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  int i = 0;
  while (n)
    out[i++] = t[--n];
  out[i] = 0;
}

static void itoa_s(int v, char *out)
{
  if (v < 0) {
    *out++ = '-';
    v = -v;
  }
  utoa_s((unsigned)v, out);
}

/* "1280x720" -> "1280 x 720" */
static void spaced_mode(const char *m, char *out, int max)
{
  int i = 0;
  for (; *m && i < max - 3; m++) {
    if (*m == 'x') {
      out[i++] = ' ';
      out[i++] = 'x';
      out[i++] = ' ';
    } else
      out[i++] = *m;
  }
  out[i] = 0;
}

static bool begins(const char *s, const char *prefix)
{
  while (*prefix)
    if (*s++ != *prefix++)
      return false;
  return true;
}

static bool always(void) { return true; }

static bool has_class(const char *name) { return dir_has("/sys/class", name); }

/* System */

static void load_system(struct page *pg)
{
  char v[96], s[64];

  head(pg, "Computer");
  field_or(pg, "Manufacturer", attr("/sys/class/dmi/id/sys_vendor"), "Not reported");
  field_or(pg, "Model", attr("/sys/class/dmi/id/product_name"), "Not reported");
  field(pg, "Firmware", attr("/sys/firmware/type"));

  head(pg, "Processor");
  if (slurp("/proc/cpuinfo", text, TEXT_MAX) < 0) {
    fail(pg, "/proc/cpuinfo");
  } else {
    if (dh_field(text, "model", v, sizeof v))
      field(pg, "Model", v);
    if (dh_field(text, "mhz", v, sizeof v) && dh_num(v))
      field_unit(pg, "Clock speed", v, " MHz");
    if (dh_field(text, "cpus", v, sizeof v))
      field(pg, "Cores", v);
  }

  head(pg, "Memory");
  if (slurp("/proc/meminfo", text, TEXT_MAX) < 0) {
    fail(pg, "/proc/meminfo");
  } else {
    if (dh_field(text, "total", v, sizeof v)) {
      dh_size(dh_num(v) * 1024, s, sizeof s);
      field(pg, "Installed", s);
    }
    if (dh_field(text, "used", v, sizeof v)) {
      dh_size(dh_num(v) * 1024, s, sizeof s);
      field(pg, "In use", s);
    }
  }
}

/* Display */

static void load_display(struct page *pg)
{
  const char *fb = "/sys/class/graphics/fb0/";
  char path[64], v[64];
#define FB(name) (dh_copy(path, fb, sizeof path), dh_append(path, name, sizeof path), attr(path))

  head(pg, "Monitor");
  field_or(pg, "Name", FB("monitor"), "Unknown (no EDID)");
  bool internal = FB("internal")[0] == '1';
  bool hdmi = FB("hdmi")[0] == '1';
  field(pg, "Connection", internal ? "Built-in panel" : hdmi ? "HDMI" : "External");
  if (hdmi)
    field(pg, "HDMI audio", FB("hdmi_audio")[0] == '1' ? "Supported" : "Not supported");
  unsigned dpi = (unsigned)dh_num(FB("dpi"));
  if (dpi) {
    utoa_s(dpi, v);
    field_unit(pg, "Pixel density", v, " DPI");
  }

  head(pg, "Output");
  spaced_mode(FB("mode"), v, sizeof v);
  if (dh_num(v))
    field(pg, "Resolution", v);
  else
    field(pg, "Resolution", "Text mode");
  field_unit(pg, "Refresh rate", FB("refresh"), " Hz");
  field_unit(pg, "Desktop frame rate", FB("rate"), " Hz");
  dh_copy(v, FB("scale_active"), sizeof v);
  if (dh_num(v)) {
    char s[32];
    utoa_s((unsigned)dh_num(v) * 100, s);
    dh_append(s, "%", sizeof s);
    if (FB("scale")[0] == 'a')
      dh_append(s, " (automatic)", sizeof s);
    field(pg, "Interface scale", s);
  }
  int nmodes = 0;
  if (slurp("/sys/class/graphics/fb0/modes", text, TEXT_MAX) > 0)
    for (const char *p = text; dh_line(&p, v, sizeof v);)
      nmodes += v[0] != 0;
  if (nmodes > 1) {
    utoa_s((unsigned)nmodes, v);
    field(pg, "Available modes", v);
  } else if (nmodes == 1) {
    field(pg, "Available modes", "Fixed by the bootloader");
  }
#undef FB

  if (has_class("backlight")) {
    const char *bl = "/sys/class/backlight/intel_backlight/";
    head(pg, "Backlight");
    dh_copy(path, bl, sizeof path);
    dh_append(path, "actual_brightness", sizeof path);
    unsigned long long cur = dh_num(attr(path));
    dh_copy(path, bl, sizeof path);
    dh_append(path, "max_brightness", sizeof path);
    unsigned long long max = dh_num(attr(path));
    if (max) {
      utoa_s((unsigned)((cur * 100 + max / 2) / max), v);
      field_unit(pg, "Level", v, "%");
    } else {
      field(pg, "Level", "Not reported");
    }
  }

  note(pg, "Change the resolution, frame rate and scale in Settings > Display.");
}

/* Storage */

static bool storage_present(void) { return has_class("block") || slurp("/proc/storage", text, TEXT_MAX) > 0; }

static void load_storage(struct page *pg)
{
  char line[160], s[96], sz[24];
  int drives = 0;

  head(pg, "Drives");
  if (slurp("/proc/storage", text, TEXT_MAX) < 0) {
    fail(pg, "/proc/storage");
  } else {
    struct dh_disk d;
    for (const char *p = text; dh_line(&p, line, sizeof line);)
      if (dh_parse_disk(line, &d)) {
        dh_copy(s, d.model[0] ? d.model : "Unnamed drive", sizeof s);
        if (d.sectors) {
          dh_size(d.sectors * 512, sz, sizeof sz);
          dh_append(s, ", ", sizeof s);
          dh_append(s, sz, sizeof s);
        }
        field(pg, d.type, s);
        drives++;
      }
    if (!drives)
      note(pg, "No internal drives found.");
  }

  struct dirent_u e;
  bool any = false;
  for (int i = 0; readdir("/sys/class/block", i, &e) == 1; i++) {
    char path[96];
    if (!any)
      head(pg, "Block devices");
    any = true;
    dh_copy(path, "/sys/class/block/", sizeof path);
    dh_append(path, e.name, sizeof path);
    dh_append(path, "/model", sizeof path);
    dh_copy(s, attr(path), sizeof s);
    if (!s[0])
      dh_copy(s, "Unnamed device", sizeof s);
    dh_copy(path, "/sys/class/block/", sizeof path);
    dh_append(path, e.name, sizeof path);
    dh_append(path, "/size", sizeof path);
    unsigned long long sectors = dh_num(attr(path));
    if (sectors) {
      dh_size(sectors * 512, sz, sizeof sz);
      dh_append(s, ", ", sizeof s);
      dh_append(s, sz, sizeof s);
    }
    field(pg, e.name, s);
  }
}

/* Network */

static bool net_present(void) { return has_class("net"); }

static void load_network(struct page *pg)
{
  struct dirent_u e;
  int n = 0;
  for (int i = 0; readdir("/sys/class/net", i, &e) == 1; i++, n++) {
    char base[64], path[96], v[64];
    dh_copy(base, "/sys/class/net/", sizeof base);
    dh_append(base, e.name, sizeof base);
    dh_append(base, "/", sizeof base);
#define NET(name) (dh_copy(path, base, sizeof path), dh_append(path, name, sizeof path), attr(path))
    head(pg, e.name);
    field(pg, "Link", NET("carrier")[0] == '1' ? "Connected" : "Disconnected");
    field(pg, "MAC address", NET("address"));
    dh_copy(v, NET("ipaddr"), sizeof v);
    field_or(pg, "IPv4 address", streq(v, "0.0.0.0") ? "" : v, "Not assigned");
    field_or(pg, "IPv6 address", NET("ip6addr"), "None yet");
    field(pg, "IPv6 link-local", NET("ip6ll"));
    field(pg, "IPv6 DNS server", NET("dns6"));
#undef NET
  }
  if (!n)
    note(pg, "No network interfaces found.");
}

/* Input */

static void load_input(struct page *pg)
{
  static const char *const speed[] = {"Slowest", "Slow", "Normal", "Fast", "Fastest"};
  char v[96];

  head(pg, "Keyboard");
  const char *layout = attr("/sys/class/input/kbd0/layout");
  field_or(pg, "Layout", streq(layout, "us") ? "US English" : streq(layout, "fr") ? "French" : layout, "Not reported");

  head(pg, "Pointer");
  unsigned lv = (unsigned)dh_num(attr("/sys/class/input/mouse0/speed"));
  field_or(pg, "Speed", lv >= 1 && lv <= 5 ? speed[lv - 1] : "", "Not reported");

  if (dir_has("/sys/class/input", "js0")) {
    head(pg, "Gamepad");
    dh_copy(v, attr("/sys/class/input/js0/id"), sizeof v);
    /* "<token> <vid> <pid> <label>" */
    const char *p = v;
    char w[3][16];
    int k = 0;
    for (; k < 3; k++) {
      while (*p == ' ')
        p++;
      int i = 0;
      while (*p && *p != ' ' && i < 15)
        w[k][i++] = *p++;
      w[k][i] = 0;
      if (!i)
        break;
    }
    while (*p == ' ')
      p++;
    if (k == 3) {
      char id[16];
      field_or(pg, "Type", p, w[0]);
      dh_copy(id, w[1], sizeof id);
      dh_append(id, ":", sizeof id);
      dh_append(id, w[2], sizeof id);
      field(pg, "USB ID", id);
    } else {
      field(pg, "Type", "Unidentified controller");
    }
    field(pg, "Rumble", attr("/sys/class/input/js0/rumble")[0] == '1' ? "Supported" : "Not supported");
  }

  note(pg, "Change the layout and pointer speed in Settings.");
}

/* Sound */

static bool sound_present(void) { return has_class("sound"); }

static void load_sound(struct page *pg)
{
  char cur[64], line[64], v[32];
  const char *card = "/sys/class/sound/card0/";
  char path[64];
#define SND(name) (dh_copy(path, card, sizeof path), dh_append(path, name, sizeof path), path)

  head(pg, "Playback");
  unsigned vol = (unsigned)dh_num(attr(SND("volume")));
  utoa_s(vol, v);
  dh_append(v, "%", sizeof v);
  if (attr(SND("muted"))[0] == '1')
    dh_append(v, ", muted", sizeof v);
  field(pg, "Volume", v);

  dh_copy(cur, attr(SND("output")), sizeof cur);
  head(pg, "Outputs");
  int n = 0;
  if (slurp(SND("outputs"), text, TEXT_MAX) < 0) {
    fail(pg, path);
  } else {
    for (const char *p = text; dh_line(&p, line, sizeof line);)
      if (line[0]) {
        field(pg, line, streq(line, cur) ? "Active" : "Available");
        n++;
      }
    if (!n)
      note(pg, "The sound card reports no outputs.");
  }

  if (dir_has("/sys/class/sound/card0", "capture")) {
    head(pg, "Recording");
    slurp(SND("capture"), text, TEXT_MAX);
    field(pg, "Microphone input", begins(text, "capturing") ? "Recording" : "Idle");
  }
#undef SND
  note(pg, "Pick the output device in Settings > Sound.");
}

/* Bluetooth */

static bool bt_present(void) { return has_class("bluetooth"); }

static const char *bt_kind_label(const char *k)
{
  if (streq(k, "audio"))
    return "Audio";
  if (streq(k, "gamepad"))
    return "Gamepad";
  if (streq(k, "input"))
    return "Input device";
  return "";
}

#define MAX_BT 32
static struct dh_bt bt_devs[MAX_BT];
static int bt_n;

static const char *bt_name_of(const char *addr)
{
  for (int i = 0; i < bt_n; i++)
    if (streq(bt_devs[i].addr, addr) && bt_devs[i].name[0])
      return bt_devs[i].name;
  return addr;
}

static void load_bluetooth(struct page *pg)
{
  static char status[2048];
  char line[160], v[96], w[24];
  const char *dir = "/sys/class/bluetooth/hci0/";
  char path[64];

  bt_n = 0;
  dh_copy(path, dir, sizeof path);
  dh_append(path, "devices", sizeof path);
  if (slurp(path, text, TEXT_MAX) >= 0)
    for (const char *p = text; bt_n < MAX_BT && dh_line(&p, line, sizeof line);)
      if (dh_parse_bt(line, &bt_devs[bt_n]))
        bt_n++;

  head(pg, "Controller");
  dh_copy(path, dir, sizeof path);
  dh_append(path, "status", sizeof path);
  if (slurp(path, status, sizeof status) < 0) {
    fail(pg, path);
    return;
  }
  if (dh_field(status, "state", v, sizeof v)) {
    static const char *const state[][2] = {
        {"off", "Off"}, {"starting", "Starting"}, {"ready", "Ready"}, {"failed", "Failed to start"}};
    const char *label = v;
    for (int i = 0; i < 4; i++)
      if (streq(v, state[i][0]))
        label = state[i][1];
    field(pg, "State", label);
  }
  if (dh_field(status, "address", v, sizeof v))
    field(pg, "Address", v);
  if (dh_field(status, "scanning", v, sizeof v))
    field(pg, "Scanning", streq(v, "yes") ? "Yes" : "No");
  if (dh_field(status, "a2dp", v, sizeof v))
    field(pg, "Audio stream", v);
  if (dh_field(status, "pairing", v, sizeof v))
    field(pg, "Last pairing", v);

  bool any = false;
  for (const char *p = status; dh_line(&p, line, sizeof line);) {
    if (!begins(line, "bond "))
      continue;
    if (!any)
      head(pg, "Paired");
    any = true;
    field(pg, bt_name_of(line + 5), line + 5);
  }

  any = false;
  for (const char *p = status; dh_line(&p, line, sizeof line);) {
    /* "connection <addr> br|le[ encrypted]" */
    if (!begins(line, "connection "))
      continue;
    const char *s = line + 11;
    int i = 0;
    while (s[i] && s[i] != ' ' && i < (int)sizeof w - 1) {
      w[i] = s[i];
      i++;
    }
    w[i] = 0;
    s += i;
    while (*s == ' ')
      s++;
    dh_copy(v, s[0] == 'l' ? "Bluetooth LE" : "Classic", sizeof v);
    if (s[2] == ' ')
      dh_append(v, ", encrypted", sizeof v);
    if (!any)
      head(pg, "Connected");
    any = true;
    field(pg, bt_name_of(w), v);
  }

  head(pg, "Nearby devices");
  if (!bt_n)
    note(pg, "None discovered. Devices show up here while the controller scans.");
  for (int i = 0; i < bt_n; i++) {
    struct dh_bt *d = &bt_devs[i];
    char sig[12];
    v[0] = 0;
    const char *k = bt_kind_label(d->kind);
    if (k[0]) {
      dh_append(v, k, sizeof v);
      dh_append(v, ", ", sizeof v);
    }
    dh_append(v, d->le ? "LE, " : "Classic, ", sizeof v);
    itoa_s(d->rssi, sig);
    dh_append(v, sig, sizeof v);
    dh_append(v, " dBm, ", sizeof v);
    dh_append(v, d->addr, sizeof v);
    field(pg, d->name[0] ? d->name : "Unnamed device", v);
  }
}

/* Camera */

static bool cam_present(void) { return has_class("video"); }

static void load_camera(struct page *pg)
{
  char line[160], v[96], label[40];

  head(pg, "Camera");
  if (slurp("/sys/class/video/cam0/info", text, TEXT_MAX) < 0) {
    fail(pg, "/sys/class/video/cam0/info");
    return;
  }
  if (dh_field(text, "name", v, sizeof v))
    field_or(pg, "Name", v, "Unnamed camera");
  if (dh_field(text, "id", v, sizeof v))
    field(pg, "USB ID", v);

  const char *ctl = attr("/sys/class/video/cam0/control");
  field(pg, "Status", ctl[0] == 's' ? "Streaming" : "Idle");

  bool any = false;
  for (const char *p = text; dh_line(&p, line, sizeof line);) {
    /* "format MJPG 1280x720 30 15" */
    if (!begins(line, "format "))
      continue;
    const char *s = line + 7;
    int i = 0;
    while (*s && *s != ' ' && i < 8)
      label[i++] = *s++;
    label[i++] = ' ';
    label[i] = 0;
    while (*s == ' ')
      s++;
    char mode[16];
    int m = 0;
    while (*s && *s != ' ' && m < 15)
      mode[m++] = *s++;
    mode[m] = 0;
    char sp[24];
    spaced_mode(mode, sp, sizeof sp);
    dh_append(label, sp, sizeof label);
    while (*s == ' ')
      s++;
    v[0] = 0;
    for (bool first = true; *s; first = false) {
      char fps[8];
      int f = 0;
      while (*s && *s != ' ' && f < 7)
        fps[f++] = *s++;
      fps[f] = 0;
      while (*s == ' ')
        s++;
      if (!first)
        dh_append(v, ", ", sizeof v);
      dh_append(v, fps, sizeof v);
    }
    if (v[0])
      dh_append(v, " fps", sizeof v);
    if (!any)
      head(pg, "Formats");
    any = true;
    field_or(pg, label, v, "No frame rates listed");
  }
}

/* Battery */

static bool battery_present(void) { return dir_has("/sys/power", "battery"); }

static void load_battery(struct page *pg)
{
  char v[32];
  head(pg, "Battery");
  field_unit(pg, "Charge", attr("/sys/power/battery/capacity"), "%");
  dh_copy(v, attr("/sys/power/battery/status"), sizeof v);
  field_or(pg, "Status", v, "Not reported");
  unsigned mins = (unsigned)dh_num(attr("/sys/power/battery/minutes"));
  if (mins && streq(v, "Discharging")) {
    dh_minutes(mins, v, sizeof v);
    field(pg, "Time remaining", v);
  }
}

/* USB */

static void load_usb(struct page *pg)
{
  char line[200], v[120], label[40];
  if (slurp("/proc/usb", text, TEXT_MAX) < 0) {
    fail(pg, "/proc/usb");
    return;
  }
  int controllers = 0;
  for (const char *p = text; dh_line(&p, line, sizeof line);) {
    struct dh_usbhc hc;
    struct dh_usb u;
    if (dh_parse_usbhc(line, &hc)) {
      dh_copy(label, "Controller ", sizeof label);
      dh_append(label, hc.addr, sizeof label);
      head(pg, label);
      field(pg, "Type", hc.type);
      field(pg, "PCI ID", hc.ids);
      controllers++;
    } else if (dh_parse_usb(line, &u)) {
      dh_copy(label, "Port ", sizeof label);
      dh_append(label, u.port, sizeof label);
      dh_copy(v, dh_usb_class(u.cls), sizeof v);
      dh_append(v, ", ", sizeof v);
      dh_append(v, u.ids, sizeof v);
      if (u.rest[0]) {
        dh_append(v, ", ", sizeof v);
        dh_append(v, u.rest, sizeof v);
      }
      field(pg, label, v);
      if (pg->n)
        pg->rows[pg->n - 1].indent = (unsigned char)(u.depth > 6 ? 6 : u.depth);
    } else {
      const char *s = line;
      while (*s == ' ')
        s++;
      if (begins(s, "xhci:"))
        s += 5;
      while (*s == ' ')
        s++;
      if (streq(s, "no usable device on any port"))
        note(pg, "No devices connected.");
      else if (streq(s, "no USB controllers found"))
        note(pg, "No USB controllers found.");
      else if (begins(s, "port ")) {
        /* UHCI root port: "port 1: no device" */
        dh_copy(label, "Port ", sizeof label);
        int i = 5;
        char num[8];
        int k = 0;
        while (s[i] && s[i] != ':' && k < (int)sizeof num - 1)
          num[k++] = s[i++];
        num[k] = 0;
        dh_append(label, num, sizeof label);
        const char *rest = s + i + (s[i] == ':');
        while (*rest == ' ')
          rest++;
        field(pg, label, streq(rest, "no device") ? "Empty" : rest);
      } else if (s[0]) {
        note(pg, s);
      }
    }
  }
  if (!controllers && !pg->n)
    note(pg, "No USB controllers found.");
}

/* PCI */

#define MAX_PCI 96
static struct dh_pci pci[MAX_PCI];

static bool pci_present(void) { return slurp("/proc/pci", text, TEXT_MAX) > 0; }

static void load_pci(struct page *pg)
{
  char line[160], v[120];
  if (slurp("/proc/pci", text, TEXT_MAX) < 0) {
    fail(pg, "/proc/pci");
    return;
  }
  int n = 0;
  for (const char *p = text; n < MAX_PCI && dh_line(&p, line, sizeof line);)
    if (dh_parse_pci(line, &pci[n]))
      n++;
  if (!n) {
    note(pg, "No PCI devices found.");
    return;
  }
  /* Group by base class, in class order, keeping bus order within a group */
  for (unsigned base = 0; base < 0x100; base++) {
    bool any = false;
    for (int i = 0; i < n; i++) {
      if (pci[i].cls >> 8 != base)
        continue;
      if (!any)
        head(pg, dh_pci_class(pci[i].cls));
      any = true;
      dh_copy(v, pci[i].name[0] ? pci[i].name : dh_pci_class(pci[i].cls), sizeof v);
      dh_append(v, ", ", sizeof v);
      dh_append(v, pci[i].ids, sizeof v);
      field(pg, pci[i].addr, v);
    }
  }
}

const struct category categories[] = {
    {"System", always, load_system, false, false},
    {"Display", always, load_display, false, false},
    {"Storage", storage_present, load_storage, false, false},
    {"Network", net_present, load_network, true, false},
    {"Input", always, load_input, true, false},
    {"Sound", sound_present, load_sound, true, false},
    {"Bluetooth", bt_present, load_bluetooth, true, false},
    {"Camera", cam_present, load_camera, true, false},
    {"Battery", battery_present, load_battery, true, false},
    {"USB", always, load_usb, false, true},
    {"PCI", pci_present, load_pci, false, false},
};
const int ncategories = (int)(sizeof categories / sizeof categories[0]);
