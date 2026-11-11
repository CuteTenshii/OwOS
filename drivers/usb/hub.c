/* hub.c: USB hub class driver. Powers and resets downstream ports, follows the status-change endpoint, and enumerates
 * devices behind each port through the xHCI core (route string and transaction-translator fields are filled there). */

#include "console.h"
#include "heap.h"
#include "kstring.h"
#include "timer.h"
#include "usbhost.h"
#include <stdbool.h>
#include <stdint.h>

#define HUB_DESC_USB2 0x29
#define HUB_DESC_SS 0x2A

#define REQ_GET_STATUS 0
#define REQ_CLEAR_FEATURE 1
#define REQ_SET_FEATURE 3
#define REQ_GET_DESCRIPTOR 6
#define REQ_SET_HUB_DEPTH 12

#define PORT_FEAT_RESET 4
#define PORT_FEAT_POWER 8
#define PORT_FEAT_C_CONNECTION 16
#define PORT_FEAT_C_BH_RESET 29
#define PORT_FEAT_C_LINK_STATE 25
#define PORT_FEAT_C_CONFIG_ERROR 26

#define PORT_STAT_CONNECTION 0x0001
#define PORT_STAT_ENABLE 0x0002
#define PORT_STAT_LOW_SPEED 0x0200
#define PORT_STAT_HIGH_SPEED 0x0400
#define PORT_CHG_CONNECTION 0x0001
#define PORT_CHG_RESET 0x0010

/* GET_STATUS wValue selecting a SuperSpeedPlus hub's 8-byte extended port status. */
#define PORT_STATUS_EXT 2

struct hub {
  struct hub *next;
  int slot;
  int nports;
  bool super;
  bool ssp;                  /* SuperSpeedPlus hub: ports report their sublink speed */
  volatile uint8_t *pending; /* bit n = port n changed (bit 0 = the hub itself) */
};

static struct hub *hubs;
static bool in_service;

static int port_status(int slot, int port, uint16_t *status, uint16_t *change)
{
  uint8_t st[4];
  if (usbh_control_in(slot, 0xA3, REQ_GET_STATUS, 0, port, st, 4) != 4)
    return -1;
  *status = st[0] | (st[1] << 8);
  *change = st[2] | (st[3] << 8);
  return 0;
}

static void port_feature(int slot, uint8_t req, int feature, int port)
{
  usbh_control_nodata(slot, 0x23, req, (uint16_t)feature, (uint16_t)port);
}

/* Status-change endpoint: remember which ports changed for hub_service. */
static void hub_irq(void *ctx, const uint8_t *data, int len)
{
  struct hub *h = ctx;
  int bytes = h->nports / 8 + 1;
  for (int i = 0; i < len && i < bytes; i++)
    h->pending[i] |= data[i];
}

static bool has_child(int slot, int port)
{
  for (int s = 1; s <= usbh_max_slot(); s++) {
    struct usbdev *c = usbh_dev(s);
    if (c && c->parent_slot == slot && c->parent_port == port)
      return true;
  }
  return false;
}

/* The negotiated speed behind an enabled SuperSpeed hub port. */
static int super_port_speed(struct hub *h, int port, uint32_t *kbps)
{
  uint8_t st[8];
  struct usbdev *d = usbh_dev(h->slot);
  if (h->ssp && d && usbh_control_in(h->slot, 0xA3, REQ_GET_STATUS, PORT_STATUS_EXT, (uint16_t)port, st, 8) == 8)
    return usb3_ext_port_speed(&d->bos, st[4] | st[5] << 8 | (uint32_t)st[6] << 16 | (uint32_t)st[7] << 24, kbps);
  *kbps = usb3_default_kbps(USB_SPEED_SUPER);
  return USB_SPEED_SUPER;
}

/* Reset a connected port and return the speed of what enabled (and its bit rate), or 0. */
static int reset_hub_port(struct hub *h, int port, uint32_t *kbps)
{
  uint16_t status = 0, change = 0;
  port_feature(h->slot, REQ_SET_FEATURE, PORT_FEAT_RESET, port);
  for (int ms = 0; ms < 500; ms += 10) {
    timer_sleep(10);
    if (port_status(h->slot, port, &status, &change) == 0 && (change & PORT_CHG_RESET))
      break;
  }
  port_feature(h->slot, REQ_CLEAR_FEATURE, h->super ? PORT_FEAT_C_BH_RESET : PORT_FEAT_C_CONNECTION + 4, port);
  if (port_status(h->slot, port, &status, &change) != 0 || !(status & PORT_STAT_ENABLE))
    return 0;
  timer_sleep(10);
  if (h->super)
    return super_port_speed(h, port, kbps);
  int speed = (status & PORT_STAT_LOW_SPEED)    ? USB_SPEED_LOW
              : (status & PORT_STAT_HIGH_SPEED) ? USB_SPEED_HIGH
                                                : USB_SPEED_FULL;
  *kbps = usb3_default_kbps(speed);
  return speed;
}

static void port_changed(struct hub *h, int port)
{
  int slot = h->slot;
  uint16_t status, change;
  if (port_status(slot, port, &status, &change) != 0)
    return;
  for (int bit = 0; bit < 5; bit++)
    if (change & (1u << bit))
      port_feature(slot, REQ_CLEAR_FEATURE, PORT_FEAT_C_CONNECTION + bit, port);
  if (h->super) {
    if (change & 0x20)
      port_feature(slot, REQ_CLEAR_FEATURE, PORT_FEAT_C_BH_RESET, port);
    if (change & 0x40)
      port_feature(slot, REQ_CLEAR_FEATURE, PORT_FEAT_C_LINK_STATE, port);
    if (change & 0x80)
      port_feature(slot, REQ_CLEAR_FEATURE, PORT_FEAT_C_CONFIG_ERROR, port);
  }

  bool connected = status & PORT_STAT_CONNECTION;
  if ((change & PORT_CHG_CONNECTION) || (!connected && has_child(slot, port)))
    usbh_detach_child(slot, port);
  if (connected && !has_child(slot, port)) {
    uint32_t kbps = 0;
    int speed = reset_hub_port(h, port, &kbps);
    if (speed)
      usbh_attach_child(slot, port, speed, kbps);
    else
      con_printf("usbhub: slot %d port %d did not enable\n", slot, port);
  }
}

static struct hub *next_pending(int *port)
{
  for (struct hub *h = hubs; h; h = h->next)
    for (int p = 1; p <= h->nports; p++)
      if (h->pending[p / 8] & (1u << (p % 8))) {
        *port = p;
        return h;
      }
  return NULL;
}

void hub_service(void)
{
  if (in_service)
    return;
  in_service = true;
  int port;
  for (struct hub *h; (h = next_pending(&port));) {
    h->pending[port / 8] &= (uint8_t)~(1u << (port % 8));
    port_changed(h, port);
  }
  in_service = false;
}

void hub_bind(struct usbdev *d)
{
  const uint8_t *p = d->cfg;
  bool in_hub = false;
  uint8_t ep = 0, interval = 0;
  uint16_t mps = 0;
  for (int i = 0; i + 1 < d->cfg_len;) {
    uint8_t l = p[i], t = p[i + 1];
    if (l < 2)
      break;
    if (t == 0x04 && i + 8 < d->cfg_len)
      in_hub = p[i + 5] == 0x09;
    else if (t == 0x05 && in_hub && i + 6 < d->cfg_len && (p[i + 3] & 3) == 3 && (p[i + 2] & 0x80) && !ep) {
      ep = p[i + 2];
      mps = (uint16_t)((p[i + 4] | (p[i + 5] << 8)) & 0x7FF);
      interval = p[i + 6];
    }
    i += l;
  }
  if (!ep)
    return;

  bool super = d->speed >= USB_SPEED_SUPER;
  uint8_t desc[16];
  if (usbh_control_in(d->slot, 0xA0, REQ_GET_DESCRIPTOR, (uint16_t)((super ? HUB_DESC_SS : HUB_DESC_USB2) << 8), 0,
                      desc, 9) != 9 ||
      desc[2] == 0)
    return;
  int nports = desc[2];
  int think = (desc[3] >> 5) & 3;
  int power_ms = desc[5] * 2;
  if (super)
    usbh_control_nodata(d->slot, 0x20, REQ_SET_HUB_DEPTH, d->depth, 0);
  if (usbh_config_hub(d->slot, nports, think) != 0)
    return;

  struct hub *h = kmalloc(sizeof *h);
  uint8_t *pending = kmalloc((uint32_t)nports / 8 + 1);
  if (!h || !pending) {
    kfree(h);
    kfree(pending);
    return;
  }
  memset(pending, 0, (uint32_t)nports / 8 + 1);
  h->slot = d->slot;
  h->nports = nports;
  h->super = super;
  h->ssp = super && d->bos.ssp;
  h->pending = pending;
  if (!usbh_open_intr(d->slot, ep, mps, interval, hub_irq, h)) {
    kfree(h);
    kfree(pending);
    return;
  }
  h->next = hubs;
  hubs = h;
  d->bound |= USBH_BOUND_HUB;

  for (int port = 1; port <= nports; port++)
    port_feature(d->slot, REQ_SET_FEATURE, PORT_FEAT_POWER, port);
  timer_sleep(power_ms > 100 ? power_ms : 100);
  for (int port = 1; port <= nports; port++)
    h->pending[port / 8] |= (uint8_t)(1u << (port % 8));
  hub_service();
}

void hub_unbind(int slot)
{
  for (struct hub **pp = &hubs; *pp; pp = &(*pp)->next) {
    struct hub *h = *pp;
    if (h->slot != slot)
      continue;
    *pp = h->next;
    kfree((void *)h->pending);
    kfree(h);
    return;
  }
}

void hub_print(int slot)
{
  for (struct hub *h = hubs; h; h = h->next)
    if (h->slot == slot)
      con_printf(" [hub %d ports%s]", h->nports, h->ssp ? ", superspeed+" : h->super ? ", superspeed" : "");
}
