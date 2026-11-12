/* usb.c - the `lsusb` command.
 *
 * Like lspci, this finds USB host controllers on the PCI bus (class 0x0C,
 * subclass 0x03) and names them by prog_if. Under each xHCI or UHCI controller
 * it lists the devices that controller's driver enumerated, with the hubs they
 * hang off; an empty UHCI root port gets a "no device" line.
 */

#include "usb.h"
#include "console.h"
#include "kstring.h"
#include "pci.h"
#include "uhci.h"
#include "usbhost.h"
#include <stdbool.h>
#include <stdint.h>

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB 0x03

/* prog_if values that distinguish the four USB host controller generations. */
#define USB_PROGIF_UHCI 0x00
#define USB_PROGIF_OHCI 0x10
#define USB_PROGIF_EHCI 0x20
#define USB_PROGIF_XHCI 0x30

static const char *controller_type(uint8_t prog_if)
{
  switch (prog_if) {
  case USB_PROGIF_UHCI:
    return "UHCI (USB 1.1)";
  case USB_PROGIF_OHCI:
    return "OHCI (USB 1.1)";
  case USB_PROGIF_EHCI:
    return "EHCI (USB 2.0)";
  case USB_PROGIF_XHCI:
    return "xHCI (USB 3.x)";
  default:
    return "USB controller";
  }
}

/* Walk a configuration descriptor blob and print its interfaces + endpoints, so
 * lsusb shows what each device actually exposes (class/subclass/protocol and
 * the bulk/interrupt endpoints a driver would use). */
static void print_config(const uint8_t *cfg, int total)
{
  for (int i = 0; i + 2 <= total;) {
    uint8_t blen = cfg[i];
    if (blen < 2)
      break;
    uint8_t btype = cfg[i + 1];
    if (btype == 0x04 && i + 9 <= total) { /* interface         */
      con_printf("         if %u class %02x/%02x/%02x\n", cfg[i + 2], cfg[i + 5], cfg[i + 6], cfg[i + 7]);
    } else if (btype == 0x05 && i + 7 <= total) { /* endpoint          */
      uint8_t addr = cfg[i + 2];
      uint8_t attr = cfg[i + 3] & 0x3;
      const char *t = attr == 2 ? "bulk" : attr == 3 ? "intr" : attr == 1 ? "isoc" : "ctrl";
      con_printf("           ep %02x%s%s\n", addr, (addr & 0x80) ? " IN  " : " OUT ", t);
    }
    i += blen;
  }
}

/* One device line (and its downstream devices, indented), with interface detail when its root port is selected. */
static void print_device(const char *hc, int slot, const char *path, int depth, int detail_port)
{
  struct usbdev *d = usbh_dev(slot);
  if (!d || !d->active)
    return;
  con_printf("%s: ", hc);
  for (int i = 0; i < depth; i++)
    con_write("  ");
  char rate[16];
  con_printf("port %s: %04x:%04x class %02x %s", path, d->vid, d->pid, d->cls,
             usb3_rate_str(d->kbps, rate, sizeof rate));
  uint32_t capable = usb3_bos_max_kbps(&d->bos);
  if (capable > d->kbps)
    con_printf(" (%s capable)", usb3_rate_str(capable, rate, sizeof rate));
  if (d->mac_ok && (d->bound & USBH_BOUND_NET))
    con_write(" [net]");
  if (d->bound & USBH_BOUND_MTP)
    con_write(" [mtp]");
  if (d->bound & USBH_BOUND_MSC)
    con_write(" [msc]");
  hub_print(slot);
  btusb_print(slot);
  uvc_print(slot);
  hid_print(slot, false);
  if (d->product[0])
    con_printf(" \"%s\"", d->product);
  con_putc('\n');
  if (detail_port == d->root_port) {
    if (d->cfg_len)
      print_config(d->cfg, d->cfg_len);
    hid_print(slot, true);
    if (d->is_net && d->mac_ok) {
      con_write("         rndis mac ");
      for (int i = 0; i < 6; i++) {
        con_put_hex(d->mac[i], 2);
        if (i < 5)
          con_putc(':');
      }
      con_putc('\n');
    } else if (d->is_net) {
      con_write("         rndis: init failed\n");
    }
  }
  for (int s = 1; s <= usbh_max_slot(); s++) {
    struct usbdev *c = usbh_dev(s);
    if (c && c->parent_slot == slot) {
      char child[24];
      ksnprintf(child, sizeof child, "%s.%u", path, c->parent_port);
      print_device(hc, s, child, depth + 1, detail_port);
    }
  }
}

/* The devices with ids first..last on root port `port`, with their hub topology below them; false if there are none. */
static bool list_port(const char *hc, int first, int last, int port, int detail_port)
{
  bool any = false;
  int max = usbh_max_slot();
  for (int s = first; s <= last && s <= max; s++) {
    struct usbdev *d = usbh_dev(s);
    if (d && d->active && d->parent_slot == 0 && d->root_port == port) {
      char path[8];
      ksnprintf(path, sizeof path, "%u", port);
      print_device(hc, s, path, 0, detail_port);
      any = true;
    }
  }
  return any;
}

static void xhci_list(int detail_port)
{
  bool any = false;
  for (int p = 1; p < 256; p++)
    any |= list_port("xhci", 1, 255, p, detail_port);
  if (!any)
    con_write("xhci: no usable device on any port\n");
}

static void uhci_list(uint8_t bus, uint8_t slot, uint8_t func, int detail_port)
{
  int first, last, nports;
  if (!uhci_controller(bus, slot, func, &first, &last, &nports)) {
    con_write("    (not driven)\n");
    return;
  }
  for (int p = 1; p <= nports; p++)
    if (!list_port("uhci", first, last, p, detail_port))
      con_printf("    port %d: no device\n", p);
}

void usb_list(int detail_port)
{
  int found = 0;

  for (int bus = 0; bus < 256; bus++) {
    for (int slot = 0; slot < 32; slot++) {
      if ((pci_read32(bus, slot, 0, 0x00) & 0xFFFF) == 0xFFFF)
        continue;
      uint8_t header = (pci_read32(bus, slot, 0, 0x0C) >> 16) & 0xFF;
      int funcs = (header & 0x80) ? 8 : 1;
      for (int f = 0; f < funcs; f++) {
        uint32_t id = pci_read32(bus, slot, f, 0x00);
        if ((id & 0xFFFF) == 0xFFFF)
          continue;
        uint32_t cls = pci_read32(bus, slot, f, 0x08);
        uint8_t class_code = (cls >> 24) & 0xFF;
        uint8_t subclass = (cls >> 16) & 0xFF;
        uint8_t prog_if = (cls >> 8) & 0xFF;
        if (class_code != PCI_CLASS_SERIAL_BUS || subclass != PCI_SUBCLASS_USB)
          continue;

        found++;
        con_printf("%02x:%02x.%01x  %04x:%04x  %s\n", bus, slot, f, id & 0xFFFF, (id >> 16) & 0xFFFF,
                   controller_type(prog_if));

        if (prog_if == USB_PROGIF_UHCI)
          uhci_list((uint8_t)bus, (uint8_t)slot, (uint8_t)f, detail_port);
        else if (prog_if == USB_PROGIF_XHCI)
          if (xhci_present())
            xhci_list(detail_port);
      }
    }
  }

  if (!found)
    con_write("no USB controllers found\n");
}
