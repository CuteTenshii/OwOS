/* rtl8139.h — RealTek RTL8139 NIC driver (Ethernet link layer only).
 *
 * The driver owns the hardware: it probes the PCI bus, sets up the receive
 * ring and transmit descriptors, and fields the card's interrupt. It knows
 * nothing about IP — inbound frames are handed up via net_rx_frame() and the
 * protocol stack pushes fully-formed Ethernet frames back down via nic_send().
 * It exposes a nic_ops the dispatch layer (nic.c) probes.
 */
#ifndef OWOS_RTL8139_H
#define OWOS_RTL8139_H

#include "nic.h"

extern const struct nic_ops rtl8139_ops;

#endif /* OWOS_RTL8139_H */
