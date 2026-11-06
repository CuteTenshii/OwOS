/* battery.c: ACPI battery detection (PNP0C0A _HID scan) and cached charge reads for neofetch. */

#include "battery.h"
#include "aml.h"
#include "power.h"
#include "timer.h"

int battery_present(void)
{
  /* Fixed for the session; the lookup scans every ACPI table, so cache it. */
  static int has_device = -1;
  if (has_device < 0) {
    /* EisaId("PNP0C0A") compressed into its four AML bytes. */
    static const unsigned char eisa[4] = {0x41, 0xD0, 0x0C, 0x0A};
    /* Fallback: a few tables store the _HID as a literal ASCII string. */
    static const unsigned char str[7] = {'P', 'N', 'P', '0', 'C', '0', 'A'};
    has_device = acpi_aml_contains(eisa, 4) || acpi_aml_contains(str, 7);
  }
  if (!has_device)
    return 0;

  /* Trust a real _BST/_BIF reading; consult _STA only on 0% to weed out phantoms (e.g. VMware). */
  if (battery_percent(0, 0, 0) > 0)
    return 1;
  return aml_battery_present();
}

int battery_percent(int *charging, int *discharging, int *minutes)
{
  /* _BST/_BIF drive the EC (milliseconds): cache and refresh every few seconds. */
  static bool valid = false;
  static int pct = -1, mins = -1;
  static bool chg = false, dis = false;
  static uint32_t last_t;
  static bool have = false;

  uint32_t now = timer_seconds();
  if (!have || (now - last_t) >= 5) {
    struct aml_battery b;
    valid = aml_read_battery(&b);
    if (valid) {
      pct = b.percent;
      chg = b.charging;
      dis = b.discharging;
      mins = b.minutes;
    } else {
      pct = -1;
      chg = dis = 0;
      mins = -1;
    }
    last_t = now;
    have = true;
  }

  if (charging)
    *charging = chg;
  if (discharging)
    *discharging = dis;
  if (minutes)
    *minutes = mins;
  return valid ? pct : -1;
}

const char *battery_error(void) { return aml_last_error(); }

/* One-line detection status for debugging real laptops; read via /proc/battery. */
const char *battery_diag(void)
{
  static char buf[320];
  int i = 0;

  /* Stage 1: hid=0 means the firmware exposes no PNP0C0A at all. */
  static const unsigned char eisa[4] = {0x41, 0xD0, 0x0C, 0x0A};
  static const unsigned char str[7] = {'P', 'N', 'P', '0', 'C', '0', 'A'};
  int hid = acpi_aml_contains(eisa, 4) || acpi_aml_contains(str, 7);

  const char *h = hid ? "hid=1 " : "hid=0 ";
  for (int k = 0; h[k] && i < (int)sizeof buf - 1; k++)
    buf[i++] = h[k];

  /* Stage 2: namespace-walk stats. */
  int nnodes = 0, bail = -1;
  aml_ns_stats(&nnodes, &bail);
  buf[i++] = 'n';
  buf[i++] = '=';
  {
    char t[12];
    int m = 0, v = nnodes;
    if (!v)
      t[m++] = '0';
    while (v) {
      t[m++] = (char)('0' + v % 10);
      v /= 10;
    }
    while (m && i < (int)sizeof buf - 1)
      buf[i++] = t[--m];
  }
  buf[i++] = ' ';
  buf[i++] = 'b';
  buf[i++] = '=';
  if (bail < 0) {
    buf[i++] = '-';
  } else {
    static const char hx[] = "0123456789ABCDEF";
    buf[i++] = '0';
    buf[i++] = 'x';
    for (int sh = 12; sh >= 0; sh -= 4)
      buf[i++] = hx[(bail >> sh) & 0xF];
  }
  buf[i++] = ' ';

  /* Stage 3: report the percentage or the failure note. */
  struct aml_battery b;
  if (aml_read_battery(&b)) {
    const char *p = "pct=";
    for (int k = 0; p[k]; k++)
      buf[i++] = p[k];
    int v = b.percent;
    char t[4];
    int m = 0;
    if (!v)
      t[m++] = '0';
    while (v) {
      t[m++] = (char)('0' + v % 10);
      v /= 10;
    }
    while (m)
      buf[i++] = t[--m];
  } else {
    const char *e = "err=";
    for (int k = 0; e[k]; k++)
      buf[i++] = e[k];
    const char *m = aml_last_error();
    for (int k = 0; m[k] && i < (int)sizeof buf - 1; k++)
      buf[i++] = m[k];
  }

  /* Distinguishes an "EC not available" sentinel (all _BST fields 0xFFFFFFFF) from one bad field. */
  if (hid && i < (int)sizeof buf - 2) {
    buf[i++] = ' ';
    aml_battery_debug(buf + i, (int)sizeof buf - i);
    while (buf[i])
      i++;
  }
  buf[i] = 0;
  return buf;
}
