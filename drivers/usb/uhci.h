/* uhci.h: UHCI (USB 1.1) host controller driver. */
#ifndef OWOS_UHCI_H
#define OWOS_UHCI_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up every UHCI controller and enumerate what is attached (part of usb_boot). */
void uhci_boot(void);

/* For lsusb: the device-id range and root-port count of the UHCI controller at that PCI function; false if it is not
 * driven. */
bool uhci_controller(uint8_t bus, uint8_t slot, uint8_t func, int *first_id, int *last_id, int *nports);

#endif /* OWOS_UHCI_H */
