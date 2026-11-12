/* usbhost.c: the controller-neutral half of the USB host core. Each controller owns a range of device ids; every
 * usbh_* call is routed to the controller owning the id or pipe. One poll thread services all controllers, so class
 * drivers only ever run in one context. */

#include "console.h"
#include "heap.h"
#include "kstring.h"
#include "sched.h"
#include "timer.h"
#include "usb.h"
#include "usbhc.h"
#include "xhci.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_RANGES 4

static struct {
  const struct usbh_hc_ops *ops;
  int first, last;
} ranges[MAX_RANGES];
static int nranges;

void usbh_register(const struct usbh_hc_ops *ops, int first, int count)
{
  if (nranges == MAX_RANGES || count <= 0) {
    con_write("usb: too many host controller families\n");
    return;
  }
  ranges[nranges].ops = ops;
  ranges[nranges].first = first;
  ranges[nranges].last = first + count - 1;
  nranges++;
}

static const struct usbh_hc_ops *owner(int id)
{
  for (int i = 0; i < nranges; i++)
    if (id >= ranges[i].first && id <= ranges[i].last)
      return ranges[i].ops;
  return NULL;
}

int usbh_control_in(int slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, uint8_t *out, int len)
{
  const struct usbh_hc_ops *o = owner(slot);
  return o ? o->control_in(slot, req_type, req, value, index, out, len) : -1;
}

int usbh_control_out(int slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, const uint8_t *data,
                     int len)
{
  const struct usbh_hc_ops *o = owner(slot);
  return o ? o->control_out(slot, req_type, req, value, index, data, len) : -1;
}

int usbh_control_nodata(int slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index)
{
  const struct usbh_hc_ops *o = owner(slot);
  return o ? o->control_nodata(slot, req_type, req, value, index) : -1;
}

struct usbh_pipe *usbh_open_intr(int slot, uint8_t ep_addr, uint16_t mps, uint8_t interval, usbh_pipe_cb cb, void *ctx)
{
  const struct usbh_hc_ops *o = owner(slot);
  return o && o->open_intr ? o->open_intr(slot, ep_addr, mps, interval, cb, ctx) : NULL;
}

struct usbh_pipe *usbh_open_intr_out(int slot, uint8_t ep_addr, uint16_t mps, uint8_t interval)
{
  const struct usbh_hc_ops *o = owner(slot);
  return o && o->open_intr_out ? o->open_intr_out(slot, ep_addr, mps, interval) : NULL;
}

struct usbh_pipe *usbh_open_bulk_in(int slot, uint8_t ep_addr, uint16_t mps, usbh_pipe_cb cb, void *ctx)
{
  const struct usbh_hc_ops *o = owner(slot);
  return o && o->open_bulk_in ? o->open_bulk_in(slot, ep_addr, mps, cb, ctx) : NULL;
}

struct usbh_pipe *usbh_open_bulk_out(int slot, uint8_t ep_addr, uint16_t mps)
{
  const struct usbh_hc_ops *o = owner(slot);
  return o && o->open_bulk_out ? o->open_bulk_out(slot, ep_addr, mps) : NULL;
}

struct usbh_pipe *usbh_open_isoc_in(int slot, uint8_t ep_addr, uint16_t mps, uint8_t burst, uint8_t mult,
                                    uint32_t payload, uint8_t binterval, usbh_isoc_cb cb, void *ctx)
{
  const struct usbh_hc_ops *o = owner(slot);
  return o && o->open_isoc_in ? o->open_isoc_in(slot, ep_addr, mps, burst, mult, payload, binterval, cb, ctx) : NULL;
}

int usbh_pipe_send(struct usbh_pipe *p, const uint8_t *data, int len)
{
  return p ? p->ops->pipe_send(p, data, len) : -1;
}

void usbh_close_pipe(struct usbh_pipe *p)
{
  if (p)
    p->ops->close_pipe(p);
}

int usbh_config_hub(int slot, int nports, int think_time)
{
  const struct usbh_hc_ops *o = owner(slot);
  return o ? o->config_hub(slot, nports, think_time) : -1;
}

int usbh_attach_child(int parent_slot, int port, int speed, uint32_t kbps)
{
  const struct usbh_hc_ops *o = owner(parent_slot);
  return o ? o->attach_child(parent_slot, port, speed, kbps) : 0;
}

void usbh_detach_child(int parent_slot, int port)
{
  const struct usbh_hc_ops *o = owner(parent_slot);
  if (o)
    o->detach_child(parent_slot, port);
}

struct usbdev *usbh_dev(int slot)
{
  const struct usbh_hc_ops *o = owner(slot);
  return o ? o->dev(slot) : NULL;
}

int usbh_max_slot(void)
{
  int max = 0;
  for (int i = 0; i < nranges; i++) {
    int m = ranges[i].ops->max_id();
    if (m > max)
      max = m;
  }
  return max;
}

void usbh_read_config(struct usbdev *d)
{
  uint8_t hdr[9];
  if (usbh_control_in(d->slot, 0x80, 0x06, 0x0200, 0, hdr, 9) != 9)
    return;
  int total = hdr[2] | (hdr[3] << 8);
  if (total > 4096)
    total = 4096;
  if (total < 9)
    return;
  d->cfg = kmalloc((uint32_t)total);
  if (d->cfg && usbh_control_in(d->slot, 0x80, 0x06, 0x0200, 0, d->cfg, total) == total) {
    d->cfg_len = total;
    usbh_control_nodata(d->slot, 0x00, 0x09, d->cfg[5], 0);
  }
}

void usbh_bind_classes(struct usbdev *d)
{
  if (!d->cfg_len)
    return;
  hub_bind(d);
  hid_bind(d);
  xbox_bind(d);
  btusb_bind(d);
  uvc_bind(d);
}

void usbh_unbind_classes(int id)
{
  hub_unbind(id);
  hid_unbind(id);
  xbox_unbind(id);
  btusb_unbind(id);
  uvc_unbind(id);
}

void usbh_service(void)
{
  for (int i = 0; i < nranges; i++)
    if (ranges[i].ops->poll)
      ranges[i].ops->poll();
  hid_tick();
  hub_service();
  btusb_tick();
  uvc_tick();
}

static void poll_thread(void)
{
  for (;;) {
    usbh_service();
    timer_sleep(1);
  }
}

void usb_boot(void)
{
  xhci_boot();
  /* xHCI's loop also runs its bulk data paths and calls usbh_service itself. */
  if (xhci_present())
    sched_create_thread("usbpoll", xhci_poll_loop);
  else if (nranges)
    sched_create_thread("usbpoll", poll_thread);
}
