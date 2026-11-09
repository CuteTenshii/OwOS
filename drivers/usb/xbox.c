/* xbox.c: Xbox One / Series controllers on USB (vendor class FF/47/D0, GIP protocol): power-on, input packets on the
 * interrupt endpoint, rumble on the interrupt OUT endpoint. Decoded state goes to the gamepad layer in hid.c. */

#include "console.h"
#include "hid.h"
#include "hid_pad.h"
#include "usbhost.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GIP_CLASS 0xFF
#define GIP_SUBCLASS 0x47
#define GIP_PROTOCOL 0xD0

static struct {
  int slot;
  struct hid_dev *pad;
  struct usbh_pipe *out;
  uint8_t seq, hat;
  uint32_t buttons;
  int32_t axes[6];
  bool guide;
} xb;

static int xbox_send(void *ctx, uint8_t id, const uint8_t *buf, int n)
{
  (void)ctx;
  (void)id;
  return usbh_pipe_send(xb.out, buf, n);
}

static void xbox_report(void *ctx, const uint8_t *data, int len)
{
  (void)ctx;
  if (!xbox_decode_guide(data, len, &xb.guide) && !xbox_decode(data, len, &xb.buttons, &xb.hat, xb.axes))
    return;
  hid_pad_report(xb.pad, xb.buttons | (xb.guide ? XBOX_BTN_GUIDE : 0), xb.hat, xb.axes);
}

void xbox_bind(struct usbdev *d)
{
  if (xb.slot || !d->cfg)
    return;
  bool in_iface = false;
  uint8_t ep_in = 0, ep_out = 0, in_interval = 4, out_interval = 4;
  uint16_t in_mps = 0, out_mps = 0;
  for (int i = 0; i + 2 <= d->cfg_len && d->cfg[i] >= 2; i += d->cfg[i]) {
    const uint8_t *p = d->cfg + i;
    if (p[1] == 0x04 && i + 9 <= d->cfg_len) {
      in_iface = !ep_in && p[3] == 0 && p[5] == GIP_CLASS && p[6] == GIP_SUBCLASS && p[7] == GIP_PROTOCOL;
    } else if (p[1] == 0x05 && in_iface && i + 7 <= d->cfg_len && (p[3] & 3) == 3) {
      uint16_t mps = (uint16_t)((p[4] | p[5] << 8) & 0x7FF);
      if (p[2] & 0x80) {
        ep_in = p[2];
        in_mps = mps;
        in_interval = p[6];
      } else {
        ep_out = p[2];
        out_mps = mps;
        out_interval = p[6];
      }
    }
  }
  if (!ep_in || !ep_out || pad_kind_usb(d->vid, d->pid) != PAD_XBOX)
    return;

  xb.slot = d->slot;
  xb.guide = false;
  xb.buttons = xb.hat = 0;
  for (int i = 0; i < 6; i++)
    xb.axes[i] = 0;
  xb.out = usbh_open_intr_out(d->slot, ep_out, out_mps, out_interval);
  xb.pad = xb.out ? hid_pad_add(PAD_XBOX, d->vid, d->pid, xbox_send, NULL) : NULL;
  if (!xb.pad || !usbh_open_intr(d->slot, ep_in, in_mps, in_interval, xbox_report, NULL)) {
    con_write("xbox: endpoint setup failed\n");
    if (xb.pad)
      hid_dev_remove(xb.pad);
    xb.slot = 0;
    xb.pad = NULL;
    return;
  }
  uint8_t init[XBOX_INIT_LEN];
  xbox_pack_init(xb.seq++, init);
  usbh_pipe_send(xb.out, init, XBOX_INIT_LEN);
  d->bound |= USBH_BOUND_HID;
}

void xbox_unbind(int slot)
{
  if (!xb.slot || xb.slot != slot)
    return;
  hid_dev_remove(xb.pad);
  xb.slot = 0;
  xb.pad = NULL;
  xb.out = NULL;
}
