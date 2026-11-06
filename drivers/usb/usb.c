/* usb.c: the `lsusb` command.
 * Lists USB host controllers on PCI by prog_if; for UHCI also reads root-hub ports and fetches device descriptors over
 * endpoint 0. Transfers use a minimal UHCI schedule (frame list, one queue head, TD chain) in palloc'd memory (phys ==
 * virt). */

#include "usb.h"
#include "console.h"
#include "io.h"
#include "paging.h"
#include "pci.h"
#include "timer.h"
#include "xhci.h"
#include <stdbool.h>
#include <stdint.h>

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB 0x03

/* prog_if values that distinguish the four USB host controller generations. */
#define USB_PROGIF_UHCI 0x00
#define USB_PROGIF_OHCI 0x10
#define USB_PROGIF_EHCI 0x20
#define USB_PROGIF_XHCI 0x30

/* UHCI I/O register offsets from the controller's BAR4 base. */
#define UHCI_USBCMD 0x00
#define UHCI_USBSTS 0x02
#define UHCI_USBINTR 0x04
#define UHCI_FRNUM 0x06
#define UHCI_FRBASEADD 0x08
#define UHCI_SOFMOD 0x0C
#define UHCI_PORTSC1 0x10
#define UHCI_PORTSC2 0x12

/* USBCMD bits. */
#define CMD_RUN 0x0001
#define CMD_HCRESET 0x0002
#define CMD_CF 0x0040
#define CMD_MAXP 0x0080

/* UHCI PORTSC bits. */
#define UHCI_PORT_CONNECTED 0x0001
#define UHCI_PORT_CSC 0x0002
#define UHCI_PORT_ENABLED 0x0004
#define UHCI_PORT_PEDC 0x0008
#define UHCI_PORT_LOWSPEED 0x0100
#define UHCI_PORT_RESET 0x0200

/* Transfer-descriptor control/status bits. */
#define TD_ACTIVE (1u << 23)
#define TD_LOWSPEED (1u << 26)
#define TD_CERR3 (3u << 27)
/* Fatal status bits (NAK is not fatal, the controller retries it). */
#define TD_ERRMASK ((1u << 22) | (1u << 21) | (1u << 20) | (1u << 18) | (1u << 17))

/* Link-pointer bits. */
#define LINK_TERMINATE 0x1
#define LINK_QH 0x2
#define LINK_DEPTH 0x4

/* Control-transfer token PIDs. */
#define PID_SETUP 0x2D
#define PID_IN 0x69
#define PID_OUT 0xE1

#define MAX_TDS 20

/* UHCI TD: 4 hardware dwords, padded to a 32-byte stride (hardware needs 16-byte alignment). */
struct uhci_td {
  volatile uint32_t link;
  volatile uint32_t status;
  volatile uint32_t token;
  volatile uint32_t buffer;
  uint32_t sw[4];
};

/* UHCI queue head */
struct uhci_qh {
  volatile uint32_t head;
  volatile uint32_t element;
  uint32_t sw[2];
};

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

/* Build a TD token: PID, address, endpoint, toggle, max length as len-1 (0x7FF = zero-length). */
static uint32_t td_token(uint8_t pid, uint8_t addr, uint8_t ep, bool toggle, int len)
{
  uint32_t maxlen = (len == 0) ? 0x7FF : ((uint32_t)(len - 1) & 0x7FF);
  return (uint32_t)pid | ((uint32_t)addr << 8) | ((uint32_t)ep << 15) | ((uint32_t)toggle << 19) | (maxlen << 21);
}

/* Put the controller in a known idle state (stopped, no interrupts, status cleared); no HCRESET. */
static void uhci_prepare(uint16_t io)
{
  outw(io + UHCI_USBCMD, 0);
  outw(io + UHCI_USBINTR, 0);
  outw(io + UHCI_USBSTS, 0x3F);
}

/* Reset and enable a root-hub port; returns true and sets *lowspeed if a device is enabled. CSC/PEDC are
 * write-1-to-clear, so write 0 there. */
static bool uhci_reset_port(uint16_t port, bool *lowspeed)
{
  uint16_t s = inw(port);
  if (!(s & UHCI_PORT_CONNECTED))
    return false;

  /* Drive reset for >= 50 ms, then release it. */
  outw(port, (s & ~(UHCI_PORT_CSC | UHCI_PORT_PEDC)) | UHCI_PORT_RESET);
  timer_sleep(60);
  s = inw(port);
  outw(port, s & ~(UHCI_PORT_CSC | UHCI_PORT_PEDC | UHCI_PORT_RESET));

  /* Brief recovery before the port can be enabled. */
  for (volatile int i = 0; i < 50000; i++)
    io_wait();

  for (int tries = 0; tries < 20; tries++) {
    s = inw(port);
    if (!(s & UHCI_PORT_CONNECTED))
      return false;
    if (s & UHCI_PORT_ENABLED)
      break;
    /* Set enable and clear any pending change bits. */
    outw(port, (s & ~UHCI_PORT_RESET) | UHCI_PORT_ENABLED | UHCI_PORT_CSC | UHCI_PORT_PEDC);
    timer_sleep(10);
  }

  s = inw(port);
  if (!(s & UHCI_PORT_ENABLED))
    return false;
  *lowspeed = s & UHCI_PORT_LOWSPEED;
  return true;
}

/* GET-style control transfer (data IN) on endpoint 0 of `addr`; reads exactly `length` bytes in maxpacket chunks.
 * Returns bytes read or -1. */
static int uhci_control_in(uint16_t io, bool lowspeed, uint8_t addr, uint8_t maxpacket, uint8_t reqtype, uint8_t req,
                           uint16_t wValue, uint16_t wIndex, uint8_t *out, uint16_t length)
{
  uint8_t *fl = palloc();
  uint8_t *page = palloc();
  if (!fl || !page) {
    if (fl)
      pfree(fl);
    if (page)
      pfree(page);
    return -1;
  }

  struct uhci_qh *qh = (struct uhci_qh *)page;
  struct uhci_td *td = (struct uhci_td *)(page + 64);
  uint8_t *setup = page + 0x400;
  uint8_t *data = page + 0x800;

  /* The 8-byte SETUP packet. */
  setup[0] = reqtype;
  setup[1] = req;
  setup[2] = wValue & 0xFF;
  setup[3] = wValue >> 8;
  setup[4] = wIndex & 0xFF;
  setup[5] = wIndex >> 8;
  setup[6] = length & 0xFF;
  setup[7] = length >> 8;

  uint32_t base = TD_ACTIVE | TD_CERR3 | (lowspeed ? TD_LOWSPEED : 0);
  int n = 0;

  /* SETUP stage: always DATA0, 8 bytes. */
  td[n].status = base;
  td[n].token = td_token(PID_SETUP, addr, 0, false, 8);
  td[n].buffer = (uint32_t)(uintptr_t)setup;
  n++;

  /* DATA IN stage: alternate the toggle, starting at DATA1. */
  bool toggle = true;
  int off = 0, remaining = length;
  while (remaining > 0 && n < MAX_TDS - 1) {
    int chunk = remaining < maxpacket ? remaining : maxpacket;
    td[n].status = base;
    td[n].token = td_token(PID_IN, addr, 0, toggle, chunk);
    td[n].buffer = (uint32_t)(uintptr_t)(data + off);
    toggle = !toggle;
    off += chunk;
    remaining -= chunk;
    n++;
  }

  /* STATUS stage: zero-length OUT, always DATA1. */
  td[n].status = base;
  td[n].token = td_token(PID_OUT, addr, 0, true, 0);
  td[n].buffer = 0;
  n++;

  /* Chain the TDs depth-first; the last one terminates the queue. */
  for (int i = 0; i < n; i++)
    td[i].link = (i + 1 < n) ? ((uint32_t)(uintptr_t)&td[i + 1] | LINK_DEPTH) : LINK_TERMINATE;

  qh->head = LINK_TERMINATE;
  qh->element = (uint32_t)(uintptr_t)&td[0];

  /* Point every frame at our queue head so it runs regardless of FRNUM. */
  uint32_t *frames = (uint32_t *)fl;
  for (int i = 0; i < 1024; i++)
    frames[i] = (uint32_t)(uintptr_t)qh | LINK_QH;

  /* Arm and start the schedule. */
  outw(io + UHCI_FRNUM, 0);
  outl(io + UHCI_FRBASEADD, (uint32_t)(uintptr_t)fl);
  outb(io + UHCI_SOFMOD, 0x40);
  outw(io + UHCI_USBSTS, 0x3F);
  outw(io + UHCI_USBCMD, CMD_RUN | CMD_CF | CMD_MAXP);

  /* Poll until the queue drains or a TD reports a fatal error. */
  int result = -1;
  for (int t = 0; t < 200; t++) {
    bool active = false, err = false;
    for (int i = 0; i < n; i++) {
      uint32_t st = td[i].status;
      if (st & TD_ACTIVE)
        active = true;
      if (st & TD_ERRMASK)
        err = true;
    }
    if (err)
      break;
    if ((qh->element & LINK_TERMINATE) || !active) {
      result = 0;
      break;
    }
    timer_sleep(1);
  }

  if (result == 0) {
    int got = 0;
    for (int i = 1; i < n - 1; i++)
      got += ((td[i].status & 0x7FF) + 1) & 0x7FF;
    if (got > length)
      got = length;
    for (int i = 0; i < got; i++)
      out[i] = data[i];
    result = got;
  }

  outw(io + UHCI_USBCMD, 0);
  pfree(fl);
  pfree(page);
  return result;
}

/* Fetch string descriptor `index` (English) and print its ASCII subset: a 2-byte read gives bLength, then the full
 * read. */
static void uhci_print_string(uint16_t io, bool lowspeed, uint8_t maxpacket, uint8_t index)
{
  if (!index)
    return;
  uint8_t hdr[2];
  if (uhci_control_in(io, lowspeed, 0, maxpacket, 0x80, 0x06, (0x03 << 8) | index, 0x0409, hdr, 2) < 2)
    return;
  int len = hdr[0];
  if (len < 4)
    return;
  if (len > 64)
    len = 64;
  uint8_t buf[64];
  if (uhci_control_in(io, lowspeed, 0, maxpacket, 0x80, 0x06, (0x03 << 8) | index, 0x0409, buf, len) < len)
    return;
  con_write(" \"");
  for (int i = 2; i + 1 < len; i += 2) {
    uint8_t lo = buf[i], hi = buf[i + 1];
    con_putc((hi == 0 && lo >= 32 && lo < 127) ? (char)lo : '?');
  }
  con_putc('"');
}

/* Enumerate the device on a freshly reset port and print vendor:product, class and product name. */
static void uhci_enumerate(uint16_t io, uint16_t portsc_off)
{
  bool lowspeed;
  if (!uhci_reset_port(io + portsc_off, &lowspeed))
    return;

  uint8_t desc[18];
  /* First read 8 bytes to learn bMaxPacketSize0 (descriptor byte 7). */
  if (uhci_control_in(io, lowspeed, 0, 8, 0x80, 0x06, 0x0100, 0, desc, 8) < 8)
    return;
  uint8_t maxpacket = desc[7] ? desc[7] : 8;

  /* Now read the full 18-byte device descriptor at the real packet size. */
  if (uhci_control_in(io, lowspeed, 0, maxpacket, 0x80, 0x06, 0x0100, 0, desc, 18) < 18)
    return;

  uint16_t vid = desc[8] | (desc[9] << 8);
  uint16_t pid = desc[10] | (desc[11] << 8);
  uint8_t cls = desc[4];
  uint8_t iproduct = desc[15];

  con_printf("      %04x:%04x  class %02x", vid, pid, cls);
  uhci_print_string(io, lowspeed, maxpacket, iproduct);
  con_putc('\n');
}

/* Read and report one UHCI root-hub port, then enumerate any device on it. */
static void uhci_probe_port(uint16_t io_base, uint16_t offset, int port)
{
  uint16_t status = inw(io_base + offset);
  con_printf("    port %u: ", (uint32_t)port);
  if (!(status & UHCI_PORT_CONNECTED)) {
    con_write("no device\n");
    return;
  }
  con_write((status & UHCI_PORT_LOWSPEED) ? "low-speed device\n" : "full-speed device\n");
  uhci_enumerate(io_base, offset);
}

/* For UHCI, BAR4 holds the I/O base */
static void uhci_probe(uint8_t bus, uint8_t slot, uint8_t func)
{
  uint32_t bar4 = pci_read32(bus, slot, func, 0x20);
  if (!(bar4 & 0x1)) {
    con_write("    (no I/O BAR; cannot probe ports)\n");
    return;
  }
  uint16_t io_base = (uint16_t)(bar4 & 0xFFFC);

  /* The controller DMAs the frame list and TDs, so enable I/O decoding (bit 0) and bus master (bit 2). */
  uint32_t cmd = pci_read32(bus, slot, func, 0x04);
  pci_write32(bus, slot, func, 0x04, cmd | 0x5);

  uhci_prepare(io_base);
  uhci_probe_port(io_base, UHCI_PORTSC1, 1);
  uhci_probe_port(io_base, UHCI_PORTSC2, 2);
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
          uhci_probe(bus, slot, f);
        else if (prog_if == USB_PROGIF_XHCI)
          xhci_init(detail_port);
      }
    }
  }

  if (!found)
    con_write("no USB controllers found\n");
}
