/* usb3.h: SuperSpeed and SuperSpeedPlus (USB 3.1/3.2) link-speed parsing: the xHCI Supported Protocol capability, the
 * BOS SuperSpeedPlus device capability and the hub extended port status. No hardware access, so the host tests build
 * it directly. */
#ifndef OWOS_USB3_H
#define OWOS_USB3_H

#include <stdbool.h>
#include <stdint.h>

/* Speed classes, numbered as the xHCI default Protocol Speed IDs (which the Slot Context Speed field takes). */
#define USB_SPEED_FULL 1
#define USB_SPEED_LOW 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 4      /* 5 Gbps Gen 1x1                     */
#define USB_SPEED_SUPER_PLUS 5 /* Gen 2x1, Gen 1x2 or Gen 2x2 (3.1/3.2) */

#define USB3_MAX_PSI 15
#define USB3_MAX_SUBLINKS 16

/* One Protocol Speed ID of a root-hub port group. */
struct usb3_psi {
  uint8_t psiv; /* the value PORTSC Port Speed reports */
  bool ssp;     /* SuperSpeedPlus link protocol        */
  bool tx;      /* asymmetric: the transmit half       */
  bool asym;
  uint32_t kbps;
};

/* An xHCI Supported Protocol extended capability (ID 2): which root ports speak which USB revision, and their speeds.
 */
struct usb3_proto {
  uint8_t major, minor; /* BCD revision, e.g. 3 and 0x10 for USB 3.1 */
  uint8_t port_first, port_count;
  uint8_t npsi;
  struct usb3_psi psi[USB3_MAX_PSI];
};

/* Parse the capability whose `ndw` dwords start at dw (dw[0] is the header). Returns 0, or -1 if it is not a USB
 * Supported Protocol capability or is truncated. */
int usb3_proto_parse(const uint32_t *dw, int ndw, struct usb3_proto *out);

/* Speed class (USB_SPEED_*) and bit rate of PORTSC speed id `psiv` on a port of protocol p, or 0 if unknown. p
 * may be NULL, in which case the xHCI default speed ids apply. */
int usb3_port_speed(const struct usb3_proto *p, uint8_t psiv, uint32_t *kbps);

/* One Sublink Speed Attribute of a SuperSpeedPlus device. */
struct usb3_sublink {
  uint8_t ssid;
  bool ssp;
  bool asym, tx;
  uint32_t kbps;
};

/* What a device's BOS descriptor says about SuperSpeed. */
struct usb3_bos {
  bool ss;  /* SuperSpeed USB device capability (type 3)          */
  bool ssp; /* SuperSpeedPlus USB device capability (type 0x0A)   */
  uint8_t min_rx_lanes, min_tx_lanes;
  uint8_t nsublinks;
  struct usb3_sublink sublinks[USB3_MAX_SUBLINKS];
};

/* Parse a BOS descriptor set (header plus device capabilities). Returns 0, or -1 if the header is malformed. */
int usb3_bos_parse(const uint8_t *bos, int len, struct usb3_bos *out);

/* Highest receive lane rate of the device's SuperSpeedPlus sublinks, 0 without a SuperSpeedPlus capability. */
uint32_t usb3_bos_max_kbps(const struct usb3_bos *b);

/* Decode a SuperSpeedPlus hub's dwExtPortStatus against that hub's BOS: speed class and receive bit rate (lanes
 * included). An unknown sublink id reads as a 5 Gbps SuperSpeed link, which is what a hub without the capability runs.
 */
int usb3_ext_port_speed(const struct usb3_bos *hub, uint32_t ext_status, uint32_t *kbps);

/* Bit rate of a speed class when nothing more specific is known. */
uint32_t usb3_default_kbps(int speed);

/* Format a bit rate ("1.5 Mbps", "480 Mbps", "10 Gbps") into buf; returns buf. */
char *usb3_rate_str(uint32_t kbps, char *buf, int size);

#endif /* OWOS_USB3_H */
