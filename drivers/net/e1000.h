/* e1000.h — Intel Gigabit Ethernet driver (8254x / 82574 / I217-I219 family).
 * (The I210/I211 are the separate "igb" family — see igb.c, not here.)
 *
 * Exports a nic_ops the dispatch layer (nic.c) probes. The driver owns the
 * hardware: PCI probe, MMIO register file, the receive/transmit descriptor
 * rings, and the receive interrupt. It speaks only Ethernet frames — inbound
 * via net_rx_frame(), outbound via the .send hook.
 */
#ifndef OWOS_E1000_H
#define OWOS_E1000_H

#include "nic.h"

extern const struct nic_ops e1000_ops;

/* Print the Intel NIC bring-up log + live link state (the `ethdiag` command).
 * Chiefly for the PCH-integrated I217/I218/I219 wake path, whose probe runs
 * before any console is visible on a laptop. */
void e1000_diag(void);

/* `txtest` command: MAC-internal-loopback transmit self-test — isolates the TX
 * engine/DMA from the external MAC<->PHY link to localize why TX is dead. */
void e1000_txtest(void);

#endif /* OWOS_E1000_H */
