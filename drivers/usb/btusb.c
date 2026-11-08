/* btusb.c: USB Bluetooth HCI transport (class E0/01/01): commands over EP0, events on the interrupt endpoint, ACL data
 * on the bulk pair. Hands packets to the host stack in drivers/bt. */

#include "bt.h"
#include "console.h"
#include "timer.h"
#include "usbhost.h"
#include <stddef.h>

#define EVT_MAX 260
#define ACL_MAX 1032

static struct {
  int slot;
  struct usbh_pipe *evt, *acl_in, *acl_out;
  uint8_t ev[EVT_MAX], acl[ACL_MAX];
  int ev_len, acl_len;
} bt;

/* Events and ACL packets arrive in endpoint-sized pieces; the HCI header says how long the whole packet is. */
static void ev_chunk(void *ctx, const uint8_t *d, int len)
{
  (void)ctx;
  if (bt.ev_len + len > EVT_MAX)
    bt.ev_len = 0;
  for (int i = 0; i < len; i++)
    bt.ev[bt.ev_len++] = d[i];
  if (bt.ev_len >= 2 && bt.ev_len >= 2 + bt.ev[1]) {
    bt_rx(BT_PKT_EVT, bt.ev, bt.ev_len);
    bt.ev_len = 0;
  }
}

static void acl_chunk(void *ctx, const uint8_t *d, int len)
{
  (void)ctx;
  if (bt.acl_len + len > ACL_MAX)
    bt.acl_len = 0;
  for (int i = 0; i < len; i++)
    bt.acl[bt.acl_len++] = d[i];
  if (bt.acl_len >= 4 && bt.acl_len >= 4 + (bt.acl[2] | bt.acl[3] << 8)) {
    bt_rx(BT_PKT_ACL, bt.acl, bt.acl_len);
    bt.acl_len = 0;
  }
}

static int bt_send(void *ctx, int type, const uint8_t *data, int len)
{
  (void)ctx;
  if (type == BT_PKT_CMD)
    return usbh_control_out(bt.slot, 0x20, 0, 0, 0, data, len) < 0 ? -1 : 0;
  if (type == BT_PKT_ACL)
    return usbh_pipe_send(bt.acl_out, data, len);
  return -1;
}

void btusb_bind(struct usbdev *d)
{
  if (bt.slot || !d->cfg)
    return;
  bool in_iface = false;
  uint8_t intr = 0, bin = 0, bout = 0;
  uint16_t intr_mps = 0, bin_mps = 0, bout_mps = 0;
  uint8_t interval = 1;
  for (int i = 0; i + 2 <= d->cfg_len && d->cfg[i] >= 2; i += d->cfg[i]) {
    const uint8_t *p = d->cfg + i;
    if (p[1] == 0x04 && i + 9 <= d->cfg_len) {
      in_iface = !intr && p[3] == 0 && p[5] == 0xE0 && p[6] == 0x01 && p[7] == 0x01;
    } else if (p[1] == 0x05 && in_iface && i + 7 <= d->cfg_len) {
      uint16_t mps = (uint16_t)((p[4] | p[5] << 8) & 0x7FF);
      if ((p[3] & 3) == 3 && (p[2] & 0x80)) {
        intr = p[2];
        intr_mps = mps;
        interval = p[6];
      } else if ((p[3] & 3) == 2) {
        if (p[2] & 0x80) {
          bin = p[2];
          bin_mps = mps;
        } else {
          bout = p[2];
          bout_mps = mps;
        }
      }
    }
  }
  if (!intr || !bin || !bout)
    return;

  bt.slot = d->slot;
  bt.evt = usbh_open_intr(d->slot, intr, intr_mps, interval, ev_chunk, NULL);
  bt.acl_in = usbh_open_bulk_in(d->slot, bin, bin_mps, acl_chunk, NULL);
  bt.acl_out = usbh_open_bulk_out(d->slot, bout, bout_mps);
  if (!bt.evt || !bt.acl_in || !bt.acl_out) {
    con_write("btusb: endpoint setup failed\n");
    bt.slot = 0;
    return;
  }
  d->bound |= USBH_BOUND_BT;
  static const struct bt_transport t = {NULL, bt_send};
  bt_attach(&t);
}

void btusb_unbind(int slot)
{
  if (bt.slot != slot)
    return;
  bt_detach();
  bt.slot = 0;
  bt.ev_len = bt.acl_len = 0;
}

void btusb_tick(void)
{
  if (bt.slot)
    bt_poll(timer_ticks() * (1000 / TIMER_HZ));
}

void btusb_print(int slot)
{
  if (bt.slot == slot)
    con_write(bt_present() ? " [bluetooth]" : " [bluetooth starting]");
}
