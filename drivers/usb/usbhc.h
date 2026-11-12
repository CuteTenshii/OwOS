/* usbhc.h: the contract between the controller-neutral USB core (usbhost.c) and a host controller driver (xhci.c,
 * uhci.c). Class drivers never include this; they use usbhost.h. */
#ifndef OWOS_USBHC_H
#define OWOS_USBHC_H

#include "usbhost.h"
#include <stdbool.h>
#include <stdint.h>

/* Device ids per controller family: xHCI slot ids are 8 bits, then 128 addresses for each of up to 8 UHCI and 4 EHCI
 * controllers. */
#define USBH_ADDRS 128
#define USBH_UHCI_FIRST_ID 256
#define USBH_UHCI_MAX_HCS 8
#define USBH_EHCI_FIRST_ID (USBH_UHCI_FIRST_ID + USBH_UHCI_MAX_HCS * USBH_ADDRS)
#define USBH_EHCI_MAX_HCS 4

struct usbh_hc_ops;

/* First member of every controller's pipe, so usbhost.c can route a pipe call to its owner. */
struct usbh_pipe {
  const struct usbh_hc_ops *ops;
};

/* One controller family's entry points. A NULL open_* means the controller does not support that transfer type. */
struct usbh_hc_ops {
  struct usbdev *(*dev)(int id);
  int (*max_id)(void); /* highest id in use, 0 for none */
  int (*control_in)(int id, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, uint8_t *out, int len);
  int (*control_out)(int id, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, const uint8_t *data,
                     int len);
  int (*control_nodata)(int id, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index);
  struct usbh_pipe *(*open_intr)(int id, uint8_t ep_addr, uint16_t mps, uint8_t interval, usbh_pipe_cb cb, void *ctx);
  struct usbh_pipe *(*open_intr_out)(int id, uint8_t ep_addr, uint16_t mps, uint8_t interval);
  struct usbh_pipe *(*open_bulk_in)(int id, uint8_t ep_addr, uint16_t mps, usbh_pipe_cb cb, void *ctx);
  struct usbh_pipe *(*open_bulk_out)(int id, uint8_t ep_addr, uint16_t mps);
  struct usbh_pipe *(*open_isoc_in)(int id, uint8_t ep_addr, uint16_t mps, uint8_t burst, uint8_t mult,
                                    uint32_t payload, uint8_t binterval, usbh_isoc_cb cb, void *ctx);
  int (*pipe_send)(struct usbh_pipe *p, const uint8_t *data, int len);
  void (*close_pipe)(struct usbh_pipe *p);
  int (*config_hub)(int id, int nports, int think_time);
  int (*attach_child)(int parent_id, int port, int speed, uint32_t kbps);
  void (*detach_child)(int parent_id, int port);
  /* Non-blocking upkeep run from the USB poll thread (completions, root-port changes); NULL if not needed. */
  void (*poll)(void);
};

/* Hand device ids first..first+count-1 to a controller. */
void usbh_register(const struct usbh_hc_ops *ops, int first, int count);

/* Enumeration steps shared by every controller, run once the device answers at its address. */
void usbh_read_product(struct usbdev *d, uint8_t index);
void usbh_read_config(struct usbdev *d); /* cache the first configuration and select it */
void usbh_bind_classes(struct usbdev *d);
void usbh_unbind_classes(int id);

/* Controller polls and class-driver ticks; the poll thread runs it about once a millisecond. */
void usbh_service(void);

#endif /* OWOS_USBHC_H */
