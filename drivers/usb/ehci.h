/* ehci.h: EHCI (USB 2.0) host controller driver. */
#ifndef OWOS_EHCI_H
#define OWOS_EHCI_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up every EHCI controller and enumerate what is attached (part of usb_boot). Runs before uhci_boot, so the
 * full- and low-speed devices it hands to the companion controllers are there when UHCI scans. */
void ehci_boot(void);

/* For lsusb: the device-id range and root-port count of the EHCI controller at that PCI function, and which root
 * ports (bit n = port n) are handed to a companion controller; false if it is not driven. */
bool ehci_controller(uint8_t bus, uint8_t slot, uint8_t func, int *first_id, int *last_id, int *nports,
                     uint32_t *companion_ports);

#endif /* OWOS_EHCI_H */
