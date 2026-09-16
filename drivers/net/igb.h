/* igb.h — Intel igb-family Gigabit Ethernet driver (I210 / I211 / 82576).
 *
 * Exports a nic_ops the dispatch layer (nic.c) probes. The igb parts are a
 * different MAC architecture from the 8254x/e1000e family in e1000.c: per-queue
 * descriptor register windows (0xC000/0xE000), advanced descriptors, and a
 * per-queue enable handshake. They share nothing but the PCI/Ethernet shape, so
 * they get their own driver rather than another branch in e1000.c.
 *
 * It speaks only Ethernet frames — inbound via net_rx_frame(), outbound via the
 * .send hook. Receive is drained off the timer tick (the .poll hook) rather
 * than an interrupt, so no per-queue MSI-X / GPIE / IVAR wiring is needed.
 */
#ifndef OWOS_IGB_H
#define OWOS_IGB_H

#include "nic.h"

extern const struct nic_ops igb_ops;

#endif /* OWOS_IGB_H */
