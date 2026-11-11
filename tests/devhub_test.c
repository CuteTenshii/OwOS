/* devhub_test.c: Device Hub's parsers against the exact text the kernel writes to /proc and /sys
 * (user/apps/devhub/parse.c). Runs in the host suite: tests/run-tests.sh devhub */
#define OWOS_SUITE_TEST
#include "../user/apps/devhub/parse.h"
#include "test.h"
#include <string.h>

static void sizes(void)
{
  char s[24];
  dh_size(0, s, sizeof s);
  expect_string(s, "0 B");
  dh_size(1023, s, sizeof s);
  expect_string(s, "1023 B");
  dh_size(1536, s, sizeof s);
  expect_string(s, "1.5 KiB");
  dh_size(2097152ULL * 512, s, sizeof s); /* a 1 GiB disk in sectors */
  expect_string(s, "1.0 GiB");
  dh_size(20ULL << 30, s, sizeof s);
  expect_string(s, "20 GiB");
  dh_size(2047ULL << 20, s, sizeof s); /* 1.999 GiB rounds up, not down to 1.9 */
  expect_string(s, "2.0 GiB");
  dh_size(3ULL << 40, s, sizeof s);
  expect_string(s, "3.0 TiB");
  dh_size(1047552ULL * 1024, s, sizeof s); /* 1023 MiB of RAM shows as 1023, not bumped */
  expect_string(s, "1023 MiB");
  dh_size(1048064ULL * 1024, s, sizeof s); /* 1023.5 MiB would print as "1024 MiB" */
  expect_string(s, "1.0 GiB");
  dh_size(1048575ULL * 1024, s, sizeof s); /* the PC above: a KiB short of 1 GiB */
  expect_string(s, "1.0 GiB");
  char tiny[6];
  dh_size(20ULL << 30, tiny, sizeof tiny);
  expect_string(tiny, "20 Gi");
}

static void minutes(void)
{
  char s[24];
  dh_minutes(45, s, sizeof s);
  expect_string(s, "45 min");
  dh_minutes(125, s, sizeof s);
  expect_string(s, "2 h 05 min");
  dh_minutes(60, s, sizeof s);
  expect_string(s, "1 h 00 min");
}

static void fields_and_lines(void)
{
  const char *cpu = "model QEMU Virtual CPU\r\nmhz 2400\ncpus 4\n";
  char v[32];
  expect(dh_field(cpu, "model", v, sizeof v));
  expect_string(v, "QEMU Virtual CPU");
  expect(dh_field(cpu, "cpus", v, sizeof v));
  expect_string(v, "4");
  expect(!dh_field(cpu, "mh", v, sizeof v)); /* a key must match a whole word */
  expect(!dh_field(cpu, "total", v, sizeof v));
  expect_eq(dh_num(" 2400 MHz"), 2400);

  const char *p = "a\n\nb";
  char line[8];
  expect(dh_line(&p, line, sizeof line) && !strcmp(line, "a"));
  expect(dh_line(&p, line, sizeof line) && !strcmp(line, ""));
  expect(dh_line(&p, line, sizeof line) && !strcmp(line, "b"));
  expect(!dh_line(&p, line, sizeof line));

  char out[8] = "ab";
  dh_append(out, "cdefghij", sizeof out);
  expect_string(out, "abcdefg");

  char model[] = "  QEMU    QEMU\tHARDDISK  ";
  dh_tidy(model);
  expect_string(model, "QEMU QEMU HARDDISK");
  char blank[] = "   ";
  dh_tidy(blank);
  expect_string(blank, "");
}

static void pci(void)
{
  struct dh_pci p;
  expect(dh_parse_pci("00:02.0 1234:1111 0300 Bochs/QEMU VGA", &p));
  expect_string(p.addr, "00:02.0");
  expect_string(p.ids, "1234:1111");
  expect_eq(p.cls, 0x0300);
  expect_string(p.name, "Bochs/QEMU VGA");
  expect_string(dh_pci_class(p.cls), "Display controller");
  expect(dh_parse_pci("00:1f.2 8086:2922 0106 ", &p) && p.name[0] == 0);
  expect_string(dh_pci_class(0x0c03), "Serial bus controller");
  expect_string(dh_pci_class(0xff00), "Other device");
  expect(!dh_parse_pci("no PCI devices", &p));
  expect(!dh_parse_pci("00:02.0 1234:11 0300 x", &p));
  expect(!dh_parse_pci("00:02.0 1234:1111 03 x", &p));
}

static void disks(void)
{
  struct dh_disk d;
  expect(dh_parse_disk("NVMe 2097152 QEMU NVMe Ctrl", &d));
  expect_string(d.type, "NVMe");
  expect_eq(d.sectors, 2097152);
  expect_string(d.model, "QEMU NVMe Ctrl");
  expect(dh_parse_disk("SATA/AHCI 0 ", &d) && d.sectors == 0);
  expect(!dh_parse_disk("IDE/ATA model-without-size", &d));
}

static void bluetooth(void)
{
  struct dh_bt d;
  expect(dh_parse_bt("AA:BB:CC:DD:EE:FF br -61 gamepad Wireless Controller", &d));
  expect_string(d.addr, "AA:BB:CC:DD:EE:FF");
  expect(!d.le);
  expect_eq(d.rssi, -61);
  expect_string(d.kind, "gamepad");
  expect_string(d.name, "Wireless Controller");

  expect(dh_parse_bt("11:22:33:44:55:66 le 0 ", &d));
  expect(d.le && d.rssi == 0 && d.kind[0] == 0 && d.name[0] == 0);

  /* a name that merely starts like a kind keeps every word */
  expect(dh_parse_bt("11:22:33:44:55:66 br -40 Audio Speaker", &d));
  expect(d.kind[0] == 0);
  expect_string(d.name, "Audio Speaker");

  expect(!dh_parse_bt("state ready", &d));
  expect(!dh_parse_bt("11:22:33:44:55:66 xx -40 Thing", &d));
}

static void usb(void)
{
  struct dh_usb u;
  expect(dh_parse_usb("xhci: port 1: 0627:0001 class 00 480 Mbps [hid kbd]", &u));
  expect_eq(u.depth, 0);
  expect_string(u.port, "1");
  expect_string(u.ids, "0627:0001");
  expect_eq(u.cls, 0);
  expect_string(u.rest, "480 Mbps [hid kbd]");
  expect_string(dh_usb_class(u.cls), "Composite device");

  expect(dh_parse_usb("xhci:     port 3.1.4: 0781:5581 class 08 5 Gbps [msc]", &u));
  expect_eq(u.depth, 2);
  expect_string(u.port, "3.1.4");
  expect_string(dh_usb_class(u.cls), "Mass storage");

  expect(!dh_parse_usb("xhci: no usable device on any port", &u));
  expect(!dh_parse_usb("    port 1: no device", &u));
  expect(!dh_parse_usb("xhci: port 1: 0627:0001 klass 00", &u));

  struct dh_usbhc h;
  expect(dh_parse_usbhc("00:04.0  1b36:000d  xHCI (USB 3.x)", &h));
  expect_string(h.addr, "00:04.0");
  expect_string(h.ids, "1b36:000d");
  expect_string(h.type, "xHCI (USB 3.x)");
  expect(!dh_parse_usbhc("xhci: port 1: 0627:0001 class 00", &h));
}

int main(void)
{
  test("sizes", sizes);
  test("minutes", minutes);
  test("fields_and_lines", fields_and_lines);
  test("pci", pci);
  test("disks", disks);
  test("bluetooth", bluetooth);
  test("usb", usb);
  return 0;
}
