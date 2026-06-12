/* xhci.h — minimal xHCI (USB 3.x host controller) driver.
 *
 * Modern laptops route every USB port through an xHCI controller (Intel
 * dropped EHCI around Skylake), so reaching a USB-tethered phone needs this.
 * Milestone 1 is enumeration: bring the controller up and read the device
 * descriptor of whatever is attached. Later milestones add bulk transfers and
 * an RNDIS/CDC driver that registers as a `nic_ops` so the IP stack can use a
 * tethered phone like any other NIC.
 */
#ifndef OWOS_XHCI_H
#define OWOS_XHCI_H

#include <stdint.h>

/* Find an xHCI controller on PCI, bring it up, and enumerate every connected
 * port (one summary line per device). `detail_port` selects a port to dump full
 * interface/endpoint detail for (-1 = summary only). Returns 1 if a controller
 * was found and initialized, 0 otherwise. */
int xhci_init(int detail_port);

/* Bring the xHCI controller up at boot and configure attached devices, so OwOS
 * is a persistent USB host (a tethered phone keeps offering tethering). Call
 * after interrupts are enabled — it uses timer_sleep. No-op if no controller. */
void xhci_boot(void);

/* USB Mass Storage (Bulk-Only Transport / SCSI) block device. A connected USB
 * flash drive / SD reader is exposed as a 512-byte-sector block device the FAT
 * driver can mount at /media. Present only after xhci_boot() has enumerated it.
 * Sector I/O is serviced by the USB poll thread, so it must not be called from
 * that thread; the block/FAT layer (shell/main thread) is the intended caller. */
int      xhci_msc_present(void);             /* 1 once a stick is configured        */
uint32_t xhci_msc_sectors(void);             /* capacity in 512-byte sectors        */
const char *xhci_msc_model(void);            /* INQUIRY vendor+product string       */
int      xhci_msc_read(uint32_t lba, void *buf);        /* one sector; 0 ok, -1 err */
int      xhci_msc_write(uint32_t lba, const void *buf); /* one sector; 0 ok, -1 err */

#endif /* OWOS_XHCI_H */
