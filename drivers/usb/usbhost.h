/* usbhost.h: xHCI host core services for USB class drivers (HID, hub). Everything here runs on the usb poll thread (or
 * the boot thread before it exists); class callbacks must not block. */
#ifndef OWOS_USBHOST_H
#define OWOS_USBHOST_H

#include <stdbool.h>
#include <stdint.h>

/* xHCI speed IDs. */
#define USB_SPEED_FULL 1
#define USB_SPEED_LOW 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 4

/* Classes a driver bound on a device. */
#define USBH_BOUND_NET 0x01
#define USBH_BOUND_MSC 0x02
#define USBH_BOUND_MTP 0x04
#define USBH_BOUND_HID 0x08
#define USBH_BOUND_HUB 0x10

struct usbdev {
  bool active;
  int slot;
  int speed;
  uint16_t vid, pid;
  uint8_t cls;
  uint8_t *cfg;
  int cfg_len;
  /* Topology: root port, hub parent (slot 0 = root port), port on that hub, hub tier, xHCI route string, and the
   * high-speed hub whose transaction translator serves a low/full-speed device. */
  uint8_t root_port;
  uint8_t depth;
  uint8_t parent_port;
  int parent_slot;
  uint32_t route;
  int tt_slot, tt_port;
  uint32_t bound;
  /* If this looks like a USB-Ethernet (RNDIS) device: the comm interface that
   * takes encapsulated commands, the bulk data pair, and the MAC we learned. */
  bool is_net;
  bool is_msc; /* bulk-only-transport mass storage (class 08/06/50) */
  bool is_mtp; /* media transfer protocol / PTP (class 06/01/01)    */
  int ctrl_iface;
  uint8_t bulk_in, bulk_out, intr_in;
  uint16_t bulk_in_mps, bulk_out_mps, intr_in_mps;
  uint8_t intr_interval;
  uint8_t mac[6];
  bool mac_ok;
};

/* Endpoint 0 requests on a configured slot; each returns -1 on failure (a stalled pipe is recovered). In returns the
 * requested length. */
int usbh_control_in(int slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, uint8_t *out, int len);
int usbh_control_out(int slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, const uint8_t *data,
                     int len);
int usbh_control_nodata(int slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index);

/* An interrupt IN endpoint kept armed: cb runs for every completed transfer with the received bytes. */
struct usbh_pipe;
typedef void (*usbh_pipe_cb)(void *ctx, const uint8_t *data, int len);
struct usbh_pipe *usbh_open_intr(int slot, uint8_t ep_addr, uint16_t mps, uint8_t interval, usbh_pipe_cb cb, void *ctx);

/* Configure a hub slot (Hub flag, port count, TT think time) and its status-change endpoint. */
int usbh_config_hub(int slot, int nports, int think_time);

/* Enumerate the device behind port `port` of hub `parent_slot` at `speed`; returns its slot or 0. */
int usbh_attach_child(int parent_slot, int port, int speed);
/* Tear down whatever is attached behind that hub port. */
void usbh_detach_child(int parent_slot, int port);

struct usbdev *usbh_dev(int slot); /* NULL for an unused slot */
int usbh_max_slot(void);
bool xhci_present(void);

/* Bulk class state, for lsusb. */
bool usbh_is_net_slot(int slot);

/* Interrupt interval field for an endpoint context: 2^n * 125 us. High/super speed bInterval is already an exponent
 * (1..16); low/full-speed bInterval is milliseconds, rounded down to a power of two of microframes (3..10 for 1..255).
 */
static inline uint32_t usbh_intr_exponent(int speed, uint8_t binterval)
{
  if (speed >= USB_SPEED_HIGH) {
    uint32_t v = binterval ? binterval : 1;
    return (v > 16 ? 16 : v) - 1;
  }
  uint32_t mf = (binterval ? binterval : 1) * 8u, e = 0;
  while (mf > 1) {
    mf >>= 1;
    e++;
  }
  return e;
}

/* Class driver entry points called by the host core. */
void hid_bind(struct usbdev *d);
void hid_unbind(int slot);
void hid_tick(void);
void hid_print(int slot, bool detail);
void hub_bind(struct usbdev *d);
void hub_unbind(int slot);
void hub_service(void);
void hub_print(int slot);

#endif /* OWOS_USBHOST_H */
