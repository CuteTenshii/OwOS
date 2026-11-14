/* neofetch.c, a system-info splash: the OwO logo beside the stats */

#include "owos.h"
#include <stdbool.h>

static const char *logo[] = {
    "   /\\___/\\", "  /       \\", "  ( O w O )", "  (   v   )", "   \\     /", "    \\___/",
};
#define LOGO_LINES 6
#define LOGO_W 16

/* 16-colour ANSI SGR escapes, the terminal renders them (term.c) */
#define CYAN "\033[96m"
#define GREEN "\033[92m"
#define GREY "\033[37m"
#define RST "\033[0m"

static void p(const char *s) { puts(s); }
static void pc(char c) { write(1, &c, 1); }

/* row buffer + builders */
/* Each stat is built into one fixed row (colour escapes baked in), then printed beside the logo */
#define MAX_ROWS 40
#define ROW_CAP 128
static char rows[MAX_ROWS][ROW_CAP];
static int nrows;
static char *wp, *wend; /* write cursor into the current row */

static void endrow(void)
{
  if (nrows < MAX_ROWS) {
    *wp = '\0';
    nrows++;
  }
}
static void ac(char c)
{
  if (wp < wend)
    *wp++ = c;
}
static void as(const char *s)
{
  while (*s && wp < wend)
    *wp++ = *s++;
}

/* Begin a row whose label is `lab` (green), leaving the cursor in grey ready for the value */
static void field(const char *lab)
{
  wp = rows[nrows < MAX_ROWS ? nrows : 0];
  wend = wp + ROW_CAP - 1;
  as(GREEN);
  as(lab);
  as(GREY);
}

static void au(unsigned v) /* unsigned decimal */
{
  char t[12];
  int n = 0;
  if (!v) {
    ac('0');
    return;
  }
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n)
    ac(t[--n]);
}
static void a2(unsigned v)
{
  ac('0' + (char)((v / 10) % 10));
  ac('0' + (char)(v % 10));
}

/* A size in MiB, switching to "N.N GiB" past a gibibyte */
static void amib(unsigned mib)
{
  if (mib >= 1024) {
    unsigned tenths = (mib * 10) / 1024;
    au(tenths / 10);
    ac('.');
    au(tenths % 10);
    as(" GiB");
  } else {
    au(mib);
    as(" MiB");
  }
}

static void asqueeze(const char *s)
{
  bool pending = false;
  bool started = false;
  for (; *s; s++) {
    if (*s == ' ' || *s == '\t') {
      if (started)
        pending = true;
      continue;
    }
    if (pending) {
      ac(' ');
      pending = false;
    }
    ac(*s);
    started = true;
  }
}

/* A clock in MHz rendered as "@ N.NN GHz", the shared CPU/GPU form */
static void a_ghz(unsigned mhz)
{
  unsigned cghz = (mhz + 5) / 10; /* hundredths of a GHz */
  as(" @ ");
  au(cghz / 100);
  ac('.');
  a2(cghz % 100);
  as(" GHz");
}

static bool has_at(const char *s)
{
  for (; *s; s++)
    if (*s == '@')
      return true;
  return false;
}

/* reading the /proc facts */
/* Each fact is its own /proc file, read through the ordinary file API, no bespoke syscall */

/* Read a whole /proc file into buf (NUL-terminated) */
static int readproc(const char *path, char *buf, int max)
{
  int n = readfile(path, buf, max - 1);
  if (n < 0)
    n = 0;
  buf[n] = '\0';
  return n;
}

static void rstrip(char *s)
{
  int n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
    s[--n] = '\0';
}

/* Value of a "key <value>" line; copies value (to EOL) into out */
static bool kvget(const char *buf, const char *key, char *out, int max)
{
  int kl = strlen(key);
  for (const char *p = buf; *p;) {
    bool match = true;
    for (int i = 0; i < kl; i++)
      if (p[i] != key[i]) {
        match = false;
        break;
      }
    if (match && p[kl] == ' ') {
      const char *v = p + kl + 1;
      int i = 0;
      while (v[i] && v[i] != '\n' && i < max - 1) {
        out[i] = v[i];
        i++;
      }
      out[i] = '\0';
      return true;
    }
    while (*p && *p != '\n')
      p++;
    if (*p == '\n')
      p++;
  }
  out[0] = '\0';
  return false;
}
static unsigned kvu(const char *buf, const char *key)
{
  char t[24];
  if (!kvget(buf, key, t, sizeof t))
    return 0;
  unsigned v = 0;
  for (const char *c = t; *c >= '0' && *c <= '9'; c++)
    v = v * 10 + (unsigned)(*c - '0');
  return v;
}
static unsigned u_of(const char *s)
{
  unsigned v = 0;
  while (*s == ' ')
    s++;
  for (; *s >= '0' && *s <= '9'; s++)
    v = v * 10 + (unsigned)(*s - '0');
  return v;
}

/* 1 if `s` begins with `pre`, case-insensitively */
static bool ci_prefix(const char *s, const char *pre)
{
  for (; *pre; s++, pre++) {
    int a = *s, b = *pre;
    if (a >= 'A' && a <= 'Z')
      a += 32;
    if (b >= 'A' && b <= 'Z')
      b += 32;
    if (a != b)
      return false;
  }
  return true;
}

/* the stats */
static void build_info(void)
{
  static char f[8192]; /* reused per /proc file */
  char v[80];

  /* Title line: "user@host" (classic neofetch), cyan like the old "OwOS" */
  {
    char title[160], un[32], hn[64];
    if (!user_ident(un, sizeof un, 0, 0))
      strcpy(un, "user");
    gethostname(hn, sizeof hn);
    strcpy(title, CYAN);
    strcat(title, un);
    strcat(title, "@");
    strcat(title, hn);
    strcat(title, GREY);
    field(title);
    endrow();
  }
  field("----------------");
  endrow();
  field("OS:      ");
  as("OwOS " OWOS_VERSION);
  endrow();
  {
    /* Kernel arch is authoritative via uname() */
    struct utsname u;
    field("Kernel:  ");
    if (uname(&u) == 0) {
      as("owos (");
      as(u.machine);
      as(strstr(u.machine, "64") ? ", 64-bit)" : ", 32-bit)"); /* x86_64, aarch64 */
    } else {
      as("owos");
    }
    endrow();
  }

  readproc("/proc/cpuinfo", f, sizeof f);
  field("CPU:     ");
  kvget(f, "model", v, sizeof v);
  if (v[0])
    asqueeze(v);
  else
    as("unknown");
  {
    unsigned mhz = kvu(f, "mhz");
    if (mhz && !has_at(v))
      a_ghz(mhz);
  }
  endrow();

  readproc("/proc/uptime", f, sizeof f);
  field("Uptime:  ");
  {
    unsigned s = u_of(f);
    unsigned h = s / 3600;
    s %= 3600;
    unsigned m = s / 60;
    s %= 60;
    if (h) {
      au(h);
      as("h ");
    }
    if (h || m) {
      au(m);
      as("m ");
    }
    au(s);
    ac('s');
  }
  endrow();

  readproc("/proc/date", f, sizeof f);
  rstrip(f);
  field("Date:    ");
  as(f);
  endrow();

  readproc("/proc/meminfo", f, sizeof f);
  field("Memory:  ");
  amib(kvu(f, "used") / 1024);
  as(" / ");
  amib(kvu(f, "total") / 1024);
  endrow();

  readproc("/proc/owofs", f, sizeof f);
  field("Disk:    ");
  au(kvu(f, "files"));
  as(" files, ");
  au(kvu(f, "bytes"));
  as(" bytes");
  endrow();

  /* /proc/pci: count the lines for the PCI total, and remember it for the GPU pass below */
  int pci_lines = 0;
  readproc("/proc/pci", f, sizeof f);
  for (const char *p = f; *p;) {
    pci_lines++;
    while (*p && *p != '\n')
      p++;
    if (*p == '\n')
      p++;
  }
  field("PCI:     ");
  au((unsigned)pci_lines);
  as(" devices");
  endrow();

  readproc("/proc/tasks", f, sizeof f);
  field("Tasks:   ");
  au(u_of(f));
  endrow();

  readproc("/sys/firmware/type", f, sizeof f);
  rstrip(f);
  field("Firmware:");
  ac(' ');
  as(f[0] ? f : "unknown");
  endrow();

  /* Host = vendor + product (brand + model), from /sys/class/dmi/id */
  readproc("/sys/class/dmi/id/product_name", f, sizeof f);
  rstrip(f);
  readproc("/sys/class/dmi/id/sys_vendor", v, sizeof v);
  rstrip(v);
  field("Host:    ");
  if (v[0] && f[0] && !ci_prefix(f, v)) {
    as(v);
    ac(' ');
    as(f);
  } else if (f[0])
    as(f);
  else if (v[0])
    as(v);
  else
    as("unknown");
  endrow();

  /* Battery: present when the sysfs node exists, pct from capacity, charge state from status */
  if (readproc("/sys/power/battery/capacity", f, sizeof f) > 0 && f[0]) {
    field("Battery: ");
    au(u_of(f));
    ac('%');
    readproc("/sys/power/battery/status", v, sizeof v);
    rstrip(v);
    if (streq(v, "Charging"))
      as(" (charging)");
    else if (streq(v, "Discharging")) {
      as(" (discharging");
      readproc("/sys/power/battery/minutes", v, sizeof v);
      unsigned mm = u_of(v);
      if (mm) {
        as(", ");
        au(mm / 60);
        as("h ");
        au(mm % 60);
        as("m left");
      }
      ac(')');
    }
    endrow();
  }

  /* /proc/storage: one "<type> <sectors> <model>" line per disk */
  readproc("/proc/storage", f, sizeof f);
  if (!f[0]) {
    field("Storage: ");
    as("none detected");
    endrow();
  } else {
    bool first = true;
    for (const char *p = f; *p;) {
      const char *eol = p;
      while (*eol && *eol != '\n')
        eol++;
      field(first ? "Storage: " : "         ");
      first = false;
      const char *q = p;
      while (q < eol && *q != ' ')
        ac(*q++); /* type */
      unsigned sectors = 0;
      if (q < eol) {
        q++;
        sectors = u_of(q);
        while (q < eol && *q != ' ')
          q++;
      }
      if (q < eol) {
        ac(' ');
        q++;
        while (q < eol)
          ac(*q++);
      } /* model */
      if (sectors) {
        as(" (");
        amib(sectors / 2048);
        ac(')');
      }
      endrow();
      p = (*eol == '\n') ? eol + 1 : eol;
    }
  }

  /* GPUs = /proc/pci lines whose class field (3rd token) starts "03" */
  readproc("/proc/pci", f, sizeof f);
  int gpus = 0;
  for (const char *p = f; *p;) {
    const char *eol = p;
    while (*eol && *eol != '\n')
      eol++;
    /* tokens: bdf vendor:device class name */
    const char *t = p;
    int tok = 0;
    const char *cls = 0, *name = 0;
    while (t < eol) {
      if (tok == 2)
        cls = t;
      if (tok == 3) {
        name = t;
        break;
      }
      while (t < eol && *t != ' ')
        t++;
      while (t < eol && *t == ' ')
        t++;
      tok++;
    }
    if (cls && cls[0] == '0' && cls[1] == '3') {
      field(gpus == 0 ? "GPU:     " : "         ");
      gpus++;
      for (const char *c = name; c && c < eol; c++)
        ac(*c);
      endrow();
    }
    p = (*eol == '\n') ? eol + 1 : eol;
  }
  if (!gpus) {
    field("GPU:     ");
    as("none detected");
    endrow();
  }

  /* The monitor: EDID brand/model + active resolution/refresh (or text mode) */
  field("Display: ");
  readproc("/sys/class/graphics/fb0/monitor", f, sizeof f);
  rstrip(f);
  readproc("/sys/class/graphics/fb0/internal", v, sizeof v);
  int internal = (v[0] == '1');
  if (f[0]) {
    as(f);
    if (internal)
      as(" (integrated)");
  } else {
    as(internal ? "Integrated panel" : "Generic display");
  }
  {
    readproc("/sys/class/graphics/fb0/width", v, sizeof v);
    unsigned w = u_of(v);
    readproc("/sys/class/graphics/fb0/height", v, sizeof v);
    unsigned h = u_of(v);
    readproc("/sys/class/graphics/fb0/refresh", v, sizeof v);
    unsigned hz = u_of(v);
    if (w && h) {
      as(" - ");
      au(w);
      ac('x');
      au(h);
      as(" @ ");
      au(hz);
      as("Hz");
    } else {
      as(" - 80x25 text");
    }
  }
  endrow();
}

/* Pair each stat row with its logo line (cyan, padded to a fixed column) */
static void render(void)
{
  int total = nrows > LOGO_LINES ? nrows : LOGO_LINES;
  for (int i = 0; i < total; i++) {
    p(CYAN);
    const char *l = (i < LOGO_LINES) ? logo[i] : "";
    int w = 0;
    for (const char *q = l; *q; q++) {
      pc(*q);
      w++;
    }
    for (; w < LOGO_W; w++)
      pc(' ');
    p(GREY);
    if (i < nrows)
      p(rows[i]);
    pc('\n');
  }
  p(RST);
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  build_info();
  render();
  return 0;
}
