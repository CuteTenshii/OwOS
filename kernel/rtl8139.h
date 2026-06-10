/* rtl8139.h — RealTek RTL8139 NIC driver (Ethernet link layer only).
 *
 * The driver owns the hardware: it probes the PCI bus, sets up the receive
 * ring and transmit descriptors, and fields the card's interrupt. It knows
 * nothing about IP — inbound frames are handed up via net_rx_frame() and the
 * protocol stack pushes fully-formed Ethernet frames back down via nic_send().
 */
#ifndef OWOS_RTL8139_H
#define OWOS_RTL8139_H

#include <stdint.h>

void           nic_init(void);   /* probe + bring up the card (prints status)  */
int            nic_present(void);/* 1 once a card has been found and enabled   */
const uint8_t *nic_mac(void);    /* our 6-byte MAC address (valid after init)  */

/* Transmit one complete Ethernet frame (dst/src/type + payload). The driver
 * copies it into a DMA buffer and pads runt frames to the 60-byte minimum. */
void           nic_send(const uint8_t *frame, uint32_t len);

#endif /* OWOS_RTL8139_H */
