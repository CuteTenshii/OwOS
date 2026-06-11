/* nic.h — link-layer (Ethernet NIC) driver interface and dispatch.
 *
 * The IP stack (net.c) is driver-agnostic: it brings the NIC up with
 * nic_init(), checks nic_present(), reads nic_mac(), and pushes outbound
 * Ethernet frames down through nic_send(). Each concrete driver (rtl8139.c,
 * e1000.c) exports a `struct nic_ops`; nic_init() probes them in order and
 * keeps the first whose hardware is present, routing the calls above to it.
 * Inbound frames still flow up through net_rx_frame() (declared in net.h).
 */
#ifndef OWOS_NIC_H
#define OWOS_NIC_H

#include <stdint.h>

/* One link-layer driver. probe() looks for its device on the PCI bus and, if
 * found, initializes it (rings, IRQ, MAC) and prints a status line; it returns
 * 1 on success, 0 if the device is absent or bring-up failed. send() transmits
 * one complete Ethernet frame; mac() returns the 6-byte address (valid only
 * after a successful probe). */
struct nic_ops {
	const char    *name;
	int          (*probe)(void);
	void         (*send)(const uint8_t *frame, uint32_t len);
	const uint8_t *(*mac)(void);
	/* Optional (may be NULL): drain any inbound frames the card has DMA'd in,
	 * for cards whose RX interrupt we can't rely on. Called off the timer tick
	 * in addition to the driver's own RX IRQ, so RX still works on a host (e.g.
	 * VMware) that routes the NIC's INTx to a line our PIC never sees. */
	void         (*poll)(void);
};

void           nic_init(void);    /* probe known drivers; keep the first present */
void           nic_set(const struct nic_ops *ops); /* install a hot-plugged driver */
int            nic_present(void); /* 1 once a NIC has been found and enabled     */
const uint8_t *nic_mac(void);     /* our 6-byte MAC address (valid after init)   */

/* Transmit one complete Ethernet frame (dst/src/type + payload). Routed to the
 * active driver, which copies it into a DMA buffer; runt frames are padded to
 * the 60-byte minimum (in hardware or by the driver). */
void           nic_send(const uint8_t *frame, uint32_t len);

/* Poll the active NIC for inbound frames (no-op if it has no poll hook or no
 * NIC is present). Driven off the timer tick as an IRQ-independent RX path. */
void           nic_poll(void);

#endif /* OWOS_NIC_H */
