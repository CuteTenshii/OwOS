/* e1000.h — Intel Gigabit Ethernet driver (8254x / 82574 / I21x family).
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

#endif /* OWOS_E1000_H */
