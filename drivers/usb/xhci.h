/* xhci.h - minimal xHCI (USB 3.x host controller) driver.
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

/* Bring the xHCI controller up at boot and configure attached devices, so OwOS
 * is a persistent USB host (a tethered phone keeps offering tethering). Called
 * by usb_boot after interrupts are enabled - it uses timer_sleep. No-op if no
 * controller.
 */
void xhci_boot(void);

/* The USB poll thread's body when an xHCI controller is up: it also drives the
 * xHCI bulk data paths and calls usbh_service for everything else. Never
 * returns. */
void xhci_poll_loop(void);

/* USB Mass Storage (Bulk-Only Transport / SCSI) block device. A connected USB
 * flash drive / SD reader is exposed as a 512-byte-sector block device the FAT
 * driver can mount at /media. Present only after xhci_boot() has enumerated it.
 * Sector I/O is serviced by the USB poll thread, so it must not be called from
 * that thread; the block/FAT layer (shell/main thread) is the intended caller.
 */
int xhci_msc_present(void);                        /* 1 once a stick is configured        */
uint32_t xhci_msc_sectors(void);                   /* capacity in 512-byte sectors        */
const char *xhci_msc_model(void);                  /* INQUIRY vendor+product string       */
int xhci_msc_read(uint32_t lba, void *buf);        /* one sector; 0 ok, -1 err */
int xhci_msc_write(uint32_t lba, const void *buf); /* one sector; 0 ok, -1 err */

/* MTP / PTP (Media Transfer Protocol). A phone or camera in "file transfer"
 * mode (USB still-image class 06/01/01) exposes its storage as objects, not
 * blocks, so it is browsed through these calls rather than mounted at /media.
 * Present only after xhci_boot() has enumerated such a device. All MTP traffic
 * runs on the USB poll thread; call these from the shell/main thread, not that
 * thread. */
#define XHCI_MTP_ROOT 0xFFFFFFFFu /* "parent" handle for the store root  */
int xhci_mtp_present(void);       /* 1 once an MTP device is ready        */
const char *xhci_mtp_model(void); /* "<manufacturer> <model>" string      */

/* List the children of `parent` (XHCI_MTP_ROOT = storage root). For each
 * object, `cb` gets its handle, name, byte size, and whether it's a folder.
 * Returns the number of objects listed, or -1 on error. */
int xhci_mtp_list(uint32_t parent, void (*cb)(uint32_t handle, const char *name, uint32_t size, int is_dir, void *ctx),
                  void *ctx);

/* Download object `handle` to owofs path `dst`. Returns 0 on success, -1 err.
 */
int xhci_mtp_get(uint32_t handle, const char *dst);

#endif /* OWOS_XHCI_H */
