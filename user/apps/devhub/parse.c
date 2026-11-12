/* parse.c, text helpers that turn /proc and /sys output into Device Hub rows */
#include "parse.h"

void dh_copy(char *out, const char *s, int max)
{
  int i = 0;
  if (max <= 0)
    return;
  for (; s && s[i] && i < max - 1; i++)
    out[i] = s[i];
  out[i] = 0;
}

void dh_append(char *out, const char *s, int max)
{
  int n = 0;
  while (n < max && out[n])
    n++;
  if (n < max)
    dh_copy(out + n, s, max - n);
}

void dh_tidy(char *s)
{
  int r = 0, w = 0;
  while (s[r] == ' ' || s[r] == '\t')
    r++;
  for (; s[r]; r++) {
    bool space = s[r] == ' ' || s[r] == '\t';
    if (space && (w == 0 || s[w - 1] == ' '))
      continue;
    s[w++] = space ? ' ' : s[r];
  }
  if (w > 0 && s[w - 1] == ' ')
    w--;
  s[w] = 0;
}

bool dh_line(const char **pos, char *out, int max)
{
  const char *p = *pos;
  if (!p || !*p)
    return false;
  int n = 0;
  while (*p && *p != '\n') {
    if (*p != '\r' && n < max - 1)
      out[n++] = *p;
    p++;
  }
  out[n] = 0;
  if (*p == '\n')
    p++;
  *pos = p;
  return true;
}

bool dh_field(const char *text, const char *key, char *out, int max)
{
  char line[160];
  for (const char *p = text; dh_line(&p, line, sizeof line);) {
    int i = 0;
    while (key[i] && line[i] == key[i])
      i++;
    if (!key[i] && line[i] == ' ') {
      dh_copy(out, line + i + 1, max);
      return true;
    }
  }
  return false;
}

unsigned long long dh_num(const char *s)
{
  unsigned long long v = 0;
  while (*s == ' ')
    s++;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (unsigned)(*s++ - '0');
  return v;
}

static void put_u64(unsigned long long v, char *out, int max)
{
  char t[24];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  int i = 0;
  while (n && i < max - 1)
    out[i++] = t[--n];
  out[i] = 0;
}

void dh_size(unsigned long long bytes, char *out, int max)
{
  static const char *const unit[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  int u = 0;
  unsigned long long scale = 1;
  while (u < 5 && bytes / scale >= 1024) {
    scale *= 1024;
    u++;
  }
  char num[24];
  /* Tenths rounded to nearest, so 1.96 GiB shows as 2.0, not 1.9 */
  unsigned long long tenths = (bytes * 10 + scale / 2) / scale;
  if (u > 0 && u < 5 && tenths >= 10235) { /* rounds to 1024 of this unit: say 1.0 of the next */
    scale *= 1024;
    u++;
    tenths = (bytes * 10 + scale / 2) / scale;
  }
  if (u == 0 || tenths >= 100) {
    put_u64((tenths + 5) / 10, num, sizeof num);
  } else {
    put_u64(tenths / 10, num, sizeof num);
    dh_append(num, ".", sizeof num);
    char d[2] = {(char)('0' + tenths % 10), 0};
    dh_append(num, d, sizeof num);
  }
  dh_copy(out, num, max);
  dh_append(out, " ", max);
  dh_append(out, unit[u], max);
}

void dh_minutes(unsigned minutes, char *out, int max)
{
  char t[12];
  out[0] = 0;
  if (minutes >= 60) {
    put_u64(minutes / 60, t, sizeof t);
    dh_append(out, t, max);
    dh_append(out, " h ", max);
    if (minutes % 60 < 10)
      dh_append(out, "0", max);
  }
  put_u64(minutes % 60, t, sizeof t);
  dh_append(out, t, max);
  dh_append(out, " min", max);
}

/* Copy the next space-delimited word at *p into out and skip the spaces after it. */
static bool word(const char **p, char *out, int max)
{
  const char *s = *p;
  while (*s == ' ')
    s++;
  int n = 0;
  while (*s && *s != ' ') {
    if (n < max - 1)
      out[n++] = *s;
    s++;
  }
  out[n] = 0;
  while (*s == ' ')
    s++;
  *p = s;
  return n > 0;
}

static int hexval(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* A word of exactly `digits` hex digits into *v. */
static bool hexword(const char *s, int digits, unsigned *v)
{
  unsigned acc = 0;
  for (int i = 0; i < digits; i++) {
    int h = hexval(s[i]);
    if (h < 0)
      return false;
    acc = acc << 4 | (unsigned)h;
  }
  if (s[digits])
    return false;
  *v = acc;
  return true;
}

/* "vvvv:dddd" with four hex digits on each side. */
static bool idpair(const char *s)
{
  unsigned v;
  char half[5];
  for (int i = 0; i < 4; i++) {
    if (!s[i])
      return false;
    half[i] = s[i];
  }
  half[4] = 0;
  if (!hexword(half, 4, &v) || s[4] != ':')
    return false;
  return hexword(s + 5, 4, &v);
}

bool dh_parse_pci(const char *line, struct dh_pci *p)
{
  const char *s = line;
  char cls[8];
  if (!word(&s, p->addr, sizeof p->addr) || !word(&s, p->ids, sizeof p->ids) || !word(&s, cls, sizeof cls))
    return false;
  if (p->addr[2] != ':' || p->addr[5] != '.' || !idpair(p->ids) || !hexword(cls, 4, &p->cls))
    return false;
  dh_copy(p->name, s, sizeof p->name);
  return true;
}

const char *dh_pci_class(unsigned cls)
{
  static const char *const names[] = {
      "Unclassified device",
      "Mass storage controller",
      "Network controller",
      "Display controller",
      "Multimedia controller",
      "Memory controller",
      "Bridge",
      "Communication controller",
      "System peripheral",
      "Input device controller",
      "Docking station",
      "Processor",
      "Serial bus controller",
      "Wireless controller",
      "Intelligent controller",
      "Satellite controller",
      "Encryption controller",
      "Signal processing controller",
      "Processing accelerator",
      "Instrumentation",
  };
  unsigned base = cls >> 8;
  if (base < sizeof names / sizeof names[0])
    return names[base];
  if (base == 0x40)
    return "Co-processor";
  return "Other device";
}

bool dh_parse_disk(const char *line, struct dh_disk *d)
{
  const char *s = line;
  char num[24];
  if (!word(&s, d->type, sizeof d->type) || !word(&s, num, sizeof num) || num[0] < '0' || num[0] > '9')
    return false;
  d->sectors = dh_num(num);
  dh_copy(d->model, s, sizeof d->model);
  return true;
}

static bool starts(const char *s, const char *prefix)
{
  while (*prefix)
    if (*s++ != *prefix++)
      return false;
  return true;
}

bool dh_parse_bt(const char *line, struct dh_bt *d)
{
  const char *s = line;
  char tr[4], rssi[8];
  if (!word(&s, d->addr, sizeof d->addr) || !word(&s, tr, sizeof tr) || !word(&s, rssi, sizeof rssi))
    return false;
  if (d->addr[2] != ':' || (!starts(tr, "br") && !starts(tr, "le")))
    return false;
  d->le = tr[0] == 'l';
  const char *r = rssi;
  int neg = *r == '-';
  if (neg)
    r++;
  if (*r < '0' || *r > '9')
    return false;
  d->rssi = (int)dh_num(r) * (neg ? -1 : 1);

  static const char *const kinds[] = {"audio ", "gamepad ", "input "};
  d->kind[0] = 0;
  for (int i = 0; i < 3; i++)
    if (starts(s, kinds[i])) {
      int n = 0;
      while (kinds[i][n] != ' ')
        n++;
      dh_copy(d->kind, kinds[i], n + 1);
      s += n + 1;
      break;
    }
  dh_copy(d->name, s, sizeof d->name);
  return true;
}

bool dh_parse_usb(const char *line, struct dh_usb *u)
{
  if (!starts(line, "xhci: ") && !starts(line, "ehci: ") && !starts(line, "uhci: "))
    return false;
  const char *s = line + 6;
  int sp = 0;
  while (s[sp] == ' ')
    sp++;
  if (!starts(s + sp, "port "))
    return false;
  u->depth = sp / 2;
  s += sp + 5;

  int n = 0;
  while (*s && *s != ':' && *s != ' ') {
    if (n < (int)sizeof u->port - 1)
      u->port[n++] = *s;
    s++;
  }
  u->port[n] = 0;
  if (*s != ':' || !n)
    return false;
  s++;

  char cls[4], kw[8];
  if (!word(&s, u->ids, sizeof u->ids) || !idpair(u->ids) || !word(&s, kw, sizeof kw) || !starts(kw, "class") ||
      !word(&s, cls, sizeof cls) || !hexword(cls, 2, &u->cls))
    return false;
  dh_copy(u->rest, s, sizeof u->rest);
  return true;
}

const char *dh_usb_class(unsigned cls)
{
  switch (cls) {
  case 0x00:
    return "Composite device";
  case 0x01:
    return "Audio";
  case 0x02:
    return "Communications";
  case 0x03:
    return "Human interface device";
  case 0x05:
    return "Physical interface";
  case 0x06:
    return "Imaging";
  case 0x07:
    return "Printer";
  case 0x08:
    return "Mass storage";
  case 0x09:
    return "Hub";
  case 0x0a:
    return "CDC data";
  case 0x0b:
    return "Smart card";
  case 0x0e:
    return "Video";
  case 0x0f:
    return "Personal healthcare";
  case 0x10:
    return "Audio/video";
  case 0xdc:
    return "Diagnostic device";
  case 0xe0:
    return "Wireless controller";
  case 0xef:
    return "Miscellaneous";
  case 0xfe:
    return "Application specific";
  case 0xff:
    return "Vendor specific";
  default:
    return "Unknown class";
  }
}

bool dh_parse_usbhc(const char *line, struct dh_usbhc *h)
{
  const char *s = line;
  if (!word(&s, h->addr, sizeof h->addr) || !word(&s, h->ids, sizeof h->ids))
    return false;
  if (h->addr[2] != ':' || h->addr[5] != '.' || !idpair(h->ids))
    return false;
  dh_copy(h->type, s, sizeof h->type);
  return true;
}
