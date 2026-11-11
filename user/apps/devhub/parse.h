/* parse.h, text helpers that turn /proc and /sys output into Device Hub rows.
 * Pure string code with no system calls, so the host test links it as is. */
#ifndef DEVHUB_PARSE_H
#define DEVHUB_PARSE_H

#include <stdbool.h>

/* Copy s into out (max bytes, always NUL-terminated) / append s to it. */
void dh_copy(char *out, const char *s, int max);
void dh_append(char *out, const char *s, int max);

/* Trim s and collapse runs of spaces or tabs to one space, in place (drives pad their model strings). */
void dh_tidy(char *s);

/* Copy the next line at *pos into out without its "\n" or "\r", then advance *pos past it.
 * False once *pos is at the end of the text. */
bool dh_line(const char **pos, char *out, int max);

/* The value after "<key> " on the line that starts with it ("model QEMU CPU" -> "QEMU CPU"). */
bool dh_field(const char *text, const char *key, char *out, int max);

/* Leading decimal digits of s, after any spaces (0 when there are none). */
unsigned long long dh_num(const char *s);

/* Byte count in binary units: "512 B", "1.5 KiB", "20 GiB" (one decimal below 10). */
void dh_size(unsigned long long bytes, char *out, int max);

/* A runtime in minutes: "45 min", "2 h 05 min". */
void dh_minutes(unsigned minutes, char *out, int max);

/* A /proc/pci line: "00:02.0 1234:1111 0300 Display controller". */
struct dh_pci {
  char addr[8];  /* "00:02.0" */
  char ids[10];  /* "1234:1111" */
  unsigned cls;  /* class << 8 | subclass */
  char name[64]; /* the kernel's description */
};
bool dh_parse_pci(const char *line, struct dh_pci *p);
/* Generic name of a PCI base class (cls >> 8), e.g. "Network controller". */
const char *dh_pci_class(unsigned cls);

/* A /proc/storage line: "NVMe 2097152 QEMU NVMe Ctrl". */
struct dh_disk {
  char type[16];
  unsigned long long sectors; /* 512-byte sectors */
  char model[48];
};
bool dh_parse_disk(const char *line, struct dh_disk *d);

/* A /sys/class/bluetooth/hci0/devices line: "<addr> br|le <rssi> [audio |gamepad |input ]<name>". */
struct dh_bt {
  char addr[18];
  bool le;
  int rssi;
  char kind[8]; /* "audio", "gamepad", "input" or "" */
  char name[48];
};
bool dh_parse_bt(const char *line, struct dh_bt *d);

/* A /proc/usb xHCI device line: "xhci: <2 spaces per hub level>port 1.2: 046d:c077 class 00 12 Mbps [msc] ...". */
struct dh_usb {
  int depth;     /* hub nesting, 0 for a device on a root port */
  char port[16]; /* "1", "1.2" */
  char ids[10];  /* "046d:c077" */
  unsigned cls;  /* device class byte */
  char rest[96]; /* link rate and whatever the drivers append */
};
bool dh_parse_usb(const char *line, struct dh_usb *u);
/* Name of a USB device class byte; class 00 means "defined per interface". */
const char *dh_usb_class(unsigned cls);

/* A /proc/usb host-controller line: "00:04.0  1b36:000d  xHCI (USB 3.x)". */
struct dh_usbhc {
  char addr[8];
  char ids[10];
  char type[32];
};
bool dh_parse_usbhc(const char *line, struct dh_usbhc *h);

#endif /* DEVHUB_PARSE_H */
