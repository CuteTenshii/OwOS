/* usb.h — list USB host controllers and probe their root-hub ports. */
#ifndef OWOS_USB_H
#define OWOS_USB_H

void usb_list(int detail_port); /* the `lsusb` command; -1 = summary only */

/* Bring up every USB host controller, enumerate what is attached, and start the poll thread. Needs interrupts on. */
void usb_boot(void);

#endif /* OWOS_USB_H */
